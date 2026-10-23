/*************************************************************************************
 * AXS15231B — Faster, logical landscape 480×320 (панель native 320×480 + Canvas rot 1/3).
 * VU: слоты как раньше (флаг AXS15231B_SPECTRUM_ENABLE false → сток).
 * Spectrum: раскладка как ILI9488 landscape + DIGI56 (не масштаб ST7789 DIGI42).
 * A/B portrait 320×480 — дрыганье не убрал; conf возвращён на landscape.
 *************************************************************************************/

#ifndef displayAXS15231Bconf_h
#define displayAXS15231Bconf_h

#define DSP_WIDTH       480
#define DSP_HEIGHT      320
#define TFT_FRAMEWDT    5
#define MAX_WIDTH       (DSP_WIDTH - TFT_FRAMEWDT * 2)

#if BITRATE_FULL
  #define TITLE_FIX 44
#else
  #define TITLE_FIX 0
#endif
#define bootLogoTop     42  /* pulse пик ~112% (H≈144); низ слота ≈186, до progress Y=216 */

/*
 * Playing+blank Cover-SS (480×320): слева слот обложки/лого, справа DS_DIGI42 mono + погода.
 * Слот 280×280; справа 200 px — «88:88» ≈185 px влезает по центру; прыжки только по Y
 * (X фиксируем по центру колонки — иначе при крупной обложке «ломаные» прыжки).
 * Без наложений на обложку / полосу факта (FACT_TOP=304).
 */
#define ST7789_COV_COVER_SIZE            280
#define ST7789_COV_COVER_X_LEFT          0
#define ST7789_COV_COVER_Y               ((DSP_HEIGHT - ST7789_COV_COVER_SIZE) / 2)
#define ST7789_COV_MAX_DECODE_DIM        512
#define ST7789_COV_FB_LEFT_W             ST7789_COV_COVER_SIZE
#define ST7789_COV_FB_RIGHT_X            ST7789_COV_COVER_SIZE
#define ST7789_COV_FB_RIGHT_W            (DSP_WIDTH - ST7789_COV_COVER_SIZE)
/* DIGI42: height≈52, yOffset≈−51 → верх глифа = top−51. */
#define ST7789_COV_SIDE_CLK_GLYPH_ABOVE  51
#define ST7789_COV_SIDE_CLK_MIN_TOP      56
/* Низ блока погоды ≈ top+GAP+WBLOCK; FACT_TOP=304 → потолок ~230; 196 — запас. */
#define ST7789_COV_SIDE_CLK_MAX_TOP      196
/* +5: низ clear ≈ baseline+1 — не лезть в погоду при мигании «:». */
#define ST7789_COV_SIDE_CLK_CLEAR_H      (ST7789_COV_SIDE_CLK_GLYPH_ABOVE + 5)
#define ST7789_COV_JUMP_Y_ONLY           1
/* AXS soft-rot: непрозрачный bg у print даёт «тени» на глифах — рисуем после fillRect. */
#define ST7789_COV_TEXT_OPAQUE_BG        0
#define ST7789_COV_WICON_W               48
#define ST7789_COV_WICON_H               48
#define ST7789_COV_USE_VC2_ICONS         1
/* Зазор часов→иконка: clear часов не должен пересекаться с bitmap погоды. */
#define ST7789_COV_WICON_GAP_BELOW       4
#define ST7789_COV_WICON_DESCENT         2
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
#define ST7789_COV_WTEMP_SHIFT_X         (-4)
#define ST7789_COV_SEAM_W                0
#define ST7789_COV_SIDE_SS_JUMP_MS       20000UL
#define ST7789_COV_COVER_RETRY_MS        5000UL
#define ST7789_COV_SMALL_CLOCK_FONT      ST240_SmallClockFont
#define ST7789_COV_LOGO_VER_TEXT_SIZE    1
#define ST7789_COV_LOGO_VER_GAP_PX       8
#define ST7789_COV_FACT_TEXT_SIZE        2
#define ST7789_COV_FACT_H                (ST7789_COV_FACT_TEXT_SIZE * 8)
#define ST7789_COV_FACT_TOP              (DSP_HEIGHT - ST7789_COV_FACT_H)
/* Faster AXS: мягче порог свайпа факта; шире hit-zone (калибровка/край JPEG). */
#define ST7789_COV_FACT_SWIPE_MIN_PX     24
#define ST7789_COV_FACT_SWIPE_PAD_PX     40
const ScrollConfig coverFactConf  PROGMEM = {{ TFT_FRAMEWDT, ST7789_COV_FACT_TOP, ST7789_COV_FACT_TEXT_SIZE, WA_LEFT }, 400, false, MAX_WIDTH, 1500, 3, 35 };

/* SROLLS — JC2432 SPECTRUM, масштаб под 480×320.
 * meta: в такт flush 50 мс — 6 px / 55 ms (быстрее 4/85; плавность упирается в soft-rot full frame). */
