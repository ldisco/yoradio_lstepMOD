#include "../../core/options.h"
#if DSP_MODEL!=DSP_DUMMY
#include "../dspcore.h"
#include "Arduino.h"
#include "widgets.h"
#include "../../core/player.h"    //  for VU widget
#include "../../core/network.h"   //  for Clock widget
#include "../../core/config.h"
#if SPECTRUM_ANALYZER_ACTIVE
#include "../../core/spectrum_analyzer.h"
#if ST7701_DIAG_FFT_INTERVAL
#include "esp_timer.h"
#endif
#endif
#include "../tools/l10n.h"
#include "../tools/psframebuffer.h"
#include "../tools/utf8Rus.h"  // Поддержка перекодировки UTF-8 -> Win1251 для не-NV3041A дисплеев

/************************
      FILL WIDGET
 ************************/
void FillWidget::init(FillConfig conf, uint16_t bgcolor){
  Widget::init(conf.widget, bgcolor, bgcolor);
  _width = conf.width;
  _height = conf.height;
  
}

void FillWidget::_draw(){
  if(!_active) return;
  dsp.fillRect(_config.left, _config.top, _width, _height, _bgcolor);
}

void FillWidget::setHeight(uint16_t newHeight){
  _height = newHeight;
  //_draw();
}
/************************
      TEXT WIDGET
 ************************/
TextWidget::~TextWidget() {
  free(_text);
  free(_oldtext);
}

void TextWidget::_charSize(uint8_t textsize, uint8_t& width, uint16_t& height){
#ifndef DSP_LCD
  width = textsize * CHARWIDTH;
  if (width == 0) width = CHARWIDTH; // защита от деления на ноль
  height = textsize * CHARHEIGHT;
  if (height == 0) height = CHARHEIGHT;
#else
  width = 1;
  height = 1;
#endif
}

void TextWidget::init(WidgetConfig wconf, uint16_t buffsize, bool uppercase, uint16_t fgcolor, uint16_t bgcolor) {
  Widget::init(wconf, fgcolor, bgcolor);
  _buffsize = buffsize;
  _text = (char *) malloc(sizeof(char) * _buffsize);
  memset(_text, 0, _buffsize);
  _oldtext = (char *) malloc(sizeof(char) * _buffsize);
  memset(_oldtext, 0, _buffsize);
  _charSize(_config.textsize, _charWidth, _textheight);
  _textwidth = _oldtextwidth = _oldleft = 0;
  _uppercase = uppercase;
}

void TextWidget::setText(const char* txt) {
  // Для NV3041A используем специализированный метод DspCore::utf8Rus(),
  // для остальных дисплеев — универсальную функцию utf8Rus() из tools/utf8Rus.cpp.
#if DSP_MODEL==DSP_NV3041A
  strlcpy(_text, dsp.utf8Rus(txt, _uppercase), _buffsize);
#else
  strlcpy(_text, utf8Rus(txt, _uppercase), _buffsize);
#endif
  _textwidth = strlen(_text) * _charWidth;
  if (strcmp(_oldtext, _text) == 0) return;
  if (_active) dsp.fillRect(_oldleft == 0 ? _realLeft() : min(_oldleft, _realLeft()),  _config.top, max(_oldtextwidth, _textwidth), _textheight, _bgcolor);
  _oldtextwidth = _textwidth;
  _oldleft = _realLeft();
  if (_active) _draw();
}

void TextWidget::setText(int val, const char *format){
  char buf[_buffsize];
  snprintf(buf, _buffsize, format, val);
  setText(buf);
}

void TextWidget::setText(const char* txt, const char *format){
  char buf[_buffsize];
  snprintf(buf, _buffsize, format, txt);
  setText(buf);
}

uint16_t TextWidget::_realLeft(bool w_fb) {
  uint16_t realwidth = (_width>0 && w_fb)?_width:dsp.width();
  switch (_config.align) {
    case WA_CENTER: return (realwidth - _textwidth) / 2; break;
    case WA_RIGHT: return (realwidth - _textwidth - (!w_fb?_config.left:0)); break;
    default: return !w_fb?_config.left:0; break;
  }
}

void TextWidget::_draw() {
  if(!_active) return;
  dsp.setTextColor(_fgcolor, _bgcolor);
  dsp.setCursor(_realLeft(), _config.top);
  dsp.setFont();
  dsp.setTextSize(_config.textsize);
  dsp.print(_text);
  strlcpy(_oldtext, _text, _buffsize);
}

/************************
      SCROLL WIDGET
 ************************/
ScrollWidget::ScrollWidget(const char* separator, ScrollConfig conf, uint16_t fgcolor, uint16_t bgcolor) {
  init(separator, conf, fgcolor, bgcolor);
}

ScrollWidget::~ScrollWidget() {
  //free(_fb);
  delete _fb;
  _fb = nullptr;
  free(_sep);
  free(_window);
}

void ScrollWidget::init(const char* separator, ScrollConfig conf, uint16_t fgcolor, uint16_t bgcolor) {
  TextWidget::init(conf.widget, conf.buffsize, conf.uppercase, fgcolor, bgcolor);
  _sep = (char *) malloc(sizeof(char) * 4);
  memset(_sep, 0, 4);
  snprintf(_sep, 4, " %.*s ", 1, separator);
  _x = conf.widget.left;
  _startscrolldelay = conf.startscrolldelay;
  _scrolldelta = conf.scrolldelta;
  _scrolltime = conf.scrolltime;
  _charSize(_config.textsize, _charWidth, _textheight);
  if (_charWidth == 0) _charWidth = CHARWIDTH; // доп. защита
  _sepwidth = strlen(_sep) * _charWidth;
  _width = conf.width;
  _backMove.width = _width;
  _window = (char *) malloc(sizeof(char) * (MAX_WIDTH / _charWidth + 1));
  memset(_window, 0, (MAX_WIDTH / _charWidth + 1));  // +1?
  _doscroll = false;
  #ifdef PSFBUFFER
  _fb = new psFrameBuffer(dsp.width(), dsp.height());
  uint16_t _rl = (_config.align==WA_CENTER)?(dsp.width()-_width)/2:_config.left;
  _fb->begin(&dsp, _rl, _config.top, _width, _textheight, _bgcolor);
  #endif
}

void ScrollWidget::_setTextParams() {
  if (_config.textsize == 0) return;
  // Проверяем _fb на nullptr перед ready():
  // на конфигурациях без PSFBUFFER буфер может не создаваться.
  if(_fb && _fb->ready()){
  #ifdef PSFBUFFER
    _fb->setTextSize(_config.textsize);
    _fb->setTextColor(_fgcolor, _bgcolor);
  #endif
  }else{
    // Сброс GFX-шрифта перед бегущей строкой: после printClock() активен DirectiveFour56,
    // без setFont() meta/title1 рисуются размером часов (как в TextWidget::_draw).
    dsp.setFont();
    dsp.setTextSize(_config.textsize);
    dsp.setTextColor(_fgcolor, _bgcolor);
  }
}

bool ScrollWidget::_checkIsScrollNeeded() {
  return _textwidth > _width;
}

