#include "options.h"
#if (TS_MODEL!=TS_MODEL_UNDEFINED) && (DSP_MODEL!=DSP_DUMMY)
#include "Arduino.h"
#include "touchscreen.h"
#include "config.h"
#include "controls.h"
#include "display.h"
#include "player.h"

#ifndef TS_X_MIN
  #define TS_X_MIN              400
#endif
#ifndef TS_X_MAX
  #define TS_X_MAX              3800
#endif
#ifndef TS_Y_MIN
  #define TS_Y_MIN              260
#endif
#ifndef TS_Y_MAX
  #define TS_Y_MAX              3800
#endif
#ifndef TS_STEPS
  #define TS_STEPS              40
#endif
// Универсально для любого тач-дисплея: верхняя полоса (название станции) — тап переключает WEB↔SD.
// Работает только при собранной поддержке SD (USE_SD, т.е. SDC_CS != 255); иначе только WEB — зона как у остального экрана (пауза/воспроизведение).
#ifndef TS_TOP_BAR_Y_MAX
  #define TS_TOP_BAR_Y_MAX      50
#endif

#if TS_MODEL==TS_MODEL_XPT2046
  #ifdef TS_SPIPINS
    SPIClass  TSSPI(HSPI);
  #endif
  #include <XPT2046_Touchscreen.h>
  XPT2046_Touchscreen ts(TS_CS);
  typedef TS_Point TSPoint;
#elif TS_MODEL==TS_MODEL_GT911
  #include "../GT911_Touchscreen/TAMC_GT911.h"
  TAMC_GT911 ts = TAMC_GT911(TS_SDA, TS_SCL, TS_INT, TS_RST, 0, 0);
  typedef TP_Point TSPoint;
#endif

void TouchScreen::init(uint16_t w, uint16_t h){
  // Инициализация членов класса
  _oldTouchX = 0;
  _oldTouchY = 0;
  _touchdelay = 0;
  
  // Инициализация тачскрина. Подробный отладочный вывод отключён по умолчанию,
  // чтобы не захламлять монитор в рабочем режиме.
  
#if (DSP_MODEL==DSP_NV3041A) && (TS_MODEL==TS_MODEL_XPT2046)
  #ifdef TS_SPIPINS
    TSSPI.begin(TS_SPIPINS);
    ts.begin(TSSPI);
  #else
    #if TS_HSPI
      ts.begin(SPI2);
    #else
      ts.begin();
    #endif
  #endif
  ts.setRotation(config.store.fliptouch ? 4 : 2);
#else
#if TS_MODEL==TS_MODEL_XPT2046
  #ifdef TS_SPIPINS
    TSSPI.begin(TS_SPIPINS);
    ts.begin(TSSPI);
  #else
    #if TS_HSPI
      ts.begin(SPI2);
    #else
      ts.begin();
    #endif
  #endif
  ts.setRotation(config.store.fliptouch?3:1);
#endif
#if TS_MODEL==TS_MODEL_GT911
  ts.begin();
  ts.setRotation(config.store.fliptouch?2:0);
#endif
#endif
  _width  = w;
  _height = h;
#if TS_MODEL==TS_MODEL_GT911
  ts.setResolution(_width, _height);
#endif
}

tsDirection_e TouchScreen::_tsDirection(uint16_t x, uint16_t y) {
  int16_t dX = x - _oldTouchX;
  int16_t dY = y - _oldTouchY;
  if (abs(dX) > 20 || abs(dY) > 20) {
    if (abs(dX) > abs(dY)) {
      if (dX > 0) {
        return TSD_RIGHT;
      } else {
        return TSD_LEFT;
      }
    } else {
      if (dY > 0) {
        return TSD_DOWN;
      } else {
        return TSD_UP;
      }
    }
  } else {
    return TDS_REQUEST;
  }
}

void TouchScreen::flip(){
#if (DSP_MODEL==DSP_NV3041A) && (TS_MODEL==TS_MODEL_XPT2046)
  ts.setRotation(config.store.fliptouch ? 4 : 2);
#else
#if TS_MODEL==TS_MODEL_XPT2046
  ts.setRotation(config.store.fliptouch?3:1);
#endif
#if TS_MODEL==TS_MODEL_GT911
  ts.setRotation(config.store.fliptouch?2:0);
#endif
#endif
}

