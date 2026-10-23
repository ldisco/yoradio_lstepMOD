/****************************************************************************************************
  УНИВЕРСАЛЬНЫЙ шаблон myoptions — опции lstepMOD с пояснениями.

  Сборка читает только:  src/myoptions.h
  Этот файл:             src/myoptions_universal.h
  Профиль проще собрать: https://lstep.site/myoptions/ → сохранить как src/myoptions.h

  Как использовать:
    1. Скопируйте нужное в src/myoptions.h (или возьмите вывод генератора).
    2. Один DSP_MODEL; остальные блоки дисплея — закомментировать.
    3. GPIO под свою разводку; 255 = «не используется».
    4. Прошивка + LittleFS.

  Пример ниже: ST7789_170 + PCM5102 + SD + BT + энкодер 2 (типовой ESP32-S3 DevKit).
****************************************************************************************************/

#ifndef myoptions_h
#define myoptions_h

/* ================================================================================================
 * WiFi — только WIFI_ATTEMPTS реально читается прошивкой (network.cpp → wifiBegin).
 * Одна «попытка» = 500 мс ожидания WL_CONNECTED; при boot на lastSSID лимит = WIFI_ATTEMPTS×2.
 * WIFI_RECONNECT_DELAY / WIFI_CONNECT_TIMEOUT в коде НЕ используются (устаревшие строки из старых myoptions).
 * ================================================================================================ */
#define WIFI_ATTEMPTS 16              /* 8–24: меньше — быстрее уход в AP; больше — дольше ждёт роутер */

/* ================================================================================================
 * Интерфейс TFT / WebUI
 * ================================================================================================ */
//#define DSP_INVERT_TITLE  false       /* Цвета заголовка станции; не влияет на DSP_DUMMY */
#define L10N_LANGUAGE RU              /* RU или EN — надписи на TFT и часть WebUI */
/* Имя устройства в WebUI (title) и на TFT «Имя: версия». Логотип ёRadio и SSID AP не меняются.
 * По умолчанию в options.h — "yoradio". Пример: #define DEVICE_NAME "Кухня" */
//#define DEVICE_NAME "yoradio"

//#define EXT_WEATHER       false     /* Расширенная погода на TFT; не для DSP_DUMMY */
//#define CLOCKFONT_MONO    false     /* Моношрифт часов; только TFT */
//#define RSSI_DIGIT        true      /* Wi‑Fi: цифры вместо «лесенки»; только TFT */
//#define BITRATE_FULL      false     /* Значок битрейта; только TFT */

/* ================================================================================================
 * Диагностика Serial / Telnet
 * ================================================================================================ */
#define AUDIO_STREAM_DIAG 0           /* 1 — [DIAG] раз в 5 с (Audio.cpp); 0 — выкл. */
#define AUDIO_READ_HEAP_GUARD_ENABLE false  /* false — стабильнее WEB-поток (Audio.cpp) */
//#define AUDIO_READ_HEAP_GUARD_LOG 1   /* Лог [HEAP-GUARD]; не влияет на саму защиту */
//#define FACTS_TASK_CORE_ID 1          /* Ядро FactsTask: 0 (дефолт) или 1; TrackFacts.cpp */
#define TEMP_DIAG_ENABLE false        /* true — [TEMP] °C в Serial ~раз в 10 с (кристалл SoC, не радиатор) */

/* ================================================================================================
 * WebUI
 * ================================================================================================ */
#define WEBUI_COVERS_ENABLE true      /* Обложки в браузере; работает и при DSP_DUMMY */
#define WEBUI_CPU_BAR_ENABLE 1        /* Полоска CPU; переключатель «CPU Info» в Settings */
#define SSL_BLOCK_LOG_ENABLE false    /* true — лог блокировок HTTPS в Serial */

