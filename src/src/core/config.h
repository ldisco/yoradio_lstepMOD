#ifndef config_h
#define config_h
#pragma once
#include "Arduino.h"
#include <SPI.h>
#include <LittleFS.h>
#include <EEPROM.h>
#include "../displays/widgets/widgetsconfig.h" //BitrateFormat

// EEPROM Configuration
// 28.02.2026: Увеличен с 768 до 1024 байт из-за переполнения после добавления TrackFacts
// config_t занимает ~310 байт, доступно 1024-500=524 байт (запас 214 байт для будущих расширений)
// При дальнейшем добавлении полей следить чтобы sizeof(config_t) < (EEPROM_SIZE - EEPROM_START)
#define EEPROM_SIZE       1024
#define EEPROM_START      500
#define EEPROM_START_IR   0
#define EEPROM_START_2    10
#define PLAYLIST_PATH     "/data/playlist.csv"
#define SSIDS_PATH        "/data/wifi.csv"
#define TMP_PATH          "/data/tmpfile.txt"
#define FAV_TMP_PATH      "/data/fav_tmp.txt"  // Временный файл только для setFavorite (не конфликтовать с netserver)
#define INDEX_PATH        "/data/index.dat"

#define PLAYLIST_SD_PATH     "/data/playlistsd.csv"
#define INDEX_SD_PATH        "/data/indexsd.dat"

#define REAL_PLAYL   config.getMode()==PM_WEB?PLAYLIST_PATH:PLAYLIST_SD_PATH
#define REAL_INDEX   config.getMode()==PM_WEB?INDEX_PATH:INDEX_SD_PATH

#define MAX_PLAY_MODE   1
#define WEATHERKEY_LENGTH 58
#define GEMINI_KEY_LENGTH 60
#define MDNS_LENGTH 24

#ifndef BUFLEN
  #define BUFLEN            250
#endif

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  #define ESP_ARDUINO_3 1
#endif

#define CONFIG_VERSION  8

/** Время последней активности веб-потока (мс), обновляется из audio_stream_activity. Используется watchdog'ом. */
extern volatile uint32_t g_lastWebStreamActivityMs;
/** true после audio_info «format is flac» до prepareForPlaying — доп. признак FLAC для TrackFacts, если BF_* ещё не совпал. */
extern volatile bool g_audioFormatFlacActive;

enum playMode_e      : uint8_t  { PM_WEB=0, PM_SDCARD=1 };

void u8fix(char *src);

void checkAllTasksStack();

