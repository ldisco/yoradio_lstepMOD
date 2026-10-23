/****************************************************************************************************
 * Пример профиля Faster — Guition JC3248W535 (AXS15231B) + PCM5102.
 * Сборка: pio run -e yoradio-jc3248w535
 * Свой конфиг удобнее собрать на lstep.site/myoptions и сохранить как src/myoptions.h.
 *
 * Spectrum по умолчанию; DLNA; Sleep full-off; wake по тачу.
 * Cover-SS / пирамидка погоды / факты на экране — в этой линейке уже есть.
 * Панель 320×480, UI landscape 480×320 (Canvas soft-rotation).
 ****************************************************************************************************/

#ifndef myoptions_h
#define myoptions_h

#define L10N_LANGUAGE RU
#define DEVICE_NAME "Faster"
#define FM_PASSWORD "Adm1nUp2"
#define DSP_INVERT_TITLE  false
#define USE_BUILTIN_LED false

/* ================================================================================================
 * DISPLAY — AXS15231B QSPI 320×480
 * ================================================================================================ */
#define DSP_MODEL         DSP_AXS15231B
#define DISPLAY_COVERS_ENABLE true
#define TFT_CS   45
#define TFT_RST  -1
#define TFT_SCK  47
#define TFT_D0   21
#define TFT_D1   48
#define TFT_D2   40
#define TFT_D3   39
#define GFX_BL   1
/* Подсветка GPIO1: LEDC в displayAXS15231B (не analogWrite).
 * BRIGHTNESS_PIN=255 — yoRadio не трогает пин напрямую; WebUI/BacklightDown → axsSetBacklightDuty. */
#define BRIGHTNESS_PIN  255
#define DSP_HSPI   false
#define DSP_FLIPSCREEN_DEFAULT false
/* type2 обычно Guition; при мусоре на экране попробуйте 1 */
/* A/B banding: type2 шлёт SWRESET 0x01 в конце init; type1 — без 0x01 (как форум Guition). */
#define AXS15231B_INIT_TYPE 1
/* Present 50 мс (~20 fps): быстрее/ровнее meta; при щелчках звука/полосах — 80 или 120. */
#define AXS15231B_FLUSH_MIN_MS 50
/*
 * Spectrum vs VU: в Faster по умолчанию анализатор (гейт options.h → SPECTRUM_*).
 * false → сток VuWidget без пересборки conf.
 */
#define AXS15231B_SPECTRUM_ENABLE true

/* ================================================================================================
 * TOUCH — AXS I2C (Guition JC3248W535). Сырьё 320×480 → UI landscape в touchscreen.cpp.
 * WAKE_PIN=TS_INT=3: deep-sleep wake.
 * ================================================================================================ */
#define TS_MODEL   TS_MODEL_AXS15231B
#define TS_SDA     4
#define TS_SCL     8
#define TS_INT     3
#define TS_RST     -1

/* ================================================================================================
 * AUDIO — PCM5102 на штатный I2S платы
 * ================================================================================================ */
#define AUDIO_STREAM_DIAG 0
#define AUDIO_READ_HEAP_GUARD_ENABLE false
/* TEMP_DIAG: true — [TEMP] °C в Serial раз в 10 с (кристалл SoC, не радиатор) */
#define TEMP_DIAG_ENABLE false
#define I2S_DOUT      41
#define I2S_BCLK      42
#define I2S_LRC       2
#define VS1053_CS     255

#define ENC_INTERNALPULLUP    false
#define ENC2_INTERNALPULLUP   false

/* ================================================================================================
 * SD — onboard HSPI (как в публичных myoptions JC3248)
 * ================================================================================================ */
#define SD_MAX_LEVELS      6
#define SDC_CS          10
#define SD_SPIPINS      12, 13, 11
#define SD_HSPI         true

/* ================================================================================================
 * DLNA
 * (Yandex Music в этом публичном снимке не входит — USE_YANDEX_MUSIC не включаем.)
 * ================================================================================================ */
#define USE_DLNA
#ifndef DLNA_HOST_DEFAULT
#define DLNA_HOST_DEFAULT ""
#endif
#ifndef DLNA_IDX_DEFAULT
#define DLNA_IDX_DEFAULT 0
#endif

/* ================================================================================================
 * SLEEP / WAKE — full-off таймер; wake по тачу (INT)
 * SLEEP_TIMER_PIN: GPIO18 (P3). Сменить после распиновки разъёма при необходимости.
 * ================================================================================================ */
#define SLEEP_TIMER_PIN 18
#define WAKE_PIN            3
#define SLEEP_DEEPSLEEP_WAKE_HOURS 8

/* ================================================================================================
 * ADVANCED — facts / covers / proxy
 * ================================================================================================ */
#define WIFI_ATTEMPTS 16
#define WEBUI_COVERS_ENABLE true
#define WEBUI_CPU_BAR_ENABLE 1
#define SSL_BLOCK_LOG_ENABLE false
#define MBEDTLS_PSRAM_ALLOC_ENABLE true
#define NETSERVER_LOOP1
#define BATTERY_OFF
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
#define TRACKFACTS_AUTO_FETCH_ENABLE false
#define TRACKFACTS_AUTO_ITUNES_ENABLE true
#define TRACKFACTS_AUTO_LASTFM_ENABLE true
#define TRACKFACTS_AUTO_GEMINI_ENABLE true
#define TRACKFACTS_AUTO_DEEPSEEK_ENABLE false
#define TRACKFACTS_AUTO_GROQ_ENABLE false
#define TRACKFACTS_AI_PROXY ""
#define STATION_SWITCH_SSL_BLOCK_MS 12000
#define SCROLL_START_DELAY 5000
#define SCROLL_STEP 4
#define SCROLL_TIME 30
#define MIN_MAX_ALLOC_HEAP_FOR_SSL_COVERS 18000
#define MIN_AUDIO_BUFFER_FOR_SSL_COVERS_PERCENT 3

#endif
