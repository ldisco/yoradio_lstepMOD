#include "../core/options.h"
#if DSP_MODEL==DSP_ST7789_170

#include "displayST7789_170.h"
#include "fonts/bootlogo.h"
#include "../core/config.h"
#include "../core/network.h"
#include "../core/display.h"
#include "../core/player.h"
#include "../core/timekeeper.h"
#include "tools/utf8Rus.h"
#include <LittleFS.h>
#include <TJpg_Decoder.h>

/* Часы справа от обложки — mono 28pt: одинаковая ширина цифр, «1» по центру ячейки. */
#define ST170_SKIP_DIGI28_CLOCK_GFXFONT
#include "fonts/DS_DIGI28pt7b_mono.h"

#ifndef DEF_SPI_FREQ
  #define DEF_SPI_FREQ        40000000UL
#endif

#if DSP_HSPI
  DspCore::DspCore(): Adafruit_ST7789(&SPI2, TFT_CS, TFT_DC, TFT_RST) {}
#else
  DspCore::DspCore(): Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST) {}
#endif

namespace st7789_170_cover {

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
static String g_badCoverUrl = "";
static const uint8_t kMaxCoverRetries = 3;
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
static uint16_t g_sideClkTop = ST170_SIDE_CLK_MIN_TOP + 20;
static uint16_t g_sideClkLeft = ST170_FB_RIGHT_X + 8;
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
/* Иконка погоды под часами: последний нарисованный тип и флаг «нужна перерисовка». */
static uint8_t g_lastWeatherIconIdx = 255;
static bool g_weatherIconDirty = false;

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
  uint16_t* src = bitmap + (drawT - y) * w + (drawL - x);
  dsp.drawRGBBitmap(drawL, drawT, src, clippedW, clippedH);
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

/* Декод JPEG в квадратный слот: режим cover (заполнить слот, лишнее обрезать bounds). */
static bool drawCoverInSlot(int16_t x, int16_t y, int16_t w, int16_t h, const String& coverUrl) {
  String fsPath;
  if (!resolveCoverPath(coverUrl, fsPath)) return false;

  ensureCoverDecoderReady();

  if (!lockLittleFS(120)) return false;

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

  /* Сначала ограничение по heap (как NV3041A), затем cover-fill — не вписывать мелко в центр. */
  const uint16_t kMaxDecodeDim = 512;
  uint8_t scaleMin = 0;
  while ((jpgW >> scaleMin) > kMaxDecodeDim || (jpgH >> scaleMin) > kMaxDecodeDim) {
    if (scaleMin >= 3) break;
    scaleMin++;
  }

  uint8_t scale = scaleMin;
  uint16_t scaledW = jpgW >> scale;
  uint16_t scaledH = jpgH >> scale;
  /* Минимальный scale (макс. детализация), при котором картинка покрывает весь слот */
  for (uint8_t s = scaleMin; s <= 3; ++s) {
    scaledW = jpgW >> s;
    scaledH = jpgH >> s;
    if (scaledW >= (uint16_t)w && scaledH >= (uint16_t)h) {
      scale = s;
      break;
    }
    scale = s;
  }

  g_coverDrawX = x + (int16_t)((int32_t)w - (int32_t)scaledW) / 2;
  g_coverDrawY = y + (int16_t)((int32_t)h - (int32_t)scaledH) / 2;
  g_coverBoundLeft = x;
  g_coverBoundTop = y;
  g_coverBoundRight = x + w;
  g_coverBoundBottom = y + h;

  TJpgDec.setJpgScale(1 << scale);
  JRESULT drawResult = TJpgDec.drawFsJpg(g_coverDrawX, g_coverDrawY, fsPath.c_str(), LittleFS);
  unlockLittleFS();
  if (drawResult == JDR_OK) vTaskDelay(pdMS_TO_TICKS(1));
  return (drawResult == JDR_OK);
}

static GFXglyph *pgmGlyphSide(const GFXfont *gfxFont, uint8_t c) {
  return gfxFont->glyph + (c - 0x20);
}

static uint8_t sideCharWidth(unsigned char c) {
  GFXglyph* glyph = pgmGlyphSide(&ST170_SmallClockFont, c);
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

/* Геометрический центр правой колонки: ровно между обложкой (X=170) и краем TFT. */
static uint16_t sideClockCenteredLeft(uint16_t tw) {
  const uint32_t panelL = ST170_FB_RIGHT_X;
  const uint32_t panelW = ST170_FB_RIGHT_W;
  if (tw >= panelW) return (uint16_t)panelL;
  return (uint16_t)(panelL + (panelW - tw) / 2u);
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
  const uint16_t tw = sideClockMaxWidth();
  const uint16_t minL = ST170_FB_RIGHT_X + ST170_SIDE_CLK_MARGIN_L;
  uint16_t maxL = DSP_WIDTH - tw - ST170_SIDE_CLK_MARGIN_R;
  if (maxL < minL) maxL = minL;
  if (g_sideClkLeft < minL) g_sideClkLeft = minL;
  if (g_sideClkLeft > maxL) g_sideClkLeft = maxL;
}

static void resetSideClockLayout() {
  const uint16_t tw = sideClockMaxWidth();
  g_sideClkLeft = sideClockCenteredLeft(tw);
  g_sideClkTop = (ST170_SIDE_CLK_MIN_TOP + ST170_SIDE_CLK_MAX_TOP) / 2;
  clampSideClockPosition();
}

static void sideClockJump(uint32_t now) {
  pickNextSideClockColor();
  const uint16_t tw = sideClockMaxWidth();
  const uint16_t minLeft = ST170_FB_RIGHT_X + ST170_SIDE_CLK_MARGIN_L;
  uint16_t maxLeft = ST170_FB_RIGHT_X + ST170_FB_RIGHT_W - tw - ST170_SIDE_CLK_MARGIN_R;
  if (maxLeft <= minLeft) g_sideClkLeft = minLeft;
  else g_sideClkLeft = (uint16_t)random(minLeft, maxLeft + 1);
  if (ST170_SIDE_CLK_MAX_TOP > ST170_SIDE_CLK_MIN_TOP) {
    g_sideClkTop = (uint16_t)random(ST170_SIDE_CLK_MIN_TOP, ST170_SIDE_CLK_MAX_TOP + 1);
  }
  clampSideClockPosition();
  g_lastJumpMs = now;
}

/* Полная очистка правой колонки — только при прыжке (убирает артефакты глифов). */
static void clearRightPanelForClockJump() {
  dsp.fillRect(ST170_FB_RIGHT_X, 0, ST170_FB_RIGHT_W, DSP_HEIGHT, config.theme.background);
}

static void updateSideClearRect() {
  const uint16_t tw = sideTextWidth(g_sideTimeBuf);
  g_sideClrTop = (g_sideClkTop > ST170_SIDE_CLK_GLYPH_ABOVE)
      ? (uint16_t)(g_sideClkTop - ST170_SIDE_CLK_GLYPH_ABOVE - 4) : 0;
  g_sideClrH = ST170_SIDE_CLK_CLEAR_H;
  g_sideClrLeft = (g_sideClkLeft > ST170_SIDE_CLK_MARGIN_L)
      ? (uint16_t)(g_sideClkLeft - ST170_SIDE_CLK_MARGIN_L) : ST170_FB_RIGHT_X;
  g_sideClrW = tw + ST170_SIDE_CLK_MARGIN_L + ST170_SIDE_CLK_MARGIN_R;
  if (g_sideClrLeft < ST170_FB_RIGHT_X) g_sideClrLeft = ST170_FB_RIGHT_X;
  if (g_sideClrLeft + g_sideClrW > DSP_WIDTH) {
    g_sideClrW = DSP_WIDTH - g_sideClrLeft;
  }
}

static void clearSideClockArea() {
  if (g_sideClrW == 0) return;
  dsp.fillRect(g_sideClrLeft, g_sideClrTop, g_sideClrW, g_sideClrH, config.theme.background);
}

static void drawSideClockDigits() {
  clampSideClockPosition();
  dsp.setTextSize(1);
  dsp.setFont(&ST170_SmallClockFont);
  char hourBuf[4];
  char minBuf[4];
  strftime(hourBuf, sizeof(hourBuf), "%H", &network.timeinfo);
  strftime(minBuf, sizeof(minBuf), "%M", &network.timeinfo);
  const uint16_t dotsLeft = sideTextWidth(hourBuf);
  const bool dotsOn = (network.timeinfo.tm_sec % 2 == 0);

  dsp.setTextColor(g_sideClkColor, config.theme.background);
  dsp.setCursor(g_sideClkLeft, g_sideClkTop);
  dsp.print(hourBuf);
  dsp.setTextColor(dotsOn ? g_sideClkColor : config.theme.background, config.theme.background);
  dsp.setCursor(g_sideClkLeft + dotsLeft, g_sideClkTop);
  dsp.print(":");
  dsp.setTextColor(g_sideClkColor, config.theme.background);
  dsp.setCursor(g_sideClkLeft + dotsLeft + sideCharWidth(':'), g_sideClkTop);
  dsp.print(minBuf);
  dsp.setFont();
  g_lastColonVisible = dotsOn ? 1 : 0;
  updateSideClearRect();
}

/* Мигание «:» — перерисовка только двоеточия (меньше артефактов). */
static void redrawSideColonOnly() {
  char hourBuf[4];
  char minBuf[4];
  strftime(hourBuf, sizeof(hourBuf), "%H", &network.timeinfo);
  strftime(minBuf, sizeof(minBuf), "%M", &network.timeinfo);
  const uint16_t dotsLeft = sideTextWidth(hourBuf);
  const uint16_t colonX = g_sideClkLeft + dotsLeft;
  const uint16_t colonSlotW = sideCharWidth(':');
  const uint16_t minX = colonX + colonSlotW;
  const uint16_t colonTop = (g_sideClkTop > ST170_SIDE_CLK_GLYPH_ABOVE)
      ? (uint16_t)(g_sideClkTop - ST170_SIDE_CLK_GLYPH_ABOVE - 2) : 0;
  dsp.fillRect(colonX, colonTop, colonSlotW, ST170_SIDE_CLK_CLEAR_H, config.theme.background);
  dsp.setTextSize(1);
  dsp.setFont(&ST170_SmallClockFont);
  const bool dotsOn = (network.timeinfo.tm_sec % 2 == 0);
  dsp.setTextColor(dotsOn ? g_sideClkColor : config.theme.background, config.theme.background);
  dsp.setCursor(colonX, g_sideClkTop);
  dsp.print(":");
  if (minBuf[0] != '\0') {
    dsp.setTextColor(g_sideClkColor, config.theme.background);
    dsp.setCursor(minX, g_sideClkTop);
    dsp.print(minBuf[0]);
  }
  dsp.setFont();
  g_lastColonVisible = dotsOn ? 1 : 0;
}

/* --- Иконка погоды под «прыгающими» часами (cover-скринсейвер, без текста) --- */

/* Показываем иконку только если погода включена в настройках и OWM уже отдал код icon. */
static bool shouldShowSideWeatherIcon() {
  return config.store.showweather
      && strlen(config.store.weatherkey) > 0
      && timekeeper.weatherIcon[0] != '\0';
}

/* Код OpenWeatherMap "01d"…"50n" → индекс набора простых пиктограмм (0…8). */
static uint8_t owmIconToIndex(const char *icon) {
  if (!icon || icon[0] < '0' || icon[1] < '0') return 255;
  const int code = (icon[0] - '0') * 10 + (icon[1] - '0');
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
}

/* Прямоугольник ST170_WICON_W×H под текущей позицией часов (центр по ширине строки HH:MM). */
static void sideWeatherIconRect(int16_t &outLeft, int16_t &outTop) {
  const char *twSrc = (g_sideTimeBuf[0] != '\0') ? g_sideTimeBuf : "88:88";
  const uint16_t tw = sideTextWidth(twSrc);
  outLeft = (int16_t)g_sideClkLeft;
  if (tw > ST170_WICON_W) {
    outLeft += (int16_t)((tw - ST170_WICON_W) / 2u);
  }
  outTop = (int16_t)(g_sideClkTop + ST170_WICON_GAP_BELOW + ST170_WICON_DESCENT);
}

static void clearSideWeatherIconArea() {
  int16_t ix = 0, iy = 0;
  sideWeatherIconRect(ix, iy);
  dsp.fillRect(ix, iy, ST170_WICON_W, ST170_WICON_H, config.theme.background);
}

/* Координаты пиктограмм заданы под макет 24×24; масштабируем к ST170_WICON_W. */
static inline int16_t wiconCoord(int16_t v24) {
  return (int16_t)((v24 * (int32_t)ST170_WICON_W + 12) / 24);
}

/* Облако из кругов + «тело» — базовый элемент большинства пиктограмм. */
static void drawWeatherCloudBlob(int16_t ox, int16_t oy, uint16_t col) {
  dsp.fillCircle(ox + wiconCoord(8), oy + wiconCoord(10), wiconCoord(5), col);
  dsp.fillCircle(ox + wiconCoord(14), oy + wiconCoord(9), wiconCoord(6), col);
  dsp.fillCircle(ox + wiconCoord(19), oy + wiconCoord(11), wiconCoord(4), col);
  dsp.fillRect(ox + wiconCoord(4), oy + wiconCoord(11), wiconCoord(17), wiconCoord(6), col);
}

/* Отрисовка одной из 9 пиктограмм в слоте ST170_WICON_W × ST170_WICON_H. */
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
        dsp.fillRect(ox + (ST170_WICON_W - w) / 2, y, w, wiconCoord(2), mistCol);
      }
      break;
  }
}

