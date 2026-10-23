#include "SleepTimer.h"

#include "../../core/options.h"
#include "../../core/config.h"
#include "../../core/player.h"
#include "../../core/netserver.h"
#include "../../core/touchscreen.h"
#include <esp_sleep.h>  // light/deep sleep и gpio wakeup
#include "driver/rtc_io.h"  // RTC pull-up / hold пинов во время sleep
// g_mainLoopHeartbeatMs уже через config.h — обновляем в цикле light sleep, чтобы FREEZE-RECOVERY не ребутил.

volatile bool g_sleepTimerShutdown = false; // Флаг блокировки автоповтора и автозапуска во время отключения по таймеру сна.

// Таблица пресетов таймера сна в минутах.
// Первый пресет = 2 минуты (для отладки), далее стандартные значения.
// [DEBUG] Для отладки первый пресет = 2мин. В продакшне заменить 2 на 15.
static const uint16_t kSleepPresets[] = {0, 2, 30, 45, 60, 90, 120};
static const uint8_t kSleepPresetsCount = sizeof(kSleepPresets) / sizeof(kSleepPresets[0]);

SleepTimer::SleepTimer()
  : _selectedMinutes(0),
    _deadlineMs(0),
    _allOffEnabled(false),
    _lastMinuteBucket(255) {
  // Регистрируем плагин в менеджере событий, чтобы on_setup/on_ticker вызывались автоматически.
  registerPlugin();
}

void SleepTimer::on_setup() {
  // Инициализируем пин аппаратного отключения питания только если он действительно задан.
  if (hasPowerPin()) {
    pinMode(SLEEP_TIMER_PIN, OUTPUT);
    _applyPowerPinInactive();
  }

  // Читаем сохранённое состояние из EEPROM. Если аппаратный пин не задан — принудительно false.
  _allOffEnabled = hasPowerPin() ? config.store.sleepTimerAllOff : false;

  // Сразу отправляем начальное состояние, чтобы Web UI корректно отрисовал кнопку и переключатель.
  pushState();
}

void SleepTimer::on_ticker() {
  // Если таймер не активен, периодически отправлять обновление не нужно.
  if (_deadlineMs == 0) {
    return;
  }

  uint32_t leftSec = remainingSeconds();

  // Обновляем Web UI только при смене "корзины" по 15 минутам, чтобы не спамить сеть лишними пакетами.
  // Отдельно разрешаем частые обновления на последней минуте для эффекта мигания индикатора.
  uint8_t minuteBucket = static_cast<uint8_t>(leftSec / 900U);
  bool lastMinute = (leftSec <= 60U);
  if (minuteBucket != _lastMinuteBucket || lastMinute) {
    _lastMinuteBucket = minuteBucket;
    pushState();
  }

  // По истечении таймера выполняем выбранный сценарий (software stop или ALL OFF).
  if (leftSec == 0U) {
    _handleExpired();
  }
}

void SleepTimer::cyclePreset() {
  // Следующее состояние берём из фиксированной циклической таблицы пресетов.
  uint16_t next = _nextPreset(_selectedMinutes);

  if (next == 0U) {
    // Переход в OFF: выключаем таймер и очищаем дедлайн.
    _selectedMinutes = 0;
    _deadlineMs = 0;
    _lastMinuteBucket = 255;
    _showToast("Таймер сна выключен");
    pushState();
    return;
  }

  // При любом ненулевом пресете запускаем таймер заново от текущего момента.
  _selectedMinutes = next;
  _deadlineMs = millis() + (static_cast<uint32_t>(next) * 60UL * 1000UL);
  _lastMinuteBucket = 255;

  char msg[96];
  snprintf(msg, sizeof(msg), "Активен таймер сна на %u мин", static_cast<unsigned>(next));
  _showToast(String(msg));
  pushState();
}

void SleepTimer::setAllOffEnabled(bool enabled) {
  // Если аппаратный пин не задан, принудительно удерживаем режим выключенным.
  if (!hasPowerPin()) {
    _allOffEnabled = false;
    config.store.sleepTimerAllOff = false;
    pushState();
    return;
  }

  _allOffEnabled = enabled;
  // Сохраняем состояние в EEPROM, чтобы переживало перезагрузку.
  config.saveValue(&config.store.sleepTimerAllOff, enabled);

  if (_allOffEnabled) {
    _showToast("Sleep timer ALL OFF: включен");
  } else {
    _showToast("Sleep timer ALL OFF: выключен");
    // При выключении ALL OFF сразу возвращаем пин в неактивное состояние.
    _applyPowerPinInactive();
  }

  pushState();
}

