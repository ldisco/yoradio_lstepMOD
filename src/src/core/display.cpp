#include "Arduino.h"
#include "options.h"
#include "WiFi.h"
#include "time.h"
#include "config.h"
#include "display.h"
#include "player.h"
#include "network.h"
#include "netserver.h"
#include "cpu_load.h"
#include "timekeeper.h"
#include "../pluginsManager/pluginsManager.h"
#include "../displays/dspcore.h"
#include "../displays/widgets/widgets.h"
#include "../displays/widgets/pages.h"
#include "../displays/tools/l10n.h"
#if DSP_MODEL==DSP_NV3041A
#include "../displays/displayNV3041A.h"
#endif
#if DSP_MODEL==DSP_ST7789_170
  #define ST7789_COV st7789_170_cover
#elif DSP_MODEL==DSP_ST7789
  #define ST7789_COV st7789_cover
#elif (DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486) && !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT)
  /* Landscape 480×320: тот же cover-blank API, что у ST7789 CYD. */
  #define ST7789_COV ili9488_cover
  #define ILI9488_LANDSCAPE_COVER_SS 1
#elif DSP_MODEL==DSP_AXS15231B
  /* Faster JC3248: Cover-SS через axs_cover (тот же .inc). */
  #define ST7789_COV axs_cover
  #define AXS15231B_COVER_SS 1
#endif
#if DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || defined(AXS15231B_COVER_SS)
  #define HAS_COVER_BLANK_SS 1
#endif

/* Тач-диалоги: пользователь «сидит» на экране — чужие NEWMODE/перерисовки плеера не срывают UI. */
static bool displayIsTouchStickyMode(displayMode_e m) {
  return m == STATIONS || m == VOL || m == NUMBERS
      || m == INFO || m == SETTINGS || m == TIMEZONE || m == WIFI;
}
/* Разрешить один выход sticky → PLAYER (таймер / тап). Без флага WiFi/CLEAR не сносят диалог. */
static volatile bool s_stickyLeaveArmed = false;
/* Пирамидка PLAYER+PLAY: ST7789 / ILI9488 landscape / Faster AXS при активном FFT.
 * Длинный город — tickCityMarquee в st7789_player_weather.inc. */
#if SPECTRUM_ANALYZER_ACTIVE && (DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || DSP_MODEL==DSP_AXS15231B)
  #define PLAYER_WEATHER_PYRAMID 1
#endif
/*
 * Faster AXS + FFT: не moveTo/moveBack у ScrollWidget погоды —
 * _clear() на Y≈116 срезает верх DIGI56 (glyphAbove≈70 от clockTop=182).
 * PLAY: пирамидка; STOP: unlock + setText без move.
 */
#if SPECTRUM_ANALYZER_ACTIVE && (DSP_MODEL==DSP_AXS15231B)
  #define AXS_WEATHER_NO_MOVE 1
#endif
#if defined(PLAYER_WEATHER_PYRAMID)
  /* Слот weatherMove для статичного блока погоды на PLAYER+PLAY. */
  static void st7789SyncPlayerWeatherSlot(bool played, bool /*hasVu*/) {
    if (!played) return;
    /* Отдельный компактный слот ST7789_PLR_WEATHER_* — не weatherMove,
     * иначе горизонтальный блок наезжает на цифры часов. */
    st7789_player_weather::setSlot((int16_t)ST7789_PLR_WEATHER_X,
                                   (int16_t)ST7789_PLR_WEATHER_Y,
                                   (int16_t)ST7789_PLR_WEATHER_W);
  }
  static void st7789EnterPlayerWeatherPlay(bool hasVu) {
    st7789SyncPlayerWeatherSlot(true, hasVu);
    st7789_player_weather::activate();
    st7789_player_weather::redraw(true);
  }
  static void st7789LeavePlayerWeatherPlay() {
    st7789_player_weather::deactivate();
  }
#endif

Display display;
#ifdef USE_NEXTION
#include "../displays/nextion.h"
Nextion nextion;
#endif

#ifndef CORE_STACK_SIZE
  #define CORE_STACK_SIZE  1024*4
#endif
#ifndef DSP_TASK_PRIORITY
  #define DSP_TASK_PRIORITY  2
#endif
#ifndef DSP_TASK_CORE_ID
  #define DSP_TASK_CORE_ID  0
#endif
#ifndef DSP_TASK_DELAY
  #define DSP_TASK_DELAY pdMS_TO_TICKS(10) // cap for 50 fps
#endif

#define DSP_QUEUE_TICKS 0

#ifndef DSQ_SEND_DELAY
  //#define DSQ_SEND_DELAY portMAX_DELAY
  #define DSQ_SEND_DELAY  pdMS_TO_TICKS(200)
#endif

QueueHandle_t displayQueue;

static void loopDspTask(void * pvParameters){
  while(true){
  #ifndef DUMMYDISPLAY
    if(displayQueue==NULL) break;
    if(timekeeper.loop0()){
      display.loop();
    #ifndef NETSERVER_LOOP1
      netserver.loop();
    #endif
    }
  #else
    timekeeper.loop0();
    #ifndef NETSERVER_LOOP1
      netserver.loop();
    #endif
  #endif
    vTaskDelay(DSP_TASK_DELAY);
  }
  vTaskDelete( NULL );
}

void Display::_createDspTask(){
  xTaskCreatePinnedToCore(loopDspTask, "DspTask", CORE_STACK_SIZE,  NULL,  DSP_TASK_PRIORITY, NULL, DSP_TASK_CORE_ID);
}

#ifndef DUMMYDISPLAY
//============================================================================================================================
DspCore dsp;

Page *pages[] = { new Page(), new Page(), new Page(), new Page() };

#if !((DSP_MODEL==DSP_ST7735 && DTYPE==INITR_BLACKTAB) || DSP_MODEL==DSP_ST7789 || DSP_MODEL==DSP_ST7796 || DSP_MODEL==DSP_ILI9488 \
 || DSP_MODEL==DSP_ILI9486 || DSP_MODEL==DSP_ILI9341 || DSP_MODEL==DSP_ILI9225 || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_NV3041A \
 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701)
  #undef  BITRATE_FULL
  #define BITRATE_FULL     false
#endif


void returnPlayer(){
  display.armStickyLeave();
  display.putRequest(NEWMODE, PLAYER);
}

void Display::armStickyLeave() {
  /* Только из реального тач-диалога — иначе «залипший» arm откроет PLAYER чужому NEWMODE. */
  if (displayIsTouchStickyMode(_mode)) {
    s_stickyLeaveArmed = true;
  }
}

void Display::cancelStickyLeave() {
  s_stickyLeaveArmed = false;
}

bool Display::playlistFocusHit(uint16_t touchY) {
  /* Геометрия из PlayListWidget — без сдвига layout; энкодер/ИК hit не проверяют. */
  if (_plwidget == NULL) return true;
  const uint16_t h = _plwidget->itemHeight();
  if (h == 0) return true;
  const uint16_t top = _plwidget->currentTop();
  return touchY >= top && touchY < (uint16_t)(top + h);
}

// Возвращает реальный размер входного буфера плеера (в байтах).
// Используется как 100% для расчёта процента и цвета полосы — так значение совпадает с диагностикой [DIAG] buf=filled/total.
static uint32_t getPlayerBufferMaxForUi() {
  uint32_t size = player.getInBufferSize();
  return size ? size : 1UL;  // защита от деления на ноль
}

// Переводит текущее заполнение буфера в процент 0..100.
static uint8_t calcBufferPercent(uint32_t currentValue) {
  const uint32_t maxValue = getPlayerBufferMaxForUi();
  if (maxValue == 0) return 0;

  uint32_t percent = (currentValue * 100UL) / maxValue;
  if (percent > 100UL) percent = 100UL;
  return static_cast<uint8_t>(percent);
}

// Локальный перевод RGB888 в RGB565.
// Делаем отдельной функцией здесь, чтобы не обращаться к приватному Config::color565().
static uint16_t toRgb565(uint8_t r, uint8_t g, uint8_t b) {
  // Укладываем 8-битные компоненты в формат 5-6-5:
  // R -> старшие 5 бит, G -> средние 6 бит, B -> младшие 5 бит.
  return ((static_cast<uint16_t>(r) & 0xF8) << 8)
       | ((static_cast<uint16_t>(g) & 0xFC) << 3)
       |  (static_cast<uint16_t>(b) >> 3);
}

// Выбирает цвет индикатора буфера для дисплея по тем же порогам, что и в WebUI:
// 0-15% -> красный, 15-50% -> жёлтый, 50-100% -> зелёный.
static uint16_t getBufferColorForDisplay(uint32_t currentValue) {
  const uint8_t percent = calcBufferPercent(currentValue);
  if (percent <= 15) return toRgb565(255, 63, 47);
  if (percent <= 50) return toRgb565(242, 203, 47);
  return toRgb565(57, 217, 100);
}

// Цвет полоски CPU на TFT — те же пороги/HEX, что getCpuBarColorByPercent в script.js:
// 0–55% зелёный, 56–90% жёлтый, 91–100% красный.
static uint16_t getCpuColorForDisplay(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  if (percent <= 55) return toRgb565(57, 217, 100);
  if (percent <= 90) return toRgb565(242, 203, 47);
  return toRgb565(255, 63, 47);
}

Display::~Display() {
  delete _pager;
  delete _footer;
  delete _plwidget;
  delete _nums;
  delete _clock;
  delete _meta;
  delete _title1;
  delete _title2;
  delete _plcurrent;
}

