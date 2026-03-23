#ifndef displayILI9488_h
#define displayILI9488_h

#include "Arduino.h"
#include <Adafruit_GFX.h>
#include "../ILI9488/ILI9486_SPI.h"
#include "fonts/bootlogo99x64.h"
#include "fonts/dsfont70.h"

typedef GFXcanvas16 Canvas;
typedef ILI9486_SPI yoDisplay;

#include "tools/commongfx.h"

#if __has_include("conf/displayILI9488conf_custom.h")
  #include "conf/displayILI9488conf_custom.h"
#else
  #if defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT
    #include "conf/displayILI9488conf_portrait.h"
  #else
    #include "conf/displayILI9488conf.h"
  #endif
#endif

#define ILI9488_SLPIN     0x10
#define ILI9488_SLPOUT    0x11
#define ILI9488_DISPOFF   0x28
#define ILI9488_DISPON    0x29

#if DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486
// Вызывается из widgets.cpp (ClockWidget) чтобы обновлять cover art на странице PLAYER.
void ili9488_updateCoverSlot(bool forceRedraw=false);
#endif

#endif
