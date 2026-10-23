#include "options.h"
#include "Arduino.h"
//#include "../AsyncWebServer/SPIFFSEditor.h"
#include <LittleFS.h>
#include <Update.h>
#include "config.h"
#include "netserver.h"
#include "player.h"
#include "telnet.h"
#include "display.h"
#include "network.h"
#include "mqtt.h"
#include "controls.h"
#include "commandhandler.h"
#include "timekeeper.h"
#include "../displays/dspcore.h"
#include "../displays/widgets/widgetsconfig.h" //BitrateFormat
#include "../plugins/TrackFacts/TrackFacts.h"
#include "../plugins/TrackFacts/TrackFactsProxy.h"
#include "../plugins/SleepTimer/SleepTimer.h"
#include "../plugins/BacklightDown/BacklightDown.h" /* BacklightDown: доступ к updateFaderState() */
#include "../displays/tools/l10n.h"
#include "config.h" // scheduleChangeModeTask
#include "cpu_load.h" // webui_freertos_cpu_percent — полоска CPU в WebUI

/* Classic ESP32 (Aiwushka/Wrover): фиксы остаются; Serial-диагностика — макрос YORADIO_CLASSIC_HTTP_HEAP_DIAG. */
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
#ifndef YORADIO_CLASSIC_HTTP_HEAP_DIAG
#define YORADIO_CLASSIC_HTTP_HEAP_DIAG 0
#endif
#if YORADIO_CLASSIC_HTTP_HEAP_DIAG
#include <esp_heap_caps.h>
static inline void netserverLogHeap8bit(const char* where) {
  Serial.printf("[HEAP-8BIT] %s free=%u largest=%u\n",
                where ? where : "?",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
static inline void netserverLogHeapInternal(const char* where) {
  Serial.printf("[HEAP-INT] %s free=%u largest=%u\n",
                where ? where : "?",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
#endif

/* Classic: плейлист в PSRAM — VFS только вне async_tcp (модель 2). */
#ifndef PLAYLIST_PSRAM_CACHE_MAX
#define PLAYLIST_PSRAM_CACHE_MAX (512 * 1024)
#endif
static uint8_t* g_playlistPsramBuf = nullptr;
static size_t   g_playlistPsramLen = 0;
/* Перечитать кэш из main loop — не из body-колбэка AsyncWebServer (тот же async_tcp). */
static volatile bool g_playlistPsramReloadReq = false;

static void netserverFreePlaylistPsram() {
  if (g_playlistPsramBuf) {
    free(g_playlistPsramBuf);
    g_playlistPsramBuf = nullptr;
  }
  g_playlistPsramLen = 0;
}

/* Читать текущий REAL_PLAYL/SD в PSRAM. Вызывать из mainTask (begin / после save), не из async. */
static void netserverLoadPlaylistPsram() {
  netserverFreePlaylistPsram();
  const char* path = (config.getMode() == PM_SDCARD) ? PLAYLIST_SD_PATH : REAL_PLAYL;
  if (!lockLittleFS(2000)) {
    Serial.printf("[PL-PSRAM] нет мьютекса LittleFS для %s\n", path);
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    unlockLittleFS();
    Serial.printf("[PL-PSRAM] open fail: %s\n", path);
    return;
  }
  size_t size = f.size();
  if (size == 0 || size > PLAYLIST_PSRAM_CACHE_MAX) {
    f.close();
    unlockLittleFS();
    Serial.printf("[PL-PSRAM] пропуск %s size=%u\n", path, (unsigned)size);
    return;
  }
  uint8_t* buf = (uint8_t*)ps_malloc(size);
  if (!buf) {
    f.close();
    unlockLittleFS();
    Serial.printf("[PL-PSRAM] ps_malloc(%u) fail\n", (unsigned)size);
    return;
  }
  size_t got = f.read(buf, size);
  f.close();
  unlockLittleFS();
  if (got != size) {
    free(buf);
    Serial.printf("[PL-PSRAM] short read %u/%u\n", (unsigned)got, (unsigned)size);
    return;
  }
  g_playlistPsramBuf = buf;
  g_playlistPsramLen = size;
  Serial.printf("[PL-PSRAM] кэш %s (%u байт)\n", path, (unsigned)size);
}
#endif

#ifdef USE_DLNA
#include "../network/dlna_worker.h"
#include "../network/dlna_service.h"
#include "../network/dlna_index.h"
#endif
#if USE_BT_I2S
#include "bt_i2s.h"
#endif

#if DSP_MODEL==DSP_DUMMY
#define DUMMYDISPLAY
#endif

#ifdef USE_SD
#include "sdmanager.h"
#endif
#ifndef MIN_MALLOC
#define MIN_MALLOC 24112
#endif
#ifndef NSQ_SEND_DELAY
  //#define NSQ_SEND_DELAY       portMAX_DELAY
  // Для requestOnChange используем неблокирующую постановку в очередь.
  // Причина: при reconnect-шторме WebSocket очередь может кратковременно заполняться,
  // и блокировка на 50мс в xQueueSend() тормозила основной цикл/аудио.
  // Нулевой timeout безопаснее: событие лучше пропустить, чем подвесить поток воспроизведения.
  #define NSQ_SEND_DELAY       0
#endif
#ifndef NS_QUEUE_TICKS
  //#define NS_QUEUE_TICKS pdMS_TO_TICKS(2)
  #define NS_QUEUE_TICKS 0
#endif

#ifdef DEBUG_V
#define DBGVB( ... ) { char buf[200]; snprintf( buf, sizeof(buf), __VA_ARGS__ ); Serial.print("[DEBUG]\t"); Serial.println(buf); }
#else
#define DBGVB( ... )
#endif

//#define CORS_DEBUG //Enable CORS policy: 'Access-Control-Allow-Origin' (for testing)

NetServer netserver;

/* -----------------------------------------------------------------------------------------------
 * DEVICE_NAME → /variables.js (yoTitle). Логотип SVG в WebUI не трогаем.
 * Экранируем ' и \, чтобы имя с апострофом не ломало JS.
 * ----------------------------------------------------------------------------------------------- */
static void deviceNameEscapeForJs(char *dst, size_t dstSize) {
  size_t j = 0;
  const char *src = DEVICE_NAME;
  for (size_t i = 0; src[i] != '\0' && (j + 2) < dstSize; ++i) {
    const char c = src[i];
    if (c == '\r' || c == '\n') continue;
    if (c == '\'' || c == '\\') {
      dst[j++] = '\\';
    }
    dst[j++] = c;
  }
  dst[j] = '\0';
}

/* Собираем variables.js в отдельный буфер (nsBuf=BUFLEN слишком тесен с yoTitle). */
static void buildVariablesJs(char *out, size_t outSize) {
  char nameEsc[48];
  deviceNameEscapeForJs(nameEsc, sizeof(nameEsc));
  const char *formAction = (network.status == CONNECTED && !config.emptyFS) ? "webboard" : "";
  const char *playMode = (network.status == CONNECTED) ? "player" : "ap";
#ifdef USE_DLNA
  const int dlnaSupported = 1;
#else
  const int dlnaSupported = 0;
#endif
#if USE_BT_I2S
  const int btSupported = 1;
#else
  const int btSupported = 0;
#endif
  /* yoTitle — отображаемое имя устройства из myoptions; yoVersion — версия прошивки. */
  snprintf(out, outSize,
           "var yoTitle='%s';\nvar yoVersion='%s';\nvar formAction='%s';\nvar playMode='%s';\nvar dlnaSupported=%d;\nvar btSupported=%d;\n",
           nameEsc, YOVERSION, formAction, playMode, dlnaSupported, btSupported);
}

AsyncWebServer webserver(80);
AsyncWebSocket websocket("/ws");

// Simple per-client WebSocket rate limiter
#ifndef WS_RATE_LIMIT_WINDOW_MS
#define WS_RATE_LIMIT_WINDOW_MS 1000U
#endif
#ifndef WS_MAX_MSG_PER_WINDOW
// Было 10/с: на телефоне слайдер + heartbeat + bootstrap съедали лимит — команды молча отбрасывались.
#define WS_MAX_MSG_PER_WINDOW 48
#endif
#ifndef WS_TRACKED_CLIENTS
#define WS_TRACKED_CLIENTS 16
#endif
static uint32_t ws_client_ids[WS_TRACKED_CLIENTS];
static uint32_t ws_client_window_start[WS_TRACKED_CLIENTS];
static uint8_t  ws_client_count[WS_TRACKED_CLIENTS];

// Запрос отложенной перестройки плейлиста после сохранения.
// Храним только clientId, чтобы после завершения сборки индекса обновить плейлист у клиента.
struct PlaylistRebuildRequest {
  uint8_t clientId;
};
// Очередь длиной 1: нам важен только ПОСЛЕДНИЙ запрос на перестройку.
static QueueHandle_t g_playlistRebuildQueue = NULL;
// Флаг инициализации фоновой задачи перестройки индекса.
static bool g_playlistRebuildTaskReady = false;

// Фоновая задача перестройки индекса плейлиста.
// Вынесена из NetServer::processQueue(), чтобы тяжелая индексация не блокировала WS-цикл.
static void playlistRebuildTask(void* param) {
  (void)param;
  PlaylistRebuildRequest req;
  for (;;) {
    // Ждем новый запрос на перестройку индекса.
    if (xQueueReceive(g_playlistRebuildQueue, &req, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    bool rebuildDone = false;
    // В SD-режиме перестраиваем индекс SD-плейлиста.
    if (config.getMode() == PM_SDCARD) {
      if (lockLittleFS(5000)) {
        config.indexSDPlaylistFile();
        unlockLittleFS();
        rebuildDone = true;
      }
    } else {
      // В WEB/DLNA-режиме перестраиваем индекс текущего источника аккуратно и последовательно.
#ifdef USE_DLNA
      if (config.getPlaylistSource() == PL_SRC_DLNA) {
        dlnaIndex.rebuildIndexFromPlaylist();
        rebuildDone = true;
      } else
#endif
      {
        if (lockLittleFS(5000)) {
          config.indexPlaylist();
          unlockLittleFS();
          config.initPlaylist();
          rebuildDone = true;
        }
      }
    }
    // После успешной перестройки отправляем клиентам событие обновления файла плейлиста.
    if (rebuildDone) {
      netserver.requestOnChange(PLAYLIST_UPDATED, req.clientId);
    } else {
      // Если FS был занят и перестройка не прошла, планируем повтор с тем же clientId.
      // Перезаписываем единственный слот очереди, чтобы не накапливать дублей.
      xQueueOverwrite(g_playlistRebuildQueue, &req);
      vTaskDelay(pdMS_TO_TICKS(120));
    }
  }
}

// Инициализация фоновой задачи перестройки индекса (однократно).
static void ensurePlaylistRebuildTask() {
  if (g_playlistRebuildTaskReady) return;
  g_playlistRebuildQueue = xQueueCreate(1, sizeof(PlaylistRebuildRequest));
  if (!g_playlistRebuildQueue) return;
  if (xTaskCreatePinnedToCore(
        playlistRebuildTask,
        "PlRebuild",
        4096,
        NULL,
        1,
        NULL,
        1) == pdPASS) {
    g_playlistRebuildTaskReady = true;
  }
}

static bool ws_rate_allowed(uint32_t clientId) {
  unsigned long now = millis();
  // find entry
  int free_idx = -1;
  for (int i = 0; i < WS_TRACKED_CLIENTS; ++i) {
    if (ws_client_ids[i] == clientId) {
      // existing
      if (now - ws_client_window_start[i] > WS_RATE_LIMIT_WINDOW_MS) {
        ws_client_window_start[i] = now;
        ws_client_count[i] = 1;
        return true;
      } else {
        if (ws_client_count[i] < WS_MAX_MSG_PER_WINDOW) {
          ws_client_count[i]++;
          return true;
        }
        return false; // rate exceeded
      }
    }
    if (ws_client_ids[i] == 0 && free_idx == -1) free_idx = i;
  }
  // not found, allocate
  int idx = free_idx >= 0 ? free_idx : 0;
  ws_client_ids[idx] = clientId;
  ws_client_window_start[idx] = now;
  ws_client_count[idx] = 1;
  return true;
}

// Освобождаем слот rate-limit при disconnect — иначе «мёртвый» clientId блокирует новые вкладки.
static void ws_rate_clear_client(uint32_t clientId) {
  for (int i = 0; i < WS_TRACKED_CLIENTS; ++i) {
    if (ws_client_ids[i] == clientId) {
      ws_client_ids[i] = 0;
      ws_client_window_start[i] = 0;
      ws_client_count[i] = 0;
      return;
    }
  }
}

// Бэктрейс: processQueue → textAll → ~AsyncWebSocketBasicMessage → CORRUPT HEAP.
// AsyncTCP (_onAck/_onPoll) и loop() одновременно трогают очередь WS — сериализуем text/textAll.
static SemaphoreHandle_t g_wsSendMux = NULL;

static void wsSendLocked(uint8_t clientId, const char* msg) {
  if (!msg || !msg[0]) return;
  bool locked = false;
  // Мьютекс сериализует text/textAll, но при таймауте 150ms НЕ теряем WS (getindex/volume и т.д.).
  if (g_wsSendMux && xSemaphoreTake(g_wsSendMux, pdMS_TO_TICKS(150)) == pdTRUE) {
    locked = true;
  }
  if (websocket.count() > 0) {
    if (clientId == 0) websocket.textAll(msg);
    else websocket.text(clientId, msg);
  }
  if (locked) xSemaphoreGive(g_wsSendMux);
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
void handleIndex(AsyncWebServerRequest * request);
void handleNotFound(AsyncWebServerRequest * request);
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);

bool  shouldReboot  = false;
#ifdef MQTT_ROOT_TOPIC
//Ticker mqttplaylistticker;
bool  mqttplaylistblock = false;
void mqttplaylistSend() {
  mqttplaylistblock = true;
// mqttplaylistticker.detach();
  mqttPublishPlaylist();
  mqttplaylistblock = false;
}
#endif

char* updateError() {
  snprintf(netserver.nsBuf, sizeof(netserver.nsBuf), "Update failed with error (%d)<br /> %s", (int)Update.getError(), Update.errorString());
  return netserver.nsBuf;
}

// Только бейдж «[статус]» в meta без названия трека — не подменять на currentFactTrackLabelForApi() в /api/current-fact.
static bool metaTitleIsSystemBadgeOnly(const String& raw) {
  String s = raw;
  s.trim();
  const int n = (int)s.length();
  if (n < 3) return false;
  if (s.charAt(0) != '[') return false;
  const int rb = s.indexOf(']');
  if (rb <= 0) return false;
  for (int i = rb + 1; i < n; i++) {
    if (s.charAt(i) != ' ') return false;
  }
  return true;
}

#ifdef USE_DLNA
namespace {
// Краткое экранирование строки для безопасной вставки в JSON в кавычках (host, error, URL).
static void dlnaJsonEscape(const char* src, char* dst, size_t dstLen) {
  size_t j = 0;
  if (!src) {
    src = "";
  }
  for (size_t i = 0; src[i] != '\0' && j + 2 < dstLen; ++i) {
    if (src[i] == '\\' || src[i] == '\"') {
      dst[j++] = '\\';
      if (j + 1 >= dstLen) {
        break;
      }
    }
    dst[j++] = src[i];
  }
  dst[j] = '\0';
}
}  // namespace
#endif

// PSRAM-кэш .gz: отдаём только при магии 0x1F 0x8B, иначе браузер получает обрыв HTML/JS (пустые настройки).
static inline bool psramBufIsGzip(const uint8_t* buf, size_t size) {
  return buf && size >= 2 && buf[0] == 0x1f && buf[1] == 0x8b;
}

bool NetServer::begin(bool quiet) {
  if(network.status==SDREADY) return true;
  if(!quiet) Serial.print("##[BOOT]#\tnetserver.begin\t");
  importRequest = IMDONE;
  irRecordEnable = false;
  playerBufMax = psramInit()?300000:1600 * config.store.abuff;
  nsQueue = xQueueCreate( 96, sizeof( nsRequestParams_t ) );
  while(nsQueue==NULL){;}
  // Мьютекс WS-отправки до первого client connect.
  if (!g_wsSendMux) g_wsSendMux = xSemaphoreCreateMutex();
  // Поднимаем фоновую задачу перестройки плейлиста сразу после старта сетевого модуля.
  ensurePlaylistRebuildTask();

#ifdef USE_DLNA
  // фоновый worker DLNA (очередь задач вне колбэков AsyncWebServer/WebSocket).
  if (!dlnaWorker.begin() && !quiet) {
    Serial.println("##[BOOT]#\tDLNA: dlnaWorker.begin failed");
  }
#endif

  //webserver.addHandler(new SPIFFSEditor(LittleFS, "admin", "admin"));

// 1. Страница выбора файла (http://ip/upload)
webserver.on("/upload", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><head><meta charset='UTF-8'>";
    html += "<style>";
    html += "body { background-color: #000; color: #f1d56e; font-family: sans-serif; text-align: center; padding-top: 50px; }";
    html += "h2 { color: #f1d56e; text-transform: uppercase; letter-spacing: 2px; }";
    html += ".container { border-top: 1px solid #333; border-bottom: 1px solid #333; padding: 40px 20px; max-width: 500px; margin: 0 auto; }";
    html += "input[type='file'] { background: #f1d56e; color: #000; padding: 10px; border-radius: 20px; border: none; margin-bottom: 20px; cursor: pointer; }";
    html += "input[type='submit'] { background-color: #f1d56e; color: #000; border: none; padding: 15px 40px; border-radius: 30px; font-size: 18px; font-weight: bold; cursor: pointer; text-transform: uppercase; }";
    html += "input[type='submit']:hover { background-color: #fff; }";
    html += ".info { color: #ccc; margin-bottom: 20px; font-size: 14px; }";
    html += "</style></head><body>";
    
    html += "<h2>ёRadio - Cover Uploader</h2>";
    html += "<div class='container'>";
    html += "<p class='info'>Выберите .jpg файл обложки<br>(имя файла должно совпадать с названием станции)</p>";
    html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
    html += "<input type='file' name='update' accept='.jpg' id='file-input'><br>";
    html += "<input type='submit' value='UPLOAD COVER'>";
    html += "</form></div>";
    
    html += "</body></html>";
    request->send(200, "text/html", html);
});

// 2. Обработчик приема файла
    // для редактора (Импорт и Save)
  // Обработчик для редактора (Save) с возвратом в плеер
  /* Дублирующий обработчик /saveplaylist удален, так как он уже зарегистрирован выше с правильной логикой (Line ~150) */
  webserver.on("/", HTTP_ANY, handleIndex);
  webserver.onNotFound(handleNotFound);
  webserver.onFileUpload(handleUpload);

  auto sendIndexHtml = [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html);
    response->addHeader("Cache-Control", "max-age=31536000");
    request->send(response);
  };

  webserver.on("/settings.html", HTTP_ANY, sendIndexHtml);
  webserver.on("/update.html", HTTP_ANY, sendIndexHtml);
  webserver.on("/ir.html", HTTP_ANY, sendIndexHtml);

  webserver.on("/variables.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    /* yoTitle/yoVersion/playMode — зависят от DEVICE_NAME и состояния WiFi. */
    char varsJs[288];
    buildVariablesJs(varsJs, sizeof(varsJs));
    // no-cache: файл содержит playMode, который зависит от состояния WiFi в момент запроса.
    // Без этого заголовка браузер кэширует ответ с playMode='ap' (пока WiFi ещё не поднялся
    // после перезагрузки), и на следующей загрузке страницы continueLoading('ap') грузит
    // options.html вместо player.html → пустой UI без плейлиста, слайдера громкости и тегов.
    AsyncWebServerResponse *varResp = request->beginResponse(200, "application/javascript", varsJs);
    varResp->addHeader("Cache-Control", "no-cache, no-store");
    request->send(varResp);
  });

// 1. Отдача вспомогательных и основных JS/CSS файлов
// Для script.js и style.css добавлена поддержка PSRAM‑кэша:
// - при старте Config::init загружает *.gz версии файлов в PSRAM;
// - здесь, при запросе со стороны браузера, мы сначала пробуем отдать данные
// из PSRAM, и только при отсутствии кэша — читаем файлы из LittleFS.
// Это существенно снижает количество операций с файловой системой
// и уменьшает фрагментацию heap при открытии WebUI с мобильных устройств.

extern uint8_t* g_psramScriptJs;
extern size_t   g_psramScriptJsSize;
extern uint8_t* g_psramStyleCss;
extern size_t   g_psramStyleCssSize;
extern uint8_t* g_psramPlayerHtml;
extern size_t   g_psramPlayerHtmlSize;
extern uint8_t* g_psramCoverJs;
extern size_t   g_psramCoverJsSize;
extern uint8_t* g_psramFactsJs;
extern size_t   g_psramFactsJsSize;
extern uint8_t* g_psramLogoSvg;
extern size_t   g_psramLogoSvgSize;
extern uint8_t* g_psramThemeCss;
extern size_t   g_psramThemeCssSize;
extern uint8_t* g_psramOptionsHtml;
extern size_t   g_psramOptionsHtmlSize;
extern uint8_t* g_psramDragplJs;
extern size_t   g_psramDragplJsSize;

// script.js (основная логика WebUI)
webserver.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3) && YORADIO_CLASSIC_HTTP_HEAP_DIAG
    /* Classic diag: ветка отдачи + INTERNAL heap; send() только ставит ответ в очередь TCP. */
    Serial.printf("[HTTP] %s\n", request->url().c_str());
    netserverLogHeap8bit("script.js");
    netserverLogHeapInternal("script.js");
#endif
    // При наличии кэша в PSRAM — отдаём его (gzip-контент), только если это реальный gzip.
    if (psramBufIsGzip(g_psramScriptJs, g_psramScriptJsSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "application/javascript", g_psramScriptJs, g_psramScriptJsSize);
        response->addHeader("Content-Encoding", "gzip");      // содержимое уже сжато
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3) && YORADIO_CLASSIC_HTTP_HEAP_DIAG
        Serial.printf("[HTTP] script.js sent branch=gzip-psram size=%u\n", (unsigned)g_psramScriptJsSize);
        netserverLogHeapInternal("script.js-after-send");
#endif
        return;
    }

    // Если на диске есть несжатый script.js — отдаём его (страховка от битого .gz вручную).
    if (LittleFS.exists("/www/script.js")) {
        AsyncWebServerResponse *response =
            request->beginResponse(LittleFS, "/www/script.js", "application/javascript");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3) && YORADIO_CLASSIC_HTTP_HEAP_DIAG
        Serial.println("[HTTP] script.js sent branch=littlefs-plain");
        netserverLogHeapInternal("script.js-after-send");
#endif
    } else if (LittleFS.exists("/www/script.js.gz")) {
        AsyncWebServerResponse *response =
            request->beginResponse(LittleFS, "/www/script.js.gz", "application/javascript");
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3) && YORADIO_CLASSIC_HTTP_HEAP_DIAG
        Serial.println("[HTTP] script.js sent branch=littlefs-gz");
        netserverLogHeapInternal("script.js-after-send");
#endif
    } else {
        request->send(404, "text/plain", "script.js not found");
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3) && YORADIO_CLASSIC_HTTP_HEAP_DIAG
        Serial.println("[HTTP] script.js sent branch=404");
