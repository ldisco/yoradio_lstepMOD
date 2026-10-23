#include "options.h"
#if (TS_MODEL!=TS_MODEL_UNDEFINED) && (DSP_MODEL!=DSP_DUMMY)
#include "Arduino.h"
#include "touchscreen.h"
#include "config.h"
#include "controls.h"
#include "display.h"
#include "player.h"
#include "timekeeper.h"
#if DSP_MODEL==DSP_ST7789
#include "../displays/displayST7789.h"
  #define COVER_FACT_SWIPE_NS st7789_cover
#elif (DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486) && !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT)
#include "../displays/displayILI9488.h"
  /* Landscape cover-SS: свайп вверх по обложке → TrackFacts (как ST7789 CYD). */
  #define COVER_FACT_SWIPE_NS ili9488_cover
#elif DSP_MODEL==DSP_AXS15231B
#include "../displays/displayAXS15231B.h"
  #define COVER_FACT_SWIPE_NS axs_cover
#endif

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
// Универсально для любого тач-дисплея: верхняя полоса (название станции) — тап переключает WEB↔SD↔BT.
#ifndef TS_TOP_BAR_Y_MAX
  #define TS_TOP_BAR_Y_MAX      50
#endif
// Порог распознавания свайпа (px): выше — VOL/плейлист; для top-bar не применяем при _touchTopBarStart.
#ifndef TS_SWIPE_THRESHOLD
  #define TS_SWIPE_THRESHOLD    20
#endif
/* Мин. сдвиг по X (px), чтобы ОТКРЫТЬ экран VOL. TS_SWIPE_THRESHOLD (20) слишком мал:
 * тап/дребезг при выходе из SS уже даёт L/R и раньше открывал громкость. */
#ifndef TS_VOL_OPEN_MIN_PX
  #define TS_VOL_OPEN_MIN_PX    48
#endif
/* После leave SS→PLAYER: второй «кусок» жеста (AXS mid-drop istouched) не должен
 * открыть плейлист. Окно от метки leave; не сбрасываем на END — истекает само. */
#ifndef TS_SS_LEAVE_PL_GUARD_MS
  #define TS_SS_LEAVE_PL_GUARD_MS  800u
#endif
/* После успешного свайпа факта: второе короткое касание AXS не читать как тап-leave. */
#ifndef TS_COVER_FACT_LEAVE_GUARD_MS
  #define TS_COVER_FACT_LEAVE_GUARD_MS  600u
#endif
/* Ниже этого пика |Δ| на слоте обложки — считаем чистым тапом (выход с SS). */
#ifndef TS_COVER_SLOT_TAP_MAX_PX
  #define TS_COVER_SLOT_TAP_MAX_PX  8
#endif

/* Метка millis() последнего leaveScreensaverToPlayer() из тача; 0 = ещё не было. */
static uint32_t s_tsSsLeaveMs = 0;
/* Метка успешного coverFactSwipeEnd (факт/тост); 0 = ещё не было. */
static uint32_t s_tsCoverFactHandledMs = 0;

static void tsLeaveScreensaverToPlayer() {
  s_tsSsLeaveMs = millis();
  leaveScreensaverToPlayer();
}

#if defined(COVER_FACT_SWIPE_NS)
/* true — сейчас нельзя уходить с Cover-SS тапом по слоту (хвост после факта). */
static bool tsCoverFactLeaveBlocked() {
  return (s_tsCoverFactHandledMs != 0u) &&
         ((uint32_t)(millis() - s_tsCoverFactHandledMs) < TS_COVER_FACT_LEAVE_GUARD_MS);
}
#endif

#if TS_MODEL==TS_MODEL_XPT2046
  #ifdef TS_SPIPINS
    SPIClass  TSSPI(HSPI);
  #endif
  /* Локальная копия с патчем begin(): isrPinptr до attachInterrupt (без StoreProhibited на ESP32). */
  #include "../XPT2046_Touchscreen/XPT2046_Touchscreen.h"
  /* PEN/T_IRQ → WAKE_PIN (если задан): IRQ-фильтр касаний как раньше; sleep-wake отдельно. */
  #if WAKE_PIN != 255
    XPT2046_Touchscreen ts(TS_CS, WAKE_PIN);
  #else
    XPT2046_Touchscreen ts(TS_CS);
  #endif
  typedef TS_Point TSPoint;
#elif TS_MODEL==TS_MODEL_GT911
  #include "../GT911_Touchscreen/TAMC_GT911.h"
  TAMC_GT911 ts = TAMC_GT911(TS_SDA, TS_SCL, TS_INT, TS_RST, 0, 0);
  typedef TP_Point TSPoint;
