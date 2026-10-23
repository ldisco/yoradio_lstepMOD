/*******************************************************************************
 * BacklightDown.cpp — реализация плагина автоматического угасания подсветки.
 * Для yoradio_lstepMOD.
 *******************************************************************************/

#include "BacklightDown.h"

/* Плагин компилируется при YO_BACKLIGHT_CTRL (PWM-пин или AXS LEDC). */
#if YO_BACKLIGHT_CTRL

#include "../../core/options.h"
#include "../../core/config.h"
#include "../../core/network.h"
#include "../../core/display.h"
#if DSP_MODEL==DSP_AXS15231B
/* Faster: яркость через LEDC, не analogWrite(BRIGHTNESS_PIN). */
extern void axsSetBacklightDuty(uint8_t duty255);
#elif DSP_MODEL==DSP_ST7701
/* Supremus: яркость через LEDC на GFX_BL. */
extern void st7701SetBacklightDuty(uint8_t duty255);
#endif

/* ─────────────────────────────────────────────
 * Инициализация статических членов
 * ─────────────────────────────────────────────*/

/* Указатель на единственный экземпляр (синглтон для колбэков Ticker) */
BacklightDown* BacklightDown::_instance = nullptr;

/* ─────────────────────────────────────────────
 * Глобальный экземпляр (создаётся до main(),
 * конструктор вызывает registerPlugin())
 * ─────────────────────────────────────────────*/
BacklightDown backlightDownPlugin;

/* ─────────────────────────────────────────────
 * Конструктор
 * ─────────────────────────────────────────────*/
BacklightDown::BacklightDown()
  : _currentBl(0),
    _isFading(false),
    _lastFadeMs(0)
{
  /* Сохраняем указатель на себя для статических колбэков */
  _instance = this;
  /* Регистрируемся в менеджере плагинов */
  registerPlugin();
}

/* ─────────────────────────────────────────────
 * Статические колбэки для Ticker
 * ─────────────────────────────────────────────*/

/* Вызывается по истечении fadertimeout секунд — запускает угасание */
void BacklightDown::_onDelayExpired() {
  if (!_instance) return;
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
  /* Soak: duty фиксирует st7701RgbSoakEnter — не гасить. */
  return;
#endif
  /* Не угасаем в режиме точки доступа (SoftAP) */
  if (network.status == SOFT_AP) return;
  /* Не угасаем если выключено в настройках */
  if (!config.store.faderon) return;
  /* Разрешаем угасание: тикер шагов запустится в _fadeTick при первом вызове */
  _instance->_isFading = true;
  /* Запускаем тикер плавного угасания (30 мс между шагами) */
  _instance->_fadeTicker.attach_ms(30, BacklightDown::_onFadeTick);
}

/* Вызывается каждые 30 мс во время угасания — выполняет один шаг */
void BacklightDown::_onFadeTick() {
  if (!_instance) return;
  _instance->_fadeTick();
}

/* ─────────────────────────────────────────────
 * Приватные вспомогательные методы
 * ─────────────────────────────────────────────*/

/* Устанавливает уровень яркости непосредственно на пине / LEDC */
void BacklightDown::_setPin(uint8_t level) {
  // Во время softSleep таймера сна не трогаем BL: иначе Ticker фейдера
  // делает ledcAttach и экран вспыхивает / мешает удержанию LOW.
  extern volatile bool g_sleepTimerShutdown;
  if (g_sleepTimerShutdown) return;
#if DSP_MODEL==DSP_AXS15231B
  /* Не повторять тот же duty — лишний ledcWrite на каждый тап давал моргание/фриз. */
  static uint8_t s_lastDuty = 0;
  static bool s_haveDuty = false;
  if (s_haveDuty && level == s_lastDuty) return;
  s_lastDuty = level;
  s_haveDuty = true;
  axsSetBacklightDuty(level);
#elif DSP_MODEL==DSP_ST7701
  static uint8_t s_lastDuty = 0;
  static bool s_haveDuty = false;
  if (s_haveDuty && level == s_lastDuty) return;
  s_lastDuty = level;
  s_haveDuty = true;
  st7701SetBacklightDuty(level);
#else
  analogWrite(BRIGHTNESS_PIN, level);
#endif
}

/* Вычисляет целевой (минимальный) уровень яркости: fadermin % → 0-255 */
uint8_t BacklightDown::_targetLevel() const {
  return (uint8_t)map(config.store.fadermin, 0, 100, 0, 255);
}

/* Вычисляет нормальный уровень яркости: brightness % → 0-255 */
uint8_t BacklightDown::_normalLevel() const {
  return (uint8_t)map(config.store.brightness, 0, 100, 0, 255);
}

/* Один шаг плавного угасания (30 мс между вызовами гарантирует Ticker) */
void BacklightDown::_fadeTick() {
  if (!_isFading) {
    /* Угасание отменили — останавливаем тикер шагов */
    _fadeTicker.detach();
    return;
  }

  uint8_t target = _targetLevel();

  /* Цель достигнута — останавливаем угасание */
  if (_currentBl == target) {
    _isFading = false;
    _fadeTicker.detach();
    return;
  }

  /* Плавный шаг: шаг = 2 (аналогично референсной реализации).
   * Направление угасания определяется знаком разности. */
  if (_currentBl > target) {
    /* Гасим: уменьшаем яркость */
    _currentBl = (_currentBl - target < 2) ? target : (_currentBl - 2);
  } else {
    /* Поднимаем (если target > current, например при изменении настроек «на лету») */
    _currentBl = (target - _currentBl < 2) ? target : (_currentBl + 2);
  }

  _setPin(_currentBl);
}

