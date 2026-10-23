// TrackFacts.cpp - Плагин для получения интересных фактов о песнях
// Слот провайдера 4 — Groq (OpenAI-совместимый API)
// Ограничение повторных запросов для metadata-провайдеров (потолок — TRACKFACTS_MAX_PER_TRACK).
// Принцип: "Собрал факт -> сохранил в файл -> забыл. Нужно показать -> прочитал файл."
#include "TrackFacts.h"
#include <Arduino.h>
#include "../../core/player.h"
#include "../../core/config.h" // lockLittleFS, isSafeForSSLForFacts, pauseLwIpBackgroundSntp
#include "../../core/timekeeper.h"
#include "../../core/options.h" // MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT — порог буфера в % для запроса фактов
#include "../../core/netserver.h" // [v0.4.4] Для доступа к AsyncWebSocket websocket
#include "../../core/transient_notifier.h"
#include "../../../myoptions.h" // [v0.3.23] Подключаем настройки прокси
#include "TrackFactsProxy.h"
#include "../../core/display.h"
#include "../../displays/dspcore.h"
#if DSP_MODEL==DSP_ST7789 || DSP_MODEL==DSP_ST7789_240 || DSP_MODEL==DSP_ST7789_76
  #include "../../displays/displayST7789.h"
  #define TF_COVER_TOAST_NS st7789_cover
#elif (DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486) && !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT)
  #include "../../displays/displayILI9488.h"
  #define TF_COVER_TOAST_NS ili9488_cover
#elif DSP_MODEL==DSP_AXS15231B
  #include "../../displays/displayAXS15231B.h"
  #define TF_COVER_TOAST_NS axs_cover
#endif

// Показать причину отказа/статус на TFT (cover-SS или PLAYER Faster), не только в WebUI.
static void trackFactsShowDisplayToast(const char* msg, uint32_t durationMs, bool waitForFact) {
#if defined(TF_COVER_TOAST_NS)
  if (msg == nullptr || msg[0] == '\0') return;
  const displayMode_e m = display.mode();
  /* Cover blank-SS — как раньше (экран уже жив). */
  if (config.isScreensaver && m == SCREENBLANK) {
    TF_COVER_TOAST_NS::showFactStatusToast(msg, durationMs, waitForFact);
    return;
  }
  /* PLAYER Faster: только после display.ready() — иначе setProvider в setup() до display.init() крашит canvas. */
  if (m == PLAYER && display.ready()) {
    TF_COVER_TOAST_NS::showFactStatusToast(msg, durationMs, waitForFact);
  }
#else
  (void)msg; (void)durationMs; (void)waitForFact;
#endif
}

// Выбранный пользователем AI (не failover iTunes).
static bool trackFactsIsAiSource(FactSource src) {
  return src == FactSource::GEMINI || src == FactSource::DEEPSEEK || src == FactSource::GROQ;
}

// Будет ли этот AI ходить через HTTP VPS-прокси (мягкий гейт кучи).
static bool trackFactsAiUsesHttpProxy(FactSource src) {
  if (src == FactSource::GEMINI) {
    return trackFactsEffectiveProxyGemini(config.store.trackFactsProxyGemini).length() > 0;
  }
  if (src == FactSource::GROQ) {
    return trackFactsEffectiveProxyGroq(config.store.trackFactsProxyGroq).length() > 0;
  }
  if (src == FactSource::DEEPSEEK) {
    return trackFactsEffectiveProxyDeepSeek(config.store.trackFactsProxyDeepSeek).length() > 0;
  }
  return false;
}

#ifndef TRACKFACTS_AUTO_FETCH_ENABLE
// false — сеть только по нажатию на обложку/лого в плеере (картинка альбома качается как раньше).
// true — фоновый запрос при смене трека. Включать через myoptions.h при стабильном потоке.
#define TRACKFACTS_AUTO_FETCH_ENABLE false
#endif

// [v0.4.0] Подключаем все провайдеры
#include "providers/GeminiProvider.h"
#include "providers/DeepSeekProvider.h"
#include "providers/GroqProvider.h"
#include "providers/LastFmProvider.h"
#include "providers/iTunesProvider.h"

/* Автозапрос при смене трека (только если TRACKFACTS_AUTO_FETCH_ENABLE=true и в WebUI выбран этот источник).
 * LLM (ключ/квота): по умолчанию ВЫКЛ — включить явным #define … true в myoptions.h.
 * iTunes / Last.fm (бесплатные справочники): по умолчанию ВКЛ; отключить — #define … false. */
#ifndef TRACKFACTS_AUTO_GEMINI_ENABLE
  #define TRACKFACTS_AUTO_GEMINI_ENABLE false
#endif

#ifndef TRACKFACTS_AUTO_DEEPSEEK_ENABLE
  #define TRACKFACTS_AUTO_DEEPSEEK_ENABLE false
#endif

#ifndef TRACKFACTS_AUTO_GROQ_ENABLE
  #define TRACKFACTS_AUTO_GROQ_ENABLE false
#endif

#ifndef TRACKFACTS_AUTO_ITUNES_ENABLE
  #define TRACKFACTS_AUTO_ITUNES_ENABLE true
#endif

#ifndef TRACKFACTS_AUTO_LASTFM_ENABLE
  #define TRACKFACTS_AUTO_LASTFM_ENABLE true
#endif

#ifndef TRACKFACTS_MAX_PER_TRACK
  #define TRACKFACTS_MAX_PER_TRACK 5
#endif

// Внешние ссылки на метаданные из конфигурации плеера
extern String metaTitle;
extern String metaArtist;
extern String currentFact;

#define FACT_FETCH_DELAY 7000  // Задержка 7 секунд после смены трека перед первым запросом факта
#define FACT_MANUAL_DELAY 2000 // Задержка 2 секунды перед ручным запросом факта (снижено с 5000 для отзывчивости UI)

static bool isIterativeSource(FactSource source);
static void stripRadioStreamPromoTail(String& s);
static size_t countFactsInPackedString(const String& s);
static bool factsStringsLookLikeCatalog(const std::vector<String>& facts);
static bool factsCacheUsableForCurrentProvider(FactSource userSource, const String& apiKey,
                                               const std::vector<String>& facts);
// Как у CoverDL: служебный бейдж UI, а не «любой '[' в названии» (feat./live…).
static bool isFactsBlockedSystemTitle(const String& title);

bool TrackFacts::isMultiFactGatherOngoing() {
  if (player.status() != PLAYING) return false;
  if (!_enabled || factLoaded || factsPerTrack <= 1) return false;
  if (!isIterativeSource(_effectiveSource)) return false;
  if (factRequestCounter >= factsPerTrack) return false;
  // Реальный сетевой запрос / ручная очередь — да, «ещё ждём».
  if (isRequestInProgress || manualRequestPending) {
    // Но если epoch уже сменился (трек другой) — для UI серия прошлого трека не актуальна.
    if (isRequestInProgress && _activeRequestEpoch != _factsSslEpoch) return false;
    return true;
  }
  // При отключённом авто — не крутить pending «навсегда» из‑за needMore.
  if (!TRACKFACTS_AUTO_FETCH_ENABLE) return false;
  // Авто-серия уже остановлена (сбой/исчерпание) — не держим ⏳.
  if (autoRequestDone) return false;
  if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(40)) == pdTRUE) {
    const bool needMore = currentFacts.size() < (size_t)factsPerTrack;
    xSemaphoreGive(_factsMutex);
    return needMore;
  }
  return false;
}

bool TrackFacts::isRequestPendingForUi() const {
  if (manualRequestPending) return true;
  if (!isRequestInProgress) return false;
  // FactsTask ещё жив, но трек уже сменился — песочные часы гасим, новый тег не блокируем.
  return _activeRequestEpoch == _factsSslEpoch;
}

// ============================================================================
// Конструктор
// ============================================================================
TrackFacts::TrackFacts()
  : lastTitle(""), lastArtist(""), currentFactIndex(0),
    factLoadTime(0), lastTickerCheck(0),     factLoaded(false),
    isRequestInProgress(false), _activeRequestEpoch(0), _enabled(false),
    currentSource(FactSource::ITUNES),
    _effectiveSource(FactSource::ITUNES),
    currentLanguage(FactLanguage::AUTO), geminiApiKey(""),
    factsPerTrack(3), factRequestCounter(0),
    trackChangeMs(0), autoRequestDone(false),
    manualRequestPending(false), manualRequestAtMs(0),
    manualRequestDeadlineMs(0), manualStableSinceMs(0),
    _pendingFetchIsManual(false),
    _manualMoreFragmentsUi(false),
    _factsSslEpoch(0),
    _failoverActive(false), _failoverNotified(false),
    _suppressFailoverToastUntilMs(0),
    lastStatusMs(0),
    _notifiedItunesAiFallback(false),
    _notifiedFlacItunesAuto(false),
    _notifiedFactExhaustedToast(false),
    _manualRequeueUsed(false),
    _manualDeadlineExtended(false),
    _notifiedHeapDeferToast(false),
    _displayFactsAreAi(false) {
  _factsMutex = xSemaphoreCreateMutex(); // [v0.3.23] Инициализация мьютекса

  _providers[0] = new GeminiProvider();     // FactSource::GEMINI = 0
  _providers[1] = new DeepSeekProvider();   // FactSource::DEEPSEEK = 1
  _providers[2] = new iTunesProvider();     // FactSource::ITUNES = 2 (по умолчанию)
  _providers[3] = new LastFmProvider();     // FactSource::LASTFM = 3
  _providers[4] = new GroqProvider();       // FactSource::GROQ = 4
  _providers[5] = nullptr;                 // FactSource::CUSTOM = 5 (не реализован)

  registerPlugin(); // Регистрация плагина в системе
}

// ============================================================================
// Инициализация при старте системы
// ============================================================================
void TrackFacts::on_setup() {
  // Создаем папку кэша фактов, если она не существует
  ensureFactsDir();
  // Инициализация без вывода в монитор
}

// ============================================================================
// Сброс состояния при смене трека
// ============================================================================
void TrackFacts::on_track_change() {
  // Сброс состояния, чтобы старые факты не оставались на экране при смене трека
  factLoaded = false;
  factLoadTime = millis();
  trackChangeMs = factLoadTime;
  autoRequestDone = false;
  manualRequestPending = false;
  manualRequestAtMs = 0;
  manualRequestDeadlineMs = 0;
  manualStableSinceMs = 0;
  _manualMoreFragmentsUi = false;
  currentFacts.clear();
  // Резервируем память под 5 элементов во избежание множественных реаллокаций в DRAM
  if (currentFacts.capacity() < 5) {
      currentFacts.reserve(5); 
  }
  currentFactIndex = 0;
  _factsSslEpoch++;
  // НЕ isRequestInProgress = false — параллельный TLS с CoverDL (см. on_station_change).
  resumeDeferredCoverDownload();
  factRequestCounter = 0;
  // [v0.4.1] Немедленно очищаем глобальную переменную, чтобы старый факт не показывался в WebUI
  ::currentFact = "";
  // Сброс уведомления failover для нового трека (покажем ещё раз если нужно)
  _failoverNotified = false;
  _notifiedItunesAiFallback = false;
  _notifiedFlacItunesAuto = false;
  _notifiedFactExhaustedToast = false;
  _notifiedHeapDeferToast = false;  // новый трек — снова разрешён один toast «мало памяти» для авто
  lastStatusMs = 0; // Сбрасываем время статуса для нового трека
}

// ============================================================================
// Сброс состояния при смене станции
// ============================================================================
void TrackFacts::on_station_change() {
  // Станция сменилась (next/prev/выбор в списке). Даже если новый поток вообще не шлёт
  // метаданные, старый факт не должен продолжать висеть на экране, и нельзя продолжать
  // привязывать факты к заголовку от предыдущей станции.
  //
  // 1) Сбрасываем "последний" заголовок и разобранные поля, чтобы on_ticker при первом же
  //    metaTitle смог распознать НОВЫЙ трек (metaTitle != lastTitle).
  lastTitle = "";
  lastArtist = "";
  lastParsedTitle = "";

  // 2) Сбрасываем локальное состояние фактов аналогично on_track_change.
  factLoaded = false;
  factLoadTime = millis();
  trackChangeMs = factLoadTime;
  autoRequestDone = false;
  manualRequestPending = false;
  manualRequestAtMs = 0;
  manualRequestDeadlineMs = 0;
  manualStableSinceMs = 0;
  _manualMoreFragmentsUi = false;
  factRequestCounter = 0;
  isRequestInProgress = false;
  resumeDeferredCoverDownload();
  _failoverActive = false;
  _failoverNotified = false;
  _notifiedItunesAiFallback = false;
  _notifiedFlacItunesAuto = false;
  _notifiedFactExhaustedToast = false;
  _notifiedHeapDeferToast = false;  // новая станция — снова разрешён один toast «мало памяти» для авто
  lastStatusMs = 0;

  // 3) Очищаем текущие факты и глобальную строку для WebUI под мьютексом, чтобы
  //    немедленно убрать старый текст из интерфейса.
  if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    currentFacts.clear();
    // [v0.4.1] Аналогично on_track_change: очищаем глобальный currentFact,
    // чтобы факт от предыдущей станции не отображался на новой.
    ::currentFact = "";
    xSemaphoreGive(_factsMutex);
  }
  _displayFactsAreAi = false;

  // 4) Резервируем ёмкость вектора, как и в on_track_change, чтобы избежать лишних реаллокаций.
  if (currentFacts.capacity() < 5) {
    currentFacts.reserve(5);
  }
}