struct theme_t {
  uint16_t background;
  uint16_t meta;
  uint16_t metabg;
  uint16_t metafill;
  uint16_t title1;
  uint16_t title2;
  uint16_t digit;
  uint16_t div;
  uint16_t weather;
  uint16_t vumax;
  uint16_t vumin;
  uint16_t clock;
  uint16_t clockbg;
  uint16_t seconds;
  uint16_t dow;
  uint16_t date;
  uint16_t heap;
  uint16_t buffer;
  uint16_t ip;
  uint16_t vol;
  uint16_t rssi;
  uint16_t bitrate;
  uint16_t volbarout;
  uint16_t volbarin;
  uint16_t plcurrent;
  uint16_t plcurrentbg;
  uint16_t plcurrentfill;
  uint16_t playlist[5];
};
struct config_t
{
  uint16_t  config_set; //must be 4262
  uint16_t  version;
  uint8_t   volume;
  int8_t    balance;
  int8_t    trebble;
  int8_t    middle;
  int8_t    bass;
  uint16_t  lastStation;
  uint16_t  countStation;
  uint8_t   lastSSID;
  bool      audioinfo;
  uint8_t   smartstart;
  int8_t    tzHour;
  int8_t    tzMin;
  uint16_t  timezoneOffset;
  bool      vumeter;
  uint8_t   softapdelay;
  bool      flipscreen;
  bool      invertdisplay;
  bool      numplaylist;
  bool      fliptouch;
  bool      dbgtouch;
  bool      dspon;
  uint8_t   brightness;
  uint8_t   contrast;
  char      sntp1[35];
  char      sntp2[35];
  bool      showweather;
  char      weatherlat[10];
  char      weatherlon[10];
  char      weatherkey[WEATHERKEY_LENGTH];
  uint16_t  _reserved;
  uint16_t  lastSdStation;
  bool      sdsnuffle;
  uint8_t   volsteps;
  uint16_t  encacc;
  uint8_t   play_mode;  //0 WEB, 1 SD
  uint8_t   irtlp;
  bool      btnpullup;
  uint16_t  btnlongpress;
  uint16_t  btnclickticks;
  uint16_t  btnpressticks;
  bool      encpullup;
  bool      enchalf;
  bool      enc2pullup;
  bool      enc2half;
  bool      forcemono;
  bool      i2sinternal;
  bool      rotate90;
  bool      screensaverEnabled;
  uint16_t  screensaverTimeout;
  bool      screensaverBlank;
  bool      screensaverPlayingEnabled;
  uint16_t  screensaverPlayingTimeout;
  bool      screensaverPlayingBlank;
  char      mdnsname[24];
  bool      skipPlaylistUpDown;
  uint16_t  abuff;
  bool      telnet;
  bool      watchdog;
  uint16_t  timeSyncInterval;
  uint16_t  timeSyncIntervalRTC;
  uint16_t  weatherSyncInterval;
  // TrackFacts settings
  bool      trackFactsEnabled;
  char      geminiApiKey[GEMINI_KEY_LENGTH];
  uint8_t   trackFactsLang;  // 0=Russian, 1=English, 2=Auto
  uint8_t   trackFactsCount; // 1-5 facts per track
  uint8_t   trackFactsProvider; // 0=Gemini, 1=DeepSeek, 2=iTunes(default), 3=LastFM, 4=MusicBrainz
  // Sleep Timer: сохраняем состояние переключателя ALL OFF между перезагрузками
  bool      sleepTimerAllOff;  // true = аппаратное отключение питания по таймеру
  // [FIX SmartStart] Флаг состояния воспроизведения, отделённый от пользовательской настройки smartstart.
  // wasPlaying=true  — плеер играл до последнего выключения/сбоя (разрешаем автостарт).
  // wasPlaying=false — пользователь или таймер сна нажал Стоп (автостарт запрещён).
  // Это решает баг "иногда автостартует после user-stop":
  // раньше smartstart=1 никогда не сбрасывался при ручной остановке.
  bool      wasPlaying;
  // Переключение next/prev только по избранным станциям (5-я колонка CSV = "1")
  bool      favOnly;
};

#if IR_PIN!=255
struct ircodes_t
{
  unsigned int ir_set; //must be 4224
  uint64_t irVals[20][3];
};
#endif

struct station_t
{
  char name[BUFLEN];
  char url[BUFLEN];
  char title[BUFLEN];
  uint16_t bitrate;
  int  ovol;
};

struct neworkItem
{
  char ssid[30];
  char password[40];
};