#endif
    }
});

// Дополнительный маршрут на случай прямого запроса .gz (некоторые клиенты могут кэшировать так)
webserver.on("/script.js.gz", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (psramBufIsGzip(g_psramScriptJs, g_psramScriptJsSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "application/javascript", g_psramScriptJs, g_psramScriptJsSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }
    if (LittleFS.exists("/www/script.js.gz")) {
        AsyncWebServerResponse *response =
            request->beginResponse(LittleFS, "/www/script.js.gz", "application/javascript");
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
    } else {
        request->send(404, "text/plain", "script.js.gz not found");
    }
});

// style.css (основные стили WebUI)
webserver.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (psramBufIsGzip(g_psramStyleCss, g_psramStyleCssSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "text/css", g_psramStyleCss, g_psramStyleCssSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }

    if (LittleFS.exists("/www/style.css.gz")) {
        AsyncWebServerResponse *response =
            request->beginResponse(LittleFS, "/www/style.css.gz", "text/css");
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
    } else if (LittleFS.exists("/www/style.css")) {
        AsyncWebServerResponse *response =
            request->beginResponse(LittleFS, "/www/style.css", "text/css");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
    } else {
        request->send(404, "text/plain", "style.css not found");
    }
});

webserver.on("/style.css.gz", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (psramBufIsGzip(g_psramStyleCss, g_psramStyleCssSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "text/css", g_psramStyleCss, g_psramStyleCssSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }

    if (LittleFS.exists("/www/style.css.gz")) {
        AsyncWebServerResponse *response =
            request->beginResponse(LittleFS, "/www/style.css.gz", "text/css");
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
    } else {
        request->send(404, "text/plain", "style.css.gz not found");
    }
});

// Прочие вспомогательные JS-файлы (обложки, факты, drag&drop)
// cover.js — отдаём из PSRAM если кэш доступен
webserver.on("/cover.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (psramBufIsGzip(g_psramCoverJs, g_psramCoverJsSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "application/javascript", g_psramCoverJs, g_psramCoverJsSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }
    if (LittleFS.exists("/www/cover.js.gz")) {
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/www/cover.js.gz", "application/javascript");
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    } else if (LittleFS.exists("/www/cover.js")) {
        request->send(LittleFS, "/www/cover.js", "application/javascript");
    } else {
        request->send(404, "text/plain", "cover.js not found");
    }
});

// facts.js — отдаём из PSRAM если кэш доступен
webserver.on("/facts.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (psramBufIsGzip(g_psramFactsJs, g_psramFactsJsSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "application/javascript", g_psramFactsJs, g_psramFactsJsSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }
    if (LittleFS.exists("/www/facts.js.gz")) {
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/www/facts.js.gz", "application/javascript");
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    } else if (LittleFS.exists("/www/facts.js")) {
        request->send(LittleFS, "/www/facts.js", "application/javascript");
    } else {
        request->send(404, "text/plain", "facts.js not found");
    }
});

// dragpl.js — только из PSRAM-кэша (как cover/facts). LittleFS.exists() в async_tcp → TWDT.
webserver.on("/dragpl.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (psramBufIsGzip(g_psramDragplJs, g_psramDragplJsSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "application/javascript", g_psramDragplJs, g_psramDragplJsSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }
    if (LittleFS.exists("/www/dragpl.js.gz")) {
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/www/dragpl.js.gz", "application/javascript");
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    } else if (LittleFS.exists("/www/dragpl.js")) {
        request->send(LittleFS, "/www/dragpl.js", "application/javascript");
    } else {
        request->send(404, "text/plain", "dragpl.js not found");
    }
});

