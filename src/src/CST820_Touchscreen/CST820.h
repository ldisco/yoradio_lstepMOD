#ifndef _CST820_H
#define _CST820_H

#include <Wire.h>

/* I2C-адрес контроллера ёмкостного тача CST820 (Guition JC2432W328) */
#define I2C_ADDR_CST820 0x15

/* Жесты из даташита / демо Guition (используем касание по FingerIndex, жесты — опционально) */
enum GESTURE {
  None = 0x00,
  SlideDown = 0x01,
  SlideUp = 0x02,
  SlideLeft = 0x03,
  SlideRight = 0x04,
  SingleTap = 0x05,
  DoubleTap = 0x0B,
  LongPress = 0x0C
};

/* Драйвер CST820: порт фабричного демо Guition под yoRadio (I2C poll). */
class CST820 {
public:
  /* int_pin = -1 или 255 — не трогать INT (нужно, если пин занят SD MISO и т.п.) */
  CST820(int8_t sda_pin = -1, int8_t scl_pin = -1, int8_t rst_pin = -1, int8_t int_pin = -1);

  void begin(void);
  /* true — палец на экране; x/y в координатах панели (обычно портрет 240×320) */
  bool getTouch(uint16_t *x, uint16_t *y, uint8_t *gesture);
  /* Перед ESP deep sleep: Standby+EnTouch (не Sleep 0xA5), RST=HIGH, лог read-back 0xFA. */
  void prepareDeepSleepWake(void);
  /* Обратная операция к prepareDeepSleepWake(): вернуть чип в рабочий режим без RST-сброса.
   * Нужна для теста линии INT — после проверки надо снять авто-Standby, иначе UI «тупит». */
  void resumeFromStandby(void);
  /* Перед light sleep: чип обязан отвечать по I2C (запрет авто-Standby), INT не используется. */
  void prepareLightSleepWake(void);
  /* Быстрый опрос «палец на экране?» (регистр FingerNum 0x02) — для пробуждения из light sleep. */
  bool fingerPresent(void);

private:
  int8_t _sda, _scl, _rst, _int;

  uint8_t i2c_read(uint8_t addr);
  uint8_t i2c_read_continuous(uint8_t addr, uint8_t *data, uint32_t length);
  /* Возвращает код Wire.endTransmission: 0 = ACK. */
  uint8_t i2c_write(uint8_t addr, uint8_t data);
};

#endif
