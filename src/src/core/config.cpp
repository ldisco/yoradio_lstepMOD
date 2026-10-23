#include "options.h"
#include "config.h"
#include "display.h"
#include "player.h"
#include "network.h"
#include "netserver.h"
#include "controls.h"
#include "timekeeper.h"
#include "telnet.h"
#include "transient_notifier.h"
#include "rtcsupport.h"
#include "../displays/tools/l10n.h"
#include "../plugins/TrackFacts/TrackFactsProxy.h"
#if DSP_MODEL==DSP_AXS15231B
/* Faster: setBrightness/doSleep → LEDC, не analogWrite. */
extern void axsSetBacklightDuty(uint8_t duty255);
#endif
#if DSP_MODEL==DSP_ST7701
/* Supremus: setBrightness/doSleep → LEDC на GFX_BL. */
extern void st7701SetBacklightDuty(uint8_t duty255);
#include <Ticker.h>
/* Отложенный EEPROM.commit после слайдера яркости (не на каждый тик) → в idle-очередь. */
static Ticker s_st7701BlCommitTicker;
static void st7701BrightnessEepromCommit() {
  yoEepromCommit();
}

/* ---- Idle EEPROM.commit (корёж RGB): put сразу, flash когда UI спокоен ---- */
static bool s_st7701EepromDirty = false;
static uint32_t s_st7701UiBusyMs = 0;
static constexpr uint32_t kSt7701EepromIdleMs = 3000;

void yoEepromMarkUiBusy() {
  s_st7701UiBusyMs = millis();
}

static bool st7701EepromUiIdle() {
  const displayMode_e m = display.mode();
  /* Плейлист и VOL — горячие зоны тача/перерисовки; commit здесь рвёт RGB. */
  if (m == STATIONS || m == VOL) return false;
  /* Пока играет: не flush — NOW через ~3 с после stream ready рвал RGB на живом PLAYER. */
  if (player.isRunning()) return false;
  /* Окно connect (_play): connproc=false, статус ещё STOPPED — тот же NOW в логе на SmartStart/смене. */
  if (!player.connproc) return false;
  if ((millis() - s_st7701UiBusyMs) < kSt7701EepromIdleMs) return false;
  return true;
}

void yoEepromCommitNow() {
  s_st7701EepromDirty = false;
#if ST7701_DIAG_NO_FLASH_COMMIT
  Serial.printf("[ST7701-DIAG] EEPROM.commit SKIP(now) t=%lu\n", (unsigned long)millis());
#else
  EEPROM.commit();
  Serial.printf("[ST7701] EEPROM.commit NOW t=%lu\n", (unsigned long)millis());
#endif
}

void yoEepromCommit() {
#if ST7701_DIAG_NO_FLASH_COMMIT
  Serial.printf("[ST7701-DIAG] EEPROM.commit SKIP t=%lu\n", (unsigned long)millis());
#else
  /* Зеркало уже через EEPROM.put. На flash — только через TryIdleFlush (~3 с тишины).
   * Немедленный NOW запрещён: play/stop на PLAYER иначе снова рвут RGB. */
  s_st7701EepromDirty = true;
  yoEepromMarkUiBusy();
#endif
}

void yoEepromTryIdleFlush() {
#if !ST7701_DIAG_NO_FLASH_COMMIT
  if (!s_st7701EepromDirty) return;
  if (!st7701EepromUiIdle()) return;
  yoEepromCommitNow();
#endif
}
#else
void yoEepromMarkUiBusy() {}
void yoEepromTryIdleFlush() {}
void yoEepromCommitNow() {
  EEPROM.commit();
}
void yoEepromCommit() {
  EEPROM.commit();
}
#endif
#include "../plugins/TrackFacts/TrackFacts.h"
extern TrackFacts trackFactsPlugin;  // экземпляр в main.cpp — нужен для isSafeForSSL* до coverDownloadTask
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_sntp.h>  // esp_sntp_stop — гасим фоновый SNTP перед пользовательским DNS/HTTPS
#include <vector>
#include <cctype>
#ifdef USE_SD
#include "sdmanager.h"
#endif
#if USE_BT_I2S
#include "bt_i2s.h"
#endif
#if USE_BT_I2S
#include "bt_i2s.h"
#endif
#ifdef USE_NEXTION
#include "../displays/nextion.h"
#endif
#ifdef USE_DLNA
#include "../network/dlna_index.h"
#endif
#include <cstddef>
#if defined(ESP32) && defined(ARDUINO)
#include <esp_heap_caps.h>
#endif

#if DSP_MODEL==DSP_DUMMY
#define DUMMYDISPLAY
#endif

// === Watchdog: метка последней активности веб-потока (обновляется из Audio при приходе данных) ===
volatile uint32_t g_lastWebStreamActivityMs = 0;
static volatile uint32_t g_webStreamContinuousSinceMs = 0;
volatile bool g_audioFormatFlacActive = false;
void audio_stream_activity(void) {
  const uint32_t now = millis();
  const uint32_t previousActivity = g_lastWebStreamActivityMs;
  /*
   * Непрерывность считаем по реальным входным блокам Audio.
   * Так стабильное окно созревает даже когда Cover/Facts/Weather сейчас не
   * опрашивают gate, и редкий запрос погоды не откладывается навсегда.
   */
  if (previousActivity == 0
      || previousActivity > now
      || (uint32_t)(now - previousActivity) > (uint32_t)BACKGROUND_NET_STREAM_SILENCE_MS) {
    g_webStreamContinuousSinceMs = now;
  }
  g_lastWebStreamActivityMs = now;
  /*
   * Первые реальные байты после internal/external recovery —
   * единственное надёжное подтверждение восстановления. TCP connect без данных
   * намеренно не закрывает пользовательский статус.
   */
  if (transientNotifier.isActive(TN_AUDIO_RECOVERY)) {
    transientNotifier.clear(TN_AUDIO_RECOVERY);
    transientNotifier.show(TN_AUDIO_RECOVERED, "Звук восстановлен");
  }
}

// Сброс очереди CoverDL при stream lost (static CoverDL — ниже по файлу).
static void coverClearPendingOnStreamLost();

void audio_recovery_started(void) {
  /*
   * Callback из Audio.cpp не меняет title/meta и поэтому
   * не запускает повторно CoverDL/TrackFacts.
   * Stream lost: гасим фоновый HTTPS (Cover/Facts).
   * Не сбрасываем g_webStreamContinuousSinceMs — при STABLE_MS=0 gate не зависит от окна;
   * сброс + 8 s STABLE давал вечный defer после recovery.
   */
  transientNotifier.clear(TN_AUDIO_RECOVERED);
  transientNotifier.show(TN_AUDIO_RECOVERY, "Восстанавливаем поток…");
  g_coverDlCancelRequested = true;
  coverClearPendingOnStreamLost();
  trackFactsPlugin.abortForModeSwitch();
  /*
   * Короткий grace (4 с) только если WiFi-карантин уже не длиннее — не дублируем
   * 20 с lost + 15 s recovery в один каскад.
   */
  const uint32_t now = millis();
  const uint32_t shortGraceUntil = now + 4000u;
  if (shortGraceUntil > g_wifiInstabilityGraceUntilMs) {
    networkArmWifiInstabilityGrace(4000);
  }
}

// === Асинхронное переключение режимов (WEB <-> SD) ===
// Задача: убрать фризы WebUI при переключении режимов.
// Проблема: Config::changeMode() может выполнять тяжёлые операции (SD mount, индекс),
// а вызов происходил из NetServer::processQueue() — то есть из кода, который должен
// быстро обрабатывать WebSocket очередь. Когда changeMode «долго думает», браузер
// получает таймауты и кажется, что Web «умер», хотя устройство продолжает играть.
// Решение: переносим changeMode в отдельную FreeRTOS-задачу. Так Web остаётся живым,
// даже если SD‑индексация длится десятки секунд.
static volatile bool g_changeModeTaskRunning = false;
static volatile int  g_changeModeTaskRequestedMode = -1;

// Защита от «мгновенного» переключения обратно в WEB после перехода в SD (объявление до scheduleChangeModeTask).
#define MODE_SWITCH_DEBOUNCE_MS 5000
static volatile unsigned long g_lastSwitchToSdTime = 0;

// После «SD Not Found» не зацикливать WEB→SD: пропуск SD в цикле и fallback на BT/DLNA.
#define SD_MOUNT_SKIP_MS 90000u
static volatile unsigned long g_sdMountUnavailableUntil = 0;

#if defined(USE_SD) && ((DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486) && !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT))
#include "../displays/displayILI9488.h"
#endif

// Тост «SD не найдена» — в WebUI и на TFT (ILI9488 landscape).
static void notifySdMountFailed() {
  netserver.sendToast("SD-карта не найдена", true);
#if defined(USE_SD) && ((DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486) && !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT))
  ili9488_cover::showFactStatusToast("SD-карта не найдена", 3500, false);
#endif
}

// Следующий режим в цикле, если SD недоступна: DLNA (если готов) → BT → WEB.
static int modeAfterFailedSdMount() {
#ifdef USE_DLNA
  if (config.dlnaPlaylistReady()) return USER_MODE_DLNA;
#endif
#if USE_BT_I2S
  return PM_BT;
#else
  return PM_WEB;
#endif
}


static void changeModeTask(void* param) {
  int mode = (int)(intptr_t)param;
  Serial.printf("[changeModeTask] START (requested=%d)\n", mode);
  config.changeMode(mode);
  Serial.println("[changeModeTask] DONE");
  g_changeModeTaskRunning = false;
  vTaskDelete(NULL);
}

bool scheduleChangeModeTask(int newmode) {
  // Защита от мгновенного переключения обратно в WEB после перехода в SD:
  // двойной клик или вторая вкладка часто шлют newmode=0 через 1–2 сек — игнорируем.
  if (newmode == PM_WEB && config.getMode() == PM_SDCARD &&
      g_lastSwitchToSdTime != 0 &&
      (unsigned long)(millis() - g_lastSwitchToSdTime) < MODE_SWITCH_DEBOUNCE_MS) {
    Serial.printf("[changeModeTask] IGNORE: switch back to WEB within %d s blocked\n", MODE_SWITCH_DEBOUNCE_MS/1000);
    return false;
  }
  // Если задача уже запущена — не создаём вторую, чтобы избежать гонок.
  if (g_changeModeTaskRunning) {
    Serial.printf("[changeModeTask] SKIP: already running (requested=%d)\n", newmode);
    return false;
  }
  g_changeModeTaskRunning = true;
  g_changeModeTaskRequestedMode = newmode;

  // Запускаем на core1: main loop и веб (core0) продолжают крутиться — дисплей и тач не зависают.
  BaseType_t res = xTaskCreatePinnedToCore(
      changeModeTask,
      "ModeSwitch",
      8192,
      (void*)(intptr_t)newmode,
      1,                                 // низкий приоритет
      NULL,
      1                                  // core1
  );
  if (res != pdPASS) {
    Serial.println("[changeModeTask] ERROR: task create failed");
    g_changeModeTaskRunning = false;
    return false;
  }
  return true;
}

#if DSP_MODEL == DSP_NV3041A
extern void invalidateCoverCache();  // из displayNV3041A.cpp
#elif DSP_MODEL == DSP_ST7789_170
extern void invalidateCoverCache();  // из displayST7789_170.cpp
#elif DSP_MODEL == DSP_ST7789
extern void invalidateCoverCache();  // из displayST7789.cpp
#elif DSP_MODEL == DSP_ILI9488 || DSP_MODEL == DSP_ILI9486
extern void invalidateCoverCache();  // из displayILI9488.cpp
#elif DSP_MODEL == DSP_AXS15231B
extern void invalidateCoverCache();  // displayAXS15231B.cpp (Cover-SS axs_cover)
#else
inline void invalidateCoverCache() {} // Заглушка для других моделей
#endif

/* После записи обложки в LittleFS — сброс кэша отрисовки на TFT. */
inline void updateDisplayCover() {
#if DSP_MODEL == DSP_NV3041A || DSP_MODEL == DSP_ILI9488 || DSP_MODEL == DSP_ILI9486 || DSP_MODEL == DSP_ST7789_170 || DSP_MODEL == DSP_ST7789 || DSP_MODEL == DSP_AXS15231B
  invalidateCoverCache();
#endif
}

Config config;
// Бит в store._reserved: 1 = выключить показ обложек на дисплее (runtime).
static const uint16_t kReservedFlagDisplayCoverDisabled = 0x0001u;

// Очередь CoverDL (нужны до setDisplayCoversEnabled — сброс при выключении показа).
static String g_lastCoverKey = "";
static uint32_t g_lastCoverScheduleMs = 0;
static String g_pendingCoverUrl = "";
static String g_pendingCoverKey = "";
static volatile bool g_coverScheduleDeferred = false;
// CoverDL для TFT: не стартовать HTTPS, пока нет playing-SCREENBLANK.
static volatile bool g_coverWaitForScreensaver = false;
// millis() входа в SCREENBLANK (0 = сейчас не в cover-скринсейвере).
static uint32_t g_coverScreensaverEnteredMs = 0;

static void coverClearPendingOnStreamLost() {
  g_coverScheduleDeferred = false;
  g_coverWaitForScreensaver = false;
  g_pendingCoverUrl = "";
  g_pendingCoverKey = "";
}

String currentCoverUrl = "/logo.svg";
String streamCoverUrl = "";

/*
 * Toast «обложка отложена» только если на экране ещё нет реальной обложки.
 * При cache-hit WebUI/TFT уже показывают JPEG — повторный defer не должен пугать пользователя.
 */
static void coverDeferScheduleWithToast() {
  g_coverScheduleDeferred = true;
  if (currentCoverUrl.length() <= 1 || currentCoverUrl == "/logo.svg") {
    transientNotifier.show(TN_COVER_DEFERRED, "Обложка отложена — приоритет звуку");
  }
}

bool isDisplayCoversAllowedByBuild() {
  return DISPLAY_COVERS_ENABLE;
}

bool isDisplayCoversEnabled() {
  if (!DISPLAY_COVERS_ENABLE) return false;
  return (config.store._reserved & kReservedFlagDisplayCoverDisabled) == 0;
}

// "Display Cover" — это только про показ на TFT.
// Когда выключатель Display Cover OFF, ESP НЕ должна тратить ресурсы на загрузку/кэширование обложек:
// в этом режиме Web-UI должен показывать обложки сам (browser iTunes / прямые icy-logo URL).
static inline bool isCoversEnabledForWebOrDisplay() {
  return isDisplayCoversEnabled();
}

void setDisplayCoversEnabled(bool enabled) {
  uint16_t next = config.store._reserved;
  if (!enabled) next |= kReservedFlagDisplayCoverDisabled;
  else next &= (uint16_t)~kReservedFlagDisplayCoverDisabled;
  if (next == config.store._reserved) return;
  // Важно: commit=true, иначе состояние "Display Cover" не сохраняется после перезагрузки
  // (аналогично как работает flipscreen).
  config.saveValue(&config.store._reserved, next, true);
  // Сброс очереди CoverDL: при выключении показа не должно быть фоновых загрузок в кэш.
  // Если Web-обложки включены, не гасим CoverDL — иначе Web перестанет получать актуальную картинку.
  if (!enabled && !WEBUI_COVERS_ENABLE) {
    g_pendingCoverUrl = "";
    g_pendingCoverKey = "";
    g_coverScheduleDeferred = false;
    g_coverWaitForScreensaver = false;
    g_lastCoverKey = "";
    currentCoverUrl = "/logo.svg";
  }
  updateDisplayCover();
  display.putRequest(NEWMODE, CLEAR);
  display.putRequest(NEWMODE, PLAYER);
}

void u8fix(char *src){
  // Защита от переполнения: пустая строка или NULL — strlen-1 даёт доступ вне массива.
  if (!src) return;
  size_t len = strlen(src);
  if (len == 0) return;
  char last = src[len - 1];
  if ((uint8_t)last >= 0xC2) src[len - 1] = '\0';
}

bool Config::_isFSempty() {
  // ВАЖНО: не трогаем LittleFS до монтирования, иначе получаем ошибки "File system is not mounted"
  // Старая логика ниже остаётся как есть, только добавляем защиту.
  if (!littleFsReady) return true;
  if (LittleFS.exists("/www/settings.html")) LittleFS.remove("/www/settings.html");
  if (LittleFS.exists("/www/update.html")) LittleFS.remove("/www/update.html");
  if (LittleFS.exists("/www/index.html")) LittleFS.remove("/www/index.html");
  if (LittleFS.exists("/www/ir.html")) LittleFS.remove("/www/ir.html");
  if (LittleFS.exists("/www/elogo.png")) LittleFS.remove("/www/elogo.png");
  if (LittleFS.exists("/www/elogo84.png")) LittleFS.remove("/www/elogo84.png");

  const char* core[][2] = {
    { "script.js.gz", "script.js" }, { "style.css.gz", "style.css" },
    { "player.html.gz", "player.html" }, { "options.html.gz", "options.html" },
    { "cover.js.gz", "cover.js" }, { "facts.js.gz", "facts.js" },
    { "dragpl.js.gz", "dragpl.js" },
  };
  for (auto &f : core) {
    char fullpath[32];
    snprintf(fullpath, sizeof(fullpath), "/www/%s", f[0]);
    if (LittleFS.exists(fullpath)) continue;
    snprintf(fullpath, sizeof(fullpath), "/www/%s", f[1]);
    if (LittleFS.exists(fullpath)) continue;
    Serial.printf("[FS] missing www: %s or %s\n", f[0], f[1]);
    return true;
  }
  if (!LittleFS.exists("/www/theme.css")) {
    Serial.println("[FS] missing www: theme.css");
    return true;
  }
#if IR_PIN != 255
  const char* irOnly[][2] = {
    { "ir.css.gz", "ir.css" }, { "irrecord.html.gz", "irrecord.html" }, { "ir.js.gz", "ir.js" },
  };
  for (auto &f : irOnly) {
    char fullpath[32];
    snprintf(fullpath, sizeof(fullpath), "/www/%s", f[0]);
    if (LittleFS.exists(fullpath)) continue;
    snprintf(fullpath, sizeof(fullpath), "/www/%s", f[1]);
    if (LittleFS.exists(fullpath)) continue;
    Serial.printf("[FS] missing www (IR): %s or %s\n", f[0], f[1]);
    return true;
  }
#endif
  return false;
}

// 1. Глобальные данные
String metaTitle = "";
String metaArtist = "";
String currentFact = "";  // Для фактов про композицию (TrackFacts плагин)
bool littleFsReady = false;

// Семафор для потокобезопасного доступа к LittleFS между задачами
SemaphoreHandle_t littleFsMutex = NULL;

// === PSRAM‑кэш для WebUI ===
// Кэшируем ВСЕ ключевые файлы WebUI в PSRAM.
// Это критически важно для мобильных браузеров: они запрашивают 6-8 файлов
// параллельно. Если все файлы читаются из LittleFS одновременно (через SPI),
// каждый запрос медленный → WebSocket таймаутит → Web «умирает».
// В PSRAM отдача мгновенная — нет блокировок SPI, нет задержек.
uint8_t* g_psramScriptJs      = nullptr;
size_t   g_psramScriptJsSize  = 0;
uint8_t* g_psramStyleCss      = nullptr;
size_t   g_psramStyleCssSize  = 0;
uint8_t* g_psramPlayerHtml    = nullptr;
size_t   g_psramPlayerHtmlSize = 0;
uint8_t* g_psramCoverJs       = nullptr;
size_t   g_psramCoverJsSize   = 0;
uint8_t* g_psramFactsJs       = nullptr;
size_t   g_psramFactsJsSize   = 0;
uint8_t* g_psramLogoSvg       = nullptr;
size_t   g_psramLogoSvgSize   = 0;
uint8_t* g_psramThemeCss      = nullptr;
size_t   g_psramThemeCssSize  = 0;
uint8_t* g_psramOptionsHtml   = nullptr;
size_t   g_psramOptionsHtmlSize = 0;
// dragpl.js (плейлист drag-n-drop в WebUI) — без PSRAM async_tcp делал LittleFS.exists() → TWDT.
uint8_t* g_psramDragplJs       = nullptr;
size_t   g_psramDragplJsSize   = 0;

static const char* kCoverDir = "/station_covers";
static const char* kCoverCacheDir = "/station_covers/cache";
static const char* kCoverCacheIndex = "/station_covers/cache_index.txt";
static const size_t kMaxCoverBytes = 600 * 1024;
static const size_t kCoverCacheBytes = COVER_CACHE_MAX_BYTES;

bool g_coverTaskRunning = false;
// Определение флага — см. подробный комментарий в config.h.
// Инициализируем false: ни одна задача CoverDL не запущена при старте прошивки.
volatile bool g_coverDlCancelRequested = false;
// v0.8.139: CoverDL отложен, пока FactsTask держит HTTPS — иначе параллельный mbedTLS → OOM (шпаргалка §7.2).
static bool g_sdCoverReady = false;
// Ключ последней сохранённой обложки из ID3-тега (по имени файла).
// Используется чтобы handleCoverArt мог найти уже готовую обложку
// не дожидаясь установки metaTitle (которая откладывается на 500мс debounce).
String g_sdFileKey = "";

// Отложенная запись обложки из SD в LittleFS — буфер и ключ передаются в фоновую задачу.
// Аудио-задача только заполняет PSRAM и даёт семафор; запись в FS выполняется в sdCoverWriteTask.
static uint8_t* g_pendingSdCoverBuf = nullptr;
static size_t g_pendingSdCoverSize = 0;
static char g_pendingSdCoverKey[128] = {0};
static SemaphoreHandle_t g_sdCoverWriteSem = nullptr;
static TaskHandle_t g_sdCoverWriteTaskHandle = nullptr;

// Флаг переключения режимов - блокирует сетевые операции
volatile bool g_modeSwitching = false;

// Timestamp последнего переключения режима для cooldown периода
volatile unsigned long g_modeSwitchTime = 0;

// Флаг переключения станции в WEB режиме.
// Ставится на короткое время во время PR_PLAY, чтобы отложить тяжёлые SSL-задачи плагинов.
volatile bool g_stationSwitching = false;

// Timestamp последнего переключения станции для короткого cooldown периода.
volatile unsigned long g_stationSwitchTime = 0;

// Сердцебиение главного цикла (main loop на core 1). Задача на core 0 использует для обнаружения зависания и мягкого WiFi reconnect.
volatile uint32_t g_mainLoopHeartbeatMs = 0;

// Период охлаждения после переключения режима (в миллисекундах)
// В течение этого времени сетевые операции (обложки, факты) блокируются
// 30 секунд - даём время WebSocket соединениям восстановиться после переключения
#define MODE_SWITCH_COOLDOWN_MS 30000

// Минимальный heap для SSL операций (150KB)
// Поднимаем порог, чтобы не запускать SSL при низкой памяти:
// в SD-режиме это снижает риск подвисаний WebUI и таймаутов AsyncTCP.
#define MIN_HEAP_FOR_SSL 150000

// Проверка находимся ли мы в периоде охлаждения после переключения режима
bool isModeSwitchCooldown() {
  if (g_modeSwitching) return true;
  if (g_modeSwitchTime == 0) return false;
  unsigned long elapsed = millis() - g_modeSwitchTime;
  return elapsed < MODE_SWITCH_COOLDOWN_MS;
}

// Проверка короткого cooldown после переключения станции.
// В отличие от смены режима, окно небольшое: только для разгрузки момента старта нового стрима.
bool isStationSwitchCooldown() {
  if (g_stationSwitching) return true;
  if (g_stationSwitchTime == 0) return false;
  unsigned long elapsed = millis() - g_stationSwitchTime;
  return elapsed < STATION_SWITCH_COOLDOWN_MS;
}

// Жёсткий "карантин" SSL после переключения станции.
// Логика:
// 1) Пока PR_PLAY ещё выполняется (g_stationSwitching=true), блок активен.
// 2) После завершения PR_PLAY держим дополнительное окно STATION_SWITCH_SSL_BLOCK_MS.
// Цель: исключить одновременный старт тяжёлых HTTPS-задач (Cover/TrackFacts)
// в самый чувствительный момент старта нового потока.
bool isStationSwitchSslBlock() {
  // Если переключение ещё в процессе — это всегда запрещённое окно.
  if (g_stationSwitching) return true;
  // Если переключений ещё не было — блок не нужен.
  if (g_stationSwitchTime == 0) return false;
  // Проверяем, прошло ли жёсткое окно после последнего переключения станции.
  unsigned long elapsed = millis() - g_stationSwitchTime;
  return elapsed < STATION_SWITCH_SSL_BLOCK_MS;
}

// Безопасно ли делать SSL для обложек — разрешено и в SD-режиме (пониженный порог heap).
// Обложки на дисплее — единственная SSL-задача, которая допустима в PM_SDCARD,
// потому что это одиночный HTTP-запрос к iTunes, не конкурирующий с аудиопотоком.
// Порог: MIN_HEAP_FOR_SSL_COVERS в options.h / myoptions.h (как в стабильной 0.8.92 + запас под S3).
// Флаг «плейлист/индекс занят»: true во время setFavorite и indexSDPlaylistFile.
// В эти моменты не запускаем загрузку обложек и TrackFacts, чтобы не конкурировать с FS и не грузить SSL.
volatile bool g_playlistBusy = false;

bool isPlaylistBusy() {
  return g_playlistBusy || config.sdIndexing;
}

// Время установки служебного заголовка "[соединение]" (LANG::const_PlConnect) для WEB-режима.
// Используется для таймаута: если за 5 секунд станция не прислала ни одного тега,
// автоматически меняем заголовок на имя станции, чтобы не висел старый тег.
static uint32_t g_connectTitleSinceMs = 0;

// Флаг: хотя бы раз получили валидное время по SNTP (год > 2020).
volatile bool g_ntpTimeSyncedOnce = false;

// Останавливает периодический SNTP lwIP (pool.ntp.org DNS в tcpip-thread).
void pauseLwIpBackgroundSntp() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
}

// Гасим SNTP только после успешной первой синхронизации и вне timeTask — иначе часы «застревают» на 1970.
void pauseLwIpBackgroundSntpForUserNet() {
  if (!g_ntpTimeSyncedOnce) return;
  if (timekeeper.busy) return;
  pauseLwIpBackgroundSntp();
}

// Оценка «крупнейшего блока» БЕЗ обхода кучи.
// ESP.getMaxAllocHeap() делает tlsf_walk_pool() под спинлоком с запретом прерываний —
// в гонке с playerTask/coverDownloadTask/Wi-Fi ISR это даёт Interrupt WDT и ребут.
// Кэш с TTL НЕ спасает: один обход в coverDownloadTask + malloc в playerTask = краш.
// Решение: НИКОГДА не вызываем getMaxAllocHeap(). Оценка max-блока = freeHeap/2
// (типичная фрагментация при воспроизведении). Это восстанавливает старую «строгость»
// гейтов SSL без обхода кучи — меньше фоновых HTTPS → WebUI и Wi-Fi отзывчивее.
uint32_t cachedMaxAllocHeap() {
  return ESP.getFreeHeap() / 2u;
}

// Пороги мягкого гейта HTTP-прокси фактов (раньше были литералами внутри isSafeForFactsHttpProxy).
// Вынесены, чтобы factsNetGateMinFreeHeap() считал запас под стек FactsTask от тех же чисел.
static constexpr uint32_t kFactsHttpProxyNeedFree = 28000u;
static constexpr uint32_t kFactsHttpProxyNeedMax  = 14000u;

// WEB-поток идёт по HTTPS? Берём только config.station.url (массив фиксированного размера в Config).
// player.getLastHost() сюда НЕ используем: это ps_ptr, который playerTask на другом ядре может
// перевыделить при реконнекте — чтение отсюда было бы гонкой с освобождением памяти.
// Если плеер не в PLAYING — второго TLS рядом с аудио нет, политика не нужна.
bool isWebAudioStreamHttps() {
  if (config.getMode() != PM_WEB) return false;
  if (player.status() != PLAYING) return false;
  return strncasecmp(config.station.url, "https://", 8) == 0;
}

// Политика «звук первым» для фактов: при HTTPS-потоке прямой TLS фактов не открываем
// (путь HTTP-прокси к VPS идёт через isSafeForFactsHttpProxy и сюда не попадает).
// Одноразовый accept: веб-диалог или свайп на TFT (риск без оверлея в текущей версии).
static volatile bool g_factsAcceptHttpsTlsRisk = false;

void factsAcceptHttpsTlsRiskOnce() {
  g_factsAcceptHttpsTlsRisk = true;
}

void factsClearHttpsTlsRiskAccept() {
  g_factsAcceptHttpsTlsRisk = false;
}

bool isHttpsAudioBlockingFactsTls() {
#if BLOCK_FACTS_TLS_ON_HTTPS_AUDIO
  if (g_factsAcceptHttpsTlsRisk) return false;
  return isWebAudioStreamHttps();
#else
  return false;
#endif
}

bool trackFactsHttpsNeedsRiskDialog() {
  if (!isWebAudioStreamHttps()) return false;
  // HTTP-прокси у текущего AI-провайдера → второго TLS нет, диалог не нужен.
  const uint8_t prov = config.store.trackFactsProvider;
  if (prov == 0) {
    return trackFactsEffectiveProxyGemini(config.store.trackFactsProxyGemini).length() == 0;
  }
  if (prov == 1) {
    return trackFactsEffectiveProxyDeepSeek(config.store.trackFactsProxyDeepSeek).length() == 0;
  }
  if (prov == 4) {
    return trackFactsEffectiveProxyGroq(config.store.trackFactsProxyGroq).length() == 0;
  }
  // iTunes / Last.fm — прямой HTTPS фактов → риск на https-аудио.
  return true;
}

// Эффективный минимум freeHeap, при котором гейт фактов пропустит запрос.
// ВАЖНО: cachedMaxAllocHeap() = freeHeap/2, поэтому порог «max-блока» X равносилен freeHeap >= 2*X.
// Если оценку max-блока когда-нибудь заменят на реальный замер — эту функцию нужно пересмотреть.
uint32_t factsNetGateMinFreeHeap(bool httpProxyPath) {
  const uint32_t needFree = httpProxyPath ? kFactsHttpProxyNeedFree : (uint32_t)MIN_HEAP_FOR_SSL_FACTS;
  const uint32_t needMax  = httpProxyPath ? kFactsHttpProxyNeedMax  : (uint32_t)MIN_MAX_ALLOC_HEAP_FOR_SSL;
  const uint32_t viaMax = needMax * 2u;
  return (needFree > viaMax) ? needFree : viaMax;
}

// ---------- Диагностика кучи (шаг 0, только чтение) ----------
// Колбэк неудачных выделений памяти: IDF вызывает его, когда heap_caps_malloc/malloc не нашёл блок
// ни в одной подходящей куче (в т.ч. esp-aes → MALLOC_CAP_DMA). Внутри колбэка ничего не печатаем
// и не выделяем — только запоминаем размер/тип памяти/функцию; печатает heapDiagPoll() из loop().
static volatile uint32_t g_heapDiagFailCount = 0;
static volatile uint32_t g_heapDiagLastFailSize = 0;
static volatile uint32_t g_heapDiagLastFailCaps = 0;
static const char* volatile g_heapDiagLastFailFunc = nullptr;
static volatile uint32_t g_heapDiagLastFailMs = 0;