#elif TS_MODEL==TS_MODEL_CST820
  #include "../CST820_Touchscreen/CST820.h"
  /* INT=255 в myoptions — не цепляем GPIO40 (там SD MISO после ремапа S3) */
  CST820 ts(TS_SDA, TS_SCL, TS_RST, TS_INT);
  static uint16_t s_cstRawX = 0, s_cstRawY = 0;
  static bool s_cstTouched = false;
  /* Последний вертикальный жест CST820 за текущее касание (SlideUp/Down). */
  static uint8_t s_cstGestLatched = 0;
#elif TS_MODEL==TS_MODEL_AXS15231B
  #include "../AXS15231B_touch/AXS15231B_Touch.h"
  /* Faster JC3248: I2C SDA/SCL; INT=3 = WAKE_PIN (wake — после полевого тача). */
  AXS15231B_Touch ts = AXS15231B_Touch(TS_SDA, TS_SCL, TS_INT, TS_RST, 0, 0);
  static uint16_t s_axsRawX = 0, s_axsRawY = 0;
  static bool s_axsTouched = false;
#endif

void TouchScreen::init(uint16_t w, uint16_t h){
  // Инициализация членов класса
  _oldTouchX = 0;
  _oldTouchY = 0;
  _touchdelay = 0;
  _touchTopBarStart = false;
  
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
  /* Подсказка по железу: ILI9488 часто не отпускает MISO — TDO тача и SDO TFT на одном GPIO конфликтуют */
  Serial.printf("##[BOOT]#\tXPT2046 CS=%d IRQ=%d (локальный lib+IRQ; общая SPI с TFT; тач мёртв — отключи TFT SDO от MISO)\n",
                TS_CS, (WAKE_PIN != 255) ? WAKE_PIN : -1);
#endif
#if TS_MODEL==TS_MODEL_GT911
  ts.begin();
#if DSP_MODEL==DSP_ST7701
  /* Guition 4848: setRotation(0)=LEFT меняет X↔Y → плейлист путается с громкостью.
   * INVERTED(1)=сырьё как UI; fliptouch → NORMAL(3) зеркало обеих осей. */
  ts.setRotation(config.store.fliptouch ? 3 : 1);
#else
  ts.setRotation(config.store.fliptouch?2:0);
#endif
#endif
#if TS_MODEL==TS_MODEL_CST820
  Serial.println("##[BOOT]#\tCST820 begin (I2C)");
  Serial.flush();
  ts.begin();
  Serial.println("##[BOOT]#\tCST820 ready");
  Serial.flush();
#endif
#if TS_MODEL==TS_MODEL_AXS15231B
  Serial.printf("##[BOOT]#\tAXS15231B touch begin I2C SDA=%d SCL=%d INT=%d\n",
                TS_SDA, TS_SCL, TS_INT);
  Serial.flush();
  ts.begin();
  Serial.println("##[BOOT]#\tAXS15231B touch ready");
  Serial.flush();
#endif
#endif
  _width  = w;
  _height = h;
#if TS_MODEL==TS_MODEL_GT911
  ts.setResolution(_width, _height);
#endif
#if TS_MODEL==TS_MODEL_AXS15231B
  /* Логическое UI 480×320; сырьё драйвера остаётся native 320×480. */
  ts.setResolution(_width, _height);
#endif
}

void TouchScreen::prepareDeepSleepWake() {
#if TS_MODEL==TS_MODEL_CST820
  /* I2C к CST820 ещё жив (дисплей только в SPI-sleep) — включаем INT и фиксируем RST. */
  ts.prepareDeepSleepWake();
#endif
}

void TouchScreen::prepareLightSleepWake() {
#if TS_MODEL==TS_MODEL_CST820
  /* Light sleep: чип остаётся доступен по I2C, авто-Standby запрещён. */
  ts.prepareLightSleepWake();
#endif
}

bool TouchScreen::pollFingerPresent() {
#if TS_MODEL==TS_MODEL_CST820
  return ts.fingerPresent();
#else
  return false;
#endif
}

/* ============================================================================
 * Диагностика линии INT тача → WAKE_PIN (пробуждение из deep sleep).
 *
 * Зачем: одиночный digitalRead() в момент касания ненадёжен — poll тача идёт раз
 * в 20 мс, а импульс INT шириной ~10 мс, поэтому его легко «проспать». Здесь линия
 * слушается аппаратным прерыванием, поэтому не теряется ни один фронт.
 *
 * Что проверяется:
 *   1) есть ли вообще провод (по поведению пина с внутренней подтяжкой вниз);
 *   2) чем INT держит линию в покое — сам (push-pull) или внешней подтяжкой;
 *   3) идут ли импульсы, когда чип уже в Standby — ровно то состояние, в котором
 *      он находится, пока ESP спит.
 * ========================================================================== */
