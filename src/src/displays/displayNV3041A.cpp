// 24.05.2025
// доработка для JC4827W543R/C
// на основе идеи sandy053
// доработано W76W
// 4pda

#include "../core/options.h"
#if DSP_MODEL==DSP_NV3041A

#include "displayNV3041A.h"
// Конфигурация уже включена внутри .h файла
#include "../core/spidog.h"
#include "../core/config.h"
#include "../core/network.h"
#include "Arduino_GFX.h"
#include <LittleFS.h>
#include <TJpg_Decoder.h>

#define TAKE_MUTEX() sdog.takeMutex()
#define GIVE_MUTEX() sdog.giveMutex()

static bool coverOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);

static int16_t g_coverDrawX = 0;
static int16_t g_coverDrawY = 0;
static int16_t g_coverBoundLeft = 0;
static int16_t g_coverBoundTop = 0;
static int16_t g_coverBoundRight = 0;
static int16_t g_coverBoundBottom = 0;
static bool g_coverDecoderReady = false;
static String g_lastDrawnCoverUrl = "";
static uint32_t g_lastCoverUpdateMs = 0;  // <- НОВОЕ: время последнего обновления обложки
// [FIX] Флаг сброса обложки — безопасная замена прямого String-сброса из другой задачи.
// g_lastDrawnCoverUrl = "" из двух задач одновременно вызывает двойной free() → heap corruption.
// Теперь invalidateCoverCache() только выставляет этот флаг (атомарная операция),
// а сам String сбрасывается только внутри updateCoverSlot (задача дисплея).
static volatile bool g_coverInvalidateNeeded = false;
// [FIX][SD COVER] URL обложки, который признан "битым" для текущего трека.
// Нужен, чтобы не пытаться декодировать один и тот же повреждённый JPEG
// на каждом тике и не перегружать главный цикл дисплея/веба.
static String g_badCoverUrl = "";
static bool g_prevScreensaver = false;  // для отлавливания момента входа/выхода из скринсейвера
// static bool g_dotsOn = false;        // УБРАНО: используем tm_sec % 2 напрямую (как в референсном My_09535m_7)
static uint32_t g_ssLastMoveMs = 0;     // время последнего перемещения часов в скринсейвере
static int16_t g_ssTop = 0;             // текущая позиция часов по Y в скринсейвере

// === РАЗМЕРЫ ЛОГОТИПА (bootlogo2) - ФИКСИРОВАННЫЕ, НЕ МЕНЯТЬ! ===
// Логотип bootlogo2 - это массив пикселей 99x64 в PROGMEM.
// НЕЛЬЗЯ использовать другие размеры - будет мусор на экране!
constexpr int16_t kLogoW = 99;   // ФИКСИРОВАННАЯ ширина логотипа bootlogo2
constexpr int16_t kLogoH = 64;   // ФИКСИРОВАННАЯ высота логотипа bootlogo2
constexpr int16_t kLogoX = 86;   // Центрирование (272-99)/2
constexpr int16_t kLogoY = 270;  // Внутри слота (208 + (200-64)/2)

// === РАЗМЕРЫ ОБЛАСТИ ДЛЯ ОБЛОЖЕК (cover art) - МОЖНО МЕНЯТЬ САМОСТОЯТЕЛЬНО ===
// kCoverSlotY: положение ВЕРХНЕГО края слота обложки. Увеличьте, чтобы опустить её ниже;
// уменьшите, чтобы поднять весь слот вместе с фоном/логотипом.
// kCoverSlotW / H: ширина и высота (рекомендуется делать квадратными).
//constexpr int16_t kCoverSlotY = 208;
// Поднимаем слот на 10px (220 -> 210)
//constexpr int16_t kCoverSlotY = 210; 
constexpr int16_t kCoverSlotY = 197; 
constexpr int16_t kCoverSlotW = 220;  // iTunes отдаёт 200x200bb.jpg — изображение будет 200x200 по центру (10px отступ с каждой стороны)
constexpr int16_t kCoverSlotH = 220; 
constexpr int16_t kCoverSlotX = 26;  // (272 - 220) / 2
// ===========================================================================
// Тонкая подстройка сдвига (пиксели): после центрирования изображения применяется
// дополнительный смещение по X/Y. Используйте для исправления смещения на пару
// пикселей, если картинка выглядит сдвинутой или требует тонкой подгонки.
constexpr int16_t kCoverTuneOffsetX = 0; // пример: 2 -> сдвиг вправо на 2px
// Тонкая подстройка внутри слота (пиксели). Отрицательное — поднимает картинку внутри слота.
// Обычно достаточно -1..-4 для мелкой корректировки; глобальный подъём делаем через kCoverSlotY.
constexpr int16_t kCoverTuneOffsetY = -1; // тонкая подстройка внутри слота
// Максимум уменьшения JPEG (масштабирование степенью 2):
// - `kCoverMaxScale`: целое 0..3. Значение n означает, что изображение может быть
//   уменьшено в шагах 1:1, 1:2, 1:4, 1:8 (n=0..3).
//   Библиотека TJpgDec при масштабе 1:2 делает картинку 300x300, 
//   которая отлично впишется в наш новый слот 220x220 (лишнее обрежется).
// [FIX] 1:8 для больших JPEG — меньше нагрузки на кучу, снижает риск CORRUPT HEAP / multi_heap_free в lwIP
constexpr uint8_t kCoverMaxScale = 3; // макс 1:8  

static void ensureCoverDecoderReady() {
  if (g_coverDecoderReady) return;
  TJpgDec.setCallback(coverOutput);
  // Цветопередача: вернуть поведение, которое работало на устройстве ранее.
  // Без swap байтов — корректные оттенки на текущем железе.
  TJpgDec.setSwapBytes(false);
  g_coverDecoderReady = true;
}

// Функция для сброса кэша обложки (вызывается из config.cpp когда меняется currentCoverUrl).
// [FIX] Только выставляем флаг — НЕ трогаем String из чужой задачи!
// Прямой g_lastDrawnCoverUrl = "" из двух разных задач → двойной free → heap corruption.
void invalidateCoverCache() {
  g_coverInvalidateNeeded = true;
}

// Разрешение пути к обложке в файловой системе.
// Принимает URL обложки как параметр, а не читает глобальную переменную.
// Это устраняет гонку данных (race condition), когда currentCoverUrl
// меняется другой задачей между проверкой и открытием файла.
static bool resolveCoverPath(const String& coverUrl, String &outPath) {
  if (!coverUrl.length() || !littleFsReady) {
    return false;
  }
  
  // ПРИОРИТЕТ 1-4: Все обложки хранятся с путём /sc/...
  if (coverUrl.startsWith("/sc/")) {
    String sub = coverUrl.substring(4);
    // Декодируем %20 и прочее
    sub.replace("%20", " ");
    outPath = "/station_covers/" + sub;
    
    // Защищаем доступ к LittleFS семафором
    if (!lockLittleFS(200)) return false;
    bool exists = LittleFS.exists(outPath);
    unlockLittleFS();
    
    return exists;
  }
  
  return false;
}

