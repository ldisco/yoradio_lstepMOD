// GeminiProvider.cpp — Провайдер фактов Google Gemini AI
// v0.4.0 — Рефакторинг: вынесено из TrackFacts.cpp
// Использует Gemini API для генерации коротких интересных фактов о треках.
// Требует API ключ (бесплатный уровень доступен в Google AI Studio).
// При непустом _proxyBase: HTTP POST на {proxy}/gemini?key=... (VPS nginx → Google).
// При пустом proxy: прямое HTTPS к generativelanguage.googleapis.com.

#include "GeminiProvider.h"
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Модель для прямого HTTPS; при работе через /gemini модель задаёт nginx на VPS.
#define GEMINI_MODEL "gemini-2.5-flash"

// Гейты сети: прямой HTTPS — isSafeForSSLForFacts; HTTP-прокси — isSafeForFactsHttpProxy().
extern volatile bool g_modeSwitching;
extern bool isSafeForSSLForFacts();
extern bool isSafeForFactsHttpProxy();

// Нормализация базового URL прокси: trim, убрать хвостовые '/', добавить http:// если нет схемы.
static String normalizeProxyBase(const String& raw) {
  String base = raw;
  base.trim();
  while (base.length() > 0 && base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  if (base.length() == 0) return base;
  // Без схемы ESP HTTPClient может не разобрать host — добавляем http://.
  if (base.indexOf("://") < 0) {
    base = String("http://") + base;
  }
  return base;
}

// ============================================================================
// Запрос факта к Google Gemini AI
// ============================================================================
FactResult GeminiProvider::fetchFact(const String& artist, const String& title,
                                       const FactFetchContext* ctx) {
  FactResult result;
  result.success = false;

  const bool useProxyEarly = (normalizeProxyBase(_proxyBase).length() > 0);
  const bool netSafe = useProxyEarly ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts();
  if (!netSafe) {
    result.errorMsg = useProxyEarly ? "HTTP proxy not safe" : "SSL not safe";
    return result;
  }

  // Проверка наличия API ключа
  if (_apiKey.length() == 0) {
    result.errorMsg = "No API key";
    Serial.println("[Gemini] No API key");
    return result;
  }

  const String proxyBase = normalizeProxyBase(_proxyBase);
  const bool useProxy = (proxyBase.length() > 0);

  // URL: proxy → {base}/gemini?key= (path без ':' — требование ESP HTTPClient + nginx location = /gemini)
  // иначе прямое HTTPS к Google с моделью из GEMINI_MODEL.
  String url;
  if (useProxy) {
    url = proxyBase + "/gemini?key=" + _apiKey;
  } else {
    url = String("https://generativelanguage.googleapis.com/v1beta/models/") +
          String(GEMINI_MODEL) + ":generateContent?key=" + _apiKey;
  }

  String prompt = makeMusicFactUserPrompt(artist, title, ctx);

  // Подготовка JSON-запроса для Gemini API (v1beta)
  JsonDocument requestDoc;
  const bool iterative =
      ctx && ctx->factTotal >= 2 && ctx->factIndex1 > 1;
  // Ниже temperature — точнее по жанру, меньше «клуб/танцпол» не к месту.
  requestDoc["generationConfig"]["temperature"] = iterative ? 0.45 : 0.32;
  // Gemini 2.5: без thinkingBudget ответ уходит в reasoning / дольше; 0 = сразу content.
  requestDoc["generationConfig"]["thinkingConfig"]["thinkingBudget"] = 0;
  JsonArray contents = requestDoc["contents"].to<JsonArray>();
  JsonObject content = contents.add<JsonObject>();
  JsonArray parts = content["parts"].to<JsonArray>();
  JsonObject part = parts.add<JsonObject>();
  part["text"] = prompt;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  Serial.printf("[Gemini] Free Heap after JSON build: %u\n", ESP.getFreeHeap());
  Serial.printf("[Gemini] mode=%s urlHost=%s\n",
                useProxy ? "proxy-http" : "direct-https",
                useProxy ? proxyBase.c_str() : "generativelanguage.googleapis.com");

  {
    uint32_t t0 = millis();
    while (!(useProxyEarly ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts()) &&
           (millis() - t0) < 4000) {
      delay(50);
    }
  }
  if (!(useProxyEarly ? isSafeForFactsHttpProxy() : isSafeForSSLForFacts())) {
    result.errorMsg = useProxyEarly ? "HTTP proxy not safe (pre-POST)" : "SSL not safe (pre-POST)";
    Serial.printf("[Gemini] Aborted before begin: unsafe heap/safety, heap=%u\n", ESP.getFreeHeap());
    return result;
  }

  HTTPClient http;
  http.setReuse(false);

  // Клиенты живут в этом scope до http.end().
  WiFiClient httpClient;
  WiFiClientSecure httpsClient;
  bool beginOk = false;

  Serial.println("[Gemini] http.begin()...");
  if (useProxy) {
    // HTTP к своему VPS (без TLS на участке ESP→VPS).
    httpClient.setTimeout(5);
    beginOk = http.begin(httpClient, url);
  } else {
    // Прямое HTTPS к Google.
    httpsClient.setInsecure(); // Без проверки сертификата (ESP32 ограничения)
    httpsClient.setTimeout(5);
    httpsClient.setHandshakeTimeout(5);
    beginOk = http.begin(httpsClient, url);
  }

  if (!beginOk) {
    result.errorMsg = "Unable to begin HTTP connection";
    Serial.println("[Gemini] Unable to begin HTTP connection");
    return result;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");
  // Proxy: nginx→Google может быть дольше; direct оставляем прежние лимиты с запасом.
  http.setTimeout(useProxy ? 45000 : 18000);
  http.setConnectTimeout(useProxy ? 10000 : 8000);

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