/* Перерисовка иконки: fullRedraw — после прыжка часов, смены минут или новых данных OWM. */
static void drawSideWeatherIcon(bool fullRedraw) {
  if (!shouldShowSideWeatherIcon()) {
    if (g_lastWeatherIconIdx != 255) {
      clearSideWeatherIconArea();
      g_lastWeatherIconIdx = 255;
    }
    return;
  }
  uint8_t idx = owmIconToIndex(timekeeper.weatherIcon);
  if (idx > 8) idx = 3;
  if (!fullRedraw && idx == g_lastWeatherIconIdx) return;
  clearSideWeatherIconArea();
  int16_t ix = 0, iy = 0;
  sideWeatherIconRect(ix, iy);
  drawWeatherIconKind(ix, iy, idx);
  g_lastWeatherIconIdx = idx;
}

static void drawLeftLogo(bool forceRedraw = false) {
  /* Пока ждём JPEG — логотип рисуем один раз, без «дрыганья» на каждом retry */
  if (g_leftShowingLogo && !forceRedraw) return;
  const int16_t logoW = 99;
  const int16_t logoH = 64;
  const int16_t lx = (ST170_FB_LEFT_W - logoW) / 2;
  const int16_t ly = (ST170_COVER_SIZE - logoH) / 2;
  dsp.fillRect(0, 0, ST170_FB_LEFT_W, ST170_COVER_SIZE, config.theme.background);
  dsp.drawRGBBitmap(lx, ly, bootlogo2, logoW, logoH);
  g_leftHasCover = false;
  g_leftShowingLogo = true;
}