/* meta: буквы Y=9 (на 1 выше); полоса 38 — не лезет на title1/bitrate. */
const ScrollConfig metaConf       PROGMEM = {{ TFT_FRAMEWDT, 9, 3, WA_LEFT }, 140, true, MAX_WIDTH, 5000, 6, 55 };
/* title1: (5+24+4)×4/3≈44; title2: было 71, −2 px выше. */
const ScrollConfig title1Conf     PROGMEM = {{ TFT_FRAMEWDT, 45, 2, WA_LEFT }, 140, true, MAX_WIDTH - TITLE_FIX, 5000, 4, 30 };
const ScrollConfig title2Conf     PROGMEM = {{ TFT_FRAMEWDT, 69, 2, WA_LEFT }, 140, false, MAX_WIDTH - TITLE_FIX, 5000, 4, 30 };
/* PLAYER: бегущая строка факта/тоста между title2 (Y=69) и пирамидкой/часами.
 * Шаг title1→title2 = 24 → факт Y=93 (size 2 → низ 109); ниже — weather/clock +21.
 * Ширина MAX_WIDTH (без TITLE_FIX) — под битрейтом место не резервируем.
 * delta/time как meta (читаемо); не 12 — слишком быстро. */
#define HAS_PLAYER_FACT_SCROLL           1
const ScrollConfig playerFactConf PROGMEM = {{ TFT_FRAMEWDT, 93, 2, WA_LEFT }, 400, false, MAX_WIDTH, 1500, 4, 55 };
/* uppercase=false: имена станций как в CSV, без CAPS.
 * Соседи — textsize 2; фокус — playlistFocusConf (3), только центральная строка. */
const ScrollConfig playlistConf        PROGMEM = {{ TFT_FRAMEWDT, 149, 2, WA_LEFT }, 140, false, MAX_WIDTH, 1000, 2, 30 };
const ScrollConfig playlistFocusConf   PROGMEM = {{ TFT_FRAMEWDT, 149, 3, WA_LEFT }, 140, false, MAX_WIDTH, 1000, 2, 30 };
#define HAS_PLAYLIST_FOCUS_CONF 1
const ScrollConfig apTitleConf    PROGMEM = {{ TFT_FRAMEWDT, TFT_FRAMEWDT, 3, WA_CENTER }, 140, false, MAX_WIDTH, 0, 4, 20 };
const ScrollConfig apSettConf     PROGMEM = {{ TFT_FRAMEWDT, DSP_HEIGHT - TFT_FRAMEWDT - 16, 2, WA_LEFT }, 140, false, MAX_WIDTH, 0, 4, 30 };
#if SPECTRUM_ANALYZER_ACTIVE
/* Off-screen: lock()/setText fillRect не режет верх DIGI56 (glyphAbove≈70 от clockTop=203).
 * PLAY: пирамидка; STOP+FFT: сток-скролл погоды пока не двигаем (AXS_WEATHER_NO_MOVE). */
const ScrollConfig weatherConf    PROGMEM = {{ 8, DSP_HEIGHT + 16, 2, WA_LEFT }, 140, true, MAX_WIDTH, 0, 4, 30 };
#else
const ScrollConfig weatherConf    PROGMEM = {{ 8, 116, 2, WA_LEFT }, 140, true, MAX_WIDTH, 0, 4, 30 };
#endif

/* BACKGROUNDS */
const FillConfig   metaBGConf     PROGMEM = {{ 0, 0, 0, WA_LEFT }, DSP_WIDTH, 51, false };
/* INVERT false: узкая полоса 0…37 — ниже title1/bitrate, выше букв (Y=9, h≈24). */
const FillConfig   metaBGConfInv  PROGMEM = {{ 0, 0, 0, WA_LEFT }, DSP_WIDTH, 37, false };
const FillConfig   volbarConf     PROGMEM = {{ TFT_FRAMEWDT, DSP_HEIGHT - TFT_FRAMEWDT - 6, 0, WA_LEFT }, MAX_WIDTH, 6, true };
const FillConfig  playlBGConf     PROGMEM = {{ 0, 143, 0, WA_LEFT }, DSP_WIDTH, 36, false };
/* Буфер и CPU: по 1 px + зазор 1 px, как Supremus (низ: … / buf / gap / cpu). */
const FillConfig  heapbarConf     PROGMEM = {{ 0, DSP_HEIGHT - 3, 0, WA_LEFT }, DSP_WIDTH, 1, false };
const FillConfig  cpubarConf      PROGMEM = {{ 0, DSP_HEIGHT - 1, 0, WA_LEFT }, DSP_WIDTH, 1, false };

