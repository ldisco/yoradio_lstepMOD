// iTunesProvider.cpp — Провайдер фактов iTunes Search API
// v0.5.0 — Прямой HTTP через WiFiClientSecure (без HTTPClient)
//
// HTTPClient на ESP32 может зависнуть навсегда при TLS (arduino-esp32 #5928).
// Используем WiFiClientSecure::connect + ручной HTTP/1.0 с поэтапным таймаутом.
//
// API: https://itunes.apple.com/search?term=artist+title&entity=song&limit=1

#include "iTunesProvider.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>

extern volatile bool g_modeSwitching;
extern bool isSafeForSSLForFacts();

static bool waitAvailable(WiFiClientSecure& c, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!c.available()) {
    if (!c.connected() || (millis() - t0) > timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return true;
}

static String readLine(WiFiClientSecure& c, uint32_t timeoutMs) {
  String line;
  line.reserve(128);
  uint32_t t0 = millis();
  while (c.connected() && (millis() - t0) < timeoutMs) {
    if (!c.available()) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    char ch = c.read();
    if (ch == '\n') break;
    if (ch != '\r') line += ch;
  }
  return line;
}

FactResult iTunesProvider::fetchFact(const String& artist, const String& title) {
  FactResult result;
  result.success = false;

  if (!isSafeForSSLForFacts()) {
    result.errorMsg = "SSL not safe";
    Serial.println("[iTunes] Skipped - SSL not safe");
    return result;
  }

  String searchTerm = artist + " " + title;
  searchTerm.replace(" ", "+");
  searchTerm.replace("\"", "");
  searchTerm.replace("'", "");
  searchTerm.replace("&", "%26");
  searchTerm.replace("#", "%23");

  String path = "/search?term=" + searchTerm + "&entity=song&limit=1";

  Serial.printf("[iTunes] Free Heap before SSL: %u\n", ESP.getFreeHeap());
  Serial.printf("[iTunes] Searching: %s - %s\n", artist.c_str(), title.c_str());
  // #region agent log
  Serial.printf("[AGENT][H6] iTunes pre-dns free=%u max=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  // #endregion

  static IPAddress cachedIp;
  static uint32_t cacheAgeMs = 0;
  const uint32_t DNS_CACHE_LIFETIME_MS = 300000; // 5 min

  IPAddress itunesIp;
  bool dnsOk = false;
  if (cachedIp != IPAddress() && cacheAgeMs != 0 && (millis() - cacheAgeMs) < DNS_CACHE_LIFETIME_MS) {
    itunesIp = cachedIp;
    dnsOk = true;
    Serial.printf("[iTunes] DNS cache hit: %s\n", itunesIp.toString().c_str());
  } else {
    uint32_t dnsStart = millis();
    dnsOk = WiFi.hostByName("itunes.apple.com", itunesIp);
    uint32_t dnsElapsed = millis() - dnsStart;
    // #region agent log
    Serial.printf("[AGENT][H6] iTunes DNS %s ip=%s elapsed=%lums\n",
                  dnsOk ? "OK" : "FAIL",
                  dnsOk ? itunesIp.toString().c_str() : "?",
                  (unsigned long)dnsElapsed);
    // #endregion
    if (dnsOk) {
      cachedIp = itunesIp;
      cacheAgeMs = millis();
    }
  }

  if (!dnsOk) {
    result.errorMsg = "DNS resolve failed";
    Serial.println("[iTunes] DNS resolve failed for itunes.apple.com");
    return result;
  }

  // #region agent log
  Serial.printf("[AGENT][H6] iTunes pre-connect ip=%s free=%u max=%u\n",
                itunesIp.toString().c_str(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  // #endregion

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(7);
  client.setTimeout(8);

  // connect(IP, port, timeout) skips DNS (already resolved above).
  // Must also pass hostname for SNI — use the overload that accepts host parameter.
  // WiFiClientSecure::connect(host, port, timeout) does DNS internally,
  // but lwIP caches the result from hostByName above, so DNS will be instant.
  uint32_t connStart = millis();
  if (!client.connect("itunes.apple.com", 443, 10000)) {
    uint32_t connElapsed = millis() - connStart;
    result.errorMsg = "Connection failed";
    Serial.printf("[iTunes] Connection failed after %lums\n", (unsigned long)connElapsed);
    // #region agent log
    Serial.printf("[AGENT][H6] iTunes connect FAIL elapsed=%lums free=%u max=%u\n",
                  (unsigned long)connElapsed,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    // #endregion
    return result;
  }

  uint32_t connElapsed = millis() - connStart;
  // #region agent log
  Serial.printf("[AGENT][H6] iTunes connected elapsed=%lums free=%u max=%u\n",
                (unsigned long)connElapsed,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  // #endregion

  // HTTP/1.0 to avoid chunked encoding
  String req = "GET " + path + " HTTP/1.0\r\n"
               "Host: itunes.apple.com\r\n"
               "Accept: application/json\r\n"
               "Connection: close\r\n\r\n";

  size_t written = client.print(req);
  if (written == 0) {
    result.errorMsg = "Failed to send request";
    Serial.println("[iTunes] Failed to send request");
    client.stop();
    return result;
  }

  // #region agent log
  Serial.printf("[AGENT][H6] iTunes request sent (%u bytes), waiting response\n", (unsigned)written);
  // #endregion

  if (!waitAvailable(client, 12000)) {
    result.errorMsg = "Response timeout";
    Serial.println("[iTunes] Response timeout (12s)");
    // #region agent log
    Serial.printf("[AGENT][H6] iTunes response TIMEOUT free=%u max=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    // #endregion
    client.stop();
    return result;
  }

  // Read status line
  String statusLine = readLine(client, 5000);
  // #region agent log
  Serial.printf("[AGENT][H6] iTunes status: %s\n", statusLine.c_str());
  // #endregion

  int httpCode = 0;
  if (statusLine.startsWith("HTTP/")) {
    int sp = statusLine.indexOf(' ');
    if (sp > 0) httpCode = statusLine.substring(sp + 1).toInt();
  }

  if (httpCode != 200) {
    result.errorMsg = "HTTP error: " + String(httpCode);
    Serial.printf("[iTunes] HTTP error: %d\n", httpCode);
    client.stop();
    return result;
  }

  // Skip headers
  int contentLength = -1;
  while (client.connected()) {
    String hdr = readLine(client, 5000);
    if (hdr.length() == 0) break;
    if (hdr.startsWith("Content-Length:") || hdr.startsWith("content-length:")) {
      contentLength = hdr.substring(hdr.indexOf(':') + 1).toInt();
    }
  }

  // Read body with hard limit (iTunes JSON for 1 result is ~1-3 KB)
  const int maxBody = 6000;
  String response;
  response.reserve(contentLength > 0 && contentLength < maxBody ? contentLength : 2048);

  uint32_t bodyStart = millis();
  while (client.connected() && (millis() - bodyStart) < 10000 && (int)response.length() < maxBody) {
    if (!client.available()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    char buf[256];
    int n = client.read((uint8_t*)buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      response += buf;
    }
  }

  client.stop();

  // #region agent log
  Serial.printf("[AGENT][H6] iTunes GET done code=%d bodyLen=%d elapsed=%lums free=%u max=%u\n",
                httpCode, (int)response.length(), (unsigned long)(millis() - bodyStart),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  // #endregion

  if (response.length() == 0) {
    result.errorMsg = "Empty response";
    Serial.println("[iTunes] Empty response body");
    return result;
  }

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

  String trackName = song["trackName"] | "?";
  String artistName = song["artistName"] | "?";
  String albumName = song["collectionName"] | "?";
  String genre = song["primaryGenreName"] | "?";

  String releaseDate = song["releaseDate"] | "";
  String releaseYear = "?";
  if (releaseDate.length() >= 4) {
    releaseYear = releaseDate.substring(0, 4);
  }

  long trackTimeMillis = song["trackTimeMillis"] | 0;
  String duration = "?";
  if (trackTimeMillis > 0) {
    float minutes = (float)trackTimeMillis / 1000.0f / 60.0f;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", minutes);
    duration = String(buf);
  }

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
