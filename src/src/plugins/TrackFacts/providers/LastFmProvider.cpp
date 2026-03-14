// LastFmProvider.cpp — Провайдер фактов Last.fm
// v0.4.0 — Рефакторинг: вынесено из TrackFacts.cpp
// Получает wiki-описание, название альбома или теги трека через Last.fm API.
// Требует API ключ (бесплатный).

#include "LastFmProvider.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// [FIX] Проверка безопасности SSL (heap, WiFi, cooldown)
extern volatile bool g_modeSwitching;
extern bool isSafeForSSLForFacts();

// ============================================================================
// Запрос к Last.fm API
// ============================================================================
FactResult LastFmProvider::fetchFact(const String& artist, const String& title) {
  FactResult result;
  result.success = false;

  // [FIX] Проверяем безопасность SSL
  if (!isSafeForSSLForFacts()) {
    result.errorMsg = "SSL not safe";
    Serial.println("[LastFM] Skipped - SSL not safe");
    return result;
  }

  // Проверка наличия API ключа
  if (_apiKey.length() == 0) {
    result.errorMsg = "No API key";
    Serial.println("[LastFM] No API key");
    return result;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Без проверки сертификата
  client.setTimeout(5);           // [FIX] 5 секунд таймаут
  client.setHandshakeTimeout(5);  // [FIX] 5 секунд на SSL handshake
  
  HTTPClient http;

  // Кодируем параметры для URL
  String a = artist; a.replace(" ", "%20");
  String t = title;  t.replace(" ", "%20");

  String url = "https://ws.audioscrobbler.com/2.0/?method=track.getInfo&api_key=" +
               _apiKey + "&artist=" + a + "&track=" + t + "&format=json";

  Serial.printf("[LastFM] Free Heap before SSL: %u, Max Alloc: %u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (!http.begin(client, url)) {
    result.errorMsg = "Unable to begin HTTP connection";
    Serial.println("[LastFM] Unable to begin HTTP connection");
    return result;
  }

  http.addHeader("Accept", "application/json");
  http.setTimeout(5000);  // [FIX] уменьшен с 8000

  // [FIX] Проверяем безопасность SSL перед GET
  if (!isSafeForSSLForFacts()) {
    http.end();
    result.errorMsg = "SSL not safe";
    Serial.println("[LastFM] Aborted before GET - SSL not safe");
    return result;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    result.errorMsg = "HTTP error: " + String(httpCode);
    Serial.printf("[LastFM] Error: %d\n", httpCode);
    http.end();
    return result;
  }

  String response = http.getString();
  http.end();

  // Парсим JSON-ответ Last.fm
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);

  if (error) {
    result.errorMsg = "JSON Parse Error: " + String(error.c_str());
    Serial.print("[LastFM] "); Serial.println(result.errorMsg);
    return result;
  }

  if (doc["track"].isNull()) {
    result.errorMsg = "No track data";
    Serial.println("[LastFM] No track data");
    return result;
  }

  JsonObject trackObj = doc["track"];

  // Приоритет 1: Wiki description (самый информативный)
  if (!trackObj["wiki"].isNull()) {
    String summary = trackObj["wiki"]["summary"].as<String>();
    // Убираем HTML-теги из описания
    int tagStart = summary.indexOf("<");
    while (tagStart != -1) {
      int tagEnd = summary.indexOf(">", tagStart);
      if (tagEnd != -1) summary.remove(tagStart, tagEnd - tagStart + 1);
      else break;
      tagStart = summary.indexOf("<");
    }
    summary.trim();
    if (summary.length() > 250) summary = summary.substring(0, 250) + "...";

    result.fact = summary;
    result.success = true;
    Serial.printf("[LastFM] Result (wiki): %s\n", summary.substring(0, 80).c_str());
    return result;
  }

  // Приоритет 2: Название альбома
  if (!trackObj["album"].isNull()) {
    String albumTitle = trackObj["album"]["title"].as<String>();
    result.fact = "Last.fm: Album - " + albumTitle;
    result.success = true;
    Serial.printf("[LastFM] Result (album): %s\n", result.fact.c_str());
    return result;
  }

  // Приоритет 3: Теги (жанры)
  if (!trackObj["toptags"].isNull()) {
    String tagsStr = "";
    JsonArray tags = trackObj["toptags"]["tag"];
    for (JsonObject tag : tags) {
      if (tagsStr.length() > 0) tagsStr += ", ";
      tagsStr += tag["name"].as<String>();
    }
    if (tagsStr.length() > 0) {
      result.fact = "Tags: " + tagsStr;
      result.success = true;
      Serial.printf("[LastFM] Result (tags): %s\n", result.fact.c_str());
      return result;
    }
  }

  result.errorMsg = "No useful data in response";
  Serial.println("[LastFM] No useful data in response");
  return result;
}