/* ─────────────────────────────────────────────
 * Публичные методы Plugin API
 * ─────────────────────────────────────────────*/

/*
 * on_setup — вызывается один раз при инициализации системы.
 * Устанавливаем начальную яркость и, если faderon включён, запускаем таймер.
 */
void BacklightDown::on_setup() {
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
  /* Opus 5.5 soak: без автоугасания — иначе рывок при смене PWM duty. */
  Serial.println("##[BOOT]#\t[SOAK] BacklightDown fade disabled");
  return;
#endif
  /* Начальный уровень яркости берём из сохранённых настроек */
  _currentBl = _normalLevel();
#if DSP_MODEL==DSP_AXS15231B
  /* Faster: при выкл. фейдере яркость уже выставил initDisplay/setBrightness —
   * повторный LEDC здесь не нужен. При faderon — синхронизируем и запускаем таймер. */
  if (config.store.faderon) {
    _setPin(_currentBl);
    _delayTicker.attach(config.store.fadertimeout, BacklightDown::_onDelayExpired);
  }
#else
  _setPin(_currentBl);
  if (config.store.faderon) {
    _delayTicker.attach(config.store.fadertimeout, BacklightDown::_onDelayExpired);
  }
#endif
}

/*
 * brightnessOn — восстанавливает полную яркость и перезапускает таймер угасания.
 * Вызывается при любом «активном» событии пользователя.
 */
void BacklightDown::brightnessOn() {
  /* Если автоугасание выключено — на обычных профилях пишем яркость;
   * на Faster/Supremus (LEDC) не трогаем на каждый тап (яркость — WebUI/setBrightness). */
  if (!config.store.faderon) {
    _currentBl = _normalLevel();
#if DSP_MODEL!=DSP_AXS15231B && DSP_MODEL!=DSP_ST7701
    _setPin(_currentBl);
#endif
    return;
  }

  /* Останавливаем таймер задержки (чтобы не срабатывал во время взаимодействия) */
  _delayTicker.detach();

  /* Прерываем текущее угасание */
  _isFading = false;
  _fadeTicker.detach();

  /* Восстанавливаем полную яркость */
  _currentBl = _normalLevel();
  _setPin(_currentBl);

  /* Перезапускаем таймер задержки перед следующим угасанием */
  _delayTicker.attach(config.store.fadertimeout, BacklightDown::_onDelayExpired);
}

/*
 * updateFaderState — вызывается из netserver.cpp после изменения настроек WebUI.
 * Пересинхронизирует состояние плагина с новыми config.store.* значениями.
 */
void BacklightDown::updateFaderState() {
  /* Полный сброс тикеров */
  _delayTicker.detach();
  _isFading = false;
  _fadeTicker.detach();

  if (config.store.faderon) {
    /* Если включили — восстанавливаем яркость и запускаем таймер */
    brightnessOn();
  } else {
    /* Если выключили — просто выставляем текущую яркость без таймера */
    _currentBl = _normalLevel();
    _setPin(_currentBl);
  }
}

/* ─── Хуки плагина: при активных событиях восстанавливаем яркость ─── */

void BacklightDown::on_start_play()  { brightnessOn(); }
void BacklightDown::on_stop_play()   { brightnessOn(); }

/* Смена метаданных (NEWTITLE) не должна будить подсветку в скринсейвере */
void BacklightDown::on_track_change() {
  if (config.isScreensaver) return;
  brightnessOn();
}

/* Нажатие кнопки — восстанавливаем яркость (btnid передаётся по ссылке, не меняем) */
void BacklightDown::on_btn_click(controlEvt_e &btnid) {
  (void)btnid; /* подавляем предупреждение unused parameter */
  brightnessOn();
}

/*
 * on_ticker — вызывается раз в секунду (timekeeper).
 * Логика как ctrls_on_loop() в референсе Maleksm:
 * - в SCREENSAVER/SCREENBLANK (config.isScreensaver) подсветку НЕ поднимаем;
 * - в интерактивных меню (VOL, STATIONS, …), но не в PLAYER — поднимаем,
 *   если яркость ещё не на целевом минимуме fadermin.
 * Ошибка v0.8.220: проверка mode()!=PLAYER без isScreensaver каждую секунду
 * возвращала полную яркость сразу после угасания в скринсейвере.
 */
void BacklightDown::on_ticker() {
  if (!config.store.faderon || _isFading) return;

  /* Скринсейвер: часы двигаются (_time/moveTo), но это не действие пользователя */
  if (config.isScreensaver) return;

  if (_currentBl != _targetLevel() && display.mode() != PLAYER) {
    static uint32_t prevRestoreMs = 0;
    if (millis() - prevRestoreMs > 1000) {
      prevRestoreMs = millis();
      brightnessOn();
    }
  }
}

#endif /* YO_BACKLIGHT_CTRL */