#if (TS_MODEL==TS_MODEL_CST820) && (WAKE_PIN != 255)
static volatile uint32_t s_wakeIntFalling = 0;  /* Счётчик спадов (INT активен низким уровнем). */
static volatile uint32_t s_wakeIntRising  = 0;  /* Счётчик подъёмов — подтверждает окончание импульса. */
static bool     s_wakeTestActive = false;       /* Активен тест: poll тача по I2C приостановлен. */
static volatile bool s_wakeTestPending = false; /* Запрос на старт из другой задачи (веб-сервер/telnet). */
static uint32_t s_wakeTestUntil  = 0;           /* Момент автоматического завершения теста. */
static uint32_t s_wakeTestNextLog = 0;          /* Когда печатать следующий промежуточный отчёт. */

/* Обработчик прерывания: только инкремент счётчиков, ничего тяжёлого.
 * C++20: volatile++ deprecated → явное присваивание. */
static void IRAM_ATTR wakeIntIsr() {
  if (digitalRead(WAKE_PIN) == LOW) {
    s_wakeIntFalling = s_wakeIntFalling + 1u;
  } else {
    s_wakeIntRising = s_wakeIntRising + 1u;
  }
}
#endif

void TouchScreen::startWakeIntTest() {
#if (TS_MODEL==TS_MODEL_CST820) && (WAKE_PIN != 255)
  /* Команда приходит из задачи веб-сервера (или telnet/MQTT), а сам тест дёргает I2C
   * и ждёт секунды. Делать это здесь нельзя: заблокируем AsyncTCP и поймаем гонку на Wire
   * с опросом тача. Поэтому только поднимаем флаг — старт произойдёт в основном цикле. */
  if (s_wakeTestActive || s_wakeTestPending) {
    Serial.println("[INT-TEST] тест уже идёт, дождитесь окончания");
    return;
  }
  s_wakeTestPending = true;
  Serial.println("[INT-TEST] запрос принят, старт в основном цикле");
#else
  Serial.println("[INT-TEST] недоступно: нужен CST820 и заданный WAKE_PIN");
#endif
}

/* Собственно запуск теста. Вызывается только из основного цикла (контекст TouchScreen::loop). */
void TouchScreen::_wakeIntTestBegin() {
#if (TS_MODEL==TS_MODEL_CST820) && (WAKE_PIN != 255)
  Serial.println("[INT-TEST] ===== старт теста линии INT -> WAKE_PIN =====");

  /* --- Шаг 1: определяем, подключён ли провод, и кто держит линию в покое ---
   * Внутренняя подтяжка ESP32-S3 ≈ 45 кОм. Если INT активно выдаёт HIGH (push-pull),
   * то даже против внутренней подтяжки ВНИЗ на пине останется высокий уровень.
   * Если же провода нет — подтяжка вниз победит и мы прочитаем 0. */
  pinMode(WAKE_PIN, INPUT_PULLDOWN);
  delay(20);
  const int levelPd = digitalRead(WAKE_PIN);
  pinMode(WAKE_PIN, INPUT_PULLUP);
  delay(20);
  const int levelPu = digitalRead(WAKE_PIN);
  pinMode(WAKE_PIN, INPUT);
  delay(20);
  const int levelFloat = digitalRead(WAKE_PIN);
  Serial.printf("[INT-TEST] GPIO%d уровни: pulldown=%d pullup=%d без_подтяжки=%d\n",
                WAKE_PIN, levelPd, levelPu, levelFloat);
  if (levelPd == 1) {
    Serial.println("[INT-TEST] линия активно держится в HIGH => провод есть, INT push-pull. "
                   "Внутренняя подтяжка в deep sleep НЕ нужна (она даёт делитель на резисторе 10к).");
  } else {
    Serial.println("[INT-TEST] подтяжка вниз пересилила => либо провода нет, либо INT с открытым "
                   "коллектором. Смотрим счётчики импульсов ниже.");
  }

  /* --- Шаг 2: переводим чип в то же состояние, что и во время сна ESP ---
   * prepareDeepSleepWake() разрешает авто-Standby и внутри ждёт ~2.5 с,
   * чтобы чип успел туда уйти. */
  ts.prepareDeepSleepWake();

  /* --- Шаг 3: слушаем линию прерыванием (без подтяжки, если её держит сам INT) --- */
  s_wakeIntFalling = 0;
  s_wakeIntRising = 0;
  pinMode(WAKE_PIN, (levelPd == 1) ? INPUT : INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WAKE_PIN), wakeIntIsr, CHANGE);

  s_wakeTestActive = true;
  s_wakeTestUntil = millis() + 20000UL;  /* 20 секунд на касания */
  s_wakeTestNextLog = millis() + 1000UL;
  Serial.println("[INT-TEST] 20 секунд: КАСАЙТЕСЬ экрана. Тач сейчас не реагирует — это нормально.");
  Serial.flush();
