#ifndef displayNV3041A_h
#define displayNV3041A_h

// Включаем опции для доступа к DSP_MODEL
#include "../core/options.h"

#include "Arduino.h"
#include <Adafruit_GFX.h>
#include "Arduino_GFX_Library.h"
#include "fonts/bootlogo.h"
#include "fonts/bootlogo99x64.h"

#if CLOCKFONT_MONO
  //#include "fonts/DS_DIGI56pt7b_mono.h"        // https://tchapi.github.io/Adafruit-GFX-Font-Customiser/
  //#include "fonts/AnitaSemi52.h" 
  // Commented heavy fonts to save flash size. Enable if needed.
  // Essential fonts for NV3041A
  #include "fonts/DirectiveFour56.h"
  //#include "fonts/AnitaSemi_square30.h"
  //#include "fonts/DirectiveFour40.h"
  #include "fonts/DirectiveFour18.h"
  //#include "fonts/PetrovSans18.h"
  
#else
  // Commented heavy fonts to reduce binary size; keep only essential small fonts.
  // #include "fonts/Deamus54.h" 
  // #include "fonts/DS_DIGI56pt7b.h"
  // #include "fonts/DirectiveFour56.h" 
#endif
#include "tools/l10n.h"

#define CHARWIDTH   6
#define CHARHEIGHT  8

/* NV3041A Parallel Pins */
#ifndef DSP_HSPI
#define DSP_HSPI         false
#endif
#ifndef TFT_CS
#define TFT_CS           45                /*  SPI CS pin  */    
#endif
#ifndef TFT_RST
#define TFT_RST          -1                /*  SPI RST pin.  set to -1 and connect to Esp EN pin */
#endif
#ifndef TFT_SCK
#define TFT_SCK          47                /*  SPI DC/RS pin  */
#endif
#ifndef TFT_D0
#define TFT_D0           21
#endif
#ifndef TFT_D1
#define TFT_D1           48
#endif
#ifndef TFT_D2
#define TFT_D2           40
#endif
#ifndef TFT_D3
#define TFT_D3           39
#endif
#ifndef TFT_DC
#define TFT_DC           255
#endif
#ifndef GFX_BL
#define GFX_BL           1 
#endif
#ifndef BRIGHTNESS_PIN
#define BRIGHTNESS_PIN   GFX_BL
#endif

typedef GFXcanvas16 Canvas;
typedef Arduino_NV3041A yoDisplay;
#define drawRGBBitmap draw16bitRGBBitmap
#define drawBitmap draw16bitRGBBitmap

#define BOOT_PRG_COLOR    0xE68B
#define BOOT_TXT_COLOR    0xFFFF
#define PINK              0xF97F

#define NV3041A_SLPIN     0x10
#define NV3041A_SLPOUT    0x11
#define NV3041A_DISPOFF   0x28
#define NV3041A_DISPON    0x29

#include "tools/commongfx.h"

#include "widgets/widgets.h"
#include "widgets/pages.h"

// Конфигурация дисплея ПОСЛЕ commongfx.h (нужны типы ScrollConfig, WidgetConfig и т.д.)
#if __has_include("conf/displayNV3041Aconf_custom.h")
  #include "conf/displayNV3041Aconf_custom.h"
#else
  #include "conf/displayNV3041Aconf.h"
#endif

extern DspCore dsp;

// Сброс кэша обложки при изменении currentCoverUrl
extern void invalidateCoverCache();

#endif