// Унифицированный endpoint плейлиста: сам решает, отдавать SD или WEB.
// Вместо chunked response (многократные open/close LittleFS) —
// используем AsyncFileResponse, который открывает файл ОДИН раз
// и стримит из одного file handle. Это радикально ускоряет отдачу
// и не блокирует другие HTTP-запросы.
webserver.on("/api/playlist", HTTP_GET, [](AsyncWebServerRequest *request){
  // REAL_PLAYL учитывает PM_SDCARD и гибрид WEB/DLNA (PL_SRC_*), иначе WebUI всегда тянул бы только /data/playlist.csv.
  const char* playlistPath = (config.getMode() == PM_SDCARD) ? PLAYLIST_SD_PATH : REAL_PLAYL;

#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
  /* Classic: только PSRAM-буфер — без exists/open/beginResponse(LittleFS) в async_tcp. */
#if YORADIO_CLASSIC_HTTP_HEAP_DIAG
  Serial.printf("[HTTP] %s path=%s\n", request->url().c_str(), playlistPath);
  netserverLogHeap8bit("api/playlist");
#endif
  AsyncWebServerResponse *response;
  if (g_playlistPsramBuf && g_playlistPsramLen > 0) {
    response = request->beginResponse_P(200, "text/csv", g_playlistPsramBuf, g_playlistPsramLen);
  } else {
    response = request->beginResponse(200, "text/csv", "");
  }
  response->addHeader("Connection", "close");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
  return;
#else
  if (!LittleFS.exists(playlistPath)) {
    Serial.printf("[API] Плейлист не найден: %s\n", playlistPath);
    request->send(404, "text/plain", "playlist not found");
    return;
  }

  // beginResponse(FS, path) создаёт AsyncFileResponse с одним file handle
  AsyncWebServerResponse *response = request->beginResponse(LittleFS, playlistPath, "text/csv");
  response->addHeader("Connection", "close");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
#endif
});

#ifdef USE_DLNA
  // --- HTTP API DLNA (контракт DLNA_INTEGRATION_PLAN.md §6.1). Тяжёлые операции только через dlnaWorker. ---
  webserver.on("/dlna/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    DlnaWorkerStatus s = dlnaWorker.status();
    char errEsc[128], phaseEsc[32];
    dlnaJsonEscape(s.lastError, errEsc, sizeof(errEsc));
    dlnaJsonEscape(s.phase, phaseEsc, sizeof(phaseEsc));
    snprintf(netserver.nsBuf, sizeof(netserver.nsBuf),
             "{\"running\":%u,\"busy\":%u,\"processed\":%lu,\"lastJob\":%u,\"queue\":%u,"
             "\"ok\":%u,\"phase\":\"%s\",\"items\":%lu,\"folders\":%u,\"error\":\"%s\"}",
             s.running ? 1u : 0u,
             s.busy ? 1u : 0u,
             (unsigned long)s.processedJobs,
             (unsigned)s.lastJob,
             (unsigned)s.queueWaiting,
             s.lastOk ? 1u : 0u,
             phaseEsc,
             (unsigned long)s.itemsBuilt,
             (unsigned)s.containersScanned,
             errEsc);
    request->send(200, "application/json", netserver.nsBuf);
  });

  webserver.on("/dlna/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    // nsBuf всего BUFLEN(~170) — controlUrl+host не влезают, JSON обрывался на cfgHost
    // («192.16…»), WebUI ломал parse и зависал на «Подключение…». Строим ответ в String.
    (void)request;
    char hostE[96], ctrlE[200], rootE[96], cfgE[96];
    dlnaJsonEscape(dlnaService.host().c_str(), hostE, sizeof(hostE));
    dlnaJsonEscape(dlnaService.controlUrl().c_str(), ctrlE, sizeof(ctrlE));
    dlnaJsonEscape(dlnaService.rootObjectId().c_str(), rootE, sizeof(rootE));
    dlnaJsonEscape(config.store.dlnaHost, cfgE, sizeof(cfgE));
    bool playlistReady = false;
    if (LittleFS.exists(PLAYLIST_DLNA_PATH)) {
      File playlist = LittleFS.open(PLAYLIST_DLNA_PATH, "r");
      playlistReady = playlist && playlist.size() > 0;
      if (playlist) playlist.close();
    }
    String out = "{\"host\":\"";
    out += hostE;
    out += "\",\"controlUrl\":\"";
    out += ctrlE;
    out += "\",\"rootObjectId\":\"";
    out += rootE;
    out += "\",\"ready\":";
    out += dlnaService.ready() ? "1" : "0";
    out += ",\"playlistReady\":";
    out += playlistReady ? "1" : "0";
    out += ",\"cfgHost\":\"";
    out += cfgE;
    out += "\"}";
    request->send(200, "application/json", out);
  });

  webserver.on("/dlna/init", HTTP_GET, [](AsyncWebServerRequest *request) {
    String hostParam = request->hasParam("host") ? request->getParam("host")->value() : String("");
    hostParam.trim();
    DlnaWorkerJob job = {DLNA_JOB_INIT, "", 0};
    if (hostParam.length() > 0) {
      // Сохраняем host в EEPROM сразу при Init — чтобы после F5 поле подставлялось из /dlna/info.
      config.saveValue(config.store.dlnaHost, hostParam.c_str(), sizeof(config.store.dlnaHost));
      strlcpy(job.objectId, hostParam.c_str(), sizeof(job.objectId));
      job.objectId[sizeof(job.objectId) - 1] = '\0';
    }
    bool q = dlnaWorker.enqueue(job, 50);
    snprintf(netserver.nsBuf, sizeof(netserver.nsBuf), "{\"ok\":%u,\"queued\":%u}", q ? 1u : 0u, q ? 1u : 0u);
    request->send(200, "application/json", netserver.nsBuf);
  });

  webserver.on("/dlna/list", HTTP_GET, [](AsyncWebServerRequest *request) {
    // реальный SOAP Browse идёт только через worker (правило проекта — тяжёлые
    // DLNA-операции не выполняются в HTTP/WS callback). Здесь просто ставим задачу в очередь,
    // а сам список WebUI забирает следующим запросом /dlna/listresult после busy=0.
    String oid = request->hasParam("objectId") ? request->getParam("objectId")->value() : String("0");
    uint32_t start = 0;
    if (request->hasParam("start")) {
      long v = request->getParam("start")->value().toInt();
      if (v > 0) start = (uint32_t)v;
    }
    DlnaWorkerJob job = {DLNA_JOB_LIST, "", 0};
    oid.trim();
    if (oid.length() == 0) {
      strlcpy(job.objectId, "0", sizeof(job.objectId));
    } else {
      strlcpy(job.objectId, oid.c_str(), sizeof(job.objectId));
      job.objectId[sizeof(job.objectId) - 1] = '\0';
    }
    job.start = start;
    // Сбрасываем прошлый ready=1 ДО постановки в очередь — иначе poll может взять старый снимок,
    // а при ожидании безопасного окна (до 20 с) фронт с коротким таймаутом покажет ложный fail.
    dlnaWorker.invalidateListSnapshot();
    bool q = dlnaWorker.enqueue(job, 50);
    snprintf(netserver.nsBuf, sizeof(netserver.nsBuf),
             "{\"queued\":%u,\"objectId\":\"%.63s\",\"start\":%lu}",
             q ? 1u : 0u,
             job.objectId,
             (unsigned long)start);
    request->send(200, "application/json", netserver.nsBuf);
  });

  webserver.on("/dlna/listresult", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Отдаёт последний готовый результат Browse (см. /dlna/list выше). ready=0, пока ни один
    // DLNA_JOB_LIST ещё не завершился — тогда UI должен подождать и переспросить.
    (void)request;
    DlnaListSnapshot snap = dlnaWorker.lastListSnapshot();
    char oidEsc[80], errEsc[128];
    dlnaJsonEscape(snap.objectId.c_str(), oidEsc, sizeof(oidEsc));
    dlnaJsonEscape(snap.error.c_str(), errEsc, sizeof(errEsc));

    String out = "{\"ready\":";
    out += snap.ready ? "1" : "0";
    out += ",\"objectId\":\"";
    out += oidEsc;
    out += "\",\"error\":\"";
    out += errEsc;
    out += "\",\"items\":[";
    for (uint16_t i = 0; i < snap.count; i++) {
      char idEsc[80], titleEsc[160];
      dlnaJsonEscape(snap.items[i].id.c_str(), idEsc, sizeof(idEsc));
      dlnaJsonEscape(snap.items[i].title.c_str(), titleEsc, sizeof(titleEsc));
      if (i > 0) out += ",";
      out += "{\"id\":\"";
      out += idEsc;
      out += "\",\"title\":\"";
      out += titleEsc;
      out += "\",\"container\":";
      out += snap.items[i].isContainer ? "1" : "0";
      out += "}";
    }
    out += "]}";
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", out);
    request->send(response);
  });

  webserver.on("/dlna/build", HTTP_GET, [](AsyncWebServerRequest *request) {
    String oid = request->hasParam("objectId")
                     ? request->getParam("objectId")->value()
                     : dlnaService.rootObjectId();
    const bool recursive = request->hasParam("recursive") &&
                           request->getParam("recursive")->value().toInt() != 0;
    uint16_t lim = recursive ? 5000 : 200;
    if (request->hasParam("limit")) {
      int v = request->getParam("limit")->value().toInt();
      if (v > 0 && v <= 65535) {
        lim = (uint16_t)v;
      }
    }
    DlnaWorkerJob job = {DLNA_JOB_BUILD, "", lim};
    job.recursive = recursive;
    oid.trim();
    if (oid.length() == 0) {
      strlcpy(job.objectId, "0", sizeof(job.objectId));
    } else {
      strlcpy(job.objectId, oid.c_str(), sizeof(job.objectId));
      job.objectId[sizeof(job.objectId) - 1] = '\0';
    }
    bool q = dlnaWorker.enqueue(job, 50);
    snprintf(netserver.nsBuf, sizeof(netserver.nsBuf),
             "{\"queued\":%u,\"objectId\":\"%.63s\",\"limit\":%u,\"recursive\":%u}",
             q ? 1u : 0u,
             job.objectId,
             (unsigned)lim,
             recursive ? 1u : 0u);
    request->send(200, "application/json", netserver.nsBuf);
  });

  webserver.on("/dlna/append", HTTP_GET, [](AsyncWebServerRequest *request) {
    String oid = request->hasParam("objectId") ? request->getParam("objectId")->value() : String("0");
    uint16_t lim = 200;
    if (request->hasParam("limit")) {
      int v = request->getParam("limit")->value().toInt();
      if (v > 0 && v <= 65535) {
        lim = (uint16_t)v;
      }
    }
    DlnaWorkerJob job = {DLNA_JOB_APPEND, "", lim};
    oid.trim();
    if (oid.length() == 0) {
      strlcpy(job.objectId, "0", sizeof(job.objectId));
    } else {
      strlcpy(job.objectId, oid.c_str(), sizeof(job.objectId));
      job.objectId[sizeof(job.objectId) - 1] = '\0';
    }
    bool q = dlnaWorker.enqueue(job, 50);
    snprintf(netserver.nsBuf, sizeof(netserver.nsBuf),
             "{\"queued\":%u,\"objectId\":\"%.63s\",\"limit\":%u}",
             q ? 1u : 0u,
             job.objectId,
             (unsigned)lim);
    request->send(200, "application/json", netserver.nsBuf);
  });

  webserver.on("/playlist/dlna", HTTP_GET, [](AsyncWebServerRequest *request) {
    (void)request;
    netserver.requestOnChange(PLAYLISTSRC_DLNA, 0);
    request->send(200, "application/json", "{\"ok\":true,\"source\":\"dlna\"}");
  });

  webserver.on("/playlist/web", HTTP_GET, [](AsyncWebServerRequest *request) {
    (void)request;
    netserver.requestOnChange(PLAYLISTSRC_WEB, 0);
    request->send(200, "application/json", "{\"ok\":true,\"source\":\"web\"}");
  });
#endif

#include "player.h" // Чтобы сервер знал о классе Player
extern Player player; 
extern String currentCoverUrl;

