/* Общая реализация cover-скринсейвера ST7789 (playing+blank).
 * Перед include: ST7789_COV_NS + макросы ST7789_COV_* в conf.h.
 */
#if !defined(ST7789_COV_USE_VC2_ICONS)
#define ST7789_COV_USE_VC2_ICONS 0
#endif
#if !defined(ST7789_COV_JUMP_Y_ONLY)
#define ST7789_COV_JUMP_Y_ONLY 0
#endif
/* 1 = print с bg (как раньше); 0 = только fg после fillRect (AXS soft-rot без «теней»). */
#if !defined(ST7789_COV_TEXT_OPAQUE_BG)
#define ST7789_COV_TEXT_OPAQUE_BG 1
#endif
#if ST7789_COV_USE_VC2_ICONS
/* Вне namespace: тот же header, что у st7789_player_weather.inc (один раз на TU). */
#include "fonts/weather_vc2_48.h"
#endif

namespace ST7789_COV_NS {

// --- JPEG / обложка ---
static int16_t g_coverDrawX = 0;
static int16_t g_coverDrawY = 0;
static int16_t g_coverBoundLeft = 0;
static int16_t g_coverBoundTop = 0;
static int16_t g_coverBoundRight = 0;
static int16_t g_coverBoundBottom = 0;
static bool g_coverDecoderReady = false;
static String g_lastDrawnCoverUrl = "";
static volatile bool g_coverInvalidateNeeded = false;
static String g_badCoverUrl = "";  /* legacy: сброс в reset/invalidate, blacklist больше не используем */
static uint8_t g_coverFailCount = 0;
static uint32_t g_lastCoverRetryMs = 0;
static bool g_leftHasCover = false;
static bool g_leftShowingLogo = false;
/* URL, для которого обложка уже успешно показана — не затирать при временном сбое FS */
static String g_stableCoverUrl = "";
static bool g_pendingCoverRedraw = false;

/* Макет playing+blank: слева обложка/лого, справа «гуляющие» часы */
static bool g_screenInitialized = false;
static char g_sideTimeBuf[12] = "";
static char g_sideTimeOld[12] = "";
static uint16_t g_sideClkTop = ST7789_COV_SIDE_CLK_MIN_TOP + 20;
/* Позиция X задаётся в resetSideClockLayout() — по центру правой колонки. */
static uint16_t g_sideClkLeft = ST7789_COV_FB_RIGHT_X;
static uint16_t g_sideClkColor = 0;
static uint16_t g_sideBaseClockColor = 0;
static uint16_t g_sideLastAppliedColor = 0;
static bool g_sideBaseColorCaptured = false;
static uint32_t g_lastJumpMs = 0;
static uint16_t g_sideClrLeft = 0;
static uint16_t g_sideClrTop = 0;
static uint16_t g_sideClrW = 0;
static uint16_t g_sideClrH = 0;
static int8_t g_lastColonVisible = -1;
/* Энкодер: после выхода из cover-скринсейвера блокируем один ложный переход в плейлист. */
static bool g_encoderNavGuardArmed = false;
/* Блок погоды под часами: кэш иконки/температуры/города и флаг «нужна перерисовка». */
static uint8_t g_lastWeatherIconIdx = 255;
static bool g_lastWeatherTempValid = false;
static int16_t g_lastWeatherTempRounded = 0;
static uint8_t g_lastWeatherTempBand = 0;
static char g_lastWeatherCityDrawn[60] = "";
static bool g_weatherIconDirty = false;

/* Бегущая строка города под погодой (не ScrollWidget — свой тик, ширина ≈ иконка+t°). */
static char g_cityScrollText[60] = "";
static bool g_cityNeedScroll = false;
static int16_t g_cityScrollOff = 0;
static uint16_t g_cityScrollTextW = 0;
static int16_t g_cityWinX = 0;
static int16_t g_cityWinW = 0;
static int16_t g_cityBaseY = 0;
static int16_t g_cityCenterX = 0;
static uint32_t g_cityScrollNextMs = 0;
static const uint16_t kCovCityScrollMs = 45;
static const uint16_t kCovCityScrollStartMs = 1500;
static const int16_t kCovCityScrollGapPx = 18;
/* Отступ окна города от шва обложки слева и от края TFT справа. */
static const int16_t kCovCityWinPadPx = 2;

/* Бегущая строка факта: штатный ScrollWidget (как weather/title), не самописный paint. */
static ScrollWidget *g_factScroll = nullptr;
static String g_factLastRaw;
static bool g_factScrollInited = false;
#if defined(HAS_PLAYER_FACT_SCROLL)
/* PLAYER Faster: отдельный виджет на Y=93 (playerFactConf); Cover-SS — по-прежнему g_factScroll. */
static ScrollWidget *g_playerFactScroll = nullptr;
static bool g_playerFactScrollInited = false;
#endif
/* Какой виджет сейчас ведёт факт/тост (смена PLAYER↔Cover — перепривязать текст). */
static ScrollWidget *g_factActiveW = nullptr;
/* Временный «тост» поверх факта (свайп вверх → «Запрос принят…»); приоритетнее currentFact. */
static String g_factStatusOverride;
/* 0=нет, иначе millis()-дедлайн для TIMED-тоста; для WAIT_FACT не используется как короткий таймер. */
static uint32_t g_factStatusUntilMs = 0;
/* true — не снимать тост по таймеру, только когда пришёл новый факт (или safety). */
static bool g_factStatusWaitForFact = false;
/* Страховка: если факт так и не пришёл (сеть/ошибка) — снять тост, не висеть вечно. */
static uint32_t g_factStatusSafetyUntilMs = 0;
/* Когда показали WAIT_FACT-тост — чтобы не снять его в тот же тик до установки pending. */
static uint32_t g_factStatusShownAtMs = 0;
/* Снимок currentFact в момент показа тоста — снимаем тост при изменении. */
static String g_factRawWhenStatusShown;
/* --- Трекинг свайпа вверх по обложке (пик хода, не точка отпускания) --- */
static bool g_coverSwipeArmed = false;
static uint16_t g_coverSwipeStartX = 0;
static uint16_t g_coverSwipeStartY = 0;
static int16_t g_coverSwipePeakDyUp = 0;
static int16_t g_coverSwipePeakDyAbs = 0; /* макс. |ΔY| — если «вверх» в координатах тача перепутан */
static int16_t g_coverSwipePeakDxAbs = 0;
static bool g_coverSwipeHwVertical = false; /* CST820: контроллер сам увидел SlideUp/Down */
/* Минимальный ход жеста (px). XPT2046 шумный и отдаёт редкие точки — порог ниже, чем у CST820. */
#ifndef ST7789_COV_FACT_SWIPE_MIN_PX
#if TS_MODEL==TS_MODEL_XPT2046
#define ST7789_COV_FACT_SWIPE_MIN_PX  22
#else
#define ST7789_COV_FACT_SWIPE_MIN_PX  36
#endif
#endif
/* Safety: максимум ожидания факта после успешной постановки запроса. */
#ifndef ST7789_COV_FACT_TOAST_SAFETY_MS
#define ST7789_COV_FACT_TOAST_SAFETY_MS  120000UL
#endif
/* Расширение hit-zone слота обложки (px) — палец часто стартует у края JPEG / с оффсетом калибровки. */
#ifndef ST7789_COV_FACT_SWIPE_PAD_PX
#if TS_MODEL==TS_MODEL_XPT2046
#define ST7789_COV_FACT_SWIPE_PAD_PX  36
#else
#define ST7789_COV_FACT_SWIPE_PAD_PX  12
#endif
#endif

static const uint16_t kSideClockPalette[] = {
  0, 0x07E0, 0xF800, 0x001F, 0xF81F, 0xFFE0, 0xFFFF, 0xFEA0
};

static bool coverOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);

static void ensureCoverDecoderReady() {
  if (g_coverDecoderReady) return;
  TJpgDec.setCallback(coverOutput);
  TJpgDec.setSwapBytes(false);
  g_coverDecoderReady = true;
}

static bool coverOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  int16_t x2 = x + (int16_t)w;
  int16_t y2 = y + (int16_t)h;
  int16_t drawL = max(x, g_coverBoundLeft);
  int16_t drawT = max(y, g_coverBoundTop);
  int16_t drawR = min(x2, g_coverBoundRight);
  int16_t drawB = min(y2, g_coverBoundBottom);
  if (drawR <= drawL || drawB <= drawT) return true;
  uint16_t clippedW = (uint16_t)(drawR - drawL);
  uint16_t clippedH = (uint16_t)(drawB - drawT);
  /* drawRGBBitmap считает stride = clippedW, а MCU от TJpgDec всегда упакован со stride = w.
   * При горизонтальном клипе (шов / край слота) один вызов читает строки «вперемешку» —
   * на TFT это выглядит как рваный/точечный правый край обложки. */
  if (clippedW == w && drawL == x) {
    /* Без горизонтального клипа: строки идут подряд, stride совпадает с clippedW. */
    uint16_t* src = bitmap + (drawT - y) * w;
    dsp.drawRGBBitmap(drawL, drawT, src, clippedW, clippedH);
  } else {
    /* Горизонтальный клип: по одной строке, шаг по исходному буферу = w. */
    for (uint16_t row = 0; row < clippedH; row++) {
      uint16_t* rowSrc = bitmap + (drawT - y + (int16_t)row) * w + (drawL - x);
      dsp.drawRGBBitmap(drawL, drawT + (int16_t)row, rowSrc, clippedW, 1);
    }
  }
  return true;
}

/* Разрешение URL /sc/... → путь в LittleFS (как NV3041A). */
static bool resolveCoverPath(const String& coverUrl, String &outPath) {
  if (!coverUrl.length() || !littleFsReady) return false;
  if (!coverUrl.startsWith("/sc/")) return false;
  String sub = coverUrl.substring(4);
  sub.replace("%20", " ");
  outPath = "/station_covers/" + sub;
  if (!lockLittleFS(200)) return false;
  bool exists = LittleFS.exists(outPath);
  unlockLittleFS();
  return exists;
}