static bool probeCoverJpeg(const String& coverUrl) {
  String fsPath;
  if (!resolveCoverPath(coverUrl, fsPath)) return false;
  if (!lockLittleFS(200)) return false;
  File f = LittleFS.open(fsPath, "r");
  if (!f) {
    unlockLittleFS();
    return false;
  }
  const size_t fSize = f.size();
  f.close();
  if (fSize < 100) {
    unlockLittleFS();
    return false;
  }
  uint16_t jpgW = 0, jpgH = 0;
  const bool ok = (TJpgDec.getFsJpgSize(&jpgW, &jpgH, fsPath.c_str(), LittleFS) == JDR_OK);
  unlockLittleFS();
  return ok;
}

static bool drawLeftCover(const String& target) {
  /* Не затираем слот, пока JPEG не готов — иначе «миг» обложка→лого при busy LittleFS */
  if (!probeCoverJpeg(target)) return false;
  dsp.fillRect(0, 0, ST170_FB_LEFT_W, ST170_COVER_SIZE, config.theme.background);
  if (!drawCoverInSlot(ST170_COVER_X_LEFT, ST170_COVER_Y,
                       ST170_COVER_SIZE, ST170_COVER_SIZE, target)) {
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
  g_weatherIconDirty = false;
  strlcpy(g_sideTimeOld, "", sizeof(g_sideTimeOld));
}

bool isPlayingBlankCoverActive() {
  return config.isScreensaver
      && display.mode() == SCREENBLANK
      && player.isRunning()
      && isDisplayCoversEnabled();
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
  } else if ((now - g_lastJumpMs) >= ST170_SIDE_SS_JUMP_MS) {
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

  /* Тихий retry с интервалом ST170_COVER_RETRY_MS: только попытка JPEG, без перерисовки логотипа */
  const bool wantCoverRetry = (g_pendingCoverRedraw || !g_leftHasCover)
      && target.startsWith("/sc/") && target != g_badCoverUrl;
  if (wantCoverRetry && (now - g_lastCoverRetryMs) >= ST170_COVER_RETRY_MS) {
    g_lastCoverRetryMs = now;
    if (drawLeftCover(target)) {
      g_pendingCoverRedraw = false;
      g_lastDrawnCoverUrl = target;
    }
  }

  /* Полная перерисовка левой колонки — только при смене URL или явном force */
  bool needLeftRedraw = forceRedraw || urlChanged;

  if (needLeftRedraw) {
    if (target == g_badCoverUrl) {
      drawLeftLogo(urlChanged || forceRedraw);
    } else {
      bool gotCover = false;
      if (target.length() && target.startsWith("/sc/")) {
        gotCover = drawLeftCover(target);
      }
      if (gotCover) {
        g_pendingCoverRedraw = false;
      } else if (g_stableCoverUrl == target && g_leftHasCover) {
        g_pendingCoverRedraw = true;
      } else if (target.startsWith("/sc/")) {
        /* Обложка ещё грузится — лого один раз, дальше ждём тихий retry */
        if (!g_leftShowingLogo) drawLeftLogo(true);
        g_pendingCoverRedraw = true;
        if (urlChanged) g_coverFailCount++;
        if (g_coverFailCount >= kMaxCoverRetries && target.length()) {
          g_badCoverUrl = target;
        }
      } else {
        drawLeftLogo(urlChanged || forceRedraw);
      }
    }
    g_lastDrawnCoverUrl = target;
  }

  if (didJump || minuteChanged || forceRedraw) {
    if (!didJump && (minuteChanged || forceRedraw)) clearSideClockArea();
    drawSideClockDigits();
    strlcpy(g_sideTimeOld, g_sideTimeBuf, sizeof(g_sideTimeOld));
    drawSideWeatherIcon(didJump || minuteChanged || forceRedraw || g_weatherIconDirty);
    g_weatherIconDirty = false;
  } else if (colonChanged) {
    redrawSideColonOnly();
  } else if (g_weatherIconDirty) {
    drawSideWeatherIcon(true);
    g_weatherIconDirty = false;
  }
}

/* Сброс кэша из config.cpp после записи JPEG в LittleFS. */
void requestCoverInvalidate() {
  g_coverInvalidateNeeded = true;
}

void notifyWeatherIconUpdated() {
  g_weatherIconDirty = true;
}

} // namespace st7789_170_cover

