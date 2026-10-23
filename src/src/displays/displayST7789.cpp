#include "../core/options.h"
#if DSP_MODEL==DSP_ST7789 || DSP_MODEL==DSP_ST7789_240 || DSP_MODEL==DSP_ST7789_76
#include "dspcore.h"
#include "../core/config.h"
#include "driver/gpio.h"
#include "esp32-hal-periman.h"

#if DSP_HSPI
DspCore::DspCore(): Adafruit_ST7789(&SPI2, TFT_CS, TFT_DC, TFT_RST) {}
#else
DspCore::DspCore(): Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST) {}
#endif

/* Принудительно OUTPUT + регистрация в Arduino periman.
 * Нужно для GPIO47/48: на variant esp32s3 пин 48 = PIN_RGB_LED (RMT),
 * без этого digitalWrite(TFT_DC) → «IO 48 is not set as GPIO» → TWDT. */
static void st7789ForceGpioOut(int pin, const char *tag) {
  if (pin < 0) return;
  gpio_hold_dis((gpio_num_t)pin);
  gpio_reset_pin((gpio_num_t)pin);
  /* Снять предыдущую шину (RMT RGB / LEDC / …) из peripheral manager.
   * Не трогаем perimanSetBusDeinit(GPIO) — pinMode сам ставит gpioDetachBus. */
  perimanClearPinBus(pin);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  const bool ok = (perimanGetPinBus(pin, ESP32_BUS_TYPE_GPIO) != NULL);
  Serial.printf("##[BOOT]#\tST7789 %s pin %d GPIO=%s\n", tag, pin, ok ? "OK" : "FAIL");
  Serial.flush();
}

void DspCore::initDisplay() {
  Serial.println("##[BOOT]#\tST7789 initDisplay begin");
  Serial.flush();
  /* TFT_SPIPINS: SCK, MISO, MOSI. GPIO19/20 — USB-PHY S3, сброс перед SPI. */
#if defined(TFT_SPIPINS)
  {
    const int tftSpiPins[] = { TFT_SPIPINS }; /* SCK, MISO, MOSI */
    for (size_t i = 0; i < sizeof(tftSpiPins) / sizeof(tftSpiPins[0]); ++i) {
      const int p = tftSpiPins[i];
      if (p == 19 || p == 20) {
        gpio_reset_pin((gpio_num_t)p);
        gpio_hold_dis((gpio_num_t)p);
      }
    }
  }
  Serial.println("##[BOOT]#\tST7789 SPI.begin (custom pins)");
  Serial.flush();
  #if DSP_HSPI
    SPI2.begin(TFT_SPIPINS);
  #else
    SPI.begin(TFT_SPIPINS);
  #endif
#endif
  st7789ForceGpioOut(TFT_CS, "CS");
  st7789ForceGpioOut(TFT_DC, "DC");
  Serial.println("##[BOOT]#\tST7789 panel init()");
  Serial.flush();
  if(DSP_MODEL==DSP_ST7789_76){
    init(76,284);
  }else{
    init(240,(DSP_MODEL==DSP_ST7789)?320:240);
  }
  /* init() Adafruit может снова трогать пины — закрепляем CS/DC ещё раз */
  st7789ForceGpioOut(TFT_CS, "CS@post");
  st7789ForceGpioOut(TFT_DC, "DC@post");
  invert();
  cp437(true);
  flip();
  setTextWrap(false);
  setTextSize(1);
  Serial.println("##[BOOT]#\tST7789 fillScreen");
  Serial.flush();
  fillScreen(0x0000);
  Serial.println("##[BOOT]#\tST7789 initDisplay done");
  Serial.flush();
}

#if DSP_MODEL==DSP_ST7789
/* Cover-скринсейвер playing+blank для JC2432 320×240 (как ST7789_170). */
#include "../core/config.h"
#include "../core/network.h"
#include "../core/display.h"
#include "../core/player.h"
#include "../core/timekeeper.h"
#include "tools/utf8Rus.h"
#include "widgets/widgets.h"
#include <LittleFS.h>
#include <TJpg_Decoder.h>
/* Ручной запрос факта со свайпа по обложке cover-SS — тот же API, что WebUI trackfactsrequest. */
#include "../plugins/TrackFacts/TrackFacts.h"
extern TrackFacts trackFactsPlugin;
/* Cover-часы DS_DIGI28 (крупнее DS_DIGI15); при обложке 200×200 колонка 120 px — строка у правого края TFT. */
#define ST170_SKIP_DIGI28_CLOCK_GFXFONT
#include "fonts/DS_DIGI28pt7b_mono.h"
#define ST240_SmallClockFont ST170_SmallClockFont
/* В displayST7789.h уже bootlogo99x64.h (массив logo); .inc ждёт bootlogo2 — алиас, без второго include. */
#define bootlogo2 logo

#define ST7789_COV_NS st7789_cover
#include "st7789_cover_screensaver.inc"
#undef ST7789_COV_NS
#undef bootlogo2

#if ST7789_SPECTRUM_ENABLE
#include "st7789_player_weather.inc"
#endif

void invalidateCoverCache() {
  st7789_cover::requestCoverInvalidate();
  /* JPEG дописан в LittleFS — сразу запросить перерисовку cover-скринсейвера. */
  if (config.isScreensaver && display.mode() == SCREENBLANK
      && player.isRunning() && isDisplayCoversEnabled()) {
    display.putRequest(CLOCK, 1);
  }
}
#endif

void DspCore::clearDsp(bool black){ fillScreen(black?0:config.theme.background); }
void DspCore::flip(){
#if DSP_MODEL==DSP_ST7789 || DSP_MODEL==DSP_ST7789_76
  setRotation(config.store.flipscreen?3:1);
#endif
#if DSP_MODEL==DSP_ST7789_240
  if(ROTATE_90){
    setRotation(config.store.flipscreen?3:1);
  }else{
    setRotation(config.store.flipscreen?2:0);
  }
#endif
}
void DspCore::invert(){ invertDisplay(config.store.invertdisplay); }
void DspCore::sleep(void){ enableSleep(true); delay(150); enableDisplay(false); delay(150); }
void DspCore::wake(void){ enableDisplay(true); delay(150); enableSleep(false); delay(150); }

#endif
