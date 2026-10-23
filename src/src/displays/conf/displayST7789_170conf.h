/*************************************************************************************
    ST7789 320x170 — макет из yoRadio (sv_09434_2).
    Координаты BatX/BatY/ProcX/… подключаются только если в myoptions.h
    не задан BATTERY_OFF (см. блок «ST7789_170» в myoptions.h).
*************************************************************************************/

#ifndef displayST7789conf_h
#define displayST7789conf_h

#include "../../../myoptions.h"

/* Прокрутка: SCROLL_* из myoptions.h (дефолты ST7789_170 — 4/30 в options.h). */

#define DSP_WIDTH       320
#define DSP_HEIGHT      170
#define TFT_FRAMEWDT    1
#define MAX_WIDTH       DSP_WIDTH-TFT_FRAMEWDT*1

#if BITRATE_FULL
  #define TITLE_FIX 39
#else
  #define TITLE_FIX 0
#endif

#define bootLogoTop     40

// Смещение часов -10 (yoRadio): в PROGMEM только uint16_t — храним как (uint16_t)(int16_t)-10.
#define ST170_CLOCK_LEFT  ((uint16_t)(int16_t)-10)
/* DS_DIGI56: baseline clockTop, верх цифр ≈ top-69 (glyph yOffset в DS_DIGI56pt7b.h) */
#define ST170_CLOCK_GLYPH_ABOVE  69
#define ST170_CLOCK_MIN_TOP      72
#define ST170_CLOCK_MAX_TOP      165
/* Высота зоны очистки часов — не ниже Y=148 (строка IP) */
#define ST170_CLOCK_CLEAR_H      (ST170_CLOCK_GLYPH_ABOVE + 6)

/* Playing+blank: квадратная обложка слева 170×170, часы справа (DS_DIGI42). */
#define ST170_COVER_SIZE         170
#define ST170_COVER_X_LEFT       0
#define ST170_COVER_Y            0
#define ST170_FB_LEFT_W          ST170_COVER_SIZE
#define ST170_FB_RIGHT_X         ST170_COVER_SIZE
#define ST170_FB_RIGHT_W         (DSP_WIDTH - ST170_COVER_SIZE)
/* Playing+blank: часы справа — DS_DIGI28 mono (компактнее 42pt, ровные цифры) */
#define ST170_SIDE_CLK_GLYPH_ABOVE  34
#define ST170_SIDE_CLK_MIN_TOP      50
/* Верхняя граница прыжка часов: ниже 165, чтобы под baseline помещалась иконка погоды 36×36. */
#define ST170_SIDE_CLK_MAX_TOP      118
#define ST170_SIDE_CLK_CLEAR_H      (ST170_SIDE_CLK_GLYPH_ABOVE + 14)
/* Иконка погоды (только cover-скринсейвер): под строкой HH:MM, без текста. */
#define ST170_WICON_W               36
#define ST170_WICON_H               36
#define ST170_WICON_GAP_BELOW       4
#define ST170_WICON_DESCENT         12
/* Симметричные поля в правой колонке — иначе часы прижимаются к обложке */
#define ST170_SIDE_CLK_MARGIN_R     12
#define ST170_SIDE_CLK_MARGIN_L     12
#define ST170_SIDE_SS_JUMP_MS       20000UL
#define ST170_COVER_RETRY_MS        5000UL

#define HIDE_DATE
#define RSSI_DIGIT false
/* При VU погода на Y=147 — узкая заливка, IP (Y=148) остаётся виден под текстом */
#define ST170_WEATHER_OVER_IP
#define ST170_WEATHER_IP_ROW  140
#define ST170_IP_ALWAYS_VISIBLE
//#define HIDE_IP
//#define HIDE_TITLE2
//#define HIDE_HEAPBAR
//#define HIDE_VU

#ifndef BATTERY_OFF
  #define BatX      276
  #define BatY      150
  #define BatFS     2
  #define ProcX     274
  #define ProcY     133
  #define ProcFS    2
  #define VoltX     147
  #define VoltY     146
  #define VoltFS    1
#endif