// ============================================================================
// Смена режима WEB/SD/BT: мгновенный сброс фактов (без ожидания SSL).
// Результат уже запущенного FactsTask отбросится по _factsSslEpoch.
// ============================================================================
void TrackFacts::abortForModeSwitch() {
  // Инвалидируем любой летящий HTTPS: fetchTask увидит stale epoch и выйдет сам.
  _factsSslEpoch++;
  // Обнуляем авто/ручные очереди — после возврата в режим старт только по новым правилам on_ticker.
  autoRequestDone = false;
  manualRequestPending = false;
  manualRequestAtMs = 0;
  manualRequestDeadlineMs = 0;
  manualStableSinceMs = 0;
  _manualMoreFragmentsUi = false;
  factLoaded = false;
  factRequestCounter = 0;
  factLoadTime = millis();
  trackChangeMs = factLoadTime;
  // Сбрасываем «последний трек», чтобы в новом режиме meta снова считалась новой.
  lastTitle = "";
  lastArtist = "";
  lastParsedTitle = "";
  // UI: убираем чужие факты от предыдущего режима.
  if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    currentFacts.clear();
    ::currentFact = "";
    xSemaphoreGive(_factsMutex);
  } else {
    currentFacts.clear();
    ::currentFact = "";
  }
  _displayFactsAreAi = false;
  _failoverNotified = false;
  _notifiedItunesAiFallback = false;
  _notifiedFlacItunesAuto = false;
  _notifiedFactExhaustedToast = false;
  _notifiedHeapDeferToast = false;  // новый режим — снова разрешён один toast «мало памяти» для авто
  // НЕ resumeDeferredCoverDownload() — при смене режима Cover тоже отменяем снаружи.
  // НЕ isRequestInProgress = false — см. комментарий у _factsSslEpoch в TrackFacts.h.
  Serial.println("[TrackFacts] abortForModeSwitch: queues cleared, sslEpoch bumped");
}

// ============================================================================
extern bool g_coverTaskRunning;
extern volatile bool g_modeSwitching;  // Флаг переключения режимов
extern volatile unsigned long g_modeSwitchTime;  // Timestamp переключения
extern bool isModeSwitchCooldown();  // Проверка cooldown периода
extern bool isSafeForSSL();  // Проверка heap/WiFi/cooldown