class Config {
  public:
    config_t store;
    station_t station;
    theme_t   theme;
#if IR_PIN!=255
    int irindex;
    uint8_t irchck;
    ircodes_t ircodes;
#endif
    BitrateFormat configFmt = BF_UNKNOWN;
    neworkItem ssids[5];
    uint8_t ssidsCount;
    uint16_t sleepfor;
    uint32_t sdResumePos;
    bool     emptyFS;
    bool     sdIndexing;  // флаг для отображения статуса индексации SD в веб
    uint16_t vuThreshold;
    uint16_t screensaverTicks;
    uint16_t screensaverPlayingTicks;
    bool     isScreensaver;
    int      newConfigMode;
    char      tmpBuf[BUFLEN];
    char     tmpBuf2[BUFLEN];
    char       ipBuf[16];
    char _stationBuf[BUFLEN/2];
  public:
    Config() {};
    //void save();
#if IR_PIN!=255
    void saveIR();
#endif
    void init();
    void loadTheme();
    uint8_t setVolume(uint8_t val);
    void saveVolume();
    void setTone(int8_t bass, int8_t middle, int8_t trebble);
    void setBalance(int8_t balance);
    uint8_t setLastStation(uint16_t val);
    uint8_t setCountStation(uint16_t val);
    uint8_t setLastSSID(uint8_t val);
    void setTitle(const char* title);
    void setStation(const char* station);
    void escapeQuotes(const char* input, char* output, size_t maxLen);
    bool parseCSV(const char* line, char* name, char* url, int &ovol);
    bool parseJSON(const char* line, char* name, char* url, int &ovol);
    bool parseWsCommand(const char* line, char* cmd, size_t cmdSize, char* val, size_t valSize);
    bool parseSsid(const char* line, char* ssid, char* pass);
    /** Загрузить станцию по номеру. useSDPlaylist=true — всегда брать плейлист SD (для перехода WEB→SD без гонки getMode() на другом ядре). */
    bool loadStation(uint16_t station, bool useSDPlaylist = false);
    bool initNetwork();
    bool saveWifi();
    void setTimeConf();
    bool saveWifiFromNextion(const char* post);
    void setSmartStart(uint8_t ss);
    void setBitrateFormat(BitrateFormat fmt) { configFmt = fmt; }
    // Текущий формат аудиопотока (AAC/MP3/FLAC/...). Нужен для безопасных ограничений
    // тяжёлых фоновых задач на "тяжёлых" форматах (например FLAC), чтобы не ловить фризы.
    BitrateFormat getBitrateFormat() const { return configFmt; }
    void initPlaylist();
    void indexPlaylist();
    void indexSDPlaylistFile();
    /** Строит индекс из буфера плейлиста (без удержания lock на время разбора). Lock только на запись. */
    void indexSDPlaylistFromBuffer(const char* buf, size_t size);
    /** Запросить отложенную перестройку индекса SD (после сохранения избранного). Вызов из loop — processSDIndexRebuild(). */
    void requestSDIndexRebuild();
    /** Выполнить отложенную перестройку индекса SD, если была запрошена. Вызывать из основного loop(). */
    void processSDIndexRebuild();
    void initSDPlaylist();
    void changeMode(int newmode=-1);
    uint16_t playlistLength();
    uint16_t lastStation(){
      return getMode()==PM_WEB?store.lastStation:store.lastSdStation;
    }
    void lastStation(uint16_t newstation){
      if(getMode()==PM_WEB) saveValue(&store.lastStation, newstation);
      else saveValue(&store.lastSdStation, newstation);
    }
    char * stationByNum(uint16_t num);
    // Проверяет, является ли станция избранной (5-я колонка CSV = "1")
    bool isFavorite(uint16_t num);
    bool setFavorite(uint16_t num, bool fav); // Обновляет признак избранного для указанной строки плейлиста.
    /** Поставить в очередь сохранение избранного. Выполняется в фоновой задаче FavUpdate — веб не блокируется. */
    void requestFavoriteUpdate(uint16_t num, bool fav);
    /** Оставлено для совместимости; реально сохранение делает задача FavUpdate. */
    void processFavoriteUpdate();
    // Ищет следующую/предыдущую избранную станцию. Возвращает 0, если избранных нет.
    uint16_t findNextFavorite(uint16_t current, bool forward);
    void setTimezone(int8_t tzh, int8_t tzm);
    void setTimezoneOffset(uint16_t tzo);
    uint16_t getTimezoneOffset();
    void setBrightness(bool dosave=false);
    void setDspOn(bool dspon, bool saveval = true);
    void sleepForAfter(uint16_t sleepfor, uint16_t sleepafter=0);
    void bootInfo();
    void doSleepW();
    void setSnuffle(bool sn);
    uint8_t getMode() { return store.play_mode/* & 0b11*/; }
    void initPlaylistMode();
    void reset();
    void enableScreensaver(bool val);
    void setScreensaverTimeout(uint16_t val);
    void setScreensaverBlank(bool val);
    void setScreensaverPlayingEnabled(bool val);
    void setScreensaverPlayingTimeout(uint16_t val);
    void setScreensaverPlayingBlank(bool val);
    void setSntpOne(const char *val);
    void setShowweather(bool val);
    void setWeatherKey(const char *val);
    void setTrackFactsEnabled(bool val);
    void setGeminiApiKey(const char *val);
    void setTrackFactsLang(uint8_t val);
    void setTrackFactsCount(uint8_t val);
    void setTrackFactsProvider(uint8_t val);
    void setSDpos(uint32_t val);
#if IR_PIN!=255
    void setIrBtn(int val);
#endif
    void resetSystem(const char *val, uint8_t clientId);
    bool LittleFSCleanup();
    void waitConnection();
    char * ipToStr(IPAddress ip);
    /** forceSDPlaylist=true — загружать из SD-плейлиста независимо от getMode() (при переходе WEB→SD). */
    bool prepareForPlaying(uint16_t stationId, bool forceSDPlaylist = false);
    void configPostPlaying(uint16_t stationId);
    /** WEB: если станция не прислала тег в течение таймаута, заменяет "[соединение]" на имя станции. */
    void updateConnectTitleFallback();
#ifdef USE_SD
    FS* SDPLFS();
#else
    FS* SDPLFS(){ return &LittleFS; }
#endif
    bool isRTCFound(){ return _rtcFound; };
    template <typename T>
    size_t getAddr(const T *field) const {
      return (size_t)((const uint8_t *)field - (const uint8_t *)&store) + EEPROM_START;
    }
    template <typename T>
    void saveValue(T *field, const T &value, bool commit=true, bool force=false){
      if(*field == value && !force) return;
      *field = value;
      size_t address = getAddr(field);
      EEPROM.put(address, value);
      if(commit)
        EEPROM.commit();
    }
    void saveValue(char *field, const char *value, size_t N, bool commit=true, bool force=false) {
      // Сначала копируем в буфер в памяти
      memset(field, 0, N);
      size_t valueLen = strlen(value);
      if (valueLen >= N) {
        Serial.printf("[saveValue] Предупреждение: строка превышает допустимую длину (%zu >= %zu)\n", valueLen, N);
        valueLen = N - 1;
      }
      // Копируем БЕЗ null-терминатора, заполняем нулями остальное
      if (valueLen > 0) {
        memcpy(field, value, valueLen);
      }
      
      size_t address = getAddr(field);
      // Записываем ВСЕ N байт в EEPROM (включая нулевое заполнение в конце)
      for (size_t i = 0; i < N; i++) {
        EEPROM.write(address + i, (uint8_t)field[i]);
      }
      if(commit)
        EEPROM.commit();
      if (field == store.geminiApiKey || field == store.weatherkey) {
        Serial.printf("[saveValue] Saving value: (secret) len=%zu, EEPROM bytes=%zu\n", valueLen, N);
      } else {
        Serial.printf("[saveValue] Saving value: '%s' (len=%zu, EEPROM bytes=%zu)\n", value, valueLen, N);
      }
    }
    uint32_t getChipId(){
      uint32_t chipId = 0;
      for(int i=0; i<17; i=i+8) {
        chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
      }
      return chipId;
    }
  private:
    template <class T> int eepromWrite(int ee, const T& value);
    template <class T> int eepromRead(int ee, T& value);
    bool _bootDone;
    bool _rtcFound;
    FS* _SDplaylistFS;
    void setDefaults();
    static void doSleep();
    uint16_t color565(uint8_t r, uint8_t g, uint8_t b);
    void _setupVersion();
    void _initHW();
    bool _isFSempty();
    uint16_t _randomStation(){
      randomSeed(esp_random() ^ millis());
      uint16_t station = random(1, store.countStation);
      return station;
    }
};


