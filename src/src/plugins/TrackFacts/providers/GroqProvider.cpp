// GroqProvider.cpp — Groq Chat Completions (формат OpenAI)
// Stage 2: при непустом _proxyBase — HTTP POST на {proxy}/groq (VPS nginx → api.groq.com).
// При пустом proxy — прямое HTTPS (как раньше; в РФ/СНГ часто недоступно без VPS).

#include "GroqProvider.h"
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Модель на VPS задаётся в теле запроса; nginx только проксирует /groq.
#define GROQ_MODEL "openai/gpt-oss-20b"

extern bool isSafeForSSLForFacts();
extern bool isSafeForFactsHttpProxy();

// Нормализация базового URL прокси (как в GeminiProvider).
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

FactResult GroqProvider::fetchFact(const String& artist, const String& title,
                                   const FactFetchContext* ctx) {
  FactResult result;
  result.success = false;

  const bool useProxyEarly = (normalizeProxyBase(_proxyBase).length() > 0);
  const bool netSafe = useProxyEarly ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts();
  if (!netSafe) {
    result.errorMsg = useProxyEarly ? "HTTP proxy not safe" : "SSL not safe";
    return result;
  }

  if (_apiKey.length() == 0) {
    result.errorMsg = "No API key";
    Serial.println("[Groq] No API key");
    return result;
  }

  const String proxyBase = normalizeProxyBase(_proxyBase);
  const bool useProxy = (proxyBase.length() > 0);

  // URL: proxy → {base}/groq; иначе прямой HTTPS к Groq OpenAI-compatible API.
  String url;
  if (useProxy) {
    url = proxyBase + "/groq";
  } else {
    url = "https://api.groq.com/openai/v1/chat/completions";
  }

  String prompt = makeMusicFactUserPrompt(artist, title, ctx);

  JsonDocument requestDoc;
  requestDoc["model"] = GROQ_MODEL;
  const bool iterative =
      ctx && ctx->factTotal >= 2 && ctx->factIndex1 > 1;
  // Параметры как в tools/test_proxy_esp (exp_proxy_esp); для 2+ факта чуть выше temperature.
  requestDoc["temperature"] = iterative ? 0.48 : 0.55;
  requestDoc["max_completion_tokens"] = 1024;
  requestDoc["reasoning_effort"] = "low";
  requestDoc["include_reasoning"] = false;

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

  Serial.printf("[Groq] Free Heap after JSON build: %u\n", ESP.getFreeHeap());
  Serial.printf("[Groq] mode=%s urlHost=%s\n",
                useProxy ? "proxy-http" : "direct-https",
                useProxy ? proxyBase.c_str() : "api.groq.com");

  {
    uint32_t t0 = millis();
    while (!(useProxyEarly ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts()) &&
           (millis() - t0) < 4000) {
      delay(50);
    }
  }
  if (!(useProxyEarly ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts())) {
    result.errorMsg = useProxyEarly ? "HTTP proxy not safe (pre-POST)" : "SSL not safe (pre-POST)";
    Serial.printf("[Groq] Aborted before begin: unsafe heap/safety, heap=%u\n", ESP.getFreeHeap());
    return result;
  }

  HTTPClient http;
  http.setReuse(false);

  WiFiClient httpClient;
  WiFiClientSecure httpsClient;
  bool beginOk = false;

  Serial.println("[Groq] http.begin()...");
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
    Serial.println("[Groq] Unable to begin HTTP connection");
    return result;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + _apiKey);
  http.addHeader("Connection", "close");
  http.setTimeout(useProxy ? 60000 : 20000);
  http.setConnectTimeout(useProxy ? 10000 : 8000);

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
    if (response.length() > 0) {
      Serial.print("[Groq] raw: ");
      Serial.println(response.substring(0, 200));
    }
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