void invalidateCoverCache() {
  st7789_170_cover::requestCoverInvalidate();
}

#ifndef BATTERY_OFF
  #include "driver/adc.h"
  #include "esp_adc_cal.h"

  #ifndef ADC_PIN
    #define ADC_PIN 5
  #endif
  #if (ADC_PIN == 5)
    #define USER_ADC_CHAN   ADC1_CHANNEL_4
  #endif

  #ifndef DELTA_BAT
    #define DELTA_BAT 0
  #endif
  #ifndef R1
    #define R1 50
  #endif
  #ifndef R2
    #define R2 100
  #endif

  float ADC_R1 = R1;
  float ADC_R2 = R2;
  float DELTA = DELTA_BAT;

  int g = 1;
  bool Charging = false;
  int reads = 100;
  float Volt = 0;
  float Volt1 = 0, Volt2 = 0, Volt3 = 0, Volt4 = 0;
  static esp_adc_cal_characteristics_t adc1_chars;
  int ChargeLevel = 0;
  float vs[22] = {2.60, 3.10, 3.20, 3.26, 3.29, 3.33, 3.37, 3.41, 3.46, 3.51, 3.56, 3.61, 3.65, 3.69, 3.72, 3.75, 3.78, 3.82, 3.88, 3.95, 4.03, 4.25};