void Display::init() {
  Serial.print("##[BOOT]#\tdisplay.init\t");
#ifdef USE_NEXTION
  nextion.begin();
#endif
#if LIGHT_SENSOR!=255
  analogSetAttenuation(ADC_0db);
#endif
  _bootStep = 0;
  dsp.initDisplay();
  displayQueue=NULL;
  displayQueue = xQueueCreate( 12, sizeof( requestParams_t ) );
  while(displayQueue==NULL){;}
  _pager = new Pager();
  _footer = new Page();
  _plwidget = new PlayListWidget();
  _nums = new NumWidget();
  _clock = new ClockWidget();
  _meta = new ScrollWidget();
  _title1 = new ScrollWidget();
  _plcurrent = new ScrollWidget();
#if DSP_MODEL==DSP_AXS15231B
  /*
   * Faster: boot-лого синхронно ДО DspTask и ДО network.begin().
   * DspTask и Wi‑Fi оба на core 0 — иначе задача дисплея голодает, BL-hold
   * даёт ~4 с темноты до лого. После _bootScreen _bootStep=1 → loop() не дублирует.
   */
  _pager->begin();
  _bootScreen();
  _createDspTask();
#else
  _createDspTask();
  while(!_bootStep==0) { vTaskDelay(pdMS_TO_TICKS(10)); }
#endif
  Serial.println("done");
}

uint16_t Display::width(){ return dsp.width(); }
uint16_t Display::height(){ return dsp.height(); }
#if TIME_SIZE>19
  #if DSP_MODEL==DSP_SSD1322
    #define BOOT_PRG_COLOR    WHITE
    #define BOOT_TXT_COLOR    WHITE
    #define PINK              WHITE
  #elif DSP_MODEL==DSP_SSD1327
    #define BOOT_PRG_COLOR    0x07
    #define BOOT_TXT_COLOR    0x3f
    #define PINK              0x02
  #else
    #define BOOT_PRG_COLOR    0xE68B
    #define BOOT_TXT_COLOR    0xFFFF
    #define PINK              0xF97F
  #endif
#endif

void Display::_bootScreen(){
  _boot = new Page();
  _boot->addWidget(new ProgressWidget(bootWdtConf, bootPrgConf, BOOT_PRG_COLOR, 0));
  _bootstring = (TextWidget*) &_boot->addWidget(new TextWidget(bootstrConf, 50, true, BOOT_TXT_COLOR, 0));

  // Добавляем строку с версией прошивки под/над строкой "Соединяюсь с %s".
  // Задача: показать пользователю текст вида "<DEVICE_NAME>: <YOVERSION>",
  // при этом НЕ ломая существующую верстку для разных дисплеев.
  // Логотип (drawLogo) остаётся штатным ёRadio; SSID AP не зависит от DEVICE_NAME.
  //
  // Подход:
  //  - Берём базовую конфигурацию bootstrConf (координаты и размер шрифта).
  //  - Создаём локальную копию конфигурации для версии (bootverConfLocal).
  //  - Вычисляем высоту строки по размеру шрифта и CHARHEIGHT.
  //  - Если под bootstrConf ещё достаточно места до низа экрана — ставим версию НИЖЕ.
  //  - Если места нет (маленькие дисплеи 128x64 и т.п.) — ставим версию ВЫШЕ на одну строку.
  //  - Таким образом для «высоких» дисплеев выполняется требование «ниже надписи»,
  //    а для компактных дисплеев мы не выходим за границы экрана.
  {
    WidgetConfig bootverConfLocal = bootstrConf;
    uint16_t lineHeight = CHARHEIGHT * bootverConfLocal.textsize;
    if (lineHeight == 0) {
      // Защита от некорректного размера шрифта: используем базовую высоту символа.
      lineHeight = CHARHEIGHT;
    }

#if DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486
    // ILI9488/ILI9486: строка версии на 4 px ниже строки «Соединяюсь с…» (после первой строки текста).
    constexpr uint16_t BOOT_VER_GAP_PX = 4;
#elif DSP_MODEL==DSP_ST7789_170
    // ST7789 170: зазор между «Соединяюсь с…» и yoradio: <версия> (вместе с bootstrConf.top=146).
    constexpr uint16_t BOOT_VER_GAP_PX = 4;
#elif DSP_MODEL==DSP_ST7789
    /* JC2432 320×240: «Соединяюсь с…» и версия слишком близко — +3 px вниз. */
    constexpr uint16_t BOOT_VER_GAP_PX = 3;
#elif DSP_MODEL==DSP_AXS15231B
    /* Faster 480×320: «Соединяюсь с…» и версия сливаются — +3 px вниз. */
    constexpr uint16_t BOOT_VER_GAP_PX = 3;
#elif DSP_MODEL==DSP_ST7701
    /* Supremus 480×480: версия ниже «Соединяюсь с…», иначе слипаются. */
    constexpr uint16_t BOOT_VER_GAP_PX = 10;
#else
    constexpr uint16_t BOOT_VER_GAP_PX = 0;
#endif
    const uint16_t verTopPrefer = (uint16_t)(bootstrConf.top + lineHeight + BOOT_VER_GAP_PX);

    // Предпочтительно размещаем строку ВЕРСИИ НИЖЕ основной boot-строки,
    // если это не выводит текст за пределы экрана.
    if ((uint16_t)(verTopPrefer + lineHeight) < dsp.height()) {
      bootverConfLocal.top = verTopPrefer;
    } else {
      // Если ниже места нет — сдвигаем строку версии ВЫШЕ на одну строку,
      // чтобы она оставалась в пределах видимой области.
      if (bootstrConf.top > lineHeight) {
        bootverConfLocal.top = (uint16_t)(bootstrConf.top - lineHeight);
      } else {
        // Совсем маленький дисплей: оставляем ту же координату,
        // чтобы хотя бы текст был виден, даже если частично перекроется.
        bootverConfLocal.top = bootstrConf.top;
      }
    }

    // Формируем строку версии в формате "<DEVICE_NAME>: <YOVERSION>".
    char verBuf[56];
    snprintf(verBuf, sizeof(verBuf), "%s: %s", DEVICE_NAME, YOVERSION);

    // Для цветных дисплеев, представленных текущими профилями в src/myoptions,
    // окрашиваем версию тем же цветом темы, которым рисуется индикатор RSSI.
    // Для остальных дисплеев сохраняем штатный BOOT_TXT_COLOR:
    // монохромные и символьные экраны не поддерживают RGB-цвет темы.
#if DSP_MODEL==DSP_NV3041A || DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_ST7789 || DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
    /* Цвет как у значка/строки Wi‑Fi (theme.rssi), не белый BOOT_TXT_COLOR. */
    const uint16_t bootVersionColor = config.theme.rssi;
#else
    const uint16_t bootVersionColor = BOOT_TXT_COLOR;
#endif

    _bootversion = (TextWidget*)&_boot->addWidget(
      new TextWidget(bootverConfLocal, sizeof(verBuf), false, bootVersionColor, 0)
    );
    _bootversion->setText(verBuf);
  }

  _pager->addPage(_boot);
  _pager->setPage(_boot, true);
  dsp.drawLogo(bootLogoTop);
  // На части дисплеев (NV3041A) drawLogo() делает clearDsp() — версия могла не попасть на экран.
  // После логотипа принудительно перерисовываем строку версии.
  if (_bootversion) {
    char verBuf2[56];
    snprintf(verBuf2, sizeof(verBuf2), "%s: %s", DEVICE_NAME, YOVERSION);
    _bootversion->setText(verBuf2);
  }
  _bootStep = 1;
}

