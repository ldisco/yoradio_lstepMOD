/*************************************************************************************
    ST7796 480X320 displays configuration file.
    Copy this file to yoRadio/src/displays/conf/displayST7789conf_custom.h
    and modify it
    More info on https://github.com/e2002/yoradio/wiki/Widgets#widgets-description
*************************************************************************************/

#ifndef displayILI9488conf_h
#define displayILI9488conf_h

#define DSP_WIDTH       480
#define DSP_HEIGHT      320
#define TFT_FRAMEWDT    10
#define MAX_WIDTH       DSP_WIDTH-TFT_FRAMEWDT*2

#if BITRATE_FULL
  #define TITLE_FIX 44
#else
  #define TITLE_FIX 0
#endif
#define bootLogoTop     110

/* SROLLS  */                            /* {{ left, top, fontsize, align }, buffsize, uppercase, width, scrolldelay, scrolldelta, scrolltime } */
const ScrollConfig metaConf       PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 4, WA_LEFT }, 140, true, MAX_WIDTH, 5000, 7, 40 };
/*
 * Зазоры как JC2432/ST7789 SPECTRUM: metaBG H=50 → +4 px → title1;
 * title1 size2 (H=16) → +4 px → title2. Было title1=62/title2=86 (лишний зазор под meta).
 * Подъём mid-блока (часы/спектр/пирамидка) = 62−54 = 8 px; футер IP/vol/rssi не трогаем.
 */
const ScrollConfig title1Conf     PROGMEM = {{ TFT_FRAMEWDT, 54, 2, WA_LEFT }, 140, true, MAX_WIDTH-(TITLE_FIX==0?6*2*7-6:TITLE_FIX), 5000, 7, 40 };
const ScrollConfig title2Conf     PROGMEM = {{ TFT_FRAMEWDT, 74, 2, WA_LEFT }, 140, false, MAX_WIDTH-TITLE_FIX, 5000, 7, 40 };
/* uppercase=false — названия станций в плейлисте как в playlist.csv / тегах, без CAPS */
const ScrollConfig playlistConf   PROGMEM = {{ TFT_FRAMEWDT, 146, 3, WA_LEFT }, 140, false, MAX_WIDTH, 1000, 7, 40 };
const ScrollConfig apTitleConf    PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 4, WA_CENTER }, 140, false, MAX_WIDTH, 0, 7, 40 };
const ScrollConfig apSettConf     PROGMEM = {{ TFT_FRAMEWDT, 320-TFT_FRAMEWDT-16, 2, WA_LEFT }, 140, false, MAX_WIDTH, 0, 7, 40 };
/* STOP: бегущая строка под title2 (74+16+4=94); было 116. */
const ScrollConfig weatherConf    PROGMEM = {{ TFT_FRAMEWDT, 94, 2, WA_LEFT }, 140, true, MAX_WIDTH, 0, 7, 40 };

/*
 * Playing+blank cover-скринсейвер (landscape 480×320), как ST7789 CYD:
 * слева квадрат обложки/лого, справа «прыгающие» DS_DIGI28 + погода.
 * 304×304 — макс. высота до полосы факта (ST7789_COV_FACT_TOP=304); Y=0 без налезания на факт.
 * Справа 176 px под часы/погоду (было 280/200).
 * JPEG в слот: cover-fill; iTunes 300×300 для ILI9488 (слот 304) — без обрезки краёв.
 */
#define ST7789_COV_COVER_SIZE            304
#define ST7789_COV_COVER_X_LEFT          0
#define ST7789_COV_COVER_Y               0
#define ST7789_COV_MAX_DECODE_DIM        512
#define ST7789_COV_FB_LEFT_W             ST7789_COV_COVER_SIZE
#define ST7789_COV_FB_RIGHT_X            ST7789_COV_COVER_SIZE
#define ST7789_COV_FB_RIGHT_W            (DSP_WIDTH - ST7789_COV_COVER_SIZE)
#define ST7789_COV_SIDE_CLK_GLYPH_ABOVE  34
#define ST7789_COV_SIDE_CLK_MIN_TOP      60
/* Ниже FACT_TOP: часы+погода не заезжают на бегущую строку факта.
 * VC2 48×48 под часами: при top=210 низ погоды ≈270, FACT_TOP=304. */
