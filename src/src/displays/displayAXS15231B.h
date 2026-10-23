#ifndef displayAXS15231B_h
#define displayAXS15231B_h

/*
 * Драйвер AXS15231B (Guition JC3248W535) — профиль Faster.
 * QSPI + Arduino_Canvas: буфер native 320×480, UI landscape 480×320 (Canvas rot 1/3).
 * QSPI + Arduino_Canvas; present после pager (axsPresentFrame), как Witaliy gfxFlushScreen.
 * Подсветка: LEDC 5 kHz на GFX_BL; BRIGHTNESS_PIN=255 (не analogWrite).
 * Яркость WebUI/фейдер — через axsSetBacklightDuty (YO_BACKLIGHT_CTRL).
 * A/B portrait rot=0 — не убрал дрыганье (артефакты UI); откат на landscape.
 */

#include "../core/options.h"
#include "Arduino.h"
#include <Adafruit_GFX.h>
#include "Arduino_GFX_Library.h"
#include "fonts/bootlogo198x128.h" /* Faster boot ×2 Catmull-Rom; не 99×64 runtime-scale */
#if CLOCKFONT_MONO
  #include "fonts/DS_DIGI56pt7b_mono.h" /* PLAYER; ≈ DS_DIGI42×(320/240) к JC2432 */
  #include "fonts/DS_DIGI68pt7b_mono.h" /* SS — крупнее 56, с запасом для прыжков */
#else
  #include "fonts/DS_DIGI56pt7b.h"
  #include "fonts/DS_DIGI68pt7b.h"
#endif
#include "tools/l10n.h"

#define CHARWIDTH   6
#define CHARHEIGHT  8

#ifndef DSP_HSPI
#define DSP_HSPI         false
#endif
#ifndef TFT_CS
#define TFT_CS           45
#endif
#ifndef TFT_RST
#define TFT_RST          -1
#endif
#ifndef TFT_SCK
#define TFT_SCK          47
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
#ifndef GFX_BL
#define GFX_BL           1
#endif
#ifndef BRIGHTNESS_PIN
#define BRIGHTNESS_PIN   GFX_BL
#endif

/* yoDisplay = Arduino_Canvas (кадр → flush). Canvas = GFXcanvas16 — как у NV3041A, для VuWidget и т.п. */
typedef GFXcanvas16 Canvas;
typedef Arduino_Canvas yoDisplay;
/* Arduino_GFX: логотип/битмапы через draw16bitRGBBitmap */
#define drawRGBBitmap draw16bitRGBBitmap
#define drawBitmap draw16bitRGBBitmap

/* Барьер draw↔flush (VuWidget) + dirty-кадр. Реализация в displayAXS15231B.cpp. */
bool axsFbTryLockMs(uint32_t waitMs);
void axsFbUnlock();
void axsMarkFbDirty();
/* После _pager->loop(): вывести Canvas на панель (как gfxFlushScreen у Witaliy). */
void axsPresentFrame();
/* Яркость подсветки 0…255 (LEDC на GFX_BL). Вызов из config/BacklightDown. */
void axsSetBacklightDuty(uint8_t duty255);

#include "tools/commongfx.h"
#include "widgets/widgets.h"
#include "widgets/pages.h"

#if __has_include("conf/displayAXS15231Bconf_custom.h")
  #include "conf/displayAXS15231Bconf_custom.h"
#else
  #include "conf/displayAXS15231Bconf.h"
#endif

/* Playing+blank Cover-SS (общий st7789_cover_screensaver.inc). */
namespace axs_cover {
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
/* PLAYER+PLAY: пирамидка погоды (общий код с ST7789 / ILI9488). */
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

extern DspCore dsp;

#endif
