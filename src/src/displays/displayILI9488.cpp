#include "../core/options.h"
#if DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486
#include "dspcore.h"
#include "../core/config.h"
#include "../core/display.h"
#include <TJpg_Decoder.h>

#if DSP_HSPI
  DspCore::DspCore(): ILI9486_SPI(&SPI2, TFT_CS, TFT_DC, TFT_RST) {}
#else
  DspCore::DspCore(): ILI9486_SPI(TFT_CS, TFT_DC, TFT_RST) {}
#endif

// ==========================================================
// Cover art (JPEG from LittleFS) for ILI9488 portrait
// ==========================================================
#if defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT

static bool coverOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);

// Слот под обложку (под PLAYER).
// По координатам важно: слот должен быть ниже ClockWidget's framebuffer,
// чтобы при обновлении времени не затирался cover art.
static constexpr int16_t kCoverSlotW = 220;
static constexpr int16_t kCoverSlotH = 220;
static constexpr int16_t kCoverSlotX = (DSP_WIDTH - kCoverSlotW) / 2;
static constexpr int16_t kCoverSlotY = 194;

static constexpr int16_t kLogoW = LOGO_WIDTH;
static constexpr int16_t kLogoH = LOGO_HEIGHT;
static constexpr int16_t kLogoX = kCoverSlotX + (kCoverSlotW - kLogoW) / 2;
static constexpr int16_t kLogoY = kCoverSlotY + (kCoverSlotH - kLogoH) / 2;

static int16_t g_coverBoundLeft = 0;
static int16_t g_coverBoundTop = 0;
static int16_t g_coverBoundRight = 0;
static int16_t g_coverBoundBottom = 0;
static int16_t g_coverDrawX = 0;
static int16_t g_coverDrawY = 0;

static bool g_coverDecoderReady = false;
// Сброс из config.cpp (updateDisplayCover) после записи JPEG в LittleFS — см. NV3041A invalidateCoverCache.
static bool g_coverInvalidateNeeded = false;

void invalidateCoverCache() { g_coverInvalidateNeeded = true; }

static void ensureCoverDecoderReady() {
  if (g_coverDecoderReady) return;
  TJpgDec.setCallback(coverOutput);
  TJpgDec.setSwapBytes(false);
  g_coverDecoderReady = true;
}

static bool resolveCoverPath(const String& coverUrl, String &outPath) {
  if (coverUrl.length() == 0 || !littleFsReady) return false;
  if (!coverUrl.startsWith("/sc/")) return false;

  String sub = coverUrl.substring(4); // remove "/sc/"
  sub.replace("%20", " ");
  outPath = "/station_covers/" + sub;

  if (!lockLittleFS(200)) return false;
  bool exists = LittleFS.exists(outPath);
  unlockLittleFS();
  return exists;
}

static bool drawCoverAt(int16_t x, int16_t y, int16_t w, int16_t h, const String& coverUrl) {
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

  uint16_t jpgW = 0;
  uint16_t jpgH = 0;
  if (TJpgDec.getFsJpgSize(&jpgW, &jpgH, fsPath.c_str(), LittleFS) != JDR_OK) {
    unlockLittleFS();
    return false;
  }

  const uint16_t kMaxDecodeDim = 512;
  uint8_t scale = 0;
  while (scale < 3 && ((jpgW >> scale) > kMaxDecodeDim || (jpgH >> scale) > kMaxDecodeDim)) {
    scale++;
  }

  uint16_t scaledW = jpgW >> scale;
  uint16_t scaledH = jpgH >> scale;
  while (scale < 3 && (scaledW > w || scaledH > h)) {
    scale++;
    scaledW = jpgW >> scale;
    scaledH = jpgH >> scale;
  }

  g_coverBoundLeft = x;
  g_coverBoundTop = y;
  g_coverBoundRight = x + w;
  g_coverBoundBottom = y + h;

  TJpgDec.setJpgScale(1 << scale); // 1,2,4,8 (not log2)

  // Центрирование декодированного изображения внутри слота.
  g_coverDrawX = x + ((int16_t)w - (int16_t)scaledW) / 2;
  g_coverDrawY = y + ((int16_t)h - (int16_t)scaledH) / 2;

  JRESULT drawResult = TJpgDec.drawFsJpg(g_coverDrawX, g_coverDrawY, fsPath.c_str(), LittleFS);
  unlockLittleFS();
  return (drawResult == JDR_OK);
}