#endif

void DspCore::initDisplay() {
  _clipping = false;
  // Разрешение 170x320 — как в yoRadio (портретная ориентация ST7789 1.9").
  init(170, 320);
  if (DEF_SPI_FREQ > 0) setSPISpeed(DEF_SPI_FREQ);
  invert();
  cp437(true);
  flip();
  setTextWrap(false);
  setTextSize(1);
  fillScreen(0x0000);
}

void DspCore::drawLogo(uint16_t top) {
  drawRGBBitmap((width() - 99) / 2, top, bootlogo2, 99, 64);
}

void DspCore::clearDsp(bool black) {
  fillScreen(black ? 0 : config.theme.background);
}

GFXglyph *pgm_read_glyph_ptr_st170(const GFXfont *gfxFont, uint8_t c) {
  return gfxFont->glyph + c;
}

uint8_t DspCore::_charWidth(unsigned char c) {
  GFXglyph* glyph = pgm_read_glyph_ptr_st170(&Clock_GFXfont, c - 0x20);
  return pgm_read_byte(&glyph->xAdvance);
}

uint16_t DspCore::textWidth(const char *txt) {
  uint16_t w = 0;
  uint16_t l = strlen(txt);
  for (uint16_t c = 0; c < l; c++) w += _charWidth(txt[c]);
  return w;
}

