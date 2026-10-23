/* PLAYER+PLAY (ST7789 / ILI9488 landscape / AXS Faster + SPECTRUM): «пирамидка» в узком слоте слева —
 * сверху иконка, под ней температура, ниже город (короткий — центр; длинный — бегущая строка).
 * ScrollWidget погоды на этом профиле не показываем (вспышка при выходе из SS).
 * Город — свой лёгкий скролл (не ScrollWidget / scrollId — не борется с title/meta).
 * ST7789_PLR_* задаются в conf.h профиля.
 * Иконки: Visual Crossing 2nd Set Color → weather_vc2_48.h (JC2432) или _64.h (ILI9488/AXS PLAYER).
 */
#if defined(ST7789_PLR_VC2_64) && ST7789_PLR_VC2_64
#include "fonts/weather_vc2_64.h"
#define PLR_VC2_ICON_COUNT  WEATHER_VC2_64_ICON_COUNT
#define PLR_VC2_W           WEATHER_VC2_64_W
#define PLR_VC2_H           WEATHER_VC2_64_H
#define PLR_VC2_ICONS       weather_vc2_64_icons
#else
#include "fonts/weather_vc2_48.h"
#define PLR_VC2_ICON_COUNT  WEATHER_VC2_ICON_COUNT
#define PLR_VC2_W           WEATHER_VC2_W
#define PLR_VC2_H           WEATHER_VC2_H
#define PLR_VC2_ICONS       weather_vc2_icons
#endif