static void heapDiagAllocFailedCb(size_t size, uint32_t caps, const char* functionName) {
  g_heapDiagFailCount = g_heapDiagFailCount + 1u;
  g_heapDiagLastFailSize = (uint32_t)size;
  g_heapDiagLastFailCaps = caps;
  g_heapDiagLastFailFunc = functionName;  // строковый литерал IDF во flash — указатель живёт всегда
  g_heapDiagLastFailMs = millis();
}

void heapDiagInit() {
#if HEAP_DIAG_ENABLE && defined(ESP32) && defined(ARDUINO)
  heap_caps_register_failed_alloc_callback(heapDiagAllocFailedCb);
#endif
}

// Раз в HEAP_DIAG_INTERVAL_MS печатает строку [HEAPDIAG]. Все вызовы — O(число куч), без обхода блоков
// и без спинлока с запретом прерываний (в отличие от getMaxAllocHeap/largest_free_block → INT-WDT).
// int    — свободно во внутренней RAM сейчас (то же, что ESP.getFreeHeap());
// intMin — минимум внутренней RAM с момента загрузки (ловит мгновенные провалы между строками);
// dma    — свободно в DMA-пригодной памяти (из неё esp-aes берёт буферы);
// dmaMin — минимум DMA-пригодной памяти с загрузки;
// psram  — свободно в PSRAM;
// fail   — сколько раз malloc не нашёл памяти; last — размер/caps/функция/сколько секунд назад;
// https/factsBusy/coverBusy — контекст: поток HTTPS, идёт ли FactsTask, идёт ли CoverDL.
void heapDiagPoll() {
#if HEAP_DIAG_ENABLE && defined(ESP32) && defined(ARDUINO)
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (lastMs != 0 && (uint32_t)(now - lastMs) < (uint32_t)HEAP_DIAG_INTERVAL_MS) return;
  lastMs = now;
  const uint32_t failCount = g_heapDiagFailCount;
  const uint32_t failAgoS = (failCount && g_heapDiagLastFailMs)
      ? (uint32_t)((now - g_heapDiagLastFailMs) / 1000u) : 0u;
  const char* failFunc = g_heapDiagLastFailFunc;
  Serial.printf("[HEAPDIAG] int=%u intMin=%u dma=%u dmaMin=%u psram=%u fail=%u last=%u/0x%X/%s/%us https=%d factsBusy=%d coverBusy=%d\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)failCount,
                (unsigned)g_heapDiagLastFailSize,
                (unsigned)g_heapDiagLastFailCaps,
                failFunc ? failFunc : "-",
                (unsigned)failAgoS,
                isWebAudioStreamHttps() ? 1 : 0,
                trackFactsPlugin.isRequestActive() ? 1 : 0,
                g_coverTaskRunning ? 1 : 0);
#endif
}

// Раз в TEMP_DIAG_INTERVAL_MS печатает [TEMP] °C с внутреннего датчика SoC (Arduino temperatureRead()).
// Вызов дешёвый; при ошибке чтения (−127 у части ядер) печатаем fail, чтобы не путать с реальным морозом.
void tempDiagPoll() {
#if TEMP_DIAG_ENABLE && defined(ESP32) && defined(ARDUINO)
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (lastMs != 0 && (uint32_t)(now - lastMs) < (uint32_t)TEMP_DIAG_INTERVAL_MS) return;
  lastMs = now;
  const float tC = temperatureRead();
  if (tC < -40.0f || tC > 125.0f) {
    Serial.printf("[TEMP] fail raw=%.1f\n", (double)tC);
    return;
  }
  Serial.printf("[TEMP] %.1f C\n", (double)tC);
#endif
}

// Стабилен ли линк для CoverDL/Facts (не во время beginReconnect и карантина после обрыва).
bool isWifiLinkStableForHeavyNet() {
  if (network.beginReconnect) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (millis() < g_wifiInstabilityGraceUntilMs) return false;
  return true;
}

// Общая проверка WEB-потока для фонового HTTP/HTTPS с заданным порогом буфера (%).
static bool isWebStreamReadyCore(uint8_t minBufferPercent) {
  if (config.getMode() != PM_WEB) {
    return true;
  }

  const uint32_t now = millis();
  const uint32_t total = player.getInBufferSize();
  const uint32_t filled = player.isRunning() ? player.inBufferFilled() : 0u;
  const uint32_t minimum = total
      ? (uint32_t)(((uint64_t)total * (uint32_t)minBufferPercent) / 100u)
      : 0u;
  const uint32_t streamHeartbeat = g_lastWebStreamActivityMs;
  const bool heartbeatFresh =
      streamHeartbeat != 0
      && streamHeartbeat <= now
      && (uint32_t)(now - streamHeartbeat) <= (uint32_t)BACKGROUND_NET_STREAM_SILENCE_MS;
  const bool ready =
      player.status() == PLAYING
      && player.isRunning()
      && player.connproc
      && !player.isWebStreamRecovering()
      /*
       * webRecoveryPending убран из gate ready:
       * после успешного PLAYING флаг мог держать gate false и spam «приоритет звука».
       * Активное восстановление по-прежнему блокирует через isWebStreamRecovering().
       */
      && isWifiLinkStableForHeavyNet()
      && total > 0
      && filled >= minimum
      && heartbeatFresh;

  if (!ready) {
    return false;
  }
  /*
   * BACKGROUND_NET_STABLE_MS=0 — gate созревает сразу после буфера/heartbeat/WiFi.
   * Иначе — непрерывное окно ведёт audio_stream_activity().
   */
  if ((uint32_t)BACKGROUND_NET_STABLE_MS == 0u) {
    return true;
  }
  return g_webStreamContinuousSinceMs != 0
      && g_webStreamContinuousSinceMs <= now
      && (uint32_t)(now - g_webStreamContinuousSinceMs) >= (uint32_t)BACKGROUND_NET_STABLE_MS;
}

bool isWebStreamReadyForCover() {
  /* Advanced/myoptions: MIN_AUDIO_BUFFER_FOR_SSL_COVERS_PERCENT (дефолт 13 в options.h).
   * Ниже 2% не пускаем — иначе CoverDL на пустом буфере. */
  uint8_t pct = (uint8_t)MIN_AUDIO_BUFFER_FOR_SSL_COVERS_PERCENT;
  if (pct < 2u) pct = 2u;
  return isWebStreamReadyCore(pct);
}

bool isWebStreamReadyForFacts() {
  return isWebStreamReadyCore((uint8_t)BACKGROUND_NET_FACTS_BUFFER_PERCENT);
}

bool isWebStreamReadyForBackgroundNet() {
  /* Legacy alias: прежний единый gate ≈ порог Facts. */
  return isWebStreamReadyForFacts();
}

bool shouldAbortBackgroundNetForAudio() {
  if (config.getMode() != PM_WEB) return false;

  const uint32_t now = millis();
  const uint32_t total = player.getInBufferSize();
  const uint32_t filled = player.isRunning() ? player.inBufferFilled() : 0u;
  const uint32_t emergencyMinimum = total
      ? (uint32_t)(((uint64_t)total * (uint32_t)BACKGROUND_NET_EMERGENCY_BUFFER_PERCENT) / 100u)
      : 0u;
  const uint32_t streamHeartbeat = g_lastWebStreamActivityMs;
  const bool heartbeatFresh =
      streamHeartbeat != 0
      && streamHeartbeat <= now
      && (uint32_t)(now - streamHeartbeat) <= (uint32_t)BACKGROUND_NET_STREAM_SILENCE_MS;

  return player.status() != PLAYING
      || !player.isRunning()
      || !player.connproc
      || player.isWebStreamRecovering()
      || player.webRecoveryPending()
      || !heartbeatFresh
      || total == 0
      || filled < emergencyMinimum;
}

/* Cover-SS gate (тело ниже): нужен forward — isSafeForSSLCovers раньше по файлу. */
static bool coverTftScreensaverGateOpen();

bool isSafeForSSLCovers() {
  if (g_modeSwitching) return false;
  if (isModeSwitchCooldown()) return false;
  if (isStationSwitchSslBlock()) return false;
  if (isPlaylistBusy()) return false; // Во время индексации/сохранения избранного не трогаем обложки
  // Пока syncTask делает getLocalTime/SNTP — не запускаем CoverDL (lwIP TCPIP lock, IDF 5.x).
  if (timekeeper.busy) return false;
  // Пока FactsTask в iTunes/HTTPS — не параллелим CoverDL (тот же lwIP DNS).
  if (trackFactsPlugin.isRequestActive()) return false;
  if (!isWifiLinkStableForHeavyNet()) return false;
  if (!isWebStreamReadyForCover()) return false;
#if BLOCK_COVER_TLS_ON_HTTPS_AUDIO
  /* PLAYER: второй TLS рядом с HTTPS-аудио нельзя.
   * Cover-SS (gate open): один JPEG на TFT — как 278; буфер/WiFi/abort после GET остаются. */
  if (isWebAudioStreamHttps() && !coverTftScreensaverGateOpen()) return false;
#endif
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_SSL_COVERS) return false;
  // Кэшированное значение — не обходим кучу под критической секцией на каждом тикере.
  // CoverDL — один короткий HTTPS; порог ниже, чем у Facts (см. MIN_MAX_ALLOC_HEAP_FOR_SSL_COVERS).
  uint32_t maxAlloc = cachedMaxAllocHeap();
  if (maxAlloc < MIN_MAX_ALLOC_HEAP_FOR_SSL_COVERS) {
    if (SSL_BLOCK_LOG_ENABLE) {
      static uint32_t lastLogMs = 0;
      uint32_t now = millis();
      if (now - lastLogMs >= 5000) {
        Serial.printf("[SSL] Blocked - max alloc %u < %u (covers), free=%u\n",
                      static_cast<unsigned>(maxAlloc),
                      static_cast<unsigned>(MIN_MAX_ALLOC_HEAP_FOR_SSL_COVERS),
                      static_cast<unsigned>(freeHeap));
        lastLogMs = now;
      }
    }
    return false;
  }
  return true;
}

// Безопасно ли делать SSL запросы (heap, WiFi, cooldown)
bool isSafeForSSL() {
  // Чтобы не заспамить монитор, логируем блокировки не чаще 1 раза в 5 сек.
  // Это особенно важно в SD-режиме, где проверки идут постоянно.
  static uint32_t lastLogMs = 0;
  auto logBlocked = [&](const char* msg) {
    if (!SSL_BLOCK_LOG_ENABLE) {
      return;
    }
    uint32_t now = millis();
    if (now - lastLogMs >= 5000) {
      Serial.println(msg);
      lastLogMs = now;
    }
  };

  // В SD-режиме отключаем любые SSL/HTTP задачи (обложки/факты).
  // Это снижает нагрузку на стек сети и память, чтобы WebUI не зависал
  // при воспроизведении локальных файлов.
  if (config.getMode() == PM_SDCARD) {
    logBlocked("[SSL] Blocked - SD mode (disabled)");
    return false;
  }

  // Проверяем cooldown
  if (isModeSwitchCooldown()) {
    logBlocked("[SSL] Blocked - mode switch cooldown");
    return false;
  }

  // Во время экрана настроек не запускаем новые SSL‑задачи (обложки/URL и т.п.),
  // чтобы не вызывать короткие фризы звука при открытии настроек.
  if (display.mode() == SETTINGS) {
    logBlocked("[SSL] Blocked - settings screen");
    return false;
  }
  
  // Проверяем WiFi
  if (WiFi.status() != WL_CONNECTED) {
    logBlocked("[SSL] Blocked - WiFi not connected");
    return false;
  }
  
  // Проверяем heap
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_SSL) {
    // Лимитируем частоту логов по low-heap аналогично другим блокировкам.
    if (SSL_BLOCK_LOG_ENABLE) {
      uint32_t now = millis();
      if (now - lastLogMs >= 5000) {
        Serial.printf("[SSL] Blocked - low heap: %u < %u\n", freeHeap, MIN_HEAP_FOR_SSL);
        lastLogMs = now;
      }
    }
    return false;
  }
  
  return true;
}

// Безопасно ли делать SSL запросы для TrackFacts (мягче по heap).
bool isSafeForSSLForFacts() {
  // Используем общие правила, но с отдельным порогом памяти.
  // Для SD-режима жёсткого запрета нет: ручной запрос фактов допускается при безопасных условиях.
  static uint32_t lastLogMs = 0;
  auto logBlocked = [&](const char* msg) {
    if (!SSL_BLOCK_LOG_ENABLE) {
      return;
    }
    uint32_t now = millis();
    if (now - lastLogMs >= 5000) {
      Serial.println(msg);
      lastLogMs = now;
    }
  };

  // Во время индексации SD или сохранения избранного не запускаем запросы фактов.
  if (isPlaylistBusy()) {
    logBlocked("[SSL] Blocked - playlist/index busy (facts)");
    return false;
  }
  if (isModeSwitchCooldown()) {
    logBlocked("[SSL] Blocked - mode switch cooldown (facts)");
    return false;
  }
  if (isStationSwitchSslBlock()) {
    logBlocked("[SSL] Blocked - station switch SSL window (facts)");
    return false;
  }
  // Пока syncTask/SNTP — не стартуем Facts (лог: assert sys_untimeout при hostByName + sntp_dns_found).
  if (timekeeper.busy) {
    logBlocked("[SSL] Blocked - SNTP/time sync busy (facts)");
    return false;
  }
  // Пока CoverDL в HTTPS — не параллелим Facts (lwIP DNS/UDP).
  if (g_coverTaskRunning) {
    logBlocked("[SSL] Blocked - cover download busy (facts)");
    return false;
  }
  // Во время экрана настроек не запускаем новые SSL‑запросы фактов, чтобы не трогать звук.
  if (display.mode() == SETTINGS) {
    logBlocked("[SSL] Blocked - settings screen (facts)");
    return false;
  }
  if (!isWifiLinkStableForHeavyNet()) {
    logBlocked("[SSL] Blocked - WiFi unstable/reconnect (facts)");
    return false;
  }
  if (!isWebStreamReadyForFacts()) {
    logBlocked("[SSL] Blocked - audio stream reserve/recovery (facts)");
    return false;
  }
  // Политика «звук первым»: поток уже держит свою TLS-сессию во внутренней RAM —
  // второй TLS фактов рядом с ним не открываем (провайдеры тоже зовут этот гейт перед HTTPS).
  if (isHttpsAudioBlockingFactsTls()) {
    logBlocked("[SSL] Blocked - HTTPS audio stream, no 2nd TLS (facts)");
    return false;
  }
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_SSL_FACTS) {
    if (SSL_BLOCK_LOG_ENABLE) {
      uint32_t now = millis();
      if (now - lastLogMs >= 5000) {
        Serial.printf("[SSL] Blocked - low heap for facts: %u < %u\n", freeHeap, MIN_HEAP_FOR_SSL_FACTS);
        lastLogMs = now;
      }
    }
    return false;
  }
  // Accept риска (диалог веба / свайп): после xTaskCreate free≈50K → est max≈25K
  // при пороге 30K — ложный abort. Пользователь уже согласился на риск второго TLS.
  if (g_factsAcceptHttpsTlsRisk) {
    return true;
  }
  // Кэшированное значение — этот путь идёт из on_ticker() каждую секунду (core1).
  uint32_t maxAlloc = cachedMaxAllocHeap();
  if (maxAlloc < MIN_MAX_ALLOC_HEAP_FOR_SSL) {
    if (SSL_BLOCK_LOG_ENABLE) {
      uint32_t now = millis();
      if (now - lastLogMs >= 5000) {
        Serial.printf("[SSL] Blocked - max alloc %u < %u (facts), free=%u\n",
                      static_cast<unsigned>(maxAlloc), static_cast<unsigned>(MIN_MAX_ALLOC_HEAP_FOR_SSL),
                      static_cast<unsigned>(freeHeap));
        lastLogMs = now;
      }
    }
    return false;
  }
  return true;
}

// Мягкий гейт для HTTP к своему VPS (Gemini/Groq proxy): без TLS handshake на ESP.
// Общие блокировки (cooldown/SNTP/Cover/WiFi) те же; пороги heap ниже, чем у прямого HTTPS.
bool isSafeForFactsHttpProxy() {
  static uint32_t lastLogMs = 0;
  auto logBlocked = [&](const char* msg) {
    if (!SSL_BLOCK_LOG_ENABLE) return;
    uint32_t now = millis();
    if (now - lastLogMs >= 5000) {
      Serial.println(msg);
      lastLogMs = now;
    }
  };
  if (isPlaylistBusy()) { logBlocked("[SSL] Blocked - playlist/index busy (facts-http)"); return false; }
  if (isModeSwitchCooldown()) { logBlocked("[SSL] Blocked - mode switch cooldown (facts-http)"); return false; }
  if (isStationSwitchSslBlock()) { logBlocked("[SSL] Blocked - station switch window (facts-http)"); return false; }
  if (timekeeper.busy) { logBlocked("[SSL] Blocked - SNTP busy (facts-http)"); return false; }
  if (g_coverTaskRunning) { logBlocked("[SSL] Blocked - cover busy (facts-http)"); return false; }
  if (display.mode() == SETTINGS) { logBlocked("[SSL] Blocked - settings (facts-http)"); return false; }
  if (!isWifiLinkStableForHeavyNet()) { logBlocked("[SSL] Blocked - WiFi unstable (facts-http)"); return false; }
  if (!isWebStreamReadyForFacts()) { logBlocked("[SSL] Blocked - audio reserve/recovery (facts-http)"); return false; }
  const uint32_t freeHeap = ESP.getFreeHeap();
  // HTTP к VPS заметно легче TLS к Google — пороги экспериментально ниже SSL-гейта.
  // Значения прежние (28000/14000), только вынесены в константы для factsNetGateMinFreeHeap().
  const uint32_t needFree = kFactsHttpProxyNeedFree;
  const uint32_t needMax = kFactsHttpProxyNeedMax;
  if (freeHeap < needFree) {
    if (SSL_BLOCK_LOG_ENABLE) {
      uint32_t now = millis();
      if (now - lastLogMs >= 5000) {
        Serial.printf("[SSL] Blocked - low heap for facts-http: %u < %u\n",
                      (unsigned)freeHeap, (unsigned)needFree);
        lastLogMs = now;
      }
    }
    return false;
  }
  if (cachedMaxAllocHeap() < needMax) {
    if (SSL_BLOCK_LOG_ENABLE) {
      uint32_t now = millis();
      if (now - lastLogMs >= 5000) {
        Serial.printf("[SSL] Blocked - max alloc for facts-http: %u < %u (free=%u)\n",
                      (unsigned)cachedMaxAllocHeap(), (unsigned)needMax, (unsigned)freeHeap);
        lastLogMs = now;
      }
    }
    return false;
  }
  return true;
}

#ifdef USE_DLNA
// DLNA_INTEGRATION_PLAN §7 п.6: тяжёлые DLNA-операции не должны совпадать с окнами
// переключения режима/станции и низким heap — иначе растёт риск фризов WebSocket и артефактов звука.
bool isSafeForDlnaHeavyWork() {
  // DLNA-плейлист и HTTP-контур имеют смысл только в WEB-режиме (в SD свой плейлист и другая логика).
  if (config.getMode() != PM_WEB) {
    return false;
  }
  // Согласованность с isSafeForSSL: пока открыт экран настроек на дисплее, не грузим сеть лишним DLNA.
  if (display.mode() == SETTINGS) {
    return false;
  }
  if (g_modeSwitching) {
    return false;
  }
  if (isModeSwitchCooldown()) {
    return false;
  }
  // Пока идёт индексация SD или тяжёлое сохранение избранного — не конкурируем за heap и SPI flash.
  if (isPlaylistBusy()) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  // Окно после смены станции: совпадает с политикой TrackFacts/обложек — не добавляем второй HTTPS/HTTP пик.
  if (isStationSwitchSslBlock()) {
    return false;
  }
  const uint32_t freeHeap = ESP.getFreeHeap();
  // Порог мягче полного SSL (MIN_HEAP_FOR_SSL), но достаточный, чтобы не стартовать при «красной» зоне кучи.
  if (freeHeap < (MIN_HEAP_FOR_SSL / 2)) {
    return false;
  }
  return true;
}
#endif

// === PSRAM-кэширование ключевых WebUI-файлов ===
// Кэшируем ВСЕ файлы WebUI, а не только script.js и style.css.
// Мобильные браузеры запрашивают 6-8 файлов одновременно.
// Если каждый файл читается из LittleFS (через SPI-флеш), параллельный доступ
// создаёт задержки > 5 сек → WebSocket таймаутит → Web «умирает» на телефонах.
// Из PSRAM всё отдаётся мгновенно — нет блокировок SPI.
// Суммарный объём кэша ~42 КБ (из 8 МБ PSRAM) — ничтожно мало.
// Верхняя граница одного файла: при битой таблице LittleFS size() может быть мусором → защита PSRAM/кучи.
#ifndef WEB_ASSET_CACHE_MAX_BYTES
#define WEB_ASSET_CACHE_MAX_BYTES (512 * 1024)
#endif
void initWebAssetsCache() {
  if (!psramInit()) {
    Serial.println("[WEB-CACHE] PSRAM не обнаружен, кэш WebUI отключён");
    return;
  }

  // Защита от повторной инициализации.
  if (g_psramScriptJs || g_psramStyleCss) {
    Serial.println("[WEB-CACHE] Кэш WebUI уже инициализирован, пропускаем");
    return;
  }

  // Расширенный список: все ключевые ресурсы WebUI ( .gz или несжатый из data/www ).
  struct AssetDesc {
    const char* pathGz;
    const char* pathPlain;
    uint8_t**   bufPtr;
    size_t*     sizePtr;
    const char* logName;
    bool        isGzip;
  };
  AssetDesc assets[] = {
    { "/www/script.js.gz",    "/www/script.js",    &g_psramScriptJs,    &g_psramScriptJsSize,    "script.js",    true  },
    { "/www/style.css.gz",    "/www/style.css",    &g_psramStyleCss,    &g_psramStyleCssSize,    "style.css",    true  },
    { "/www/player.html.gz",  "/www/player.html",  &g_psramPlayerHtml,  &g_psramPlayerHtmlSize,  "player.html",  true  },
    { "/www/cover.js.gz",     "/www/cover.js",     &g_psramCoverJs,     &g_psramCoverJsSize,     "cover.js",     true  },
    { "/www/facts.js.gz",     "/www/facts.js",     &g_psramFactsJs,     &g_psramFactsJsSize,     "facts.js",     true  },
    { "/www/logo.svg.gz",     "/www/logo.svg",     &g_psramLogoSvg,     &g_psramLogoSvgSize,     "logo.svg",     true  },
    { "/www/theme.css",       nullptr,             &g_psramThemeCss,    &g_psramThemeCssSize,    "theme.css",    false },
    { "/www/options.html.gz", "/www/options.html", &g_psramOptionsHtml, &g_psramOptionsHtmlSize, "options.html", true  },
    { "/www/dragpl.js.gz",    "/www/dragpl.js",    &g_psramDragplJs,    &g_psramDragplJsSize,    "dragpl.js",    true  },
  };

  for (auto &a : assets) {
    *a.bufPtr  = nullptr;
    *a.sizePtr = 0;

    // Какой файл реально есть в LittleFS: .gz или plain (для theme.css — только plain).
    const char* loadPath = nullptr;
    bool loadGzip = a.isGzip;
    if (LittleFS.exists(a.pathGz)) {
      loadPath = a.pathGz;
    } else if (a.pathPlain && LittleFS.exists(a.pathPlain)) {
      loadPath = a.pathPlain;
      loadGzip = false;
    }
    if (!loadPath) {
      Serial.printf("[WEB-CACHE] Файл %s не найден в LittleFS, кэш пропущен\n", a.logName);
      continue;
    }

    // Для безопасности читаем файл под мьютексом LittleFS.
    if (!lockLittleFS(2000)) {
      Serial.printf("[WEB-CACHE] Не удалось взять мьютекс для %s, кэш пропущен\n", a.logName);
      continue;
    }

    File f = LittleFS.open(loadPath, "r");
    if (!f) {
      unlockLittleFS();
      Serial.printf("[WEB-CACHE] Ошибка открытия %s, кэш пропущен\n", loadPath);
      continue;
    }

    size_t size = f.size();
    if (size == 0) {
      f.close();
      unlockLittleFS();
      Serial.printf("[WEB-CACHE] Пустой файл %s, кэширование не имеет смысла\n", loadPath);
      continue;
    }
    if (size > WEB_ASSET_CACHE_MAX_BYTES) {
      f.close();
      unlockLittleFS();
      Serial.printf("[WEB-CACHE] Файл %s слишком большой для кэша (%u > %u), пропуск\n",
                    loadPath, (unsigned)size, (unsigned)WEB_ASSET_CACHE_MAX_BYTES);
      continue;
    }

    // Выделяем буфер в PSRAM под весь файл разом.
    uint8_t* buf = (uint8_t*)ps_malloc(size);
    if (!buf) {
      f.close();
      unlockLittleFS();
      Serial.printf("[WEB-CACHE] Не удалось выделить %u байт в PSRAM для %s\n", (unsigned)size, loadPath);
      continue;
    }

    // Читаем файл небольшими порциями, чтобы не блокировать систему.
    size_t readTotal = 0;
    while (readTotal < size) {
      size_t chunk = size - readTotal;
      if (chunk > 1024) chunk = 1024;
      int r = f.read(buf + readTotal, chunk);
      if (r <= 0) break;
      readTotal += (size_t)r;
      // Короткая задержка, чтобы не отнимать все тики у других задач
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    f.close();
    unlockLittleFS();

    if (readTotal != size) {
      // Если прочитали не весь файл — освобождаем память и не используем кэш.
      free(buf);
      Serial.printf("[WEB-CACHE] Не удалось полностью прочитать %s (%u/%u байт)\n",
                    loadPath, (unsigned)readTotal, (unsigned)size);
      continue;
    }

    // [v0.8.205] Валидация gzip-контента: не кэшировать заведомо битый .gz.
    // Корень "белого экрана": в data-разделе иногда оказывался обрезанный script.js.gz
    // (например 2 байта — только сигнатура 0x1F 0x8B). Проверка только сигнатуры (v0.8.201)
    // такой файл ПРОПУСКАЛА как валидный. Минимальный корректный gzip = 10 байт заголовок + 8 байт
    // footer (CRC32+ISIZE) = 18 байт. Если контент короче или нет сигнатуры — кэш пропускаем,
    // и обработчик отдачи откатится на несжатый файл из LittleFS (если он есть).
    if (loadGzip && (size < 18 || buf[0] != 0x1F || buf[1] != 0x8B)) {
      free(buf);
      Serial.printf("[WEB-CACHE] %s выглядит как битый gzip (%u байт), кэш пропущен — fallback на LittleFS\n",
                    loadPath, (unsigned)size);
      continue;
    }

    /* Устаревший .gz при живом plain: кэш предпочитает .gz → браузер получает старый JS → вечный спиннер.
     * ISIZE (последние 4 байта gzip, LE) = размер несжатых данных; не совпал с plain → берём plain. */
    if (loadGzip && a.pathPlain && LittleFS.exists(a.pathPlain)) {
      File pf = LittleFS.open(a.pathPlain, "r");
      if (pf) {
        const size_t plainSize = pf.size();
        pf.close();
        const uint32_t isize = (uint32_t)buf[size - 4]
                             | ((uint32_t)buf[size - 3] << 8)
                             | ((uint32_t)buf[size - 2] << 16)
                             | ((uint32_t)buf[size - 1] << 24);
        if (plainSize > 0 && isize != (uint32_t)plainSize) {
          Serial.printf("[WEB-CACHE] %s.gz УСТАРЕЛ (ISIZE=%u, plain=%u) — грузим plain\n",
                        a.logName, (unsigned)isize, (unsigned)plainSize);
          free(buf);
          buf = nullptr;
          if (!lockLittleFS(2000)) {
            Serial.printf("[WEB-CACHE] Нет мьютекса для plain %s, кэш пропущен\n", a.logName);
            continue;
          }
          File fp = LittleFS.open(a.pathPlain, "r");
          if (!fp) {
            unlockLittleFS();
            continue;
          }
          const size_t psz = fp.size();
          if (psz == 0 || psz > WEB_ASSET_CACHE_MAX_BYTES) {
            fp.close();
            unlockLittleFS();
            continue;
          }
          uint8_t* pbuf = (uint8_t*)ps_malloc(psz);
          if (!pbuf) {
            fp.close();
            unlockLittleFS();
            continue;
          }
          const size_t got = fp.read(pbuf, psz);
          fp.close();
          unlockLittleFS();
          if (got != psz) {
            free(pbuf);
            continue;
          }
          buf = pbuf;
          size = psz;
          loadGzip = false;
        }
      }
    }

    *a.bufPtr  = buf;
    *a.sizePtr = size;
    Serial.printf("[WEB-CACHE] %s загружен в PSRAM (%u байт)%s\n", a.logName, (unsigned)size, loadGzip ? "" : " (plain)");
  }
}

static void ensureCoverDir() {
  if (!littleFsReady) return;
  
  if (!LittleFS.exists(kCoverDir)) {
    LittleFS.mkdir(kCoverDir);
  }
  
  if (!LittleFS.exists(kCoverCacheDir)) {
    LittleFS.mkdir(kCoverCacheDir);
  }
}

static bool isHttpUrl(const String& url) {
  return url.startsWith("http://") || url.startsWith("https://");
}

// В icy-url / icy-logo часто приходит ссылка на сайт станции (Telegram, YouTube и т.д.),
// а не прямой JPEG/PNG — скачивание как картинки даёт connection refused / мусор и съедает TLS/heap.
// Не требуем расширение в пути — иначе ломаются нормальные CDN без .jpg в URL.
// Отсекаем только «голый» корень хоста (https://roxyradio.hu/, https://example.com) — это HTML, не картинка.
bool isLikelyDirectCoverImageUrl(const String& url) {
  if (!isHttpUrl(url)) return false;
  String low = url;
  low.trim();
  low.toLowerCase();
  static const char* kDeny[] = {
      "t.me/",       "telegram.me/", "youtube.com/", "youtu.be/",
      "facebook.com/", "fb.com/",    "instagram.com/", "twitter.com/", "x.com/",
      "vk.com/",     "ok.ru/",      "tiktok.com/",    "soundcloud.com/",
  };
  for (size_t i = 0; i < sizeof(kDeny) / sizeof(kDeny[0]); ++i) {
    if (low.indexOf(kDeny[i]) >= 0) return false;
  }
  int q = low.indexOf('?');
  String base = (q >= 0) ? low.substring(0, q) : low;
  int scheme = base.indexOf("://");
  if (scheme < 0) return false;
  int pathStart = base.indexOf('/', scheme + 3);
  String pathOnly;
  if (pathStart < 0) {
    pathOnly = "/";
  } else {
    pathOnly = base.substring(pathStart);
  }
  while (pathOnly.length() > 1 && pathOnly.endsWith("/")) {
    pathOnly = pathOnly.substring(0, pathOnly.length() - 1);
  }
  if (pathOnly.length() == 0 || pathOnly == "/") return false;
  return true;
}

static uint32_t coverHash(const String& key) {
  uint32_t hash = 5381;
  for (size_t i = 0; i < key.length(); ++i) {
    hash = ((hash << 5) + hash) + static_cast<uint8_t>(key[i]);
  }
  return hash;
}

static String coverCachePathForKey(const String& key) {
  ensureCoverDir();
  char buf[64];
  snprintf(buf, sizeof(buf), "%s/%08lx.jpg", kCoverCacheDir, static_cast<unsigned long>(coverHash(key)));
  return String(buf);
}

String coverCacheUrlForKey(const String& key) {
  char buf[64];
  snprintf(buf, sizeof(buf), "/sc/cache/%08lx.jpg", static_cast<unsigned long>(coverHash(key)));
  return String(buf);
}

bool coverCacheExists(const String& key) {
  if (!littleFsReady) return false;
  // Пока coverDownloadTask на другом ядре — не лезем в LittleFS из playerTask.
  if (g_coverTaskRunning) return false;
  // LittleFS.exists может быть медленным при высокой нагрузке на ФС (vfs_littlefs_stat).
  // Кэшируем результат для текущего ключа, чтобы избежать повторных системных вызовов в setTitle.
  static String lastCheckedKey;
  static bool lastResult = false;
  if (key == lastCheckedKey) return lastResult;
  
  lastCheckedKey = key;
  lastResult = LittleFS.exists(coverCachePathForKey(key));
  return lastResult;
}

static String normalizeCoverKey(const String& value) {
  String normalized;
  normalized.reserve(value.length());
  String trimmed = value;
  trimmed.trim();
  for (size_t i = 0; i < trimmed.length(); ++i) {
    unsigned char ch = static_cast<unsigned char>(trimmed[i]);
    if (isalnum(ch)) {
      normalized += static_cast<char>(tolower(ch));
    }
  }
  return normalized;
}

static bool findManualCover(const String& stationName, String& coverUrl) {
  if (!littleFsReady || !stationName.length()) {
    return false;
  }
  // Не сканируем LittleFS из playerTask, пока coverDownloadTask активна.
  if (g_coverTaskRunning) {
    return false;
  }
  
  String targetKey = normalizeCoverKey(stationName);
  if (!targetKey.length()) {
    return false;
  }

  auto checkDirect = [&](String candidate) -> bool {
    if (!candidate.length()) return false;
    String path = String("/station_covers/") + candidate + ".jpg";
    bool exists = LittleFS.exists(path);
    if (exists) {
      coverUrl = String("/sc/") + candidate + ".jpg";
      return true;
    }
    return false;
  };

  String trimmed = stationName;
  trimmed.trim();
  if (checkDirect(trimmed)) return true;
  
  String underscore = trimmed;
  underscore.replace(" ", "_");
  if (checkDirect(underscore)) return true;

  File dir = LittleFS.open(kCoverDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      String fullName = entry.name();
      if (fullName.endsWith(".jpg") || fullName.endsWith(".JPG")) {
        int slash = fullName.lastIndexOf('/');
        String baseName = (slash >= 0) ? fullName.substring(slash + 1) : fullName;
        int dot = baseName.lastIndexOf('.');
        if (dot > 0) {
          String core = baseName.substring(0, dot);
          String normalizedCore = normalizeCoverKey(core);
          if (normalizedCore == targetKey) {
            coverUrl = String("/sc/") + baseName;
            entry.close();
            dir.close();
            return true;
          }
        }
      }
    }
    entry.close();
  }

  dir.close();
  return false;
}

