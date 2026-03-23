/*************************************************************************************
    ILI9488/ILI9486 320x480 (portrait) configuration file.
    Enable by defining: ILI9488_PORTRAIT true (e.g. in myoptions.h)
*************************************************************************************/

#ifndef displayILI9488conf_portrait_h
#define displayILI9488conf_portrait_h

#define DSP_WIDTH       320
#define DSP_HEIGHT      480
#define TFT_FRAMEWDT    10
#define MAX_WIDTH       DSP_WIDTH-TFT_FRAMEWDT*2

#if BITRATE_FULL
  #define TITLE_FIX 44
#else
  #define TITLE_FIX 0
#endif

// Центрируем логотип по высоте для 480px
#define bootLogoTop     260

/* SROLLS  */                            /* {{ left, top, fontsize, align }, buffsize, uppercase, width, scrolldelay, scrolldelta, scrolltime } */
const ScrollConfig metaConf       PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 4, WA_LEFT }, 140, true, MAX_WIDTH, 5000, 5, 40 };
const ScrollConfig title1Conf     PROGMEM = {{ TFT_FRAMEWDT, 56, 2, WA_LEFT }, 140, true, MAX_WIDTH-(TITLE_FIX==0?6*2*7-6:TITLE_FIX), 5000, 5, 40 };
const ScrollConfig title2Conf     PROGMEM = {{ TFT_FRAMEWDT, 77, 2, WA_LEFT }, 140, true, MAX_WIDTH-TITLE_FIX, 5000, 5, 40 };
const ScrollConfig playlistConf   PROGMEM = {{ TFT_FRAMEWDT, 200, 3, WA_LEFT }, 140, true, MAX_WIDTH, 1000, 5, 40 };
const ScrollConfig apTitleConf    PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 4, WA_CENTER }, 140, false, MAX_WIDTH, 0, 5, 40 };
const ScrollConfig apSettConf     PROGMEM = {{ TFT_FRAMEWDT, DSP_HEIGHT-TFT_FRAMEWDT-16, 2, WA_LEFT }, 140, false, MAX_WIDTH, 0, 5, 40 };
const ScrollConfig weatherConf    PROGMEM = {{ TFT_FRAMEWDT, 97, 2, WA_LEFT }, 140, true, MAX_WIDTH, 0, 5, 40 };

/* BACKGROUNDS  */                       /* {{ left, top, fontsize, align }, width, height, outlined } */
const FillConfig   metaBGConf     PROGMEM = {{ 0, 0, 0, WA_LEFT }, DSP_WIDTH, 50, false };
const FillConfig   metaBGConfInv  PROGMEM = {{ 0, 50, 0, WA_LEFT }, DSP_WIDTH, 2, false };
// Перенос логики расположения индикатора громкости ближе к NV3041A.
// (Сохраняем совместимость с остальным portrait-layout: VU/clock зона остаётся ниже.)
const FillConfig   volbarConf     PROGMEM = {{ 74, 465, 0, WA_LEFT }, 230, 8, true };
const FillConfig  playlBGConf     PROGMEM = {{ 0, 192, 0, WA_LEFT }, DSP_WIDTH, 36, false };
const FillConfig  heapbarConf     PROGMEM = {{ 0, DSP_HEIGHT-2, 0, WA_LEFT }, DSP_WIDTH, 2, false };

