/****************************************************************************************************
  ESP32S3_N16R8 NV3041A (JC4827W543) Touch GT911 + I2S + SD(FPI). 
  (Maleksm мод + батарейка  + Mute. Делитель (38.5х99.2) - 1-й (S3) пин.)
****************************************************************************************************/

#ifndef myoptions_h
#define myoptions_h

// Настройки WiFi. это в ходе экспериментов, можно убрать.
//#define WIFI_ATTEMPTS 32  // Увеличиваем количество попыток подключения
#define WIFI_ATTEMPTS 16  // Увеличиваем количество попыток подключения
#define WIFI_RECONNECT_DELAY 1000  // Задержка перед переподключением в мс
//#define WIFI_CONNECT_TIMEOUT 10000  // Таймаут подключения в мс
#define WIFI_CONNECT_TIMEOUT 5000  // Таймаут подключения в мс

//#define LED_BUILTIN_S3    48        /* S3-onboard RGB led pin */
//#define USE_BUILTIN_LED   false     /* Use S3-onboard RGB led */
//#define DSP_INVERT_TITLE  false     /*  Инверсные цвета названия станции (заголовок дисплея). По умолчанию "true"  */
//#define EXT_WEATHER       false     /*  Расширенная погода. По умолчанию "true"  */
//#define CLOCKFONT_MONO    false     /* Откл/Вкл моношрифтов для часов TFT дисплеев. По умолчанию "true"  */
//#define RSSI_DIGIT        true      /* Графический/цифровой значок Wi-Fi. (false - лесенка, true - цифирьки) */
//#define BITRATE_FULL      false     /* Значек битрейта. По умолчанию "true" */ 
#define L10N_LANGUAGE RU    /*  Language (EN, RU). More info in yoRadio/locale/displayL10n_(en|ru).h*/


// [DIAG] в Serial: только если 1. Для #if нельзя писать false — используйте 0 или закомментируйте строку.
#define AUDIO_STREAM_DIAG 0
// Возвращаем поведение чтения WEB-потока ближе к "как раньше":
// отключаем экспериментальный HEAP-GUARD, который при пороге 80KB
// часто пропускал чтение сокета и провоцировал фризы/просадки буфера.
// Если строку закомментировать — в options.h снова станет true (по умолчанию ВКЛ).
#define AUDIO_READ_HEAP_GUARD_ENABLE false
// Строка [HEAP-GUARD] в мониторе (0/1). На саму защиту (пропуск чтения + delay) не влияет.
// #define AUDIO_READ_HEAP_GUARD_LOG 1
/*временный диагностический мониторинг потока и сети, чтобы по логам понять, что чаще виновато в фризах: сеть, сам поток или код.
выводится раз в 5 секунд (в Serial и telnet)
Одна строка в формате:

##AUDIO.INFO#: [DIAG] buf=12345/655349 1% slow=150 lost=0 netAvail=8192 heap=108000 rssi=-62
Расшифровка:
buf — заполнение буфера в байтах и его полный размер.
% — заполнение буфера в процентах (низкий % при фризах → буфер часто пустеет).
slow — сколько раз за последнюю секунду буфер был ниже одного блока (чем больше, тем чаще «медленный» поток и риск дропаутов).
lost — счётчик секунд без данных от сети (рост до 10 → «Stream lost» и реконнект).
netAvail — сколько байт было доступно от сети в момент последнего вызова (0 при пустом буфере → сеть не успевает; большое значение при пустом буфере → узкое место скорее декодер/код).
heap — свободная куча (резкие просадки могут совпадать с фризами).
rssi — уровень WiFi в dBm (слабее −70…−80 при фризах намекает на сеть).*/

// WebUI: показывать обложки для WEB и SD режимов.
// true  - включено (ID3 из SD + iTunes, как в веб-радио)
// false - отключено (в вебе всегда logo)
#define WEBUI_COVERS_ENABLE true