// Рисует JPEG-обложку в указанной области экрана.
// coverUrl — URL обложки (напр. "/sc/cache/12345678.jpg"), передаётся явно
// чтобы избежать гонки данных с глобальной currentCoverUrl.
static bool drawCoverAt(int16_t x, int16_t y, int16_t w, int16_t h, const String& coverUrl) {
  String fsPath;
  // Передаём URL явно — resolveCoverPath НЕ читает глобальную переменную
  if (!resolveCoverPath(coverUrl, fsPath)) {
    Serial.printf("[COVER-DECODE] resolve path failed for url=%s\n", coverUrl.c_str());
    return false;
  }

  Serial.printf("[COVER-DECODE] start url=%s fs=%s\n", coverUrl.c_str(), fsPath.c_str());

  ensureCoverDecoderReady();

  uint16_t jpgW = 0;
  uint16_t jpgH = 0;
  
  // Если LittleFS временно занята фоновыми операциями (скачивание/кэш),
  // даём чуть больше времени на захват, чтобы не пропускать первую отрисовку обложки.
  // При этом таймаут всё ещё короткий и не блокирует UI надолго.
  if (!lockLittleFS(120)) {
    return false;
  }
  
  // Проверка размера файла перед декодированием (защита от пустых/битых файлов)
  File f = LittleFS.open(fsPath, "r");
  if (!f) {
    unlockLittleFS();
    Serial.printf("[COVER-DECODE] open failed: %s\n", fsPath.c_str());
    return false;
  }
  size_t fSize = f.size();
  f.close();
  
  if (fSize < 100) { // Слишком маленький файл для JPEG
    unlockLittleFS();
    Serial.printf("[COVER-DECODE] file too small: %s (%u bytes)\n", fsPath.c_str(), (unsigned)fSize);
    return false;
  }

  // КРИТИЧНО: используем getFsJpgSize() с ЯВНОЙ передачей LittleFS.
  bool sizeOk = (TJpgDec.getFsJpgSize(&jpgW, &jpgH, fsPath.c_str(), LittleFS) == JDR_OK);
  
  if (!sizeOk) {
    unlockLittleFS();
    Serial.printf("[COVER-DECODE] getFsJpgSize failed for %s\n", fsPath.c_str());
    return false;
  }

  // Вычисляем степень масштабирования (log2).
  // scale=0 → 1:1, scale=1 → 1:2, scale=2 → 1:4, scale=3 → 1:8
  // [FIX] Для очень больших JPEG (например 2000x2000) сразу используем не менее 1:4,
  // чтобы не создавать огромные промежуточные буферы и не нагружать кучу (риск CORRUPT HEAP).
  const uint16_t kMaxDecodeDim = 512;
  uint8_t scale = 0;
  while ((jpgW >> scale) > kMaxDecodeDim || (jpgH >> scale) > kMaxDecodeDim) {
    if (scale >= kCoverMaxScale) break;
    scale++;
  }
  uint16_t scaledW = jpgW >> scale;
  uint16_t scaledH = jpgH >> scale;
  while ((scaledW > (uint16_t)w || scaledH > (uint16_t)h) && scale < kCoverMaxScale) {
    scale++;
    scaledW = jpgW >> scale;
    scaledH = jpgH >> scale;
  }

  g_coverDrawX = x + (int16_t)((int32_t)w - scaledW) / 2 + kCoverTuneOffsetX;
  g_coverDrawY = y + (int16_t)((int32_t)h - scaledH) / 2 + kCoverTuneOffsetY;
  // Bounds = размер СЛОТА (не картинки!). Это ключевое исправление:
  // картинка, превышающая слот, обрезается по его краям, а не выходит за пределы.
  // Раньше bounds = размер картинки → увеличение kCoverSlotW/H не давало эффекта.
  g_coverBoundLeft = x;
  g_coverBoundTop = y;
  g_coverBoundRight = x + w;
  g_coverBoundBottom = y + h;

  // ИСПРАВЛЕНИЕ: API TJpgDec.setJpgScale() принимает 1, 2, 4, 8 (степень двойки),
  // а НЕ 0, 1, 2, 3 (лог2). Старый код передавал raw scale (0..4) — это приводило
  // к некорректному масштабированию или ошибке декодера.
  // Преобразуем: scale=0→1, scale=1→2, scale=2→4, scale=3→8
  TJpgDec.setJpgScale(1 << scale);

  Serial.printf("[COVER-DECODE] jpeg=%ux%u scale=1:%d out=%ux%u slot=%dx%d\n",
                jpgW, jpgH, (1 << scale), scaledW, scaledH, w, h);

  // Рисуем JPEG на дисплее. LittleFS передаётся явно,
  // чтобы библиотека TJpgDec не использовала SPIFFS по умолчанию.
  // drawFsJpg() возвращает JRESULT: JDR_OK (0) = успех, иначе ошибка.
  JRESULT drawResult = TJpgDec.drawFsJpg(g_coverDrawX, g_coverDrawY, fsPath.c_str(), LittleFS);
  
  // Освобождаем семафор после завершения всех операций с файлом
  unlockLittleFS();
  
  if (drawResult != JDR_OK) {
    Serial.printf("[COVER-DECODE] drawFsJpg failed: code=%d\n", drawResult);
  } else {
    Serial.printf("[COVER-DECODE] drawFsJpg success: %s\n", fsPath.c_str());
    // Даём сетевому стеку обработать пакеты — снижает риск порчи кучи при конкуренции с lwIP
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  
  return (drawResult == JDR_OK);
}

static bool coverOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  // Жесткое отсечение области обложки, чтобы не красить за пределами слота даже при сбое масштабирования
  int16_t x2 = x + w;
  int16_t y2 = y + h;
  int16_t drawL = max(x, g_coverBoundLeft);
  int16_t drawT = max(y, g_coverBoundTop);
  int16_t drawR = min(x2, g_coverBoundRight);
  int16_t drawB = min(y2, g_coverBoundBottom);
  if (drawR <= drawL || drawB <= drawT) return true; // блок полностью вне рамки
  uint16_t clippedW = drawR - drawL;
  uint16_t clippedH = drawB - drawT;
  // Смещаем указатель на начало нужного блока, если обрезка произошла
  uint16_t* src = bitmap + (drawT - y) * w + (drawL - x);
  // Мьютекс УБРАН: drawRGBBitmap внутри вызывает startWrite() → TAKE_MUTEX(),
  // поэтому внешний TAKE_MUTEX() создавал вложенный захват → spi_device_acquire_bus assert → crash.
  // (старая обёртка мьютексом не удаляется, а комментируется)
  //TAKE_MUTEX();
  dsp.drawRGBBitmap(drawL, drawT, src, clippedW, clippedH);
  //GIVE_MUTEX();
  return true;
}

