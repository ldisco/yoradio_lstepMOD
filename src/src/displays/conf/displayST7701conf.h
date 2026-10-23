/*************************************************************************************
 * ST7701 — Supremus, квадрат 480×480 (черновик раскладки).
 * Ориентир: Faster 480×320, растянут по Y (~×1.5) + запас снизу.
 * VU: слоты ниже (флаг ST7701_SPECTRUM_ENABLE false → сток).
 * Spectrum: под DIGI56 на 480×480; Cover-SS — отдельным шагом.
 *************************************************************************************/

#ifndef displayST7701conf_h
#define displayST7701conf_h

#define DSP_WIDTH       480
#define DSP_HEIGHT      480
#define TFT_FRAMEWDT    5
#define MAX_WIDTH       (DSP_WIDTH - TFT_FRAMEWDT * 2)

#if BITRATE_FULL
  #define TITLE_FIX 60  /* под бейдж 56 + зазор */
#else
  #define TITLE_FIX 0
#endif
/* Ориентир; drawLogo ST7701 сам центрирует слот по (H−maxH)/2. Пик ~125% H≈160. */
#define bootLogoTop     160

/* SROLLS — meta pad как Faster 281 (Y=9 / Inv H=37), не лезет на title1.
 * title1/2: Faster 45/69 + зазор; +2 от meta / +2 от title1 → 47/72. */
const ScrollConfig metaConf       PROGMEM = {{ TFT_FRAMEWDT, 9, 3, WA_LEFT }, 140, true, MAX_WIDTH, 5000, 6, 55 };
const ScrollConfig title1Conf     PROGMEM = {{ TFT_FRAMEWDT, 49, 2, WA_LEFT }, 140, true, MAX_WIDTH - TITLE_FIX, 5000, 4, 30 };
const ScrollConfig title2Conf     PROGMEM = {{ TFT_FRAMEWDT, 75, 2, WA_LEFT }, 140, false, MAX_WIDTH - TITLE_FIX, 5000, 4, 30 };
/* size 3 как ILI9488: на 480×480 size 2 слишком мелко; playlBG H=36 под строку. */
const ScrollConfig playlistConf   PROGMEM = {{ TFT_FRAMEWDT, 222, 3, WA_LEFT }, 140, false, MAX_WIDTH, 1000, 2, 30 };
const ScrollConfig apTitleConf    PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 3, WA_CENTER }, 140, false, MAX_WIDTH, 0, 4, 20 };
const ScrollConfig apSettConf     PROGMEM = {{ TFT_FRAMEWDT, DSP_HEIGHT - TFT_FRAMEWDT - 16, 2, WA_LEFT }, 140, false, MAX_WIDTH, 0, 4, 30 };
const ScrollConfig weatherConf    PROGMEM = {{ 8, 174, 2, WA_LEFT }, 140, true, MAX_WIDTH, 0, 4, 30 };

/* BACKGROUNDS — Inv H=37 как Faster: буквы size3 (Y=9, h≈24) в полосе, ниже title1. */
const FillConfig   metaBGConf     PROGMEM = {{ 0, 0, 0, WA_LEFT }, DSP_WIDTH, 56, false };
const FillConfig   metaBGConfInv  PROGMEM = {{ 0, 0, 0, WA_LEFT }, DSP_WIDTH, 37, false };
const FillConfig   volbarConf     PROGMEM = {{ TFT_FRAMEWDT, DSP_HEIGHT - TFT_FRAMEWDT - 6, 0, WA_LEFT }, MAX_WIDTH, 6, true };
const FillConfig  playlBGConf     PROGMEM = {{ 0, 214, 0, WA_LEFT }, DSP_WIDTH, 36, false };
/* Буфер и CPU: по 1 px, между ними зазор 1 px (не слипать). Низ экрана: … / buf / gap / cpu. */
const FillConfig  heapbarConf     PROGMEM = {{ 0, DSP_HEIGHT - 3, 0, WA_LEFT }, DSP_WIDTH, 1, false };
const FillConfig  cpubarConf      PROGMEM = {{ 0, DSP_HEIGHT - 1, 0, WA_LEFT }, DSP_WIDTH, 1, false };