uint32_t SleepTimer::remainingSeconds() const {
  if (_deadlineMs == 0) {
    return 0;
  }

  uint32_t now = millis();
  if (now >= _deadlineMs) {
    return 0;
  }

  return static_cast<uint32_t>((_deadlineMs - now) / 1000UL);
}

bool SleepTimer::hasPowerPin() const {
  return SLEEP_TIMER_PIN != 255;
}

void SleepTimer::pushState(uint8_t clientId) {
  // Формируем единый JSON-объект состояния таймера для фронтенда.
  // Параметры:
  // - m: выбранный пресет (минуты),
  // - left: оставшееся время (сек),
  // - active: таймер активен,
  // - alloff: режим аппаратного отключения,
  // - supported: доступен ли аппаратный пин.
  char buf[196];
  snprintf(
    buf,
    sizeof(buf),
    "{\"sleep\":{\"m\":%u,\"left\":%lu,\"active\":%d,\"alloff\":%d,\"supported\":%d}}",
    static_cast<unsigned>(_selectedMinutes),
    static_cast<unsigned long>(remainingSeconds()),
    _deadlineMs != 0 ? 1 : 0,
    _allOffEnabled ? 1 : 0,
    hasPowerPin() ? 1 : 0
  );

  if (websocket.count() == 0) {
    return;
  }
  if (clientId == 0) {
    websocket.textAll(buf);
  } else {
    websocket.text(clientId, buf);
  }
}

uint8_t SleepTimer::_powerActiveLevel() const {
  return SLEEP_TIMER_LEVEL ? HIGH : LOW;
}

uint8_t SleepTimer::_powerInactiveLevel() const {
  return SLEEP_TIMER_LEVEL ? LOW : HIGH;
}

void SleepTimer::_applyPowerPinInactive() {
  if (!hasPowerPin()) {
    return;
  }
  digitalWrite(SLEEP_TIMER_PIN, _powerInactiveLevel());
}

