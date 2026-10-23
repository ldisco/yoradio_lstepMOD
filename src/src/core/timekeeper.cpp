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
#include "../plugins/TrackFacts/TrackFacts.h"
#if USE_BT_I2S
#include "bt_i2s.h"
#endif
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// CoverDL и SNTP не должны одновременно дергать lwIP DNS/UDP (IDF 5.x: assert udp_new_ip_type).
extern bool g_coverTaskRunning;
extern TrackFacts trackFactsPlugin;
extern volatile uint32_t g_wifiInstabilityGraceUntilMs;

// Extern для флага переключения режимов
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

#define SYNC_STACK_SIZE       1024 * 8
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
  bool doWeather = false;
  if (timekeeper.forceWeather && timekeeper.forceTimeSync) {
    timekeeper.timeTask();
    doWeather = true;
  } 
  else if (timekeeper.forceWeather) {
    doWeather = true;
  }
  else if (timekeeper.forceTimeSync) {
    timekeeper.timeTask();
  }
  if (doWeather) {
    timekeeper.forceWeather = false;
  }
  // HTTP погоды — только после sync/SNTP и с busy=false (лог: Cover «SNTP/time sync», connect timeout 3000 ms).
  timekeeper.busy = false;
  if (doWeather) {
    vTaskDelay(pdMS_TO_TICKS(300));
    timekeeper.weatherTask();
  }
  vTaskDelete(NULL);
}