void Display::_buildPager(){
  _meta->init("*", metaConf, config.theme.meta, config.theme.metabg);
  _title1->init("*", title1Conf, config.theme.title1, config.theme.background);
  _clock->init(clockConf, 0, 0);
  #if DSP_MODEL==DSP_NOKIA5110
    _plcurrent->init("*", playlistConf, 0, 1);
  #elif defined(HAS_PLAYLIST_FOCUS_CONF)
    /* Фокус крупнее соседей (playlistConf); высота ряда — от focus conf. */
    _plcurrent->init("*", playlistFocusConf, config.theme.plcurrent, config.theme.plcurrentbg);
  #else
    _plcurrent->init("*", playlistConf, config.theme.plcurrent, config.theme.plcurrentbg);
  #endif
  _plwidget->init(_plcurrent);
  #if !defined(DSP_LCD)
    #if defined(HAS_PLAYLIST_FOCUS_CONF)
    _plcurrent->moveTo({TFT_FRAMEWDT, (uint16_t)(_plwidget->currentTop()), (int16_t)playlistFocusConf.width});
    #else
    _plcurrent->moveTo({TFT_FRAMEWDT, (uint16_t)(_plwidget->currentTop()), (int16_t)playlistConf.width});
    #endif
  #endif
  #ifndef HIDE_TITLE2
    _title2 = new ScrollWidget("*", title2Conf, config.theme.title2, config.theme.background);
  #endif
  #if !defined(DSP_LCD) && DSP_MODEL!=DSP_NOKIA5110
    _plbackground = new FillWidget(playlBGConf, config.theme.plcurrentfill);
    #if DSP_INVERT_TITLE || defined(DSP_OLED)
      _metabackground = new FillWidget(metaBGConf, config.theme.metafill);
    #else
      #if DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
      /* После INVERT metafill=div (255,255,0), фон букв=metabg (251,219,0) — полоса другого оттенка.
       * AXS (281) + ST7701/Supremus: заливка pad = metabg, иначе следы скролла на фоне. */
      _metabackground = new FillWidget(metaBGConfInv, config.theme.metabg);
      #else
      _metabackground = new FillWidget(metaBGConfInv, config.theme.metafill);
      #endif
    #endif
  #endif
  #if DSP_MODEL==DSP_NOKIA5110
    _plbackground = new FillWidget(playlBGConf, 1);
    //_metabackground = new FillWidget(metaBGConf, 1);
  #endif
  #ifndef HIDE_VU
    #if SPECTRUM_ANALYZER_ACTIVE
    /*
     * SPECTRUM_ANALYZER_ACTIVE: переключатель VU в WebUI управляет FFT-виджетом
     * (ST7789 или ILI9488 landscape). Указатель _vuwidget общий — PLAY/STOP/
     * SHOWVUMETER и lock/unlock без дублирования. Пирамидка погоды —
     * PLAYER_WEATHER_PYRAMID (ST7789 / ILI9488 landscape / AXS + FFT).
     */
    _vuwidget = new SpectrumWidget(spectrumConf, spectrumBandsConf,
                                   config.theme.vumax, config.theme.vumin,
                                   config.theme.background);
    #else
    _vuwidget = new VuWidget(vuConf, bandsConf, config.theme.vumax, config.theme.vumin, config.theme.background);
    #endif
  #endif
  #ifndef HIDE_VOLBAR
    _volbar = new SliderWidget(volbarConf, config.theme.volbarin, config.theme.background, 100, config.theme.volbarout);
  #endif
  #ifndef HIDE_HEAPBAR
    // Начальный max — запасной; при первом обновлении вызовется setMax(player.getInBufferSize()).
    _heapbar = new SliderWidget(heapbarConf, config.theme.buffer, config.theme.background, 655350UL);
  #endif
  /* Полоска CPU как _heapbar: SliderWidget + lock по «CPU Info» (webCpuInfo).
   * Геометрия — cpubarConf в conf DSP (ST7701 / AXS Faster). */
#if (DSP_MODEL==DSP_ST7701 || DSP_MODEL==DSP_AXS15231B) && WEBUI_CPU_BAR_ENABLE
  #ifndef HIDE_CPUBAR
    _cpubar = new SliderWidget(cpubarConf, config.theme.buffer, config.theme.background, 100);
  #endif
#endif
  #ifndef HIDE_VOL
    _voltxt = new TextWidget(voltxtConf, 10, false, config.theme.vol, config.theme.background);
  #endif
  #ifndef HIDE_IP
    _volip = new TextWidget(iptxtConf, 30, false, config.theme.ip, config.theme.background);
  #endif
  #ifndef HIDE_RSSI
    _rssi = new TextWidget(rssiConf, 20, false, config.theme.rssi, config.theme.background);
  #endif
  _nums->init(numConf, 10, false, config.theme.digit, config.theme.background);
  #ifndef HIDE_WEATHER
    _weather = new ScrollWidget("\007", weatherConf, config.theme.weather, config.theme.background);
  #endif
  
  if(_volbar)   _footer->addWidget( _volbar);
  if(_voltxt)   _footer->addWidget( _voltxt);
  if(_volip)    _footer->addWidget( _volip);
  if(_rssi)     _footer->addWidget( _rssi);
  if(_heapbar)  _footer->addWidget( _heapbar);
  if(_cpubar)   _footer->addWidget( _cpubar);
#if DSP_MODEL==DSP_ST7789_170
  if(_volip) _volip->unlock();
#endif
  
  if(_metabackground) pages[PG_PLAYER]->addWidget( _metabackground);
  pages[PG_PLAYER]->addWidget(_meta);
  pages[PG_PLAYER]->addWidget(_title1);
  if(_title2) pages[PG_PLAYER]->addWidget(_title2);
  if(_weather) pages[PG_PLAYER]->addWidget(_weather);
  #if BITRATE_FULL
    _fullbitrate = new BitrateWidget(fullbitrateConf, config.theme.bitrate, config.theme.background);
    pages[PG_PLAYER]->addWidget( _fullbitrate);
  #else
    _bitrate = new TextWidget(bitrateConf, 30, false, config.theme.bitrate, config.theme.background);
    pages[PG_PLAYER]->addWidget( _bitrate);
  #endif
  if(_vuwidget) pages[PG_PLAYER]->addWidget( _vuwidget);
  pages[PG_PLAYER]->addWidget(_clock);
  pages[PG_SCREENSAVER]->addWidget(_clock);
  pages[PG_PLAYER]->addPage(_footer);

  if(_metabackground) pages[PG_DIALOG]->addWidget( _metabackground);
  pages[PG_DIALOG]->addWidget(_meta);
  pages[PG_DIALOG]->addWidget(_nums);
  
  #if !defined(DSP_LCD) && DSP_MODEL!=DSP_NOKIA5110
    pages[PG_DIALOG]->addPage(_footer);
  #endif
  #if !defined(DSP_LCD)
  if(_plbackground) {
    pages[PG_PLAYLIST]->addWidget( _plbackground);
    _plbackground->setHeight(_plwidget->itemHeight());
    _plbackground->moveTo({0,(uint16_t)(_plwidget->currentTop()-playlistConf.widget.textsize*2), (int16_t)playlBGConf.width});
  }
  #endif
  pages[PG_PLAYLIST]->addWidget(_plcurrent);
  pages[PG_PLAYLIST]->addWidget(_plwidget);
  for(const auto& p: pages) _pager->addPage(p);
}

void Display::_apScreen() {
  if(_boot) _pager->removePage(_boot);
  #ifndef DSP_LCD
    _boot = new Page();
    #if DSP_MODEL!=DSP_NOKIA5110
      #if DSP_INVERT_TITLE || defined(DSP_OLED)
      _boot->addWidget(new FillWidget(metaBGConf, config.theme.metafill));
      #else
      #if DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
      /* Как PLAYER: pad metabg, не metafill (см. комментарий у _metabackground). */
      _boot->addWidget(new FillWidget(metaBGConfInv, config.theme.metabg));
      #else
      _boot->addWidget(new FillWidget(metaBGConfInv, config.theme.metafill));
      #endif
      #endif
    #endif
    ScrollWidget *bootTitle = (ScrollWidget*) &_boot->addWidget(new ScrollWidget("*", apTitleConf, config.theme.meta, config.theme.metabg));
    bootTitle->setText("ёRadio AP Mode");
    TextWidget *apname = (TextWidget*) &_boot->addWidget(new TextWidget(apNameConf, 30, false, config.theme.title1, config.theme.background));
    apname->setText(LANG::apNameTxt);
    TextWidget *apname2 = (TextWidget*) &_boot->addWidget(new TextWidget(apName2Conf, 30, false, config.theme.clock, config.theme.background));
    apname2->setText(apSsid);
    TextWidget *appass = (TextWidget*) &_boot->addWidget(new TextWidget(apPassConf, 30, false, config.theme.title1, config.theme.background));
    appass->setText(LANG::apPassTxt);
    TextWidget *appass2 = (TextWidget*) &_boot->addWidget(new TextWidget(apPass2Conf, 30, false, config.theme.clock, config.theme.background));
    appass2->setText(apPassword);
    ScrollWidget *bootSett = (ScrollWidget*) &_boot->addWidget(new ScrollWidget("*", apSettConf, config.theme.title2, config.theme.background));
    bootSett->setText(config.ipToStr(WiFi.softAPIP()), LANG::apSettFmt);
    _pager->addPage(_boot);
    _pager->setPage(_boot);
  #else
    dsp.apScreen();
  #endif
}

void Display::_start() {
  if(_boot) _pager->removePage(_boot);
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
  /* Opus 5.5: не PLAYER и не AP-скролл — только статичный паттерн. */
  st7701RgbSoakEnter();
  _bootStep = 2;
  return;
#endif
  #ifdef USE_NEXTION
    nextion.wake();
  #endif
  if (network.status != CONNECTED && network.status != SDREADY) {
    _apScreen();
    #ifdef USE_NEXTION
      nextion.apScreen();
    #endif
    _bootStep = 2;
    return;
  }
  #ifdef USE_NEXTION
    //nextion.putcmd("page player");
    nextion.start();
  #endif
  _buildPager();
  _mode = PLAYER;
  config.setTitle(LANG::const_PlReady);
  
  if(_heapbar)  _heapbar->lock(!config.store.audioinfo);
  // Аналог heapbar↔audioinfo: полоска CPU на TFT видна только при «CPU Info» (webCpuInfo).
  if(_cpubar)   _cpubar->lock(!config.store.webCpuInfo);
  
  if(_weather)  _weather->lock(!config.store.showweather);
  if(_weather && config.store.showweather)  _weather->setText(LANG::const_getWeather);
#if defined(PLAYER_WEATHER_PYRAMID)
  /* PLAY: пирамидка (ScrollWidget locked). STOP: стоковая бегущая строка — не lock здесь. */
  if (_weather && player.isRunning()) _weather->lock(true);
#endif

  if(_vuwidget) _vuwidget->lock();
  if(_rssi)     _setRSSI(WiFi.RSSI());
  #ifndef HIDE_IP
    if(_volip) _volip->setText(config.ipToStr(WiFi.localIP()), iptxtFmt);
  #endif
#if DSP_MODEL==DSP_ST7789_170
  if(_volip) _volip->unlock();
  if(_heapbar && config.store.audioinfo) _heapbar->unlock();
#endif
  _pager->setPage( pages[PG_PLAYER]);
  _volume();
  _station();
  _time(false);
  _bootStep = 2;
  pluginsManager::getInstance().on_display_player();
}

void Display::_showDialog(const char *title){
  dsp.setScrollId(NULL);
  _pager->setPage( pages[PG_DIALOG]);
  #ifdef META_MOVE
    _meta->moveTo(metaMove);
  #endif
  _meta->setAlign(WA_CENTER);
#if DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486
  /* Диалог VOL: очистить полосу meta — иначе бегущая строка станции + «ГРОМКОСТЬ» с артефактами. */
  dsp.fillRect(0, 0, DSP_WIDTH, 50, config.theme.metabg);
#endif
  _meta->setText(title);
  dsp.setScrollId(NULL);
}