#endif
}

void TouchScreen::_wakeIntTestService() {
#if (TS_MODEL==TS_MODEL_CST820) && (WAKE_PIN != 255)
  if (!s_wakeTestActive) return;

  const uint32_t now = millis();

  /* Раз в секунду показываем накопленные фронты — видно момент касания. */
  if ((int32_t)(now - s_wakeTestNextLog) >= 0) {
    s_wakeTestNextLog = now + 1000UL;
    /* Остаток считаем со защитой от «отрицательного» значения в беззнаковой арифметике. */
    const uint32_t leftMs = ((int32_t)(s_wakeTestUntil - now) > 0) ? (s_wakeTestUntil - now) : 0;
    Serial.printf("[INT-TEST] осталось %lu с: спадов=%lu подъёмов=%lu уровень=%d\n",
                  (unsigned long)(leftMs / 1000UL),
                  (unsigned long)s_wakeIntFalling,
                  (unsigned long)s_wakeIntRising,
                  digitalRead(WAKE_PIN));
  }

  if ((int32_t)(now - s_wakeTestUntil) < 0) return;

  /* --- Итог и возврат тача в рабочий режим --- */
  detachInterrupt(digitalPinToInterrupt(WAKE_PIN));
  const uint32_t falling = s_wakeIntFalling;
  Serial.printf("[INT-TEST] ===== итог: спадов=%lu подъёмов=%lu =====\n",
                (unsigned long)falling, (unsigned long)s_wakeIntRising);
  if (falling > 0) {
    Serial.println("[INT-TEST] ВЕРДИКТ: импульсы INT доходят до пина в режиме Standby. "
                   "Линия рабочая — значит мешает уровень: убрать внутреннюю подтяжку "
                   "и/или уменьшить последовательный резистор 10к.");
  } else {
    Serial.println("[INT-TEST] ВЕРДИКТ: ни одного фронта. До пина импульс не доходит — "
                   "проверить провод/резистор и точку пайки INT на плате.");
  }
  Serial.flush();

  ts.resumeFromStandby();
  s_wakeTestActive = false;
#endif
}

