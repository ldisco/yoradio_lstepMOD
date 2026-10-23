#include "../core/options.h"
#if DSP_MODEL==DSP_ST7701

#include "displayST7701.h"
#include "../core/config.h"
#include "../core/network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Тонкий bring-up ST7701 для 4848S040 (профиль Supremus).
 * Init: st7701_type9_init_operations (Guition / Arduino_GFX ESP32_4848S040_86BOX_GUITION).
 * Пины — из myoptions (ST7701_* / GFX_BL).
 *
 * Bounce = 0: на стоке Arduino_GFX без XIP/IRAM_SAFE bounce даёт Cache panic.
 * Spike Opus 4.1 (bounce+custom_sdkconfig) поле 2026-09-24 — НЕ OK; env снят.
 */

static Arduino_DataBus *s_st7701Bus = new Arduino_SWSPI(
  GFX_NOT_DEFINED /* DC */, ST7701_CS,
  ST7701_SCK, ST7701_SDA, GFX_NOT_DEFINED /* MISO */);

static Arduino_ESP32RGBPanel *s_st7701Rgb = new Arduino_ESP32RGBPanel(
  ST7701_DE, ST7701_VSYNC, ST7701_HSYNC, ST7701_PCLK,
  ST7701_R0, ST7701_R1, ST7701_R2, ST7701_R3, ST7701_R4,
  ST7701_G0, ST7701_G1, ST7701_G2, ST7701_G3, ST7701_G4, ST7701_G5,
  ST7701_B0, ST7701_B1, ST7701_B2, ST7701_B3, ST7701_B4,
  1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
  1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */,
  /* prefer_speed 8 MHz (меньше голод DMA). Bounce запрещён на стоке. */
  0 /* pclk_active_neg */, 8000000 /* prefer_speed */, false /* useBigEndian */,
  0 /* de_idle_high */, 0 /* pclk_idle_high */, 0 /* bounce_buffer_size_px */);

#if ST7701_BOOT_ORIENTATION_180
static const uint8_t s_st7701BootRot = 2;
#else
static const uint8_t s_st7701BootRot = 0;
#endif

/*
 * DspCore = Arduino_RGB_Display: прямой вывод в RGB FB (auto_flush).
 * Canvas soft-rot как у AXS здесь не нужен — панель квадрат 480×480.
 */
DspCore::DspCore()
  : Arduino_RGB_Display(
      480, 480, s_st7701Rgb, s_st7701BootRot, true /* auto_flush */,
      s_st7701Bus, GFX_NOT_DEFINED /* RST */,
      st7701_type9_init_operations, sizeof(st7701_type9_init_operations)) {}

/* ---- подсветка LEDC (как Faster / AXS), без analogWrite на BRIGHTNESS_PIN ---- */
static bool s_st7701BlAttached = false;
static bool s_st7701BlBootHold = true;

static void st7701BacklightInit() {
  if (s_st7701BlAttached) return;
  /* Guition 4848S040 + AP3031: 5 kHz — обрез ~55%; 1 kHz — обрез ~10–13%.
   * 150 Hz / 8 bit на ESP32-S3 Arduino: ledc «div_param=0» → attach FAIL → BL
   * остаётся LOW (чёрный экран). 250 Hz / 8 bit даёт целый делитель (80M/256/250).
   * Запас: 1000 Hz; крайний случай — GPIO HIGH. */
  if (ledcAttach((uint8_t)GFX_BL, 250.0, 8)) {
    s_st7701BlAttached = true;
    Serial.println("[ST7701] backlight LEDC 250Hz/8bit on GFX_BL");
    return;
  }
  Serial.println("[ST7701] ledcAttach(GFX_BL, 250, 8) FAILED — fallback 1000Hz");
  if (ledcAttach((uint8_t)GFX_BL, 1000.0, 8)) {
    s_st7701BlAttached = true;
    Serial.println("[ST7701] backlight LEDC 1kHz/8bit on GFX_BL");
    return;
  }
  Serial.println("[ST7701] ledcAttach FAILED — BL GPIO HIGH (no PWM)");
  pinMode((uint8_t)GFX_BL, OUTPUT);
  digitalWrite((uint8_t)GFX_BL, HIGH);
}