void Display::_swichMode(displayMode_e newmode) {
  #ifdef USE_NEXTION
    //nextion.swichMode(newmode);
    nextion.putRequest({NEWMODE, newmode});
  #endif
#if defined(PLAYER_WEATHER_PYRAMID)
  /* Уход с PLAYER — убрать статичный блок погоды до смены страницы.
   * ScrollWidget _weather НЕ unlock: иначе при выходе из SS кратко вспыхивает
   * старая бегущая строка в слоте пирамидки. */
  if (_mode == PLAYER && newmode != PLAYER) {
    st7789LeavePlayerWeatherPlay();
    if (_weather) _weather->lock(true);
  }
#endif
  if (newmode == _mode || (network.status != CONNECTED && network.status != SDREADY)) return;
  /* Тач-диалог: чужие NEWMODE (LOST/SS/CLEAR/PLAYER от WiFi) не принимать.
   * PLAYER только если armStickyLeave() — таймер waitAndReturn / явный тап. */
  if (displayIsTouchStickyMode(_mode) && newmode != _mode) {
    if (displayIsTouchStickyMode(newmode)) {
      /* STATIONS↔VOL и т.п. — ок */
      s_stickyLeaveArmed = false;
    } else if (newmode == PLAYER && s_stickyLeaveArmed) {
      s_stickyLeaveArmed = false;
    } else {
      return;
    }
  }
  if (displayIsTouchStickyMode(newmode)) {
    s_stickyLeaveArmed = false;
  }
  displayMode_e prevMode = _mode;
  if (_mode == STATIONS && newmode != STATIONS) {
    #if DSP_MODEL==DSP_NV3041A
      dsp.clearDsp();
    #endif
  }
  _mode = newmode;
#if DSP_MODEL==DSP_ST7701
  /* Любая смена экрана — не flush EEPROM в этот кадр (idle-commit). */
  yoEepromMarkUiBusy();
#endif
  /* Уход с VOL: не commit здесь (вспышка вместе с rebuild PLAYER).
   * Сдвигаем отложенный saveVolume на ~3 с уже на экране PLAYER. */
  if (prevMode == VOL && newmode != VOL) {
    player.bumpVolumeSaveTimer();
  }
  dsp.setScrollId(NULL);
  if (newmode == PLAYER) {
#if DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || defined(AXS15231B_COVER_SS)
    /* Cover blank: lock()+clear при входе; без unlock — только «:» без цифр. */
    _clock->unlock();
#endif
    config.isScreensaver = false;
    if(player.isRunning())
      if(clockMove.width<0) _clock->moveBack(); else _clock->moveTo(clockMove);
    else
      _clock->moveBack();
#if DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || defined(AXS15231B_COVER_SS)
    _clock->draw(true);
#endif
    #ifdef DSP_LCD
      dsp.clearDsp();
    #endif
    numOfNextStation = 0;
    #ifdef META_MOVE
      _meta->moveBack();
    #endif
    _meta->setAlign(metaConf.widget.align);
    _meta->setText(config.station.name);
    _nums->setText("");
    _pager->setPage( pages[PG_PLAYER]);
    config.setDspOn(config.store.dspon, false);
    // Вернуть VU при входе на PLAYER (из SDCHANGE/VOL и т.п.) — иначе остаётся locked.
    _layoutChange(player.isRunning());
    pluginsManager::getInstance().on_display_player();
  }
  if (newmode == SCREENSAVER || newmode == SCREENBLANK) {
    config.isScreensaver = true;
    /*
     * На время скринсейвера VU/спектр держим locked: WebUI может прислать
     * SHOWVUMETER, а виджет не должен рисовать и не должен менять раскладку часов.
     */
    if (_vuwidget) _vuwidget->lock();
    #if DSP_MODEL==DSP_NV3041A
      dsp.clearDsp();  // Очистка всего экрана при переходе в скринсейвер (убирает логотип и артефакты)
    #endif
#if DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || defined(AXS15231B_COVER_SS)
    /* Cover-SS: очистка до setPage, иначе часы плеера остаются на TFT.
       blank ON — не Cover-SS (гашение ниже). */
    if (newmode == SCREENBLANK && player.isRunning() && isDisplayCoversEnabled()
        && !config.store.screensaverPlayingBlank) {
      dsp.clearDsp();
      ST7789_COV::preparePlayingBlankEntry();
      /* ClockWidget плеера не должен рисовать поверх cover-layout. */
      _clock->lock(true);
    }
#endif
    _pager->setPage( pages[PG_SCREENSAVER]);
    if (newmode == SCREENBLANK) {
      _clock->clear();
      _clock->lock(true); /* blank/Cover: ClockWidget SS не рисует «белые часы» под гашением */
#if defined(HAS_COVER_BLANK_SS)
      /* Playing + blank OFF + Display Cover → Cover-SS; blank ON или Cover OFF → гашение. */
      if (player.isRunning() && isDisplayCoversEnabled()
          && !config.store.screensaverPlayingBlank) {
        /* CoverDL только в активном Cover-SS (не при blank). */
        notifyCoverScreensaverEntered();
        if (config.store.dspon) config.setDspOn(true, false);
        ST7789_COV::resetEncoderNavGuard();
        putRequest(CLOCK, 1);
      } else {
        /* Реальный blank: чёрный кадр, потом BL/sleep — иначе часто «белый» theme.background. */
        dsp.clearDsp(true);
#if defined(ILI9488_LANDSCAPE_COVER_SS)
        /* ILI9488+XPT: soft blank (только BL), без dsp.sleep — тач на общей SPI. */
#if BRIGHTNESS_PIN!=255
        analogWrite(BRIGHTNESS_PIN, 0);
#endif
#else
        config.setDspOn(false, false);
#endif
      }
#else
      dsp.clearDsp(true);
      config.setDspOn(false, false);
#endif
    }
  }else{
    config.screensaverTicks=SCREENSAVERSTARTUPDELAY;
    config.screensaverPlayingTicks=SCREENSAVERSTARTUPDELAY;
    config.isScreensaver = false;
    notifyCoverScreensaverLeft();
  }
  if (newmode == VOL) {
    // Для NV3041A нужна очистка экрана перед VOL, иначе остаются артефакты от обложек
    #if DSP_MODEL==DSP_NV3041A
      dsp.clearDsp();
    #endif
    #ifndef HIDE_IP
      _showDialog(LANG::const_DlgVolume);
    #else
      _showDialog(config.ipToStr(WiFi.localIP()));
    #endif
    // Процент 0–100 для NumWidget (как в _volume()), не сырое значение 0–254.
    _nums->setText((config.store.volume * 100) / 254, numtxtFmt);
    timekeeper.waitAndReturnPlayer(3);
  }
  if (newmode == LOST)      _showDialog(LANG::const_DlgLost);
  if (newmode == UPDATING)  _showDialog(LANG::const_DlgUpdate);
  if (newmode == SLEEPING)  _showDialog("SLEEPING");
  if (newmode == SDCHANGE)  _showDialog(LANG::const_waitForSD);
  if (newmode == INFO || newmode == SETTINGS || newmode == TIMEZONE || newmode == WIFI) _showDialog(LANG::const_DlgNextion);
  if (newmode == NUMBERS) _showDialog("");
  if (newmode == STATIONS) {
    _pager->setPage( pages[PG_PLAYLIST]);
    _plcurrent->setText("");
    currentPlItem = config.lastStation();
    _drawPlaylist();
  }
  
}

void Display::resetQueue(){
  if(displayQueue!=NULL) xQueueReset(displayQueue);
}

void Display::_drawPlaylist() {
#if DSP_MODEL==DSP_ST7701
  /* Скролл плейлиста — UI busy, EEPROM.commit только после ухода в idle. */
  yoEepromMarkUiBusy();
#endif
  /* Пользователь ещё в списке — не отдавать PLAYER от «просроченного» таймера. */
  cancelStickyLeave();
  //dsp.drawPlaylist(currentPlItem);
  _plwidget->drawPlaylist(currentPlItem);
  /* 4 с после крайнего касания / перерисовки → PLAYER. */
  timekeeper.waitAndReturnPlayer(4);
}

void Display::_drawNextStationNum(uint16_t num) {
  timekeeper.waitAndReturnPlayer(30);
  _meta->setText(config.stationByNum(num));
  _nums->setText(num, "%d");
}

void Display::putRequest(displayRequestType_e type, int payload){
  if(displayQueue==NULL) return;
  requestParams_t request;
  request.type = type;
  request.payload = payload;
  // Не блокируем отправителя (WS/CLI): очередь полна — пропускаем запрос.
  if(uxQueueSpacesAvailable(displayQueue) == 0) return;
  xQueueSend(displayQueue, &request, 0);
  #ifdef USE_NEXTION
    nextion.putRequest(request);
  #endif
}

