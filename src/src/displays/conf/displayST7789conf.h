/*************************************************************************************
    ST7789 320x240 displays configuration file.
    Copy this file to yoRadio/src/displays/conf/displayST7789conf_custom.h
    and modify it
    More info on https://github.com/e2002/yoradio/wiki/Widgets#widgets-description
*************************************************************************************/

#ifndef displayST7789conf_h
#define displayST7789conf_h

#define DSP_WIDTH       320
#define DSP_HEIGHT      240   /* Landscape JC2432 / ST7789 320×240 — нужен cover-скринсейверу */
//#define TFT_FRAMEWDT    8
#define TFT_FRAMEWDT    5
#define MAX_WIDTH       DSP_WIDTH-TFT_FRAMEWDT*2
//#define PLMITEMS        11
//#define PLMITEMLENGHT   40
//#define PLMITEMHEIGHT   22

#if BITRATE_FULL
  #define TITLE_FIX 44
#else
  #define TITLE_FIX 0
#endif
#define bootLogoTop     68

#if ST7789_SPECTRUM_ENABLE
/* Playing+blank cover-скринсейвер (режим спектра): слот обложки 200×200 (= iTunes JPEG 1:1). */
#define ST7789_COV_COVER_SIZE            200
#define ST7789_COV_COVER_X_LEFT          0
/* Слот 200×200 по центру по вертикали на экране 240 px (Y=20). */
#define ST7789_COV_COVER_Y               ((DSP_HEIGHT - ST7789_COV_COVER_SIZE) / 2)
#define ST7789_COV_FB_LEFT_W             ST7789_COV_COVER_SIZE
#define ST7789_COV_FB_RIGHT_X            ST7789_COV_COVER_SIZE
#define ST7789_COV_FB_RIGHT_W            (DSP_WIDTH - ST7789_COV_COVER_SIZE)
/* Правая колонка 120 px — DS_DIGI28 (~135 px «88:88»), прижаты к X=200; ~15 px у правого края TFT могут обрезаться. */
#define ST7789_COV_SIDE_CLK_GLYPH_ABOVE  34
#define ST7789_COV_SIDE_CLK_MIN_TOP      50
/* Ниже 146: под baseline часов — VC2 48×48 + t° + город (было 162 под 32×32). */
#define ST7789_COV_SIDE_CLK_MAX_TOP      146
#define ST7789_COV_SIDE_CLK_CLEAR_H      (ST7789_COV_SIDE_CLK_GLYPH_ABOVE + 14)
#define ST7789_COV_WICON_W               48
#define ST7789_COV_WICON_H               48
/* Cover-SS: те же bitmap VC2 48×48, что у пирамидки PLAYER (не геометрия). */
#define ST7789_COV_USE_VC2_ICONS         1
/* Зазор иконки под часами. */
#define ST7789_COV_WICON_GAP_BELOW       0
#define ST7789_COV_WICON_DESCENT         2
/* Подписи погоды: иконка под HH, температура под MM (одна линия), город — по центру HH:MM. */
#define ST7789_COV_WTEMP_GAP             2
#define ST7789_COV_WCITY_GAP             2
#define ST7789_COV_WTEMP_TEXT_SIZE       2
#define ST7789_COV_WCITY_TEXT_SIZE       1
#define ST7789_COV_WTEMP_H               16
#define ST7789_COV_WCITY_H               8
#define ST7789_COV_WBLOCK_H              (ST7789_COV_WICON_H + ST7789_COV_WCITY_GAP + ST7789_COV_WCITY_H)
#define ST7789_COV_SIDE_CLK_MARGIN_R     4
/* Отступ часов от обложки: было 6, −4 px ближе к обложке (глифы DS_DIGI28). */
#define ST7789_COV_SIDE_CLK_MARGIN_L     2
/* Иконка: −3 (было −6) — +3 px вправо, меньше наезд на обложку. */
#define ST7789_COV_WICON_SHIFT_X         (-3)
/* t°: −8 (было −6) — ближе к иконке на 2 px влево. */
#define ST7789_COV_WTEMP_SHIFT_X         (-8)
/* 0 = не затирать правый край обложки; защита от вылета глифов — через MARGIN_L. */
#define ST7789_COV_SEAM_W                0
#define ST7789_COV_SIDE_SS_JUMP_MS       20000UL
#define ST7789_COV_COVER_RETRY_MS        5000UL
#define ST7789_COV_SMALL_CLOCK_FONT      ST240_SmallClockFont
/* Под логотипом (когда нет JPEG): версия YOVERSION — шрифт/цвет как на boot (size 1, theme.rssi). */
#define ST7789_COV_LOGO_VER_TEXT_SIZE    1
#define ST7789_COV_LOGO_VER_GAP_PX       8
/* Факт TrackFacts: полоса внизу экрана (под слотом обложки); скролл — через ScrollWidget coverFactConf. */
#define ST7789_COV_FACT_TEXT_SIZE        2
#define ST7789_COV_FACT_H                (ST7789_COV_FACT_TEXT_SIZE * 8)
#define ST7789_COV_FACT_TOP              (DSP_HEIGHT - ST7789_COV_FACT_H)
/* PLAYER+PLAY: «пирамидка» слева (иконка → t° → город) в зазоре до цифр часов. */
#define ST7789_PLR_WICON_W               48
#define ST7789_PLR_WICON_H               48
/* GFX textSize только целые 1/2/… — «чуть меньше 2» нельзя; оставляем 2 (1 будет вдвое мельче). */
#define ST7789_PLR_WTEMP_TEXT_SIZE       2
#define ST7789_PLR_WCITY_TEXT_SIZE       1
#define ST7789_PLR_ROW_GAP               2
/* Левый край зоны часов: слот шире — чтобы t° size=2 («+20°C») не обрезалась. */
#define ST7789_PLR_WEATHER_CLOCK_LEFT    78
#define ST7789_PLR_WEATHER_X             TFT_FRAMEWDT
/* Пирамидка: было 76, опустили на 1 px. */
#define ST7789_PLR_WEATHER_Y             77
#define ST7789_PLR_WEATHER_W             (ST7789_PLR_WEATHER_CLOCK_LEFT - ST7789_PLR_WEATHER_X)
#else
/* Default-ветка (без спектра): возвращаем базовую геометрию как в эталоне copy2/254. */
#define ST7789_COV_COVER_SIZE            170
#define ST7789_COV_COVER_X_LEFT          0
#define ST7789_COV_COVER_Y               ((DSP_HEIGHT - ST7789_COV_COVER_SIZE) / 2)
#define ST7789_COV_FB_LEFT_W             ST7789_COV_COVER_SIZE
#define ST7789_COV_FB_RIGHT_X            ST7789_COV_COVER_SIZE
#define ST7789_COV_FB_RIGHT_W            (DSP_WIDTH - ST7789_COV_COVER_SIZE)
#define ST7789_COV_SIDE_CLK_GLYPH_ABOVE  34
#define ST7789_COV_SIDE_CLK_MIN_TOP      50
#define ST7789_COV_SIDE_CLK_MAX_TOP      188
#define ST7789_COV_SIDE_CLK_CLEAR_H      (ST7789_COV_SIDE_CLK_GLYPH_ABOVE + 14)
#define ST7789_COV_WICON_W               36
#define ST7789_COV_WICON_H               36
#define ST7789_COV_WICON_GAP_BELOW       4
#define ST7789_COV_WICON_DESCENT         12
#define ST7789_COV_WTEMP_GAP             2
#define ST7789_COV_WCITY_GAP             2
#define ST7789_COV_WTEMP_TEXT_SIZE       2
#define ST7789_COV_WCITY_TEXT_SIZE       1
#define ST7789_COV_WTEMP_H               16
#define ST7789_COV_WCITY_H               8
#define ST7789_COV_WBLOCK_H              (ST7789_COV_WICON_H + ST7789_COV_WCITY_GAP + ST7789_COV_WCITY_H)
#define ST7789_COV_SIDE_CLK_MARGIN_R     12
#define ST7789_COV_SIDE_CLK_MARGIN_L     12
/* Default-ветка: без доп. сдвига иконки/t° (как SPECTRUM — только при необходимости). */
#define ST7789_COV_WICON_SHIFT_X         0
#define ST7789_COV_WTEMP_SHIFT_X         0
/* Та же логика, что в SPECTRUM-ветке: не клипать JPEG «швом» (см. coverOutput). */
#define ST7789_COV_SEAM_W                0
#define ST7789_COV_SIDE_SS_JUMP_MS       20000UL
#define ST7789_COV_COVER_RETRY_MS        5000UL
#define ST7789_COV_SMALL_CLOCK_FONT      ST240_SmallClockFont
/* Под логотипом (когда нет JPEG): версия YOVERSION — шрифт/цвет как на boot (size 1, theme.rssi). */
#define ST7789_COV_LOGO_VER_TEXT_SIZE    1
#define ST7789_COV_LOGO_VER_GAP_PX       8
/* Факт TrackFacts: полоса внизу экрана (под слотом обложки); скролл — через ScrollWidget coverFactConf. */
#define ST7789_COV_FACT_TEXT_SIZE        2
#define ST7789_COV_FACT_H                (ST7789_COV_FACT_TEXT_SIZE * 8)
#define ST7789_COV_FACT_TOP              (DSP_HEIGHT - ST7789_COV_FACT_H)
#endif