/* Декод JPEG в квадратный слот cover-SS (cover-fill + клип по bounds).
 * TJpgDec без upscale; для ILI9488 качаем 300×300 — в слот 304 влезает целиком. */
static bool drawCoverInSlot(int16_t x, int16_t y, int16_t w, int16_t h, const String& coverUrl) {
  String fsPath;
  if (!resolveCoverPath(coverUrl, fsPath)) return false;

  ensureCoverDecoderReady();

  if (!lockLittleFS(500)) return false;

  File f = LittleFS.open(fsPath, "r");
  if (!f) {
    unlockLittleFS();
    return false;
  }
  size_t fSize = f.size();
  f.close();
  if (fSize < 100) {
    unlockLittleFS();
    return false;
  }

  uint16_t jpgW = 0, jpgH = 0;
  if (TJpgDec.getFsJpgSize(&jpgW, &jpgH, fsPath.c_str(), LittleFS) != JDR_OK) {
    unlockLittleFS();
    return false;
  }

  /* Потолок декода по heap (ST7789_COV_MAX_DECODE_DIM в conf.h), затем вписывание в слот. */
#ifndef ST7789_COV_MAX_DECODE_DIM
#define ST7789_COV_MAX_DECODE_DIM 512
#endif
  const uint16_t kMaxDecodeDim = ST7789_COV_MAX_DECODE_DIM;
  uint8_t scaleMin = 0;
  while ((jpgW >> scaleMin) > kMaxDecodeDim || (jpgH >> scaleMin) > kMaxDecodeDim) {
    if (scaleMin >= 3) break;
    scaleMin++;
  }

  /* contain → cover-fill (как до правки): слот заполнен, но для 300×300 в слоте 304
   * картинка целиком (iTunes 300 — см. upscaleItunesArtworkUrl для ILI9488). */
  uint8_t scale = scaleMin;
  uint16_t scaledW = jpgW >> scale;
  uint16_t scaledH = jpgH >> scale;
  bool coverFillsSlot = false;
  for (uint8_t s = scaleMin; s <= 3; ++s) {
    scaledW = jpgW >> s;
    scaledH = jpgH >> s;
    if (scaledW >= (uint16_t)w && scaledH >= (uint16_t)h) {
      scale = s;
      coverFillsSlot = true;
      break;
    }
  }
  if (!coverFillsSlot) {
    scale = scaleMin;
    scaledW = jpgW >> scale;
    scaledH = jpgH >> scale;
  }

  /* Центр слота: поля (letterbox) уже залиты fillRect в drawLeftCover. */
  g_coverDrawX = x + (int16_t)((int32_t)w - (int32_t)scaledW) / 2;
  g_coverDrawY = y + (int16_t)((int32_t)h - (int32_t)scaledH) / 2;
  g_coverBoundLeft = x;
  g_coverBoundTop = y;
  /* Клип только по слоту: MCU 16×16 могут на 1–2 px вылезти за scaledW — не затираем часы справа. */
  g_coverBoundRight = x + w;
  g_coverBoundBottom = y + h;

  TJpgDec.setJpgScale(1 << scale);
  /* Снимаем lock до drawFsJpg (как NV3041A): иначе CoverDL/TrackFacts ждут FS
   * сотни мс — декод «молчит», на экране только лого. */
  unlockLittleFS();
  JRESULT drawResult = TJpgDec.drawFsJpg(g_coverDrawX, g_coverDrawY, fsPath.c_str(), LittleFS);
  if (drawResult == JDR_OK) vTaskDelay(pdMS_TO_TICKS(1));
  return (drawResult == JDR_OK);
}

static GFXglyph *pgmGlyphSide(const GFXfont *gfxFont, uint8_t c) {
  return gfxFont->glyph + (c - 0x20);
}

static uint8_t sideCharWidth(unsigned char c) {
  GFXglyph* glyph = pgmGlyphSide(&ST7789_COV_SMALL_CLOCK_FONT, c);
  return pgm_read_byte(&glyph->xAdvance);
}

static uint16_t sideTextWidth(const char *txt) {
  uint16_t w = 0;
  uint16_t l = strlen(txt);
  for (uint16_t i = 0; i < l; i++) w += sideCharWidth(txt[i]);
  return w;
}

static void captureSideBaseColor() {
  if (g_sideBaseColorCaptured) return;
  g_sideBaseClockColor = config.theme.clock;
  g_sideLastAppliedColor = config.theme.clock;
  g_sideClkColor = config.theme.clock;
  g_sideBaseColorCaptured = true;
}

static uint16_t sideClockMaxWidth() {
  /* «88:88» — широкие цифры; + запас под выступ глифов за xAdvance */
  return sideTextWidth("88:88") + 10;
}

/* Фактическая ширина текущих HH:MM (для центрирования, не по «88:88»). */
static uint16_t sideClockLiveWidth(const char *hourBuf, const char *minBuf) {
  return (uint16_t)(sideTextWidth(hourBuf) + sideCharWidth(':') + sideTextWidth(minBuf));
}

/* Геометрический центр правой колонки: ровно между обложкой и правым краем TFT. */
static uint16_t sideClockCenteredLeft(uint16_t tw) {
  const int32_t panelL = (int32_t)ST7789_COV_FB_RIGHT_X;
  const int32_t panelR = panelL + (int32_t)ST7789_COV_FB_RIGHT_W;
  if (tw >= ST7789_COV_FB_RIGHT_W) return (uint16_t)panelL;
  const int32_t centerX = panelL + (int32_t)ST7789_COV_FB_RIGHT_W / 2;
  int32_t left = centerX - (int32_t)tw / 2;
  if (left < panelL) left = panelL;
  if (left + (int32_t)tw > panelR) left = panelR - (int32_t)tw;
  return (uint16_t)left;
}

static void pickNextSideClockColor() {
  captureSideBaseColor();
  const uint8_t n = sizeof(kSideClockPalette) / sizeof(kSideClockPalette[0]);
  uint16_t next = g_sideLastAppliedColor;
  /* Индекс 0 = базовый цвет темы — в прыжке берём только яркие цвета палитры (1..n-1) */
  for (uint8_t attempt = 0; attempt < 12; ++attempt) {
    uint8_t idx = static_cast<uint8_t>(random(1, n));
    uint16_t cand = kSideClockPalette[idx];
    if (cand != g_sideLastAppliedColor) {
      next = cand;
      break;
    }
  }
  if (next == g_sideLastAppliedColor) {
    for (uint8_t idx = 1; idx < n; ++idx) {
      if (kSideClockPalette[idx] != g_sideLastAppliedColor) {
        next = kSideClockPalette[idx];
        break;
      }
    }
  }
  g_sideClkColor = next;
  g_sideLastAppliedColor = next;
}

static void clampSideClockPosition() {
  char hourBuf[4];
  char minBuf[4];
  strftime(hourBuf, sizeof(hourBuf), "%H", &network.timeinfo);
  strftime(minBuf, sizeof(minBuf), "%M", &network.timeinfo);
  const uint16_t tw = sideClockLiveWidth(hourBuf, minBuf);
  const uint16_t minL = ST7789_COV_FB_RIGHT_X + ST7789_COV_SIDE_CLK_MARGIN_L;
  uint16_t maxL = ST7789_COV_FB_RIGHT_X + ST7789_COV_FB_RIGHT_W - tw - ST7789_COV_SIDE_CLK_MARGIN_R;
  if (maxL < minL) maxL = minL;
  if (g_sideClkLeft < minL) g_sideClkLeft = minL;
  if (g_sideClkLeft > maxL) g_sideClkLeft = maxL;
}

static void resetSideClockLayout() {
  char hourBuf[4];
  char minBuf[4];
  strftime(hourBuf, sizeof(hourBuf), "%H", &network.timeinfo);
  strftime(minBuf, sizeof(minBuf), "%M", &network.timeinfo);
  const uint16_t tw = sideClockLiveWidth(hourBuf, minBuf);
  g_sideClkLeft = sideClockCenteredLeft(tw);
  g_sideClkTop = (ST7789_COV_SIDE_CLK_MIN_TOP + ST7789_COV_SIDE_CLK_MAX_TOP) / 2;
  clampSideClockPosition();
}

static void sideClockJump(uint32_t now) {
  pickNextSideClockColor();
  char hourBuf[4];
  char minBuf[4];
  strftime(hourBuf, sizeof(hourBuf), "%H", &network.timeinfo);
  strftime(minBuf, sizeof(minBuf), "%M", &network.timeinfo);
#if ST7789_COV_JUMP_Y_ONLY
  /* Крупная обложка → узкая колонка: X по центру, прыжок только по Y (без наложений). */
  const uint16_t tw = sideClockLiveWidth(hourBuf, minBuf);
  g_sideClkLeft = sideClockCenteredLeft(tw);
#else
  const uint16_t tw = sideClockMaxWidth();
  const uint16_t minLeft = ST7789_COV_FB_RIGHT_X + ST7789_COV_SIDE_CLK_MARGIN_L;
  uint16_t maxLeft = ST7789_COV_FB_RIGHT_X + ST7789_COV_FB_RIGHT_W - tw - ST7789_COV_SIDE_CLK_MARGIN_R;
  if (maxLeft <= minLeft) g_sideClkLeft = minLeft;
  else g_sideClkLeft = (uint16_t)random(minLeft, maxLeft + 1);
#endif
  if (ST7789_COV_SIDE_CLK_MAX_TOP > ST7789_COV_SIDE_CLK_MIN_TOP) {
    g_sideClkTop = (uint16_t)random(ST7789_COV_SIDE_CLK_MIN_TOP, ST7789_COV_SIDE_CLK_MAX_TOP + 1);
  }
  clampSideClockPosition();
  g_lastJumpMs = now;
}

/* Полная очистка правой колонки — только при прыжке (убирает артефакты глифов).
 * Нижнюю полосу факта (ST7789_COV_FACT_TOP..) не трогаем — иначе бегущая строка мигает. */