static void st7701BacklightLevel(uint8_t duty255) {
  /* Hold: не поднимать свет до первого кадра boot-лого. */
  if (s_st7701BlBootHold && duty255 != 0) return;
  st7701BacklightInit();
  if (!s_st7701BlAttached) {
    /* PWM нет: 0 = OFF, иначе полный свет. */
    pinMode((uint8_t)GFX_BL, OUTPUT);
    digitalWrite((uint8_t)GFX_BL, duty255 ? HIGH : LOW);
    return;
  }
  /* Не дублировать тот же duty — лишний ledcWrite на RGB + AP3031 даёт рывок. */
  static uint8_t s_lastDuty = 0;
  static bool s_haveDuty = false;
  if (s_haveDuty && duty255 == s_lastDuty) return;
  s_lastDuty = duty255;
  s_haveDuty = true;
  ledcWrite((uint8_t)GFX_BL, duty255);
}

void st7701SetBacklightDuty(uint8_t duty255) {
  st7701BacklightLevel(duty255);
}

static void st7701BacklightFromConfig() {
  st7701BacklightLevel((uint8_t)map(config.store.brightness, 0, 100, 0, 255));
}

static void st7701BacklightReleaseBoot() {
  if (!s_st7701BlBootHold) return;
  s_st7701BlBootHold = false;
  st7701BacklightFromConfig();
  Serial.println("[ST7701] backlight ON after first logo frame");
}

#if ST7701_RGB_SOAK
/*
 * Opus 5.5 soak: геометрия (сдвиг) vs яркость/VCOM на 50% сером.
 * Шаг задаётся ST7701_RGB_SOAK_STEP ('a'…'e') на этапе сборки.
 */
char st7701RgbSoakStep() {
  const char s = (char)ST7701_RGB_SOAK_STEP;
  if (s >= 'a' && s <= 'e') return s;
  if (s >= 'A' && s <= 'E') return (char)(s - 'A' + 'a');
  return 'a';
}

static void st7701RgbSoakApplyBl() {
  s_st7701BlBootHold = false;
  st7701BacklightInit();
  /* (a): duty 255 = почти DC при 250 Hz — частоту не меняем. (b…e): штатный % из store. */
  if (st7701RgbSoakStep() == 'a') {
    st7701BacklightLevel(255);
    Serial.println("[SOAK] BL duty=255 (diag DC)");
  } else {
    st7701BacklightFromConfig();
    Serial.printf("[SOAK] BL duty from store.brightness=%u%%\n",
                  (unsigned)config.store.brightness);
  }
}

static void st7701RgbSoakDrawPattern() {
  /* 50% серый — на нём лучше видно VCOM/мерцание (Opus). */
  const uint16_t gray = ((uint16_t)(16) << 11) | ((uint16_t)(32) << 5) | (uint16_t)(16);
  const uint16_t ink = 0xFFFF;
  const uint16_t accent = 0xF800; /* красный маркер шага */
  dsp.fillScreen(gray);

  /* Вертикальные линии 1 px (левая половина). */
  for (int16_t x = 0; x < 240; x += 8) {
    dsp.drawFastVLine(x, 40, 400, ink);
  }
  /* Сетка 40×40 (правая половина + низ). */
  for (int16_t x = 240; x < 480; x += 40) {
    dsp.drawFastVLine(x, 40, 400, ink);
  }
  for (int16_t y = 40; y < 480; y += 40) {
    dsp.drawFastHLine(0, y, 480, ink);
  }
  /* Поля серого без сетки — нижняя полоса для «моргания» без линий. */
  dsp.fillRect(0, 440, 480, 40, gray);

  dsp.setTextColor(accent, gray);
  dsp.setTextSize(3);
  dsp.setCursor(8, 8);
  dsp.printf("SOAK %c", st7701RgbSoakStep());
}

