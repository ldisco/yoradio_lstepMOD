/**
 * Драйвер тача AXS15231B (I2C) — порт протокола из JC3248W535C / Witaliy.
 * MVP Faster: одно касание; без жестов двумя пальцами.
 */

#include "AXS15231B_Touch.h"

#define AXS15231B_I2C_ADDR 0x3B
/* В протоколе пакета заложено до 2 точек; читаем обе, в UI используем points[0]. */
#define AXS_MAX_TOUCH_NUMBER 2

AXS15231B_Touch::AXS15231B_Touch(uint8_t _sda, uint8_t _scl, uint8_t _int, int8_t _rst,
                                 uint16_t _width, uint16_t _height)
  : pinSda(_sda), pinScl(_scl), pinInt(_int), pinRst(_rst),
    width(_width), height(_height), rotation(0) {}

void AXS15231B_Touch::begin() {
  Wire.begin(pinSda, pinScl);
  /* INT/RST: на JC3248 RST=-1; INT=3 — для wake позже, poll идёт по I2C. */
  if (pinRst >= 0) {
    pinMode((uint8_t)pinRst, OUTPUT);
    digitalWrite((uint8_t)pinRst, HIGH);
  }
  if (pinInt != 255) {
    pinMode(pinInt, INPUT);
  }
}

void AXS15231B_Touch::setRotation(uint8_t rot) {
  /* Поворот координат делаем в touchscreen.cpp под Canvas soft-rot UI. */
  rotation = rot;
}

void AXS15231B_Touch::setResolution(uint16_t _width, uint16_t _height) {
  width = _width;
  height = _height;
}

void AXS15231B_Touch::read() {
  uint8_t data[AXS_MAX_TOUCH_NUMBER * 6 + 2] = {0};

  /* Команда чтения отчёта касаний (как в эталоне Guition/Witaliy). */
  const uint8_t read_cmd[11] = {
    0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00,
    (uint8_t)((AXS_MAX_TOUCH_NUMBER * 6 + 2) >> 8),
    (uint8_t)((AXS_MAX_TOUCH_NUMBER * 6 + 2) & 0xff),
    0x00, 0x00, 0x00
  };

  Wire.beginTransmission(AXS15231B_I2C_ADDR);
  Wire.write(read_cmd, 11);
  if (Wire.endTransmission() != 0) {
    isTouched = false;
    touches = 0;
    return;
  }

  if (Wire.requestFrom((uint8_t)AXS15231B_I2C_ADDR, (uint8_t)sizeof(data)) != sizeof(data)) {
    isTouched = false;
    touches = 0;
    return;
  }

  for (int i = 0; i < (int)sizeof(data); i++) {
    data[i] = Wire.read();
  }

  touches = data[1];
  if (touches > 0 && touches <= AXS_MAX_TOUCH_NUMBER) {
    uint16_t rawX = ((data[2] & 0x0F) << 8) | data[3];
    uint16_t rawY = ((data[4] & 0x0F) << 8) | data[5];

    /* Мусорные коды «нет касания» / выбросы. */
    if (rawX == 273 && rawY == 273) {
      isTouched = false;
      touches = 0;
      return;
    }
    if (rawX > 4000 || rawY > 4000) {
      isTouched = false;
      touches = 0;
      return;
    }

    /* Клип в native панели 320×480 (не в UI landscape). */
    if (rawX > 320) rawX = 320;
    if (rawY > 480) rawY = 480;

    isTouched = true;
    points[0] = TP_Point(0, rawX, rawY, 0);

    if (touches > 1) {
      rawX = ((data[8] & 0x0F) << 8) | data[9];
      rawY = ((data[10] & 0x0F) << 8) | data[11];
      if (rawX != 273 && rawY != 273 && rawX <= 4000 && rawY <= 4000) {
        if (rawX > 320) rawX = 320;
        if (rawY > 480) rawY = 480;
        points[1] = TP_Point(1, rawX, rawY, 0);
      } else {
        touches = 1;
      }
    }
  } else {
    isTouched = false;
    touches = 0;
  }
}
