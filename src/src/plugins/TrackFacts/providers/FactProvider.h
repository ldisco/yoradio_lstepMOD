// FactProvider.h — Базовый интерфейс для провайдеров фактов
// v0.4.0 — Рефакторинг: каждый провайдер в отдельном файле
// Все провайдеры наследуют этот интерфейс и реализуют метод fetchFact()
#ifndef FACT_PROVIDER_H
#define FACT_PROVIDER_H

#include <Arduino.h>
#include <vector>

// Результат запроса к провайдеру
struct FactResult {
  bool success;     // Успешно ли получен факт
  String fact;      // Текст факта
  String errorMsg;  // Сообщение об ошибке (если success == false)
};

// Перечисление языков для генерации факта
// (дублируется для удобства провайдеров, чтобы не тянуть весь TrackFacts.h)
enum class ProviderLanguage {
  RUSSIAN = 0,
  ENGLISH = 1,
  AUTO = 2
};

// ============================================================================
// Базовый класс-интерфейс для всех провайдеров фактов
// ============================================================================
class FactProvider {
public:
  virtual ~FactProvider() {}

  // Основной метод: получить факт о треке по имени артиста и названию
  // Возвращает FactResult с текстом факта или ошибкой
  virtual FactResult fetchFact(const String& artist, const String& title) = 0;

  // Имя провайдера для логов (например, "MusicBrainz", "Gemini" и т.д.)
  virtual const char* name() const = 0;

  // Требуется ли API ключ для работы провайдера
  virtual bool needsApiKey() const = 0;

  // Установка API ключа (для провайдеров, которым он нужен)
  virtual void setApiKey(const String& key) { _apiKey = key; }
  
  // Установка языка для генерации факта
  virtual void setLanguage(ProviderLanguage lang) { _language = lang; }

protected:
  String _apiKey;                              // API ключ (используется Gemini, DeepSeek, Last.fm)
  ProviderLanguage _language = ProviderLanguage::AUTO;  // Язык по умолчанию
};

#endif
