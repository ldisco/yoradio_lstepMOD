// DeepSeekProvider.cpp — Провайдер фактов DeepSeek AI
// v0.4.0 — Рефакторинг: вынесено из TrackFacts.cpp
// Использует DeepSeek Chat API (формат OpenAI) для генерации коротких фактов.
// Требует API ключ (платный, но есть приветственный бонус).
//
// При непустом _proxyBase — HTTP POST на {proxy}/deepseek (VPS nginx), иначе прямой HTTPS.

#include "DeepSeekProvider.h"
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

extern volatile bool g_modeSwitching;
extern bool isSafeForSSLForFacts();
extern bool isSafeForFactsHttpProxy();

// Нормализация базового URL прокси (как в GroqProvider / GeminiProvider).
static String normalizeProxyBase(const String& raw) {
  String base = raw;
  base.trim();
  while (base.length() > 0 && base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  if (base.length() == 0) return base;
  if (base.indexOf("://") < 0) {
    base = String("http://") + base;
  }
  return base;
}

// ============================================================================
// Запрос факта к DeepSeek AI
// ============================================================================
FactResult DeepSeekProvider::fetchFact(const String& artist, const String& title,
                                       const FactFetchContext* ctx) {
  FactResult result;
  result.success = false;

  const bool useProxyEarly = (normalizeProxyBase(_proxyBase).length() > 0);
  const bool netSafe = useProxyEarly ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts();
  if (!netSafe) {
    result.errorMsg = useProxyEarly
        ? "Сейчас нельзя запросить факт. Подождите и повторите."
        : "SSL not safe";
    return result;
  }

  if (_apiKey.length() == 0) {
    result.errorMsg = "No API key";
    Serial.println("[DeepSeek] No API key");
    return result;
  }

  const String proxyBase = normalizeProxyBase(_proxyBase);
  const bool useProxy = (proxyBase.length() > 0);

  // TEMP: proxy → {base}/deepseek; иначе прямой HTTPS к DeepSeek.
  String url;
  if (useProxy) {
    url = proxyBase + "/deepseek";
  } else {
    url = "https://api.deepseek.com/chat/completions";
  }

  String prompt = makeMusicFactUserPrompt(artist, title, ctx);

  JsonDocument requestDoc;
  requestDoc["model"] = "deepseek-chat";
  const bool iterative =
      ctx && ctx->factTotal >= 2 && ctx->factIndex1 > 1;
  requestDoc["temperature"] = iterative ? 0.45 : 0.32;

  JsonArray messages = requestDoc["messages"].to<JsonArray>();
  JsonObject systemMsg = messages.add<JsonObject>();
  systemMsg["role"] = "system";
  systemMsg["content"] =
      "Brief music trivia for the exact track given. One fact; must match artist and title. "
      "Never invent genre or scene. Never say information is missing.";

  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = prompt;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  Serial.printf("[DeepSeek] Free Heap after JSON build: %u\n", ESP.getFreeHeap());
  Serial.printf("[DeepSeek] mode=%s url=%s\n",
                useProxy ? "proxy-http" : "direct-https",
                useProxy ? url.c_str() : "api.deepseek.com");

  {
    uint32_t t0 = millis();
    while (!(useProxyEarly ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts()) &&
           (millis() - t0) < 4000) {
      delay(50);
    }
  }
  if (!(useProxyEarly ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts())) {
    result.errorMsg = useProxyEarly
        ? "Сейчас нельзя запросить факт. Подождите и повторите."
        : "SSL not safe (pre-POST)";
    Serial.printf("[DeepSeek] Aborted before begin: unsafe heap/safety, heap=%u\n", ESP.getFreeHeap());
    return result;
  }

  HTTPClient http;
  http.setReuse(false);

  WiFiClient httpClient;
  WiFiClientSecure httpsClient;
  bool beginOk = false;

  Serial.println("[DeepSeek] http.begin()...");
  if (useProxy) {
    httpClient.setTimeout(5);
    beginOk = http.begin(httpClient, url);
  } else {
    httpsClient.setInsecure();
    httpsClient.setTimeout(5);
    httpsClient.setHandshakeTimeout(5);
    beginOk = http.begin(httpsClient, url);
  }

  if (!beginOk) {
    result.errorMsg = "Unable to begin HTTP connection";
    Serial.println("[DeepSeek] Unable to begin HTTP connection");
    return result;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + _apiKey);
  http.addHeader("Connection", "close");
  http.setTimeout(useProxy ? 60000 : 18000);
  http.setConnectTimeout(useProxy ? 10000 : 8000);

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

  String fact = String(factText);
  fact.trim();
  fact.replace("**", "");
  fact.replace("*", "");

  result.fact = fact;
  result.success = true;
  Serial.printf("[DeepSeek] Result: %s\n", fact.c_str());
  return result;
}