void ScrollWidget::setText(const char* txt) {
  // Жёсткая защита входа:
  // 1) txt может прийти как nullptr при гонке/сбое источника меты;
  // 2) внутренние буферы должны быть валидны до любых операций форматирования.
  if (!_text || !_oldtext || _buffsize < 2) return;
  if (!txt) txt = "";

  // Сначала копируем вход в локальный буфер фиксированного размера.
  // Это защищает от изменения исходного указателя во время конвертации utf8Rus()
  // и не даёт читать "живой" внешний буфер напрямую.
  // SCROLL_SETTEXT_UTF8_MAX > BUFLEN: кириллица в UTF-8 ≈2 байта/символ — иначе
  // длинный факт обрезается ещё до utf8Rus (на TFT рвалось на «сингл с деб…»).
  char localTxt[SCROLL_SETTEXT_UTF8_MAX];
  memset(localTxt, 0, sizeof(localTxt));
  strlcpy(localTxt, txt, sizeof(localTxt));

  // Аналогично TextWidget::setText: NV3041A имеет собственную реализацию utf8Rus в DspCore,
  // все остальные модели используют общую функцию utf8Rus().
  const char* converted = "";
#if DSP_MODEL==DSP_NV3041A
  converted = dsp.utf8Rus(localTxt, _uppercase);
#else
  converted = utf8Rus(localTxt, _uppercase);
#endif
  if (!converted) converted = "";
  strlcpy(_text, converted, _buffsize);
  if (strcmp(_oldtext, _text) == 0) return;
  _textwidth = strlen(_text) * _charWidth;
  // Единый флаг готовности кадрового буфера с проверкой nullptr.
  const bool fbReady = (_fb && _fb->ready());
  _x = fbReady ? 0 : _config.left;
  _doscroll = _checkIsScrollNeeded();
  if (dsp.getScrollId() == this) dsp.setScrollId(NULL);
  _scrolldelay = millis();
  if (_active) {
    _setTextParams();
    if (_doscroll) {
      if(fbReady){
      #ifdef PSFBUFFER
        _fb->fillRect(0, 0, _width, _textheight, _bgcolor);
        _fb->setCursor(0, 0);
        snprintf(_window, _width / _charWidth + 1, "%s", _text); //TODO
        _fb->print(_window);
        _fb->display();
      #endif
      } else {
#if DSP_MODEL==DSP_ST7789_170 && defined(ST170_WEATHER_OVER_IP)
        if(_config.top >= ST170_WEATHER_IP_ROW) {
          uint16_t cw = _textwidth + _charWidth * 2;
          if(cw > _width) cw = _width;
          if(cw < _charWidth) cw = _charWidth;
          dsp.fillRect(_realLeft(), _config.top, cw, _textheight, _bgcolor);
        } else {
          dsp.fillRect(_config.left,  _config.top, _width, _textheight, _bgcolor);
        }
#else
        dsp.fillRect(_config.left,  _config.top, _width, _textheight, _bgcolor);
#endif
        dsp.setCursor(_config.left, _config.top);
        snprintf(_window, _width / _charWidth + 1, "%s", _text); //TODO
        dsp.setClipping({_config.left, _config.top, _width, _textheight});
        dsp.print(_window);
        dsp.clearClipping();
      }
    } else {
      if(fbReady){
      #ifdef PSFBUFFER
        _fb->fillRect(0, 0, _width, _textheight, _bgcolor);
        _fb->setCursor(_realLeft(true), 0);
        _fb->print(_text);
        _fb->display();
      #endif
      } else {
#if DSP_MODEL==DSP_ST7789_170 && defined(ST170_WEATHER_OVER_IP)
        if(_config.top >= ST170_WEATHER_IP_ROW) {
          uint16_t cw = _textwidth + _charWidth * 2;
          if(cw > _width) cw = _width;
          if(cw < _charWidth) cw = _charWidth;
          dsp.fillRect(_realLeft(), _config.top, cw, _textheight, _bgcolor);
        } else {
          dsp.fillRect(_config.left, _config.top, _width, _textheight, _bgcolor);
        }
#else
        dsp.fillRect(_config.left, _config.top, _width, _textheight, _bgcolor);
#endif
        dsp.setCursor(_realLeft(), _config.top);
        //dsp.setClipping({_config.left, _config.top, _width, _textheight});
        dsp.print(_text);
        //dsp.clearClipping();
      }
    }
    strlcpy(_oldtext, _text, _buffsize);
  }
}

void ScrollWidget::setText(const char* txt, const char *format){
  char buf[_buffsize];
  snprintf(buf, _buffsize, format, txt);
  setText(buf);
}

void ScrollWidget::loop() {
  if(_locked) return;
  if (!_doscroll || _config.textsize == 0 || (dsp.getScrollId() != NULL && dsp.getScrollId() != this)) return;
  const bool fbReady = (_fb && _fb->ready());
  uint16_t fbl = fbReady ? 0 : _config.left;
  if (_checkDelay(_x == fbl ? _startscrolldelay : _scrolltime, _scrolldelay)) {
    _calcX();
    if (_active) _draw();
  }
}

void ScrollWidget::_clear(){
  if(_fb && _fb->ready()){
    #ifdef PSFBUFFER
    _fb->fillRect(0, 0, _width, _textheight, _bgcolor);
    _fb->display();
    #endif
  } else {
#if DSP_MODEL==DSP_ST7789_170 && defined(ST170_WEATHER_OVER_IP)
    if(_config.top < ST170_WEATHER_IP_ROW)
#endif
      dsp.fillRect(_config.left, _config.top, _width, _textheight, _bgcolor);
  }
}

void ScrollWidget::_draw() {
  if(!_active || _locked) return;
  _setTextParams();
  const bool fbReady = (_fb && _fb->ready());
  if (_doscroll) {
    uint16_t fbl = fbReady ? 0 : _config.left;
    uint16_t _newx = fbl - _x;
    const char* _cursor = _text + _newx / _charWidth;
    uint16_t hiddenChars = _cursor - _text;
    uint8_t addChars = fbReady ? 2 : 1;
    if (hiddenChars < strlen(_text)) {
    //TODO
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wformat-truncation="
      snprintf(_window, _width / _charWidth + addChars, "%s%s%s", _cursor, _sep, _text);
    #pragma GCC diagnostic pop
    } else {
      const char* _scursor = _sep + (_cursor - (_text + strlen(_text)));
      snprintf(_window, _width / _charWidth + addChars, "%s%s", _scursor, _text);
    }
    if(fbReady){
    #ifdef PSFBUFFER
      _fb->fillRect(0, 0, _width, _textheight, _bgcolor);
      _fb->setCursor(_x + hiddenChars * _charWidth, 0);
      _fb->print(_window);
      _fb->display();
    #endif
    } else {
      dsp.setCursor(_x + hiddenChars * _charWidth, _config.top);
      dsp.setClipping({_config.left, _config.top, _width, _textheight});
      dsp.print(_window);
      #ifndef DSP_LCD
        dsp.print(" ");
      #endif
      dsp.clearClipping();
    }
  } else {
    if(fbReady){
    #ifdef PSFBUFFER
      _fb->fillRect(0, 0, _width, _textheight, _bgcolor);
      _fb->setCursor(_realLeft(true), 0);
      _fb->print(_text);
      _fb->display();
    #endif
    } else {
#if DSP_MODEL==DSP_ST7789_170 && defined(ST170_WEATHER_OVER_IP)
      if(_config.top >= ST170_WEATHER_IP_ROW) {
        uint16_t cw = _textwidth + _charWidth * 2;
        if(cw > _width) cw = _width;
        if(cw < _charWidth) cw = _charWidth;
        dsp.fillRect(_realLeft(), _config.top, cw, _textheight, _bgcolor);
      } else {
        dsp.fillRect(_config.left, _config.top, _width, _textheight, _bgcolor);
      }
#else
      dsp.fillRect(_config.left, _config.top, _width, _textheight, _bgcolor);
#endif
      dsp.setCursor(_realLeft(), _config.top);
      dsp.setClipping({_realLeft(), _config.top, _width, _textheight});
      dsp.print(_text);
      dsp.clearClipping();
    }
  }
}

void ScrollWidget::_calcX() {
  if (!_doscroll || _config.textsize == 0) return;
  _x -= _scrolldelta;
  const bool fbReady = (_fb && _fb->ready());
  uint16_t fbl = fbReady ? 0 : _config.left;
  if (-_x > _textwidth + _sepwidth - fbl) {
    _x = fbl;
    dsp.setScrollId(NULL);
  } else {
    dsp.setScrollId(this);
  }
}

bool ScrollWidget::_checkDelay(int m, uint32_t &tstamp) {
  if (millis() - tstamp > m) {
    tstamp = millis();
    return true;
  } else {
    return false;
  }
}