extern Config config;
extern String currentCoverUrl;
extern String streamCoverUrl;
extern String metaTitle;
extern String metaArtist;
extern String currentFact;
extern bool littleFsReady;
extern SemaphoreHandle_t littleFsMutex;

// === PSRAM-кэш для WebUI-ассетов ===
// Буферы ниже заполняются один раз при старте (Config::init) и далее используются
// веб-сервером для быстрой отдачи статики из PSRAM, не трогая LittleFS/SD.
// [FIX v2] Кэшируем ВСЕ ключевые файлы, а не только script.js/style.css.
// Это критично для мобильных браузеров: параллельные запросы к LittleFS
// создают задержки > 5 сек → WebSocket таймаутит → Web «умирает».
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

// Инициализация PSRAM-кэша веб-страниц (читаем файлы из LittleFS в PSRAM).
// Вызывается из Config::init после успешного монтирования LittleFS.
void initWebAssetsCache();

// === Асинхронное переключение режимов (WEB <-> SD) ===
// Критично для стабильности WebUI:
// Переключение режима может включать тяжёлые операции (mount SD, индексация),
// поэтому нельзя выполнять Config::changeMode() прямо внутри обработчика WebSocket
// (NetServer::processQueue) — иначе Web «умирает» из-за блокировки event-loop.
//
// Эта функция планирует переключение режима в отдельной FreeRTOS-задаче и
// возвращает управление сразу, чтобы Web продолжал обслуживаться.
bool scheduleChangeModeTask(int newmode);

