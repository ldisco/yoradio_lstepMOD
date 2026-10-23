#include "options.h"

#if USE_BT_I2S

#include "bt_i2s.h"
#include "player.h"
#include "driver/gpio.h"
#include "esp32-hal-periman.h"

BtI2s btI2s;

/*
 * ESP32-S3 DevKit: GPIO47/48 часто заняты RMT RGB LED — без сброса periman
 * digitalWrite(48) даёт «IO 48 is not set as GPIO» (см. displayST7789.cpp).
 */
static void btForceGpioOut(uint8_t pin, uint8_t idleLevel) {
  if (pin == 255) return;
  gpio_hold_dis((gpio_num_t)pin);
  gpio_reset_pin((gpio_num_t)pin);
  perimanClearPinBus(pin);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, idleLevel);
}

// Короткий импульс на KEY-вход QCC5125 (активный уровень HIGH, 3.3 V).
void BtI2s::_pulseKey(uint8_t pin) {
  if (pin == 255) return;
  // Перед каждым импульсом — на всякий случай вернуть пин в GPIO OUTPUT (WebUI шлёт vol пачками).
  btForceGpioOut(pin, LOW);
  digitalWrite(pin, HIGH);
  delay(50);
  digitalWrite(pin, LOW);
}

void BtI2s::begin() {
#if BT_I2S_SEL_PIN != 255
  btForceGpioOut(BT_I2S_SEL_PIN, BT_I2S_SEL_LEVEL_ESP);
  // По умолчанию — тракт ESP (WEB/SD), чтобы старт не оставил ЦАП на «пустом» BT.
#endif
#if BT_KEY_PLAY_PIN != 255
  btForceGpioOut(BT_KEY_PLAY_PIN, LOW);
#endif
#if BT_KEY_VOLP_PIN != 255
  btForceGpioOut(BT_KEY_VOLP_PIN, LOW);
#endif
#if BT_KEY_VOLM_PIN != 255
  btForceGpioOut(BT_KEY_VOLM_PIN, LOW);
#endif
#if BT_MUTE_STATUS_PIN != 255
  pinMode(BT_MUTE_STATUS_PIN, INPUT_PULLUP);
#endif
  _btActive = false;
}

void BtI2s::selectSource(bool btActive) {
#if BT_I2S_SEL_PIN != 255
  digitalWrite(BT_I2S_SEL_PIN, btActive ? BT_I2S_SEL_LEVEL_BT : BT_I2S_SEL_LEVEL_ESP);
#endif
  _btActive = btActive;
}

void BtI2s::applyEspI2sPinout() {
#if I2S_DOUT != 255 && !I2S_INTERNAL
  #if defined(I2S_MCLK) && (I2S_MCLK != 255) && BT_USE_MCLK
    player.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  #else
    player.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  #endif
#endif
}

void BtI2s::enterBtMode() {
  // Переключаем 74HC157 на тракт QCC5125, затем импульс PLAY — старт BT-аудио при входе в режим.
  selectSource(true);
  _btActive = true;
  delay(50);
  // Импульс PLAY только если считаем BT на паузе — иначе двойной toggle остановит звук.
  if (!_btPlaying) {
    _pulseKey(BT_KEY_PLAY_PIN);
    _btPlaying = true;
    delay(80);
  }
}

void BtI2s::exitBtMode() {
  // Пауза на QCC5125 только когда логически «играет» — без лишнего toggle.
  if (_btPlaying) {
    _pulseKey(BT_KEY_PLAY_PIN);
    _btPlaying = false;
    delay(80);
  }
  selectSource(false);
  applyEspI2sPinout();
  _btActive = false;
}

void BtI2s::volumeUp()   { _pulseKey(BT_KEY_VOLP_PIN); }
void BtI2s::volumeDown() { _pulseKey(BT_KEY_VOLM_PIN); }
void BtI2s::playPause() {
  _pulseKey(BT_KEY_PLAY_PIN);
  _btPlaying = !_btPlaying;
}
void BtI2s::pairingMode() {
  // Для QCC5125 часто PLAY удержание/двойной тап — на плате может быть отдельный KEY; пока дублируем PLAY.
  _pulseKey(BT_KEY_PLAY_PIN);
}

#endif // USE_BT_I2S