#define ST7789_COV_SIDE_CLK_MAX_TOP      210
#define ST7789_COV_SIDE_CLK_CLEAR_H      (ST7789_COV_SIDE_CLK_GLYPH_ABOVE + 14)
/* Cover-SS погода: как JC2432/ST7789 SPECTRUM — VC2 48×48 (не геометрия 36). */
#define ST7789_COV_WICON_W               48
#define ST7789_COV_WICON_H               48
#define ST7789_COV_USE_VC2_ICONS         1
#define ST7789_COV_WICON_GAP_BELOW       0
#define ST7789_COV_WICON_DESCENT         2
/* Подписи: иконка под HH, t° под MM, город по центру HH:MM (+ скролл длинного). */
#define ST7789_COV_WTEMP_GAP             2
#define ST7789_COV_WCITY_GAP             2
#define ST7789_COV_WTEMP_TEXT_SIZE       2
#define ST7789_COV_WCITY_TEXT_SIZE       1
#define ST7789_COV_WTEMP_H               16
#define ST7789_COV_WCITY_H               8
#define ST7789_COV_WBLOCK_H              (ST7789_COV_WICON_H + ST7789_COV_WCITY_GAP + ST7789_COV_WCITY_H)
#define ST7789_COV_SIDE_CLK_MARGIN_R     4
#define ST7789_COV_SIDE_CLK_MARGIN_L     4
#define ST7789_COV_WICON_SHIFT_X         (-3)
#define ST7789_COV_WTEMP_SHIFT_X         (-8)
#define ST7789_COV_SEAM_W                0
#define ST7789_COV_SIDE_SS_JUMP_MS       20000UL
#define ST7789_COV_COVER_RETRY_MS        5000UL
#define ST7789_COV_SMALL_CLOCK_FONT      ST240_SmallClockFont
#define ST7789_COV_LOGO_VER_TEXT_SIZE    1
#define ST7789_COV_LOGO_VER_GAP_PX       8
#define ST7789_COV_FACT_TEXT_SIZE        2
#define ST7789_COV_FACT_H                (ST7789_COV_FACT_TEXT_SIZE * 8)
#define ST7789_COV_FACT_TOP              (DSP_HEIGHT - ST7789_COV_FACT_H)

/* Факт в cover-SS: ScrollWidget как у ST7789; buffsize 400 под длинные UTF-8. */
const ScrollConfig coverFactConf  PROGMEM = {{ TFT_FRAMEWDT, ST7789_COV_FACT_TOP, ST7789_COV_FACT_TEXT_SIZE, WA_LEFT }, 400, false, MAX_WIDTH, 1500, 3, 35 };

/* BACKGROUNDS  */                       /* {{ left, top, fontsize, align }, width, height, outlined } */
const FillConfig   metaBGConf     PROGMEM = {{ 0, 0, 0, WA_LEFT }, DSP_WIDTH, 50, false };
/* При DSP_INVERT_TITLE false рисуется metaBGConfInv. Было height=2 — зелёная «линейка»
 * под жёлтой плашкой (metafill=div). Как ST7789/JC2432 — height 0, без артефакта. */
const FillConfig   metaBGConfInv  PROGMEM = {{ 0, 50, 0, WA_LEFT }, DSP_WIDTH, 0, false };
const FillConfig   volbarConf     PROGMEM = {{ TFT_FRAMEWDT, DSP_HEIGHT-TFT_FRAMEWDT-8, 0, WA_LEFT }, MAX_WIDTH, 8, true };
const FillConfig  playlBGConf     PROGMEM = {{ 0, 138, 0, WA_LEFT }, DSP_WIDTH, 36, false };
const FillConfig  heapbarConf     PROGMEM = {{ 0, DSP_HEIGHT-2, 0, WA_LEFT }, DSP_WIDTH, 2, false };

/* WIDGETS  */                           /* { left, top, fontsize, align } */
const WidgetConfig bootstrConf    PROGMEM = { 0, 243, 1, WA_CENTER };
const WidgetConfig bitrateConf    PROGMEM = { 6, 54, 2, WA_RIGHT };
const WidgetConfig voltxtConf     PROGMEM = { 0, DSP_HEIGHT-38, 2, WA_CENTER };
const WidgetConfig  iptxtConf     PROGMEM = { TFT_FRAMEWDT, DSP_HEIGHT-38, 2, WA_LEFT };
const WidgetConfig   rssiConf     PROGMEM = { TFT_FRAMEWDT, DSP_HEIGHT-38-6, 3, WA_RIGHT };
const WidgetConfig numConf        PROGMEM = { 0, 200, 0, WA_CENTER };
const WidgetConfig apNameConf     PROGMEM = { TFT_FRAMEWDT, 88, 3, WA_CENTER };
const WidgetConfig apName2Conf    PROGMEM = { TFT_FRAMEWDT, 120, 3, WA_CENTER };
const WidgetConfig apPassConf     PROGMEM = { TFT_FRAMEWDT, 173, 3, WA_CENTER };
const WidgetConfig apPass2Conf    PROGMEM = { TFT_FRAMEWDT, 205, 3, WA_CENTER };
const WidgetConfig  clockConf     PROGMEM = { TFT_FRAMEWDT*2, 230, 0, WA_RIGHT };
/* Сток yoRadio ILI9488 landscape: вертикальные L/R слева от часов (см. yoRadio displayILI9488conf.h). */
const WidgetConfig vuConf         PROGMEM = { TFT_FRAMEWDT, 136, 1, WA_LEFT };