void ScrollWidget::_reset(){
  dsp.setScrollId(NULL);
  const bool fbReady = (_fb && _fb->ready());
  _x = fbReady ? 0 : _config.left;
  _scrolldelay = millis();
  _doscroll = _checkIsScrollNeeded();
  #ifdef PSFBUFFER
  if (_fb) {
    _fb->freeBuffer();
    uint16_t _rl = (_config.align==WA_CENTER)?(dsp.width()-_width)/2:_config.left;
    _fb->begin(&dsp, _rl, _config.top, _width, _textheight, _bgcolor);
  }
  #endif
}

/************************
      SLIDER WIDGET
 ************************/
void SliderWidget::init(FillConfig conf, uint16_t fgcolor, uint16_t bgcolor, uint32_t maxval, uint16_t oucolor) {
  Widget::init(conf.widget, fgcolor, bgcolor);
  _width = conf.width; _height = conf.height; _outlined = conf.outlined; _oucolor = oucolor, _max = maxval;
  _oldvalwidth = _value = 0;
}

void SliderWidget::setValue(uint32_t val) {
  _value = val;
  if (_active && !_locked) _drawslider();

}

void SliderWidget::setColor(uint16_t fgcolor) {
  // Если цвет не изменился, выходим без лишней перерисовки.
  if (_fgcolor == fgcolor) return;

  // Обновляем цвет заполненной части полосы.
  _fgcolor = fgcolor;

  // Если виджет активен и не заблокирован, сразу перерисовываем
  // всю полосу, чтобы новый цвет применился даже при неизменной ширине.
  if (_active && !_locked) _draw();
}

void SliderWidget::setMax(uint32_t maxval) {
  // Защита от деления на ноль в map(_value, 0, _max, ...).
  if (maxval == 0) maxval = 1;
  if (_max == maxval) return;
  _max = maxval;
  if (_active && !_locked) _draw();
}

void SliderWidget::_drawslider() {
  uint16_t valwidth = map(_value, 0, _max, 0, _width - _outlined * 2);
  if (_oldvalwidth == valwidth) return;
  dsp.fillRect(_config.left + _outlined + min(valwidth, _oldvalwidth), _config.top + _outlined, abs(_oldvalwidth - valwidth), _height - _outlined * 2, _oldvalwidth > valwidth ? _bgcolor : _fgcolor);
  _oldvalwidth = valwidth;
}

void SliderWidget::_draw() {
  if(_locked) return;
  _clear();
  if(!_active) return;
  if (_outlined) dsp.drawRect(_config.left, _config.top, _width, _height, _oucolor);
  uint16_t valwidth = map(_value, 0, _max, 0, _width - _outlined * 2);
  dsp.fillRect(_config.left + _outlined, _config.top + _outlined, valwidth, _height - _outlined * 2, _fgcolor);
}

void SliderWidget::_clear() {
//  _oldvalwidth = 0;
  dsp.fillRect(_config.left, _config.top, _width, _height, _bgcolor);
}
void SliderWidget::_reset() {
  _oldvalwidth = 0;
}
/************************
      VU WIDGET
 ************************/
#if !defined(DSP_LCD) && !defined(DSP_OLED)
VuWidget::~VuWidget() {
  if(_canvas) free(_canvas);
}

#if DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_ST7789 || DSP_MODEL==DSP_ST7789_240 || DSP_MODEL==DSP_ST7789_76 \
    || (((DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486)) && !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT)) \
    || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
/* ST7789* / ILI9488 landscape / AXS Faster / Supremus ST7701: VU как в yoRadio (WA_LEFT → вертикальные L/R).
 * Без AXS/ST7701 в этом #if панель попадала в lstepMOD-ветку (canvas width×height*2) —
 * тот же «битый» VU, что на _170 до v0.8.194. */
void VuWidget::init(WidgetConfig wconf, VUBandsConfig bands, uint16_t vumaxcolor, uint16_t vumincolor, uint16_t bgcolor) {
  Widget::init(wconf, bgcolor, bgcolor);
  _vumaxcolor = vumaxcolor;
  _vumincolor = vumincolor;
  _bands = bands;
  _canvas = new Canvas(_bands.width * 2 + _bands.space, _bands.height);
}

void VuWidget::_draw(){
  if(!_active || _locked) return;
  static uint16_t measL, measR;
  uint16_t bandColor;
  uint16_t dimension = _config.align ? _bands.width : _bands.height;
  uint16_t vulevel = player.get_VUlevel(dimension);

  uint8_t L = (vulevel >> 8) & 0xFF;
  uint8_t R = vulevel & 0xFF;

  bool played = player.isRunning();
  if(played){
    measL = (L >= measL) ? measL + _bands.fadespeed : L;
    measR = (R >= measR) ? measR + _bands.fadespeed : R;
  }else{
    if(measL < dimension) measL += _bands.fadespeed;
    if(measR < dimension) measR += _bands.fadespeed;
  }
  if(measL > dimension) measL = dimension;
  if(measR > dimension) measR = dimension;

  uint8_t h = (dimension / _bands.perheight) - _bands.vspace;
  _canvas->fillRect(0, 0, _bands.width * 2 + _bands.space, _bands.height, _bgcolor);

  for(int i = 0; i < dimension; i++){
    if(i % (dimension / _bands.perheight) == 0){
      if(_config.align){
        #ifndef BOOMBOX_STYLE
          bandColor = (i > _bands.width - (_bands.width / _bands.perheight) * 4) ? _vumaxcolor : _vumincolor;
          _canvas->fillRect(i, 0, h, _bands.height, bandColor);
          _canvas->fillRect(i + _bands.width + _bands.space, 0, h, _bands.height, bandColor);
        #else
          bandColor = (i > (_bands.width / _bands.perheight)) ? _vumincolor : _vumaxcolor;
          _canvas->fillRect(i, 0, h, _bands.height, bandColor);
          bandColor = (i > _bands.width - (_bands.width / _bands.perheight) * 3) ? _vumaxcolor : _vumincolor;
          _canvas->fillRect(i + _bands.width + _bands.space, 0, h, _bands.height, bandColor);
        #endif
      }else{
        bandColor = (i < (_bands.height / _bands.perheight) * 3) ? _vumaxcolor : _vumincolor;
        _canvas->fillRect(0, i, _bands.width, h, bandColor);
        _canvas->fillRect(_bands.width + _bands.space, i, _bands.width, h, bandColor);
      }
    }
  }

  if(_config.align){
    #ifndef BOOMBOX_STYLE
      _canvas->fillRect(_bands.width - measL, 0, measL, _bands.width, _bgcolor);
      _canvas->fillRect(_bands.width * 2 + _bands.space - measR, 0, measR, _bands.width, _bgcolor);
#if DSP_MODEL==DSP_AXS15231B
      /* Не писать в FB, пока flush читает его (~50–90 ms) — иначе tearing. */
      if (!axsFbTryLockMs(0)) return;
#endif
      dsp.drawRGBBitmap(_config.left, _config.top, _canvas->getBuffer(), _bands.width * 2 + _bands.space, _bands.height);
#if DSP_MODEL==DSP_AXS15231B
      axsMarkFbDirty();
      axsFbUnlock();
#endif
    #else
      _canvas->fillRect(0, 0, _bands.width - (_bands.width - measL), _bands.width, _bgcolor);
      _canvas->fillRect(_bands.width * 2 + _bands.space - measR, 0, measR, _bands.width, _bgcolor);
#if DSP_MODEL==DSP_AXS15231B
      if (!axsFbTryLockMs(0)) return;
#endif
      dsp.drawRGBBitmap(_config.left, _config.top, _canvas->getBuffer(), _bands.width * 2 + _bands.space, _bands.height);
#if DSP_MODEL==DSP_AXS15231B
      axsMarkFbDirty();
      axsFbUnlock();
#endif
    #endif
  }else{
    _canvas->fillRect(0, 0, _bands.width, measL, _bgcolor);
    _canvas->fillRect(_bands.width + _bands.space, 0, _bands.width, measR, _bgcolor);
#if DSP_MODEL==DSP_AXS15231B
    if (!axsFbTryLockMs(0)) return;
#endif
    dsp.drawRGBBitmap(_config.left, _config.top, _canvas->getBuffer(), _bands.width * 2 + _bands.space, _bands.height);
#if DSP_MODEL==DSP_AXS15231B
    axsMarkFbDirty();
    axsFbUnlock();
#endif
  }
}