tsDirection_e TouchScreen::_tsDirection(uint16_t x, uint16_t y) {
  int16_t dX = x - _oldTouchX;
  int16_t dY = y - _oldTouchY;
  if (abs(dX) > TS_SWIPE_THRESHOLD || abs(dY) > TS_SWIPE_THRESHOLD) {
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

bool TouchScreen::_inTopBar(uint16_t touchY) const {
  /* CST820 / NV3041A+GT911 / AXS Faster: после map «верх» UI (название станции) — у большого Y. */
#if ((DSP_MODEL==DSP_NV3041A) && (TS_MODEL==TS_MODEL_GT911)) || (TS_MODEL==TS_MODEL_CST820) || (TS_MODEL==TS_MODEL_AXS15231B)
  return touchY >= (uint16_t)(_height - TS_TOP_BAR_Y_MAX);
#else
  return touchY < TS_TOP_BAR_Y_MAX;
#endif
}

void TouchScreen::flip(){
#if (DSP_MODEL==DSP_NV3041A) && (TS_MODEL==TS_MODEL_XPT2046)
  ts.setRotation(config.store.fliptouch ? 4 : 2);
#else
#if TS_MODEL==TS_MODEL_XPT2046
  ts.setRotation(config.store.fliptouch?3:1);
#endif
#if TS_MODEL==TS_MODEL_GT911
#if DSP_MODEL==DSP_ST7701
  ts.setRotation(config.store.fliptouch ? 3 : 1);
#else
  ts.setRotation(config.store.fliptouch?2:0);
#endif
#endif
#endif
}

void TouchScreen::loop(){
  uint16_t touchX, touchY;
  static bool wastouched = false;  // Изменено на false для правильной первоначальной логики
  static uint32_t touchLongPress;
  static tsDirection_e direct;
  static uint16_t touchVol, touchStation;
  /* Последняя точка жеста (на END TOUCH координат уже нет). */
  static uint16_t s_lastTouchX = 0, s_lastTouchY = 0;
  /* Был свайп VOL/плейлист в этом жесте — не считать отпускание тапом Play/Stop. */
  static bool s_gestureWasSwipe = false;
  /* В этом жесте уже открыли/ведём VOL — дальше мелкие шаги крутят громкость. */
  static bool s_volSwipeArmed = false;
  /* Вариант A: сколько подряд опросов L/R с dx≥порога — отсекает одиночный выброс/тык. */
  static uint8_t s_volHorizHits = 0;
  /* Вышли из SS в этом жесте (шум L/R) — до отпускания пальца не открывать PL. */
  static bool s_ssExitNavGuard = false;
  /* Старт пальца в слоте обложки Cover-SS — END не считать тапом-выходом из SS. */
  static bool s_coverSsSlotGesture = false;
  /* Палец вниз был на SS/blank — этот жест НИКОГДА не открывает PL (даже после leave→PLAYER). */
  static bool s_gestureStartedOnSs = false;
  if (!_checklpdelay(20, _touchdelay)) return;
#if (TS_MODEL==TS_MODEL_CST820) && (WAKE_PIN != 255)
  /* Отложенный старт теста линии INT: команда пришла из другой задачи, выполняем здесь. */
  if (s_wakeTestPending) {
    s_wakeTestPending = false;
    _wakeIntTestBegin();
    return;
  }
  /* Во время теста линии INT опрос по I2C приостановлен: иначе чип не уйдёт в Standby
   * и мы проверим не то состояние, в котором он находится во время сна ESP. */
  if (s_wakeTestActive) {
    _wakeIntTestService();
    return;
  }
#endif
#if TS_MODEL==TS_MODEL_GT911
  ts.read();
#endif
#if TS_MODEL==TS_MODEL_CST820
  {
    uint8_t gest = 0;
    /* Poll I2C: панель отдаёт портрет 240×320; ниже маппим в landscape дисплея */
    s_cstTouched = ts.getTouch(&s_cstRawX, &s_cstRawY, &gest);
    /* SlideUp/Down латчим на весь жест — контроллер отдаёт код коротко. */
    if (gest == SlideUp || gest == SlideDown) {
      s_cstGestLatched = gest;
    }
  }
#endif
#if TS_MODEL==TS_MODEL_AXS15231B
  /* Poll I2C: native 320×480; ниже — в UI landscape 480×320 (Canvas rot 1/3). */
  ts.read();
  s_axsTouched = ts.isTouched;
  if (s_axsTouched) {
    s_axsRawX = ts.points[0].x;
    s_axsRawY = ts.points[0].y;
  }
#endif
  bool istouched = _istouched();

  // Подробный поток отладки касаний отключён по умолчанию.
  // При необходимости можно вернуть через config.store.dbgtouch.
  if(istouched){
    /* Пока палец на экране громкости — не даём таймеру закрыть VOL (как плейлист при скролле). */
    /* Пока палец на экране громкости — не даём таймеру закрыть VOL (как плейлист при скролле). */
    if (display.mode() == VOL) {
      display.cancelStickyLeave();
      timekeeper.waitAndReturnPlayer(3);
    }
    /* Плейлист: касание продлевает 4 с и снимает «вооружённый» выход на PLAYER. */
    if (display.mode() == STATIONS) {
      display.cancelStickyLeave();
      timekeeper.waitAndReturnPlayer(4);
    }
  #if TS_MODEL==TS_MODEL_XPT2046
    TSPoint p = ts.getPoint();
    /* map() не клипует: сырой ADC вне TS_* даёт отрицательные/заэкранные uint16. */
    {
      long mx = map((long)p.x, (long)TS_X_MIN, (long)TS_X_MAX, 0L, (long)(_width ? _width - 1 : 0));
      long my = map((long)p.y, (long)TS_Y_MIN, (long)TS_Y_MAX, 0L, (long)(_height ? _height - 1 : 0));
      if (mx < 0) mx = 0;
      if (my < 0) my = 0;
      if (_width && mx >= (long)_width) mx = _width - 1;
      if (_height && my >= (long)_height) my = _height - 1;
      touchX = (uint16_t)mx;
      touchY = (uint16_t)my;
#if (DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486) && !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT)
      /* ILI9488 landscape + XPT2046 (DevKit): сырой ADC Y противоположен rotation TFT.
       * Без зеркала тап «сверху» (meta) даёт большой touchY → переключение WEB/SD срабатывало снизу. */
      if (_height > 0) touchY = (uint16_t)(_height - 1u - touchY);
#endif
    }
  #elif TS_MODEL==TS_MODEL_GT911
    TSPoint p = ts.points[0];
    touchX = p.x;
    touchY = p.y;
    #if (DSP_MODEL==DSP_NV3041A)
      touchY = _height - touchY;
    #elif (DSP_MODEL==DSP_ST7701)
      /* rot INVERTED: громкость L/R OK; плейлист U/D были наоборот → зеркало Y.
       * При fliptouch драйвер уже зеркалит обе оси (rot NORMAL) — не дублируем. */
      if (!config.store.fliptouch && _height > 0)
        touchY = (uint16_t)(_height - 1u - touchY);
    #endif
  #elif TS_MODEL==TS_MODEL_CST820
    /* DSP_ST7789: setRotation(1) → 320×240. Сырой CST820 ≈ 240×320 портрет.
     * rotation1: Xdisp=Yraw, Ydisp=(H-1)-Xraw; fliptouch — зеркало. */
    if (config.store.fliptouch) {
      touchX = (_width > 0 && s_cstRawY < _width) ? (_width - 1 - s_cstRawY) : 0;
      touchY = s_cstRawX;
    } else {
      touchX = s_cstRawY;
      touchY = (_height > 0 && s_cstRawX < _height) ? (_height - 1 - s_cstRawX) : 0;
    }
    if (touchX >= _width) touchX = _width ? _width - 1 : 0;
    if (touchY >= _height) touchY = _height ? _height - 1 : 0;
  #elif TS_MODEL==TS_MODEL_AXS15231B
    /* AXS Canvas rot 1/3 → UI 480×320. Сырьё: X≤320, Y≤480.
     * X=rawY — громкость L/R совпала; Y без (H-1)-rawX — иначе плейлист и top-bar (SD) перевёрнуты. */
    if (config.store.fliptouch) {
      touchX = (_width > 0 && s_axsRawY < _width) ? (_width - 1 - s_axsRawY) : 0;
      touchY = (_height > 0 && s_axsRawX < _height) ? (_height - 1 - s_axsRawX) : 0;
    } else {
      touchX = s_axsRawY;
      touchY = s_axsRawX;
    }
    if (touchX >= _width) touchX = _width ? _width - 1 : 0;
    if (touchY >= _height) touchY = _height ? _height - 1 : 0;
  #endif
    /* Всегда обновляем «хвост» жеста — на END TOUCH координат уже нет. */
    s_lastTouchX = touchX;
    s_lastTouchY = touchY;
  if (!wastouched) { /*     START TOUCH     */
#if (TS_MODEL==TS_MODEL_CST820) && (WAKE_PIN != 255)
      /* Диагностика INT: I2C уже видит палец — есть ли LOW на GPIO пробуждения?
       * Если всегда 1 при касании — линия IRQ на модуле не дергается (не регистры). */
      {
        static uint8_t s_wakePinProbeLeft = 8;
        if (s_wakePinProbeLeft > 0) {
          s_wakePinProbeLeft--;
          pinMode(WAKE_PIN, INPUT);
          Serial.printf("[CST820] touch I2C ok, WAKE_PIN %d = %d (1=нет IRQ, 0=импульс INT)\n",
                        WAKE_PIN, digitalRead(WAKE_PIN));
        }
      }
#endif
      _oldTouchX = touchX;
      _oldTouchY = touchY;
      _touchTopBarStart = _inTopBar(touchY);
      touchVol = touchX;
      touchStation = touchY;
      direct = TDS_REQUEST;
      touchLongPress=millis();
      s_gestureWasSwipe = false;
      s_volSwipeArmed = false;
      s_volHorizHits = 0;
      s_ssExitNavGuard = false;
      s_coverSsSlotGesture = false;
      /* SS/blank ИЛИ хвост после leave (дроп istouched → END → новый BEGIN уже на PLAYER).
       * Решение на BEGIN — весь жест до отпускания; окно на END не сбрасываем. */
      {
        const bool onSs =
            (display.mode() == SCREENSAVER || display.mode() == SCREENBLANK);
        const bool postLeaveGuard =
            (s_tsSsLeaveMs != 0u) &&
            ((uint32_t)(millis() - s_tsSsLeaveMs) < TS_SS_LEAVE_PL_GUARD_MS);
        s_gestureStartedOnSs = onSs || postLeaveGuard;
      }
#if defined(COVER_FACT_SWIPE_NS)
      /* Cover-SS: начинаем трекинг свайпа вверх по обложке (пик хода). */
      COVER_FACT_SWIPE_NS::coverFactSwipeBegin(touchX, touchY);
      s_coverSsSlotGesture = COVER_FACT_SWIPE_NS::isCoverFactSwipeArmed();
#endif
    } else { /*     SWIPE TOUCH     */
#if defined(COVER_FACT_SWIPE_NS)
      /* Копим пик «вверх» до отпускания. */
      #if TS_MODEL==TS_MODEL_CST820
      COVER_FACT_SWIPE_NS::coverFactSwipeSample(touchX, touchY, s_cstGestLatched);
      #else
      COVER_FACT_SWIPE_NS::coverFactSwipeSample(touchX, touchY, 0);
      #endif
      /* Жест факта по обложке: не уводить SS→PLAYER и не открывать PL в том же касании. */
      const bool coverFactArmed = COVER_FACT_SWIPE_NS::isCoverFactSwipeArmed()
          || s_coverSsSlotGesture;
#else
      const bool coverFactArmed = false;
#endif
      direct = _tsDirection(touchX, touchY);
      if (!_touchTopBarStart) {
      switch (direct) {
        case TSD_LEFT:
        case TSD_RIGHT: {
            touchLongPress=millis();
            s_gestureWasSwipe = true;
            /* Cover-SS / жест с SS: не leave на полдороге (вертикаль часто сначала как L/R). */
            if (coverFactArmed) break;
#if defined(COVER_FACT_SWIPE_NS)
            if (s_gestureStartedOnSs
                && COVER_FACT_SWIPE_NS::isPlayingBlankCoverActive()) {
              break;
            }
#endif
            /* Свайп L/R из SS или PLAYER → VOL. Тап/дребезг (~20 px) не открывает:
             * нужен сдвиг ≥ TS_VOL_OPEN_MIN_PX; из SS при малом сдвиге — только PLAYER. */
            const bool fromSs =
                (display.mode() == SCREENSAVER || display.mode() == SCREENBLANK);
            if (fromSs || display.mode() == PLAYER || display.mode() == VOL) {
              if (fromSs) {
                tsLeaveScreensaverToPlayer();
                /* Тот же жест потом может дать U/D — не пускать в PL. */
                s_ssExitNavGuard = true;
              }
              const int16_t dxPx = (int16_t)abs((int)touchVol - (int)touchX);
              /* Уже в VOL — продолжаем без счётчика. Иначе два подряд опроса с dx≥порога
               * (~40 мс): один выброс AXS / тык с SS не открывает громкость. */
              if (!s_volSwipeArmed && display.mode() == VOL) {
                s_volSwipeArmed = true;
                s_volHorizHits = 0;
              } else if (!s_volSwipeArmed) {
                if (dxPx >= (int16_t)TS_VOL_OPEN_MIN_PX) {
                  if (s_volHorizHits < 255) s_volHorizHits++;
                  if (s_volHorizHits >= 2) s_volSwipeArmed = true;
                } else {
                  s_volHorizHits = 0;
                }
              }
#if USE_BT_I2S
              if (config.getMode() == PM_BT) {
                if (s_volSwipeArmed) {
                  int16_t xDelta = map(abs(touchVol - touchX), 0, _width, 0, TS_STEPS);
                  if (xDelta > 1) {
                    controlsEvent((touchVol - touchX) < 0);
                    touchVol = touchX;
                  }
                }
                break;
              }
#endif
              if (s_volSwipeArmed) {
                display.putRequest(NEWMODE, VOL);
                int16_t xDelta = map(abs(touchVol - touchX), 0, _width, 0, TS_STEPS);
                if (xDelta > 1) {
                  /* CST820 на JC2432: ось X зеркальна относительно громкости — инвертируем. */
                  #if TS_MODEL==TS_MODEL_CST820
                  controlsEvent((touchVol - touchX)>0);
                  #else
                  controlsEvent((touchVol - touchX)<0);
                  #endif
                  touchVol = touchX;
                }
              }
            }
            break;
          }
        case TSD_UP:
        case TSD_DOWN: {
            touchLongPress=millis();
            s_gestureWasSwipe = true;
            /* Вертикаль — сброс счётчика горизонтали (не копить hits по диагонали). */
            if (!s_volSwipeArmed) s_volHorizHits = 0;
            /* Жест начат на SS / cover-факт / только что leave — PL в этом касании запрещён. */
            if (coverFactArmed || s_ssExitNavGuard || s_gestureStartedOnSs) break;
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
            if (!s_volSwipeArmed) s_volHorizHits = 0;
            break;
      }
      }
    }
    if (config.store.dbgtouch) {
      Serial.print(", x = ");
#if TS_MODEL==TS_MODEL_CST820
      Serial.print(s_cstRawX);
      Serial.print("->");
      Serial.print(touchX);
      Serial.print(", y = ");
      Serial.print(s_cstRawY);
      Serial.print("->");
      Serial.println(touchY);
#elif TS_MODEL==TS_MODEL_AXS15231B
      Serial.print(s_axsRawX);
      Serial.print("->");
      Serial.print(touchX);
      Serial.print(", y = ");
      Serial.print(s_axsRawY);
      Serial.print("->");
      Serial.println(touchY);
#else
      Serial.print(p.x);
      Serial.print(", y = ");
      Serial.println(p.y);
#endif
    }
  }else{
    if (wastouched) {/*     END TOUCH     */
#if defined(COVER_FACT_SWIPE_NS)
      /* Cover-SS: свайп вверх по обложке → факт (тост до появления факта). */
      /* Досылаем последнюю точку: у XPT2046 на END уже нет getPoint(), только s_lastTouch*. */
      #if TS_MODEL==TS_MODEL_CST820
      COVER_FACT_SWIPE_NS::coverFactSwipeSample(s_lastTouchX, s_lastTouchY, s_cstGestLatched);
      #else
      COVER_FACT_SWIPE_NS::coverFactSwipeSample(s_lastTouchX, s_lastTouchY, 0);
      #endif
      const bool coverFactSwipeHandled =
          COVER_FACT_SWIPE_NS::coverFactSwipeEnd(s_lastTouchX, s_lastTouchY);
      if (coverFactSwipeHandled) {
        /* Факт принят — блокируем ложный тап-leave от второго куска AXS. */
        s_tsCoverFactHandledMs = millis();
        direct = TSD_STAY;
      }
#else
      const bool coverFactSwipeHandled = false;
#endif
#if TS_MODEL==TS_MODEL_CST820
      s_cstGestLatched = 0; /* жест закончен — сброс аппаратного флага */
#endif
      if (!coverFactSwipeHandled) {
        uint32_t pressTicks = millis()-touchLongPress;
          if (s_coverSsSlotGesture) {
          /* Обложка Cover-SS: чистый тап — выход; недосвайп / хвост после факта — остаёмся. */
          if (!s_gestureWasSwipe && pressTicks > 50 && pressTicks < BTN_PRESS_TICKS * 2) {
#if defined(COVER_FACT_SWIPE_NS)
            if (tsCoverFactLeaveBlocked()) {
              /* окно после handled — не leave */
            } else if (COVER_FACT_SWIPE_NS::coverFactSwipeLastPeakAbs() >
                       (int16_t)TS_COVER_SLOT_TAP_MAX_PX) {
              /* Был ход пальца, но порог факта не взят — не считать тапом. */
            } else
#endif
            {
              tsLeaveScreensaverToPlayer();
            }
          }
        } else if (pressTicks < BTN_PRESS_TICKS*2){
          if(pressTicks > 50) {
            /* Конец свайпа часто даёт TDS_REQUEST → иначе toggle() гасит поток. */
            if (!s_gestureWasSwipe) {
#ifdef USE_SD
            if (_touchTopBarStart && (display.mode() == SCREENSAVER || display.mode() == SCREENBLANK)) {
              tsLeaveScreensaverToPlayer();
            } else if (_touchTopBarStart && display.mode() == PLAYER) {
              /* WEB↔SD↔BT только с экрана плеера — в плейлисте top-bar не меняет режим. */
              scheduleChangeModeTask(config.nextUserModeCommand());
            } else if (direct == TDS_REQUEST)
#else
            if (direct == TDS_REQUEST)
#endif
            {
              /* Плейлист: Play только тапом по выделенной строке (2b). */
              if (display.mode() == STATIONS) {
                if (display.playlistFocusHit(s_lastTouchY))
                  onBtnClick(EVT_BTNCENTER);
              } else {
                onBtnClick(EVT_BTNCENTER);
              }
            }
            }
          }
        }else if (!s_gestureWasSwipe){
          /* Долгий тап: из SS/blank — на PLAYER; иначе плейлист↔плеер. */
          if (display.mode() == SCREENSAVER || display.mode() == SCREENBLANK) {
            tsLeaveScreensaverToPlayer();
          } else {
            /* ТЕСТ 2026-10-01: long-press STATIONS↔PLAYER временно выкл —
             * проверить sticky без выхода «держал палец ≥1 с». Вернуть после поля. */
            // display.armStickyLeave();
            // display.putRequest(NEWMODE, display.mode() == PLAYER ? STATIONS : PLAYER);
          }
        }
      }
      s_gestureWasSwipe = false;
      s_volSwipeArmed = false;
      s_volHorizHits = 0;
      s_ssExitNavGuard = false;
      s_coverSsSlotGesture = false;
      s_gestureStartedOnSs = false;
      _touchTopBarStart = false;
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
#elif TS_MODEL==TS_MODEL_CST820
  return s_cstTouched;
#elif TS_MODEL==TS_MODEL_AXS15231B
  return s_axsTouched;
#endif
}

#endif  // TS_MODEL!=TS_MODEL_UNDEFINED
