// GroqProvider.cpp — Groq Chat Completions (формат OpenAI)
#include "GroqProvider.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

extern bool isSafeForSSLForFacts();

FactResult GroqProvider::fetchFact(const String& artist, const String& title,
                                   const FactFetchContext* ctx) {
  FactResult result;
  result.success = false;

  if (!isSafeForSSLForFacts()) {
    result.errorMsg = "SSL not safe";
    return result;
  }

  if (_apiKey.length() == 0) {
    result.errorMsg = "No API key";
    Serial.println("[Groq] No API key");
    return result;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);
  client.setHandshakeTimeout(5);
  HTTPClient http;

  String url = "https://api.groq.com/openai/v1/chat/completions";

  String prompt = makeMusicFactUserPrompt(artist, title, ctx);

  JsonDocument requestDoc;
  requestDoc["model"] = "llama-3.1-8b-instant";
  requestDoc["max_tokens"] = 128;
  const bool iterative =
      ctx && ctx->factTotal >= 2 && ctx->factIndex1 > 1;
  requestDoc["temperature"] = iterative ? 0.55 : 0.35;

  JsonArray messages = requestDoc["messages"].to<JsonArray>();
  JsonObject systemMsg = messages.add<JsonObject>();
  systemMsg["role"] = "system";
  systemMsg["content"] =
      "You assist with brief music trivia. Reply only with verifiable, source-plausible information; "
      "never invent instruments, genres, charts, or session details. If uncertain for this exact "
      "track, say clearly that reliable public information is sparse.";

  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = prompt;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  Serial.printf("[Groq] Free Heap after JSON build: %u\n", ESP.getFreeHeap());

  {
    uint32_t t0 = millis();
    while (!isSafeForSSLForFacts() && (millis() - t0) < 4000) {
      delay(50);
    }
  }
  if (!isSafeForSSLForFacts()) {
    result.errorMsg = "SSL not safe (pre-POST)";
    Serial.printf("[Groq] Aborted before begin: unsafe heap/safety, heap=%u\n", ESP.getFreeHeap());
    return result;
  }

  Serial.println("[Groq] http.begin()...");
  if (!http.begin(client, url)) {
    result.errorMsg = "Unable to begin HTTP connection";
    Serial.println("[Groq] Unable to begin HTTP connection");
    return result;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + _apiKey);
  http.addHeader("Connection", "close");
  http.setTimeout(20000);
  http.setConnectTimeout(8000);

  Serial.printf("[Groq] POST start, body=%u bytes, heap=%u\n",
                static_cast<unsigned>(requestBody.length()), ESP.getFreeHeap());

  int httpCode = http.POST(requestBody);

  Serial.printf("[Groq] POST done, httpCode=%d, heap=%u\n", httpCode, ESP.getFreeHeap());

  if (httpCode <= 0) {
    result.errorMsg = "Connection failed: " + http.errorToString(httpCode) + " (" + String(httpCode) + ")";
    Serial.printf("[Groq] Connection failed, error: %s (%d)\n",
                  http.errorToString(httpCode).c_str(), httpCode);
    http.end();
    return result;
  }

  if (httpCode != HTTP_CODE_OK) {
    String errBody = http.getString();
    result.errorMsg = "HTTP error: " + String(httpCode);
    Serial.printf("[Groq] Error: %d\n", httpCode);
    if (errBody.length() > 0) {
      Serial.print("[Groq] Response: ");
      Serial.println(errBody.substring(0, 150));
    }
    http.end();
    return result;
  }

  String response = http.getString();
  http.end();

  JsonDocument responseDoc;
  DeserializationError error = deserializeJson(responseDoc, response);

  if (error) {
    result.errorMsg = "JSON Parse Error: " + String(error.c_str());
    Serial.print("[Groq] ");
    Serial.println(result.errorMsg);
    return result;
  }

  const char* factText = responseDoc["choices"][0]["message"]["content"];
  if (factText == nullptr) {
    result.errorMsg = "No fact text in response";
    Serial.println("[Groq] No fact text in response");
    return result;
  }

  String fact = String(factText);
  fact.trim();
  fact.replace("**", "");
  fact.replace("*", "");

  result.fact = fact;
  result.success = true;
  Serial.printf("[Groq] Result: %s\n", fact.c_str());
  return result;
}