// 2. API для обложки с ЗАПРЕТОМ КЭША (чтобы не жать F5)
webserver.on("/api/current-cover", HTTP_GET, [](AsyncWebServerRequest *request) {
    extern String currentCoverUrl; 
    extern Player player;          
    extern String metaTitle;       // Наша новая переменная из config.cpp
  
    // 1. Заголовок для обложки / поля title в JSON для cover.js (iTunes поиск).
    // Важно: как в 0.8.92 — приоритет у metaTitle. Иначе при station.title == const_PlConnect
    // подмена на строку «Подключение…» шла РАНЬШЕ metaTitle и ломала title в /api/current-cover.
    String displayTitle;
    if (metaTitle.length() > 0) {
      displayTitle = metaTitle;
    } else if (config.getMode() == PM_WEB && config.station.title[0] != '\0' &&
               strcmp(config.station.title, LANG::const_PlConnect) == 0) {
      displayTitle = String(LANG::const_PlConnect);
    } else {
      displayTitle = String(config.station.name);
    }
    String coverTitle = displayTitle;
    if (config.getMode() == PM_SDCARD) {
      // Удаляем расширение файла и числовые префиксы для лучшего поиска в iTunes.
      int dotPos = coverTitle.lastIndexOf('.');
      if (dotPos > 0 && (coverTitle.length() - dotPos) <= 5) {
        coverTitle = coverTitle.substring(0, dotPos);
      }
      int i = 0;
      while (i < coverTitle.length() && isdigit(coverTitle[i])) {
        i++;
      }
      if (i > 0 && i < coverTitle.length()) {
        while (i < coverTitle.length() && coverTitle[i] == ' ') {
          i++;
        }
        if (i < coverTitle.length() && (coverTitle[i] == '|' || coverTitle[i] == '-' || coverTitle[i] == '.' || coverTitle[i] == ':')) {
          i++;
          while (i < coverTitle.length() && coverTitle[i] == ' ') {
            i++;
          }
          if (i < coverTitle.length()) {
            coverTitle = coverTitle.substring(i);
          }
        }
      }
    }

    // 2. Определяем URL (если плеер стоп — принудительно лого)
    String effectiveUrl = currentCoverUrl; 
    if (player.status() == STOPPED) {
        effectiveUrl = "/logo.svg";
    }

    // Общий переключатель WebUI-обложек (для WEB и SD режимов).
    // Если отключено — всегда отдаём логотип.
    if (!WEBUI_COVERS_ENABLE) {
      effectiveUrl = "/logo.svg";
    }
    
    // В SD-режиме: если обложки из ID3 нет (logo.svg),
    // то для WebUI разрешаем поиск через iTunes — как в веб-радио.
    // Если ID3-обложка есть, currentCoverUrl уже указывает на неё и будет отдана.
    if (WEBUI_COVERS_ENABLE && config.getMode() == PM_SDCARD && effectiveUrl == "/logo.svg" && player.status() == PLAYING) {
      // Минимальный фильтр системных слов, чтобы не искать "timeout" в iTunes.
      String coverTitleLower = coverTitle;
      coverTitleLower.toLowerCase();
      // Добавлена блокировка API-запросов во время переключения режимов для предотвращения зависаний
      extern volatile bool g_modeSwitching;
      if (!g_modeSwitching && coverTitle.length() >= 3 && !coverTitle.startsWith("[") && coverTitleLower.indexOf("timeout") < 0) {
        effectiveUrl = "SEARCH_ITUNES";
      }
    }
    
    // Если планируется загрузка с iTunes, проверяем наличие в локальном кэше ПЕРЕД отправкой ответа.
    // Если в кэше уже есть — отдаём её URL сразу.
    // Важно: когда Display Cover OFF, ESP не должна тратить ресурсы на чтение/использование cache —
    // в этом режиме пусть браузер делает iTunes сам (см. cover.js).
    if (WEBUI_COVERS_ENABLE && effectiveUrl == "SEARCH_ITUNES" && isDisplayCoversEnabled()) {
        String key = String("ITUNES:") + coverTitle;
        // Используем статический кэш в config.cpp внутри coverCacheExists для минимизации VFS вызовов.
        extern bool coverCacheExists(const String& key);
        extern String coverCacheUrlForKey(const String& key);
        if (coverCacheExists(key)) {
            effectiveUrl = coverCacheUrlForKey(key);
        }
    }

    // TFT: показ обложек выключен — если у нас нет ни кэша, ни выбранной картинки,
    // веб может показать icy-logo по прямой ссылке. Если же currentCoverUrl уже на треке/кэше,
    // не перетираем его.
    if (WEBUI_COVERS_ENABLE && !isDisplayCoversEnabled() && effectiveUrl == "/logo.svg" && player.status() == PLAYING) {
      extern String streamCoverUrl;
      bool haveDirectIcyLogo = false;
      if (streamCoverUrl.length() > 8) {
        String sc = streamCoverUrl;
        sc.trim();
        if ((sc.startsWith("http://") || sc.startsWith("https://")) && isLikelyDirectCoverImageUrl(sc)) {
          effectiveUrl = sc;
          haveDirectIcyLogo = true;
        }
      }

      // Если прямого icy-logo нет — включаем клиентский iTunes через cover.js.
      // В этом режиме ESP не будет кэшировать обложки (CoverDL отключён).
      if (!haveDirectIcyLogo) {
        String coverTitleLower = coverTitle;
        coverTitleLower.toLowerCase();
        extern volatile bool g_modeSwitching;
        if (!g_modeSwitching && coverTitle.length() >= 3 && !coverTitle.startsWith("[") &&
            coverTitleLower.indexOf("timeout") < 0) {
          effectiveUrl = "SEARCH_ITUNES";
        }
      }
    }

    // Не отдаём в WebUI ссылку на HTML/главную как «обложку» (например https://roxyradio.hu/).
    // Если в effectiveUrl попал такой URL — даём клиенту iTunes при нормальном title (как у Roxy FLAC).
    if ((effectiveUrl.startsWith("http://") || effectiveUrl.startsWith("https://")) &&
        !isLikelyDirectCoverImageUrl(effectiveUrl)) {
      effectiveUrl = "/logo.svg";
      String coverTitleLower2 = coverTitle;
      coverTitleLower2.toLowerCase();
      extern volatile bool g_modeSwitching;
      if (WEBUI_COVERS_ENABLE && player.status() == PLAYING && !g_modeSwitching &&
          coverTitle.length() >= 3 && !coverTitle.startsWith("[") &&
          coverTitleLower2.indexOf("timeout") < 0) {
        effectiveUrl = "SEARCH_ITUNES";
      }
    }

    // 3. Формируем JSON (Важно: ТРИ поля %s для ТРЕХ переменных)
    // Ограничиваем длину полей (%.128s), иначе url+station+title по 250 байт переполняют json[512].
        char json[512];
        snprintf(json, sizeof(json), "{\"url\":\"%.128s\",\"station\":\"%.128s\",\"title\":\"%.128s\"}", 
          effectiveUrl.c_str(), 
          config.station.name,
          coverTitle.c_str()); // Теперь всё на своих местах
    
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    
    // Заголовки кэша
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    response->addHeader("Connection", "close");
    
    request->send(response);
});

// 3. API для фактов про композицию (аналогично cover)
webserver.on("/api/current-fact", HTTP_GET, [](AsyncWebServerRequest *request) {
    extern String metaTitle;
    extern String currentFact;
  extern TrackFacts trackFactsPlugin;
    
    String fact = currentFact.length() > 0 ? currentFact : "";
    String title;
    if (config.getMode() == PM_WEB && config.station.title[0] != '\0' &&
        strcmp(config.station.title, LANG::const_PlConnect) == 0) {
      title = String(LANG::const_PlConnect);
    } else if (metaTitle.length() > 0) {
      title = metaTitle;
      if (trackFactsPlugin.isEnabled() && !metaTitleIsSystemBadgeOnly(metaTitle) &&
          trackFactsPlugin.factLabelSyncedWithMeta(metaTitle)) {
        String cleanTitle = trackFactsPlugin.currentFactTrackLabelForApi();
        if (cleanTitle.length() > 0) title = cleanTitle;
      }
    } else {
      title = String(config.station.name);
    }
    String status = "";

    // Служебные сообщения не считаем фактом — отдаем их отдельно в поле status.
    if (fact.startsWith("Факты не найдены") || fact.startsWith("No facts found")) {
      status = fact;
      fact = "";
    }
    
    // Создаем JSON-ответ со списком фактов
    String jsonFacts = "[";
    if (fact.length() > 0) {
        int start = 0;
        int end = fact.indexOf("###");
        bool first = true;
        while (end != -1) {
            String f = fact.substring(start, end);
            f.trim();
            if (f.length() > 0) {
                if (!first) jsonFacts += ",";
                // Экранируем спецсимволы для корректного JSON
                f.replace("\\", "\\\\");
                f.replace("\"", "\\\"");
                f.replace("\n", "\\n");
                f.replace("\r", "\\r");
                jsonFacts += "\"" + f + "\"";
                first = false;
            }
            start = end + 3;
            end = fact.indexOf("###", start);
        }
        String f = fact.substring(start);
        f.trim();
        if (f.length() > 0) {
            if (!first) jsonFacts += ",";
            f.replace("\\", "\\\\");
            f.replace("\"", "\\\"");
            f.replace("\n", "\\n");
            f.replace("\r", "\\r");
            jsonFacts += "\"" + f + "\"";
        }
    }
    jsonFacts += "]";
    
    // Экранируем заголовок трека
    String escapedTitle = title;
    escapedTitle.replace("\\", "\\\\");
    escapedTitle.replace("\"", "\\\"");
    escapedTitle.replace("\n", " ");
    
    // В стопе не считаем запрос «висящим» — WebUI закрывает диалог и не крутит таймер ожидания.
    const bool playbackActive = (player.status() == PLAYING);
    // pending=1: активный запрос ИЛИ ещё не добрана серия facts-per-track (диалог должен продолжать опрос)
    const bool factPending =
        playbackActive &&
        (trackFactsPlugin.isRequestPendingForUi() || trackFactsPlugin.isMultiFactGatherOngoing());
    // requestPending=1: только реальный активный запрос к провайдеру ТЕКУЩЕГО трека.
    // После смены трека epoch++ — ⏳ гасим сразу, даже если старый FactsTask ещё доживает TLS.
    const bool requestPending =
        playbackActive &&
        trackFactsPlugin.isRequestPendingForUi();
    const bool manualMore =
        playbackActive && trackFactsPlugin.isEnabled() &&
        trackFactsPlugin.manualMoreFragmentsUi() && fact.length() > 0;
    const bool incompleteIterative =
        playbackActive && trackFactsPlugin.isEnabled() &&
        trackFactsPlugin.incompleteIterativeSeriesForApi();
    String json = "{\"facts\":" + jsonFacts + ",\"title\":\"" + escapedTitle + "\",\"pending\":" + String(factPending ? 1 : 0) + ",\"requestPending\":" + String(requestPending ? 1 : 0) + ",\"manualMore\":" + String(manualMore ? 1 : 0) + ",\"incompleteIterative\":" + String(incompleteIterative ? 1 : 0) + ",\"tfprovider\":" + String((int)config.store.trackFactsProvider) + ",\"factsAi\":" + String(trackFactsPlugin.factsContentFromAi() ? 1 : 0) + ",\"status\":\"" + status + "\"}";
    
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    
    // Заголовки для предотвращения кэширования в браузере
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    response->addHeader("Connection", "close");
    
    request->send(response);
});

// player.html — отдаём из PSRAM если кэш доступен (критично для мобильных)
webserver.on("/player.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (psramBufIsGzip(g_psramPlayerHtml, g_psramPlayerHtmlSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "text/html", g_psramPlayerHtml, g_psramPlayerHtmlSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }
    if (LittleFS.exists("/www/player.html.gz")) {
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/www/player.html.gz", "text/html");
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    } else if (LittleFS.exists("/www/player.html")) {
        request->send(LittleFS, "/www/player.html", "text/html");
    } else {
        request->send(404, "text/plain", "player.html not found");
    }
});

// options.html — отдаём из PSRAM если кэш доступен
webserver.on("/options.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (psramBufIsGzip(g_psramOptionsHtml, g_psramOptionsHtmlSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "text/html", g_psramOptionsHtml, g_psramOptionsHtmlSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }
    if (LittleFS.exists("/www/options.html.gz")) {
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/www/options.html.gz", "text/html");
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    } else if (LittleFS.exists("/www/options.html")) {
        request->send(LittleFS, "/www/options.html", "text/html");
    } else {
        request->send(404, "text/plain", "options.html not found");
    }
});

// logo.svg — отдаём из PSRAM если кэш доступен
webserver.on("/logo.svg", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (psramBufIsGzip(g_psramLogoSvg, g_psramLogoSvgSize)) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "image/svg+xml", g_psramLogoSvg, g_psramLogoSvgSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }
    if (LittleFS.exists("/www/logo.svg.gz")) {
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/www/logo.svg.gz", "image/svg+xml");
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    } else if (LittleFS.exists("/www/logo.svg")) {
        request->send(LittleFS, "/www/logo.svg", "image/svg+xml");
    } else {
        request->send(404, "text/plain", "logo.svg not found");
    }
});

// theme.css — отдаём из PSRAM если кэш доступен (НЕ gzip, файл хранится без сжатия)
webserver.on("/theme.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (g_psramThemeCss && g_psramThemeCssSize) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "text/css", g_psramThemeCss, g_psramThemeCssSize);
        // theme.css загружен без gzip (isGzip=false в initWebAssetsCache)
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }
    if (LittleFS.exists("/www/theme.css")) {
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/www/theme.css", "text/css");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
    } else {
        request->send(404, "text/plain", "theme.css not found");
    }
});

// 3. Статика
// === КРИТИЧНО: ПОРЯДОК РЕГИСТРАЦИИ serveStatic ОЧЕНЬ ВАЖЕН! ===
// AsyncWebServer сравнивает URL по префиксу и выбирает ПЕРВЫЙ подходящий.
// Поэтому более специфичные пути (/sc/, /data/) должны быть ПЕРЕД корневым (/).
// Если поставить "/" первым, то запросы на "/sc/..." будут обрабатываться как "/"
// и искать файлы в /www/sc/... (что неправильно).
// ПРАВИЛЬНЫЙ ПОРЯДОК:
// 1. /sc/    -> /station_covers/  (обложки альбомов)
// 2. /data/  -> /data/            (плейлисты, настройки)
// 3. /       -> /www/             (веб-интерфейс, должен быть ПОСЛЕДНИМ)
// Экспорт плейлиста (как yoRadio-main / improve4): URL /data/playlist.csv,
// отдача chunkedHtmlPage + application/octet-stream. ДО serveStatic("/data/"),
// иначе статика может подставить .gz и Content-Encoding → ошибка в браузере.
webserver.on(PLAYLIST_PATH, HTTP_GET, [](AsyncWebServerRequest *request) {
#ifdef MQTT_ROOT_TOPIC
    while (mqttplaylistblock) vTaskDelay(5);
#endif
    if (config.getMode() == PM_SDCARD) {
        netserver.chunkedHtmlPage("application/octet-stream", request, PLAYLIST_SD_PATH);
    } else {
        netserver.chunkedHtmlPage("application/octet-stream", request, PLAYLIST_PATH);
    }
});