/* WIDGETS  */                           /* { left, top, fontsize, align } */
const WidgetConfig bootstrConf    PROGMEM = { 0, 364, 1, WA_CENTER };
const WidgetConfig bitrateConf    PROGMEM = { 6, 62, 2, WA_RIGHT };
const WidgetConfig voltxtConf     PROGMEM = { 1, DSP_HEIGHT-19, 2, WA_LEFT };
const WidgetConfig  iptxtConf     PROGMEM = { 1, DSP_HEIGHT-36, 2, WA_CENTER };
const WidgetConfig   rssiConf     PROGMEM = { TFT_FRAMEWDT, DSP_HEIGHT-38-6, 3, WA_RIGHT };
const WidgetConfig numConf        PROGMEM = { 0, 300, 0, WA_CENTER };
const WidgetConfig apNameConf     PROGMEM = { TFT_FRAMEWDT, 120, 3, WA_CENTER };
const WidgetConfig apName2Conf    PROGMEM = { TFT_FRAMEWDT, 152, 3, WA_CENTER };
const WidgetConfig apPassConf     PROGMEM = { TFT_FRAMEWDT, 220, 3, WA_CENTER };
const WidgetConfig apPass2Conf    PROGMEM = { TFT_FRAMEWDT, 252, 3, WA_CENTER };
// Позиции часов и VU оставляем максимально близкими к landscape-конфигу ILI9488,
// т.к. ClockWidget использует PSF-буфер и "верх" влияет на размещение окна вывода.
// Координаты часов/ВУ на NV3041 ориентировочно переносим на ILI9488 portrait.
// Высота экрана одинаковая (480), поэтому по Y используем те же top-значения.
// По X ширина отличается: 272 -> 320 (масштаб ~1.176). Для часов берём "0" как в NV3041 (margin справа = 0).
// Часы немного "высоковаты" в portrait — сдвигаем вниз.
const WidgetConfig  clockConf     PROGMEM = { 0, 190, 0, WA_RIGHT };
// В portrait-режиме VU рисуется в цикле постоянно и может перетирать область часов.
// Поэтому смещаем VU сильнее вниз и уменьшаем высоту полос.
// Сдвиг VU ещё ниже, чтобы он не перетирал ClockWidget в PLAYER.
//const WidgetConfig vuConf         PROGMEM = { TFT_FRAMEWDT, 340, 1, WA_LEFT };
// vuConf из NV3041: top=409. По X переносим масштаб 272->320.
// NV3041 vu left = TFT_FRAMEWDT(1)+32 = 33. 33 * 320/272 ~= 38.8 -> 39.
const WidgetConfig vuConf         PROGMEM = { 35, 416, 1, WA_LEFT };

const WidgetConfig bootWdtConf    PROGMEM = { 0, 324, 1, WA_CENTER };
const ProgressConfig bootPrgConf  PROGMEM = { 90, 14, 4 };
const BitrateConfig fullbitrateConf PROGMEM = {{DSP_WIDTH-TFT_FRAMEWDT-33, 51, 2, WA_LEFT}, 42 };

/* BANDS  */                             /* { onebandwidth, onebandheight, bandsHspace, bandsVspace, numofbands, fadespeed } */
// Bands ближе к рабочим параметрам landscape ILI9488, чтобы не было лишнего клиппинга.
// Уменьшаем высоту VU, т.к. общий размер рисуемого canvas = height*2 + space.
// Чуть уменьшаем perheight, чтобы полосы были толще и менее "тонкие".
//const VUBandsConfig bandsConf     PROGMEM = { 32, 70, 4, 2, 8, 3 };
/* BANDS  */                             /* { onebandwidth, onebandheight, bandsHspace, bandsVspace, numofbands, fadespeed } */
//const VUBandsConfig bandsConf     PROGMEM = { 40, 96, 6, 2, 8, 10 };
const VUBandsConfig bandsConf     PROGMEM = { 232, 7, 6, 1, 11, 5 };

/* STRINGS  */
const char         numtxtFmt[]    PROGMEM = "%d";
const char           rssiFmt[]    PROGMEM = "WiFi %d";
const char          iptxtFmt[]    PROGMEM = "%s";
const char         voltxtFmt[]    PROGMEM = "\023\025%d";
const char        bitrateFmt[]    PROGMEM = "%d kBs";

/* MOVES  */                             /* { left, top, width } */
const MoveConfig    clockMove     PROGMEM = { 0, 230, -1 };
const MoveConfig   weatherMove    PROGMEM = { TFT_FRAMEWDT, 97, MAX_WIDTH };
//const MoveConfig   weatherMoveVU  PROGMEM = { TFT_FRAMEWDT, 88, MAX_WIDTH-89+TFT_FRAMEWDT };
const MoveConfig   weatherMoveVU  PROGMEM = { TFT_FRAMEWDT, 97, MAX_WIDTH };
#endif