namespace st7789_player_weather {

static bool g_active = false;
static int16_t g_slotX = 0;
static int16_t g_slotY = 0;
static int16_t g_slotW = 0;

static uint16_t g_clipX = 0;
static uint16_t g_clipY = 0;
static uint16_t g_clipW = 0;
static uint16_t g_clipH = 0;

static uint8_t g_lastIconIdx = 255;
static bool g_lastTempValid = false;
static int16_t g_lastTempRounded = 0;
static uint8_t g_lastTempBand = 0;
static char g_lastCityDrawn[60] = "";
static bool g_dirty = false;

/* Бегущая строка города: окно = ширина слота пирамидки. */
static char g_cityText[60] = "";
static bool g_cityNeedScroll = false;
static int16_t g_cityOff = 0;
static uint16_t g_cityTextW = 0;
static int16_t g_cityBaseY = 0;
static uint32_t g_cityNextMs = 0;
static const uint16_t kCityScrollMs = 45;
static const uint16_t kCityScrollStartMs = 1500;
static const int16_t kCityScrollGapPx = 18;
/* Отступ окна города от краёв слота пирамидки (анти-артефакты на правом краю глифа). */
static const int16_t kCityWinPadPx = 2;

static bool shouldShowBlock() {
  return config.store.showweather
      && strlen(config.store.weatherkey) > 0
      && (timekeeper.weatherIcon[0] != '\0'
          || timekeeper.weatherCity[0] != '\0'
          || timekeeper.weatherTempValid);
}

/* OWM icon "01d"/"01n"/… → индекс в PLR_VC2_ICONS[]. Учитываем день/ночь. */
static uint8_t owmIconToIndex(const char *icon) {
  if (!icon || icon[0] < '0' || icon[1] < '0') return 255;
  const int code = (icon[0] - '0') * 10 + (icon[1] - '0');
  const bool night = (icon[2] == 'n' || icon[2] == 'N');
  switch (code) {
    case 1:  return night ? 1u : 0u;  /* clear-night / clear-day */
    case 2:  return night ? 3u : 2u;  /* partly-cloudy-* */
    case 3:  return 4u;              /* cloudy */
    case 4:  return 4u;              /* overcast → cloudy */
    case 9:  return night ? 6u : 5u; /* showers-* */
    case 10: return 7u;              /* rain */
    case 11: return 8u;              /* thunder-rain */
    case 13: return 9u;              /* snow */
    case 50: return 10u;             /* fog */
    default: return 4u;
  }
}

static char *cityTitleLabel(const char *owmCity) {
  static char buf[60];
  strlcpy(buf, utf8Rus(owmCity, false), sizeof(buf));
  if (buf[0] != '\0') {
    const uint8_t c = (uint8_t)buf[0];
    if (c >= 0xE0 && c <= 0xFF) {
      buf[0] = (char)(c - 0x20);
    } else if (c == 0xB8) {
      buf[0] = (char)0xA8;
    } else if (c >= 'a' && c <= 'z') {
      buf[0] = (char)toupper(c);
    }
  }
  return buf;
}

static const char *tempLabel() {
  static char buf[16];
  if (!timekeeper.weatherTempValid) {
    buf[0] = '\0';
    return buf;
  }
  snprintf(buf, sizeof(buf), "%+.0f\260C", (double)timekeeper.weatherTemp);
  return buf;
}

static uint16_t gfxTextWidth(const char *txt) {
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  dsp.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  return w;
}

static int16_t gfxTextCenterX(int16_t anchorCenterX, const char *txt) {
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  dsp.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  return anchorCenterX - (int16_t)(w / 2) - x1;
}

static int16_t gfxBaselineForCenterY(int16_t centerY, const char *txt, uint8_t textSize) {
  dsp.setTextSize(textSize);
  dsp.setFont();
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  dsp.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  return centerY - (int16_t)y1 - (int16_t)(h / 2u);
}

static uint8_t tempBand(float tempC) {
  if (tempC > 30.0f) return 5;
  if (tempC > 18.0f) return 4;
  if (tempC >= 5.0f) return 3;
  if (tempC >= -5.0f) return 2;
  return 1;
}

static uint16_t tempColor(float tempC) {
  switch (tempBand(tempC)) {
    case 5: return 0xF800;
    case 4: return 0xFD20;
    case 3: return 0xFFE0;
    case 2: return 0x07FF;
    default: return 0x001F;
  }
}

/* Вертикальная «пирамидка»: иконка → t° → город, всё по центру слота. */
struct PlayerWeatherLayout {
  int16_t blockX;
  int16_t blockY;
  int16_t blockW;
  int16_t blockH;
  int16_t slotCenterX;
  int16_t iconX;
  int16_t iconY;
  int16_t tempBaselineY;
  int16_t cityBaselineY;
};

static void calcLayout(PlayerWeatherLayout &layout) {
  layout.blockX = g_slotX;
  layout.blockW = g_slotW;
  layout.slotCenterX = g_slotX + g_slotW / 2;

  /* Иконка: ещё +3 px вверх (было −6) — запас над пирамидкой, город дальше от спектра. */
  layout.iconY = g_slotY - 9;
  if (layout.iconY < 0) layout.iconY = 0;
  layout.iconX = layout.slotCenterX - (int16_t)(ST7789_PLR_WICON_W / 2u);

  /* t° и город якорятся к низу иконки; зазоры ужаты, чтобы «Москва» не ложилась на анализатор. */
  int16_t tempCenterY = layout.iconY + (int16_t)ST7789_PLR_WICON_H
                      + (int16_t)ST7789_PLR_ROW_GAP + 2;
  if (timekeeper.weatherTempValid) {
    layout.tempBaselineY = gfxBaselineForCenterY(tempCenterY, tempLabel(),
                                                   ST7789_PLR_WTEMP_TEXT_SIZE);
  } else {
    layout.tempBaselineY = tempCenterY;
  }

  int16_t cityCenterY = tempCenterY
                      + (int16_t)(8 * ST7789_PLR_WTEMP_TEXT_SIZE)
                      + (int16_t)ST7789_PLR_ROW_GAP;
  if (timekeeper.weatherCity[0] != '\0') {
    layout.cityBaselineY = gfxBaselineForCenterY(cityCenterY,
        cityTitleLabel(timekeeper.weatherCity), ST7789_PLR_WCITY_TEXT_SIZE);
  } else {
    layout.cityBaselineY = cityCenterY;
  }

  /* Зона очистки от поднятой иконки до низа города. */
  layout.blockY = layout.iconY;
  layout.blockH = (int16_t)g_clipH + (g_slotY - layout.iconY);
}

static void clearBlock(const PlayerWeatherLayout &layout) {
  dsp.fillRect(layout.blockX, layout.blockY, layout.blockW, layout.blockH,
               config.theme.background);
}

/* Bitmap VC2 вместо геометрических кругов/линий. */
static void drawWeatherIconKind(int16_t ox, int16_t oy, uint8_t kind) {
  if (kind >= PLR_VC2_ICON_COUNT) return;
#if DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  /* Arduino_GFX draw16bitRGBBitmap — non-const bitmap (как boot logo на AXS). */
  dsp.drawRGBBitmap(ox, oy, const_cast<uint16_t*>(PLR_VC2_ICONS[kind]),
                    PLR_VC2_W, PLR_VC2_H);
#else
  dsp.drawRGBBitmap(ox, oy, PLR_VC2_ICONS[kind], PLR_VC2_W, PLR_VC2_H);
#endif
}

static void drawTemp(const PlayerWeatherLayout &layout) {
  if (!timekeeper.weatherTempValid) return;
  const char *tLabel = tempLabel();
  if (tLabel[0] == '\0') return;
  dsp.setTextSize(ST7789_PLR_WTEMP_TEXT_SIZE);
  dsp.setFont();
  dsp.setTextColor(tempColor(timekeeper.weatherTemp), config.theme.background);
  const int16_t tx = gfxTextCenterX(layout.slotCenterX, tLabel);
  dsp.setClipping({ g_clipX, g_clipY, g_clipW, g_clipH });
  dsp.setCursor(tx, layout.tempBaselineY);
  dsp.print(tLabel);
  dsp.clearClipping();
}

static void resetCityMarquee() {
  g_cityOff = 0;
  g_cityNextMs = millis() + kCityScrollStartMs;
}

static void clearCityMarqueeState() {
  g_cityText[0] = '\0';
  g_cityNeedScroll = false;
  g_cityOff = 0;
  g_cityTextW = 0;
  g_cityBaseY = 0;
  g_cityNextMs = 0;
}

/* Только полоса города — без clearBlock всей пирамидки (тик скролла). */
static void paintCityMarquee(const PlayerWeatherLayout &layout) {
  if (g_cityText[0] == '\0') return;

  const uint8_t ts = ST7789_PLR_WCITY_TEXT_SIZE;
  const uint16_t th = (uint16_t)(8u * ts);
  int16_t winX = layout.blockX + kCityWinPadPx;
  int16_t winW = layout.blockW - (int16_t)(2 * kCityWinPadPx);
  if (winW <= 0) return;
  g_cityBaseY = layout.cityBaselineY;

  dsp.setTextSize(ts);
  dsp.setFont();
  dsp.setClipping({ (uint16_t)winX, (uint16_t)g_cityBaseY, (uint16_t)winW, th });
  dsp.fillRect(winX, g_cityBaseY, winW, th, config.theme.background);
  /* Без цвета фона у глифа — иначе при скролле пиксели/fill левее окна портят картинку. */
  dsp.setTextColor(config.theme.weathercity);
  if (!g_cityNeedScroll) {
    int16_t tx = gfxTextCenterX(layout.slotCenterX, g_cityText);
    if (tx < winX) tx = winX;
    dsp.setCursor(tx, g_cityBaseY);
    dsp.print(g_cityText);
  } else {
    const int16_t cycle = (int16_t)g_cityTextW + kCityScrollGapPx;
    dsp.setCursor(winX - g_cityOff, g_cityBaseY);
    dsp.print(g_cityText);
    dsp.setCursor(winX - g_cityOff + cycle, g_cityBaseY);
    dsp.print(g_cityText);
  }
  dsp.clearClipping();
}

static void setupCityMarquee(const PlayerWeatherLayout &layout, const char *cityDrawn) {
  strlcpy(g_cityText, cityDrawn, sizeof(g_cityText));
  if (g_cityText[0] == '\0') {
    clearCityMarqueeState();
    return;
  }
  dsp.setTextSize(ST7789_PLR_WCITY_TEXT_SIZE);
  dsp.setFont();
  g_cityTextW = gfxTextWidth(g_cityText);
  const int16_t winW = layout.blockW - (int16_t)(2 * kCityWinPadPx);
  g_cityNeedScroll = (winW > 0) && (g_cityTextW > (uint16_t)winW);
  resetCityMarquee();
  paintCityMarquee(layout);
}

static void resetCache() {
  g_lastIconIdx = 255;
  g_lastTempValid = false;
  g_lastTempRounded = 0;
  g_lastTempBand = 0;
  g_lastCityDrawn[0] = '\0';
  g_dirty = false;
  clearCityMarqueeState();
}

void setSlot(int16_t x, int16_t y, int16_t w) {
  g_slotX = x;
  g_slotY = y;
  g_slotW = w;
  g_clipX = (x > 0) ? (uint16_t)x : 0u;
  g_clipY = (y > 0) ? (uint16_t)y : 0u;
  g_clipW = (w > 0) ? (uint16_t)w : 0u;
  /* Высота «пирамидки»: иконка + t°(size) + город + зазоры + запас. */
  g_clipH = (uint16_t)(ST7789_PLR_WICON_H
                       + (uint16_t)(8 * ST7789_PLR_WTEMP_TEXT_SIZE)
                       + 8
                       + ST7789_PLR_ROW_GAP * 2u + 8u);
}

void activate() {
  g_active = true;
  resetCache();
}

void deactivate() {
  if (!g_active) return;
  PlayerWeatherLayout layout;
  calcLayout(layout);
  clearBlock(layout);
  g_active = false;
  resetCache();
}

bool isActive() {
  return g_active;
}

void notifyUpdated() {
  g_dirty = true;
}

void tickCityMarquee() {
  if (!g_active || !g_cityNeedScroll || g_cityText[0] == '\0') return;
  if (!shouldShowBlock()) return;
  const uint32_t now = millis();
  if ((int32_t)(now - g_cityNextMs) < 0) return;
  g_cityNextMs = now + kCityScrollMs;
  g_cityOff++;
  const int16_t cycle = (int16_t)g_cityTextW + kCityScrollGapPx;
  if (g_cityOff >= cycle) g_cityOff = 0;

  PlayerWeatherLayout layout;
  calcLayout(layout);
  paintCityMarquee(layout);
}

void redraw(bool fullRedraw) {
  if (!g_active) return;

  PlayerWeatherLayout layout;
  calcLayout(layout);

  if (!shouldShowBlock()) {
    if (g_lastIconIdx != 255 || g_lastTempValid || g_lastCityDrawn[0] != '\0') {
      clearBlock(layout);
      resetCache();
    }
    return;
  }

  uint8_t idx = 255;
  if (timekeeper.weatherIcon[0] != '\0') {
    idx = owmIconToIndex(timekeeper.weatherIcon);
    if (idx >= PLR_VC2_ICON_COUNT) idx = 4; /* cloudy fallback */
  }
  const bool tempValid = timekeeper.weatherTempValid;
  const int16_t tempRounded = tempValid ? (int16_t)lroundf(timekeeper.weatherTemp) : 0;
  const uint8_t band = tempValid ? tempBand(timekeeper.weatherTemp) : 0;
  char cityDrawn[60];
  cityDrawn[0] = '\0';
  if (timekeeper.weatherCity[0] != '\0') {
    strlcpy(cityDrawn, cityTitleLabel(timekeeper.weatherCity), sizeof(cityDrawn));
  }

  /* fullRedraw=true с часов каждую секунду: слот мог быть затёрт clearClock —
   * нужно перерисовать, но clearBlock при том же контенте даёт моргание. */
  const bool contentSame = idx == g_lastIconIdx
      && tempValid == g_lastTempValid
      && (!tempValid || tempRounded == g_lastTempRounded)
      && (!tempValid || band == g_lastTempBand)
      && strcmp(cityDrawn, g_lastCityDrawn) == 0;
  if (!fullRedraw && !g_dirty && contentSame) return;

  /* Стираем только при смене данных / dirty; после тика часов — рисуем поверх. */
  if (g_dirty || !contentSame) {
    clearBlock(layout);
  }
  if (idx < PLR_VC2_ICON_COUNT) {
    drawWeatherIconKind(layout.iconX, layout.iconY, idx);
  }
  drawTemp(layout);
  if (!contentSame || g_dirty) {
    setupCityMarquee(layout, cityDrawn);
  } else {
    /* Тот же город после clearClock — продолжить скролл с текущим offset. */
    strlcpy(g_cityText, cityDrawn, sizeof(g_cityText));
    paintCityMarquee(layout);
  }

  g_lastIconIdx = idx;
  g_lastTempValid = tempValid;
  g_lastTempRounded = tempRounded;
  g_lastTempBand = band;
  strlcpy(g_lastCityDrawn, cityDrawn, sizeof(g_lastCityDrawn));
  g_dirty = false;
}

} // namespace st7789_player_weather