/* SROLLS */
const ScrollConfig metaConf    PROGMEM = { { TFT_FRAMEWDT + 1, 2, 3, WA_CENTER }, 140, false, DSP_WIDTH, SCROLL_START_DELAY, SCROLL_STEP, SCROLL_TIME };
const ScrollConfig title1Conf     PROGMEM = { { TFT_FRAMEWDT, 32, 2, WA_LEFT }, 140, false, DSP_WIDTH - TITLE_FIX - 3, SCROLL_START_DELAY, SCROLL_STEP, SCROLL_TIME };
const ScrollConfig title2Conf     PROGMEM = { { TFT_FRAMEWDT, 51, 2, WA_LEFT }, 140, false, DSP_WIDTH - TITLE_FIX - 3, SCROLL_START_DELAY, SCROLL_STEP, SCROLL_TIME };
/* uppercase=false — названия станций в плейлисте как в playlist.csv, без принудительного CAPS */
const ScrollConfig playlistConf   PROGMEM = { { TFT_FRAMEWDT, 112, 2, WA_LEFT }, 140, false, MAX_WIDTH, SCROLL_PLAYLIST_START_DELAY, SCROLL_STEP, SCROLL_TIME };
const ScrollConfig apTitleConf   PROGMEM = { { TFT_FRAMEWDT, TFT_FRAMEWDT, 3, WA_CENTER }, 140, false, MAX_WIDTH, 0, SCROLL_STEP, SCROLL_AP_TIME };
const ScrollConfig apSettConf    PROGMEM = { { TFT_FRAMEWDT, 170 - 20, 1, WA_LEFT }, 140, false, MAX_WIDTH, 0, SCROLL_STEP, SCROLL_TIME };
/* Погода как в yoRadio (Y=51); при VU — weatherMove на 147, рядом с IP */
const ScrollConfig weatherConf  PROGMEM = { { TFT_FRAMEWDT, 51, 2, WA_LEFT }, 300, false, MAX_WIDTH, 0, SCROLL_STEP, SCROLL_TIME };

/* BACKGROUNDS */
const FillConfig  metaBGConf       PROGMEM = { { 0, 0, 0, WA_LEFT }, DSP_WIDTH, 0, false };
const FillConfig  metaBGConfInv  PROGMEM = { { 0, 0, 0, WA_LEFT }, DSP_WIDTH, 0, false };
const FillConfig  volbarConf         PROGMEM = { { 0, 156, 0, WA_LEFT }, 273, 0, false };
const FillConfig  playlBGConf      PROGMEM = { { 0, 107, 0, WA_LEFT }, DSP_WIDTH, 24, false };
/* Полоса буфера: yoRadio (Y=165, h=4) — хорошо видна; цвет по % в display.cpp */
const FillConfig  heapbarConf     PROGMEM = { { 0, 165, 0, WA_RIGHT }, 273, 4, false };

/* WIDGETS */
/* Boot: «Соединяюсь с…» чуть выше, чтобы не слипалось со строкой версии под ней */
const WidgetConfig bootstrConf    PROGMEM = { 0, 146, 1, WA_CENTER };
const WidgetConfig bitrateConf    PROGMEM = { TFT_FRAMEWDT, 32,  2, WA_RIGHT };
const WidgetConfig voltxtConf     PROGMEM = { TFT_FRAMEWDT, 214,  0, WA_CENTER };
const WidgetConfig  iptxtConf     PROGMEM = { 50, 148, 1, WA_CENTER };
const WidgetConfig   rssiConf     PROGMEM = { 1, 68, 3, WA_RIGHT };
const WidgetConfig numConf        PROGMEM = { 0, 120, 72, WA_CENTER };
const WidgetConfig apNameConf     PROGMEM = { TFT_FRAMEWDT, 40, 2, WA_CENTER };
const WidgetConfig apName2Conf    PROGMEM = { TFT_FRAMEWDT, 65, 2, WA_CENTER };
const WidgetConfig apPassConf     PROGMEM = { TFT_FRAMEWDT, 100, 2, WA_CENTER };
const WidgetConfig apPass2Conf    PROGMEM = { TFT_FRAMEWDT, 125, 2, WA_CENTER };
const WidgetConfig  clockConf     PROGMEM = { ST170_CLOCK_LEFT, 141, 52, WA_RIGHT };
const WidgetConfig vuConf         PROGMEM = { 3, 81, 1, WA_LEFT };
const WidgetConfig bootWdtConf    PROGMEM = { TFT_FRAMEWDT, 120, 2, WA_CENTER };

const ProgressConfig  bootPrgConf  PROGMEM = { 100, 14, 4 };
const BitrateConfig fullbitrateConf PROGMEM = { {DSP_WIDTH - TITLE_FIX - 1, 29, 2, WA_CENTER}, TITLE_FIX };

/* BANDS */
const VUBandsConfig bandsConf   	PROGMEM = { 11, 65, 5, 2, 12, 3 };

/* STRINGS */
const char         numtxtFmt[]    PROGMEM = "%d";
const char           rssiFmt[]    PROGMEM = "rssi %d dBm";
const char          iptxtFmt[]    PROGMEM = "%s";
const char         voltxtFmt[]    PROGMEM = "";
const char        bitrateFmt[]    PROGMEM = "%d kBs";

/* MOVES */
const MoveConfig    clockMove     PROGMEM = { ST170_CLOCK_LEFT, 141, 0 };
const MoveConfig   weatherMove    PROGMEM = { TFT_FRAMEWDT, 147, 0 };
const MoveConfig   weatherMoveVU  PROGMEM = { TFT_FRAMEWDT, 147, 0 };
#endif