static void clearRightPanelForClockJump() {
  const int16_t clearH = (ST7789_COV_FACT_TOP > 0) ? (int16_t)ST7789_COV_FACT_TOP : (int16_t)DSP_HEIGHT;
  dsp.fillRect(ST7789_COV_FB_RIGHT_X, 0, ST7789_COV_FB_RIGHT_W, clearH, config.theme.background);
}

/* Опциональный шов: затираем SEAM_W px у правого края обложки (если SEAM_W>0).
 * При SEAM_W=0 — no-op: рамка обложки сохраняется; вылет глифов часов
 * компенсируется ST7789_COV_SIDE_CLK_MARGIN_L. */
static void repairCoverClockSeam() {
  if (ST7789_COV_SEAM_W <= 0) return;
  const int16_t seamX = (int16_t)(ST7789_COV_FB_LEFT_W - ST7789_COV_SEAM_W);
  if (seamX < 0) return;
  dsp.fillRect(seamX, 0, ST7789_COV_SEAM_W, DSP_HEIGHT, config.theme.background);
}

static void updateSideClearRect() {
  const uint16_t tw = sideTextWidth(g_sideTimeBuf);
  g_sideClrTop = (g_sideClkTop > ST7789_COV_SIDE_CLK_GLYPH_ABOVE)
      ? (uint16_t)(g_sideClkTop - ST7789_COV_SIDE_CLK_GLYPH_ABOVE - 4) : 0;
  g_sideClrH = ST7789_COV_SIDE_CLK_CLEAR_H;
  g_sideClrLeft = (g_sideClkLeft > ST7789_COV_SIDE_CLK_MARGIN_L)
      ? (uint16_t)(g_sideClkLeft - ST7789_COV_SIDE_CLK_MARGIN_L) : ST7789_COV_FB_RIGHT_X;
  g_sideClrW = tw + ST7789_COV_SIDE_CLK_MARGIN_L + ST7789_COV_SIDE_CLK_MARGIN_R;
  if (g_sideClrLeft < ST7789_COV_FB_RIGHT_X) g_sideClrLeft = ST7789_COV_FB_RIGHT_X;
  if (g_sideClrLeft + g_sideClrW > DSP_WIDTH) {
    g_sideClrW = DSP_WIDTH - g_sideClrLeft;
  }
}

static void clearSideClockArea() {
  if (g_sideClrW == 0) return;
  dsp.fillRect(g_sideClrLeft, g_sideClrTop, g_sideClrW, g_sideClrH, config.theme.background);
}

/* Цвет цифр Cover-SS: при OPAQUE_BG=0 — только fg (фон уже залит fillRect). */
static void sideClkSetColor(uint16_t fg) {
#if ST7789_COV_TEXT_OPAQUE_BG
  dsp.setTextColor(fg, config.theme.background);
#else
  dsp.setTextColor(fg);
#endif
}

static void drawSideClockDigits() {
  clampSideClockPosition();
  dsp.setTextSize(1);
  dsp.setFont(&ST7789_COV_SMALL_CLOCK_FONT);
  char hourBuf[4];
  char minBuf[4];
  strftime(hourBuf, sizeof(hourBuf), "%H", &network.timeinfo);
  strftime(minBuf, sizeof(minBuf), "%M", &network.timeinfo);
  const uint16_t dotsLeft = sideTextWidth(hourBuf);
  const bool dotsOn = (network.timeinfo.tm_sec % 2 == 0);

  sideClkSetColor(g_sideClkColor);
  dsp.setCursor(g_sideClkLeft, g_sideClkTop);
  dsp.print(hourBuf);
  /* Выключенное «:» — заливка слота, не «прозрачная» печать цветом фона (иначе тени на AXS). */
  if (dotsOn) {
    sideClkSetColor(g_sideClkColor);
    dsp.setCursor(g_sideClkLeft + dotsLeft, g_sideClkTop);
    dsp.print(":");
  } else {
    const uint16_t colonTop = (g_sideClkTop > ST7789_COV_SIDE_CLK_GLYPH_ABOVE)
        ? (uint16_t)(g_sideClkTop - ST7789_COV_SIDE_CLK_GLYPH_ABOVE - 2) : 0;
    dsp.fillRect(g_sideClkLeft + dotsLeft, colonTop, sideCharWidth(':'),
                 ST7789_COV_SIDE_CLK_CLEAR_H, config.theme.background);
  }
  sideClkSetColor(g_sideClkColor);
  dsp.setCursor(g_sideClkLeft + dotsLeft + sideCharWidth(':'), g_sideClkTop);
  dsp.print(minBuf);
  dsp.setFont();
  g_lastColonVisible = dotsOn ? 1 : 0;
  updateSideClearRect();
}

/* Мигание «:» — clear слота + обе цифры MM (не только первую — иначе артефакты на «1»). */
static void redrawSideColonOnly() {
  char hourBuf[4];
  char minBuf[4];
  strftime(hourBuf, sizeof(hourBuf), "%H", &network.timeinfo);
  strftime(minBuf, sizeof(minBuf), "%M", &network.timeinfo);
  const uint16_t dotsLeft = sideTextWidth(hourBuf);
  const uint16_t colonX = g_sideClkLeft + dotsLeft;
  const uint16_t colonSlotW = sideCharWidth(':');
  const uint16_t minX = colonX + colonSlotW;
  const uint16_t minW = sideTextWidth(minBuf);
  const uint16_t colonTop = (g_sideClkTop > ST7789_COV_SIDE_CLK_GLYPH_ABOVE)
      ? (uint16_t)(g_sideClkTop - ST7789_COV_SIDE_CLK_GLYPH_ABOVE - 2) : 0;
  /* Стираем «:» + MM целиком — иначе при mono/смене ширины остаются тени. */
  dsp.fillRect(colonX, colonTop, (uint16_t)(colonSlotW + minW + 2), ST7789_COV_SIDE_CLK_CLEAR_H,
               config.theme.background);
  dsp.setTextSize(1);
  dsp.setFont(&ST7789_COV_SMALL_CLOCK_FONT);
  const bool dotsOn = (network.timeinfo.tm_sec % 2 == 0);
  if (dotsOn) {
    sideClkSetColor(g_sideClkColor);
    dsp.setCursor(colonX, g_sideClkTop);
    dsp.print(":");
  }
  sideClkSetColor(g_sideClkColor);
  dsp.setCursor(minX, g_sideClkTop);
  dsp.print(minBuf);
  dsp.setFont();
  g_lastColonVisible = dotsOn ? 1 : 0;
}

/* --- Блок погоды под «прыгающими» часами: иконка, температура, город --- */

/* Показываем блок, если погода включена и OWM уже отдал хотя бы icon или city. */
static bool shouldShowSideWeatherBlock() {
  return config.store.showweather
      && strlen(config.store.weatherkey) > 0
      && (timekeeper.weatherIcon[0] != '\0'
          || timekeeper.weatherCity[0] != '\0'
          || timekeeper.weatherTempValid);
}

/* Код OpenWeatherMap "01d"…"50n" → индекс иконки. */
static uint8_t owmIconToIndex(const char *icon) {
  if (!icon || icon[0] < '0' || icon[1] < '0') return 255;
  const int code = (icon[0] - '0') * 10 + (icon[1] - '0');
#if ST7789_COV_USE_VC2_ICONS
  /* Как у PLAYER: Visual Crossing 2nd Color, день/ночь. */
  const bool night = (icon[2] == 'n' || icon[2] == 'N');
  switch (code) {
    case 1:  return night ? 1u : 0u;
    case 2:  return night ? 3u : 2u;
    case 3:  return 4u;
    case 4:  return 4u;
    case 9:  return night ? 6u : 5u;
    case 10: return 7u;
    case 11: return 8u;
    case 13: return 9u;
    case 50: return 10u;
    default: return 4u;
  }
#else
  switch (code) {
    case 1:  return 0; /* ясно */
    case 2:  return 1; /* малооблачно */
    case 3:  return 2; /* облачно */
    case 4:  return 3; /* пасмурно */
    case 9:  return 4; /* ливень */
    case 10: return 5; /* дождь */
    case 11: return 6; /* гроза */
    case 13: return 7; /* снег */
    case 50: return 8; /* туман */
    default: return 3;
  }
#endif
}

/* Город в виде «Москва»: первая буква заглавная, остальные строчные (CP1251 после utf8Rus). */
static char *sideCityTitleLabel(const char *owmCity) {
  static char buf[60];
  strlcpy(buf, utf8Rus(owmCity, false), sizeof(buf));
  if (buf[0] != '\0') {
    const uint8_t c = (uint8_t)buf[0];
    if (c >= 0xE0 && c <= 0xFF) {
      buf[0] = (char)(c - 0x20);
    } else if (c == 0xB8) {
      buf[0] = (char)0xA8; /* ё → Ё */
    } else if (c >= 'a' && c <= 'z') {
      buf[0] = (char)toupper(c);
    }
  }
  return buf;
}

/* Строка температуры «-8°C» (целые градусы со знаком). */
static const char *sideTempLabel() {
  static char buf[16];
  if (!timekeeper.weatherTempValid) {
    buf[0] = '\0';
    return buf;
  }
  /* \260 — символ градуса в CP1251 (как в LANG::weatherFmt). */
  snprintf(buf, sizeof(buf), "%+.0f\260C", (double)timekeeper.weatherTemp);
  return buf;
}

/* Ширина строки default-шрифта через getTextBounds (корректно для кириллицы). */
static uint16_t sideGfxTextWidth(const char *txt) {
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  dsp.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  return w;
}

/* X курсора для центрирования строки относительно anchorCenterX. */
static int16_t sideGfxTextCenterX(int16_t anchorCenterX, const char *txt) {
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  dsp.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  return anchorCenterX - (int16_t)(w / 2) - x1;
}

/* Y baseline default-шрифта, чтобы вертикальный центр строки совпал с centerY. */
static int16_t sideGfxBaselineForCenterY(int16_t centerY, const char *txt, uint8_t textSize) {
  dsp.setTextSize(textSize);
  dsp.setFont();
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  dsp.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  return centerY - (int16_t)y1 - (int16_t)(h / 2u);
}