// Логи блокировки SSL (cooldown/heap/SD и т.д.).
// false - скрыть периодические сообщения в мониторе.
#define SSL_BLOCK_LOG_ENABLE true

// HTTPS: суммарный free heap + отдельно MIN_MAX_ALLOC_HEAP_FOR_SSL (см. options.h).
// (-32512) при freeHeap 60–70K = фрагментация: смотреть getMaxAllocHeap в логах блокировки.
// Runtime (FLAC+iTunes+cover): max≈31.7K при free≈54K — порог 32768/52000 резал всё подряд.
#define MIN_HEAP_FOR_SSL_FACTS 44000
#define MIN_HEAP_FOR_SSL_COVERS 44000
// Непрерывный блок под mbedTLS; 30K достаточно для типичного рукопожатия при фрагментации ~31K max.
#define MIN_MAX_ALLOC_HEAP_FOR_SSL 30000

// TrackFacts: мин. заполнение аудио-буфера (%) перед SSL (в options.h по умолчанию 8).
// В 0.8.92 этого процента вообще не было — факты стартовали чаще, но при просадке буфера сильнее били по стабильности.
// 6% — мягкий компромисс: меньше ложных «низкий буфер», защита от фризов сохраняется.
// Закомментируй строку — вернётся дефолт 8 из options.h.
#define MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT 6

// Отображение обложек на дисплее (TFT).
#define DISPLAY_COVERS_ENABLE true

// 2000 мс как в options.h по умолчанию: при FLAC+Facts чуть больше времени до TLS обложки после старта потока.
#define COVER_DOWNLOAD_DELAY_MS 2000
#define DISPLAY_COVER_REFRESH_MS 1500

// Лимиты файлового кэша обложек/фактов.
// Если оставить как есть или удалить строки, сработают дефолты из options.h/TrackFacts.h.
#define COVER_CACHE_MAX_BYTES (5 * 1024 * 1024)
// #define LITTLEFS_MIN_FREE_BYTES (2 * 1024 * 1024)
#define FACTS_CACHE_MAX_BYTES (2 * 1024 * 1024) // Сейчас задаётся в TrackFacts.h

// Жёсткое окно блокировки SSL после переключения станции (мс).
// Выведено в myoptions для удобства тюнинга; текущее дефолтное значение 20000.
#define STATION_SWITCH_SSL_BLOCK_MS 20000

// TrackFacts: автозапрос AI-фактов (ручной запрос по нажатию остаётся доступен всегда).
// true  - авто-режим как сейчас
// false - только ручной запрос
#define TRACKFACTS_AUTO_GEMINI_ENABLE true
#define TRACKFACTS_AUTO_DEEPSEEK_ENABLE false

// ===== Sleep Timer (аппаратное отключение питания) =====
// Если оставить 255, таймер сна будет работать только программно (без обесточивания).
// Для аппаратного выключения укажите GPIO, управляющий реле/MOSFET.
#define SLEEP_TIMER_PIN 18
// Активный уровень на управляющем пине питания.
// #define SLEEP_TIMER_LEVEL LOW

// ===== Поддержка ЦАП ES9038Q2M =====
// Включение поддержки ЦАП ES9038
// Если true, будет вызвана функция инициализации es9038_on_setup() при старте
// Если false, ЦАП ES9038 не используется в проекте
#define USE_ES9038_DAC false  // выставьте в true, чтобы задействовать ES9038Q2M

// Пины для подключения ES9038 (soft I2C)
// SDA - линия данных, SCL - линия тактирования
// Определяются ТОЛЬКО если включён USE_ES9038_DAC.
// Если ES9038 не используется, пины устанавливаются в 255 (не назначены),
// что позволяет использовать эти GPIO для других целей.
#if USE_ES9038_DAC
  #define EQ_SDA_PIN 16  // Подключение к GPIO16
  #define EQ_SCL_PIN 15  // Подключение к GPIO15