void DspCore::_getTimeBounds() {
  _timewidth = textWidth(_timeBuf);
  char buf[4];
  strftime(buf, 4, "%H", &network.timeinfo);
  _dotsLeft = textWidth(buf);
}

void DspCore::_clockSeconds() {
  setTextSize(3);
  setTextColor(config.theme.seconds, config.theme.background);
  setCursor(width() - 11 - clockRightSpace - CHARWIDTH * 6, clockTop - clockTimeHeight + 34);
  sprintf(_bufforseconds, "%02d", network.timeinfo.tm_sec);
  if (!config.isScreensaver) print(_bufforseconds);

  setTextSize(1);
  setFont(&Clock_GFXfont);
  setTextColor((network.timeinfo.tm_sec % 2 == 0) ? config.theme.clock : (CLOCKFONT_MONO ? config.theme.clockbg : config.theme.background), config.theme.background);
  setCursor(_timeleft + _dotsLeft, clockTop);
  print(":");
  setFont();

#ifndef BATTERY_OFF
  if (!config.isScreensaver) {
    setTextSize(BatFS);

    if (Charging) {
      setTextColor(color565(0, 255, 255), config.theme.background);
      if (network.timeinfo.tm_sec % 1 == 0) {
        if (g == 1) { setCursor(BatX, BatY); print("\xA0\xA2\x9E\x9F"); }
        if (g == 2) { setCursor(BatX, BatY); print("\xA0\x9E\x9E\xA3"); }
        if (g == 3) { setCursor(BatX, BatY); print("\x9D\x9E\xA2\xA3"); }
        if (g >= 4) { g = 0; setCursor(BatX, BatY); print("\x9D\xA2\xA2\x9F"); }
        g++;
      }
    }

    if (Volt < 2.8f) {
      if (network.timeinfo.tm_sec % 1 == 0) {
        if (g == 1) { setTextColor(color565(255, 0, 0), config.theme.background); setCursor(BatX, BatY); print("\xA0\xA2\xA2\xA3"); }
        if (g >= 2) { g = 0; setTextColor(color565(255, 0, 0), config.theme.background); setCursor(BatX, BatY); print("\x9D\x9E\x9E\x9F"); }
        g++;
      }
    }

    if (network.timeinfo.tm_sec % 6 == 0) {
#if defined(USER_ADC_CHAN)
      float tempmVolt = 0;
      adc1_config_width(ADC_WIDTH_BIT_12);
      adc1_config_channel_atten(USER_ADC_CHAN, ADC_ATTEN_DB_12);
      esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 0, &adc1_chars);
      for (int i = 0; i < reads; i++) {
        tempmVolt += esp_adc_cal_raw_to_voltage(adc1_get_raw(USER_ADC_CHAN), &adc1_chars);
      }
#else
      // ADC_PIN не сопоставлен с каналом — задайте ADC_PIN в myoptions (как для NV3041A) или BATTERY_OFF.
      float tempmVolt = 0;
#endif

#if defined(USER_ADC_CHAN)
      float mVolt = (tempmVolt / reads) / 1000.0f;
#else
      float mVolt = 0;
#endif
      Volt = (mVolt + 0.0028f * mVolt * mVolt + 0.0096f * mVolt - 0.051f) / (ADC_R2 / (ADC_R1 + ADC_R2)) + DELTA;
      if (Volt < 1) Volt = 0;

      if ((Volt > Volt1) && (Volt1 > Volt2) && (Volt2 > Volt3) && (Volt3 > Volt4)) {
        Charging = true;
        setTextColor(color565(0, 255, 255), config.theme.background);
      } else {
        Charging = false;
      }
      Volt4 = Volt3;
      Volt3 = Volt2;
      Volt2 = Volt1;
      Volt1 = Volt;

      int idx = 0;
      while (true) {
        if (Volt < vs[idx + 1]) {
          mVolt = Volt - vs[idx];
          ChargeLevel = idx * 5 + round(mVolt / ((vs[idx + 1] - vs[idx]) / 5));
          break;
        }
        idx++;
      }
      if (ChargeLevel < 0) ChargeLevel = 0;
      if (ChargeLevel > 100) ChargeLevel = 100;

      setTextSize(BatFS);
      if (!Charging) {
        setCursor(BatX, BatY);
        if (Volt >= 3.82f) { setTextColor(color565(0, 255, 0), config.theme.background); print("\xA0\xA2\xA2\xA3"); }
        if ((Volt < 3.82f) && (Volt >= 3.72f)) { setTextColor(color565(0, 255, 0), config.theme.background); print("\x9D\xA2\xA2\xA3"); }
        if ((Volt < 3.72f) && (Volt >= 3.61f)) { setTextColor(color565(0, 255, 0), config.theme.background); print("\x9D\xA1\xA2\xA3"); }
        if ((Volt < 3.61f) && (Volt >= 3.46f)) { setTextColor(color565(0, 255, 0), config.theme.background); print("\x9D\x9E\xA2\xA3"); }
        if ((Volt < 3.46f) && (Volt >= 3.33f)) { setTextColor(color565(0, 255, 0), config.theme.background); print("\x9D\x9E\xA1\xA3"); }
        if ((Volt < 3.33f) && (Volt >= 3.20f)) { setTextColor(color565(255, 255, 0), config.theme.background); print("\x9D\x9E\x9E\xA3"); }
        if ((Volt < 3.20f) && (Volt >= 2.8f)) { setTextColor(color565(255, 0, 0), config.theme.background); print("\x9D\x9E\x9E\x9F"); }
      }

      if (Volt < 2.8f) { setTextColor(color565(255, 0, 0), config.theme.background); }

#ifndef HIDE_VOLT
      setTextSize(VoltFS);
      setCursor(VoltX, VoltY);
      printf("%.3fv", Volt);
#endif
      setTextSize(ProcFS);
      setCursor(ProcX, ProcY);
      printf("%3i%%", ChargeLevel);
    }
  }