/* Цвет температуры по диапазонам (cover-скринсейвер): приоритет от более высокой t. */
static uint8_t sideWeatherTempBand(float tempC) {
  if (tempC > 30.0f) return 5;   /* > +30 — красный */
  if (tempC > 18.0f) return 4;   /* +18…+30 — оранжевый */
  if (tempC >= 5.0f) return 3;   /* +5…+18 — жёлтый */
  if (tempC >= -5.0f) return 2;  /* -5…+5 — голубой */
  return 1;                      /* < -5 — синий */
}

static uint16_t sideWeatherTempColor(float tempC) {
  switch (sideWeatherTempBand(tempC)) {
    case 5: return 0xF800; /* красный */
    case 4: return 0xFD20; /* оранжевый */
    case 3: return 0xFFE0; /* жёлтый */
    case 2: return 0x07FF; /* голубой */
    default: return 0x001F; /* синий */
  }
}

/* Геометрия блока погоды: иконка и город — под HH, температура — под MM. */
struct SideWeatherLayout {
  int16_t blockX;
  int16_t blockY;
  int16_t blockW;
  int16_t blockH;
  int16_t iconX;
  int16_t iconY;
  int16_t tempCenterX;
  int16_t tempBaselineY;
  int16_t cityCenterX;
  int16_t cityBaselineY;
};

static void sideWeatherLayout(SideWeatherLayout &layout) {
  char hourBuf[4];
  char minBuf[4];
  strftime(hourBuf, sizeof(hourBuf), "%H", &network.timeinfo);
  strftime(minBuf, sizeof(minBuf), "%M", &network.timeinfo);
  const uint16_t hourW = sideTextWidth(hourBuf);
  const uint16_t colonW = sideCharWidth(':');
  const uint16_t minW = sideTextWidth(minBuf);
  const uint16_t clockW = hourW + colonW + minW;
  const int16_t minStartX = (int16_t)g_sideClkLeft + (int16_t)hourW + (int16_t)colonW;

  /* Центры цифр HH, MM и всей строки HH:MM. */
  const int16_t hourCenterX = (int16_t)g_sideClkLeft + (int16_t)(hourW / 2u);
  const int16_t minCenterX = minStartX + (int16_t)(minW / 2u);
  const int16_t clockCenterX = (int16_t)g_sideClkLeft + (int16_t)(clockW / 2u);

  /* Иконка — по центру под HH + сдвиг к обложке (город не трогаем). */
  layout.iconX = hourCenterX - (int16_t)(ST7789_COV_WICON_W / 2) + (int16_t)ST7789_COV_WICON_SHIFT_X;
  layout.iconY = (int16_t)(g_sideClkTop + ST7789_COV_WICON_GAP_BELOW + ST7789_COV_WICON_DESCENT);

  /* Температура — по центру под MM + горизонтальный сдвиг к иконке. */
#if ST7789_COV_USE_VC2_ICONS
  /* Вертикаль = середина bitmap 48×48; +2 px вниз — ближе к центру значка визуально. */
  const int16_t tempRowCenterY = layout.iconY + (int16_t)(ST7789_COV_WICON_H / 2u) + 2;
#else
  /* Offset 6 = прежние 7 минус 1 px вверх. */
  const int16_t tempRowCenterY =
      (int16_t)(g_sideClkTop + 6 + (ST7789_COV_WICON_H / 2u));
#endif
  layout.tempCenterX = minCenterX + (int16_t)ST7789_COV_WTEMP_SHIFT_X;
  if (timekeeper.weatherTempValid) {
    layout.tempBaselineY = sideGfxBaselineForCenterY(tempRowCenterY, sideTempLabel(),
                                                     ST7789_COV_WTEMP_TEXT_SIZE);
  } else {
    layout.tempBaselineY = tempRowCenterY;
  }

  /* Город — по центру HH:MM; −6 px вверх к иконке/t° (ещё −3 после полевого теста). */
  layout.cityCenterX = clockCenterX;
  const int16_t cityRowCenterY = layout.iconY + (int16_t)ST7789_COV_WICON_H
                               + (int16_t)ST7789_COV_WCITY_GAP + (int16_t)(ST7789_COV_WCITY_H / 2u)
                               - 6;
  if (timekeeper.weatherCity[0] != '\0') {
    layout.cityBaselineY = sideGfxBaselineForCenterY(cityRowCenterY,
        sideCityTitleLabel(timekeeper.weatherCity), ST7789_COV_WCITY_TEXT_SIZE);
  } else {
    layout.cityBaselineY = cityRowCenterY;
  }

  layout.blockY = layout.iconY;
  layout.blockH = ST7789_COV_WICON_H + ST7789_COV_WCITY_GAP + ST7789_COV_WCITY_H;

  /* Зона очистки: только иконка + t° (не вся ширина длинного города — иначе лезем на обложку).
   * Город — отдельное окно-marquee в правой колонке. */
  int16_t blockLeft = layout.iconX;
  int16_t blockRight = layout.iconX + (int16_t)ST7789_COV_WICON_W;

  if (timekeeper.weatherTempValid) {
    dsp.setTextSize(ST7789_COV_WTEMP_TEXT_SIZE);
    dsp.setFont();
    const char *tempLabel = sideTempLabel();
    const uint16_t tempW = sideGfxTextWidth(tempLabel);
    const int16_t tempLeft = sideGfxTextCenterX(layout.tempCenterX, tempLabel);
    const int16_t tempRight = tempLeft + (int16_t)tempW;
    if (tempLeft < blockLeft) blockLeft = tempLeft;
    if (tempRight > blockRight) blockRight = tempRight;
    const int16_t tempBottom = layout.tempBaselineY + 2;
    if (tempBottom > layout.blockY + layout.blockH) {
      layout.blockH = tempBottom - layout.blockY;
    }
  }
  if (timekeeper.weatherCity[0] != '\0') {
    /* Высоту блока под город расширяем; по X — не раздуваем (marquee). */
    const int16_t cityBottom = layout.cityBaselineY + (int16_t)(8 * ST7789_COV_WCITY_TEXT_SIZE);
    if (cityBottom > layout.blockY + layout.blockH) {
      layout.blockH = cityBottom - layout.blockY;
    }
  }

  layout.blockX = blockLeft - 2;
  layout.blockW = blockRight - blockLeft + 4;
  /* Жёстко в правой колонке — никогда не заходить на обложку (X < FB_RIGHT_X). */
  if (layout.blockX < (int16_t)ST7789_COV_FB_RIGHT_X) {
    layout.blockW -= (int16_t)ST7789_COV_FB_RIGHT_X - layout.blockX;
    layout.blockX = (int16_t)ST7789_COV_FB_RIGHT_X;
  }
  if (layout.blockW < 0) layout.blockW = 0;
  if (layout.blockX + layout.blockW > (int16_t)DSP_WIDTH) {
    layout.blockW = (int16_t)DSP_WIDTH - layout.blockX;
  }
}

static void clearSideWeatherBlock(const SideWeatherLayout &layout) {
  dsp.fillRect(layout.blockX, layout.blockY, layout.blockW, layout.blockH, config.theme.background);
}

#if ST7789_COV_USE_VC2_ICONS
/* Bitmap VC2 48×48 — тот же набор, что у пирамидки PLAYER. */
static void drawWeatherIconKind(int16_t ox, int16_t oy, uint8_t kind) {
  if (kind >= WEATHER_VC2_ICON_COUNT) return;
#if DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  dsp.drawRGBBitmap(ox, oy, const_cast<uint16_t*>(weather_vc2_icons[kind]),
                    WEATHER_VC2_W, WEATHER_VC2_H);
#else
  dsp.drawRGBBitmap(ox, oy, weather_vc2_icons[kind], WEATHER_VC2_W, WEATHER_VC2_H);
#endif
}
#else
/* Координаты пиктограмм заданы под макет 24×24; масштабируем к ST7789_COV_WICON_W. */
static inline int16_t wiconCoord(int16_t v24) {
  return (int16_t)((v24 * (int32_t)ST7789_COV_WICON_W + 12) / 24);
}

/* Облако из кругов + «тело» — базовый элемент большинства пиктограмм. */
static void drawWeatherCloudBlob(int16_t ox, int16_t oy, uint16_t col) {
  dsp.fillCircle(ox + wiconCoord(8), oy + wiconCoord(10), wiconCoord(5), col);
  dsp.fillCircle(ox + wiconCoord(14), oy + wiconCoord(9), wiconCoord(6), col);
  dsp.fillCircle(ox + wiconCoord(19), oy + wiconCoord(11), wiconCoord(4), col);
  dsp.fillRect(ox + wiconCoord(4), oy + wiconCoord(11), wiconCoord(17), wiconCoord(6), col);
}