/* SROLLS  */                            /* {{ left, top, fontsize, align }, buffsize, uppercase, width, scrolldelay, scrolldelta, scrolltime } */
const ScrollConfig metaConf       PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 3, WA_LEFT }, 140, true, MAX_WIDTH, 5000, 5, 30 };
#if ST7789_SPECTRUM_ENABLE
/*
 * title1/title2 подняты: зазор от низа meta (Y=29) до title1 = 4 px,
 * такой же как между title1 и title2 (fontsize 2 → высота строки 16 px).
 * title1: 29+4=33; title2: 33+16+4=53.
 * title2 uppercase=false — регистр как в ID3/ICY (не принудительный CAPS через utf8Rus).
 */
const ScrollConfig title1Conf     PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT + 3 * 8 + 4, 2, WA_LEFT }, 140, true, MAX_WIDTH-TITLE_FIX, 5000, 4, 30 };
const ScrollConfig title2Conf     PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT + 3 * 8 + 4 + 2 * 8 + 4, 2, WA_LEFT }, 140, false, MAX_WIDTH-TITLE_FIX, 5000, 4, 30 };
#else
const ScrollConfig title1Conf     PROGMEM = {{ TFT_FRAMEWDT, 48, 2, WA_LEFT }, 140, true, MAX_WIDTH-TITLE_FIX, 5000, 4, 30 };
/* title2: без CAPS — отображение как в теге файла / метаданных потока */
const ScrollConfig title2Conf     PROGMEM = {{ TFT_FRAMEWDT, 72, 2, WA_LEFT }, 140, false, MAX_WIDTH-TITLE_FIX, 5000, 4, 30 };
#endif
const ScrollConfig playlistConf   PROGMEM = {{ TFT_FRAMEWDT, 112, 2, WA_LEFT }, 140, true, MAX_WIDTH, 1000, 2, 30 };
const ScrollConfig apTitleConf    PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 3, WA_CENTER }, 140, false, MAX_WIDTH, 0, 4, 20 };
const ScrollConfig apSettConf     PROGMEM = {{ TFT_FRAMEWDT, 240-TFT_FRAMEWDT-16, 2, WA_LEFT }, 140, false, MAX_WIDTH, 0, 4, 30 };
const ScrollConfig weatherConf    PROGMEM = {{ 8, 87, 2, WA_LEFT }, 140, true, MAX_WIDTH, 0, 4, 30 };
/*
 * Факт в cover-скринсейвере: тот же ScrollWidget, что weather/meta/title.
 * Чуть медленнее погоды на паузе (weather: delta=4, time=30) → delta=3, time=35.
 * uppercase=false — факты читаются как обычный текст; start delay 1.5 с.
 */