void VuWidget::loop(){
  if(_active || !_locked) _draw();
}

void VuWidget::_clear(){
#if DSP_MODEL==DSP_AXS15231B
  if (!axsFbTryLockMs(3)) return;
#endif
  dsp.fillRect(_config.left, _config.top, _bands.width * 2 + _bands.space, _bands.height, _bgcolor);
#if DSP_MODEL==DSP_AXS15231B
  axsMarkFbDirty();
  axsFbUnlock();
#endif
}

#else // не ST7789* — lstepMOD VU (горизонтальные L/R друг под другом: NV3041/ILI/…)

void VuWidget::init(WidgetConfig wconf, VUBandsConfig bands, uint16_t vumaxcolor, uint16_t vumincolor, uint16_t bgcolor) {
  Widget::init(wconf, bgcolor, bgcolor);
  _vumaxcolor = vumaxcolor;
  _vumincolor = vumincolor;
  _bands = bands;
  uint16_t canvasW = _bands.width ? _bands.width : 1;
  uint16_t canvasH = _bands.height * 2 + _bands.space;
  if (!canvasH) canvasH = 1;
  _canvas = new Canvas(canvasW, canvasH);
}


void VuWidget::_draw(){
  if(!_active || _locked) return;
#if !defined(USE_NEXTION) && I2S_DOUT==255
/*  static uint8_t cc = 0;
  cc++;
  if(cc>0){
    player.getVUlevel();
    cc=0;
  }*/
#endif
  static uint16_t measL, measR;
  uint16_t bandColor;

  if(_bands.width == 0) _bands.width = 1;
  if(_bands.height == 0) _bands.height = 1;
  if(_bands.perheight == 0) _bands.perheight = 1;

  uint16_t dimension = _bands.width;
  if (!dimension) dimension = 1; // страховка от нулевого диапазона
  uint16_t perh = _bands.perheight;
  uint16_t bandStep = dimension / perh;
  if (!bandStep) bandStep = 1; // предотвращает деление по модулю на ноль
  uint16_t widthDiv = _bands.width / perh;
  if (!widthDiv) widthDiv = 1; // стабилизирует расчёт цветовых зон
  uint8_t h = bandStep > _bands.vspace ? bandStep - _bands.vspace : 1;
  uint16_t vulevel = player.get_VUlevel(dimension);
  
  uint8_t L = (vulevel >> 8) & 0xFF;
  uint8_t R = vulevel & 0xFF;
  
  bool played = player.isRunning();
  if(played){
    measL=(L>=measL)?measL + _bands.fadespeed:L;
    measR=(R>=measR)?measR + _bands.fadespeed:R;
  }else{
    if(measL<dimension) measL += _bands.fadespeed;
    if(measR<dimension) measR += _bands.fadespeed;
  }
  if(measL>dimension) measL=dimension;
  if(measR>dimension) measR=dimension;
  
  _canvas->fillRect(0, 0, _bands.width, _bands.height * 2 + _bands.space, _bgcolor);
  for(int i=0; i<dimension; i++){
    if(i % bandStep == 0){
       bandColor = (i>_bands.width-widthDiv*5)?_vumaxcolor:_vumincolor;
      _canvas->fillRect(i, 0, h, _bands.height, bandColor);
      _canvas->drawRect(i, 0, h, _bands.height, 0xFFFF);
      _canvas->fillRect(i, _bands.height+_bands.space, h, _bands.height, bandColor);
      _canvas->drawRect(i, _bands.height+_bands.space, h, _bands.height, 0xFFFF);
    }
  }
  _canvas->fillRect(_bands.width-measL, 0, measL, _bands.height, _bgcolor);
  _canvas->fillRect(_bands.width-measR, _bands.height+_bands.space, measR, _bands.height, _bgcolor);
  dsp.drawRGBBitmap(_config.left, _config.top, _canvas->getBuffer(), _bands.width, _bands.height*2+_bands.space);
}

void VuWidget::loop(){
  if(_active || !_locked) _draw();
}

void VuWidget::_clear(){
  dsp.fillRect(_config.left, _config.top, _bands.width, _bands.height*2+_bands.space, _bgcolor);
}

#endif // ST7789* yoRadio VU
#else // DSP_LCD
VuWidget::~VuWidget() { }
void VuWidget::init(WidgetConfig wconf, VUBandsConfig bands, uint16_t vumaxcolor, uint16_t vumincolor, uint16_t bgcolor) {
  Widget::init(wconf, bgcolor, bgcolor);
}
void VuWidget::_draw(){ }
void VuWidget::loop(){ }
void VuWidget::_clear(){ }
#endif

#if SPECTRUM_ANALYZER_ACTIVE
/************************
      SPECTRUM WIDGET
 ************************/
void SpectrumWidget::init(WidgetConfig wconf, SpectrumConfig spectrum, uint16_t peakColor,
                          uint16_t bandColor, uint16_t bgcolor) {
  Widget::init(wconf, bandColor, bgcolor);
  _spectrum = spectrum;
  _peakColor = peakColor;
  _bandColor = bandColor;
  _highBandColor = peakColor;
  memset(_oldLevels, 0xFF, sizeof(_oldLevels));
  memset(_oldPeaks, 0xFF, sizeof(_oldPeaks));
}

void SpectrumWidget::loop() {
  if (!_active || _locked) return;
  const uint32_t now = millis();
  if ((now - _lastDrawMs) < _spectrum.frameIntervalMs) return;
  _lastDrawMs = now;
#if ST7701_DIAG_FFT_INTERVAL
  /* Ритм попыток кадра: 50→400→50 = голод loop. Плюс miss/paint — ниже в _draw. */
  {
    static int64_t s_prevUs = 0;
    static uint32_t s_n = 0;
    static uint32_t s_minMs = UINT32_MAX;
    static uint32_t s_maxMs = 0;
    static uint32_t s_sumMs = 0;
    const int64_t tUs = esp_timer_get_time();
    if (s_prevUs > 0) {
      const uint32_t dMs = (uint32_t)((tUs - s_prevUs) / 1000);
      if (dMs < s_minMs) s_minMs = dMs;
      if (dMs > s_maxMs) s_maxMs = dMs;
      s_sumMs += dMs;
      ++s_n;
      if (dMs >= 100U || s_n >= 25U) {
        Serial.printf("[FFT-int] d=%lu min=%lu max=%lu avg=%lu n=%lu\n",
                      (unsigned long)dMs, (unsigned long)s_minMs,
                      (unsigned long)s_maxMs,
                      (unsigned long)(s_sumMs / s_n), (unsigned long)s_n);
        if (s_n >= 25U) {
          s_n = 0;
          s_minMs = UINT32_MAX;
          s_maxMs = 0;
          s_sumMs = 0;
        }
      }
    }
    s_prevUs = tUs;
  }
#endif
  _draw();
}