/* WIDGETS */
const WidgetConfig bootstrConf    PROGMEM = { 0, 243, 1, WA_CENTER };
const WidgetConfig bitrateConf    PROGMEM = { 105, 255, 1, WA_LEFT };
/* Футер ещё ниже; до volbar(309) зазор ~5 px. */
const WidgetConfig voltxtConf     PROGMEM = { 0, 296, 1, WA_CENTER };
const WidgetConfig  iptxtConf     PROGMEM = { TFT_FRAMEWDT, 296, 1, WA_LEFT };
const WidgetConfig   rssiConf     PROGMEM = { TFT_FRAMEWDT, 288, 2, WA_RIGHT };
const WidgetConfig numConf        PROGMEM = { 0, 200, 0, WA_CENTER };
const WidgetConfig apNameConf     PROGMEM = { TFT_FRAMEWDT, 88, 2, WA_CENTER };
const WidgetConfig apName2Conf    PROGMEM = { TFT_FRAMEWDT, 120, 2, WA_CENTER };
const WidgetConfig apPassConf     PROGMEM = { TFT_FRAMEWDT, 173, 2, WA_CENTER };
const WidgetConfig apPass2Conf    PROGMEM = { TFT_FRAMEWDT, 205, 2, WA_CENTER };
const WidgetConfig  clockConf     PROGMEM = { 8, 248, 0, WA_RIGHT };
/* VU всегда в conf; SpectrumWidget — только при SPECTRUM_ANALYZER_ACTIVE. */
const WidgetConfig vuConf         PROGMEM = { TFT_FRAMEWDT, 133, 1, WA_LEFT };

const WidgetConfig bootWdtConf    PROGMEM = { 0, 216, 1, WA_CENTER };
const ProgressConfig bootPrgConf  PROGMEM = { 90, 14, 4 };
const BitrateConfig fullbitrateConf PROGMEM = {{ DSP_WIDTH - 42, 40, 2, WA_LEFT }, 42 };

/* BANDS: классический VU (масштаб от JC2432) */
const VUBandsConfig bandsConf     PROGMEM = { 36, 133, 4, 2, 10, 2 };

#if SPECTRUM_ANALYZER_ACTIVE
/*
 * FFT: clockMove.top=203 (+21 под полосу факта Y=93); спектр Y=232 без сдвига.
 * Полосы заметно толще стока ILI9488 (bandGap 5/segGap 3 → bandGap 1 / segGap 1, H=50):
 *   ширина столбца ~+4 px; LED-сегмент 2→5 px. Низ спектра ≈282, футер Y=296.
 * PLAY: пирамидка слева (как ILI9488 DIGI56); moveTo скролла погоды — AXS_WEATHER_NO_MOVE.
 */
const WidgetConfig spectrumConf   PROGMEM = { TFT_FRAMEWDT, 232, 1, WA_LEFT };
const SpectrumConfig spectrumBandsConf PROGMEM = { MAX_WIDTH, 50, 12, 8, 1, 1, 40 };

/* PLAYER+PLAY: пирамидка (иконка→t°→город), общий st7789_player_weather.inc.
 * Слот как ILI9488 landscape (480×320 + DIGI56); длинный город — marquee в .inc.
 * Y=133: +21 от прежних 112 — зазор 24 px под низом факта (109). */
#define ST7789_PLR_VC2_64                1
#define ST7789_PLR_WICON_W               64
#define ST7789_PLR_WICON_H               64
#define ST7789_PLR_WTEMP_TEXT_SIZE       2
#define ST7789_PLR_WCITY_TEXT_SIZE       1
#define ST7789_PLR_ROW_GAP               2
#define ST7789_PLR_WEATHER_CLOCK_LEFT    130
#define ST7789_PLR_WEATHER_X             TFT_FRAMEWDT
#define ST7789_PLR_WEATHER_Y             133
#define ST7789_PLR_WEATHER_W             (ST7789_PLR_WEATHER_CLOCK_LEFT - ST7789_PLR_WEATHER_X)
#endif

/* STRINGS */
const char         numtxtFmt[]    PROGMEM = "%d";
const char           rssiFmt[]    PROGMEM = "WiFi %d";
const char          iptxtFmt[]    PROGMEM = "\010 %s";
const char         voltxtFmt[]    PROGMEM = "\023\025%d";
const char        bitrateFmt[]    PROGMEM = "%d kBs";

/* MOVES */
#if SPECTRUM_ANALYZER_ACTIVE
/*
 * PLAY: часы вверх под спектр (width≥0 → moveTo). VU-профиль: ветка #else, width=-1.
 * Погода: AXS_WEATHER_NO_MOVE — не moveTo (clear режет верх DIGI56).
 * top=203 (+21): верх глифа DIGI56 ≈133 — вровень с пирамидкой после полосы факта.
 */
const MoveConfig    clockMove     PROGMEM = { 8, 203, 0 };
#else
const MoveConfig    clockMove     PROGMEM = { 8, 248, -1 };
#endif
const MoveConfig   weatherMove    PROGMEM = { 8, 116, MAX_WIDTH };
const MoveConfig   weatherMoveVU  PROGMEM = { 105, 116, MAX_WIDTH - 105 + TFT_FRAMEWDT };

#endif
