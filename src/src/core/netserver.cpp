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
#include "../plugins/SleepTimer/SleepTimer.h"
#include "../displays/tools/l10n.h"
#include "config.h" // scheduleChangeModeTask

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
  // [FIX][SD+WEB] Для requestOnChange используем неблокирующую постановку в очередь.
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

AsyncWebServer webserver(80);
AsyncWebSocket websocket("/ws");

// Simple per-client WebSocket rate limiter
#ifndef WS_RATE_LIMIT_WINDOW_MS
#define WS_RATE_LIMIT_WINDOW_MS 1000U
#endif
#ifndef WS_MAX_MSG_PER_WINDOW
#define WS_MAX_MSG_PER_WINDOW 10
#endif
#ifndef WS_TRACKED_CLIENTS
#define WS_TRACKED_CLIENTS 16
#endif
static uint32_t ws_client_ids[WS_TRACKED_CLIENTS];
static uint32_t ws_client_window_start[WS_TRACKED_CLIENTS];
static uint8_t  ws_client_count[WS_TRACKED_CLIENTS];

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
//  mqttplaylistticker.detach();
  mqttPublishPlaylist();
  mqttplaylistblock = false;
}
#endif

char* updateError() {
  snprintf(netserver.nsBuf, sizeof(netserver.nsBuf), "Update failed with error (%d)<br /> %s", (int)Update.getError(), Update.errorString());
  return netserver.nsBuf;
}

bool NetServer::begin(bool quiet) {
  if(network.status==SDREADY) return true;
  if(!quiet) Serial.print("##[BOOT]#\tnetserver.begin\t");
  importRequest = IMDONE;
  irRecordEnable = false;
  playerBufMax = psramInit()?300000:1600 * config.store.abuff;
  nsQueue = xQueueCreate( 32, sizeof( nsRequestParams_t ) );
  while(nsQueue==NULL){;}

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
  /* [Gemini3Pro] Дублирующий обработчик /saveplaylist удален, так как он уже зарегистрирован выше с правильной логикой (Line ~150) */
  webserver.on("/", HTTP_ANY, handleIndex);
  webserver.onNotFound(handleNotFound);
  webserver.onFileUpload(handleUpload);

  auto sendIndexHtml = [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html);
    response->addHeader("Cache-Control","max-age=31536000");
    request->send(response);
  };

  webserver.on("/settings.html", HTTP_ANY, sendIndexHtml);
  webserver.on("/update.html", HTTP_ANY, sendIndexHtml);
  webserver.on("/ir.html", HTTP_ANY, sendIndexHtml);

  webserver.on("/variables.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    snprintf(netserver.nsBuf, sizeof(netserver.nsBuf),
             "var yoVersion='%s';\nvar formAction='%s';\nvar playMode='%s';\n",
             YOVERSION,
             (network.status == CONNECTED && !config.emptyFS)?"webboard":"",
             (network.status == CONNECTED)?"player":"ap");
    request->send(200, "application/javascript", netserver.nsBuf);
  });

// 1. Отдача вспомогательных и основных JS/CSS файлов
//
// Для script.js и style.css добавлена поддержка PSRAM‑кэша:
//  - при старте Config::init загружает *.gz версии файлов в PSRAM;
//  - здесь, при запросе со стороны браузера, мы сначала пробуем отдать данные
//    из PSRAM, и только при отсутствии кэша — читаем файлы из LittleFS.
//
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

// script.js (основная логика WebUI)
webserver.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    // При наличии кэша в PSRAM — отдаём его (gzip-контент).
    if (g_psramScriptJs && g_psramScriptJsSize) {
        AsyncWebServerResponse *response =
            request->beginResponse_P(200, "application/javascript", g_psramScriptJs, g_psramScriptJsSize);
        response->addHeader("Content-Encoding", "gzip");      // содержимое уже сжато
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
        return;
    }

    // Fallback: читаем из LittleFS как раньше (gz при наличии).
    if (LittleFS.exists("/www/script.js.gz")) {
        AsyncWebServerResponse *response =
            request->beginResponse(LittleFS, "/www/script.js.gz", "application/javascript");
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
    } else if (LittleFS.exists("/www/script.js")) {
        AsyncWebServerResponse *response =
            request->beginResponse(LittleFS, "/www/script.js", "application/javascript");
        response->addHeader("Cache-Control", "max-age=31536000");
        request->send(response);
    } else {
        request->send(404, "text/plain", "script.js not found");
    }
});