// Обновление области обложки/логотипа.
//
// Логика работы:
// 1. Если URL обложки изменился — очищаем область и рисуем новое.
// 2. Периодический опрос каждые 2 сек — на случай, когда файл
//    ещё загружается фоновой задачей и появится позже.
// 3. forceRedraw=true — перерисовать БЕЗ очистки (после _clockDate()
//    которая может затереть часть области). Это устраняет мерцание.
// 4. Защита от зацикливания: после 10 неудач для одного URL — стоп.
static uint8_t g_coverFailCount = 0;
// [FIX][SD COVER] Сокращаем лимит повторов для битого файла.
// Если getFsJpgSize стабильно падает, дальнейшие попытки не восстанавливают файл,
// но создают фоновую нагрузку и могут "подвешивать" Web/Display.
static const uint8_t kMaxCoverRetries = 3;

static void updateCoverSlot(bool forceRedraw) {
  // В скринсейвере обложка не отображается
  if (config.isScreensaver) return;

  // [FIX] Применяем сброс кэша из главной задачи дисплея — не из вызвавшей задачи.
  // Это единственное место, где String безопасно изменять (одна задача).
  if (g_coverInvalidateNeeded) {
    g_coverInvalidateNeeded = false;
    g_lastDrawnCoverUrl = "";
    g_lastCoverUpdateMs = 0;
  }

  // [FIX] Глобальный переключатель отображения обложек на дисплее.
  // Если отключено — всегда рисуем логотип и выходим.
  if (!isDisplayCoversEnabled()) {
    static String kDisabledTag = "LOGO_DISABLED";
    uint32_t now = millis();
    bool urlChanged = (kDisabledTag != g_lastDrawnCoverUrl);
    if (urlChanged || forceRedraw) {
      dsp.fillRect(kCoverSlotX, kCoverSlotY, kCoverSlotW, kCoverSlotH, config.theme.background);
      dsp.drawRGBBitmap(kLogoX, kLogoY, bootlogo2, kLogoW, kLogoH);
      g_lastDrawnCoverUrl = kDisabledTag;
      g_lastCoverUpdateMs = now;
    }
    return;
  }

  // Копия по значению, чтобы другой поток не изменил currentCoverUrl во время декода (гонка → heap)
  String target = String(currentCoverUrl.c_str());
  uint32_t now = millis();
  bool urlChanged = (target != g_lastDrawnCoverUrl);
  // [FIX][SD COVER] Для URL, уже помеченного как битый, повторы отключаем.
  // Это защищает от цикла "decode fail -> retry -> decode fail" для одного и того же файла.
  bool isKnownBadCover = (target == g_badCoverUrl);
  bool retryDue = (!urlChanged && !forceRedraw && g_coverFailCount > 0
                   && target.startsWith("/sc/")
                   && !isKnownBadCover
                   // Интервал повторной попытки отрисовки задаётся через
                   // DISPLAY_COVER_REFRESH_MS (options.h / myoptions.h).
                   // По умолчанию 2000 мс, рекомендуемый диапазон 1000–5000 мс.
                   && (now - g_lastCoverUpdateMs) >= DISPLAY_COVER_REFRESH_MS
                   && g_coverFailCount < kMaxCoverRetries);

  // При смене URL сбрасываем счётчик неудач
  if (urlChanged) {
    // [FIX][SD COVER] Смена URL = новый кандидат обложки.
    // Снимаем "карантин" с прошлого битого URL, чтобы не блокировать новый трек.
    g_badCoverUrl = "";
    g_coverFailCount = 0;
  }

  // Событийная отрисовка:
  // - при смене URL обложки,
  // - при явном forceRedraw (перерисовка интерфейса/смена режима).
  bool needsNewDraw = urlChanged || forceRedraw || retryDue;

  // Если ничего не изменилось И не нужна принудительная перерисовка — выходим
  if (!needsNewDraw) return;

  // Очищаем область ТОЛЬКО при реальной смене URL или если принудительно (forceRedraw),
  // но только если URL изменился, чтобы не было мерцания на каждый тик часов.
  if (urlChanged) {
    dsp.fillRect(kCoverSlotX, kCoverSlotY, kCoverSlotW, kCoverSlotH, config.theme.background);
  }

  bool drawn = false;
  // Пробуем нарисовать обложку из файловой системы
  if (target.startsWith("/sc/") && littleFsReady) {
    drawn = drawCoverAt(kCoverSlotX, kCoverSlotY, kCoverSlotW, kCoverSlotH, target);
    if (!drawn) {
      if (urlChanged || retryDue) {
        g_coverFailCount++;
        Serial.printf("[COVER-ERR] Попытка %d из %d не удалась\n",
                      g_coverFailCount, kMaxCoverRetries);
        // [FIX][SD COVER] Если после нескольких попыток JPEG не декодируется —
        // считаем файл повреждённым, блокируем дальнейшие ретраи этого URL
        // и удаляем кэш-файл (если это /sc/cache/...), чтобы не возвращаться к нему снова.
        if (g_coverFailCount >= kMaxCoverRetries) {
          g_badCoverUrl = target;
          if (target.startsWith("/sc/cache/")) {
            String badFsPath = "/station_covers/" + target.substring(4);
            if (lockLittleFS(120)) {
              // [FIX][SD COVER] Удаляем только кэш-файл; ручные обложки не трогаем.
              LittleFS.remove(badFsPath);
              unlockLittleFS();
            }
            Serial.printf("[COVER-ERR] Кэш-обложка помечена битой и удалена: %s\n", badFsPath.c_str());
          } else {
            Serial.printf("[COVER-ERR] Обложка помечена битой (без удаления): %s\n", target.c_str());
          }
        }
      }
      // ОЧИСТКА: если обложка не прорисовывалась полностью или вообще не открылась,
      // очищаем область повторно, чтобы не оставалось "мусора" (30% картинок и т.д.)
      dsp.fillRect(kCoverSlotX, kCoverSlotY, kCoverSlotW, kCoverSlotH, config.theme.background);
    } else {
      g_coverFailCount = 0;
    }
  }

  // Если обложка не отрисована — показываем логотип. ГАРАНТИРОВАНО на чистом фоне.
  if (!drawn) {
    dsp.drawRGBBitmap(kLogoX, kLogoY, bootlogo2, kLogoW, kLogoH);
  }

  // Обновляем кэш URL после события отрисовки.
  if (needsNewDraw) {
    g_lastDrawnCoverUrl = target;
    g_lastCoverUpdateMs = now;
  }
}