void Display::_layoutChange(bool played){
  /*
   * Раскладка часов/VU/погоды относится только к странице PLAYER.
   * _clock общий с PG_SCREENSAVER: при SCREENBLANK/SCREENSAVER он _active=true,
   * и moveTo()/moveBack() безусловно вызывают _draw() → часы плеера (дата/dow)
   * и чёрные mono-подложки рисуются поверх cover-скринсейвера (артефакт при
   * SHOWVUMETER / PSTART из WebUI, пока активен скринсейвер).
   * Настройку VU при этом уже сохранили в store — применим при возврате на PLAYER.
   */
  if (_mode != PLAYER) {
    /* На любой не-PLAYER странице VU не должен оставаться «разблокированным». */
    if (_vuwidget && !_vuwidget->locked()) _vuwidget->lock();
    return;
  }
#if defined(PLAYER_WEATHER_PYRAMID)
  /* STOP/PAUSE: убрать пирамидку; бегущую строку вернём ниже после moveBack. */
  if (!played) {
    st7789LeavePlayerWeatherPlay();
  }
#endif
  if(config.store.vumeter && _vuwidget){
    if(played){
      if(_vuwidget) _vuwidget->unlock();
#if DSP_MODEL==DSP_ST7789_170 || (DSP_MODEL==DSP_ST7701 && !SPECTRUM_ANALYZER_ACTIVE)
      /* ST7789_170 / Supremus VU: не двигать часы на PSTART — moveTo/moveBack
       * чистят зону → рывок на RGB. Макет фиксированный (clockMove.width=-1).
       * Supremus + FFT: как Faster — moveTo(clockMove), иначе спектр на часах. */
#else
      //_clock->moveTo(clockMove);
      if(clockMove.width<0) _clock->moveBack(); else _clock->moveTo(clockMove);
#endif
#if !defined(AXS_WEATHER_NO_MOVE)
#if DSP_MODEL==DSP_ST7701
      /* Погоду паркуем под VU один раз; дальше не moveBack/moveTo на stop/play. */
      static bool s_st7701WeatherParked = false;
      if (_weather && !s_st7701WeatherParked) {
        _weather->moveTo(weatherMoveVU);
        s_st7701WeatherParked = true;
      }
#else
      if(_weather) _weather->moveTo(weatherMoveVU);
#endif
#endif
    }else{
      if(_vuwidget) if(!_vuwidget->locked()) _vuwidget->lock();
#if DSP_MODEL==DSP_ST7789_170 || (DSP_MODEL==DSP_ST7701 && !SPECTRUM_ANALYZER_ACTIVE)
      /* Не возвращаем часы — без лишней очистки зоны. */
#else
      _clock->moveBack();
#endif
#if !defined(AXS_WEATHER_NO_MOVE)
#if DSP_MODEL!=DSP_ST7701
      if(_weather) _weather->moveBack();
#endif
#endif
    }
  }else{
    if(played){
#if DSP_MODEL==DSP_ST7789_170 || (DSP_MODEL==DSP_ST7701 && !SPECTRUM_ANALYZER_ACTIVE)
      /* Часы на месте. */
#else
      if(clockMove.width<0) _clock->moveBack(); else _clock->moveTo(clockMove);
#endif
#if !defined(AXS_WEATHER_NO_MOVE)
#if DSP_MODEL!=DSP_ST7701
      if(_weather) _weather->moveTo(weatherMove);
#endif
#endif
      //_clock->moveBack();
    }else{
#if !defined(AXS_WEATHER_NO_MOVE)
#if DSP_MODEL!=DSP_ST7701
      if(_weather) _weather->moveBack();
#endif
#endif
#if DSP_MODEL==DSP_ST7789_170 || (DSP_MODEL==DSP_ST7701 && !SPECTRUM_ANALYZER_ACTIVE)
      /* Часы на месте. */
#else
      _clock->moveBack();
#endif
    }
  }
#if defined(PLAYER_WEATHER_PYRAMID)
  /* PLAY: ScrollWidget locked + пирамидка. STOP: стоковая бегущая строка (unlock). */
  const bool hasVu = config.store.vumeter && (_vuwidget != nullptr);
  if (played && config.store.showweather) {
    if (_weather) _weather->lock(true);
    st7789EnterPlayerWeatherPlay(hasVu);
  } else if (!played && config.store.showweather && _weather) {
    _weather->unlock();
    if (timekeeper.weatherBuf) _weather->setText(timekeeper.weatherBuf);
    else _weather->setText(LANG::const_getWeather);
  } else if (_weather) {
    _weather->lock(true);
  }
#endif
}

