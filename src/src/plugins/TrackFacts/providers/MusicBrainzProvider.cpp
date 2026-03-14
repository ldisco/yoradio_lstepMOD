// MusicBrainzProvider.cpp — Провайдер фактов MusicBrainz
// v0.4.0 — Рефакторинг: вынесено из TrackFacts.cpp
// Бесплатный провайдер, без API ключа. Поддерживает VPS Relay (socat TCP->SSL)
// и прямой запрос к musicbrainz.org как fallback.

#include "MusicBrainzProvider.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "../../../../myoptions.h" // Настройки TF_MB_RELAY_IP / TF_MB_RELAY_PORT

// [FIX] Проверка безопасности SSL (heap, WiFi, cooldown)
extern volatile bool g_modeSwitching;
extern bool isSafeForSSLForFacts();

// ============================================================================
// Основной метод: пробуем VPS Relay, при неудаче — прямой запрос
// ============================================================================
FactResult MusicBrainzProvider::fetchFact(const String& artist, const String& title) {
  // [FIX] Проверяем безопасность SSL
  if (!isSafeForSSLForFacts()) {
    FactResult result;
    result.success = false;
    result.errorMsg = "SSL not safe";
    Serial.println("[MusicBrainz] Skipped - SSL not safe");
    return result;
  }

#ifdef TF_MB_RELAY_IP
  if (strlen(TF_MB_RELAY_IP) > 0) {
    Serial.printf("[MusicBrainz] Using VPS Relay: %s:%d\n", TF_MB_RELAY_IP, TF_MB_RELAY_PORT);
    FactResult result = fetchViaProxy(artist, title);
    if (result.success) return result;
    // Если прокси не сработал — пробуем прямой запрос
    Serial.println("[MusicBrainz] VPS Relay failed, trying direct...");
  }
#endif
  return fetchDirect(artist, title);
}

// ============================================================================
// Формирование текста факта на выбранном языке
// ============================================================================
String MusicBrainzProvider::buildFactString(const String& date, const String& country, const String& tagsList) {
  String fact = "";
  if (_language == ProviderLanguage::RUSSIAN || _language == ProviderLanguage::AUTO) {
    fact = "MB: Релиз " + date;
    if (country != "?") fact += ", " + country;
    if (tagsList.length() > 0) fact += ". Жанр: " + tagsList;
  } else {
    fact = "MB: Released " + date;
    if (country != "?") fact += ", " + country;
    if (tagsList.length() > 0) fact += ". Genre: " + tagsList;
  }
  return fact;
}

// ============================================================================
// [v0.3.23] Запрос через VPS Relay (TCP -> SSL прокси)
// ESP32 подключается по простому TCP к VPS, который пересылает в MusicBrainz через SSL
// ============================================================================
FactResult MusicBrainzProvider::fetchViaProxy(const String& artist, const String& title) {
  FactResult result;
  result.success = false;

  WiFiClient vpsClient;
  vpsClient.setTimeout(5000); // 5 секунд таймаут

  if (!vpsClient.connect(TF_MB_RELAY_IP, TF_MB_RELAY_PORT)) {
    result.errorMsg = "VPS Connect failed";
    Serial.println("[MusicBrainz] VPS Connect failed.");
    return result;
  }

  // Кодируем параметры для URL
  String a = artist; a.replace(" ", "%20"); a.replace("\"", "");
  String t = title;  t.replace(" ", "%20"); t.replace("\"", "");
  String queryPar = "artist:\"" + a + "\"%20AND%20recording:\"" + t + "\"";
  String path = "/ws/2/recording/?query=" + queryPar + "&fmt=json&limit=1";

  // Формируем HTTP-запрос вручную
  String request = "GET " + path + " HTTP/1.1\r\n" +
                   "Host: musicbrainz.org\r\n" +
                   "User-Agent: YoRadio/0.8\r\n" +
                   "Accept: application/json\r\n" +
                   "Connection: close\r\n\r\n";

  vpsClient.print(request);

  // Читаем ответ, пропуская HTTP-заголовки
  bool isHeader = true;
  String response = "";
  uint32_t timeout = millis();

  while (vpsClient.connected() || vpsClient.available()) {
    if (millis() - timeout > 5000) break; // Таймаут чтения 5 сек

    if (vpsClient.available()) {
      if (isHeader) {
        String line = vpsClient.readStringUntil('\n');
        if (line == "\r" || line == "") isHeader = false; // Пустая строка — конец заголовков
      } else {
        response += vpsClient.readString();
      }
      timeout = millis();
    }
  }
  vpsClient.stop();

  if (response.length() == 0) {
    result.errorMsg = "Empty response from VPS";
    return result;
  }

  // Парсим JSON-ответ
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);

  if (error) {
    result.errorMsg = "JSON Parse Error: " + String(error.c_str());
    Serial.print("[MusicBrainz] "); Serial.println(result.errorMsg);
    return result;
  }

  JsonArray recordings = doc["recordings"];
  if (recordings.size() == 0) {
    result.errorMsg = "No recordings found";
    Serial.println("[MusicBrainz] No recordings found (VPS)");
    return result;
  }

  JsonObject rec = recordings[0];
  String date = rec["first-release-date"] | "?";
  String country = "?";

  JsonArray artistCredit = rec["artist-credit"];
  if (artistCredit.size() > 0) {
    JsonObject artistObj = artistCredit[0]["artist"];
    if (!artistObj["area"].isNull()) {
      country = artistObj["area"]["name"] | "?";
    }
  }

  String tagsList = "";
  if (rec["tags"].is<JsonArray>()) {
    JsonArray tags = rec["tags"];
    int tagLimit = 3;
    for (JsonObject tag : tags) {
      if (tagLimit-- <= 0) break;
      if (tagsList.length() > 0) tagsList += ", ";
      tagsList += tag["name"].as<String>();
    }
  }

  result.fact = buildFactString(date, country, tagsList);
  result.success = true;
  Serial.printf("[MusicBrainz] Result (via VPS): %s\n", result.fact.c_str());
  return result;
}