#else
  #define EQ_SDA_PIN 255
  #define EQ_SCL_PIN 255
#endif

// ===== File Manager Password =====
// Пароль для файлового менеджера (мин. 8 символов, 2 цифры, 2 буквы разного регистра)
// Закомментируйте для отключения защиты паролем.
#define FM_PASSWORD "Adm1nUp2"

/* TrackFacts MusicBrainz Relay */
#define TF_MB_RELAY_IP   "_._._._"  /* Your VPS IP */
#define TF_MB_RELAY_PORT 4444            /* Your socat port */

/* Display */
//#define DSP_MODEL     DSP_DUMMY
//#define DSP_MODEL         DSP_NV3041A
//#define DSP_MODEL    DSP_ST7735  /* Select ST7735*/
//#define DTYPE     INITR_GREENTAB  /*  ST7735 display submodel. По умолчанию "INITR_BLACKTAB" */
//#define DSP_MODEL    DSP_ILI9341 /* Select ILI9341*/
//#define DSP_MODEL   DSP_ST7789  /* Select ST7789*/
//#define DSP_MODEL   DSP_ST7789_170  /* Select ST7789x170*/
#define DSP_MODEL    DSP_ILI9488 /* Select ILI9488*/
#define ILI9488_PORTRAIT true
//#define DSP_MODEL    DSP_ST7796 /* Select ILI9486*/
//#define DSP_MODEL    DSP_SSD1306x32    /* Select SSD1306-128х32*/
//#define DSP_MODEL    DSP_SH1106    /* Select SH1106-128х64*/

// Пины для параллельного интерфейса NV3041A (плата JC4827W543)
///#define TFT_CS  45
///#define TFT_RST -1
///#define TFT_SCK 47
///#define TFT_D0 21
///#define TFT_D1 48
///#define TFT_D2 40
///#define TFT_D3 39
///#define TFT_DC 255

// Управление яркостью
///#define GFX_BL 1 
//#define BRIGHTNESS_PIN  GFX_BL    /* (FSPI WP)    can be BLK, BL - Pin for brightness (output 0 - 3v3) */

/*  CAPACITIVE TOUCHSCREEN  Раскомментировать, если используется. */
///#define  TS_MODEL   TS_MODEL_GT911
//for JC4827W543 (NV3041A)
/*  Capacitive I2C touch screen  */
/// #define TS_SDA      8    
///#define TS_SCL      4       
///#define TS_INT      3    
///#define TS_RST      38

/* TFT */
#define TFT_CS          10    /* (FSPI CS0)        can be CS pin ( ) */
#define TFT_RST         -1    /* SPI RST pin. (-1 if connect to EN) */
#define TFT_DC           9    /* (FSPI HD)         can be DC, RS */
#define BRIGHTNESS_PIN  3   /* (FSPI WP)    can be BLK, BL - Pin for brightness (output 0 - 3v3) */
					/* Connect TFT_MOSI  to pin  11     (FSPI D)      can be SDA, DIN, SDI */
					/* Connect TFT_SCLK  to pin  12     (FSPI CLK)   can be SCL, SCK, CLK */
/* **************************************** */

/*  I2S DAC    */
// При USE_ES9038_DAC=true пины I2S должны отличаться от EQ_SDA/EQ_SCL
#if USE_ES9038_DAC
  #define I2S_DOUT      7    /*  = DIN connection */
  #define I2S_BCLK      5    /*  = BCLK Bit clock */
  #define I2S_LRC       6    /*  = WSEL Left Right Clock */
#else
  //#define I2S_DOUT      16    /*  = DIN connection. Should be set to 255 if the board is not used */
  //#define I2S_BCLK      7    /*  = BCLK Bit clock */
  //#define I2S_LRC       15    /*  = WSEL Left Right Clock */
  #define I2S_DOUT      17    /*  = DIN connection. Should be set to 255 if the board is not used */
  #define I2S_BCLK      16    /*  = BCLK Bit clock */
  #define I2S_LRC       18    /*  = WSEL Left Right Clock */
