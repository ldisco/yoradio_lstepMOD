#include "options.h"
#include <ESPmDNS.h>
#include "time.h"
#include "rtcsupport.h"
#include "network.h"
#include "display.h"
#include "config.h"
#include "telnet.h"
#include "netserver.h"
#include "player.h"
#include "mqtt.h"
#include "timekeeper.h"
#include "transient_notifier.h"
#include "../pluginsManager/pluginsManager.h"

#ifndef WIFI_ATTEMPTS
  #define WIFI_ATTEMPTS  16
#endif

#ifndef SEARCH_WIFI_CORE_ID
  #define SEARCH_WIFI_CORE_ID  0
#endif
MyNetwork network;

// Карантин после обрыва/реконнекта Wi‑Fi (лог terminal 18: FREEZE-RECOVERY при NO_AP_FOUND, SSL -76 при ASSOC_LEAVE).
volatile uint32_t g_wifiInstabilityGraceUntilMs = 0;

#ifndef WIFI_INSTABILITY_GRACE_CAP_MS
  // Верхняя граница карантина — не копить 90+45+15+12 с каскадом.
  #define WIFI_INSTABILITY_GRACE_CAP_MS 30000u
#endif
#ifndef WIFI_INSTABILITY_GRACE_LOST_MS
  #define WIFI_INSTABILITY_GRACE_LOST_MS 20000u
#endif
#ifndef WIFI_INSTABILITY_GRACE_GOT_IP_MS
  #define WIFI_INSTABILITY_GRACE_GOT_IP_MS 10000u
#endif

// Продлевает карантин только вперёд; не дальше WIFI_INSTABILITY_GRACE_CAP_MS от «сейчас».
static void armWifiInstabilityGrace(uint32_t durationMs) {
  const uint32_t now = millis();
  uint32_t until = now + durationMs;
  const uint32_t capUntil = now + (uint32_t)WIFI_INSTABILITY_GRACE_CAP_MS;
  if (until > capUntil) {
    until = capUntil;
  }
  if (until > g_wifiInstabilityGraceUntilMs) {
    g_wifiInstabilityGraceUntilMs = until;
  }
}

void networkArmWifiInstabilityGrace(uint32_t durationMs) {
  armWifiInstabilityGrace(durationMs);
}

// Debounce для WiFi-событий: при "мигании" WiFi (помехи от соседей)
// не обрабатываем потерю/восстановление чаще, чем раз в 2 секунды.
static volatile uint32_t g_lastWiFiEventMs = 0;
static const uint32_t    kWiFiEventDebounceMs = 2000;

// Объявление: определение ниже wifiBegin; вызывается из WiFiReconnected и setWifiParams.
static void syncLastSsidFromConnected();

void MyNetwork::WiFiReconnected(WiFiEvent_t event, WiFiEventInfo_t info){
  uint32_t now = millis();
  if (now - g_lastWiFiEventMs < kWiFiEventDebounceMs) return;
  g_lastWiFiEventMs = now;

  const bool wasReconnecting = network.beginReconnect;
  network.beginReconnect = false;
  // Короткий карантин после GOT_IP (было 45 с — gate false минутами).
  armWifiInstabilityGrace(WIFI_INSTABILITY_GRACE_GOT_IP_MS);
  player.lockOutput = false;
  syncLastSsidFromConnected(); // Запоминаем сеть, на которой восстановились (для следующего boot).
  vTaskDelay(pdMS_TO_TICKS(100));
  /* Не срывать STATIONS/VOL на PLAYER — иначе список «мелькает» при GOT_IP/recovery. */
  const bool keepUi = (display.mode() == STATIONS || display.mode() == VOL);
  if (!keepUi) {
    display.putRequest(NEWMODE, PLAYER);
  }
  if(config.getMode()==PM_SDCARD) {
    network.status=CONNECTED;
    display.putRequest(NEWIP, 0);
    Serial.println("[WIFI] Reconnected in SD mode");
  }else{
    if (network.lostPlaying) {
      /*
       * После реального GOT_IP восстановление можно начать сразу,
       * но только через общий single-flight. Это не даёт Wi-Fi callback и
       * auto-retry одновременно поставить два PR_PLAY в очередь.
       */
      player.requestWebRecovery("wifi-reconnected", true);
    }
  }
  if (wasReconnecting) {
    /* Сообщаем только переход LOST→GOT_IP, но не обычный cold boot. */
    transientNotifier.show(TN_NETWORK_RECOVERED, "Сеть восстановлена");
  }
  #ifdef MQTT_ROOT_TOPIC
    connectToMqtt();
  #endif
}

