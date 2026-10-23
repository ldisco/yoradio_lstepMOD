#ifndef BACKLIGHTDOWN_PLUGIN_H
#define BACKLIGHTDOWN_PLUGIN_H

/*******************************************************************************
 * Плагин BacklightDown для yoradio_lstepMOD
 * Автоматическое плавное снижение яркости дисплея после периода бездействия.
 *
 * Логика:
 *  - Если faderon == false (выкл. в WebUI) → яркость управляется только через
 *    стандартный ползунок brightness и настройки DOWN_LEVEL/DOWN_INTERVAL в myoptions.h.
 *  - Если faderon == true (вкл. в WebUI) → после fadertimeout секунд бездействия
 *    яркость плавно снижается до fadermin (%).
 *  - Яркость восстанавливается при старте/остановке воспроизведения, смене трека,
 *    нажатии кнопки или клике энкодера.
 *
 * Хранится в config.store: faderon, fadermin (0-100%), fadertimeout (сек.)
 *******************************************************************************/

#include <Arduino.h>
#include "../../core/options.h"
#include "../../pluginsManager/pluginsManager.h"

/* Плагин компилируется при управлении подсветкой: PWM-пин или AXS LEDC. */
#if YO_BACKLIGHT_CTRL

#include <Ticker.h>

class BacklightDown : public Plugin {
public:
  /* Конструктор: регистрирует плагин в менеджере событий */
  BacklightDown();

  /* ──────────── Переопределяем хуки Plugin ──────────── */

  /* Вызывается один раз при старте системы */
  void on_setup() override;

  /* Вызывается при старте воспроизведения — восстанавливаем яркость */
  void on_start_play() override;

  /* Вызывается при остановке воспроизведения — восстанавливаем яркость */
  void on_stop_play() override;

  /* Вызывается при смене трека — восстанавливаем яркость */
  void on_track_change() override;

  /* Вызывается раз в секунду: поддерживаем яркость вне режима PLAYER */
  void on_ticker() override;

  /* Вызывается при нажатии кнопки — восстанавливаем яркость */
  void on_btn_click(controlEvt_e &btnid) override;

  /* ──────────── Публичный API ──────────── */

  /*
   * Восстанавливает полную яркость и перезапускает таймер угасания.
   * Вызывается из on_start_play, on_stop_play, on_track_change, on_btn_click,
   * а также из netserver при изменении настроек.
   */
  void brightnessOn();

  /*
   * Пересчитывает состояние фейдера после изменения настроек из WebUI.
   * Вызывается из netserver.cpp при сохранении faderon/fadermin/fadertimeout.
   */
  void updateFaderState();

private:
  /* Таймер задержки: отсчитывает fadertimeout секунд перед началом угасания */
  Ticker _delayTicker;

  /* Таймер шага угасания: вызывает _fadeTick() каждые 30 мс */
  Ticker _fadeTicker;

  /* Текущий уровень яркости пина (0-255) */
  uint8_t _currentBl;

  /* Флаг: идёт ли сейчас процесс угасания */
  volatile bool _isFading;

  /* Момент последнего шага угасания (для ограничения частоты 30 мс) */
  uint32_t _lastFadeMs;

  /* ──────────── Статические колбэки для Ticker ──────────── */

  /* Указатель на единственный экземпляр (для статических колбэков) */
  static BacklightDown* _instance;

  /* Колбэк: вызывается Ticker'ом по истечении fadertimeout — запускает угасание */
  static void _onDelayExpired();

  /* Колбэк: вызывается Ticker'ом каждые 30 мс — выполняет один шаг угасания */
  static void _onFadeTick();

  /* ──────────── Приватные методы ──────────── */

  /* Устанавливает реальный уровень яркости на пине (0-255) */
  void _setPin(uint8_t level);

  /* Вычисляет целевой уровень яркости по config.store.fadermin (% → 0-255) */
  uint8_t _targetLevel() const;

  /* Вычисляет нормальный уровень яркости по config.store.brightness (% → 0-255) */
  uint8_t _normalLevel() const;

  /* Внутренний шаг плавного угасания */
  void _fadeTick();
};

/* Единственный глобальный экземпляр плагина */
extern BacklightDown backlightDownPlugin;

#endif /* YO_BACKLIGHT_CTRL */
#endif /* BACKLIGHTDOWN_PLUGIN_H */