webserver.serveStatic("/sc/", LittleFS, "/station_covers/").setCacheControl("max-age=3600");
webserver.serveStatic("/data/", LittleFS, "/data/").setCacheControl("max-age=3600");
webserver.serveStatic("/", LittleFS, "/www/").setCacheControl("max-age=31536000");

// Обработчик для редактора плейлиста (Экспорт / Загрузка в редактор)
webserver.on("/saveplaylist", HTTP_POST, 
    [](AsyncWebServerRequest *request) {
      request->send(200, "text/plain", "OK");
    },
    NULL, 
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!index) {
          // Открываем файл. total покажет нам полный размер плейлиста
          request->_tempFile = LittleFS.open("/data/playlist.csv", "w");
          Serial.printf("[SYSTEM] Saving Playlist... Total size: %u bytes\n", total);
      }
      
      if (request->_tempFile) {
          request->_tempFile.write(data, len);
      }
      
      if (index + len == total) {
          if (request->_tempFile) request->_tempFile.close();
          // Принудительная индексация
          cmd.exec("indexplaylist", ""); 
          Serial.println("[SYSTEM] Playlist saved and re-indexed!");
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
          /* Classic: только флаг — сам open/read в NetServer::loop (mainTask). */
          g_playlistPsramReloadReq = true;
#endif
      }
    }
  );

#ifdef CORS_DEBUG
  DefaultHeaders::Instance().addHeader(F("Access-Control-Allow-Origin"), F("*"));
  DefaultHeaders::Instance().addHeader(F("Access-Control-Allow-Headers"), F("content-type"));
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
  /* Classic: кэш плейлиста до старта HTTP — VFS здесь в mainTask, не в async. */
  netserverLoadPlaylistPsram();
#endif
  webserver.begin();
  //if(strlen(config.store.mdnsname)>0)
  // MDNS.begin(config.store.mdnsname);
  websocket.onEvent(onWsEvent);
  webserver.addHandler(&websocket);
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3) && YORADIO_CLASSIC_HTTP_HEAP_DIAG
  /* Classic diag: снимок heap сразу после старта HTTP. */
  netserverLogHeap8bit("netserver.begin");
#endif
  if(!quiet) Serial.println("done");
  return true;
}

size_t NetServer::chunkedHtmlPageCallback(uint8_t* buffer, size_t maxLen, size_t index){
  File requiredfile;
  // Все файлы (включая SD-плейлист) теперь на LittleFS.
  // Ранее для SD-плейлиста использовался SDPLFS() (т.е. чтение с SD-карты),
  // что приводило к SPI-конкуренции с аудиопотоком и таймаутам в браузере.
  requiredfile = LittleFS.open(netserver.chunkedPathBuffer, "r");
  if (!requiredfile) return 0;
  size_t filesize = requiredfile.size();
  size_t needread = filesize - index;
  if (!needread) {
    requiredfile.close();
    display.unlock();
    return 0;
  }
  #ifdef MAX_PL_READ_BYTES
    if(maxLen>MAX_PL_READ_BYTES) maxLen=MAX_PL_READ_BYTES;
  #endif
  size_t canread = (needread > maxLen) ? maxLen : needread;
  DBGVB("[%s] seek to %d in %s and read %d bytes with maxLen=%d", __func__, index, netserver.chunkedPathBuffer, canread, maxLen);
  //netserver.loop();
  requiredfile.seek(index, SeekSet);
  requiredfile.read(buffer, canread);
  index += canread;
  if (requiredfile) requiredfile.close();
  return canread;
}

