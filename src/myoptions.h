/****************************************************************************************************
  СГЕНЕРИРОВАНО: lstepMOD myoptions generator v.0.62
  Плата: ILIK (ILI9488 480×320 + XPT2046)
  Целевой файл: src/myoptions.h
  ----- ЧТО ДЕЛАТЬ ДАЛЬШЕ -----
  1) SAVE TO PROJECT → диалог «Сохранить как» → src/myoptions.h
  2) Соберите:  pio run -e yoradio-esp32s3
  3) LittleFS:  pio run -e yoradio-esp32s3 -t buildfs
  4) Залейте app + LittleFS (upload / uploadfs).
  Примечание env: UART → ARDUINO_USB_CDC_ON_BOOT=0. ЦАП: PCM5102 + MUTE на GPIO4. XPT2046 PENIRQ → GPIO8. FFT landscape ILI9488 (без пирамидки погоды).
****************************************************************************************************/

#ifndef myoptions_h
#define myoptions_h

/* ================================================================================================
 * LANGUAGE / BASIC
 * ================================================================================================ */
#define L10N_LANGUAGE RU
#define DEVICE_NAME "ILIK"
/* DEV_COLOR — акцент имени/UI (TFT + WebUI); hex #RRGGBB */
#define DEV_COLOR "#ec407a"
/* Пароль File Manager WebUI; пустое поле в генераторе = без #define (открытый FM) */
#define FM_PASSWORD "Adm1nUp2"
#define DSP_INVERT_TITLE  false

/* ================================================================================================
 * DISPLAY
 * ================================================================================================ */
#define DSP_MODEL         DSP_ILI9488
#define ILI9488_PORTRAIT  false
#define DISPLAY_COVERS_ENABLE true
/* SPI дисплея (без TFT_SPIPINS): на S3 DevKit MOSI(SDI)=11, SCLK(SCK)=12, MISO(SDO)=13.
 * CS / D/C(DC) / BL — в форме; RST=-1 часто через EN модуля. */
#define TFT_CS   10
#define TFT_RST  -1
#define TFT_DC   9
#define BRIGHTNESS_PIN  14
#define DSP_HSPI   false
#define DSP_FLIPSCREEN_DEFAULT false
/* Визуализация: стоковый VU-метр */
#define ST7789_SPECTRUM_ENABLE false

/* ================================================================================================
 * TOUCH
 * ================================================================================================ */
#define TS_MODEL   TS_MODEL_XPT2046
/* Общая SPI с TFT (без TS_HSPI): на S3 DevKit MOSI(SDI/TDI)=11, SCLK(SCK/TCK)=12, MISO(SDO/TDO)=13 — те же линии, что у дисплея.
 * В форме тача свой только TS_CS (TCS). PEN (T_IRQ / PENIRQ) часто → WAKE_PIN (8). */
#define TS_CS      16
#define TS_HSPI    false

/* ================================================================================================
 * AUDIO
 * ================================================================================================ */
/* AUDIO_STREAM_DIAG: 1 — [DIAG] раз в ~5 с; 0 — выкл. */
#define AUDIO_STREAM_DIAG 0
/* AUDIO_READ_HEAP_GUARD: true — пауза чтения при мало heap; false — непрерывный поток */
#define AUDIO_READ_HEAP_GUARD_ENABLE false
/* I2S → PCM5102 */
#define I2S_DOUT      7
#define I2S_BCLK      5
#define I2S_LRC       6
#define VS1053_CS     255
/* Аппаратный MUTE */
#define MUTE_PIN      4
#define MUTE_VAL      HIGH   /* уровень при stop (звук выкл) */

/* ================================================================================================
 * ENCODERS
 * ================================================================================================ */
#define ENC_BTNL    1
#define ENC_BTNR    2
#define ENC_BTNB    42
#define ENC_INTERNALPULLUP    true
#define ENC2_INTERNALPULLUP   true

/* ================================================================================================
 * SD CARD
 * ================================================================================================ */
#define SD_MAX_LEVELS      6
#define SDC_CS          38
#define SD_SPIPINS      39, 40, 41    /* SCK, MISO, MOSI */
#define SD_HSPI         true

/* ================================================================================================
 * BLUETOOTH
 * ================================================================================================ */
