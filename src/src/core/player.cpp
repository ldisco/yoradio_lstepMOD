#include "options.h"
#include "player.h"
#include "volume_curve.h" // [Gemini3Pro] Подключаем таблицу логарифмической громкости
#include "config.h"
#include "telnet.h"
#include "display.h"
#include "sdmanager.h"
#include "netserver.h"
#include "timekeeper.h"
#include "../displays/tools/l10n.h"
#include "../pluginsManager/pluginsManager.h"
#ifdef USE_NEXTION
#include "../displays/nextion.h"
#endif
Player player;
QueueHandle_t playerQueue;

#if VS1053_CS!=255 && !I2S_INTERNAL
  #if VS_HSPI
    Player::Player(): Audio(VS1053_CS, VS1053_DCS, VS1053_DREQ, &SPI2) {}
  #else
    Player::Player(): Audio(VS1053_CS, VS1053_DCS, VS1053_DREQ, &SPI) {}
  #endif
  void ResetChip(){
    pinMode(VS1053_RST, OUTPUT);
    digitalWrite(VS1053_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(30));
    digitalWrite(VS1053_RST, HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
#else
  #if !I2S_INTERNAL
    Player::Player() {}
  #else
    Player::Player(): Audio(true, I2S_DAC_CHANNEL_BOTH_EN)  {}
  #endif
#endif


void Player::init() {
  Serial.print("##[BOOT]#\tplayer.init\t");
  playerQueue=NULL;
  _resumeFilePos = 0;
  _hasError=false;
  playerQueue = xQueueCreate( 5, sizeof( playerRequestParams_t ) );
  setOutputPins(false);
  vTaskDelay(pdMS_TO_TICKS(50));
#ifdef MQTT_ROOT_TOPIC
  memset(burl, 0, MQTT_BURL_SIZE);
#endif
  if(MUTE_PIN!=255) pinMode(MUTE_PIN, OUTPUT);
  #if I2S_DOUT!=255
    #if !I2S_INTERNAL
      setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    #endif
  #else
    SPI.begin();
    if(VS1053_RST>0) ResetChip();
    begin();
  #endif
  setBalance(config.store.balance);
  setTone(config.store.bass, config.store.middle, config.store.trebble);
  setVolume(0);
  _status = STOPPED;
  _volTimer=false;
  //randomSeed(analogRead(0));
  #if PLAYER_FORCE_MONO
    forceMono(true);
  #endif
  _loadVol(config.store.volume);
  setConnectionTimeout(CONNECTION_TIMEOUT, CONNECTION_TIMEOUT_SSL);
  Serial.println("done");
}

void Player::sendCommand(playerRequestParams_t request){
  if(playerQueue==NULL) return;
  xQueueSend(playerQueue, &request, PLQ_SEND_DELAY);
}

void Player::resetQueue(){
  if(playerQueue!=NULL) xQueueReset(playerQueue);
}

void Player::stopInfo() {
  // ВАЖНО: здесь нельзя сбрасывать SmartStart.
  // Иначе после сетевого обрыва/ошибки стрима устройство забывает,
  // что должно было автоматически возобновить воспроизведение.
  netserver.requestOnChange(MODE, 0);
}

void Player::setError(){
  _hasError=true;
  config.setTitle(config.tmpBuf);
  telnet.printf("##ERROR#:\t%s\n", config.tmpBuf);
}

void Player::setError(const char *e){
  strlcpy(config.tmpBuf, e, sizeof(config.tmpBuf));
  setError();
}

void Player::_stop(bool alreadyStopped){
  log_i("%s called", __func__);
  if(config.getMode()==PM_SDCARD && !alreadyStopped) config.sdResumePos = player.getFilePos();
  _status = STOPPED;
  setOutputPins(false);

  // Ensure format is updated before title
  if(!_hasError) config.setTitle((display.mode()==LOST || display.mode()==UPDATING) ? "" : LANG::const_PlStopped);
  netserver.requestOnChange(STATIONNAME, 0);
  config.setBitrateFormat(BF_UNKNOWN);
  config.station.bitrate = 0;
  #ifdef USE_NEXTION
    nextion.bitrate(config.station.bitrate);
  #endif
  setDefaults();
  // [FIX] Всегда вызываем stopSong(), чтобы сбросить состояние Audio (m_client, m_f_running).
  // При ошибке подключения мы вызываем _stop(true) и раньше не вызывали stopSong() — следующий
  // выбор станции тогда не воспроизводился, т.к. клиент оставался в состоянии неудачной попытки.
  stopSong();
  netserver.requestOnChange(BITRATE, 0);
  display.putRequest(DBITRATE);
  display.putRequest(PSTOP);
  //setDefaults();
  //if(!alreadyStopped) stopSong();
  if(!lockOutput) {
    if (alreadyStopped) {
      // [FIX SmartStart] Поток остановился сам (ошибка декодера, bitreader underflow, обрыв, EOF) —
      // wasPlaying не сбрасываем, чтобы watchdog мог перезапустить ту же станцию.
      // При ошибке подключения (_hasError) обновляем MODE в UI.
      if (_hasError) netserver.requestOnChange(MODE, 0);
    } else {
      // Пользователь нажал Стоп — фиксируем wasPlaying=false, чтобы SmartStart не стартовал после перезагрузки.
      config.saveValue(&config.store.wasPlaying, false);
      stopInfo();
    }
  }
  if (player_on_stop_play) player_on_stop_play();
  pluginsManager::getInstance().on_stop_play();
}

void Player::initHeaders(const char *file) {
  if(strlen(file)==0 || true) return; //TODO Read TAGs
  connecttoFS(sdman,file);
  eofHeader = false;
  while(!eofHeader) Audio::loop();
  //netserver.requestOnChange(SDPOS, 0);
  setDefaults();
}
void resetPlayer(){
  if(!config.store.watchdog) return;
  player.resetQueue();
  player.sendCommand({PR_STOP, 0});
  player.loop();
}

#ifndef PL_QUEUE_TICKS
  #define PL_QUEUE_TICKS 0
#endif
#ifndef PL_QUEUE_TICKS_ST
  #define PL_QUEUE_TICKS_ST 15
#endif
void Player::loop() {
  if(playerQueue==NULL) return;
  playerRequestParams_t requestP;
  if(xQueueReceive(playerQueue, &requestP, isRunning()?PL_QUEUE_TICKS:PL_QUEUE_TICKS_ST)){
    switch (requestP.type){
      case PR_STOP: _stop(); break;
      case PR_PLAY: {
        if (requestP.payload>0) {
          config.setLastStation((uint16_t)requestP.payload);
        }
        _play((uint16_t)abs(requestP.payload), false);
        if (player_on_station_change) player_on_station_change();
        pluginsManager::getInstance().on_station_change();
        break;
      }
      #ifdef USE_SD
      case PR_PLAY_SD: {
        // Переход WEB→SD: грузим из SD-плейлиста без опоры на getMode() (гонка кэшей Core 0/1).
        config.store.play_mode = PM_SDCARD;
        if (requestP.payload > 0) {
          config.setLastStation((uint16_t)requestP.payload);
        }
        _play((uint16_t)abs(requestP.payload), true);
        if (player_on_station_change) player_on_station_change();
        pluginsManager::getInstance().on_station_change();
        break;
      }
      #endif
      case PR_TOGGLE: {
        toggle();
        break;
      }
      case PR_VOL: {
        config.setVolume(requestP.payload);
        Audio::setVolume(volToI2S(requestP.payload));
        break;
      }
      #ifdef USE_SD
      case PR_CHECKSD: {
        // Дебаунс: переключаем на WEB только после нескольких подряд отсутствий карты,
        // чтобы не срабатывать на кратковременные глюки детекции (и не «выкидывать» из SD при клике по треку).
        static uint8_t sdAbsentCount = 0;
        if (config.getMode() == PM_SDCARD) {
          if (!sdman.cardPresent()) {
            if (sdAbsentCount < 255) sdAbsentCount++;
            if (sdAbsentCount >= 3) {
              sdAbsentCount = 0;
              sdman.stop();
              config.changeMode(PM_WEB);
            }
          } else {
            sdAbsentCount = 0;
          }
        } else {
          sdAbsentCount = 0;
        }
        break;
      }
      #endif
      case PR_VUTONUS: {
        if(config.vuThreshold>10) config.vuThreshold -=10;
        break;
      }
      case PR_BURL: {
      #ifdef MQTT_ROOT_TOPIC
        if(strlen(burl)>0){
          browseUrl();
        }
      #endif
        break;
      }
          
      default: break;
    }
  }
  Audio::loop();
  if(!isRunning() && _status==PLAYING) _stop(true);
  if(_volTimer){
    if((millis()-_volTicks)>3000){
      config.saveVolume();
      _volTimer=false;
    }
  }
  /*
#ifdef MQTT_ROOT_TOPIC
  if(strlen(burl)>0){
    browseUrl();
  }
#endif*/
}

void Player::setOutputPins(bool isPlaying) {
 // if(REAL_LEDBUILTIN!=255) digitalWrite(REAL_LEDBUILTIN, LED_INVERT?!isPlaying:isPlaying);
  bool _ml = MUTE_LOCK?!MUTE_VAL:(isPlaying?!MUTE_VAL:MUTE_VAL);
  if(MUTE_PIN!=255) digitalWrite(MUTE_PIN, _ml);
}

void Player::_play(uint16_t stationId, bool forceSDPlaylist) {
  log_i("%s called, stationId=%d", __func__, stationId);
  _hasError=false;
  setDefaults();
  _status = STOPPED;
  setOutputPins(false);
  remoteStationName = false;

  // При PR_PLAY_SD выставляем режим SD до prepareForPlaying, чтобы configPostPlaying сохранил lastSdStation.
  if (forceSDPlaylist) config.store.play_mode = PM_SDCARD;

  if(!config.prepareForPlaying(stationId, forceSDPlaylist)) return;
  _loadVol(config.store.volume);

  // Окно STATION_SWITCH_SSL_BLOCK_MS: не запускать HTTPS обложки/факты сразу после старта веб-станции
  // (иначе mbedTLS конкурирует с FLAC/WebSocket и даёт -32512 SSL OOM при фрагментированной куче).
  if (config.getMode() == PM_WEB) {
    g_stationSwitchTime = millis();
  }

  bool isConnected = false;
  if((config.getMode()==PM_SDCARD || forceSDPlaylist) && SDC_CS!=255){
    isConnected=connecttoFS(sdman,config.station.url,config.sdResumePos==0?_resumeFilePos:config.sdResumePos-player.sd_min);
  }else {
    if (!forceSDPlaylist) config.saveValue(&config.store.play_mode, static_cast<uint8_t>(PM_WEB));
  }
  connproc = false;
  if(config.getMode()==PM_WEB) isConnected=connecttohost(config.station.url);
  connproc = true;
  if(isConnected){
    _status = PLAYING;
    if(config.getMode() == PM_WEB) g_lastWebStreamActivityMs = millis();
    config.configPostPlaying(stationId);
    setOutputPins(true);
    if (player_on_start_play) player_on_start_play();
    pluginsManager::getInstance().on_start_play();
  }else{
    telnet.printf("##ERROR#:\tError connecting to %.128s\n", config.station.url);
    snprintf(config.tmpBuf, sizeof(config.tmpBuf), "Error connecting to %.128s", config.station.url); setError();
    _stop(true);
  };
}

#ifdef MQTT_ROOT_TOPIC
void Player::browseUrl(){
  _hasError=false;
  remoteStationName = true;
  config.setDspOn(1);
  resumeAfterUrl = _status==PLAYING;
  display.putRequest(PSTOP);
  setOutputPins(false);
  config.setTitle(LANG::const_PlConnect);
  if (connecttohost(burl)){
    _status = PLAYING;
    config.setTitle("");
    netserver.requestOnChange(MODE, 0);
    setOutputPins(true);
    display.putRequest(PSTART);
    if (player_on_start_play) player_on_start_play();
    pluginsManager::getInstance().on_start_play();
  }else{
    telnet.printf("##ERROR#:\tError connecting to %.128s\n", burl);
    snprintf(config.tmpBuf, sizeof(config.tmpBuf), "Error connecting to %.128s", burl); setError();
    _stop(true);
  }
  //memset(burl, 0, MQTT_BURL_SIZE);
}
#endif

void Player::prev() {
  uint16_t lastStation = config.lastStation();
  // Если включён режим "только избранные" — переходим к предыдущей избранной
  if(config.store.favOnly) {
    uint16_t fav = config.findNextFavorite(lastStation, false);
    if(fav > 0) {
      config.lastStation(fav);
      sendCommand({PR_PLAY, fav});
    }
    return;
  }
  if(config.getMode()==PM_WEB || !config.store.sdsnuffle){
    if (lastStation == 1) config.lastStation(config.playlistLength()); else config.lastStation(lastStation-1);
  }
  sendCommand({PR_PLAY, config.lastStation()});
}

void Player::next() {
  uint16_t lastStation = config.lastStation();
  // Если включён режим "только избранные" — переходим к следующей избранной
  if(config.store.favOnly) {
    uint16_t fav = config.findNextFavorite(lastStation, true);
    if(fav > 0) {
      config.lastStation(fav);
      sendCommand({PR_PLAY, fav});
    }
    return;
  }
  if(config.getMode()==PM_WEB || !config.store.sdsnuffle){
    if (lastStation == config.playlistLength()) config.lastStation(1); else config.lastStation(lastStation+1);
  }else{
    config.lastStation(random(1, config.playlistLength()));
  }
  sendCommand({PR_PLAY, config.lastStation()});
}

void Player::toggle() {
  if (_status == PLAYING) {
    sendCommand({PR_STOP, 0});
  } else {
    sendCommand({PR_PLAY, config.lastStation()});
  }
}

void Player::stepVol(bool up) {
  if (up) {
    if (config.store.volume <= 254 - config.store.volsteps) {
      setVol(config.store.volume + config.store.volsteps);
    }else{
      setVol(254);
    }
  } else {
    if (config.store.volume >= config.store.volsteps) {
      setVol(config.store.volume - config.store.volsteps);
    }else{
      setVol(0);
    }
  }
}

uint8_t Player::volToI2S(uint8_t volume) {
  // [Gemini3Pro] Новая реализация с логарифмической кривой
  // Старая линейная логика (map) удалена, так как она не учитывает особенности слуха.
  
  // 1. Ограничиваем входное значение максимумом (254)
  if (volume > 254) volume = 254;
  
  // 2. Рассчитываем реальный диапазон с учетом настройки ovol (ограничение макс. громкости)
  // config.station.ovol - это некий "отступ" сверху (attenuation?). 
  // В оригинале было: maxRange = 254 - config.station.ovol * 3;
  // Если ovol=0, то volume (0..254) мапится в (0..254).
  // Если ovol>0, то выходной диапазон сжимается.
  
  // Мы применим логарифмическую кривую к ВХОДНОМУ значению volume (0..254),
  // а затем, если нужно, обрежем результат по ovol.
  
  // Получаем "человеческую" громкость из таблицы
  uint8_t logVol = getLogVolume(volume);
  
  // Применяем ограничение максимальной громкости (ovol), если оно используется
  // Логика из оригинала: уменьшаем итоговую громкость.
  // ovol * 3 - это сколько откусить от верха.
  int maxLimit = 254 - (config.station.ovol * 3);
  if (maxLimit < 0) maxLimit = 0;
  
  // Масштабируем полученную логарифмическую громкость в диапазон [0..maxLimit]
  // Это сохранит плавность кривой, но сделает "тише" максимум.
  uint8_t finalVol = map(logVol, 0, 254, 0, maxLimit);
  
  return finalVol;

  /* [Gemini3Pro] Старый код закомментирован для истории
  // Защита от деления на ноль в функции map()
  int maxRange = 254 - config.station.ovol * 3;
  if (maxRange <= 0) {
    maxRange = 1;  // Минимальное значение для избежания деления на ноль
  }
  
  int vol = map(volume, 0, maxRange, 0, 254);
  if (vol > 254) vol = 254;
  if (vol < 0) vol = 0;
  return vol;
  */
}

void Player::_loadVol(uint8_t volume) {
  setVolume(volToI2S(volume));
}

void Player::setVol(uint8_t volume) {
  _volTicks = millis();
  _volTimer = true;
  player.sendCommand({PR_VOL, volume});
}