// Основной цикл (вызывается каждую секунду)
// ============================================================================
void TrackFacts::on_ticker() {
  if (!_enabled) return;

  const bool playing = (player.status() == PLAYING);
  static bool prevPlaying = false;

  // [v0.4.1] При остановке потока — прерываем фоновый запрос фактов (как при смене трека) и сбрасываем UI.
  if (!playing) {
    if (prevPlaying) {
      _factsSslEpoch++;
      manualRequestPending = false;
      manualRequestDeadlineMs = 0;
      manualRequestAtMs = 0;
      _manualMoreFragmentsUi = false;
    }
    prevPlaying = false;

    if (::currentFact.length() > 0) {
      if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentFacts.clear();
        ::currentFact = "";
        xSemaphoreGive(_factsMutex);
      } else {
        currentFacts.clear();
        ::currentFact = "";
      }
      _displayFactsAreAi = false;
      factLoaded = false;
      factRequestCounter = 0;
    }
    return;
  }

  prevPlaying = true;

  uint32_t now = millis();
  if (now - lastTickerCheck < 1000) return; // Проверка раз в секунду
  lastTickerCheck = now;

  // Дедлайн ручного запроса — ДО раннего return по обложке: иначе пока CoverDL блокирует тикер,
  // отмена не проверялась, а после обложки срабатывала уже во время FactsTask (ложный тост).
  if (manualRequestPending && manualRequestDeadlineMs > 0 && now > manualRequestDeadlineMs &&
      !isRequestInProgress) {
    String rejectReason;
    if (!canRunManualLowBufferRetry(now, &rejectReason)) {
      if (rejectReason.length() == 0) {
        rejectReason =
            "Запрос факта отклонён: поток нестабилен или буфер слишком мал. Попробуйте позже.";
      }
      sendStatus(rejectReason.c_str(), true);
      manualRequestPending = false;
      manualRequestDeadlineMs = 0;
      _manualMoreFragmentsUi = false;
    } else if (!_manualDeadlineExtended) {
      // Условия буфера уже ОК, но тикер долго не доходил до fetch (обложка) — продлеваем окно.
      // Анти-спам I2: только ОДИН раз на нажатие. Раньше продление шло бесконечно, если fetch
      // блокировала память (canRunManualLowBufferRetry кучу не смотрит) — ⏳ висел до смены трека,
      // а isRequestPending() всё это время держал и CoverDL.
      _manualDeadlineExtended = true;
      manualRequestDeadlineMs = now + 30000u;
    } else {
      // Второе истечение дедлайна: снимаем очередь с прежним текстом отказа по дедлайну (текст не новый).
      sendStatus("Запрос факта отклонён: поток нестабилен или буфер слишком мал. Попробуйте позже.", true);
      manualRequestPending = false;
      manualRequestDeadlineMs = 0;
      _manualMoreFragmentsUi = false;
    }
  }

  // Обновляем окно стабильности потока для сценария ручного low-buffer retry.
  // Ручной запрос допускается при буфере >= MANUAL_FACTS_MIN_BUFFER_PERCENT,
  // если поток стабилен MANUAL_FACTS_STABLE_WINDOW_MS (см. options.h).
  updateManualStabilityWindow(now);

  // Смену трека обрабатываем ДО пауз Cover/8с — иначе WebUI залипает на старом теге + ⏳,
  // пока on_ticker не дойдёт до детекта (Cover deferred / Facts SSL busy).
  if (metaTitle.length() > 0 && metaTitle != lastTitle) {
    lastTitle = metaTitle;

    // Парсинг артиста и названия из метаданных
    String artist = metaArtist;
    String title = metaTitle;
    if (artist.length() == 0 && metaTitle.indexOf(" - ") > 0) {
      int separatorPos = metaTitle.indexOf(" - ");
      artist = metaTitle.substring(0, separatorPos);
      title = metaTitle.substring(separatorPos + 3);
    } else if (artist.length() > 0 && metaTitle.indexOf(" - ") > 0) {
      int separatorPos = metaTitle.indexOf(" - ");
      title = metaTitle.substring(separatorPos + 3);
    }
    stripRadioStreamPromoTail(artist);
    stripRadioStreamPromoTail(title);
    lastArtist = artist;
    lastParsedTitle = title;
    trackChangeMs = now;
    autoRequestDone = false;
    manualRequestPending = false;
    manualRequestAtMs = 0;
    manualRequestDeadlineMs = 0;
    manualStableSinceMs = 0;
    _manualMoreFragmentsUi = false;

    // --- ПРОВЕРКА [v0.3]: Игнорируем метаданные, если они совпадают с названием станции ---
    // Это предотвращает лишние запросы и сброс состояния, когда станция вместо песни шлет свое название.
    String stationName = String(config.station.name);
    stationName.trim();
    if (lastParsedTitle.equalsIgnoreCase(stationName) || lastParsedTitle.length() < 3) {
      // Это либо название станции, либо слишком короткая строка. Игнорируем.
      lastTitle = metaTitle; // Чтобы не зацикливаться, помечаем заголовок как обработанный
      return;
    }

    // Очистка локального состояния для нового трека
    if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      currentFacts.clear();
      ::currentFact = "";  // Очистка глобальной переменной (убирает текст с WebUI)
      xSemaphoreGive(_factsMutex);
    }
    _displayFactsAreAi = false;
    factLoaded = false;
    // Инвалидируем летящий FactsTask прошлого трека (результат отбросится; UI ⏳ гаснет через epoch).
    _factsSslEpoch++;
    // НЕ isRequestInProgress = false — пока TLS жив, CoverDL ждёт; UI смотрит isRequestPendingForUi().
    resumeDeferredCoverDownload();
    factLoadTime = now;
    factRequestCounter = 0;
    _failoverNotified = false;
    _notifiedItunesAiFallback = false;
    _notifiedFlacItunesAuto = false;
    _notifiedFactExhaustedToast = false;
    _notifiedHeapDeferToast = false;  // новый трек по ICY — снова разрешён один toast «мало памяти» для авто
    lastStatusMs = 0;

    // --- Поиск фактов в файловом кэше (читаем "на лету" из файла) ---
    String trackKey = generateTrackKey(lastArtist, lastParsedTitle);

    // Временно храним загруженные факты, чтобы не блокировать надолго основную петлю
    std::vector<String> tmpFacts;
    if (getFactsFromCache(trackKey, tmpFacts) &&
        factsCacheUsableForCurrentProvider(currentSource, geminiApiKey, tmpFacts)) {
      if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentFacts = tmpFacts;

        // Формируем строку с разделителями ### для передачи в JS
        String allFacts = "";
        for (size_t i = 0; i < currentFacts.size(); i++) {
          if (i > 0) allFacts += "###";
          allFacts += currentFacts[i];
        }
        ::currentFact = allFacts;
        xSemaphoreGive(_factsMutex);

        // Если в кэше уже достаточно фактов, помечаем как "загружено"
        if (currentFacts.size() >= (size_t)factsPerTrack) {
          factLoaded = true;
          autoRequestDone = true; // Кэш уже есть, авто-запрос не нужен
        }
        Serial.printf("[TrackFacts] Cache hit: %d facts for '%s'\n",
                      currentFacts.size(), trackKey.c_str());
        Serial.printf("[TrackFacts] UI Updated (Cache): %s\n", allFacts.c_str());
        _displayFactsAreAi =
            isIterativeSource(_effectiveSource) && !factsStringsLookLikeCatalog(tmpFacts);
      }
    } else if (!tmpFacts.empty()) {
      Serial.println("[TrackFacts] Cache skip: в файле iTunes/Last.fm, выбран AI — запросим LLM");
    }
  }

  // Если в данный момент скачивается обложка — ждем.
  // Это предотвращает конфликт за оперативную память (SSL) и LittleFS.
  if (g_coverTaskRunning) {
    factLoadTime = now; // Смещаем время начала, чтобы не стартануть сразу после обложки
    return;
  }

  // [v0.3.3] Пауза 8 секунд после завершения загрузки обложки для ПЕРВОГО авто-запроса.
  // Ручной запрос (окно фактов) не блокируем — иначе пользователь жмёт обложку много раз вхолостую.
  if (now - factLoadTime < 8000 && !factLoaded && factRequestCounter == 0 &&
      !manualRequestPending) {
    return;
  }

  if (metaTitle.length() == 0) return;

  // --- Логика последовательного накопления фактов ---
  // [v0.4.1] ОГРАНИЧЕНИЕ: Для iTunes/LastFM (не итеративных) — один запрос.
  // Повторные запросы для метаданных не имеют смысла, они возвращают то же самое.
  if (!factLoaded && currentFacts.size() < (size_t)factsPerTrack && !isRequestInProgress) {
    uint32_t now = millis();
    const bool allowAutoTrackFetch = TRACKFACTS_AUTO_FETCH_ENABLE;
    bool canAuto =
        allowAutoTrackFetch && (!autoRequestDone && (now - trackChangeMs >= FACT_FETCH_DELAY));
    bool canManual = (manualRequestPending && now >= manualRequestAtMs);
    // Авто: лимит попыток factRequestCounter; ручной — всегда можно повторить после сбоя gate/heap.
    const bool mayStartFetch =
        canManual || (canAuto && factRequestCounter < factsPerTrack);

    // Выровнено с CoverDL (startsWith '[' / служебные бейджи):
    // раньше indexOf('[') блокировал и нормальные треки с «[feat. …]» — fetch не стартовал,
    // а ⏳ висел до дедлайна (типичный кейс SD #39).
    if (mayStartFetch && !isFactsBlockedSystemTitle(lastParsedTitle)) {
      checkFailover();
      const bool iterativeContinueBatch =
          factsPerTrack > 1 &&
          isIterativeSource(_effectiveSource) &&
          !factLoaded &&
          currentFacts.size() > 0 &&
          currentFacts.size() < (size_t)factsPerTrack;
      // Ранее на FLAC авто-AI блокировали; сейчас эксперимент — авто-AI разрешён и на FLAC.
      // (Ручной запрос и раньше был доступен.) Позже — выключатель в WebUI.
      // ВАЖНО: используем _effectiveSource (после failover без ключа → ITUNES).
      /* Per-provider авто: LLM — opt-in (макрос true); iTunes/Last.fm — opt-out (макрос false).
       * Ручной запрос (canManual) этими флагами не блокируется. */
      const bool autoGeminiBlocked =
        (canAuto && _effectiveSource == FactSource::GEMINI && !TRACKFACTS_AUTO_GEMINI_ENABLE);
      const bool autoDeepSeekBlocked =
        (canAuto && _effectiveSource == FactSource::DEEPSEEK && !TRACKFACTS_AUTO_DEEPSEEK_ENABLE);
      const bool autoGroqBlocked =
        (canAuto && _effectiveSource == FactSource::GROQ && !TRACKFACTS_AUTO_GROQ_ENABLE);
      const bool autoItunesBlocked =
        (canAuto && _effectiveSource == FactSource::ITUNES && !TRACKFACTS_AUTO_ITUNES_ENABLE);
      const bool autoLastFmBlocked =
        (canAuto && _effectiveSource == FactSource::LASTFM && !TRACKFACTS_AUTO_LASTFM_ENABLE);
      if (autoGeminiBlocked || autoDeepSeekBlocked || autoGroqBlocked ||
          autoItunesBlocked || autoLastFmBlocked) {
        if (!iterativeContinueBatch) {
          autoRequestDone = true;  // Не повторяем автопопытки до следующей смены трека.
          if (now - lastStatusMs > 5000) {
            lastStatusMs = now;
          }
          canAuto = false;
          if (!canManual) return;
        }
      }
      /* FLAC + авто-AI: раньше блокировали ради стабильности звука.
       * Эксперимент: разрешаем авто-AI и на FLAC (ручной и так был доступен).
       * Позже — выключатель в WebUI. */

      // Не запускаем SSL-запрос, пока аудио-буфер ниже порога (в % от размера буфера).
      // Факты разрешены при заполнении >= MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT
      // (значение — options.h / myoptions.h, без «застывшего» процента в комментарии).
      if (player.isRunning()) {
        uint32_t total = player.getInBufferSize();
        uint32_t minFilled = total ? (total * (uint32_t)MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT / 100u) : 0u;
        if (player.inBufferFilled() < minFilled) {
          // Для АВТО-режима сохраняем старое строгое поведение: низкий буфер = отложить.
          // Для РУЧНОГО запроса — special retry: MANUAL_FACTS_MIN_BUFFER_PERCENT
          // + стабильный поток MANUAL_FACTS_STABLE_WINDOW_MS.
          bool allowManualLowBuffer = false;
          String lowBufManualReject;
          if (canManual) {
            allowManualLowBuffer = canRunManualLowBufferRetry(now, &lowBufManualReject);
            if (!allowManualLowBuffer && now - lastStatusMs > 3000) {
              // Если ручной запрос пока нельзя выполнить, сообщаем пользователю причину.
              // Сообщение отправляем не чаще, чем раз в 3 секунды, чтобы не спамить toast.
              sendStatus(lowBufManualReject.c_str(), true);
              lastStatusMs = now;
            }
          }
          if (!allowManualLowBuffer) {
            // Serial отдельно от lastStatusMs: раз в 30 с — постоянное «тонкое» аудио иначе засоряло монитор каждые 5 с.
            static uint32_t lastLowBufferSerialMs = 0;
            if (now - lastLowBufferSerialMs >= 30000u) {
              Serial.println("[TrackFacts] Запрос отложен: низкий буфер потока (приоритет — стабильность звука)");
              lastLowBufferSerialMs = now;
            }
            if (now - lastStatusMs > 5000) {
              lastStatusMs = now;
            }
            // Ждём стабилизацию потока — запрос в очереди, WebUI может показывать ⏳.
            // Остальные отказы: снимаем очередь, иначе /api/current-fact остаётся pending и ⏳ не исчезает.
            if (canManual && lowBufManualReject.length() > 0) {
              if (lowBufManualReject.indexOf("ждём стабилизацию") < 0 &&
                  lowBufManualReject.indexOf("поток ещё не стабилен") < 0) {
                manualRequestPending = false;
                manualRequestDeadlineMs = 0;
                _manualMoreFragmentsUi = false;
              }
            }
            return;
          }
        }
      }

      // Перед сетевым запросом проверяем безопасность сети именно для фактов.
      // HTTP-прокси к VPS — мягкий гейт; прямое HTTPS — прежний SSL-гейт.
      const bool httpProxyPath =
          trackFactsIsAiSource(_effectiveSource) &&
          trackFactsAiUsesHttpProxy(_effectiveSource);
      const bool netGateOk = httpProxyPath ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts();
      // Политика «звук первым»: прямой HTTPS фактов при HTTPS-потоке запрещён (прокси-путь — без TLS, не трогаем).
      const bool httpsAudioBlock = !httpProxyPath && isHttpsAudioBlockingFactsTls();
      // Анти-спам I2: до xTaskCreate запас «гейт + стек + slack».
      // Стек в запас НЕ кладём:
      //  - httpsRiskSoft — accept на HTTPS-аудио (диалог/свайп);
      //  - ручной запрос на HTTP-аудио — нет второго TLS с потоком; иначе free≈77K
      //    при need=78K даёт ложный отказ (без proxy на http раньше работало).
      const uint32_t freeNow = ESP.getFreeHeap();
      const bool httpsRiskSoft =
          !httpProxyPath && isWebAudioStreamHttps() && !httpsAudioBlock;
      const bool httpManualSoft =
          canManual && !isWebAudioStreamHttps();
      const uint32_t needWithTask = factsNetGateMinFreeHeap(httpProxyPath)
          + ((httpsRiskSoft || httpManualSoft) ? 0u : (uint32_t)FACTS_TASK_STACK_BYTES)
          + (uint32_t)FACTS_TASK_HEAP_SLACK;
      const bool netOk = netGateOk && freeNow >= needWithTask;
      bool fetchStarted = false;
      const bool hardBlock = !netOk &&
          (httpsAudioBlock || (freeNow < needWithTask && !g_modeSwitching && !g_coverTaskRunning));
      if (netOk) {
        _pendingFetchIsManual = canManual;
        transientNotifier.clear(TN_FACT_DEFERRED);
        // Accept риска НЕ сбрасываем здесь: FactsTask/провайдеры ещё зовут
        // isSafeForSSLForFacts(); сброс — в конце этой попытки (fetchTask / create fail).
        fetchFact(lastArtist, lastParsedTitle);
        factLoadTime = now;
        fetchStarted = true;
      } else if (hardBlock) {
        factsClearHttpsTlsRiskAccept();
        // Serial — не чаще раза в 30 с (иначе при авто-режиме строка шла бы каждую секунду).
        static uint32_t lastHardBlockLogMs = 0;
        if (lastHardBlockLogMs == 0 || now - lastHardBlockLogMs >= 30000u) {
          Serial.printf("[TrackFacts] Запрос не запущен: %s (free=%u need=%u manual=%d)\n",
                        httpsAudioBlock ? "HTTPS-поток, второй TLS запрещён"
                                        : "не хватит памяти на задачу запроса",
                        (unsigned)freeNow, (unsigned)needWithTask, canManual ? 1 : 0);
          lastHardBlockLogMs = now;
        }
        // Тексты — прежние, без новых формулировок:
        //  - «глубокая» нехватка (старая ветка free<35000 / est<20000) — её прежний текст, не красный;
        //  - остальное (HTTPS-поток или памяти не хватит на задачу) — текст, который раньше
        //    показывал abort внутри FactsTask в этой же ситуации (красный).
        const bool deepLow = (freeNow < 35000u || cachedMaxAllocHeap() < 20000u);
        const bool useLegacyLowText = deepLow && !httpsAudioBlock;
        const char* msg = useLegacyLowText
            ? "Сейчас мало памяти для запроса факта. Музыка играет — подождите и повторите."
            : "Сейчас мало свободной памяти для запроса факта. Музыка продолжает играть.";
        const bool msgIsError = !useLegacyLowText;
        if (canManual) {
          // Ручной: один toast на нажатие, очередь снимаем (⏳ гаснет, CoverDL больше не ждёт факт).
          dropManualRequestWithToast(msg, msgIsError);
        } else if (canAuto) {
          if (httpsAudioBlock) {
            // На HTTPS-станции авто-запрос до смены трека не повторяем и не показываем toast.
            autoRequestDone = true;
          } else if (!_notifiedHeapDeferToast) {
            // Авто при нехватке памяти: тихо ждём (задача не создаётся), toast — один раз за трек.
            sendStatus(msg, msgIsError);
            _notifiedHeapDeferToast = true;
          }
        }
      } else {
        // Детальные логи в монитор порта для анализа задержек
        if (now - lastStatusMs > 3000) {
          if (g_modeSwitching) {
            uint32_t timeLeft = 0;
            if (now < g_modeSwitchTime + 30000) timeLeft = (g_modeSwitchTime + 30000 - now) / 1000;
            Serial.printf("[TrackFacts] Запрос отложен: Блокировка режима (еще %u сек)\n", timeLeft);
            sendStatus("Режим переключается. Ожидание стабильности...");
          } else if (g_coverTaskRunning) {
            Serial.println("[TrackFacts] Запрос отложен: Загрузка обложки на Core 1");
            sendStatus("Загрузка обложки. Факты в очереди...");
          } else if (ESP.getFreeHeap() < 35000 || cachedMaxAllocHeap() < 20000) {
            Serial.printf("[TrackFacts] Запрос отложен: мало кучи (free=%u est=%u)\n",
                          (unsigned)ESP.getFreeHeap(), (unsigned)cachedMaxAllocHeap());
            sendStatus("Сейчас мало памяти для запроса факта. Музыка играет — подождите и повторите.");
          } else if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[TrackFacts] Запрос отложен: Нет соединения WiFi");
            sendStatus("WiFi не подключен. Запрос отложен.");
          } else if (!isWebStreamReadyForFacts()) {
            /*
             * Это не ошибка запроса: устройство сознательно
             * сохраняет сеть и буфер для музыки. Ручной запрос остаётся pending.
             * Сообщение не чаще 15 с — убран минутный spam.
             */
            static uint32_t lastAudioPriorityLogMs = 0;
            if (now - lastAudioPriorityLogMs >= 15000u) {
              Serial.println("[TrackFacts] Запрос отложен: приоритет восстановления звука");
              lastAudioPriorityLogMs = now;
            }
            if (canManual) {
              transientNotifier.show(
                  TN_FACT_DEFERRED,
                  "Факт отложен — приоритет звуку. Повторим автоматически.");
            }
          }
          lastStatusMs = now;
        }
      }
      // Для AI с factsPerTrack>1 нужны отдельные запросы на каждый факт; autoRequestDone=true
      // после первого же старте блокировал бы все следующие (canAuto стал бы ложным навсегда).
      if (canAuto && fetchStarted) {
        if (!isIterativeSource(_effectiveSource) || factsPerTrack <= 1) {
          autoRequestDone = true;
        }
      }
      // Ручную очередь снимаем только когда FactsTask действительно создан.
      // При audio-priority/heap/Wi-Fi defer запрос пользователя не теряется.
      if (canManual && fetchStarted) {
        manualRequestPending = false;
        manualRequestDeadlineMs = 0;
      }
    }
  }

  // Фактов нет, дальше запросы не планируются: различаем «источник ответил, данных нет» и «запросы исчерпаны без успеха».
  // При factsPerTrack==1 и сбое AI factLoaded остаётся false, но factRequestCounter уже 1 — это не «лимит API», а сеть/SSL/ключ.
  // Не показываем сообщение, пока FactsTask в полёте: счётчик уже увеличен, а currentFact ещё пуст.
  if (!isRequestInProgress && (factLoaded || factRequestCounter >= factsPerTrack) && currentFacts.empty() && ::currentFact.length() == 0) {
    if (factLoaded) {
      ::currentFact = (currentLanguage == FactLanguage::RUSSIAN) ?
          "Факты не найдены (источник не вернул данные по этому треку)." :
          "No facts found for this track from the selected source.";
    } else if (!_notifiedFactExhaustedToast) {
      // Язык UI: «Русский» и «Auto» — русские строки (Auto в getLanguageCode() тоже ru).
      const bool ruUi =
          (currentLanguage == FactLanguage::RUSSIAN || currentLanguage == FactLanguage::AUTO);
      // iTunes не требует API-ключа — отдельный текст без упоминания ключа (не путать пользователя).
      FactProvider* exhaustedProv = getProvider(_effectiveSource);
      const bool provNeedsKey = (exhaustedProv != nullptr && exhaustedProv->needsApiKey());
      const char* exhaustedMsg = nullptr;
      if (provNeedsKey) {
        exhaustedMsg = ruUi
            ? "Факт не получен: проверьте Wi‑Fi, ключ API провайдера и повторите чуть позже."
            : "Fact not fetched: check Wi‑Fi, provider API key, and try again shortly.";
      } else {
        // iTunes и прочие источники без ключа — только сеть/очередь/SSL.
        exhaustedMsg = ruUi
            ? "Факт не получен: проверьте Wi‑Fi и повторите чуть позже (каталог iTunes временно недоступен)."
            : "Fact not fetched: check Wi‑Fi and try again shortly (iTunes catalog temporarily unavailable).";
      }
      sendStatus(exhaustedMsg, true);
      _notifiedFactExhaustedToast = true;
    }
  }
}

