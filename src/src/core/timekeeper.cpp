#include "options.h"
#include "Arduino.h"
#include "timekeeper.h"
#include "config.h"
#include "network.h"
#include "display.h"
#include "player.h"
#include "netserver.h"
#include "rtcsupport.h"
#include "../displays/tools/l10n.h"
#include "../pluginsManager/pluginsManager.h"

// [FIX] Extern для флага переключения режимов
extern volatile bool g_modeSwitching;
// Watchdog: время последней активности веб-потока (обновляется из audio_stream_activity)
extern volatile uint32_t g_lastWebStreamActivityMs;
// Сердцебиение главного цикла (core 1). Если долго не обновляется — считаем зависание, делаем WiFi reconnect без перезагрузки.
extern volatile uint32_t g_mainLoopHeartbeatMs;

#ifdef USE_NEXTION
#include "../displays/nextion.h"
#endif
#if DSP_MODEL==DSP_DUMMY
#define DUMMYDISPLAY
#endif

#if RTCSUPPORTED
  //#define TIME_SYNC_INTERVAL  24*60*60*1000
  #define TIME_SYNC_INTERVAL  config.store.timeSyncIntervalRTC*60*60*1000
#else
  #define TIME_SYNC_INTERVAL  config.store.timeSyncInterval*60*1000
#endif
#define WEATHER_SYNC_INTERVAL config.store.weatherSyncInterval*60*1000

#define SYNC_STACK_SIZE       1024 * 4
#define SYNC_TASK_CORE        0
#define SYNC_TASK_PRIORITY    3
#define WEATHER_STRING_L      254

#ifdef HEAP_DBG
  void printHeapFragmentationInfo(const char* title){
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    float fragmentation = 100.0 * (1.0 - ((float)largestBlock / (float)freeHeap));
    Serial.printf("\n****** %s ******\n", title);
    Serial.printf("* Free heap: %u bytes\n", freeHeap);
    Serial.printf("* Largest free block: %u bytes\n", largestBlock);
    Serial.printf("* Fragmentation: %.2f%%\n", fragmentation);
    Serial.printf("*************************************\n\n");
  }
  #define HEAP_INFO() printHeapFragmentationInfo(__PRETTY_FUNCTION__)
#else
  #define HEAP_INFO()
#endif

TimeKeeper timekeeper;

void _syncTask(void *pvParameters) {
  if (timekeeper.forceWeather && timekeeper.forceTimeSync) {
    timekeeper.timeTask();
    timekeeper.weatherTask();
  } 
  else if (timekeeper.forceWeather) {
    timekeeper.weatherTask();
  }
  else if (timekeeper.forceTimeSync) {
    timekeeper.timeTask();
  }
  timekeeper.busy = false;
  vTaskDelete(NULL);
}

TimeKeeper::TimeKeeper(){
  busy          = false;
  forceWeather  = true;
  forceTimeSync = true;
  _returnPlayerTime = _doAfterTime = 0;
  weatherBuf=NULL;
  #if (DSP_MODEL!=DSP_DUMMY || defined(USE_NEXTION)) && !defined(HIDE_WEATHER)
    weatherBuf = (char *) malloc(sizeof(char) * WEATHER_STRING_L);
    memset(weatherBuf, 0, WEATHER_STRING_L);
  #endif
}

bool TimeKeeper::loop0(){ // core0 (display)
  if (network.status != CONNECTED) return true;
  uint32_t currentTime = millis();
  static uint32_t _last1s = 0;
  static uint32_t _last2s = 0;
  static uint32_t _last5s = 0;
  if (currentTime - _last1s >= 1000) { // 1sec
    _last1s = currentTime;
//#ifndef DUMMYDISPLAY
#if !defined(DUMMYDISPLAY) || defined(USE_NEXTION)
  #ifndef UPCLOCK_CORE1
    _upClock();
  #endif
#endif
  }
  if (currentTime - _last2s >= 2000) { // 2sec
    _last2s = currentTime;
    _upRSSI();
  }
  if (currentTime - _last5s >= 5000) { // 5sec
    _last5s = currentTime;
    //HEAP_INFO();
  }

  // [FIX] Проверка зависания главного цикла.
  // Перезагрузка ТОЛЬКО если main loop не обновлялся >60 с И аудио-стрим тоже мёртв.
  // Если стрим активен (g_lastWebStreamActivityMs свежий), значит устройство работает,
  // а main loop просто заблокирован на lwIP-мьютексе из-за параллельного SSL-запроса —
  // это временная ситуация, и перезагрузка только ухудшит UX.
  static uint32_t _lastFreezeCheckMs = 0;
  static uint32_t _lastFreezeRecoveryMs = 0;
  if (currentTime - _lastFreezeCheckMs >= 15000) {
    _lastFreezeCheckMs = currentTime;
    uint32_t loopAge = currentTime - g_mainLoopHeartbeatMs;
    uint32_t streamAge = currentTime - g_lastWebStreamActivityMs;
    if (WiFi.status() == WL_CONNECTED &&
        loopAge > 60000 &&
        streamAge > 60000 &&
        (currentTime - _lastFreezeRecoveryMs) > 120000) {
      _lastFreezeRecoveryMs = currentTime;
      Serial.printf("[FREEZE-RECOVERY] Main loop stuck %lus, stream silent %lus — rebooting\n",
                    (unsigned long)(loopAge / 1000),
                    (unsigned long)(streamAge / 1000));
      delay(50);
      ESP.restart();
    }
    // НЕ логируем «почти зависание» из loop0(): это та же задача дисплея (core 0), что и плеер —
    // Serial.printf здесь блокирует CPU/UART и даёт щелчки/артефакты в звуке.
  }

  return true; // just in case
}

