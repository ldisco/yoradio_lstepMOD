#include "../core/options.h"
#if DSP_MODEL==DSP_AXS15231B

#include "displayAXS15231B.h"
#include "../core/config.h"
#include "../core/network.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/*
 * Тонкий init под JC3248W535.
 * init-последовательность: type2 (часто Guition 320×480); при чёрном/мусорном
 * экране можно переключить на type1 через AXS15231B_INIT_TYPE 1 в myoptions.
 */
#ifndef AXS15231B_INIT_TYPE
#define AXS15231B_INIT_TYPE 2
#endif

/* Полный кадр по QSPI ~300 KB. Present — после pager (паттерн Witaliy), не «глушим» loop. */
#ifndef AXS15231B_FLUSH_MIN_MS
#define AXS15231B_FLUSH_MIN_MS 120u
#endif
/*
 * Диагностика banding: FREEZE стопает present. type1 снял полосы — FREEZE выкл (0).
 * Вернуть >0 только для A/B «полосы без QSPI».
 */
#ifndef AXS15231B_FREEZE_AFTER_MS
#define AXS15231B_FREEZE_AFTER_MS 0u
#endif

static Arduino_DataBus *s_axsBus =
  new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_D0, TFT_D1, TFT_D2, TFT_D3);

#if AXS15231B_INIT_TYPE == 1
static Arduino_AXS15231B *s_axsPanel = new Arduino_AXS15231B(
  s_axsBus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, false /* IPS */,
  320, 480, 0, 0, 0, 0,
  axs15231b_320480_type1_init_operations,
  sizeof(axs15231b_320480_type1_init_operations));
#else
static Arduino_AXS15231B *s_axsPanel = new Arduino_AXS15231B(
  s_axsBus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, false /* IPS */,
  320, 480, 0, 0, 0, 0,
  axs15231b_320480_type2_init_operations,
  sizeof(axs15231b_320480_type2_init_operations));
#endif

static SemaphoreHandle_t s_axsQspiMtx = nullptr;
static volatile bool s_axsFlushBusy = false;
/* true = в framebuffer есть изменения, нужен flush на панель. */
static volatile bool s_axsFbDirty = true;
/* После FREEZE_AFTER_MS — полный стоп QSPI present (диагностика). */
static uint32_t s_axsFirstFlushMs = 0;
static volatile bool s_axsFlushFrozen = false;

/* Диагностика flush в Serial: 1 = [AXS-FLUSH] раз в ~2 с. По умолчанию выкл — не захламлять монитор. */
#ifndef AXS15231B_FLUSH_STATS
#define AXS15231B_FLUSH_STATS 0
#endif

#if AXS15231B_FLUSH_STATS
static uint32_t s_flushStatCount = 0;
static uint32_t s_flushStatUsSum = 0;
static uint32_t s_flushStatUsMin = UINT32_MAX;
static uint32_t s_flushStatUsMax = 0;
static uint32_t s_flushStatWindowMs = 0;

static void axsFlushStatNote(uint32_t elapsedUs) {
  s_flushStatCount++;
  s_flushStatUsSum += elapsedUs;
  if (elapsedUs < s_flushStatUsMin) s_flushStatUsMin = elapsedUs;
  if (elapsedUs > s_flushStatUsMax) s_flushStatUsMax = elapsedUs;

  const uint32_t now = millis();
  if (s_flushStatWindowMs == 0) s_flushStatWindowMs = now;
  if ((now - s_flushStatWindowMs) < 2000u) return;

  const uint32_t windowMs = now - s_flushStatWindowMs;
  const uint32_t n = s_flushStatCount;
  const uint32_t avg = n ? (s_flushStatUsSum / n) : 0;
  const uint32_t perSec = windowMs ? (n * 1000u / windowMs) : 0;
  Serial.printf("[AXS-FLUSH] %u/s n=%u us min=%u avg=%u max=%u dirty=%u (throttle=%ums)\n",
                (unsigned)perSec, (unsigned)n,
                (unsigned)(s_flushStatUsMin == UINT32_MAX ? 0 : s_flushStatUsMin),
                (unsigned)avg, (unsigned)s_flushStatUsMax,
                (unsigned)(s_axsFbDirty ? 1 : 0),
                (unsigned)AXS15231B_FLUSH_MIN_MS);

  s_flushStatCount = 0;
  s_flushStatUsSum = 0;
  s_flushStatUsMin = UINT32_MAX;
  s_flushStatUsMax = 0;
  s_flushStatWindowMs = now;
}
#endif

