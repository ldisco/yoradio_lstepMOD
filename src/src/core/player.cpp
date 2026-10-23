#include "options.h"
#include "player.h"
#include "volume_curve.h" // таблица логарифмической громкости
#include "config.h"
#include "telnet.h"
#include "display.h"
#include "sdmanager.h"
#include "netserver.h"
#include "timekeeper.h"
#include "transient_notifier.h"
#if SPECTRUM_ANALYZER_ACTIVE
#include "spectrum_analyzer.h"
#endif
#include "../displays/tools/l10n.h"
#include "../pluginsManager/pluginsManager.h"
#include "../plugins/TrackFacts/TrackFacts.h"
#if USE_BT_I2S
#include "bt_i2s.h"
#endif
#ifdef USE_NEXTION
#include "../displays/nextion.h"
#endif
extern TrackFacts trackFactsPlugin;  // экземпляр в main.cpp — отмена HTTPS при смене станции
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
  // [ОТКАТ] setAudioTaskCore(AUDIOTASK_CORE=0) убран: декодер на Core 0 + DspTask (JPEG cover)
  // голодали IDLE0 → TWDT → циклический reboot. Как в backup 241: PeriodicTask остаётся
  // на Core 1 по умолчанию (m_audioTaskCoreId=1). Флаг -DAUDIOTASK_CORE в platformio снова мёртвый.
  /* MUTE: pinMode до первого digitalWrite в setOutputPins */
  if(MUTE_PIN!=255) pinMode(MUTE_PIN, OUTPUT);
  setOutputPins(false);
  vTaskDelay(pdMS_TO_TICKS(50));
#ifdef MQTT_ROOT_TOPIC
  memset(burl, 0, MQTT_BURL_SIZE);
#endif
  #if I2S_DOUT!=255
    #if !I2S_INTERNAL
      #if USE_BT_I2S
      btI2s.applyEspI2sPinout();
      #else
      setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
      setOutputSampleRate(SR_48000);
      #endif
    #endif
  #else
    SPI.begin();
    if(VS1053_RST>0) ResetChip();
    begin();
  #endif
  setBalance(config.store.balance);
  setTone(config.store.bass, config.store.middle, config.store.trebble);
  setVolume(0);
#if SPECTRUM_ANALYZER_ACTIVE
  /*
   * FFT-задачу запускаем после настройки I2S и фильтров, но до первого PLAY.
   * Ошибка инициализации не блокирует плеер: звук продолжит работать без спектра.
   * Гейт SPECTRUM_ANALYZER_ACTIVE — общий для ST7789 и ILI9488 landscape.
   */
  spectrumAnalyzer.begin();
#endif
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

bool Player::requestWebRecovery(const char* source, bool bypassCooldown) {
  /*
   * Единая точка постановки внешнего recovery.
   * Внутренний reconnect Audio имеет приоритет; пока он работает или _play()
   * уже подключает станцию, новый PR_PLAY только размножил бы DNS/TLS-запросы.
   */
  if (playerQueue == NULL
      || config.getMode() != PM_WEB
      || !config.store.wasPlaying
      || g_modeSwitching
      || !connproc
      || isWebStreamRecovering()) {
    return false;
  }

  const uint32_t now = millis();
  const uint32_t lastRequest = _lastWebRecoveryRequestMs;
  if (!bypassCooldown
      && lastRequest != 0
      && (uint32_t)(now - lastRequest) < (uint32_t)WEB_RECOVERY_COOLDOWN_MS) {
    return false;
  }

  bool expected = false;
  if (!_webRecoveryQueued.compare_exchange_strong(expected, true)) {
    return false;
  }

  playerRequestParams_t request = {PR_PLAY, -((int)config.lastStation())};
  if (request.payload == 0 || xQueueSend(playerQueue, &request, 0) != pdTRUE) {
    _webRecoveryQueued.store(false);
    return false;
  }

  _lastWebRecoveryRequestMs = now;
  transientNotifier.clear(TN_AUDIO_RECOVERED);
  transientNotifier.show(TN_AUDIO_RECOVERY, "Восстанавливаем поток…");
  Serial.printf("[RECOVERY] Запрос принят: источник=%s, станция=%u\n",
                source ? source : "unknown", (unsigned)config.lastStation());
  return true;
}