void MyNetwork::WiFiLostConnection(WiFiEvent_t event, WiFiEventInfo_t info){
  uint32_t now = millis();
  if (now - g_lastWiFiEventMs < kWiFiEventDebounceMs) return;
  g_lastWiFiEventMs = now;
  /* Следующий GOT_IP снова имеет право показать переход восстановления. */
  transientNotifier.clear(TN_NETWORK_RECOVERED);

  // Короткий карантин после обрыва (было 90 с + каскад recovery/retry).
  armWifiInstabilityGrace(WIFI_INSTABILITY_GRACE_LOST_MS);
  if(!network.beginReconnect){
    Serial.printf("Lost connection, reconnecting to %s...\n", config.ssids[config.store.lastSSID-1].ssid);
    if(config.getMode()==PM_SDCARD) {
      network.status=SDREADY;
      display.putRequest(NEWIP, 0);
      Serial.println("[WIFI] Lost in SD mode — playing continues");
      // Не вызываем WiFi.reconnect() здесь: вызов из контекста WiFi-события при
      // активном разрыве соединений (WebSocket и т.д.) приводил к перезагрузке (rst:0xc).
      // Переподключение выполнит main loop с задержкой ~1.5 с.
      network.sdReconnectAfterMs = millis() + 1500;
    }else{
      network.lostPlaying = player.isRunning();
      if (network.lostPlaying) { player.lockOutput = true; player.sendCommand({PR_STOP, 0}); }
      /* Плейлист/громкость не перекрывать диалогом LOST — иначе «мелькает» плеер при обрыве. */
      if (display.mode() != STATIONS && display.mode() != VOL) {
        display.putRequest(NEWMODE, LOST);
      }
      WiFi.reconnect();
    }
  }
  network.beginReconnect = true;
  if (config.getMode() != PM_SDCARD) WiFi.reconnect();
}

// Синхронизируем lastSSID в EEPROM с фактически подключённой сетью (не только при boot wifiBegin).
static void syncLastSsidFromConnected() {
  String cur = WiFi.SSID();
  if (cur.length() == 0) return;
  for (uint8_t i = 0; i < config.ssidsCount; i++) {
    if (cur.equals(config.ssids[i].ssid)) {
      config.setLastSSID(i + 1);
      return;
    }
  }
}

bool MyNetwork::wifiBegin(bool silent, bool rotateOnFail){
  uint8_t ls = (config.store.lastSSID == 0 || config.store.lastSSID > config.ssidsCount) ? 0 : config.store.lastSSID - 1;
  uint8_t startedls = ls;
  uint8_t errcnt = 0;
  /* rotateOnFail=true (boot): как сток — WIFI_ATTEMPTS на SSID, потом следующий.
   * rotateOnFail=false: чуть дольше на одном SSID (фоновый searchWiFi / узкие вызовы). */
  const uint8_t maxAttempts = rotateOnFail ? (uint8_t)WIFI_ATTEMPTS : (uint8_t)(WIFI_ATTEMPTS * 2);
#if (DSP_MODEL == DSP_ST7701) && ST7701_DIAG_WIFI_PERSISTENT_OFF
  /* Отдельно от NO_FLASH_COMMIT: не писать WiFi-конфиг в NVS при connect. */
  WiFi.persistent(false);
  {
    static bool s_diagLogged = false;
    if (!s_diagLogged) {
      Serial.println("[ST7701-DIAG] WIFI_PERSISTENT_OFF=1 — WiFi.persistent(false)");
      s_diagLogged = true;
    }
  }
#endif
  while (true) {
    if(!silent){
      Serial.printf("##[BOOT]#\tAttempt to connect to %s\n", config.ssids[ls].ssid);
      Serial.print("##[BOOT]#\t");
      display.putRequest(BOOTSTRING, ls);
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssids[ls].ssid, config.ssids[ls].password);
    while (WiFi.status() != WL_CONNECTED) {
      if(!silent) Serial.print(".");
      vTaskDelay(pdMS_TO_TICKS(500));
      if(REAL_LEDBUILTIN!=255 && !silent) digitalWrite(REAL_LEDBUILTIN, !digitalRead(REAL_LEDBUILTIN));
      errcnt++;
      /* NO_AP_FOUND: не жечь весь WIFI_ATTEMPTS на отсутствующий SSID — сразу следующий. */
      const bool ssidGone = (WiFi.status() == WL_NO_SSID_AVAIL);
      if (ssidGone && errcnt >= 4) {
        if(!silent) {
          Serial.println();
          Serial.printf("##[BOOT]#\tSSID \"%s\" not in range (WL_NO_SSID_AVAIL), skip\n", config.ssids[ls].ssid);
        }
        errcnt = 0;
        WiFi.mode(WIFI_OFF);
        if (!rotateOnFail) {
          return false; /* без обхода списка — наружу (редко) */
        }
        ls++;
        if (ls > config.ssidsCount - 1) ls = 0;
        break;
      }
      if (errcnt > maxAttempts) {
        errcnt = 0;
        if(!silent) Serial.println();
        WiFi.mode(WIFI_OFF);
        if (!rotateOnFail) {
          return false;
        }
        ls++;
        if (ls > config.ssidsCount - 1) ls = 0;
        break;
      }
    }
    if (WiFi.status() == WL_CONNECTED) {
      syncLastSsidFromConnected();
      return true;
    }
    if (!rotateOnFail) {
      return false;
    }
    if (ls == startedls) {
      return false;
    }
  }
  return false;
}