/* Отрисовка одной из 9 пиктограмм в слоте ST7789_COV_WICON_W × ST7789_COV_WICON_H. */
static void drawWeatherIconKind(int16_t ox, int16_t oy, uint8_t kind) {
  const uint16_t sunCol = 0xFE60;
  const uint16_t cloudLt = 0xC618;
  const uint16_t cloudDk = 0x9CD1;
  const uint16_t rainCol = 0x3D7F;
  const uint16_t boltCol = 0xFEE0;
  const uint16_t snowCol = 0xFFFF;
  const uint16_t mistCol = 0xB596;
  const int16_t cx = ox + wiconCoord(12);
  const int16_t cy = oy + wiconCoord(10);

  switch (kind) {
    case 0:
      dsp.fillCircle(cx, cy, wiconCoord(5), sunCol);
      {
        static const int8_t kRayDx[] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        static const int8_t kRayDy[] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        for (int8_t i = 0; i < 8; ++i) {
          dsp.drawLine(cx + kRayDx[i] * wiconCoord(7), cy + kRayDy[i] * wiconCoord(7),
                       cx + kRayDx[i] * wiconCoord(10), cy + kRayDy[i] * wiconCoord(10), sunCol);
        }
      }
      break;
    case 1:
      dsp.fillCircle(ox + wiconCoord(8), oy + wiconCoord(7), wiconCoord(4), sunCol);
      drawWeatherCloudBlob(ox + wiconCoord(2), oy + wiconCoord(8), cloudLt);
      break;
    case 2:
      drawWeatherCloudBlob(ox + wiconCoord(1), oy + wiconCoord(6), cloudLt);
      break;
    case 3:
      drawWeatherCloudBlob(ox + 0, oy + wiconCoord(5), cloudDk);
      break;
    case 4:
      drawWeatherCloudBlob(ox + wiconCoord(1), oy + wiconCoord(4), cloudDk);
      for (int16_t x : { (int16_t)(ox + wiconCoord(8)), (int16_t)(ox + wiconCoord(13)), (int16_t)(ox + wiconCoord(18)) }) {
        dsp.drawLine(x, oy + wiconCoord(16), x, oy + wiconCoord(21), rainCol);
      }
      break;
    case 5:
      drawWeatherCloudBlob(ox + wiconCoord(1), oy + wiconCoord(3), cloudDk);
      for (int16_t x : { (int16_t)(ox + wiconCoord(7)), (int16_t)(ox + wiconCoord(12)), (int16_t)(ox + wiconCoord(17)) }) {
        dsp.drawLine(x, oy + wiconCoord(15), x + wiconCoord(1), oy + wiconCoord(22), rainCol);
      }
      break;
    case 6:
      drawWeatherCloudBlob(ox + wiconCoord(1), oy + wiconCoord(3), cloudDk);
      dsp.fillTriangle(ox + wiconCoord(14), oy + wiconCoord(14), ox + wiconCoord(11), oy + wiconCoord(18),
                       ox + wiconCoord(13), oy + wiconCoord(18), boltCol);
      dsp.fillTriangle(ox + wiconCoord(13), oy + wiconCoord(18), ox + wiconCoord(10), oy + wiconCoord(23),
                       ox + wiconCoord(16), oy + wiconCoord(17), boltCol);
      break;
    case 7:
      drawWeatherCloudBlob(ox + wiconCoord(1), oy + wiconCoord(3), cloudLt);
      for (int16_t cx2 : { (int16_t)(ox + wiconCoord(8)), (int16_t)(ox + wiconCoord(13)), (int16_t)(ox + wiconCoord(18)) }) {
        const int16_t cy2 = oy + wiconCoord(18);
        dsp.drawLine(cx2 - wiconCoord(2), cy2, cx2 + wiconCoord(2), cy2, snowCol);
        dsp.drawLine(cx2, cy2 - wiconCoord(2), cx2, cy2 + wiconCoord(2), snowCol);
      }
      break;
    default:
      for (int8_t row = 0; row < 3; ++row) {
        const int16_t y = oy + wiconCoord(7) + row * wiconCoord(5);
        const int16_t w = wiconCoord(14) - row * wiconCoord(2);
        dsp.fillRect(ox + (ST7789_COV_WICON_W - w) / 2, y, w, wiconCoord(2), mistCol);
      }
      break;
  }
}
#endif /* ST7789_COV_USE_VC2_ICONS */

/* Температура «+24°C» под MM, цвет — по диапазону t. */
static void drawSideWeatherTemp(const SideWeatherLayout &layout) {
  if (!timekeeper.weatherTempValid) return;
  const char *tempLabel = sideTempLabel();
  if (tempLabel[0] == '\0') return;
  dsp.setTextSize(ST7789_COV_WTEMP_TEXT_SIZE);
  dsp.setFont();
#if ST7789_COV_TEXT_OPAQUE_BG
  dsp.setTextColor(sideWeatherTempColor(timekeeper.weatherTemp), config.theme.background);
#else
  /* После clearSideWeatherBlock — без bg-заливки глифов (тени на «1»/«4» на AXS). */
  dsp.setTextColor(sideWeatherTempColor(timekeeper.weatherTemp));
#endif
  const int16_t tx = sideGfxTextCenterX(layout.tempCenterX, tempLabel);
  dsp.setCursor(tx, layout.tempBaselineY);
  dsp.print(tempLabel);
}

/* Название города: короткое — по центру; длинное — бегущая в окне block (иконка+t°). */
static void clearCovCityMarqueeState() {
  g_cityScrollText[0] = '\0';
  g_cityNeedScroll = false;
  g_cityScrollOff = 0;
  g_cityScrollTextW = 0;
  g_cityWinX = 0;
  g_cityWinW = 0;
  g_cityBaseY = 0;
  g_cityCenterX = 0;
  g_cityScrollNextMs = 0;
}

static void clampCovCityWin(int16_t &winX, int16_t &winW) {
  const int16_t colL = (int16_t)ST7789_COV_FB_RIGHT_X + kCovCityWinPadPx;
  const int16_t colR = (int16_t)DSP_WIDTH - kCovCityWinPadPx;
  if (winX < colL) {
    winW -= (colL - winX);
    winX = colL;
  }
  if (winW < 0) winW = 0;
  if (winX + winW > colR) winW = colR - winX;
  if (winW < 0) winW = 0;
}

static void paintCovCityMarquee() {
  if (g_cityScrollText[0] == '\0' || g_cityWinW <= 0) return;
  int16_t winX = g_cityWinX;
  int16_t winW = g_cityWinW;
  clampCovCityWin(winX, winW);
  if (winW <= 0) return;

  const uint8_t ts = ST7789_COV_WCITY_TEXT_SIZE;
  const uint16_t th = (uint16_t)(8u * ts);
  dsp.setTextSize(ts);
  dsp.setFont();
  /* Сначала clip: fillRect/глифы не должны рисовать на обложку / за правый край.
   * Текст без второго цвета фона — иначе writeFillRect глифа при x<clip даёт артефакты. */
  dsp.setClipping({ (uint16_t)winX, (uint16_t)g_cityBaseY, (uint16_t)winW, th });
  dsp.fillRect(winX, g_cityBaseY, winW, th, config.theme.background);
  dsp.setTextColor(config.theme.weathercity);
  if (!g_cityNeedScroll) {
    int16_t tx = sideGfxTextCenterX(g_cityCenterX, g_cityScrollText);
    if (tx < winX) tx = winX;
    dsp.setCursor(tx, g_cityBaseY);
    dsp.print(g_cityScrollText);
  } else {
    const int16_t cycle = (int16_t)g_cityScrollTextW + kCovCityScrollGapPx;
    dsp.setCursor(winX - g_cityScrollOff, g_cityBaseY);
    dsp.print(g_cityScrollText);
    dsp.setCursor(winX - g_cityScrollOff + cycle, g_cityBaseY);
    dsp.print(g_cityScrollText);
  }
  dsp.clearClipping();
}

static void setupCovCityMarquee(const SideWeatherLayout &layout, const char *cityDrawn) {
  strlcpy(g_cityScrollText, cityDrawn, sizeof(g_cityScrollText));
  /* Окно = block (иконка+t°) + pad от обложки и правого края. */
  g_cityWinX = layout.blockX;
  g_cityWinW = layout.blockW;
  clampCovCityWin(g_cityWinX, g_cityWinW);
  g_cityBaseY = layout.cityBaselineY;
  g_cityCenterX = layout.cityCenterX;
  if (g_cityScrollText[0] == '\0' || g_cityWinW <= 0) {
    clearCovCityMarqueeState();
    return;
  }
  dsp.setTextSize(ST7789_COV_WCITY_TEXT_SIZE);
  dsp.setFont();
  g_cityScrollTextW = sideGfxTextWidth(g_cityScrollText);
  g_cityNeedScroll = (g_cityScrollTextW > (uint16_t)g_cityWinW);
  g_cityScrollOff = 0;
  g_cityScrollNextMs = millis() + kCovCityScrollStartMs;
  paintCovCityMarquee();
}

static void drawSideWeatherCity(const SideWeatherLayout &layout) {
  if (timekeeper.weatherCity[0] == '\0') {
    clearCovCityMarqueeState();
    return;
  }
  setupCovCityMarquee(layout, sideCityTitleLabel(timekeeper.weatherCity));
}

bool isPlayingBlankCoverActive(); /* forward: определён ниже */

/* Тик бегущей строки города (из Display::loop, чаще CLOCK). */
void tickWeatherCityMarquee() {
  if (!isPlayingBlankCoverActive()) return;
  if (!g_cityNeedScroll || g_cityScrollText[0] == '\0') return;
  if (!shouldShowSideWeatherBlock()) return;
  const uint32_t now = millis();
  if ((int32_t)(now - g_cityScrollNextMs) < 0) return;
  g_cityScrollNextMs = now + kCovCityScrollMs;
  g_cityScrollOff++;
  const int16_t cycle = (int16_t)g_cityScrollTextW + kCovCityScrollGapPx;
  if (g_cityScrollOff >= cycle) g_cityScrollOff = 0;
  paintCovCityMarquee();
}