void NetServer::chunkedHtmlPage(const String& contentType, AsyncWebServerRequest *request, const char * path) {
  memset(chunkedPathBuffer, 0, sizeof(chunkedPathBuffer));
  strlcpy(chunkedPathBuffer, path, sizeof(chunkedPathBuffer)-1);
  AsyncWebServerResponse *response;
  #ifndef NETSERVER_LOOP1
  display.lock();
  #endif
  response = request->beginChunkedResponse(contentType, chunkedHtmlPageCallback);
  // HTML-оболочки (player/options): без годового кэша — иначе после обновления прошивки нужен Ctrl+F5.
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

#ifndef DSP_NOT_FLIPPED
  #define DSP_CAN_FLIPPED true
#else
  #define DSP_CAN_FLIPPED false
#endif
#if !defined(HIDE_WEATHER) && (!defined(DUMMYDISPLAY) && !defined(USE_NEXTION))
  #define SHOW_WEATHER  true
#else
  #define SHOW_WEATHER  false
#endif

const char *getFormat(BitrateFormat _format) {
  switch (_format) {
    case BF_MP3:  return "MP3";
    case BF_AAC:  return "AAC";
    case BF_FLAC: return "FLC";
    case BF_OGG:  return "OGG";
    case BF_WAV:  return "WAV";
    default:      return "bitrate";
  }
}

void NetServer::processQueue(){
  if(nsQueue==NULL) return;
  // cleanupClients() перенесён в loop() с интервалом 2сек
  // Раньше здесь жёстко стоял лимит 12 сообщений за один вызов.
  // В SD‑режиме при активном WebUI очередь могла копиться быстрее, чем
  // обрабатываться, и из‑за дополнительного ограничения в requestOnChange()
  // обновления для браузера просто переставали попадать в очередь —
  // визуально «умирал» Web, хотя система продолжала работать.
  // Новый подход:
  // - лимит делаем более гибким и выше (48);
  // - при желании можно будет дополнительно адаптировать его под Heap/режим.
  int processed = 0;
  int maxPerCycle = 48;
  nsRequestParams_t request;
  while(processed < maxPerCycle && xQueueReceive(nsQueue, &request, NS_QUEUE_TICKS)){
    processed++;
    uint8_t clientId = request.clientId;
    wsBuf[0]='\0';
    switch (request.type) {
      case PLAYLIST:        getPlaylist(clientId); break;
      // Только обновить плейлист у клиентов, БЕЗ перестройки индекса. Вызывается из FavUpdate после setFavorite:
      // позиции строк в плейлисте не меняются (меняется только 5-я колонка 0/1), indexSDPlaylistFile() не нужен
      // и блокировал main loop на десятки секунд при 3000+ треках → веб «умирал», FREEZE-RECOVERY.
      case PLAYLIST_UPDATED: getPlaylist(clientId); break;
      case PLAYLISTSAVED:   {
        // Полное сохранение плейлиста: ставим задачу индексации в фоновую очередь.
        // Это критично для больших плейлистов — иначе processQueue() блокируется на секунды/минуты.
        if (g_playlistRebuildTaskReady && g_playlistRebuildQueue != NULL) {
          PlaylistRebuildRequest req;
          req.clientId = clientId;
          xQueueOverwrite(g_playlistRebuildQueue, &req);
        } else {
          // Fallback: если задача по какой-то причине не поднялась, хотя бы обновим ссылку на файл.
          getPlaylist(clientId);
        }
        break;
      }
      case GETACTIVE: {
          // Сборка списка групп настроек сразу в wsBuf (BUFLEN*2 = 340 байт).
          // Раньше использовался nsBuf (170 байт): при DSP_ST7789_170 (не DUMMY) в ответ попадают
          // group_vu, group_display, group_tft, group_brightness, group_weather и др. — суммарно ~190 байт.
          // nsBuf переполнялся, JSON обрезался, WebUI не мог распарсить act → пустая страница настроек.
          // С Dummy дисплеем групп меньше (DUMMYDISPLAY, DSP_NOT_FLIPPED) — в 170 байт влезало, поэтому «работало».
          bool dbgact = false, nxtn=false;
          #define APPEND_ACT(name) snprintf(wsBuf + strlen(wsBuf), sizeof(wsBuf) - strlen(wsBuf), "\"%s\",", name);
          snprintf(wsBuf, sizeof(wsBuf), "{\"act\":[");
          APPEND_ACT("group_wifi");
          // Все секции настроек — и в STA (CONNECTED), и в точке доступа (SOFT_AP).
          if (network.status == CONNECTED || network.status == SOFT_AP) {
            APPEND_ACT("group_system");
            if (YO_BACKLIGHT_CTRL || DSP_CAN_FLIPPED || DSP_MODEL == DSP_NOKIA5110 || dbgact)    APPEND_ACT("group_display");
          #ifdef USE_NEXTION
            APPEND_ACT("group_nextion");
            if (!SHOW_WEATHER || dbgact)                        APPEND_ACT("group_weather");
            nxtn=true;
          #endif
                                                              #if defined(LCD_I2C) || defined(DSP_OLED)
                                                                APPEND_ACT("group_oled");
                                                              #endif
                                                              #if !defined(HIDE_VU) && !defined(DUMMYDISPLAY)
                                                                APPEND_ACT("group_vu");
                                                              #endif
            if (YO_BACKLIGHT_CTRL || nxtn || dbgact)        APPEND_ACT("group_brightness");
            if (DSP_CAN_FLIPPED || dbgact)                      APPEND_ACT("group_tft");
            if (TS_MODEL != TS_MODEL_UNDEFINED || dbgact)       APPEND_ACT("group_touch");
            if (DSP_MODEL == DSP_NOKIA5110)                     APPEND_ACT("group_nokia");
                                                                APPEND_ACT("group_timezone");
            if (SHOW_WEATHER || dbgact)                         APPEND_ACT("group_weather");
                                                                APPEND_ACT("group_controls");
            if (ENC_BTNL != 255 || ENC2_BTNL != 255 || dbgact)  APPEND_ACT("group_encoder");
            if (IR_PIN != 255 || dbgact)                        APPEND_ACT("group_ir");
            if (!psramInit())                                   APPEND_ACT("group_buffer");
                                                              #if RTCSUPPORTED
                                                                APPEND_ACT("group_rtc");
                                                              #else
                                                                APPEND_ACT("group_wortc");
                                                              #endif
          }
          #undef APPEND_ACT
          // Убрать хвостовую запятую и закрыть JSON-массив act.
          size_t len = strlen(wsBuf);
          if (len > 0 && wsBuf[len - 1] == ',') wsBuf[len - 1] = '\0';
          snprintf(wsBuf + strlen(wsBuf), sizeof(wsBuf) - strlen(wsBuf), "]}");
          break;
        }
      case GETINDEX:      {
          // Сначала STATION и MODE, затем TITLE: иначе WebUI получает meta раньше,
          // чем playerwrap перейдёт в playing, и клиентский guard отбрасывает тег
          // до следующей смены трека в эфире.
          requestOnChange(STATION, clientId);
          requestOnChange(MODE, clientId);
          requestOnChange(TITLE, clientId);
          requestOnChange(VOLUME, clientId); 
          requestOnChange(EQUALIZER, clientId); 
          requestOnChange(BALANCE, clientId); 
          requestOnChange(BITRATE, clientId); 
          requestOnChange(SDINIT, clientId);
          requestOnChange(GETPLAYERMODE, clientId); 
          if (config.getMode()==PM_SDCARD) { requestOnChange(SDPOS, clientId); requestOnChange(SDLEN, clientId); requestOnChange(SDSNUFFLE, clientId); } 
          // Таймер сна: pushState при старте мог уйти в пустой эфир — досылаем подключившемуся клиенту.
          sleepTimerPlugin.pushState(clientId);
          // Важно не завершать весь processQueue() здесь:
          // после GETINDEX в очереди уже могут лежать другие события для UI,
          // и их нужно обработать в этом же проходе цикла.
          break;
        }
      // webcpu — прошивка собрана с WEBUI_CPU_BAR_ENABLE (показ переключателя в настройках).
      // wci — пользовательское «CPU Info» (полоска в WebUI), поле config.store.webCpuInfo.
      case GETSYSTEM:     snprintf (wsBuf, sizeof(wsBuf), "{\"sst\":%d,\"aif\":%d,\"vu\":%d,\"softr\":%d,\"vut\":%d,\"mdns\":\"%s\",\"ipaddr\":\"%s\", \"abuff\": %d, \"telnet\": %d, \"watchdog\": %d, \"webcpu\": %d, \"wci\": %d }", 
                                  config.store.smartstart != 2, 
                                  config.store.audioinfo, 
                                  config.store.vumeter, 
                                  config.store.softapdelay,
                                  config.vuThreshold,
                                  config.store.mdnsname,
                                  config.ipToStr(WiFi.localIP()),
                                  config.store.abuff,
                                  config.store.telnet,
                                  config.store.watchdog,
                                  WEBUI_CPU_BAR_ENABLE ? 1 : 0,
                                  config.store.webCpuInfo ? 1 : 0); 
                                  break;
      case GETSCREEN:     snprintf (wsBuf, sizeof(wsBuf),
                                  /* Добавлены поля fadr/fmin/fadt для BacklightDown */
                                  "{\"flip\":%d,\"inv\":%d,\"nump\":%d,\"coven\":%d,\"covopt\":%d,\"tsf\":%d,\"tsd\":%d,\"dspon\":%d,\"br\":%d,\"con\":%d,\"scre\":%d,\"scrt\":%d,\"scrb\":%d,\"scrpe\":%d,\"scrpt\":%d,\"scrpb\":%d,\"fadr\":%d,\"fmin\":%d,\"fadt\":%d}",
                                  config.store.flipscreen,
                                  config.store.invertdisplay,
                                  config.store.numplaylist,
                                  isDisplayCoversEnabled(),
                                  isDisplayCoversAllowedByBuild(),
                                  config.store.fliptouch,
                                  config.store.dbgtouch,
                                  config.store.dspon,
                                  config.store.brightness,
                                  config.store.contrast,
                                  config.store.screensaverEnabled,
                                  config.store.screensaverTimeout,
                                  config.store.screensaverBlank,
                                  config.store.screensaverPlayingEnabled,
                                  config.store.screensaverPlayingTimeout,
                                  config.store.screensaverPlayingBlank,
                                  /* BacklightDown: fadr=faderon, fmin=fadermin(%), fadt=fadertimeout(сек) */
                                  (int)config.store.faderon,
                                  (int)config.store.fadermin,
                                  (int)config.store.fadertimeout);
                                  break;
      case GETTIMEZONE:   snprintf (wsBuf, sizeof(wsBuf), "{\"tzh\":%d,\"tzm\":%d,\"sntp1\":\"%s\",\"sntp2\":\"%s\", \"timeint\":%d,\"timeintrtc\":%d}", 
                                  config.store.tzHour, 
                                  config.store.tzMin, 
                                  config.store.sntp1, 
                                  config.store.sntp2,
                                  config.store.timeSyncInterval,
                                  config.store.timeSyncIntervalRTC); 
                                  break;
      case GETWEATHER:    snprintf (wsBuf, sizeof(wsBuf), "{\"wen\":%d,\"wlat\":\"%s\",\"wlon\":\"%s\",\"wkey\":\"%s\",\"wint\":%d}", 
                                  config.store.showweather, 
                                  config.store.weatherlat, 
                                  config.store.weatherlon, 
                                  config.store.weatherkey,
                                  config.store.weatherSyncInterval); 
                                  break;
      case GETCONTROLS:   snprintf (wsBuf, sizeof(wsBuf), "{\"vols\":%d,\"enca\":%d,\"irtl\":%d,\"skipup\":%d,\"favonly\":%d}", 
                                  config.store.volsteps, 
                                  config.store.encacc, 
                                  config.store.irtlp,
                                  config.store.skipPlaylistUpDown,
                                  config.store.favOnly); 
                                  break;
      case GETTRACKFACTS: {
                                  // Mask API key for security
                                  String keyHolder = (strlen(config.store.geminiApiKey) > 0) ? "********" : "";
                                  // Буфер под три proxy URL + флаги UI + factsTlsRisk.
                                  char tfBuf[640];
                                  snprintf(tfBuf, sizeof(tfBuf),
                                    "{\"tfen\":%d,\"tfkey\":\"%s\",\"tflang\":%d,\"tfcount\":%d,\"tfprovider\":%d,"
                                    "\"tfproxyg\":\"%.63s\",\"tfproxyq\":\"%.63s\",\"tfproxyd\":\"%.63s\","
                                    "\"tfpuiG\":%d,\"tfpuiQ\":%d,\"tfpuiD\":%d,\"factsTlsRisk\":%d}",
                                  config.store.trackFactsEnabled ? 1 : 0,
                                  keyHolder.c_str(),
                                  config.store.trackFactsLang,
                                  config.store.trackFactsCount,
                                  config.store.trackFactsProvider,
                                  config.store.trackFactsProxyGemini,
                                  config.store.trackFactsProxyGroq,
                                  config.store.trackFactsProxyDeepSeek,
                                  TRACKFACTS_PROXY_UI_GEMINI,
                                  TRACKFACTS_PROXY_UI_GROQ,
                                  TRACKFACTS_PROXY_UI_DEEPSEEK,
                                  trackFactsHttpsNeedsRiskDialog() ? 1 : 0);
                                  if (websocket.count() > 0) {
                                    if (clientId == 0) websocket.textAll(tfBuf);
                                    else websocket.text(clientId, tfBuf);
                                  }
                                  }
                                  break;
      case DSPON:         snprintf (wsBuf, sizeof(wsBuf), "{\"dspontrue\":%d}", 1); break;
      case STATION:       requestOnChange(STATIONNAME, clientId); requestOnChange(ITEM, clientId); break;
      case STATIONNAME: {
        // Во время индексации SD не показываем старую веб-станцию — только статус индексации.
        if (config.sdIndexing) {
          snprintf(wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"nameset\", \"value\": \"%s\"}]}", "Индексация SD...");
        } else if (config.getMode() == PM_SDCARD) {
          // Верхняя строка WebUI = то же, что TFT: config.station.name
          // (папка / Album / Artist из ID3-конвейера). Раньше слали CSV-имя файла
          // (*.mp3) → дубль с meta «Artist - Title». Fallback на плейлист/"SD" только если name пуст.
          const char* nm = config.station.name;
          if (!nm || !nm[0]) {
            uint16_t idx = config.lastStation();
            uint16_t plen = config.playlistLength();
            nm = (plen > 0 && idx >= 1 && idx <= plen) ? config.stationByNum(idx) : "SD";
          }
          snprintf(wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"nameset\", \"value\": \"%.200s\"}]}", nm ? nm : "SD");
        } else {
          snprintf(wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"nameset\", \"value\": \"%s\"}]}", config.station.name);
        }
        break;
      }
      case ITEM:          snprintf (wsBuf, sizeof(wsBuf), "{\"current\": %d}", config.lastStation()); break;
      case TITLE: {
        // Во время индексации SD не показываем тег предыдущей веб-станции — только статус индексации.
        extern String metaTitle;
        if (config.sdIndexing) {
          snprintf(wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"meta\", \"value\": \"%s\"}]}", "Индексация SD...");
        } else if (config.getMode() == PM_SDCARD) {
          // В SD-режиме не шлём старый meta с веб-радио: текущий трек из metaTitle или "Загрузка плейлиста...".
          const char* tit = (metaTitle.length() > 0) ? metaTitle.c_str() : "Загрузка плейлиста...";
          snprintf(wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"meta\", \"value\": \"%.200s\"}]}", tit);
        } else {
          // %.200s — title до BUFLEN не должен раздувать wsBuf (BUFLEN*2) и портить кучу.
          snprintf(wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"meta\", \"value\": \"%.200s\"}]}", config.station.title);
        }
        break;
      }
      // Remap 0-254 (Internal) to 0-100 (WebUI)
      case VOLUME:        snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"volume\", \"value\": %d}]}", (config.store.volume * 100) / 254); telnet.printf("##CLI.VOL#: %d\n", config.store.volume); break;
      // Добавлена проверка playerBufMax > 0 для защиты от деления на ноль
      // Процент буфера считаем от реального размера (player.getInBufferSize()), как в [DIAG], чтобы веб/дисплей совпадали с логом.
      case NRSSI:         { uint32_t total = player.getInBufferSize(); int heapPct = (player.isRunning() && config.store.audioinfo && total > 0) ? (int)(100*player.inBufferFilled()/total) : 0;
#if WEBUI_CPU_BAR_ENABLE
                                  // Полоску CPU шлём только если и сборка, и пользователь включил «CPU Info» (экономия и скрытие визуала).
                                  if (config.store.webCpuInfo) {
                                    const int cpuPct = webui_freertos_cpu_percent();
                                    snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"rssi\", \"value\": %d}, {\"id\":\"heap\", \"value\": %d}, {\"id\":\"cpubar\", \"value\": %d}]}", rssi, heapPct, cpuPct);
                                  } else {
                                    snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"rssi\", \"value\": %d}, {\"id\":\"heap\", \"value\": %d}]}", rssi, heapPct);
                                  }
#else
                                  snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"rssi\", \"value\": %d}, {\"id\":\"heap\", \"value\": %d}]}", rssi, heapPct);
#endif
                                  } break;
      case SDPOS:         {
                                  // Вызывать getFilePos/getFileSize только в SD-режиме — иначе "audio is not a file" при переходе SD->WEB
                                  // (запрос мог остаться в очереди до смены режима).
                                  bool running = (config.getMode() == PM_SDCARD) && player.isRunning();
                                  snprintf (wsBuf, sizeof(wsBuf), "{\"sdpos\": %lu,\"sdend\": %lu,\"sdtpos\": %lu,\"sdtend\": %lu}", 
                                  running ? player.getFilePos() : 0u, 
                                  running ? player.getFileSize() : 0u, 
                                  running ? player.getAudioCurrentTime() : 0u, 
                                  running ? player.getAudioFileDuration() : 0u); 
                                  }
                                  break;
      case SDLEN:         snprintf (wsBuf, sizeof(wsBuf), "{\"sdmin\": %lu,\"sdmax\": %lu}", player.sd_min, player.sd_max); break;
      case SDSNUFFLE:     snprintf (wsBuf, sizeof(wsBuf), "{\"snuffle\": %d}", config.store.sdsnuffle); break;
      case BITRATE:       snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"bitrate\", \"value\": %d}, {\"id\": \"fmt\", \"value\": \"%s\"}]}", config.station.bitrate, getFormat(config.configFmt)); break;
      case MODE: {
          bool uiPlaying = (player.status() == PLAYING);
#if USE_BT_I2S
          if (config.getMode() == PM_BT) uiPlaying = btI2s.isBtPlaying();
#endif
          snprintf(wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"playerwrap\", \"value\": \"%s\"}]}", uiPlaying ? "playing" : "stopped");
          telnet.info();
          break;
        }
      case EQUALIZER:     snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"bass\", \"value\": %d}, {\"id\": \"middle\", \"value\": %d}, {\"id\": \"trebble\", \"value\": %d}]}", config.store.bass, config.store.middle, config.store.trebble); break;
      case BALANCE:       snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\": \"balance\", \"value\": %d}]}", config.store.balance); break;
#ifdef USE_SD
      case SDINIT:        snprintf (wsBuf, sizeof(wsBuf), "{\"sdinit\": %d}", config.isSdAvailableForMode() ? 1 : 0); break;
#else
      case SDINIT:        snprintf (wsBuf, sizeof(wsBuf), "{\"sdinit\": 0}"); break;
#endif
      case GETPLAYERMODE: {
          const char* pm = "modeweb";
          if (config.getMode() == PM_SDCARD) {
            pm = "modesd";
          }
#if USE_BT_I2S
          else if (config.getMode() == PM_BT) {
            pm = "modebt";
          }
#endif
#ifdef USE_DLNA
          else if (config.getPlaylistSource() == PL_SRC_DLNA) {
            pm = "modedlna";
          }
          const bool dlnaReady = config.dlnaPlaylistReady();
#else
          const bool dlnaReady = false;
#endif
#if USE_BT_I2S
          const bool digVol = (config.getMode() != PM_BT);
          const bool digEq = (config.getMode() != PM_BT);
          const bool btPlaying = (config.getMode() == PM_BT) && btI2s.isBtPlaying();
          snprintf(wsBuf, sizeof(wsBuf),
                   "{\"playermode\":\"%s\",\"supportsDigitalVolume\":%d,\"supportsEq\":%d,\"dlnaReady\":%d,\"btPlaying\":%d}",
                   pm, digVol ? 1 : 0, digEq ? 1 : 0, dlnaReady ? 1 : 0, btPlaying ? 1 : 0);
#else
          snprintf(wsBuf, sizeof(wsBuf), "{\"playermode\":\"%s\",\"dlnaReady\":%d}",
                   pm, dlnaReady ? 1 : 0);
#endif
          break;
        }
      case SDINDEXING:    snprintf (wsBuf, sizeof(wsBuf), "{\"sdindexing\": %d}", config.sdIndexing ? 1 : 0); break;
#ifdef USE_DLNA
      case PLAYLISTSRC_DLNA: {
          if (config.getMode() == PM_SDCARD) {
            break;
          }
          config.setPlaylistSource(PL_SRC_DLNA);
          dlnaService.setHost(String(config.store.dlnaHost));
          config.initPlaylist();
          dlnaIndex.rebuildIndexFromPlaylist();
          if (player.status() == PLAYING) {
            player.sendCommand({PR_PLAY, (int)config.lastStation()});
          } else {
            config.loadStation(config.lastStation());
          }
          requestOnChange(PLAYLIST, clientId);
          requestOnChange(STATIONNAME, clientId);
          requestOnChange(STATION, clientId);
          requestOnChange(ITEM, clientId);
          requestOnChange(TITLE, clientId);
          requestOnChange(GETPLAYERMODE, clientId);
          break;
        }
      case PLAYLISTSRC_WEB: {
          if (config.getMode() == PM_SDCARD) {
            break;
          }
          config.setPlaylistSource(PL_SRC_WEB);
          config.initPlaylist();
          if (player.status() == PLAYING) {
            player.sendCommand({PR_PLAY, (int)config.lastStation()});
          } else {
            config.loadStation(config.lastStation());
          }
          requestOnChange(PLAYLIST, clientId);
          requestOnChange(STATIONNAME, clientId);
          requestOnChange(STATION, clientId);
          requestOnChange(ITEM, clientId);
          requestOnChange(TITLE, clientId);
          requestOnChange(GETPLAYERMODE, clientId);
          break;
        }
