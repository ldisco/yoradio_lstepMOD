#include "Arduino.h"
#include "core/options.h"
#include "core/config.h"
#include "pluginsManager/pluginsManager.h"
#include "plugins/TrackFacts/TrackFacts.h"
#include "plugins/SleepTimer/SleepTimer.h"
#include "core/telnet.h"
#include "core/player.h"
#include "core/display.h"
#include "core/network.h"
#include "core/netserver.h"
#include "core/controls.h"
//#include "core/mqtt.h"
#include "core/optionschecker.h"
#include "core/timekeeper.h"
#if defined(MBEDTLS_PSRAM_ALLOC_ENABLE) && MBEDTLS_PSRAM_ALLOC_ENABLE
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#endif
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#ifdef USE_NEXTION
#include "displays/nextion.h"
#endif

#if USE_OTA
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
#include <NetworkUdp.h>
#else
#include <WiFiUdp.h>
#endif
#include <ArduinoOTA.h>
#endif

#if DSP_HSPI || TS_HSPI || VS_HSPI
SPIClass  SPI2(HSPI);
#endif

TrackFacts trackFactsPlugin;
SleepTimer sleepTimerPlugin;

#include "plugins/FsInfo/FsInfo.h"
FsInfo fsInfoPlugin;

#if defined(MBEDTLS_PSRAM_ALLOC_ENABLE) && MBEDTLS_PSRAM_ALLOC_ENABLE
/* mbedTLS → PSRAM: HTTPS-FLAC держит TLS в internal RAM → iTunes handshake -32512 без этого. */
static void* mbedtlsPsramCalloc(size_t count, size_t size) {
  if (count == 0 || size == 0 || count > (SIZE_MAX / size)) return nullptr;
  void* ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ptr) {
    ptr = heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return ptr;
}

static void mbedtlsPsramFree(void* ptr) {
  heap_caps_free(ptr);
}
#endif

extern __attribute__((weak)) void yoradio_on_setup();
extern volatile bool g_sleepTimerShutdown;

// LittleFS fail на cold boot после ES9038-рестарта: RTC-счётчик вместо while(true).
RTC_DATA_ATTR static uint8_t g_littleFsBootRetryCount = 0;
#define LITTLEFS_BOOT_RETRY_MAX  3

/* Boot-loop guard убран: ложные INT_WDT при ES9038 cold-start сбрасывали SmartStart.
 * Автостарт отменяет только явный Стоп (wasPlaying=false). */

