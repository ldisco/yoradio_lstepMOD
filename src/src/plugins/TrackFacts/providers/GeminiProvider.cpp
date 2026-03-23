// GeminiProvider.cpp — Провайдер фактов Google Gemini AI
// v0.4.0 — Рефакторинг: вынесено из TrackFacts.cpp
// Использует Gemini 1.5 Flash API для генерации коротких интересных фактов о треках.
// Требует API ключ (бесплатный уровень доступен в Google AI Studio).

#include "GeminiProvider.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Модель Gemini для генерации контента
#define GEMINI_MODEL "gemini-1.5-flash"

// [FIX] Проверка безопасности SSL (heap, WiFi, cooldown)
extern volatile bool g_modeSwitching;
extern bool isSafeForSSLForFacts();

// ============================================================================
// Запрос факта к Google Gemini AI
// ============================================================================
FactResult GeminiProvider::fetchFact(const String& artist, const String& title) {
  FactResult result;
  result.success = false;

  // [FIX] Проверяем безопасность SSL
  if (!isSafeForSSLForFacts()) {
    result.errorMsg = "SSL not safe";
    return result;
  }

  // Проверка наличия API ключа
  if (_apiKey.length() == 0) {
    result.errorMsg = "No API key";
    Serial.println("[Gemini] No API key");
    return result;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Без проверки сертификата (ESP32 ограничения)
  client.setTimeout(5);  // [FIX] 5 секунд таймаут на чтение/запись сокета
  client.setHandshakeTimeout(5);  // [FIX] 5 секунд лимит на SSL handshake (иначе зависает навсегда!)
  HTTPClient http;

  // URL для Gemini 1.5 Flash API
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" +
               String(GEMINI_MODEL) + ":generateContent?key=" + _apiKey;

  // Формирование промпта на нужном языке
  String langInstruction;
  if (_language == ProviderLanguage::RUSSIAN) {
    langInstruction = "На русском языке";
  } else if (_language == ProviderLanguage::ENGLISH) {
    langInstruction = "In English";
  } else {
    langInstruction = "In Russian"; // AUTO -> русский по умолчанию
  }

  String prompt = langInstruction + ", напиши короткий интересный факт (17-20 слов) про музыкальную композицию: \"" +
                  artist + " - " + title + "\". Факт должен быть информативным и увлекательным, без вводных фраз.";

  // Подготовка JSON-запроса для Gemini API (v1beta)
  JsonDocument requestDoc;
  JsonArray contents = requestDoc["contents"].to<JsonArray>();
  JsonObject content = contents.add<JsonObject>();
  JsonArray parts = content["parts"].to<JsonArray>();
  JsonObject part = parts.add<JsonObject>();
  part["text"] = prompt;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  Serial.printf("[Gemini] Free Heap after JSON build: %u\n", ESP.getFreeHeap());

  {
    uint32_t t0 = millis();
    while (!isSafeForSSLForFacts() && (millis() - t0) < 4000) {
      delay(50);
    }
  }
  if (!isSafeForSSLForFacts()) {
    result.errorMsg = "SSL not safe (pre-POST)";
    Serial.printf("[Gemini] Aborted before begin: unsafe heap/safety, heap=%u\n", ESP.getFreeHeap());
    return result;
  }

  Serial.println("[Gemini] http.begin()...");
  if (!http.begin(client, url)) {
    result.errorMsg = "Unable to begin HTTP connection";
    Serial.println("[Gemini] Unable to begin HTTP connection");
    return result;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.setTimeout(18000);
  http.setConnectTimeout(8000);

  Serial.printf("[Gemini] POST start, body=%u bytes, heap=%u\n",
                static_cast<unsigned>(requestBody.length()), ESP.getFreeHeap());

  int httpCode = http.POST(requestBody);

  Serial.printf("[Gemini] POST done, httpCode=%d, heap=%u\n", httpCode, ESP.getFreeHeap());

  if (httpCode <= 0) {
    result.errorMsg = "Connection failed: " + http.errorToString(httpCode) + " (" + String(httpCode) + ")";
    Serial.printf("[Gemini] Connection failed, error: %s (%d)\n",
                  http.errorToString(httpCode).c_str(), httpCode);
    http.end();
    return result;
  }

  if (httpCode != HTTP_CODE_OK) {
    String errBody = http.getString();
    result.errorMsg = "HTTP error: " + String(httpCode);
    Serial.printf("[Gemini] Error: %d\n", httpCode);
    if (errBody.length() > 0) {
      Serial.print("[Gemini] Response: ");
      Serial.println(errBody.substring(0, 150));
    }
    http.end();
    return result;
  }

  String response = http.getString();
  http.end();

  // Парсим JSON-ответ Gemini
  JsonDocument responseDoc;
  DeserializationError error = deserializeJson(responseDoc, response);

  if (error) {
    result.errorMsg = "JSON Parse Error: " + String(error.c_str());
    Serial.print("[Gemini] "); Serial.println(result.errorMsg);
    return result;
  }

  const char* factText = responseDoc["candidates"][0]["content"]["parts"][0]["text"];
  if (factText == nullptr) {
    result.errorMsg = "No fact text in response";
    Serial.println("[Gemini] No fact text in response");
    return result;
  }

  // Очищаем текст факта от markdown-разметки
  String fact = String(factText);
  fact.trim();
  fact.replace("**", "");
  fact.replace("*", "");

  result.fact = fact;
  result.success = true;
  Serial.printf("[Gemini] Result: %s\n", fact.c_str());
  return result;
}