void SleepTimer::_handleExpired() {
  // Сначала сбрасываем внутреннее состояние таймера, чтобы повторно не сработать на следующем тике.
  _selectedMinutes = 0;
  _deadlineMs = 0;
  _lastMinuteBucket = 255;

  // Сообщаем пользователю о срабатывании таймера.
  _showToast("Таймер сна завершен");
  pushState();

  // Если режим ALL OFF не активен (в меню/EEPROM) или аппаратный пин не задан,
  // выполняем мягкий сценарий: просто останавливаем воспроизведение программно.
  //
  // NOTE: используем конфиг как источник истины (а не член _allOffEnabled), чтобы
  // исключить рассинхрон состояния при любых сценариях/вызовах.
  const bool allOffEnabledNow = (config.store.sleepTimerAllOff && hasPowerPin());
  if (!allOffEnabledNow) {
    // Таймер сна = программный стоп → wasPlaying=false.
    // Без этого auto-retry по smartstart мог перезапустить воспроизведение.
    g_sleepTimerShutdown = true; // Ставим блокировку, чтобы авто-ретрай не перезапустил воспроизведение.
    config.saveValue(&config.store.wasPlaying, false); // Фиксируем программную остановку, чтобы SmartStart не срабатывал.
    player.sendCommand({PR_STOP, 0}); // Останавливаем воспроизведение в мягком сценарии.
    return;
  }

  // Аппаратный сценарий выключения:
  // Mute → погасить экран → силовой пин → если реле не обесточило:
  //   CST820 → light sleep + опрос тача по I2C;
  //   остальные → deep sleep по INT/WAKE_PIN/таймеру.
  g_sleepTimerShutdown = true; // Блокируем авто-ретрай на время аппаратного выключения/сна.

  if (MUTE_PIN != 255) {
    pinMode(MUTE_PIN, OUTPUT);
    digitalWrite(MUTE_PIN, MUTE_VAL);
  }

  config.setDspOn(false, false); // не пишем dspon=false в EEPROM — только гасим экран на время сна
  delay(150);

  pinMode(SLEEP_TIMER_PIN, OUTPUT);
  digitalWrite(SLEEP_TIMER_PIN, _powerActiveLevel());

  // Если реле не обесточило плату — через 5 с уходим в программный сон.
  delay(5000);

#if (TS_MODEL == TS_MODEL_CST820)
  // CST820: INT не даёт импульсов (INT-TEST: 0 фронтов) — deep sleep по GPIO бесполезен.
  // Light sleep + FingerNum по I2C: касание экрана будит без зависимости от INT.
  Serial.println("[SleepTimer] Реле не обесточило плату, уходим в softSleep (опрос тача по I2C)");
  Serial.flush();
  _enterLightSleepTouchWake();
  return;
#else
  Serial.println("[SleepTimer] Реле не обесточило плату, уходим в deepSleep");
  Serial.flush();
  // Приоритеты deep sleep:
  //  1) GT911 + TS_INT — пробуждение по тачу (TS_WAKE_LEVEL).
  //  2) WAKE_PIN — кнопка / внешний сигнал (LOW).
  //  3) таймер SLEEP_DEEPSLEEP_WAKE_HOURS (3…8 ч).
  #if (TS_MODEL == TS_MODEL_GT911) && (TS_INT != 255)
    {
      const uint8_t tsWakeLevel = (TS_WAKE_LEVEL == LOW) ? 0 : 1;
      Serial.printf("[SleepTimer] DeepSleep wake by TOUCH (GT911), level=%s, pin=%d\n",
                    (tsWakeLevel == 0) ? "LOW" : "HIGH", TS_INT);
      esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
      esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(TS_INT), tsWakeLevel);
    }
  #elif WAKE_PIN != 255
    {
      const gpio_num_t wakePin = static_cast<gpio_num_t>(WAKE_PIN);
      rtc_gpio_init(wakePin);
      rtc_gpio_set_direction(wakePin, RTC_GPIO_MODE_INPUT_ONLY);
      rtc_gpio_pulldown_dis(wakePin);
      rtc_gpio_pullup_en(wakePin);
      const int idleLevel = rtc_gpio_get_level(wakePin);
      Serial.printf("[SleepTimer] DeepSleep wake by WAKE_PIN (LOW), pin=%d, idle=%d (ожидаем 1)\n",
                    WAKE_PIN, idleLevel);
      Serial.flush();
      esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
      esp_sleep_enable_ext0_wakeup(wakePin, 0 /* LOW */);
      rtc_gpio_hold_en(wakePin);
    }
  #else
    {
      uint8_t wakeHours = SLEEP_DEEPSLEEP_WAKE_HOURS;
      if (wakeHours < 3) wakeHours = 3;
      if (wakeHours > 8) wakeHours = 8;
      Serial.printf("[SleepTimer] Нет тача и WAKE_PIN, таймер-пробуждение на %u ч\n", (unsigned)wakeHours);
      esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(wakeHours) * 3600ULL * 1000000ULL);
    }
  #endif
  #if WAKE_PIN != 255 || ((TS_MODEL == TS_MODEL_GT911) && (TS_INT != 255))
    {
      uint8_t wakeHours = SLEEP_DEEPSLEEP_WAKE_HOURS;
      if (wakeHours < 3) wakeHours = 3;
      if (wakeHours > 8) wakeHours = 8;
      Serial.printf("[SleepTimer] запасной таймер-wake %u ч\n", (unsigned)wakeHours);
      esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(wakeHours) * 3600ULL * 1000000ULL);
    }
  #endif
  Serial.flush();
  esp_deep_sleep_start();
#endif
}