void st7701RgbSoakEnter() {
  Serial.printf("##[BOOT]#\t[SOAK] enter step=%c (Opus 5.5 static)\n", st7701RgbSoakStep());
  st7701RgbSoakDrawPattern();
  st7701RgbSoakApplyBl();
  Serial.flush();
}

void st7701RgbSoakRefresh() {
  st7701RgbSoakDrawPattern();
  /* BL не дёргать на каждом refresh — только картинка против случайного UI. */
}
#endif

void DspCore::initDisplay() {
  Serial.println("##[BOOT]#\tST7701 initDisplay begin");
  Serial.flush();

  /* Cold-boot: BL LOW до первого лого (без мусора GRAM на яркой панели). */
  pinMode((uint8_t)GFX_BL, OUTPUT);
  digitalWrite((uint8_t)GFX_BL, LOW);
  s_st7701BlAttached = false;
  s_st7701BlBootHold = true;

  begin();
  setTextWrap(false);
  setTextSize(1);
  flip();
  invert();
  fillScreen(config.theme.background);

  Serial.println("##[BOOT]#\tST7701 initDisplay done (BL hold until logo)");
  Serial.flush();
}

void DspCore::clearDsp(bool black) {
  fillScreen(black ? 0 : config.theme.background);
}

void DspCore::flip() {
  /* Квадрат: 0 / 2 (flipscreen = 180°). Boot-ориентация уже в конструкторе. */
  uint8_t base = ST7701_BOOT_ORIENTATION_180 ? 2 : 0;
  setRotation(config.store.flipscreen ? (uint8_t)((base + 2) % 4) : base);
}

void DspCore::invert() {
  invertDisplay(config.store.invertdisplay);
}

void DspCore::sleep(void) {
  /* RGB-панель: гасим только подсветку (displayOff на части ревизий GFX нестабилен). */
  st7701BacklightLevel(0);
}

void DspCore::wake(void) {
  st7701BacklightFromConfig();
}

/* ---- Boot logo 198×128 + bilinear scale (как Faster AXS) ---- */
static inline uint8_t st7701Rgb565R(uint16_t c) { return (uint8_t)((c >> 11) & 0x1F); }
static inline uint8_t st7701Rgb565G(uint16_t c) { return (uint8_t)((c >> 5) & 0x3F); }
static inline uint8_t st7701Rgb565B(uint16_t c) { return (uint8_t)(c & 0x1F); }

static inline uint16_t st7701Rgb565Pack(uint16_t r, uint16_t g, uint16_t b) {
  return (uint16_t)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F));
}

static uint16_t st7701SampleBootLogoBilinear(uint32_t x256, uint32_t y256) {
  uint16_t x0 = (uint16_t)(x256 >> 8);
  uint16_t y0 = (uint16_t)(y256 >> 8);
  uint8_t fx = (uint8_t)(x256 & 0xFFu);
  uint8_t fy = (uint8_t)(y256 & 0xFFu);

  if (x0 >= AXS_BOOT_LOGO_W)  { x0 = AXS_BOOT_LOGO_W - 1;  fx = 0; }
  if (y0 >= AXS_BOOT_LOGO_H)  { y0 = AXS_BOOT_LOGO_H - 1;  fy = 0; }
  const uint16_t x1 = (x0 + 1u < AXS_BOOT_LOGO_W) ? (uint16_t)(x0 + 1u) : x0;
  const uint16_t y1 = (y0 + 1u < AXS_BOOT_LOGO_H) ? (uint16_t)(y0 + 1u) : y0;

  const uint16_t c00 = (uint16_t)pgm_read_word(&axs_boot_logo[(uint32_t)y0 * AXS_BOOT_LOGO_W + x0]);
  const uint16_t c10 = (uint16_t)pgm_read_word(&axs_boot_logo[(uint32_t)y0 * AXS_BOOT_LOGO_W + x1]);
  const uint16_t c01 = (uint16_t)pgm_read_word(&axs_boot_logo[(uint32_t)y1 * AXS_BOOT_LOGO_W + x0]);
  const uint16_t c11 = (uint16_t)pgm_read_word(&axs_boot_logo[(uint32_t)y1 * AXS_BOOT_LOGO_W + x1]);

  const uint16_t ifx = (uint16_t)(256u - fx);
  const uint16_t ify = (uint16_t)(256u - fy);

  const uint16_t r0 = (uint16_t)((st7701Rgb565R(c00) * ifx + st7701Rgb565R(c10) * fx) >> 8);
  const uint16_t g0 = (uint16_t)((st7701Rgb565G(c00) * ifx + st7701Rgb565G(c10) * fx) >> 8);
  const uint16_t b0 = (uint16_t)((st7701Rgb565B(c00) * ifx + st7701Rgb565B(c10) * fx) >> 8);
  const uint16_t r1 = (uint16_t)((st7701Rgb565R(c01) * ifx + st7701Rgb565R(c11) * fx) >> 8);
  const uint16_t g1 = (uint16_t)((st7701Rgb565G(c01) * ifx + st7701Rgb565G(c11) * fx) >> 8);
  const uint16_t b1 = (uint16_t)((st7701Rgb565B(c01) * ifx + st7701Rgb565B(c11) * fx) >> 8);

  return st7701Rgb565Pack((uint16_t)((r0 * ify + r1 * fy) >> 8),
                          (uint16_t)((g0 * ify + g1 * fy) >> 8),
                          (uint16_t)((b0 * ify + b1 * fy) >> 8));
}