bool TimeKeeper::loop1(){ // core1 (player)
  uint32_t currentTime = millis();
  static uint32_t _last1s = 0;
  static uint32_t _last2s = 0;
  if (currentTime - _last1s >= 1000) { // 1sec
    pluginsManager::getInstance().on_ticker();
    resumeDeferredCoverDownload(); // отложенная обложка после окна станции / когда Facts SSL освободился
    _last1s = currentTime;
//#ifndef DUMMYDISPLAY
#if !defined(DUMMYDISPLAY) || defined(USE_NEXTION)
  #ifdef UPCLOCK_CORE1
    _upClock();
  #endif
#endif
    _upScreensaver();
    _upSDPos();
    _returnPlayer();
    _doAfterWait();
    _doWatchDog(); // Проверка веб-потока: при зависании — переподключение (если watchdog включён)
  }
  if (currentTime - _last2s >= 2000) { // 2sec
    _last2s = currentTime;
  }

  //#ifdef DUMMYDISPLAY
  #if defined(DUMMYDISPLAY) && !defined(USE_NEXTION)
  return true;
  #endif
  // Sync weather & time
  static uint32_t lastWeatherTime = 0;
  if (currentTime - lastWeatherTime >= WEATHER_SYNC_INTERVAL) {
    lastWeatherTime = currentTime;
    forceWeather = true;
  }
  static uint32_t lastTimeTime = 0;
  if (currentTime - lastTimeTime >= TIME_SYNC_INTERVAL) {
    lastTimeTime = currentTime;
    forceTimeSync = true;
  }
  // [FIX] Блокируем sync при переключении режимов - это вызывало crash
  if (!busy && !g_modeSwitching && (forceWeather || forceTimeSync) && network.status == CONNECTED) {
    busy = true;
    //config.setTimeConf();
    xTaskCreatePinnedToCore(
      _syncTask,
      "syncTask",
      SYNC_STACK_SIZE,
      NULL,           // Params
      SYNC_TASK_PRIORITY,
      NULL,           // Descriptor
      SYNC_TASK_CORE
    );
  }
  
  return true; // just in case
}

void TimeKeeper::waitAndReturnPlayer(uint8_t time_s){
  _returnPlayerTime = millis()+time_s*1000;
}
void TimeKeeper::_returnPlayer(){
  if(_returnPlayerTime>0 && millis()>=_returnPlayerTime){
    _returnPlayerTime = 0;
    display.putRequest(NEWMODE, PLAYER);
  }
}

void TimeKeeper::waitAndDo(uint8_t time_s, void (*callback)()){
  _doAfterTime = millis()+time_s*1000;
  _aftercallback = callback;
}
void TimeKeeper::_doAfterWait(){
  if(_doAfterTime>0 && millis()>=_doAfterTime){
    _doAfterTime = 0;
    _aftercallback();
  }
}

void TimeKeeper::_upClock(){
#if RTCSUPPORTED
  if(config.isRTCFound()) rtc.getTime(&network.timeinfo);
#else
  if(network.timeinfo.tm_year>100 || network.status == SDREADY) {
    network.timeinfo.tm_sec++;
    mktime(&network.timeinfo);
  }
#endif
  if(display.ready()) display.putRequest(CLOCK);
}

void TimeKeeper::_upScreensaver(){
#ifndef DSP_LCD
  if(!display.ready()) return;
  if(config.store.screensaverEnabled && display.mode()==PLAYER && !player.isRunning()){
    config.screensaverTicks++;
    if(config.screensaverTicks > config.store.screensaverTimeout+SCREENSAVERSTARTUPDELAY){
      if(config.store.screensaverBlank){
        display.putRequest(NEWMODE, SCREENBLANK);
      }else{
        display.putRequest(NEWMODE, SCREENSAVER);
      }
      config.screensaverTicks=SCREENSAVERSTARTUPDELAY;
    }
  }
  if(config.store.screensaverPlayingEnabled && display.mode()==PLAYER && player.isRunning()){
    config.screensaverPlayingTicks++;
    if(config.screensaverPlayingTicks > config.store.screensaverPlayingTimeout*60+SCREENSAVERSTARTUPDELAY){
      if(config.store.screensaverPlayingBlank){
        display.putRequest(NEWMODE, SCREENBLANK);
      }else{
        display.putRequest(NEWMODE, SCREENSAVER);
      }
      config.screensaverPlayingTicks=SCREENSAVERSTARTUPDELAY;
    }
  }
#endif
}