void Player::resetQueue(){
  if(playerQueue!=NULL) xQueueReset(playerQueue);
  /* После очистки очереди в ней гарантированно нет technical PR_PLAY. */
  _webRecoveryQueued.store(false);
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
  if(!alreadyStopped) _hasError = false; // Сбрасываем ошибку при ручной остановке, чтобы показать "[остановлено]"
  if(config.getMode()==PM_SDCARD && !alreadyStopped) config.sdResumePos = player.getFilePos();
  _status = STOPPED;
  setOutputPins(false);

  // Технический стоп при смене режима (changeMode / g_modeSwitching):
  // плеер обязан освободить дескрипторы (stopSong), но это НЕ «Стоп» пользователя.
  // Иначе веб залипает в «[остановлено]», а wasPlaying=false ломает SmartStart.
  const bool modeSwitchStop = g_modeSwitching;

  // В веб шлём PlStopped / STATIONNAME только при обычном стопе, не при mode-switch.
  if(!modeSwitchStop) {
    if(!_hasError) config.setTitle((display.mode()==LOST || display.mode()==UPDATING) ? "" : LANG::const_PlStopped);
    netserver.requestOnChange(STATIONNAME, 0);
  }
  config.setBitrateFormat(BF_UNKNOWN);
  config.station.bitrate = 0;
  #ifdef USE_NEXTION
    nextion.bitrate(config.station.bitrate);
  #endif
  setDefaults();
  // Всегда вызываем stopSong(), чтобы сбросить состояние Audio (m_client, m_f_running).
  // При ошибке подключения мы вызываем _stop(true) и раньше не вызывали stopSong() — следующий
  // выбор станции тогда не воспроизводился, т.к. клиент оставался в состоянии неудачной попытки.
  stopSong();
  // Битрейт в UI обнуляем только вне смены режима — иначе веб мигает 0 kBits и «stopped».
  if(!modeSwitchStop) {
    netserver.requestOnChange(BITRATE, 0);
  }
  display.putRequest(DBITRATE);
  display.putRequest(PSTOP);
  //setDefaults();
  //if(!alreadyStopped) stopSong();
  if(!lockOutput) {
    if (alreadyStopped) {
      // Поток/декодер остановился сами (ошибка, EOF, FLAC guard) — wasPlaying не трогаем:
      // [RETRY]/watchdog переподключают; после перезагрузки SmartStart восстановит сессию.
      if (_hasError) netserver.requestOnChange(MODE, 0);
    } else if(!modeSwitchStop) {
      // Только явный PR_STOP (кнопка/веб «Стоп», не lockOutput и не смена режима).
      config.saveValue(&config.store.wasPlaying, false);
      stopInfo();
    }
  }
  if (!alreadyStopped && !modeSwitchStop) {
    /*
     * Явный Stop завершает transient recovery без сообщения
     * «восстановлено»: пользователь сам прекратил воспроизведение.
     */
    transientNotifier.clear(TN_AUDIO_RECOVERY);
    transientNotifier.clear(TN_AUDIO_RECOVERED);
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
      case PR_STOP:
        /*
         * Явный Stop отменяет recovery, но технический PR_STOP
         * при Wi-Fi loss (lockOutput=true) не должен снимать single-flight:
         * за ним уже может стоять единственный retry от GOT_IP.
         */
        if (!lockOutput && !g_modeSwitching) {
          _webRecoveryQueued.store(false);
        }
        _stop();
        break;
      case PR_PLAY: {
        const bool isAutoRetry = requestP.payload < 0;
        const uint16_t requestedStation = (uint16_t)abs(requestP.payload);
        /*
         * Отложенный technical retry не должен воскресить поток
         * после ручного Stop или после ухода пользователя из WEB-режима.
         */
        if (isAutoRetry && (!config.store.wasPlaying || config.getMode() != PM_WEB)) {
          _webRecoveryQueued.store(false);
          break;
        }
        if (isAutoRetry && requestedStation != config.lastStation()) {
          if (config.store.audioinfo) {
            Serial.printf("[RETRY] Skip stale retry station %u, current is %u\n",
                          (unsigned)requestedStation, (unsigned)config.lastStation());
          }
          _webRecoveryQueued.store(false);
          break;
        }
        if (requestP.payload > 0) {
          /* Явный Play/выбор станции заменяет любой прежний recovery-сеанс. */
          _webRecoveryQueued.store(false);
          config.setLastStation((uint16_t)requestP.payload);
          // Явный выбор станции / Play — фиксируем «пользователь хочет играть» (даже если connect упадёт).
          config.saveValue(&config.store.wasPlaying, true);
        }
        _play(requestedStation, false);
        if (isAutoRetry) {
          /*
           * Снимаем single-flight только после полного выхода из _play().
           * Во время блокирующего DNS/TLS connproc=false и этот флаг вместе закрывают
           * окно для повторных watchdog/auto-retry.
           */
          _webRecoveryQueued.store(false);
        }
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
          config.saveValue(&config.store.wasPlaying, true);
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
#if USE_BT_I2S
        if (config.getMode() == PM_BT) {
          // BT: громкость QCC5125 только кнопками vol± (stepVol). Абсолютный слайдер WebUI
          // обновляет store.volume для volinfo, но не шлёт десятки импульсов на GPIO.
          break;
        }
#endif
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
  // [v0.8.216] SmartStart: битый FLAC-поток (lesonparisien) не должен давать «Стоп» в UI и не трогать wasPlaying.
  // Раньше stopSong() на ошибке FLAC → !isRunning() → _stop(true) → веб «остановлено», хотя пользователь не жмал Стоп.
  // При smartstart+wasPlaying+WEB+FLAC оставляем PLAYING; decode ресинхронизируется (v0.8.215), retry/watchdog подхватят.
  if(!isRunning() && _status==PLAYING) {
    const bool flacSmartStartKeepPlaying =
        (config.store.smartstart == 1) && config.store.wasPlaying
        && (config.getMode() == PM_WEB) && (getCodec() == 5); // 5 == CODEC_FLAC (enum в Audio private)
    if(!flacSmartStartKeepPlaying) {
      _stop(true);
    }
  }
  if(_volTimer){
    if((millis()-_volTicks)>3000){
      /* На экране VOL не commit: иначе ~за секунду до закрытия VOL→PLAYER вспышка RGB (flash stall).
       * Запись отложится до ухода с VOL (или до следующего тика после закрытия). */
      if (display.mode() != VOL) {
        config.saveVolume();
        _volTimer=false;
      }
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
  // Onboard LED DevKit (ESP32-S3 RGB, GPIO48): не держим постоянно включённым ни в PLAY, ни в STOP.
  // Моргание при Wi‑Fi/boot — только в network.cpp; сюда не переносим.
  if(REAL_LEDBUILTIN!=255) digitalWrite(REAL_LEDBUILTIN, LOW);
  (void)isPlaying;
  bool _ml = MUTE_LOCK?!MUTE_VAL:(isPlaying?!MUTE_VAL:MUTE_VAL);
  if(MUTE_PIN!=255) digitalWrite(MUTE_PIN, _ml);
}

void Player::_play(uint16_t stationId, bool forceSDPlaylist) {
  log_i("%s called, stationId=%d", __func__, stationId);
#if USE_BT_I2S
  if (config.getMode() == PM_BT) return;
#endif
  _hasError=false;

  // Станции рабочие (на другом радио те же URL играют). Ломается сетевой стек ESP:
  // CoverDL/Facts продолжают HTTPS/DNS параллельно connecttohost → lwIP «залипает»,
  // все станции дают Error connecting, веб перестаёт отвечать. Отмена была только в changeMode().
  if (config.getMode() == PM_WEB && !forceSDPlaylist) {
    if (g_coverTaskRunning) {
      g_coverDlCancelRequested = true;
    }
    trackFactsPlugin.abortForModeSwitch();
  }

  setDefaults();
  _status = STOPPED;
  setOutputPins(false);
  remoteStationName = false;

  // При PR_PLAY_SD выставляем режим SD до prepareForPlaying, чтобы configPostPlaying сохранил lastSdStation.
  if (forceSDPlaylist) config.store.play_mode = PM_SDCARD;

  if(!config.prepareForPlaying(stationId, forceSDPlaylist)) return;
  /* Сразу закрываем «можно flush EEPROM»: иначе ~3 с STOPPED до connect → commit NOW на RGB. */
  connproc = false;
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
  /* connproc=false уже сразу после prepareForPlaying (блок EEPROM flush на RGB). */
  if (config.getMode() == PM_WEB) {
    // IDF 5.x: SNTP (timeTask/syncTask) и hostByName потока не должны идти параллельно в lwIP
    // (лог: assert udp_new_ip_type — Required to lock TCPIP core functionality).
    // CoverDL/Facts уже вызывают pauseLwIpBackgroundSntp; плеер — нет, отсюда краш ~20 с после старта.
    // Потолок ожидания SNTP/syncTask: 3 с (было 20 с).
    // При залипшем busy connect всё равно продолжаем — иначе SmartStart «висит» ~20 с.
    const uint32_t syncWait0 = millis();
    while (timekeeper.busy && (millis() - syncWait0) < 3000) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (timekeeper.busy) {
      Serial.println("[Player] timekeeper.busy timeout 3s — connect anyway");
    }
    /*
     * Не стартуем connecttohost параллельно internal Stream-lost recovery:
     * иначе два TLS на один lwIP → errno 113, DNS -54, WebUI перестаёт отвечать.
     */
    const uint32_t recWait0 = millis();
    while (isWebStreamRecovering() && (millis() - recWait0) < 8000) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    pauseLwIpBackgroundSntp();
    isConnected = connecttohost(config.station.url);
  }
  connproc = true;
  if(isConnected){
    _status = PLAYING;
    // Не трогаем g_lastWebStreamActivityMs здесь: TCP-connect ≠ реальный поток.
    // Иначе watchdog/auto-retry считают станцию «живой» и сбрасывают счётчик неудач.
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
  // То же правило lwIP: не конкурировать с фоновым SNTP при DNS URL.
  pauseLwIpBackgroundSntp();
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
#if USE_BT_I2S
  if (config.getMode() == PM_BT) {
    btI2s.playPause();
    config.syncBtUi();
    return;
  }
#endif
  if (_status == PLAYING) {
    sendCommand({PR_STOP, 0});
  } else {
    sendCommand({PR_PLAY, config.lastStation()});
  }
}

void Player::stepVol(bool up) {
#if USE_BT_I2S
  if (config.getMode() == PM_BT) {
    if (up) {
      if (config.store.volume <= 254 - config.store.volsteps) {
        config.setVolume(config.store.volume + config.store.volsteps);
      } else {
        config.setVolume(254);
      }
      btI2s.volumeUp();
    } else {
      if (config.store.volume >= config.store.volsteps) {
        config.setVolume(config.store.volume - config.store.volsteps);
      } else {
        config.setVolume(0);
      }
      btI2s.volumeDown();
    }
    return;
  }
#endif
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
  // Логарифмическая кривая громкости.
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

  /* Старый код закомментирован для истории
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

void Player::bumpVolumeSaveTimer() {
  /* После VOL→PLAYER: дать экрану успокоиться, потом EEPROM.commit (~3 с). */
  _volTicks = millis();
  _volTimer = true;
}