const WidgetConfig bootWdtConf    PROGMEM = { 0, 216, 1, WA_CENTER };
const ProgressConfig bootPrgConf  PROGMEM = { 90, 14, 4 };
/* Значок битрейта: 1 px под metaBG (H=50), как ST7789 под meta. */
const BitrateConfig fullbitrateConf PROGMEM = {{DSP_WIDTH-TFT_FRAMEWDT-38, 51, 2, WA_LEFT}, 42 };

/* BANDS  */                             /* { onebandwidth, onebandheight, bandsHspace, bandsVspace, numofbands, fadespeed } */
/* Сток yoRadio: canvas (32*2+4)×130, полосы ~32 px — не сужать до _170 (11 px). */
const VUBandsConfig bandsConf     PROGMEM = { 32, 130, 4, 2, 10, 3 };

#if SPECTRUM_ANALYZER_ACTIVE
/*
 * FFT вместо бокового VU — раскладка как JC2432/ST7789 SPECTRUM, масштаб 480×320.
 * Mid-блок поднят на 8 px вместе с title (было clock 190 / spectrum 236).
 * DS_DIGI56 (yAdvance≈110): глифы [top−110 .. top]; fullclock дата ≈ top+10 .. top+26.
 * clockMove.top=182 → глифы ≈72..182, дата ≈192..208; спектр Y=228 H=42 → 228..270
 * (футер iptxt/vol ≈282 без сдвига). Погода — пирамидка слева; на STOP — бегущая строка.
 * 12 полос × 8 сегментов; frameIntervalMs=40 — запас SPI vs ST7789 (33).
 */
const WidgetConfig spectrumConf   PROGMEM = { TFT_FRAMEWDT, 228, 1, WA_LEFT };
/* H=42: низ ≈270, над футером iptxt/vol (Y≈282); запас под красные пики над полосами. */
const SpectrumConfig spectrumBandsConf PROGMEM = { MAX_WIDTH, 42, 12, 8, 5, 3, 40 };

/* PLAYER+PLAY: пирамидка (иконка→t°→город), общий st7789_player_weather.inc.
 * VC2 64×64 только PLAYER ILI9488 (SS остаётся 48). Слот чуть шире под t° size=2.
 * Y=112: −8 с title-подъёмом; iconY=Y−9 → низ иконки ≈167. */
#define ST7789_PLR_VC2_64                1
#define ST7789_PLR_WICON_W               64
#define ST7789_PLR_WICON_H               64
#define ST7789_PLR_WTEMP_TEXT_SIZE       2
#define ST7789_PLR_WCITY_TEXT_SIZE       1
#define ST7789_PLR_ROW_GAP               2
#define ST7789_PLR_WEATHER_CLOCK_LEFT    130
#define ST7789_PLR_WEATHER_X             TFT_FRAMEWDT
#define ST7789_PLR_WEATHER_Y             112
#define ST7789_PLR_WEATHER_W             (ST7789_PLR_WEATHER_CLOCK_LEFT - ST7789_PLR_WEATHER_X)
#endif

/* STRINGS  */
const char         numtxtFmt[]    PROGMEM = "%d";
const char           rssiFmt[]    PROGMEM = "WiFi %d";
const char          iptxtFmt[]    PROGMEM = "%s";
const char         voltxtFmt[]    PROGMEM = "\023\025%d";
const char        bitrateFmt[]    PROGMEM = "%d kBs";

/* MOVES  */                             /* { left, top, width } */
#if SPECTRUM_ANALYZER_ACTIVE
/*
 * При PLAY часы поднимаются (width≥0 → moveTo); иначе width=-1 оставлял сток Y=230
 * и глифы DIGI56 наезжали на спектр. Mid −8 px с titles. Погода — колонка слева.
 */
const MoveConfig    clockMove     PROGMEM = { TFT_FRAMEWDT * 2, 182, 0 };
const MoveConfig   weatherMove    PROGMEM = { TFT_FRAMEWDT, 102, 100 };
const MoveConfig   weatherMoveVU  PROGMEM = { TFT_FRAMEWDT, 102, 100 };
#else
const MoveConfig    clockMove     PROGMEM = { 0, 230, -1 };
const MoveConfig   weatherMove    PROGMEM = { 8, 120, MAX_WIDTH };
/* Сток yoRadio: при VU погода чуть правее (X=89), не вниз как на ST7789_170 */
const MoveConfig   weatherMoveVU  PROGMEM = { 89, 120, MAX_WIDTH-89+TFT_FRAMEWDT };
#endif

#endif