#define MIN_HEAP_FOR_SSL_FACTS 44000        /* config.cpp — порог heap для TrackFacts */
#define MIN_HEAP_FOR_SSL_COVERS 44000       /* config.cpp — порог heap для обложек HTTPS */
#define MIN_MAX_ALLOC_HEAP_FOR_SSL 30000    /* config.cpp — мин. непрерывный блок кучи */
#define MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT 6  /* TrackFacts.cpp — % буфера перед SSL */
#define MIN_AUDIO_BUFFER_FOR_SSL_COVERS_PERCENT 13 /* CoverDL / options.h дефолт — % буфера перед HTTPS обложки */

/* ================================================================================================
 * Обложки JPEG / TrackFacts
 * DISPLAY_COVERS_ENABLE — загрузка и TFT; при DSP_DUMMY игнорируется отрисовка.
 * COVER_* — не используются на TFT при DISPLAY_COVERS_ENABLE false.
 * ================================================================================================ */
#define DISPLAY_COVERS_ENABLE true      /* ST7789_170: JPEG только в скринсейвере cover; NV3041/9488 — в плеере */
#define COVER_DOWNLOAD_DELAY_MS 2000    /* config.cpp — задержка перед HTTPS обложки */
#define DISPLAY_COVER_REFRESH_MS 1500   /* displayNV3041A/ILI9488 — период обновления слота; не для ST7789_170 в плеере */
#define COVER_CACHE_MAX_BYTES (2 * 1024 * 1024)   /* Лимит папки кэша обложек на LittleFS (2 МБ).
                                                     * Это НЕ вся флеш: N16R8 = 16 МБ flash, раздел LittleFS ≈ 10,7 МБ
                                                     * (partitions.csv). PSRAM 8 МБ — оперативная память, не для файлов.
                                                     * Отдельно FACTS_CACHE + WebUI + playlist; оставьте запас (LITTLEFS_MIN_FREE_BYTES). */
#define FACTS_CACHE_MAX_BYTES (2 * 1024 * 1024)   /* Лимит папки кэша фактов TrackFacts (2 МБ); не суммируется с COVER в один файл */

#ifndef TRACKFACTS_MAX_PER_TRACK
#define TRACKFACTS_MAX_PER_TRACK 5      /* Внутренний лимит AI-циклов */
#endif
#ifndef TRACKFACTS_CONFIG_COUNT_MAX
#define TRACKFACTS_CONFIG_COUNT_MAX 3   /* Макс. фактов в WebUI/EEPROM */
#endif

#define STATION_SWITCH_SSL_BLOCK_MS 20000       /* config.cpp — пауза HTTPS после смены станции (мс); профили часто 12000 */
#define TRACKFACTS_AUTO_FETCH_ENABLE false      /* общий рубильник авто-фактов; false = только вручную (WebUI/свайп) */
#define TRACKFACTS_AUTO_GEMINI_ENABLE true      /* при авто ON: Gemini; иначе только ручной запрос */
#define TRACKFACTS_AUTO_DEEPSEEK_ENABLE false   /* DeepSeek; обычно один AI-провайдер */
#define TRACKFACTS_AUTO_GROQ_ENABLE false       /* Groq; вкл. только явным true */
#define TRACKFACTS_AUTO_ITUNES_ENABLE true      /* бесплатно */
#define TRACKFACTS_AUTO_LASTFM_ENABLE true      /* бесплатно */

/* ================================================================================================
 * Sleep Timer (плагин SleepTimer.cpp)
 * ================================================================================================ */
#define SLEEP_TIMER_PIN 40            /* GPIO реле/MOSFET питания; 255 — только софт-стоп без обесточивания */
// #define SLEEP_TIMER_LEVEL LOW       /* Уровень на SLEEP_TIMER_PIN в режиме «питание выключено»: LOW или HIGH */
#define SLEEP_DEEPSLEEP_WAKE_HOURS 8  /* Часы до автопробуждения из deepSleep, если нет тача и WAKE_PIN (3…8) */

/* ================================================================================================
 * DLNA — в myoptions: #define USE_DLNA (не флаг platformio.ini)
 * ================================================================================================ */
