/*************************************************************************************
    NV3041A 480x272 displays configuration file.
    Copy this file to yoRadio/src/displays/conf/displayNV3041Aconf_custom.h
    and modify it
    More info on https://github.com/e2002/yoradio/wiki/Widgets#widgets-description
*************************************************************************************/

#ifndef displayNV3041Aconf_h
#define displayNV3041Aconf_h

#include "../widgets/widgets.h"
#include "../../../myoptions.h"

/* Прокрутка бегущей строки: SCROLL_* из myoptions.h (дефолты — в options.h).
 * Только для NV3041A; другие TFT — свои display*conf.h. */
#ifndef SCROLL_START_DELAY
#define SCROLL_START_DELAY 5000
#endif
#ifndef SCROLL_STEP
#define SCROLL_STEP 5
#endif
#ifndef SCROLL_TIME
#define SCROLL_TIME 40
#endif
#ifndef SCROLL_PLAYLIST_START_DELAY
#define SCROLL_PLAYLIST_START_DELAY 1000
#endif
#ifndef SCROLL_WEATHER_STEP
#define SCROLL_WEATHER_STEP 2
#endif
#ifndef SCROLL_WEATHER_TIME
#define SCROLL_WEATHER_TIME 5
#endif

#define DSP_WIDTH       272
#define DSP_HEIGHT      480
//#define TFT_FRAMEWDT    5
#define TFT_FRAMEWDT    1

#define MAX_WIDTH       DSP_WIDTH-TFT_FRAMEWDT

#if BITRATE_FULL
  #define TITLE_FIX 44
#else
  #define TITLE_FIX 0
#endif
#if BOOTLOGO_TYPE == 0
  #define bootLogoTop 110
#else
  #define bootLogoTop 0
#endif

#ifndef BATTERY_OFF
  #define BatX      122			// X coordinate for batt. ( X  )
  #define BatY      DSP_HEIGHT -76		// Y cordinate for batt. ( Y  )
  #define BatFS     2	// FontSize for batt. (   )
  #define ProcX     190		// X coordinate for percent ( X   )
  #define ProcY     DSP_HEIGHT-76	// Y coordinate for percent ( Y   )
  #define ProcFS    2		// FontSize for percent (    )
  #define VoltX      TFT_FRAMEWDT	+10		// X coordinate for voltage ( X  )
  #define VoltY      DSP_HEIGHT-76		// Y coordinate for voltage ( Y  )
  #define VoltFS     2		// FontSize for voltage (   )
#endif

// === CLOCK OFFSETS (смещения для элементов часов). так быстрее настраивать чем цифирки подбирать в нескольких строках ===
#define CLOCK_TIME_Y_OFFSET      62   // Смещение Y для времени (основные часы)
#define CLOCK_SECONDS_X_OFFSET   0   // Смещение X для секунд
#define CLOCK_SECONDS_Y_OFFSET   115   // Смещение Y для секунд
#define CLOCK_DOW_X_OFFSET        0   // Смещение X для дня недели
#define CLOCK_DOW_Y_OFFSET       77   // Смещение Y для дня недели
#define CLOCK_DATE_X_OFFSET     0  // Смещение X для даты 32
#define CLOCK_DATE_Y_OFFSET      77   // Смещение Y для даты
#define CLOCK_DOTS_X_OFFSET       1   // Смещение X для двоеточия
#define CLOCK_DOTS_Y_OFFSET      75   // Смещение Y для двоеточия
#define CLOCK_LINE_V_X_OFFSET    10   // Смещение X для вертикальной линии
#define CLOCK_LINE_V_Y_OFFSET    -2   // Смещение Y для вертикальной линии
#define CLOCK_LINE_H_X_OFFSET    10   // Смещение X для горизонтальной линии
#define CLOCK_LINE_H_Y_OFFSET    32   // Смещение Y для горизонтальной линии
#define CLOCK_LINE_H_WIDTH       62   // Длина горизонтальной линии