// Функции для потокобезопасного доступа к LittleFS
inline bool lockLittleFS(uint32_t timeoutMs = 1000) {
  if (littleFsMutex == NULL) return false;
  return xSemaphoreTake(littleFsMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

inline void unlockLittleFS() {
  if (littleFsMutex != NULL) {
    xSemaphoreGive(littleFsMutex);
  }
}

#if DSP_HSPI || TS_HSPI || VS_HSPI
extern SPIClass  SPI2;
#endif

// [FIX] Cooldown период после переключения режимов
// Блокирует сетевые операции (обложки, факты) на 30 сек после переключения WEB<->SD
extern volatile bool g_modeSwitching;
extern volatile unsigned long g_modeSwitchTime;
// [FIX] Короткий cooldown после переключения станции (внутри WEB режима).
// Нужен для мягкой стабилизации потока до запуска тяжёлых SSL-задач плагинов.
extern volatile bool g_stationSwitching;
extern volatile unsigned long g_stationSwitchTime;
// Сердцебиение главного цикла (обновляется в loop()). Задача на core 0: если >35 сек нет обновления — зависание, делаем WiFi reconnect без перезагрузки.
extern volatile uint32_t g_mainLoopHeartbeatMs;
bool isModeSwitchCooldown();
bool isStationSwitchCooldown();
bool isStationSwitchSslBlock(); // true в окне жёсткого post-switch карантина: не запускать SSL для обложек/фактов
bool isPlaylistBusy();  // true во время индексации SD или сохранения избранного (не запускать обложки/факты)
bool isSafeForSSL();   // Проверка heap, WiFi, cooldown перед SSL
bool isSafeForSSLForFacts();  // Проверка heap/WiFi/cooldown перед SSL для TrackFacts

bool coverCacheExists(const String& key);
String coverCacheUrlForKey(const String& key);
// После завершения SSL в TrackFacts — запустить отложенную загрузку обложки (если была отложена).
void resumeDeferredCoverDownload(void);
// Runtime-тоггл показа обложек на TFT (доступен только если DISPLAY_COVERS_ENABLE=true в myoptions.h).
bool isDisplayCoversAllowedByBuild();
bool isDisplayCoversEnabled();
void setDisplayCoversEnabled(bool enabled);

#endif
