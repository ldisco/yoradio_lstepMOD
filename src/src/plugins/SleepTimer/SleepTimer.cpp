#include "SleepTimer.h"

#include "../../core/options.h"
#include "../../core/config.h"
#include "../../core/player.h"
#include "../../core/netserver.h"
#include <esp_sleep.h>  // [FIX Задача 2] Для esp_deep_sleep_start() и gpio wakeup

volatile bool g_sleepTimerShutdown = false; // Флаг блокировки автоповтора и автозапуска во время отключения по таймеру сна.

// Таблица пресетов таймера сна в минутах.
// [FIX Задача 2] Первый пресет = 2 минуты (для отладки), далее стандартные значения.
// [DEBUG] Для отладки первый пресет = 2мин. В продакшне заменить 2 на 15.
static const uint16_t kSleepPresets[] = {0, 15, 30, 45, 60, 90, 120};
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
    // [FIX SmartStart] Таймер сна = программный стоп → wasPlaying=false.
    // Без этого auto-retry по smartstart мог перезапустить воспроизведение.
    g_sleepTimerShutdown = true; // Ставим блокировку, чтобы авто-ретрай не перезапустил воспроизведение.
    config.saveValue(&config.store.wasPlaying, false); // Фиксируем программную остановку, чтобы SmartStart не срабатывал.
    player.sendCommand({PR_STOP, 0}); // Останавливаем воспроизведение в мягком сценарии.
    return;
  }

  // Аппаратный сценарий выключения (ВАЖНО: не вызываем player.stop() напрямую):
  // 1) wasPlaying=false, 2) Stop плеер, 3) Mute, 4) погасить экран, 5) активировать силовой пин,
  // 6) если не обесточило — deepSleep с пробуждением по WAKE_PIN.
  // [FIX SmartStart] Аппаратное отключение → тоже сбрасываем wasPlaying.
  g_sleepTimerShutdown = true; // Блокируем авто-ретрай на время аппаратного выключения/DeepSleep.

  // [FIX Задача 2] Останавливаем плеер перед аппаратным выключением
  player.sendCommand({PR_STOP, 0}); // Останавливаем плеер перед отключением питания.
  delay(200); // Даём время обработать команду остановки.
  config.saveValue(&config.store.wasPlaying, true); // Сохраняем флаг для восстановления после пробуждения.

  if (MUTE_PIN != 255) {
    pinMode(MUTE_PIN, OUTPUT);
    digitalWrite(MUTE_PIN, MUTE_VAL);
  }

  config.setDspOn(false);
  delay(150);

  pinMode(SLEEP_TIMER_PIN, OUTPUT);
  digitalWrite(SLEEP_TIMER_PIN, _powerActiveLevel());

  // [FIX Задача 2] Если реле не обесточило плату (не подключено физически),
  // через 5 секунд уводим ESP32 в deepSleep с пробуждением по WAKE_PIN.
  // Это гарантирует отключение даже без физического реле.
  delay(5000);
  // Если мы всё ещё работаем — значит реле не сработало. Уходим в deepSleep.
  Serial.println("[SleepTimer] Реле не обесточило плату, уходим в deepSleep");
  Serial.flush();
  // Настройка источника пробуждения из deepSleep.
  //
  // Приоритеты:
  //  1) Если используется ёмкостной тачскрин GT911 и задан пин TS_INT —
  //     пробуждаем по прерыванию тача (касание экрана).
  //     Уровень срабатывания задаётся через TS_WAKE_LEVEL (LOW/HIGH) в myoptions.h.
  //  2) В противном случае, если задан WAKE_PIN — пробуждаем по этому пину
  //     (обычно кнопка питания/«Power»).
  //  3) Если ни тач, ни WAKE_PIN не заданы — используем таймер-пробуждение (4 часа)
  //     как запасной вариант, чтобы плата не «зависала» навсегда.
  #if (TS_MODEL == TS_MODEL_GT911) && (TS_INT != 255)
    {
      // Преобразуем TS_WAKE_LEVEL (LOW/HIGH) в логический уровень для ext0 (0/1).
      const uint8_t tsWakeLevel = (TS_WAKE_LEVEL == LOW) ? 0 : 1;
      Serial.printf("[SleepTimer] DeepSleep wake by TOUCH (GT911), level=%s, pin=%d\n",
                    (tsWakeLevel == 0) ? "LOW" : "HIGH", TS_INT);
      esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(TS_INT), tsWakeLevel);
    }
  #elif WAKE_PIN != 255
    {
      // Классический сценарий: пробуждение по кнопке WAKE_PIN (активный LOW).
      // При необходимости можно в будущем добавить отдельный уровень для WAKE_PIN,
      // аналогично TS_WAKE_LEVEL.
      Serial.printf("[SleepTimer] DeepSleep wake by WAKE_PIN (LOW), pin=%d\n", WAKE_PIN);
      esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(WAKE_PIN), 0 /* LOW */);
    }
  #else
    // [FIX] Без WAKE_PIN и тача ESP32 мгновенно перезагрузится из deepSleep.
    // Используем таймер-пробуждение как запасной вариант (4 часа).
    Serial.println("[SleepTimer] Нет тача и WAKE_PIN, используем таймер-пробуждение на 8 часов");
    esp_sleep_enable_timer_wakeup(8ULL * 3600ULL * 1000000ULL);
  #endif
  esp_deep_sleep_start();
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