void SleepTimer::_enterLightSleepTouchWake() {
  // Останавливаем плеер до сна. wasPlaying=true — после касания вернём эфир.
  player.sendCommand({PR_STOP, 0});
  delay(200);
  config.saveValue(&config.store.wasPlaying, true);

  // Тач должен отвечать по I2C (запрет авто-Standby). INT не используем.
  touchscreen.prepareLightSleepWake();

  // Подсветка уже погашена в setDspOn(false). Пин BL не переконфигурируем —
  // remux LEDC→GPIO давал вспышку. Запись в BL во сне режется g_sleepTimerShutdown.

  // esp_light_sleep на этом S3 → RTCWDT. Soft-sleep: опрос FingerNum.
  // 100 мс: короткое касание не проскакивает между опросами (раньше 500 мс).
  const uint32_t pollMs = 100UL;
  uint8_t wakeHours = SLEEP_DEEPSLEEP_WAKE_HOURS;
  if (wakeHours < 3) wakeHours = 3;
  if (wakeHours > 8) wakeHours = 8;
  const uint32_t maxMs = static_cast<uint32_t>(wakeHours) * 3600UL * 1000UL;
  const uint32_t startedMs = millis();
  uint32_t pollCount = 0;
  const char* wakeReason = "timeout";

  Serial.printf("[SleepTimer] softSleep: poll=%lu мс, лимит=%u ч, касание=I2C",
                (unsigned long)pollMs, (unsigned)wakeHours);
#if WAKE_PIN != 255
  Serial.printf(", запасной GPIO%d→GND", WAKE_PIN);
  pinMode(WAKE_PIN, INPUT_PULLUP);
#endif
  Serial.println();
  Serial.flush();

  while ((millis() - startedMs) < maxMs) {
    g_mainLoopHeartbeatMs = millis();

#if WAKE_PIN != 255
    if (digitalRead(WAKE_PIN) == LOW) {
      wakeReason = "WAKE_PIN";
      break;
    }
#endif
    // До 3 быстрых попыток: одиночный I2C-сбой не должен требовать второго тычка.
    bool finger = false;
    for (uint8_t t = 0; t < 3; ++t) {
      if (touchscreen.pollFingerPresent()) { finger = true; break; }
      if (t < 2) delay(5);
    }
    if (finger) {
      wakeReason = "touch";
      break;
    }

    delay(pollMs);
    pollCount++;

    // ~30 с при poll=100 мс → каждые 300 циклов
    if ((pollCount % 300U) == 0U) {
      Serial.printf("[SleepTimer] softSleep alive, ~%lu с\n",
                    (unsigned long)((millis() - startedMs) / 1000UL));
      Serial.flush();
    }
  }

  Serial.printf("[SleepTimer] softSleep EXIT: reason=%s, slept≈%lu с, polls=%lu\n",
                wakeReason,
                (unsigned long)((millis() - startedMs) / 1000UL),
                (unsigned long)pollCount);
  Serial.flush();

  _applyPowerPinInactive();
  if (MUTE_PIN != 255) {
    digitalWrite(MUTE_PIN, MUTE_LOCK ? MUTE_VAL : !MUTE_VAL);
  }

  // Сначала снимаем флаг — иначе setBrightness откажется писать в BL.
  g_sleepTimerShutdown = false;
  g_mainLoopHeartbeatMs = millis();

  config.setDspOn(true, true);
#if YO_BACKLIGHT_CTRL
  config.setBrightness(false);
  Serial.printf("[SleepTimer] дисплей восстановлен: dspon=%d brightness=%u\n",
                (int)config.store.dspon, (unsigned)config.store.brightness);
#endif
  _showToast("Пробуждение");
  pushState();

  if (config.store.smartstart == 1) {
    player.sendCommand({PR_PLAY, config.lastStation()});
  }
}
uint16_t SleepTimer::_nextPreset(uint16_t current) const {
  for (uint8_t i = 0; i < kSleepPresetsCount; ++i) {
    if (kSleepPresets[i] == current) {
      uint8_t nextIndex = static_cast<uint8_t>((i + 1U) % kSleepPresetsCount);
      return kSleepPresets[nextIndex];
    }
  }

  // Если текущее значение внезапно вне таблицы (например, после обновления прошивки),
  // безопасно возвращаемся к первому рабочему пресету 15 минут.
  return 15;
}

void SleepTimer::_showToast(const String& text) {
  // Используем штатный формат toast, который уже поддерживается фронтендом.
  char buf[220];
  snprintf(buf, sizeof(buf), "{\"toast\":\"%s\",\"isErr\":0}", text.c_str());
  websocket.textAll(buf);
}
