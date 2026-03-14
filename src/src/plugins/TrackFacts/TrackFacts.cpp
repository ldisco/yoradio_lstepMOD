// TrackFacts.cpp - Плагин для получения интересных фактов о песнях
// v0.4.1 - iTunes по умолчанию (pos 2), MusicBrainz (pos 4)
// Ограничение повторных запросов для metadata-провайдеров. Max 3 факта.
// Принцип: "Собрал факт -> сохранил в файл -> забыл. Нужно показать -> прочитал файл."
#include "TrackFacts.h"
#include <Arduino.h>
#include "../../core/player.h"
#include "../../core/config.h" // Для lockLittleFS/unlockLittleFS, littleFsReady
#include "../../core/netserver.h" // [v0.4.4] Для доступа к AsyncWebSocket websocket
#include "../../../myoptions.h" // [v0.3.23] Подключаем настройки прокси

// [v0.4.0] Подключаем все провайдеры
#include "providers/GeminiProvider.h"
#include "providers/DeepSeekProvider.h"
#include "providers/MusicBrainzProvider.h"
#include "providers/LastFmProvider.h"
#include "providers/iTunesProvider.h"

// Внешние ссылки на метаданные из конфигурации плеера
extern String metaTitle;
extern String metaArtist;
extern String currentFact;

#define FACT_FETCH_DELAY 7000  // Задержка 7 секунд после смены трека перед первым запросом факта
#define FACT_MANUAL_DELAY 5000 // Задержка 5 секунд перед ручным запросом факта

// ============================================================================
// Конструктор
// ============================================================================
TrackFacts::TrackFacts()
  : lastTitle(""), lastArtist(""), currentFactIndex(0),
    factLoadTime(0), lastTickerCheck(0), factLoaded(false),
    isRequestInProgress(false), _enabled(false),
    currentSource(FactSource::ITUNES),
    _effectiveSource(FactSource::ITUNES),
    currentLanguage(FactLanguage::AUTO), geminiApiKey(""),
    factsPerTrack(3), factRequestCounter(0),
    trackChangeMs(0), autoRequestDone(false),
    manualRequestPending(false), manualRequestAtMs(0),
    _failoverActive(false), _failoverNotified(false),
    lastStatusMs(0) {
  _factsMutex = xSemaphoreCreateMutex(); // [v0.3.23] Инициализация мьютекса

  // [v0.4.1] Создаём экземпляры всех провайдеров (позиции iTunes и MusicBrainz поменяны)
  _providers[0] = new GeminiProvider();     // FactSource::GEMINI = 0
  _providers[1] = new DeepSeekProvider();   // FactSource::DEEPSEEK = 1
  _providers[2] = new iTunesProvider();     // FactSource::ITUNES = 2 (по умолчанию)
  _providers[3] = new LastFmProvider();     // FactSource::LASTFM = 3
  _providers[4] = new MusicBrainzProvider();// FactSource::MUSICBRAINZ = 4
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
  currentFacts.clear();
  // [Gemini3Pro] Резервируем память под 5 элементов во избежание множественных реаллокаций в DRAM
  if (currentFacts.capacity() < 5) {
      currentFacts.reserve(5); 
  }
  currentFactIndex = 0;
  isRequestInProgress = false;
  factRequestCounter = 0;
  // [v0.4.1] Немедленно очищаем глобальную переменную, чтобы старый факт не показывался в WebUI
  ::currentFact = "";
  // Сброс уведомления failover для нового трека (покажем ещё раз если нужно)
  _failoverNotified = false;
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
  factRequestCounter = 0;
  isRequestInProgress = false;
  _failoverActive = false;
  _failoverNotified = false;
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

  // 4) Резервируем ёмкость вектора, как и в on_track_change, чтобы избежать лишних реаллокаций.
  if (currentFacts.capacity() < 5) {
    currentFacts.reserve(5);
  }
}