void TrackFacts::processMetadata() {
  // Логика перенесена в on_ticker
}

// ============================================================================
// Failover: проверка API ключа и переключение на iTunes
// ============================================================================

// [v0.4.0] Получить провайдер по индексу FactSource (с проверкой границ)
FactProvider* TrackFacts::getProvider(FactSource src) {
  int idx = static_cast<int>(src);
  if (idx < 0 || idx > 5) return nullptr;
  return _providers[idx];
}

// [v0.4.0] Синхронизировать API ключ, язык и proxy URL со всеми провайдерами
void TrackFacts::syncProviderSettings() {
  ProviderLanguage pLang = static_cast<ProviderLanguage>(static_cast<int>(currentLanguage));
  // Effective proxy: веб EEPROM приоритетнее compile-time TRACKFACTS_* .
  const String proxyGemini =
      trackFactsEffectiveProxyGemini(config.store.trackFactsProxyGemini);
  const String proxyGroq =
      trackFactsEffectiveProxyGroq(config.store.trackFactsProxyGroq);
  const String proxyDeepSeek =
      trackFactsEffectiveProxyDeepSeek(config.store.trackFactsProxyDeepSeek);
  for (int i = 0; i < 6; i++) {
    if (_providers[i]) {
      _providers[i]->setApiKey(geminiApiKey);
      _providers[i]->setLanguage(pLang);
      if (i == static_cast<int>(FactSource::GEMINI)) {
        _providers[i]->setProxyBase(proxyGemini);
      } else if (i == static_cast<int>(FactSource::GROQ)) {
        _providers[i]->setProxyBase(proxyGroq);
      } else if (i == static_cast<int>(FactSource::DEEPSEEK)) {
        _providers[i]->setProxyBase(proxyDeepSeek);
      } else {
        _providers[i]->setProxyBase(String());
      }
    }
  }
}

// Проверка необходимости failover перед каждым запросом.
// Если провайдер требует ключ, а ключ пуст — переключаемся на iTunes.
// [v0.4.2] Теперь уведомляем пользователя о причине переключения.
void TrackFacts::checkFailover() {
  FactProvider* provider = getProvider(currentSource);
  if (provider && provider->needsApiKey() && geminiApiKey.length() == 0) {
    // Используем iTunes вместо провайдера без ключа
    _effectiveSource = FactSource::ITUNES;
    _failoverActive = true;
    
    // Уведомляем пользователя один раз за песню (не сразу после смены провайдера — даём время ввести ключ)
    if (!_failoverNotified && millis() >= _suppressFailoverToastUntilMs) {
      char buf[128];
      snprintf(buf, sizeof(buf), "%s: нет API ключа. Переключено на iTunes.", provider->name());
      sendStatus(buf, true); // true = ошибка (красный тост)
      _failoverNotified = true;
    }
  } else {
    // Ключ в порядке или провайдер не требует ключа — используем выбранный
    _effectiveSource = currentSource;
    _failoverActive = false;
  }
}

// Анти-спам I2: снять ручной запрос из очереди и показать ОДИН toast с причиной.
// Используется для отказов, которые сами по себе не пройдут в ближайшие секунды
// (памяти не хватит даже на задачу запроса; поток HTTPS — второй TLS запрещён политикой).
// Раньше такой запрос оставался pending и порождал toast каждые 3–10 с до смены трека.
void TrackFacts::dropManualRequestWithToast(const char* msg, bool isError) {
  if (msg != nullptr && msg[0] != '\0') {
    sendStatus(msg, isError);
  }
  manualRequestPending = false;
  manualRequestAtMs = 0;
  manualRequestDeadlineMs = 0;
  _manualMoreFragmentsUi = false;
}

// [v0.4.2] Метод отправки статуса в WebUI через WebSocket
void TrackFacts::sendStatus(const char* msg, bool isError) {
  if (msg == nullptr) return;
  
  // Формируем JSON пакет: {"toast": "текст", "isErr": 0/1}
  // Ограничиваем длину msg (%.200s), чтобы не переполнить buf и не раздуть WebSocket-кадр.
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"toast\": \"%.200s\", \"isErr\": %d}", msg, isError ? 1 : 0);
  
  // [v0.4.4] Отправляем всем подключенным веб-клиентам
  websocket.textAll(buf);
  
  // Cover-SS (свайп): та же причина на TFT, чтобы не смотреть только в веб.
  trackFactsShowDisplayToast(msg, isError ? 5000u : 4000u, false);
  
  // Дублируем в Serial для монитора
  Serial.printf("[TrackFacts] Toast: %s\n", msg);
}

// ============================================================================
// Определяет, нужно ли кэшировать факт от данного провайдера
// Кэшируем AI (Gemini, DeepSeek, Groq) и iTunes
// ============================================================================
bool TrackFacts::shouldCacheFact(FactSource source) const {
  return (source == FactSource::GEMINI ||
          source == FactSource::DEEPSEEK ||
          source == FactSource::GROQ ||
          source == FactSource::ITUNES);
}

// Порождает ли источник по одному факту за запрос (требуется несколько запросов для накопления)
static bool isIterativeSource(FactSource source) {
  return (source == FactSource::GEMINI || source == FactSource::DEEPSEEK ||
          source == FactSource::GROQ);
}

static bool factsStringsLookLikeCatalog(const std::vector<String>& facts) {
  for (const auto& f : facts) {
    String t = f;
    t.trim();
    if (t.length() == 0) continue;
    if (t.startsWith("iTunes:")) return true;
    if (t.startsWith("Last.fm")) return true;
  }
  return false;
}

/**
 * Файловый кэш ключится только artist|title — без провайдера.
 * Если в файле лежит iTunes/Last.fm (fallback или слот 2 серии), а в WebUI выбран Gemini/DeepSeek/Groq,
 * кэш игнорируем — иначе сеть к AI не вызывается и пользователь «всегда видит iTunes».
 */
static bool factsCacheUsableForCurrentProvider(FactSource userSource, const String& apiKey,
                                               const std::vector<String>& facts) {
  if (facts.empty()) return false;
  if (!trackFactsIsAiSource(userSource)) return true;
  if (apiKey.length() == 0) return true; // failover без ключа → iTunes ожидаем
  return !factsStringsLookLikeCatalog(facts);
}

// ============================================================================
// Запуск задачи получения факта (в отдельном потоке на Core 1)
// ============================================================================
void TrackFacts::fetchFact(const String& artist, const String& title) {
  if (isRequestInProgress) {
    Serial.println("[TrackFacts] Skip: previous request still in progress (wait for POST/SSL to finish)");
    return;
  }
  // Сразу блокируем повторный вход из on_ticker (иначе два одобрения до xTaskCreate → два FactsTask).
  isRequestInProgress = true;
  // Снимок epoch для UI: после смены трека (_factsSslEpoch++) ⏳ гаснет, хотя TLS ещё доживает.
  _activeRequestEpoch = _factsSslEpoch;

  // --- Проверка failover перед запросом ---
  checkFailover();

  // Политика WEB+FLAC: раньше авто-AI подменяли на iTunes; эксперимент — оставляем выбранный AI.
  // (Ручной запрос и так шёл в AI.) Позже — выключатель авто на FLAC в WebUI.

  TaskParams* params = new TaskParams;
  params->artist = artist;
  params->title = title;
  params->instance = this;
  params->sslEpoch = _factsSslEpoch;
  // Номер слота серии = сколько фактов уже в RAM + 1 (не factRequestCounter — он растёт только после gate).
  {
    uint8_t slot1 = 1;
    if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      slot1 = (uint8_t)(currentFacts.size() + 1);
      if (slot1 < 1) slot1 = 1;
      xSemaphoreGive(_factsMutex);
    }
    params->factIndex1 = slot1;
  }
  params->factTotal = factsPerTrack;
  params->priorFactsBrief = "";
  params->isManual = _pendingFetchIsManual;
  if (factsPerTrack >= 2 && isIterativeSource(_effectiveSource)) {
    if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      const size_t cap = 450;
      for (const auto& f : currentFacts) {
        if (params->priorFactsBrief.length() >= cap) break;
        String part = f;
        part.trim();
        if (part.length() > 120) part = part.substring(0, 120) + "...";
        if (params->priorFactsBrief.length() > 0) params->priorFactsBrief += " | ";
        params->priorFactsBrief += part;
        if (params->priorFactsBrief.length() > cap) {
          params->priorFactsBrief = params->priorFactsBrief.substring(0, cap);
          break;
        }
      }
      xSemaphoreGive(_factsMutex);
    }
  }

  // Сетевой запрос фактов выполняем на Core 0 (рядом с playerTask), приоритет 1.
  // Раньше задача жила на Core 1 — там она во время SSL handshake (~2 сек, CPU-bound mbedtls)
  // конкурировала за процессор с loopTask (Arduino main) и AsyncTCP (WebSocket): браузер
  // мог получать pong с опозданием и AsyncTCP _poll рвал WS-сокет (визуально — "веб виснет
  // на 30 сек", потом сам поднимается). Перенос на Core 0 разгружает Core 1.
  //
  // Попытка такого переноса в v0.8.77 ловила редкий assert в lwIP (pbuf_free) — он возникал,
  // когда параллельно playerTask читал из tcp_recv той же станции. В текущей сборке у нас уже
  // включён STATION_SWITCH_COOLDOWN_MS / isSafeForSSLForFacts() (см. config.cpp): FactsTask
  // НЕ стартует одновременно со свежим подключением станции, и low-buffer guard в on_ticker
  // дополнительно откладывает запрос пока поток не стабилизирован — гонки с pbuf_free больше нет.
  //
  // Размер стека оставляем 16KB (FACTS_TASK_STACK_BYTES, options.h) — безопасно для JSON/HTTP.
  // Тот же макрос учитывается в on_ticker как запас памяти ДО создания задачи.
  const BaseType_t taskOk = xTaskCreatePinnedToCore(
    fetchTask,
    "FactsTask",
    FACTS_TASK_STACK_BYTES,
    params,
    1, // Приоритет 1 (как CoverDL/playerTask) — иначе TLS handshake CPU-starved → сервер EOF
    NULL,
    FACTS_TASK_CORE_ID  // myoptions.h: 0 или 1 — см. комментарий FACTS_TASK_CORE_ID
  );
  if (taskOk != pdPASS) {
    Serial.println("[TrackFacts] Failed to create FactsTask (low heap?)");
    factsClearHttpsTlsRiskAccept();
    isRequestInProgress = false;
    delete params;
    sendStatus("Не удалось запустить запрос факта (мало памяти). Подождите и повторите.", true);
  }
}