// Интервал проверки watchdog (сек); таймаут «поток мёртв» (мс) — переподключаем, если нет данных дольше.
#define WATCHDOG_CHECK_INTERVAL_SEC  30
#define WATCHDOG_STREAM_TIMEOUT_MS   45000

void TimeKeeper::_doWatchDog(){
  // Включён ли вообще watchdog в настройках.
  if(!config.store.watchdog) return;

  // Работает только в WEB‑режиме (SD‑режим отслеживается отдельно).
  if(config.getMode() != PM_WEB) return;

  // [FIX SmartStart] Если wasPlaying=false, значит пользователь/таймер явно остановил плеер.
  // В этом случае ничего не перезапускаем и тем более не перелистываем станции.
  if(!config.store.wasPlaying) return;

  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if(now - lastCheck < (uint32_t)WATCHDOG_CHECK_INTERVAL_SEC * 1000) return;
  lastCheck = now;

  // Если относительно недавно приходили данные по веб‑потоку — всё в порядке.
  if(now - g_lastWebStreamActivityMs <= WATCHDOG_STREAM_TIMEOUT_MS) return;

  // Долго не было данных — считаем поток зависшим.
  // По договорённости НЕ меняем станцию автоматически (чтобы пользователь не думал,
  // что «радио само перескакивает»), а просто даём повторный старт текущей.
  uint16_t st = config.lastStation();
  if(st == 0) return;

  Serial.printf("[Watchdog] Поток без данных >%us, переподключение станции %u\n",
                (unsigned)(WATCHDOG_STREAM_TIMEOUT_MS / 1000), (unsigned)st);

  // Сбрасываем таймер активности, чтобы не триггериться повторно сразу же:
  // успешный рестарт обновит g_lastWebStreamActivityMs через audio_stream_activity().
  g_lastWebStreamActivityMs = now;

  // Перезапускаем текущую станцию (аналогично ручному старту).
  player.sendCommand({PR_PLAY, (int)st});
}

void TimeKeeper::_upRSSI(){
  if(network.status == CONNECTED){
    netserver.setRSSI(WiFi.RSSI());
    netserver.requestOnChange(NRSSI, 0);
    if(display.ready()) display.putRequest(DSPRSSI, netserver.getRSSI());
  }
#ifdef USE_SD
  if(display.mode()!=SDCHANGE) player.sendCommand({PR_CHECKSD, 0});
#endif
  player.sendCommand({PR_VUTONUS, 0});
}

void TimeKeeper::_upSDPos(){
  // Отправляем SDPOS только раз в 2 секунды чтобы не перегружать WebSocket
  static uint32_t lastSdPosUpdate = 0;
  uint32_t now = millis();
  if(now - lastSdPosUpdate < 2000) return;
  lastSdPosUpdate = now;
  if(player.isRunning() && config.getMode()==PM_SDCARD) netserver.requestOnChange(SDPOS, 0);
}

void TimeKeeper::timeTask(){
  static uint8_t tsFailCnt = 0;
  config.waitConnection();
  if(getLocalTime(&network.timeinfo)){
    tsFailCnt = 0;
    forceTimeSync = false;
    mktime(&network.timeinfo);
    display.putRequest(CLOCK, 1);
    network.requestTimeSync(true);
    #if RTCSUPPORTED
      if (config.isRTCFound()) rtc.setTime(&network.timeinfo);
    #endif
  }else{
    if(tsFailCnt<4){
      forceTimeSync = true;
      tsFailCnt++;
    }else{
      forceTimeSync = false;
      tsFailCnt=0;
    }
  }
}
void TimeKeeper::weatherTask(){
  forceWeather = false;
  if(!weatherBuf || strlen(config.store.weatherkey)==0 || !config.store.showweather) return;
  _getWeather();
}