const ScrollConfig coverFactConf  PROGMEM = {{ TFT_FRAMEWDT, ST7789_COV_FACT_TOP, ST7789_COV_FACT_TEXT_SIZE, WA_LEFT }, 400, false, MAX_WIDTH, 1500, 3, 35 };

/* BACKGROUNDS  */                       /* {{ left, top, fontsize, align }, width, height, outlined } */
const FillConfig   metaBGConf     PROGMEM = {{ 0, 0, 0, WA_LEFT }, DSP_WIDTH, 38, false };
/* При DSP_INVERT_TITLE false рисуется metaBGConfInv. Stock height=1 на Y=38 —
 * жёлтая «линейка» под названием. Как на ST7789_170 — height 0, без артефакта. */
const FillConfig   metaBGConfInv  PROGMEM = {{ 0, 38, 0, WA_LEFT }, DSP_WIDTH, 0, false };
const FillConfig   volbarConf     PROGMEM = {{ TFT_FRAMEWDT, 240-TFT_FRAMEWDT-6, 0, WA_LEFT }, MAX_WIDTH, 6, true };
const FillConfig  playlBGConf     PROGMEM = {{ 0, 107, 0, WA_LEFT }, DSP_WIDTH, 24, false };
const FillConfig  heapbarConf     PROGMEM = {{ 0, 239, 0, WA_LEFT }, DSP_WIDTH, 1, false };

