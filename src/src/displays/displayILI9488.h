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

#if !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT)
/* Playing+blank cover-скринсейвер landscape 480×320 (общий .inc с ST7789 CYD). */
namespace ili9488_cover {
  bool isPlayingBlankCoverActive();
  void tickPlayingBlankScreen(bool forceRedraw);
  void tickFactMarquee(bool forceRedraw = false);
  void tickWeatherCityMarquee();
  void preparePlayingBlankEntry();
  void resetEncoderNavGuard();
  void armEncoderNavGuard();
  bool consumeEncoderNavGuard();
  void notifyWeatherIconUpdated();
  bool isTouchInCoverSlot(uint16_t x, uint16_t y);
  void showFactStatusToast(const char *msg, uint32_t durationMs = 4000, bool waitForFact = false);
  bool isCoverFactSwipeArmed();
  int16_t coverFactSwipeLastPeakAbs();
  void coverFactSwipeBegin(uint16_t x, uint16_t y);
  void coverFactSwipeSample(uint16_t x, uint16_t y, uint8_t hwGesture = 0);
  bool coverFactSwipeEnd(uint16_t endX, uint16_t endY);
  void requestCoverInvalidate();
}
void invalidateCoverCache();
#if SPECTRUM_ANALYZER_ACTIVE
/* PLAYER+PLAY: пирамидка погоды (общий код с ST7789 / JC2432). */
namespace st7789_player_weather {
  void setSlot(int16_t x, int16_t y, int16_t w);
  void activate();
  void deactivate();
  bool isActive();
  void notifyUpdated();
  void redraw(bool fullRedraw);
  void tickCityMarquee();
}
#endif
#endif
#endif

#endif
