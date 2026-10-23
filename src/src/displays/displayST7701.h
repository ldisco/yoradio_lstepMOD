#ifndef displayST7701_h
#define displayST7701_h

/*
 * Драйвер ST7701S RGB 480×480 — профиль Supremus (ESP32-4848S040).
 * Arduino_GFX: SWSPI init + ESP32 RGB panel, последовательность st7701_type9 (Guition).
 * Подсветка: LEDC 250 Hz (fallback 1 kHz) на GFX_BL; BRIGHTNESS_PIN=255.
 * (150 Hz/8bit на S3 Arduino — attach FAIL → чёрный экран.)
 * Boot-лого: 198×128 + заметный pulse по центру экрана (~50→125→100%).
 */

#include "../core/options.h"
#include "Arduino.h"
#include <Adafruit_GFX.h>
#include "Arduino_GFX_Library.h"
#include "fonts/bootlogo198x128.h" /* как Faster: 198×128 + pulse; не 99×64 */
#if CLOCKFONT_MONO
  #include "fonts/DS_DIGI56pt7b_mono.h" /* PLAYER */
  #include "fonts/DS_DIGI68pt7b_mono.h" /* SS — крупнее, прыжки по экрану */
#else
  #include "fonts/DS_DIGI56pt7b.h"
  #include "fonts/DS_DIGI68pt7b.h"
#endif
#include "tools/l10n.h"

#define CHARWIDTH   6
#define CHARHEIGHT  8

#ifndef ST7701_CS
#define ST7701_CS    39
#endif
#ifndef ST7701_SCK
#define ST7701_SCK   48
#endif
#ifndef ST7701_SDA
#define ST7701_SDA   47
#endif
#ifndef ST7701_DE
#define ST7701_DE    18
#endif
#ifndef ST7701_VSYNC
#define ST7701_VSYNC 17
#endif
#ifndef ST7701_HSYNC
#define ST7701_HSYNC 16
#endif
#ifndef ST7701_PCLK
#define ST7701_PCLK  21
#endif
#ifndef ST7701_R0
#define ST7701_R0 11
#endif
#ifndef ST7701_R1
#define ST7701_R1 12
#endif
#ifndef ST7701_R2
#define ST7701_R2 13
#endif
#ifndef ST7701_R3
#define ST7701_R3 14
#endif
#ifndef ST7701_R4
#define ST7701_R4 0
#endif
#ifndef ST7701_G0
#define ST7701_G0 8
#endif
#ifndef ST7701_G1
#define ST7701_G1 20
#endif
#ifndef ST7701_G2
#define ST7701_G2 3
#endif
#ifndef ST7701_G3
#define ST7701_G3 46
#endif
#ifndef ST7701_G4
#define ST7701_G4 9
#endif
#ifndef ST7701_G5
#define ST7701_G5 10
#endif
#ifndef ST7701_B0
#define ST7701_B0 4
#endif
#ifndef ST7701_B1
#define ST7701_B1 5
#endif
#ifndef ST7701_B2
#define ST7701_B2 6
#endif
#ifndef ST7701_B3
#define ST7701_B3 7
#endif
#ifndef ST7701_B4
#define ST7701_B4 15
#endif
#ifndef GFX_BL
#define GFX_BL 38
#endif
#ifndef BRIGHTNESS_PIN
#define BRIGHTNESS_PIN GFX_BL
#endif
#ifndef ST7701_BOOT_ORIENTATION_180
#define ST7701_BOOT_ORIENTATION_180 false
#endif

typedef GFXcanvas16 Canvas;
typedef Arduino_RGB_Display yoDisplay;
/* Arduino_GFX: битмапы через draw16bitRGBBitmap */
#define drawRGBBitmap draw16bitRGBBitmap
#define drawBitmap draw16bitRGBBitmap

/* Яркость 0…255 (LEDC на GFX_BL). WebUI / BacklightDown / setBrightness. */
void st7701SetBacklightDuty(uint8_t duty255);

#if ST7701_RGB_SOAK
/* Opus 5.5: статичный паттерн + BL по шагу; держать экран без PLAYER/AP UI. */
void st7701RgbSoakEnter();
void st7701RgbSoakRefresh();
char st7701RgbSoakStep();
#endif

#include "tools/commongfx.h"
#include "widgets/widgets.h"
#include "widgets/pages.h"

#if __has_include("conf/displayST7701conf_custom.h")
  #include "conf/displayST7701conf_custom.h"
#else
  #include "conf/displayST7701conf.h"
#endif

extern DspCore dsp;

#endif