#endif
/* **************************************** */

/*  VS1053 PINS */
#define VS1053_CS     255    /*  XCS pin. Should be set to 255 if the board is not used */
//#define VS1053_DCS    18   /*             = XDCS pin.  */
//#define VS1053_DREQ   16   /*             = DREQ pin.  */
//#define VS1053_RST    -1   /* Set to -1 if connected to Esp EN pin, or any other = XRESET pin. */
					/* Connect VS1053 MOSI to  11   (FSPI D)     = SI pin.  */
					/* Connect VS1053 MISO to  13   (FSPI Q)     = MISO pin. */
					/* Connect VS1053 SCK to   12   (FSPI CLK)   = SCK pin. */
/* **************************************** */

/*  BUTTONS  */
//#define BTN_LEFT    6    /*  VolDown, Prev */
//#define BTN_CENTER  4    /*  Play, Stop, Show playlist */
//#define BTN_RIGHT   5    /*  VolUp, Next */
//#define BTN_UP      255   /*  Prev, Move Up */
//#define BTN_DOWN    255   /*  Next, Move Down */
//#define BTN_INTERNALPULLUP    false    /*  Enable the weak pull up resistors. По умолчанию "true" */
//#define BTN_LONGPRESS_LOOP_DELAY  200   /*  Delay between calling DuringLongPress event . По умолчанию "200" */
//#define BTN_CLICK_TICKS     300   /*  Event Timing https://github.com/mathertel/OneButton#event-timing По умолчанию "300" */
//#define BTN_PRESS_TICKS     500   /*  Event Timing https://github.com/mathertel/OneButton#event-timing По умолчанию "500" */
/* **************************************** */

/*  ENCODERs  */
#define ENC_BTNL              4       /*  Левое вращение энкодера (S1, DT)*/
#define ENC_BTNR              5       /*  Правое вращение энкодера (S2, CLK) */
#define ENC_BTNB              8       /*  Кнопка энкодера (Key, SW)*/
//#define ENC2_BTNL             6       /*  Левое вращение энкодера-2 (S1, DT)*/
//#define ENC2_BTNR             5       /*  Правое вращение энкодера-2 (S2, CLK) */
//#define ENC2_BTNB             4       /*  Кнопка энкодера-2 (Key, SW)*/
/// #define ENC_INTERNALPULLUP    false    /*  Enable the weak pull up resistors. По умолчанию "true"  */
///
//  #define ENC2_INTERNALPULLUP   false    /*  Enable the weak pull up resistors. По умолчанию "true"  */

/*  SDCARD  */
#define SD_MAX_LEVELS      6     /* Глубина рекурсии по папкам SD (оригинал: 3) */
// #define SDC_CS          39      /* SDCARD CS pin */
					// Connect SDC_MOSI to  40  /* On board can be D1 pin */
					// Connect SDC_SCK to   41   /* On board can be CLK pin */
					// Connect SDC_MISO to  42  /* On board can be D0 pin */
// #define SD_SPIPINS  41, 42, 40    /* SCK, MISO, MOSI */
////#define SDC_CS        10        /* SDCARD CS pin */
////#define SD_SPIPINS  12, 13, 11      /* SCK, MISO, MOSI */
////#define SD_HSPI     true      /* use HSPI for SD (miso=12, mosi=13, clk=14) instead of VSPI (by default)  */
/* **************************************** */


/*  TOUCHSCREEN  */
//#define TS_MODEL              TS_MODEL_GT911  /*  See description/available values in yoRadio/src/core/options.h  */

/*  Resistive SPI touch screen  */
/*  TS VSPI PINS. CLK must be connected to pin 18
                  DIN must be connected to pin 23
                  DO  must be connected to pin 19
                  IRQ - not connected */