//#define USE_DLNA
#ifndef DLNA_HOST_DEFAULT
#define DLNA_HOST_DEFAULT ""          /* пусто = выбрать в WebUI */
#endif
#ifndef DLNA_IDX_DEFAULT
#define DLNA_IDX_DEFAULT 0            /* индекс контейнера */
#endif

/* ================================================================================================
 * ЦАП ES9038Q2M (опционально): пишите блок только если нужен ES9038.
 * Нет макроса = выкл. (дефолт в options.h). PCM5102/другие I2S — без этих строк.
 * USE_ES9038_DAC — main.cpp, es9038q2m_init.cpp; пины EQ ≠ I2S_DOUT/BCLK/LRC
 * ================================================================================================ */
/* #define USE_ES9038_DAC true */
/* #define EQ_SDA_PIN 16 */           /* Soft I2C SDA → ES9038 */
/* #define EQ_SCL_PIN 15 */           /* Soft I2C SCL → ES9038 */

#define FM_PASSWORD "Adm1nUp2"        /* FsInfo.cpp — файловый менеджер; закомментируйте — без пароля */

/* ================================================================================================
 * ДИСПЛЕЙ — выберите ОДИН DSP_MODEL (список в options.h)
 *
 * Тема (options.h, после объявления DSP_*):
 *   DSP_ST7789_170  → mytheme_st7789_170.h (yoRadio, src/mytheme.h не читается)
 *   любой другой    → src/mytheme.h пользователя (COLOR_* в корне sketch)
 *
 * Альтернативы (закомментированы):
 *   DSP_DUMMY, DSP_NV3041A, DSP_ILI9488, DSP_ST7789, DSP_ST7735, DSP_ILI9341, …
 * ================================================================================================ */

/* --- ST7789 320×170 (SPI) — АКТИВНЫЙ пример --- */
#define DSP_MODEL         DSP_ST7789_170
#define TFT_CS   10                   /* SPI Chip Select */
#define TFT_RST  -1                   /* -1 = reset через EN ESP */
#define TFT_DC    9                   /* Data/Command */
#define BRIGHTNESS_PIN  3             /* Подсветка PWM; 255 — нет; не для DSP_DUMMY */
#define DSP_HSPI   false              /* false — FSPI (MOSI 11, SCLK 12 на S3) */
#define DSP_FLIPSCREEN_DEFAULT true   /* false — без переворота при первой прошивке; true — сразу «перевёрнуто»;
                                         дальше ориентация в EEPROM, меняется из WebUI/энкодера */
/* MOSI → 11, SCLK → 12 */

/* --- ILI9488 / ILI9486 3.5" SPI portrait 320×480 ---
//#define DSP_MODEL         DSP_ILI9488
//#define ILI9488_PORTRAIT  true        // Обязательно true для portrait conf
//#define TFT_CS   10
//#define TFT_RST  -1
//#define TFT_DC    9
//#define BRIGHTNESS_PIN  3
//#define DSP_HSPI   false
//#define DSP_FLIPSCREEN_DEFAULT false  // false — без переворота; true — перевёрнуто
*/

/* --- NV3041A 480×272 FPIO (JC4827W543) ---
//#define DSP_MODEL         DSP_NV3041A
//#define TFT_CS   45
//#define TFT_RST  -1
//#define TFT_SCK  47
//#define TFT_D0   21
//#define TFT_D1   48
//#define TFT_D2   40
//#define TFT_D3   39
//#define TFT_DC   255                  /* 255 — нет отдельного DC на FPIO */
//#define GFX_BL   1                    /* Подсветка NV3041A; вместо BRIGHTNESS_PIN */
//#define DSP_FLIPSCREEN_DEFAULT false  // false — без переворота; true — перевёрнуто
*/

/* --- ST7735 (нужен DTYPE) ---
//#define DSP_MODEL    DSP_ST7735
//#define DTYPE        INITR_BLACKTAB   /* INITR_GREENTAB, INITR_144GREENTAB, … */
//#define TFT_CS  5
//#define TFT_DC  4
*/