/* WIDGETS  */                           /* { left, top, fontsize, align } */
const WidgetConfig bootstrConf    PROGMEM = { 0, 182, 1, WA_CENTER };
const WidgetConfig bitrateConf    PROGMEM = { 70, 191, 1, WA_LEFT };
const WidgetConfig voltxtConf     PROGMEM = { 0, 214, 1, WA_CENTER };
const WidgetConfig  iptxtConf     PROGMEM = { TFT_FRAMEWDT, 214, 1, WA_LEFT };
const WidgetConfig   rssiConf     PROGMEM = { TFT_FRAMEWDT, 208, 2, WA_RIGHT };
const WidgetConfig numConf        PROGMEM = { 0, 120+30, 0, WA_CENTER };
const WidgetConfig apNameConf     PROGMEM = { TFT_FRAMEWDT, 66, 2, WA_CENTER };
const WidgetConfig apName2Conf    PROGMEM = { TFT_FRAMEWDT, 90, 2, WA_CENTER };
const WidgetConfig apPassConf     PROGMEM = { TFT_FRAMEWDT, 130, 2, WA_CENTER };
const WidgetConfig apPass2Conf    PROGMEM = { TFT_FRAMEWDT, 154, 2, WA_CENTER };
const WidgetConfig  clockConf     PROGMEM = { 8, 176, 0, WA_RIGHT };
const WidgetConfig vuConf         PROGMEM = { TFT_FRAMEWDT, 100, 1, WA_LEFT };

const WidgetConfig bootWdtConf    PROGMEM = { 0, 162, 1, WA_CENTER };
const ProgressConfig bootPrgConf  PROGMEM = { 90, 14, 4 };
/*
 * Значок битрейта: top=30 (1 px под meta, низ meta Y=29).
 * Правый край квадрата 42×42 вровень с правым краем полосы meta (X=DSP_WIDTH=320) → left=278.
 */
const BitrateConfig fullbitrateConf PROGMEM = {{ DSP_WIDTH - 42, TFT_FRAMEWDT + 3 * 8 + 1, 2, WA_LEFT }, 42 };

/* BANDS  */                             /* { onebandwidth, onebandheight, bandsHspace, bandsVspace, numofbands, fadespeed } */
const VUBandsConfig bandsConf     PROGMEM = { 24, 100, 4, 2, 10, 2 };

#if ST7789_SPECTRUM_ENABLE
/*
 * Полноширинная зона X=5..314, Y=148..202.
 * 12 столбцов, зазор между столбцами 4 px (было 3).
 * 8 гориз. сегментов: высота 4 px, зазор 3 px (было 6 px / 1 px).
 */
const WidgetConfig spectrumConf   PROGMEM = { TFT_FRAMEWDT, 148, 1, WA_LEFT };
/* frameIntervalMs=33 — синхронно с ANALYSIS_INTERVAL_MS анализатора (~30 FPS) */
const SpectrumConfig spectrumBandsConf PROGMEM = { MAX_WIDTH, 55, 12, 8, 4, 3, 33 };
#endif

/* STRINGS  */
const char         numtxtFmt[]    PROGMEM = "%d";
const char           rssiFmt[]    PROGMEM = "WiFi %d";
const char          iptxtFmt[]    PROGMEM = "\010 %s";
const char         voltxtFmt[]    PROGMEM = "\023\025%d";
const char        bitrateFmt[]    PROGMEM = "%d kBs";

/* MOVES  */                             /* { left, top, width } */
#if ST7789_SPECTRUM_ENABLE
/*
 * При PLAY часы поднимаются: глифы DS_DIGI42 занимают Y≈80..132,
 * служебные дата/секунды заканчиваются до начала спектра на Y=148.
 * Погода получает узкую левую колонку X=5..79 и не пересекает часы справа.
 */
const MoveConfig    clockMove     PROGMEM = { 8, 132, 0 };
const MoveConfig   weatherMove    PROGMEM = { TFT_FRAMEWDT, 105, 75 };
const MoveConfig   weatherMoveVU  PROGMEM = { TFT_FRAMEWDT, 105, 75 };
#else
const MoveConfig    clockMove     PROGMEM = { 8, 180, -1 };
const MoveConfig   weatherMove    PROGMEM = { 8, 97, MAX_WIDTH };
const MoveConfig   weatherMoveVU  PROGMEM = { 70, 97, MAX_WIDTH-70+TFT_FRAMEWDT };
#endif

#endif
