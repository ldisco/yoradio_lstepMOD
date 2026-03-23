#include "../../core/options.h"
#if DSP_MODEL!=DSP_DUMMY
#include "../dspcore.h"
#include "Arduino.h"
#include "widgets.h"
#include "../../core/player.h"    //  for VU widget
#include "../../core/network.h"   //  for Clock widget
#include "../../core/config.h"
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
  // [FIX][STABILITY] Проверяем _fb на nullptr перед ready():
  // на конфигурациях без PSFBUFFER буфер может не создаваться.
  if(_fb && _fb->ready()){
  #ifdef PSFBUFFER
    _fb->setTextSize(_config.textsize);
    _fb->setTextColor(_fgcolor, _bgcolor);
  #endif
  }else{
    dsp.setTextSize(_config.textsize);
    dsp.setTextColor(_fgcolor, _bgcolor);
  }
}

bool ScrollWidget::_checkIsScrollNeeded() {
  return _textwidth > _width;
}

void ScrollWidget::setText(const char* txt) {
  // [FIX][BOOTLOOP][SD] Жёсткая защита входа:
  // 1) txt может прийти как nullptr при гонке/сбое источника меты;
  // 2) внутренние буферы должны быть валидны до любых операций форматирования.
  if (!_text || !_oldtext || _buffsize < 2) return;
  if (!txt) txt = "";

  // [FIX][BOOTLOOP][SD] Сначала копируем вход в локальный буфер фиксированного размера.
  // Это защищает от изменения исходного указателя во время конвертации utf8Rus()
  // и не даёт читать "живой" внешний буфер напрямую.
  char localTxt[BUFLEN];
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
  // [FIX][STABILITY] Единый флаг готовности кадрового буфера с проверкой nullptr.
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
        dsp.fillRect(_config.left,  _config.top, _width, _textheight, _bgcolor);
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
        dsp.fillRect(_config.left, _config.top, _width, _textheight, _bgcolor);
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
      dsp.fillRect(_config.left, _config.top, _width, _textheight, _bgcolor);
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
#else // DSP_LCD
VuWidget::~VuWidget() { }
void VuWidget::init(WidgetConfig wconf, VUBandsConfig bands, uint16_t vumaxcolor, uint16_t vumincolor, uint16_t bgcolor) {
  Widget::init(wconf, bgcolor, bgcolor);
}
void VuWidget::_draw(){ }
void VuWidget::loop(){ }
void VuWidget::_clear(){ }
#endif

/************************
      NUM & CLOCK
 ************************/
#if !defined(DSP_LCD)
  #if DSP_MODEL==DSP_NV3041A
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
  _textheight = TIME_SIZE/*wconf.textsize*/;
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
    dsp.fillRect(_oldleft == 0 ? _realLeft() : min(_oldleft, _realLeft()),  _config.top, max(_oldtextwidth, _textwidth), realth, _bgcolor);
  #endif

  _oldtextwidth = _textwidth;
  _oldleft = _realLeft();
  if (_active) _draw();
}

void NumWidget::setText(int val, const char *format){
  char buf[_buffsize];
  snprintf(buf, _buffsize, format, val);
  setText(buf);
}

void NumWidget::_getBounds() {
  _textwidth= _textWidth(_text);
}

void NumWidget::_draw() {
#ifndef DSP_LCD
  if(!_active || TIME_SIZE<2) return;
  dsp.setTextSize(Clock_GFXfontPtr==nullptr?TIME_SIZE:1);
  dsp.setFont(Clock_GFXfontPtr);
  dsp.setTextColor(_fgcolor, _bgcolor);
#endif
  if(!_active) return;
  dsp.setCursor(_realLeft(), _config.top);
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

#if DSP_MODEL!=DSP_NV3041A
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
#if DSP_MODEL==DSP_NV3041A
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
#if DSP_MODEL==DSP_NV3041A
  if(!_active) return;
  dsp.printClock(_config.top, _config.left, _config.textsize, force);
  return;
#endif
  if(!_active) return;
  _printClock(_getTime() || force);
}

void ClockWidget::_draw(){
#if DSP_MODEL==DSP_NV3041A
  if(!_active) return;
  dsp.printClock(_config.top, _config.left, _config.textsize, true);
  return;
#endif
  if(!_active) return;
  _printClock(true);
}

void ClockWidget::_reset(){
#if DSP_MODEL==DSP_NV3041A
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

  // Рамка и метки каналов L/R — рядом с VU-полосами.
  // ИСПРАВЛЕНИЕ:
  // 1) делаем размеры рамки (25x43),
  // 2) сдвигаем блок чуть выше для "воздуха" (на 396 вместо 401).
  // 3) сдвиг влево на 2 px (4→2), вниз на 2 px (396→398) для рамки и текста L/R.
   dsp.drawRect(2, 398, 25, 43, config.theme.div);
  //dsp.drawRect(2, 400, 43, config.theme.div);
  dsp.setFont();
  dsp.setTextColor(config.theme.div, _bgcolor);
  dsp.setTextSize(2);
  dsp.setCursor(10, 402);
  dsp.printf("L");
  dsp.setCursor(10, 419);
  dsp.printf("R");

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
  _plItemHeight = playlistConf.widget.textsize*(CHARHEIGHT-1)+playlistConf.widget.textsize*4;
  if (_plItemHeight == 0) _plItemHeight = CHARHEIGHT + 4; // защита от деления на ноль
  _plTtemsCount = round((float)dsp.height()/_plItemHeight);
  if(_plTtemsCount%2==0) _plTtemsCount++;
  _plCurrentPos = _plTtemsCount/2;
  _plYStart = (dsp.height() / 2 - _plItemHeight / 2) - _plItemHeight * (_plTtemsCount - 1) / 2 + playlistConf.widget.textsize*2;
  #else
  _plTtemsCount = PLMITEMS;
  _plCurrentPos = 1;
  #endif
}

uint8_t PlayListWidget::_fillPlMenu(int from, uint8_t count) {
  int     ls      = from;
  uint8_t c       = 0;
  bool    finded  = false;
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