#endif
}

void DspCore::_clockDate() {
  setTextSize(2);
#ifndef HIDE_DATE
  if (_olddateleft > 0) dsp.fillRect(_olddateleft, clockTop + 10, _olddatewidth, CHARHEIGHT, config.theme.background);
  dsp.fillRect(80, clockTop + 8, 230, 20, config.theme.background);
  setTextColor(config.theme.date, config.theme.background);
  setCursor(_dateleft, clockTop + 10);
  setTextSize(2);
  if (!config.isScreensaver) print(_dateBuf);
#endif
  strlcpy(_oldDateBuf, _dateBuf, sizeof(_oldDateBuf));
  _olddatewidth = _datewidth;
  _olddateleft = _dateleft;
  setTextColor(config.theme.dow, config.theme.background);
  setCursor(width() - 11 - clockRightSpace - CHARWIDTH * 6, clockTop - CHARHEIGHT * 3 - 19);
  print(utf8Rus(LANG::dow[network.timeinfo.tm_wday], true));
}

void DspCore::_clockTime(bool redraw) {
  if (_oldtimeleft > 0 && !CLOCKFONT_MONO) {
    const uint16_t clrTop = (clockTop > ST170_CLOCK_GLYPH_ABOVE) ? (uint16_t)(clockTop - ST170_CLOCK_GLYPH_ABOVE) : 0;
    dsp.fillRect(_oldtimeleft, clrTop, _oldtimewidth, ST170_CLOCK_CLEAR_H, config.theme.background);
  }
  _timeleft = width() - clockRightSpace - CHARWIDTH * 3 * 2 - 24 - _timewidth;
  setTextSize(1);
  setFont(&Clock_GFXfont);

  if (CLOCKFONT_MONO) {
    setCursor(_timeleft, clockTop);
    setTextColor(config.theme.clockbg, config.theme.background);
    print("88:88");
  }
  setTextColor(config.theme.clock, config.theme.background);

  if (config.isScreensaver) {
    /* Обложка только в playing+blank — в _clockTime для SCREENSAVER не рисуем. */
    fillRect(0, 0, width(), height(), config.theme.background);
  }

  setCursor(_timeleft, clockTop);
  print(_timeBuf);
  setFont();
  strlcpy(_oldTimeBuf, _timeBuf, sizeof(_oldTimeBuf));
  _oldtimewidth = _timewidth;
  _oldtimeleft = _timeleft;

  if (!config.isScreensaver) {
    drawFastVLine(width() - clockRightSpace - CHARWIDTH * 3 * 2 - 19, clockTop - clockTimeHeight, clockTimeHeight + 3, config.theme.div);
    drawFastHLine(width() - clockRightSpace - CHARWIDTH * 3 * 2 - 19, clockTop - clockTimeHeight + 29, 44, config.theme.div);
  }

  sprintf(_buffordate, "%2d %s %d", network.timeinfo.tm_mday, LANG::mnths[network.timeinfo.tm_mon], network.timeinfo.tm_year + 1900);
  strlcpy(_dateBuf, utf8Rus(_buffordate, false), sizeof(_dateBuf));
  _datewidth = strlen(_dateBuf) * CHARWIDTH * 2;
  _dateleft = _timeleft + (width() - _timeleft - _datewidth - clockRightSpace) / 2;
}