#endif
      #ifdef USE_SD
        case CHANGEMODE: {
          // КРИТИЧНО: переключение режима может быть «тяжёлым» (SD mount / индексация).
          // Если вызвать config.changeMode() прямо здесь, мы блокируем обработку WebSocket очереди,
          // и браузер (особенно телефон) получает таймауты и перестаёт обновляться.
          // Поэтому планируем переключение в отдельной задаче и выходим сразу.
          scheduleChangeModeTask(config.newConfigMode);
          return;
          break;
        }
      #endif
      default:          break;
    }
    if (strlen(wsBuf) > 0) {
      // Заменён 'return' на 'continue': ранний 'return' при websocket.count()==0
      // прерывал весь цикл processQueue и выбрасывал оставшиеся кадры пачки (VOLUME/TITLE/playermode).
      // При кратковременном реконнекте клиент мог переподключиться к следующей итерации,
      // поэтому правильнее пропустить только этот кадр, не теряя остальные.
      // count()/client() уже отсекают DISCONNECTED и _client==NULL.
      if(websocket.count() == 0) continue;
      // processQueue без wsSendLocked — основной поток WS-ответов (GETINDEX, VOLUME, TITLE…).
      if (clientId == 0) { websocket.textAll(wsBuf); }else{ websocket.text(clientId, wsBuf); }
  #ifdef MQTT_ROOT_TOPIC
      if (clientId == 0 && (request.type == STATION || request.type == ITEM || request.type == TITLE || request.type == MODE)) mqttPublishStatus();
      if (clientId == 0 && request.type == VOLUME) mqttPublishVolume();
  #endif
    }
    // Состояние таймера сна не входит в JSON GETSYSTEM — отдельным сообщением, иначе UI не знает supported/alloff.
    if (request.type == GETSYSTEM) {
      sleepTimerPlugin.pushState(clientId);
    }
  }
}

void NetServer::loop() {
  // Убран early return при SDREADY: при нём не вызывались processQueue(), pingAll(), cleanupClients(),
  // из-за чего веб переставал получать обновления и «умирал» при работе в SD с WiFi (после потери WiFi status=SDREADY).
  // В main-ветке этого return не было — обработка очереди и keepalive должны выполняться всегда.

#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
  /* Classic: отложенная перечитка playlist.csv в PSRAM после /saveplaylist. */
  if (g_playlistPsramReloadReq) {
    g_playlistPsramReloadReq = false;
    netserverLoadPlaylistPsram();
  }
#endif

  // [HEARTBEAT] Убрали периодический вывод в консоль Free Heap и размера очереди, 
  // чтобы не мешать логам индексации SD и не переполнять Serial при активных запросах.
  /*
  static uint32_t lastHb = 0;
  if (millis() - lastHb > 10000) {
    lastHb = millis();
    Serial.printf("[WEB] Heartbeat: queue=%d, heap=%u\n", (int)uxQueueMessagesWaiting(nsQueue), (unsigned int)ESP.getFreeHeap());
  }
  */

  if (shouldReboot) {
    Serial.println("Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP.restart();
  }
#ifdef USE_DLNA
  // Build/Append в worker завершился — обновляем плейлист в WebUI без ручного refresh.
  if (config.consumeDlnaPlaylistUpdated()) {
    if (config.getMode() == PM_WEB && config.getPlaylistSource() == PL_SRC_DLNA) {
      config.initPlaylist();
      requestOnChange(PLAYLIST, 0);
      requestOnChange(GETPLAYERMODE, 0);
    }
  }
#endif
  // Сразу убираем DISCONNECTED-зомби до processQueue — иначе гонка text() vs delete.
  websocket.purgeDisconnected();
  processQueue();
  // Периодический ПРОТОКОЛЬНЫЙ keepalive для WebSocket.
  // Ранее отправляли текстовый JSON {"pong":1}, но это не WebSocket ping frame.
  // На части мобильных клиентов сокет всё равно уходил в rx timeout (AsyncTCP _poll timeout 4)
  // при отсутствии входящего трафика от браузера.
  // Теперь шлём именно pingAll(), чтобы клиент отвечал pong на уровне протокола WS.
  // Это снижает вероятность "тихого" отвала Web при SD-проигрывании.
  static uint32_t lastWsKeepalive = 0;
  if (websocket.count() > 0 && (millis() - lastWsKeepalive >= 5000)) {
    lastWsKeepalive = millis();
    websocket.pingAll();
  }
  // Не ограничиваем количество клиентов через cleanupClients(2).
  // Ранее лимит "2" вызывал каскадные disconnect/reconnect (особенно при входе с телефона),
  // что совпадало с таймаутами AsyncTCP и визуальной "смертью" WebUI.
  // Оставляем только чистку реально мёртвых сокетов без принудительного выталкивания активных.
  static uint32_t lastCleanup = 0;
  if(millis() - lastCleanup > 2000) {
    lastCleanup = millis();
    websocket.cleanupClients();
  }
  switch (importRequest) {
    case IMPL:    importPlaylist();  importRequest = IMDONE; break;
    case IMWIFI:  config.saveWifi(); importRequest = IMDONE; break;
    default:      break;
  }
  //processQueue();
}

void NetServer::sendToast(const char* msg, bool isError) {
  if (!msg || !msg[0]) return;
  snprintf(wsBuf, sizeof(wsBuf), "{\"toast\":\"%.200s\",\"isErr\":%d}", msg, isError ? 1 : 0);
  wsSendLocked(0, wsBuf);
}

#if IR_PIN!=255
void NetServer::irToWs(const char* protocol, uint64_t irvalue) {
  wsBuf[0]='\0';
  snprintf (wsBuf, sizeof(wsBuf), "{\"ircode\": %llu, \"protocol\": \"%s\"}", irvalue, protocol);
  wsSendLocked(0, wsBuf);
}
void NetServer::irValsToWs() {
  if (!irRecordEnable) return;
  wsBuf[0]='\0';
  snprintf (wsBuf, sizeof(wsBuf), "{\"irvals\": [%llu, %llu, %llu]}", config.ircodes.irVals[config.irindex][0], config.ircodes.irVals[config.irindex][1], config.ircodes.irVals[config.irindex][2]);
  wsSendLocked(0, wsBuf);
}
#endif

void NetServer::onWsMessage(void *arg, uint8_t *data, size_t len, uint8_t clientId) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    char local[128];
    size_t safeLen = (len >= sizeof(local)) ? sizeof(local) - 1 : len;
    memcpy(local, data, safeLen);
    local[safeLen] = 0;
    // ping не учитываем в лимите — иначе конкурирует с реальным UI на мобильных.
    bool pingKeepalive = (safeLen >= 5 && strncmp(local, "ping=", 5) == 0);
    if (!pingKeepalive && !ws_rate_allowed(clientId)) {
      if (config.store.audioinfo) Serial.printf("[WEBSOCKET] client #%lu rate limit exceeded\n", (unsigned long)clientId);
      return;
    }
    if (config.parseWsCommand(local, _wscmd, sizeof(_wscmd), _wsval, sizeof(_wsval))) {
      if (strcmp(_wscmd, "ping") == 0) {
        // pong без wsSendLocked: при FLAC мьютекс 150ms мог не взяться → pong терялся → reconnect.
        websocket.text(clientId, "{\"pong\": 1}");
        return;
      }
      if (strcmp(_wscmd, "trebble") == 0) {
        int8_t valb = atoi(_wsval);
        config.setTone(config.store.bass, config.store.middle, valb);
        return;
      }
      if (strcmp(_wscmd, "middle") == 0) {
        int8_t valb = atoi(_wsval);
        config.setTone(config.store.bass, valb, config.store.trebble);
        return;
      }
      if (strcmp(_wscmd, "bass") == 0) {
        int8_t valb = atoi(_wsval);
        config.setTone(valb, config.store.middle, config.store.trebble);
        return;
      }
      if (strcmp(_wscmd, "submitplaylistdone") == 0) {
#ifdef MQTT_ROOT_TOPIC
        //mqttplaylistticker.attach(5, mqttplaylistSend);
        timekeeper.waitAndDo(5, mqttplaylistSend);
#endif
        // Бесшовное сохранение: не перезапускаем воспроизведение.
        // importPlaylist() уже вызвал PLAYLISTSAVED → getPlaylist() → WebUI обновится.
        return;
      }
      
      uint32_t _wsCmdStartMs = millis();
      if(cmd.exec(_wscmd, _wsval, clientId)){
        uint32_t _wsCmdElapsedMs = millis() - _wsCmdStartMs;
        if (_wsCmdElapsedMs >= 500) {
          Serial.printf("[WS-DIAG] cmd='%s' client=%u handled in %lu ms\n",
                        _wscmd, (unsigned)clientId, (unsigned long)_wsCmdElapsedMs);
        }
        return;
      }
    }
  }
}

void NetServer::getPlaylist(uint8_t clientId) {
  // Используем /api/playlist вместо прямого PLAYLIST_PATH.
  // serveStatic("/data/") перехватывал /data/playlist.csv и ВСЕГДА отдавал WEB-плейлист,
  // игнорируя SD-режим. /api/playlist корректно определяет режим и отдаёт нужный файл.
  snprintf(nsBuf, sizeof(nsBuf), "{\"file\": \"http://%s/api/playlist\"}", config.ipToStr(WiFi.localIP()));
  wsSendLocked(clientId, nsBuf);
}

int NetServer::_readPlaylistLine(File &file, char * line, size_t size){
  int bytesRead = file.readBytesUntil('\n', line, size);
  if(bytesRead>0){
    line[bytesRead] = 0;
    if(line[bytesRead-1]=='\r') line[bytesRead-1]=0;
  }
  return bytesRead;
}

bool NetServer::importPlaylist() {
  // Разрешаем импорт в SD-режиме — запись идёт в PLAYLIST_SD_PATH.
  const char* destPath = (config.getMode()==PM_SDCARD) ? PLAYLIST_SD_PATH : PLAYLIST_PATH;
  const char* destIndex = (config.getMode()==PM_SDCARD) ? INDEX_SD_PATH : INDEX_PATH;
  File tempfile = LittleFS.open(TMP_PATH, "r");
  if (!tempfile) {
    return false;
  }
  char linePl[BUFLEN*3];
  int sOvol;
  _readPlaylistLine(tempfile, linePl, sizeof(linePl)-1);
  if (config.parseCSV(linePl, nsBuf, nsBuf2, sOvol)) {
    // Вместо rename копируем содержимое через open("w").
    // rename не работает если dest-файл открыт другим HTTP-ответом (open FD).
    // open("w") безопасно обрезает и перезаписывает даже открытый файл.
    tempfile.seek(0);
    File destFile = LittleFS.open(destPath, "w");
    if (destFile) {
      uint8_t cpBuf[512];
      while (tempfile.available()) {
        size_t rd = tempfile.read(cpBuf, sizeof(cpBuf));
        if (rd > 0) destFile.write(cpBuf, rd);
      }
      destFile.close();
    }
    tempfile.close();
    LittleFS.remove(TMP_PATH);
    // Не удаляем индекс отдельно — indexSDPlaylistFile()/indexPlaylist() перезапишет его.
    // Удаление создавало окно, когда при перезагрузке индекс отсутствовал,
    // и initSDPlaylist() запускал полное сканирование SD, теряя избранные.
    requestOnChange(PLAYLISTSAVED, 0);
    return true;
  }
  if (config.parseJSON(linePl, nsBuf, nsBuf2, sOvol)) {
    File playlistfile = LittleFS.open(destPath, "w");
    snprintf(linePl, sizeof(linePl)-1, "%s\t%s\t%d", nsBuf, nsBuf2, 0);
    playlistfile.println(linePl);
    while (tempfile.available()) {
      _readPlaylistLine(tempfile, linePl, sizeof(linePl)-1);
      if (config.parseJSON(linePl, nsBuf, nsBuf2, sOvol)) {
        snprintf(linePl, sizeof(linePl)-1, "%s\t%s\t%d", nsBuf, nsBuf2, 0);
        playlistfile.println(linePl);
      }
    }
    playlistfile.flush();
    playlistfile.close();
    tempfile.close();
    LittleFS.remove(TMP_PATH);
    requestOnChange(PLAYLISTSAVED, 0);
    return true;
  }
  tempfile.close();
  LittleFS.remove(TMP_PATH);
  return false;
}

void NetServer::requestOnChange(requestType_e request, uint8_t clientId) {
  if(nsQueue==NULL) return;
  // Ранее здесь была жёсткая отсечка:
  // if(uxQueueSpacesAvailable(nsQueue) < 4) return;
  // Идея была в том, чтобы «оставить место для важных команд», но на практике
  // в SD‑режиме при активном WebUI очередь часто оказывалась «почти полной»,
  // и обновления состояния (VUmeter, позиция трека, режим и т.п.)
  // просто не попадали в очередь. В результате WebUI переставал получать данные
  // и визуально «умирал», хотя устройство продолжало играть и отвечать.
  // Новый подход: при переполнении пропускаем только «не критичные» события.
  // Критичные статусы (TITLE/STATION/MODE/GETACTIVE/GETINDEX) пытаемся доставить даже при высокой нагрузке.
  UBaseType_t freeSlots = uxQueueSpacesAvailable(nsQueue);
  // VOLUME, EQUALIZER, BALANCE добавлены в список критичных событий.
  // Ранее при заполнении очереди (freeSlots==0) они молча отбрасывались (NSQ_SEND_DELAY=0),
  // из-за чего на медленном телефоне громкость и эквалайзер не отображались после подключения.
  bool isCritical =
    request == TITLE ||
    request == STATION ||
    request == STATIONNAME ||
    request == ITEM ||
    request == BITRATE ||
    request == MODE ||
    request == PLAYLISTSAVED ||
    request == PLAYLIST_UPDATED ||
    request == GETACTIVE ||
    request == GETINDEX ||
    request == GETPLAYERMODE ||
    request == VOLUME ||      // критично: начальный уровень громкости должен дойти до клиента
    request == EQUALIZER ||   // критично: настройки эквалайзера не должны теряться при нагрузке
    request == BALANCE        // критично: баланс каналов — часть начального состояния UI
#ifdef USE_DLNA
    || request == PLAYLISTSRC_DLNA ||
    request == PLAYLISTSRC_WEB
#endif
    ;
  if (freeSlots == 0 && !isCritical) {
    return;
  }
  
  // Формируем событие для WebSocket-очереди.
  nsRequestParams_t nsrequest;
  // Сохраняем тип изменения (meta, sdpos, mode и т.д.).
  nsrequest.type = request;
  // Сохраняем целевой clientId (0 = broadcast всем клиентам).
  nsrequest.clientId = clientId;
  // Критичные события ждём коротко (до 2 мс), чтобы не терять title/menu.
  TickType_t sendTicks = (freeSlots == 0 && isCritical) ? pdMS_TO_TICKS(2) : NSQ_SEND_DELAY;
  xQueueSend(nsQueue, &nsrequest, sendTicks);
}