struct CoverCacheEntry {
  uint32_t ts;
  size_t size;
  String path;
};

static uint32_t coverTimestamp() {
  if (network.timeinfo.tm_year > 100) {
    return static_cast<uint32_t>(time(nullptr));
  }
  return static_cast<uint32_t>(millis());
}

static void loadCoverCacheIndex(std::vector<CoverCacheEntry>& entries) {
  entries.clear();
  if (!littleFsReady) return;
  File f = LittleFS.open(kCoverCacheIndex, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    unsigned long ts = 0;
    unsigned long size = 0;
    char path[96] = {0};
    if (sscanf(line.c_str(), "%lu %lu %95s", &ts, &size, path) == 3) {
      CoverCacheEntry entry;
      entry.ts = static_cast<uint32_t>(ts);
      entry.size = static_cast<size_t>(size);
      entry.path = String(path);
      entries.push_back(entry);
    }
  }
  f.close();
}

static void saveCoverCacheIndex(const std::vector<CoverCacheEntry>& entries) {
  if (!littleFsReady) return;
  File f = LittleFS.open(kCoverCacheIndex, "w");
  if (!f) return;
  for (const auto& entry : entries) {
    f.printf("%lu %lu %s\n", static_cast<unsigned long>(entry.ts), static_cast<unsigned long>(entry.size), entry.path.c_str());
  }
  f.close();
}

static void registerCoverCacheFile(const String& path) {
  if (!littleFsReady) return;
  File f = LittleFS.open(path, "r");
  if (!f) return;
  size_t size = f.size();
  f.close();

  std::vector<CoverCacheEntry> entries;
  loadCoverCacheIndex(entries);

  CoverCacheEntry entry;
  entry.ts = coverTimestamp();
  entry.size = size;
  entry.path = path;
  entries.push_back(entry);

  size_t total = 0;
  std::vector<CoverCacheEntry> filtered;
  filtered.reserve(entries.size());
  for (const auto& e : entries) {
    if (!LittleFS.exists(e.path)) continue;
    total += e.size;
    filtered.push_back(e);
  }

  auto lowFreeSpace = [&]() -> bool {
    size_t totalBytes = LittleFS.totalBytes();
    if (totalBytes == 0) return false;
    size_t usedBytes = LittleFS.usedBytes();
    if (usedBytes >= totalBytes) return true;
    return (totalBytes - usedBytes) < LITTLEFS_MIN_FREE_BYTES;
  };

  while ((total > kCoverCacheBytes || lowFreeSpace()) && !filtered.empty()) {
    CoverCacheEntry old = filtered.front();
    filtered.erase(filtered.begin());
    if (LittleFS.exists(old.path)) {
      LittleFS.remove(old.path);
    }
    if (total >= old.size) total -= old.size;
    else total = 0;
  }

  saveCoverCacheIndex(filtered);
}

static bool downloadCoverToFile(const String& url, const char* outPath) {
  if (!isHttpUrl(url)) return false;
  if (!isLikelyDirectCoverImageUrl(url)) {
    Serial.printf("[COVER-DL] Skip URL (not a direct cover image): %s\n", url.c_str());
    return false;
  }
  if (!littleFsReady) return false;

  Serial.printf("[COVER-DL] START url=%s -> %s\n", url.c_str(), outPath);

  ensureCoverDir();

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  bool useTls = url.startsWith("https://");

  HTTPClient http;
  http.setTimeout(8000);   // 8 секунд: было 15с — при медленном потоке держало сокет слишком долго
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const char* headers[] = {"Location", "Content-Type"};
  http.collectHeaders(headers, 2);

  bool httpReady = false;
  if (useTls) {
    secureClient.setInsecure();
    secureClient.setTimeout(5000);   // 5 секунд: было 10с
    secureClient.setHandshakeTimeout(5);  // 5 секунд лимит на SSL handshake
    httpReady = http.begin(secureClient, url);
  } else {
    plainClient.setTimeout(10000);   // 10 секунд для HTTP
    httpReady = http.begin(plainClient, url);
  }

  if (!httpReady) {
    Serial.println("[COVER-DL] HTTP begin failed");
    return false;
  }
  
  int code = http.GET();
  /*
   * GET/handshake может кратковременно блокироваться. Сразу после
   * возврата: если резерв опасно просел — не читаем JPEG (приоритет звуку).
   * Cover-SS: GET уже прошёл — дочитываем тело (как стабильные станции на 2–6%
   * буфера в старых прошивках); иначе вечный цикл start→abort на emergency 25%.
   * На PLAYER / вне SS abort остаётся.
   */
  if (shouldAbortBackgroundNetForAudio() && !coverTftScreensaverGateOpen()) {
    coverDeferScheduleWithToast();
    http.end();
    Serial.println("[COVER-DL] Deferred after GET — audio needs network priority");
    return false;
  }
  String contentType = http.header("Content-Type");
  String location = http.header("Location");
  Serial.printf("[COVER-DL] HTTP code=%d, content-type=%s, location=%s\n",
                code,
                contentType.length() ? contentType.c_str() : "<none>",
                location.length() ? location.c_str() : "<none>");

  if (code != HTTP_CODE_OK) {
    http.end();
    Serial.println("[COVER-DL] Non-200 response, abort");
    return false;
  }

  int total = http.getSize();
  if (total > 0 && static_cast<size_t>(total) > kMaxCoverBytes) {
    Serial.printf("[COVER-DL] File too large: %d bytes\n", total);
    http.end();
    return false;
  }

  // PSRAM staging: скачиваем ВСЁ изображение в PSRAM-буфер,
  // затем атомарно записываем в LittleFS под мьютексом.
  // Это решает проблему «битых» обложек: раньше display-задача могла прочитать
  // файл в середине записи (мьютекс снимался на каждый чанк).
  // PSRAM staging buffer: максимум kMaxCoverBytes (~600KB), обычно картинки 10-30KB.
  size_t allocSize = (total > 0) ? (size_t)total : 65536;  // если размер неизвестен, аллоцируем 64KB
  if (allocSize > kMaxCoverBytes) allocSize = kMaxCoverBytes;
  uint8_t* stagingBuf = (uint8_t*)ps_malloc(allocSize);
  if (!stagingBuf) {
    // Фоллбэк: если PSRAM не хватило, читаем без staging (как раньше)
    Serial.printf("[COVER-DL] PSRAM alloc failed (%u), fallback to direct write\n", (unsigned)allocSize);
    stagingBuf = nullptr;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  unsigned long startTime = millis();
  const unsigned long maxDownloadTime = 8000;
  
  bool downloadSuccess = false;
  
  if (stagingBuf) {
    // === Путь 1: PSRAM staging (рекомендуемый) ===
    while (http.connected() && (millis() - startTime) < maxDownloadTime) {
      // changeMode попросил отменить загрузку (переключение режима) —
      // прерываем сеть немедленно. downloadSuccess остаётся false → ниже штатно удалим частичный файл.
      if (g_coverDlCancelRequested) {
        Serial.println("[COVER-DL] Cancelled during PSRAM-staged download (mode switch)");
        break;
      }
      /*
       * Не рвём JPEG посередине по shouldAbortBackgroundNet:
       * обрыв на 20–50 KB отравляет lwIP → errno 113 → HTTP :80 и WS не отвечают (Firefox timeout).
       * Старт Cover уже отфильтрован gate isWebStreamReadyForCover(); докачиваем или выходим только по cancel.
       */
      size_t available = stream->available();
      if (!available) {
        if (total > 0 && written >= (size_t)total) { downloadSuccess = true; break; }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      size_t toRead = (available < 4096) ? available : 4096;
      if (written + toRead > allocSize) toRead = allocSize - written;
      if (toRead == 0) break;  // буфер полон
      int readCount = stream->readBytes(stagingBuf + written, toRead);
      if (readCount > 0) written += (size_t)readCount;
      if (total > 0 && written >= (size_t)total) { downloadSuccess = true; break; }
      if (written >= kMaxCoverBytes) break;
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!downloadSuccess && written > 0 && !http.connected() && (total <= 0 || written >= (size_t)total))
      downloadSuccess = true;

    http.end();  // Закрываем сокет ДО записи в LittleFS
    // Убираем File out — он больше не нужен (мы не открывали файл до staging)
    // ... ниже пишем из буфера

    if (downloadSuccess && written > 0 && written <= kMaxCoverBytes) {
      if (lockLittleFS(2000)) {
        File out = LittleFS.open(outPath, "w");
        if (out) {
          out.write(stagingBuf, written);
          out.close();
        }
        unlockLittleFS();
      }
    }
    free(stagingBuf);
  } else {
    // === Путь 2: Фоллбэк без PSRAM — прямая запись чанками (старое поведение) ===
    if (!lockLittleFS(2000)) {
      Serial.println("[COVER-DL] LittleFS lock failed (fallback)");
      http.end();
      return false;
    }
    File out = LittleFS.open(outPath, "w");
    unlockLittleFS();
    if (!out) {
      Serial.println("[COVER-DL] Failed to open output file (fallback)");
      http.end();
      return false;
    }
    uint8_t buffer[1024];
    while (http.connected() && (millis() - startTime) < maxDownloadTime) {
      // Тот же кооперативный выход, что и в PSRAM-пути выше.
      if (g_coverDlCancelRequested) {
        Serial.println("[COVER-DL] Cancelled during fallback download (mode switch)");
        break;
      }
      /* См. комментарий в PSRAM-пути — не abort mid-download (WebUI timeout). */
      size_t available = stream->available();
      if (!available) {
        if (total > 0 && written >= (size_t)total) { downloadSuccess = true; break; }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      size_t toRead = (available < sizeof(buffer)) ? available : sizeof(buffer);
      int readCount = stream->readBytes(buffer, toRead);
      if (readCount > 0) {
        if (lockLittleFS(100)) {
          out.write(buffer, readCount);
          unlockLittleFS();
          written += (size_t)readCount;
        } else break;
      }
      if (total > 0 && written >= (size_t)total) { downloadSuccess = true; break; }
      if (written >= kMaxCoverBytes) break;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!downloadSuccess && written > 0 && !http.connected() && (total <= 0 || written >= (size_t)total))
      downloadSuccess = true;
    if (lockLittleFS(1000)) { out.close(); unlockLittleFS(); } else { out.close(); }
    http.end();
  }
  
  if (!downloadSuccess || written == 0 || written > kMaxCoverBytes) {
    Serial.printf("[COVER-DL] FAIL success=%d written=%u\n", downloadSuccess ? 1 : 0, (unsigned)written);
    if (lockLittleFS(1000)) { LittleFS.remove(outPath); unlockLittleFS(); }
    return false;
  }
  
  registerCoverCacheFile(outPath);
  Serial.printf("[COVER-DL] OK bytes=%u\n", (unsigned)written);
  return true;
}

// Подстрока "timeout" в любом регистре — типичный текст сетевой/аудио ошибки, не название трека.
static bool titleContainsTimeout(const char* s) {
  if (!s) return false;
  static const char kWord[] = "timeout";
  for (const char* p = s; *p; p++) {
    size_t i = 0;
    for (; kWord[i] && p[i]; i++) {
      if (std::tolower((unsigned char)p[i]) != (unsigned char)kWord[i]) break;
    }
    if (kWord[i] == '\0') return true;
  }
  return false;
}

/* Apple CDN: .../NNNxNNNbb.jpg — размер под TFT-слот (TJpgDec без upscale).
 * ILI9488 cover-SS ~304px → 300×300 (не трогаем здесь).
 * DSP_ST7789 (JC2432 слот 200) → 200×200 как в v0.8.261 — иначе 600 поверх HTTPS-FLAC.
 * Прочие профили → 600×600 (их размер; править только когда гоняем тот профиль). */
static void upscaleItunesArtworkUrl(String &artUrl) {
  const int bbPos = artUrl.lastIndexOf("bb.jpg");
  if (bbPos < 0) return;
  const int slash = artUrl.lastIndexOf('/', bbPos);
  if (slash < 0) return;
  const String seg = artUrl.substring(slash + 1);
  if (seg.indexOf('x') < 0) return;
#if (DSP_MODEL == DSP_ILI9488 || DSP_MODEL == DSP_ILI9486) \
    && !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT)
  if (seg.endsWith("300x300bb.jpg")) return;
  artUrl = artUrl.substring(0, slash + 1) + "300x300bb.jpg";
#elif DSP_MODEL == DSP_ST7789
  /* Только JC2432 / ST7789 320×240 — текущий полевой профиль. */
  if (seg.endsWith("200x200bb.jpg")) return;
  artUrl = artUrl.substring(0, slash + 1) + "200x200bb.jpg";
#else
  if (seg.endsWith("600x600bb.jpg")) return;
  artUrl = artUrl.substring(0, slash + 1) + "600x600bb.jpg";
#endif
}

static String fetchiTunesCoverUrl(const String& title) {
  // Проверяем безопасность SSL (разрешено в SD-режиме с пониженным порогом)
  if (!isSafeForSSLCovers()) {
    Serial.println("[COVER] iTunes search skipped - SSL not safe");
    return "";
  }
  if (titleContainsTimeout(title.c_str())) return "";
  
  String query = title;
  query.replace("[", "");
  query.replace("]", "");
  query.replace("(", "");
  query.replace(")", "");
  query.trim();
  if (query.length() < 3) return "";
  query.replace(" ", "+");
  query.replace("&", "%26");
  query.replace("#", "%23");

  String url = String("https://itunes.apple.com/search?term=") + query + "&entity=musicTrack&limit=1";

  #if CORE_DEBUG_LEVEL > 0
  Serial.println("[COVER] iTunes search: " + query);
  #endif

  // Ещё раз проверяем перед SSL подключением
  if (!isSafeForSSLCovers()) {
    Serial.println("[COVER] iTunes search aborted - SSL not safe");
    return "";
  }

  // Гасим фоновый SNTP: иначе sntp_dns_found гоняется с http.GET/hostByName (assert udp_new_ip_type).
  pauseLwIpBackgroundSntpForUserNet();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);  // 5 секунд таймаут (client expects seconds, not ms!)
  client.setHandshakeTimeout(5);  // 5 секунд на SSL handshake

  HTTPClient http;
  http.setTimeout(5000);   // 5 секунд HTTP таймаут
  http.setConnectTimeout(3000);  // 3 секунды на подключение
  
  if (!http.begin(client, url)) {
    #if CORE_DEBUG_LEVEL > 0
    Serial.println("[COVER] iTunes HTTP begin failed");
    #endif
    return "";
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    #if CORE_DEBUG_LEVEL > 0
    Serial.printf("[COVER] iTunes HTTP error: %d\n", code);
    #endif
    // Явное закрытие SSL-сессии при ошибке.
    // HTTPClient::~dtor тоже вызовет end(), но явный вызов гарантирует
    // немедленное освобождение pbufs/сокетов — критично при низком heap.
    http.end();
    return "";
  }

  // Проверяем размер ответа перед разбором JSON
  String payload = http.getString();
  http.end();

  if (payload.length() == 0) {
    #if CORE_DEBUG_LEVEL > 0
    Serial.println("[COVER] iTunes response empty");
    #endif
    return "";
  }

  if (payload.length() > 16384) {  // Более 16KB JSON слишком большой
    #if CORE_DEBUG_LEVEL > 0
    Serial.printf("[COVER] iTunes response too large: %u bytes\n", (unsigned int)payload.length());
    #endif
    return "";
  }

  JsonDocument doc;  // ArduinoJson v7: используем JsonDocument вместо StaticJsonDocument
  DeserializationError err = deserializeJson(doc, payload);
  
  if (err) {
    #if CORE_DEBUG_LEVEL > 0
    Serial.printf("[COVER] JSON parse error: %s\n", err.c_str());
    #endif
    return "";
  }

  const char* art = doc["results"][0]["artworkUrl100"];
  if (!art) {
    #if CORE_DEBUG_LEVEL > 0
    Serial.println("[COVER] No artwork found in iTunes response");
    #endif
    return "";
  }
  
  String artUrl = String(art);
  upscaleItunesArtworkUrl(artUrl);

  /* Apple CDN mzstatic отдаёт тот же JPEG напрямую по HTTP:80 без редиректа
   * (проверено ответом 200 и Content-Type image/jpeg). После защищённого поиска
   * iTunes второй TLS для самой публичной картинки не нужен. Это сохраняет
   * внутреннюю RAM на HTTPS-FLAC станции Roxy и исключает второй handshake. */
  if (artUrl.startsWith("https://") && artUrl.indexOf(".mzstatic.com/") > 0) {
    artUrl = "http://" + artUrl.substring(8);
  }

  Serial.println("[COVER] iTunes artwork TFT: " + artUrl);
  
  return artUrl;
}

extern TrackFacts trackFactsPlugin;
// Определение ниже (после cleanTrackPrefix); нужно для coverDownloadTask.
static String cleanTrackPrefix(const String& title);
static String normalizeTitleForCoverSearch(const String& title);
static void scheduleCoverDownload(const String& url, const String& key);
static void handleCoverArt(const char* title);

// Диагностика: почему isSafeForSSLCovers() вернул false (лог только при defer/skip CoverDL).
static const char* coverSslBlockReason() {
  if (g_modeSwitching) return "mode_switching";
  if (isModeSwitchCooldown()) return "mode_cooldown";
  if (isStationSwitchSslBlock()) return "station_ssl_window";
  if (isPlaylistBusy()) return "playlist_busy";
  if (timekeeper.busy) return "sntp_busy";
  if (trackFactsPlugin.isRequestActive()) return "facts_ssl_active";
  if (!isWifiLinkStableForHeavyNet()) return "wifi_unstable";
  if (!isWebStreamReadyForCover()) return "audio_reserve";
#if BLOCK_COVER_TLS_ON_HTTPS_AUDIO
  if (isWebAudioStreamHttps() && !coverTftScreensaverGateOpen()) return "https_audio";
#endif
  if (ESP.getFreeHeap() < MIN_HEAP_FOR_SSL_COVERS) return "low_heap";
  if (cachedMaxAllocHeap() < MIN_MAX_ALLOC_HEAP_FOR_SSL_COVERS) return "low_max_alloc";
  return "unknown";
}

// Не параллелить CoverDL с mbedTLS в FactsTask: иначе (-32512) SSL OOM при FLAC+DeepSeek+icy-logo.
static void waitUntilTrackFactsSslIdle(uint32_t maxWaitMs) {
  const uint32_t t0 = millis();
  while (trackFactsPlugin.isRequestPending()) {
    // Не ждём фактов, если changeMode уже запросил отмену —
    // выходим из ожидания немедленно, а сам факт отмены проверит и обработает вызывающий код.
    if (g_coverDlCancelRequested) break;
    if ((millis() - t0) > maxWaitMs) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Единая точка выхода задачи CoverDL по кооперативной отмене.
// Вызывается ТОЛЬКО из самой coverDownloadTask (никогда снаружи) — задача сама решает,
// что безопасно прерваться прямо сейчас (нет незакрытых файлов/сокетов на этом шаге).
// Обязательно сбрасывает оба флага: g_coverTaskRunning (задача реально закончилась) и
// g_coverDlCancelRequested (чтобы следующий запуск CoverDL не отменился сразу же).
static void coverDlHandleCancelAndExit(const char* stage) {
  Serial.printf("[COVER] Cancelled by changeMode at stage=%s\n", stage);
  g_coverTaskRunning = false;
  g_coverDlCancelRequested = false;
  vTaskDelete(NULL);
}

// Объявление до coverDownloadTask; тело — рядом с coverTftScreensaverGateOpen().
static bool coverWebFlacBlocksDeviceDownload();

static void coverDownloadTask(void* param) {
  // Последний рубеж: задачу могли создать до смены format is flac.
  if (coverWebFlacBlocksDeviceDownload()) {
    Serial.println("[COVER] Skip CoverDL on WEB+FLAC (WebUI covers OK, lwIP for audio/:80)");
    g_coverTaskRunning = false;
    g_coverScheduleDeferred = false;
    vTaskDelete(NULL);
    return;
  }
  // Если в момент старта уже активен/ожидается SSL фактов, кратко ждём.
  // Это закрывает окно гонки между scheduleCoverDownload() и реальным стартом задачи.
  while (trackFactsPlugin.isRequestPending()) {
    // На этом шаге ничего ещё не скачано и не записано —
    // можно выйти сразу, без создания HTTPS-соединений/файлов.
    if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("wait_facts_pending_start"); return; }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // Пока syncTask ждёт SNTP — не лезем в HTTPS (гонка DNS с pool.ntp.org).
  {
    const uint32_t syncWait0 = millis();
    while (timekeeper.busy && (millis() - syncWait0) < 120000) {
      if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("wait_sntp_start"); return; }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  // Даже после busy=false SNTP мог остаться активным после прошлого sync — гасим до iTunes HTTPS.
  // Не вызываем pauseLwIpBackgroundSntp() здесь: до первого NTP это мешало syncTask стартовать/дождаться SNTP.

  // Ещё одна точка перед реальной работой с сетью/FS.
  if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("before_safety_check"); return; }

  // Проверяем безопасность SSL (разрешено в SD-режиме)
  if (!isSafeForSSLCovers()) {
    // Любая временная блокировка SSL не должна терять очередь: если URL/key уже в pending —
    // откладываем и ждём resumeDeferredCoverDownload() (timekeeper 1 Гц).
    // Раньше defer был только для station/SNTP/Facts; mode_cooldown/wifi_grace давали
    // «Task skipped» без g_coverScheduleDeferred → CoverDL на TFT не повторялся.
    if (g_pendingCoverUrl.length() > 0 && g_pendingCoverKey.length() > 0) {
      const char* reason = coverSslBlockReason();
      if (strcmp(reason, "audio_reserve") == 0) {
        coverDeferScheduleWithToast();
      } else {
        g_coverScheduleDeferred = true;
      }
      // Не чаще раза в 30 с — иначе монитор забивается при «залипшем» low_max_alloc.
      static uint32_t lastCoverDeferLogMs = 0;
      const uint32_t nowMs = millis();
      if (nowMs - lastCoverDeferLogMs >= 30000u) {
        if (strcmp(reason, "low_max_alloc") == 0 || strcmp(reason, "low_heap") == 0) {
          Serial.printf("[COVER] Deferred — SSL not safe (%s, free=%u est=%u); retry later\n",
                        reason, static_cast<unsigned>(ESP.getFreeHeap()),
                        static_cast<unsigned>(cachedMaxAllocHeap()));
        } else {
          Serial.printf("[COVER] Deferred — SSL not safe (%s); retry when safe\n", reason);
        }
        lastCoverDeferLogMs = nowMs;
      }
      // Backoff выставит resumeDeferredCoverDownload при следующем poll.
      g_coverTaskRunning = false;
      vTaskDelete(NULL);
      return;
    }
    Serial.printf("[COVER] Task skipped - SSL not safe (%s), no pending URL\n", coverSslBlockReason());
    g_coverTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }
  
  // Защита от пустых или некорректных данных
  if (g_pendingCoverUrl.length() == 0 || g_pendingCoverKey.length() == 0) {
    g_coverTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }
  
  String url = g_pendingCoverUrl;
  String key = g_pendingCoverKey;
  g_coverTaskRunning = true;

  // Если не нужно ни на Web, ни на TFT — не кэшируем и не тратим сеть.
  if (!isCoversEnabledForWebOrDisplay()) {
    g_coverTaskRunning = false;
    g_pendingCoverUrl = "";
    g_pendingCoverKey = "";
    vTaskDelete(NULL);
    return;
  }

  if (!littleFsReady) {
    g_coverTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }

  // Проверяем ещё раз после паузы
  if (!isSafeForSSLCovers()) {
    if (strcmp(coverSslBlockReason(), "audio_reserve") == 0) {
      coverDeferScheduleWithToast();
    } else {
      g_coverScheduleDeferred = true;
    }
    Serial.printf("[COVER] Deferred after initial wait — SSL not safe (%s)\n", coverSslBlockReason());
    g_coverTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }

  // Увеличенная задержка для стабилизации аудио потока.
  // Даём время системе стабилизироваться после переключения режима/станции,
  // чтобы тяжёлый TLS-запрос за обложкой не мешал буферам аудио.
  // Время задержки настраивается через COVER_DOWNLOAD_DELAY_MS (options.h / myoptions.h),
  // по умолчанию 2000 мс. Рекомендуемый диапазон: 1000–5000 мс.
  // Если задача стартовала уже из playing-SCREENBLANK (гейт CoverDL), длинная пауза
  // не нужна: поток к этому моменту уже устоялся, ждали COVER_SS_START_DELAY_MS снаружи.
  // Раньше это была одна сплошная задержка vTaskDelay(COVER_DOWNLOAD_DELAY_MS) —
  // самое "тихое" и при этом самое ДОЛГОЕ окно задачи, где отмена не проверялась вообще.
  // Разбиваем её на короткие шаги по 50мс и проверяем флаг отмены — это и есть основной путь,
  // по которому changeMode получает быстрый (десятки мс) отклик на cancel, а не ждёт до 2 секунд.
  {
    const bool alreadyInCoverSs = config.isScreensaver && display.mode() == SCREENBLANK && player.isRunning();
    const uint32_t delayMs = alreadyInCoverSs ? 0u : (uint32_t)COVER_DOWNLOAD_DELAY_MS;
    uint32_t waitedMs = 0;
    const uint32_t stepMs = 50;
    while (waitedMs < delayMs) {
      if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("initial_delay"); return; }
      vTaskDelay(pdMS_TO_TICKS(stepMs));
      waitedMs += stepMs;
    }
  }
  // За COVER_DOWNLOAD_DELAY FactsTask мог стартовать DeepSeek — снова ждём конца SSL фактов.
  waitUntilTrackFactsSslIdle(120000);
  if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("after_wait_facts_1"); return; }

  // Перед HTTPS — SNTP/syncTask мог стартовать во время COVER_DOWNLOAD_DELAY.
  {
    const uint32_t syncWait1 = millis();
    while (timekeeper.busy && (millis() - syncWait1) < 120000) {
      if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("wait_sntp_after_delay"); return; }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  
  // Проверяем ещё раз после задержки
  if (!isSafeForSSLCovers()) {
    g_coverScheduleDeferred = true;
    Serial.printf("[COVER] Deferred after delay — SSL not safe (%s)\n", coverSslBlockReason());
    g_coverTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }
  // Последняя проверка перед тем, как задача пойдёт резолвить/скачивать URL.
  if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("before_url_resolve"); return; }

  // WEB: фоновые обложки используют общий строгий audio-priority gate.
  // SD не трогаем: локальный файл не конкурирует с Wi‑Fi-потоком так же, как WEB.
  if (config.getMode() == PM_WEB) {
    const uint32_t gateT0 = millis();
    const uint32_t gateCapMs = 45000u; // максимум ожидания улёжки звука, дальше отложим CoverDL
    bool audioReady = false;
    while ((millis() - gateT0) < gateCapMs) {
      if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("wait_audio_stable"); return; }
      /*
       * Один и тот же gate теперь защищает Cover/Facts/Weather:
       * высокий резерв, свежие реальные аудиоданные, отсутствие любого recovery
       * и непрерывное стабильное окно. Так подсистемы не расходятся в трактовке
       * «поток уже достаточно здоров для фонового HTTP».
       */
      if (isWebStreamReadyForCover()) {
        audioReady = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!audioReady) {
      // Не стартуем TLS в развал потока; resumeDeferredCoverDownload() подхватит позже.
      coverDeferScheduleWithToast();
      Serial.println("[COVER] Deferred — audio buffer/stream not stable yet");
      g_coverTaskRunning = false;
      vTaskDelete(NULL);
      return;
    }
    // После ожидания звука условия SSL могли измениться — перепроверяем перед HTTPS.
    if (!isSafeForSSLCovers()) {
      if (strcmp(coverSslBlockReason(), "audio_reserve") == 0) {
        coverDeferScheduleWithToast();
      } else {
        g_coverScheduleDeferred = true;
      }
      Serial.println("[COVER] Deferred after audio gate — SSL not safe");
      g_coverTaskRunning = false;
      vTaskDelete(NULL);
      return;
    }
    if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("after_audio_gate"); return; }
    /*
     * Устойчивое окно достигнуто: закрываем defer-состояние,
     * чтобы новая отдельная просадка могла быть объяснена пользователю один раз.
     */
    transientNotifier.clear(TN_COVER_DEFERRED);
  }

  // Не тратим SSL на t.me / youtube и т.п. — сразу уходим в iTunes по текущему тегу.
  if (url != "SEARCH_ITUNES" && url.length() && !isLikelyDirectCoverImageUrl(url)) {
    Serial.println("[COVER] Stream cover URL is not a direct image — fallback to iTunes");
    if (streamCoverUrl == url) streamCoverUrl = "";
    String song = cleanTrackPrefix(metaTitle);
    String coverSearchTitle = normalizeTitleForCoverSearch(song);
    if (coverSearchTitle.length() < 3) coverSearchTitle = song;
    if (coverSearchTitle.length() > 5 && !coverSearchTitle.startsWith("[")) {
      String itunesKey = String("ITUNES:") + coverSearchTitle;
      g_coverTaskRunning = false;
      scheduleCoverDownload("SEARCH_ITUNES", itunesKey);
      vTaskDelete(NULL);
      return;
    }
    g_coverTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }

  if (url == "SEARCH_ITUNES") {
    // Почему здесь НЕ используем только metaTitle:
    // 1) coverDownloadTask запускается асинхронно (с задержкой/в отдельной задаче),
    // а metaTitle за это время может уже поменяться на следующий трек.
    // 2) В результате возникал рассинхрон: задача была создана для трека A,
    // но фактический запрос в iTunes уходил для трека B.
    // 3) Это приводило к "не тем" обложкам и ощущению, что дисплей/Web "застрял".
    // Что изменено:
    // - было:  fetchiTunesCoverUrl(metaTitle)
    // - стало: fetchiTunesCoverUrl(searchTitle), где searchTitle берется из key
    // формата "ITUNES:<название>", если такой ключ передан планировщиком.
    // Почему это безопасно для WebUI:
    // - меняется только строка поискового запроса к iTunes в фоновой задаче;
    // - путь к кэшу и currentCoverUrl по-прежнему строятся из того же key;
    // - Web получает URL из currentCoverUrl/кэша и не зависит от того,
    // из какой переменной был собран поисковый текст.
    String searchTitle = metaTitle;
    if (key.startsWith("ITUNES:")) {
      searchTitle = key.substring(7);
    }
    searchTitle = normalizeTitleForCoverSearch(searchTitle);

    Serial.println("[COVER] Searching iTunes for: " + searchTitle);

    // Валидация защищает от мусорных/системных строк и слишком длинных значений,
    // которые только нагружают сеть и не дают валидный результат поиска.
    if (searchTitle.length() < 3 || searchTitle.length() > 200) {
      Serial.println("[COVER] Invalid track title for iTunes search");
      g_coverTaskRunning = false;
      vTaskDelete(NULL);
      return;
    }

    // Мусор из ошибки потока (HTTP 404 и т.д.) в meta — не слать в iTunes.
    if (searchTitle.indexOf("HTTP/") >= 0) {
      Serial.println("[COVER] iTunes search skipped — not a track title");
      g_coverTaskRunning = false;
      vTaskDelete(NULL);
      return;
    }

    waitUntilTrackFactsSslIdle(120000);
    // Перед самим HTTPS-запросом к iTunes — последний шанс выйти дёшево,
    // ничего в сети/FS ещё не начато.
    if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("before_itunes_search"); return; }

    // FLAC-поток: iTunes-поиск только при устойчивом буфере (как Facts gate).
    if (!isWebStreamReadyForCover()) {
      coverDeferScheduleWithToast();
      Serial.println("[COVER] Deferred before iTunes search — audio reserve/recovery");
      g_coverTaskRunning = false;
      g_coverScheduleDeferred = true;
      vTaskDelete(NULL);
      return;
    }

    // Выполняем поиск строго по зафиксированному searchTitle.
    // Это гарантирует, что результат соответствует именно той задаче,
    // для которой был создан key.
    url = fetchiTunesCoverUrl(searchTitle);

    // Короткая пауза после iTunes-запроса, чтобы не создавать плотную серию
    // сетевых операций при частой смене треков.
    vTaskDelay(pdMS_TO_TICKS(500));
    waitUntilTrackFactsSslIdle(120000);
  }

  // Перед скачиванием самого изображения — ещё одна проверка.
  // Дальше внутри downloadCoverToFile() отмена уже проверяется прямо в цикле чтения потока.
  if (g_coverDlCancelRequested) { coverDlHandleCancelAndExit("before_download"); return; }
  if (!isWebStreamReadyForCover()) {
    /*
     * iTunes-поиск уже мог съесть запас FLAC-буфера.
     * Не начинаем второй сетевой этап (JPEG) без порога Cover (~35%).
     */
    coverDeferScheduleWithToast();
    Serial.println("[COVER] Deferred before image download — audio reserve/recovery");
    g_coverTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }

  if (url.length()) {
    waitUntilTrackFactsSslIdle(120000);
    #if CORE_DEBUG_LEVEL > 0
    Serial.println("[COVER] Downloading: " + url);
    #endif
    String path = coverCachePathForKey(key);
    if (downloadCoverToFile(url, path.c_str())) {
      // После cancel/смены режима файл мог дописаться, но на TFT/URL его не навешиваем.
      if (g_coverDlCancelRequested || g_modeSwitching
#if USE_BT_I2S
          || config.getMode() == PM_BT
#endif
         ) {
        Serial.println("[COVER] Downloaded OK, skip UI apply (mode switch/BT/cancel)");
      } else {
        currentCoverUrl = coverCacheUrlForKey(key);
        updateDisplayCover();
        #if CORE_DEBUG_LEVEL > 0
        Serial.println("[COVER] Downloaded successfully, cache invalidated");
        #endif
      }
      /* Анти-дубль: этот key уже в кэше — не оставляем pending/defer на второй HTTPS
       * (иначе resumeDeferred сразу жжёт тот же JPEG поверх WEB+FLAC → Stream lost). */
      if (g_pendingCoverKey == key) {
        g_pendingCoverUrl = "";
        g_pendingCoverKey = "";
        g_coverScheduleDeferred = false;
      }
    } else {
      #if CORE_DEBUG_LEVEL > 0
      Serial.println("[COVER] Download failed");
      #endif
    }
  }

  // Если пока работала эта задача, пришла новая обложка с другим key — не теряем её.
  const bool hasDeferredCover =
    (g_pendingCoverUrl.length() > 0 && g_pendingCoverKey.length() > 0 && g_pendingCoverKey != key);

  g_coverTaskRunning = false;

  // Если во время скачивания (уже после последней проверки выше, например
  // прямо в downloadCoverToFile) подъехала отмена от changeMode — НЕ запускаем следующую (deferred)
  // обложку сразу же. changeMode сейчас переключает режим; новая CoverDL-задача воссоздала бы
  // ту же гонку за LittleFS/heap, которую мы только что оборвали. Сбрасываем флаг — это нормальное,
  // "штатное" завершение отмены, а не сбой.
  if (g_coverDlCancelRequested) {
    g_coverDlCancelRequested = false;
    Serial.println("[COVER] Cancelled after download attempt — skip deferred re-run for now");
    vTaskDelete(NULL);
    return;
  }

  if (hasDeferredCover) {
    // Новая задача ещё не создана — гарантированно нет активной CoverDL,
    // поэтому безопасно сбросить флаг отмены здесь (защита от редкого edge-case гонки).
    g_coverDlCancelRequested = false;
    g_coverTaskRunning = true;
    /* 8 КБ достаточно для локальных переменных CoverDL: JSON и JPEG хранятся
     * в heap/PSRAM. Стек обязан быть во внутренней RAM, иначе LittleFS при
     * отключённом flash-cache вызывает esp_task_stack_is_sane_cache_disabled(). */
    BaseType_t nextRes = xTaskCreatePinnedToCore(
        coverDownloadTask, "CoverDL", 8192, NULL, 1, NULL, 1);
    if (nextRes != pdPASS) {
      g_coverTaskRunning = false;
      Serial.println("[COVER] Failed to create deferred CoverDL task");
    }
  }

  vTaskDelete(NULL);
}