void DspCore::printClock(uint16_t top, uint16_t rightspace, uint16_t timeheight, bool redraw) {
  clockTop = top;
  if (config.isScreensaver) {
    if (clockTop < ST170_CLOCK_MIN_TOP) clockTop = ST170_CLOCK_MIN_TOP;
    if (clockTop > ST170_CLOCK_MAX_TOP) clockTop = ST170_CLOCK_MAX_TOP;
  }
  // left из clockConf может быть отрицательным (yoRadio: -10), передан как uint16_t.
  clockRightSpace = (int16_t)rightspace;
  clockTimeHeight = timeheight;
  strftime(_timeBuf, sizeof(_timeBuf), "%H:%M", &network.timeinfo);
  if (strcmp(_oldTimeBuf, _timeBuf) != 0 || redraw) {
    _getTimeBounds();
    _clockTime(redraw);
    if (!config.isScreensaver) {
      if (strcmp(_oldDateBuf, _dateBuf) != 0 || redraw) _clockDate();
    }
  }
  _clockSeconds();
}

void DspCore::clearClock() {
  const uint16_t clrTop = (clockTop > ST170_CLOCK_GLYPH_ABOVE) ? (uint16_t)(clockTop - ST170_CLOCK_GLYPH_ABOVE) : 0;
  dsp.fillRect(_timeleft, clrTop, _timewidth + CHARWIDTH * 3 * 2 + 24, ST170_CLOCK_CLEAR_H, config.theme.background);
}

void DspCore::startWrite(void) { Adafruit_ST7789::startWrite(); }
void DspCore::endWrite(void) { Adafruit_ST7789::endWrite(); }
void DspCore::loop(bool force) { (void)force; }

void DspCore::charSize(uint8_t textsize, uint8_t& w, uint16_t& h) {
  w = textsize * CHARWIDTH;
  h = textsize * CHARHEIGHT;
}

void DspCore::setTextSize(uint8_t s) { Adafruit_GFX::setTextSize(s); }

void DspCore::flip() {
  setRotation(config.store.flipscreen ? 3 : 1);
}

void DspCore::invert() {
  invertDisplay(!config.store.invertdisplay);
}

void DspCore::sleep(void) { enableSleep(true); delay(150); enableDisplay(false); delay(150); }
void DspCore::wake(void) { enableDisplay(true); delay(150); enableSleep(false); delay(150); }

void DspCore::writePixel(int16_t x, int16_t y, uint16_t color) {
  if (_clipping) {
    if ((x < _cliparea.left) || (x > _cliparea.left + _cliparea.width) || (y < _cliparea.top) || (y > _cliparea.top + _cliparea.height)) return;
  }
  Adafruit_ST7789::writePixel(x, y, color);
}

void DspCore::writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (_clipping) {
    if ((x < _cliparea.left) || (x >= _cliparea.left + _cliparea.width) || (y < _cliparea.top) || (y > _cliparea.top + _cliparea.height)) return;
  }
  Adafruit_ST7789::writeFillRect(x, y, w, h, color);
}

void DspCore::setClipping(clipArea ca) {
  _cliparea = ca;
  _clipping = true;
}

void DspCore::clearClipping() {
  _clipping = false;
}

void DspCore::setNumFont() {
  setFont(&Clock_GFXfont);
  setTextSize(1);
}

#endif
