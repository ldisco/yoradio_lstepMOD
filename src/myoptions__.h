/****************************************************************************************************
  СГЕНЕРИРОВАНО: lstepMOD myoptions generator v.0.62
  Плата: Selebrity (JC2432W328 ST7789)
  Целевой файл: src/myoptions.h
  ----- ЧТО ДЕЛАТЬ ДАЛЬШЕ -----
  1) SAVE TO PROJECT → диалог «Сохранить как» → src/myoptions.h
  2) Соберите:  pio run -e yoradio-jc2432w328
  3) LittleFS:  pio run -e yoradio-jc2432w328 -t buildfs
  4) Залейте app + LittleFS (upload / uploadfs).
  Примечание env: Обязательно pio run -e yoradio-jc2432w328 (UART/CH340).
****************************************************************************************************/

#ifndef myoptions_h
#define myoptions_h

/* ================================================================================================
 * LANGUAGE / BASIC
 * ================================================================================================ */
#define L10N_LANGUAGE RU
#define DEVICE_NAME "Selebrity"
/* DEV_COLOR — акцент имени/UI (TFT + WebUI); hex #RRGGBB */
#define DEV_COLOR "#d4d4d4"
/* Пароль File Manager WebUI; пустое поле в генераторе = без #define (открытый FM) */
#define FM_PASSWORD "Adm1nUp2"
#define DSP_INVERT_TITLE  false
#define USE_BUILTIN_LED false

/* ================================================================================================
 * DISPLAY
 * ================================================================================================ */
#define DSP_MODEL         DSP_ST7789
#define DISPLAY_COVERS_ENABLE true
/* SPI дисплея: TFT_SPIPINS SCLK(SCK)=19, MISO(SDO)=20, MOSI(SDI)=9.
 * CS / D/C(DC) / BL — в форме; RST=-1 часто через EN модуля. */
#define TFT_CS   47
#define TFT_RST  -1
#define TFT_DC   48
#define BRIGHTNESS_PIN  8
#define DSP_HSPI   false
#define TFT_SPIPINS      19, 20, 9
#define DSP_FLIPSCREEN_DEFAULT false
/* Визуализация: FFT-анализатор 12 полос */
#define ST7789_SPECTRUM_ENABLE true

/* ================================================================================================
 * TOUCH
 * ================================================================================================ */
#define TS_MODEL   TS_MODEL_CST820
#define TS_SDA     16
#define TS_SCL     15
#define TS_INT     255
#define TS_RST     17

/* ================================================================================================
 * AUDIO
 * ================================================================================================ */
/* AUDIO_STREAM_DIAG: 1 — [DIAG] раз в ~5 с; 0 — выкл. */
#define AUDIO_STREAM_DIAG 0
/* AUDIO_READ_HEAP_GUARD: true — пауза чтения при мало heap; false — непрерывный поток */
#define AUDIO_READ_HEAP_GUARD_ENABLE false
/* I2S → PCM5102 */
#define I2S_DOUT      7
#define I2S_BCLK      2
#define I2S_LRC       42
#define VS1053_CS     255

#define ENC_INTERNALPULLUP    false
#define ENC2_INTERNALPULLUP   false

/* ================================================================================================
 * SD CARD
 * ================================================================================================ */
#define SD_MAX_LEVELS      6
#define SDC_CS          38
#define SD_SPIPINS      39, 40, 1    /* SCK, MISO, MOSI */
#define SD_HSPI         true

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
#define SLEEP_TIMER_PIN 45
#define WAKE_PIN            3
#define SLEEP_DEEPSLEEP_WAKE_HOURS 8

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
#define MIN_HEAP_FOR_SSL_FACTS 44000
#define MIN_HEAP_FOR_SSL_COVERS 44000
#define MIN_MAX_ALLOC_HEAP_FOR_SSL 30000
#define MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT 2
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
#define TRACKFACTS_AUTO_FETCH_ENABLE false
#define TRACKFACTS_AUTO_ITUNES_ENABLE true
#define TRACKFACTS_AUTO_LASTFM_ENABLE true
#define TRACKFACTS_AUTO_GEMINI_ENABLE true
#define TRACKFACTS_AUTO_DEEPSEEK_ENABLE false
#define TRACKFACTS_AUTO_GROQ_ENABLE false
/* VPS-прокси TrackFacts (Gemini+Groq). Пустой URL = поле в WebUI; веб-URL приоритетнее compile-time. */
#define TRACKFACTS_AI_PROXY ""
/* Пауза HTTPS после смены станции (обложки/факты), мс */
#define STATION_SWITCH_SSL_BLOCK_MS 12000
/* Бегущая строка TFT */
#define SCROLL_START_DELAY 5000
#define SCROLL_STEP 4
#define SCROLL_TIME 30
/* Свои #define (ADVANCED → «+») */
#define MIN_MAX_ALLOC_HEAP_FOR_SSL_COVERS 18000
#define MIN_AUDIO_BUFFER_FOR_SSL_COVERS_PERCENT 3

#endif