extern bool g_coverTaskRunning;
extern TrackFacts trackFactsPlugin;

// CoverDL качает JPEG только для TFT (веб берёт обложку сам). На экране она
// нужна лишь в playing-SCREENBLANK — до этого HTTPS не стартуем.
// WEB+FLAC: на PLAYER CoverDL не стартуем (lwIP/веб).
// Исключение: playing+SCREENBLANK — один JPEG для TFT, когда пользователь смотрит cover-SS.
static bool coverTftScreensaverGateOpen() {
  if (!isDisplayCoversEnabled()) return false;
  /* blank ON = гашение, не Cover-SS — CoverDL не стартуем. */
  if (config.store.screensaverPlayingBlank) return false;
  if (!player.isRunning()) return false;
  if (!config.isScreensaver) return false;
  if (display.mode() != SCREENBLANK) return false;
  if (g_coverScreensaverEnteredMs == 0) return false;
  return (millis() - g_coverScreensaverEnteredMs) >= (uint32_t)COVER_SS_START_DELAY_MS;
}

static bool coverWebFlacBlocksDeviceDownload() {
  if (config.getMode() != PM_WEB) return false;
  if (!g_audioFormatFlacActive) return false;
  if (coverTftScreensaverGateOpen()) return false;
  return true;
}

// Общий старт CoverDL из pending URL/key. false — задачу не создали (caller решает defer).
static bool tryStartCoverDownloadTask(const char* logTag) {
  if (g_coverTaskRunning) return false;
  if (g_pendingCoverUrl.length() == 0 || g_pendingCoverKey.length() == 0) return false;
  if (!littleFsReady || network.status != CONNECTED || isPlaylistBusy()) return false;
  if (trackFactsPlugin.isRequestPending()) return false;
  if (coverWebFlacBlocksDeviceDownload()) return false;
  g_coverDlCancelRequested = false;
  g_coverTaskRunning = true;
  BaseType_t res = xTaskCreatePinnedToCore(
      coverDownloadTask, "CoverDL", 8192, NULL, 1, NULL, 1);
  if (res != pdPASS) {
    g_coverTaskRunning = false;
    Serial.printf("[COVER] %s: failed to create CoverDL task\n", logTag);
    return false;
  }
  /* Старт = уже не «отложено»: иначе после OK resumeDeferred поднимает тот же URL снова. */
  g_coverScheduleDeferred = false;
  Serial.printf("[COVER] %s: CoverDL started (screensaver gate)\n", logTag);
  return true;
}

static void scheduleCoverDownload(const String& url, const String& key) {
  if (!isCoversEnabledForWebOrDisplay()) return;
  if (!littleFsReady) return;
  if (network.status != CONNECTED) return;
  if (isPlaylistBusy()) return; // Во время индексации/сохранения избранного не ставим в очередь загрузку обложек
  /* WEB+FLAC: HTTPS на PLAYER не стартуем (lwIP), но URL/key в очередь — CoverDL в cover-SS. */
  if (coverWebFlacBlocksDeviceDownload()) {
    if (url.length() == 0 || key.length() == 0) return;
    if (key.length() && key == g_lastCoverKey && coverCacheExists(key)) return;
    g_lastCoverKey = key;
    g_lastCoverScheduleMs = millis();
    g_pendingCoverUrl = url;
    g_pendingCoverKey = key;
    g_coverWaitForScreensaver = true;
    Serial.printf("[COVER] WEB+FLAC: queued until SCREENBLANK+%ums\n", (unsigned)COVER_SS_START_DELAY_MS);
    return;
  }
  // WEB+FLAC: не планируем CoverDL пока буфер потока под угрозой — иначе iTunes/JPEG → Stream lost.
  if (config.getMode() == PM_WEB && shouldAbortBackgroundNetForAudio()) {
    if (url.length() && key.length()) {
      g_pendingCoverUrl = url;
      g_pendingCoverKey = key;
      g_coverScheduleDeferred = true;
    }
    return;
  }
  if (key.length() && key == g_lastCoverKey) {
    // Для одного и того же трека допускаем редкий повтор, если кэша всё ещё нет.
    // Это помогает после временных сетевых сбоев без спама HTTPS.
    if (coverCacheExists(key)) return;
    const uint32_t nowMs = millis();
    if (nowMs - g_lastCoverScheduleMs < 15000u) return;
  }
  
  // Дополнительная защита от некорректных данных
  if (url.length() == 0 || key.length() == 0) return;
  if (url.length() > 512 || key.length() > 128) return; // Разумные ограничения
  
  g_lastCoverKey = key;
  g_lastCoverScheduleMs = millis();
  g_pendingCoverUrl = url;
  g_pendingCoverKey = key;
  // TFT: пока не playing-SCREENBLANK + COVER_SS_START_DELAY_MS — только очередь, без HTTPS.
  if (!coverTftScreensaverGateOpen()) {
    g_coverWaitForScreensaver = true;
    Serial.printf("[COVER] queued until SCREENBLANK+%ums\n", (unsigned)COVER_SS_START_DELAY_MS);
    return;
  }
  g_coverWaitForScreensaver = false;
  // v0.8.139: не параллелить CoverDL с FactsTask (DeepSeek → iTunes и т.д.) — иначе (-32512) SSL OOM.
  if (trackFactsPlugin.isRequestPending()) {
    g_coverScheduleDeferred = true;
    Serial.println("[COVER] schedule deferred — TrackFacts SSL busy");
    return;
  }
  if (g_coverTaskRunning) return;
  if (!tryStartCoverDownloadTask("schedule")) {
    /* tryStart уже залогировал отказ создать задачу */
  }
}

// Пауза между попытками resume при нехватке кучи — не спамим CoverDL каждую секунду (FLAC+HTTPS).
static uint32_t g_coverResumeBackoffUntilMs = 0;

// Вызывается из TrackFacts::fetchTask после снятия isRequestInProgress — поднимает отложенный CoverDL.
void resumeDeferredCoverDownload(void) {
  if (!isCoversEnabledForWebOrDisplay()) {
    g_coverScheduleDeferred = false;
    g_coverWaitForScreensaver = false;
    return;
  }
  if (!g_coverScheduleDeferred) return;
  /* WEB+FLAC на PLAYER: ждём cover-SS, defer не сбрасываем. */
  if (coverWebFlacBlocksDeviceDownload()) return;
  // Пока плеер на PLAYER — CoverDL не поднимаем (экономия SSL для потока).
  if (!coverTftScreensaverGateOpen()) return;
  if (trackFactsPlugin.isRequestPending()) return;
  if (!isSafeForSSLCovers()) {
    // Ждём конца окна станции / heap (флаг defer не сбрасываем). При low_heap/low_max_alloc — backoff длиннее.
    const char* reason = coverSslBlockReason();
    if (strcmp(reason, "low_max_alloc") == 0 || strcmp(reason, "low_heap") == 0) {
      const uint32_t nowMs = millis();
      if (nowMs < g_coverResumeBackoffUntilMs) return;
      // Было 8 с — при «залипшем» est=free/2 спам каждую секунду. Держим 30 с.
      g_coverResumeBackoffUntilMs = nowMs + 30000u;
      static uint32_t lastCoverHeapLogMs = 0;
      if (nowMs - lastCoverHeapLogMs >= 30000u) {
        Serial.printf("[COVER] Отложено (мало непрерывной кучи: %s, free=%u est=%u). "
                      "Факты/обложка ждут освобождения памяти; повтор через ~30 с.\n",
                      reason, (unsigned)ESP.getFreeHeap(), (unsigned)cachedMaxAllocHeap());
        lastCoverHeapLogMs = nowMs;
      }
    }
    return;
  }
  g_coverResumeBackoffUntilMs = 0;
  if (g_pendingCoverUrl.length() == 0 || g_pendingCoverKey.length() == 0) {
    g_coverScheduleDeferred = false;
    return;
  }
  /* Анти-дубль: JPEG уже на FS — повторный iTunes/HTTPS не нужен. */
  if (coverCacheExists(g_pendingCoverKey)) {
    Serial.println("[COVER] resumeDeferred: кэш уже есть — skip повторного CoverDL");
    g_pendingCoverUrl = "";
    g_pendingCoverKey = "";
    g_coverScheduleDeferred = false;
    return;
  }
  if (g_coverTaskRunning) return;
  if (!littleFsReady) return;
  if (network.status != CONNECTED) return;
  if (isPlaylistBusy()) return;
  // WEB: не поднимаем CoverDL без общего устойчивого окна аудио — иначе задача
  // снова уйдёт в wait и будет крутиться вхолостую каждую секунду.
  if (config.getMode() == PM_WEB) {
    /* Повторный запуск использует тот же gate, что и первый. */
    if (!isWebStreamReadyForCover()) return;
    /* Буфер под угрозой / recovery — не жечь второй TLS (defer оставляем). */
    if (shouldAbortBackgroundNetForAudio()) return;
  }
  g_coverScheduleDeferred = false;
  // См. аналогичный комментарий в scheduleCoverDownload — активной
  // задачи здесь нет (проверено выше g_coverTaskRunning), безопасно сбросить флаг отмены.
  g_coverDlCancelRequested = false;
  g_coverTaskRunning = true;
  /* Повторный запуск использует тот же безопасный размер внутреннего стека. */
  BaseType_t res = xTaskCreatePinnedToCore(
      coverDownloadTask, "CoverDL", 8192, NULL, 1, NULL, 1);
  if (res != pdPASS) {
    Serial.println("[COVER] resumeDeferred: failed to create CoverDL task");
    g_coverTaskRunning = false;
    g_coverScheduleDeferred = true;
  } else {
    Serial.println("[COVER] deferred cover task started (SSL safe)");
  }
}

void notifyCoverScreensaverEntered(void) {
  // Запоминаем момент входа: CoverDL можно стартовать не раньше чем через COVER_SS_START_DELAY_MS.
  g_coverScreensaverEnteredMs = millis();
  /* WEB+FLAC: один раз при входе в SS — если очередь пуста, повторить постановку по meta. */
  if (g_pendingCoverUrl.length() == 0 && config.getMode() == PM_WEB && g_audioFormatFlacActive
      && isDisplayCoversEnabled() && metaTitle.length() > 5 && !metaTitle.startsWith("[")) {
    handleCoverArt(metaTitle.c_str());
  }
}

void notifyCoverScreensaverLeft(void) {
  g_coverScreensaverEnteredMs = 0;
  // Pending URL не сбрасываем — при следующем SCREENBLANK подхватит poll.
}

void pollCoverDownloadAfterScreensaver(void) {
  if (!g_coverWaitForScreensaver) return;
  if (!coverTftScreensaverGateOpen()) return;
  if (g_coverTaskRunning) return;

  if (g_pendingCoverUrl.length() == 0 || g_pendingCoverKey.length() == 0) {
    g_coverWaitForScreensaver = false;
    return;
  }

  /* Не долбим SSL каждый кадр DspTask — раз в ~2 с достаточно. */
  static uint32_t s_lastSsPollMs = 0;
  const uint32_t now = millis();
  if ((now - s_lastSsPollMs) < 2000u) return;

  if (trackFactsPlugin.isRequestPending() || !isSafeForSSLCovers()) {
    g_coverScheduleDeferred = true;
    /* Флаг g_coverWaitForScreensaver НЕ сбрасываем — иначе спам defer каждый кадр. */
    static uint32_t s_lastDeferLogMs = 0;
    if (now - s_lastDeferLogMs >= 15000u) {
      Serial.printf("[COVER] SS: CoverDL отложен (%s)\n", coverSslBlockReason());
      s_lastDeferLogMs = now;
    }
    s_lastSsPollMs = now;
    return;
  }

  s_lastSsPollMs = now;
  g_coverWaitForScreensaver = false;
  if (!tryStartCoverDownloadTask("screensaver")) {
    g_coverScheduleDeferred = true;
    g_coverWaitForScreensaver = true;
    s_lastSsPollMs = 0;
  }
}

// 2. Вспомогательная функция (обязательно ПЕРЕД handleCoverArt)
String urlEncode(String str) {
    String encodedString = "";
    for (int i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        if (isAlphaNumeric(c)) encodedString += c;
        else if (c == ' ') encodedString += '+';
        else encodedString += '_';
    }
    return encodedString;
}

/**
 * Очистка названия трека от служебных префиксов
 * Удаляет паттерны типа: "11 | ", "5. ", "123 - ", "7: " и т.д.
 * Примеры:
 * "11 | Mina - Garden Of Delight" -> "Mina - Garden Of Delight"
 * "5. Artist - Song" -> "Artist - Song"
 * "123 - Title" -> "Title"
 */
String cleanTrackPrefix(const String& title) {
    String cleaned = title;
    cleaned.trim();
    
    // Паттерн: цифры + разделитель (|, -, ., :, пробел) + пробелы
    int i = 0;
    
    // Пропускаем начальные цифры
    while (i < cleaned.length() && isdigit(cleaned[i])) {
        i++;
    }
    
    // Если нашли цифры в начале
    if (i > 0 && i < cleaned.length()) {
        // Пропускаем пробелы после цифр
        while (i < cleaned.length() && cleaned[i] == ' ') {
            i++;
        }
        
        // Проверяем разделители: | - . :
        if (i < cleaned.length() && (cleaned[i] == '|' || cleaned[i] == '-' || 
                                      cleaned[i] == '.' || cleaned[i] == ':')) {
            i++; // Пропускаем разделитель
            
            // Пропускаем пробелы после разделителя
            while (i < cleaned.length() && cleaned[i] == ' ') {
                i++;
            }
            
            // Возвращаем строку без префикса
            cleaned = cleaned.substring(i);
            cleaned.trim();
        }
    }
    
    return cleaned;
}

// Нормализация строки именно для поиска обложки в iTunes.
// Убирает тех. хвосты ретрансляторов/сборок, которые часто ломают матчинг.
static String normalizeTitleForCoverSearch(const String& title) {
    String normalized = cleanTrackPrefix(title);
    normalized.trim();

    // Сжимаем повторные пробелы/табы до одного пробела.
    String compact;
    compact.reserve(normalized.length());
    bool prevSpace = false;
    for (int i = 0; i < normalized.length(); i++) {
        char c = normalized.charAt(i);
        bool isSpace = (c == ' ' || c == '\t');
        if (isSpace) {
            if (!prevSpace) {
                compact += ' ';
                prevSpace = true;
            }
        } else {
            compact += c;
            prevSpace = false;
        }
    }
    compact.trim();
    normalized = compact;

    // Доп. фильтр для ретрансляторов вида "... *** www.ipmusic.ch".
    // Раньше это срабатывало только внутри последней пары "(...)".
    // Здесь режем хвост с доменом даже без скобок, чтобы iTunes не искал по мусору.
    String normalizedLower = normalized;
    normalizedLower.toLowerCase();
    if (normalizedLower.indexOf("ipmusic.ch") >= 0) {
      // Отрежем с начала "***" (если есть) или с ближайшего "www." до конца.
      int ipPos = normalizedLower.indexOf("ipmusic.ch");
      int starPos = normalizedLower.lastIndexOf("***", ipPos);
      int wwwPos = normalizedLower.lastIndexOf("www.", ipPos);
      int cutPos = (starPos >= 0) ? starPos : ((wwwPos >= 0) ? wwwPos : ipPos);
      normalized = normalized.substring(0, cutPos);
      normalized.trim();
    }

    // Срезаем только короткий "технический" хвост в последних скобках.
    // Например: "(cDjShaman.cut)", "(stream rip)".
    while (normalized.endsWith(")")) {
        int openPos = normalized.lastIndexOf('(');
        if (openPos < 0) break;

        String tail = normalized.substring(openPos);
        if (tail.length() > 48) break; // вероятнее музыкальный блок, оставляем

        String tailLower = tail;
        tailLower.toLowerCase();
        const bool looksTechnical =
            (tailLower.indexOf(".cut") >= 0) ||
            (tailLower.indexOf("cdj") >= 0) ||
            (tailLower.indexOf("djshaman") >= 0) ||
            (tailLower.indexOf("stream") >= 0) ||
            (tailLower.indexOf("record") >= 0) ||
            (tailLower.indexOf("rip") >= 0) ||
            (tailLower.indexOf("www.") >= 0) ||
            (tailLower.indexOf("*** www.ipmusic.ch") >= 0) ||
            (tailLower.indexOf("http") >= 0);
        if (!looksTechnical) break;

        normalized = normalized.substring(0, openPos);
        normalized.trim();
    }

    return normalized;
}

/**
 * Логика выбора обложки (Cover Art).
 * Вызывается каждый раз, когда меняется название станции или тег песни.
 * Устанавливает глобальную переменную currentCoverUrl.
 */
static void handleCoverArt(const char* title) {
  if (!littleFsReady) {
    currentCoverUrl = "/logo.svg";
    updateDisplayCover();
    return;
  }

#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
  /* Classic (Aiwushka/Wrover): findManualCover → exists/openNextFile из playerTask → lock_init abort.
   * Веб-обложки (cover.js) не зависят от этого скана. S3 не трогаем. */
  (void)title;
  currentCoverUrl = "/logo.svg";
  updateDisplayCover();
  return;
#endif

  if (config.getMode() == PM_SDCARD) {
    // В SD-режиме используем фактическое имя трека, игнорируя служебные заглушки вроде "[next track]".
    String sdTitle = title ? String(title) : String("");
    sdTitle.trim();
    if (sdTitle.length() < 3 || sdTitle.startsWith("[") || titleContainsTimeout(sdTitle.c_str())) {
      currentCoverUrl = "/logo.svg";
      updateDisplayCover();
      return;
    }

    // Если не нужно ни на Web, ни на TFT — без кэша/iTunes/ID3-записи на устройство.
    if (!isCoversEnabledForWebOrDisplay()) {
      currentCoverUrl = "/logo.svg";
      updateDisplayCover();
      return;
    }

    // ПРИОРИТЕТ 1: file-key — обложка из ID3-тега, сохранённая по имени файла.
    // audio_id3image() сохраняет обложку с ключом "sd:file:<имя_файла_без_расширения>",
    // потому что в момент вызова metaTitle ещё не установлен (debounce 500мс).
    // Если при смене трека файл уже был обработан — показываем сразу, без iTunes.
    if (g_sdFileKey.length() > 8 && coverCacheExists(g_sdFileKey)) {
      currentCoverUrl = coverCacheUrlForKey(g_sdFileKey);
      updateDisplayCover();
      Serial.printf("[SD Cover] Showing from file-key: %s\n", g_sdFileKey.c_str());
      return;
    }

    // ПРИОРИТЕТ 2: title-key — кэш по названию трека (из предыдущих запусков или iTunes).
    String sdKey = String("sd:") + sdTitle;
    if (coverCacheExists(sdKey)) {
      currentCoverUrl = coverCacheUrlForKey(sdKey);
      updateDisplayCover();
      return;
    }

    // ПРИОРИТЕТ 3: Нет кэша — показываем логотип, затем ищем через iTunes.
    currentCoverUrl = "/logo.svg";
    updateDisplayCover();
    String coverSearchTitle = normalizeTitleForCoverSearch(sdTitle);
    String sdTitleLower = sdTitle;
    sdTitleLower.toLowerCase();
    const bool origLooksIpmusic = sdTitleLower.indexOf("ipmusic.ch") >= 0;
    if (coverSearchTitle.length() < 3) {
      // Если после нормализации остался только ipmusic-хвост,
      // не возвращаем "как было", чтобы не слать мусор в iTunes.
      if (origLooksIpmusic) return;
      coverSearchTitle = sdTitle;
    }
    scheduleCoverDownload("SEARCH_ITUNES", String("ITUNES:") + coverSearchTitle);
    return;
  }
    // Храним состояние предыдущих данных, чтобы не обновлять картинку, если песня/станция не изменились
    static String lastStationName;
    static String lastSongTitle;

    // WEB: без активного потока не тянем обложки (и не копим TLS в очереди).
    if (player.status() != PLAYING || !player.isRunning()) {
      currentCoverUrl = "/logo.svg";
      updateDisplayCover();
      lastStationName = "";
      lastSongTitle = "";
      return;
    }

    // --- 1. ПРОВЕРКА СОСТОЯНИЯ ПЛЕЕРА ---
    // Если радио остановлено или только загружается (системные теги [готов], [остановлено]),
    // мы не ищем обложку, а сразу показываем стандартный логотип.
    bool isPlayingNow = true;
    if (!title || strlen(title) < 3 || strstr(title, "[остановлено]") || strstr(title, "[готов]") ||
        strstr(title, "[соедин") != nullptr || strstr(title, "[connect") != nullptr) {
        isPlayingNow = false;
    }

    // Дополнительная защита: если в title прилетела системная ошибка подключения
    // ("Error connecting to ..." или "Request http ... failed!"), НЕ пытаемся
    // искать по ней обложку в iTunes. Такие строки не являются названием трека
    // и только создают лишние запросы и ошибки DNS/TLS.
    if (title && (strstr(title, "Error connecting to ") || strstr(title, "Request http") )) {
        isPlayingNow = false;
    }
    if (title && titleContainsTimeout(title)) {
        isPlayingNow = false;
    }

    if (!isPlayingNow) {
        currentCoverUrl = "/logo.svg";
        updateDisplayCover();
        lastStationName = ""; 
        lastSongTitle = "";
        return;
    }

    String stationName = String(config.station.name);
    String currentSong = cleanTrackPrefix(String(title));
    String manualCoverUrl;
    const bool haveStationCover = findManualCover(stationName, manualCoverUrl);

    // Нет реального StreamTitle — только имя станции (fallback). iTunes по нему бессмысленен и грузит сеть.
    // В этом случае показываем station-cover (если есть), иначе логотип.
    if (currentSong.length() > 0 && currentSong == stationName) {
      currentCoverUrl = haveStationCover ? manualCoverUrl : "/logo.svg";
      updateDisplayCover();
      return;
    }

    if (stationName == lastStationName && currentSong == lastSongTitle) {
      return;
    }

    lastStationName = stationName;
    lastSongTitle = currentSong;

    // Показ обложек на TFT выключен — не кэшируем и не тянем icy-logo/iTunes; веб отдаёт icy-logo в /api/current-cover.
    if (!isCoversEnabledForWebOrDisplay()) {
      currentCoverUrl = haveStationCover ? manualCoverUrl : "/logo.svg";
      updateDisplayCover();
      return;
    }

    if (g_sdCoverReady) {
      String key = String("sd:") + metaTitle;
      if (coverCacheExists(key)) {
        currentCoverUrl = coverCacheUrlForKey(key);
        updateDisplayCover();
        g_sdCoverReady = false;
        return;
      }
    }

    if (currentSong.length() > 5 && !currentSong.startsWith("[")) {
      // ПРИОРИТЕТ 1: пользовательские обложки из папки station_covers
      if (haveStationCover) {
        currentCoverUrl = manualCoverUrl;
        updateDisplayCover();
        return;
      }
      
      String coverSearchTitle = normalizeTitleForCoverSearch(currentSong);
      String currentSongLower = currentSong;
      currentSongLower.toLowerCase();
      const bool origLooksIpmusic = currentSongLower.indexOf("ipmusic.ch") >= 0;
      if (coverSearchTitle.length() < 3) {
        if (origLooksIpmusic) {
          currentCoverUrl = haveStationCover ? manualCoverUrl : "/logo.svg";
          updateDisplayCover();
          return;
        }
        coverSearchTitle = currentSong;
      }
      String key = String("ITUNES:") + coverSearchTitle;
      if (coverCacheExists(key)) {
        currentCoverUrl = coverCacheUrlForKey(key);
        updateDisplayCover();
      } else {
        // Не мигаем пустым/служебным состоянием: пока ищем трек, держим station-cover (если есть).
        currentCoverUrl = haveStationCover ? manualCoverUrl : "/logo.svg";
        updateDisplayCover();
        scheduleCoverDownload("SEARCH_ITUNES", key);
      }
      return;
    }

    // Если по тегу трека искать нечего/нельзя, используем обложку станции из icy-logo/icy-url.
    if (streamCoverUrl.length() > 7 && isHttpUrl(streamCoverUrl) && isLikelyDirectCoverImageUrl(streamCoverUrl)) {
      String key = String("URL:") + streamCoverUrl;
      if (coverCacheExists(key)) {
        currentCoverUrl = coverCacheUrlForKey(key);
        updateDisplayCover();
      } else {
        currentCoverUrl = haveStationCover ? manualCoverUrl : "/logo.svg";
        updateDisplayCover();
        scheduleCoverDownload(streamCoverUrl, key);
      }
      return;
    }

    currentCoverUrl = haveStationCover ? manualCoverUrl : "/logo.svg";
    updateDisplayCover();
}