// ============================================================================
// Задача FreeRTOS для сетевых запросов (ядро: FACTS_TASK_CORE_ID в myoptions.h / options.h)
// [v0.4.0] Рефакторинг: делегируем запрос провайдеру через интерфейс FactProvider
// ============================================================================
// vTaskDelete не вызывает деструкторы — accept риска сбрасываем явно на каждом выходе.
#define TRACKFACTS_TASK_EXIT(inst, params) do { \
  factsClearHttpsTlsRiskAccept(); \
  (inst)->isRequestInProgress = false; \
  resumeDeferredCoverDownload(); \
  delete (params); \
  vTaskDelete(NULL); \
} while (0)
void TrackFacts::fetchTask(void* parameter) {
  TaskParams* p = (TaskParams*)parameter;
  if (p && p->instance) {
    TrackFacts* instance = p->instance;
    const uint32_t taskStartMs = millis();

    // Если факты отключены — выходим без логов и без сетевых запросов.
    if (!instance->_enabled) {
        TRACKFACTS_TASK_EXIT(instance, p);
        return;
    }

    if (p->sslEpoch != instance->_factsSslEpoch) {
      Serial.println("[TrackFacts] Task aborted: stale sslEpoch (station/track changed before SSL)");
      TRACKFACTS_TASK_EXIT(instance, p);
      return;
    }

    // --- ЖЕСТКАЯ ПРОВЕРКА: если трек сменился, немедленно прерываемся ---
    String currentTitle = instance->lastParsedTitle;
    String paramTitle = p->title;
    currentTitle.trim();
    paramTitle.trim();

    if (currentTitle.length() > 0 && !paramTitle.equalsIgnoreCase(currentTitle)) {
        Serial.printf("[TrackFacts] Task aborted: track changed. Req: '%s', Curr: '%s'\n",
                      paramTitle.c_str(), currentTitle.c_str());
        TRACKFACTS_TASK_EXIT(instance, p);
        return;
    }

    // Ждём завершения SNTP-sync и CoverDL — иначе lwIP assert при параллельном DNS.
    // Каждые ~100 мс проверяем epoch: трек сменился → сразу выходим, не держим ⏳/SSL busy до 120 с.
    {
      const uint32_t netWait0 = millis();
      extern bool g_coverTaskRunning;
      while ((timekeeper.busy || g_coverTaskRunning) && (millis() - netWait0) < 120000) {
        if (p->sslEpoch != instance->_factsSslEpoch) {
          Serial.println("[TrackFacts] Task aborted during net-wait: track/station changed");
          TRACKFACTS_TASK_EXIT(instance, p);
          return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
    // Гасим фоновый SNTP перед Facts DNS/HTTPS (только после первого успешного NTP).
    pauseLwIpBackgroundSntpForUserNet();

    // Гейт сети перед запросом: HTTPS — строгий; HTTP-прокси к VPS — мягче (D3).
    // После xTaskCreate куча проседает — ждём восстановления до 8 с, не abort сразу.
    {
      const FactSource planned = instance->_effectiveSource;
      const bool httpProxyPath =
          trackFactsIsAiSource(planned) && trackFactsAiUsesHttpProxy(planned);
      const uint32_t gateWait0 = millis();
      while ((millis() - gateWait0) < 8000) {
        if (p->sslEpoch != instance->_factsSslEpoch) {
          Serial.println("[TrackFacts] Task aborted during gate-wait: track/station changed");
          TRACKFACTS_TASK_EXIT(instance, p);
          return;
        }
        const bool okNow =
            httpProxyPath ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts();
        if (okNow) break;
        vTaskDelay(pdMS_TO_TICKS(80));
      }
      const bool netOkFinal =
          httpProxyPath ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts();
      if (!netOkFinal) {
        Serial.println(httpProxyPath
            ? "[TrackFacts] Task aborted: HTTP-proxy gate (heap/wifi/cooldown)"
            : "[TrackFacts] Task aborted: SSL not safe (heap/wifi/cooldown)");
        const bool audioReadyFinal = isWebStreamReadyForFacts();
        const char* why = audioReadyFinal
            ? "Сейчас мало свободной памяти для запроса факта. Музыка продолжает играть."
            : "Факт отложен — приоритет звуку. Повторим автоматически.";
        // Ручной — всегда тост; авто — один раз за трек (ручной повтор не блокируется).
        if (p->isManual || !instance->_notifiedFactExhaustedToast) {
          if (audioReadyFinal) {
            instance->sendStatus(why, true);
          } else {
            transientNotifier.show(TN_FACT_DEFERRED, why);
          }
          if (!p->isManual) instance->_notifiedFactExhaustedToast = true;
        }
        // Анти-спам I2: повторяем ручной запрос только если причина временная и только ОДИН раз.
        // Память / HTTPS-поток сами не пройдут — повтор давал цикл «создал задачу → abort» раз в ~10 с.
        const bool heapOrPolicyFail =
            (!httpProxyPath && isHttpsAudioBlockingFactsTls()) ||
            ESP.getFreeHeap() < factsNetGateMinFreeHeap(httpProxyPath);
        if (p->isManual && !heapOrPolicyFail && !instance->_manualRequeueUsed) {
          /*
           * Между on_ticker и реальным DNS поток мог просесть.
           * Возвращаем ручной запрос в очередь вместо ложного «нажмите ещё раз»;
           * следующий тик запустит его только после общего окна стабильности.
           */
          instance->_manualRequeueUsed = true;
          instance->manualRequestPending = true;
          instance->manualRequestAtMs = millis() + 2000u;
          instance->manualRequestDeadlineMs = millis() + 90000u;
        }
        TRACKFACTS_TASK_EXIT(instance, p);
        return;
      }
    }

    // [v0.4.0] Синхронизируем настройки (ключ, язык) перед запросом
    instance->syncProviderSettings();

    // Источник по номеру факта в серии (facts per track ≥ 2, выбран AI):
    // 1 → выбранный AI; 2 → сначала iTunes, при ошибке снова AI; 3+ → AI.
    FactSource userAi = instance->currentSource;
    if (!trackFactsIsAiSource(userAi)) {
      userAi = instance->_effectiveSource; // если выбран не AI — как раньше
    }
    FactSource sourceToUse = instance->_effectiveSource;
    const uint8_t slot = p->factIndex1; // 1..N (из currentFacts.size()+1 в fetchFact)
    bool slot2PreferItunes =
        (instance->factsPerTrack >= 2) &&
        trackFactsIsAiSource(userAi) &&
        (slot == 2);
    if (slot2PreferItunes) {
      sourceToUse = FactSource::ITUNES;
      // Слот 2 при N≥2 — штатный iTunes, не сбой выбранного AI (Gemini/Groq/DeepSeek).
      Serial.printf("[TrackFacts] Серия slot=%u/%u: слот 2 → iTunes (задумка серии, AI=%d)\n",
                    (unsigned)slot,
                    (unsigned)instance->factsPerTrack,
                    (int)userAi);
    } else if (trackFactsIsAiSource(userAi) && (slot == 1 || slot >= 3)) {
      // Явно AI-слоты (на случай если failover ранее подменил effective).
      if (instance->geminiApiKey.length() > 0) {
        sourceToUse = userAi;
      }
    }
    FactProvider* provider = instance->getProvider(sourceToUse);

    // После сбойного HTTPS дать lwIP «остыть» перед снятием busy и CoverDL — иначе редкий assert
    // pbuf_free(... p->ref > 0) в Audio::processWebStream при параллельном чтении потока.
    bool needLwIpCoolDown = false;

    if (provider) {
      /*
       * Последний TOCTOU-гейт стоит непосредственно перед
       * provider->fetchFact(), то есть перед его DNS/HTTP. Настройки провайдера
       * и выбор fallback выше могли занять время и изменить состояние потока.
       */
      if (!isWebStreamReadyForFacts()) {
        transientNotifier.show(
            TN_FACT_DEFERRED,
            "Факт отложен — приоритет звуку. Повторим автоматически.");
        // Анти-спам I2: повтор ручного запроса — один раз на нажатие (раньше без ограничения).
        if (p->isManual && !instance->_manualRequeueUsed) {
          instance->_manualRequeueUsed = true;
          instance->manualRequestPending = true;
          instance->manualRequestAtMs = millis() + 2000u;
          instance->manualRequestDeadlineMs = millis() + 90000u;
        }
        TRACKFACTS_TASK_EXIT(instance, p);
        return;
      }
      // Реальный сетевой запрос: считаем попытку только здесь (после gate и xTaskCreate).
      instance->factRequestCounter++;
      Serial.printf("[TrackFacts] Fetching from %s...\n", provider->name());
      FactFetchContext fctx;
      const FactFetchContext* ctxPtr = nullptr;
      if (instance->factsPerTrack >= 2 && isIterativeSource(sourceToUse)) {
        fctx.factIndex1 = p->factIndex1;
        fctx.factTotal = p->factTotal;
        fctx.priorFactsBrief = p->priorFactsBrief;
        ctxPtr = &fctx;
      }
      FactResult result = provider->fetchFact(p->artist, p->title, ctxPtr);
      bool usedItunesFallback = false;
      bool usedItunesAsSeriesSlot2 = false;
      // Сохраняем текст ошибки AI до fallback — если iTunes тоже пуст, тост про ИИ, не про каталог.
      String aiFailMsgForToast;

      // Слот 2 серии AI: iTunes пуст/ошибка → повтор выбранным AI.
      if (slot2PreferItunes && (!result.success || result.fact.length() == 0)) {
        FactProvider* aiProv = instance->getProvider(userAi);
        if (aiProv && instance->geminiApiKey.length() > 0) {
          Serial.printf("[TrackFacts] slot2 iTunes miss → fallback AI %s\n", aiProv->name());
          sourceToUse = userAi;
          provider = aiProv;
          // Для AI-слота контекст серии (не дублировать факт 1).
          FactFetchContext fctx2;
          const FactFetchContext* ctx2 = nullptr;
          if (instance->factsPerTrack >= 2 && isIterativeSource(userAi)) {
            fctx2.factIndex1 = p->factIndex1;
            fctx2.factTotal = p->factTotal;
            fctx2.priorFactsBrief = p->priorFactsBrief;
            ctx2 = &fctx2;
          }
          if (isWebStreamReadyForFacts()) {
            result = aiProv->fetchFact(p->artist, p->title, ctx2);
          } else {
            /*
             * После первого провайдера запас мог просесть:
             * второй DNS/HTTP не запускаем, даже если это полезный fallback.
             */
            result.success = false;
            result.errorMsg = "audio priority deferred";
            transientNotifier.show(TN_FACT_DEFERRED, "Факт отложен — приоритет звуку");
          }
        }
      } else if (slot2PreferItunes && result.success) {
        usedItunesAsSeriesSlot2 = true;
      }

      // Слот 3+ AI-серии (N≥2): снова выбранный LLM, не запасной iTunes.
      // Слот 2 уже отдан iTunes по задумке серии — повтор каталога дал бы дубликат в UI.
      const bool aiSeriesSlot3Plus =
          (instance->factsPerTrack >= 2) &&
          trackFactsIsAiSource(userAi) &&
          (slot >= 3);

      // Запасной iTunes при сети/SSL или временной недоступности AI (5xx, 429).
      // В т.ч. слот 1 серии и ручной запрос: при 503 Google «high demand» сразу даём каталог,
      // а не оставляем пользователя с пустым ⏳ и следующей зависшей попыткой.
      // Ответы 401/403/400 — ключ/запрос: iTunes не подменяем (не маскируем ошибку ключа).
      const String& aiErr = result.errorMsg;
      const bool aiTransientNet =
          aiErr.indexOf("Connection failed") >= 0 || aiErr.indexOf("SSL not safe") >= 0;
      const bool aiTransientHttp =
          aiErr.indexOf("HTTP error: 5") >= 0 || aiErr.indexOf("HTTP error: 429") >= 0;
      if (!result.success &&
          !slot2PreferItunes &&
          !aiSeriesSlot3Plus &&
          (sourceToUse == FactSource::DEEPSEEK || sourceToUse == FactSource::GEMINI ||
           sourceToUse == FactSource::GROQ) &&
          (aiTransientNet || aiTransientHttp)) {
        aiFailMsgForToast = result.errorMsg;
        FactProvider* fb = instance->getProvider(FactSource::ITUNES);
        if (fb) {
          Serial.println("[TrackFacts] AI unreachable, one-shot iTunes fallback");
          // После отказа AI по heap/ssl подождать восстановления кучи и lwIP.
          {
            uint32_t t0 = millis();
            while (!isSafeForSSLForFacts() && (millis() - t0) < 6000) {
              if (p->sslEpoch != instance->_factsSslEpoch) break;
              vTaskDelay(pdMS_TO_TICKS(80));
            }
          }
          vTaskDelay(pdMS_TO_TICKS(1200));
          if (p->sslEpoch != instance->_factsSslEpoch) {
            result.success = false;
            result.errorMsg = "stale epoch";
          } else if (isWebStreamReadyForFacts()) {
            result = fb->fetchFact(p->artist, p->title, nullptr);
            if (result.success && result.fact.length() > 0) {
              usedItunesFallback = true;
              if (!instance->_notifiedItunesAiFallback) {
                instance->sendStatus(
                    "AI временно не ответил; показан факт из запасного каталога.");
                instance->_notifiedItunesAiFallback = true;
              }
            }
          } else {
            result.success = false;
            result.errorMsg = "audio priority deferred";
            transientNotifier.show(TN_FACT_DEFERRED, "Факт отложен — приоритет звуку");
          }
        }
      }

      // Слот 3+ AI-серии: iTunes fallback намеренно не вызывался — лог для диагностики.
      if (!result.success && aiSeriesSlot3Plus &&
          (aiTransientNet || aiTransientHttp)) {
        Serial.printf("[TrackFacts] slot%u: %s недоступен — без iTunes fallback (серия N≥2)\n",
                      (unsigned)slot, provider->name());
      }

      const bool epochStale = (p->sslEpoch != instance->_factsSslEpoch);

      if (!epochStale && result.success && result.fact.length() > 0) {
        // Если плагин выключили пока запрос летел — не обновляем UI
        if (!instance->_enabled) {
          Serial.println("[TrackFacts] Disabled during request — discarding result");
          TRACKFACTS_TASK_EXIT(instance, p);
          return;
        }
        // Кэшируем AI-факты; текст iTunes-fallback / слот 2 серии / каталог при выбранном AI — не в файл.
        const bool mayWriteCache =
            instance->shouldCacheFact(sourceToUse) &&
            !usedItunesFallback &&
            !usedItunesAsSeriesSlot2 &&
            !(trackFactsIsAiSource(instance->currentSource) &&
              !isIterativeSource(sourceToUse));
        if (mayWriteCache) {
          String trackKey = instance->generateTrackKey(p->artist, p->title);
          instance->saveFactToCache(trackKey, result.fact);
        }
        instance->_displayFactsAreAi =
            !usedItunesFallback && !usedItunesAsSeriesSlot2 && isIterativeSource(sourceToUse);
        // Обновляем UI
        instance->updateDisplayFact(result.fact);
        
        // Один запуск не-итеративных провайдеров (iTunes, LastFM) и запасной iTunes после AI
        // возвращает всю информацию за раз. AI-провайдеры (итеративные) требуют 
        // несколько запросов для сбора нужного количества фактов.
        // Слот 2 = iTunes по задумке: НЕ ставим factLoaded, если ещё нужны слоты 3+.
        const bool moreSlotsNeeded =
            instance->currentFacts.size() < (size_t)instance->factsPerTrack;
        if (usedItunesAsSeriesSlot2 && moreSlotsNeeded) {
          // Продолжаем серию (следующий авто/ручной слот — снова AI).
          instance->factLoaded = false;
        } else if (!isIterativeSource(sourceToUse) || usedItunesFallback) {
          instance->factLoaded = true;
        } else if (instance->currentFacts.size() >= (size_t)instance->factsPerTrack) {
          instance->factLoaded = true;  // AI — достаточно фактов собрано
        }
        if (instance->factLoaded && isIterativeSource(sourceToUse)) {
          instance->autoRequestDone = true;
        }
      } else if (epochStale) {
        Serial.println("[TrackFacts] Discarding fact result: station/track changed during fetch (SSL epoch)");
      } else {
        Serial.printf("[TrackFacts] %s failed: %s\n", provider->name(), result.errorMsg.c_str());
        // Понятный тост на веб + TFT (свайп) — и для авто, и для ручного:
        // иначе после 503/SSL пользователь видит вечный ⏳ без объяснения.
        {
          // Если был transient AI и fallback iTunes не спас — поясняем по исходной ошибке ИИ.
          String why = (aiFailMsgForToast.length() > 0) ? aiFailMsgForToast : result.errorMsg;
          if (why.indexOf("SSL not safe") >= 0 || why.indexOf("heap") >= 0) {
            why = "Сейчас мало памяти для запроса факта. Подождите и повторите.";
          } else if (why.indexOf("HTTP error: 503") >= 0 || why.indexOf("HTTP error: 429") >= 0) {
            why = aiFailMsgForToast.length() > 0
                ? "Сервис ИИ перегружен; запасной каталог тоже не ответил. Смените трек или повторите позже."
                : "Сервис ИИ временно перегружен (нет ответа). Попробуйте позже или смените трек.";
          } else if (why.indexOf("HTTP error: 5") >= 0) {
            why = "Сервис ИИ временно недоступен. Попробуйте позже.";
          } else if (why.indexOf("Connection failed") >= 0) {
            why = "Нет связи с сервисом фактов. Проверьте Wi‑Fi и повторите.";
          } else if (why.equals("No API key") ||
                     why.indexOf("HTTP error: 400") >= 0 ||
                     why.indexOf("HTTP error: 401") >= 0 ||
                     why.indexOf("HTTP error: 403") >= 0) {
            // Ключ/авторизация: в Serial остаётся технический код (см. выше [TrackFacts] … failed),
            // пользователю — простая подсказка без HTTP-цифр.
            why = String("Проверьте ключ API ") + provider->name() + " в настройках.";
          } else if (why.length() == 0) {
            why = "Факт не получен. Повторите чуть позже.";
          } else {
            why = String("Факт не получен: ") + why;
          }
          // Анти-спам: один подробный тост на трек при авто-серии; ручной — всегда.
          if (p->isManual || !instance->_notifiedFactExhaustedToast) {
            instance->sendStatus(why.c_str(), true);
            if (!p->isManual) instance->_notifiedFactExhaustedToast = true;
          }
        }
        // [v0.4.1] Если обычный (не AI) провайдер ответил ошибкой (например, 404),
        // больше не пытаемся его насиловать — помечаем как "загружено"
        if (!isIterativeSource(sourceToUse) && !usedItunesAsSeriesSlot2) {
          instance->factLoaded = true;
          instance->autoRequestDone = true;
        }
        // AI-слот 1 серии после 503+неудачный iTunes: не крутить бесконечные слоты 2/3 впустую —
        // останавливаем авто до следующей смены трека (ручной тап всё ещё можно).
        if (isIterativeSource(sourceToUse) && !usedItunesAsSeriesSlot2 &&
            instance->currentFacts.empty()) {
          instance->autoRequestDone = true;
        }
      }
      if (!result.success) {
        needLwIpCoolDown = true;
      }
    } else {
      Serial.printf("[TrackFacts] Unknown/null provider for source %d\n", (int)sourceToUse);
    }

    // Сначала cooldown (пока isRequestInProgress=true) — иначе on_ticker стартует второй FactsTask
    // параллельно с ещё живым первым и зависает Cover/⏳.
    if (needLwIpCoolDown) {
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
    instance->_manualMoreFragmentsUi = false;
    factsClearHttpsTlsRiskAccept();
    instance->isRequestInProgress = false;
  }
  resumeDeferredCoverDownload();
  if (p) delete p;
  vTaskDelete(NULL);
}
#undef TRACKFACTS_TASK_EXIT

// ============================================================================
// === ФАЙЛОВЫЙ КЭШ (File-based Cache) ===
// Замена старого JSON-кэша в RAM на отдельные файлы в LittleFS
// ============================================================================

// DJB2 хэш — тот же алгоритм, что используется для обложек в config.cpp
uint32_t TrackFacts::djb2Hash(const String& str) {
  uint32_t hash = 5381;
  for (size_t i = 0; i < str.length(); ++i) {
    hash = ((hash << 5) + hash) + static_cast<uint8_t>(str[i]);
  }
  return hash;
}

// Генерация нормализованного ключа трека: "artist|title" в нижнем регистре с обрезкой пробелов
String TrackFacts::generateTrackKey(const String& artist, const String& title) {
  String key = artist + "|" + title;
  key.toLowerCase();
  key.trim();
  return key;
}

// Генерация полного пути к файлу кэша: /data/facts/<hash>.txt
String TrackFacts::factsCachePath(const String& trackKey) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s/%08lx.txt",
           FACTS_CACHE_DIR, static_cast<unsigned long>(djb2Hash(trackKey)));
  return String(buf);
}

// Создание папки кэша /data/facts/, если она не существует
void TrackFacts::ensureFactsDir() {
  extern bool littleFsReady;
  if (!littleFsReady) return;

  if (!LittleFS.exists("/data")) {
    LittleFS.mkdir("/data");
  }
  if (!LittleFS.exists(FACTS_CACHE_DIR)) {
    LittleFS.mkdir(FACTS_CACHE_DIR);
    Serial.println("[TrackFacts] Created cache dir: /data/facts/");
  }
}

// ============================================================================
// Чтение фактов из файлового кэша
// Читает файл "на лету" — НЕ загружает весь кэш в RAM!
// Каждая строка файла — отдельный факт. Используем lockLittleFS для потокобезопасности.
// ============================================================================
bool TrackFacts::getFactsFromCache(const String& trackKey, std::vector<String>& facts) {
  extern bool littleFsReady;
  if (!littleFsReady) return false;

  String path = factsCachePath(trackKey);

  // Захватываем семафор LittleFS (макс ожидание 1 сек)
  if (!lockLittleFS(1000)) {
    Serial.println("[TrackFacts] Cache read: failed to lock LittleFS");
    return false;
  }

  if (!LittleFS.exists(path)) {
    unlockLittleFS();
    return false;
  }

  File f = LittleFS.open(path, "r");
  if (!f) {
    unlockLittleFS();
    return false;
  }

  // Читаем строки файла — каждая строка это один факт
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      facts.push_back(line);
    }
  }
  f.close();
  unlockLittleFS();

  return !facts.empty();
}

