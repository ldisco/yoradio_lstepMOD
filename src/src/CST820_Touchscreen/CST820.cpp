#include "../core/options.h"
#if TS_MODEL == TS_MODEL_CST820

#include <Arduino.h>
#include "CST820.h"

CST820::CST820(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin, int8_t int_pin) {
  _sda = sda_pin;
  _scl = scl_pin;
  _rst = rst_pin;
  /* 255 в myoptions = «пин не используем» (как у остальных TS_*) */
  _int = (int_pin == 255) ? (int8_t)-1 : int_pin;
}

void CST820::begin(void) {
  /* Явные SDA/SCL обязательны: на S3 DevKit дефолт Wire 8/9 = BL/MOSI TFT у JC2432 */
  if (_sda != -1 && _scl != -1) {
    Wire.begin(_sda, _scl);
  } else {
    Wire.begin();
  }

  /* Последовательность INT из демо Guition (только если пин свободен) */
  if (_int != -1) {
    pinMode(_int, OUTPUT);
    digitalWrite(_int, HIGH);
    delay(1);
    digitalWrite(_int, LOW);
    delay(1);
  }

  if (_rst != -1 && _rst != 255) {
    pinMode(_rst, OUTPUT);
    digitalWrite(_rst, LOW);
    delay(10);
    digitalWrite(_rst, HIGH);
    delay(300);
  }

  /* Пока ESP бодрствует — запрет авто-Standby (демо Guition / poll I2C).
   * 0xFE=0xFF → DisAutoSleep: иначе чип уходит в мониторинг через ~2 с и UI «тупит».
   * Перед deep sleep это снимем в prepareDeepSleepWake(): Standby как раз будит хост по IRQ. */
  uint8_t ackFe = i2c_write(0xFE, 0xFF);
  /* 0xFA IrqCtl: бит 6 EnTouch — LOW-импульс INT при касании (карта CST816S/CST820). */
  uint8_t ackFa = i2c_write(0xFA, 0x40);
  /* Шире импульс IRQ, чтобы digitalRead/мультиметр/ext0 успевали его увидеть. */
  i2c_write(0xED, 100);
  delay(5);
  uint8_t chipId = i2c_read(0xA7);   /* 0xB7 = CST820, семейство CST816 ≈ 0xB4…0xB6 */
  uint8_t irqCtl = i2c_read(0xFA);
  Serial.printf("##[BOOT]#\tCST820 ChipID=0x%02X IrqCtl=0x%02X (want 0x40) ACK FE=%u FA=%u\n",
                chipId, irqCtl, ackFe, ackFa);
}

void CST820::prepareDeepSleepWake(void) {
  /* Не пишем Sleep (CST816: 0xA5/0xE5 = PowerMode Sleep) — там сканирование OFF, IRQ молчит.
   * Нужен Standby/Monitoring: чип сам уходит туда по AutoSleepTime (0xF9), INT будит хост.
   * Раньше здесь был 0xFE=0xFF (запрет автосна) — это как раз НЕ давало войти в Standby. */
  uint8_t chipId = i2c_read(0xA7);
  uint8_t ackFa = i2c_write(0xFA, 0x40);  /* EnTouch=1, EnTest=0 — без периодических ложных импульсов */
  uint8_t ackEd = i2c_write(0xED, 100);   /* ширина IRQ ~10 мс (ед. 0.1 мс); дефолт 1 мс мультиметр не видит */
  uint8_t ackF9 = i2c_write(0xF9, 2);     /* через 2 с без касания → Standby */
  uint8_t ackFe = i2c_write(0xFE, 0x00);  /* разрешить авто-Standby (0 = auto sleep ON) */
  delay(5);
  uint8_t irqCtl = i2c_read(0xFA);
  uint8_t autoSlp = i2c_read(0xF9);
  uint8_t disAuto = i2c_read(0xFE);
  Serial.printf("[CST820] wake-prep ChipID=0x%02X IrqCtl=0x%02X (wr ACK %u, want 0x40) "
                "IrqWidth ACK %u AutoSleep=0x%02X (ACK %u) DisAutoSleep=0x%02X (ACK %u, want 0x00)\n",
                chipId, irqCtl, ackFa, ackEd, autoSlp, ackF9, disAuto, ackFe);
  Serial.flush();
  /* Дать чипу уйти в Standby ДО того, как ESP погасит CPU (дефолт AutoSleepTime = 2 с). */
  delay(2500);
  /* RST должен остаться HIGH. В deep sleep обычный GPIO всплывает — тогда чип в сбросе и INT мёртв. */
  if (_rst != -1 && _rst != 255) {
    pinMode(_rst, OUTPUT);
    digitalWrite(_rst, HIGH);
  }
}