// 2. Обработчики событий аудио-библиотеки

// Если станция присылает ссылку в icy-url (часто главная страница, не картинка).
// Не затираем URL из icy-logo, если он уже указывает на изображение.
void audio_icyurl(const char* info) {
    if (!info || strlen(info) < 8) return;
    String s = String(info);
    s.trim();
    if (!isHttpUrl(s)) return;
    if (!isLikelyDirectCoverImageUrl(s)) return;
    if (streamCoverUrl.length() > 10) {
      String low = streamCoverUrl;
      low.toLowerCase();
      const bool haveImg = low.endsWith(".jpg") || low.endsWith(".jpeg") || low.endsWith(".png") ||
                           low.endsWith(".webp") || low.endsWith(".gif") || low.endsWith(".svg");
      if (haveImg) {
        String ns = s;
        ns.toLowerCase();
        const bool newIsImg = ns.endsWith(".jpg") || ns.endsWith(".jpeg") || ns.endsWith(".png") ||
                              ns.endsWith(".webp") || ns.endsWith(".gif") || ns.endsWith(".svg");
        if (!newIsImg) return;
      }
    }
    streamCoverUrl = s;
}

// URL обложки из заголовка icy-logo (часто JPG/PNG) — приоритетнее iTunes.
void audio_icylogo(const char* info) {
    if (!info || strlen(info) < 8) return;
    String u = String(info);
    u.trim();
    if (!isHttpUrl(u)) return;
    if (!isLikelyDirectCoverImageUrl(u)) return;
    streamCoverUrl = u;
}

// Строка не должна попадать в metaTitle/metaArtist для TrackFacts и обложек:
// сетевые ошибки, служебные бейджи UI ([соединение], [остановлено]…).
static bool isNonTrackMetaForFacts(const char* s) {
  if (!s || !s[0]) return true;
  if (titleContainsTimeout(s)) return true;
  // Бейдж вида «[статус]» — не название трека (см. metaTitleIsSystemBadgeOnly в netserver.cpp).
  if (s[0] == '[') return true;
  return false;
}

// Когда меняется песня (метаданные) — только для WEB режима.
// Для SD режима метаданные обрабатываются через ID3 колбэки в audiohandlers.h.
void audio_showtitle(const char* info) {
    // В SD режиме пропускаем — там ID3 теги обрабатываются отдельно.
    if (config.getMode() == PM_SDCARD) return;
    
    if (info && strlen(info) > 3) {
        String song = String(info);
        song.trim();
        
        // Очищаем от служебных префиксов типа "11 | ", "5. " и т.д.
        song = cleanTrackPrefix(song);

        // AUDIO_ERROR("timeout") → setTitle → сюда: не кормим TrackFacts/iTunes служебным текстом.
        if (isNonTrackMetaForFacts(song.c_str())) return;
        
        extern String metaTitle;
        extern String metaArtist;
        
        metaTitle = song;
        
        // Parse artist from "Artist - Title" format
        int dashPos = song.indexOf(" - ");
        if (dashPos > 0) {
            metaArtist = song.substring(0, dashPos);
        } else {
            metaArtist = song;
        }
        
        // Передаём очищенное название для поиска обложки
        handleCoverArt(song.c_str());
    }
}

/**
 * Фоновая задача: запись обложки из PSRAM в LittleFS.
 * Вызывается по семафору из audio_id3image после того, как аудио-задача полностью
 * прочитала картинку в PSRAM. Тяжёлые операции (запись в FS, обновление индекса кэша)
 * выполняются здесь, не блокируя декодер.
 */
static void sdCoverWriteTask(void* param) {
  for (;;) {
    if (!g_sdCoverWriteSem) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    if (xSemaphoreTake(g_sdCoverWriteSem, portMAX_DELAY) != pdTRUE) continue;

    uint8_t* buf = g_pendingSdCoverBuf;
    size_t sz = g_pendingSdCoverSize;
    char keyBuf[128];
    strlcpy(keyBuf, g_pendingSdCoverKey, sizeof(keyBuf));
    g_pendingSdCoverBuf = nullptr;
    g_pendingSdCoverSize = 0;
    g_pendingSdCoverKey[0] = '\0';

    if (!buf || sz == 0) continue;

    if (!isCoversEnabledForWebOrDisplay()) {
      free(buf);
      continue;
    }

    String keyStr(keyBuf);
    String path = coverCachePathForKey(keyStr);
    bool ok = false;
    if (lockLittleFS(500)) {
      ensureCoverDir();
      File out = LittleFS.open(path, "w");
      if (out) {
        out.write(buf, sz);
        out.close();
        ok = LittleFS.exists(path);
      }
      unlockLittleFS();
    }
    free(buf);

    if (ok) {
      registerCoverCacheFile(path);
      // Поздняя запись ID3 после ухода из SD (типично SD→BT) не должна вернуть обложку на TFT.
      if (config.getMode() != PM_SDCARD || g_modeSwitching) {
        Serial.println("[SD Cover] Saved OK, skip UI apply (left SD / mode switch)");
      } else {
        currentCoverUrl = coverCacheUrlForKey(keyStr);
        g_sdFileKey = keyStr;
        g_sdCoverReady = true;
        updateDisplayCover();
        Serial.printf("[SD Cover] Saved from ID3 (deferred): key=%s size=%u path=%s\n", keyBuf, (unsigned)sz, path.c_str());
      }
    }
  }
}

    void audio_id3image(File& file, const size_t pos, const size_t size) {
      if (config.getMode() != PM_SDCARD) return;
      if (!isCoversEnabledForWebOrDisplay()) return;
      if (size == 0 || size > kMaxCoverBytes) return;

      size_t saved_pos = file.position();
      file.seek(pos);

      // Данные APIC-фрейма начинаются НЕ с JPEG/PNG-сигнатуры:
      // сначала идут text encoding(1) + MIME(null-term) + picture type(1) + description(null-term),
      // и только потом — сами байты картинки. FLAC PICTURE block аналогичен.
      // Сканируем первые 300 байт в поиске JPEG (FF D8) или PNG (89 50 4E 47) магика.
      const size_t scanLen = (size < 300) ? size : 300;
      uint8_t scanBuf[300];
      file.read(scanBuf, scanLen);

      size_t imageOffset = 0;
      bool found = false;
      for (size_t i = 0; i + 3 < scanLen; i++) {
        bool isJpeg = (scanBuf[i] == 0xFF && scanBuf[i+1] == 0xD8);
        bool isPng  = (scanBuf[i] == 0x89 && scanBuf[i+1] == 0x50 &&
                       scanBuf[i+2] == 0x4E && scanBuf[i+3] == 0x47);
        if (isJpeg || isPng) { imageOffset = i; found = true; break; }
      }

      if (!found) {
        file.seek(saved_pos);
        return;
      }

      size_t imagePos  = pos + imageOffset;
      size_t imageSize = size - imageOffset;
      if (imageSize == 0 || imageSize > kMaxCoverBytes) {
        file.seek(saved_pos);
        return;
      }

      // Строим ключ из имени файла (metaTitle ещё не готов — debounce 500мс)
      const char* rawName = file.name();
      const char* slash = strrchr(rawName, '/');
      if (slash) rawName = slash + 1;
      char fileKey[128];
      strlcpy(fileKey, rawName, sizeof(fileKey));
      char* ext = strrchr(fileKey, '.');
      if (ext) *ext = '\0';
      String key = String("sd:file:") + String(fileKey);
      if (key.length() <= 8) key = "sd:file:unknown";

      // ID3 raw > 96 КБ не кэшируем для TFT (иначе decode 2000×2000 → WDT).
      // Для дисплея — iTunes, как в WEB. При ≤96 КБ поведение прежнее.
      if (imageSize > MAX_SD_ID3_COVER_BYTES) {
        Serial.printf("[SD Cover] ID3 oversized: %u > %u — skip cache, iTunes for TFT\n",
                      (unsigned)imageSize, (unsigned)MAX_SD_ID3_COVER_BYTES);
        file.seek(saved_pos);
        String searchTitle = normalizeTitleForCoverSearch(String(fileKey));
        if (searchTitle.length() >= 3) {
          scheduleCoverDownload("SEARCH_ITUNES", String("ITUNES:") + searchTitle);
        }
        return;
      }

      // Staging через PSRAM: читаем ВСЮ картинку из SD в PSRAM-буфер,
      // валидируем целостность; запись в LittleFS выполняется в фоне (sdCoverWriteTask).
      // Это решает две проблемы:
      // 1) Раньше чтение из SD шло чанками по 512 байт с записью в LittleFS без мьютекса —
      // конкурентный доступ из display-задачи → битые файлы → "раз-через-раз" обложки.
      // 2) Если SD-чтение обрывалось (readCount==0), файл оставался неполным, но
      // g_sdCoverReady всё равно ставился → TJpgDec падал на битом JPEG.
      uint8_t* stagingBuf = (uint8_t*)ps_malloc(imageSize);
      if (!stagingBuf) {
        Serial.printf("[SD Cover] PSRAM alloc failed (%u bytes)\n", (unsigned)imageSize);
        file.seek(saved_pos);
        return;
      }

      // Даём планировщику выполнить аудио-задачу после аллокации.
#if COVER_SD_YIELD_MS > 0
      vTaskDelay(pdMS_TO_TICKS(COVER_SD_YIELD_MS));
#endif

      file.seek(imagePos);
      size_t totalRead = 0;
      while (totalRead < imageSize) {
        size_t chunk = (imageSize - totalRead > 4096) ? 4096 : (imageSize - totalRead);
        size_t readCount = file.read(stagingBuf + totalRead, chunk);
        if (readCount == 0) break;    // SD-карта перестала отдавать данные
        totalRead += readCount;
        // Yield после каждого чанка — аудио-буфер не проседает при больших обложках.
#if COVER_SD_YIELD_MS > 0
        vTaskDelay(pdMS_TO_TICKS(COVER_SD_YIELD_MS));
#endif
      }
      file.seek(saved_pos);

      // Проверяем, что прочитали ВСЮ картинку целиком
      if (totalRead < imageSize) {
        Serial.printf("[SD Cover] Incomplete read: got %u of %u bytes\n",
                      (unsigned)totalRead, (unsigned)imageSize);
        free(stagingBuf);
        return;
      }

      // Запись в LittleFS перенесена в sdCoverWriteTask — аудио-задача только
      // заполняет PSRAM и передаёт буфер в фоновую задачу. Тяжёлая запись в FS и обновление
      // индекса кэша выполняются там, без блокировки декодера. Риск потери обложки при
      // снятии питания до записи — приемлем по требованию пользователя.
      if (g_pendingSdCoverBuf) {
        free(g_pendingSdCoverBuf);
        g_pendingSdCoverBuf = nullptr;
      }
      g_pendingSdCoverBuf = stagingBuf;
      g_pendingSdCoverSize = totalRead;
      strlcpy(g_pendingSdCoverKey, key.c_str(), sizeof(g_pendingSdCoverKey));
      if (g_sdCoverWriteSem)
        xSemaphoreGive(g_sdCoverWriteSem);
    }

void Config::init() {
  EEPROM.begin(EEPROM_SIZE);
  sdResumePos = 0;
  screensaverTicks = 0;
  screensaverPlayingTicks = 0;
  newConfigMode = 0;
  isScreensaver = false;
  memset(tmpBuf, 0, BUFLEN);
  //bootInfo();
#if RTCSUPPORTED
  _rtcFound = false;
  BOOTLOG("RTC begin(SDA=%d,SCL=%d)", RTC_SDA, RTC_SCL);
  if(rtc.init()){
    BOOTLOG("done");
    _rtcFound = true;
  }else{
    BOOTLOG("[ERROR] - Couldn't find RTC");
  }
#endif
  emptyFS = true;
  sdIndexing = false;
#if IR_PIN!=255
    irindex=-1;
#endif
#if defined(SD_SPIPINS) || SD_HSPI
  #if !defined(SD_SPIPINS)
    SDSPI.begin();
  #else
    SDSPI.begin(SD_SPIPINS); // SCK, MISO, MOSI
  #endif
#endif
  eepromRead(EEPROM_START, store);
  // ВАЖНО ДЛЯ Serial Monitor:
  // На некоторых системах (особенно после upload + auto-monitor) порт открывается
  // не мгновенно. Если вызвать bootInfo() слишком рано, первые строки теряются.
  // Архитектуру не меняем: bootInfo() остаётся в config.cpp, но перед вызовом
  // даём короткое «окно» для подключения монитора.
  // Faster/AXS: 0 — иначе ~3.5 с темноты до boot-лого на каждом cold-boot.
  // Остальные профили: 3500 мс для захвата PlatformIO Monitor.
#if DSP_MODEL==DSP_AXS15231B
  constexpr uint32_t kBootSerialWaitMs = 0;
#else
  constexpr uint32_t kBootSerialWaitMs = 3500;
#endif
  if (kBootSerialWaitMs > 0 && millis() < kBootSerialWaitMs) {
    delay(kBootSerialWaitMs - millis());
  }
  bootInfo(); // https://github.com/e2002/yoradio/pull/149
  // Лог DEBUG: информация о trackFactsCount до проверки версии.
  // Закомментировано, чтобы не захламлять монитор. При необходимости вернуть — раскомментируйте следующую строку.
  // Serial.printf("[CONFIG-INIT] Before version check: trackFactsCount=%d\n", store.trackFactsCount);
  
  // [REMOVED] Aggressive EEPROM corruption check that was erasing valid API keys
  // (API keys can be any length and start with any character, e.g., DeepSeek starts with "sk-")
  // Proper config validation is done via config_set check below.
  
  // Показываем что прочитано из EEPROM ДО любых миграций
  Serial.printf("[CONFIG-INIT] Read from EEPROM: version=%d, trackFactsProvider=%d\n", 
                store.version, store.trackFactsProvider);
  
  if (store.config_set != 4262) {
    Serial.println("[CONFIG-INIT] config_set != 4262, calling setDefaults()");
    setDefaults();
  }
  if(store.version>CONFIG_VERSION) store.version=1;
  bool needMigration = store.version != CONFIG_VERSION;
  if(needMigration) {
    Serial.printf("[CONFIG-INIT] Migration needed: %d -> %d\n", store.version, CONFIG_VERSION);
  }
  while(store.version!=CONFIG_VERSION) _setupVersion();
  // Лог DEBUG: информация после миграции конфигурации.
  // Закомментировано, чтобы сократить вывод в монитор. Для отладки раскомментируйте следующую строку.
  // Serial.printf("[CONFIG-INIT] After migration: trackFactsCount=%d, needMigration=%d\n", store.trackFactsCount, needMigration);
  
  // После миграции ИЛИ если конфиг поломан - ВСЕГДА переинициализируем TrackFacts поля
  if(store.version==CONFIG_VERSION) {
    bool needSave = false;
    
    // Дополнительная проверка на мусор trackFactsCount
    if (store.trackFactsCount == 0 ||
        store.trackFactsCount > TRACKFACTS_CONFIG_COUNT_MAX) {
      store.trackFactsCount = 1;
      needSave = true;
    }
    
    // Защита только от мусора в EEPROM (если значение больше 4, сбрасываем в iTunes)
    // НЕ проверяем наличие ключа здесь! Пользователь сначала выбирает провайдера,
    // потом вводит ключ, потом сохраняет. Проверка ключа - в момент запроса факта.
    if (store.trackFactsProvider > 4) {
        store.trackFactsProvider = 2; // iTunes (по умолчанию)
        needSave = true;
    }

    if(needSave) {
      // Если обнаружен мусор в TrackFacts полях — сохраняем исправленные значения в EEPROM
      Serial.println("[CONFIG-INIT] Fixing garbage in TrackFacts config, saving to EEPROM");
      eepromWrite(EEPROM_START, store);
    }
  }
  
  BOOTLOG("CONFIG_VERSION\t%d", store.version);
  
#if USE_BT_I2S
  store.play_mode = store.play_mode & 0b111;
  if (store.play_mode > MAX_PLAY_MODE) store.play_mode = PM_WEB;
#else
  store.play_mode = store.play_mode & 0b11;
  if (store.play_mode > MAX_PLAY_MODE) store.play_mode = PM_WEB;
#endif
  // Валидация источника URL-плейлиста (WEB/DLNA) после чтения EEPROM.
  // Если в памяти мусор или значение вне диапазона, безопасно возвращаем WEB.
  if (store.playlistSource > PL_SRC_DLNA) store.playlistSource = PL_SRC_WEB;
  // lastDlnaStation хранит 1-based индекс, ноль считаем неинициализированным состоянием.
  if (store.lastDlnaStation == 0) store.lastDlnaStation = 1;
  _initHW();
  
  // Монтируем файловую систему LittleFS.
  // На ESP32-S3 иногда требуется несколько попыток монтирования.
  // Метка раздела "spiffs" берётся из partitions.csv.
  littleFsReady = false;
  // Метка раздела совпадает с первым полем в partitions.csv (строка раздела данных ФС).
  const char* partLabel = "spiffs";
  for (int attempt = 0; attempt < 3 && !littleFsReady; ++attempt) {
    // На третьей попытке форматируем раздел (после битой заливки образа / неверного offset при upload).
    bool doFormat = (attempt == 2);
    if (LittleFS.begin(doFormat, "/littlefs", 10, partLabel)) {
      // Основной признак «ФС жива»: ненулевой размер раздела.
      size_t tbytes = LittleFS.totalBytes();
      bool fsLooksOk = (tbytes > 0);
      // Запасной тест: на редких связках core totalBytes() давал 0 при рабочей ФС — пробуем открыть известный файл.
      if (!fsLooksOk) {
        File probe = LittleFS.open("/www/theme.css", "r");
        if (probe) {
          fsLooksOk = true;
          probe.close();
          Serial.println("[LittleFS] totalBytes==0, но /www/theme.css открывается — считаем ФС валидной");
        }
      }
      if (fsLooksOk) {
        littleFsReady = true;
        Serial.printf("[LittleFS] OK attempt=%d format=%d total=%u used=%u\n",
                      attempt, doFormat ? 1 : 0, (unsigned)tbytes, (unsigned)LittleFS.usedBytes());
      } else {
        Serial.printf("[LittleFS] begin OK, но ФС пуста/бита (total=%u) — end() и повтор\n", (unsigned)tbytes);
        LittleFS.end();
      }
    } else {
      Serial.printf("[LittleFS] begin failed attempt=%d format=%d (проверьте образ LittleFS и адрес заливки)\n",
                    attempt, doFormat ? 1 : 0);
      delay(1000);
    }
  }

  if (!littleFsReady) {
    BOOTLOG("FATAL: LittleFS mount failed! Check partitions.csv / FS upload offset / chip erase.");
    return;
  }

  // Создаём семафор для потокобезопасного доступа к LittleFS
  if (littleFsMutex == NULL) {
    littleFsMutex = xSemaphoreCreateMutex();
  }

  BOOTLOG("LittleFS mounted successfully");

#ifdef USE_DLNA
  // После тестов DLNA в EEPROM мог остаться PL_SRC_DLNA при пустом или заглушечном CSV —
  // тогда интернет-станции не играют (REAL_PLAYL указывает на playlist_dlna.csv).
  if (store.playlistSource == PL_SRC_DLNA) {
    bool revertToWeb = false;
    if (!LittleFS.exists(PLAYLIST_DLNA_PATH)) {
      revertToWeb = true;
    } else {
      File pl = LittleFS.open(PLAYLIST_DLNA_PATH, "r");
      if (!pl || pl.size() < 32) {
        revertToWeb = true;
      } else {
        // Заглушки Build/Append содержат 127.0.0.1 — не реальные потоки.
        char head[128];
        const size_t n = pl.readBytes(head, sizeof(head) - 1);
        head[n] = '\0';
        pl.close();
        if (strstr(head, "127.0.0.1") != nullptr || strstr(head, "dlna-placeholder") != nullptr) {
          revertToWeb = true;
        }
      }
    }
    if (revertToWeb) {
      Serial.println("[CONFIG-INIT] DLNA-плейлист пуст/заглушка — возврат на WEB");
      saveValue(&store.playlistSource, (uint8_t)PL_SRC_WEB, true, true);
      saveValue(&store.lastPlayedSource, (uint8_t)PL_SRC_WEB, true, true);
    }
  }
  // Старые прошивки могли не синхронизировать playlistSource с lastPlayedSource после reboot.
  if (store.lastPlayedSource == PL_SRC_DLNA && store.playlistSource == PL_SRC_WEB && dlnaPlaylistReady()) {
    Serial.println("[CONFIG-INIT] Восстановление PL_SRC_DLNA из lastPlayedSource");
    saveValue(&store.playlistSource, (uint8_t)PL_SRC_DLNA, true, true);
  }
#endif

  // Создаём папки для обложек сразу после монтирования ФС
  ensureCoverDir();

  // Задача отложенной записи обложки SD в LittleFS — тяжёлая запись выполняется
  // в фоне, не блокируя аудио-задачу. Семафор и задача создаются один раз при старте.
  if (g_sdCoverWriteSem == NULL) {
    g_sdCoverWriteSem = xSemaphoreCreateBinary();
    if (g_sdCoverWriteSem != NULL &&
        xTaskCreatePinnedToCore(sdCoverWriteTask, "SDCvWr", 4096, NULL, 0, &g_sdCoverWriteTaskHandle, 1) != pdPASS)
      g_sdCoverWriteTaskHandle = NULL;
  }

  // Инициализируем PSRAM‑кэш для ключевых WebUI-файлов (script.js/style.css).
  // Это важный шаг для снижения нагрузки на LittleFS и ускорения загрузки
  // веб-интерфейса с телефонов в SD‑режиме.
  // Faster/AXS: кэш после display.init() (main) — иначе ~1–3 с темноты до boot-лого.
#if DSP_MODEL!=DSP_AXS15231B
  initWebAssetsCache();
#endif
  
  emptyFS = _isFSempty();
  //if(emptyFS) BOOTLOG("LittleFS is empty!");
  if(emptyFS) BOOTLOG("LittleFS is empty!");
  ssidsCount = 0;
  #ifdef USE_SD
  // И плейлист, и индекс теперь хранятся на LittleFS,
  // поэтому _SDplaylistFS всегда указывает на LittleFS (независимо от режима).
  _SDplaylistFS = &LittleFS;
  #else
  //_SDplaylistFS = &LittleFS;
  _SDplaylistFS = &LittleFS;
  #endif
  _bootDone=false;
  // setTimeConf() только из timeTask() после Wi‑Fi — иначе lwIP SNTP дергает DNS
  // параллельно с CoverDL/потоком → assert udp_new_ip_type (IDF 5.x).
}

void Config::_setupVersion(){
  uint16_t currentVersion = store.version;
  Serial.printf("[_setupVersion] Migrating from version %d to %d\n", currentVersion, currentVersion + 1);
  switch(currentVersion){
    case 1:
      saveValue(&store.screensaverEnabled, false);
      saveValue(&store.screensaverTimeout, (uint16_t)20);
      break;
    case 2:
      snprintf(tmpBuf, MDNS_LENGTH, "yoradio-%x", (unsigned int)getChipId());
      saveValue(store.mdnsname, tmpBuf, MDNS_LENGTH);
      saveValue(&store.skipPlaylistUpDown, false);
      break;
    case 3:
      saveValue(&store.screensaverBlank, false);
      saveValue(&store.screensaverPlayingEnabled, false);
      saveValue(&store.screensaverPlayingTimeout, (uint16_t)5);
      saveValue(&store.screensaverPlayingBlank, false);
      break;
    case 4:
      saveValue(&store.abuff, (uint16_t)(VS1053_CS==255?7:10));
      saveValue(&store.telnet, true);
      saveValue(&store.watchdog, true);
      saveValue(&store.timeSyncInterval, (uint16_t)60);    //min
      saveValue(&store.timeSyncIntervalRTC, (uint16_t)24); //hours
      saveValue(&store.weatherSyncInterval, (uint16_t)30); // min
      break;
    case 5:
      // TrackFacts settings - инициализируем правильно
      store.trackFactsEnabled = false;
      // Очищаем буфер ключа полностью
      memset(store.geminiApiKey, 0, GEMINI_KEY_LENGTH);
      store.trackFactsLang = (uint8_t)2; // Auto
      store.trackFactsCount = (uint8_t)1; // По умолчанию 1; потолок — TRACKFACTS_CONFIG_COUNT_MAX
      store.trackFactsProvider = (uint8_t)2; // iTunes (по умолчанию)
      // Сохраняем ВСЮ конфигурацию
      eepromWrite(EEPROM_START, store);
      break;
    case 6:
      // Sleep Timer ALL OFF - сохраняем переключатель между перезагрузками
      saveValue(&store.sleepTimerAllOff, false);
      break;
    case 7:
      // Новое поле wasPlaying для разделения пользовательской настройки и флага состояния.
      saveValue(&store.wasPlaying, false);
      break;
    case 8:
      // DLNA: добавляем базовые поля конфигурации и дефолтим их безопасными значениями.
      saveValue(&store.playlistSource, (uint8_t)PL_SRC_WEB, false);
      saveValue(&store.lastDlnaStation, (uint16_t)1, false);
      saveValue(&store.lastPlayedSource, (uint8_t)PL_SRC_WEB, false);
      saveValue(&store.dlnaIDX, (uint16_t)DLNA_IDX_DEFAULT, false);
      saveValue(store.dlnaHost, DLNA_HOST_DEFAULT, sizeof(store.dlnaHost), false);
      // Один commit на весь пакет миграции, чтобы не делать лишние циклы записи EEPROM.
      yoEepromCommitNow();
      break;
    case 9:
      // WebUI CPU bar: новое поле webCpuInfo — по умолчанию включено (как поведение до появления выключателя).
      saveValue(&store.webCpuInfo, true);
      break;
    case 10:
      // SD: отпечаток CID пока неизвестен — при первом mount выполнится spot-check и запомнится.
      saveValue(&store.sdCardFingerprint, (uint64_t)0);
      break;
    case 11:
      /* BacklightDown: инициализируем новые поля автоугасания подсветки.
       * Значения по умолчанию определяются из myoptions.h (DOWN_LEVEL / DOWN_INTERVAL)
       * при YO_BACKLIGHT_CTRL; иначе фейдер выключен. */
      #if YO_BACKLIGHT_CTRL
        #ifdef DOWN_LEVEL
          saveValue(&store.faderon,      true,                     false);
          /* DOWN_LEVEL (0-255) → конвертируем в проценты для хранения */
          saveValue(&store.fadermin,     (uint8_t)map(DOWN_LEVEL, 0, 255, 0, 100), false);
        #else
          saveValue(&store.faderon,      false,                    false);
          saveValue(&store.fadermin,     (uint8_t)10,              false);  /* 10% (~25/255) */
        #endif
        #ifdef DOWN_INTERVAL
          saveValue(&store.fadertimeout, (uint16_t)DOWN_INTERVAL,  false);
        #else
          saveValue(&store.fadertimeout, (uint16_t)60,             false);
        #endif
        yoEepromCommitNow();
      #else
        /* Нет управления подсветкой — фейдер отключён */
        saveValue(&store.faderon,      false,      false);
        saveValue(&store.fadermin,     (uint8_t)10, false);
        saveValue(&store.fadertimeout, (uint16_t)60);
      #endif
      break;
    case 12:
      /* TrackFacts VPS-прокси: веб-URL для Gemini/Groq (пусто = compile-time / прямое HTTPS). */
      memset(store.trackFactsProxyGemini, 0, TRACKFACTS_PROXY_URL_LENGTH);
      memset(store.trackFactsProxyGroq, 0, TRACKFACTS_PROXY_URL_LENGTH);
      eepromWrite(EEPROM_START, store);
      break;
    case 13:
      /* DeepSeek proxy URL в конце config_t (не сдвигает старые поля). */
      memset(store.trackFactsProxyDeepSeek, 0, TRACKFACTS_PROXY_URL_LENGTH);
      eepromWrite(EEPROM_START, store);
      break;
    default:
      break;
  }
  currentVersion++;
  store.version = currentVersion;
  // Всегда используем eepromWrite для сохранения версии и избегаем проблем со старым saveValue
  eepromWrite(EEPROM_START, store);
}

#ifdef USE_DLNA
static volatile bool g_dlnaPlaylistUpdatedPending = false;
#endif

void Config::setPlaylistSource(uint8_t src) {
  // playlistSource — текущий источник; lastPlayedSource — для восстановления после reboot.
  saveValue(&store.playlistSource, src);
  saveValue(&store.lastPlayedSource, src);
}

#ifdef USE_DLNA
void Config::notifyDlnaPlaylistUpdated() {
  g_dlnaPlaylistUpdatedPending = true;
}

bool Config::consumeDlnaPlaylistUpdated() {
  if (!g_dlnaPlaylistUpdatedPending) return false;
  g_dlnaPlaylistUpdatedPending = false;
  return true;
}
#endif

bool Config::dlnaPlaylistReady() {
#ifdef USE_DLNA
  if (!lockLittleFS(250)) return false;
  bool ready = false;
  if (LittleFS.exists(PLAYLIST_DLNA_PATH) && LittleFS.exists(INDEX_DLNA_PATH)) {
    File playlist = LittleFS.open(PLAYLIST_DLNA_PATH, "r");
    File index = LittleFS.open(INDEX_DLNA_PATH, "r");
    ready = playlist && index && playlist.size() > 0 && index.size() >= sizeof(uint32_t);
    if (playlist) playlist.close();
    if (index) index.close();
  }
  unlockLittleFS();
  return ready;
#else
  return false;
#endif
}

#ifdef USE_SD
bool Config::isSdAvailableForMode() const {
#if SDC_CS == 255
  return false;
#else
  if (g_sdMountUnavailableUntil != 0 && (unsigned long)millis() < g_sdMountUnavailableUntil) {
    return false;
  }
  return true;
#endif
}
#endif

int Config::nextUserModeCommand() {
#if USE_BT_I2S
  if (getMode() == PM_BT) return PM_WEB;
#endif

  if (getMode() == PM_SDCARD) {
#ifdef USE_DLNA
    if (dlnaPlaylistReady()) return USER_MODE_DLNA;
#endif
#if USE_BT_I2S
    return PM_BT;
#else
    return PM_WEB;
#endif
  }

#ifdef USE_DLNA
  if (getPlaylistSource() == PL_SRC_DLNA) {
#if USE_BT_I2S
    return PM_BT;
#else
    return PM_WEB;
#endif
  }
#endif

#ifdef USE_SD
  if (SDC_CS != 255 && isSdAvailableForMode()) return PM_SDCARD;
#endif
#ifdef USE_DLNA
  if (dlnaPlaylistReady()) return USER_MODE_DLNA;
#endif
#if USE_BT_I2S
  return PM_BT;
#else
  return PM_WEB;
#endif
}