static void axsEnsureQspiMutex() {
  if (s_axsQspiMtx) return;
  s_axsQspiMtx = xSemaphoreCreateMutex();
}

bool axsFbTryLockMs(uint32_t waitMs) {
  axsEnsureQspiMutex();
  if (!s_axsQspiMtx) return true;
  return xSemaphoreTake(s_axsQspiMtx, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void axsFbUnlock() {
  if (s_axsQspiMtx) xSemaphoreGive(s_axsQspiMtx);
}

void axsMarkFbDirty() {
  s_axsFbDirty = true;
}

/*
 * Единая точка вывода кадра на панель. Таймаут короткий → пропуск кадра, не блок DspTask.
 * waitTicks=portMAX_DELAY только для init (нужен первый кадр).
 * После успешного flush снимаем dirty.
 */
static bool axsFlushLocked(Arduino_Canvas *canvas, TickType_t waitTicks) {
  if (!canvas) return false;
  if (s_axsFlushFrozen) return false;
  axsEnsureQspiMutex();
  if (!s_axsQspiMtx) {
#if AXS15231B_FLUSH_STATS
    const uint32_t t0 = micros();
#endif
    canvas->flush();
    s_axsFbDirty = false;
#if AXS15231B_FLUSH_STATS
    axsFlushStatNote(micros() - t0);
#endif
    return true;
  }
  if (xSemaphoreTake(s_axsQspiMtx, waitTicks) != pdTRUE) return false;
  s_axsFlushBusy = true;
#if AXS15231B_FLUSH_STATS
  const uint32_t t0 = micros();
#endif
  canvas->flush();
#if AXS15231B_FLUSH_STATS
  const uint32_t dt = micros() - t0;
#endif
  s_axsFbDirty = false;
  s_axsFlushBusy = false;
  xSemaphoreGive(s_axsQspiMtx);
#if AXS15231B_FLUSH_STATS
  axsFlushStatNote(dt);
#endif
  if (s_axsFirstFlushMs == 0) s_axsFirstFlushMs = millis();
  return true;
}

/*
 * Подсветка GPIO1 как у Witaliy76/JC3248W535C: свой LEDC 5 kHz / 8 bit.
 * BRIGHTNESS_PIN=255 — yoRadio/analogWrite этот пин не трогает (тест B мерцания).
 * Core 3.x: ledcAttach(pin, freq, res) + ledcWrite(pin, duty).
 *
 * Cold-boot hold: config._initHW / BacklightDown.on_setup зовут setBrightness
 * ДО display.init и ДО drawLogo. Если сразу зажечь BL — на экране GRAM-мусор
 * («квадратики»), потом ещё задержка begin/fill, потом снова мусор, потом лого.
 * Пока s_axsBlBootHold — игнорируем duty>0; свет только после первого flush лого.
 */
static bool s_axsBlAttached = false;
static bool s_axsBlBootHold = true;

static void axsBacklightInit() {
  if (s_axsBlAttached) return;
#ifndef GFX_BL
#error "GFX_BL required for AXS backlight"
#endif
  if (!ledcAttach((uint8_t)GFX_BL, 5000.0, 8)) {
    Serial.println("[AXS] ledcAttach(GFX_BL, 5000, 8) FAILED");
    return;
  }
  s_axsBlAttached = true;
  Serial.println("[AXS] backlight LEDC 5kHz/8bit on GFX_BL");
}

static void axsBacklightLevel(uint8_t duty255) {
  /* Hold: не цепляем LEDC и не поднимаем свет до первого кадра boot-лого. */
  if (s_axsBlBootHold && duty255 != 0) return;
  axsBacklightInit();
  if (!s_axsBlAttached) return;
  ledcWrite((uint8_t)GFX_BL, duty255);
}

/* Публичная обёртка: WebUI / setBrightness / BacklightDown → LEDC, не analogWrite. */
void axsSetBacklightDuty(uint8_t duty255) {
  axsBacklightLevel(duty255);
}

static void axsBacklightFromConfig() {
  axsBacklightLevel((uint8_t)map(config.store.brightness, 0, 100, 0, 255));
}

/* Снять hold и зажечь store.brightness (после первого flush лого). */
static void axsBacklightReleaseBoot() {
  if (!s_axsBlBootHold) return;
  s_axsBlBootHold = false;
  axsBacklightFromConfig();
  Serial.println("[AXS] backlight ON after first logo frame");
}

/*
 * Canvas = native 320×480 (как панель). Landscape — soft-rotation Canvas (1/3):
 * у AXS15231B MADCTL landscape часто не работает; flush всегда шлёт WIDTH×HEIGHT.
 * A/B portrait rot=0 дрыганье не снял — оставлен landscape.
 */
DspCore::DspCore() : Arduino_Canvas(320, 480, s_axsPanel, 0, 0, 1) {}

void DspCore::writePixelPreclipped(int16_t x, int16_t y, uint16_t color) {
  Arduino_Canvas::writePixelPreclipped(x, y, color);
  s_axsFbDirty = true;
}

void DspCore::writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  Arduino_Canvas::writeFillRectPreclipped(x, y, w, h, color);
  s_axsFbDirty = true;
}

void DspCore::initDisplay() {
  Serial.println("##[BOOT]#\tAXS15231B initDisplay begin");
  Serial.flush();

  axsEnsureQspiMutex();
  /*
   * Cold-boot: BL гасим GPIO LOW и НЕ поднимаем до первого flush лого (drawLogo).
   * Раньше свет включали сразу после fill — полевой cold-boot: темнота → квадратики → лого.
   * До begin() только GPIO (без LEDC): иначе begin() может сорвать канал LEDC,
   * а s_axsBlAttached=true мешает переподключить → чёрный экран.
   */
#ifndef GFX_BL
#error "GFX_BL required for AXS backlight"
#endif
  pinMode((uint8_t)GFX_BL, OUTPUT);
  digitalWrite((uint8_t)GFX_BL, LOW);
  s_axsBlAttached = false;
  s_axsBlBootHold = true;

  /*
   * QSPI Hz: 80e6 дал точки-артефакты, дрыганье не снял. Держим 40e6 (чуть быстрее дефолта).
   * Гипотеза «клок = корень» ослаблена — дальше FREEZE_AFTER_MS.
   */
  const int32_t axsQspiHz = 40000000;
  Serial.printf("[AXS] Canvas/panel begin(%ld Hz)\n", (long)axsQspiHz);
  begin(axsQspiHz);
  setTextWrap(false);
  setTextSize(1);
  flip();
  invert();
  fillScreen(config.theme.background);
  axsMarkFbDirty();
  axsFlushLocked(this, portMAX_DELAY);
  /* Свет не здесь — axsBacklightReleaseBoot() после первого кадра drawLogo. */

  Serial.println("##[BOOT]#\tAXS15231B initDisplay done (BL hold until logo)");
  Serial.flush();
}

void DspCore::clearDsp(bool black) {
  fillScreen(black ? 0 : config.theme.background);
  axsMarkFbDirty();
}

void DspCore::flip() {
  /* Landscape как JC2432/ST7789: 1 / 3 (flipscreen = 180° в той же ориентации). */
  setRotation(config.store.flipscreen ? 3 : 1);
}

void DspCore::invert() {
  invertDisplay(config.store.invertdisplay);
}

void DspCore::sleep(void) {
  /* Гасим LEDC; displayOff панели — под тем же QSPI-мьютексом, что и flush. */
  axsBacklightLevel(0);
  axsEnsureQspiMutex();
  if (s_axsQspiMtx && xSemaphoreTake(s_axsQspiMtx, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (s_axsPanel) s_axsPanel->displayOff();
    xSemaphoreGive(s_axsQspiMtx);
  }
}

void DspCore::wake(void) {
  axsEnsureQspiMutex();
  if (s_axsQspiMtx && xSemaphoreTake(s_axsQspiMtx, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (s_axsPanel) s_axsPanel->displayOn();
    xSemaphoreGive(s_axsQspiMtx);
  }
  axsBacklightFromConfig();
  axsFlushLocked(this, pdMS_TO_TICKS(50));
}

void DspCore::loop(bool force) {
  /* Present вынесен в Display::loop сразу после pager (axsPresentFrame). */
  (void)force;
}

/*
 * Boot-логотип Faster: PROGMEM 198×128 (Catmull-Rom offline из штатного 99×64).
 * Финальный кадр — 1:1 без runtime-scale; pulse — лёгкий bilinear от hi-res
 * (не от 99×64 → без «квадратиков»). Перегенерация: node tools/gen_bootlogo_198.js
 */
static inline uint8_t axsRgb565R(uint16_t c) { return (uint8_t)((c >> 11) & 0x1F); }
static inline uint8_t axsRgb565G(uint16_t c) { return (uint8_t)((c >> 5) & 0x3F); }
static inline uint8_t axsRgb565B(uint16_t c) { return (uint8_t)(c & 0x1F); }

static inline uint16_t axsRgb565Pack(uint16_t r, uint16_t g, uint16_t b) {
  return (uint16_t)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F));
}

static uint16_t axsSampleBootLogoBilinear(uint32_t x256, uint32_t y256) {
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

  const uint16_t r0 = (uint16_t)((axsRgb565R(c00) * ifx + axsRgb565R(c10) * fx) >> 8);
  const uint16_t g0 = (uint16_t)((axsRgb565G(c00) * ifx + axsRgb565G(c10) * fx) >> 8);
  const uint16_t b0 = (uint16_t)((axsRgb565B(c00) * ifx + axsRgb565B(c10) * fx) >> 8);
  const uint16_t r1 = (uint16_t)((axsRgb565R(c01) * ifx + axsRgb565R(c11) * fx) >> 8);
  const uint16_t g1 = (uint16_t)((axsRgb565G(c01) * ifx + axsRgb565G(c11) * fx) >> 8);
  const uint16_t b1 = (uint16_t)((axsRgb565B(c01) * ifx + axsRgb565B(c11) * fx) >> 8);

  return axsRgb565Pack((uint16_t)((r0 * ify + r1 * fy) >> 8),
                       (uint16_t)((g0 * ify + g1 * fy) >> 8),
                       (uint16_t)((b0 * ify + b1 * fy) >> 8));
}

static void axsBlitBootLogoScaled(DspCore *gfx, int16_t left, int16_t top,
                                  uint16_t dstW, uint16_t dstH) {
  if (!gfx || !dstW || !dstH) return;

  /* Точный базовый размер — 1:1 из PROGMEM, без повторного resample. */
  if (dstW == AXS_BOOT_LOGO_W && dstH == AXS_BOOT_LOGO_H) {
    gfx->draw16bitRGBBitmap(left, top,
                            const_cast<uint16_t*>(axs_boot_logo),
                            AXS_BOOT_LOGO_W, AXS_BOOT_LOGO_H);
    return;
  }

  uint16_t line[230];
  if (dstW > (uint16_t)(sizeof(line) / sizeof(line[0]))) {
    dstW = (uint16_t)(sizeof(line) / sizeof(line[0]));
  }

  const uint32_t xDen = (dstW > 1u) ? (uint32_t)(dstW - 1u) : 1u;
  const uint32_t yDen = (dstH > 1u) ? (uint32_t)(dstH - 1u) : 1u;

  for (uint16_t y = 0; y < dstH; ++y) {
    const uint32_t y256 = ((uint32_t)y * (AXS_BOOT_LOGO_H - 1u) * 256u) / yDen;
    for (uint16_t x = 0; x < dstW; ++x) {
      const uint32_t x256 = ((uint32_t)x * (AXS_BOOT_LOGO_W - 1u) * 256u) / xDen;
      line[x] = axsSampleBootLogoBilinear(x256, y256);
    }
    gfx->draw16bitRGBBitmap(left, (int16_t)(top + (int16_t)y), line, dstW, 1);
  }
}

void DspCore::drawLogo(uint16_t top) {
  /*
   * Scale-pulse (вариант A «поживее»): старт ~74%, пик ~112.5%, финал 100%.
   * Q8: 256 = 1.0× от PROGMEM 198×128. Кадр ~40 мс → вся анимация ~0.5 с.
   */
  static const uint16_t kPulseQ8[] = {
    190, 210, 230, 248, 256, 272, 288, 280, 268, 260, 256
  };
  constexpr uint16_t kMaxQ8 = 288;
  const uint16_t maxW = (uint16_t)(((uint32_t)AXS_BOOT_LOGO_W * kMaxQ8) / 256u);
  const uint16_t maxH = (uint16_t)(((uint32_t)AXS_BOOT_LOGO_H * kMaxQ8) / 256u);
  const int16_t areaL = (int16_t)(((int16_t)width() - (int16_t)maxW) / 2);
  const int16_t areaT = (int16_t)top;
  const uint16_t bg = config.theme.background;

  for (uint8_t i = 0; i < (uint8_t)(sizeof(kPulseQ8) / sizeof(kPulseQ8[0])); ++i) {
    const uint16_t q = kPulseQ8[i];
    const uint16_t dw = (uint16_t)(((uint32_t)AXS_BOOT_LOGO_W * q) / 256u);
    const uint16_t dh = (uint16_t)(((uint32_t)AXS_BOOT_LOGO_H * q) / 256u);
    const int16_t lx = (int16_t)(areaL + (int16_t)((maxW - dw) / 2));
    const int16_t ly = (int16_t)(areaT + (int16_t)((maxH - dh) / 2));

    fillRect(areaL, areaT, (int16_t)maxW, (int16_t)maxH, bg);
    axsBlitBootLogoScaled(this, lx, ly, dw, dh);
    axsMarkFbDirty();
    axsFlushLocked(this, pdMS_TO_TICKS(80));
    /* Первый кадр pulse уже на панели — можно зажечь BL (без GRAM-мусора). */
    if (i == 0) axsBacklightReleaseBoot();
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

void axsPresentFrame() {
  /*
   * Отрисовали виджеты → один flush. FREEZE_AFTER_MS: стоп present для A/B «софт vs HW».
   */
  if (s_axsFlushFrozen) return;
#if AXS15231B_FREEZE_AFTER_MS > 0
  if (s_axsFirstFlushMs != 0 &&
      (millis() - s_axsFirstFlushMs) >= (uint32_t)AXS15231B_FREEZE_AFTER_MS) {
    s_axsFlushFrozen = true;
    Serial.println("[AXS] FREEZE: no more flush. If still jitters => HW/BL/panel; if stable => refresh path");
    Serial.flush();
    return;
  }
#endif
  static uint32_t s_lastFlushMs = 0;
  const uint32_t now = millis();
  if (!s_axsFbDirty) return;
  if ((now - s_lastFlushMs) < AXS15231B_FLUSH_MIN_MS) return;
  if (s_axsFlushBusy) return;
  if (!axsFlushLocked(&dsp, 0)) return;
  s_lastFlushMs = now;
}

/* ---- Clock / Num: PLAYER=DS_DIGI56, SS=DS_DIGI68 + fullclock (дата/день/сек) ---- */

/* Глиф «8» DIGI56: height 70, yOffset −69. */
static const uint16_t AXS_CLOCK_GLYPH_ABOVE = 70;
/* DIGI68 (scale 68/56): height 85, yOffset −84. */
static const uint16_t AXS_CLOCK_SS_GLYPH_ABOVE = 85;
static const uint8_t AXS_CLK_SUPER = 4;   /* TIME_SIZE/17 — секунды / день */
static const uint8_t AXS_CLK_SPACE = 10;  /* (5*super)/2 */
static const uint8_t AXS_CLK_DATE_SZ = 1; /* дата мельче, чтобы не вылезала за край */
/* Высота блока PLAYER: глифы + отступ + строка даты. */
static const uint16_t AXS_CLOCK_BLOCK_H =
    (uint16_t)(AXS_CLOCK_GLYPH_ABOVE + AXS_CLK_SPACE + CHARHEIGHT * AXS_CLK_DATE_SZ + 2);
/* На SS только HH:MM — без даты/секунд. */
static const uint16_t AXS_CLOCK_SS_H = (uint16_t)(AXS_CLOCK_SS_GLYPH_ABOVE + 4);

static uint16_t s_axsClockLeft = 0;
static uint16_t s_axsClockWidth = 0;
static uint16_t s_axsClockTop = 0;
static uint16_t s_axsTimeWidth = 0;
static uint16_t s_axsDotsLeft = 0;
static uint16_t s_axsLinesLeft = 0;
static char s_axsOldTime[8] = "";
static bool s_axsDots = true;
static bool s_axsWasScreensaver = false;

static const GFXfont *axsClockFont() {
  return config.isScreensaver ? &DS_DIGI68pt7b : &Clock_GFXfont;
}

static uint16_t axsGlyphAbove() {
  return config.isScreensaver ? AXS_CLOCK_SS_GLYPH_ABOVE : AXS_CLOCK_GLYPH_ABOVE;
}

static uint8_t axsClockCharAdvance(char c, const GFXfont *font) {
  GFXglyph *glyph = pgm_read_glyph_ptr(font, (uint8_t)c - 0x20);
  return pgm_read_byte(&glyph->xAdvance);
}

static uint16_t axsClockTextWidth(const char *txt, const GFXfont *font) {
  if (!txt) return 0;
  uint16_t w = 0;
  for (const char *p = txt; *p; ++p) w = (uint16_t)(w + axsClockCharAdvance(*p, font));
  return w;
}

static uint16_t axsClockClearH() {
  return config.isScreensaver ? AXS_CLOCK_SS_H : AXS_CLOCK_BLOCK_H;
}

void DspCore::charSize(uint8_t textsize, uint8_t& width, uint16_t& height) {
  width = textsize * CHARWIDTH;
  height = textsize * CHARHEIGHT;
}

uint16_t DspCore::textWidth(const char *txt) {
  return axsClockTextWidth(txt, axsClockFont());
}

void DspCore::setNumFont() {
  setFont(&Clock_GFXfont);
  setTextSize(1);
}

void DspCore::clearClock() {
  if (s_axsClockWidth == 0) return;
  const uint16_t above = axsGlyphAbove();
  const uint16_t clrTop =
      (s_axsClockTop > above) ? (uint16_t)(s_axsClockTop - above) : 0;
  fillRect((int16_t)s_axsClockLeft, (int16_t)clrTop,
           (int16_t)s_axsClockWidth + 2, (int16_t)axsClockClearH(), config.theme.background);
  axsMarkFbDirty();
}

void DspCore::printClock(uint16_t top, uint16_t rightspace, uint16_t timeheight, bool redraw) {
  (void)timeheight;
  const bool ss = config.isScreensaver;
  const GFXfont *font = axsClockFont();
  const uint16_t glyphAbove = axsGlyphAbove();
  /* Смена PLAYER↔SS — полная перерисовка (иначе остаются секунды/дата / чужой шрифт). */
  if (ss != s_axsWasScreensaver) {
    redraw = true;
    if (s_axsClockWidth > 0) {
      /* Стираем с запасом под больший из двух шрифтов. */
      const uint16_t aboveMax =
          (AXS_CLOCK_SS_GLYPH_ABOVE > AXS_CLOCK_GLYPH_ABOVE) ? AXS_CLOCK_SS_GLYPH_ABOVE
                                                            : AXS_CLOCK_GLYPH_ABOVE;
      const uint16_t hMax =
          (AXS_CLOCK_SS_H > AXS_CLOCK_BLOCK_H) ? AXS_CLOCK_SS_H : AXS_CLOCK_BLOCK_H;
      const uint16_t clrTop =
          (s_axsClockTop > aboveMax) ? (uint16_t)(s_axsClockTop - aboveMax) : 0;
      fillRect((int16_t)s_axsClockLeft, (int16_t)clrTop,
               (int16_t)s_axsClockWidth + 2, (int16_t)hMax, config.theme.background);
      axsMarkFbDirty();
    }
    s_axsClockWidth = 0;
    s_axsWasScreensaver = ss;
  }

  char timeBuf[8];
  strftime(timeBuf, sizeof(timeBuf), "%H:%M", &network.timeinfo);
  const bool timeChanged = (strcmp(s_axsOldTime, timeBuf) != 0);
  const bool fullRedraw = redraw || timeChanged || (s_axsClockWidth == 0);

  const uint16_t timeW = axsClockTextWidth(timeBuf, font);
  /* На SS колонка секунд/дня не нужна — только HH:MM. */
  const uint16_t rightside = ss ? 0
      : (uint16_t)(CHARWIDTH * AXS_CLK_SUPER * 2 + AXS_CLK_SPACE * 2 + 1);
  const uint16_t clockW = (uint16_t)(timeW + rightside);

  /*
   * PLAYER: WA_RIGHT — rightspace = отступ справа (clockConf.left).
   * SS: moveTo задаёт абсолютный X в _config.left → рисуем от него (прыжок по всему экрану).
   */
  int16_t left;
  if (ss) {
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
  const uint16_t dotsLeft = axsClockTextWidth(hourBuf, font);
  const uint16_t linesLeft = (uint16_t)((uint16_t)left + timeW + AXS_CLK_SPACE);

  if (fullRedraw) {
    if (s_axsClockWidth > 0) clearClock();
    else {
      const uint16_t clrTop =
          (top > glyphAbove) ? (uint16_t)(top - glyphAbove) : 0;
      fillRect(left, (int16_t)clrTop, (int16_t)clockW + 2, (int16_t)axsClockClearH(),
               config.theme.background);
    }

    s_axsClockLeft = (uint16_t)left;
    s_axsClockWidth = clockW;
    s_axsClockTop = top;
    s_axsTimeWidth = timeW;
    s_axsDotsLeft = dotsLeft;
    s_axsLinesLeft = linesLeft;
    strlcpy(s_axsOldTime, timeBuf, sizeof(s_axsOldTime));

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
      drawFastVLine((int16_t)linesLeft, (int16_t)(top - AXS_CLOCK_GLYPH_ABOVE),
                    (int16_t)AXS_CLOCK_GLYPH_ABOVE, config.theme.div);
      drawFastHLine((int16_t)linesLeft, (int16_t)(top - AXS_CLOCK_GLYPH_ABOVE / 2),
                    (int16_t)(CHARWIDTH * AXS_CLK_SUPER * 2 + AXS_CLK_SPACE), config.theme.div);

      /* День — две буквы после utf8Rus (пнд→пн), иначе вылезает за край. */
      setFont();
      setTextSize(AXS_CLK_SUPER);
      setTextColor(config.theme.dow, config.theme.background);
      setCursor((int16_t)(linesLeft + AXS_CLK_SPACE + 1),
                (int16_t)(top - CHARHEIGHT * AXS_CLK_SUPER));
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
          (uint16_t)(strlen(dateBuf) * CHARWIDTH * AXS_CLK_DATE_SZ);
      setTextSize(AXS_CLK_DATE_SZ);
      setTextColor(config.theme.date, config.theme.background);
      setCursor((int16_t)(left + clockW - dateW), (int16_t)(top + AXS_CLK_SPACE));
      print(dateBuf);
#endif
    }
  }

  /* Секунды только на PLAYER — на SS только HH:MM + «:». */
  if (!ss) {
    setFont();
    setTextSize(AXS_CLK_SUPER);
    setTextColor(config.theme.seconds, config.theme.background);
    char secBuf[4];
    snprintf(secBuf, sizeof(secBuf), "%02d", network.timeinfo.tm_sec);
    setCursor((int16_t)(s_axsLinesLeft + AXS_CLK_SPACE + 1),
              (int16_t)(s_axsClockTop - AXS_CLOCK_GLYPH_ABOVE));
    print(secBuf);
  }

  /* Мигание «:» */
  setFont(font);
  setTextSize(1);
  setTextColor(s_axsDots ? config.theme.clock
                         : (CLOCKFONT_MONO ? config.theme.clockbg : config.theme.background),
               config.theme.background);
  setCursor((int16_t)(s_axsClockLeft + s_axsDotsLeft), (int16_t)s_axsClockTop);
  print(":");
  s_axsDots = !s_axsDots;

  setFont();
  setTextSize(1);
  axsMarkFbDirty();
}

/* Cover-SS + (при FFT) пирамидка PLAYER — как ILI9488 landscape. */
#include "../core/network.h"
#include "../core/player.h"
#include "../core/display.h"
#include "../core/timekeeper.h"
#include "tools/utf8Rus.h"
#include "widgets/widgets.h"
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include "../plugins/TrackFacts/TrackFacts.h"
extern TrackFacts trackFactsPlugin;
/* Cover-SS: DS_DIGI42 mono (ровная «1»); Clock_GFXfont плеера = DIGI56 — не подменять.
 * Макрос переименовывает GFXfont из mono-header в ST170_SideClockFont. */
#define Clock_GFXfont ST170_SideClockFont
#include "fonts/DS_DIGI42pt7b_mono.h"
#undef Clock_GFXfont
#define ST240_SmallClockFont ST170_SideClockFont
/* Cover-SS лого 99×64 (boot Faster — 198×128 отдельно). */
#include "fonts/bootlogo99x64.h"
#define bootlogo2 logo

#define ST7789_COV_NS axs_cover
#include "st7789_cover_screensaver.inc"
#undef ST7789_COV_NS
#undef bootlogo2

#if SPECTRUM_ANALYZER_ACTIVE
#include "st7789_player_weather.inc"
#endif

void invalidateCoverCache() {
  axs_cover::requestCoverInvalidate();
  if (config.isScreensaver && display.mode() == SCREENBLANK
      && player.isRunning() && isDisplayCoversEnabled()) {
    display.putRequest(CLOCK, 1);
  }
}

#endif /* DSP_AXS15231B */