void SpectrumWidget::_draw() {
  uint8_t levels[MAX_BANDS] = {0};
  uint8_t peaks[MAX_BANDS] = {0};
#if ST7701_DIAG_FFT_INTERVAL
  /* Уточнение: пустой snapshot vs долгий paint vs кадр без смены полос (= «замер» при ровном loop). */
  static uint32_t s_ok = 0, s_miss = 0, s_same = 0;
  static uint32_t s_paintMinUs = UINT32_MAX, s_paintMaxUs = 0, s_paintSumUs = 0;
  static uint32_t s_bandsSum = 0, s_win = 0;
  const int64_t t0 = esp_timer_get_time();
#endif
  if (!spectrumAnalyzer.snapshot(levels, peaks, _lastSequence)) {
#if ST7701_DIAG_FFT_INTERVAL
    ++s_miss;
    ++s_win;
    if (s_win >= 25U) {
      Serial.printf("[FFT-draw] miss=%lu ok=%lu same=%lu paintUs min=%lu max=%lu avg=%lu bandsAvg=%lu\n",
                    (unsigned long)s_miss, (unsigned long)s_ok, (unsigned long)s_same,
                    (unsigned long)(s_paintMinUs == UINT32_MAX ? 0 : s_paintMinUs),
                    (unsigned long)s_paintMaxUs,
                    (unsigned long)(s_ok ? (s_paintSumUs / s_ok) : 0),
                    (unsigned long)(s_ok ? (s_bandsSum / s_ok) : 0));
      s_ok = s_miss = s_same = 0;
      s_paintMinUs = UINT32_MAX;
      s_paintMaxUs = s_paintSumUs = s_bandsSum = s_win = 0;
    }
#endif
    return;
  }

  const uint8_t bands = min<uint8_t>(_spectrum.bandCount, MAX_BANDS);
  uint8_t painted = 0;
  for (uint8_t band = 0; band < bands; ++band) {
    if (levels[band] == _oldLevels[band] && peaks[band] == _oldPeaks[band]) continue;
    _drawBand(band, levels[band], peaks[band]);
    _oldLevels[band] = levels[band];
    _oldPeaks[band] = peaks[band];
    ++painted;
  }
#if ST7701_DIAG_FFT_INTERVAL
  {
    const uint32_t paintUs = (uint32_t)(esp_timer_get_time() - t0);
    ++s_ok;
    ++s_win;
    if (painted == 0) ++s_same;
    s_bandsSum += painted;
    s_paintSumUs += paintUs;
    if (paintUs < s_paintMinUs) s_paintMinUs = paintUs;
    if (paintUs > s_paintMaxUs) s_paintMaxUs = paintUs;
    if (s_win >= 25U) {
      Serial.printf("[FFT-draw] miss=%lu ok=%lu same=%lu paintUs min=%lu max=%lu avg=%lu bandsAvg=%lu\n",
                    (unsigned long)s_miss, (unsigned long)s_ok, (unsigned long)s_same,
                    (unsigned long)(s_paintMinUs == UINT32_MAX ? 0 : s_paintMinUs),
                    (unsigned long)s_paintMaxUs,
                    (unsigned long)(s_ok ? (s_paintSumUs / s_ok) : 0),
                    (unsigned long)(s_ok ? (s_bandsSum / s_ok) : 0));
      s_ok = s_miss = s_same = 0;
      s_paintMinUs = UINT32_MAX;
      s_paintMaxUs = s_paintSumUs = s_bandsSum = s_win = 0;
    }
  }
#endif
}

void SpectrumWidget::_drawBand(uint8_t band, uint8_t level, uint8_t peak) {
  if (!_spectrum.bandCount || !_spectrum.segments) return;

  const uint16_t totalBandGaps = (_spectrum.bandCount - 1U) * _spectrum.bandGap;
  if (_spectrum.width <= totalBandGaps) return;
  const uint16_t bandWidth = (_spectrum.width - totalBandGaps) / _spectrum.bandCount;
  if (!bandWidth) return;

  const uint16_t occupiedWidth = bandWidth * _spectrum.bandCount + totalBandGaps;
  const uint16_t leftMargin = (_spectrum.width - occupiedWidth) / 2U;
  const uint16_t x = _config.left + leftMargin + band * (bandWidth + _spectrum.bandGap);

  const uint16_t totalSegmentGaps = (_spectrum.segments - 1U) * _spectrum.segmentGap;
  if (_spectrum.height <= totalSegmentGaps) return;
  const uint16_t segmentHeight = (_spectrum.height - totalSegmentGaps) / _spectrum.segments;
  if (!segmentHeight) return;

  const uint16_t occupiedHeight = segmentHeight * _spectrum.segments + totalSegmentGaps;
  const uint16_t topMargin = (_spectrum.height - occupiedHeight) / 2U;

  /*
   * Скругление LED-сегментов (radius=1): при высоте/ширине ≤2 px радиус 0,
   * иначе fillRoundRect даёт артефакты на микросегментах.
   */
  const uint8_t segmentRadius = (segmentHeight > 2U && bandWidth > 2U) ? 1U : 0U;

  for (uint8_t segment = 0; segment < _spectrum.segments; ++segment) {
    const uint16_t y = _config.top + topMargin + _spectrum.height
                     - topMargin - (segment + 1U) * segmentHeight
                     - segment * _spectrum.segmentGap;
    const bool isPeak = peak > 0 && segment == static_cast<uint8_t>(peak - 1U);
    const bool isActive = segment < level;

    uint16_t color = _bgcolor;
    if (isPeak) {
      color = _peakColor;
    } else if (isActive) {
      /*
       * Два верхних LED-сегмента окрашиваем цветом перегруза, остальные —
       * основным цветом спектра. Это сохраняет характер старых музыкальных центров.
       */
      color = segment >= (_spectrum.segments - 2U) ? _highBandColor : _bandColor;
    }
    if (segmentRadius) {
      dsp.fillRoundRect(x, y, bandWidth, segmentHeight, segmentRadius, color);
    } else {
      dsp.fillRect(x, y, bandWidth, segmentHeight, color);
    }
  }
}

void SpectrumWidget::_reset() {
  memset(_oldLevels, 0xFF, sizeof(_oldLevels));
  memset(_oldPeaks, 0xFF, sizeof(_oldPeaks));
  _lastSequence = 0;
  _lastDrawMs = 0;
  spectrumAnalyzer.reset();
}

void SpectrumWidget::_clear() {
  dsp.fillRect(_config.left, _config.top, _spectrum.width, _spectrum.height, _bgcolor);
  _reset();
}
#endif

/************************
      NUM & CLOCK
 ************************/
#if !defined(DSP_LCD)
  #if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  const GFXfont* Clock_GFXfontPtr = nullptr;
  #define CLOCKFONT5x7
  #elif TIME_SIZE<19 //19->NOKIA
  const GFXfont* Clock_GFXfontPtr = nullptr;
  #define CLOCKFONT5x7
  #else
  const GFXfont* Clock_GFXfontPtr = &Clock_GFXfont;
  #endif
#endif //!defined(DSP_LCD)

#if !defined(CLOCKFONT5x7) && !defined(DSP_LCD) && !defined(ARDUINO_GFX_H)
  inline GFXglyph *pgm_read_glyph_ptr(const GFXfont *gfxFont, uint8_t c) {
    return gfxFont->glyph + c;
  }
  uint8_t _charWidth(unsigned char c){
    GFXglyph *glyph = pgm_read_glyph_ptr(&Clock_GFXfont, c - 0x20);
    return pgm_read_byte(&glyph->xAdvance);
  }
  uint16_t _textHeight(){
    GFXglyph *glyph = pgm_read_glyph_ptr(&Clock_GFXfont, '8' - 0x20);
    return pgm_read_byte(&glyph->height);
  }
#else //!defined(CLOCKFONT5x7) && !defined(DSP_LCD)
  uint8_t _charWidth(unsigned char c){
  #ifndef DSP_LCD
    uint16_t width = static_cast<uint16_t>(CHARWIDTH) * static_cast<uint16_t>(TIME_SIZE);
    return width > 255 ? 255 : static_cast<uint8_t>(width);
  #else
    return 1;
  #endif
  }
  uint16_t _textHeight(){
    return CHARHEIGHT * TIME_SIZE;
  }
#endif
uint16_t _textWidth(const char *txt){
  uint16_t w = 0, l=strlen(txt);
  for(uint16_t c=0;c<l;c++) w+=_charWidth(txt[c]);
//  #if DSP_MODEL==DSP_ILI9225
//  return w+l;
//  #else
  return w;
//  #endif
}

/************************
      NUM WIDGET
 ************************/