#if USE_BT_I2S
void Config::syncBtUi() {
  if (getMode() != PM_BT) return;
  // Верхняя строка — источник; нижняя — локальный Play/Stop (пока нет AVRCP-тегов).
  setStation("Bluetooth");
  // Те же системные строки, что WEB/SD: [готов] / [остановлено] из displayL10n_*.h
  setTitle(btI2s.isBtPlaying() ? LANG::const_PlReady : LANG::const_PlStopped);
  // ESP-декодер в BT не крутится → idle-скринсейвер ошибочно считает «тишину»; сбрасываем счётчики.
  screensaverTicks = SCREENSAVERSTARTUPDELAY;
  screensaverPlayingTicks = SCREENSAVERSTARTUPDELAY;
  // Если уже ушли в SS/blank — вернуть PLAYER (тач/веб не должны «залипать» в blank).
  if (display.mode() == SCREENSAVER || display.mode() == SCREENBLANK) {
    setDspOn(true, false);
    display.putRequest(NEWMODE, PLAYER);
  }
  netserver.requestOnChange(MODE, 0);
  netserver.requestOnChange(GETPLAYERMODE, 0);
}
#endif

void Config::changeMode(int newmode){
#ifdef USE_SD
  // Вызов без аргумента используется аппаратной кнопкой MODE: выбираем следующий логический режим,
  // включая DLNA как WEB-аудиопайплайн с отдельным источником плейлиста.
  if (newmode < 0) newmode = nextUserModeCommand();

#ifdef USE_DLNA
  const bool targetDlna = (newmode == USER_MODE_DLNA);
  if (targetDlna && !dlnaPlaylistReady()) {
    Serial.println("[changeMode] DLNA skipped: playlist is not configured");
    netserver.requestOnChange(GETPLAYERMODE, 0);
    return;
  }
  // Для внутренней state machine DLNA остаётся PM_WEB; различие хранится в playlistSource.
  if (targetDlna) {
    if (getPlaylistSource() != PL_SRC_DLNA) setPlaylistSource(PL_SRC_DLNA);
    newmode = PM_WEB;
  } else if (newmode == PM_WEB && getPlaylistSource() != PL_SRC_WEB) {
    // Явный переход в WEB всегда возвращает интернет-радиостанции, а не прежний DLNA CSV.
    setPlaylistSource(PL_SRC_WEB);
  }
#endif

  Serial.printf("[changeMode] START: current=%d, requested=%d\n", (int)getMode(), newmode);
  g_modeSwitching = true;
  vTaskDelay(pdMS_TO_TICKS(20)); // короткий yield вместо 100 мс

  // Сброс Facts/Cover без ожидания SSL (раньше: wait timekeeper 3с + Cover 2с + Facts 5с).
  {
    extern TrackFacts trackFactsPlugin;
    trackFactsPlugin.abortForModeSwitch();
  }
  // Сразу убираем обложку с логики UI — иначе поздний CoverDL/ID3-write вернёт картинку SD уже в BT.
  currentCoverUrl = "/logo.svg";
  streamCoverUrl = "";
  g_sdCoverReady = false;
  g_sdFileKey = "";
  updateDisplayCover();
  g_pendingCoverUrl = "";
  g_pendingCoverKey = "";
  g_coverScheduleDeferred = false;
  g_coverWaitForScreensaver = false;
  g_coverScreensaverEnteredMs = 0;
  if (g_coverTaskRunning) {
    g_coverDlCancelRequested = true;
    Serial.println("[changeMode] Requesting CoverDL cancel (cooperative, no wait)");
  }
  // НЕ ждём Cover/Facts/timekeeper — режим переключаем сразу; задачи сами выйдут по cancel/epoch.

  bool pir = player.isRunning();
#if USE_BT_I2S
  // Без SD-карты на плате всё равно разрешаем WEB↔BT; SD-режим — только при SDC_CS.
  if (SDC_CS == 255 && (newmode == PM_SDCARD || getMode() == PM_SDCARD)) {
    g_modeSwitching = false;
    return;
  }
#else
  if (SDC_CS == 255) {
    g_modeSwitching = false;
    return;
  }
#endif
  if(getMode()==PM_SDCARD) {
    sdResumePos = player.getFilePos();
  }
  if(network.status==SOFT_AP || display.mode()==LOST){
    saveValue(&store.play_mode, static_cast<uint8_t>(PM_SDCARD));
    delay(50);
    yoEepromCommitNow();
    ESP.restart();
  }
  // SD нужна только при переходе в PM_SDCARD, не в BT.
  if(!sdman.ready && newmode==PM_SDCARD) {
    if(!sdman.startQuick()){
      Serial.println("##[ERROR]#\tSD Not Found");
      // Карта не читается — тост, cooldown SD в цикле, переходим к BT/DLNA без залипания в WEB.
      g_sdMountUnavailableUntil = millis() + SD_MOUNT_SKIP_MS;
      notifySdMountFailed();
      netserver.requestOnChange(SDINIT, 0);
      sdman.stop();
      newmode = modeAfterFailedSdMount();
#ifdef USE_DLNA
      if (newmode == USER_MODE_DLNA) {
        if (!dlnaPlaylistReady()) {
#if USE_BT_I2S
          newmode = PM_BT;
#else
          newmode = PM_WEB;
          g_modeSwitching = false;
          netserver.requestOnChange(GETPLAYERMODE, 0);
          return;
#endif
        } else {
          if (getPlaylistSource() != PL_SRC_DLNA) setPlaylistSource(PL_SRC_DLNA);
          newmode = PM_WEB;
        }
      }
#endif
      if (newmode == PM_SDCARD) {
        g_modeSwitching = false;
        netserver.requestOnChange(GETPLAYERMODE, 0);
        return;
      }
      Serial.printf("[changeMode] SD недоступна → fallback mode=%d\n", newmode);
    } else {
      // Успешный mount — снова предлагать SD в цикле режимов.
      g_sdMountUnavailableUntil = 0;
      vTaskDelay(pdMS_TO_TICKS(10)); // даём core0 обновить дисплей после инита SD
    }
  }
#if USE_BT_I2S
  // Уход из BT: вернуть селектор на ESP и восстановить I2S pinout до смены store.play_mode.
  if(getMode()==PM_BT && newmode!=PM_BT) {
    btI2s.exitBtMode();
  }
  // Вход в BT: остановить WEB/SD декодер до переключения SEL (лимит ~250 мс, не 1 с).
  if(newmode==PM_BT && getMode()!=PM_BT && pir) {
    player.sendCommand({PR_STOP, 0});
    uint32_t waitMs = 0;
    while(player.status()==PLAYING && waitMs<250){
      player.loop();
      delay(10);
      waitMs += 10;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
#endif
  // При уходе из SD гарантируем остановку плеера, чтобы не оставить открытые дескрипторы.
  // Смотрим актуальный status (не stale pir) — иначе второй PR_STOP после BT даёт STOPPED.
  const bool leavingSd = (getMode()==PM_SDCARD) && (newmode!=PM_SDCARD);
  if(leavingSd && player.status()==PLAYING){
    player.sendCommand({PR_STOP, 0});
    uint32_t waitMs = 0;
    while(player.status()==PLAYING && waitMs<250){
      player.loop();
      delay(10);
      waitMs+=10;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  if(newmode<0||newmode>MAX_PLAY_MODE){
    store.play_mode++;
    if(getMode() > MAX_PLAY_MODE) store.play_mode=0;
  }else{
    store.play_mode=(playMode_e)newmode;
  }
  saveValue(&store.play_mode, store.play_mode, true, true);
  // _SDplaylistFS всегда LittleFS — плейлист и индекс больше не на SD-карте.\n  _SDplaylistFS = &LittleFS;
#if USE_BT_I2S
  if(getMode()==PM_BT){
    btI2s.enterBtMode();
    currentCoverUrl = "/logo.svg";
    metaTitle = "";
    streamCoverUrl = "";
    updateDisplayCover();
    syncBtUi();
    display.putRequest(NEWMODE, PLAYER);
    display.putRequest(NEWSTATION);
    g_modeSwitching = false;
    g_modeSwitchTime = millis();
    Serial.printf("[changeMode] DONE: BT mode (cooldown %d sec)\n", MODE_SWITCH_COOLDOWN_MS/1000);
    return;
  }
#endif
  if(getMode()==PM_SDCARD){
    // При заходе в SD сразу сбрасываем текущее изображение и метатеги для обложки.
    currentCoverUrl = "/logo.svg";
    metaTitle = "";
    streamCoverUrl = "";
    g_sdCoverReady = false;
    g_sdFileKey = "";  // Сбрасываем file-key предыдущего трека
    updateDisplayCover();
    // Уведомляем веб-клиента о смене режима чтобы сбросить обложку немедленно.
    netserver.requestOnChange(GETPLAYERMODE, 0);
    netserver.requestOnChange(TITLE, 0);
    netserver.requestOnChange(STATIONNAME, 0);
    if(pir) player.sendCommand({PR_STOP, 0});
    display.putRequest(NEWMODE, SDCHANGE);
    #ifdef NETSERVER_LOOP1
    // Короткое ожидание UI SDCHANGE (раньше до ~1.5 с) — не блокируем тач надолго.
    for (int w = 0; w < 40 && display.mode() != SDCHANGE; w++) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    #endif
    delay(20);
  }
  if(getMode()==PM_WEB) {
    if(network.status==SDREADY) {
      yoEepromCommitNow();
      ESP.restart();
    }
    // После остановки — короткая пауза, чтобы аудиобиблиотека освободила файловые дескрипторы.
    delay(50);
    sdman.stop();
    // Сбрасываем метаданные SD при выходе в WEB режим.
    metaTitle = "";
    currentCoverUrl = "/logo.svg";
    updateDisplayCover();
    // Уведомляем веб о смене режима для обновления плейлиста.
    // STATIONNAME здесь НЕ шлём: station.name ещё от SD (папка) → веб мигал папкой
    // до loadStation веб-станции. Актуальный nameset придёт после initPlaylistMode / GETINDEX / PLAY.
    netserver.requestOnChange(GETPLAYERMODE, 0);
    netserver.requestOnChange(TITLE, 0);
    // Сразу показать PLAYER (BT→WEB): иначе UI ждёт initPlaylistMode/loadStation по 5–6 с.
    display.putRequest(NEWMODE, PLAYER);
    display.putRequest(NEWSTATION);
  }
  if(!_bootDone) return;
  initPlaylistMode();
  vTaskDelay(pdMS_TO_TICKS(5)); // даём core0 обработать очередь дисплея/веб после тяжёлого init
  // Сначала чистим очереди netserver/display, и только потом PR_PLAY.
  // Иначе changeModeTask (параллельно с плеером) через resetQueue() сносит MODE=playing,
  // который плеер успел поставить сразу после старта — веб залипает в «остановлено».
  netserver.resetQueue();
  display.resetQueue();
  // При переходе в SD шлём PR_PLAY_SD, чтобы плеер (Core 0) грузил из SD-плейлиста без гонки с getMode().
  // Для WEB режима используем accessor lastStation(), чтобы корректно учитывать WEB/DLNA source.
  // В BT декодер уже остановлен → pir=false; без PLAY после BT→WEB висим в STOPPED до Auto-retry 5с (лог 22:33:33→37).
  if (pir || (getMode() == PM_WEB && store.wasPlaying))
    player.sendCommand({(getMode()==PM_SDCARD) ? PR_PLAY_SD : PR_PLAY, (getMode()==PM_WEB) ? lastStation() : store.lastSdStation});
  //netserver.requestOnChange(GETPLAYERMODE, 0);
  netserver.requestOnChange(GETINDEX, 0);
  //netserver.requestOnChange(GETMODE, 0);
 // netserver.requestOnChange(CHANGEMODE, 0);
  display.putRequest(NEWMODE, PLAYER);
  display.putRequest(NEWSTATION);
  
  // Принудительно обновляем VU-метр после смены режима (решает проблему исчезновения VU)
  display.putRequest(SHOWVUMETER);

  // ВАЖНО: раньше здесь был websocket.closeAll(), который принудительно
  // отключал все Web-клиенты при смене режима. На практике это часто приводило
  // к ощущению «Web умер» при переключении WEB->SD: телефон успевал потерять
  // соединение в самый тяжёлый момент (SD mount/индексация) и иногда не
  // переподключался корректно.
  // Мы НЕ закрываем клиентов насильно. Вместо этого оставляем соединение жить
  // и даём AsyncWebSocket самому почистить «мертвых» клиентов через cleanupClients().
  // Это повышает шанс непрерывной работы WebUI при переходах.
  
  g_modeSwitching = false;
  g_modeSwitchTime = millis();
  Serial.printf("[changeMode] DONE: newMode=%d (cooldown %d sec)\n", (int)getMode(), MODE_SWITCH_COOLDOWN_MS/1000);
#endif
}

#ifdef USE_SD
// Проверяет до 3 файлов из кэшированного SD-плейлиста на реальном носителе (первый, середина, последний).
// Возвращает true, если хотя бы один путь отсутствует на SD — нужна полная переиндексация.
static bool sdPlaylistSpotCheckFailed() {
  if (!LittleFS.exists(PLAYLIST_SD_PATH) || !LittleFS.exists(INDEX_SD_PATH)) {
    return true;
  }
  File index = LittleFS.open(INDEX_SD_PATH, "r");
  if (!index) {
    return true;
  }
  size_t indexSize = index.size();
  if (indexSize < 4 || (indexSize % 4) != 0) {
    index.close();
    return true;
  }
  uint16_t total = (uint16_t)(indexSize / 4);
  // Номера станций для выборочной проверки (1-based): начало, середина, конец.
  uint16_t samples[3];
  uint8_t sampleCount = 0;
  samples[sampleCount++] = 1;
  if (total > 1) {
    samples[sampleCount++] = (uint16_t)(total / 2);
  }
  if (total > 2) {
    samples[sampleCount++] = total;
  }
  File playlist = LittleFS.open(PLAYLIST_SD_PATH, "r");
  if (!playlist) {
    index.close();
    return true;
  }
  int sOvol = 0;
  bool failed = false;
  for (uint8_t si = 0; si < sampleCount && !failed; si++) {
    uint16_t num = samples[si];
    index.seek((num - 1) * 4, SeekSet);
    uint32_t pos = 0;
    if (index.readBytes((char *)&pos, 4) != 4) {
      failed = true;
      break;
    }
    playlist.seek(pos, SeekSet);
    String line = playlist.readStringUntil('\n');
    if (!config.parseCSV(line.c_str(), config.tmpBuf, config.tmpBuf2, sOvol)) {
      failed = true;
      break;
    }
    if (!sdman.exists(config.tmpBuf2)) {
      Serial.printf("[INIT-SD] spot-check: файл не найден на SD: %s\n", config.tmpBuf2);
      failed = true;
    }
  }
  playlist.close();
  index.close();
  return failed;
}

// Сохраняет отпечаток текущей SD-карты в EEPROM после успешной индексации.
static void sdSaveCardFingerprint() {
  uint64_t fp = 0;
  if (sdman.readCardFingerprint(&fp) && fp != 0) {
    config.saveValue(&config.store.sdCardFingerprint, fp);
    Serial.printf("[INIT-SD] Отпечаток SD сохранён: %016llX\n", (unsigned long long)fp);
  }
}
#endif

void Config::initSDPlaylist() {
#ifdef USE_SD
  // [РЕМОНТ SD] Проверяем существование и РАЗМЕР файла индекса.
  // НОВОЕ ПОВЕДЕНИЕ: indexsd.dat больше не хранится на карте SD.
  // Мы переносим индекс на LittleFS, чтобы:
  // - ускорить последовательное чтение индекса (внутренняя флеш быстрее SD);
  // - снизить количество случайных обращений к SD во время воспроизведения;
  // - минимизировать влияние SD I/O на сетевой стек и WebUI.
  bool indexExists = LittleFS.exists(INDEX_SD_PATH);
  size_t indexSize = 0;
  if (indexExists) {
    File f = LittleFS.open(INDEX_SD_PATH, "r");
    indexSize = f.size();
    f.close();
  }
  
  // Проверяем также наличие плейлиста на LittleFS.
  // После миграции (плейлист был на SD, теперь на LittleFS) файл может отсутствовать,
  // и тогда нужно переиндексировать, чтобы он появился во внутренней флеш.
  bool playlistOnLFS = LittleFS.exists(PLAYLIST_SD_PATH);
  
  // Индексируем, если плейлист отсутствует на LittleFS
  // Если плейлист ЕСТЬ, но индекс отсутствует/битый — просто пересоздаём индекс из файла,
  // НЕ сканируем SD заново. Это сохраняет избранные и жанры.
  bool needFullScan = !playlistOnLFS;
  bool needReindex = playlistOnLFS && (!indexExists || indexSize < 4);
  if (!playlistOnLFS && indexExists) {
    Serial.println("[INIT-SD] Плейлист не найден на LittleFS (миграция?) — полная переиндексация");
  }
  if (needReindex) {
    Serial.println("[INIT-SD] Плейлист есть, но индекс отсутствует/битый — пересоздаю из файла");
    indexSDPlaylistFile();
    // Обновляем флаги после пересоздания
    indexExists = LittleFS.exists(INDEX_SD_PATH);
    if (indexExists) {
      File f = LittleFS.open(INDEX_SD_PATH, "r");
      indexSize = f.size();
      f.close();
    }
    // Если после пересборки индекс пустой (плейлист пуст или битый) — нужна полная индексация SD
    if (indexSize < 4) {
      needFullScan = true;
      Serial.println("[INIT-SD] Индекс пуст после пересборки — выполняю полное сканирование SD");
    }
  }

  // Смена физической SD-карты: сравниваем CID с сохранённым отпечатком + spot-check файлов.
  if (!needFullScan && sdman.ready) {
    uint64_t currentFp = 0;
    const bool fpReadable = sdman.readCardFingerprint(&currentFp);
    if (fpReadable && store.sdCardFingerprint != 0 && currentFp != store.sdCardFingerprint) {
      Serial.printf(
        "[INIT-SD] Смена SD-карты (CID %016llX -> %016llX) — полная переиндексация\n",
        (unsigned long long)store.sdCardFingerprint,
        (unsigned long long)currentFp
      );
      needFullScan = true;
    } else if (sdPlaylistSpotCheckFailed()) {
      Serial.println("[INIT-SD] spot-check не прошёл — полная переиндексация SD");
      needFullScan = true;
    } else if (fpReadable && currentFp != 0 && store.sdCardFingerprint == 0) {
      // Первый запуск после миграции: плейлист валиден — запоминаем CID без пересканирования.
      saveValue(&store.sdCardFingerprint, currentFp);
      Serial.printf("[INIT-SD] Отпечаток SD запомнен: %016llX\n", (unsigned long long)currentFp);
    }
  }
  
  Serial.printf("[INIT-SD] Ищу индекс (LittleFS): %s (Размер: %d байт)\n", INDEX_SD_PATH, (int)indexSize);
  Serial.printf(
    "[INIT-SD] Нужно ли сканировать SD: %s\n",
    needFullScan ? "ДА" : "НЕТ (Индекс и SD совпадают)"
  );

  if(needFullScan) {
    sdIndexing = true;
    setTitle("Индексация SD карты...");
    // Убрали переключение режима дисплея (NEWMODE, SDCHANGE), так как на S3 
    // это вызывает Hard Panic (LoadProhibited) в задачах отрисовки виджетов.
    // display.putRequest(NEWMODE, SDCHANGE); 
    netserver.requestOnChange(SDINDEXING, 0);
    
    Serial.println("[INIT-SD] Начинаю вызов sdman.indexSDPlaylist()...");
    sdman.indexSDPlaylist();
    Serial.println("[INIT-SD] Вызов sdman.indexSDPlaylist() завершен.");
    sdSaveCardFingerprint();
    
    sdIndexing = false;
    setTitle("Загрузка плейлиста...");
    netserver.requestOnChange(SDINDEXING, 0);
    // Не вызываем смену мода дисплея здесь, чтобы избежать краша.
    // Плеер сам обновит экран при старте первого трека.
    // display.putRequest(NEWMODE, PLAYER);
    display.putRequest(SHOWVUMETER);
  }
  
  // Перепроверяем файл после возможной индексации
  if (LittleFS.exists(INDEX_SD_PATH)) {
    Serial.println("[INIT-SD] Индексный файл (LittleFS) доступен для чтения.");
    File index = LittleFS.open(INDEX_SD_PATH, "r");
    if(needFullScan){
      lastStation(_randomStation());
      sdResumePos = 0;
    }
    index.close();
  } else {
    Serial.println("[INIT-SD] ОШИБКА: Индексный файл все еще не виден!");
  }
#endif
}

bool Config::LittleFSCleanup(){
  bool ret = (LittleFS.exists(PLAYLIST_SD_PATH)) || (LittleFS.exists(INDEX_SD_PATH)) || (LittleFS.exists(INDEX_PATH));
  if(LittleFS.exists(PLAYLIST_SD_PATH)) LittleFS.remove(PLAYLIST_SD_PATH);
  if(LittleFS.exists(INDEX_SD_PATH)) LittleFS.remove(INDEX_SD_PATH);
  if(LittleFS.exists(INDEX_PATH)) LittleFS.remove(INDEX_PATH);
  return ret;
}

void Config::waitConnection(){
#if I2S_DOUT==255
  return;
#endif
  while(!player.connproc) vTaskDelay(50);
  vTaskDelay(500);
}

char * Config::ipToStr(IPAddress ip){
  snprintf(ipBuf, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return ipBuf;
}
bool Config::prepareForPlaying(uint16_t stationId, bool forceSDPlaylist){
#if USE_BT_I2S
  if (getMode() == PM_BT) return false;
#endif
  setDspOn(1);
  vuThreshold = 0;
  screensaverTicks=SCREENSAVERSTARTUPDELAY;
  screensaverPlayingTicks=SCREENSAVERSTARTUPDELAY;
  if(getMode()!=PM_SDCARD && !forceSDPlaylist) {
    display.putRequest(PSTOP);
  }
  if(!loadStation(stationId, forceSDPlaylist)) return false;
  // Немедленный сброс обложки на логотип при смене станции/трека.
  // Раньше старая обложка висела, пока не скачается новая — пользователь видел
  // обложку от предыдущей станции даже если новая не работает.
  currentCoverUrl = "/logo.svg";
  streamCoverUrl = "";
  g_sdCoverReady = false;
  g_sdFileKey = "";
  g_lastCoverKey = "";
  updateDisplayCover();
  // Сброс метаданных Web/фактов: иначе в WebUI и плагинах висит старый трек до первого ICY-тега.
  metaTitle = "";
  metaArtist = "";
  // В SD-режиме больше не ставим заглушку "[next track]", чтобы метаданные из ID3 сразу перетирали старый текст.
  setTitle((getMode()==PM_WEB && !forceSDPlaylist) ? LANG::const_PlConnect : "");
  station.bitrate=0;
  setBitrateFormat(BF_UNKNOWN);
  g_audioFormatFlacActive = false;
  display.putRequest(DBITRATE);
  display.putRequest(NEWSTATION);
  /* Пока экран STATIONS или VOL — не уходим на PLAYER из play/recovery
   * (иначе список/громкость сносит при reconnect/ошибке потока). */
  if (display.mode() != STATIONS && display.mode() != VOL) {
    display.putRequest(NEWMODE, PLAYER);
  }
  // [SD+WEB][CRITICAL] Уведомляем WebUI только через очередь NetServer.
  // Прямой вызов netserver.loop() из prepareForPlaying() опасен, потому что:
  // 1) prepareForPlaying() запускается из задачи плеера (другое ядро);
  // 2) основной netserver.loop() уже крутится в main loop;
  // 3) параллельный вход в AsyncWebSocket/AsyncTCP даёт гонки (pcb is NULL, rx timeout, фриз).
  // Поэтому здесь только ставим события в очередь, без прямой обработки сокетов.
  netserver.requestOnChange(STATION, 0);
  // Мгновенно сбрасываем bitrate в WebUI при переключении станции.
  // Это убирает ситуацию, когда старый bitrate остается видимым до прихода новых данных потока.
  netserver.requestOnChange(BITRATE, 0);
  // [SD+WEB] Обновление состояния плеера ("playing/stopped") также отправляем через очередь.
  netserver.requestOnChange(MODE, 0);
  // SmartStart больше не переводим во временное состояние 0.
  // Флаг пользователя должен оставаться стабильным между перезагрузками и обрывами сети.
  return true;
}
void Config::configPostPlaying(uint16_t stationId){
  if(getMode()==PM_SDCARD) {
    sdResumePos = 0;
    saveValue(&store.lastSdStation, stationId);
  } else {
    // В WEB-режиме сохраняем last station по активному источнику WEB/DLNA.
    lastStation(stationId);
  }
  // Не трогаем smartstart — это пользовательская настройка (1=вкл, 2=выкл).
  // Вместо этого ставим wasPlaying=true: плеер сейчас играет → при перезагрузке можно автостартовать.
  saveValue(&store.wasPlaying, true);
  netserver.requestOnChange(MODE, 0);
  //display.putRequest(NEWMODE, PLAYER);
  display.putRequest(PSTART);
  // Принудительно обновляем VU-метр после начала воспроизведения.
  // При смене треков в SD-режиме событие PSTART может потеряться из-за
  // переполнения очереди дисплея — SHOWVUMETER служит страховкой.
  display.putRequest(SHOWVUMETER);
}

// WEB: если станция не прислала ни одного тега за разумное время после "[соединение]",
// автоматически подставляем в заголовок имя станции, чтобы не висел старый трек.
void Config::updateConnectTitleFallback() {
  // Если таймер не активен — ничего делать не нужно.
  if (g_connectTitleSinceMs == 0) return;

  // Проверяем только в WEB-режиме при реально играющем потоке.
  if (getMode() != PM_WEB || player.status() != PLAYING) {
    g_connectTitleSinceMs = 0;
    return;
  }

  uint32_t now = millis();
  const uint32_t timeoutMs = 5000; // 5 секунд ожидания тега
  if (now - g_connectTitleSinceMs < timeoutMs) return;

  // Таймаут истёк — сбрасываем таймер.
  g_connectTitleSinceMs = 0;

  // Если за это время метаданные так и не пришли и в заголовке до сих пор
  // служебная строка "[соединение]" или пусто — подставляем имя станции.
  if (strlen(config.station.title) == 0 ||
      strcmp(config.station.title, LANG::const_PlConnect) == 0) {
    // Важно: используем текущее имя станции без изменения, чтобы не трогать
    // существующую логику Playlist/Config.
    setTitle(config.station.name);
  }
}
#ifdef USE_SD
// Переопределяем указатель файловой системы для плейлиста в режиме SD.
// Это критически важно, чтобы плеер искал .mp3 на карте памяти, а не в LittleFS.
fs::FS* Config::SDPLFS() {
    // Всегда возвращаем LittleFS для чтения плейлиста и индекса.
    // Ранее в SD-режиме возвращался &sdman, и каждое чтение плейлиста/индекса
    // шло через SPI к SD-карте, конкурируя с аудиопотоком.
    // Теперь и playlistsd.csv, и indexsd.dat хранятся на LittleFS.
    // Аудиофайлы по-прежнему читаются напрямую через sdman (connecttoFS).
    return (fs::FS*)&LittleFS;
}
#endif

void Config::initPlaylistMode(){
  uint16_t _lastStation = 0;
#if USE_BT_I2S
  // Старт в BT: не грузим WEB-плейлист/станцию — только переключаем тракт и UI.
  if (getMode() == PM_BT) {
    btI2s.enterBtMode();
    syncBtUi();
    _bootDone = true;
    return;
  }
#endif
  uint16_t cs = playlistLength();
  #ifdef USE_SD
    if(getMode()==PM_SDCARD){
      if(!sdman.start()){
        store.play_mode=PM_WEB;
        Serial.println("SD Mount Failed");
        changeMode(PM_WEB);
        _lastStation = store.lastStation;
      }else{
        if(_bootDone) Serial.println("SD Mounted"); else BOOTLOG("SD Mounted");
          if(_bootDone) Serial.println("Waiting for SD card indexing..."); else BOOTLOG("Waiting for SD card indexing...");
          initSDPlaylist();
          if(_bootDone) Serial.println("done"); else BOOTLOG("done");
          _lastStation = store.lastSdStation;
          
          if(_lastStation>cs && cs>0){
            _lastStation=1;
          }
          if(_lastStation==0) {
            _lastStation = _randomStation();
          }
      }
    }else{
      Serial.println("done");
      // В WEB режиме восстанавливаем last station из активного источника (WEB/DLNA).
      _lastStation = lastStation();
    }
  #else //ifdef USE_SD
    store.play_mode=PM_WEB;
    // Без SD также учитываем активный WEB/DLNA источник.
    _lastStation = lastStation();
  #endif
  if(getMode()==PM_WEB && !emptyFS) initPlaylist();
  // I1: явно видно в Serial, если индекс не строили из-за «пустой» ФС (не хватает файлов /www).
  if(getMode()==PM_WEB && emptyFS) Serial.println("[PL] initPlaylist skipped: LittleFS considered empty (see [FS] missing www)");
  log_i("%d" ,_lastStation);
  if (_lastStation == 0 && cs > 0) {
    _lastStation = getMode()==PM_WEB?1:_randomStation();
  }
  lastStation(_lastStation);
  saveValue(&store.play_mode, store.play_mode, true, true);
  _bootDone = true;
  loadStation(_lastStation);
}

void Config::_initHW(){
  loadTheme();
  #if IR_PIN!=255
  eepromRead(EEPROM_START_IR, ircodes);
  if(ircodes.ir_set!=4224){
    ircodes.ir_set=4224;
    memset(ircodes.irVals, 0, sizeof(ircodes.irVals));
  }
  #endif
  #if BRIGHTNESS_PIN!=255
    pinMode(BRIGHTNESS_PIN, OUTPUT);
    setBrightness(false);
  #elif DSP_MODEL==DSP_AXS15231B
    /* Faster: без pinMode/analogWrite; LEDC уже в initDisplay, применяем store.brightness. */
    setBrightness(false);
  #elif DSP_MODEL==DSP_ST7701
    /* Supremus: LEDC на GFX_BL, без analogWrite. */
    setBrightness(false);
  #endif
  #if USE_BT_I2S
    btI2s.begin();
  #endif
}

uint16_t Config::color565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void Config::loadTheme(){
  theme.background    = color565(COLOR_BACKGROUND);
  theme.meta          = color565(COLOR_STATION_NAME);
  theme.metabg        = color565(COLOR_STATION_BG);
  theme.metafill      = color565(COLOR_STATION_FILL);
  theme.title1        = color565(COLOR_SNG_TITLE_1);
  theme.title2        = color565(COLOR_SNG_TITLE_2);
  theme.digit         = color565(COLOR_DIGITS);
  theme.div           = color565(COLOR_DIVIDER);
  theme.weather       = color565(COLOR_WEATHER);
  theme.weathercity   = color565(COLOR_WEATHER_CITY);
  theme.vumax         = color565(COLOR_VU_MAX);
  theme.vumin         = color565(COLOR_VU_MIN);
  theme.clock         = color565(COLOR_CLOCK);
  theme.clockbg       = color565(COLOR_CLOCK_BG);
  theme.seconds       = color565(COLOR_SECONDS);
  theme.dow           = color565(COLOR_DAY_OF_W);
  theme.date          = color565(COLOR_DATE);
  theme.heap          = color565(COLOR_HEAP);
  theme.buffer        = color565(COLOR_BUFFER);
  theme.ip            = color565(COLOR_IP);
  theme.vol           = color565(COLOR_VOLUME_VALUE);
  theme.rssi          = color565(COLOR_RSSI);
  theme.bitrate       = color565(COLOR_BITRATE);
  theme.volbarout     = color565(COLOR_VOLBAR_OUT);
  theme.volbarin      = color565(COLOR_VOLBAR_IN);
  theme.plcurrent     = color565(COLOR_PL_CURRENT);
  theme.plcurrentbg   = color565(COLOR_PL_CURRENT_BG);
  theme.plcurrentfill = color565(COLOR_PL_CURRENT_FILL);
  theme.playlist[0]   = color565(COLOR_PLAYLIST_0);
  theme.playlist[1]   = color565(COLOR_PLAYLIST_1);
  theme.playlist[2]   = color565(COLOR_PLAYLIST_2);
  theme.playlist[3]   = color565(COLOR_PLAYLIST_3);
  theme.playlist[4]   = color565(COLOR_PLAYLIST_4);
  #include "../displays/tools/tftinverttitle.h"
}

template <class T> int Config::eepromWrite(int ee, const T& value) {
  const uint8_t* p = (const uint8_t*)(const void*)&value;
  int i;
  int totalSize = sizeof(value);

  // Защита от записи за пределы эмулируемого EEPROM (раньше config_t=536 при слоте 524 — порча NVS).
  if (ee < 0 || totalSize <= 0 || (ee + totalSize) > EEPROM_SIZE) {
    Serial.printf("[eepromWrite] ERROR: out of bounds ee=%d size=%d EEPROM_SIZE=%d\n",
                  ee, totalSize, EEPROM_SIZE);
    return 0;
  }
  
  // ЛОГ: подавлено — подробный вывод записи EEPROM отключён
  // Чтобы включить, раскомментируйте строку ниже или определите VERBOSE_EEPROM
  // Serial.printf("[eepromWrite] Writing %d bytes to EEPROM at offset %d\n", totalSize, ee);
  
  for (i = 0; i < totalSize; i++) {
    EEPROM.write(ee + i, *p);
    if ((i + 1) % 32 == 0) {
      // Flush каждые 32 байта (boot/миграция — сразу, не idle-defer)
      yoEepromCommitNow();
      delay(1);
    }
    p++;
  }
  
  // Final commit сразу: иначе отложенный idle может не успеть до reboot
  yoEepromCommitNow();
  delay(5);  // ESP32 EEPROM требует время на финализацию

  // ЛОГ: подавлено — окончание записи EEPROM не выводится
  // Для отладки: раскомментируйте строку ниже или определите VERBOSE_EEPROM
  // Serial.printf("[eepromWrite] Completed writing %d bytes\n", i);
  return i;
}

template <class T> int Config::eepromRead(int ee, T& value) {
  // Очищаем буфер перед чтением
  memset(&value, 0, sizeof(value));

  uint8_t* p = (uint8_t*)(void*)&value;
  int i;
  int totalSize = sizeof(value);
  
  // ЛОГ: подавлено — подробный вывод чтения EEPROM отключён
  // Чтобы включить, раскомментируйте строку ниже или определите VERBOSE_EEPROM
  // Serial.printf("[eepromRead] Reading %d bytes from EEPROM at offset %d\n", totalSize, ee);
  
  for (i = 0; i < totalSize; i++) {
    uint8_t byte = EEPROM.read(ee + i);
    p[i] = byte;
    
    // ЛОГ: подавлено — подробное логирование по-байтово отключено
    // Для отладки: раскомментируйте блок ниже или определите VERBOSE_EEPROM
    // if (i < 60) {
    // Serial.printf("[eepromRead] Byte %d: 0x%02X (%c)\n", i, byte, (byte >= 32 && byte < 127) ? byte : '.');
    // }
  }
  
  // НЕ МОДИФИЦИРУЕМ последний байт! Просто завершаем чтение.
  // ЛОГ: подавлено — окончание чтения EEPROM (подробности подавлены)
  // Serial.printf("[eepromRead] Read %d bytes completed\n", i);
  return i;
}

void Config::reset(){
  setDefaults();
  delay(500);
  yoEepromCommitNow();
  ESP.restart();
}
void Config::enableScreensaver(bool val){
  saveValue(&store.screensaverEnabled, val);
#ifndef DSP_LCD
  display.putRequest(NEWMODE, PLAYER);
#endif
}
void Config::setScreensaverTimeout(uint16_t val){
  val=constrain(val,5,65520);
  saveValue(&store.screensaverTimeout, val);
#ifndef DSP_LCD
  display.putRequest(NEWMODE, PLAYER);
#endif
}
void Config::setScreensaverBlank(bool val){
  saveValue(&store.screensaverBlank, val);
#ifndef DSP_LCD
  display.putRequest(NEWMODE, PLAYER);
#endif
}
void Config::setScreensaverPlayingEnabled(bool val){
  saveValue(&store.screensaverPlayingEnabled, val);
#ifndef DSP_LCD
  display.putRequest(NEWMODE, PLAYER);
#endif
}
void Config::setScreensaverPlayingTimeout(uint16_t val){
  /* playing screensaver: секунды, минимум 5 (раньше были минуты — см. timekeeper.cpp) */
  val=constrain(val,5,65520);
  config.saveValue(&config.store.screensaverPlayingTimeout, val);
#ifndef DSP_LCD
  display.putRequest(NEWMODE, PLAYER);
#endif
}
void Config::setScreensaverPlayingBlank(bool val){
  saveValue(&store.screensaverPlayingBlank, val);
#ifndef DSP_LCD
  display.putRequest(NEWMODE, PLAYER);
#endif
}
void Config::setSntpOne(const char *val){
  bool tzdone = false;
  if (strlen(val) > 0 && strlen(store.sntp2) > 0) {
    configTime(store.tzHour * 3600 + store.tzMin * 60, getTimezoneOffset(), val, store.sntp2);
    tzdone = true;
  } else if (strlen(val) > 0) {
    configTime(store.tzHour * 3600 + store.tzMin * 60, getTimezoneOffset(), val);
    tzdone = true;
  }
  if (tzdone) {
    timekeeper.forceTimeSync = true;
    saveValue(config.store.sntp1, val, 35);
  }
}
void Config::setShowweather(bool val){
  config.saveValue(&config.store.showweather, val);
  timekeeper.forceWeather = true;
  display.putRequest(SHOWWEATHER);
}
void Config::setWeatherKey(const char *val){
  saveValue(store.weatherkey, val, WEATHERKEY_LENGTH);
  display.putRequest(NEWMODE, CLEAR);
  display.putRequest(NEWMODE, PLAYER);
}

void Config::setTrackFactsEnabled(bool val){
  saveValue(&store.trackFactsEnabled, val);
}

void Config::setGeminiApiKey(const char *val){
  // Используем saveValue - точно как для weatherkey
  // Это гарантирует правильное сохранение отдельного поля
  saveValue(store.geminiApiKey, val, GEMINI_KEY_LENGTH);
  display.putRequest(NEWMODE, CLEAR);
  display.putRequest(NEWMODE, PLAYER);
}

void Config::setTrackFactsLang(uint8_t val){
  saveValue(&store.trackFactsLang, val);
  Serial.printf("[setTrackFactsLang] Set to: %d\n", val);
}

void Config::setTrackFactsCount(uint8_t val){
  if (val >= 1 && val <= TRACKFACTS_CONFIG_COUNT_MAX) {
    saveValue(&store.trackFactsCount, val);
    Serial.printf("[setTrackFactsCount] Set to: %d\n", val);
  }
}

void Config::setTrackFactsProxyGemini(const char *val){
  // Веб-URL прокси для Gemini (пустая строка сбрасывает к compile-time / прямому HTTPS)
  saveValue(store.trackFactsProxyGemini, val ? val : "", TRACKFACTS_PROXY_URL_LENGTH);
}

void Config::setTrackFactsProxyGroq(const char *val){
  // Веб-URL прокси для Groq (пустая строка сбрасывает к compile-time / прямому HTTPS)
  saveValue(store.trackFactsProxyGroq, val ? val : "", TRACKFACTS_PROXY_URL_LENGTH);
}

void Config::setTrackFactsProxyDeepSeek(const char *val){
  saveValue(store.trackFactsProxyDeepSeek, val ? val : "", TRACKFACTS_PROXY_URL_LENGTH);
}

void Config::setTrackFactsProvider(uint8_t val){
  // Сохранение провайдера TrackFacts в EEPROM
  // Используем force=true чтобы гарантировать запись даже если значение "кажется" тем же
  // (защита от race condition при быстрых переключениях в веб-интерфейсе)
  if(val <= 4) {
    store.trackFactsProvider = val;
    // Сохраняем ВСЮ конфигурацию чтобы гарантировать запись
    eepromWrite(EEPROM_START, store);
    Serial.printf("[setTrackFactsProvider] Saved provider=%d to EEPROM (full write)\n", val);
    
    // Верификация - перечитываем и проверяем
    config_t verify;
    eepromRead(EEPROM_START, verify);
    if(verify.trackFactsProvider != val) {
      Serial.printf("[setTrackFactsProvider] ERROR: verification failed! Read back %d instead of %d\n", 
                    verify.trackFactsProvider, val);
    } else {
      Serial.printf("[setTrackFactsProvider] Verified OK: provider=%d\n", verify.trackFactsProvider);
    }
  }
}

void Config::setSDpos(uint32_t val){
  if (getMode()==PM_SDCARD){
    sdResumePos = 0;
    if(!player.isRunning()){
      player.setResumeFilePos(val-player.sd_min);
      player.sendCommand({PR_PLAY, config.store.lastSdStation});
    }else{
      player.setFilePos(val-player.sd_min);
    }
  }
}
#if IR_PIN!=255
void Config::setIrBtn(int val){
  irindex = val;
  netserver.irRecordEnable = (irindex >= 0);
  irchck = 0;
  netserver.irValsToWs();
  if (irindex < 0) saveIR();
}
#endif
void Config::resetSystem(const char *val, uint8_t clientId){
  if (strcmp(val, "system") == 0) {
    saveValue(&store.smartstart, (uint8_t)2, false);
    saveValue(&store.wasPlaying, false, false);  // Сброс при reset system
    saveValue(&store.audioinfo, false, false);
    saveValue(&store.vumeter, false, false);
    saveValue(&store.softapdelay, (uint8_t)0, false);
    saveValue(&store.abuff, (uint16_t)(VS1053_CS==255?7:10), false);
    saveValue(&store.telnet, true);
    saveValue(&store.watchdog, true);
    // WebUI «CPU Info»: при сбросе SYSTEM — выключено (как при первой прошивке).
    saveValue(&store.webCpuInfo, false, false);
    snprintf(store.mdnsname, MDNS_LENGTH, "yoradio-%x", (unsigned int)getChipId());
    saveValue(store.mdnsname, store.mdnsname, MDNS_LENGTH, true, true);
    display.putRequest(NEWMODE, CLEAR); display.putRequest(NEWMODE, PLAYER);
    netserver.requestOnChange(GETSYSTEM, clientId);
    return;
  }
  if (strcmp(val, "screen") == 0) {
  // Сброс экрана: переворот — из myoptions (DSP_FLIPSCREEN_DEFAULT), не жёстко false.
    saveValue(&store.flipscreen, DSP_FLIPSCREEN_DEFAULT, false);
    display.flip();
    saveValue(&store.invertdisplay, false, false);
    display.invert();
    saveValue(&store.dspon, true, false);
    saveValue(&store.brightness, (uint8_t)100, false);
    setBrightness(false);
    saveValue(&store.contrast, (uint8_t)55, false);
    display.setContrast();
    saveValue(&store.numplaylist, false);
    saveValue(&store.screensaverEnabled, false);
    saveValue(&store.screensaverTimeout, (uint16_t)20);
    saveValue(&store.screensaverBlank, false);
    saveValue(&store.screensaverPlayingEnabled, false);
    saveValue(&store.screensaverPlayingTimeout, (uint16_t)5);
    saveValue(&store.screensaverPlayingBlank, false);
    /* BacklightDown: сброс настроек автоугасания подсветки к значениям по умолчанию */
    #if YO_BACKLIGHT_CTRL
      #ifdef DOWN_LEVEL
        saveValue(&store.faderon,      true,                                       false);
        saveValue(&store.fadermin,     (uint8_t)map(DOWN_LEVEL, 0, 255, 0, 100),  false);
      #else
        saveValue(&store.faderon,      false,                                      false);
        saveValue(&store.fadermin,     (uint8_t)10,                               false);
      #endif
      #ifdef DOWN_INTERVAL
        saveValue(&store.fadertimeout, (uint16_t)DOWN_INTERVAL);
      #else
        saveValue(&store.fadertimeout, (uint16_t)60);
      #endif
    #else
      saveValue(&store.faderon,      false,      false);
      saveValue(&store.fadermin,     (uint8_t)10, false);
      saveValue(&store.fadertimeout, (uint16_t)60);
    #endif
    // Сброс "screen" всегда возвращает показ обложек на дисплей в режим ON.
    setDisplayCoversEnabled(true);
    display.putRequest(NEWMODE, CLEAR); display.putRequest(NEWMODE, PLAYER);
    netserver.requestOnChange(GETSCREEN, clientId);
    return;
  }
  if (strcmp(val, "timezone") == 0) {
    saveValue(&store.tzHour, (int8_t)3, false);
    saveValue(&store.tzMin, (int8_t)0, false);
    saveValue(store.sntp1, "pool.ntp.org", 35, false);
    saveValue(store.sntp2, "0.ru.pool.ntp.org", 35);
    saveValue(&store.timeSyncInterval, (uint16_t)60);
    saveValue(&store.timeSyncIntervalRTC, (uint16_t)24);
    configTime(store.tzHour * 3600 + store.tzMin * 60, getTimezoneOffset(), store.sntp1, store.sntp2);
    timekeeper.forceTimeSync = true;
    netserver.requestOnChange(GETTIMEZONE, clientId);
    return;
  }
  if (strcmp(val, "weather") == 0) {
    saveValue(&store.showweather, false, false);
    saveValue(store.weatherlat, "55.7512", 10, false);
    saveValue(store.weatherlon, "37.6184", 10, false);
    saveValue(store.weatherkey, "", WEATHERKEY_LENGTH);
    saveValue(&store.weatherSyncInterval, (uint16_t)30);
    //network.trueWeather=false;
    display.putRequest(NEWMODE, CLEAR); display.putRequest(NEWMODE, PLAYER);
    netserver.requestOnChange(GETWEATHER, clientId);
    return;
  }
  if (strcmp(val, "controls") == 0) {
    saveValue(&store.volsteps, (uint8_t)1, false);
    saveValue(&store.fliptouch, false, false);
    saveValue(&store.dbgtouch, false, false);
    saveValue(&store.skipPlaylistUpDown, false);
    setEncAcceleration(200);
    setIRTolerance(40);
    netserver.requestOnChange(GETCONTROLS, clientId);
    return;
  }
  if (strcmp(val, "1") == 0) {
    config.reset();
    return;
  }
}



void Config::setDefaults() {
  store.config_set = 4262;
  store.version = CONFIG_VERSION;
  store.volume = 12;
  store.balance = 0;
  store.trebble = 0;
  store.middle = 0;
  store.bass = 0;
  store.lastStation = 0;
  store.countStation = 0;
  store.lastSSID = 0;
  store.audioinfo = false;
  store.smartstart = 2;
  store.tzHour = 3;
  store.tzMin = 0;
  store.timezoneOffset = 0;

  store.vumeter=false;
  store.softapdelay=0;
  // Переворот дисплея при factory defaults — из myoptions.h (DSP_FLIPSCREEN_DEFAULT).
  store.flipscreen = DSP_FLIPSCREEN_DEFAULT;
  store.invertdisplay=false;
  store.numplaylist=false;
  store.fliptouch=false;
  store.dbgtouch=false;
  store.dspon=true;
  store.brightness=100;
  store.contrast=55;
  strlcpy(store.sntp1,"pool.ntp.org", 35);
  strlcpy(store.sntp2,"1.ru.pool.ntp.org", 35);
  store.showweather=false;
  strlcpy(store.weatherlat,"55.7512", 10);
  strlcpy(store.weatherlon,"37.6184", 10);
  strlcpy(store.weatherkey,"", WEATHERKEY_LENGTH);
  // По умолчанию "Display Cover" выключен.
  store._reserved = kReservedFlagDisplayCoverDisabled;
  store.lastSdStation = 0;
  store.sdsnuffle = false;
  store.volsteps = 1;
  store.encacc = 200;
  store.play_mode = 0;
  store.irtlp = 35;
  store.btnpullup = true;
  store.btnlongpress = 200;
  store.btnclickticks = 300;
  store.btnpressticks = 500;
  store.encpullup = false;
  store.enchalf = false;
  store.enc2pullup = false;
  store.enc2half = false;
  store.forcemono = false;
  store.i2sinternal = false;
  store.rotate90 = false;
  store.screensaverEnabled = false;
  store.screensaverTimeout = 20;
  store.screensaverBlank = false;
  /* BacklightDown: значения по умолчанию при первой прошивке */
  #if YO_BACKLIGHT_CTRL
    #ifdef DOWN_LEVEL
      store.faderon    = true;
      store.fadermin   = (uint8_t)map(DOWN_LEVEL, 0, 255, 0, 100); /* DOWN_LEVEL→% */
    #else
      store.faderon    = false;
      store.fadermin   = 10;  /* 10% (~25/255) */
    #endif
    #ifdef DOWN_INTERVAL
      store.fadertimeout = (uint16_t)DOWN_INTERVAL;
    #else
      store.fadertimeout = 60;
    #endif
  #else
    store.faderon      = false;
    store.fadermin     = 10;
    store.fadertimeout = 60;
  #endif
  snprintf(store.mdnsname, MDNS_LENGTH, "yoradio-%x", (unsigned int)getChipId());
  store.skipPlaylistUpDown = false;
  store.screensaverPlayingEnabled = false;
  store.screensaverPlayingTimeout = 5;
  store.screensaverPlayingBlank = false;
  store.abuff = VS1053_CS==255?7:10;
  store.telnet = true;
  store.watchdog = true;
  store.timeSyncInterval = 60;    //min
  store.timeSyncIntervalRTC = 24; //hour
  store.weatherSyncInterval = 30; //min
  // TrackFacts - устанавливаем ДО eepromWrite
  store.trackFactsEnabled = false;
  memset(store.geminiApiKey, 0, GEMINI_KEY_LENGTH);
  store.trackFactsLang = 2; // Auto
  store.trackFactsCount = 1; // По умолчанию 1; потолок — TRACKFACTS_CONFIG_COUNT_MAX
  store.trackFactsProvider = 2; // iTunes (по умолчанию)
  // VPS-прокси TrackFacts — пусто: берём myoptions или прямое HTTPS
  memset(store.trackFactsProxyGemini, 0, TRACKFACTS_PROXY_URL_LENGTH);
  memset(store.trackFactsProxyGroq, 0, TRACKFACTS_PROXY_URL_LENGTH);
  memset(store.trackFactsProxyDeepSeek, 0, TRACKFACTS_PROXY_URL_LENGTH);
  // Sleep Timer all off - по умолчанию выключен
  store.sleepTimerAllOff = false;
  // По умолчанию плеер не играл — автостарт запрещён
  store.wasPlaying = false;
  store.favOnly = false;
  // DLNA: инициализация каркаса хранения источника и состояния DLNA.
  store.playlistSource = (uint8_t)PL_SRC_WEB;
  store.lastDlnaStation = 1;
  store.lastPlayedSource = (uint8_t)PL_SRC_WEB;
  memset(store.dlnaHost, 0, sizeof(store.dlnaHost));
  strlcpy(store.dlnaHost, DLNA_HOST_DEFAULT, sizeof(store.dlnaHost));
  store.dlnaIDX = (uint16_t)DLNA_IDX_DEFAULT;
  // WebUI «CPU Info»: полоска загрузки CPU внизу плеера — по умолчанию выключена при первой прошивке.
  store.webCpuInfo = false;
  store.sdCardFingerprint = 0;
  eepromWrite(EEPROM_START, store);
}

void Config::setTimezone(int8_t tzh, int8_t tzm) {
  saveValue(&store.tzHour, tzh, false);
  saveValue(&store.tzMin, tzm);
}

void Config::setTimezoneOffset(uint16_t tzo) {
  saveValue(&store.timezoneOffset, tzo);
}

uint16_t Config::getTimezoneOffset() {
  return 0; // TODO
}

void Config::setSnuffle(bool sn){
  saveValue(&store.sdsnuffle, sn);
  if(store.sdsnuffle) player.next();
}

#if IR_PIN!=255
void Config::saveIR(){
  eepromWrite(EEPROM_START_IR, ircodes);
}
#endif

void Config::saveVolume(){
  saveValue(&store.volume, store.volume, true, true);
}

uint8_t Config::setVolume(uint8_t val) {
  store.volume = val;
  display.putRequest(DRAWVOL);
  netserver.requestOnChange(VOLUME, 0);
  return store.volume;
}

void Config::setTone(int8_t bass, int8_t middle, int8_t trebble) {
  saveValue(&store.bass, bass, false);
  saveValue(&store.middle, middle, false);
  saveValue(&store.trebble, trebble);
#if USE_BT_I2S
  if (getMode() != PM_BT)
#endif
  player.setTone(store.bass, store.middle, store.trebble);
  netserver.requestOnChange(EQUALIZER, 0);
}

void Config::setSmartStart(uint8_t ss) {
  saveValue(&store.smartstart, ss);
}

void Config::setBalance(int8_t balance) {
  saveValue(&store.balance, balance);
  player.setBalance(store.balance);
  netserver.requestOnChange(BALANCE, 0);
}

uint8_t Config::setLastStation(uint16_t val) {
  // Сохраняем позицию в контекст текущего режима и playlistSource.
  lastStation(val);
  // Возвращаем актуальное значение через общий accessor.
  return lastStation();
}

uint8_t Config::setCountStation(uint16_t val) {
  saveValue(&store.countStation, val);
  return store.countStation;
}

uint8_t Config::setLastSSID(uint8_t val) {
  saveValue(&store.lastSSID, val);
  return store.lastSSID;
}

void Config::setTitle(const char* title) {
  static bool settingTitle = false;
  if (settingTitle) return; // Предотвращение бесконечной рекурсии (setTitle -> setError -> setTitle)
  settingTitle = true;

  // Копируем в локальный буфер сразу: вызывающий код может передать указатель
  // на String/буфер, который потом меняется (другой поток, реаллокация).
  char titleCopy[BUFLEN];
  memset(titleCopy, 0, sizeof(titleCopy));
  if (title) strlcpy(titleCopy, title, sizeof(titleCopy));
  title = titleCopy;

  // Когда плеер остановлен И в заголовке уже висит системный статус ("[остановлено]", "[готов]"),
  // не даём колбэкам (ID3, StreamTitle) перезаписать его старым треком.
  // Но НЕ блокируем, если в заголовке пусто или "[соединение]" — это означает, что станция только
  // что переключилась и ещё не перешла в PLAYING, а мы уже должны принять первый реальный тег.
  if (player.status() == STOPPED && title && strlen(title) > 0) {
    const bool isSystemStatus =
      (strstr(title, "[остановлено]") != NULL) ||
      (strstr(title, "[готов]") != NULL) ||
      (strstr(title, "[соедин") != NULL);      // "[соединение]" — тоже системный, пропускаем
    if (!isSystemStatus) {
      // Проверяем, что в текущем заголовке УЖЕ стоит статус остановки — только тогда блокируем.
      const char* cur = config.station.title;
      bool curIsStop = cur && cur[0] &&
        ((strstr(cur, "[остановлено]") != NULL) || (strstr(cur, "[готов]") != NULL));
      if (curIsStop) {
        settingTitle = false;
        return;
      }
    }
  }

  // Если заголовок устанавливается в "[соединение]" (LANG::const_PlConnect)
  // в WEB-режиме — запоминаем время. Если в течение нескольких секунд станция не пришлёт
  // ни одного тега, мы позже автоматически заменим этот заголовок на имя станции.
  // Любой ДРУГОЙ заголовок (реальный тег) сбрасывает таймер.
  if (title && strlen(title) > 0 && strcmp(title, LANG::const_PlConnect) == 0 &&
      getMode() == PM_WEB) {
    g_connectTitleSinceMs = millis();
  } else {
    g_connectTitleSinceMs = 0;
  }

  vuThreshold = 0;
  memset(config.station.title, 0, BUFLEN);
  strlcpy(config.station.title, title, BUFLEN);
  u8fix(config.station.title);
  handleCoverArt(title);

  audio_showtitle(title);


  netserver.requestOnChange(TITLE, 0);
  // Убрано netserver.loop() из setTitle, так как это вызывает побочные эффекты
  // и рекурсивную обработку очереди в неподходящий момент.
  // netserver.loop();
  display.putRequest(NEWTITLE);

  settingTitle = false;
}

void Config::setStation(const char* station) {
  memset(config.station.name, 0, BUFLEN);
  strlcpy(config.station.name, station, BUFLEN);
  u8fix(config.station.name);
}

// ============================================================================
// isFavorite(num) — проверяет 5-ю колонку CSV ("1" = избранное).
// Читает одну строку из плейлиста по индексу, разбирает TAB-разделённые поля.
// ============================================================================
bool Config::isFavorite(uint16_t num) {
  uint16_t total = playlistLength();
  if (num == 0 || num > total) return false;

  fs::FS* pfs = SDPLFS();
  File playlist = pfs->open(REAL_PLAYL, "r");
  File index    = pfs->open(REAL_INDEX, "r");
  if (!playlist || !index) { playlist.close(); index.close(); return false; }

  index.seek((num - 1) * 4, SeekSet);
  uint32_t pos;
  index.readBytes((char*)&pos, 4);
  index.close();

  playlist.seek(pos, SeekSet);
  String line = playlist.readStringUntil('\n');
  playlist.close();

  // Ищем 5-ю колонку (индекс 4): name\turl\tovol\tgenre\tfavorite
  int tabCount = 0;
  const char* cursor = line.c_str();
  for (int col = 0; col < 4; col++) {
    const char* tab = strchr(cursor, '\t');
    if (!tab) return false; // нет 5-й колонки
    cursor = tab + 1;
  }
  // cursor теперь указывает на 5-ю колонку
  return (cursor[0] == '1');
}

// Максимальный размер плейлиста для варианта с буфером в PSRAM (одно чтение — правка — одна запись).
// Для ~3300 треков при ~130 байт на строку достаточно 450 KB.
#define PLAYLIST_PSRAM_MAX_SIZE (450000u)

bool Config::setFavorite(uint16_t num, bool fav) { // Обновляет признак избранного для строки плейлиста.
  const char* playlistPath = (getMode() == PM_SDCARD) ? PLAYLIST_SD_PATH : PLAYLIST_PATH; // Выбираем файл плейлиста по текущему режиму.
  if (num == 0 || !LittleFS.exists(playlistPath)) return false; // Защищаемся от некорректного номера и отсутствия файла.
  if (!lockLittleFS(2000)) return false; // Блокируем LittleFS для потокобезопасной операции.
  File in = LittleFS.open(playlistPath, "r"); // Открываем исходный плейлист для чтения.
  if (!in) { unlockLittleFS(); return false; } // При ошибке открытия снимаем блокировку и выходим.

  size_t fsize = in.size();
  // [PSRAM] Если есть PSRAM и плейлист помещается — читаем весь файл в PSRAM, правим строку в памяти, пишем обратно.
  // Так мы разгружаем FS: одно чтение и одна запись вместо построчного чтения + временный файл + копирование.
#if defined(ESP32) && defined(ARDUINO)
  if (psramInit() && fsize > 0 && fsize <= PLAYLIST_PSRAM_MAX_SIZE && (size_t)ESP.getFreePsram() >= fsize + 20000) {
    void* buf = heap_caps_malloc(fsize + 1, MALLOC_CAP_SPIRAM);
    if (buf) {
      char* p = (char*)buf;
      size_t rd = in.read((uint8_t*)p, fsize);
      in.close();
      if (rd != fsize) { heap_caps_free(buf); unlockLittleFS(); return false; }
      p[fsize] = '\0';
      unlockLittleFS(); // Отпускаем lock на время правки в памяти — веб может обслуживать запросы

      // Ищем начало строки с номером num (нумерация с 1).
      uint16_t lineNum = 0;
      char* lineStart = p;
      for (char* cur = p; *cur; cur++) {
        if (*cur == '\n') {
          lineNum++;
          if (lineNum == num) break;
          lineStart = cur + 1;
        }
      }
      if (lineNum == num) {
        // В строке ищем 4-й таб — после него идёт 5-я колонка (favorite), один символ '0' или '1'.
        int tabs = 0;
        char* col5 = nullptr;
        for (char* c = lineStart; *c && *c != '\n'; c++) {
          if (*c == '\t') {
            tabs++;
            if (tabs == 4) { col5 = c + 1; break; }
          }
        }
        if (col5 && (*col5 == '0' || *col5 == '1')) {
          *col5 = fav ? '1' : '0';
          if (!lockLittleFS(2000)) { heap_caps_free(buf); return false; }
          File out = LittleFS.open(playlistPath, "w");
          if (out && out.write((uint8_t*)p, fsize) == fsize) {
            out.close();
            heap_caps_free(buf);
            unlockLittleFS();
            return true;
          }
          if (out) out.close();
          unlockLittleFS();
        }
      }
      heap_caps_free(buf);
      // Fallback в стандартный путь — снова берём lock и открываем файл
      if (!lockLittleFS(2000)) return false;
      in = LittleFS.open(playlistPath, "r");
      if (!in) { unlockLittleFS(); return false; }
    } else {
      // PSRAM alloc failed — идём в стандартный путь (tmp файл)
    }
  }
#endif

  // Временный файл только для избранного (FAV_TMP_PATH), чтобы не конфликтовать с netserver (TMP_PATH).
  File tmp = LittleFS.open(FAV_TMP_PATH, "w", true); // Открываем временный файл для перезаписи плейлиста.
  if (!tmp) { in.close(); unlockLittleFS(); return false; } // При ошибке закрываем входной файл и выходим.
  uint16_t lineNum = 0; // Счётчик текущей строки плейлиста.
  bool updated = false; // Флаг, что нужная строка была обновлена.
  bool lineLenChanged = false; // Признак, что длина изменяемой строки поменялась (устаревает индекс SD).
  while (in.available()) { // Читаем весь плейлист построчно.
    String line = in.readStringUntil('\n'); // Получаем строку плейлиста.
    if (line.endsWith("\r")) line.remove(line.length() - 1); // Удаляем CR для Windows-окончаний строк.
    lineNum++; // Увеличиваем номер строки.
    if (lineNum == num) { // Если это целевая строка, обновляем 5-ю колонку.
      const size_t origLen = line.length(); // Длина строки ДО правки — сравним с новой ниже.
      String cols[5]; // Буфер для пяти колонок плейлиста.
      int col = 0; // Индекс текущей колонки.
      int start = 0; // Начальная позиция подстроки.
      for (int i = 0; i < (int)line.length() && col < 4; ++i) { // Ищем первые 4 таба.
        if (line[i] == '\t') { // Разделитель колонок.
          cols[col++] = line.substring(start, i); // Сохраняем колонку.
          start = i + 1; // Сдвигаем начало следующей колонки.
        }
      }
      cols[col++] = line.substring(start); // Записываем последнюю колонку из оставшейся части строки.
      while (col < 5) cols[col++] = ""; // Доводим количество колонок до 5.
      cols[4] = fav ? "1" : "0"; // Проставляем признак избранного.
      line = cols[0] + "\t" + cols[1] + "\t" + cols[2] + "\t" + cols[3] + "\t" + cols[4]; // Формируем обновлённую строку.
      updated = true; // Фиксируем, что обновление выполнено.
      // Если у строки не было колонок жанр/favorite (старый 3-колоночный SD-плейлист),
      // достраивание до 5 колонок УДЛИНЯЕТ строку → байтовые смещения в indexsd.dat «съезжают».
      // Запоминаем этот факт, чтобы ниже запросить одноразовую безопасную переиндексацию SD.
      if (line.length() != origLen) lineLenChanged = true;
    }
    tmp.print(line); // Записываем строку в временный файл.
    tmp.print("\n"); // Возвращаем перевод строки.
  }
  in.close(); // Закрываем исходный плейлист.
  tmp.close(); // Закрываем временный файл.

  File tmpIn = LittleFS.open(FAV_TMP_PATH, "r"); // Открываем временный файл для копирования.
  if (!tmpIn) { unlockLittleFS(); return false; } // При ошибке открытия снимаем блокировку и выходим.
  File dest = LittleFS.open(playlistPath, "w"); // Открываем целевой плейлист на перезапись.
  if (!dest) { tmpIn.close(); unlockLittleFS(); return false; } // При ошибке закрываем временный файл и выходим.
  uint8_t buf[512]; // Буфер копирования данных.
  while (tmpIn.available()) { // Копируем файл блоками.
    size_t rd = tmpIn.read(buf, sizeof(buf)); // Читаем блок из временного файла.
    if (rd > 0) dest.write(buf, rd); // Пишем блок в целевой файл.
  }
  dest.close(); // Закрываем целевой файл.
  tmpIn.close(); // Закрываем временный файл.
  LittleFS.remove(FAV_TMP_PATH); // Удаляем временный файл (свой путь — не конфликт с netserver).
  unlockLittleFS(); // Снимаем блокировку файловой системы.
  // Разовая самопочинка для СТАРЫХ (3-колоночных) SD-плейлистов: если длина строки
  // изменилась при достраивании колонки favorite, значит смещения в indexsd.dat устарели.
  // Запрашиваем безопасную переиндексацию из уже существующего файла (без сканирования SD-карты),
  // выполнится в основном loop() через processSDIndexRebuild(). Для новых 5-колоночных плейлистов
  // длина строки не меняется → эта ветка не срабатывает и тяжёлой операции не будет.
  if (updated && lineLenChanged && getMode() == PM_SDCARD) requestSDIndexRebuild();
  return updated; // Возвращаем статус обновления.
}

// ============================================================================
// findNextFavorite — ищет ближайшую избранную станцию в заданном направлении.
// Обходит весь плейлист циклически (wraparound). Возвращает 0 если нет избранных.
// ============================================================================
uint16_t Config::findNextFavorite(uint16_t current, bool forward) {
  uint16_t total = playlistLength();
  if (total == 0) return 0;

  // Обходим все станции в заданном направлении
  for (uint16_t i = 0; i < total; i++) {
    if (forward) {
      current = (current >= total) ? 1 : current + 1;
    } else {
      current = (current <= 1) ? total : current - 1;
    }
    if (isFavorite(current)) return current;
  }
  return 0; // ни одной избранной не найдено
}

void Config::indexPlaylist() {
  File playlist = LittleFS.open(PLAYLIST_PATH, "r");
  if (!playlist) {
    return;
  }
  int sOvol;
  File index = LittleFS.open(INDEX_PATH, "w");
  if (!index) {
    // Не смогли создать index.dat — пишем в Serial, чтобы на стенде было видно причину пустого плейлиста.
    Serial.println("[PL] index.dat: не удалось открыть на запись");
    playlist.close();
    return;
  }
  uint32_t entries = 0;
  while (playlist.available()) {
    uint32_t pos = playlist.position();
    if (parseCSV(playlist.readStringUntil('\n').c_str(), tmpBuf, tmpBuf2, sOvol)) {
      index.write((uint8_t *) &pos, 4);
      entries++;
    }
  }
  index.close();
  playlist.close();
  // Одна строка на перестройку: проверка I1 (после OTA LittleFS индекс должен собраться сам).
  Serial.printf("[PL] index.dat rebuilt: %u entries (%u bytes)\n", (unsigned)entries, (unsigned)(entries * 4u));
}

// Флаг отложенной перестройки индекса SD: при сохранении избранного в SD не вызываем
// indexSDPlaylistFile() из обработчика WebSocket, чтобы не блокировать веб. Перестройка
// выполняется в основном loop() через processSDIndexRebuild().
static bool g_sdIndexRebuildPending = false;

// ————— Самый надёжный вариант: сохранение избранного в отдельной задаче + очередь —————
// setFavorite() долго держит LittleFS (3300 треков = тяжёлая запись). Вызов из loop() блокирует
// весь цикл и WebSocket → веб «умирает» при нескольких кликах по звезде. Решение: запросы
// складываем в очередь, отдельная задача по одному выполняет setFavorite(). Веб и музыка не блокируются.
#define FAV_QUEUE_LEN  24
#define FAV_TASK_STACK 4096
struct FavRequest { uint16_t num; bool fav; };
static QueueHandle_t g_favQueue  = NULL;
static bool          g_favInited = false;

static void favUpdateTask(void* param) {
  FavRequest req;
  for (;;) {
    if (xQueueReceive(g_favQueue, &req, portMAX_DELAY) != pdTRUE) continue;
    g_playlistBusy = true;
    bool ok = config.setFavorite(req.num, req.fav);
    g_playlistBusy = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    // ВАЖНО: setFavorite меняет только 5-ю колонку (favorite) и НЕ затрагивает длину строк.
    // Позиции строк в плейлисте остаются теми же, поэтому INDEX_SD пересобирать не нужно.
    // Это убирает тяжёлую операцию indexSDPlaylistFile() из цепочки сохранения избранного,
    // чтобы веб и основной цикл не «умирали» на больших плейлистах (3000+ треков).
    // Шлём PLAYLIST_UPDATED, а не PLAYLISTSAVED: перестройка индекса не нужна (менялась только 5-я колонка),
    // а indexSDPlaylistFile() в main loop блокировала цикл на десятки секунд → веб зависал, FREEZE-RECOVERY.
    if (ok) {
      netserver.requestOnChange(PLAYLIST_UPDATED, 0);
    }
  }
}

void Config::requestFavoriteUpdate(uint16_t num, bool fav) {
  if (!g_favInited) {
    g_favQueue = xQueueCreate(FAV_QUEUE_LEN, sizeof(FavRequest));
    if (g_favQueue && xTaskCreatePinnedToCore(favUpdateTask, "FavUpdate", FAV_TASK_STACK, NULL, 1, NULL, 1) == pdPASS)
      g_favInited = true;
    if (!g_favInited) return; // очередь/задача не созданы — выходим без потери данных при следующем вызове
  }
  FavRequest req = { num, fav };
  // Краткая блокировка (50 ms), если очередь полна — не теряем запрос при серии кликов.
  xQueueSend(g_favQueue, &req, pdMS_TO_TICKS(50));
}

void Config::processFavoriteUpdate() {
  // Больше не вызываем setFavorite из loop — всё делает фоновая задача FavUpdate.
  (void)0;
}

void Config::requestSDIndexRebuild() {
  g_sdIndexRebuildPending = true;
}

void Config::processSDIndexRebuild() {
  if (!g_sdIndexRebuildPending) return;
  g_sdIndexRebuildPending = false;
  if (getMode() != PM_SDCARD) return;
  g_playlistBusy = true; // блокируем обложки и TrackFacts на время перестройки индекса
  if (!lockLittleFS(2000)) {
    g_sdIndexRebuildPending = true; // повторить позже
    g_playlistBusy = false;
    return;
  }
  indexSDPlaylistFile();
  unlockLittleFS();
  g_playlistBusy = false;
}

// Перестроить INDEX_SD из существующего PLAYLIST_SD на LittleFS,
// БЕЗ сканирования SD-карты. Используется при сохранении избранного.
// Вызывать при удержанном lockLittleFS или из processSDIndexRebuild().
void Config::indexSDPlaylistFile() {
  File playlist = LittleFS.open(PLAYLIST_SD_PATH, "r");
  if (!playlist) {
    Serial.println("[indexSDFile] Плейлист не найден, пропуск");
    return;
  }
  int sOvol;
  File index = LittleFS.open(INDEX_SD_PATH, "w");
  while (playlist.available()) {
    uint32_t pos = playlist.position();
    if (parseCSV(playlist.readStringUntil('\n').c_str(), tmpBuf, tmpBuf2, sOvol)) {
      index.write((uint8_t *) &pos, 4);
    }
  }
  index.close();
  playlist.close();
  Serial.println("[indexSDFile] Индекс перестроен из существующего плейлиста");
}

// Строит индекс из буфера (плейлист уже в памяти). Lock только на время записи файла — веб не блокируется на разборе.
#define INDEX_POSITIONS_MAX 6000
void Config::indexSDPlaylistFromBuffer(const char* buf, size_t size) {
  if (!buf || size == 0) return;
  uint32_t* positions = (uint32_t*)malloc(INDEX_POSITIONS_MAX * sizeof(uint32_t));
  if (!positions) return;
  size_t count = 0;
  char lineBuf[260];
  char nameBuf[BUFLEN];
  char urlBuf[BUFLEN];
  int sOvol = 0;
  size_t pos = 0;
  while (pos < size && count < INDEX_POSITIONS_MAX) {
    size_t lineStart = pos;
    while (pos < size && buf[pos] != '\n') pos++;
    size_t lineLen = pos - lineStart;
    if (lineLen >= sizeof(lineBuf)) { pos++; continue; }
    memcpy(lineBuf, buf + lineStart, lineLen);
    lineBuf[lineLen] = '\0';
    if (lineLen > 0 && lineBuf[lineLen - 1] == '\r') lineBuf[lineLen - 1] = '\0';
    if (parseCSV(lineBuf, nameBuf, urlBuf, sOvol))
      positions[count++] = (uint32_t)lineStart;
    pos++;
  }
  if (count == 0) { free(positions); return; }
  if (!lockLittleFS(2000)) { free(positions); return; }
  File index = LittleFS.open(INDEX_SD_PATH, "w");
  if (index) {
    for (size_t i = 0; i < count; i++)
      index.write((uint8_t*)&positions[i], 4);
    index.close();
    Serial.printf("[indexSDFile] Индекс перестроен из буфера, записей %u\n", (unsigned)count);
  }
  unlockLittleFS();
  free(positions);
}

void Config::initPlaylist() {
  //store.countStation = 0;
  // В SD-режиме плейлист инициализируется отдельными путями (initSDPlaylist и т.д.).
  if (getMode() == PM_SDCARD) {
    return;
  }
  // Для WEB/DLNA используем REAL_INDEX — иначе при PL_SRC_DLNA останется старый только WEB-индекс.
  if (!LittleFS.exists(REAL_INDEX)) {
#ifdef USE_DLNA
    if (getPlaylistSource() == PL_SRC_DLNA) {
      dlnaIndex.rebuildIndexFromPlaylist();
      return;
    }
#endif
    indexPlaylist();
    return;
  }
#ifdef USE_DLNA
  // DLNA-индекс — прежняя логика (только «файла нет»), его не трогаем.
  if (getPlaylistSource() == PL_SRC_DLNA) return;
#endif
  // I1: WEB index.dat есть, но пустой или битый (размер не кратен 4 байтам) — считаем, что его нет.
  // После OTA LittleFS / прерванной записи такой файл давал «0 станций» до ручной загрузки плейлиста.
  // Если плейлист действительно пуст, перестройка дешёвая (файл из пары байт).
  {
    File idx = LittleFS.open(INDEX_PATH, "r");
    const size_t idxSize = idx ? idx.size() : 0;
    if (idx) idx.close();
    if (idxSize < 4 || (idxSize % 4) != 0) {
      Serial.printf("[PL] index.dat invalid (size=%u) — rebuild\n", (unsigned)idxSize);
      indexPlaylist();
    }
  }

  /*if (LittleFS.exists(INDEX_PATH)) {
    File index = LittleFS.open(INDEX_PATH, "r");
    store.countStation = index.size() / 4;
    index.close();
    saveValue(&store.countStation, store.countStation, true, true);
  }*/
}
uint16_t Config::playlistLength(){
  uint16_t out = 0;
  // SDPLFS() теперь всегда LittleFS — индекс и в SD, и в WEB считывается одинаково.
  fs::FS* indexFS = SDPLFS();

  if (indexFS->exists(REAL_INDEX)) {
    File index = indexFS->open(REAL_INDEX, "r");
    size_t fileSize = index.size();
    index.close();
    
    // Защита от деления на ноль и некорректных размеров
    if (fileSize >= 4 && fileSize % 4 == 0) {
      out = fileSize / 4;
    }
  }
  return out;
}

#ifdef USE_SD
uint16_t Config::sdPlaylistLength() {
  if (!LittleFS.exists(INDEX_SD_PATH)) {
    return 0;
  }
  File index = LittleFS.open(INDEX_SD_PATH, "r");
  if (!index) {
    return 0;
  }
  size_t fileSize = index.size();
  index.close();
  if (fileSize >= 4 && (fileSize % 4) == 0) {
    return (uint16_t)(fileSize / 4);
  }
  return 0;
}
#endif

bool Config::loadStation(uint16_t ls, bool useSDPlaylist) {
  int sOvol;
  uint16_t cs;
  const char* playPath;
  const char* idxPath;
  if (useSDPlaylist) {
    // При переходе WEB→SD задача плеера на Core 0 может ещё видеть getMode()==PM_WEB.
    // Явно используем SD-плейлист/индекс, чтобы не открыть веб-URL по тому же номеру.
    if (!LittleFS.exists(INDEX_SD_PATH)) {
      memset(station.url, 0, BUFLEN);
      memset(station.name, 0, BUFLEN);
      strncpy(station.name, "ёRadio", BUFLEN);
      station.ovol = 0;
      return false;
    }
    File idxFile = LittleFS.open(INDEX_SD_PATH, "r");
    size_t sz = idxFile.size();
    idxFile.close();
    cs = (sz >= 4 && sz % 4 == 0) ? (uint16_t)(sz / 4) : 0;
    playPath = PLAYLIST_SD_PATH;
    idxPath  = INDEX_SD_PATH;
  } else {
    cs = playlistLength();
    playPath = REAL_PLAYL;
    idxPath  = REAL_INDEX;
  }
  if (cs == 0) {
    memset(station.url, 0, BUFLEN);
    memset(station.name, 0, BUFLEN);
    strncpy(station.name, "ёRadio", BUFLEN);
    station.ovol = 0;
    return false;
  }
  if (ls > cs) {
    ls = 1;
  }
  fs::FS* playlistFS = SDPLFS();
  fs::FS* indexFS    = SDPLFS();

  File playlist = playlistFS->open(playPath, "r");
  File index    = indexFS->open(idxPath, "r");
  index.seek((ls - 1) * 4, SeekSet);
  uint32_t pos;
  index.readBytes((char *) &pos, 4);
  index.close();
  playlist.seek(pos, SeekSet);
  if (parseCSV(playlist.readStringUntil('\n').c_str(), tmpBuf, tmpBuf2, sOvol)) {
    memset(station.url, 0, BUFLEN);
    memset(station.name, 0, BUFLEN);
    // Для SD режима: используем имя из CSV как начальное значение station.name (fallback).
    // ID3 теги перезапишут его через flushSdId3Meta() когда придут.
    // Это предотвращает показ пустой верхней строки до прихода ID3.
    strncpy(station.name, tmpBuf, BUFLEN);
    // station.url всегда заполняем - это путь к файлу
    strncpy(station.url, tmpBuf2, BUFLEN);
    
    // Ограничиваем ovol для предотвращения деления на ноль в volToI2S
    if (sOvol < 0) sOvol = 0;
    if (sOvol > 80) sOvol = 80;  // Максимум 80, чтобы 254 - 80*3 = 14 > 0
    
    station.ovol = sOvol;
    setLastStation(ls);
    if (useSDPlaylist) saveValue(&store.lastSdStation, ls);
  }
  playlist.close();
  return true;
}

char * Config::stationByNum(uint16_t num){
  // И плейлист, и индекс на LittleFS (SDPLFS() = &LittleFS всегда).
  fs::FS* playlistFS = SDPLFS();
  fs::FS* indexFS    = SDPLFS();

  File playlist = playlistFS->open(REAL_PLAYL, "r");
  File index    = indexFS->open(REAL_INDEX, "r");
  index.seek((num - 1) * 4, SeekSet);
  uint32_t pos;
  memset(_stationBuf, 0, sizeof(_stationBuf));
  index.readBytes((char *) &pos, 4);
  index.close();
  playlist.seek(pos, SeekSet);
  strncpy(_stationBuf, playlist.readStringUntil('\t').c_str(), sizeof(_stationBuf));
  _stationBuf[sizeof(_stationBuf) - 1] = '\0';  // strncpy не дописывает \0 при длинной строке
  playlist.close();
  return _stationBuf;
}

void Config::escapeQuotes(const char* input, char* output, size_t maxLen) {
  size_t j = 0;
  for (size_t i = 0; input[i] != '\0' && j < maxLen - 1; ++i) {
    if (input[i] == '"' && j < maxLen - 2) {
      output[j++] = '\\';
      output[j++] = '"';
    } else {
      output[j++] = input[i];
    }
  }
  output[j] = '\0';
}

// Снять одну пару обрамляющих CSV-кавычек "..." (если обе есть).
// DLNA playlist_dlna.csv писал url как "http://..." — без снятия Audio парсит
// хост как '"http' → DNS fail. WEB-плейлист без кавычек — no-op.
static void stripSurroundingCsvQuotes(char* field) {
  if (!field) return;
  size_t n = strlen(field);
  if (n < 2) return;
  if (field[0] != '"' || field[n - 1] != '"') return;
  memmove(field, field + 1, n - 2);
  field[n - 2] = '\0';
}

bool Config::parseCSV(const char* line, char* name, char* url, int &ovol) {
  char *tmpe;
  const char* cursor = line;
  char buf[5];
  tmpe = strstr(cursor, "\t");
  if (tmpe == NULL) return false;
  // Ограничиваем длину буфером BUFLEN (caller передаёт nameBuf[BUFLEN]).
  size_t nameLen = (size_t)(tmpe - cursor + 1);
  strlcpy(name, cursor, nameLen < BUFLEN ? nameLen : BUFLEN);
  stripSurroundingCsvQuotes(name);
  if (strlen(name) == 0) return false;
  cursor = tmpe + 1;
  tmpe = strstr(cursor, "\t");
  if (tmpe == NULL) return false;
  size_t urlLen = (size_t)(tmpe - cursor + 1);
  strlcpy(url, cursor, urlLen < BUFLEN ? urlLen : BUFLEN);
  stripSurroundingCsvQuotes(url);
  if (strlen(url) == 0) return false;
  cursor = tmpe + 1;
  if (strlen(cursor) == 0) return false;
  strlcpy(buf, cursor, sizeof(buf));
  ovol = atoi(buf);
  return true;
}

bool Config::parseJSON(const char* line, char* name, char* url, int &ovol) {
  char* tmps, *tmpe;
  const char* cursor = line;
  char port[8], host[246], file[254];
  tmps = strstr(cursor, "\":\"");
  if (tmps == NULL) return false;
  tmpe = strstr(tmps, "\",\"");
  if (tmpe == NULL) return false;
  strlcpy(name, tmps + 3, tmpe - tmps - 3 + 1);
  if (strlen(name) == 0) return false;
  cursor = tmpe + 3;
  tmps = strstr(cursor, "\":\"");
  if (tmps == NULL) return false;
  tmpe = strstr(tmps, "\"}");
  if (tmpe == NULL) return false;
  strlcpy(port, tmps + 3, tmpe - tmps - 3 + 1);
  ovol = atoi(port);
  return true;
}

bool Config::parseWsCommand(const char* line, char* cmd, size_t cmdSize, char* val, size_t valSize) {
  char *tmpe;
  if (!line || !cmd || !val || cmdSize == 0 || valSize == 0) return false;
  tmpe = strstr(line, "=");
  if (tmpe == NULL) return false;
  memset(cmd, 0, cmdSize);
  strlcpy(cmd, line, (size_t)(tmpe - line + 1) < cmdSize ? (size_t)(tmpe - line + 1) : cmdSize);
  memset(val, 0, valSize);
  strlcpy(val, tmpe + 1, valSize);
  return true;
}

bool Config::parseSsid(const char* line, char* ssid, char* pass) {
  char *tmpe;
  tmpe = strstr(line, "\t");
  if (tmpe == NULL) return false;
  uint16_t pos = tmpe - line;
  if (pos > 29 || strlen(line) > 71) return false;
  memset(ssid, 0, 30);
  strlcpy(ssid, line, (size_t)(pos + 1) < 30 ? (size_t)(pos + 1) : 30);
  memset(pass, 0, 40);
  // Ограничиваем длину буфером 40 — иначе при длинном пароле переполнение pass.
  strlcpy(pass, line + pos + 1, 40);
  return true;
}

bool Config::saveWifiFromNextion(const char* post){
  File file = LittleFS.open(SSIDS_PATH, "w");
  if (!file) {
    return false;
  } else {
    file.print(post);
    file.close();
    yoEepromCommitNow();
    ESP.restart();
    return true;
  }
}

bool Config::saveWifi() {
  if (!LittleFS.exists(TMP_PATH)) return false;
  LittleFS.remove(SSIDS_PATH);
  LittleFS.rename(TMP_PATH, SSIDS_PATH);
  yoEepromCommitNow();
  ESP.restart();
  return true;
}

void Config::setTimeConf(){
  if(strlen(store.sntp1)>0 && strlen(store.sntp2)>0){
    configTime(store.tzHour * 3600 + store.tzMin * 60, getTimezoneOffset(), store.sntp1, store.sntp2);
  }else if(strlen(store.sntp1)>0){
    configTime(store.tzHour * 3600 + store.tzMin * 60, getTimezoneOffset(), store.sntp1);
  }
}

bool Config::initNetwork() {
  File file = LittleFS.open(SSIDS_PATH, "r");
  if (!file || file.isDirectory()) {
    return false;
  }
  char ssidval[30], passval[40];
  uint8_t c = 0;
  while (file.available()) {
    if (parseSsid(file.readStringUntil('\n').c_str(), ssidval, passval)) {
      strlcpy(ssids[c].ssid, ssidval, 30);
      strlcpy(ssids[c].password, passval, 40);
      ssidsCount++;
      c++;
    }
  }
  file.close();
  return true;
}

void Config::setBrightness(bool dosave){
#if YO_BACKLIGHT_CTRL
  // softSleep таймера сна: не поднимаем PWM (Ticker/фейдер иначе вспыхивает экран).
  extern volatile bool g_sleepTimerShutdown;
  if (g_sleepTimerShutdown) return;
  if(!store.dspon && dosave) {
    display.wakeup();
  }
#if BRIGHTNESS_PIN!=255
  analogWrite(BRIGHTNESS_PIN, map(store.brightness, 0, 100, 0, 255));
#elif DSP_MODEL==DSP_AXS15231B
  /* Faster: LEDC на GFX_BL, без analogWrite (тест B). */
  axsSetBacklightDuty((uint8_t)map(store.brightness, 0, 100, 0, 255));
#elif DSP_MODEL==DSP_ST7701
  st7701SetBacklightDuty((uint8_t)map(store.brightness, 0, 100, 0, 255));
#endif
  if(!store.dspon) store.dspon = true;
  if(dosave){
#if DSP_MODEL==DSP_ST7701
    /* RGB 4848: commit на каждый тик слайдера рвёт DMA → «корёжит».
     * put в зеркало сразу; flash — через 700 мс после последнего изменения. */
    saveValue(&store.brightness, store.brightness, false, true);
    saveValue(&store.dspon, store.dspon, false, true);
    s_st7701BlCommitTicker.detach();
    s_st7701BlCommitTicker.once_ms(700, st7701BrightnessEepromCommit);
#else
    saveValue(&store.brightness, store.brightness, false, true);
    saveValue(&store.dspon, store.dspon, true, true);
#endif
  }
#endif
#ifdef USE_NEXTION
  nextion.wake();
  char cmd[15];
  snprintf(cmd, 15, "dims=%d", store.brightness);
  nextion.putcmd(cmd);
  if(!store.dspon) store.dspon = true;
  if(dosave){
    saveValue(&store.brightness, store.brightness, false, true);
    saveValue(&store.dspon, store.dspon, true, true);
  }
#endif
}

void Config::setDspOn(bool dspon, bool saveval){
  if(saveval){
    store.dspon = dspon;
    saveValue(&store.dspon, store.dspon, true, true);
  }
#ifdef USE_NEXTION
  if(!dspon) nextion.sleep();
  else nextion.wake();
#endif
  if(!dspon){
#if BRIGHTNESS_PIN!=255
  analogWrite(BRIGHTNESS_PIN, 0);
#elif DSP_MODEL==DSP_AXS15231B
  axsSetBacklightDuty(0);
#elif DSP_MODEL==DSP_ST7701
  st7701SetBacklightDuty(0);
#endif
    display.deepsleep();
  }else{
    display.wakeup();
#if BRIGHTNESS_PIN!=255
  analogWrite(BRIGHTNESS_PIN, map(store.brightness, 0, 100, 0, 255));
#elif DSP_MODEL==DSP_AXS15231B
  axsSetBacklightDuty((uint8_t)map(store.brightness, 0, 100, 0, 255));
#elif DSP_MODEL==DSP_ST7701
  st7701SetBacklightDuty((uint8_t)map(store.brightness, 0, 100, 0, 255));
#endif
  }
}

void Config::doSleep(){
  if(BRIGHTNESS_PIN!=255) analogWrite(BRIGHTNESS_PIN, 0);
#if DSP_MODEL==DSP_AXS15231B
  axsSetBacklightDuty(0);
#endif
#if DSP_MODEL==DSP_ST7701
  st7701SetBacklightDuty(0);
#endif
  display.deepsleep();
#ifdef USE_NEXTION
  nextion.sleep();
#endif
#if !defined(ARDUINO_ESP32C3_DEV)
  if(WAKE_PIN!=255) esp_sleep_enable_ext0_wakeup((gpio_num_t)WAKE_PIN, LOW);
  esp_sleep_enable_timer_wakeup(config.sleepfor * 60 * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

void Config::doSleepW(){
  if(BRIGHTNESS_PIN!=255) analogWrite(BRIGHTNESS_PIN, 0);
#if DSP_MODEL==DSP_AXS15231B
  axsSetBacklightDuty(0);
#endif
#if DSP_MODEL==DSP_ST7701
  st7701SetBacklightDuty(0);
#endif
  display.deepsleep();
#ifdef USE_NEXTION
  nextion.sleep();
#endif
#if !defined(ARDUINO_ESP32C3_DEV)
  if(WAKE_PIN!=255) esp_sleep_enable_ext0_wakeup((gpio_num_t)WAKE_PIN, LOW);
  esp_deep_sleep_start();
#endif
}

void Config::sleepForAfter(uint16_t sf, uint16_t sa){
  sleepfor = sf;
  if(sa > 0) timekeeper.waitAndDo(sa * 60, doSleep);
  else doSleep();
}

void Config::bootInfo() {
  // === ИНФОРМАЦИЯ О СИСТЕМЕ ПРИ ЗАГРУЗКЕ ===
  // Выводится в Serial Monitor при каждом старте устройства.
  // Если не видите этот вывод - проверьте скорость монитора (115200 baud)
  // и убедитесь что монитор подключен ДО перезагрузки устройства.
  
  Serial.flush(); // Принудительно очищаем буфер перед выводом
  delay(100);     // Увеличенная задержка для стабилизации связи
  
  // Дополнительные отладочные сообщения для проверки связи

  Serial.flush();
  delay(50);
  
  BOOTLOG("************************************************");
  BOOTLOG("*            ёRadio %s            *", YOVERSION);
  BOOTLOG("************************************************");
  BOOTLOG("EEPROM:		size=%d, start=%d, available=%d", EEPROM_SIZE, EEPROM_START, EEPROM_SIZE - EEPROM_START);
  BOOTLOG("config_t:	sizeof=%d bytes", sizeof(config_t));
  if(sizeof(config_t) > (EEPROM_SIZE - EEPROM_START)) {
    Serial.println("##[ERROR]#\tconfig_t TOO LARGE FOR EEPROM!");
    Serial.printf("##[ERROR]#\tconfig_t=%d bytes, available=%d bytes\n", sizeof(config_t), EEPROM_SIZE - EEPROM_START);
  }
  BOOTLOG("arduino:\t%d", ARDUINO);
  BOOTLOG("compiler:\t%s", __VERSION__);
  BOOTLOG("esp32core:\t%d.%d.%d", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
  uint32_t chipId = 0;
  for(int i=0; i<17; i=i+8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }
  BOOTLOG("chip:\t\tmodel: %s | rev: %d | id: %lu | cores: %d | psram: %lu", ESP.getChipModel(), ESP.getChipRevision(), chipId, ESP.getChipCores(), ESP.getPsramSize());
  BOOTLOG("display:\t%d", DSP_MODEL);
  if(VS1053_CS==255) {
    BOOTLOG("audio:\t\t%s (%d, %d, %d)", "I2S", I2S_DOUT, I2S_BCLK, I2S_LRC);
  }else{
    BOOTLOG("audio:\t\t%s (%d, %d, %d, %d, %s)", "VS1053", VS1053_CS, VS1053_DCS, VS1053_DREQ, VS1053_RST, VS_HSPI?"true":"false");
  }
  BOOTLOG("audioinfo:\t%s", store.audioinfo?"true":"false");
  BOOTLOG("smartstart:\t%d", store.smartstart);
  BOOTLOG("vumeter:\t%s", store.vumeter?"true":"false");
  BOOTLOG("softapdelay:\t%d", store.softapdelay);
  BOOTLOG("flipscreen:\t%s", store.flipscreen?"true":"false");
  BOOTLOG("invertdisplay:\t%s", store.invertdisplay?"true":"false");
  BOOTLOG("showweather:\t%s", store.showweather?"true":"false");
  BOOTLOG("buttons:\tleft=%d, center=%d, right=%d, up=%d, down=%d, mode=%d, pullup=%s", 
          BTN_LEFT, BTN_CENTER, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_MODE, BTN_INTERNALPULLUP?"true":"false");
  BOOTLOG("encoders:\tl1=%d, b1=%d, r1=%d, pullup=%s, l2=%d, b2=%d, r2=%d, pullup=%s", 
          ENC_BTNL, ENC_BTNB, ENC_BTNR, ENC_INTERNALPULLUP?"true":"false", ENC2_BTNL, ENC2_BTNB, ENC2_BTNR, ENC2_INTERNALPULLUP?"true":"false");
  BOOTLOG("ir:\t\t%d", IR_PIN);
  if(SDC_CS!=255) BOOTLOG("SD:\t\t%d", SDC_CS);
  BOOTLOG("------------------------------------------------");
  
  Serial.flush();
  delay(50);
}