// === CLOCK CLEAR AREA EXTRA (запас для очистки области часов) ===
// Минимальные значения для теста: прямоугольник будет ближе к размеру самих часов
#define CLOCK_CLEAR_EXTRA_W  0   // Минимальный запас по ширине (рекомендуется 4-8 для теста)
#define CLOCK_CLEAR_EXTRA_H  0  // Минимальный запас по высоте (рекомендуется 2-6 для теста)

/* Скринсейвер: иконка погоды + город под «прыгающими» часами (отдельные константы, clockConf не трогаем). */
#define NV3041_SS_WICON_W           72   /* крупнее 48 — лучше читается на 480px высоте */
#define NV3041_SS_WICON_H           72
#define NV3041_SS_WICON_GAP_BELOW   4    /* зазор сразу под цифрами часов */
#define NV3041_SS_CLOCK_DESCENT     14   /* descent шрифта DirectiveFour56 от baseline */
#define NV3041_SS_WICON_CITY_GAP    6
#define NV3041_SS_WICON_CITY_H      20   /* город, setTextSize(2) */
#define NV3041_SS_WICON_BLOCK_H     (NV3041_SS_WICON_H + NV3041_SS_WICON_CITY_GAP + NV3041_SS_WICON_CITY_H)

/* SROLLS  */                            /* {{ left, top, fontsize, align }, buffsize, uppercase, width, scrolldelay, scrolldelta, scrolltime } */
const ScrollConfig metaConf       PROGMEM = {{ TFT_FRAMEWDT, 9, 3, WA_CENTER}, 140, true, MAX_WIDTH, SCROLL_START_DELAY, SCROLL_STEP, SCROLL_TIME };
const ScrollConfig title1Conf     PROGMEM = {{ TFT_FRAMEWDT, 47, 2, WA_LEFT }, 210, true, 209, SCROLL_START_DELAY, SCROLL_STEP, SCROLL_TIME };
//const ScrollConfig title2Conf     PROGMEM = {{ TFT_FRAMEWDT, 68, 2, WA_LEFT }, 140, false, MAX_WIDTH -10, 5000, 7, 40 };
const ScrollConfig title2Conf     PROGMEM = {{ TFT_FRAMEWDT, 68, 2, WA_LEFT }, 140, false, 209, SCROLL_START_DELAY, SCROLL_STEP, SCROLL_TIME };
const ScrollConfig playlistConf   PROGMEM = {{ TFT_FRAMEWDT, 146, 2, WA_LEFT }, 140, false, MAX_WIDTH, SCROLL_PLAYLIST_START_DELAY, SCROLL_STEP, SCROLL_TIME };
const ScrollConfig apTitleConf    PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 4, WA_CENTER }, 140, false, MAX_WIDTH, 0, SCROLL_STEP, SCROLL_TIME };
const ScrollConfig apSettConf     PROGMEM = {{ TFT_FRAMEWDT, 320-TFT_FRAMEWDT-16, 2, WA_LEFT }, 140, false, MAX_WIDTH, 0, SCROLL_STEP, SCROLL_TIME };
const ScrollConfig weatherConf    PROGMEM = {{ TFT_FRAMEWDT, 94, 2, WA_LEFT }, 240, false, MAX_WIDTH, 0, SCROLL_WEATHER_STEP, SCROLL_WEATHER_TIME };

/* BACKGROUNDS  */                       /* {{ left, top, fontsize, align }, width, height, outlined } */
const FillConfig   metaBGConf     PROGMEM = {{ 0, 0, 0, WA_LEFT }, DSP_WIDTH, 40, false };
const FillConfig   metaBGConfInv  PROGMEM = {{ 0, 38, 0, WA_LEFT }, DSP_WIDTH, 2, false };
const FillConfig   volbarConf     PROGMEM = {{ TFT_FRAMEWDT+70, DSP_HEIGHT-TFT_FRAMEWDT-14, 0, WA_LEFT }, MAX_WIDTH-76, 8, true };
const FillConfig  playlBGConf     PROGMEM = {{ TFT_FRAMEWDT, 138, 0, WA_LEFT }, DSP_WIDTH, 36, false };
const FillConfig  heapbarConf     PROGMEM = {{ TFT_FRAMEWDT, DSP_HEIGHT-2, 0, WA_LEFT }, DSP_WIDTH-36, 2, false };