TimeKeeper::TimeKeeper(){
  busy          = false;
  forceWeather  = true;
  forceTimeSync = true;
  _returnPlayerTime = _doAfterTime = 0;
  weatherBuf=NULL;
  weatherIcon[0] = '\0';
  weatherCity[0] = '\0';
  /* Температура ещё не получена — на скринсейвере покажем только город. */
  weatherTemp = 0.0f;
  weatherTempValid = false;
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

  // Проверка зависания главного цикла (возврат к более живучему recovery-подходу).
  // Играет + loop и поток молчат >30 с — reboot. Без PLAYING «тишина» нормальна (обрыв Wi‑Fi, [RETRY]).
  // Вне PLAYING reboot только если loop мёртв >90 с. Карантин после ASSOC_LEAVE — g_wifiInstabilityGraceUntilMs.
  static uint32_t _lastFreezeCheckMs = 0;
  static uint32_t _lastFreezeRecoveryMs = 0;
  if (currentTime - _lastFreezeCheckMs >= 15000) {
    _lastFreezeCheckMs = currentTime;
    // Сердцебиение loop() пишется на Core 1, проверка — на Core 0: одно чтение volatile на тик.
    const uint32_t loopHb = g_mainLoopHeartbeatMs;
    const uint32_t streamHb = g_lastWebStreamActivityMs;
    // Гонка ядер: loopHb чуть «впереди» millis() здесь → (currentTime - loopHb) как uint32
    // даёт ~UINT32_MAX → ложный reboot при живом потоке (в логе «stuck 4294967s», stream silent 0s).
    if (loopHb != 0 && loopHb <= currentTime) {
      uint32_t loopAge = currentTime - loopHb;
      // Возраст потока — только при валидной метке (та же защита от underflow).
      uint32_t streamAge = (streamHb != 0 && streamHb <= currentTime)
                           ? (currentTime - streamHb)
                           : 0;
      const bool wifiOk = (WiFi.status() == WL_CONNECTED) && !network.beginReconnect
                          && (currentTime >= g_wifiInstabilityGraceUntilMs);
      // Во время ALL OFF light-sleep цикл намеренно блокирует loop() на Core1 —
      // это не зависание. Сердцебиение там обновляется вручную; на всякий случай
      // полностью отключаем FREEZE-RECOVERY, пока поднят g_sleepTimerShutdown.
      extern volatile bool g_sleepTimerShutdown;
      if (g_sleepTimerShutdown) {
        return true;
      }
      const bool loopStuck = (loopAge > 30000);
      const bool streamStuck = (player.status() == PLAYING) && (streamHb != 0)
                               && (streamAge > 30000);
      const bool hardLoopFreeze = (loopAge > 90000);
      const bool shouldReboot = wifiOk && loopStuck && (streamStuck || hardLoopFreeze)
                                && ((currentTime - _lastFreezeRecoveryMs) > 120000);
      if (shouldReboot) {
        _lastFreezeRecoveryMs = currentTime;
        Serial.printf("[FREEZE-RECOVERY] Main loop stuck %lus, stream silent %lus, playing=%d — rebooting\n",
                      (unsigned long)(loopAge / 1000),
                      (unsigned long)(streamAge / 1000),
                      (int)(player.status() == PLAYING));
        // Сообщаем пользователю перед перезагрузкой — WebSocket на core 0 ещё работает.
        // Задержка 500 мс даёт AsyncTCP доставить сообщение в браузер до рестарта.
        netserver.sendToast("Перезагрузка: зависание обнаружено...", true);
        delay(500);
        ESP.restart();
      }
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
  /*
   * Запуск syncTask: throttle + проверка xTaskCreate.
   * Раньше busy=true ставился до create без проверки OK → при FAIL залипал навсегда;
   * timeTask() при раннем return с forceTimeSync=true → шторм create/delete на core0.
   */
  if (!busy && !g_modeSwitching && (forceWeather || forceTimeSync) && network.status == CONNECTED) {
    static uint32_t lastSyncLaunchMs = 0;
    // Не чаще раза в 2 с — даже если forceTimeSync постоянно true.
    if (currentTime - lastSyncLaunchMs >= 2000u) {
      busy = true;
      BaseType_t syncOk = xTaskCreatePinnedToCore(
        _syncTask,
        "syncTask",
        SYNC_STACK_SIZE,
        NULL,
        SYNC_TASK_PRIORITY,
        NULL,
        SYNC_TASK_CORE
      );
      if (syncOk != pdPASS) {
        // Нехватка heap — не держим busy, иначе _play() ждёт до потолка и SNTP мёртв.
        busy = false;
        Serial.println("[TimeKeeper] syncTask create FAIL — busy сброшен");
      } else {
        lastSyncLaunchMs = currentTime;
      }
    }
  }
  
  return true; // just in case
}

void TimeKeeper::waitAndReturnPlayer(uint8_t time_s){
  _returnPlayerTime = millis()+time_s*1000;
}
void TimeKeeper::_returnPlayer(){
  if(_returnPlayerTime>0 && millis()>=_returnPlayerTime){
    _returnPlayerTime = 0;
    /* Уже ушли с диалога — не вооружать sticky «вхолостую». */
    displayMode_e m = display.mode();
    if (m != STATIONS && m != VOL && m != NUMBERS
        && m != INFO && m != SETTINGS && m != TIMEZONE && m != WIFI) {
      return;
    }
    display.armStickyLeave();
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
  // BT: ESP-плеер STOPPED, но звук с QCC5125 — считаем «играет» для таймера SS.
  bool srcPlaying = player.isRunning();
#if USE_BT_I2S
  if (config.getMode() == PM_BT) {
    srcPlaying = btI2s.isBtPlaying();
  }
#endif
  if(config.store.screensaverEnabled && display.mode()==PLAYER && !srcPlaying){
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
  if(config.store.screensaverPlayingEnabled && display.mode()==PLAYER && srcPlaying){
    config.screensaverPlayingTicks++;
    /* playing-скринсейвер: таймаут в секундах (как idle screensaver), не в минутах */
    if(config.screensaverPlayingTicks > config.store.screensaverPlayingTimeout+SCREENSAVERSTARTUPDELAY){
#if DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_ST7789 || DSP_MODEL==DSP_AXS15231B
      /* blank ON → гашение (SCREENBLANK без Cover). blank OFF + Display Cover → Cover-SS.
         blank OFF + Cover OFF → обычный SCREENSAVER (часы). */
      if(config.store.screensaverPlayingBlank){
        display.putRequest(NEWMODE, SCREENBLANK);
      }else if(isDisplayCoversEnabled()){
        display.putRequest(NEWMODE, SCREENBLANK);
      }else{
        display.putRequest(NEWMODE, SCREENSAVER);
      }
#else
      if(config.store.screensaverPlayingBlank){
        display.putRequest(NEWMODE, SCREENBLANK);
      }else{
        display.putRequest(NEWMODE, SCREENSAVER);
      }
#endif
      config.screensaverPlayingTicks=SCREENSAVERSTARTUPDELAY;
    }
  }
#endif
}

// Интервал проверки watchdog (сек); таймаут «поток мёртв» (мс) — переподключаем, если нет данных дольше.
// Пользовательский запрос: уменьшить ожидание до ~20 секунд, чтобы не «висеть» на дохлой станции.
// Частоту проверки делаем 5 сек, чтобы реакция была близка к порогу таймаута.
#define WATCHDOG_CHECK_INTERVAL_SEC  5
#define WATCHDOG_STREAM_TIMEOUT_MS   20000
// После стольких watchdog-ретраев подряд — STOP и wasPlaying=false (v0.8.197).
#define WATCHDOG_MAX_RETRIES_PER_STATION 2

void TimeKeeper::_doWatchDog(){
  // Локальный антизацикливатель watchdog по конкретной станции.
  static uint16_t wdStationId = 0;      // Станция, для которой считаем watchdog-ретраи.
  static uint8_t  wdRetryCount = 0;     // Количество watchdog-ретраев подряд по wdStationId.
  static uint8_t  wdPrevStatus = STOPPED; // Предыдущий статус плеера (для сброса при новом Play).
  static uint32_t wdPlayStartMs = 0;    // Момент перехода в PLAYING — grace до первой проверки.

  // Включён ли вообще watchdog в настройках.
  if(!config.store.watchdog) return;

  // Работает только в WEB‑режиме (SD‑режим отслеживается отдельно).
  if(config.getMode() != PM_WEB) {
    wdStationId = 0;
    wdRetryCount = 0;
    return;
  }

  // Если wasPlaying=false, значит пользователь/таймер явно остановил плеер.
  if(!config.store.wasPlaying) return;

  uint32_t now = millis();
  // Новый старт (в т.ч. ручной Play на ту же станцию): сброс счётчика, иначе retry=5 → мгновенный STOP.
  uint8_t plSt = player.status();
  if(plSt == PLAYING && wdPrevStatus != PLAYING) {
    wdPlayStartMs = now;
    wdRetryCount = 0;
    wdStationId = config.lastStation();
  }
  wdPrevStatus = plSt;

  static uint32_t lastCheck = 0;
  if(now - lastCheck < (uint32_t)WATCHDOG_CHECK_INTERVAL_SEC * 1000) return;
  lastCheck = now;

  // Watchdog только при реальном PLAYING: на connect/SmartStart плеер ещё STOPPED,
  // а g_lastWebStreamActivityMs==0 даёт ложное «тишина >20с» и шторм лишних PR_PLAY поверх connecttohost().
  if (plSt != PLAYING) return;

  /*
   * Если декодер уже остановлен, восстановлением владеет auto-retry
   * из main.cpp. Если Audio сам выполняет Stream lost reconnect, _play() занят
   * DNS/TLS или coordinated PR_PLAY уже стоит в очереди — watchdog только наблюдает.
   * Это устраняет прежнюю гонку watchdog × auto-retry × internal httpPrint.
   */
  if (!player.isRunning()
      || !player.connproc
      || player.webRecoveryPending()
      || player.isWebStreamRecovering()) {
    return;
  }

  // До истечения таймаута после Play не считаем ретраи (парсинг заголовков, icy-br).
  if(plSt == PLAYING && (now - wdPlayStartMs) < WATCHDOG_STREAM_TIMEOUT_MS) return;

  // Реальные данные потока (audio_stream_activity) — станция жива.
  if(now - g_lastWebStreamActivityMs <= WATCHDOG_STREAM_TIMEOUT_MS) {
    wdRetryCount = 0;
    return;
  }

  uint16_t st = config.lastStation();
  if(st == 0) return;

  if(wdStationId != st) {
    wdStationId = st;
    wdRetryCount = 0;
  }

  /*
   * После диагностического лимита не бросаем живой, но беззвучный
   * поток навсегда. Общий координатор сам выдержит cooldown; здесь лишь реже
   * предупреждаем, не посылая PR_STOP и не меняя wasPlaying.
   */
  if (wdRetryCount >= WATCHDOG_MAX_RETRIES_PER_STATION) {
    static uint32_t lastWdWarnMs = 0;
    if (now - lastWdWarnMs >= 60000) {
      lastWdWarnMs = now;
      Serial.printf("[Watchdog] Станция %u долго без данных (retry=%u), переподключение продолжается\n",
                    (unsigned)st, (unsigned)wdRetryCount);
    }
  }

  if (player.requestWebRecovery("watchdog")) {
    if(wdRetryCount < 255) wdRetryCount++;
    Serial.printf("[Watchdog] Поток без данных >%us, coordinated recovery станции %u (попытка %u)\n",
                  (unsigned)(WATCHDOG_STREAM_TIMEOUT_MS / 1000),
                  (unsigned)st, (unsigned)wdRetryCount);
  }
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
  // Пока CoverDL/Facts в iTunes/HTTPS — не трогаем SNTP (лог: assert udp_new_ip_type / sys_untimeout).
  const uint32_t netWait0 = millis();
  while ((g_coverTaskRunning || trackFactsPlugin.isRequestActive())
         && (millis() - netWait0) < 120000) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // SNTP настраиваем только здесь (не в config.init до Wi‑Fi).
  // Gate isWebStreamReadyForBackgroundNet убран:
  // SNTP идёт после WiFi; mutex с Cover/Facts — цикл ожидания выше.
  config.setTimeConf();
  config.waitConnection();
  // До 15 с ждём ответ NTP (по умолчанию Arduino — 5 с, на загруженном lwIP мало).
  if (getLocalTime(&network.timeinfo, 15000)) {
    if (network.timeinfo.tm_year + 1900 >= 2020) {
      g_ntpTimeSyncedOnce = true;
    }
    tsFailCnt = 0;
    forceTimeSync = false;
    mktime(&network.timeinfo);
    display.putRequest(CLOCK, 1);
    network.requestTimeSync(true);
    #if CORE_DEBUG_LEVEL > 0
    Serial.printf("[NTP] Sync OK: %04d-%02d-%02d %02d:%02d:%02d\n",
                  network.timeinfo.tm_year + 1900,
                  network.timeinfo.tm_mon + 1,
                  network.timeinfo.tm_mday,
                  network.timeinfo.tm_hour,
                  network.timeinfo.tm_min,
                  network.timeinfo.tm_sec);
    #endif
    #if RTCSUPPORTED
      if (config.isRTCFound()) rtc.setTime(&network.timeinfo);
    #endif
    // Фоновый SNTP после успешного getLocalTime() — гасим до следующего forceTimeSync.
    pauseLwIpBackgroundSntp();
  } else {
    if (tsFailCnt < 4) {
      forceTimeSync = true;
      tsFailCnt++;
      #if CORE_DEBUG_LEVEL > 0
      Serial.printf("[NTP] Sync FAIL (retry %u/4), SNTP left running\n", (unsigned)tsFailCnt);
      #endif
    } else {
      forceTimeSync = false;
      tsFailCnt = 0;
      pauseLwIpBackgroundSntp();
    }
  }
}
void TimeKeeper::weatherTask(){
  forceWeather = false;
  if(!weatherBuf || strlen(config.store.weatherkey)==0 || !config.store.showweather) return;
  // Не параллелить Weather HTTPS с CoverDL, окном станции и Facts (лог: SSL OOM -32512 рядом с Gemini 503).
  const uint32_t coverWait0 = millis();
  while ((g_coverTaskRunning || isStationSwitchSslBlock() || trackFactsPlugin.isRequestActive())
         && (millis() - coverWait0) < 120000) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  if (g_coverTaskRunning || isStationSwitchSslBlock() || trackFactsPlugin.isRequestActive()) return;
  // Gate по % буфера убран — погода без лишнего ожидания буфера;
  // ждём только Cover/Facts/окно станции (цикл и проверки выше).
  _getWeather();
}

bool _getWeather() {
#if (DSP_MODEL!=DSP_DUMMY || defined(USE_NEXTION)) && !defined(HIDE_WEATHER)
  config.waitConnection();
  // TOCTOU-gate isWebStreamReadyForBackgroundNet убран.
  // Как CoverDL: гасим фоновый SNTP перед HTTPS/DNS (assert udp_new_ip_type, IDF 5.x).
  pauseLwIpBackgroundSntpForUserNet();

  // Браузер/PC ходят по HTTPS; HTTP:80 с ESP даёт DNS OK + client available timeout (порт режется/не отвечает).
  char url[320] = {0};
  snprintf(url, sizeof(url),
           "https://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s&units=metric&lang=ru&appid=%s",
           config.store.weatherlat, config.store.weatherlon, config.store.weatherkey);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  secureClient.setTimeout(8000);
  secureClient.setHandshakeTimeout(8);

  HTTPClient http;
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(secureClient, url)) {
    Serial.println("##WEATHER###: HTTP begin failed");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("##WEATHER###: HTTP code=%d\n", code);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  char line[1024];
  size_t pos = 0;
  unsigned long timeout = millis();
  while ((http.connected() || (stream && stream->available())) && pos < sizeof(line) - 1) {
    while (stream && stream->available() && pos < sizeof(line) - 1) {
      line[pos++] = (char)stream->read();
    }
    if (strstr(line, "\"temp\"") != NULL) break;
    if ((millis() - timeout) > 8000UL) {
      Serial.println("##WEATHER###: read timeout");
      http.end();
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  line[pos] = '\0';
  http.end();

  if (strstr(line, "{") == NULL || strstr(line, "\"temp\"") == NULL) {
    Serial.println("##WEATHER###: weather not found !");
    return false;
  }

  /* parse it — без изменений от copy2/231 */
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

  if(!result) return false;

  press = press / 1.333; // hPa -> mmHg

  if (wind_gust > 0.1) {
      snprintf(gust_buf, sizeof(gust_buf), "%s%.0f", LANG::prv, wind_gust);
  }

  #ifdef USE_NEXTION
    // ... Nextion logic omitted ...
  #endif

  Serial.printf("##WEATHER###: description: %s, temp:%.1f C, pressure:%dmmHg, humidity:%d%%, wind: %d\n", desc, tempf, press, hum, (int)(wind_deg/22.5));
  /* Сохраняем код иконки OWM для cover-скринсейвера (ST7789_170): рисуем bitmap под часами. */
  strlcpy(timekeeper.weatherIcon, icon, sizeof(timekeeper.weatherIcon));
  strlcpy(timekeeper.weatherCity, stanc, sizeof(timekeeper.weatherCity));
  /* Температура для подписи города на скринсейвере NV3041A («Москва, +28°C»). */
  timekeeper.weatherTemp = tempf;
  timekeeper.weatherTempValid = true;
  #if EXT_WEATHER
    snprintf(timekeeper.weatherBuf, WEATHER_STRING_L, LANG::weatherFmt, desc, tempf, tempfl, press, hum, LANG::wind[(int)(wind_deg/22.5)], wind_speed, gust_buf, stanc);
  #else
    snprintf(timekeeper.weatherBuf, WEATHER_STRING_L, LANG::weatherFmt, desc, tempf, press, hum);
  #endif

  display.putRequest(NEWWEATHER);
  return true;
#endif // if (DSP_MODEL!=DSP_DUMMY || defined(USE_NEXTION)) && !defined(HIDE_WEATHER)
  return false;
}

//******************
