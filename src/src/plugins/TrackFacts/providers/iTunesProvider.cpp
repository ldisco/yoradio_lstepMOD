// iTunesProvider.cpp — Провайдер фактов iTunes Search API
// v0.4.0 — Новый провайдер: бесплатный, без ключа, быстрый
// Использует iTunes Search API для получения метаданных о треке:
//   - год релиза, альбом, жанр, длительность, цена
// API: https://itunes.apple.com/search?term=artist+title&entity=song&limit=1
// Ограничение: ~20 запросов в минуту (Apple rate limit)

#include "iTunesProvider.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// [FIX] Проверка безопасности SSL (heap, WiFi, cooldown)
extern volatile bool g_modeSwitching;
extern bool isSafeForSSLForFacts();

// ============================================================================
// Запрос к iTunes Search API
// ============================================================================
FactResult iTunesProvider::fetchFact(const String& artist, const String& title) {
  FactResult result;
  result.success = false;

  // [FIX] Проверяем безопасность SSL
  if (!isSafeForSSLForFacts()) {
    result.errorMsg = "SSL not safe";
    Serial.println("[iTunes] Skipped - SSL not safe");
    return result;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Без проверки сертификата (ESP32 ограничения)
  client.setTimeout(5);           // [FIX] 5 секунд таймаут
  client.setHandshakeTimeout(5);  // [FIX] 5 секунд на SSL handshake
  
  HTTPClient http;

  // Кодируем параметры для URL
  // iTunes API принимает один поисковый запрос (term), объединяем артиста и название
  String searchTerm = artist + " " + title;
  searchTerm.replace(" ", "+"); // iTunes предпочитает + вместо %20
  searchTerm.replace("\"", "");
  searchTerm.replace("'", "");

  String url = "https://itunes.apple.com/search?term=" + searchTerm + "&entity=song&limit=1";

  Serial.printf("[iTunes] Free Heap before SSL: %u\n", ESP.getFreeHeap());
  Serial.printf("[iTunes] Searching: %s - %s\n", artist.c_str(), title.c_str());

  if (!http.begin(client, url)) {
    result.errorMsg = "Unable to begin HTTP connection";
    Serial.println("[iTunes] Unable to begin HTTP connection");
    return result;
  }

  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");
  http.setTimeout(5000);  // [FIX] уменьшен с 8000

  // [FIX] Проверяем безопасность SSL перед GET
  if (!isSafeForSSLForFacts()) {
    http.end();
    result.errorMsg = "SSL not safe";
    Serial.println("[iTunes] Aborted before GET - SSL not safe");
    return result;
  }

  int httpCode = http.GET();

  if (httpCode <= 0) {
    result.errorMsg = "Connection failed: " + http.errorToString(httpCode) + " (" + String(httpCode) + ")";
    Serial.printf("[iTunes] Connection failed, error: %s (%d)\n",
                  http.errorToString(httpCode).c_str(), httpCode);
    http.end();
    return result;
  }

  if (httpCode != HTTP_CODE_OK) {
    result.errorMsg = "HTTP error: " + String(httpCode);
    Serial.printf("[iTunes] HTTP error: %d\n", httpCode);
    http.end();
    return result;
  }

  String response = http.getString();
  http.end();

  // Парсим JSON-ответ iTunes
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);

  if (error) {
    result.errorMsg = "JSON Parse Error: " + String(error.c_str());
    Serial.print("[iTunes] "); Serial.println(result.errorMsg);
    return result;
  }

  int resultCount = doc["resultCount"] | 0;
  if (resultCount == 0) {
    result.errorMsg = "No results found";
    Serial.println("[iTunes] No results found");
    return result;
  }

  JsonObject song = doc["results"][0];

  // Извлекаем метаданные о треке
  String trackName = song["trackName"] | "?";
  String artistName = song["artistName"] | "?";
  String albumName = song["collectionName"] | "?";
  String genre = song["primaryGenreName"] | "?";
  
  // Год релиза: дата в формате "2009-07-06T07:00:00Z"
  String releaseDate = song["releaseDate"] | "";
  String releaseYear = "?";
  if (releaseDate.length() >= 4) {
    releaseYear = releaseDate.substring(0, 4);
  }

  // Длительность трека в миллисекундах -> минуты
  long trackTimeMillis = song["trackTimeMillis"] | 0;
  String duration = "?";
  if (trackTimeMillis > 0) {
    float minutes = (float)trackTimeMillis / 1000.0f / 60.0f;
    // Форматируем: "3.5 мин" или "3.5 min"
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", minutes);
    duration = String(buf);
  }

  // Цена трека
  float trackPrice = song["trackPrice"] | -1.0f;
  String currency = song["currency"] | "";
  String priceStr = "";
  if (trackPrice >= 0 && currency.length() > 0) {
    if (trackPrice == 0) {
      priceStr = (_language == ProviderLanguage::ENGLISH) ? "Free" : "Бесплатно";
    } else {
      char buf[16];
      snprintf(buf, sizeof(buf), "%.2f", trackPrice);
      priceStr = String(buf) + " " + currency;
    }
  }

  // Формируем текст факта на выбранном языке
  // Максимально информативный формат с всеми доступными данными
  String fact = "";
  if (_language == ProviderLanguage::RUSSIAN || _language == ProviderLanguage::AUTO) {
    fact = "iTunes: ";
    if (releaseYear != "?") fact += "Релиз " + releaseYear;
    if (genre != "?") fact += ", " + genre;
    if (albumName != "?") fact += ". Альбом: " + albumName;
    if (duration != "?") fact += " (" + duration + " мин)";
    if (priceStr.length() > 0) fact += ". Цена: " + priceStr;
  } else {
    fact = "iTunes: ";
    if (releaseYear != "?") fact += "Released " + releaseYear;
    if (genre != "?") fact += ", " + genre;
    if (albumName != "?") fact += ". Album: " + albumName;
    if (duration != "?") fact += " (" + duration + " min)";
    if (priceStr.length() > 0) fact += ". Price: " + priceStr;
  }

  result.fact = fact;
  result.success = true;
  Serial.printf("[iTunes] Result: %s\n", fact.c_str());
  return result;
}