// Оптимизация QSPI для NV3041A: увеличиваем частоту до 80 МГц
Arduino_DataBus *bus = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_D0, TFT_D1, TFT_D2, TFT_D3, 80000000);

DspCore::DspCore(): Arduino_NV3041A(bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, true /* IPS */) {}

#include "tools/utf8RusGFX.h"
///////////////////////////////////////////////////////////////
#ifndef BATTERY_OFF

  #include "driver/adc.h"			// Подключение необходимого драйвера
  #include "esp_adc_cal.h"			// Подключение необходимой библиотеки

  #ifndef ADC_PIN
    #define ADC_PIN 5
  #endif
  #if (ADC_PIN == 5)
    #define USER_ADC_CHAN 	ADC1_CHANNEL_4
  #endif

  #ifndef R1
    #define R1 33		// Номинал резистора на плюс (+)
  #endif
  #ifndef R2
    #define R2 100		// Номинал резистора на плюс (-)
  #endif
  #ifndef DELTA_BAT
    #define DELTA_BAT 0	// Величина коррекции напряжения батареи
  #endif

  float ADC_R1 = R1;		// Номинал резистора на плюс (+)
  float ADC_R2 = R2;		// Номинал резистора на минус (-)
  float DELTA = DELTA_BAT;	// Величина коррекции напряжения батареи

  uint8_t g, t = 1;		// Счётчики для мигалок и осреднений
  bool Charging = false;	// Признак, что подключено зарядное устройство

  uint8_t reads = 100;    	// Количество замеров в одном измерении
  float Volt = 0; 			// Напряжение на батарее
  float Volt1, Volt2, Volt3, Volt4, Volt5 = 0;	 // Предыдущие замеры напряжения на батарее
  static esp_adc_cal_characteristics_t adc1_chars;

  uint8_t ChargeLevel;
// ==================== Массив напряжений на батарее, соответствующий проценту оставшегося заряда: 
  float vs[22] = {2.60, 3.10, 3.20, 3.26, 3.29, 3.33, 3.37, 3.41, 3.46, 3.51, 3.56, 3.61, 3.65, 3.69, 3.72, 3.75, 3.78, 3.82, 3.88, 3.95, 4.03, 4.25};

  #endif
/////////////////////////////////////////////////////////////////////////////////

// === Буферы для оптимизации ===
static uint16_t* _plBuffer = nullptr;
static uint16_t _plBufferWidth = 0;
static uint16_t _plBufferHeight = 0;
static char* _textBuffer = nullptr;

// === ФУНКЦИЯ БЕЗОПАСНОЙ ОЧИСТКИ БУФЕРОВ ===
// Вызывается при критических ошибках памяти или сбросе дисплея
static void cleanupDisplayBuffers() {
  Serial.println("[БЕЗОПАСНОСТЬ] Очистка буферов дисплея");
  if (_textBuffer) {
    free(_textBuffer);
    _textBuffer = nullptr;
    Serial.println("[БЕЗОПАСНОСТЬ] Текстовый буфер освобожден");
  }
  if (_plBuffer) {
    free(_plBuffer);
    _plBuffer = nullptr;
    Serial.println("[БЕЗОПАСНОСТЬ] Буфер плейлиста освобожден");
  }
  _plBufferWidth = 0;
  _plBufferHeight = 0;
}

// === ПРОВЕРКА ЦЕЛОСТНОСТИ УКАЗАТЕЛЕЙ ===
// Простая проверка что указатели буферов не повреждены
static bool areBuffersValid() {
  // Проверяем что указатели в разумных пределах памяти ESP32
  // ESP32 имеет DRAM в диапазоне 0x3FFB0000-0x3FFFFFFF и PSRAM если есть
  uintptr_t textAddr = (uintptr_t)_textBuffer;
  uintptr_t plAddr = (uintptr_t)_plBuffer;
  
  if (_textBuffer && (textAddr < 0x3F000000 || textAddr > 0x40000000)) {
    Serial.printf("[БЕЗОПАСНОСТЬ] Подозрительный адрес textBuffer: 0x%08X\n", textAddr);
    return false;
  }
  if (_plBuffer && (plAddr < 0x3F000000 || plAddr > 0x40000000)) {
    Serial.printf("[БЕЗОПАСНОСТЬ] Подозрительный адрес plBuffer: 0x%08X\n", plAddr);
    return false;
  }
  return true;
}

// Локальная функция для корректного вывода кириллицы в Canvas
static void printCyrillicWithShift(Canvas* canvas, const char* str) {
    for (int i = 0; i < strlen(str); i++) {
        unsigned char ch = (unsigned char)str[i];
        if (ch >= 0xC0) ch--;
        canvas->write(ch);
    }
}

