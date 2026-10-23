#include "dlna_ssdp.h"
#include <WiFiUdp.h>

DlnaSsdp dlnaSsdp;

// Адрес и порт для SSDP M-SEARCH
#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900

// Тело запроса на поиск медиа-сервера (порт 1900 — стандартный для SSDP)
#define SSDP_MSEARCH_REQ "M-SEARCH * HTTP/1.1\r\n" \
                         "HOST: " SSDP_ADDR ":1900\r\n" \
                         "MAN: \"ssdp:discover\"\r\n" \
                         "MX: 2\r\n" \
                         "ST: upnp:rootdevice\r\n" \
                         "\r\n"

#define SSDP_MSEARCH_REQ_2 "M-SEARCH * HTTP/1.1\r\n" \
                           "HOST: " SSDP_ADDR ":1900\r\n" \
                           "MAN: \"ssdp:discover\"\r\n" \
                           "MX: 2\r\n" \
                           "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n" \
                           "\r\n"

// Парсер заголовков нечувствителен к регистру: Android CyberLink отвечает "Location:",
// тогда как часть других серверов использует "LOCATION:". HTTP/SSDP допускает оба варианта.
static String getHeaderValue(const String& response, const char* header) {
  String wanted = String(header);
  wanted.toUpperCase();
  int lineStart = 0;
  while (lineStart < response.length()) {
    int lineEnd = response.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = response.length();
    String line = response.substring(lineStart, lineEnd);
    line.trim();
    int separator = line.indexOf(':');
    if (separator > 0) {
      String name = line.substring(0, separator);
      name.trim();
      name.toUpperCase();
      if (name == wanted) {
        String value = line.substring(separator + 1);
        value.trim();
        return value;
      }
    }
    lineStart = lineEnd + 1;
  }
  return "";
}

// Проверяет, что LOCATION относится к ожидаемому хосту пользователя.
// Порт в expectedHost игнорируем: у Android DLNA HTTP-порт файлов (18192) часто не совпадает с UPnP-портом в SSDP.
static bool locationMatchesExpectedHost(const String& locationUrl, const char* expectedHost) {
  if (expectedHost == nullptr || expectedHost[0] == '\0') return true;
  String wanted = String(expectedHost);
  wanted.trim();
  if (wanted.length() == 0) return true;
  // Снимаем схему и путь, оставляем host[:port] для извлечения IP.
  int scheme = wanted.indexOf("://");
  if (scheme >= 0) {
    wanted = wanted.substring(scheme + 3);
    int slash = wanted.indexOf('/');
    if (slash >= 0) wanted = wanted.substring(0, slash);
  }
  int colon = wanted.indexOf(':');
  String ip = (colon >= 0) ? wanted.substring(0, colon) : wanted;
  ip.trim();
  if (ip.length() == 0) return true;
  // LOCATION может быть http или https; сравниваем только IP-часть.
  return locationUrl.indexOf(String("http://") + ip) >= 0 ||
         locationUrl.indexOf(String("https://") + ip) >= 0;
}

// Ответ SSDP с MediaServer предпочтительнее rootdevice (роутеры часто отвечают только rootdevice).
static bool ssdpResponseIsMediaServer(const String& response) {
  return response.indexOf("MediaServer:1") >= 0 ||
         response.indexOf("ContentDirectory:1") >= 0;
}

DlnaSsdpResult DlnaSsdp::discover(uint32_t timeoutMs, const char* expectedHost) {
  DlnaSsdpResult result;
  result.ok = false;
  DlnaSsdpResult fallbackResult; // первый подходящий rootdevice, если MediaServer так и не пришёл
  fallbackResult.ok = false;

  WiFiUDP udp;
  // M-SEARCH получает unicast-ответ на исходный порт. Не вступаем в multicast-группу 1900:
  // прежний beginMulticast(0.0.0.0, 1900) выбирал неверный интерфейс/группу на ESP32-S3.
  const uint16_t localReplyPort = 1901;
  if (!udp.begin(localReplyPort)) {
    return result;
  }

  // Отправляем M-SEARCH запрос (два типа для надежности)
  udp.beginPacket(SSDP_ADDR, SSDP_PORT);
  udp.write((const uint8_t*)SSDP_MSEARCH_REQ, strlen(SSDP_MSEARCH_REQ));
  udp.endPacket();
  
  vTaskDelay(pdMS_TO_TICKS(100));
  
  udp.beginPacket(SSDP_ADDR, SSDP_PORT);
  udp.write((const uint8_t*)SSDP_MSEARCH_REQ_2, strlen(SSDP_MSEARCH_REQ_2));
  udp.endPacket();

  uint32_t t0 = millis();
  char packetBuffer[512];

  // Ждём ответ в течение таймаута
  while (millis() - t0 < timeoutMs) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
      if (len > 0) {
        packetBuffer[len] = 0;
        String response = String(packetBuffer);
        // Отлавливаем только валидные ответы
        if (response.indexOf("HTTP/1.1 200 OK") >= 0) {
          String locationUrl = getHeaderValue(response, "LOCATION");
          if (locationUrl.length() == 0) continue;
          if (!locationMatchesExpectedHost(locationUrl, expectedHost)) continue;
          DlnaSsdpResult candidate;
          candidate.ok = true;
          candidate.locationUrl = locationUrl;
          candidate.server = getHeaderValue(response, "SERVER");
          // Явный host — берём первый подходящий ответ и выходим (не ждём лучшего).
          if (expectedHost != nullptr && expectedHost[0] != '\0') {
            result = candidate;
            break;
          }
          // Автопоиск: приоритет MediaServer, иначе запоминаем первый rootdevice как запасной.
          if (ssdpResponseIsMediaServer(response)) {
            result = candidate;
            break;
          }
          if (!fallbackResult.ok) fallbackResult = candidate;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // Небольшая пауза, чтобы не забивать CPU
  }

  udp.stop();
  if (!result.ok && fallbackResult.ok) result = fallbackResult;
  return result;
}