// ============================================================================
// Сохранение нового факта в файловый кэш
// Дописывает факт как новую строку в файл трека.
// Проверяет дубликаты; в кэш-файле — литерал 10 строк (не TRACKFACTS_MAX_PER_TRACK /
// TRACKFACTS_CONFIG_COUNT_MAX: те задают циклы AI и лимит WebUI/EEPROM).
// Использует lockLittleFS для потокобезопасности.
// ============================================================================
void TrackFacts::saveFactToCache(const String& trackKey, const String& fact) {
  extern bool littleFsReady;
  if (!littleFsReady) return;

  // Захватываем семафор LittleFS (макс ожидание 2 сек)
  if (!lockLittleFS(2000)) {
    Serial.println("[TrackFacts] Cache write: failed to lock LittleFS");
    return;
  }

  ensureFactsDir();
  String path = factsCachePath(trackKey);

  // Читаем существующие факты для проверки дубликатов
  std::vector<String> existing;
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) existing.push_back(line);
      }
      f.close();
    }
  }

  // Проверка дубликатов — если такой факт уже есть, не сохраняем
  for (const auto& e : existing) {
    if (e == fact) {
      unlockLittleFS();
      return;
    }
  }

  // Лимит строк в кэш-файле трека: литерал 10 (отдельно от TRACKFACTS_MAX_PER_TRACK /
  // TRACKFACTS_CONFIG_COUNT_MAX — код ниже сознательно не меняем без отдельной просьбы).
  if (existing.size() >= 10) {
    existing.erase(existing.begin()); // Удаляем самый старый факт
  }
  existing.push_back(fact);

  // Перезаписываем файл с обновлённым содержимым
  File f = LittleFS.open(path, "w");
  if (f) {
    for (const auto& line : existing) {
      f.print(line);
      f.print("\n");
    }
    f.close();

    // Регистрируем файл в индексе и проверяем лимит кэша
    registerFactsCacheFile(path);

    Serial.printf("[TrackFacts] Cached fact to '%s' (%d total)\n",
                  path.c_str(), existing.size());
  }

  unlockLittleFS();
}

// ============================================================================
// Структура записи индекса кэша фактов (аналог CoverCacheEntry из config.cpp)
// ============================================================================
struct FactsCacheEntry {
  uint32_t ts;    // Временная метка (Unix или millis)
  size_t size;    // Размер файла в байтах
  String path;    // Полный путь к файлу
};

// Получение текущего timestamp (аналогично coverTimestamp из config.cpp)
static uint32_t factsTimestamp() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0) && timeinfo.tm_year > 100) {
    return static_cast<uint32_t>(time(nullptr));
  }
  return static_cast<uint32_t>(millis());
}

// Загрузка индексного файла кэша фактов
static void loadFactsIndex(std::vector<FactsCacheEntry>& entries) {
  entries.clear();
  extern bool littleFsReady;
  if (!littleFsReady) return;

  File f = LittleFS.open("/data/facts/index.txt", "r");
  if (!f) return;

  /*
   * Индекс при лимите 1 МБ может содержать тысячи строк. FactsTask и DspTask живут на Core 0:
   * длинный цикл без отдачи процессора вместе с выводом кадра на панель не пускал IDLE0
   * дольше 5 с → task_wdt → перезагрузка (лог 2026-10-08 09:37, backtrace в axsPresentFrame).
   * Каждые 32 строки отдаём один тик планировщику.
   */
  uint32_t lineNo = 0;
  while (f.available()) {
    if ((++lineNo & 31u) == 0) vTaskDelay(1);
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    unsigned long ts = 0;
    unsigned long size = 0;
    char path[96] = {0};
    if (sscanf(line.c_str(), "%lu %lu %95s", &ts, &size, path) == 3) {
      FactsCacheEntry entry;
      entry.ts = static_cast<uint32_t>(ts);
      entry.size = static_cast<size_t>(size);
      entry.path = String(path);
      entries.push_back(entry);
    }
  }
  f.close();
}