void DspCore::initDisplay() {
  // === БЕЗОПАСНАЯ ИНИЦИАЛИЗАЦИЯ ===
  // При каждой инициализации дисплея очищаем старые буферы
  // чтобы избежать накопления утечек памяти
  cleanupDisplayBuffers();
  
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH); // включить подсветку
  begin();
  init();

  ensureCoverDecoderReady();

  flip();
  setTextWrap(false);
  setTextSize(1);
  fillScreen(0x0000);
  invert();
  
  // === Расчёт размеров и позиций для высокого разрешения ===
  // Высота одного элемента плейлиста
  plItemHeight = playlistConf.widget.textsize*(CHARHEIGHT-1)+playlistConf.widget.textsize*4;
  if (plItemHeight == 0) plItemHeight = CHARHEIGHT + 4; // защита от деления на ноль
  // Количество видимых элементов (нечётное, чтобы текущий был по центру)
  plTtemsCount = round((float)height()/plItemHeight);
  if(plTtemsCount%2==0) plTtemsCount++;
  // Ограничение на разумные пределы (например, минимум 3, максимум 15)
  if(plTtemsCount < 3) plTtemsCount = 3;
  if(plTtemsCount > 15) plTtemsCount = 15;
  // Позиция текущего элемента (по центру)
  plCurrentPos = plTtemsCount/2;
  // Начальная координата по Y для первого элемента
  plYStart = (height() / 2 - plItemHeight / 2) - plItemHeight * (plTtemsCount - 1) / 2 + playlistConf.widget.textsize*2;

  // === ПРОВЕРКА ЦЕЛОСТНОСТИ БУФЕРОВ ПЕРЕД РАБОТОЙ ===
  if (!areBuffersValid()) {
    Serial.println("[БЕЗОПАСНОСТЬ] Обнаружено повреждение буферов - принудительная очистка");
    cleanupDisplayBuffers();
  }

  // === Выделение памяти для текстового буфера ===
  if (!_textBuffer) {
    _textBuffer = (char*)malloc(256);
    if (_textBuffer == NULL) {
      Serial.println("[БЕЗОПАСНОСТЬ] Ошибка выделения памяти для текстового буфера!");
    } else {
      // Очищаем буфер для безопасности
      memset(_textBuffer, 0, 256);
    }
  }

  // === Выделение памяти для буфера плейлиста ===
  _plBufferWidth = width();
  _plBufferHeight = plTtemsCount * plItemHeight;
  
  // ЗАЩИТА от утечек памяти: освобождаем старый буфер если размеры изменились
  uint32_t reqBufferSize = _plBufferWidth * _plBufferHeight * 2;
  static uint32_t lastBufferSize = 0;
  
  if (_plBuffer && (reqBufferSize != lastBufferSize)) {
    Serial.println("[БЕЗОПАСНОСТЬ] Размер буфера плейлиста изменился - пересоздание");
    free(_plBuffer);
    _plBuffer = nullptr;
  }
  
  if (!_plBuffer) {
    // ЗАЩИТА от слишком больших выделений памяти (максимум ~200КБ)
    if (reqBufferSize > 200000) {
      Serial.printf("[БЕЗОПАСНОСТЬ] Запрос слишком большого буфера: %u байт. Отказ!\n", reqBufferSize);
      return;
    }
    
    _plBuffer = (uint16_t*)malloc(reqBufferSize);
    if (_plBuffer == NULL) {
      Serial.printf("[БЕЗОПАСНОСТЬ] Ошибка выделения памяти для буфера плейлиста! Запрошено: %u байт\n", reqBufferSize);
      // Попробуем освободить текстовый буфер для освобождения места
      if (_textBuffer) {
        free(_textBuffer);
        _textBuffer = nullptr;
      }
    } else {
      lastBufferSize = reqBufferSize;
      Serial.printf("[БЕЗОПАСНОСТЬ] Буфер плейлиста выделен: %u байт\n", reqBufferSize);
    }
  }
}

void DspCore::drawLogo(uint16_t top) 
{ 
    clearDsp(); // Очистка экрана перед выводом логотипа
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, LOW);
#if BOOTLOGO_TYPE == 0
  // Логотип bootlogo2: ФИКСИРОВАННЫЙ размер 99x64 (kLogoW x kLogoH)
  // Передаём пустой URL (обложка ещё не загружена при загрузке)
  if (!drawCoverAt((width() - kLogoW) / 2, top, kLogoW, kLogoH, currentCoverUrl)) {
    drawRGBBitmap((width() - kLogoW) / 2, top, bootlogo2, kLogoW, kLogoH);
  }
#else
  // Кастомный логотип 60x53
  if (!drawCoverAt((width() - 250) / 2, top, 60, 53, currentCoverUrl)) {
    drawRGBBitmap((width() - 250) / 2, top, bootlogo2custom, 60, 53);
  }
#endif
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, HIGH);

    // [FIX] Вывод версии прошивки прямо в drawLogo, чтобы гарантированно была видна при загрузке.
    // Виджет версии в display.cpp после drawLogo мог не успеть отрисоваться или перекрываться.
    {
      char verBuf[48];
      snprintf(verBuf, sizeof(verBuf), "yoradio: %s", YOVERSION);
      setTextSize(1);
      setTextColor((uint16_t)0xFFFF, (uint16_t)0x0000);
      setFont();
      const char* out = utf8Rus(verBuf, false);
      uint16_t tw = (out ? strlen(out) : 0) * 6; // приблизительная ширина при textSize 1
      if (tw > width()) tw = width();
      setCursor((width() - tw) / 2, 226); // под строкой «Соединяюсь с» (bootstrConf.top=210 + 16), чуть ниже чтобы не в нахлёст
      if (out) print(out);
    }
}

void DspCore::printPLitem(uint8_t pos, const char* item){
  setTextSize(playlistConf.widget.textsize);
  int16_t itemY = plYStart + pos * plItemHeight;
  int16_t itemH = plItemHeight - 2;
  int16_t itemW = width();
  Canvas lineCanvas(itemW, itemH);

  uint16_t bg = config.theme.background;
  uint16_t fg = config.theme.playlist[0];
  if (pos == plCurrentPos) {
    bg = config.theme.plcurrentbg;
    fg = config.theme.plcurrent;
  } else {
    uint8_t plColor = (abs(pos - plCurrentPos) - 1) > 4 ? 4 : abs(pos - plCurrentPos) - 1;
    fg = config.theme.playlist[plColor];
  }

  lineCanvas.fillScreen(bg);
  lineCanvas.setTextColor(fg, bg);
  lineCanvas.setTextSize(playlistConf.widget.textsize);
  lineCanvas.setCursor(TFT_FRAMEWDT, 1);
  lineCanvas.setTextWrap(false);
  printCyrillicWithShift(&lineCanvas, utf8Rus(item, false));
  drawRGBBitmap(0, itemY, (uint16_t*)lineCanvas.getBuffer(), itemW, itemH);
}

void DspCore::drawPlaylist(uint16_t currentItem) {
  int ls = currentItem - plCurrentPos;
  uint8_t c = 0;
  bool finded = false;
  if (config.playlistLength() == 0) return;

  File playlist = config.SDPLFS()->open(REAL_PLAYL, "r");
  File index = config.SDPLFS()->open(REAL_INDEX, "r");
  while (true) {
    if (ls < 1) {
      ls++;
      printPLitem(c, "");
      c++;
      if (c >= plTtemsCount) break;
      continue;
    }
    if (!finded) {
      index.seek((ls - 1) * 4, SeekSet);
      uint32_t pos;
      index.readBytes((char*)&pos, 4);
      finded = true;
      index.close();
      playlist.seek(pos, SeekSet);
    }
    bool pla = true;
    while (pla) {
      pla = playlist.available();
      String stationName = playlist.readStringUntil('\n');
      stationName = stationName.substring(0, stationName.indexOf('\t'));
      if (config.store.numplaylist && stationName.length() > 0) {
        stationName = String(ls + c) + " " + stationName;
      }
      printPLitem(c, stationName.c_str());
      c++;
      if (c >= plTtemsCount) break;
    }
    break;
  }
  playlist.close();
  if (c < plTtemsCount) {
    fillRect(0, c * plItemHeight + plYStart, width(), height() / 2, config.theme.background);
  }
}

void DspCore::clearDsp(bool black) { fillScreen(black?0:config.theme.background); }

uint8_t DspCore::_charWidth(unsigned char c){
  GFXglyph *glyph = pgm_read_glyph_ptr(&DirectiveFour56, c - 0x20);
  return pgm_read_byte(&glyph->xAdvance);
}

