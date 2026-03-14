#ifndef common_gfx_h
#define common_gfx_h
#include "../widgets/widgetsconfig.h" // displayXXXDDDDconf.h
#include "utf8Rus.h"

typedef struct clipArea {
  uint16_t left; 
  uint16_t top; 
  uint16_t width;  
  uint16_t height;
} clipArea;

class psFrameBuffer;
#if DSP_MODEL==DSP_NV3041A
class ScrollWidget;
#endif

class DspCore: public yoDisplay {
  public:
#if DSP_MODEL==DSP_NV3041A
    uint16_t plItemHeight, plTtemsCount, plCurrentPos;
    int plYStart;
#endif
    DspCore();
    void initDisplay();
    void clearDsp(bool black=false);
    void printClock(){}
    #ifdef DSP_OLED
    #if DSP_MODEL!=DSP_NV3041A
    inline void loop(bool force=false){
      #if DSP_MODEL==DSP_NOKIA5110
        if(digitalRead(TFT_CS)==LOW) return;
        display();
      #else
        display();
        //delay(DSP_MODEL==DSP_ST7920?20:5);
        vTaskDelay(DSP_MODEL==DSP_ST7920?10:0);
      #endif
    }
    inline void drawLogo(uint16_t top) {
      #if DSP_MODEL!=DSP_SSD1306x32
        drawBitmap((width()  - LOGO_WIDTH ) / 2, top, logo, LOGO_WIDTH, LOGO_HEIGHT, 1);
      #else
        setTextSize(1); setCursor((width() - 6*CHARWIDTH) / 2, 0); setTextColor(TFT_FG, TFT_BG); print(utf8Rus("ёRadio", false));
      #endif
      display();
    }
    #endif
    #else
      #ifndef DSP_LCD
      #if DSP_MODEL!=DSP_NV3041A && DSP_MODEL!=DSP_DUMMY
      inline void loop(bool force=false){}
      inline void drawLogo(uint16_t top){ drawRGBBitmap((width() - LOGO_WIDTH) / 2, top, logo, LOGO_WIDTH, LOGO_HEIGHT); }
      #elif DSP_MODEL==DSP_DUMMY
      inline void loop(bool force=false){}
      inline void drawLogo(uint16_t top){}
      #endif
      #endif
    #endif
    #ifdef DSP_LCD
      uint16_t width();
      uint16_t height();
      void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
      void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color){}
      void setTextSize(uint8_t s){}
      void setTextSize(uint8_t sx, uint8_t sy){}
      void setTextColor(uint16_t c, uint16_t bg){}
      void setFont(){}
      void apScreen();
      void drawLogo(uint16_t top){}
      void loop(bool force=false){}
    #endif
    void flip();
    void invert();
    void sleep();
    void wake();
#if DSP_MODEL==DSP_NV3041A
    void drawLogo(uint16_t top);
    void printClock(uint16_t top, uint16_t rightspace, uint16_t timeheight, bool redraw);
    void clearClock();
  char* utf8Rus(const char* str, bool uppercase);
    void drawPlaylist(uint16_t currentItem);
    void printPLitem(uint8_t pos, const char* item);
    void loop(bool force=false);
    void charSize(uint8_t textsize, uint8_t& width, uint16_t& height);
    void setTextSize(uint8_t s);
  void setNumFont();
    #ifndef DSP_LCD
      void startWrite(void);
      void endWrite(void);
    #endif
#endif
    void setScrollId(void * scrollid) { _scrollid = scrollid; }
    void * getScrollId() { return _scrollid; }
    uint16_t textWidth(const char *txt);
    #if !defined(DSP_LCD)
      #if DSP_MODEL==DSP_NV3041A
      void writePixel(int16_t x, int16_t y, uint16_t color);
      void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
      #else
      inline void writePixel(int16_t x, int16_t y, uint16_t color) {
        if(_clipping){
          if ((x < _cliparea.left) || (x > _cliparea.left+_cliparea.width) || (y < _cliparea.top) || (y > _cliparea.top + _cliparea.height)) return;
        }
        yoDisplay::writePixel(x, y, color);
      }
      inline void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        if(_clipping){
          if ((x < _cliparea.left) || (x >= _cliparea.left+_cliparea.width) || (y < _cliparea.top) || (y > _cliparea.top + _cliparea.height))  return;
        }
        yoDisplay::writeFillRect(x, y, w, h, color);
      }
      #endif
    #else
      inline void writePixel(int16_t x, int16_t y, uint16_t color) { }
      inline void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) { }
    #endif
    inline void setClipping(clipArea ca){
      _cliparea = ca;
      _clipping = true;
    }
    inline void clearClipping(){
      _clipping = false;
      #ifdef DSP_LCD
      setClipping({0, 0, width(), height()});
      #endif
    }
  private:
#if DSP_MODEL==DSP_NV3041A
    bool _isAwake = false;
    char  _timeBuf[20], _dateBuf[20], _oldTimeBuf[20], _oldDateBuf[20], _bufforseconds[4], _buffordate[40];
    uint16_t _timewidth, _timeleft, _datewidth, _dateleft, _oldtimeleft, _oldtimewidth, _olddateleft, _olddatewidth, clockTop, clockRightSpace, clockTimeHeight, _dotsLeft;
    bool _printdots;
    void _getTimeBounds();
    void _clockSeconds();
    void _clockDate();
    void _clockTime();
    uint8_t _charWidth(unsigned char c);
#endif
    bool _clipping;
    clipArea _cliparea;
    void * _scrollid;
    #ifdef PSFBUFFER
    psFrameBuffer* _fb=nullptr;
    #endif
};

extern DspCore dsp;
#endif
