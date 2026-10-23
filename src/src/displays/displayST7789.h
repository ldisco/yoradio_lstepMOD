#ifndef displayST7789_h
#define displayST7789_h

#include "Arduino.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#if DSP_MODEL==DSP_ST7789_76
  #include "fonts/bootlogo62x40.h"
  #include "fonts/dsfont35.h"
#else
  #include "fonts/bootlogo99x64.h"
  #include "fonts/dsfont52.h"
#endif

typedef GFXcanvas16 Canvas;
typedef Adafruit_ST7789 yoDisplay;

#include "tools/commongfx.h"

#if __has_include("conf/displayST7789conf_custom.h")
  #include "conf/displayST7789conf_custom.h"
#else
  #if DSP_MODEL==DSP_ST7789
    #include "conf/displayST7789conf.h"
  #elif DSP_MODEL==DSP_ST7789_76
    #include "conf/displayST7789_76conf.h"
  #else
    #include "conf/displayST7789_240conf.h"
  #endif
#endif

#if DSP_MODEL==DSP_ST7789
/* Обложка в playing-скринсейвере (ST7789 320×240): Display Cover + while playing. */
namespace st7789_cover {
  bool isPlayingBlankCoverActive();
  void tickPlayingBlankScreen(bool forceRedraw);
  void tickFactMarquee(bool forceRedraw = false);
  void tickWeatherCityMarquee();
  void preparePlayingBlankEntry();
  void resetEncoderNavGuard();
  void armEncoderNavGuard();
  bool consumeEncoderNavGuard();
  void notifyWeatherIconUpdated();
  /* Точка в слоте обложки (координаты после маппинга тача). */
  bool isTouchInCoverSlot(uint16_t x, uint16_t y);
  /* Статусная бегущая строка внизу cover-SS.
   * waitForFact=true — держать до появления нового факта (или safety-таймаута). */
  void showFactStatusToast(const char *msg, uint32_t durationMs = 4000, bool waitForFact = false);
  /* Свайп вверх по обложке: begin на START, sample на движении, end на отпускании.
   * end() возвращает true, если жест обработан (не будить PLAYER). */
  bool isCoverFactSwipeArmed();
  int16_t coverFactSwipeLastPeakAbs();
  void coverFactSwipeBegin(uint16_t x, uint16_t y);
  /* hwGesture: для CST820 — GESTURE (SlideUp/Down); иначе 0. */
  void coverFactSwipeSample(uint16_t x, uint16_t y, uint8_t hwGesture = 0);
  bool coverFactSwipeEnd(uint16_t endX, uint16_t endY);
}
#if ST7789_SPECTRUM_ENABLE
/* PLAYER+PLAY: «пирамидка» погоды вместо бегущей строки (только при ST7789_SPECTRUM_ENABLE). */
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
void invalidateCoverCache();
#endif

#endif