void Display::loop() {
  if(_bootStep==0) {
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
    /* Без boot-лого/pulse — сразу к DSP_START → статичный паттерн. */
    _bootStep = 1;
    return;
#endif
    _pager->begin();
    _bootScreen();
    return;
  }
#if (DSP_MODEL == DSP_ST7701) && ST7701_RGB_SOAK
  /* Статика: не pager, не idle EEPROM.flush — иначе «моргание» от софта, не от панели. */
  if (_bootStep >= 2) return;
#endif
  if(displayQueue==NULL || _locked || _sleeping) return;
#if DSP_MODEL==DSP_ST7701
  /* Грязный EEPROM → flash, когда не STATIONS/VOL и ~3 с без UI-busy / save. */
  yoEepromTryIdleFlush();
#endif
  pollCoverDownloadAfterScreensaver();
  /*
   * SS/blank: сначала очередь (NEWMODE PLAYER), иначе после setDspOn(true) один кадр
   * _pager рисует часы SS — «первый тач → часы, второй → плеер».
   */
  requestParams_t request;
  if (( _mode == SCREENSAVER || _mode == SCREENBLANK)
      && xQueueReceive(displayQueue, &request, 0)) {
    bool pm_result = true;
    pluginsManager::getInstance().on_display_queue(request, pm_result);
    if (pm_result) {
      switch (request.type) {
        case NEWMODE: _swichMode((displayMode_e)request.payload); break;
        case CLOCK:
#if defined(HAS_COVER_BLANK_SS)
          if (_mode == SCREENBLANK && player.isRunning()) {
            ST7789_COV::tickPlayingBlankScreen(request.payload == 1);
            break;
          }
#endif
          if (_mode == PLAYER || _mode == SCREENSAVER) _time(request.payload == 1);
          break;
        default: break;
      }
    }
    if (_mode == PLAYER) {
#if DSP_MODEL==DSP_AXS15231B
      axsPresentFrame();
#endif
      dsp.loop();
      return;
    }
  }
  _pager->loop();
#ifdef USE_NEXTION
  nextion.loop();
#endif
  if(xQueueReceive(displayQueue, &request, DSP_QUEUE_TICKS)){
    bool pm_result = true;
    pluginsManager::getInstance().on_display_queue(request, pm_result);
    if(pm_result)
      switch (request.type){
        case NEWMODE: _swichMode((displayMode_e)request.payload); break;
        case CLOSEPLAYLIST: player.sendCommand({PR_PLAY, request.payload}); break;
        case CLOCK:
#if defined(HAS_COVER_BLANK_SS)
          /* Playing + Display Cover: обложка / fallback-часы без виджета ClockWidget. */
          if (_mode == SCREENBLANK && player.isRunning()) {
            ST7789_COV::tickPlayingBlankScreen(request.payload == 1);
            break;
          }
#endif
          if(_mode==PLAYER || _mode==SCREENSAVER) _time(request.payload==1); 
          /*#ifdef USE_NEXTION
            if(_mode==TIMEZONE) nextion.localTime(network.timeinfo);
            if(_mode==INFO)     nextion.rssi();
          #endif*/
          break;
        case NEWTITLE:
          /* Не трогать meta/title поверх плейлиста/громкости (recovery шлёт NEWSTATION/TITLE). */
          if (displayIsTouchStickyMode(_mode)) break;
          _title();
          break;
        case NEWSTATION:
          if (displayIsTouchStickyMode(_mode)) break;
          _station();
          break;
        case NEXTSTATION:
          if (displayIsTouchStickyMode(_mode) && _mode != NUMBERS) break;
          _drawNextStationNum(request.payload);
          break;
        case DRAWPLAYLIST: _drawPlaylist(); break;
        case DRAWVOL: _volume(); break;
        case DBITRATE: {
            if (displayIsTouchStickyMode(_mode)) break;
            char buf[20]; 
            snprintf(buf, 20, bitrateFmt, config.station.bitrate); 
            if(_bitrate) { _bitrate->setText(config.station.bitrate==0?"":buf); } 
            if(_fullbitrate) { 
              _fullbitrate->setBitrate(config.station.bitrate); 
              _fullbitrate->setFormat(config.configFmt); 
            } 
          }
          break;
        case AUDIOINFO:
          if(_heapbar)  {
            _heapbar->lock(!config.store.audioinfo);
            const uint32_t bufFilled = player.inBufferFilled();
            // 100% = реальный размер буфера (как в [DIAG] buf=filled/total).
            _heapbar->setMax(getPlayerBufferMaxForUi());
            _heapbar->setColor(getBufferColorForDisplay(bufFilled));
            _heapbar->setValue(bufFilled);
          }
          break;
        case WEBCPUINFO:
          // Полная аналогия AUDIOINFO/_heapbar: lock по выключателю + сразу обновить значение/цвет.
          if(_cpubar)  {
            _cpubar->lock(!config.store.webCpuInfo);
            if (config.store.webCpuInfo) {
              const int cpuPct = webui_freertos_cpu_percent();
              _cpubar->setMax(100);
              // Сначала длина, потом цвет: иначе setColor→_draw() заливает СТАРУЮ ширину новым цветом.
              _cpubar->setValue(static_cast<uint32_t>(cpuPct));
              _cpubar->setColor(getCpuColorForDisplay(cpuPct));
            }
          }
          break;
        case SHOWVUMETER: {
          if(_vuwidget){
            /*
             * VU/спектр только на PLAYER. При скринсейвере (cover-blank и т.п.)
             * store.vumeter уже сохранён из WebUI; виджет не разблокируем и
             * _layoutChange не вызываем — иначе moveTo часов портит SS.
             * При выходе на PLAYER _layoutChange применит настройку штатно.
             */
            const bool onPlayer = (_mode == PLAYER);
            const bool shouldShow = onPlayer && config.store.vumeter && player.isRunning();
            _vuwidget->lock(!shouldShow);
            if (onPlayer) {
              _layoutChange(player.isRunning());
            }
          }
          break;
        }
        case SHOWWEATHER: {
          if(_weather) _weather->lock(!config.store.showweather);
#if DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || defined(AXS15231B_COVER_SS)
          ST7789_COV::notifyWeatherIconUpdated();
#endif
#if defined(PLAYER_WEATHER_PYRAMID)
          /* PLAY: только пирамидка. STOP на PLAYER: стоковая бегущая строка. */
          if (!config.store.showweather) {
            st7789LeavePlayerWeatherPlay();
            if (_weather) _weather->lock(true);
          } else if (_mode == PLAYER && player.isRunning()) {
            if (_weather) _weather->lock(true);
            st7789SyncPlayerWeatherSlot(true, config.store.vumeter && _vuwidget);
            st7789_player_weather::activate();
            st7789_player_weather::redraw(true);
          } else if (_mode == PLAYER && _weather) {
            st7789LeavePlayerWeatherPlay();
            _weather->unlock();
            _weather->setText(LANG::const_getWeather);
          }
#endif
#if DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || defined(AXS15231B_COVER_SS)
          if (_mode == SCREENBLANK && player.isRunning() && isDisplayCoversEnabled()) {
            putRequest(CLOCK, 1);
          }
          #ifndef HIDE_IP
          if(_volip) _volip->setText(config.ipToStr(WiFi.localIP()), iptxtFmt);
          #endif
#if !(defined(PLAYER_WEATHER_PYRAMID))
          if(config.store.showweather && _weather) _weather->setText(LANG::const_getWeather);
#endif
#elif DSP_MODEL==DSP_NV3041A
          notifyNv3041SsWeatherUpdated();
          if (_mode == SCREENSAVER) putRequest(CLOCK, 1);
          if(!config.store.showweather){
            #ifndef HIDE_IP
            if(_volip) _volip->setText(config.ipToStr(WiFi.localIP()), iptxtFmt);
            #endif
          }else{
            if(_weather) _weather->setText(LANG::const_getWeather);
          }
#else
          /* AXS и прочие без Cover-SS: пирамидка уже выше; здесь STOP/выкл. */
          if(!config.store.showweather){
            #ifndef HIDE_IP
            if(_volip) _volip->setText(config.ipToStr(WiFi.localIP()), iptxtFmt);
            #endif
          }else{
#if !defined(PLAYER_WEATHER_PYRAMID)
            if(_weather) _weather->setText(LANG::const_getWeather);
#endif
          }
#endif
          break;
        }
        case NEWWEATHER: {
#if DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || defined(AXS15231B_COVER_SS)
          ST7789_COV::notifyWeatherIconUpdated();
#endif
#if defined(PLAYER_WEATHER_PYRAMID)
          st7789_player_weather::notifyUpdated();
          if (_mode == PLAYER && player.isRunning() && config.store.showweather) {
            if (_weather) _weather->lock(true);
            st7789SyncPlayerWeatherSlot(true, config.store.vumeter && _vuwidget);
            st7789_player_weather::redraw(true);
          } else if (_mode == PLAYER && !player.isRunning() && config.store.showweather && _weather) {
            /* STOP: кормим бегущую строку как сток. */
            st7789LeavePlayerWeatherPlay();
            _weather->unlock();
            if (timekeeper.weatherBuf) _weather->setText(timekeeper.weatherBuf);
          }
#endif
#if DSP_MODEL==DSP_ST7789_170 || DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || defined(AXS15231B_COVER_SS)
          if (_mode == SCREENBLANK && player.isRunning() && isDisplayCoversEnabled()) {
            putRequest(CLOCK, 1);
          }
          #ifndef HIDE_IP
          if(_volip) _volip->setText(config.ipToStr(WiFi.localIP()), iptxtFmt);
          #endif
#if DSP_MODEL==DSP_ST7789_170 || (DSP_MODEL==DSP_ST7789 && !ST7789_SPECTRUM_ENABLE) || (defined(ILI9488_LANDSCAPE_COVER_SS) && !defined(PLAYER_WEATHER_PYRAMID))
          if(_weather && timekeeper.weatherBuf) _weather->setText(timekeeper.weatherBuf);
#endif
#elif DSP_MODEL==DSP_NV3041A
          notifyNv3041SsWeatherUpdated();
          if (_mode == SCREENSAVER) putRequest(CLOCK, 1);
          if(_weather && timekeeper.weatherBuf) _weather->setText(timekeeper.weatherBuf);
#else
          /* AXS без Cover-SS: при пирамидке PLAY уже выше; иначе сток-скролл. */
#if !defined(PLAYER_WEATHER_PYRAMID)
          if(_weather && timekeeper.weatherBuf) _weather->setText(timekeeper.weatherBuf);
#endif
#endif
          break;
        }
        case BOOTSTRING: {
          if(_bootstring) _bootstring->setText(config.ssids[request.payload].ssid, LANG::bootstrFmt);
          /*#ifdef USE_NEXTION
            char buf[50];
            snprintf(buf, 50, bootstrFmt, config.ssids[request.payload].ssid);
            nextion.bootString(buf);
          #endif*/
          break;
        }
        case WAITFORSD: {
          if(_bootstring) _bootstring->setText(LANG::const_waitForSD);
          break;
        }
        case SDFILEINDEX: {
          if(_mode == SDCHANGE) _nums->setText(request.payload, "%d");
          break;
        }
        case DSPRSSI:
          if(_rssi){ _setRSSI(request.payload); }
          if (_heapbar && config.store.audioinfo) {
            const uint32_t bufFilled = player.isRunning() ? player.inBufferFilled() : 0;
            _heapbar->setMax(getPlayerBufferMaxForUi());
            _heapbar->setColor(getBufferColorForDisplay(bufFilled));
            _heapbar->setValue(bufFilled);
          }
          // Тот же тик, что NRSSI→#cpubar в вебе; процент из общего семпла (кэш в cpu_load).
          if (_cpubar && config.store.webCpuInfo) {
            const int cpuPct = webui_freertos_cpu_percent();
            _cpubar->setMax(100);
            // Сначала длина, потом цвет: иначе setColor→_draw() заливает СТАРУЮ ширину новым цветом.
            _cpubar->setValue(static_cast<uint32_t>(cpuPct));
            _cpubar->setColor(getCpuColorForDisplay(cpuPct));
          }
          break;
        case PSTART:
#if DSP_MODEL==DSP_ST7701
          yoEepromMarkUiBusy();
#endif
          _layoutChange(true);
          break;
        case PSTOP:
#if DSP_MODEL==DSP_ST7701
          yoEepromMarkUiBusy();
#endif
          _layoutChange(false);
          break;
        case DSP_START: _start();  break;
        case NEWIP: {
          #ifndef HIDE_IP
            if(_volip) _volip->setText(config.ipToStr(WiFi.localIP()), iptxtFmt);
          #endif
          break;
        }
        default: break;

        // check if there are more messages waiting in the Q, in this case break the loop() and go
        // for another round to evict next message, do not waste time to redraw the screen, etc...
        if (uxQueueMessagesWaiting(displayQueue))
          return;
      }
  }

#if DSP_MODEL==DSP_ST7789 || defined(ILI9488_LANDSCAPE_COVER_SS) || defined(AXS15231B_COVER_SS)
  /* Cover-SS: бегущая строка факта крутится чаще, чем CLOCK (~1 Гц). */
  if (ST7789_COV::isPlayingBlankCoverActive()) {
    ST7789_COV::tickFactMarquee(false);
    ST7789_COV::tickWeatherCityMarquee();
  }
#if defined(AXS15231B_COVER_SS)
  else if (_mode == PLAYER) {
    /* Faster: та же tickFactMarquee → playerFactConf (Y=93), без отдельного API. */
    ST7789_COV::tickFactMarquee(false);
  }
#endif
#endif
#if defined(PLAYER_WEATHER_PYRAMID)
  /* PLAYER: бегущая строка города в пирамидке погоды. */
  if (st7789_player_weather::isActive()) {
    st7789_player_weather::tickCityMarquee();
  }
#endif

#if DSP_MODEL==DSP_AXS15231B
  /* Flush после pager, очереди (CLOCK/Cover-SS) и marquee — иначе JPEG/лого в canvas не на панели. */
  axsPresentFrame();
#endif

  dsp.loop();
/*
  #if I2S_DOUT==255
  player.computeVUlevel();
  #endif
*/
}

void Display::_setRSSI(int rssi) {
  if(!_rssi) return;
#if RSSI_DIGIT
  _rssi->setText(rssi, rssiFmt);
  return;
#endif
  char rssiG[3];
  int rssi_steps[] = {RSSI_STEPS};
  if(rssi >= rssi_steps[0]) strlcpy(rssiG, "\004\006", 3);
  if(rssi >= rssi_steps[1] && rssi < rssi_steps[0]) strlcpy(rssiG, "\004\005", 3);
  if(rssi >= rssi_steps[2] && rssi < rssi_steps[1]) strlcpy(rssiG, "\004\002", 3);
  if(rssi >= rssi_steps[3] && rssi < rssi_steps[2]) strlcpy(rssiG, "\003\002", 3);
  if(rssi <  rssi_steps[3] || rssi >=  0) strlcpy(rssiG, "\001\002", 3);
  _rssi->setText(rssiG);
}

void Display::_station() {
  _meta->setAlign(metaConf.widget.align);
  _meta->setText(config.station.name);
/*#ifdef USE_NEXTION
  nextion.newNameset(config.station.name);
  nextion.bitrate(config.station.bitrate);
  nextion.bitratePic(ICON_NA);
#endif*/
}