// ============================================================================
// Прямой запрос к musicbrainz.org (fallback, если прокси недоступен)
// ============================================================================
FactResult MusicBrainzProvider::fetchDirect(const String& artist, const String& title) {
  FactResult result;
  result.success = false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // [ПРАВКА v0.3.9] Явное разрешение DNS для IPv4
  const char* host = "musicbrainz.org";
  IPAddress ip;
  Serial.printf("[MusicBrainz] Resolving host: %s\n", host);

  if (!WiFi.hostByName(host, ip)) {
    result.errorMsg = "DNS Failed for musicbrainz.org";
    Serial.println("[MusicBrainz] DNS Failed for musicbrainz.org");
    return result;
  }
  Serial.printf("[MusicBrainz] Resolved IP: %s\n", ip.toString().c_str());

  // Кодируем параметры для URL
  String a = artist; a.replace(" ", "%20"); a.replace("\"", "");
  String t = title;  t.replace(" ", "%20"); t.replace("\"", "");
  String queryPar = "artist:\"" + a + "\"%20AND%20recording:\"" + t + "\"";

  // Используем IP вместо доменного имени для принудительного IPv4
  String url = "https://" + ip.toString() + "/ws/2/recording/?query=" + queryPar + "&fmt=json&limit=1";

  Serial.printf("[MusicBrainz] Free Heap before SSL: %u\n", ESP.getFreeHeap());

  if (!http.begin(client, url)) {
    result.errorMsg = "Unable to begin HTTP connection";
    Serial.println("[MusicBrainz] Unable to begin HTTP connection");
    return result;
  }

  // Обязательный Host-заголовок при использовании IP в URL
  http.addHeader("Host", "musicbrainz.org");
  http.setUserAgent("Mozilla/5.0 (compatible; YoRadio/0.8; https://github.com/e2002/yoradio)");
  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");
  http.addHeader("Accept-Encoding", "identity"); // Отключаем gzip
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);

  Serial.println("[MusicBrainz] Sending GET request (direct)...");
  int httpCode = http.GET();

  if (httpCode <= 0) {
    result.errorMsg = "Connection failed: " + http.errorToString(httpCode) + " (" + String(httpCode) + ")";
    Serial.printf("[MusicBrainz] Connection failed, error: %s (%d)\n",
                  http.errorToString(httpCode).c_str(), httpCode);
    http.end();
    return result;
  }

  if (httpCode != HTTP_CODE_OK) {
    result.errorMsg = "HTTP error: " + String(httpCode);
    Serial.printf("[MusicBrainz] HTTP error: %d\n", httpCode);
    http.end();
    return result;
  }

  String response = http.getString();
  http.end();

  // Парсим JSON-ответ
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);

  if (error) {
    result.errorMsg = "JSON Parse Error: " + String(error.c_str());
    Serial.print("[MusicBrainz] "); Serial.println(result.errorMsg);
    return result;
  }

  JsonArray recordings = doc["recordings"];
  if (recordings.size() == 0) {
    result.errorMsg = "No recordings found";
    Serial.println("[MusicBrainz] No recordings found (direct)");
    return result;
  }

  JsonObject rec = recordings[0];
  String date = rec["first-release-date"] | "?";
  String country = "?";

  JsonArray artistCredit = rec["artist-credit"];
  if (artistCredit.size() > 0) {
    JsonObject artistObj = artistCredit[0]["artist"];
    if (!artistObj["area"].isNull()) {
      country = artistObj["area"]["name"] | "?";
    }
  }

  String tagsList = "";
  if (rec["tags"].is<JsonArray>()) {
    JsonArray tags = rec["tags"];
    int tagLimit = 3;
    for (JsonObject tag : tags) {
      if (tagLimit-- <= 0) break;
      if (tagsList.length() > 0) tagsList += ", ";
      tagsList += tag["name"].as<String>();
    }
  }

  result.fact = buildFactString(date, country, tagsList);
  result.success = true;
  Serial.printf("[MusicBrainz] Result (direct): %s\n", result.fact.c_str());
  return result;
}