//#define TS_CS                 255           /*  Touch screen CS pin  */
/*  TS HSPI PINS. CLK must be connected to pin 14
                  DIN must be connected to pin 13
                  DO  must be connected to pin 12
                  IRQ - not connected */
//#define TS_HSPI               false         /*  Use HSPI for Touch screen  */

/*  Capacitive I2C touch screen  */
//#define TS_SDA                4
//#define TS_SCL                5
//#define TS_INT                6
//#define TS_RST                7
/******************************************/



/*  Other settings.  */
// Переносим netserver.loop() в основной цикл (core1),
// чтобы WebUI не зависал из-за задач дисплея на core0.
#define NETSERVER_LOOP1
//#define MUTE_PIN    2           /*  MUTE Pin  (Pin2 - можно совместно использовать с диодом на devboard) */
//#define MUTE_VAL    LOW     /*  Write this to MUTE_PIN when player is stopped. По умолчанию "HIGH" */
//#define PLAYER_FORCE_MONO false   /*  mono option on boot - false stereo, true mono. По умолчанию "false" */
//#define I2S_INTERNAL  false /*  If true - use esp32 internal DAC. По умолчанию "false" */
//#define ROTATE_90 false     /*  Optional 90 degree rotation for square displays. По умолчанию "false"*/
//#define HIDE_VOLPAGE       /* Скрыть страницу "Громкость", ориентируемся по прогрессбару. (МОД nva_lw и Maleksm)  */
//#define HIDE_DATE            /* Скрыть дату. (МОД nva_lw и Maleksm)  */
//#define BOOMBOX_STYLE     /*  Разные варианты "показометра" VUmetr. Столбик, если строку закоментировать. */
//#define WAKE_PIN              255

//#define AUTOBACKLIGHT(x)    *function*    /*  Autobacklight function. See options.h for exsample  */
//#define AUTOBACKLIGHT_MAX     2500
//#define AUTOBACKLIGHT_MIN     12
///#define DOWN_LEVEL           42      /* lowest level brightness (from 0 to 255, default "2"). (МОД Maleksm) */
///#define DOWN_INTERVAL        60     /* interval for BacklightDown in sec (60 sec = 1 min, default "60"). (МОД Maleksm) */
/* ***************************************** */

/*  Аккумулятор */
#define BATTERY_OFF         /*  Выключить показатель уровня заряда и напряжения аккумулятора */
//#define HIDE_VOLT             /*  Скрыть только показатель напряжения аккумулятора */
//Connect ADC_PIN  to    1     /*  (Пин GPIO1) для считывания с делителя напряжения аккумулятора  */
//#define R1               38.5      /*  Номинал резистора, подключенного к плюсу батареи, в КОм или Ом. По умолчанию 50.  */
//#define R2               99.2      /*  Номинал резистора, подключенного к минусу, в КОм  или Ом. По умолчанию 100. */
//#define DELTA_BAT       -0.009    /*  Величина коррекции показаний напряжения батареи в вольтах */
/* *********** ****************************** */
//#define GRND_HEIGHT     231     /* (231 м) Высота местности над уровнем моря в метрах для поправки в давление */

/*  IR control  */
////#define IR_PIN            4
////#define IR_TIMEOUT        80              /*  see kTimeout description in IRremoteESP8266 example */
                   /*  https://github.com/crankyoldgit/IRremoteESP8266/blob/master/examples/IRrecvDumpV2/IRrecvDumpV2.ino */
/******************************************/

/*  RTC control  */
//#define RTC_MODULE    DS3231  /* или DS1307  */
//#define RTC_SDA       7    /* 47 */
//#define RTC_SCL       8    /* 48 */
/* **************************************** */

// Text scrolling tuning
#define SCROLL_STEP 1
#define SCROLL_TIME 20
#define SCROLL_START_DELAY 1000

#endif
