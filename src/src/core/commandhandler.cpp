#include <Arduino.h>
#include "options.h"
#include "commandhandler.h"
#include "player.h"
#include "display.h"
#include "netserver.h"
#include "config.h"
#include "controls.h"
#include "telnet.h"
#include "../plugins/TrackFacts/TrackFacts.h"
#include "../plugins/SleepTimer/SleepTimer.h"

#if DSP_MODEL==DSP_DUMMY
#define DUMMYDISPLAY
#endif

CommandHandler cmd;

bool CommandHandler::exec(const char *command, const char *value, uint8_t cid) {
  if (strEquals(command, "start"))    { player.sendCommand({PR_PLAY, config.lastStation()}); return true; }
  if (strEquals(command, "stop"))     { player.sendCommand({PR_STOP, 0}); return true; }
  if (strEquals(command, "toggle"))   { player.toggle(); return true; }
  if (strEquals(command, "prev"))     { player.prev(); return true; }
  if (strEquals(command, "next"))     { player.next(); return true; }
  if (strEquals(command, "volm"))     { player.stepVol(false); return true; }
  if (strEquals(command, "volp"))     { player.stepVol(true); return true; }
#ifdef USE_SD
  if (strEquals(command, "mode"))     { config.changeMode(atoi(value)); return true; }
#endif
  if (strEquals(command, "reset") && cid==0)    { config.reset(); return true; }
  if (strEquals(command, "ballance")) { int bal = atoi(value); bal = (bal < 0) ? 0 : ((bal > 32) ? 32 : bal); config.setBalance(bal); return true; }
  if (strEquals(command, "playstation") || strEquals(command, "play")){ 
    int id = atoi(value);
    if (id < 1) id = 1;
    uint16_t cs = config.playlistLength();
    if (cs == 0) cs = 1;
    if (id > (int)cs) id = (int)cs;
    player.sendCommand({PR_PLAY, (uint16_t)id});
    return true;
  }
  if (strEquals(command, "vol")){
    int v = atoi(value);
    v = (v < 0) ? 0 : ((v > 254) ? 254 : v);
    config.saveValue(&config.store.volume, (uint8_t)v);
    player.setVol((uint8_t)v);
    return true;
  }
  if (strEquals(command, "dspon"))     { config.setDspOn(atoi(value)!=0); return true; }
  if (strEquals(command, "dim"))       { int d=atoi(value); config.saveValue(&config.store.brightness, (uint8_t)(d < 0 ? 0 : (d > 100 ? 100 : d)), false); config.setBrightness(true); return true; }
  if (strEquals(command, "clearLittleFS")){ config.LittleFSCleanup(); config.saveValue(&config.store.play_mode, static_cast<uint8_t>(PM_WEB)); return true; }
  /*********************************************/
  /****************** WEBSOCKET ****************/
  /*********************************************/
  if (strEquals(command, "getindex"))  {
    // Важно: не отправляем websocket.textAll() напрямую из контекста WS-команды
    // (через sleepTimerPlugin.pushState), чтобы не ловить реэнтерабельные подвисания.
    netserver.requestOnChange(GETINDEX, cid);
    return true;
  }

  if (strEquals(command, "getsystem"))  {
    netserver.requestOnChange(GETSYSTEM, cid);
    return true;
  }
  if (strEquals(command, "getscreen"))  { netserver.requestOnChange(GETSCREEN, cid); return true; }
  if (strEquals(command, "gettimezone")){ netserver.requestOnChange(GETTIMEZONE, cid); return true; }
  if (strEquals(command, "getcontrols")){ netserver.requestOnChange(GETCONTROLS, cid); return true; }
  if (strEquals(command, "getweather")) { netserver.requestOnChange(GETWEATHER, cid); return true; }
  if (strEquals(command, "getactive"))  { netserver.requestOnChange(GETACTIVE, cid); return true; }
  if (strEquals(command, "newmode"))    { config.newConfigMode = atoi(value); netserver.requestOnChange(CHANGEMODE, cid); return true; }
  
  if (strEquals(command, "invertdisplay")){ config.saveValue(&config.store.invertdisplay, static_cast<bool>(atoi(value))); display.invert(); return true; }
  if (strEquals(command, "numplaylist"))  { config.saveValue(&config.store.numplaylist, static_cast<bool>(atoi(value))); display.putRequest(NEWMODE, CLEAR); display.putRequest(NEWMODE, PLAYER); return true; }
  if (strEquals(command, "fliptouch"))    { config.saveValue(&config.store.fliptouch, static_cast<bool>(atoi(value))); flipTS(); return true; }
  if (strEquals(command, "dbgtouch"))     { config.saveValue(&config.store.dbgtouch, static_cast<bool>(atoi(value))); return true; }
  if (strEquals(command, "flipscreen"))   { config.saveValue(&config.store.flipscreen, static_cast<bool>(atoi(value))); display.flip(); display.putRequest(NEWMODE, CLEAR); display.putRequest(NEWMODE, PLAYER); return true; }
  if (strEquals(command, "displaycover")) { setDisplayCoversEnabled(static_cast<bool>(atoi(value))); netserver.requestOnChange(GETSCREEN, cid); return true; }
  if (strEquals(command, "brightness"))   { if (!config.store.dspon) netserver.requestOnChange(DSPON, 0); config.saveValue(&config.store.brightness, static_cast<uint8_t>(atoi(value)), false); config.setBrightness(true); return true; }
  if (strEquals(command, "screenon"))     { config.setDspOn(static_cast<bool>(atoi(value))); return true; }
  if (strEquals(command, "contrast"))     { config.saveValue(&config.store.contrast, static_cast<uint8_t>(atoi(value))); display.setContrast(); return true; }
  if (strEquals(command, "screensaverenabled")){ config.enableScreensaver(static_cast<bool>(atoi(value))); return true; }
  if (strEquals(command, "screensavertimeout")){ config.setScreensaverTimeout(static_cast<uint16_t>(atoi(value))); return true; }
  if (strEquals(command, "screensaverblank"))  { config.setScreensaverBlank(static_cast<bool>(atoi(value))); return true; }
  if (strEquals(command, "screensaverplayingenabled")){ config.setScreensaverPlayingEnabled(static_cast<bool>(atoi(value))); return true; }
  if (strEquals(command, "screensaverplayingtimeout")){ config.setScreensaverPlayingTimeout(static_cast<uint16_t>(atoi(value))); return true; }
  if (strEquals(command, "screensaverplayingblank"))  { config.setScreensaverPlayingBlank(static_cast<bool>(atoi(value))); return true; }
  if (strEquals(command, "abuff")){ config.saveValue(&config.store.abuff, static_cast<uint16_t>(atoi(value))); return true; }
  if (strEquals(command, "telnet")){ config.saveValue(&config.store.telnet, static_cast<bool>(atoi(value))); telnet.toggle(); return true; }
  if (strEquals(command, "watchdog")){ config.saveValue(&config.store.watchdog, static_cast<bool>(atoi(value))); return true; }
  
  if (strEquals(command, "tzh"))        { config.saveValue(&config.store.tzHour, static_cast<int8_t>(atoi(value))); return true; }
  if (strEquals(command, "tzm"))        { config.saveValue(&config.store.tzMin, static_cast<int8_t>(atoi(value))); return true; }
  if (strEquals(command, "sntp2"))      { config.saveValue(config.store.sntp2, value, 35, false); return true; }
  if (strEquals(command, "sntp1"))      { config.setSntpOne(value); return true; }
  if (strEquals(command, "timeint"))    { config.saveValue(&config.store.timeSyncInterval, static_cast<uint16_t>(atoi(value))); return true; }
  if (strEquals(command, "timeintrtc")) { config.saveValue(&config.store.timeSyncIntervalRTC, static_cast<uint16_t>(atoi(value))); return true; }
  
  if (strEquals(command, "volsteps"))         { config.saveValue(&config.store.volsteps, static_cast<uint8_t>(atoi(value))); return true; }
  if (strEquals(command, "encacc"))  { setEncAcceleration(static_cast<uint16_t>(atoi(value))); return true; }
  if (strEquals(command, "irtlp"))            { setIRTolerance(static_cast<uint8_t>(atoi(value))); return true; }
  if (strEquals(command, "oneclickswitching")){ config.saveValue(&config.store.skipPlaylistUpDown, static_cast<bool>(atoi(value))); return true; }
  // Переключение next/prev только по избранным станциям
  if (strEquals(command, "favonly")){ config.saveValue(&config.store.favOnly, static_cast<bool>(atoi(value))); return true; }
  if (strEquals(command, "showweather"))      { config.setShowweather(static_cast<bool>(atoi(value))); return true; }
  if (strEquals(command, "lat"))              { config.saveValue(config.store.weatherlat, value, 10, false); return true; }
  if (strEquals(command, "lon"))              { config.saveValue(config.store.weatherlon, value, 10, false); return true; }
  if (strEquals(command, "key"))              { config.setWeatherKey(value); return true; }
  if (strEquals(command, "wint"))  { config.saveValue(&config.store.weatherSyncInterval, static_cast<uint16_t>(atoi(value))); return true; }
  
  if (strEquals(command, "volume"))  { 
      int v = atoi(value);
      // [Gemini3Pro] Remap 0-100 (WebUI) to 0-254 (Internal)
      v = (v * 254) / 100;
      if (v > 254) v = 254;
      player.setVol(static_cast<uint8_t>(v)); 
      return true; 
  }
  if (strEquals(command, "sdpos"))   { config.setSDpos(static_cast<uint32_t>(atoi(value))); return true; }
  if (strEquals(command, "snuffle")) { config.setSnuffle(strcmp(value, "true") == 0); return true; }
  if (strEquals(command, "balance")) { config.setBalance(static_cast<uint8_t>(atoi(value))); return true; }
  // [FIX] Защита от двойной перезагрузки: если из настроек отправляют и reboot, и boot,
  // или кнопка срабатывает дважды — игнорируем второй вызов в течение 3 секунд.
  if (strEquals(command, "reboot") || strEquals(command, "boot")) {
    static uint32_t s_lastRebootRequest = 0;
    uint32_t now = millis();
    if (s_lastRebootRequest != 0 && (now - s_lastRebootRequest) < 3000) {
      return true; // уже перезагружаемся, второй вызов игнорируем
    }
    s_lastRebootRequest = now;
    ESP.restart();
    return true;
  }
  if (strEquals(command, "format"))  { LittleFS.format(); ESP.restart(); return true; }
  if (strEquals(command, "submitplaylist"))  { player.sendCommand({PR_STOP, 0}); return true; }
  // [FIX] Тихое сохранение (toggle favorite) — не останавливаем воспроизведение.
  if (strEquals(command, "submitplaylistsilent"))  { return true; }
  if (strEquals(command, "favset"))  {
    char local[32]; // Локальный буфер для безопасного разбора параметра.
    strlcpy(local, value, sizeof(local)); // Копируем строку вида "id,value".
    char *comma = strchr(local, ','); // Ищем разделитель между ID и значением.
    if (!comma) return true; // Если формат некорректный — выходим без ошибок.
    *comma = '\0'; // Разделяем строку на две части.
    uint16_t itemId = static_cast<uint16_t>(atoi(local)); // Преобразуем ID в число.
    int fav = atoi(comma + 1); // Преобразуем значение избранного в число.
    // [FIX] Не вызываем setFavorite() здесь — он держит lockLittleFS и долго пишет в FS,
    // из-за чего WebSocket-обработчик блокируется, музыка фризит, веб «умирает».
    // Ставим задачу в очередь; выполнится в основном loop() через processFavoriteUpdate().
    config.requestFavoriteUpdate(itemId, fav != 0);
    return true; // Команда обработана.
  }

#if IR_PIN!=255
  if (strEquals(command, "irbtn"))  { config.setIrBtn(atoi(value)); return true; }
  if (strEquals(command, "chkid"))  { config.irchck = static_cast<uint8_t>(atoi(value)); return true; }
  if (strEquals(command, "irclr"))  { config.ircodes.irVals[config.irindex][static_cast<uint8_t>(atoi(value))] = 0; return true; }
#endif
  if (strEquals(command, "reset"))  { config.resetSystem(value, cid); return true; }

  if (strEquals(command, "smartstart")){
    // SmartStart хранится как явный флаг пользователя:
    // 1 = включено, 2 = выключено. Значение 0 больше не используем.
    uint8_t ss = atoi(value) == 1 ? 1 : 2;
    config.setSmartStart(ss);
    return true;
  }
  if (strEquals(command, "sleeptimerstep")) { sleepTimerPlugin.cyclePreset(); return true; }
  if (strEquals(command, "sleeptimeralloff")) { sleepTimerPlugin.setAllOffEnabled(atoi(value) != 0); return true; }
  if (strEquals(command, "audioinfo")) { config.saveValue(&config.store.audioinfo, static_cast<bool>(atoi(value))); display.putRequest(AUDIOINFO); return true; }
  if (strEquals(command, "vumeter"))   { config.saveValue(&config.store.vumeter, static_cast<bool>(atoi(value))); display.putRequest(SHOWVUMETER); return true; }
  if (strEquals(command, "softap"))    { config.saveValue(&config.store.softapdelay, static_cast<uint8_t>(atoi(value))); return true; }
  if (strEquals(command, "mdnsname"))  { config.saveValue(config.store.mdnsname, value, MDNS_LENGTH); return true; }
  if (strEquals(command, "rebootmdns")){
    if(strlen(config.store.mdnsname)>0) snprintf(config.tmpBuf, sizeof(config.tmpBuf), "{\"redirect\": \"http://%s.local/settings.html\"}", config.store.mdnsname);
    else snprintf(config.tmpBuf, sizeof(config.tmpBuf), "{\"redirect\": \"http://%s/settings.html\"}", config.ipToStr(WiFi.localIP()));
    websocket.text(cid, config.tmpBuf); vTaskDelay(pdMS_TO_TICKS(500)); ESP.restart();
    return true;
  }
  
  // TrackFacts commands
  extern TrackFacts trackFactsPlugin;
  if (strEquals(command, "trackfactsenabled")) { 
    Serial.printf("[CMD] trackfactsenabled=%s\n", value);
    config.setTrackFactsEnabled(static_cast<bool>(atoi(value)));
    trackFactsPlugin.setEnabled(config.store.trackFactsEnabled);
    return true; 
  }
  if (strEquals(command, "trackfactslang")) { 
    Serial.printf("[CMD] trackfactslang=%s\n", value);
    config.setTrackFactsLang(static_cast<uint8_t>(atoi(value)));
    trackFactsPlugin.setLanguage(static_cast<FactLanguage>(config.store.trackFactsLang));
    return true; 
  }
  if (strEquals(command, "trackfactscount")) { 
    Serial.printf("[CMD] trackfactscount=%s\n", value);
    config.setTrackFactsCount(static_cast<uint8_t>(atoi(value)));
    trackFactsPlugin.setFactCount(config.store.trackFactsCount);
    return true; 
  }
  if (strEquals(command, "trackfactsprovider")) {
    Serial.printf("[CMD] trackfactsprovider=%s\n", value);
    config.setTrackFactsProvider(static_cast<uint8_t>(atoi(value)));
    trackFactsPlugin.setProvider(config.store.trackFactsProvider);
    return true;
  }
  if (strEquals(command, "applytrackfacts")) { 
    if (strEquals(value, "********")) {
      Serial.println("[CMD] applytrackfacts received placeholder, ignoring key update");
      return true;
    }
    config.setGeminiApiKey(value);
    trackFactsPlugin.setGeminiApiKey(config.store.geminiApiKey);
    return true; 
  }
  if (strEquals(command, "gettrackfacts")) { 
    netserver.requestOnChange(GETTRACKFACTS, cid);
    return true; 
  }
  if (strEquals(command, "trackfactsrequest")) {
    // [v0.4.2] Ручной запрос факта из WebUI. 
    // [FIX] Если TrackFacts выключен — игнорируем запрос
    if (!trackFactsPlugin.isEnabled()) return true;
    // [FIX] Показываем подтверждение только если запрос РЕАЛЬНО поставлен в очередь.
    // Если факт уже есть в RAM/файловом кэше — не спамим всплывающим "ожидание...".
    String rejectReason;
    if (trackFactsPlugin.requestManualFact(&rejectReason)) {
      trackFactsPlugin.sendStatus("Запрос факта принят. Ожидание...");
    } else if (rejectReason.length() > 0 &&
               rejectReason.indexOf("уже доступен") < 0 &&
               rejectReason.indexOf("из файлового кэша") < 0 &&
               rejectReason.indexOf("уже выполняется") < 0) {
      // Отправляем красный toast только для реального отказа запуска.
      // Информационные причины ("уже есть факт"/"уже выполняется") не считаем ошибкой.
      trackFactsPlugin.sendStatus(rejectReason.c_str(), true);
    }
    return true;
  }
  
  return false;
}


