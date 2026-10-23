#ifndef network_h
#define network_h
#include <WiFi.h>

enum n_Status_e { CONNECTED, SOFT_AP, FAILED, SDREADY };

class MyNetwork {
  public:
    n_Status_e status;
    struct tm timeinfo;
    bool lostPlaying = false, beginReconnect = false;
    // В SD при потере WiFi не вызываем WiFi.reconnect() из обработчика события
    // (это приводило к перезагрузке). Вместо этого main loop вызовет reconnect после задержки.
    uint32_t sdReconnectAfterMs = 0;
  public:
    MyNetwork() {};
    void begin();
    void requestTimeSync(bool withTelnetOutput=false, uint8_t clientId=0);
    void requestWeatherSync();
    void setWifiParams();
    // rotateOnFail=false: только текущий SSID без обхода (редко); true: обход списка как сток.
    bool wifiBegin(bool silent=false, bool rotateOnFail=false);
  private:
    void raiseSoftAP();
    static void WiFiLostConnection(WiFiEvent_t event, WiFiEventInfo_t info);
    static void WiFiReconnected(WiFiEvent_t event, WiFiEventInfo_t info);
};

extern MyNetwork network;

// До этого момента (millis) не считаем Wi‑Fi стабильным: после ASSOC_LEAVE/NO_AP_FOUND не гоняем SSL и FREEZE-RECOVERY.
extern volatile uint32_t g_wifiInstabilityGraceUntilMs;
/** Карантин SSL/фона после обрыва Wi‑Fi или stream lost (не укорачивает уже более длинный). */
void networkArmWifiInstabilityGrace(uint32_t durationMs);

extern __attribute__((weak)) void network_on_connect();

#endif