void NumWidget::init(WidgetConfig wconf, uint16_t buffsize, bool uppercase, uint16_t fgcolor, uint16_t bgcolor) {
  Widget::init(wconf, fgcolor, bgcolor);
  _buffsize = buffsize;
  _text = (char *) malloc(sizeof(char) * _buffsize);
  memset(_text, 0, _buffsize);
  _oldtext = (char *) malloc(sizeof(char) * _buffsize);
  memset(_oldtext, 0, _buffsize);
  _textwidth = _oldtextwidth = _oldleft = 0;
  _uppercase = uppercase;
#if DSP_MODEL==DSP_NV3041A
  // NV3041A: цифры громкости — шрифт DirectiveFour56 (см. DspCore::setNumFont), не setTextSize(TIME_SIZE).
  _textheight = 58;
#elif DSP_MODEL==DSP_ST7789_170
  // ST7789_170: цифры — Clock_GFXfont (DS_DIGI56), см. setNumFont().
  _textheight = 56;
#elif DSP_MODEL==DSP_AXS15231B
  /* DS_DIGI56 (масштаб JC2432 DIGI42 на 320 px по высоте). */
  _textheight = 56;
#elif DSP_MODEL==DSP_ST7701
  /* Supremus: тот же DS_DIGI56 (dsfont70). */
  _textheight = 56;
#else
  _textheight = TIME_SIZE/*wconf.textsize*/;
#endif
}

void NumWidget::setText(const char* txt) {
  strlcpy(_text, txt, _buffsize);
  _getBounds();
  if (strcmp(_oldtext, _text) == 0) return;
  uint16_t realth = _textheight;
#if defined(DSP_OLED) && DSP_MODEL!=DSP_SSD1322
  if(Clock_GFXfontPtr==nullptr) realth = _textheight * 8; //CHARHEIGHT
#endif
  if (_active)
  #ifndef CLOCKFONT5x7
    dsp.fillRect(_oldleft == 0 ? _realLeft() : min(_oldleft, _realLeft()),  _config.top-_textheight+1, max(_oldtextwidth, _textwidth), realth, _bgcolor);
  #else
#if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
    // Затирание по реальной геометрии глифа (xAdvance «1»/«8» одинаковый — старый min(left) оставляет хвосты).
    dsp.setNumFont();
    dsp.clearClipping();
    int16_t bx, by; uint16_t bw, bh;
    if (_oldtext[0]) {
      dsp.getTextBounds(_oldtext, _oldleft, _config.top, &bx, &by, &bw, &bh);
      dsp.fillRect(bx - 4, by - 4, bw + 8, bh + 8, _bgcolor);
    }
    dsp.getTextBounds(_text, 0, 0, &bx, &by, &bw, &bh);
    _oldleft = ((int16_t)dsp.width() - (int16_t)bw) / 2 - bx;
    dsp.getTextBounds(_text, _oldleft, _config.top, &bx, &by, &bw, &bh);
    dsp.fillRect(bx - 4, by - 4, bw + 8, bh + 8, _bgcolor);
#else
    dsp.fillRect(_oldleft == 0 ? _realLeft() : min(_oldleft, _realLeft()),  _config.top, max(_oldtextwidth, _textwidth), realth, _bgcolor);
#endif
  #endif

  _oldtextwidth = _textwidth;
#if !(DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701)
  _oldleft = _realLeft();
#endif
  if (_active) _draw();
}

void NumWidget::setText(int val, const char *format){
  char buf[_buffsize];
  snprintf(buf, _buffsize, format, val);
  setText(buf);
}

void NumWidget::_getBounds() {
#if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  // Реальная ширина GFX-шрифта цифр (не CHARWIDTH*TIME_SIZE — даёт ~420 px на символ).
  _textwidth = dsp.textWidth(_text);
#else
  _textwidth= _textWidth(_text);
#endif
}

void NumWidget::_draw() {
#ifndef DSP_LCD
  if(!_active || TIME_SIZE<2) return;
#if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  // Экран VOL/NUMBERS: тот же цифровой шрифт, что задуман в DspCore::setNumFont().
  dsp.setNumFont();
#else
  dsp.setTextSize(Clock_GFXfontPtr==nullptr?TIME_SIZE:1);
  dsp.setFont(Clock_GFXfontPtr);
#endif
  dsp.setTextColor(_fgcolor, _bgcolor);
#endif
  if(!_active) return;
#if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  dsp.setCursor(_oldleft, _config.top);  // X уже посчитан через getTextBounds в setText()
#else
  dsp.setCursor(_realLeft(), _config.top);
#endif
  dsp.print(_text);
  strlcpy(_oldtext, _text, _buffsize);
  dsp.setFont();
}

/**************************
      PROGRESS WIDGET
 **************************/
void ProgressWidget::_progress() {
  char buf[_width + 1];
  snprintf(buf, _width, "%*s%.*s%*s", _pg <= _barwidth ? 0 : _pg - _barwidth, "", _pg <= _barwidth ? _pg : 5, ".....", _width - _pg, "");
  _pg++; if (_pg >= _width + _barwidth) _pg = 0;
  setText(buf);
}

bool ProgressWidget::_checkDelay(int m, uint32_t &tstamp) {
  if (millis() - tstamp > m) {
    tstamp = millis();
    return true;
  } else {
    return false;
  }
}

void ProgressWidget::loop() {
  if (_checkDelay(_speed, _scrolldelay)) {
    _progress();
  }
}

/**************************
      CLOCK WIDGET
 **************************/
void ClockWidget::init(WidgetConfig wconf, uint16_t fgcolor, uint16_t bgcolor){
  Widget::init(wconf, fgcolor, bgcolor);
  _timeheight = _textHeight();
  _fullclock = TIME_SIZE>35 || DSP_MODEL==DSP_ILI9225;
  if(_fullclock) _superfont = TIME_SIZE / 17; //magick
  else if(TIME_SIZE==19 || TIME_SIZE==2) _superfont=1;
  else _superfont=0;
  _space = (5*_superfont)/2; //magick
  #ifndef HIDE_DATE
  if(_fullclock){
    _dateheight = _superfont<4?1:2;
    _clockheight = _timeheight + _space + CHARHEIGHT * _dateheight;
  } else {
    _clockheight = _timeheight;
  }
  #else
    _clockheight = _timeheight;
  #endif
  _getTimeBounds();
#ifdef PSFBUFFER
  _fb = new psFrameBuffer(dsp.width(), dsp.height());
  _begin();
#endif
}

void ClockWidget::_begin(){
#ifdef PSFBUFFER
  _fb->begin(&dsp, _clockleft, _config.top-_timeheight, _clockwidth, _clockheight+1, config.theme.background);
#endif
}

bool ClockWidget::_getTime(){
  strftime(_timebuffer, sizeof(_timebuffer), "%H:%M", &network.timeinfo);
  bool ret = network.timeinfo.tm_sec==0 || _forceflag!=network.timeinfo.tm_year;
  _forceflag = network.timeinfo.tm_year;
  return ret;
}

uint16_t ClockWidget::_left(){
  if(_fb->ready()) return 0; else return _clockleft;
}
uint16_t ClockWidget::_top(){
  if(_fb->ready()) return _timeheight; else return _config.top;
}

void ClockWidget::_getTimeBounds() {
  _timewidth = _textWidth(_timebuffer);
  uint8_t fs = _superfont>0?_superfont:TIME_SIZE;
  uint16_t rightside = CHARWIDTH * fs * 2; // seconds
  if(_fullclock){
    rightside += _space*2+1; //2space+vline
    _clockwidth = _timewidth+rightside;
  } else {
    if(_superfont==0)
      _clockwidth = _timewidth;
    else
      _clockwidth = _timewidth + rightside;
  }
  switch(_config.align){
    case WA_LEFT: _clockleft = _config.left; break;
    case WA_RIGHT: _clockleft = dsp.width()-_clockwidth-_config.left; break;
    default:
      _clockleft = (dsp.width()/2 - _clockwidth/2)+_config.left;
      break;
  }
  char buf[4];
  strftime(buf, 4, "%H", &network.timeinfo);
  _dotsleft=_textWidth(buf);
}