void searchWiFi(void * pvParameters){
  if(!network.wifiBegin(true)){
    vTaskDelay(pdMS_TO_TICKS(10000));
    xTaskCreatePinnedToCore(searchWiFi, "searchWiFi", 1024 * 4, NULL, 0, NULL, SEARCH_WIFI_CORE_ID);
  }else{
    network.status = CONNECTED;
    netserver.begin(true);
    telnet.begin(true);
    network.setWifiParams();
    display.putRequest(NEWIP, 0);
    #ifdef MQTT_ROOT_TOPIC
      mqttInit();
    #endif
  }
  vTaskDelete( NULL );
}

#define DBGAP false

void MyNetwork::begin() {
  BOOTLOG("network.begin");
  config.initNetwork();
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
  /* Soak: WiFi по шагу; аудио режется в main (smartstart skip). */
  {
    char step = (char)ST7701_RGB_SOAK_STEP;
    if (step >= 'A' && step <= 'E') step = (char)(step - 'A' + 'a');
    if (step < 'a' || step > 'e') step = 'a';
    Serial.printf("##[BOOT]#\t[SOAK] network step=%c\n", step);
    if (step == 'a' || step == 'b') {
      WiFi.mode(WIFI_OFF);
      status = FAILED; /* не CONNECTED/SDREADY → playerTask не крутит WEB */
      Serial.println("##[BOOT]#\t[SOAK] WIFI_OFF (no init)");
      Serial.println("##[BOOT]#\tdone");
      return;
    }
    if (step == 'd' || step == 'e') {
      raiseSoftAP();
      if (step == 'e') {
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        Serial.println("##[BOOT]#\t[SOAK] SoftAP + WIFI_POWER_8_5dBm");
      } else {
        Serial.println("##[BOOT]#\t[SOAK] SoftAP (default TX)");
      }
      Serial.println("##[BOOT]#\tdone");
      return;
    }
    /* 'c' — STA ниже; SoftAP-fallback запрещён (иначе смешение с d). */
  }
#endif
  if (config.ssidsCount == 0 || DBGAP) {
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
    /* soak 'c' без сетей — не AP. */
    {
      char step = (char)ST7701_RGB_SOAK_STEP;
      if (step >= 'A' && step <= 'E') step = (char)(step - 'A' + 'a');
      if (step == 'c') {
        WiFi.mode(WIFI_OFF);
        status = FAILED;
        Serial.println("##[BOOT]#\t[SOAK] step=c no SSIDs — WIFI_OFF");
        Serial.println("##[BOOT]#\tdone");
        return;
      }
    }
#endif
    raiseSoftAP();
    return;
  }
  if(config.getMode()!=PM_SDCARD){
    /* Boot WiFi как сток yoRadio: один обход списка с lastSSID.
     * Раньше (v0.8.207): 3 цикла только lastSSID + паузы 5 с + ещё fallback — на чужой
     * локации и на «телефон уже видит клиента» заставка висела лишние секунды.
     * Защита SoftAP после watchdog-reboot: при полном провале списка — одна короткая
     * пауза 2 с и второй такой же обход (без 3×5 с). */
    bool wifiOk = wifiBegin(false, true);
    if (!wifiOk) {
      Serial.println("##[BOOT]#\tWiFi list pass failed, retry once in 2s...");
      vTaskDelay(pdMS_TO_TICKS(2000));
      wifiOk = wifiBegin(false, true);
    }
    if(!wifiOk){
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
      {
        char step = (char)ST7701_RGB_SOAK_STEP;
        if (step >= 'A' && step <= 'E') step = (char)(step - 'A' + 'a');
        if (step == 'c') {
          WiFi.mode(WIFI_OFF);
          status = FAILED;
          Serial.println("##[BOOT]#\t[SOAK] step=c STA fail — no SoftAP fallback");
          Serial.println("##[BOOT]#\tdone");
          return;
        }
      }
#endif
      raiseSoftAP();
      Serial.println("##[BOOT]#\tdone");
      return;
    }
    Serial.println(".");
    status = CONNECTED;
    /* Паузу 1–2 с после IP убрали (сток без неё). Телефон часто уже показывает
     * «подключено» на ассоциации, а ESP ещё держал заставку из‑за этой задержки.
     * SNTP по-прежнему отложен в main (+15 с), SmartStart — свой короткий delay. */
    setWifiParams();
    Serial.printf("##[BOOT]#\tWiFi IP %s, MAC %s\n",
                  config.ipToStr(WiFi.localIP()), WiFi.macAddress().c_str());
    #ifdef MQTT_ROOT_TOPIC
      mqttInit();
    #endif
  }else{
    status = SDREADY;
    xTaskCreatePinnedToCore(searchWiFi, "searchWiFi", 1024 * 4, NULL, 0, NULL, SEARCH_WIFI_CORE_ID);
  }
  
  Serial.println("##[BOOT]#\tdone");
  if(REAL_LEDBUILTIN!=255) digitalWrite(REAL_LEDBUILTIN, LOW);
  
#if RTCSUPPORTED
  if(config.isRTCFound()){
    rtc.getTime(&network.timeinfo);
    mktime(&network.timeinfo);
    display.putRequest(CLOCK);
  }
#endif
  if (network_on_connect) network_on_connect();
  pluginsManager::getInstance().on_connect();
}