uint16_t DspCore::textWidth(const char *txt){
  uint16_t w = 0, l=strlen(txt);
  for(uint16_t c=0;c<l;c++) w+=_charWidth(txt[c]);
  return w;
}

void DspCore::_getTimeBounds() {
  _timewidth = textWidth(_timeBuf);
  char buf[4];
  strftime(buf, 4, "%H", &network.timeinfo);
  _dotsLeft=textWidth(buf);
}

void DspCore::_clockSeconds(){
  // Защита от неинициализированных координат
  if(_timewidth == 0) _getTimeBounds();

  // Историческое поведение: мигание по чётности секунд (видно в момент «прыжка»).
  bool dotsVisible = (network.timeinfo.tm_sec % 2 == 0);

  if(config.isScreensaver) {
    // Скринсейвер: двоеточие с непрозрачным фоном (для перезатирки старого символа)
    setTextSize(1);
    setFont(&DirectiveFour56);
    // Два аргумента setTextColor: рисует фон символа (затирает предыдущий «:»)
    setTextColor(dotsVisible ? config.theme.clock : config.theme.background, config.theme.background);
    setCursor(_timeleft + _dotsLeft + CLOCK_DOTS_X_OFFSET + 55, clockTop - CHARHEIGHT + CLOCK_DOTS_Y_OFFSET - 25);
    print(":");
    setFont();
    return;
  }

  // Основной экран: двоеточие с прозрачным фоном (один аргумент setTextColor),
  // т.к. у шрифта DirectiveFour56 большой bounding box, затирающий всё ниже цифр.
  setTextSize(1);
  setFont(&DirectiveFour56);
  // Один аргумент setTextColor: прозрачный фон, не затираем область под символом
  setTextColor(dotsVisible ? config.theme.clock : config.theme.background);
  setCursor(_timeleft + _dotsLeft + CLOCK_DOTS_X_OFFSET - 1 + 56, clockTop - CHARHEIGHT + CLOCK_DOTS_Y_OFFSET - 18);
  print(":");
  setFont();

/////////////////////////////////////////////////////////////////////////////
  #ifndef BATTERY_OFF
  if(!config.isScreensaver) {
    // Оптимизация: кэширование цветов
    static const uint16_t BAT_COLORS[] = {
      color565(100, 255, 150),  // >85% зеленый
      color565(50, 255, 100),   // 70-85% зеленый
      color565(0, 255, 0),      // 55-70% зеленый
      color565(75, 255, 0),     // 40-55% зеленый
      color565(150, 255, 0),    // 25-40% зеленый
      color565(255, 255, 0),    // 10-25% желтый
      color565(255, 0, 0)       // 0-10% красный
    };
// ================================ Отрисовка мигалок ===================================
  setTextSize(BatFS);		//    setTextSize(2)

  if (Charging)		// Если идёт зарядка (подключено зарядное устр-во) - бегающие квадратики - цвет Светлосиний (Cyan)
    {
	setTextColor(color565(0, 255, 255), config.theme.background);				// Светлосиний (Cyan)
	if (network.timeinfo.tm_sec % 1 == 0)
	{
           setCursor(BatX, BatY);
		if (g == 1) { print("\xA0\xA2\x9E\x9F"); } 			// 2 квад. в конце
		if (g == 2) { print("\xA0\x9E\x9E\xA3"); } 			// 2 квад. по краям
		if (g == 3) { print("\x9D\x9E\xA2\xA3"); } 			// 2 квад. в начале
		if (g >= 4) {g = 0; print("\x9D\xA2\xA2\x9F");} 		// 2 квад. в середине
                g++;
	}
    }

// ============================= Отрисовка предупреждающей мигалки ==========================
    if (Volt < 2.8 )                 //мигающие квадратики - Красный (Red)
    {
     if (network.timeinfo.tm_sec % 1 == 0)
      {
        setCursor(BatX, BatY);
        setTextColor(color565(255, 0, 0), config.theme.background);
        if (g == 1) { print("\xA0\xA2\xA2\xA3");} 				// полная - 6 кв.
        if (g >= 2) {g = 0; print("\x9D\x9E\x9E\x9F");} 			// пустая - 0 кв.
         g++;
      }
     }

// ========================== Расчёт и отрисовка напряжений и заряда ==========================
   if (network.timeinfo.tm_sec % 5 == 0)
  {

          //  Читаем АЦП "reads"= раз и складываем результат в милливольтах
  float tempmVolt = 0;

         //  Настройка и инициализация АЦП
  adc1_config_width(ADC_WIDTH_BIT_12); 
  adc1_config_channel_atten(USER_ADC_CHAN, ADC_ATTEN_DB_12);

          //  Расчет характеристик АЦП т.е. коэффициенты усиления и смещения
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 0, &adc1_chars);

  for(uint8_t i = 0; i < reads; i++){
    tempmVolt += esp_adc_cal_raw_to_voltage(adc1_get_raw(USER_ADC_CHAN), &adc1_chars);
//    vTaskDelay(5);
                                           }

  float mVolt = (tempmVolt / reads) / 1000;		       //  Получаем средний результат в Вольтах

          //  Коррекция результата и получение напряжения батареи в Вольтах
  Volt = (mVolt + 0.0028 * mVolt * mVolt + 0.0096 * mVolt - 0.051) / (ADC_R2 / (ADC_R1 + ADC_R2)) + DELTA;
  if (Volt < 0) Volt = 0;

          // подготовка к контролю подключения зарядного устройства	- - - - - - - - - - - - - - - - - - - - - - - - - - - - 
  if (Volt3 > 0) {Volt = (Volt + Volt1 + Volt2 + Volt3) / 4;}
  Volt4 += Volt;
  Volt3 = Volt2; Volt2 = Volt1; Volt1 = Volt;
  t++;
          //	- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// ============ Рассчитываем остаток заряда в процентах ====================================
// Поиск индекса, соответствующего вольтажу. Индекс соответствует проценту оставшегося заряда

  uint8_t idx = 0;
  while (true) {
//================= Получение % оставшегося заряда ============================
    if (Volt < vs[idx]) {ChargeLevel =0; break;}
    if (Volt < vs[idx+1]) {mVolt = Volt - vs[idx]; ChargeLevel = idx * 5 + round(mVolt /((vs[idx+1] - vs[idx]) / 5 )); break;}
    else {idx++;}
                 }
    if (ChargeLevel < 0) ChargeLevel = 0; if (ChargeLevel > 100) ChargeLevel = 100;
// ===================== Отрисовка статической батарейки =================================
  setTextSize(BatFS);		// setTextSize(2)
  if (!Charging)		// Если не идёт зарядка
  {
  setCursor(BatX, BatY);

  if (				Volt >= 3.82) {setTextColor(BAT_COLORS[0], config.theme.background); print("\xA0\xA2\xA2\xA3");}    //больше 85% (6 квад.) - зел.
  if ((Volt < 3.82) && (Volt >= 3.72)) {setTextColor(BAT_COLORS[1], config.theme.background); print("\x9D\xA2\xA2\xA3");}    //от70 до 85% (5 квад.) - зел.
  if ((Volt < 3.72) && (Volt >= 3.61)) {setTextColor(BAT_COLORS[2], config.theme.background); print("\x9D\xA1\xA2\xA3");}    //от 55 до 70% (4 квад.) - зел.
  if ((Volt < 3.61) && (Volt >= 3.46)) {setTextColor(BAT_COLORS[3], config.theme.background); print("\x9D\x9E\xA2\xA3");}    //от 40 до 55% (3 квад.) - зел.
  if ((Volt < 3.46) && (Volt >= 3.33)) {setTextColor(BAT_COLORS[4], config.theme.background); print("\x9D\x9E\xA1\xA3");}    //от 25 до 40% (2 квад.) - зел.
  if ((Volt < 3.33) && (Volt >= 3.20)) {setTextColor(BAT_COLORS[5], config.theme.background); print("\x9D\x9E\x9E\xA3");} //от 10 до 25% (1 квад.) - жёлт.
  if ((Volt < 3.20) && (Volt >= 2.8)) {setTextColor(BAT_COLORS[6], config.theme.background); print("\x9D\x9E\x9E\x9F");}      //от 0 до 10% (0 квад.) - крас.
   }

  if (Volt < 2.8) {setTextColor(color565(255, 0, 0), config.theme.background);	}	// (0%) установка цвет Красный (Red)

// =============== Вывод цифровых значений напряжения  на дисплей ============================
  #ifndef HIDE_VOLT				// ========== Начало вывода напряжения
  setTextSize(VoltFS); 			// setTextSize(2)
  setCursor(VoltX, VoltY); 		// Установка координат для вывода напряжения
  printf("%.3fv", Volt);			// Вывод напряжения (текущим цветом)
  #endif 				// =========== Конец вывода напряжения

// =================== Вывод цифровых значений заряда на дисплей ============================
  setTextSize(ProcFS); 			// setTextSize(2)
  setCursor(ProcX, ProcY); 		// Установка координат для вывода
  printf("%3i%%", ChargeLevel); 	// Вывод процентов заряда батареи (текущим цветом) - формат вправо

//Serial.printf("#BATT#: ADC: %i reads, V-batt: %.3f v, Capacity: %i\n", reads, Volt, ChargeLevel);	// Вывод значений в COM-порт для контроля
  }

// ===================  Контроль подключения зарядного устройства ============================
   if (network.timeinfo.tm_sec % 60 == 0)
  {
    t -= 1;
    if (t > 0) Volt4 = Volt4 / t; // защита от деления на ноль
    if ((Volt5 > 0) && ((Volt4 - Volt5) > 0.001)) {
      Charging = true;						// установка признака, что подключено зарядное устройство
      setTextColor(color565(0, 255, 255), config.theme.background);			// Светло-синий (Cyan)
								}
    else {Charging = false;}					// установка признака, что зарядное устройство не подключено
    Volt5 = Volt4; Volt4 = 0; t = 1;
  }
          //	- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  }
  #endif			//   #ifndef BATTERY_OFF
/////////////////////////////////////////////////////////////////////////////////
}