bool _getWeather() {
#if (DSP_MODEL!=DSP_DUMMY || defined(USE_NEXTION)) && !defined(HIDE_WEATHER)
  static AsyncClient * weatherClient = NULL;
  static const char* host = "api.openweathermap.org";
  if(weatherClient) return false;
  weatherClient = new AsyncClient();
  if(!weatherClient) return false;

  weatherClient->onError([](void * arg, AsyncClient * client, int error){
    Serial.println("##WEATHER###: connection error");
    weatherClient = NULL;
    delete client;
  }, NULL);

  weatherClient->onConnect([](void * arg, AsyncClient * client){
    weatherClient->onError(NULL, NULL);
    weatherClient->onDisconnect([](void * arg, AsyncClient * c){ weatherClient = NULL; delete c; }, NULL);
    
    char httpget[280] = {0};
    snprintf(httpget, sizeof(httpget),
         "GET /data/2.5/weather?lat=%s&lon=%s&units=%s&lang=%s&appid=%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
         config.store.weatherlat, config.store.weatherlon, "metric", "ru", config.store.weatherkey, host);
    client->write(httpget);
    
    client->onData([](void * arg, AsyncClient * c, void * data, size_t len){
      char *d = (char*)data;
      // Погода от OpenWeatherMap обычно помещается в один-два пакета.
      // Ищем начало JSON (открывающая фигурная скобка)
      char *bodyStart = strstr(d, "{");
      if (bodyStart == NULL) return; // Пропускаем пакеты с заголовками без тела

      char line[1024];
      size_t copyLen = (len > 1023) ? 1023 : len;
      memcpy(line, d, copyLen);
      line[copyLen] = '\0';

      /* parse it */
      char *cursor;
      char desc[120], icon[5];
      float tempf, tempfl, wind_speed;
      int hum, press, wind_deg;
      bool result = true;
      char stanc[60] = {0};
      float wind_gust = 0.0;
      char gust_buf[24] = {0};

      cursor = strstr(line, "\"description\":\"");
      if (cursor) { sscanf(cursor, "\"description\":\"%119[^\"]", desc); }else{ result=false; }
      cursor = strstr(line, "\"icon\":\"");
      if (cursor) { sscanf(cursor, "\"icon\":\"%4[^\"]", icon); }else{ result=false; }
      cursor = strstr(line, "\"temp\":");
      if (cursor) { sscanf(cursor, "\"temp\":%f", &tempf); }else{ result=false; }
      cursor = strstr(line, "\"pressure\":");
      if (cursor) { sscanf(cursor, "\"pressure\":%d", &press); }else{ result=false; }
      cursor = strstr(line, "\"humidity\":");
      if (cursor) { sscanf(cursor, "\"humidity\":%d", &hum); }else{ result=false; }
      cursor = strstr(line, "\"feels_like\":");
      if (cursor) { sscanf(cursor, "\"feels_like\":%f", &tempfl); }else{ result=false; }
      cursor = strstr(line, "\"grnd_level\":");
      if (cursor) { sscanf(cursor, "\"grnd_level\":%d", &press); }
      cursor = strstr(line, "\"speed\":");
      if (cursor) { sscanf(cursor, "\"speed\":%f", &wind_speed); }else{ result=false; }
      cursor = strstr(line, "\"gust\":");
      if (cursor) { sscanf(cursor, "\"gust\":%f", &wind_gust); }
      cursor = strstr(line, "\"deg\":");
      if (cursor) { sscanf(cursor, "\"deg\":%d", &wind_deg); }else{ result=false; }
      cursor = strstr(line, "\"name\":\"");
      if (cursor) { sscanf(cursor, "\"name\":\"%59[^\"]", stanc); }
      
      if(!result) return;
      
      press = press / 1.333; // hPa -> mmHg
      
      if (wind_gust > 0.1) {
          snprintf(gust_buf, sizeof(gust_buf), "%s%.0f", LANG::prv, wind_gust);
      }

      #ifdef USE_NEXTION
        // ... Nextion logic omitted ...
      #endif
      
      Serial.printf("##WEATHER###: description: %s, temp:%.1f C, pressure:%dmmHg, humidity:%d%%, wind: %d\n", desc, tempf, press, hum, (int)(wind_deg/22.5));
      #if EXT_WEATHER
        snprintf(timekeeper.weatherBuf, WEATHER_STRING_L, LANG::weatherFmt, desc, tempf, tempfl, press, hum, LANG::wind[(int)(wind_deg/22.5)], wind_speed, gust_buf, stanc);
      #else
        snprintf(timekeeper.weatherBuf, WEATHER_STRING_L, LANG::weatherFmt, desc, tempf, press, hum);
      #endif

      display.putRequest(NEWWEATHER);
    }, NULL); // <-- client->onData
  }, NULL); // <-- weatherClient->onConnect
  config.waitConnection();
  if(!weatherClient->connect(host, 80)){
    Serial.println("##WEATHER###: connection failed");
    AsyncClient * client = weatherClient;
    weatherClient = NULL;
    delete client;
  }

  return true;
#endif // if (DSP_MODEL!=DSP_DUMMY || defined(USE_NEXTION)) && !defined(HIDE_WEATHER)
  return false;
}

//******************
