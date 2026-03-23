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

// Инстанцирование плагинов
TrackFacts trackFactsPlugin;
SleepTimer sleepTimerPlugin;

// [v0.4.2] Плагин информации о свободном месте в файловой системе
#include "plugins/FsInfo/FsInfo.h"
FsInfo fsInfoPlugin;

extern __attribute__((weak)) void yoradio_on_setup();
extern volatile bool g_sleepTimerShutdown; // Глобальный флаг отключения по таймеру сна.

// [FIX] Задача плеера на core 0: при блокировке потока (реконнект, парсинг и т.д.) блокируется только она,
// главный цикл (core 1) продолжает работать — веб и тач отзывчивы, пользователь может остановить/сменить станцию.
#define PLAYER_TASK_STACK  12288
#define PLAYER_TASK_CORE   0  // core 1 = main loop; когда плеер блокируется здесь, main loop не страдает
#define PLAYER_TASK_PRIO   1
static void playerTask(void* arg) {
  for (;;) {
    if (network.status == CONNECTED || network.status == SDREADY)
      player.loop();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

#if USE_OTA
void setupOTA(){
  char safeHost[MDNS_LENGTH] = {0};
  strncpy(safeHost, config.store.mdnsname, sizeof(safeHost) - 1);
  safeHost[sizeof(safeHost) - 1] = '\0';

  // Минимальная валидация имени хоста для mDNS/OTA:
  // только [a-zA-Z0-9-], не пустое, не начинается/заканчивается '-'.
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

  // Костыль: при холодном старте I2S/ES9038 может не инициализироваться корректно.
  // Принудительный рестарт переводит в ESP_RST_SW, где всё работает стабильно.
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

  // Стабилизация при старте: инициализация PSRAM и задержки для надёжного холодного пуска
  if (!psramInit()) {
    Serial.println("##[ERROR]#\tPSRAM init failed!");
  } else {
    Serial.printf("##[BOOT]#\tPSRAM: %lu bytes\n", ESP.getPsramSize());
  }
  delay(100);  // Стабилизация после power-on (100ms — безопасно для watchdog)

  if(REAL_LEDBUILTIN!=255) {
    pinMode(REAL_LEDBUILTIN, OUTPUT);  // Без gpio_reset_pin — он вызывает циклический ребут
  }
  delay(50);   // Стабилизация GPIO

  // 1. Инициализируем конфигурацию и LittleFS ПЕРВЫМИ
  config.init();
  delay(100);  // Короткая стабилизация после config.init()
  
  if (yoradio_on_setup) yoradio_on_setup();
  
  // 2. Инициализируем плагины (теперь LittleFS точно готов)
  pluginsManager::getInstance().on_setup();
  
  // Загрузка настроек TrackFacts из config (по умолчанию выключен)
  trackFactsPlugin.setEnabled(config.store.trackFactsEnabled);
  // Передаём ключ (если есть)
  trackFactsPlugin.setGeminiApiKey(String(config.store.geminiApiKey));
  trackFactsPlugin.setLanguage(static_cast<FactLanguage>(config.store.trackFactsLang));
  // Если count=0 или мусор, устанавливаем по умолчанию 1
  uint8_t factCount = config.store.trackFactsCount;
  if(factCount == 0 || factCount > 5) factCount = 1;
  trackFactsPlugin.setFactCount(factCount);
  // Провайдер из конфига (failover на iTunes если нужен ключ но его нет - в checkFailover)
  uint8_t provider = config.store.trackFactsProvider;
  if (provider > 4) provider = 2; // Мусор в EEPROM -> iTunes
  trackFactsPlugin.setProvider(provider);
  display.init();
  player.init();
  network.begin();
  if (network.status != CONNECTED && network.status!=SDREADY) {
    netserver.begin();
    initControls();
    display.putRequest(DSP_START);
    while(!display.ready()) vTaskDelay(pdMS_TO_TICKS(10));
    xTaskCreatePinnedToCore(playerTask, "PlayerTask", PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO, NULL, PLAYER_TASK_CORE);
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
  // [FIX SmartStart] Автостарт только если: smartstart==1, wasPlaying==true и есть валидная станция.
  // После зависания/перезагрузки lastStation мог не загрузиться — не шлём PR_PLAY(0).
  if (config.store.smartstart == 1 && config.store.wasPlaying) {
    uint16_t st = config.lastStation();
    if (st == 0 && config.playlistLength() > 0) st = 1;
    if (st > 0) {
      vTaskDelay(pdMS_TO_TICKS(99));
      player.sendCommand({PR_PLAY, (int)st});
    }
  }
  // Задача плеера на core 0: поток может блокироваться — тогда виснет только плеер, веб/тач (core 1) работают.
  xTaskCreatePinnedToCore(playerTask, "PlayerTask", PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO, NULL, PLAYER_TASK_CORE);
  pluginsManager::getInstance().on_end_setup();

  // Приоритет loop() не поднимаем: vTaskPrioritySet(2) на core 1 давал ощутимые побочные эффекты
  // в связке с дисплеем/сетью. Защита от ложного FREEZE — в timekeeper (стрим + таймаут) и
  // низкий приоритет CoverDL/FactsTask (0).
}

#include "core/audiohandlers.h"

void loop() {
  // Сердцебиение для обнаружения зависания: задача на core 0 проверяет; если >35 сек нет обновления — мягкий WiFi reconnect без перезагрузки.
  g_mainLoopHeartbeatMs = millis();
  // [FIX] Сначала обрабатываем тач и веб — при реконнекте потока player.loop() может блокироваться
  // в parseHttpResponseHeader() до таймаута (2 сек). 
  loopControls();
  #ifdef NETSERVER_LOOP1
  netserver.loop();
  #endif

  timekeeper.loop1();
  telnet.loop();

  // [FIX] Проверяем debounce ID3 тегов для SD режима
  flushSdId3Meta();

  // [FIX] Отложенное сохранение избранного и перестройка индекса SD (чтобы не блокировать веб).
  config.processFavoriteUpdate();
  config.processSDIndexRebuild();

  // [FIX WEB без тегов] Обновляем заголовок для станций без метаданных:
  // если после "[соединение]" долго нет тега, подменяем его на имя станции.
  config.updateConnectTitleFallback();

  // player.loop() вынесен в playerTask (core 0) — главный цикл не блокируется на потоке, веб/тач всегда отзывчивы.
#if USE_OTA
  if (network.status == CONNECTED || network.status==SDREADY)
    ArduinoOTA.handle();
#endif

  // [FIX] В SD при потере WiFi reconnect перенесён из обработчика события в main loop,
  // чтобы избежать перезагрузки (вызов WiFi.reconnect() из контекста события при разрыве
  // сокетов приводил к rst:0xc). Выполняем с задержкой ~1.5 с после sdReconnectAfterMs.
  if (network.status == SDREADY && network.sdReconnectAfterMs != 0 && millis() >= network.sdReconnectAfterMs) {
    network.sdReconnectAfterMs = 0;
    Serial.println("[WIFI] SD mode: deferred reconnect");
    WiFi.reconnect();
  }

  // [FIX] WiFi health-check: если WiFi потерян и не восстановился за 30 сек,
  // принудительно reconnect. Спасает от зависания при микро-помехах
  {
    static uint32_t lastWiFiCheck = 0; // Метка последней проверки статуса WiFi.
    static uint32_t wifiLostSince = 0; // Метка начала потери WiFi.
    static uint32_t lastReconnectAttempt = 0; // Метка последней попытки reconnection.
    static uint32_t lastHardReset = 0; // Метка последнего жёсткого перезапуска WiFi-стека.
    uint32_t now = millis();
    if (now - lastWiFiCheck > 5000) {
      lastWiFiCheck = now;
      if (WiFi.status() != WL_CONNECTED) {
        if (wifiLostSince == 0) {
          wifiLostSince = now; // Фиксируем момент потери связи.
          lastReconnectAttempt = 0; // Сбрасываем таймер попыток переподключения.
          lastHardReset = 0; // Сбрасываем таймер жёсткого ресета.
        } else if (now - wifiLostSince > 30000) {
          if (now - lastReconnectAttempt > 30000) {
            lastReconnectAttempt = now;
            Serial.println("[WIFI-WDT] WiFi lost >30s, forcing reconnect");
            WiFi.disconnect(false); // Мягко разрываем соединение без очистки сохранённых SSID.
            delay(100); // Короткая пауза, чтобы стек применил разрыв.
            WiFi.reconnect(); // Повторная попытка подключиться к последней сети.
          }
          if (now - wifiLostSince > 120000 && now - lastHardReset > 120000) {
            lastHardReset = now;
            uint8_t ls = (config.store.lastSSID == 0 || config.store.lastSSID > config.ssidsCount) ? 0 : config.store.lastSSID - 1; // Вычисляем индекс последней сети.
            if (config.ssidsCount > 0) {
              WiFi.disconnect(false); // Разрываем соединение перед полным перезапуском WiFi.
              WiFi.mode(WIFI_OFF); // Полностью выключаем WiFi-радиомодуль.
              delay(200); // Даём модулю корректно перейти в OFF.
              WiFi.mode(WIFI_STA); // Возвращаем режим клиента.
              WiFi.begin(config.ssids[ls].ssid, config.ssids[ls].password); // Запускаем подключение к сохранённой сети.
            } else {
              WiFi.disconnect(false); // Разрываем соединение, если сетей нет в конфиге.
              delay(100); // Небольшая пауза перед повторной попыткой.
              WiFi.reconnect(); // Пробуем восстановить соединение.
            }
          }
        }
      } else {
        wifiLostSince = 0; // Сбрасываем отметку потери связи при успешном подключении.
        lastReconnectAttempt = 0; // Сбрасываем таймер переподключения.
        lastHardReset = 0; // Сбрасываем таймер жёсткого ресета.
      }
    }
  }

  // [FIX] Auto-retry: если стрим упал с ошибкой (smartstart остался 1),
  // пробуем переподключиться с нарастающей задержкой (5→10→20→40→40 сек).

  {
    static uint32_t lastRetryMs = 0;
    static uint8_t  retryCount  = 0;

    if (player.isRunning()) {
      retryCount  = 0;
      lastRetryMs = 0;
      g_sleepTimerShutdown = false; // Снимаем блокировку, если воспроизведение снова запущено.
    } else if (config.store.smartstart == 1 && config.store.wasPlaying
               && !player.isRunning() && !g_sleepTimerShutdown // Не перезапускаем, пока идёт сценарий таймера сна.
               && config.getMode() == PM_WEB && WiFi.status() == WL_CONNECTED) {
      uint32_t now = millis();
      if (lastRetryMs == 0) lastRetryMs = now;             // первый обрыв — засекаем
      uint32_t delayMs = 5000u * (1u << min(retryCount, (uint8_t)3)); // 5, 10, 20, 40 сек
      if (now - lastRetryMs >= delayMs) {
        lastRetryMs = now;
        retryCount++;
        if (retryCount == 3) {
          // После трёх неудачных попыток считаем, что мог «залипнуть» WiFi/DNS.
          // Выполняем мягкий перезапуск WiFi без стирания сохранённых сетей:
          Serial.println("[RETRY] 3 fails in a row, restarting WiFi stack");
          WiFi.disconnect(false);
          delay(100);
          WiFi.reconnect();
        }
        if (retryCount <= 5) {
          Serial.printf("[RETRY] Auto-retry #%d, delay was %lu ms\n", retryCount, delayMs);
          player.sendCommand({PR_PLAY, config.lastStation()});
        } else {
          Serial.println("[RETRY] Сдаёмся после 5 попыток");
          // ВАЖНО: не отключаем SmartStart автоматически.
          // Пользовательский флаг должен сохраняться, чтобы следующая
          // успешная реконнекция или перезагрузка снова пыталась возобновить воспроизведение.
          retryCount = 0;
          lastRetryMs = 0;
        }
      }
    }
  }
}
