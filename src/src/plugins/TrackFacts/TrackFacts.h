// TrackFacts.h - Плагин для отображения интересных фактов о песнях
// v0.4.1 - iTunes по умолчанию (позиция 2), MusicBrainz перенесён на позицию 4
// Ограничение повторных запросов для метаданных-провайдеров. Max 3 факта.
#ifndef TrackFacts_H
#define TrackFacts_H

#include "../../pluginsManager/pluginsManager.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <vector>
#include <freertos/semphr.h>   // [v0.3.23] Мьютекс для защиты потоков
#include "../../../myoptions.h" // [v0.3.23] Подключаем настройки прокси

// [v0.4.0] Подключаем базовый интерфейс провайдеров
#include "providers/FactProvider.h"

// Источники данных для получения фактов
// [v0.4.1] iTunes и MusicBrainz поменяны местами: iTunes — по умолчанию (2)
enum class FactSource {
  GEMINI = 0,       // Google Gemini Flash (бесплатный уровень)
  DEEPSEEK = 1,     // DeepSeek AI
  ITUNES = 2,       // [v0.4.1] iTunes Search API (бесплатно, по умолчанию)
  LASTFM = 3,       // База Last.fm (нужен ключ)
  MUSICBRAINZ = 4,  // [v0.4.1] MusicBrainz (без ключа, через VPS relay)
  CUSTOM = 5
};

// Выбор языка для генерации факта
enum class FactLanguage {
  RUSSIAN = 0,
  ENGLISH = 1,
  AUTO = 2
};

class TrackFacts : public Plugin {
public:
  TrackFacts();
  void on_ticker() override;           // Вызывается каждую секунду из основного цикла
  void on_track_change() override;     // Вызывается при смене трека плеером (по метаданным)
  void on_station_change() override;   // Вызывается при смене станции (next/prev/выбор в списке)
  void on_setup() override;            // Инициализация при старте системы

  // Методы для изменения конфигурации извне (например, из web-сервера)
  void setFactSource(FactSource source);
  void setProvider(uint8_t provider);  // Установка провайдера по индексу из веб-морды
  void setGeminiApiKey(const String& key);
  void setLanguage(FactLanguage lang);
  void setFactCount(uint8_t count);    // Настройка количества фактов на один трек (1-5)
  void setEnabled(bool enabled);
  bool requestManualFact(String* rejectReason = nullptr); // Ручной запрос факта из WebUI (true = запрос поставлен в очередь)
  bool isRequestPending() const { return manualRequestPending || isRequestInProgress; }
  
  // [v0.4.2] Система уведомлений (Toast)
  // Отправляет статусное сообщение в WebUI через WebSocket
  void sendStatus(const char* msg, bool isError = false);

  // Методы для получения текущих настроек
  bool isEnabled() const { return _enabled; }
  FactLanguage getLanguage() const { return currentLanguage; }
  uint8_t getFactsCount() const { return factsPerTrack; }
  bool isRequestActive() const { return isRequestInProgress; }  // [FIX] Для changeMode() — ждём завершения SSL

private:
  String lastTitle;       // Оригинальный заголовок для отслеживания смены трека
  String lastArtist;      // Распарсенное имя исполнителя
  String lastParsedTitle; // Распарсенное название песни
  std::vector<String> currentFacts;    // Вектор накопленных фактов для текущего трека
  SemaphoreHandle_t _factsMutex;       // [v0.3.23] Мьютекс для безопасного доступа к текущим фактам
  uint8_t currentFactIndex;
  uint32_t factLoadTime;  // Время последнего запроса (для таймеров задержки)
  uint32_t lastTickerCheck;
  bool factLoaded;        // Флаг, что все факты для трека загружены (кэш или API)
  bool isRequestInProgress; // Флаг активного HTTP-запроса
  bool _enabled;

  // Параметры для задачи FreeRTOS (сетевые запросы в отдельном потоке)
  struct TaskParams {
      String artist;
      String title;
      TrackFacts* instance;
      /** Снимок _factsSslEpoch на момент fetchFact; при смене станции/трека эпоха растёт — результат отбрасываем. */
      uint32_t sslEpoch;
  };
  static void fetchTask(void* parameter);

  FactSource currentSource;           // Провайдер, выбранный пользователем
  FactSource _effectiveSource;        // Реально используемый провайдер (может отличаться при failover)
  FactLanguage currentLanguage;
  String geminiApiKey;
  uint8_t factsPerTrack;              // Сколько всего фактов нужно собрать для одного трека
  uint16_t factRequestCounter;
  uint32_t trackChangeMs;        // Время начала текущего трека
  bool autoRequestDone;          // Был ли выполнен авто-запрос для трека
  bool manualRequestPending;     // Ожидается ручной запрос
  uint32_t manualRequestAtMs;    // Когда можно делать ручной запрос
  uint32_t manualRequestDeadlineMs; // До какого момента ручной запрос может ждать условий, иначе отменяем с ошибкой
  uint32_t manualStableSinceMs;     // С какого времени поток считается непрерывно стабильным для low-buffer manual retry
  /** Установлен в on_ticker перед fetchFact: true = ручной запрос (не подменять AI→iTunes на FLAC). */
  bool _pendingFetchIsManual;
  /** Увеличивается при смене станции/трека; не сбрасывать isRequestInProgress там — иначе CoverDL параллелит TLS с FactsTask → -32512. */
  uint32_t _factsSslEpoch;

