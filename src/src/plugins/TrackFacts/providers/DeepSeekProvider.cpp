// DeepSeekProvider.cpp — Провайдер фактов DeepSeek AI
// v0.4.0 — Рефакторинг: вынесено из TrackFacts.cpp
// Использует DeepSeek Chat API (формат OpenAI) для генерации коротких фактов.
// Требует API ключ (платный, но есть приветственный бонус).

#include "DeepSeekProvider.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// [FIX] Проверка безопасности SSL (heap, WiFi, cooldown)
extern volatile bool g_modeSwitching;
extern bool isSafeForSSLForFacts();

// ============================================================================
// Запрос факта к DeepSeek AI
// ============================================================================
FactResult DeepSeekProvider::fetchFact(const String& artist, const String& title) {
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
    Serial.println("[DeepSeek] No API key");
    return result;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Без проверки сертификата
  client.setTimeout(5);  // [FIX] 5 секунд таймаут на чтение/запись сокета
  client.setHandshakeTimeout(5);  // [FIX] 5 секунд лимит на SSL handshake (иначе зависает навсегда!)
  HTTPClient http;

  String url = "https://api.deepseek.com/chat/completions";

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

  // Формирование JSON-запроса в стиле OpenAI API
  JsonDocument requestDoc;
  requestDoc["model"] = "deepseek-chat";

  JsonArray messages = requestDoc["messages"].to<JsonArray>();
  JsonObject systemMsg = messages.add<JsonObject>();
  systemMsg["role"] = "system";
  systemMsg["content"] = "You are a helpful music expert.";

  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = prompt;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  Serial.printf("[DeepSeek] Free Heap after JSON build: %u\n", ESP.getFreeHeap());

  // Важно: ждать стабильной кучи ДО http.begin — begin+TLS буферы сильно съедают heap,
  // иначе цикл «pre-POST» срабатывал уже после begin и давал ложные отказы / тишину в логе.
  {
    uint32_t t0 = millis();
    while (!isSafeForSSLForFacts() && (millis() - t0) < 4000) {
      delay(50);
    }
  }
  if (!isSafeForSSLForFacts()) {
    result.errorMsg = "SSL not safe (pre-POST)";
    Serial.printf("[DeepSeek] Aborted before begin: unsafe heap/safety, heap=%u\n", ESP.getFreeHeap());
    return result;
  }

  Serial.println("[DeepSeek] http.begin()...");
  if (!http.begin(client, url)) {
    result.errorMsg = "Unable to begin HTTP connection";
    Serial.println("[DeepSeek] Unable to begin HTTP connection");
    return result;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + _apiKey);
  http.addHeader("Connection", "close");
  http.setTimeout(18000);
  http.setConnectTimeout(8000);

  Serial.printf("[DeepSeek] POST start, body=%u bytes, heap=%u\n",
                static_cast<unsigned>(requestBody.length()), ESP.getFreeHeap());

  int httpCode = http.POST(requestBody);

  Serial.printf("[DeepSeek] POST done, httpCode=%d, heap=%u\n", httpCode, ESP.getFreeHeap());

  if (httpCode <= 0) {
    result.errorMsg = "Connection failed: " + http.errorToString(httpCode) + " (" + String(httpCode) + ")";
    Serial.printf("[DeepSeek] Connection failed, error: %s (%d)\n",
                  http.errorToString(httpCode).c_str(), httpCode);
    http.end();
    return result;
  }

  if (httpCode != HTTP_CODE_OK) {
    String errBody = http.getString();
    result.errorMsg = "HTTP error: " + String(httpCode);
    Serial.printf("[DeepSeek] Error: %d\n", httpCode);
    if (errBody.length() > 0) {
      Serial.print("[DeepSeek] Response: ");
      Serial.println(errBody.substring(0, 150));
    }
    http.end();
    return result;
  }

  String response = http.getString();
  http.end();

  // Парсим JSON-ответ DeepSeek (формат OpenAI)
  JsonDocument responseDoc;
  DeserializationError error = deserializeJson(responseDoc, response);

  if (error) {
    result.errorMsg = "JSON Parse Error: " + String(error.c_str());
    Serial.print("[DeepSeek] "); Serial.println(result.errorMsg);
    return result;
  }

  const char* factText = responseDoc["choices"][0]["message"]["content"];
  if (factText == nullptr) {
    result.errorMsg = "No fact text in response";
    Serial.println("[DeepSeek] No fact text in response");
    return result;
  }

  // Очищаем текст факта от markdown-разметки
  String fact = String(factText);
  fact.trim();
  fact.replace("**", "");
  fact.replace("*", "");

  result.fact = fact;
  result.success = true;
  Serial.printf("[DeepSeek] Result: %s\n", fact.c_str());
  return result;
}