void DspCore::_clockDate(){
  if(config.isScreensaver) return; 

  int16_t baseLineY = clockTop + CLOCK_DATE_Y_OFFSET;
  int16_t clearY = baseLineY - 22;
  int16_t clearH = 28;
  // Очищаем всю строку для предотвращения артефактов при изменении длины текста или при загрузке
  dsp.fillRect(0, clearY, width(), clearH, config.theme.background);

  // 1. ПОДГОТОВКА ДНЯ НЕДЕЛИ (Заглавная буква, UTF-8 кириллица)
  char dowStrRaw[32];
  int wday = network.timeinfo.tm_wday;
  if (wday < 0 || wday > 6) wday = 0;
  strlcpy(dowStrRaw, LANG::dowf[wday], sizeof(dowStrRaw));
  if ((uint8_t)dowStrRaw[0] == 0xD0 && (uint8_t)dowStrRaw[1] >= 0xB0 && (uint8_t)dowStrRaw[1] <= 0xBF) {
      dowStrRaw[1] -= 0x20;
  } else if ((uint8_t)dowStrRaw[0] == 0xD1 && (uint8_t)dowStrRaw[1] >= 0x80 && (uint8_t)dowStrRaw[1] <= 0x8F) {
      dowStrRaw[0] = 0xD0;
      dowStrRaw[1] += 0x20;
  }
  
  // 2. ПОДГОТОВКА ДАТЫ (Год 2 цифры)
  char dateStr[16];
  sprintf(dateStr, "%02d.%02d.%02d", network.timeinfo.tm_mday, network.timeinfo.tm_mon + 1, (network.timeinfo.tm_year + 1900) % 100);

  setFont(&DirectiveFour18);
  setTextSize(1);
  int16_t margin = TFT_FRAMEWDT + 1; // Небольшой отступ

  // День недели СЛЕВА
  setTextColor(config.theme.dow, config.theme.background);
  setCursor(margin, baseLineY); 
  print(utf8Rus(dowStrRaw, false));

  // Дата СПРАВА
  setTextColor(config.theme.date, config.theme.background);
  int16_t dx1, dy1;
  uint16_t dw, dh;
  getTextBounds(dateStr, 0, 0, &dx1, &dy1, &dw, &dh);
  setCursor(width() - dw - margin, baseLineY); 
  print(dateStr);

  setFont(NULL);
  strlcpy(_oldDateBuf, dateStr, sizeof(_oldDateBuf));
  _olddatewidth = dw;
  _olddateleft = width() - dw - margin;
}

void DspCore::_clockTime(){
    if(_oldtimeleft>0)
        dsp.fillRect(_oldtimeleft, clockTop-CLOCK_CLEAR_EXTRA_H-10, _oldtimewidth+CHARWIDTH*3*2+CLOCK_CLEAR_EXTRA_W-10, clockTimeHeight+CLOCK_CLEAR_EXTRA_H+CHARHEIGHT, config.theme.background);
    _timeleft = width()-clockRightSpace-CHARWIDTH*4*2-24-_timewidth;
    setTextSize(1);
    setFont(&DirectiveFour56);
    setTextColor(config.theme.clock, config.theme.background);
    setCursor(_timeleft+55,clockTop + CLOCK_TIME_Y_OFFSET-20);
    print(_timeBuf);
    setFont();
    strlcpy(_oldTimeBuf, _timeBuf, sizeof(_timeBuf));
    _oldtimewidth = _timewidth;
    _oldtimeleft = _timeleft;
    if(!config.isScreensaver) {
        // Логотип/обложка отрисовывается через updateCoverSlot() в printClock(), 
        // чтобы избежать двойной перерисовки и конфликтов координат.
    }
    int mon = network.timeinfo.tm_mon;
    if (mon < 0 || mon > 11) mon = 0;
    sprintf(_buffordate, "%2d.%s.%02d", network.timeinfo.tm_mday, LANG::mnths[mon], (network.timeinfo.tm_year + 1900) % 100);
    strlcpy(_dateBuf, utf8Rus(_buffordate, false), sizeof(_dateBuf));
    _datewidth = strlen(_dateBuf) * CHARWIDTH*2;
    _dateleft = width() - 10 - clockRightSpace - _datewidth;
}