/* --- Без дисплея ---
//#define DSP_MODEL         DSP_DUMMY
   DSP_DUMMY: нет TFT/тача/SCROLL/BATTERY на экране; WebUI + Telnet остаются.
*/

/* ================================================================================================
 * Сенсорный экран — только при DSP_MODEL != DSP_DUMMY
 * GT911 — I2C (NV3041A); TS_CS — резистивный SPI
 * ================================================================================================ */
//#define TS_MODEL   TS_MODEL_GT911     /* touchscreen.cpp */
//#define TS_SDA     8
//#define TS_SCL     4
//#define TS_INT     3
//#define TS_RST     38
//#define TS_CS      14                 /* Резистивный SPI-тач */
//#define TS_HSPI    false

/* ================================================================================================
 * I2S → PCM5102 (при USE_ES9038_DAC false)
 * ================================================================================================ */
#if !USE_ES9038_DAC
  #define I2S_DOUT      7             /* DIN → PCM5102 */
  #define I2S_BCLK      16            /* BCK */
  #define I2S_LRC       15            /* LCK / WS */
#endif

/* ================================================================================================
 * I2S → ES9038 (при USE_ES9038_DAC true; пины ≠ EQ_SDA/EQ_SCL)
 * ================================================================================================ */
#if USE_ES9038_DAC
  #define I2S_DOUT      7
  #define I2S_BCLK      5
  #define I2S_LRC       6
#endif

/* ================================================================================================
 * Bluetooth QCC5125 (USE_BT_I2S — bt_i2s.cpp; режим WEB/SD/BT в WebUI)
 * Документация: bt_docs/BT_USER_RU.md, bt_docs/BT_WIRING_RU.md
 * ================================================================================================ */
#define USE_BT_I2S 1                  /* 0 — BT вырезан из прошивки */
#if USE_BT_I2S
  #define BT_I2S_SEL_PIN        2     /* SEL 74HC157; не совмещать с WAKE_PIN */
  #define BT_I2S_SEL_LEVEL_BT   HIGH
  #define BT_I2S_SEL_LEVEL_ESP  LOW   /* Уровень SEL, когда на ЦАП идёт звук с ESP (WEB/SD) */
  #define BT_USE_MCLK           false /* true — линия MCLK (нужна ES9038); для PCM5102 оставьте false */
  //#define I2S_MCLK            11    /* GPIO MCLK; только при BT_USE_MCLK true */
  #define BT_MUTE_STATUS_PIN    255   /* 255 — нет входа MUTE с модуля QCC5125; иначе номер GPIO */
  #define BT_KEY_PLAY_PIN       12    /* Импульс 3.3 V Play/Pause на модуль QCC5125 */
  #define BT_KEY_VOLP_PIN       9     /* Импульс «громче» на модуль */
  #define BT_KEY_VOLM_PIN       10    /* Импульс «тише» на модуль */
#endif

/* ================================================================================================
 * VS1053 — отдельный MP3-чип (вместо I2S DAC; audioVS1053)
 * VS1053_CS=255 — чип отключён (типично при PCM5102/ES9038)
 * ================================================================================================ */
#define VS1053_CS     255             /* 255 — не используется */
//#define VS1053_DCS    18
//#define VS1053_DREQ   16
//#define VS1053_RST    -1
//#define VS_HSPI       false

/* ================================================================================================
 * Кнопки (controls.cpp / OneButton)
 * ================================================================================================ */
//#define BTN_LEFT    6
//#define BTN_CENTER  4
//#define BTN_RIGHT   5
//#define BTN_UP      255
//#define BTN_DOWN    255
//#define BTN_MODE    255
//#define BTN_INTERNALPULLUP    false

/* ================================================================================================
 * Энкодер 1 (yoEncoder; ENC_BTNL+ENC_BTNR обязательны в паре)
 * ================================================================================================ */
