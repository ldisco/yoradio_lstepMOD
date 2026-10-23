#ifndef bt_i2s_h
#define bt_i2s_h
#pragma once

#include "Arduino.h"

#if USE_BT_I2S

// Управление QCC5125 + селектор 74HC157 (I2S ESP/BT → ES9038).
// Все пины и уровни задаются в myoptions.h; при USE_BT_I2S=false модуль не компилируется.

class BtI2s {
public:
  // Инициализация GPIO (SEL, KEY); вызывается один раз из Config::_initHW().
  void begin();
  // Маршрут I2S: true = линия BT-модуля, false = выход ESP.
  void selectSource(bool btActive);
  void enterBtMode();
  void exitBtMode();
  // Восстановить pinout I2S ESP (в т.ч. MCLK при BT_USE_MCLK) после выхода из BT.
  void applyEspI2sPinout();
  void volumeUp();
  void volumeDown();
  void playPause();
  void pairingMode();
  bool isBtModeActive() const { return _btActive; }
  // Логическое «играет» на QCC5125 (AVRCP-тегов нет — только toggle по KEY PLAY).
  bool isBtPlaying() const { return _btPlaying; }

private:
  bool _btActive = false;
  bool _btPlaying = false;
  void _pulseKey(uint8_t pin);
};

extern BtI2s btI2s;

#endif // USE_BT_I2S

#endif