#define USE_BT_I2S 1
#if USE_BT_I2S
  #define BT_I2S_SEL_PIN        17
  #define BT_I2S_SEL_LEVEL_BT   HIGH
  #define BT_I2S_SEL_LEVEL_ESP  LOW
  #define BT_USE_MCLK           false
  #define BT_MUTE_STATUS_PIN    255
  #define BT_KEY_VOLP_PIN       47
  #define BT_KEY_VOLM_PIN       48
  #define BT_KEY_PLAY_PIN       18
#endif

/* ================================================================================================
 * DLNA
 * ================================================================================================ */
#define USE_DLNA
#ifndef DLNA_HOST_DEFAULT
#define DLNA_HOST_DEFAULT ""
#endif
#ifndef DLNA_IDX_DEFAULT
#define DLNA_IDX_DEFAULT 0
#endif

/* ================================================================================================
 * IR / SLEEP / WAKE
 * ================================================================================================ */
#define SLEEP_TIMER_PIN 15
#define WAKE_PIN            8
#define SLEEP_DEEPSLEEP_WAKE_HOURS 8
#define IR_PIN            21
#define IR_TIMEOUT        80

/* ================================================================================================
 * ADVANCED
 * ================================================================================================ */
#define WIFI_ATTEMPTS 16
/* WEBUI_COVERS: обложки в браузере; независимо от TFT */
#define WEBUI_COVERS_ENABLE true
/* WEBUI_CPU_BAR: 1 — полоска CPU; 0 — нет в прошивке */
#define WEBUI_CPU_BAR_ENABLE 1
/* SSL_BLOCK_LOG: true — почему заблокирован HTTPS; false — тише */
#define SSL_BLOCK_LOG_ENABLE false
/* TEMP_DIAG: true — [TEMP] °C в Serial раз в ~10 с (кристалл SoC, не радиатор) */
#define TEMP_DIAG_ENABLE false
/* DevKit S3 с PSRAM: mbedTLS calloc/free в PSRAM — FLAC+HTTPS аудио + iTunes/Cover
 * без SSL OOM (-32512); стеки FreeRTOS и LittleFS остаются во внутренней RAM. */
#define MBEDTLS_PSRAM_ALLOC_ENABLE true
/* WebUI в loop() Core 1; без макроса = DspTask Core 0 */
#define NETSERVER_LOOP1
/* Скрыть индикатор батареи на TFT */
#define BATTERY_OFF
/* Пороги HTTPS (факты / обложки).
 * maxAlloc ≈ free/2 (без обхода кучи). Смягчение D3: 36k / 20k типично для S3+PSRAM+FLAC. */
#define MIN_HEAP_FOR_SSL_FACTS 36000
#define MIN_HEAP_FOR_SSL_COVERS 36000
#define MIN_MAX_ALLOC_HEAP_FOR_SSL 20000
#define MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT 6
#define COVER_DOWNLOAD_DELAY_MS 2000
#define DISPLAY_COVER_REFRESH_MS 1500
#define COVER_CACHE_MAX_BYTES (2 * 1024 * 1024)
#define FACTS_CACHE_MAX_BYTES (2 * 1024 * 1024)
#ifndef TRACKFACTS_MAX_PER_TRACK
#define TRACKFACTS_MAX_PER_TRACK 5
#endif
#ifndef TRACKFACTS_CONFIG_COUNT_MAX
#define TRACKFACTS_CONFIG_COUNT_MAX 3
#endif
/* Авто-факты: ВЫКЛ = только вручную в WebUI; ВКЛ = фон при смене трека */
#define TRACKFACTS_AUTO_FETCH_ENABLE true
#define TRACKFACTS_AUTO_ITUNES_ENABLE true
#define TRACKFACTS_AUTO_LASTFM_ENABLE true
#define TRACKFACTS_AUTO_GEMINI_ENABLE true
#define TRACKFACTS_AUTO_DEEPSEEK_ENABLE false
#define TRACKFACTS_AUTO_GROQ_ENABLE false
/* VPS-прокси TrackFacts (Gemini+Groq). Пустой URL = поле в WebUI; веб-URL приоритетнее compile-time. */
#define TRACKFACTS_AI_PROXY ""
/* Пауза HTTPS после смены станции (обложки/факты), мс */
#define STATION_SWITCH_SSL_BLOCK_MS 20000
/* Бегущая строка TFT */
#define SCROLL_START_DELAY 5000
#define SCROLL_STEP 5
#define SCROLL_TIME 40

#endif
