// yoRadio lstepMOD — smoke-test VPS-прокси TrackFacts (tools/test_proxy_esp)
// Не стоковый yoRadio: тот же формат URL, что GeminiProvider в lstepMOD.
// Wi‑Fi → HTTP :80 на ваш VPS (/health, /groq, /gemini) → Google Gemini / Groq.
// После OK — PROXY_BASE перенесите в lstepMOD WebUI → track facts → AI proxy URL.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

#ifndef PROXY_BASE
#define PROXY_BASE "http://ваш-сервер.example.com"
#endif

// Хост и порт после разбора PROXY_BASE (как normalizeProxyBase в yoRadio)
static String g_proxyHost;
static uint16_t g_proxyPort = 80;

static const char* GROQ_ARTIST   = "Ed Sheeran";
static const char* GROQ_TITLE    = "Beautiful";
static const char* GEMINI_ARTIST = "Adele";
static const char* GEMINI_TITLE  = "Hello";
static const char* GROQ_MODEL    = "openai/gpt-oss-20b";

static String g_geminiKey;
static String g_groqKey;
static bool g_groqDone = false;
static bool g_geminiDone = false;
static bool g_geminiScheduled = false;
static uint32_t g_geminiAtMs = 0;

// Разбор базового URL прокси: trim, убрать схему и хвостовые '/', host[:port]
static bool parseProxyBase(const char* raw, String& host, uint16_t& port) {
  String s(raw);
  s.trim();
  if (s.length() == 0) return false;
  if (s.startsWith("http://")) s = s.substring(7);
  else if (s.startsWith("https://")) s = s.substring(8);
  while (s.endsWith("/")) s.remove(s.length() - 1);
  const int colon = s.indexOf(':');
  if (colon > 0) {
    host = s.substring(0, colon);
    port = (uint16_t)s.substring(colon + 1).toInt();
    if (port == 0) port = 80;
  } else {
    host = s;
    port = 80;
  }
  return host.length() > 0;
}

static String makePrompt(const char* artist, const char* title) {
  String p;
  p.reserve(520);
  p  = "На русском языке один интересный факт из 19-22 слов про \"";
  p += artist;
  p += " - ";
  p += title;
  p += "\". Факт только про этого исполнителя и эту песню; не приписывай другой жанр, клуб, "
       "танцпол, ремикс-культуру или чарты, если из имени и названия это не следует. Пиши только "
       "то, в чём уверен по общеизвестным сведениям об этой записи; не додумывай. Если деталей "
       "мало — осторожный факт только про этого исполнителя или смысл названия, без общих клише. "
       "Не пиши, что информации нет. Без вводных фраз.";
  return p;
}

static bool connectWiFi() {
  Serial.printf("[WiFi] %s ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) {
      Serial.println("[WiFi] timeout");
      return false;
    }
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  Serial.printf("[WiFi] OK %s RSSI=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

static bool probeHealth() {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(5000);
  if (!http.begin(client, g_proxyHost.c_str(), g_proxyPort, "/health")) {
    return false;
  }
  const int code = http.GET();
  const String body = (code > 0) ? http.getString() : String();
  http.end();
  Serial.printf("[proxy] /health → %d '%s'\n", code, body.c_str());
  return code == 200;
}

static void printFact(const char* tag, const char* artist, const char* title, const String& fact) {
  Serial.printf("========== ФАКТ [%s] %s - %s ==========\n", tag, artist, title);
  Serial.println(fact);
  Serial.println("================================================");
}

static String makeGroqUserPrompt(const char* artist, const char* title) {
  String p;
  p.reserve(420);
  p  = "Задача: один короткий факт на русском, ровно 19-22 слова, про трек \"";
  p += artist;
  p += " - ";
  p += title;
  p += "\". Только про этого исполнителя и эту песню. Не выдумывай жанр/клуб/чарты. "
       "В ответе (content) напиши ТОЛЬКО текст факта — без кавычек, без вступлений, "
       "без англ. рассуждений. Если мало данных — осторожный факт про исполнителя или название.";
  return p;
}

static bool fetchGroqFact() {
  if (g_groqKey.length() == 0) {
    Serial.println("[Groq] нет GROQ_API_KEY в secrets.h — пропуск");
    return false;
  }

  JsonDocument requestDoc;
  requestDoc["model"] = GROQ_MODEL;
  requestDoc["temperature"] = 0.55;
  requestDoc["max_completion_tokens"] = 1024;
  requestDoc["reasoning_effort"] = "low";
  requestDoc["include_reasoning"] = false;

  JsonArray messages = requestDoc["messages"].to<JsonArray>();
  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = makeGroqUserPrompt(GROQ_ARTIST, GROQ_TITLE);

  String body;
  serializeJson(requestDoc, body);
  Serial.printf("[Groq] POST /groq %s - %s body=%u\n",
                GROQ_ARTIST, GROQ_TITLE, (unsigned)body.length());

  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(60000);
  http.setConnectTimeout(10000);
  if (!http.begin(client, g_proxyHost.c_str(), g_proxyPort, "/groq")) {
    Serial.println("[Groq] begin fail");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + g_groqKey);
  http.addHeader("Connection", "close");

  const uint32_t t0 = millis();
  const int code = http.POST(body);
  Serial.printf("[Groq] code=%d dt=%lums\n", code, (unsigned long)(millis() - t0));
  if (code <= 0) {
    Serial.printf("[Groq] err %s\n", http.errorToString(code).c_str());
    http.end();
    return false;
  }
  String response = http.getString();
  http.end();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Groq] HTTP %d: %s\n", code, response.substring(0, 250).c_str());
    return false;
  }

  JsonDocument responseDoc;
  if (deserializeJson(responseDoc, response)) {
    Serial.println("[Groq] JSON fail");
    return false;
  }

  const char* text = responseDoc["choices"][0]["message"]["content"];
  String fact = text ? String(text) : String();
  fact.trim();
  fact.replace("**", "");
  fact.replace("*", "");

  if (fact.length() == 0) {
    Serial.println("[Groq] content пуст");
    Serial.printf("[Groq] raw: %s\n", response.substring(0, 350).c_str());
    return false;
  }
  printFact("Groq", GROQ_ARTIST, GROQ_TITLE, fact);
  return true;
}

