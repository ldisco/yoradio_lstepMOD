/*
 * [P0 2026-10-08 · подпись: Claude Opus] Согласование RX-буферов Wi-Fi с окном TCP патченого lwIP.
 *
 * Зачем:
 *   Для FLAC в ядро подложены liblwip.a / libesp_netif.a из «Audio+fix+core+3.3.5»
 *   (tools/patch_audio_fix_libs.py). В этом lwIP окно приёма TCP = 32768 байт на КАЖДОЕ
 *   соединение (в штатной 3.3.6 было 5760). А число RX-буферов Wi-Fi задаёт не sdkconfig,
 *   а ядро Arduino: WiFiGeneric.cpp → wifiLowLevelInit() жёстко ставит static_rx=4,
 *   dynamic_rx=32, при этом rx_ba_win остаётся 16 из sdkconfig.
 *
 *   В ESP-IDF буфер Wi-Fi и буфер lwIP — один и тот же (пакет передаётся по ссылке).
 *   Правило Espressif («Wi-Fi Performance and Power Save»): окно TCP ≈ dynamic_rx (КБ),
 *   при нескольких потоках окно делится между ними; rx_ba_win ≤ min(2×static_rx, dynamic_rx).
 *   У нас: поток FLAC (~23 сегмента окна) + загрузка обложки / факты / погода одновременно
 *   на 32 общих буфера, BA-окно 16 при норме ≤ 8. Когда аудио-буфер полон и сокет не
 *   читается, буферы заняты → драйвер не принимает ничего (ответы DNS, SYN-ACK) →
 *   DNS -54, errno 113, у роутера «not responding», при этом WL_CONNECTED. См.
 *   docs/handoffs/HANDOFF_P0_SOUND_DNS_WIFI.md, раздел H8.
 *
 * Как:
 *   Линкер перенаправляет все вызовы esp_wifi_init() сюда (-Wl,--wrap=esp_wifi_init
 *   в platformio.ini, во всех env). Мы поправляем только RX-параметры и зовём
 *   настоящую функцию. TX-настройки Arduino не трогаем.
 *
 * Память:
 *   static_rx — DMA-буферы во внутренней RAM, ~1.6 КБ каждый (было 4, стало 8 → +~6.4 КБ).
 *   dynamic_rx — выделяются только по мере прихода пакетов; при
 *   CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y (так в sdkconfig ядра) — сначала в PSRAM.
 *   Это предел, а не резерв: в покое память не тратится.
 */
#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>

/* [P0 2026-10-08 · Claude Opus] Целевые значения. Меняются здесь, в одном месте. */
#ifndef WIFI_RX_STATIC_BUF_NUM
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
/* Classic: не поднимать 4→8 — DMA ~1.6 КБ×4 во INTERNAL; S3 оставляем 8. */
#define WIFI_RX_STATIC_BUF_NUM 4
#else
#define WIFI_RX_STATIC_BUF_NUM 8    /* DMA-буферы приёма (внутренняя RAM) */
#endif
#endif
#ifndef WIFI_RX_DYNAMIC_BUF_NUM
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
/* Classic (Aiwushka/Wrover): 64 при отдаче script.js оставлял INTERNAL ~22 КБ — UI не догружался. */
#define WIFI_RX_DYNAMIC_BUF_NUM 24
#else
#define WIFI_RX_DYNAMIC_BUF_NUM 64  /* S3: предел ~2 окна TCP по 32 КБ + запас */
#endif
#endif

extern "C" {

/* Настоящая esp_wifi_init из libesp_wifi.a — имя создаёт линкер при --wrap. */
esp_err_t __real_esp_wifi_init(const wifi_init_config_t *config);

/*
 * [P0 2026-10-08 · Claude Opus] Обёртка: копируем конфиг Arduino, поднимаем RX до
 * согласованных значений (только вверх — если кто-то задал больше, не уменьшаем),
 * BA-окно ограничиваем правилом Espressif. Остальные поля без изменений.
 */
esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *config) {
  if (config == nullptr) {
    return __real_esp_wifi_init(config);
  }

  wifi_init_config_t cfg = *config;
  const int oldStatic = cfg.static_rx_buf_num;
  const int oldDynamic = cfg.dynamic_rx_buf_num;
  const int oldBa = cfg.rx_ba_win;

  if (cfg.static_rx_buf_num < WIFI_RX_STATIC_BUF_NUM) {
    cfg.static_rx_buf_num = WIFI_RX_STATIC_BUF_NUM;
  }
  /* dynamic_rx = 0 у IDF означает «без лимита» — такой случай не трогаем. */
  if (cfg.dynamic_rx_buf_num != 0) {
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
    /* Classic: жёсткий потолок (Arduino даёт 32 — одного «поднять до 64» мало, нужно снизить). */
    cfg.dynamic_rx_buf_num = WIFI_RX_DYNAMIC_BUF_NUM;
#else
    if (cfg.dynamic_rx_buf_num < WIFI_RX_DYNAMIC_BUF_NUM) {
      cfg.dynamic_rx_buf_num = WIFI_RX_DYNAMIC_BUF_NUM;
    }
#endif
  }
  /* dynamic_rx не может быть меньше static_rx (требование IDF Kconfig). */
  if (cfg.dynamic_rx_buf_num != 0 && cfg.dynamic_rx_buf_num < cfg.static_rx_buf_num) {
    cfg.dynamic_rx_buf_num = cfg.static_rx_buf_num;
  }
  /* BA-окно приёма (AMPDU) ≤ min(2×static_rx, dynamic_rx) — иначе пачка кадров
     от роутера не помещается в DMA-буферы и теряется. */
  if (cfg.ampdu_rx_enable) {
    int baMax = cfg.static_rx_buf_num * 2;
    if (cfg.dynamic_rx_buf_num != 0 && cfg.dynamic_rx_buf_num < baMax) {
      baMax = cfg.dynamic_rx_buf_num;
    }
    if (cfg.rx_ba_win > baMax) {
      cfg.rx_ba_win = baMax;
    }
  }

  esp_err_t err = __real_esp_wifi_init(&cfg);
  if (err != ESP_OK) {
    /* Страховка: если драйвер не принял новые значения (например, не хватило
       внутренней RAM под DMA-буферы) — стартуем с исходным конфигом Arduino,
       чтобы устройство не осталось без Wi-Fi. */
    Serial.printf("[WIFI-RX] init 0x%x with tuned RX -> fallback to Arduino defaults\n", (unsigned)err);
    cfg = *config;
    err = __real_esp_wifi_init(&cfg);
  }

  /* Технический лог для проверки на железе (Serial; на экран не выводится). */
  Serial.printf("[WIFI-RX] static %d->%d dynamic %d->%d ba %d->%d err=0x%x int_free=%u\n",
                oldStatic, cfg.static_rx_buf_num, oldDynamic, cfg.dynamic_rx_buf_num,
                oldBa, cfg.rx_ba_win, (unsigned)err,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return err;
}

}  // extern "C"