void CST820::resumeFromStandby(void) {
  /* Возврат в рабочий режим после prepareDeepSleepWake() без аппаратного сброса:
   * 0xFE=0xFF снова запрещает авто-Standby (иначе чип «засыпает» каждые 2 с и poll тупит),
   * 0xFA=0x40 оставляем — impulse INT нам не мешает, а линия остаётся живой. */
  uint8_t ackFe = i2c_write(0xFE, 0xFF);
  uint8_t ackFa = i2c_write(0xFA, 0x40);
  delay(5);
  Serial.printf("[CST820] resume: DisAutoSleep=0x%02X IrqCtl=0x%02X (ACK FE=%u FA=%u)\n",
                i2c_read(0xFE), i2c_read(0xFA), ackFe, ackFa);
}

void CST820::prepareLightSleepWake(void) {
  /* Light sleep будит хост опросом I2C, а не импульсом INT.
   * Поэтому чип должен оставаться в активном режиме: DisAutoSleep=0xFF.
   * RST оставляем HIGH — иначе контроллер в постоянном сбросе и I2C молчит. */
  if (_rst != -1 && _rst != 255) {
    pinMode(_rst, OUTPUT);
    digitalWrite(_rst, HIGH);
  }
  uint8_t ackFe = i2c_write(0xFE, 0xFF);
  delay(5);
  Serial.printf("[CST820] light-sleep-prep ChipID=0x%02X DisAutoSleep=0x%02X (ACK FE=%u, want 0xFF)\n",
                i2c_read(0xA7), i2c_read(0xFE), ackFe);
  Serial.flush();
}

bool CST820::fingerPresent(void) {
  /* Регистр 0x02 FingerNum: 0 = нет касания, 1+ = палец(ы). */
  return i2c_read(0x02) != 0;
}

bool CST820::getTouch(uint16_t *x, uint16_t *y, uint8_t *gesture) {
  bool finger = (bool)i2c_read(0x02);

  *gesture = i2c_read(0x01);
  if (!(*gesture == SlideUp || *gesture == SlideDown)) {
    *gesture = None;
  }

  uint8_t data[4];
  i2c_read_continuous(0x03, data, 4);
  *x = ((data[0] & 0x0f) << 8) | data[1];
  *y = ((data[2] & 0x0f) << 8) | data[3];

  return finger;
}

uint8_t CST820::i2c_read(uint8_t addr) {
  uint8_t rdData = 0;
  /* Без таймаута демо зависает, если чип не отвечает (не тот пин / нет питания) */
  for (uint8_t attempt = 0; attempt < 8; ++attempt) {
    Wire.beginTransmission(I2C_ADDR_CST820);
    Wire.write(addr);
    if (Wire.endTransmission(false) != 0) {
      delay(1);
      continue;
    }
    const uint8_t n = Wire.requestFrom((uint8_t)I2C_ADDR_CST820, (uint8_t)1);
    if (n == 0) {
      delay(1);
      continue;
    }
    if (Wire.available()) {
      rdData = Wire.read();
    }
    return rdData;
  }
  return 0;
}

uint8_t CST820::i2c_read_continuous(uint8_t addr, uint8_t *data, uint32_t length) {
  Wire.beginTransmission(I2C_ADDR_CST820);
  Wire.write(addr);
  if (Wire.endTransmission(true)) {
    for (uint32_t i = 0; i < length; ++i) {
      data[i] = 0;
    }
    return 1;
  }
  Wire.requestFrom((uint8_t)I2C_ADDR_CST820, (uint8_t)length);
  for (uint32_t i = 0; i < length; ++i) {
    data[i] = Wire.available() ? (uint8_t)Wire.read() : 0;
  }
  return 0;
}

uint8_t CST820::i2c_write(uint8_t addr, uint8_t data) {
  Wire.beginTransmission(I2C_ADDR_CST820);
  Wire.write(addr);
  Wire.write(data);
  return Wire.endTransmission();
}

#endif /* TS_MODEL_CST820 */