// Дополнительный маршрут на случай прямого запроса .gz (некоторые клиенты могут кэшировать так)
webserver.on("/script.js.gz", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (g_psramScriptJs && g_psramScriptJsSize) {
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
    if (g_psramStyleCss && g_psramStyleCssSize) {
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
    if (g_psramStyleCss && g_psramStyleCssSize) {
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
// [FIX v2] cover.js — отдаём из PSRAM если кэш доступен
webserver.on("/cover.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (g_psramCoverJs && g_psramCoverJsSize) {
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

// [FIX v2] facts.js — отдаём из PSRAM если кэш доступен
webserver.on("/facts.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (g_psramFactsJs && g_psramFactsJsSize) {
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

// [FIX v2] dragpl.js — отдаём из PSRAM если кэш доступен (dragpl.js маленький, без gz)
webserver.on("/dragpl.js", HTTP_GET, [](AsyncWebServerRequest *request) {
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
// [FIX v3] Вместо chunked response (многократные open/close LittleFS) —
// используем AsyncFileResponse, который открывает файл ОДИН раз
// и стримит из одного file handle. Это радикально ускоряет отдачу
// и не блокирует другие HTTP-запросы.
webserver.on("/api/playlist", HTTP_GET, [](AsyncWebServerRequest *request){
  const char* playlistPath = (config.getMode() == PM_SDCARD) ? PLAYLIST_SD_PATH : PLAYLIST_PATH;

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
});

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
      // [FIX] Удаляем расширение файла и числовые префиксы для лучшего поиска в iTunes.
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

    // [FIX] Общий переключатель WebUI-обложек (для WEB и SD режимов).
    // Если отключено — всегда отдаём логотип.
    if (!WEBUI_COVERS_ENABLE) {
      effectiveUrl = "/logo.svg";
    }
    
    // [FIX] В SD-режиме: если обложки из ID3 нет (logo.svg),
    // то для WebUI разрешаем поиск через iTunes — как в веб-радио.
    // Если ID3-обложка есть, currentCoverUrl уже указывает на неё и будет отдана.
    if (WEBUI_COVERS_ENABLE && config.getMode() == PM_SDCARD && effectiveUrl == "/logo.svg" && player.status() == PLAYING) {
      // [FIX] Минимальный фильтр системных слов, чтобы не искать "timeout" в iTunes.
      String coverTitleLower = coverTitle;
      coverTitleLower.toLowerCase();
      // [FIX] Добавлена блокировка API-запросов во время переключения режимов для предотвращения зависаний
      extern volatile bool g_modeSwitching;
      if (!g_modeSwitching && coverTitle.length() >= 3 && !coverTitle.startsWith("[") && coverTitleLower.indexOf("timeout") < 0) {
        effectiveUrl = "SEARCH_ITUNES";
      }
    }
    
    // [FIX] Если планируется загрузка с iTunes, проверяем наличие в локальном кэше ПЕРЕД отправкой ответа.
    // Если в кэше уже есть — отдаём её URL сразу.
    //
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
        if (sc.startsWith("http://") || sc.startsWith("https://")) {
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

    // 3. Формируем JSON (Важно: ТРИ поля %s для ТРЕХ переменных)
    // [FIX] Ограничиваем длину полей (%.128s), иначе url+station+title по 250 байт переполняют json[512].
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
    } else {
      title = String(config.station.name);
    }
    String status = "";

    // [FIX] Служебные сообщения не считаем фактом — отдаем их отдельно в поле status.
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
    
    // [FIX] pending=1 если запрос факта в очереди/выполняется, status — служебное сообщение
    String json = "{\"facts\":" + jsonFacts + ",\"title\":\"" + escapedTitle + "\",\"pending\":" + String(trackFactsPlugin.isRequestPending() ? 1 : 0) + ",\"status\":\"" + status + "\"}";
    
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    
    // Заголовки для предотвращения кэширования в браузере
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    response->addHeader("Connection", "close");
    
    request->send(response);
});

// Чтение (Экспорт)
webserver.on("/playlist.csv", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists("/data/playlist.csv")) {
        request->send(LittleFS, "/data/playlist.csv", "text/csv");
        Serial.println("[SYSTEM] Export: Playlist sent to browser");
    } else {
        request->send(404, "text/plain", "File not found");
    }
});

// [FIX v2] player.html — отдаём из PSRAM если кэш доступен (критично для мобильных)
webserver.on("/player.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (g_psramPlayerHtml && g_psramPlayerHtmlSize) {
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

// [FIX v2] options.html — отдаём из PSRAM если кэш доступен
webserver.on("/options.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (g_psramOptionsHtml && g_psramOptionsHtmlSize) {
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

// [FIX v2] logo.svg — отдаём из PSRAM если кэш доступен
webserver.on("/logo.svg", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (g_psramLogoSvg && g_psramLogoSvgSize) {
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

// [FIX v2] theme.css — отдаём из PSRAM если кэш доступен (НЕ gzip, файл хранится без сжатия)
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
//
// ПРАВИЛЬНЫЙ ПОРЯДОК:
// 1. /sc/    -> /station_covers/  (обложки альбомов)
// 2. /data/  -> /data/            (плейлисты, настройки)
// 3. /       -> /www/             (веб-интерфейс, должен быть ПОСЛЕДНИМ)
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
      }
    }
  );

#ifdef CORS_DEBUG
  DefaultHeaders::Instance().addHeader(F("Access-Control-Allow-Origin"), F("*"));
  DefaultHeaders::Instance().addHeader(F("Access-Control-Allow-Headers"), F("content-type"));
#endif
  webserver.begin();
  //if(strlen(config.store.mdnsname)>0)
  //  MDNS.begin(config.store.mdnsname);
  websocket.onEvent(onWsEvent);
  webserver.addHandler(&websocket);
  if(!quiet) Serial.println("done");
  return true;
}

size_t NetServer::chunkedHtmlPageCallback(uint8_t* buffer, size_t maxLen, size_t index){
  File requiredfile;
  // [FIX v2] Все файлы (включая SD-плейлист) теперь на LittleFS.
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
  response->addHeader("Cache-Control","max-age=31536000");
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
  // [FIX] cleanupClients() перенесён в loop() с интервалом 2сек
  // Раньше здесь жёстко стоял лимит 12 сообщений за один вызов.
  // В SD‑режиме при активном WebUI очередь могла копиться быстрее, чем
  // обрабатываться, и из‑за дополнительного ограничения в requestOnChange()
  // обновления для браузера просто переставали попадать в очередь —
  // визуально «умирал» Web, хотя система продолжала работать.
  //
  // Новый подход:
  //  - лимит делаем более гибким и чуть выше (24);
  //  - при желании можно будет дополнительно адаптировать его под Heap/режим.
  int processed = 0;
  const int maxPerCycle = 24;  // обрабатываем до 24 запросов за один проход
  nsRequestParams_t request;
  while(processed < maxPerCycle && xQueueReceive(nsQueue, &request, NS_QUEUE_TICKS)){
    processed++;
    uint8_t clientId = request.clientId;
    wsBuf[0]='\0';
    switch (request.type) {
      case PLAYLIST:        getPlaylist(clientId); break;
      // [FIX] Только обновить плейлист у клиентов, БЕЗ перестройки индекса. Вызывается из FavUpdate после setFavorite:
      // позиции строк в плейлисте не меняются (меняется только 5-я колонка 0/1), indexSDPlaylistFile() не нужен
      // и блокировал main loop на десятки секунд при 3000+ треках → веб «умирал», FREEZE-RECOVERY.
      case PLAYLIST_UPDATED: getPlaylist(clientId); break;
      case PLAYLISTSAVED:   {
        #ifdef USE_SD
        if(config.getMode()==PM_SDCARD) {
          // Перестройка индекса только при полном сохранении/импорте плейлиста (не при setFavorite).
          config.indexSDPlaylistFile();
        }
        #endif
        if(config.getMode()==PM_WEB){
          config.indexPlaylist(); 
          config.initPlaylist(); 
        }
        getPlaylist(clientId); break;
      }
      case GETACTIVE: {
          bool dbgact = false, nxtn=false;
          //String act = F("\"group_wifi\",");
          nsBuf[0]='\0';
          APPEND_GROUP("group_wifi");
          if (network.status == CONNECTED) {
                                                                //act += F("\"group_system\",");
                                                                APPEND_GROUP("group_system");
            if (BRIGHTNESS_PIN != 255 || DSP_CAN_FLIPPED || DSP_MODEL == DSP_NOKIA5110 || dbgact)    APPEND_GROUP("group_display");
          #ifdef USE_NEXTION
                                                                APPEND_GROUP("group_nextion");
            if (!SHOW_WEATHER || dbgact)                        APPEND_GROUP("group_weather");
            nxtn=true;
          #endif
                                                              #if defined(LCD_I2C) || defined(DSP_OLED)
                                                                APPEND_GROUP("group_oled");
                                                              #endif
                                                              #if !defined(HIDE_VU) && !defined(DUMMYDISPLAY)
                                                                APPEND_GROUP("group_vu");
                                                              #endif
            if (BRIGHTNESS_PIN != 255 || nxtn || dbgact)        APPEND_GROUP("group_brightness");
            if (DSP_CAN_FLIPPED || dbgact)                      APPEND_GROUP("group_tft");
            if (TS_MODEL != TS_MODEL_UNDEFINED || dbgact)       APPEND_GROUP("group_touch");
            if (DSP_MODEL == DSP_NOKIA5110)                     APPEND_GROUP("group_nokia");
                                                                APPEND_GROUP("group_timezone");
            if (SHOW_WEATHER || dbgact)                         APPEND_GROUP("group_weather");
                                                                APPEND_GROUP("group_controls");
            if (ENC_BTNL != 255 || ENC2_BTNL != 255 || dbgact)  APPEND_GROUP("group_encoder");
            if (IR_PIN != 255 || dbgact)                        APPEND_GROUP("group_ir");
            if (!psramInit())                                   APPEND_GROUP("group_buffer");
                                                              #if RTCSUPPORTED
                                                                APPEND_GROUP("group_rtc");
                                                              #else
                                                                APPEND_GROUP("group_wortc");
                                                              #endif
          }
          size_t len = strlen(nsBuf);
          if (len > 0 && nsBuf[len - 1] == ',') nsBuf[len - 1] = '\0';
          
          snprintf(wsBuf, sizeof(wsBuf), "{\"act\":[%s]}", nsBuf);
          break;
        }
      case GETINDEX:      {
          requestOnChange(STATION, clientId); 
          requestOnChange(TITLE, clientId); 
          requestOnChange(VOLUME, clientId); 
          requestOnChange(EQUALIZER, clientId); 
          requestOnChange(BALANCE, clientId); 
          requestOnChange(BITRATE, clientId); 
          requestOnChange(MODE, clientId); 
          requestOnChange(SDINIT, clientId);
          requestOnChange(GETPLAYERMODE, clientId); 
          if (config.getMode()==PM_SDCARD) { requestOnChange(SDPOS, clientId); requestOnChange(SDLEN, clientId); requestOnChange(SDSNUFFLE, clientId); } 
          // Таймер сна: pushState при старте мог уйти в пустой эфир — досылаем подключившемуся клиенту.
          sleepTimerPlugin.pushState(clientId);
          return; 
          break;
        }
      case GETSYSTEM:     snprintf (wsBuf, sizeof(wsBuf), "{\"sst\":%d,\"aif\":%d,\"vu\":%d,\"softr\":%d,\"vut\":%d,\"mdns\":\"%s\",\"ipaddr\":\"%s\", \"abuff\": %d, \"telnet\": %d, \"watchdog\": %d }", 
                                  config.store.smartstart != 2, 
                                  config.store.audioinfo, 
                                  config.store.vumeter, 
                                  config.store.softapdelay,
                                  config.vuThreshold,
                                  config.store.mdnsname,
                                  config.ipToStr(WiFi.localIP()),
                                  config.store.abuff,
                                  config.store.telnet,
                                  config.store.watchdog); 
                                  break;
      case GETSCREEN:     snprintf (wsBuf, sizeof(wsBuf), "{\"flip\":%d,\"inv\":%d,\"nump\":%d,\"coven\":%d,\"covopt\":%d,\"tsf\":%d,\"tsd\":%d,\"dspon\":%d,\"br\":%d,\"con\":%d,\"scre\":%d,\"scrt\":%d,\"scrb\":%d,\"scrpe\":%d,\"scrpt\":%d,\"scrpb\":%d}", 
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
                                  config.store.screensaverPlayingBlank);
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
                                  snprintf (wsBuf, sizeof(wsBuf), "{\"tfen\":%d,\"tfkey\":\"%s\",\"tflang\":%d,\"tfcount\":%d,\"tfprovider\":%d}", 
                                  config.store.trackFactsEnabled ? 1 : 0, 
                                  keyHolder.c_str(), 
                                  config.store.trackFactsLang,
                                  config.store.trackFactsCount,
                                  config.store.trackFactsProvider); 
                                  }
                                  break;
      case DSPON:         snprintf (wsBuf, sizeof(wsBuf), "{\"dspontrue\":%d}", 1); break;
      case STATION:       requestOnChange(STATIONNAME, clientId); requestOnChange(ITEM, clientId); break;
      case STATIONNAME: {
        // Во время индексации SD не показываем старую веб-станцию — только статус индексации.
        if (config.sdIndexing) {
          snprintf(wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"nameset\", \"value\": \"%s\"}]}", "Индексация SD...");
        } else if (config.getMode() == PM_SDCARD) {
          // В SD-режиме не шлём старый nameset с веб-радио: берём имя из плейлиста SD или "SD".
          uint16_t idx = config.lastStation();
          uint16_t plen = config.playlistLength();
          const char* nm = (plen > 0 && idx >= 1 && idx <= plen) ? config.stationByNum(idx) : "SD";
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
          snprintf(wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"meta\", \"value\": \"%s\"}]}", config.station.title);
        }
        break;
      }
      // [Gemini3Pro] Remap 0-254 (Internal) to 0-100 (WebUI)
      case VOLUME:        snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"volume\", \"value\": %d}]}", (config.store.volume * 100) / 254); telnet.printf("##CLI.VOL#: %d\n", config.store.volume); break;
      // [Gemini3Pro] Добавлена проверка playerBufMax > 0 для защиты от деления на ноль
      // Процент буфера считаем от реального размера (player.getInBufferSize()), как в [DIAG], чтобы веб/дисплей совпадали с логом.
      case NRSSI:         { uint32_t total = player.getInBufferSize(); int heapPct = (player.isRunning() && config.store.audioinfo && total > 0) ? (int)(100*player.inBufferFilled()/total) : 0; snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"rssi\", \"value\": %d}, {\"id\":\"heap\", \"value\": %d}]}", rssi, heapPct); } break;
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
      case MODE:          snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"playerwrap\", \"value\": \"%s\"}]}", player.status() == PLAYING ? "playing" : "stopped"); telnet.info(); break;
      case EQUALIZER:     snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\":\"bass\", \"value\": %d}, {\"id\": \"middle\", \"value\": %d}, {\"id\": \"trebble\", \"value\": %d}]}", config.store.bass, config.store.middle, config.store.trebble); break;
      case BALANCE:       snprintf (wsBuf, sizeof(wsBuf), "{\"payload\":[{\"id\": \"balance\", \"value\": %d}]}", config.store.balance); break;
      case SDINIT:        snprintf (wsBuf, sizeof(wsBuf), "{\"sdinit\": %d}", SDC_CS!=255); break;
      case GETPLAYERMODE: snprintf (wsBuf, sizeof(wsBuf), "{\"playermode\": \"%s\"}", config.getMode()==PM_SDCARD?"modesd":"modeweb"); break;
      case SDINDEXING:    snprintf (wsBuf, sizeof(wsBuf), "{\"sdindexing\": %d}", config.sdIndexing ? 1 : 0); break;
      #ifdef USE_SD
        case CHANGEMODE: {
          {
            FILE* f = fopen("debug-0da86f.log", "a");
            if (f) {
              // #region agent log
              fprintf(
                f,
                "{\"sessionId\":\"0da86f\",\"runId\":\"run1\",\"hypothesisId\":\"H5\",\"location\":\"netserver.cpp:processQueue:CHANGEMODE\",\"message\":\"web requested mode switch\",\"data\":{\"newConfigMode\":%d,\"currentMode\":%d},\"timestamp\":%lu}\n",
                (int)config.newConfigMode,
                (int)config.getMode(),
                (unsigned long)millis()
              );
              // #endregion
              fclose(f);
            }
          }
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
      // [FIX] Проверяем наличие подключенных клиентов перед отправкой
      if(websocket.count() == 0) return;
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
  // [FIX] Убран early return при SDREADY: при нём не вызывались processQueue(), pingAll(), cleanupClients(),
  // из-за чего веб переставал получать обновления и «умирал» при работе в SD с WiFi (после потери WiFi status=SDREADY).
  // В main-ветке этого return не было — обработка очереди и keepalive должны выполняться всегда.

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
  processQueue();
  // [FIX][SD+WEB] Периодический ПРОТОКОЛЬНЫЙ keepalive для WebSocket.
  // Ранее отправляли текстовый JSON {"pong":1}, но это не WebSocket ping frame.
  // На части мобильных клиентов сокет всё равно уходил в rx timeout (AsyncTCP _poll timeout 4)
  // при отсутствии входящего трафика от браузера.
  //
  // Теперь шлём именно pingAll(), чтобы клиент отвечал pong на уровне протокола WS.
  // Это снижает вероятность "тихого" отвала Web при SD-проигрывании.
  static uint32_t lastWsKeepalive = 0;
  if (websocket.count() > 0 && (millis() - lastWsKeepalive >= 5000)) {
    lastWsKeepalive = millis();
    websocket.pingAll();
  }
  // [FIX][SD+WEB] Не ограничиваем количество клиентов через cleanupClients(2).
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
  snprintf(wsBuf, sizeof(wsBuf), "{\"toast\":\"%s\",\"isErr\":%d}", msg, isError ? 1 : 0);
  websocket.textAll(wsBuf);
}

#if IR_PIN!=255
void NetServer::irToWs(const char* protocol, uint64_t irvalue) {
  wsBuf[0]='\0';
  snprintf (wsBuf, sizeof(wsBuf), "{\"ircode\": %llu, \"protocol\": \"%s\"}", irvalue, protocol);
  websocket.textAll(wsBuf);
}
void NetServer::irValsToWs() {
  if (!irRecordEnable) return;
  wsBuf[0]='\0';
  snprintf (wsBuf, sizeof(wsBuf), "{\"irvals\": [%llu, %llu, %llu]}", config.ircodes.irVals[config.irindex][0], config.ircodes.irVals[config.irindex][1], config.ircodes.irVals[config.irindex][2]);
  websocket.textAll(wsBuf);
}
#endif

void NetServer::onWsMessage(void *arg, uint8_t *data, size_t len, uint8_t clientId) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  // Rate limit incoming WS messages per client to avoid floods
  if (!ws_rate_allowed(clientId)) {
    if (config.store.audioinfo) Serial.printf("[WEBSOCKET] client #%lu rate limit exceeded\n", (unsigned long)clientId);
    return;
  }
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    char local[128];
    size_t safeLen = (len >= sizeof(local)) ? sizeof(local) - 1 : len;
    memcpy(local, data, safeLen);
    local[safeLen] = 0;
    if (config.parseWsCommand(local, _wscmd, sizeof(_wscmd), _wsval, sizeof(_wsval))) {
      if (strcmp(_wscmd, "ping") == 0) {
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
        // [FIX] Бесшовное сохранение: не перезапускаем воспроизведение.
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
  // [FIX] Используем /api/playlist вместо прямого PLAYLIST_PATH.
  // serveStatic("/data/") перехватывал /data/playlist.csv и ВСЕГДА отдавал WEB-плейлист,
  // игнорируя SD-режим. /api/playlist корректно определяет режим и отдаёт нужный файл.
  snprintf(nsBuf, sizeof(nsBuf), "{\"file\": \"http://%s/api/playlist\"}", config.ipToStr(WiFi.localIP()));
  if (clientId == 0) { websocket.textAll(nsBuf); } else { websocket.text(clientId, nsBuf); }
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
  // [FIX] Разрешаем импорт в SD-режиме — запись идёт в PLAYLIST_SD_PATH.
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
    // [FIX] Вместо rename копируем содержимое через open("w").
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
  //   if(uxQueueSpacesAvailable(nsQueue) < 4) return;
  // Идея была в том, чтобы «оставить место для важных команд», но на практике
  // в SD‑режиме при активном WebUI очередь часто оказывалась «почти полной»,
  // и обновления состояния (VUmeter, позиция трека, режим и т.п.)
  // просто не попадали в очередь. В результате WebUI переставал получать данные
  // и визуально «умирал», хотя устройство продолжало играть и отвечать.
  //
  // Новый подход: добавляем только мягкую защиту «очевидного переполнения» —
  // не шлём запрос, если в очереди НЕТ свободных слотов вообще.
  UBaseType_t freeSlots = uxQueueSpacesAvailable(nsQueue);
  if (freeSlots == 0) {
    // Очередь реально забита «под завязку» — аккуратно пропускаем событие.
    return;
  }
  
  // [FIX] Предотвращаем рекурсивный loop() внутри xQueueSend, если мы уже находимся в процессе сетевой обработки.
  // Это может вызвать двойной вызов pbuf_free в lwIP и панику ядра (assert p->ref > 0)
  static bool inRequest = false;
  if (inRequest) return;
  inRequest = true;

  // [FIX][SD+WEB] Формируем событие для WebSocket-очереди.
  nsRequestParams_t nsrequest;
  // [FIX][SD+WEB] Сохраняем тип изменения (meta, sdpos, mode и т.д.).
  nsrequest.type = request;
  // [FIX][SD+WEB] Сохраняем целевой clientId (0 = broadcast всем клиентам).
  nsrequest.clientId = clientId;
  // [FIX][SD+WEB] Неблокирующая отправка: при переполнении просто пропускаем событие,
  // чтобы не блокировать поток, который вызвал requestOnChange (критично для плавного SD-аудио).
  xQueueSend(nsQueue, &nsrequest, NSQ_SEND_DELAY);

  inRequest = false;
}

void NetServer::resetQueue(){
  if(nsQueue!=NULL) xQueueReset(nsQueue);
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  static int freeSpace = 0;
  if(request->url()=="/upload"){
    if (!index) {
      // [FIX] НЕ удаляем playlist/index здесь — файл может быть ещё открыт
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
      // [FIX v3] НЕ отправляем GETINDEX при подключении WS-клиента.
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
    case WS_EVT_DISCONNECT: if (config.store.audioinfo) Serial.printf("[WEBSOCKET] client #%lu disconnected\n", client->id()); break;
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
    snprintf (netserver.nsBuf, sizeof(netserver.nsBuf), "var yoVersion='%s';\nvar formAction='%s';\nvar playMode='%s';\n", YOVERSION, (network.status == CONNECTED && !config.emptyFS)?"webboard":"", (network.status == CONNECTED)?"player":"ap");
    request->send(200, "text/html", netserver.nsBuf);
    return;
  }
  if (strcmp(request->url().c_str(), "/settings.html") == 0 || strcmp(request->url().c_str(), "/update.html") == 0 || strcmp(request->url().c_str(), "/ir.html") == 0){
    //request->send_P(200, "text/html", index_html);
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html);
    response->addHeader("Cache-Control","max-age=31536000");
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
    // [FIX] Отдаём плеер (index) при CONNECTED и при SDREADY (SD без WiFi — пользователь вводит IP для плеера).
    // Редирект на settings только когда реально нужна настройка (AP/нет сети), а не при работе в SD.
    if (network.status == CONNECTED || network.status == SDREADY) {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html);
      response->addHeader("Cache-Control","max-age=31536000");
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
      // [Gemini3Pro] Проверка safterd > 0 (было >= 0), чтобы предотвратить мгновенный сон при вводе 0
      if(sford > 0 && safterd > 0){ request->send(200, "text/plain", ""); config.sleepForAfter(sford, safterd); return; }
    }
    request->send(404, "text/plain", "Not found");
    
  }else{
    request->send(404, "text/plain", "Not found");
  }
}