#ifndef DSP_LCD

#if DSP_MODEL!=DSP_NV3041A && DSP_MODEL!=DSP_ST7789_170 && DSP_MODEL!=DSP_AXS15231B && DSP_MODEL!=DSP_ST7701
Adafruit_GFX& ClockWidget::getRealDsp(){
#ifdef PSFBUFFER
  if (_fb && _fb->ready()) return *_fb;
#endif
  return dsp;
}

void ClockWidget::_printClock(bool force){
  auto& gfx = getRealDsp();
  gfx.setTextSize(Clock_GFXfontPtr==nullptr?TIME_SIZE:1);
  gfx.setFont(Clock_GFXfontPtr);
#if DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486
  ili9488_updateCoverSlot(force);
#endif
  bool clockInTitle=!config.isScreensaver && _config.top<_timeheight; //DSP_SSD1306x32
  if(force){
    _clearClock();
    _getTimeBounds();
    #ifndef DSP_OLED
    if(CLOCKFONT_MONO) {
      gfx.setTextColor(config.theme.clockbg, config.theme.background);
      gfx.setCursor(_left(), _top());
      gfx.print("88:88");
    }
    #endif
    if(clockInTitle)
      gfx.setTextColor(config.theme.meta, config.theme.metabg);
    else
      gfx.setTextColor(config.theme.clock, config.theme.background);
    gfx.setCursor(_left(), _top());
    gfx.print(_timebuffer);
    if(_fullclock){
      // lines, date & dow
      bool fullClockOnScreensaver = (!config.isScreensaver || (_fb->ready() && FULL_SCR_CLOCK));
      _linesleft = _left()+_timewidth+_space;
      if(fullClockOnScreensaver){
        gfx.drawFastVLine(_linesleft, _top()-_timeheight, _timeheight, config.theme.div);
        gfx.drawFastHLine(_linesleft, _top()-(_timeheight)/2, CHARWIDTH * _superfont * 2 + _space, config.theme.div);
        gfx.setFont();
        gfx.setTextSize(_superfont);
        gfx.setCursor(_linesleft+_space+1, _top()-CHARHEIGHT * _superfont);
        gfx.setTextColor(config.theme.dow, config.theme.background);
        // NV3041A — своя реализация utf8Rus в DspCore, остальные — глобальная функция utf8Rus().
#if DSP_MODEL==DSP_NV3041A
        gfx.print(dsp.utf8Rus(LANG::dow[network.timeinfo.tm_wday], false));
#else
        gfx.print(utf8Rus(LANG::dow[network.timeinfo.tm_wday], false));
#endif
        snprintf(_tmp, sizeof(_tmp), "%2d %s %d", network.timeinfo.tm_mday,LANG::mnths[network.timeinfo.tm_mon], network.timeinfo.tm_year+1900);
        #ifndef HIDE_DATE
        // Перекодировка даты в кириллицу с учётом регистра для всех типов дисплеев.
#if DSP_MODEL==DSP_NV3041A
        strlcpy(_datebuf, dsp.utf8Rus(_tmp, true), sizeof(_datebuf));
#else
        strlcpy(_datebuf, utf8Rus(_tmp, true), sizeof(_datebuf));
#endif
        uint16_t _datewidth = strlen(_datebuf) * CHARWIDTH*_dateheight;
        gfx.setTextSize(_dateheight);
        #if DSP_MODEL==DSP_GC9A01A
        gfx.setCursor((dsp.width()-_datewidth)/2, _top() + _space);
        #else
        gfx.setCursor(_left()+_clockwidth-_datewidth, _top() + _space);
        #endif
        gfx.setTextColor(config.theme.date, config.theme.background);
        gfx.print(_datebuf);
        #endif
      }
    }
  }
  if(_fullclock || _superfont>0){
    gfx.setFont();
    gfx.setTextSize(_superfont);
    if(!_fullclock){
      #ifndef CLOCKFONT5x7
      gfx.setCursor(_left()+_timewidth+_space, _top()-_timeheight+_space);
      #else
      gfx.setCursor(_left()+_timewidth+_space, _top());
      #endif
    }else{
      gfx.setCursor(_linesleft+_space+1, _top()-_timeheight);
    }
    gfx.setTextColor(config.theme.seconds, config.theme.background);
    snprintf(_tmp, sizeof(_tmp), "%02d", network.timeinfo.tm_sec);
    gfx.print(_tmp);
  }
  gfx.setTextSize(Clock_GFXfontPtr==nullptr?TIME_SIZE:1);
  gfx.setFont(Clock_GFXfontPtr);
  #ifndef DSP_OLED
  gfx.setTextColor(dots ? config.theme.clock : (CLOCKFONT_MONO?config.theme.clockbg:config.theme.background), config.theme.background);
  #else
  if(clockInTitle)
    gfx.setTextColor(dots ? config.theme.meta:config.theme.metabg, config.theme.metabg);
  else
    gfx.setTextColor(dots ? config.theme.clock:config.theme.background, config.theme.background);
  #endif
  dots=!dots;
  gfx.setCursor(_left()+_dotsleft, _top());
  gfx.print(":");
  gfx.setFont();
  if(_fb->ready()) _fb->display();
}
#endif

void ClockWidget::_clearClock(){
#if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  dsp.clearClock();
  return;
#endif
#ifdef PSFBUFFER
  if(_fb->ready()) _fb->clear();
  else
#endif
#ifndef CLOCKFONT5x7
  dsp.fillRect(_left(), _top()-_timeheight, _clockwidth, _clockheight+1, config.theme.background);
#else
  dsp.fillRect(_left(), _top(), _clockwidth+1, _clockheight+1, config.theme.background);
#endif
}

void ClockWidget::draw(bool force){
#if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  if(!_active) return;
  dsp.printClock(_config.top, _config.left, _config.textsize, force);
  return;
#endif
  if(!_active) return;
  _printClock(_getTime() || force);
}

void ClockWidget::_draw(){
#if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  if(!_active) return;
  dsp.printClock(_config.top, _config.left, _config.textsize, true);
  return;
#endif
  if(!_active) return;
  _printClock(true);
}

void ClockWidget::_reset(){
#if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  _clearClock();
  return;
#endif
#ifdef PSFBUFFER
  if(_fb->ready()) {
    _fb->freeBuffer();
    _getTimeBounds();
    _begin();
  }
#endif
}

void ClockWidget::_clear(){
  _clearClock();
}
#else //#ifndef DSP_LCD

void ClockWidget::_printClock(bool force){
  strftime(_timebuffer, sizeof(_timebuffer), "%H:%M", &network.timeinfo);
  if(force){
    dsp.setCursor(dsp.width()-5, 0);
    dsp.print(_timebuffer);
  }
  dsp.setCursor(dsp.width()-5+2, 0);
  dsp.print((network.timeinfo.tm_sec % 2 == 0)?":":" ");
}

void ClockWidget::_clearClock(){}

void ClockWidget::draw(bool force){
  if(!_active) return;
  _printClock(true);
}
void ClockWidget::_draw(){
  if(!_active) return;
  _printClock(true);
}
void ClockWidget::_reset(){}
void ClockWidget::_clear(){}
#endif //#ifndef DSP_LCD

/**************************
      BITRATE WIDGET
 **************************/
void BitrateWidget::init(BitrateConfig bconf, uint16_t fgcolor, uint16_t bgcolor){
  Widget::init(bconf.widget, fgcolor, bgcolor);
  _dimension = bconf.dimension;
  _bitrate = 0;
  _format = BF_UNKNOWN;
  _charSize(bconf.widget.textsize, _charWidth, _textheight);
  memset(_buf, 0, 6);
}

void BitrateWidget::setBitrate(uint16_t bitrate){
  _bitrate = bitrate;
  if(_bitrate > 20000)  _bitrate = _bitrate / 1000;
  _draw();
}

void BitrateWidget::setFormat(BitrateFormat format){
  _format = format;
  _draw();
}

