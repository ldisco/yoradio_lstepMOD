/*
плагин. для включения режима 16-bit data words, для es9038q2m

Выводы для подключения задаются в myoptions.h:
  #define EQ_SDA_PIN ...
  #define EQ_SCL_PIN ...
*/
#include "es9038q2m_init.h"
#include <Arduino.h>

// Подключаем глобальные настройки проекта (пины и флаги)
#include "../../../myoptions.h"

#if USE_ES9038_DAC

// Адрес ЦАПА //
#define ES90381 0x48

// Регистры для инициализации //
#define Mad_AutoSelect 0x01

typedef uint8_t   u8;
typedef int8_t    s8;
typedef uint16_t  u16;
typedef int16_t   s16;
typedef uint32_t  u32;
typedef int32_t   s32;

static uint8_t _i2c_scl;
static uint8_t _i2c_sda;

#define IIC_SCL(n)                                                            \
  (n ? digitalWrite (_i2c_scl, 1) : digitalWrite (_i2c_scl, 0))
#define IIC_SDA(n)                                                            \
  (n ? digitalWrite (_i2c_sda, 1) : digitalWrite (_i2c_sda, 0))
#define READ_SDA digitalRead (_i2c_sda)

#define delay_us(n) ets_delay_us (n)

static void
SWire_init (void)
{
  _i2c_scl = EQ_SCL_PIN;
  _i2c_sda = EQ_SDA_PIN;
  pinMode(_i2c_scl, OUTPUT);
  pinMode(_i2c_sda, OUTPUT);

  IIC_SDA (1);
  IIC_SCL (1);
}

static void
SDA_IN (void)
{
  pinMode(_i2c_sda, INPUT);
}

static void
SDA_OUT (void)
{
  pinMode(_i2c_sda, OUTPUT);
}

static void
IIC_Start (void)
{
  SDA_OUT ();
  IIC_SDA (1);
  IIC_SCL (1);
  delay_us (4);
  IIC_SDA (0);
  delay_us (4);
  IIC_SCL (0);
}

static void
IIC_Stop (void)
{
  SDA_OUT ();
  IIC_SCL (0);
  IIC_SDA (0);
  delay_us (4);
  IIC_SCL (1);
  delay_us (4);
  IIC_SDA (1);
}

static unsigned char
IIC_Wait_Ack (void)
{
  unsigned char ucErrTime = 0;
  SDA_IN ();
  IIC_SDA (1);
  delay_us (1);
  IIC_SCL (1);
  delay_us (1);
  while (READ_SDA)
    {
      ucErrTime++;
      if (ucErrTime > 250)
        {
          IIC_Stop ();
          return 1;
        }
    }
  IIC_SCL (0);
  return 0;
}

static void
IIC_Send_Byte (unsigned char txd)
{
  unsigned char t;
  SDA_OUT ();
  IIC_SCL (0);
  for (t = 0; t < 8; t++)
    {
      IIC_SDA ((txd & 0x80) >> 7);
      txd <<= 1;
      delay_us (2);
      IIC_SCL (1);
      delay_us (2);
      IIC_SCL (0);
      delay_us (2);
    }
}

static int SWire_write_bytes(uint16_t addr, const void *data, size_t len)
{
  IIC_Start ();

  IIC_Send_Byte ((addr << 1) | 0);
  if (IIC_Wait_Ack ())
    {
      IIC_Stop ();
      return 1;
    }

  for (u16 u8_index = 0; u8_index < len; u8_index++)
    {
      u8 dat = ((u8*)data)[u8_index];
      IIC_Send_Byte (dat);
      if (IIC_Wait_Ack ())
        {
          IIC_Stop ();
          return 1;
        }
    }
  IIC_Stop ();
  return 0;
}
//************************************************************************8

static void es9038_write_reg(u8 reg, u8 val) {
    u8 data[] = {reg, val};
    SWire_write_bytes(ES90381, data, 2);
    delay(2); 
}

static void es9038_on_setup(void) {
    delay(1000); // Ждем инициализации основной платы
    SWire_init();
    delay(10);

    // 1. Установка 16-bit режима
    // Регистр 0x01: [4:2] = 001 (I2S), [1:0] = 00 (16-bit)
    es9038_write_reg(0x01, 0x04); 

    // 2. Настройка фильтра для "лампового" звука
    // Регистр 0x07: Bits [7:5] - выбор фильтра.
    // Значение 0x60 (01100000) = Slow roll-off, Minimum phase filter
    // Этот фильтр минимизирует пред-эхо, делая звук более натуральным и "жирным" внизу.
    es9038_write_reg(0x07, 0x60);

    // 3. Настройка DPLL (Чистота высоких частот)
    // Регистр 0x0C (12): Настройка полосы пропускания для PCM.
    // По умолчанию там 0x55. Установка в 0x22 (меньшее значение) 
    // делает звук более собранным и детальным на ВЧ за счет лучшей фильтрации джиттера.
    es9038_write_reg(0x0C, 0x22);

    // 4. Включение THD компенсации (Опционально, добавляет "чистоты" в тракт)
    // Регистр 0x0D (13): Bit [6] = 1 (Enable THD compensation)
    // По умолчанию 0x40.
    es9038_write_reg(0x0D, 0x40);
    // 5. Добавление теплоты (Вторая гармоника H2)
    // Мы записываем число 250 (это 0x00FA в шестнадцатеричной системе)
    es9038_write_reg(22, 0xFA); // Младший байт (LSB)
    es9038_write_reg(23, 0x00); // Старший байт (MSB)

    delay(5);
    // Удержание шины
IIC_SCL(0); /*не отпускать шину! 
  подробности - 
  строка 390 - https://github.com/nvv13/test/blob/main/test-mk/avr/attiny85/init_es9038q2m/decoder--250516-164223.txt 
  */ /*Не дает контроллеру STC сбросить настройки.*/
IIC_SDA(0);
}

void yoradio_on_setup() {
es9038_on_setup();
}

es9038q2m::es9038q2m() {
  registerPlugin();
  log_i("Plugin es9038q2m is registered");
}

// Создаём глобальный экземпляр плагина, чтобы он автоматически регистрировался
static es9038q2m es9038q2m_instance;

#else

// Если плагин отключён, определяем пустой stub, чтобы вызовы безопасно проигнорировались
void yoradio_on_setup() {}

#endif // USE_ES9038_DAC