void DspCore::printClock(uint16_t top, uint16_t rightspace, uint16_t timeheight, bool redraw){
   //TAKE_MUTEX();
  // Мьютекс НЕ берём: он не рекурсивный, а внутренние вызовы (drawRGBBitmap, fillRect и др.)
  // обращаются к SPI через startWrite() → TAKE_MUTEX(). Повторный захват вызывает
  // spi_device_acquire_bus assert → бесконечный crash-loop. (оригинальная строка оставлена)
  clockRightSpace = rightspace;
  clockTimeHeight = timeheight;
  // В скринсейвере перемещаем часы каждые ~20 секунд по вертикали, не выходя за экран.
  // ВАЖНО: очистку экрана при входе в скринсейвер делаем ОДИН раз (по флагу g_prevScreensaver),
  // а не при каждом redraw — иначе при частых перерисовках (мигание точек) цифры будут «дрыгаться».
  if(config.isScreensaver) {
    uint32_t now = millis();
    if(!g_prevScreensaver) {
      // Вход в скринсейвер — полная очистка экрана
      fillScreen(config.theme.background);
      g_ssTop = top; // стартовая позиция из конфигурации
      g_ssLastMoveMs = now;
    }
    uint32_t movePeriodMs = 20000; // 20 секунд между перемещениями
    if(now - g_ssLastMoveMs > movePeriodMs) {
      // Полная очистка перед перемещением, чтобы не осталось артефактов от старой позиции
      fillScreen(config.theme.background);
      int16_t margin = 10;
      int16_t maxTop = max<int16_t>(margin, height() - clockTimeHeight - CHARHEIGHT*2 - margin);
      if(maxTop < margin) maxTop = margin; // защита от некорректных размеров
      g_ssTop = random(margin, maxTop);
      g_ssLastMoveMs = now;
      redraw = true; // принудительная перерисовка часов на новой позиции
    }
    clockTop = g_ssTop;
  } else {
    clockTop = top;
    // Выход из скринсейвера: сбрасываем запомненный URL обложки,
    // чтобы updateCoverSlot перерисовал логотип/обложку на основном экране
    if(g_prevScreensaver) {
      g_lastDrawnCoverUrl = ""; // сброс — при следующем updateCoverSlot отрисуется заново
    }
  }
  g_prevScreensaver = config.isScreensaver;
  //strftime(_timeBuf, sizeof(_timeBuf), "%H:%M", &network.timeinfo);
  strftime(_timeBuf, sizeof(_timeBuf), "%H %M", &network.timeinfo);
  if(strcmp(_oldTimeBuf, _timeBuf)!=0 || redraw){
    _getTimeBounds();
    _clockTime();
/*    if(strcmp(_oldDateBuf, _dateBuf)!=0 || redraw) */_clockDate();

  }
  _clockSeconds();
  if(!config.isScreensaver) {
    // Обновляем обложку/логотип (только при смене URL или принудительном redraw)
    updateCoverSlot(redraw);
  }
  //GIVE_MUTEX();
  // Мьютекс не нужен — см. комментарий в начале функции
}

void DspCore::clearClock(){
 // TAKE_MUTEX();
  // Очищает область под временем, секундами, днями недели и линиями (но не датой)
  // Запасы по ширине и высоте берутся из CLOCK_CLEAR_EXTRA_W и CLOCK_CLEAR_EXTRA_H (см. displayNV3041Aconf.h)
  dsp.fillRect(_timeleft, clockTop-CLOCK_CLEAR_EXTRA_H-10, _timewidth+CHARWIDTH*3*2+CLOCK_CLEAR_EXTRA_W-10, clockTimeHeight+CLOCK_CLEAR_EXTRA_H+CHARHEIGHT+5, config.theme.background);
//GIVE_MUTEX();
}

void DspCore::startWrite(void) {
TAKE_MUTEX();
  //ILI9486_SPI::startWrite();
}

void DspCore::endWrite(void) { 
  //ILI9486_SPI::endWrite();
GIVE_MUTEX();
}
  
void DspCore::loop(bool force) {
}

void DspCore::charSize(uint8_t textsize, uint8_t& width, uint16_t& height){
  width = textsize * CHARWIDTH;
  height = textsize * CHARHEIGHT;
}

void DspCore::setTextSize(uint8_t s){
  Arduino_GFX::setTextSize(s);
}

void DspCore::flip(){
 TAKE_MUTEX();
  //setRotation(config.store.flipscreen?0:2);
  setRotation(config.store.flipscreen?1:3);
 GIVE_MUTEX();
}

void DspCore::invert(){
  TAKE_MUTEX();
  invertDisplay(config.store.invertdisplay);
  GIVE_MUTEX();
}

void DspCore::sleep(void) { 
  Serial.println("DspCore::sleep");
  _isAwake = false;
  displayOff();
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, LOW); // выключить подсветку при уходе в сон
  }

void DspCore::wake(void) {
  if(_isAwake) {  // Использовать переменную класса
    Serial.println("DspCore::wake - SKIPPED (already awake)");
    return;
  }
  Serial.println("DspCore::wake");
  _isAwake = true;  // Использовать переменную класса
    displayOn();
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
}

void DspCore::writePixel(int16_t x, int16_t y, uint16_t color) {
  if(_clipping){
    if ((x < _cliparea.left) || (x > _cliparea.left+_cliparea.width) || (y < _cliparea.top) || (y > _cliparea.top + _cliparea.height)) return;
  }
  Arduino_GFX::drawPixel(x, y, color);
}

void DspCore::writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if(_clipping){
    if ((x < _cliparea.left) || (x >= _cliparea.left+_cliparea.width) || (y < _cliparea.top) || (y > _cliparea.top + _cliparea.height))  return;
  }
  Arduino_GFX::writeFillRect(x, y, w, h, color);
}

void DspCore::setNumFont(){
  setFont(&DirectiveFour56);
  setTextSize(1);
  //  setFont(NULL);
  //  setTextSize(12);
}
#endif