// ============================================================================
extern bool g_coverTaskRunning;
extern volatile bool g_modeSwitching;  // [FIX] Флаг переключения режимов
extern volatile unsigned long g_modeSwitchTime;  // [FIX] Timestamp переключения
extern bool isModeSwitchCooldown();  // [FIX] Проверка cooldown периода
extern bool isSafeForSSL();  // [FIX] Проверка heap/WiFi/cooldown

// Основной цикл (вызывается каждую секунду)
// ============================================================================
void TrackFacts::on_ticker() {
  if (!_enabled) return;
  
  // [v0.4.1] Сбрасываем отображение при остановке композиции
  if (player.status() != PLAYING) {
    if (::currentFact.length() > 0) {
      ::currentFact = "";
      currentFacts.clear();
      factLoaded = false;
      factRequestCounter = 0;
    }
    return;
  }

  uint32_t now = millis();
  if (now - lastTickerCheck < 1000) return; // Проверка раз в секунду
  lastTickerCheck = now;

  // Если в данный момент скачивается обложка — ждем. 
  // Это предотвращает конфликт за оперативную память (SSL) и LittleFS.
  if (g_coverTaskRunning) {
    factLoadTime = now; // Смещаем время начала, чтобы не стартануть сразу после обложки
    return;
  }

  // [v0.3.3] Пауза 8 секунд после завершения загрузки обложки, 
  // чтобы дать системе освободить память и избежать ошибки -32512.
  // Также это гарантирует, что мы не попадем в "шторм" SSL рукопожатий.
  if (now - factLoadTime < 8000 && !factLoaded && factRequestCounter == 0) return;

  if (metaTitle.length() == 0) return;

  // --- Детектирование смены трека по метаданным ---
  if (metaTitle != lastTitle) {
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
    lastArtist = artist;
    lastParsedTitle = title;
    trackChangeMs = now;
    autoRequestDone = false;
    manualRequestPending = false;
    manualRequestAtMs = 0;

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
    factLoaded = false;
    isRequestInProgress = false;
    factLoadTime = now;
    factRequestCounter = 0;
    _failoverNotified = false;

    // --- Поиск фактов в файловом кэше (читаем "на лету" из файла) ---
    String trackKey = generateTrackKey(lastArtist, lastParsedTitle);
    
    // Временно храним загруженные факты, чтобы не блокировать надолго основную петлю
    std::vector<String> tmpFacts;
    if (getFactsFromCache(trackKey, tmpFacts)) {
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
      }
    }
  }

  // --- Логика последовательного накопления фактов ---
  // [v0.4.1] ОГРАНИЧЕНИЕ: Для iTunes/MusicBrainz/LastFM делаем только ОДИН запрос.
  // Повторные запросы для метаданных не имеют смысла, они возвращают то же самое.
  if (!factLoaded && currentFacts.size() < (size_t)factsPerTrack && !isRequestInProgress && factRequestCounter < factsPerTrack) {
    uint32_t now = millis();
    bool canAuto = (!autoRequestDone && (now - trackChangeMs >= FACT_FETCH_DELAY));
    bool canManual = (manualRequestPending && now >= manualRequestAtMs);

    if ((canAuto || canManual) && lastParsedTitle.indexOf("[") < 0) {
      // [FIX v0.8.78] На FLAC-потоках DeepSeek/Gemini иногда вызывают заметные фризы
      // и даже сетевые assert (lwIP/pbuf_free) из-за тяжёлых TLS/JSON операций параллельно
      // с декодированием. Для стабильности: на FLAC блокируем АВТОМАТИЧЕСКИЕ AI-запросы,
      // но сохраняем возможность РУЧНОГО запроса по желанию пользователя.
      // Важно: файловый кэш при этом продолжает работать — если факт уже в кэше, он покажется.
      const bool isFlacStream =
        (config.getMode() == PM_WEB) && (config.getBitrateFormat() == BF_FLAC);
      const bool isHeavyAiSource =
        (currentSource == FactSource::GEMINI || currentSource == FactSource::DEEPSEEK);
      if (isFlacStream && isHeavyAiSource && canAuto) {
        autoRequestDone = true;  // не пытаться автозапросом снова и снова
        if (now - lastStatusMs > 5000) {
          sendStatus("Авто-AI-факты отключены для FLAC (стабильность аудио). Ручной запрос доступен.");
          lastStatusMs = now;
        }
        // Авто-запрос заблокирован, но ручной (canManual) может пойти дальше по обычной ветке.
        canAuto = false;
        if (!canManual) return;
      }

      // [FIX] Перед сетевым запросом проверяем безопасность SSL именно для фактов.
      if (isSafeForSSLForFacts()) {
        factRequestCounter++;
        fetchFact(lastArtist, lastParsedTitle);
        factLoadTime = now;
      } else {
        // [v0.4.3] Детальные логи в монитор порта для анализа задержек
        if (now - lastStatusMs > 3000) {
          if (g_modeSwitching) {
            uint32_t timeLeft = 0;
            if (now < g_modeSwitchTime + 30000) timeLeft = (g_modeSwitchTime + 30000 - now) / 1000;
            Serial.printf("[TrackFacts] Запрос отложен: Блокировка режима (еще %u сек)\n", timeLeft);
            sendStatus("Режим переключается. Ожидание стабильности...");
          } else if (g_coverTaskRunning) {
            Serial.println("[TrackFacts] Запрос отложен: Загрузка обложки на Core 1");
            sendStatus("Загрузка обложки. Факты в очереди...");
          } else if (ESP.getFreeHeap() < 35000) {
            Serial.printf("[TrackFacts] Запрос отложен: Критически мало памяти (%u байт)\n", ESP.getFreeHeap());
            sendStatus("Мало памяти для SSL. Ждем освобождения...");
          } else if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[TrackFacts] Запрос отложен: Нет соединения WiFi");
            sendStatus("WiFi не подключен. Запрос отложен.");
          }
          lastStatusMs = now;
        }
      }
      if (canAuto) {
        autoRequestDone = true;
      }
      if (canManual) {
        manualRequestPending = false;
      }
    }
  }

  // [v0.4.1] Если загрузка завершена (или лимит 8 для AI) и фактов нет — стоп
  if ((factLoaded || factRequestCounter >= factsPerTrack) && currentFacts.empty() && ::currentFact.length() == 0) {
      ::currentFact = (currentLanguage == FactLanguage::RUSSIAN) ?
          "Факты не найдены (ошибка API или превышен лимит)." :
          "No facts found (API error or limit reached).";
  }
}