static void st7701BlitBootLogoScaled(DspCore *gfx, int16_t left, int16_t top,
                                     uint16_t dstW, uint16_t dstH) {
  if (!gfx || !dstW || !dstH) return;

  if (dstW == AXS_BOOT_LOGO_W && dstH == AXS_BOOT_LOGO_H) {
    gfx->draw16bitRGBBitmap(left, top,
                            const_cast<uint16_t*>(axs_boot_logo),
                            AXS_BOOT_LOGO_W, AXS_BOOT_LOGO_H);
    return;
  }

  /* Запас под pulse до ~125% (198×320/256 ≈ 247). */
  uint16_t line[256];
  if (dstW > (uint16_t)(sizeof(line) / sizeof(line[0]))) {
    dstW = (uint16_t)(sizeof(line) / sizeof(line[0]));
  }

  const uint32_t xDen = (dstW > 1u) ? (uint32_t)(dstW - 1u) : 1u;
  const uint32_t yDen = (dstH > 1u) ? (uint32_t)(dstH - 1u) : 1u;

  for (uint16_t y = 0; y < dstH; ++y) {
    const uint32_t y256 = ((uint32_t)y * (AXS_BOOT_LOGO_H - 1u) * 256u) / yDen;
    for (uint16_t x = 0; x < dstW; ++x) {
      const uint32_t x256 = ((uint32_t)x * (AXS_BOOT_LOGO_W - 1u) * 256u) / xDen;
      line[x] = st7701SampleBootLogoBilinear(x256, y256);
    }
    gfx->draw16bitRGBBitmap(left, (int16_t)(top + (int16_t)y), line, dstW, 1);
  }
}