  // === Failover (Отказоустойчивость) ===
  // Если пользователь выбрал провайдера, требующего ключ, но ключ не задан,
  // система автоматически переключается на MusicBrainz и уведомляет пользователя.
  bool _failoverActive;               // Флаг: работает ли failover (переключено на MusicBrainz)
  bool _failoverNotified;             // Флаг: уведомление уже показано пользователю (показываем 1 раз)
  uint32_t _suppressFailoverToastUntilMs; // Не показывать тост «нет ключа» до этого времени (ввод ключа в настройках)
  void checkFailover();               // Проверка необходимости failover перед каждым запросом

  // [v0.4.1] Массив провайдеров — создаются в конструкторе, индексированы по FactSource
  FactProvider* _providers[6];         // Gemini=0, DeepSeek=1, iTunes=2, LastFM=3, MusicBrainz=4, Custom=5
  FactProvider* getProvider(FactSource src); // Получить провайдер по FactSource
  void syncProviderSettings();         // Синхронизировать API ключ и язык со всеми провайдерами

  uint32_t lastStatusMs;               // Время последней отправки статуса (анти-спам)

  // === Файловый кэш (File-based Cache) ===
  // Каждый трек хранится в отдельном .txt файле в папке /data/facts/
  // Имя файла — DJB2 хэш от нормализованного ключа "artist|title"
  // Индексный файл index.txt отслеживает все файлы для LRU-ротации
  static constexpr const char* FACTS_CACHE_DIR  = "/data/facts";
  static constexpr const char* FACTS_INDEX_PATH = "/data/facts/index.txt";
#ifdef FACTS_CACHE_MAX_BYTES
  static constexpr size_t FACTS_CACHE_MAX_BYTES_LIMIT = (size_t)FACTS_CACHE_MAX_BYTES;
#else
  static constexpr size_t FACTS_CACHE_MAX_BYTES_LIMIT = 1 * 1024 * 1024; // 1 МБ лимит на папку
#endif

  // DJB2 хэш для генерации имени файла (аналогично coverHash из config.cpp)
  static uint32_t djb2Hash(const String& str);

  // Генерация полного пути к файлу кэша: /data/facts/<hash>.txt
  String factsCachePath(const String& trackKey);

  // Генерация нормализованного ключа трека: "artist|title" в нижнем регистре
  String generateTrackKey(const String& artist, const String& title);

  // Создание папки кэша, если она не существует
  void ensureFactsDir();

  // Чтение фактов из файлового кэша (возвращает true если найдены)
  // Читает файл "на лету", не загружая весь кэш в RAM
  bool getFactsFromCache(const String& trackKey, std::vector<String>& facts);

  // Сохранение нового факта в файловый кэш (дописывает строку в файл трека)
  // Проверяет дубликаты, ограничивает до 10 фактов на трек
  void saveFactToCache(const String& trackKey, const String& fact);

  // Регистрация файла в индексе + ротация LRU при превышении лимита 1 МБ
  void registerFactsCacheFile(const String& path);

  // Обновляет глобальную переменную ::currentFact для отображения в WebUI и на экране
  void updateDisplayFact(const String& fact);

  // Определяет, нужно ли кэшировать факт от данного провайдера
  // Кэшируем только "дорогие" факты от AI (Gemini, DeepSeek)
  bool shouldCacheFact(FactSource source) const;

  // [v0.4.0] Основной метод запроса — делегирует провайдеру через FactProvider
  void fetchFact(const String& artist, const String& title);

  // Обновляет окно "стабильного потока" для ручного low-buffer запроса.
  // Логика: если поток WEB играет, буфер >= порога и есть регулярная активность, накапливаем время стабильности.
  // Если любое условие нарушено — окно стабильности сбрасывается.
  void updateManualStabilityWindow(uint32_t now);

  // Проверка, можно ли запускать ручной запрос в режиме low-buffer retry.
  // Возвращает true, если буфер >= 5% и поток стабилен непрерывно 10 секунд.
  // При rejectReason != nullptr заполняет человекочитаемую причину отказа для toast в WebUI.
  bool canRunManualLowBufferRetry(uint32_t now, String* rejectReason = nullptr) const;

  // Служебные методы
  void processMetadata();
  String getLanguageCode();
};

#endif
