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
#include "../pluginsManager/pluginsManager.h"

#ifndef WIFI_ATTEMPTS
  #define WIFI_ATTEMPTS  16
#endif

#ifndef SEARCH_WIFI_CORE_ID
  #define SEARCH_WIFI_CORE_ID  0
#endif
MyNetwork network;

// [FIX] Debounce для WiFi-событий: при "мигании" WiFi (помехи от соседей)
// не обрабатываем потерю/восстановление чаще, чем раз в 2 секунды.
static volatile uint32_t g_lastWiFiEventMs = 0;
static const uint32_t    kWiFiEventDebounceMs = 2000;

void MyNetwork::WiFiReconnected(WiFiEvent_t event, WiFiEventInfo_t info){
  uint32_t now = millis();
  if (now - g_lastWiFiEventMs < kWiFiEventDebounceMs) return;
  g_lastWiFiEventMs = now;

  network.beginReconnect = false;
  player.lockOutput = false;
  vTaskDelay(pdMS_TO_TICKS(100));
  display.putRequest(NEWMODE, PLAYER);
  if(config.getMode()==PM_SDCARD) {
    network.status=CONNECTED;
    display.putRequest(NEWIP, 0);
    Serial.println("[WIFI] Reconnected in SD mode");
  }else{
    display.putRequest(NEWMODE, PLAYER);
    if (network.lostPlaying) player.sendCommand({PR_PLAY, config.lastStation()});
  }
  #ifdef MQTT_ROOT_TOPIC
    connectToMqtt();
  #endif
}

void MyNetwork::WiFiLostConnection(WiFiEvent_t event, WiFiEventInfo_t info){
  uint32_t now = millis();
  if (now - g_lastWiFiEventMs < kWiFiEventDebounceMs) return;
  g_lastWiFiEventMs = now;

  if(!network.beginReconnect){
    Serial.printf("Lost connection, reconnecting to %s...\n", config.ssids[config.store.lastSSID-1].ssid);
    if(config.getMode()==PM_SDCARD) {
      network.status=SDREADY;
      display.putRequest(NEWIP, 0);
      Serial.println("[WIFI] Lost in SD mode — playing continues");
      // [FIX] Не вызываем WiFi.reconnect() здесь: вызов из контекста WiFi-события при
      // активном разрыве соединений (WebSocket и т.д.) приводил к перезагрузке (rst:0xc).
      // Переподключение выполнит main loop с задержкой ~1.5 с.
      network.sdReconnectAfterMs = millis() + 1500;
    }else{
      network.lostPlaying = player.isRunning();
      if (network.lostPlaying) { player.lockOutput = true; player.sendCommand({PR_STOP, 0}); }
      display.putRequest(NEWMODE, LOST);
      WiFi.reconnect();
    }
  }
  network.beginReconnect = true;
  if (config.getMode() != PM_SDCARD) WiFi.reconnect();
}

bool MyNetwork::wifiBegin(bool silent){
  uint8_t ls = (config.store.lastSSID == 0 || config.store.lastSSID > config.ssidsCount) ? 0 : config.store.lastSSID - 1;
  uint8_t startedls = ls;
  uint8_t errcnt = 0;
  //WiFi.mode(WIFI_STA);
  while (true) {
    if(!silent){
      Serial.printf("##[BOOT]#\tAttempt to connect to %s\n", config.ssids[ls].ssid);
      Serial.print("##[BOOT]#\t");
      display.putRequest(BOOTSTRING, ls);
    }
    //WiFi.disconnect(true, true); //disconnect & erase internal credentials https://github.com/e2002/yoradio/pull/164/commits/89d8b4450dde99cd7930b84bb14d81dab920b879
    //delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssids[ls].ssid, config.ssids[ls].password);
    while (WiFi.status() != WL_CONNECTED) {
      if(!silent) Serial.print(".");
      vTaskDelay(pdMS_TO_TICKS(500));
      if(REAL_LEDBUILTIN!=255 && !silent) digitalWrite(REAL_LEDBUILTIN, !digitalRead(REAL_LEDBUILTIN));
      errcnt++;
      if (errcnt > WIFI_ATTEMPTS) {
        errcnt = 0;
        ls++;
        if (ls > config.ssidsCount - 1) ls = 0;
        if(!silent) Serial.println();
        WiFi.mode(WIFI_OFF);
        break;
      }
    }
    if (WiFi.status() != WL_CONNECTED && ls == startedls) {
      return false; break;
    }
    if (WiFi.status() == WL_CONNECTED) {
      config.setLastSSID(ls + 1);
      return true; break;
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
  if (config.ssidsCount == 0 || DBGAP) {
    raiseSoftAP();
    return;
  }
  if(config.getMode()!=PM_SDCARD){
    if(!wifiBegin()){
      raiseSoftAP();
      Serial.println("##[BOOT]#\tdone");
      return;
    }
    Serial.println(".");
    status = CONNECTED;
    setWifiParams();
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
  WiFi.onEvent(WiFiReconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiLostConnection, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  //config.setTimeConf(); //??
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
    // [FIX] Останавливаем предыдущий экземпляр mDNS перед новым запуском.
    // Повторный MDNS.begin() без MDNS.end() приводит к heap corruption
    // (утечка памяти из-за не очищенных внутренних структур mDNS-стека).
    MDNS.end();
    MDNS.begin(safeMdns);
  }
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
