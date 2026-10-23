#ifndef display_h
#define display_h
#include "common.h"

#if DSP_MODEL==DSP_DUMMY
#define DUMMYDISPLAY
#endif

#ifndef DUMMYDISPLAY
class ScrollWidget;
class PlayListWidget;
class BitrateWidget;
class FillWidget;
class SliderWidget;
class Pager;
class Page;
class Widget;
class NumWidget;
class ClockWidget;
class TextWidget;
    
class Display {
  public:
    uint16_t currentPlItem;
    uint16_t numOfNextStation;
    displayMode_e _mode;
  public:
    Display() {};
    ~Display();
    displayMode_e mode() { return _mode; }
    void mode(displayMode_e m) { _mode=m; }
    void init();
    void loop();
    void _start();
    bool ready() { return _bootStep==2; }
    void resetQueue();
    void putRequest(displayRequestType_e type, int payload=0);
    /* Разрешить следующий NEWMODE→PLAYER из тач-диалога (таймер / явный выход). */
    void armStickyLeave();
    /* Снять разрешение: скролл/касание плейлиста после сработавшего таймера (очередь PLAYER сброшена). */
    void cancelStickyLeave();
    /* Тач в зоне выделенной строки плейлиста (для Play только по фокусу). */
    bool playlistFocusHit(uint16_t touchY);
    void flip();
    void invert();
    bool deepsleep();
    void wakeup();
    void setContrast();
    void lock()   { _locked=true; }
    void unlock() { _locked=false; }
    uint16_t width();
    uint16_t height();
  private:
    ScrollWidget *_meta, *_title1, *_plcurrent, *_weather, *_title2;
    PlayListWidget *_plwidget;
    BitrateWidget *_fullbitrate;
    FillWidget *_metabackground, *_plbackground;
    SliderWidget *_volbar, *_heapbar, *_cpubar = nullptr;
    Pager *_pager;
    Page *_footer;
    /*
     * Общий указатель: при SPECTRUM_ANALYZER_ACTIVE — SpectrumWidget
     * (ST7789 / ILI9488 landscape), иначе — прежний VuWidget.
     */
    Widget *_vuwidget;
    NumWidget *_nums;
    ClockWidget *_clock;
    Page *_boot;
    // Текстовые виджеты:
    // _bootstring  - строка "Соединяюсь с %s" при старте
    // _bootversion - строка вида "<DEVICE_NAME>: lstepMOD v0.8.xx" (имя из myoptions.h)
    TextWidget *_bootstring, *_bootversion, *_volip, *_voltxt, *_rssi, *_bitrate;
    bool _locked = false;
    volatile bool _sleeping = false;
    uint8_t _bootStep;
    void _time(bool redraw = false);
    void _apScreen();
    void _swichMode(displayMode_e newmode);
    void _drawPlaylist();
    void _volume();
    void _title();
    void _station();
    void _drawNextStationNum(uint16_t num);
    void _createDspTask();
    void _showDialog(const char *title);
    void _buildPager();
    void _bootScreen();
    void _layoutChange(bool played);
    void _setRSSI(int rssi);
};

#else

class Display {
  public:
    uint16_t currentPlItem;
    uint16_t numOfNextStation;
    displayMode_e _mode;
  public:
    Display() {};
    displayMode_e mode() { return _mode; }
    void mode(displayMode_e m) { _mode=m; }
    void init();
    void _start();
    void putRequest(displayRequestType_e type, int payload=0);
    void loop(){}
    bool ready() { return true; }
    void resetQueue(){}
    /* Заглушка: на TFT armStickyLeave разрешает NEWMODE→PLAYER из sticky-диалога;
     * у DSP_DUMMY экрана нет — no-op, чтобы controls/timekeeper линковались. */
    void armStickyLeave(){}
    void cancelStickyLeave(){}
    bool playlistFocusHit(uint16_t){ return true; }
    void centerText(const char* text, uint8_t y, uint16_t fg, uint16_t bg){}
    void rightText(const char* text, uint8_t y, uint16_t fg, uint16_t bg){}
    void flip(){}
    void invert(){}
    void setContrast(){}
    bool deepsleep(){return true;}
    void wakeup(){}
    void printPLitem(uint8_t pos, const char* item){}
    void lock()   {}
    void unlock() {}
    uint16_t width(){ return 0; }
    uint16_t height(){ return 0; }
  private:
    void _createDspTask();
};

#endif

extern Display display;


#endif