// Плеер на core 0 — блок на потоке не стопорит веб/тач на core 1.
#define PLAYER_TASK_STACK  12288
#define PLAYER_TASK_CORE   0
#define PLAYER_TASK_PRIO   1
static void playerTask(void* arg) {
  for (;;) {
    if (network.status == CONNECTED || network.status == SDREADY)
      player.loop();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/* USE_NETSERVER_TASK: веб на Core 0 (не конкурировать с audio на Core 1). Сейчас 0 — откат: в loop() Core 1. */
#define USE_NETSERVER_TASK    0
#define NETSERVER_TASK_CORE   0
#define NETSERVER_TASK_PRIO   1
#define NETSERVER_TASK_STACK  16384
#if USE_NETSERVER_TASK
static TaskHandle_t g_netserverTaskHandle = nullptr;
static void netserverTask(void* arg) {
  for (;;) {
    netserver.loop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
static void startNetserverTaskOnce() {
  if (g_netserverTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(netserverTask, "NetSrv", NETSERVER_TASK_STACK, NULL,
                          NETSERVER_TASK_PRIO, &g_netserverTaskHandle, NETSERVER_TASK_CORE);
}
#endif

#if USE_OTA
void setupOTA(){
  char safeHost[MDNS_LENGTH] = {0};
  strncpy(safeHost, config.store.mdnsname, sizeof(safeHost) - 1);
  safeHost[sizeof(safeHost) - 1] = '\0';

  bool hostOk = (safeHost[0] != '\0');
  size_t hostLen = strlen(safeHost);
  if (hostOk && (safeHost[0] == '-' || safeHost[hostLen - 1] == '-')) {
    hostOk = false;
  }
  if (hostOk) {
    for (size_t i = 0; i < hostLen; ++i) {
      char c = safeHost[i];
      bool isLower = (c >= 'a' && c <= 'z');
      bool isUpper = (c >= 'A' && c <= 'Z');
      bool isDigit = (c >= '0' && c <= '9');
      bool isDash = (c == '-');
      if (!(isLower || isUpper || isDigit || isDash)) {
        hostOk = false;
        break;
      }
    }
  }
  if (!hostOk) {
    snprintf(safeHost, sizeof(safeHost), "yoradio-%x", (unsigned int)getChipId());
  }

  ArduinoOTA.setHostname(safeHost);
#ifdef OTA_PASS
  ArduinoOTA.setPassword(OTA_PASS);
#endif
  ArduinoOTA
    .onStart([]() {
      player.sendCommand({PR_STOP, 0});
      display.putRequest(NEWMODE, UPDATING);
      telnet.printf("Start OTA updating %s\n", ArduinoOTA.getCommand() == U_FLASH?"firmware":"filesystem");
    })
    .onEnd([]() {
      telnet.printf("\nEnd OTA update, Rebooting...\n");
      ESP.restart();
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      telnet.printf("Progress OTA: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      telnet.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) {
        telnet.printf("Auth Failed\n");
      } else if (error == OTA_BEGIN_ERROR) {
        telnet.printf("Begin Failed\n");
      } else if (error == OTA_CONNECT_ERROR) {
        telnet.printf("Connect Failed\n");
      } else if (error == OTA_RECEIVE_ERROR) {
        telnet.printf("Receive Failed\n");
      } else if (error == OTA_END_ERROR) {
        telnet.printf("End Failed\n");
      }
    });
  ArduinoOTA.begin();
}
#endif

void setup() {
  Serial.begin(115200);
  // Иначе FREEZE-RECOVERY ложно сработает до первого loop() (heartbeat==0).
  g_mainLoopHeartbeatMs = millis();
  heapDiagInit();

  /* После deep sleep: снять RTC hold до init тача/I2C. */
  {
    const esp_reset_reason_t rr = esp_reset_reason();
    const esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
    Serial.printf("##[BOOT]# reset_reason=%d (5=DSLEEP) wakeup_cause=%d (2=EXT0)\n",
                  (int)rr, (int)wc);
#if (TS_MODEL == TS_MODEL_CST820) && (TS_RST != 255)
    rtc_gpio_hold_dis(static_cast<gpio_num_t>(TS_RST));
#endif
#if WAKE_PIN != 255
    rtc_gpio_hold_dis(static_cast<gpio_num_t>(WAKE_PIN));
#endif
  }

  // ES9038: cold POWERON → SW-restart, иначе I2S часто мёртв.
  #if USE_ES9038_DAC
  {
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
      Serial.println("##[BOOT]# COLD START. Wait 0.5s and restart...");
      delay(500);
      ESP.restart();
    }
  }
  #endif

  Serial.println("##[BOOT]#\tsetup: before psramInit");
  Serial.flush();
  if (!psramInit()) {
    Serial.println("##[ERROR]#\tPSRAM init failed!");
  } else {
    Serial.printf("##[BOOT]#\tPSRAM: %lu bytes\n", ESP.getPsramSize());
#if defined(MBEDTLS_PSRAM_ALLOC_ENABLE) && MBEDTLS_PSRAM_ALLOC_ENABLE
    /* Allocator до Wi-Fi/плеера — ещё нет активных TLS-сессий. */
    const int allocResult = mbedtls_platform_set_calloc_free(mbedtlsPsramCalloc, mbedtlsPsramFree);
    Serial.printf("##[BOOT]#\tmbedTLS PSRAM allocator: %s\n", allocResult == 0 ? "enabled" : "failed");
#endif
  }
  Serial.flush();
  delay(100);

  if(REAL_LEDBUILTIN!=255) {
#if defined(PIN_RGB_LED)
    /* Не GPIO48=TFT_DC на JC2432 (виртуальный RGB_BUILTIN). */
    if (REAL_LEDBUILTIN == (SOC_GPIO_PIN_COUNT + PIN_RGB_LED) || REAL_LEDBUILTIN == PIN_RGB_LED) {
      Serial.println("##[BOOT]#\tskip LED_BUILTIN (GPIO48 = TFT_DC on JC2432)");
    } else
#endif
    {
      pinMode(REAL_LEDBUILTIN, OUTPUT);  // без gpio_reset_pin — циклический ребут
    }
  }
  delay(50);

  config.init();
  delay(100);

  // Не hang на LittleFS: до N ESP.restart(), потом setup с WARN.
  if (!littleFsReady) {
    if (g_littleFsBootRetryCount < LITTLEFS_BOOT_RETRY_MAX) {
      ++g_littleFsBootRetryCount;
      Serial.printf("##[BOOT]# LittleFS not ready — auto-restart %u/%u (RTC retry)\n",
                    (unsigned)g_littleFsBootRetryCount, (unsigned)LITTLEFS_BOOT_RETRY_MAX);
      delay(500);
      ESP.restart();
    }
  } else {
    g_littleFsBootRetryCount = 0;
  }
  if (!littleFsReady) {
    g_littleFsBootRetryCount = 0;
    Serial.println("##[WARN]## littleFsReady==0 after retries; setup continues. Re-flash LittleFS if problems persist.");
  }

  if (yoradio_on_setup) yoradio_on_setup();

  pluginsManager::getInstance().on_setup();

  trackFactsPlugin.setEnabled(config.store.trackFactsEnabled);
  trackFactsPlugin.setGeminiApiKey(String(config.store.geminiApiKey));
  trackFactsPlugin.setLanguage(static_cast<FactLanguage>(config.store.trackFactsLang));
  uint8_t factCount = config.store.trackFactsCount;
  if (factCount == 0 || factCount > TRACKFACTS_CONFIG_COUNT_MAX) factCount = 1;
  trackFactsPlugin.setFactCount(factCount);
  uint8_t provider = config.store.trackFactsProvider;
  if (provider > 4) provider = 2;
  trackFactsPlugin.setProvider(provider);
  display.init();
#if DSP_MODEL==DSP_AXS15231B
  /* WebUI PSRAM-кэш после boot-лого — иначе cold-boot темнеет дольше. */
  initWebAssetsCache();
#endif
  player.init();
  network.begin();
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
  /* WIFI_OFF / STA-fail: AsyncWebServer::begin без lwIP → assert. SoftAP/STA OK — ниже. */
  {
    char step = (char)ST7701_RGB_SOAK_STEP;
    if (step >= 'A' && step <= 'E') step = (char)(step - 'A' + 'a');
    const bool wifiOffSteps = (step == 'a' || step == 'b');
    const bool noLwip = wifiOffSteps || (step == 'c' && network.status != CONNECTED);
    if (noLwip) {
      Serial.println("##[BOOT]#\t[SOAK] skip netserver/telnet (no lwIP)");
      initControls();
      display.putRequest(DSP_START);
      while (!display.ready()) vTaskDelay(pdMS_TO_TICKS(10));
      Serial.println("##[BOOT]#\t[SOAK] ready — watch pattern + slow-mo");
      return;
    }
  }
#endif
  if (network.status != CONNECTED && network.status!=SDREADY) {
    netserver.begin();
    initControls();
    display.putRequest(DSP_START);
    while(!display.ready()) vTaskDelay(pdMS_TO_TICKS(10));
    xTaskCreatePinnedToCore(playerTask, "PlayerTask", PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO, NULL, PLAYER_TASK_CORE);
    #if USE_NETSERVER_TASK
    startNetserverTaskOnce();
    #endif
    return;
  }
  if(SDC_CS!=255) {
    display.putRequest(WAITFORSD, 0);
    Serial.print("##[BOOT]#\tSD search\t");
  }
  config.initPlaylistMode();
  netserver.begin();
  telnet.begin();
  initControls();
  display.putRequest(DSP_START);
  while(!display.ready()) vTaskDelay(pdMS_TO_TICKS(10));
  #if USE_OTA
    setupOTA();
  #endif
  if (config.getMode()==PM_SDCARD) player.initHeaders(config.station.url);
  player.lockOutput=false;
  // SmartStart в setup: wasPlaying сбрасывает только Стоп; ошибка connect → auto-retry в loop.
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
  Serial.println("##[BOOT]#\t[SOAK] smartstart/audio suppressed");
#else
  if (config.store.smartstart == 1 && config.store.wasPlaying) {
    uint16_t st = config.lastStation();
    if (st == 0 && config.playlistLength() > 0) st = 1;
    if (st > 0) {
      vTaskDelay(pdMS_TO_TICKS(99));
      player.sendCommand({PR_PLAY, (int)st});
    }
  }
#endif
  xTaskCreatePinnedToCore(playerTask, "PlayerTask", PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO, NULL, PLAYER_TASK_CORE);
  #if USE_NETSERVER_TASK
  startNetserverTaskOnce();
  #endif
  pluginsManager::getInstance().on_end_setup();
  {
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    Serial.printf("##[BOOT]# loopTask свободно стека: ~%u байт (чем больше — тем лучше; <2K — риск ipc1)\n",
                  (unsigned)(hwm * sizeof(StackType_t)));
  }
  // Приоритет loop() не поднимать (побочки с дисплеем/сетью); FREEZE — в timekeeper.
}

#include "core/audiohandlers.h"

void loop() {
  g_mainLoopHeartbeatMs = millis();
  // Тач/веб до блокировок player (parseHttpResponseHeader).
  loopControls();
  #if defined(NETSERVER_LOOP1) && !USE_NETSERVER_TASK
  netserver.loop();
  #endif

  timekeeper.loop1();
  telnet.loop();

  flushSdId3Meta();
  config.processFavoriteUpdate();
  config.processSDIndexRebuild();
  config.updateConnectTitleFallback(); // WEB без тегов: долгое «[соединение]» → имя станции

  heapDiagPoll();
  // Диагностика температуры кристалла: [TEMP] °C раз в TEMP_DIAG_INTERVAL_MS (TEMP_DIAG_ENABLE в myoptions).
  tempDiagPoll();

#if USE_OTA
  if (network.status == CONNECTED || network.status==SDREADY)
    ArduinoOTA.handle();
#endif

  // SD: WiFi.reconnect не из обработчика события (rst:0xc).
  if (network.status == SDREADY && network.sdReconnectAfterMs != 0 && millis() >= network.sdReconnectAfterMs) {
    network.sdReconnectAfterMs = 0;
    Serial.println("[WIFI] SD mode: deferred reconnect");
    WiFi.reconnect();
  }

  // WiFi health: 30 с → soft reconnect; 120 с → OFF/STA begin. SoftAP не трогаем.
  {
    static uint32_t lastWiFiCheck = 0;
    static uint32_t wifiLostSince = 0;
    static uint32_t lastReconnectAttempt = 0;
    static uint32_t lastHardReset = 0;
    uint32_t now = millis();
    if (now - lastWiFiCheck > 5000) {
      lastWiFiCheck = now;
      const bool wifiWdtArmed = (network.status == CONNECTED || network.status == SDREADY)
                                && (WiFi.getMode() & WIFI_MODE_STA);
      if (!wifiWdtArmed) {
        wifiLostSince = 0;
        lastReconnectAttempt = 0;
        lastHardReset = 0;
      } else if (WiFi.status() != WL_CONNECTED) {
        if (wifiLostSince == 0) {
          wifiLostSince = now;
          lastReconnectAttempt = 0;
          lastHardReset = 0;
        } else if (now - wifiLostSince > 30000) {
          if (now - lastReconnectAttempt > 30000) {
            lastReconnectAttempt = now;
            Serial.println("[WIFI-WDT] WiFi lost >30s, forcing reconnect");
            WiFi.disconnect(false);
            delay(100);
            WiFi.reconnect();
          }
          if (now - wifiLostSince > 120000 && now - lastHardReset > 120000) {
            lastHardReset = now;
            uint8_t ls = (config.store.lastSSID == 0 || config.store.lastSSID > config.ssidsCount) ? 0 : config.store.lastSSID - 1;
            if (config.ssidsCount > 0) {
              WiFi.disconnect(false);
              WiFi.mode(WIFI_OFF);
              delay(200);
              WiFi.mode(WIFI_STA);
              WiFi.begin(config.ssids[ls].ssid, config.ssids[ls].password);
            } else {
              WiFi.disconnect(false);
              delay(100);
              WiFi.reconnect();
            }
          }
        }
      } else {
        wifiLostSince = 0;
        lastReconnectAttempt = 0;
        lastHardReset = 0;
      }
    }
  }

  // Auto-retry стрима (smartstart=1): backoff 5→40 с; только requestWebRecovery (без disconnect в loop).
#if !((DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK)
  {
    static uint32_t lastRetryMs = 0;
    static uint8_t  retryCount  = 0;

    if (player.isRunning()) {
      retryCount  = 0;
      lastRetryMs = 0;
      g_sleepTimerShutdown = false;
    } else if (!player.connproc) {
      // connect ещё идёт — не второй PR_PLAY (DNS -54).
      lastRetryMs = 0;
    } else if (config.store.smartstart == 1 && config.store.wasPlaying
               && !player.isRunning() && !g_sleepTimerShutdown // sleep-timer: не перезапускать эфир
               && config.getMode() == PM_WEB && WiFi.status() == WL_CONNECTED) {
      uint32_t now = millis();
      if (lastRetryMs == 0) lastRetryMs = now;
      uint32_t delayMs = 5000u * (1u << min(retryCount, (uint8_t)3));
      if (retryCount >= 2) delayMs += 15000u; // иначе шторм TLS/DNS, веб :80 мёртв
      if (now - lastRetryMs >= delayMs) {
        // Пачки по 5; счётчик только после успешной постановки recovery.
        if (retryCount < 5) {
          if (player.requestWebRecovery("auto-retry")) {
            lastRetryMs = now;
            retryCount++;
            Serial.printf("[RETRY] Auto-retry #%d, delay was %lu ms\n", retryCount, delayMs);
          }
        } else {
          Serial.println("[RETRY] Сдаёмся после 5 попыток (новая пачка через 5 с)");
          retryCount = 0;
          lastRetryMs = now;
        }
      }
    }
  }
#endif
}