static bool fetchGeminiFact() {
  if (g_geminiKey.length() == 0) {
    Serial.println("[Gemini] нет GEMINI_API_KEY в secrets.h — пропуск");
    return false;
  }

  const String uri = String("/gemini?key=") + g_geminiKey;

  JsonDocument requestDoc;
  requestDoc["generationConfig"]["temperature"] = 0.32;
  requestDoc["generationConfig"]["thinkingConfig"]["thinkingBudget"] = 0;
  JsonArray contents = requestDoc["contents"].to<JsonArray>();
  JsonObject content = contents.add<JsonObject>();
  JsonArray parts = content["parts"].to<JsonArray>();
  JsonObject part = parts.add<JsonObject>();
  part["text"] = makePrompt(GEMINI_ARTIST, GEMINI_TITLE);

  String body;
  serializeJson(requestDoc, body);
  Serial.printf("[Gemini] POST /gemini %s - %s body=%u\n",
                GEMINI_ARTIST, GEMINI_TITLE, (unsigned)body.length());

  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(45000);
  http.setConnectTimeout(10000);
  if (!http.begin(client, g_proxyHost.c_str(), g_proxyPort, uri)) {
    Serial.println("[Gemini] begin fail");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  const uint32_t t0 = millis();
  const int code = http.POST(body);
  Serial.printf("[Gemini] code=%d dt=%lums\n", code, (unsigned long)(millis() - t0));
  if (code <= 0) {
    Serial.printf("[Gemini] err %s\n", http.errorToString(code).c_str());
    http.end();
    return false;
  }
  String response = http.getString();
  http.end();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Gemini] HTTP %d: %s\n", code, response.substring(0, 250).c_str());
    return false;
  }

  JsonDocument responseDoc;
  if (deserializeJson(responseDoc, response)) {
    Serial.println("[Gemini] JSON fail");
    return false;
  }
  const char* text = responseDoc["candidates"][0]["content"]["parts"][0]["text"];
  if (!text) {
    Serial.println("[Gemini] no text");
    return false;
  }
  String fact = text;
  fact.trim();
  fact.replace("**", "");
  fact.replace("*", "");
  printFact("Gemini", GEMINI_ARTIST, GEMINI_TITLE, fact);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== yoRadio test_proxy_esp: Groq + Gemini через VPS :80 ===");

  if (!parseProxyBase(PROXY_BASE, g_proxyHost, g_proxyPort)) {
    Serial.println("[proxy] PROXY_BASE пуст или неверный — см. include/secrets.h");
    return;
  }
  Serial.printf("[proxy] base=%s host=%s port=%u\n", PROXY_BASE, g_proxyHost.c_str(), g_proxyPort);

  g_geminiKey = String(GEMINI_API_KEY);
  g_geminiKey.trim();
  g_groqKey = String(GROQ_API_KEY);
  g_groqKey.trim();
  Serial.printf("[keys] gemini=%s groq=%s\n",
                g_geminiKey.length() ? "ok" : "EMPTY",
                g_groqKey.length() ? "ok" : "EMPTY");

  if (!connectWiFi()) return;
  if (!probeHealth()) {
    Serial.println("[proxy] /health fail — сначала SETUP_VPS_PROXY.md на VPS");
    return;
  }

  g_groqDone = fetchGroqFact();
  g_geminiScheduled = true;
  g_geminiAtMs = millis() + 5000;
  Serial.println("[Gemini] запуск через 5с...");
}

void loop() {
  const uint32_t now = millis();

  if (g_geminiScheduled && !g_geminiDone && (int32_t)(now - g_geminiAtMs) >= 0) {
    g_geminiDone = fetchGeminiFact();
    g_geminiScheduled = false;
  }

  static uint32_t lastHb = 0;
  if (now - lastHb >= 10000) {
    lastHb = now;
    Serial.printf("[hb] up=%lus groq=%s gemini=%s heap=%u\n",
                  now / 1000UL,
                  g_groqDone ? "OK" : "no",
                  g_geminiDone ? "OK" : (g_geminiScheduled ? "wait" : "no"),
                  ESP.getFreeHeap());
  }
  delay(50);
}