//#define ENC_BTNL    4
//#define ENC_BTNR    5
//#define ENC_BTNB    8
#define ENC_INTERNALPULLUP    false   /* true — внутренняя подтяжка ESP на пинах энкодера 1 (ENC_BTNL/R/B) */

/* ================================================================================================
 * Энкодер 2 — включён в примере
 * ================================================================================================ */
#define ENC2_BTNL   4                 /* DT */
#define ENC2_BTNR   5                 /* CLK */
#define ENC2_BTNB   6                 /* Кнопка */
#define ENC2_INTERNALPULLUP   false   /* true — внутренняя подтяжка ESP на пинах энкодера 2 */
//#define ENC2_HALFQUARD  false         /* 4/2/1 шага на щелчок; см. controls.cpp */

/* ================================================================================================
 * SD-карта (WEB ↔ SD ↔ BT; sdmanager)
 * ================================================================================================ */
#define SD_MAX_LEVELS      6          /* Глубина индексации папок */
#define SDC_CS          13            /* CS; не конфликтовать с TFT_CS */
#define SD_SPIPINS      12, 13, 11    /* SCK, MISO, MOSI */
//#define SD_HSPI         false

/* ================================================================================================
 * Поведение
 * ================================================================================================ */
#define NETSERVER_LOOP1               /* main.cpp/display.cpp — WebUI в loop(); рекомендуется для TFT */

#define WAKE_PIN            2         /* Пробуждение из сна; 255 — выкл.; не дублировать с BT_SEL */
//#define MUTE_PIN    2                 /* Аппаратный mute усилителя */
//#define MUTE_VAL    LOW

/* BacklightDown: живые настройки — WebUI/EEPROM (faderon, fadermin %, fadertimeout).
 * DOWN_* — только seed при первой прошивке / сбросе (config.cpp), если заданы.
 * DOWN_LEVEL: шкала 0…255 → fadermin %; DOWN_INTERVAL: секунды до угасания.
 * Без макросов: fader off, 10%, 60 с. Нужен YO_BACKLIGHT_CTRL (BRIGHTNESS_PIN≠255 или AXS/ST7701). */
//#define DOWN_LEVEL           42
//#define DOWN_INTERVAL        60

/* ================================================================================================
 * Аккумулятор на TFT — не для DSP_DUMMY; координаты в display*conf.h
 * ================================================================================================ */
#define BATTERY_OFF                   /* Уберите + ADC_PIN/R1/R2 для показа батареи */
//#define HIDE_VOLT                     /* Скрыть только напряжение */
//Connect ADC_PIN  to    1
//#define R1               38.5
//#define R2               99.2
//#define DELTA_BAT       -0.009

/* ================================================================================================
 * ИК-пульт (controls.cpp; только при IR_PIN != 255)
 * ================================================================================================ */
//#define IR_PIN            17
//#define IR_TIMEOUT        80          /* С IR_PIN; иначе дефолт 80 из options.h */

/* ================================================================================================
 * RTC I2C (опционально)
 * ================================================================================================ */
//#define RTC_MODULE    DS3231          /* или DS1307 */
//#define RTC_SDA       7
//#define RTC_SCL       8

/* ================================================================================================
 * Nextion (опционально; NEXTION_RX/TX в options.h)
 * ================================================================================================ */
//#define NEXTION_RX   255
//#define NEXTION_TX   255

/* ================================================================================================
 * Бегущая строка SCROLL_* — только DSP_NV3041A, ILI9488 portrait, DSP_ST7789_170
 * При DSP_DUMMY и прочих дисплеях — не используется (свои conf/*.h)
 * ================================================================================================ */
#define SCROLL_START_DELAY 5000
#define SCROLL_STEP 4                 /* ST7789_170: 4; NV3041A/9488 portrait: 5 */
#define SCROLL_TIME 30                /* ST7789_170: 30; NV3041A/9488: 40 */
// #define SCROLL_PLAYLIST_START_DELAY 1000  /* В основном NV3041A */
// #define SCROLL_WEATHER_STEP 2
// #define SCROLL_WEATHER_TIME 5

#endif
