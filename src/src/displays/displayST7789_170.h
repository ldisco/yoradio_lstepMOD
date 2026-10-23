#ifndef displayST7789_170_h
#define displayST7789_170_h

#include "../core/options.h"
#include "Arduino.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#if CLOCKFONT_MONO
  #include "fonts/DS_DIGI56pt7b_mono.h"
#else
  #include "fonts/DS_DIGI56pt7b.h"
#endif
#include "tools/l10n.h"

#define CHARWIDTH   6
#define CHARHEIGHT  8

typedef GFXcanvas16 Canvas;
typedef Adafruit_ST7789 yoDisplay;
#include "widgets/widgets.h"
#include "widgets/pages.h"

#if __has_include("conf/displayST7789conf_custom.h")
  #include "conf/displayST7789conf_custom.h"
#else
  #include "conf/displayST7789_170conf.h"
#endif

#define BOOT_PRG_COLOR    0xE68B
#define BOOT_TXT_COLOR    0xFFFF
#define PINK              0xF97F

// Обложка в playing-скринсейвере (ST7789_170): Display Cover + while playing, не «blank screen».
namespace st7789_170_cover {
  /* Активен ли режим: играет + SCREENBLANK + включён Display Cover. */
  bool isPlayingBlankCoverActive();
  /* Тик отрисовки (обложка по центру или fallback: логотип слева + малые часы справа). */
  void tickPlayingBlankScreen(bool forceRedraw);
  /* Энкодер: после выхода из cover-скринсейвера один раз блокируем переход в плейлист. */
  void resetEncoderNavGuard();
  void armEncoderNavGuard();
  bool consumeEncoderNavGuard();
  /* Новые данные погоды с OWM — перерисовать иконку при следующем тике cover-скринсейвера. */
  void notifyWeatherIconUpdated();
}

void invalidateCoverCache();

typedef struct clipArea {
  uint16_t left;
  uint16_t top;
  uint16_t width;
  uint16_t height;
} clipArea;

class DspCore: public Adafruit_ST7789 {
  public:
    DspCore();
    void initDisplay();
    void drawLogo(uint16_t top);
    void clearDsp(bool black=false);
    void printClock(uint16_t top, uint16_t rightspace, uint16_t timeheight, bool redraw);
    void clearClock();
    void loop(bool force=false);
    void charSize(uint8_t textsize, uint8_t& width, uint16_t& height);
    void setTextSize(uint8_t s);
    void flip();
    void invert();
    void sleep();
    void wake();
    void startWrite(void);
    void endWrite(void);
    void writePixel(int16_t x, int16_t y, uint16_t color);
    void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void setClipping(clipArea ca);
    void clearClipping();
    void setScrollId(void * scrollid) { _scrollid = scrollid; }
    void * getScrollId() { return _scrollid; }
    void setNumFont();
    uint16_t textWidth(const char *txt);
  private:
    char  _timeBuf[20], _dateBuf[20], _oldTimeBuf[20], _oldDateBuf[20], _bufforseconds[4], _buffordate[40];
    uint16_t _timewidth, _timeleft, _datewidth, _dateleft, _oldtimeleft, _oldtimewidth, _olddateleft, _olddatewidth;
    uint16_t clockTop, clockTimeHeight, _dotsLeft;
    int16_t clockRightSpace;
    bool _clipping;
    clipArea _cliparea;
    void * _scrollid;
    void _getTimeBounds();
    void _clockSeconds();
    void _clockDate();
    void _clockTime(bool redraw);
    uint8_t _charWidth(unsigned char c);
};

extern DspCore dsp;

#endif