void TrackFacts::processMetadata() {
  // Логика перенесена в on_ticker
}

// ============================================================================
// Failover: Проверка API ключа и авто-переключение на MusicBrainz
// ============================================================================

// [v0.4.0] Получить провайдер по индексу FactSource (с проверкой границ)
FactProvider* TrackFacts::getProvider(FactSource src) {
  int idx = static_cast<int>(src);
  if (idx < 0 || idx > 5) return nullptr;
  return _providers[idx];
}

// [v0.4.0] Синхронизировать API ключ и язык со всеми провайдерами
void TrackFacts::syncProviderSettings() {
  ProviderLanguage pLang = static_cast<ProviderLanguage>(static_cast<int>(currentLanguage));
  for (int i = 0; i < 6; i++) {
    if (_providers[i]) {
      _providers[i]->setApiKey(geminiApiKey);
      _providers[i]->setLanguage(pLang);
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
    
    // Уведомляем пользователя один раз за песню
    if (!_failoverNotified) {
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

// [v0.4.2] Метод отправки статуса в WebUI через WebSocket
void TrackFacts::sendStatus(const char* msg, bool isError) {
  if (msg == nullptr) return;
  
  // Формируем JSON пакет: {"toast": "текст", "isErr": 0/1}
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"toast\": \"%s\", \"isErr\": %d}", msg, isError ? 1 : 0);
  
  // [v0.4.4] Отправляем всем подключенным веб-клиентам
  // Используем extern AsyncWebSocket websocket из netserver.h
  websocket.textAll(buf);
  
  // Дублируем в Serial для монитора
  Serial.printf("[TrackFacts] Toast: %s\n", msg);
}

// ============================================================================
// Определяет, нужно ли кэшировать факт от данного провайдера
// Кэшируем только "дорогие" факты от AI (Gemini, DeepSeek)
// MusicBrainz/LastFM — лёгкие и бесплатные, не кэшируем
// ============================================================================
bool TrackFacts::shouldCacheFact(FactSource source) const {
  // Кэшируем "дорогие" AI-запросы и iTunes (по просьбе пользователя)
  return (source == FactSource::GEMINI || 
          source == FactSource::DEEPSEEK || 
          source == FactSource::ITUNES);
}

// Порождает ли источник по одному факту за запрос (требуется несколько запросов для накопления)
bool isIterativeSource(FactSource source) {
  return (source == FactSource::GEMINI || source == FactSource::DEEPSEEK);
}

// ============================================================================
// Запуск задачи получения факта (в отдельном потоке на Core 1)
// ============================================================================
void TrackFacts::fetchFact(const String& artist, const String& title) {
  if (isRequestInProgress) return;

  // --- Проверка failover перед запросом ---
  checkFailover();

  // Если failover только что уведомил пользователя и это первый запрос,
  // даём пользователю увидеть сообщение, не запускаем fetch сразу
  // (на следующем тике уведомление уже показано, _failoverNotified = true)
  // Этот блок не нужен — уведомление уже установлено в checkFailover(),
  // а запрос всё равно будет на MusicBrainz. Пусть идёт.

  isRequestInProgress = true;

  TaskParams* params = new TaskParams;
  params->artist = artist;
  params->title = title;
  params->instance = this;

  // Сетевой запрос фактов выполняем на Core 1.
  // Попытка переноса на Core 0 в v0.8.77 привела к редким assert в lwIP (pbuf_free),
  // поэтому возвращаем стабильную схему размещения задач как до изменения.
  // Размер стека оставляем 16KB (16384) — безопасно для JSON/HTTP.
  xTaskCreatePinnedToCore(
    fetchTask,
    "FactsTask",
    16384, // Вернули безопасный размер стека 16KB
    params,
    1,
    NULL,
    1 // Core 1: стабильный вариант без lwIP assert
  );
}

// ============================================================================
// Задача FreeRTOS для сетевых запросов (выполняется на Core 1)
// [v0.4.0] Рефакторинг: делегируем запрос провайдеру через интерфейс FactProvider
// ============================================================================
void TrackFacts::fetchTask(void* parameter) {
  TaskParams* p = (TaskParams*)parameter;
  if (p && p->instance) {
    TrackFacts* instance = p->instance;

    // [FIX] Если факты отключены — выходим без логов и без сетевых запросов.
    if (!instance->_enabled) {
        instance->isRequestInProgress = false;
        delete p;
        vTaskDelete(NULL);
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
        instance->isRequestInProgress = false;
        delete p;
        vTaskDelete(NULL);
        return;
    }

    // [FIX] Проверка безопасности SSL ПЕРЕД запуском запроса
    //        Это критично: SSL handshake может блокировать на 5+ секунд
    if (!isSafeForSSLForFacts()) {
      Serial.println("[TrackFacts] Task aborted: SSL not safe (heap/wifi/cooldown)");
      instance->isRequestInProgress = false;
      delete p;
      vTaskDelete(NULL);
      return;
    }

    // [v0.4.0] Синхронизируем настройки (ключ, язык) перед запросом
    instance->syncProviderSettings();

    // Используем _effectiveSource (может быть изменён failover-ом)
    FactSource sourceToUse = instance->_effectiveSource;
    FactProvider* provider = instance->getProvider(sourceToUse);

    if (provider) {
      Serial.printf("[TrackFacts] Fetching from %s...\n", provider->name());
      FactResult result = provider->fetchFact(p->artist, p->title);

      if (result.success && result.fact.length() > 0) {
        // [FIX] Если плагин выключили пока запрос летел — не обновляем UI
        if (!instance->_enabled) {
          Serial.println("[TrackFacts] Disabled during request — discarding result");
          instance->isRequestInProgress = false;
          delete p;
          vTaskDelete(NULL);
          return;
        }
        // Кэшируем "дорогие" AI-факты (Gemini, DeepSeek)
        if (instance->shouldCacheFact(sourceToUse)) {
          String trackKey = instance->generateTrackKey(p->artist, p->title);
          instance->saveFactToCache(trackKey, result.fact);
        }
        // Обновляем UI
        instance->updateDisplayFact(result.fact);
        
        // [v0.4.1] Один запуск FREE-провайдеров (iTunes, MusicBrainz, LastFM)
        // возвращает всю информацию за раз. AI-провайдеры (итеративные) требуют 
        // несколько запросов для сбора нужного количества фактов.
        if (!isIterativeSource(sourceToUse)) {
          instance->factLoaded = true;  // Обычный провайдер — один запрос и хватит
        } else if (instance->currentFacts.size() >= (size_t)instance->factsPerTrack) {
          instance->factLoaded = true;  // AI — достаточно фактов собрано
        }
      } else {
        Serial.printf("[TrackFacts] %s failed: %s\n", provider->name(), result.errorMsg.c_str());
        // [v0.4.1] Если обычный (не AI) провайдер ответил ошибкой (например, 404),
        // больше не пытаемся его насиловать — помечаем как "загружено"
        if (!isIterativeSource(sourceToUse)) {
          instance->factLoaded = true;
        }
      }
    } else {
      Serial.printf("[TrackFacts] Unknown/null provider for source %d\n", (int)sourceToUse);
    }

    // Сбрасываем флаг после завершения всей работы
    instance->isRequestInProgress = false;
  }
  if (p) delete p;
  vTaskDelete(NULL);
}

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
// Проверяет дубликаты, ограничивает до 10 фактов на трек.
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

  // Ограничение: максимум 10 фактов на один трек
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

  while (f.available()) {
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

  for (const auto& entry : entries) {
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

  // Удаляем старую запись для этого пути (если обновляем существующий файл)
  for (auto it = entries.begin(); it != entries.end(); ) {
    if (it->path == path) {
      it = entries.erase(it);
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

  // Подсчитываем общий объём, удаляя записи для несуществующих файлов
  size_t total = 0;
  std::vector<FactsCacheEntry> filtered;
  filtered.reserve(entries.size());
  for (const auto& e : entries) {
    if (!LittleFS.exists(e.path)) continue;
    total += e.size;
    filtered.push_back(e);
  }

  // LRU ротация: удаляем самые старые файлы, пока объём > FACTS_CACHE_MAX_BYTES (1 МБ)
  while (total > FACTS_CACHE_MAX_BYTES && !filtered.empty()) {
    FactsCacheEntry oldest = filtered.front();
    filtered.erase(filtered.begin());
    if (LittleFS.exists(oldest.path)) {
      LittleFS.remove(oldest.path);
      Serial.printf("[TrackFacts] LRU evict: %s (%d bytes)\n",
                    oldest.path.c_str(), oldest.size);
    }
    if (total >= oldest.size) total -= oldest.size;
    else total = 0;
  }

  // Сохраняем обновлённый индекс
  saveFactsIndex(filtered);
}

// ============================================================================
// Обновление глобальной переменной ::currentFact для отображения в WebUI и на экране
// Добавляет факт к списку текущих фактов и формирует строку с разделителями ###
// ============================================================================
void TrackFacts::updateDisplayFact(const String& fact) {
  // Serial.printf("[TrackFacts] DEBUG: updateDisplayFact called with '%s'\n", fact.c_str());
  if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // Проверяем дубликаты
    bool exists = false;
    for (const auto& f : currentFacts) {
      if (f == fact) {
        exists = true;
        break;
      }
    }

    if (!exists) {
        currentFacts.push_back(fact);

        // Формируем новую строку ::currentFact
        String allFacts = "";
        for (size_t i = 0; i < currentFacts.size(); i++) {
          if (i > 0) allFacts += "###";
          allFacts += currentFacts[i];
        }
        
        ::currentFact = allFacts;
        
        Serial.printf("[TrackFacts] UI Updated: %d facts, total length: %d bytes\n", 
                      (int)currentFacts.size(), (int)allFacts.length());
        // Serial.printf("[TrackFacts] Current Facts: %s\n", allFacts.c_str());
    } else {
        // Serial.println("[TrackFacts] Fact already exists in current list");
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
  // [v0.4.1] Установка провайдера по ID из веб-интерфейса (iTunes и MusicBrainz поменяны)
  FactSource oldSource = currentSource;
  if (provider == 0) {
    currentSource = FactSource::GEMINI;
  } else if (provider == 1) {
    currentSource = FactSource::DEEPSEEK;
  } else if (provider == 2) {
    currentSource = FactSource::ITUNES;       // [v0.4.1] Был MusicBrainz, теперь iTunes
  } else if (provider == 3) {
    currentSource = FactSource::LASTFM;
  } else if (provider == 4) {
    currentSource = FactSource::MUSICBRAINZ;  // [v0.4.1] Был iTunes, теперь MusicBrainz
  }

  if (currentSource != oldSource) {
    // [v0.4.4] Сбрасываем состояние при смене провайдера, чтобы не висели факты
    // от старого источника (например, DeepSeek) после переключения на iTunes.
    factRequestCounter = 0;
    factLoaded = false;
    isRequestInProgress = false;
    _failoverActive = false;
    _failoverNotified = false;
    autoRequestDone = false; // Разрешаем повторный авто-запрос для текущей песни
    lastStatusMs = 0;

    // Очищаем кэш фактов и глобальную строку под мьютексом — WebUI должен немедленно
    // перестать отображать результат старого провайдера. Новый провайдер заполнит
    // currentFacts при следующем успешном запросе.
    if (xSemaphoreTake(_factsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      currentFacts.clear();
      ::currentFact = "";
      xSemaphoreGive(_factsMutex);
    }

    // Даем возможность новому провайдеру сработать быстрее (сбрасываем таймер ожидания),
    // чтобы автозапрос не ждал ещё 7 секунд после переключения провайдера.
    factLoadTime = millis() - 40000;
    
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
  }
}

void TrackFacts::setLanguage(FactLanguage lang) {
  currentLanguage = lang;
}

void TrackFacts::setFactCount(uint8_t count) {
  // [v0.4.1] Количество фактов для сбора (1-3, уменьшено с 10)
  if (count > 0 && count <= 3) {
    factsPerTrack = count;
  }
}

void TrackFacts::setEnabled(bool enabled) {
  _enabled = enabled;
  if (!enabled) {
    // [FIX] При выключении чистим состояние и убираем факт из WebUI.
    // Это предотвращает остаточные логи/данные после отключения в веб-настройках.
    isRequestInProgress = false;
    factLoaded = false;
    factRequestCounter = 0;
    currentFacts.clear();
    ::currentFact = "";
  }
}

// Ручной запрос факта из WebUI (по клику на логотип/обложку)
void TrackFacts::requestManualFact() {
  if (!_enabled) return;
  if (!currentFacts.empty()) return; // Уже есть факты в кэше
  manualRequestPending = true;
  manualRequestAtMs = millis() + FACT_MANUAL_DELAY;
}