// Сохранение индексного файла кэша фактов
static void saveFactsIndex(const std::vector<FactsCacheEntry>& entries) {
  extern bool littleFsReady;
  if (!littleFsReady) return;

  File f = LittleFS.open("/data/facts/index.txt", "w");
  if (!f) return;

  /* Запись большого индекса = серия стираний flash (кэш отключён, чтение PSRAM у DspTask стоит).
   * Между порциями по 32 строки отдаём тик, чтобы IDLE0 успевал сбросить сторожевой таймер. */
  uint32_t entryNo = 0;
  for (const auto& entry : entries) {
    if ((++entryNo & 31u) == 0) vTaskDelay(1);
    f.printf("%lu %lu %s\n",
             static_cast<unsigned long>(entry.ts),
             static_cast<unsigned long>(entry.size),
             entry.path.c_str());
  }
  f.close();
}

// ============================================================================
// Регистрация файла факта в индексе + LRU ротация при превышении 1 МБ
// Аналог registerCoverCacheFile из config.cpp
// ВАЖНО: вызывается внутри lockLittleFS, поэтому повторно не блокируем
// ============================================================================
void TrackFacts::registerFactsCacheFile(const String& path) {
  extern bool littleFsReady;
  if (!littleFsReady) return;

  // Получаем размер файла
  File f = LittleFS.open(path, "r");
  if (!f) return;
  size_t fileSize = f.size();
  f.close();

  // Загружаем текущий индекс
  std::vector<FactsCacheEntry> entries;
  loadFactsIndex(entries);

  // Удаляем старую запись для этого пути (если обновляем существующий файл).
  // wasIndexed — трек уже был в индексе (2-й и следующие факты того же трека).
  bool wasIndexed = false;
  for (auto it = entries.begin(); it != entries.end(); ) {
    if (it->path == path) {
      it = entries.erase(it);
      wasIndexed = true;
    } else {
      ++it;
    }
  }

  // Добавляем новую запись с текущим timestamp
  FactsCacheEntry newEntry;
  newEntry.ts = factsTimestamp();
  newEntry.size = fileSize;
  newEntry.path = path;
  entries.push_back(newEntry);

  /*
   * Общий объём считаем по размерам из индекса, БЕЗ LittleFS.exists() на каждую запись.
   * Раньше на каждое сохранение факта шли тысячи exists() по всему кэшу на Core 0 —
   * вместе с выводом кадра это давало task_wdt (IDLE0) и перезагрузку.
   * Записи об удалённых снаружи файлах (WebUI/FsInfo) отсеиваются ниже, при LRU-вытеснении:
   * они стоят в начале списка (старые) и уходят первыми.
   */
  size_t total = 0;
  for (const auto& e : entries) {
    total += e.size;
  }

  /*
   * Тот же трек, лимит не превышен — индекс не переписываем: перезапись большого
   * индекса на каждый 2-й/3-й факт = лишние стирания flash. Размер в индексе отстаёт
   * на длину одной строки факта (сотни байт) — на лимит 1 МБ это не влияет;
   * при следующей перезаписи индекса размер обновится.
   */
  if (wasIndexed && total <= FACTS_CACHE_MAX_BYTES_LIMIT) {
    return;
  }

  // LRU ротация: удаляем самые старые файлы, пока объём > FACTS_CACHE_MAX_BYTES_LIMIT
  size_t evictCount = 0;
  while (total > FACTS_CACHE_MAX_BYTES_LIMIT && !entries.empty()) {
    FactsCacheEntry oldest = entries.front();
    entries.erase(entries.begin());
    if (LittleFS.exists(oldest.path)) {
      LittleFS.remove(oldest.path);
      Serial.printf("[TrackFacts] LRU evict: %s (%d bytes)\n",
                    oldest.path.c_str(), oldest.size);
    }
    if (total >= oldest.size) total -= oldest.size;
    else total = 0;
    // Удаление файла = стирание flash; между удалениями отдаём тик IDLE0.
    if ((++evictCount & 7u) == 0) vTaskDelay(1);
  }

  // Сохраняем обновлённый индекс
  saveFactsIndex(entries);
}

// ============================================================================
// Обновление глобальной переменной ::currentFact для отображения в WebUI и на экране
// Добавляет факт к списку текущих фактов и формирует строку с разделителями ###
// ============================================================================
void TrackFacts::updateDisplayFact(const String& fact) {
  if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    String toAdd = fact;
    bool exists = false;
    for (const auto& f : currentFacts) {
      if (f == toAdd) {
        exists = true;
        break;
      }
    }
    // Дубликат (в т.ч. повтор iTunes после 429 на слоте 3) — не добавляем и не помечаем « [2]».
    if (exists) {
      Serial.println("[TrackFacts] Пропуск дубликата факта (уже в списке)");
      xSemaphoreGive(_factsMutex);
      return;
    }

    if (!exists) {
        currentFacts.push_back(toAdd);

        String allFacts = "";
        for (size_t i = 0; i < currentFacts.size(); i++) {
          if (i > 0) allFacts += "###";
          allFacts += currentFacts[i];
        }

        ::currentFact = allFacts;

        Serial.printf("[TrackFacts] UI Updated: %d facts, total length: %d bytes\n",
                      (int)currentFacts.size(), (int)allFacts.length());
    }
    xSemaphoreGive(_factsMutex);
  } else {
    Serial.println("[TrackFacts] updateDisplayFact: Mutex lock failed!");
  }
}

// ============================================================================
// Служебные методы
// ============================================================================

String TrackFacts::getLanguageCode() {
  switch (currentLanguage) {
    case FactLanguage::RUSSIAN: return "ru";
    case FactLanguage::ENGLISH: return "en";
    case FactLanguage::AUTO: return "ru";
    default: return "en";
  }
}

void TrackFacts::setFactSource(FactSource source) {
  currentSource = source;
  // Сброс failover при смене провайдера
  _failoverActive = false;
  _failoverNotified = false;
}

void TrackFacts::setProvider(uint8_t provider) {
  FactSource oldSource = currentSource;
  if (provider == 0) {
    currentSource = FactSource::GEMINI;
  } else if (provider == 1) {
    currentSource = FactSource::DEEPSEEK;
  } else if (provider == 2) {
    currentSource = FactSource::ITUNES;
  } else if (provider == 3) {
    currentSource = FactSource::LASTFM;
  } else if (provider == 4) {
    currentSource = FactSource::GROQ;
  }

  if (currentSource != oldSource) {
    // Инвалидация старых FactsTask: если в полёте был запрос прежнего провайдера,
    // его результат не должен попасть в UI после смены источника.
    _factsSslEpoch++;
    // [v0.4.4] Сбрасываем состояние при смене провайдера, чтобы не висели факты
    // от старого источника (например, DeepSeek) после переключения на iTunes.
    factRequestCounter = 0;
    factLoaded = false;
    isRequestInProgress = false;
    resumeDeferredCoverDownload();
    _failoverActive = false;
    _failoverNotified = false;
    _notifiedItunesAiFallback = false;
    _notifiedFlacItunesAuto = false;
    _notifiedFactExhaustedToast = false;
    autoRequestDone = false; // Разрешаем повторный авто-запрос для текущей песни
    lastStatusMs = 0;
    _effectiveSource = currentSource;

    FactProvider* newProv = getProvider(currentSource);
    const bool needsKeyButEmpty =
      newProv && newProv->needsApiKey() && geminiApiKey.length() == 0;
    // Пока пользователь вводит ключ в настройках — не спамим тостом failover и не форсим немедленный SSL.
    _suppressFailoverToastUntilMs = needsKeyButEmpty ? (millis() + 90000u) : 0u;

    // Очищаем кэш фактов и глобальную строку под мьютексом — WebUI должен немедленно
    // перестать отображать результат старого провайдера. Новый провайдер заполнит
    // currentFacts при следующем успешном запросе.
    if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      currentFacts.clear();
      ::currentFact = "";
      xSemaphoreGive(_factsMutex);
    }
    _displayFactsAreAi = false;

    // Даем возможность новому провайдеру сработать быстрее (сбрасываем таймер ожидания),
    // чтобы автозапрос не ждал ещё 7 секунд после переключения провайдера.
    // Если выбран AI без ключа — не форсим немедленный fetch+iTunes failover (пользователь ещё в настройках).
    factLoadTime = needsKeyButEmpty ? millis() : (millis() - 40000);
    
    // Уведомляем пользователя
    sendStatus("Провайдер изменен. Поиск фактов...");
  }
}

void TrackFacts::setGeminiApiKey(const String& key) {
  geminiApiKey = key;
  // Если ключ был установлен, сбрасываем failover —
  // теперь провайдер может работать нормально
  if (key.length() > 0) {
    _failoverActive = false;
    _failoverNotified = false;
    _suppressFailoverToastUntilMs = 0;
  }
}

void TrackFacts::setLanguage(FactLanguage lang) {
  currentLanguage = lang;
}

void TrackFacts::setFactCount(uint8_t count) {
  if (count > 0 && count <= TRACKFACTS_MAX_PER_TRACK) {
    factsPerTrack = count;
  }
}

String TrackFacts::currentFactTrackLabelForApi() const {
  if (!_enabled || lastParsedTitle.length() == 0) return String();
  if (lastArtist.length() > 0) return lastArtist + " - " + lastParsedTitle;
  return lastParsedTitle;
}

bool TrackFacts::factLabelSyncedWithMeta(const String& liveMetaTitle) const {
  // Нельзя подменять live ICY старым label плагина: WS уже показал новый трек,
  // а on_ticker ещё не успел — иначе facts.js вернёт старый тег + ⏳.
  if (!_enabled || liveMetaTitle.length() == 0) return false;
  return lastTitle.length() > 0 && lastTitle == liveMetaTitle;
}

bool TrackFacts::manualMoreFragmentsUi() const {
  if (!_enabled || !_manualMoreFragmentsUi) return false;
  return manualRequestPending || isRequestInProgress;
}

bool TrackFacts::incompleteIterativeSeriesForApi() const {
  if (!_enabled || factLoaded || factsPerTrack <= 1) return false;
  if (!isIterativeSource(_effectiveSource)) return false;
  if (isRequestInProgress || manualRequestPending) return false;
  if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(40)) != pdTRUE) return false;
  const size_t n = currentFacts.size();
  xSemaphoreGive(_factsMutex);
  return n > 0 && n < (size_t)factsPerTrack;
}

void TrackFacts::setEnabled(bool enabled) {
  _enabled = enabled;
  if (!enabled) {
    // При выключении чистим состояние и убираем факт из WebUI.
    // Это предотвращает остаточные логи/данные после отключения в веб-настройках.
    isRequestInProgress = false;
    resumeDeferredCoverDownload();
    factLoaded = false;
    factRequestCounter = 0;
    _manualMoreFragmentsUi = false;
    currentFacts.clear();
    ::currentFact = "";
  }
}

// Как у CoverDL / metaTitleIsSystemBadgeOnly:
// блокируем только служебные заголовки UI, НЕ любой '[' внутри названия трека.
// Иначе «ily … [feat. Emilee]» принимался (toast), но fetch никогда не вызывался.
static bool isFactsBlockedSystemTitle(const String& title) {
  String s = title;
  s.trim();
  if (s.length() == 0) return true;

  // Строка целиком вида "[статус]" (опционально пробелы после ']') — бейдж, не трек.
  if (s.charAt(0) == '[') {
    const int rb = s.indexOf(']');
    if (rb > 0) {
      bool onlySpacesAfter = true;
      for (int i = rb + 1; i < (int)s.length(); i++) {
        if (s.charAt(i) != ' ') {
          onlySpacesAfter = false;
          break;
        }
      }
      if (onlySpacesAfter) return true;
    }
    // Как cover: начинается с '[' — не отправляем в сеть (напр. "[LIVE] …" тоже отсекается).
    return true;
  }

  // Явные служебные подстроки (на случай частичных/локализованных вариантов в meta).
  if (s.indexOf("[остановлено]") >= 0) return true;
  if (s.indexOf("[готов]") >= 0) return true;
  if (s.indexOf("[соедин") >= 0) return true;   // "[соединение]"
  if (s.indexOf("[connect") >= 0) return true;
  return false;
}