static bool coverOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  // Clip to cover slot to avoid painting outside even when decoder scaling misbehaves.
  int16_t x2 = x + (int16_t)w;
  int16_t y2 = y + (int16_t)h;
  int16_t drawL = max(x, g_coverBoundLeft);
  int16_t drawT = max(y, g_coverBoundTop);
  int16_t drawR = min(x2, g_coverBoundRight);
  int16_t drawB = min(y2, g_coverBoundBottom);
  if (drawR <= drawL || drawB <= drawT) return true;

  uint16_t clippedW = drawR - drawL;
  uint16_t clippedH = drawB - drawT;

  // Move bitmap pointer to clipped rectangle start.
  uint16_t *src = bitmap + (drawT - y) * w + (drawL - x);
  dsp.drawRGBBitmap(drawL, drawT, src, clippedW, clippedH);
  return true;
}

void ili9488_updateCoverSlot(bool forceRedraw) {
  if (config.isScreensaver) return;

  static String g_lastDrawnCoverUrl = "";
  static uint32_t g_lastCoverUpdateMs = 0;
  static uint8_t g_coverFailCount = 0;
  static constexpr uint8_t kMaxCoverRetries = 3;

  // Только из задачи дисплея (String), не из CoverDL — см. NV3041A.
  if (g_coverInvalidateNeeded) {
    g_coverInvalidateNeeded = false;
    g_lastDrawnCoverUrl = "";
    g_lastCoverUpdateMs = 0;
    g_coverFailCount = 0;
  }

  // If cover art is disabled - just show fallback logo.
  if (!isDisplayCoversEnabled()) {
    dsp.fillRect(kCoverSlotX, kCoverSlotY, kCoverSlotW, kCoverSlotH, config.theme.background);
    dsp.drawRGBBitmap(kLogoX, kLogoY, logo, kLogoW, kLogoH);
    return;
  }

  uint32_t now = millis();
  String target = String(currentCoverUrl.c_str());
  bool urlChanged = (target != g_lastDrawnCoverUrl);

  bool retryDue =
      (!urlChanged && !forceRedraw &&
       g_coverFailCount > 0 &&
       (now - g_lastCoverUpdateMs) >= DISPLAY_COVER_REFRESH_MS &&
       g_coverFailCount < kMaxCoverRetries);

  bool needsDraw = urlChanged || forceRedraw || retryDue;
  if (!needsDraw) return;

  if (urlChanged) g_coverFailCount = 0;

  dsp.fillRect(kCoverSlotX, kCoverSlotY, kCoverSlotW, kCoverSlotH, config.theme.background);

  bool ok = drawCoverAt(kCoverSlotX, kCoverSlotY, kCoverSlotW, kCoverSlotH, target);
  if (!ok) {
    // Fallback: draw built-in logo in slot
    dsp.drawRGBBitmap(kLogoX, kLogoY, logo, kLogoW, kLogoH);
    if (urlChanged || retryDue) g_coverFailCount++;
  } else {
    g_coverFailCount = 0;
  }

  g_lastDrawnCoverUrl = target;
  g_lastCoverUpdateMs = now;
}
#else
// Non-portrait: keep function stub to satisfy widgets.cpp.
void ili9488_updateCoverSlot(bool /*forceRedraw*/) {}
void invalidateCoverCache() {}
#endif

void DspCore::initDisplay() {
  setSpiKludge(false);
  init();
  cp437(true);
  setTextWrap(false);
  setTextSize(1);
  fillScreen(0x0000);
  invert();
  flip();
}

void DspCore::clearDsp(bool black){
  fillScreen(black ? 0 : config.theme.background);
  // Отображаем логотип на странице PLAYER.
  // Для NV3041A логотип/обложка управляется отдельной логикой внутри printClock(),
  // а для ILI9488 логотип сейчас иначе не рисуется в PLAYER.
  if(!black && !config.isScreensaver && display.mode()==PLAYER){
    drawLogo(bootLogoTop);
  }
}
void DspCore::flip(){
  // ILI9486_SPI базово инициализируется как 320x480.
  // Для "альбомной" ориентации используем rotation 1/3 (как было),
  // для "портретной" — rotation 0/2.
#if defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT
  setRotation(config.store.flipscreen ? 2 : 0);
#else
  setRotation(config.store.flipscreen ? 3 : 1);
#endif
}
void DspCore::invert(){ invertDisplay(config.store.invertdisplay); }
void DspCore::sleep(void){ sendCommand(ILI9488_SLPIN); delay(150); sendCommand(ILI9488_DISPOFF); delay(150); }
void DspCore::wake(void){ sendCommand(ILI9488_DISPON); delay(150); sendCommand(ILI9488_SLPOUT); delay(150); }

#endif
