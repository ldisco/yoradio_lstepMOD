## Как вывести обложки (cover art) на ILI9488 (portrait 320x480)

Это пошаговая инструкция, что именно добавить/где именно править, чтобы `currentCoverUrl` (картинки, которые скачиваются/кэшируются) реально рисовались на дисплее ILI9488.

### Важно перед началом
1. Не трогайте версии/счётчики: `YOVERSION` и EEPROM `CONFIG_VERSION` не менять.
2. В проекте уже есть библиотеки: `LittleFS` и `TJpg_Decoder` (см. `platformio.ini`). Если вы их удаляли/меняли — верните.
3. Для простоты мы будем:
   - декодировать **JPEG** из `LittleFS` (кэш `/station_covers/...`),
   - рисовать в “слот обложки” (x/y/w/h),
   - если JPEG не найден/не декодируется — рисовать стандартный `logo` (fallback).

### Какие файлы нужно править (точный список)
Минимально рекомендуется править 4 файла:
1. `src/src/displays/displayILI9488.h`
2. `src/src/displays/displayILI9488.cpp`
3. `src/src/displays/widgets/widgets.cpp`
4. (по желанию) `src/src/displays/conf/displayILI9488conf_portrait.h` — только чтобы удобно настроить слот (если хотите)

В этой инструкции мы обязятельно правим первые 3 файла.

---

## Шаг 1. Объявить функцию “обновить обложку” для ILI9488
**Файл:** `src/src/displays/displayILI9488.h`

Найдите конец файла (после `ILI9488_DISPON/SLPOUT` секции). Добавьте объявление:

```cpp
#if DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486
  void ili9488_updateCoverSlot(bool forceRedraw=false);
#endif
```

Зачем: `widgets.cpp` сможет вызвать вашу функцию, когда обновляется часы.

---

## Шаг 2. Реализовать декодирование JPEG и отрисовку cover-слота
**Файл:** `src/src/displays/displayILI9488.cpp`

У вас сейчас файл маленький (init/flip/invert/sleep). Его нужно расширить.

### 2.1 Добавьте нужные include’ы
В начало файла (после текущих `#include`) добавьте:

```cpp
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include "../core/config.h"   // currentCoverUrl, config.theme, config.isScreensaver
#include "../core/options.h"  // DISPLAY_COVERS_ENABLE, DISPLAY_COVER_REFRESH_MS (если не подключено)
```

Если `config.h` уже есть — дублировать не нужно (оставьте один).

### 2.2 Вставьте код “как в NV3041A”, но под ILI9488
Вставьте в `displayILI9488.cpp` (например, после `DspCore::wake()` перед `#endif`) такой блок функций и глобальных переменных:

#### 2.2.1 Слот обложки (x/y/w/h)
Простейший стартовый вариант для portrait 320x480:

```cpp
// Слот обложки: start-значения под 320x480 (позже можно подвигать)
static constexpr int16_t kCoverSlotX = 50;   // (320 - 220)/2
static constexpr int16_t kCoverSlotY = 197;  // как у NV3041A (примерная переносимость по Y)
static constexpr int16_t kCoverSlotW = 220;
static constexpr int16_t kCoverSlotH = 220;

static constexpr int16_t kLogoW = 99;  // logo в bootlogo99x64.h
static constexpr int16_t kLogoH = 64;
static constexpr int16_t kLogoX = (320 - kLogoW) / 2;
static constexpr int16_t kLogoY = kCoverSlotY + (kCoverSlotH - kLogoH) / 2;
```

Дальше, если картинка/логотип мешает времени/VU — меняйте только `kCoverSlotX/Y/W/H`.

#### 2.2.2 Callback + resolve path + draw
Скопируйте логику из `displayNV3041A.cpp` (функции `coverOutput`, `resolveCoverPath`, `drawCoverAt`) и адаптируйте:
- используйте `dsp.drawRGBBitmap(...)` (она у ILI9488 есть),
- путь к картинкам делайте так же: `/sc/...` -> `/station_covers/...`.

Важное правило: `decode` в `drawCoverAt()` возвращает `true/false`.

#### 2.2.3 Функция, которую будет вызывать `widgets.cpp`
Добавьте реализацию:

```cpp
void ili9488_updateCoverSlot(bool forceRedraw) {
  // В скринсейвере можно не рисовать, чтобы не мешать времени
  if (config.isScreensaver) return;

  static String lastUrl = "";
  static uint32_t lastMs = 0;
  uint32_t now = millis();

  // currentCoverUrl обновляется в config.cpp
  String target = String(currentCoverUrl.c_str());
  bool urlChanged = (target != lastUrl);

  // throttle: не перерисовываем слишком часто
  if (!forceRedraw && !urlChanged) {
    if (now - lastMs < DISPLAY_COVER_REFRESH_MS) return;
  }

  // очищаем слот
  dsp.fillRect(kCoverSlotX, kCoverSlotY, kCoverSlotW, kCoverSlotH, config.theme.background);

  bool ok = drawCoverAt(kCoverSlotX, kCoverSlotY, kCoverSlotW, kCoverSlotH, target);
  if (!ok) {
    // fallback: логотип logo[99x64]
    dsp.drawRGBBitmap(kLogoX, kLogoY, logo, kLogoW, kLogoH);
  }

  lastUrl = target;
  lastMs = now;
}
```

Где `drawCoverAt(...)` — это вспомогательная функция из адаптированного блока (по образцу NV3041A).

> Примечание: `logo` — это массив из `fonts/bootlogo99x64.h`, который уже подключён в `displayILI9488.h`.

---

## Шаг 3. Вызвать обновление обложек при перерисовке часов
**Файл:** `src/src/displays/widgets/widgets.cpp`

Найдите функцию:
`void ClockWidget::_printClock(bool force){ ... }`

Там, где внутри `#if DSP_MODEL!=DSP_NV3041A` идёт отрисовка времени (строки с `gfx.setCursor(...)` и `gfx.print(_timebuffer)`).

Добавьте в начало `_printClock()` до `gfx.print(_timebuffer)` вот такой вызов:

```cpp
#if DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486
  ili9488_updateCoverSlot(force);
#endif
```

Зачем: часы обновляются раз в секунду (и ещё чаще при событиях), значит обложка гарантированно начнёт появляться без отдельного “таймера”.

---

## Шаг 4 (опционально). Подстроить координаты слота
Если обложка мешает UI:
- сначала сдвиньте по Y: `kCoverSlotY += 10` или `-= 10`,
- потом по X: `kCoverSlotX += 5` (обычно центр можно оставить),
- уменьшите W/H (например 200x200 вместо 220x220), если надо.

---

## Если не появится/появится “не сразу”
1. В консоли/мониторе будет `Serial.printf(...)` из вашей адаптированной `drawCoverAt`/`resolveCoverPath`. Если их нет — добавьте временно:
   - вывод targetUrl
   - “resolve path failed / open failed”
2. Убедитесь, что в LittleFS действительно есть файлы:
   - папка `/station_covers`
   - кэш подпапок `/station_covers/cache`

---

## Чек-лист перед “финальной” пробой
- [ ] Файл `displayILI9488.cpp` содержит `ili9488_updateCoverSlot()` и helper’ы `resolveCoverPath/drawCoverAt/coverOutput`
- [ ] Файл `displayILI9488.h` содержит объявление `ili9488_updateCoverSlot`
- [ ] В `widgets.cpp` есть вызов `ili9488_updateCoverSlot(force)` внутри `ClockWidget::_printClock()`
- [ ] Вы не меняли `YOVERSION` и `CONFIG_VERSION`