/* Перерисовка блока: fullRedraw — после прыжка часов, смены минут или новых данных OWM. */
static void drawSideWeatherBlock(bool fullRedraw) {
  SideWeatherLayout layout;
  sideWeatherLayout(layout);

  if (!shouldShowSideWeatherBlock()) {
    if (g_lastWeatherIconIdx != 255 || g_lastWeatherTempValid || g_lastWeatherCityDrawn[0] != '\0') {
      clearSideWeatherBlock(layout);
      g_lastWeatherIconIdx = 255;
      g_lastWeatherTempValid = false;
      g_lastWeatherTempRounded = 0;
      g_lastWeatherTempBand = 0;
      g_lastWeatherCityDrawn[0] = '\0';
      clearCovCityMarqueeState();
    }
    return;
  }

  uint8_t idx = 255;
  if (timekeeper.weatherIcon[0] != '\0') {
    idx = owmIconToIndex(timekeeper.weatherIcon);
#if ST7789_COV_USE_VC2_ICONS
    if (idx >= WEATHER_VC2_ICON_COUNT) idx = 4;
#else
    if (idx > 8) idx = 3;
#endif
  }
  const bool tempValid = timekeeper.weatherTempValid;
  const int16_t tempRounded = tempValid ? (int16_t)lroundf(timekeeper.weatherTemp) : 0;
  const uint8_t tempBand = tempValid ? sideWeatherTempBand(timekeeper.weatherTemp) : 0;
  char cityDrawn[60];
  cityDrawn[0] = '\0';
  if (timekeeper.weatherCity[0] != '\0') {
    strlcpy(cityDrawn, sideCityTitleLabel(timekeeper.weatherCity), sizeof(cityDrawn));
  }

  const bool sameContent = !fullRedraw
      && idx == g_lastWeatherIconIdx
      && tempValid == g_lastWeatherTempValid
      && (!tempValid || tempRounded == g_lastWeatherTempRounded)
      && (!tempValid || tempBand == g_lastWeatherTempBand)
      && strcmp(cityDrawn, g_lastWeatherCityDrawn) == 0;
  if (sameContent) return;

  clearSideWeatherBlock(layout);
#if ST7789_COV_USE_VC2_ICONS
  if (idx < WEATHER_VC2_ICON_COUNT) {
#else
  if (idx <= 8) {
#endif
    drawWeatherIconKind(layout.iconX, layout.iconY, idx);
  }
  drawSideWeatherTemp(layout);
  /* Город: сброс скролла только при смене текста; иначе сохранить offset после прыжка часов. */
  if (strcmp(cityDrawn, g_lastWeatherCityDrawn) != 0 || cityDrawn[0] == '\0') {
    drawSideWeatherCity(layout);
  } else {
    g_cityWinX = layout.blockX;
    g_cityWinW = layout.blockW;
    g_cityBaseY = layout.cityBaselineY;
    g_cityCenterX = layout.cityCenterX;
    strlcpy(g_cityScrollText, cityDrawn, sizeof(g_cityScrollText));
    paintCovCityMarquee();
  }

  g_lastWeatherIconIdx = idx;
  g_lastWeatherTempValid = tempValid;
  g_lastWeatherTempRounded = tempRounded;
  g_lastWeatherTempBand = tempBand;
  strlcpy(g_lastWeatherCityDrawn, cityDrawn, sizeof(g_lastWeatherCityDrawn));
}

/* Версия прошивки под логотипом: тот же size/цвет, что boot-строка версии (theme.rssi). */
static void drawLogoFirmwareVersion(int16_t logoBottomY) {
  /* YOVERSION уже вида «lstepMOD v0.8.xxx» — без префикса «yoradio:». */
  const char *ver = YOVERSION;
  const uint8_t ts = ST7789_COV_LOGO_VER_TEXT_SIZE;
  const uint8_t cw = (uint8_t)(6u * ts);
  const uint16_t th = (uint16_t)(8u * ts);
  const size_t len = strlen(ver);
  const int16_t tw = (int16_t)(len * cw);
  int16_t tx = (int16_t)((ST7789_COV_FB_LEFT_W - tw) / 2);
  if (tx < 0) tx = 0;
  int16_t ty = (int16_t)(logoBottomY + ST7789_COV_LOGO_VER_GAP_PX);
  /* Не выходим за нижний край слота обложки (факт рисуется ещё ниже, вне слота). */
  const int16_t slotBottom = (int16_t)(ST7789_COV_COVER_Y + ST7789_COV_COVER_SIZE);
  if ((ty + (int16_t)th) > slotBottom) {
    ty = (int16_t)(slotBottom - (int16_t)th);
  }
  if (ty < ST7789_COV_COVER_Y) ty = ST7789_COV_COVER_Y;
  dsp.setFont();
  dsp.setTextSize(ts);
  dsp.setTextColor(config.theme.rssi, config.theme.background);
  dsp.setCursor(tx, ty);
  dsp.print(ver);
}

static void drawLeftLogo(bool forceRedraw = false) {
  /* Пока ждём JPEG — логотип рисуем один раз, без «дрыганья» на каждом retry */
  if (g_leftShowingLogo && !forceRedraw) return;
  const int16_t logoW = 99;
  const int16_t logoH = 64;
  const int16_t slotY = ST7789_COV_COVER_Y;
  const int16_t lx = (ST7789_COV_FB_LEFT_W - logoW) / 2;
  /* Логотип — по центру слота обложки (слот уже смещён по Y в conf.h). */
  const int16_t ly = slotY + (ST7789_COV_COVER_SIZE - logoH) / 2;
  dsp.fillRect(0, slotY, ST7789_COV_FB_LEFT_W, ST7789_COV_COVER_SIZE, config.theme.background);
#if DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  dsp.drawRGBBitmap(lx, ly, const_cast<uint16_t*>(bootlogo2), logoW, logoH);
#else
  dsp.drawRGBBitmap(lx, ly, bootlogo2, logoW, logoH);
#endif
  /* Под логотипом — текущая версия прошивки (как на заставке загрузки). */
  drawLogoFirmwareVersion(ly + logoH);
  g_leftHasCover = false;
  g_leftShowingLogo = true;
}

static bool drawLeftCover(const String& target) {
  /* Один проход decode (без отдельного probe): меньше гонок за lock LittleFS. */
  dsp.fillRect(0, ST7789_COV_COVER_Y, ST7789_COV_FB_LEFT_W, ST7789_COV_COVER_SIZE, config.theme.background);
  if (!drawCoverInSlot(ST7789_COV_COVER_X_LEFT, ST7789_COV_COVER_Y,
                       ST7789_COV_COVER_SIZE, ST7789_COV_COVER_SIZE, target)) {
    return false;
  }
  g_leftHasCover = true;
  g_leftShowingLogo = false;
  g_stableCoverUrl = target;
  g_badCoverUrl = "";
  g_coverFailCount = 0;
  return true;
}

static void resetPlayingBlankState() {
  g_screenInitialized = false;
  g_lastJumpMs = 0;
  g_sideClrW = 0;
  g_lastColonVisible = -1;
  g_lastDrawnCoverUrl = "";
  g_stableCoverUrl = "";
  g_leftHasCover = false;
  g_leftShowingLogo = false;
  g_pendingCoverRedraw = false;
  g_badCoverUrl = "";
  g_lastCoverRetryMs = 0;
  g_coverFailCount = 0;
  g_encoderNavGuardArmed = false;
  g_lastWeatherIconIdx = 255;
  g_lastWeatherTempValid = false;
  g_lastWeatherTempRounded = 0;
  g_lastWeatherTempBand = 0;
  g_lastWeatherCityDrawn[0] = '\0';
  g_weatherIconDirty = false;
  strlcpy(g_sideTimeOld, "", sizeof(g_sideTimeOld));
  /* Скрыть/очистить бегущую строку факта при сбросе cover-SS. */
  g_factLastRaw = "";
  g_factStatusOverride = "";
  g_factStatusUntilMs = 0;
  g_factStatusWaitForFact = false;
  g_factStatusSafetyUntilMs = 0;
  g_factStatusShownAtMs = 0;
  g_factRawWhenStatusShown = "";
  g_factActiveW = nullptr;
  g_coverSwipeArmed = false;
  g_coverSwipePeakDyUp = 0;
  g_coverSwipePeakDyAbs = 0;
  g_coverSwipePeakDxAbs = 0;
  g_coverSwipeHwVertical = false;
  if (g_factScroll) {
    g_factScroll->lock(true);
    g_factScroll->setActive(false);
  }
  /* PLAYER-виджет не трогаем: сброс Cover-SS не должен гасить факт на PLAYER. */
  /* Cover-SS рисует часы шрифтом DS_DIGI28 — сброс, иначе ClockWidget на PLAYER без цифр. */
  dsp.setFont();
}

bool isPlayingBlankCoverActive() {
  /* blank ON = гашение, не Cover-SS. */
  return config.isScreensaver
      && display.mode() == SCREENBLANK
      && player.isRunning()
      && isDisplayCoversEnabled()
      && !config.store.screensaverPlayingBlank;
}

/* Сброс состояния при входе в playing+blank: экран уже очищен снаружи (display.cpp). */
void preparePlayingBlankEntry() {
  resetPlayingBlankState();
}

void resetEncoderNavGuard() {
  g_encoderNavGuardArmed = false;
}

void armEncoderNavGuard() {
  g_encoderNavGuardArmed = true;
}

bool consumeEncoderNavGuard() {
  if (!g_encoderNavGuardArmed) return false;
  g_encoderNavGuardArmed = false;
  return true;
}

/* Ленивая инициализация ScrollWidget факта (один раз за сессию). */
static void ensureFactScrollWidget() {
  if (g_factScrollInited) return;
  /* coverFactConf: size 2, buffsize 400, delta/time чуть медленнее weatherConf. */
  g_factScroll = new ScrollWidget("*", coverFactConf, config.theme.title2, config.theme.background);
  g_factScroll->setActive(false);
  g_factScroll->lock(true);
  g_factLastRaw = "";
  g_factScrollInited = true;
}

#if defined(HAS_PLAYER_FACT_SCROLL)
/* PLAYER: тот же ScrollWidget, другой conf (Y=93) — без нового API/архитектуры. */
static void ensurePlayerFactScrollWidget() {
  if (g_playerFactScrollInited) return;
  g_playerFactScroll = new ScrollWidget("*", playerFactConf, config.theme.title2, config.theme.background);
  g_playerFactScroll->setActive(false);
  g_playerFactScroll->lock(true);
  g_playerFactScrollInited = true;
}
#endif

/* Спрятать виджет факта (уход со экрана / смена режима). */
static void lockFactWidget(ScrollWidget *w) {
  if (!w) return;
  if (!w->locked()) {
    w->lock(true);
    w->setActive(false);
  }
}

/* Активный носитель факта: Cover-SS низ ИЛИ PLAYER Y=93 (только Faster с макросом). */
static ScrollWidget *selectFactScrollWidget() {
  if (isPlayingBlankCoverActive()) {
    ensureFactScrollWidget();
#if defined(HAS_PLAYER_FACT_SCROLL)
    lockFactWidget(g_playerFactScroll); /* null-safe, без ранней аллокации */
#endif
    return g_factScroll;
  }
#if defined(HAS_PLAYER_FACT_SCROLL)
  /* Не до ready(): setActive→fillRect по неинициализированному canvas (boot StoreProhibited). */
  if (display.mode() == PLAYER && display.ready()) {
    ensurePlayerFactScrollWidget();
    lockFactWidget(g_factScroll);
    return g_playerFactScroll;
  }
#endif
  return nullptr;
}

/* currentFact может содержать несколько фактов через «###» — для TFT склеиваем через « * ».
 * Пишем сразу в out (без промежуточного BUFLEN) — иначе длинный UTF-8 факт обрезается. */
static void buildFactDisplayFromCurrent(char *out, size_t outSz) {
  if (!out || outSz < 2) return;
  size_t j = 0;
  const char *p = currentFact.c_str();
  while (*p && (j + 1) < outSz) {
    if (p[0] == '#' && p[1] == '#' && p[2] == '#') {
      /* Разделитель между фактами в одной бегущей строке. */
      if ((j + 3) < outSz) {
        out[j++] = ' ';
        out[j++] = '*';
        out[j++] = ' ';
      }
      p += 3;
      continue;
    }
    out[j++] = *p++;
  }
  out[j] = '\0';
}

/* Сколько пикселей «вверх экрана» между двумя Y (с учётом инверсии тача). */
static int16_t coverSwipeDyTowardTop(uint16_t fromY, uint16_t toY) {
  /* CST820 / NV3041A+GT911 / AXS Faster: после маппинга Y растёт к верху (как TS_TOP_BAR). */
#if ((DSP_MODEL==DSP_NV3041A) && (TS_MODEL==TS_MODEL_GT911)) \
    || (TS_MODEL==TS_MODEL_CST820) || (TS_MODEL==TS_MODEL_AXS15231B)
  return (int16_t)toY - (int16_t)fromY;
#else
  return (int16_t)fromY - (int16_t)toY;
#endif
}

/* Показать статусную бегущую строку (тост): Cover-SS низ или PLAYER Y=93 (Faster).
 * waitForFact=true — крутить до смены currentFact (или safety); durationMs тогда игнор. */
void showFactStatusToast(const char *msg, uint32_t durationMs, bool waitForFact) {
  if (!msg || msg[0] == '\0') return;
  ScrollWidget *w = selectFactScrollWidget();
  if (!w) return;

  g_factRawWhenStatusShown = currentFact;
  g_factStatusOverride = msg;
  g_factStatusWaitForFact = waitForFact;
  if (waitForFact) {
    /* Короткий таймер не ставим — ждём факт; safety на случай сетевого сбоя. */
    g_factStatusUntilMs = 0;
    g_factStatusSafetyUntilMs = millis() + ST7789_COV_FACT_TOAST_SAFETY_MS;
    g_factStatusShownAtMs = millis();
  } else {
    g_factStatusUntilMs = millis() + (durationMs > 0 ? durationMs : 4000u);
    g_factStatusSafetyUntilMs = 0;
    g_factStatusShownAtMs = millis();
  }

  g_factActiveW = w;
  w->unlock();
  w->setActive(true);
  /* Пустая строка → setText(msg): иначе ScrollWidget может не перерисовать тот же текст. */
  w->setText("");
  w->setText(msg);
}

/* Слот обложки + pad: старт жеста у края JPEG / с оффсетом калибровки XPT2046. */
bool isTouchInCoverSlot(uint16_t x, uint16_t y) {
  const int pad = (int)ST7789_COV_FACT_SWIPE_PAD_PX;
  const int x0 = (int)ST7789_COV_COVER_X_LEFT - pad;
  const int y0 = (int)ST7789_COV_COVER_Y - pad;
  const int x1 = (int)ST7789_COV_COVER_X_LEFT + (int)ST7789_COV_COVER_SIZE + pad;
  /* Высота: слот + pad, но не уже экрана — полоса факта тоже считается «по обложке». */
  const int y1slot = (int)ST7789_COV_COVER_Y + (int)ST7789_COV_COVER_SIZE + pad;
  const int y1 = (y1slot < (int)DSP_HEIGHT) ? y1slot : (int)DSP_HEIGHT;
  return ((int)x >= x0 && (int)x < x1 && (int)y >= y0 && (int)y < y1);
}

/* Жест факта по обложке активен — touchscreen не должен уводить SS→PLAYER→PL. */
bool isCoverFactSwipeArmed() {
  return g_coverSwipeArmed;
}

/* Пик |ΔX|/|ΔY| последнего жеста (живёт после End до следующего Begin).
 * touchscreen: недосвайп vs чистый тап по слоту. */
int16_t coverFactSwipeLastPeakAbs() {
  const int16_t dy = g_coverSwipePeakDyAbs;
  const int16_t dx = g_coverSwipePeakDxAbs;
  return (dx > dy) ? dx : dy;
}

/* Старт жеста: cover-SS + палец в зоне обложки. TrackFacts.enabled проверяем на End (тост). */
void coverFactSwipeBegin(uint16_t x, uint16_t y) {
  g_coverSwipeArmed = false;
  g_coverSwipePeakDyUp = 0;
  g_coverSwipePeakDyAbs = 0;
  g_coverSwipePeakDxAbs = 0;
  g_coverSwipeHwVertical = false;
  if (!isPlayingBlankCoverActive()) return;
  if (!isTouchInCoverSlot(x, y)) return;
  g_coverSwipeArmed = true;
  g_coverSwipeStartX = x;
  g_coverSwipeStartY = y;
}

/* Движение: копим пик «вверх» и |ΔY|/|ΔX|; для CST820 — аппаратный SlideUp/Down. */
void coverFactSwipeSample(uint16_t x, uint16_t y, uint8_t hwGesture) {
  if (!g_coverSwipeArmed) return;
  const int16_t dyUp = coverSwipeDyTowardTop(g_coverSwipeStartY, y);
  const int16_t dyAbs = (int16_t)abs((int)y - (int)g_coverSwipeStartY);
  const int16_t dx = (int16_t)abs((int)x - (int)g_coverSwipeStartX);
  if (dyUp > g_coverSwipePeakDyUp) g_coverSwipePeakDyUp = dyUp;
  if (dyAbs > g_coverSwipePeakDyAbs) g_coverSwipePeakDyAbs = dyAbs;
  if (dx > g_coverSwipePeakDxAbs) g_coverSwipePeakDxAbs = dx;
#if TS_MODEL==TS_MODEL_CST820
  /* Контроллер отдаёт жест в портретных осях панели — после rotate1 это «вертикаль» экрана. */
  if (hwGesture == 0x01 || hwGesture == 0x02) { /* SlideDown / SlideUp из CST820.h */
    g_coverSwipeHwVertical = true;
  }
#else
  (void)hwGesture;
#endif
}

/* major достаточно большой; minor — шум/диагональ (до ~2.5×, мягче прежних *5). */
static bool coverSwipeMajorOk(int16_t major, int16_t minor, int16_t minPx) {
  if (major < minPx) return false;
  return (minor <= 0) || ((int32_t)major * 10 >= (int32_t)minor * 4);
}

/* Отпускание: пик хода → requestManualFact. XPT2046: ось «вверх» часто инвертирована или это X. */
bool coverFactSwipeEnd(uint16_t endX, uint16_t endY) {
  if (!g_coverSwipeArmed) return false;
  g_coverSwipeArmed = false;

  if (!isPlayingBlankCoverActive()) return false;

  const int16_t dyEndUp = coverSwipeDyTowardTop(g_coverSwipeStartY, endY);
  const int16_t dyUp = (dyEndUp > g_coverSwipePeakDyUp) ? dyEndUp : g_coverSwipePeakDyUp;
  const int16_t dyEndAbs = (int16_t)abs((int)endY - (int)g_coverSwipeStartY);
  const int16_t dyAbs = (dyEndAbs > g_coverSwipePeakDyAbs) ? dyEndAbs : g_coverSwipePeakDyAbs;
  const int16_t dxEnd = (int16_t)abs((int)endX - (int)g_coverSwipeStartX);
  const int16_t dx = (dxEnd > g_coverSwipePeakDxAbs) ? dxEnd : g_coverSwipePeakDxAbs;

  const int16_t minPx = (int16_t)ST7789_COV_FACT_SWIPE_MIN_PX;
  /* A) свайп к верху экрана; B) любая вертикаль (Y тача инвертирован); C) CST820 HW. */
  bool okTowardTop = coverSwipeMajorOk(dyUp, dx, minPx);
  bool okEitherVert = coverSwipeMajorOk(dyAbs, dx, minPx);
  bool okHwVert = g_coverSwipeHwVertical && coverSwipeMajorOk(dyAbs, dx, minPx);
#if TS_MODEL==TS_MODEL_XPT2046 || TS_MODEL==TS_MODEL_AXS15231B
  /* D) оси X/Y перепутаны / soft-rot: «вверх» часто приходит как большой |ΔX|.
   * Порог чуть выше вертикали — короткий тап по X не уводит в факт. */
  bool okSwappedAxis = coverSwipeMajorOk(dx, dyAbs, (int16_t)(minPx + 12));
#else
  bool okSwappedAxis = false;
#endif
  if (!okTowardTop && !okEitherVert && !okHwVert && !okSwappedAxis) return false;

  /* Плагин выключен — не будим плеер, показываем причину на полосе факта. */
  if (!trackFactsPlugin.isEnabled()) {
    showFactStatusToast("TrackFacts выключен в настройках.", 4000, false);
    return true;
  }

  const String factBefore = currentFact;
  String rejectReason;
  /* https без proxy: в текущей версии свайп = риск без TFT-диалога (оверлей — later). */
  factsAcceptHttpsTlsRiskOnce();
  if (trackFactsPlugin.requestManualFact(&rejectReason)) {
    /* Держим тост, пока в currentFact не появится новый текст. */
    const char *okMsg = "Запрос принят, готовим текст о треке…";
    showFactStatusToast(okMsg, 0, true);
    trackFactsPlugin.sendStatus(okMsg);
  } else if (rejectReason.length() > 0) {
    /* Кэш/уже есть / ошибка — факт либо уже в currentFact, либо не будет: короткий тост. */
    const bool gotNewFactNow =
        (currentFact.length() > 0 && currentFact != factBefore);
    if (gotNewFactNow) {
      /* Подняли из кэша — короткий статус, затем сам факт. */
      showFactStatusToast(rejectReason.c_str(), 2500, false);
      g_factLastRaw = "";
    } else {
      showFactStatusToast(rejectReason.c_str(), 4500, false);
    }
    if (rejectReason.indexOf("уже доступен") < 0 &&
        rejectReason.indexOf("из файлового кэша") < 0 &&
        rejectReason.indexOf("уже выполняется") < 0) {
      trackFactsPlugin.sendStatus(rejectReason.c_str(), true);
    }
    g_factLastRaw = "";
  } else {
    showFactStatusToast("Запрос факта…", 3000, false);
  }
  return true;
}

/* Синхронизация текста + штатный ScrollWidget::loop() (как у weather/title).
 * Cover-SS: низ экрана. Faster PLAYER: Y=93 при HAS_PLAYER_FACT_SCROLL. */
void tickFactMarquee(bool forceRedraw) {
  ScrollWidget *w = selectFactScrollWidget();
  if (!w) {
    /* Вне Cover-SS и вне PLAYER-факта — спрятать оба носителя. */
    lockFactWidget(g_factScroll);
#if defined(HAS_PLAYER_FACT_SCROLL)
    lockFactWidget(g_playerFactScroll);
#endif
    g_factActiveW = nullptr;
    g_factLastRaw = "";
    g_factStatusOverride = "";
    g_factStatusUntilMs = 0;
    g_factStatusWaitForFact = false;
    g_factStatusSafetyUntilMs = 0;
    return;
  }

  /* Смена экрана PLAYER↔Cover: перенести тост/факт на новый виджет. */
  if (w != g_factActiveW) {
    g_factActiveW = w;
    if (g_factStatusOverride.length() > 0) {
      w->unlock();
      w->setActive(true);
      w->setText("");
      w->setText(g_factStatusOverride.c_str());
    } else {
      g_factLastRaw = "";
      forceRedraw = true;
    }
  }

  const uint32_t now = millis();
  bool statusActive = (g_factStatusOverride.length() > 0);

  if (statusActive && g_factStatusWaitForFact) {
    /* Ждём новый факт; safety — если сеть молчит слишком долго. */
    const bool factArrived =
        (currentFact.length() > 0 && currentFact != g_factRawWhenStatusShown);
    const bool safetyExpired =
        (g_factStatusSafetyUntilMs != 0) && ((int32_t)(now - g_factStatusSafetyUntilMs) >= 0);
    /* Запрос уже не идёт, факт не сменился (отказ/таймаут очереди) — не держать 120 с. */
    const bool requestDoneNoFact =
        !factArrived &&
        ((now - g_factStatusShownAtMs) > 3000u) &&
        !trackFactsPlugin.isRequestPending() &&
        !trackFactsPlugin.isRequestActive();
    if (factArrived || safetyExpired || requestDoneNoFact) {
      statusActive = false;
      g_factStatusOverride = "";
      g_factStatusWaitForFact = false;
      g_factStatusUntilMs = 0;
      g_factStatusSafetyUntilMs = 0;
      g_factRawWhenStatusShown = "";
      forceRedraw = true;
      g_factLastRaw = "";
    }
  } else if (statusActive) {
    /* Обычный (короткий) тост по таймеру; досрочно — если факт уже сменился. */
    const bool timedOut =
        (g_factStatusUntilMs != 0) && ((int32_t)(now - g_factStatusUntilMs) >= 0);
    const bool factArrived =
        (currentFact.length() > 0 && currentFact != g_factRawWhenStatusShown);
    if (timedOut || factArrived) {
      statusActive = false;
      g_factStatusOverride = "";
      g_factStatusUntilMs = 0;
      g_factRawWhenStatusShown = "";
      forceRedraw = true;
      g_factLastRaw = "";
    }
  }

  if (statusActive) {
    /* Крутим статусную строку, пока ждём факт / пока тикает короткий тост. */
    if (!w->locked()) {
      w->loop();
    }
    return;
  }

  const bool rawChanged = (currentFact != g_factLastRaw);

  if (rawChanged || forceRedraw) {
    g_factLastRaw = currentFact;
    /* UTF-8 буфер: SCROLL_SETTEXT_UTF8_MAX — тот же лимит, что в ScrollWidget::setText. */
    char disp[SCROLL_SETTEXT_UTF8_MAX];
    buildFactDisplayFromCurrent(disp, sizeof(disp));

    if (disp[0] == '\0') {
      /* Факта нет — очистить полосу и остановить скролл. */
      lockFactWidget(w);
      return;
    }

    /* Показать виджет; при force — сброс через пустую строку, иначе setText не перерисует тот же текст. */
    w->unlock();
    w->setActive(true);
    if (forceRedraw) {
      w->setText("");
    }
    w->setText(disp);
  }

  /* Плавный скролл — тот же loop(), что у meta/title1/weather на PLAYER. */
  if (!w->locked()) {
    w->loop();
  }
}

void tickPlayingBlankScreen(bool forceRedraw) {
  if (!isPlayingBlankCoverActive()) {
    resetPlayingBlankState();
    return;
  }

  if (g_coverInvalidateNeeded) {
    g_coverInvalidateNeeded = false;
    g_badCoverUrl = "";
    g_coverFailCount = 0;
    g_lastCoverRetryMs = 0;
    /* Файл обложки обновился — перерисуем, но не сбрасываем уже показанную картинку при сбое */
    g_pendingCoverRedraw = true;
  }

  captureSideBaseColor();
  const uint32_t now = millis();
  String target = String(currentCoverUrl.c_str());
  const bool urlChanged = (target != g_lastDrawnCoverUrl);
  if (urlChanged) {
    g_leftShowingLogo = false;
    g_coverFailCount = 0;
    g_badCoverUrl = "";
  }

  strftime(g_sideTimeBuf, sizeof(g_sideTimeBuf), "%H:%M", &network.timeinfo);
  const bool minuteChanged = (strcmp(g_sideTimeOld, g_sideTimeBuf) != 0);
  const bool colonState = (network.timeinfo.tm_sec % 2 == 0);
  const bool colonChanged = (g_lastColonVisible < 0) || ((colonState ? 1 : 0) != g_lastColonVisible);

  bool didJump = false;
  if (g_lastJumpMs == 0) {
    resetSideClockLayout();
    pickNextSideClockColor();
    g_lastJumpMs = now;
    didJump = true;
  } else if ((now - g_lastJumpMs) >= ST7789_COV_SIDE_SS_JUMP_MS) {
    clearRightPanelForClockJump();
    sideClockJump(now);
    didJump = true;
  }

  if (!g_screenInitialized) {
    dsp.fillScreen(config.theme.background);
    g_screenInitialized = true;
    forceRedraw = true;
    didJump = true;
  }

  /* Тихий retry: пока обложки нет — каждый CLOCK-тик (~1 с); после успеха — редкий refresh. */
  const bool wantCoverRetry = (g_pendingCoverRedraw || !g_leftHasCover) && target.startsWith("/sc/");
  const uint32_t coverRetryInterval = (!g_leftHasCover || g_pendingCoverRedraw)
      ? 0u : ST7789_COV_COVER_RETRY_MS;
  if (wantCoverRetry && (now - g_lastCoverRetryMs) >= coverRetryInterval) {
    g_lastCoverRetryMs = now;
    if (drawLeftCover(target)) {
      g_pendingCoverRedraw = false;
      g_lastDrawnCoverUrl = target;
    }
  }

  /* Полная перерисовка левой колонки — только при смене URL или явном force */
  bool needLeftRedraw = forceRedraw || urlChanged;

  if (needLeftRedraw) {
    bool gotCover = false;
    if (target.length() && target.startsWith("/sc/")) {
      gotCover = drawLeftCover(target);
    }
    if (gotCover) {
      g_pendingCoverRedraw = false;
    } else if (g_stableCoverUrl == target && g_leftHasCover) {
      /* Файл на диске есть, но decode временно не прошёл — не затираем уже показанную обложку. */
      g_pendingCoverRedraw = true;
    } else if (target.startsWith("/sc/")) {
      /* JPEG ещё качается или LittleFS занята — лого один раз, дальше retry каждый тик. */
      if (!g_leftShowingLogo) drawLeftLogo(true);
      g_pendingCoverRedraw = true;
    } else {
      drawLeftLogo(urlChanged || forceRedraw);
    }
    g_lastDrawnCoverUrl = target;
  }

  if (didJump || minuteChanged || forceRedraw) {
    if (!didJump && (minuteChanged || forceRedraw)) clearSideClockArea();
    drawSideClockDigits();
    strlcpy(g_sideTimeOld, g_sideTimeBuf, sizeof(g_sideTimeOld));
    drawSideWeatherBlock(didJump || minuteChanged || forceRedraw || g_weatherIconDirty);
    g_weatherIconDirty = false;
  } else if (colonChanged) {
    redrawSideColonOnly();
  } else if (g_weatherIconDirty) {
    drawSideWeatherBlock(true);
    g_weatherIconDirty = false;
  }

  /* После часов/иконки — шов между колонками (DS_DIGI28 рисуется шире cursor). */
  repairCoverClockSeam();

  /* Факт: полная перерисовка при force/init; плавный скролл — из Display::loop → tickFactMarquee. */
  tickFactMarquee(forceRedraw);

#if DSP_MODEL==DSP_AXS15231B
  /* Canvas Faster: декод JPEG и fillRect не помечают dirty сами — без этого flush пропускает кадр. */
  axsMarkFbDirty();
#endif
}

/* Сброс кэша из config.cpp после записи JPEG в LittleFS. */
void requestCoverInvalidate() {
  g_coverInvalidateNeeded = true;
}

void notifyWeatherIconUpdated() {
  g_weatherIconDirty = true;
}

} // namespace ST7789_COV_NS