/* WIDGETS */
const WidgetConfig bootstrConf    PROGMEM = { 0, 360, 1, WA_CENTER };
const WidgetConfig bitrateConf    PROGMEM = { 105, 380, 1, WA_LEFT };
/* Футер как Faster: к volbar(469) зазор ~13 px текст / ~21 px Wi‑Fi. */
const WidgetConfig voltxtConf     PROGMEM = { 0, 456, 1, WA_CENTER };
const WidgetConfig  iptxtConf     PROGMEM = { TFT_FRAMEWDT, 456, 1, WA_LEFT };
const WidgetConfig   rssiConf     PROGMEM = { TFT_FRAMEWDT, 448, 2, WA_RIGHT };
const WidgetConfig numConf        PROGMEM = { 0, 300, 0, WA_CENTER };
const WidgetConfig apNameConf     PROGMEM = { TFT_FRAMEWDT, 120, 2, WA_CENTER };
const WidgetConfig apName2Conf    PROGMEM = { TFT_FRAMEWDT, 160, 2, WA_CENTER };
const WidgetConfig apPassConf     PROGMEM = { TFT_FRAMEWDT, 240, 2, WA_CENTER };
const WidgetConfig apPass2Conf    PROGMEM = { TFT_FRAMEWDT, 280, 2, WA_CENTER };
/* Baseline 390: глиф 320…390, дата ≈400…408; VU top 275 — смещение ~115 как Faster. */
const WidgetConfig  clockConf     PROGMEM = { 8, 390, 0, WA_RIGHT };
/* VU всегда в conf; SpectrumWidget — только при SPECTRUM_ANALYZER_ACTIVE. */
const WidgetConfig vuConf         PROGMEM = { TFT_FRAMEWDT, 275, 1, WA_LEFT };

const WidgetConfig bootWdtConf    PROGMEM = { 0, 324, 1, WA_CENTER };
const ProgressConfig bootPrgConf  PROGMEM = { 90, 14, 4 };
/* Бейдж крупнее Faster (42→56); textsize 2; Y −2 от 44. */
const BitrateConfig fullbitrateConf PROGMEM = {{ DSP_WIDTH - 56, 42, 2, WA_LEFT }, 56 };

/* BANDS: классический VU (ветка yoRadio WA_LEFT, как Faster/JC2432). */
const VUBandsConfig bandsConf     PROGMEM = { 36, 133, 4, 2, 10, 2 };

#if SPECTRUM_ANALYZER_ACTIVE
/*
 * FFT под 480×480: clockMove.top=320 (глиф ~250…320); спектр Y=370, H=70.
 * Погода Y=174 выше глифа — hold/off-screen не нужен (в отличие от Faster AXS).
 * frameIntervalMs=50 — в пару к ST7701 ANALYSIS_INTERVAL_MS=50 (Opus 2.1, ~20 Гц).
 */
const WidgetConfig spectrumConf   PROGMEM = { TFT_FRAMEWDT, 370, 1, WA_LEFT };
const SpectrumConfig spectrumBandsConf PROGMEM = { MAX_WIDTH, 70, 12, 8, 1, 1, 50 };
#endif

/* STRINGS */
const char         numtxtFmt[]    PROGMEM = "%d";
const char           rssiFmt[]    PROGMEM = "WiFi %d";
const char          iptxtFmt[]    PROGMEM = "\010 %s";
const char         voltxtFmt[]    PROGMEM = "\023\025%d";
const char        bitrateFmt[]    PROGMEM = "%d kBs";

/* MOVES */
#if SPECTRUM_ANALYZER_ACTIVE
/* PLAY: часы вверх под спектр (width≥0 → moveTo). VU-профиль: ветка #else, width=-1. */
const MoveConfig    clockMove     PROGMEM = { 8, 320, 0 };
#else
const MoveConfig    clockMove     PROGMEM = { 8, 390, -1 };
#endif
const MoveConfig   weatherMove    PROGMEM = { 8, 174, MAX_WIDTH };
const MoveConfig   weatherMoveVU  PROGMEM = { 105, 174, MAX_WIDTH - 105 + TFT_FRAMEWDT };

#endif