void MyNetwork::setWifiParams(){
  WiFi.setSleep(false);
  syncLastSsidFromConnected();
  WiFi.onEvent(WiFiReconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiLostConnection, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  //config.setTimeConf(); //??
  /* Classic ESP32 (Wrover / D0WD): не стартуем mDNS.
   * На Aiwushka: Ready → через ~25 с abort в mdns _udp_recv / tcpip (stack_sentry).
   * Ветка только CONFIG_IDF_TARGET_ESP32 — на ESP32-S3 этот #if ложен, код ниже как раньше. */
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
  Serial.println("[mDNS] skipped on classic ESP32 (use http://IP/)");
#else
  if(strlen(config.store.mdnsname)>0) {
    // Валидируем имя хоста перед передачей в mDNS: плохое имя из EEPROM
    // вызывает разыменование нулевого указателя внутри _mdns_append_answer.
    // Допустимы только [a-zA-Z0-9-], без '-' в начале и конце строки.
    char safeMdns[MDNS_LENGTH] = {0};
    strncpy(safeMdns, config.store.mdnsname, sizeof(safeMdns) - 1);
    bool nameOk = (safeMdns[0] != '\0');
    size_t nameLen = strlen(safeMdns);
    if (nameOk && (safeMdns[0] == '-' || safeMdns[nameLen - 1] == '-')) nameOk = false;
    if (nameOk) {
      for (size_t i = 0; i < nameLen; ++i) {
        char c = safeMdns[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) {
          nameOk = false; break;
        }
      }
    }
    if (!nameOk) {
      snprintf(safeMdns, sizeof(safeMdns), "yoradio-%x", (unsigned int)config.getChipId());
      Serial.printf("[mDNS] Bad hostname in EEPROM, using fallback: %s\n", safeMdns);
    }
    // Останавливаем предыдущий экземпляр mDNS перед новым запуском.
    // Повторный MDNS.begin() без MDNS.end() приводит к heap corruption
    // (утечка памяти из-за не очищенных внутренних структур mDNS-стека).
    MDNS.end();
    MDNS.begin(safeMdns);
  }
#endif
}

void MyNetwork::requestTimeSync(bool withTelnetOutput, uint8_t clientId) {
  if (withTelnetOutput) {
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    if (config.store.tzHour < 0) {
      telnet.printf(clientId, "##SYS.DATE#: %s%03d:%02d\n> ", timeStringBuff, config.store.tzHour, config.store.tzMin);
    } else {
      telnet.printf(clientId, "##SYS.DATE#: %s+%02d:%02d\n> ", timeStringBuff, config.store.tzHour, config.store.tzMin);
    }
  }
}

void rebootTime() {
  ESP.restart();
}

void MyNetwork::raiseSoftAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid, apPassword);
  Serial.println("##[BOOT]#");
  BOOTLOG("************************************************");
  BOOTLOG("Running in AP mode");
  BOOTLOG("Connect to AP %s with password %s", apSsid, apPassword);
  BOOTLOG("and go to http:/192.168.4.1/ to configure");
  BOOTLOG("************************************************");
  status = SOFT_AP;
  if(config.store.softapdelay>0)
    timekeeper.waitAndDo(config.store.softapdelay*60, rebootTime);
}

void MyNetwork::requestWeatherSync(){
  display.putRequest(NEWWEATHER);
}