// Убирает хвосты вида "*** www.station..." / URL, которые станции дописывают в ICY-meta.
static void stripRadioStreamPromoTail(String& s) {
  s.trim();
  if (s.length() == 0) return;

  int pos = s.indexOf(" ***");
  if (pos >= 0) {
    s = s.substring(0, pos);
    s.trim();
    return;
  }
  pos = s.indexOf("\t***");
  if (pos >= 0) {
    s = s.substring(0, pos);
    s.trim();
    return;
  }
  int tri = s.indexOf("***");
  if (tri > 0) {
    s = s.substring(0, tri);
    s.trim();
    return;
  }
  pos = s.indexOf("http://");
  if (pos > 0) {
    s = s.substring(0, pos);
    s.trim();
    return;
  }
  pos = s.indexOf("https://");
  if (pos > 0) {
    s = s.substring(0, pos);
    s.trim();
    return;
  }
  pos = s.indexOf(" | www.");
  if (pos > 0) {
    s = s.substring(0, pos);
    s.trim();
    return;
  }
  pos = s.indexOf(" www.");
  if (pos > 0) {
    s = s.substring(0, pos);
    s.trim();
  }
}

static size_t countFactsInPackedString(const String& s) {
  if (s.length() == 0) return 0;
  size_t n = 1;
  int p = 0;
  for (;;) {
    int idx = s.indexOf("###", p);
    if (idx < 0) break;
    n++;
    p = idx + 3;
  }
  return n;
}

// Ручной запрос факта из WebUI (по клику на логотип/обложку в плеере — не загрузка JPG обложки).
// Возвращает true, если был запланирован НОВЫЙ сетевой запрос.
// Возвращает false, если полный комплект уже в RAM/файле или запрос уже в процессе.
bool TrackFacts::requestManualFact(String* rejectReason) {
  // Подробная проверка №1: если модуль выключен, ничего не делаем.
  if (!_enabled) {
    if (rejectReason) *rejectReason = "TrackFacts выключен в настройках.";
    return false;
  }

  // Ручной запрос поддерживается в WEB и SD-режиме.
  // В SD-режиме факты тоже можно запрашивать, если есть интернет (WiFi проверяется ниже).
  uint8_t mode = config.getMode();
  if (mode != PM_WEB && mode != PM_SDCARD) {
    if (rejectReason) *rejectReason = "Ручной запрос факта доступен только в WEB/SD-режимах.";
    return false;
  }

  if (player.status() != PLAYING) {
    if (rejectReason) {
      *rejectReason = (currentLanguage == FactLanguage::ENGLISH)
          ? "Facts only while playing. If audio already plays — wait a second and try again (player still switching)."
          : "Запрос факта — только во время воспроизведения. Если музыка уже играет — подождите секунду и повторите (плеер ещё переключается).";
    }
    return false;
  }

  size_t ramFactCount = 0;
  bool hasFactsInRam = false;
  if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    ramFactCount = currentFacts.size();
    hasFactsInRam = !currentFacts.empty() || (::currentFact.length() > 0);
    xSemaphoreGive(_factsMutex);
  } else {
    hasFactsInRam = (::currentFact.length() > 0);
    if (hasFactsInRam) {
      ramFactCount = countFactsInPackedString(::currentFact);
    }
  }

  checkFailover();
  const bool iterative = isIterativeSource(_effectiveSource);
  const bool wantMoreIterativeFacts =
      iterative && factsPerTrack > 1 && ramFactCount > 0 &&
      ramFactCount < (size_t)factsPerTrack;

  // Уже есть полный набор (или один факт у неитеративного источника) — только крутим существующее в UI.
  if (hasFactsInRam && !wantMoreIterativeFacts) {
    if (rejectReason) *rejectReason = "Факт уже доступен: используем данные из памяти/кэша.";
    return false;
  }

  // Файловый кэш: полный набор или метаданные (iTunes/Last.fm) — без сети.
  // Для AI с «недобором»: если RAM пуста — поднимаем часть с диска и ждём следующего нажатия для API.
  String artist = lastArtist;
  String title = lastParsedTitle;
  artist.trim();
  title.trim();
  // Та же политика, что у CoverDL: служебный бейдж — не ставим сетевую очередь впустую.
  if (isFactsBlockedSystemTitle(title)) {
    if (rejectReason) {
      *rejectReason = (currentLanguage == FactLanguage::ENGLISH)
          ? "Facts are not available for system status titles."
          : "Запрос факта недоступен для служебного статуса (не название трека).";
    }
    return false;
  }
  if (title.length() > 0) {
    String trackKey = generateTrackKey(artist, title);
    std::vector<String> cachedFacts;
    if (getFactsFromCache(trackKey, cachedFacts) &&
        factsCacheUsableForCurrentProvider(currentSource, geminiApiKey, cachedFacts)) {
      const size_t n = cachedFacts.size();
      const bool cacheStopsHere =
          n >= (size_t)factsPerTrack || (!isIterativeSource(_effectiveSource) && n > 0);

      if (cacheStopsHere) {
        if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          currentFacts = cachedFacts;
          String allFacts = "";
          for (size_t i = 0; i < currentFacts.size(); i++) {
            if (i > 0) allFacts += "###";
            allFacts += currentFacts[i];
          }
          ::currentFact = allFacts;
          autoRequestDone = true;
          xSemaphoreGive(_factsMutex);
        } else {
          String allFacts = "";
          for (size_t i = 0; i < cachedFacts.size(); i++) {
            if (i > 0) allFacts += "###";
            allFacts += cachedFacts[i];
          }
          ::currentFact = allFacts;
        }
        _displayFactsAreAi =
            isIterativeSource(_effectiveSource) && !factsStringsLookLikeCatalog(cachedFacts);
        if (rejectReason) *rejectReason = "Факт получен из файлового кэша, сетевой запрос не требуется.";
        return false;
      }

      // На диске больше строк, чем в RAM — подтянуть без сети (не ждать стабилизацию потока).
      if (isIterativeSource(_effectiveSource) && n > 0 && n < (size_t)factsPerTrack &&
          ramFactCount < n) {
        if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          currentFacts = cachedFacts;
          String allFacts = "";
          for (size_t i = 0; i < currentFacts.size(); i++) {
            if (i > 0) allFacts += "###";
            allFacts += currentFacts[i];
          }
          ::currentFact = allFacts;
          autoRequestDone = false;
          xSemaphoreGive(_factsMutex);
        } else {
          String allFacts = "";
          for (size_t i = 0; i < cachedFacts.size(); i++) {
            if (i > 0) allFacts += "###";
            allFacts += cachedFacts[i];
          }
          ::currentFact = allFacts;
        }
        _displayFactsAreAi =
            isIterativeSource(_effectiveSource) && !factsStringsLookLikeCatalog(cachedFacts);
        if (rejectReason) {
          *rejectReason = "Часть фактов из файлового кэша. Следующий фрагмент — ещё одно нажатие.";
        }
        return false;
      }
    }
  }

  // Подробная проверка №4: если ручной запрос уже ожидает исполнения
  // или сетевой запрос уже выполняется, не дублируем очередь.
  if (manualRequestPending || isRequestInProgress) {
    if (rejectReason) *rejectReason = "Запрос факта уже выполняется. Подождите завершения.";
    return false;
  }

  // Перед постановкой ручного запроса проверяем, что поток достаточно стабилен
  // для low-buffer retry (MANUAL_FACTS_MIN_BUFFER_PERCENT + MANUAL_FACTS_STABLE_WINDOW_MS).
  // Это предотвращает долгий pending в ситуациях, когда звук уже рвётся.
  uint32_t now = millis();
  String localRejectReason;
  if (!canRunManualLowBufferRetry(now, &localRejectReason)) {
    if (rejectReason) *rejectReason = localRejectReason;
    return false;
  }

  // Здесь реально планируем новый сетевой запрос.
  {
    size_t nExisting = 0;
    if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      nExisting = currentFacts.size();
      xSemaphoreGive(_factsMutex);
    }
    _manualMoreFragmentsUi = (nExisting > 0);
  }
  manualRequestPending = true;
  // Новое нажатие — снова разрешены один повтор после abort и одно продление дедлайна.
  _manualRequeueUsed = false;
  _manualDeadlineExtended = false;
  manualRequestAtMs = now + FACT_MANUAL_DELAY;
  // Ограничиваем время ожидания условий запуска:
  // FACT_MANUAL_DELAY + окно стабильности + EXTRA (пока ждём конец CoverDL без обработки fetch).
  manualRequestDeadlineMs =
      manualRequestAtMs + MANUAL_FACTS_STABLE_WINDOW_MS + MANUAL_FACTS_EXTRA_WAIT_MS;
  if (rejectReason) *rejectReason = "";
  return true;
}

void TrackFacts::updateManualStabilityWindow(uint32_t now) {
  // Условие 1: low-buffer retry актуален только для WEB и только когда реально играет поток.
  if (config.getMode() != PM_WEB || player.status() != PLAYING || !player.isRunning()) {
    manualStableSinceMs = 0;
    return;
  }

  // Условие 2: буфер не ниже MANUAL_FACTS_MIN_BUFFER_PERCENT (options.h).
  uint32_t total = player.getInBufferSize();
  if (total == 0) {
    manualStableSinceMs = 0;
    return;
  }
  uint32_t filled = player.inBufferFilled();
  uint32_t minRetryFilled = (total * (uint32_t)MANUAL_FACTS_MIN_BUFFER_PERCENT) / 100u;
  if (filled < minRetryFilled) {
    manualStableSinceMs = 0;
    return;
  }

  // Условие 3: должны регулярно приходить новые данные потока.
  // Если активность не обновлялась >1500 мс, считаем поток нестабильным.
  extern volatile uint32_t g_lastWebStreamActivityMs;
  uint32_t silenceMs = now - g_lastWebStreamActivityMs;
  if (silenceMs > 1500) {
    manualStableSinceMs = 0;
    return;
  }

  // Все условия выполнены — запускаем/продолжаем окно стабильности.
  if (manualStableSinceMs == 0) {
    manualStableSinceMs = now;
  }
}

bool TrackFacts::canRunManualLowBufferRetry(uint32_t now, String* rejectReason) const {
  // Проверка базового состояния сети/плеера.
  if (WiFi.status() != WL_CONNECTED) {
    if (rejectReason) *rejectReason = "Запрос факта отклонён: WiFi не подключен.";
    return false;
  }
  if (player.status() != PLAYING || !player.isRunning()) {
    if (rejectReason) *rejectReason = "Запрос факта отклонён: воспроизведение не активно.";
    return false;
  }

  // Поддерживаем ручной запрос в WEB и SD.
  // Для других режимов запрос невалиден.
  uint8_t mode = config.getMode();
  if (mode != PM_WEB && mode != PM_SDCARD) {
    if (rejectReason) *rejectReason = "Запрос факта отклонён: режим не поддерживается.";
    return false;
  }
  // Для SD-проигрывания не применяем WEB-ограничения по буферу/стабильности потока:
  // в локальном воспроизведении они не отражают реальный риск "подвисания" сети.
  if (mode == PM_SDCARD) {
    if (rejectReason) *rejectReason = "";
    return true;
  }

  // Проверяем минимальный ручной порог буфера (MANUAL_FACTS_MIN_BUFFER_PERCENT).
  uint32_t total = player.getInBufferSize();
  uint32_t filled = player.inBufferFilled();
  if (total == 0) {
    if (rejectReason) *rejectReason = "Запрос факта отклонён: буфер потока не готов.";
    return false;
  }
  uint32_t retryMinFilled = (total * (uint32_t)MANUAL_FACTS_MIN_BUFFER_PERCENT) / 100u;
  if (filled < retryMinFilled) {
    if (rejectReason) {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "Запрос факта отклонён: буфер %u%% < %u%%.",
               (unsigned)((filled * 100u) / total),
               (unsigned)MANUAL_FACTS_MIN_BUFFER_PERCENT);
      *rejectReason = String(msg);
    }
    return false;
  }

  // Требуем непрерывно стабильный поток в течение MANUAL_FACTS_STABLE_WINDOW_MS.
  if (manualStableSinceMs == 0) {
    if (rejectReason) *rejectReason = "Запрос факта отклонён: ждём стабилизацию потока (10 сек).";
    return false;
  }
  uint32_t stableMs = now - manualStableSinceMs;
  if (stableMs < MANUAL_FACTS_STABLE_WINDOW_MS) {
    if (rejectReason) {
      uint32_t leftSec = (MANUAL_FACTS_STABLE_WINDOW_MS - stableMs + 999u) / 1000u;
      char msg[180];
      snprintf(msg, sizeof(msg),
               "Запрос факта отклонён: поток ещё не стабилен (%u сек до допуска).",
               (unsigned)leftSec);
      *rejectReason = String(msg);
    }
    return false;
  }

  if (rejectReason) *rejectReason = "";
  return true;
}