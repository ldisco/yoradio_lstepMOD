#ifndef AXS15231B_TOUCH_H
#define AXS15231B_TOUCH_H

#include "Arduino.h"
#include <Wire.h>

/* Точка касания (как у GT911 TP_Point) — одна точка для MVP Faster. */
class TP_Point {
public:
  TP_Point(void) : id(0), x(0), y(0), size(0) {}
  TP_Point(uint8_t id_, uint16_t x_, uint16_t y_, uint16_t size_)
    : id(id_), x(x_), y(y_), size((uint8_t)size_) {}
  bool operator==(TP_Point p) { return (x == p.x && y == p.y && id == p.id); }
  bool operator!=(TP_Point p) { return !(*this == p); }
  uint8_t id;
  uint16_t x;
  uint16_t y;
  uint8_t size;
};

/*
 * Ёмкостный тач AXS15231B (Guition JC3248W535): I2C 0x3B.
 * Сырые координаты — native панели 320×480 (портрет).
 * Маппинг в UI landscape 480×320 — в touchscreen.cpp (как CST820).
 */
class AXS15231B_Touch {
public:
  AXS15231B_Touch(uint8_t _sda, uint8_t _scl, uint8_t _int, int8_t _rst,
                  uint16_t _width, uint16_t _height);
  void begin();
  void setRotation(uint8_t rot);
  void setResolution(uint16_t _width, uint16_t _height);
  void read();
  uint8_t touches = 0;
  bool isTouched = false;
  TP_Point points[2];

private:
  uint8_t pinSda;
  uint8_t pinScl;
  uint8_t pinInt;
  int8_t pinRst;
  uint16_t width;
  uint16_t height;
  uint8_t rotation = 0;
};

#endif