void NetServer::resetQueue(){
  if(nsQueue!=NULL) xQueueReset(nsQueue);
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  static int freeSpace = 0;
  if(request->url()=="/upload"){
    if (!index) {
      // НЕ удаляем playlist/index здесь — файл может быть ещё открыт
      // другим HTTP-ответом (/api/playlist). Удаление открытого FD вызывает
      // ошибку esp_littlefs и последующий крэш pbuf_free в AsyncTCP.
      // importPlaylist() сам перезапишет файл через open("w") + copy.
      freeSpace = (float)LittleFS.totalBytes()/100*68-LittleFS.usedBytes();
      request->_tempFile = LittleFS.open(TMP_PATH , "w");
    }else{
      
    }
    if (len) {
      if(freeSpace>index+len){
        request->_tempFile.write(data, len);
      }
    }
    if (final) {
      request->_tempFile.close();
      freeSpace = 0;
    }
  }else if(request->url()=="/update"){
    if (!index) {
      int target = U_FLASH;
      if (request->hasParam("updatetarget", true)) {
        String val = request->getParam("updatetarget", true)->value();
        target = (val == "LittleFS") ? U_SPIFFS : U_FLASH;
      }
      Serial.printf("Update Start: %s\n", filename.c_str());
      player.sendCommand({PR_STOP, 0});
      display.putRequest(NEWMODE, UPDATING);
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, target)) {
        Update.printError(Serial);
        request->send(200, "text/html", updateError());
      }
    }
    if (!Update.hasError()) {
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
        request->send(200, "text/html", updateError());
      }
    }
    if (final) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %uB\n", index + len);
      } else {
        Update.printError(Serial);
        request->send(200, "text/html", updateError());
      }
    }
  } else { // Обработка файлов из меню "/webboard"
    DBGVB("File: %s, size:%u bytes, index: %u, final: %s\n", filename.c_str(), len, index, final?"true":"false");
    
    if (!index) {
      player.sendCommand({PR_STOP, 0});
      
      String spath = "/www/"; 
      String finalName = filename;

      // 1. Сначала проверяем системные файлы (ПЛЕЙЛИСТ И ИЗБРАННОЕ)
      if (filename == "playlist.csv" || filename == "wifi.csv" || filename == "settings.json") {
        spath = "/data/";
      } 
      // 2. Затем проверяем обложки
      else if (filename.endsWith(".jpg")) {
        spath = "/station_covers/";
        finalName.replace(" ", "_"); // Заменяем пробелы только для картинок
        if (!LittleFS.exists("/station_covers")) {
            LittleFS.mkdir("/station_covers");
        }
      }

      // WebBoard загружает несжатые системные файлы. Старый *.gz иначе продолжал иметь
      // приоритет в PSRAM-кэше, поэтому пользователь после успешной загрузки видел прежний UI.
      // Удаляем только парный gzip и отключаем его RAM-копию; новый файл начнёт отдаваться сразу.
      if (spath == "/www/" && filename == "script.js") {
        LittleFS.remove("/www/script.js.gz");
        g_psramScriptJsSize = 0;
      } else if (spath == "/www/" && filename == "style.css") {
        LittleFS.remove("/www/style.css.gz");
        g_psramStyleCssSize = 0;
      } else if (spath == "/www/" && filename == "player.html") {
        LittleFS.remove("/www/player.html.gz");
        g_psramPlayerHtmlSize = 0;
      }
      
      request->_tempFile = LittleFS.open(spath + finalName, "w");
      Serial.printf("WebBoard saving to: %s%s\n", spath.c_str(), finalName.c_str());
    }
    
    if (len) {
      if (request->_tempFile) request->_tempFile.write(data, len);
    }
    
    if (final) {
      if (request->_tempFile) request->_tempFile.close();
      // Важно: проверяем оригинальное имя файла для индексации
      if (filename == "playlist.csv") {
          config.indexPlaylist();
          Serial.println("Playlist indexed!");
      }
    }
  }
}  
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      // НЕ отправляем GETINDEX при подключении WS-клиента.
      // JS сам пошлёт getindex=1 после загрузки player.html (continueLoading).
      // Ранее серверный GETINDEX + клиентский getindex=1 вызывали двойную загрузку
      // плейлиста (/api/playlist), что исчерпывало TCP-сокеты на мобильных.
      if (config.store.audioinfo) {
        IPAddress rip = client->remoteIP();
        // Сразу после accept remoteIP() иногда ещё 0.0.0.0 — не ошибка клиента.
        if (rip == IPAddress(0, 0, 0, 0))
          Serial.printf("[WEBSOCKET] client #%lu connected (IP pending)\n", client->id());
        else
          Serial.printf("[WEBSOCKET] client #%lu connected from %s\n", client->id(), config.ipToStr(rip));
      }
      break;
    case WS_EVT_DISCONNECT:
      ws_rate_clear_client(client->id());
      if (config.store.audioinfo) Serial.printf("[WEBSOCKET] client #%lu disconnected\n", client->id());
      break;
    case WS_EVT_DATA: netserver.onWsMessage(arg, data, len, client->id()); break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}
void handleNotFound(AsyncWebServerRequest * request) {
#if defined(HTTP_USER) && defined(HTTP_PASS)
  if(network.status == CONNECTED)
    if (request->url() == "/logout") {
      request->send(401);
      return;
    }
    if (!request->authenticate(HTTP_USER, HTTP_PASS)) {
      return request->requestAuthentication();
    }
#endif
  if(request->url()=="/emergency") { request->send_P(200, "text/html", emergency_form); return; }
  if(request->method() == HTTP_POST && request->url()=="/webboard" && config.emptyFS) { request->redirect("/"); ESP.restart(); return; }
  if (request->method() == HTTP_GET) {
    DBGVB("[%s] client ip=%s request of %s", __func__, config.ipToStr(request->client()->remoteIP()), request->url().c_str());
    if (strcmp(request->url().c_str(), PLAYLIST_PATH) == 0 || 
        strcmp(request->url().c_str(), SSIDS_PATH) == 0 || 
        strcmp(request->url().c_str(), INDEX_PATH) == 0 || 
        strcmp(request->url().c_str(), TMP_PATH) == 0 || 
        strcmp(request->url().c_str(), PLAYLIST_SD_PATH) == 0 || 
        strcmp(request->url().c_str(), INDEX_SD_PATH) == 0) {
#ifdef MQTT_ROOT_TOPIC
      if (strcmp(request->url().c_str(), PLAYLIST_PATH) == 0) while (mqttplaylistblock) vTaskDelay(5);
#endif
      if(strcmp(request->url().c_str(), PLAYLIST_PATH) == 0 && config.getMode()==PM_SDCARD){
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, PLAYLIST_SD_PATH, "application/octet-stream");
        response->addHeader("Connection", "close");
        request->send(response);
      }else{
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, request->url(), "application/octet-stream");
        response->addHeader("Connection", "close");
        request->send(response);
      }
      return;
    }// if (strcmp(request->url().c_str(), PLAYLIST_PATH) == 0 || 
  }// if (request->method() == HTTP_GET)
  
  if (request->method() == HTTP_POST) {
    if(request->url()=="/webboard"){ request->redirect("/"); return; } // <--post files from /data/www
    if(request->url()=="/upload"){ // <--upload playlist.csv or wifi.csv
      if (request->hasParam("plfile", true, true)) {
        netserver.importRequest = IMPL;
        request->send(200);
      } else if (request->hasParam("wifile", true, true)) {
        netserver.importRequest = IMWIFI;
        request->send(200);
      } else {
        request->send(404);
      }
      return;
    }
    if(request->url()=="/update"){ // <--upload firmware
      shouldReboot = !Update.hasError();
      AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot ? "OK" : updateError());
      response->addHeader("Connection", "close");
      request->send(response);
      return;
    }
  }// if (request->method() == HTTP_POST)
  
  if (request->url() == "/favicon.ico") {
    request->send(200, "image/x-icon", "data:,");
    return;
  }
  if (request->url() == "/variables.js") {
    /* Запасной путь (основной — webserver.on выше); тот же DEVICE_NAME → yoTitle. */
    char varsJs[288];
    buildVariablesJs(varsJs, sizeof(varsJs));
    AsyncWebServerResponse *varResp = request->beginResponse(200, "application/javascript", varsJs);
    varResp->addHeader("Cache-Control", "no-cache, no-store");
    request->send(varResp);
    return;
  }
  if (strcmp(request->url().c_str(), "/settings.html") == 0 || strcmp(request->url().c_str(), "/update.html") == 0 || strcmp(request->url().c_str(), "/ir.html") == 0){
    //request->send_P(200, "text/html", index_html);
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
    return;
  }
  if (request->method() == HTTP_GET && request->url() == "/webboard") {
    request->send_P(200, "text/html", emptyfs_html);
    return;
  }
  Serial.print("Not Found: ");
  Serial.println(request->url());
  request->send(404, "text/plain", "Not found");
}

void handleIndex(AsyncWebServerRequest * request) {
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3) && YORADIO_CLASSIC_HTTP_HEAP_DIAG
  /* Classic diag: корень WebUI — URL + heap на входе в handleIndex. */
  Serial.printf("[HTTP] %s\n", request->url().c_str());
  netserverLogHeap8bit("handleIndex");
#endif
  if(config.emptyFS){
    if(request->url()=="/" && request->method() == HTTP_GET ) { request->send_P(200, "text/html", emptyfs_html); return; }
    if(request->url()=="/" && request->method() == HTTP_POST) {
      if(request->arg("ssid")!="" && request->arg("pass")!=""){
        netserver.nsBuf[0]='\0';
        snprintf(netserver.nsBuf, sizeof(netserver.nsBuf), "%s\t%s", request->arg("ssid").c_str(), request->arg("pass").c_str());
        request->redirect("/");
        config.saveWifiFromNextion(netserver.nsBuf);
        return;
      }
      request->redirect("/"); 
      ESP.restart();
      return;
    }
    Serial.print("Not Found: ");
    Serial.println(request->url());
    request->send(404, "text/plain", "Not found");
    return;
  } // end if(config.emptyFS)
#if defined(HTTP_USER) && defined(HTTP_PASS)
  if(network.status == CONNECTED)
    if (!request->authenticate(HTTP_USER, HTTP_PASS)) {
      return request->requestAuthentication();
    }
#endif
  if (strcmp(request->url().c_str(), "/") == 0 && request->params() == 0) {
    // Отдаём плеер (index) при CONNECTED и при SDREADY (SD без WiFi — пользователь вводит IP для плеера).
    // Редирект на settings только когда реально нужна настройка (AP/нет сети), а не при работе в SD.
    if (network.status == CONNECTED || network.status == SDREADY) {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html);
      response->addHeader("Cache-Control", "max-age=31536000");
      request->send(response);
    } else {
      request->redirect("/settings.html");
    }
    return;
  }
  if(network.status == CONNECTED){
    int paramsNr = request->params();
    if(paramsNr==1){
      AsyncWebParameter* p = request->getParam(0);
      if(cmd.exec(p->name().c_str(),p->value().c_str())) {
        if(p->name()=="reset" || p->name()=="clearLittleFS") request->redirect("/");
        if(p->name()=="clearLittleFS") { vTaskDelay(pdMS_TO_TICKS(100)); ESP.restart(); }
        request->send(200, "text/plain", "");
        return;
      }
    }
    if (request->hasArg("trebble") && request->hasArg("middle") && request->hasArg("bass")) {
      config.setTone(request->getParam("bass")->value().toInt(), request->getParam("middle")->value().toInt(), request->getParam("trebble")->value().toInt());
      request->send(200, "text/plain", "");
      return;
    }
    if (request->hasArg("sleep")) {
      int sford = request->getParam("sleep")->value().toInt();
      int safterd = request->hasArg("after")?request->getParam("after")->value().toInt():0;
      // Проверка safterd > 0 (было >= 0), чтобы предотвратить мгновенный сон при вводе 0
      if(sford > 0 && safterd > 0){ request->send(200, "text/plain", ""); config.sleepForAfter(sford, safterd); return; }
    }
    request->send(404, "text/plain", "Not found");
    
  }else{
    request->send(404, "text/plain", "Not found");
  }
}