//TODO move to parent
void BitrateWidget::_charSize(uint8_t textsize, uint8_t& width, uint16_t& height){
#ifndef DSP_LCD
  width = textsize * CHARWIDTH;
  height = textsize * CHARHEIGHT;
#else
  width = 1;
  height = 1;
#endif
}

void BitrateWidget::_draw(){
  _clear();
  if(!_active || _format == BF_UNKNOWN) return;

  dsp.drawRect(_config.left, _config.top, _dimension, _dimension, _fgcolor);
  dsp.fillRect(_config.left, _config.top + _dimension/2+1, _dimension, _dimension/2-1, _fgcolor);

#if DSP_MODEL==DSP_NV3041A
  /* Рамка L/R у VU — координаты только под NV3041A 480×480. */
  dsp.drawRect(2, 398, 25, 43, config.theme.div);
  dsp.setFont();
  dsp.setTextColor(config.theme.div, _bgcolor);
  dsp.setTextSize(2);
  dsp.setCursor(10, 402);
  dsp.printf("L");
  dsp.setCursor(10, 419);
  dsp.printf("R");
#endif

  dsp.setFont();
  dsp.setTextSize(_config.textsize);
  dsp.setTextColor(_fgcolor, _bgcolor);
  if(_bitrate < 999)  snprintf(_buf, 6, "%d", _bitrate);
  else {
    float br = (float)_bitrate / 1000;
    snprintf(_buf, 6, "%.1f", br);
  }

  dsp.setCursor(_config.left + _dimension/2 - _charWidth*strlen(_buf)/2 + 1, _config.top + _dimension/4 - _textheight/2 + 2);
  dsp.print(_buf);
  dsp.setTextColor(_bgcolor, _fgcolor);
  dsp.setCursor(_config.left + _dimension/2 - _charWidth*3/2 + 1, _config.top + _dimension - _dimension/4 - _textheight/2);
  switch(_format){
    case BF_MP3:  dsp.print("mp3"); break;
    case BF_AAC:  dsp.print("aac"); break;
    case BF_FLAC: dsp.print("flc"); break;
    case BF_OGG:  dsp.print("ogg"); break;
    case BF_WAV:  dsp.print("wav"); break;
    default: break;
  }
}

void BitrateWidget::_clear() {
  dsp.fillRect(_config.left, _config.top, _dimension*1.08, _dimension, _bgcolor);
}


/**************************
      PLAYLIST WIDGET
 **************************/
void PlayListWidget::init(ScrollWidget* current){
  Widget::init({0, 0, 0, WA_LEFT}, 0, 0);
  _current = current;
  #ifndef DSP_LCD
  /* Высота ряда — по фокусу (если есть), иначе по playlistConf; соседи рисуются меньшим size. */
  #if defined(HAS_PLAYLIST_FOCUS_CONF)
  const uint8_t plTs = playlistFocusConf.widget.textsize;
  #else
  const uint8_t plTs = playlistConf.widget.textsize;
  #endif
  _plItemHeight = plTs*(CHARHEIGHT-1)+plTs*4;
  if (_plItemHeight == 0) _plItemHeight = CHARHEIGHT + 4; // защита от деления на ноль
  _plTtemsCount = round((float)dsp.height()/_plItemHeight);
  if(_plTtemsCount%2==0) _plTtemsCount++;
  _plCurrentPos = _plTtemsCount/2;
  _plYStart = (dsp.height() / 2 - _plItemHeight / 2) - _plItemHeight * (_plTtemsCount - 1) / 2 + plTs*2;
  #else
  _plTtemsCount = PLMITEMS;
  _plCurrentPos = 1;
  #endif
}

uint8_t PlayListWidget::_fillPlMenu(int from, uint8_t count) {
  int     ls      = from;
  uint8_t c       = 0;
  bool    finded  = false;
#if (DSP_MODEL == DSP_ST7701) && ST7701_DIAG_PL_NOFS
  /* Experiment 1: без LittleFS — только отрисовка фейковых имён из RAM.
   * Если при NOFS=1 корёж уходит, а при FS остаётся — виновато чтение файла. */
  {
    static bool s_nofsLogged = false;
    if (!s_nofsLogged) {
      Serial.println("[ST7701-DIAG] PL_NOFS=1 — playlist draw from RAM, no LittleFS");
      s_nofsLogged = true;
    }
    for (uint8_t i = 0; i < count; i++) {
      const int num = from + (int)i;
      if (num < 1) {
        _printPLitem(i, "");
      } else {
        char buf[28];
        snprintf(buf, sizeof(buf), "Diag %d", num);
        _printPLitem(i, buf);
      }
    }
    return count;
  }
#endif
  if (config.playlistLength() == 0) {
    return 0;
  }
  File playlist = config.SDPLFS()->open(REAL_PLAYL, "r");
  File index = config.SDPLFS()->open(REAL_INDEX, "r");
  while (true) {
    if (ls < 1) {
      ls++;
      _printPLitem(c, "");
      c++;
      continue;
    }
    if (!finded) {
      index.seek((ls - 1) * 4, SeekSet);
      uint32_t pos;
      index.readBytes((char *) &pos, 4);
      finded = true;
      index.close();
      playlist.seek(pos, SeekSet);
    }
    bool pla = true;
    while (pla) {
      pla = playlist.available();
      String stationName = playlist.readStringUntil('\n');
      stationName = stationName.substring(0, stationName.indexOf('\t'));
      if(config.store.numplaylist && stationName.length()>0) stationName = String(from+c)+" "+stationName;
      _printPLitem(c, stationName.c_str());
      c++;
      if (c >= count) break;
    }
    break;
  }
  playlist.close();
  return c;
}
#ifndef DSP_LCD
void PlayListWidget::drawPlaylist(uint16_t currentItem) {
  uint8_t lastPos = _fillPlMenu(currentItem - _plCurrentPos, _plTtemsCount);
  if(lastPos<_plTtemsCount){
    dsp.fillRect(0, lastPos*_plItemHeight+_plYStart, dsp.width(), dsp.height()/2, config.theme.background);
  }
}

void PlayListWidget::_printPLitem(uint8_t pos, const char* item){
  dsp.setTextSize(playlistConf.widget.textsize);
  if (pos == _plCurrentPos) {
    _current->setText(item);
  } else {
    uint8_t plColor = (abs(pos - _plCurrentPos)-1)>4?4:abs(pos - _plCurrentPos)-1;
    dsp.setTextColor(config.theme.playlist[plColor], config.theme.background);
    dsp.setCursor(TFT_FRAMEWDT, _plYStart + pos * _plItemHeight);
    dsp.fillRect(0, _plYStart + pos * _plItemHeight - 1, dsp.width(), _plItemHeight - 2, config.theme.background);
    // Пункты плейлиста также должны корректно отображать кириллицу на всех дисплеях.
#if DSP_MODEL==DSP_NV3041A
    dsp.print(dsp.utf8Rus(item, _current->uppercase()));
#else
    dsp.print(utf8Rus(item, _current->uppercase()));
#endif
  }
}
#else
void PlayListWidget::_printPLitem(uint8_t pos, const char* item){
  if (pos == _plCurrentPos) {
    _current->setText(item);
  } else {
    dsp.setCursor(1, pos);
    char tmp[dsp.width()] = {0};
    // Буфер для временного текста плейлиста с перекодировкой под конкретный дисплей.
#if DSP_MODEL==DSP_NV3041A
    strlcpy(tmp, dsp.utf8Rus(item, _current->uppercase()), dsp.width());
#else
    strlcpy(tmp, utf8Rus(item, _current->uppercase()), dsp.width());
#endif
    dsp.print(tmp);
  }
}

void PlayListWidget::drawPlaylist(uint16_t currentItem) {
  dsp.clear();
  _fillPlMenu(currentItem - _plCurrentPos, _plTtemsCount);
  dsp.setCursor(0,1);
  dsp.write(uint8_t(126));
}
#endif


#endif // #if DSP_MODEL!=DSP_DUMMY