/* WIDGETS  */                           /* { left, top, fontsize, align } */
const WidgetConfig bootstrConf    PROGMEM = { 0, 210, 1, WA_CENTER };
//const WidgetConfig bootstrConf    PROGMEM = { 0, 210, 2, WA_CENTER };
const WidgetConfig bitrateConf    PROGMEM = { 6, 110, 2, WA_LEFT };
const WidgetConfig voltxtConf     PROGMEM = { TFT_FRAMEWDT, DSP_HEIGHT-19, 2, WA_LEFT };
//const WidgetConfig  iptxtConf     PROGMEM = { TFT_FRAMEWDT, DSP_HEIGHT-38, 2, WA_LEFT };
const WidgetConfig  iptxtConf     PROGMEM = { TFT_FRAMEWDT, DSP_HEIGHT-36, 2, WA_CENTER };
//const WidgetConfig   rssiConf     PROGMEM = { TFT_FRAMEWDT, DSP_HEIGHT-48, 3, WA_RIGHT };
const WidgetConfig   rssiConf     PROGMEM = { TFT_FRAMEWDT, DSP_HEIGHT-42, 3, WA_RIGHT };
const WidgetConfig numConf        PROGMEM = { 0, 170, 70, WA_CENTER };
const WidgetConfig apNameConf     PROGMEM = { TFT_FRAMEWDT, 88, 3, WA_CENTER };
const WidgetConfig apName2Conf    PROGMEM = { TFT_FRAMEWDT, 120, 3, WA_CENTER };
const WidgetConfig apPassConf     PROGMEM = { TFT_FRAMEWDT, 173, 3, WA_CENTER };
const WidgetConfig apPass2Conf    PROGMEM = { TFT_FRAMEWDT, 205, 3, WA_CENTER };
const WidgetConfig  clockConf     PROGMEM = { 0, 116, 70, WA_RIGHT };  /* 52 is a fixed font size. do not change */
const WidgetConfig vuConf         PROGMEM = { TFT_FRAMEWDT+32, 409, 1, WA_LEFT }; /* Возвращено к 408 */
const WidgetConfig bootWdtConf    PROGMEM = { 0, 180, 1, WA_CENTER };
const ProgressConfig bootPrgConf  PROGMEM = { 90, 14, 4 };
//const BitrateConfig fullbitrateConf PROGMEM = {{10, 300, 2, WA_LEFT}, 52 };
const BitrateConfig fullbitrateConf PROGMEM = {{220, 39, 2, WA_RIGHT}, 52 };

/* BANDS  */                             /* { onebandwidth, onebandheight, bandsHspace, bandsVspace, numofbands, fadespeed } */
//const VUBandsConfig bandsConf     PROGMEM = { 40, 96, 6, 2, 8, 10 };
const VUBandsConfig bandsConf     PROGMEM = { 232, 10, 6, 1, 11, 5 };
/* STRINGS  */
const char         numtxtFmt[]    PROGMEM = "%d";
//const char           rssiFmt[]    PROGMEM = "WiFi %d";
const char           rssiFmt[]    PROGMEM = "WiFi %d";

const char          iptxtFmt[]    PROGMEM = "%s";
const char         voltxtFmt[]    PROGMEM = "\023\025%d";
//const char         voltxtFmt[]    PROGMEM = "VOL %d";

const char        bitrateFmt[]    PROGMEM = "%d kBs";

/* MOVES  */                             /* { left, top, width } */
//const MoveConfig    clockMove     PROGMEM = { 48, 116, -1 /* MAX_WIDTH */ }; // -1 disables move
const MoveConfig    clockMove     PROGMEM = { 0, 116, 0 /* MAX_WIDTH */ };
const MoveConfig   weatherMove    PROGMEM = { 10, 92, -1};
const MoveConfig   weatherMoveVU  PROGMEM = { 10, 92, -1};



#endif