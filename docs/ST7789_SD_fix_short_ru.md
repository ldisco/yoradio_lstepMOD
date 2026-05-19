# Короткий фикс: SD + ST7789

Проблема: `Adafruit ST7735/ST7789` тянула `SD@1.3.0`, конфликтуя со штатной `SD@3.3.6` (ESP32), из-за чего падала сборка и ломался `TJpg_Decoder`.

Что сделано:
- Библиотека `Adafruit ST7735 and ST7789 Library` переведена в локальную (`lib/...`).
- В её `library.properties` удалена зависимость `SD` из `depends`.
- В `platformio.ini` убрана внешняя запись `adafruit/Adafruit ST7735 and ST7789 Library` из `lib_deps`.
- В `build_flags` добавлены переносимые include-пути через `$PROJECT_PACKAGES_DIR`:
  `fatfs/src`, `fatfs/vfs`, `fatfs/diskio`.

Что это дает:
- SD не отключается, `DSP_ST7789` и обложки сохраняются.
- В сборке остаётся одна корректная SD-реализация (framework SD).
- Сборка `platformio run -e yoradio-esp32s3` проходит успешно.