char *split(char *str, const char *delim) {
  char *dmp = strstr(str, delim);
  if (dmp == NULL) return NULL;
  *dmp = '\0'; 
  return dmp + strlen(delim);
}

void Display::_title() {
  // До _bootStep>=2 виджеты title ещё не из _buildPager() — setText давал StoreProhibited на SD-boot.
  if (_bootStep < 2 || !_title1) return;

  // Снимок title в локальный буфер (не VLA) — config.station.title может меняться во время отрисовки.
  char titleSnapshot[BUFLEN];
  memset(titleSnapshot, 0, sizeof(titleSnapshot));
  strlcpy(titleSnapshot, config.station.title, sizeof(titleSnapshot));

  if (titleSnapshot[0] != '\0') {
    // Split "Artist - Title" только на копии, не трогая config.station.title.
    char splitBuf[BUFLEN];
    memset(splitBuf, 0, sizeof(splitBuf));
    strlcpy(splitBuf, titleSnapshot, sizeof(splitBuf));

    char *stitle = split(splitBuf, " - ");
    if(stitle && _title2){
      _title1->setText(splitBuf);
      _title2->setText(stitle);
    }else{
      _title1->setText(titleSnapshot);
      if(_title2) _title2->setText("");
    }
    /*#ifdef USE_NEXTION
      nextion.newTitle(titleSnapshot);
    #endif*/
  }else{
    _title1->setText("");
    if(_title2) _title2->setText("");
  }
  if (player_on_track_change) player_on_track_change();
  pluginsManager::getInstance().on_track_change();
#if defined(HAS_COVER_BLANK_SS)
  /* Смена трека в playing cover-скринсейвере — перерисовать обложку. */
  if (_mode == SCREENBLANK && player.isRunning() && isDisplayCoversEnabled()) {
    putRequest(CLOCK, 1);
  }
#endif
}

void Display::_time(bool redraw) {
  
#if LIGHT_SENSOR!=255
  if(config.store.dspon) {
    config.store.brightness = AUTOBACKLIGHT(analogRead(LIGHT_SENSOR));
    config.setBrightness();
  }
#endif
  // NV3041A: скринсейвер в DspCore::printClock().
  #if DSP_MODEL==DSP_NV3041A
  #elif DSP_MODEL==DSP_ST7789_170
  /* ST7789_170: смещение часов по Y каждые 20 с (как в yoRadio), без выхода за экран. */
  /* ST7789_170: при каждом "прыжке" часов в скринсейвере меняем цвет случайно из заданной палитры.
     Логика безопасная:
     1) один раз сохраняем базовые цвета темы (clock/seconds);
     2) в момент сдвига часов выбираем следующий цвет;
     3) при выходе из скринсейвера возвращаем исходные цвета темы. */
  static bool st170BaseColorsCaptured = false;
  static uint16_t st170BaseClockColor = 0;
  static uint16_t st170BaseSecondsColor = 0;
  static bool st170ScreensaverColorApplied = false;
  static uint16_t st170LastAppliedClockColor = 0;

  /* Палитра: текущий цвет темы + ярко-зелёный, красный, ярко-синий, фиолетовый, жёлтый, белый, золотистый. */
  static const uint16_t st170ClockPalette[] = {
    0,      // специальный элемент: "текущий базовый цвет темы"
    0x07E0, // ярко-зелёный
    0xF800, // красный
    0x001F, // ярко-синий
    0xF81F, // фиолетовый (magenta)
    0xFFE0, // жёлтый
    0xFFFF, // белый
    0xFEA0  // золотистый (RGB565 для #FFD700)
  };

  // Фиксируем исходные цвета темы один раз, чтобы корректно восстановить их после скринсейвера.
  if(!st170BaseColorsCaptured){
    st170BaseClockColor = config.theme.clock;
    st170BaseSecondsColor = config.theme.seconds;
    st170LastAppliedClockColor = config.theme.clock;
    st170BaseColorsCaptured = true;
  }

  if(config.isScreensaver && display.mode()==SCREENSAVER){
    static uint32_t lastSsMoveMs = 0;
    if(lastSsMoveMs == 0) lastSsMoveMs = millis();
    if(millis() - lastSsMoveMs >= 20000UL){
      lastSsMoveMs = millis();

      // Выбор случайного цвета именно в момент "прыжка" часов.
      // ВАЖНО: цвет должен реально МЕНЯТЬСЯ каждый прыжок.
      // Поэтому не допускаем повтор текущего уже применённого цвета.
      const uint8_t st170PaletteCount = sizeof(st170ClockPalette) / sizeof(st170ClockPalette[0]);
      uint16_t nextClockColor = st170LastAppliedClockColor;
      for(uint8_t attempt = 0; attempt < 8; ++attempt){
        uint8_t colorIndex = static_cast<uint8_t>(random(0, st170PaletteCount));
        uint16_t candidateColor = (colorIndex == 0) ? st170BaseClockColor : st170ClockPalette[colorIndex];
        if(candidateColor != st170LastAppliedClockColor){
          nextClockColor = candidateColor;
          break;
        }
      }
      // Резервный безопасный сдвиг: если вдруг рандом несколько раз подряд дал тот же цвет,
      // принудительно берём ярко-зелёный (либо базовый, если он не совпадает).
      if(nextClockColor == st170LastAppliedClockColor){
        nextClockColor = (st170BaseClockColor != st170LastAppliedClockColor) ? st170BaseClockColor : 0x07E0;
      }

      // Применяем один цвет и к HH:MM, и к секундам — визуально выглядит цельно и читаемо.
      config.theme.clock = nextClockColor;
      config.theme.seconds = nextClockColor;
      st170LastAppliedClockColor = nextClockColor;
      st170ScreensaverColorApplied = true;

      uint16_t ftMin = ST170_CLOCK_MIN_TOP;
      uint16_t ftMax = ST170_CLOCK_MAX_TOP;
      if(ftMax > ftMin){
        uint16_t ft = (uint16_t)random(ftMin, ftMax + 1);
        _clock->moveTo({clockConf.left, ft, 0});
      }
    }
  }else{
    // После выхода из скринсейвера обязательно возвращаем стандартные цвета темы.
    if(st170ScreensaverColorApplied){
      config.theme.clock = st170BaseClockColor;
      config.theme.seconds = st170BaseSecondsColor;
      st170LastAppliedClockColor = st170BaseClockColor;
      st170ScreensaverColorApplied = false;
      redraw = true;  // перерисовать часы сразу с базовым цветом темы, не ждать следующего тика
    }
  }
  #elif DSP_MODEL==DSP_ST7789
  /* JC2432 ST7789 320×240: прыжок часов раз в минуту (как сток) + смена цвета как на _170. */
  static bool st7789BaseColorsCaptured = false;
  static uint16_t st7789BaseClockColor = 0;
  static uint16_t st7789BaseSecondsColor = 0;
  static bool st7789ScreensaverColorApplied = false;
  static uint16_t st7789LastAppliedClockColor = 0;
  static const uint16_t st7789ClockPalette[] = {
    0, 0x07E0, 0xF800, 0x001F, 0xF81F, 0xFFE0, 0xFFFF, 0xFEA0
  };
  if(!st7789BaseColorsCaptured){
    st7789BaseClockColor = config.theme.clock;
    st7789BaseSecondsColor = config.theme.seconds;
    st7789LastAppliedClockColor = config.theme.clock;
    st7789BaseColorsCaptured = true;
  }
  if(config.isScreensaver && display.mode()==SCREENSAVER){
    /* Один прыжок на минуту (tm_sec==0 иначе мог бы сработать несколько раз за секунду). */
    static int8_t st7789LastJumpMin = -1;
    if(network.timeinfo.tm_sec == 0 && network.timeinfo.tm_min != st7789LastJumpMin){
      st7789LastJumpMin = network.timeinfo.tm_min;
      /* Случайный цвет при прыжке — без повтора предыдущего. */
      const uint8_t palCnt = sizeof(st7789ClockPalette) / sizeof(st7789ClockPalette[0]);
      uint16_t nextClockColor = st7789LastAppliedClockColor;
      for(uint8_t attempt = 0; attempt < 8; ++attempt){
        uint8_t colorIndex = static_cast<uint8_t>(random(0, palCnt));
        uint16_t candidateColor = (colorIndex == 0) ? st7789BaseClockColor : st7789ClockPalette[colorIndex];
        if(candidateColor != st7789LastAppliedClockColor){
          nextClockColor = candidateColor;
          break;
        }
      }
      if(nextClockColor == st7789LastAppliedClockColor){
        nextClockColor = (st7789BaseClockColor != st7789LastAppliedClockColor) ? st7789BaseClockColor : 0x07E0;
      }
      config.theme.clock = nextClockColor;
      config.theme.seconds = nextClockColor;
      st7789LastAppliedClockColor = nextClockColor;
      st7789ScreensaverColorApplied = true;

      #if TIME_SIZE<19
        uint16_t ft=static_cast<uint16_t>(random(TFT_FRAMEWDT, (dsp.height()-TIME_SIZE*CHARHEIGHT-TFT_FRAMEWDT)));
      #else
        uint16_t ft=static_cast<uint16_t>(random(TFT_FRAMEWDT+TIME_SIZE, (dsp.height()-_clock->dateSize()-TFT_FRAMEWDT*2)));
      #endif
      uint16_t lt=static_cast<uint16_t>(random(TFT_FRAMEWDT, (dsp.width()-_clock->clockWidth()-TFT_FRAMEWDT)));
      if(clockConf.align==WA_CENTER) lt-=(dsp.width()-_clock->clockWidth())/2;
      _clock->moveTo({lt, ft, 0});
    }
  }else{
    if(st7789ScreensaverColorApplied){
      config.theme.clock = st7789BaseClockColor;
      config.theme.seconds = st7789BaseSecondsColor;
      st7789LastAppliedClockColor = st7789BaseClockColor;
      st7789ScreensaverColorApplied = false;
      redraw = true;
    }
  }
  #elif DSP_MODEL==DSP_AXS15231B || DSP_MODEL==DSP_ST7701
  /* Faster AXS / Supremus ST7701: прыжок SS каждые 20 с + смена цвета.
   * На SS — DIGI68, только HH:MM; 3 прыжка за ~60 с. */
  static bool axsBaseColorsCaptured = false;
  static uint16_t axsBaseClockColor = 0;
  static uint16_t axsBaseSecondsColor = 0;
  static bool axsScreensaverColorApplied = false;
  static uint16_t axsLastAppliedClockColor = 0;
  static const uint16_t axsClockPalette[] = {
    0, 0x07E0, 0xF800, 0x001F, 0xF81F, 0xFFE0, 0xFFFF, 0xFEA0
  };
  if (!axsBaseColorsCaptured) {
    axsBaseClockColor = config.theme.clock;
    axsBaseSecondsColor = config.theme.seconds;
    axsLastAppliedClockColor = config.theme.clock;
    axsBaseColorsCaptured = true;
  }
  if (config.isScreensaver && display.mode() == SCREENSAVER) {
    static uint32_t axsLastSsMoveMs = 0;
    if (axsLastSsMoveMs == 0) axsLastSsMoveMs = millis();
    if (millis() - axsLastSsMoveMs >= 20000UL) {
      axsLastSsMoveMs = millis();
      const uint8_t palCnt = sizeof(axsClockPalette) / sizeof(axsClockPalette[0]);
      uint16_t nextClockColor = axsLastAppliedClockColor;
      for (uint8_t attempt = 0; attempt < 8; ++attempt) {
        uint8_t colorIndex = static_cast<uint8_t>(random(0, palCnt));
        uint16_t candidateColor = (colorIndex == 0) ? axsBaseClockColor : axsClockPalette[colorIndex];
        if (candidateColor != axsLastAppliedClockColor) {
          nextClockColor = candidateColor;
          break;
        }
      }
      if (nextClockColor == axsLastAppliedClockColor) {
        nextClockColor = (axsBaseClockColor != axsLastAppliedClockColor) ? axsBaseClockColor : 0x07E0;
      }
      config.theme.clock = nextClockColor;
      config.theme.seconds = nextClockColor;
      axsLastAppliedClockColor = nextClockColor;
      axsScreensaverColorApplied = true;

      /* SS DIGI68: «88:88» ≈293 px, глиф ~85 px над baseline — прыжок в пределах экрана. */
      const uint16_t ssClkW = 300;
      const uint16_t ssGlyphAbove = 85;
      const uint16_t ftMin = (uint16_t)(TFT_FRAMEWDT + ssGlyphAbove);
      const uint16_t ftMax = (dsp.height() > TFT_FRAMEWDT + 2)
                                 ? (uint16_t)(dsp.height() - TFT_FRAMEWDT) : ftMin;
      uint16_t ft = (ftMax > ftMin) ? (uint16_t)random(ftMin, ftMax + 1) : ftMin;
      const uint16_t ltMin = (uint16_t)TFT_FRAMEWDT;
      const uint16_t ltMax = (dsp.width() > ssClkW + TFT_FRAMEWDT)
                                 ? (uint16_t)(dsp.width() - ssClkW - TFT_FRAMEWDT) : ltMin;
      uint16_t lt = (ltMax > ltMin) ? (uint16_t)random(ltMin, ltMax + 1) : ltMin;
      _clock->moveTo({lt, ft, 0});
    }
  } else {
    if (axsScreensaverColorApplied) {
      config.theme.clock = axsBaseClockColor;
      config.theme.seconds = axsBaseSecondsColor;
      axsLastAppliedClockColor = axsBaseClockColor;
      axsScreensaverColorApplied = false;
      redraw = true;
    }
  }
  #else
  if(config.isScreensaver && network.timeinfo.tm_sec % 60 == 0){
    #if TIME_SIZE<19
      uint16_t ft=static_cast<uint16_t>(random(TFT_FRAMEWDT, (dsp.height()-TIME_SIZE*CHARHEIGHT-TFT_FRAMEWDT)));
    #else
      uint16_t ft=static_cast<uint16_t>(random(TFT_FRAMEWDT+TIME_SIZE, (dsp.height()-_clock->dateSize()-TFT_FRAMEWDT*2)));
    #endif
    uint16_t lt=static_cast<uint16_t>(random(TFT_FRAMEWDT, (dsp.width()-_clock->clockWidth()-TFT_FRAMEWDT)));
    if(clockConf.align==WA_CENTER) lt-=(dsp.width()-_clock->clockWidth())/2;
    _clock->moveTo({lt, ft, 0});
  }
  #endif
  // Для NV3041A миганием точек и перемещением часов управляет сама DspCore::printClock.
  // Здесь используем обычный redraw: printClock всё равно вызывается раз в секунду из
  // дисплейной задачи, а _clockSeconds() внутри него мигает двоеточием по millis().
  // Цифры при этом перерисовываются только при смене минуты или прыжке скринсейвера.
  _clock->draw(redraw);
#if defined(PLAYER_WEATHER_PYRAMID)
  /* Часы при PLAY затирают слот погоды слева — восстанавливаем статичный блок. */
  if (_mode == PLAYER && st7789_player_weather::isActive()) {
    st7789_player_weather::redraw(true);
  }
#endif
#if DSP_MODEL==DSP_ST7789_170
  /* Часы clearClock затирают правую часть строки IP (Y=148) — перерисовка после часов, как в yoRadio по порядку footer. */
  #ifndef HIDE_IP
  if(_volip && _mode == PLAYER && !config.isScreensaver && !_volip->locked()) {
    _volip->setActive(true);
  }
  #endif
#endif
#if DSP_MODEL==DSP_AXS15231B
  /* Часы (clear + дата) могут задеть футер — восстановить громкость/IP/RSSI после тика. */
  if (_mode == PLAYER && !config.isScreensaver) {
#ifndef HIDE_VOL
    if (_voltxt && !_voltxt->locked()) _voltxt->setActive(true);
#endif
#ifndef HIDE_IP
    if (_volip && !_volip->locked()) _volip->setActive(true);
#endif
    if (_rssi && !_rssi->locked()) _rssi->setActive(true);
#ifndef HIDE_VOLBAR
    if (_volbar && !_volbar->locked()) _volbar->setActive(true);
#endif
  }
#endif
  /*#ifdef USE_NEXTION
    nextion.printClock(network.timeinfo);
  #endif*/
}