void DspCore::drawLogo(uint16_t top) {
  /*
   * Boot-лого Supremus 480×480: PROGMEM 198×128 + заметный scale-pulse.
   * Старт ~50%, пик ~125%, финал 100%; ~13 кадров × 55 мс ≈ 0.7 с.
   * Слот pulse центрируем по экрану (top из conf — запасной ориентир).
   * Пик H≈160: низ слота ≈320, до progress Y=324. BL после 1-го кадра.
   */
  (void)top;
  static const uint16_t kPulseQ8[] = {
    128, 155, 185, 215, 245, 270, 295, 320, 300, 280, 265, 258, 256
  };
  constexpr uint16_t kMaxQ8 = 320;
  const uint16_t maxW = (uint16_t)(((uint32_t)AXS_BOOT_LOGO_W * kMaxQ8) / 256u);
  const uint16_t maxH = (uint16_t)(((uint32_t)AXS_BOOT_LOGO_H * kMaxQ8) / 256u);
  const int16_t areaL = (int16_t)(((int16_t)width() - (int16_t)maxW) / 2);
  /* Ровно по центру экрана: (H − maxH) / 2, не bootLogoTop сверху. */
  const int16_t areaT = (int16_t)(((int16_t)height() - (int16_t)maxH) / 2);
  const uint16_t bg = config.theme.background;

  for (uint8_t i = 0; i < (uint8_t)(sizeof(kPulseQ8) / sizeof(kPulseQ8[0])); ++i) {
    const uint16_t q = kPulseQ8[i];
    const uint16_t dw = (uint16_t)(((uint32_t)AXS_BOOT_LOGO_W * q) / 256u);
    const uint16_t dh = (uint16_t)(((uint32_t)AXS_BOOT_LOGO_H * q) / 256u);
    const int16_t lx = (int16_t)(areaL + (int16_t)((maxW - dw) / 2));
    const int16_t ly = (int16_t)(areaT + (int16_t)((maxH - dh) / 2));

    fillRect(areaL, areaT, (int16_t)maxW, (int16_t)maxH, bg);
    st7701BlitBootLogoScaled(this, lx, ly, dw, dh);
    if (i == 0) st7701BacklightReleaseBoot();
    vTaskDelay(pdMS_TO_TICKS(55));
  }
}

/* ---- часы / NumWidget: PLAYER=DS_DIGI56, SS=DS_DIGI68 (как Faster) ---- */
static GFXglyph *st7701Glyph(const GFXfont *font, uint8_t c) {
  return font->glyph + c;
}

static uint8_t st7701CharAdvance(unsigned char c, const GFXfont *font) {
  GFXglyph *glyph = st7701Glyph(font, (uint8_t)(c - 0x20));
  return pgm_read_byte(&glyph->xAdvance);
}

static uint16_t st7701TextWidth(const char *txt, const GFXfont *font) {
  uint16_t w = 0;
  for (const char *p = txt; *p; ++p) w = (uint16_t)(w + st7701CharAdvance((unsigned char)*p, font));
  return w;
}

/* DIGI56: height 70, yOffset −69. DIGI68: height 85, yOffset −84. */
static const uint16_t ST7701_CLOCK_GLYPH_ABOVE = 70;
static const uint16_t ST7701_CLOCK_SS_GLYPH_ABOVE = 85;
/* Как Faster: секунды/день size4, дата size1 под HH:MM. */
static const uint8_t ST7701_CLK_SUPER = 4;
static const uint8_t ST7701_CLK_SPACE = 10;
static const uint8_t ST7701_CLK_DATE_SZ = 1;
static const uint16_t ST7701_CLOCK_BLOCK_H =
    (uint16_t)(ST7701_CLOCK_GLYPH_ABOVE + ST7701_CLK_SPACE + CHARHEIGHT * ST7701_CLK_DATE_SZ + 2);
static const uint16_t ST7701_CLOCK_SS_H = (uint16_t)(ST7701_CLOCK_SS_GLYPH_ABOVE + 4);

static uint16_t s_clkLeft = 0, s_clkTop = 0, s_clkWidth = 0;
static uint16_t s_clkDotsLeft = 0, s_clkLinesLeft = 0;
static char s_clkOldTime[8] = "";
static bool s_clkDots = true;
static bool s_st7701WasScreensaver = false;

static const GFXfont *st7701ClockFont() {
  return config.isScreensaver ? &DS_DIGI68pt7b : &Clock_GFXfont;
}

static uint16_t st7701GlyphAbove() {
  return config.isScreensaver ? ST7701_CLOCK_SS_GLYPH_ABOVE : ST7701_CLOCK_GLYPH_ABOVE;
}

static uint16_t st7701ClockClearH() {
  return config.isScreensaver ? ST7701_CLOCK_SS_H : ST7701_CLOCK_BLOCK_H;
}

void DspCore::charSize(uint8_t textsize, uint8_t& width, uint16_t& height) {
  width = textsize * CHARWIDTH;
  height = textsize * CHARHEIGHT;
}

