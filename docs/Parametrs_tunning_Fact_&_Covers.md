# Быстрая настройка стабильности фактов и обложек

Этот файл про параметры в `myoptions.h`, которые влияют на устойчивость HTTPS (DeepSeek/Gemini/iTunes) и на поведение обложек.

## Что делает каждый параметр

- `AUDIO_STREAM_DIAG`
  - `0` — выключена подробная диагностика аудиопотока (рекомендуется для обычной работы).
  - `1` — в Serial идут служебные строки `DIAG` (удобно при отладке фризов/просадок).

- `AUDIO_READ_HEAP_GUARD_ENABLE`
  - `false` — не пропускает чтение аудиосокета из-за HEAP-guard (обычно стабильнее на сложных FLAC-станциях).
  - `true` — включает защиту чтения по порогу heap; может помочь при редких OOM, но иногда провоцирует просадки буфера.

- `SSL_BLOCK_LOG_ENABLE`
  - `true` — печатает причину, почему SSL запрос не стартовал (`low heap`, `cooldown`, `WiFi`, и т.д.).
  - `false` — тише лог, но сложнее диагностика.

- `MIN_HEAP_FOR_SSL_FACTS` и `MIN_HEAP_FOR_SSL_COVERS`
  - Минимум свободной кучи для старта HTTPS задач фактов/обложек.
  - Слишком высоко -> частые `SSL not safe`.
  - Слишком низко -> риск `SSL - Memory allocation failed` внутри mbedTLS.

- `MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT`
  - Минимальный процент заполнения аудиобуфера перед запуском HTTPS-фактов.
  - Выше значение -> безопаснее звук, но реже старт фактов.
  - Ниже значение -> факты стартуют чаще, но больше риск просадки аудио.

- `COVER_DOWNLOAD_DELAY_MS`
  - Пауза перед скачиванием обложки после постановки задачи.
  - Нужно, чтобы поток успел стабилизироваться после смены трека/станции.

- `DISPLAY_COVER_REFRESH_MS`
  - Интервал повторной попытки отрисовать/обновить обложку, если предыдущая попытка не удалась.

- `COVER_CACHE_MAX_BYTES`
  - Лимит кэша обложек в LittleFS (папка `/station_covers/cache`), LRU-очистка при превышении.
  - По умолчанию: `3 * 1024 * 1024` (3 МБ).

- `LITTLEFS_MIN_FREE_BYTES`
  - Минимальный запас свободного места LittleFS. При снижении ниже порога кэш обложек чистится (LRU).
  - По умолчанию: `2 * 1024 * 1024` (2 МБ).

- `FACTS_CACHE_MAX_BYTES`
  - Лимит файлового кэша фактов (папка `/data/facts`), LRU-очистка при превышении.
  - По умолчанию: `1 * 1024 * 1024` (1 МБ).

- `STATION_SWITCH_SSL_BLOCK_MS`
  - "Карантин" SSL после переключения станции: в это окно не стартуют тяжёлые HTTPS-задачи.
  - По умолчанию: `20000` мс.

- `TRACKFACTS_AUTO_GEMINI_ENABLE`, `TRACKFACTS_AUTO_DEEPSEEK_ENABLE`
  - Включают/отключают авто-запросы AI-фактов для соответствующего провайдера.
  - `true` — авто как обычно, `false` — только ручной запрос из UI (по нажатию).

## Стартовые (оптимальные) значения для большинства ESP32-S3 + FLAC

Рекомендуемый стартовый профиль:

- `AUDIO_STREAM_DIAG 0`
- `AUDIO_READ_HEAP_GUARD_ENABLE false`
- `SSL_BLOCK_LOG_ENABLE true` (на время диагностики; после стабилизации можно `false`)
- `MIN_HEAP_FOR_SSL_FACTS 58000`
- `MIN_HEAP_FOR_SSL_COVERS 58000`
- `MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT 6`
- `COVER_DOWNLOAD_DELAY_MS 1500`
- `DISPLAY_COVER_REFRESH_MS 1500`
- `COVER_CACHE_MAX_BYTES (3 * 1024 * 1024)`
- `LITTLEFS_MIN_FREE_BYTES (2 * 1024 * 1024)`
- `FACTS_CACHE_MAX_BYTES (1 * 1024 * 1024)`
- `STATION_SWITCH_SSL_BLOCK_MS 20000`
- `TRACKFACTS_AUTO_GEMINI_ENABLE true`
- `TRACKFACTS_AUTO_DEEPSEEK_ENABLE true`

## Как подбирать под конкретное устройство

1. Включить диагностику причин блокировки SSL:
   - `SSL_BLOCK_LOG_ENABLE true`
   - Прогон 15-30 минут на проблемной станции.

2. Если часто `SSL not safe (low heap)`:
   - Понизить оба порога `MIN_HEAP_FOR_SSL_*` шагом `2000`.
   - Нижняя практичная граница обычно `52000-54000`.

3. Если часто `SSL - Memory allocation failed (-32512)`:
   - Поднять оба порога `MIN_HEAP_FOR_SSL_*` шагом `2000`.
   - Одновременно увеличить `COVER_DOWNLOAD_DELAY_MS` до `2000-2500`.

4. Если звук проседает при фактах:
   - Увеличить `MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT` до `7-8`.

5. Если обложки появляются с заметной задержкой, но звук стабильный:
   - Уменьшить `COVER_DOWNLOAD_DELAY_MS` до `1200-1500`.

6. После стабилизации:
   - Можно вернуть `SSL_BLOCK_LOG_ENABLE false`, чтобы уменьшить шум в Serial.

## Диапазоны и "потолки" (практически)

- `COVER_CACHE_MAX_BYTES`
  - Практический диапазон: `1-6 МБ` (зависит от размера раздела LittleFS).
  - Не ставьте выше `LittleFS.totalBytes() - LITTLEFS_MIN_FREE_BYTES`, иначе будет постоянная LRU-чистка.

- `FACTS_CACHE_MAX_BYTES`
  - Практический диапазон: `256 КБ - 2 МБ`.
  - Слишком большой лимит обычно не даёт выгоды, но увеличивает число файлов и время обслуживания индекса.

- `LITTLEFS_MIN_FREE_BYTES`
  - Практический диапазон: `512 КБ - 3 МБ`.
  - Для стабильной работы файловых операций рекомендуется держать минимум `1-2 МБ`.

- `STATION_SWITCH_SSL_BLOCK_MS`
  - Практический диапазон: `8000-30000` мс.
  - Меньше — быстрее старт HTTPS после switch, но выше риск конфликтов SSL/аудио на тяжёлых потоках.

## Короткая памятка "что менять первым"

- Проблема: `SSL not safe (low heap)` -> снизить `MIN_HEAP_FOR_SSL_*`.
- Проблема: `-32512 SSL - Memory allocation failed` -> повысить `MIN_HEAP_FOR_SSL_*` и/или увеличить `COVER_DOWNLOAD_DELAY_MS`.
- Проблема: фризы аудио на фактах -> повысить `MIN_AUDIO_BUFFER_FOR_SSL_FACTS_PERCENT`.

