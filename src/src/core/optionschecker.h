#ifndef optionschecker_h
#define optionschecker_h

#if REAL_LEDBUILTIN==TFT_RST
#  error LED_BUILTIN IS THE SAME AS TFT_RST. Check it in myoptions.h
#endif

#if REAL_LEDBUILTIN==VS1053_RST
#  error LED_BUILTIN IS THE SAME AS VS1053_RST. Check it in myoptions.h
#endif

#if (I2S_DOUT!=255) && (VS1053_CS!=255)
#  error YOU MUST CHOOSE BETWEEN I2S DAC AND VS1053 BY DISABLING THE SECOND MODULE IN THE myoptions.h
#endif

#if !(defined(ARDUINO_ESP32_DEV) || defined(ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32C3_DEV))
#  error ONLY MODULES "ESP32 Dev Module", "ESP32 Wrover Module" AND "ESP32 S3 Dev Module" ARE SUPPORTED. PLEASE SELECT ONE OF THEM IN THE MENU >> TOOLS >> BOARD
#endif

#if USE_BT_I2S
#  if BT_I2S_SEL_PIN == 255
#    error USE_BT_I2S: задайте BT_I2S_SEL_PIN в myoptions.h
#  endif
#  if (BT_I2S_SEL_PIN == I2S_DOUT) || (BT_I2S_SEL_PIN == I2S_BCLK) || (BT_I2S_SEL_PIN == I2S_LRC)
#    error BT_I2S_SEL_PIN конфликтует с I2S_DOUT/BCLK/LRC
#  endif
#  if defined(I2S_MCLK) && (I2S_MCLK != 255)
#    if (BT_I2S_SEL_PIN == I2S_MCLK) || (I2S_DOUT == I2S_MCLK) || (I2S_BCLK == I2S_MCLK) || (I2S_LRC == I2S_MCLK)
#      error I2S_MCLK или BT_I2S_SEL_PIN пересекаются с линиями I2S
#    endif
#  endif
#endif

#endif