uint16_t DspCore::textWidth(const char *txt) {
  return st7701TextWidth(txt, st7701ClockFont());
}

void DspCore::setNumFont() {
  setFont(&Clock_GFXfont);
  setTextSize(1);
}

void DspCore::clearClock() {
  if (s_clkWidth == 0) return;
  const uint16_t above = st7701GlyphAbove();
  const uint16_t clrTop =
      (s_clkTop > above) ? (uint16_t)(s_clkTop - above) : 0;
  fillRect((int16_t)s_clkLeft, (int16_t)clrTop,
           (int16_t)s_clkWidth + 2, (int16_t)st7701ClockClearH(), config.theme.background);
}

void DspCore::printClock(uint16_t top, uint16_t rightspace, uint16_t timeheight, bool redraw) {
  (void)timeheight;
  const bool ss = config.isScreensaver;
  const GFXfont *font = st7701ClockFont();
  const uint16_t glyphAbove = st7701GlyphAbove();

  /* Смена PLAYER↔SS — полная перерисовка (иначе чужой шрифт / секунды/дата). */
  if (ss != s_st7701WasScreensaver) {
    redraw = true;
    if (s_clkWidth > 0) {
      const uint16_t aboveMax =
          (ST7701_CLOCK_SS_GLYPH_ABOVE > ST7701_CLOCK_GLYPH_ABOVE) ? ST7701_CLOCK_SS_GLYPH_ABOVE
                                                                   : ST7701_CLOCK_GLYPH_ABOVE;
      const uint16_t hMax =
          (ST7701_CLOCK_SS_H > ST7701_CLOCK_BLOCK_H) ? ST7701_CLOCK_SS_H : ST7701_CLOCK_BLOCK_H;
      const uint16_t clrTop =
          (s_clkTop > aboveMax) ? (uint16_t)(s_clkTop - aboveMax) : 0;
      fillRect((int16_t)s_clkLeft, (int16_t)clrTop,
               (int16_t)s_clkWidth + 2, (int16_t)hMax, config.theme.background);
    }
    s_clkWidth = 0;
    s_st7701WasScreensaver = ss;
  }

  char timeBuf[8];
  strftime(timeBuf, sizeof(timeBuf), "%H:%M", &network.timeinfo);
  const bool timeChanged = (strcmp(s_clkOldTime, timeBuf) != 0);
  const bool fullRedraw = redraw || timeChanged || (s_clkWidth == 0);

  const uint16_t timeW = st7701TextWidth(timeBuf, font);
  /* На SS колонка секунд/дня не нужна — только HH:MM. */
  const uint16_t rightside = ss ? 0
      : (uint16_t)(CHARWIDTH * ST7701_CLK_SUPER * 2 + ST7701_CLK_SPACE * 2 + 1);
  const uint16_t clockW = (uint16_t)(timeW + rightside);

  int16_t left;
  if (ss) {
    /* SS: moveTo задаёт абсолютный X в rightspace / _config.left. */
    left = (int16_t)rightspace;
    const int16_t maxLeft = (int16_t)width() - (int16_t)timeW - (int16_t)TFT_FRAMEWDT;
    if (left < (int16_t)TFT_FRAMEWDT) left = (int16_t)TFT_FRAMEWDT;
    if (maxLeft > (int16_t)TFT_FRAMEWDT && left > maxLeft) left = maxLeft;
  } else {
    left = (int16_t)width() - (int16_t)clockW - (int16_t)rightspace;
    if (left < 0) left = 0;
  }

  char hourBuf[4];
  strftime(hourBuf, sizeof(hourBuf), "%H", &network.timeinfo);
  const uint16_t dotsLeft = st7701TextWidth(hourBuf, font);
  const uint16_t linesLeft = (uint16_t)((uint16_t)left + timeW + ST7701_CLK_SPACE);

  if (fullRedraw) {
    if (s_clkWidth > 0) clearClock();
    else {
      const uint16_t clrTop =
          (top > glyphAbove) ? (uint16_t)(top - glyphAbove) : 0;
      fillRect(left, (int16_t)clrTop, (int16_t)clockW + 2, (int16_t)st7701ClockClearH(),
               config.theme.background);
    }
    s_clkLeft = (uint16_t)left;
    s_clkWidth = clockW;
    s_clkTop = top;
    s_clkDotsLeft = dotsLeft;
    s_clkLinesLeft = linesLeft;
    strlcpy(s_clkOldTime, timeBuf, sizeof(s_clkOldTime));

    setFont(font);
    setTextSize(1);
#if CLOCKFONT_MONO
    setTextColor(config.theme.clockbg, config.theme.background);
    setCursor(left, (int16_t)top);
    print("88:88");
#endif
    setTextColor(config.theme.clock, config.theme.background);
    setCursor(left, (int16_t)top);
    print(timeBuf);

    if (!ss) {
      /* Вертикаль + горизонт: секунды сверху, день снизу (как Faster). */
      drawFastVLine((int16_t)linesLeft, (int16_t)(top - ST7701_CLOCK_GLYPH_ABOVE),
                    (int16_t)ST7701_CLOCK_GLYPH_ABOVE, config.theme.div);
      drawFastHLine((int16_t)linesLeft, (int16_t)(top - ST7701_CLOCK_GLYPH_ABOVE / 2),
                    (int16_t)(CHARWIDTH * ST7701_CLK_SUPER * 2 + ST7701_CLK_SPACE), config.theme.div);

      /* День — две буквы после utf8Rus (пнд→пн), иначе вылезает за край. */
      setFont();
      setTextSize(ST7701_CLK_SUPER);
      setTextColor(config.theme.dow, config.theme.background);
      setCursor((int16_t)(linesLeft + ST7701_CLK_SPACE + 1),
                (int16_t)(top - CHARHEIGHT * ST7701_CLK_SUPER));
      {
        char dowBuf[8];
        strlcpy(dowBuf, utf8Rus(LANG::dow[network.timeinfo.tm_wday], false), sizeof(dowBuf));
        dowBuf[2] = '\0';
        print(dowBuf);
      }

#ifndef HIDE_DATE
      char dateTmp[40];
      snprintf(dateTmp, sizeof(dateTmp), "%2d %s %d", network.timeinfo.tm_mday,
               LANG::mnths[network.timeinfo.tm_mon], network.timeinfo.tm_year + 1900);
      char dateBuf[48];
      strlcpy(dateBuf, utf8Rus(dateTmp, true), sizeof(dateBuf));
      const uint16_t dateW =
          (uint16_t)(strlen(dateBuf) * CHARWIDTH * ST7701_CLK_DATE_SZ);
      setTextSize(ST7701_CLK_DATE_SZ);
      setTextColor(config.theme.date, config.theme.background);
      setCursor((int16_t)(left + clockW - dateW), (int16_t)(top + ST7701_CLK_SPACE));
      print(dateBuf);
#endif
    }
  }

  /* Секунды только на PLAYER — на SS только HH:MM + «:». */
  if (!ss) {
    setFont();
    setTextSize(ST7701_CLK_SUPER);
    setTextColor(config.theme.seconds, config.theme.background);
    char secBuf[4];
    snprintf(secBuf, sizeof(secBuf), "%02d", network.timeinfo.tm_sec);
    setCursor((int16_t)(s_clkLinesLeft + ST7701_CLK_SPACE + 1),
              (int16_t)(s_clkTop - ST7701_CLOCK_GLYPH_ABOVE));
    print(secBuf);
  }

  /* Мигание «:» */
  setFont(font);
  setTextSize(1);
  setTextColor(s_clkDots ? config.theme.clock
                         : (CLOCKFONT_MONO ? config.theme.clockbg : config.theme.background),
               config.theme.background);
  setCursor((int16_t)(s_clkLeft + s_clkDotsLeft), (int16_t)s_clkTop);
  print(":");
  s_clkDots = !s_clkDots;

  setFont();
  setTextSize(1);
}

#endif /* DSP_ST7701 */