void TouchScreen::loop(){
  uint16_t touchX, touchY;
  static bool wastouched = false;  // Изменено на false для правильной первоначальной логики
  static uint32_t touchLongPress;
  static tsDirection_e direct;
  static uint16_t touchVol, touchStation;
  if (!_checklpdelay(20, _touchdelay)) return;
#if TS_MODEL==TS_MODEL_GT911
  ts.read();
#endif
  bool istouched = _istouched();

  // Подробный поток отладки касаний отключён по умолчанию.
  // При необходимости можно вернуть через config.store.dbgtouch.
  if(istouched){
  #if TS_MODEL==TS_MODEL_XPT2046
    TSPoint p = ts.getPoint();
    touchX = map(p.x, TS_X_MIN, TS_X_MAX, 0, _width);
    touchY = map(p.y, TS_Y_MIN, TS_Y_MAX, 0, _height);
  #elif TS_MODEL==TS_MODEL_GT911
    TSPoint p = ts.points[0];
    touchX = p.x;
    touchY = p.y;
    #if (DSP_MODEL==DSP_NV3041A)
      touchY = _height - touchY;
    #endif
  #endif
  if (!wastouched) { /*     START TOUCH     */
      _oldTouchX = touchX;
      _oldTouchY = touchY;
      touchVol = touchX;
      touchStation = touchY;
      direct = TDS_REQUEST;
      touchLongPress=millis();
    } else { /*     SWIPE TOUCH     */
      direct = _tsDirection(touchX, touchY);
      switch (direct) {
        case TSD_LEFT:
        case TSD_RIGHT: {
            touchLongPress=millis();
            if(display.mode()==PLAYER || display.mode()==VOL){
              int16_t xDelta = map(abs(touchVol - touchX), 0, _width, 0, TS_STEPS);
              display.putRequest(NEWMODE, VOL);
              if (xDelta>1) {
                controlsEvent((touchVol - touchX)<0);
                touchVol = touchX;
              }
            }
            break;
          }
        case TSD_UP:
        case TSD_DOWN: {
            touchLongPress=millis();
            if(display.mode()==PLAYER || display.mode()==STATIONS){
              int16_t yDelta = map(abs(touchStation - touchY), 0, _height, 0, TS_STEPS);
              display.putRequest(NEWMODE, STATIONS);
              if (yDelta>1) {
                controlsEvent((touchStation - touchY)<0);
                touchStation = touchY;
              }
            }
            break;
          }
        default:
            break;
      }
    }
    if (config.store.dbgtouch) {
      Serial.print(", x = ");
      Serial.print(p.x);
      Serial.print(", y = ");
      Serial.println(p.y);
    }
  }else{
    if (wastouched) {/*     END TOUCH     */
      if (direct == TDS_REQUEST) {
        uint32_t pressTicks = millis()-touchLongPress;
        if( pressTicks < BTN_PRESS_TICKS*2){
          if(pressTicks > 50) {
#ifdef USE_SD
            // Тап по верхней полосе — переключение WEB↔SD. У NV3041A+GT911 Y инвертирован (верх = большие значения).
            #if (DSP_MODEL==DSP_NV3041A) && (TS_MODEL==TS_MODEL_GT911)
            bool inTopBar = (_oldTouchY >= (uint16_t)(_height - TS_TOP_BAR_Y_MAX));
            #else
            bool inTopBar = (_oldTouchY < TS_TOP_BAR_Y_MAX);
            #endif
            if (inTopBar) {
              playMode_e next = (config.getMode() == PM_WEB) ? PM_SDCARD : PM_WEB;
              {
                FILE* f = fopen("debug-0da86f.log", "a");
                if (f) {
                  // #region agent log
                  fprintf(
                    f,
                    "{\"sessionId\":\"0da86f\",\"runId\":\"run1\",\"hypothesisId\":\"H5\",\"location\":\"touchscreen.cpp:topbar_tap\",\"message\":\"top bar touch requested mode switch\",\"data\":{\"touchY\":%u,\"height\":%u,\"nextMode\":%d,\"currentMode\":%d},\"timestamp\":%lu}\n",
                    (unsigned)_oldTouchY,
                    (unsigned)_height,
                    (int)next,
                    (int)config.getMode(),
                    (unsigned long)millis()
                  );
                  // #endregion
                  fclose(f);
                }
              }
              scheduleChangeModeTask((int)next);
            } else
#endif
            onBtnClick(EVT_BTNCENTER);
          }
        }else{
          display.putRequest(NEWMODE, display.mode() == PLAYER ? STATIONS : PLAYER);
        }
      }
      direct = TSD_STAY;
    }
  }
  wastouched = istouched;
}

bool TouchScreen::_checklpdelay(int m, uint32_t &tstamp) {
  if (millis() - tstamp > m) {
    tstamp = millis();
    return true;
  } else {
    return false;
  }
}

bool TouchScreen::_istouched(){
#if TS_MODEL==TS_MODEL_XPT2046
  return ts.touched();
#elif TS_MODEL==TS_MODEL_GT911
  return ts.isTouched;
#endif
}

#endif  // TS_MODEL!=TS_MODEL_UNDEFINED