void Display::_volume() {
  // Конвертируем внутреннее значение 0-254 в процент 0-100 для отображения
  uint8_t volPercent = (config.store.volume * 100) / 254;
  static uint8_t s_lastVolUiPercent = 255;
  
  if(_volbar) _volbar->setValue(volPercent);  // Прогресс-бар 0-100%
  #ifndef HIDE_VOL
    if(_voltxt) _voltxt->setText(volPercent, voltxtFmt);  // Текст 0-100%
  #endif
  if(_mode==VOL) {
    _nums->setText(volPercent, numtxtFmt);  // Числовое отображение 0-100%
    /* Как DRAWPLAYLIST: каждый апдейт громкости на VOL продлевает 3 с до PLAYER. */
    timekeeper.waitAndReturnPlayer(3);
    s_lastVolUiPercent = volPercent;
  } else {
    s_lastVolUiPercent = 255;
  }
  /*#ifdef USE_NEXTION
    nextion.setVol(config.store.volume, _mode == VOL);
  #endif*/
}

void Display::flip(){ dsp.flip(); }

void Display::invert(){ dsp.invert(); }

void  Display::setContrast(){
  #if DSP_MODEL==DSP_NOKIA5110
    dsp.setContrast(config.store.contrast);
  #endif
}

bool Display::deepsleep(){
  // Стоп display task loop до SPI sleep — иначе VuWidget::_draw() конфликтует с dsp.sleep().
  _sleeping = true;
#if DSP_MODEL==DSP_ST7701
  /* Перед сном — сбросить dirty на flash (SmartStart / громкость). */
  yoEepromCommitNow();
#endif
  vTaskDelay(pdMS_TO_TICKS(50));  // Даём текущему циклу _draw() завершиться
#if defined(LCD_I2C) || defined(DSP_OLED) || BRIGHTNESS_PIN!=255
  dsp.sleep();
  return true;
#elif DSP_MODEL==DSP_NV3041A
  dsp.sleep();
  return true;
#endif
  return false;
}

void Display::wakeup(){
#if defined(LCD_I2C) || defined(DSP_OLED) || BRIGHTNESS_PIN!=255
  dsp.wake();
#elif DSP_MODEL==DSP_NV3041A
  dsp.wake();
#endif
  _sleeping = false;
}
//============================================================================================================================
#else // !DUMMYDISPLAY
//============================================================================================================================
void Display::init(){
  _createDspTask();
  #ifdef USE_NEXTION
  nextion.begin(true);
  #endif
}
void Display::_start(){
  #ifdef USE_NEXTION
  //nextion.putcmd("page player");
  nextion.start();
  #endif
  config.setTitle(LANG::const_PlReady);
}

void Display::putRequest(displayRequestType_e type, int payload){
  if(type==DSP_START) _start();
  #ifdef USE_NEXTION
    requestParams_t request;
    request.type = type;
    request.payload = payload;
    nextion.putRequest(request);
  #else
    if(type==NEWMODE) mode((displayMode_e)payload);
  #endif
}
//============================================================================================================================
#endif // DUMMYDISPLAY
