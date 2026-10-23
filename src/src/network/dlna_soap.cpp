#include "dlna_soap.h"

#include <HTTPClient.h>

// См. такой же макрос в dlna_worker.cpp — здесь свой, чтобы dlna_soap.cpp не тянул
// dlna_worker.h только ради логирования. Префикс тот же ([DLNA]) для единого поиска в логе.
#define DLNA_LOG(fmt, ...) Serial.printf("[DLNA] " fmt "\n", ##__VA_ARGS__)

DlnaSoap dlnaSoap;

// SOAP-ответ несёт DIDL-Lite (сам по себе XML) как экранированную строку внутри тега <Result>,
// иначе он сломал бы внешний XML-конверт. Разворачиваем стандартные XML-entity перед разбором DIDL.
// Амперсанд разворачиваем последним, чтобы не задеть уже развёрнутые "<", ">" и т.д.
static String unescapeXmlEntities(String value) {
  value.replace("&lt;", "<");
  value.replace("&gt;", ">");
  value.replace("&quot;", "\"");
  value.replace("&apos;", "'");
  value.replace("&#39;", "'");
  value.replace("&amp;", "&");
  return value;
}

// Достаёт содержимое первого тега tagName из xml. В отличие от простого extractTag (dlna_desc.cpp),
// этот вариант допускает атрибуты у открывающего тега (нужно для <res protocolInfo="..." size="...">URL</res>)
// и проверяет, что после имени тега идёт '>', пробел или '/', иначе "<res" совпадёт с "<resXxx" по префиксу.
static String extractTagContent(const String& xml, const char* tagName) {
  String openStart = String("<") + tagName;
  int p = xml.indexOf(openStart);
  if (p < 0) return "";
  size_t afterNamePos = (size_t)p + openStart.length();
  char afterName = (xml.length() > (int)afterNamePos) ? xml[afterNamePos] : '\0';
  if (afterName != '>' && afterName != ' ' && afterName != '\t' && afterName != '/') return "";
  int gt = xml.indexOf('>', p);
  if (gt < 0) return "";
  String closeTag = String("</") + tagName + ">";
  int close = xml.indexOf(closeTag, gt + 1);
  if (close < 0) return "";
  String out = xml.substring(gt + 1, close);
  out.trim();
  return out;
}

// Достаёт значение атрибута attrName="..." из фрагмента открывающего тега (например id="...").
static String extractAttr(const String& openTag, const char* attrName) {
  String needle = String(attrName) + "=\"";
  int p = openTag.indexOf(needle);
  if (p < 0) return "";
  p += needle.length();
  int e = openTag.indexOf('"', p);
  if (e < 0) return "";
  return openTag.substring(p, e);
}

// Разбирает DIDL-Lite (уже развёрнутый из entity) на список записей container/item.
// Простой посимвольный скан вместо полноценного XML-парсера — тот же стиль, что уже
// используется в dlna_desc.cpp (проект сознательно не тянет тяжёлую XML-библиотеку).
static uint16_t parseDidl(const String& didl, DlnaBrowseEntry* out, uint16_t maxEntries) {
  uint16_t found = 0;
  int pos = 0;
  while (found < maxEntries) {
    int containerPos = didl.indexOf("<container", pos);
    int itemPos = didl.indexOf("<item", pos);
    if (containerPos < 0 && itemPos < 0) break;

    int nextPos;
    bool isContainer;
    if (containerPos < 0) {
      nextPos = itemPos; isContainer = false;
    } else if (itemPos < 0) {
      nextPos = containerPos; isContainer = true;
    } else if (containerPos < itemPos) {
      nextPos = containerPos; isContainer = true;
    } else {
      nextPos = itemPos; isContainer = false;
    }

    String closeTag = isContainer ? String("</container>") : String("</item>");
    int closeEnd = didl.indexOf(closeTag, nextPos);
    if (closeEnd < 0) break; // битый/усечённый XML — дальше не парсим, что успели, то и вернём
    closeEnd += closeTag.length();

    String block = didl.substring(nextPos, closeEnd);
    int gt = block.indexOf('>');
    String openTag = (gt >= 0) ? block.substring(0, gt + 1) : block;

    DlnaBrowseEntry& e = out[found];
    e.id = extractAttr(openTag, "id");
    e.title = extractTagContent(block, "dc:title");
    e.upnpClass = extractTagContent(block, "upnp:class");
    e.isContainer = isContainer;
    e.resUrl = isContainer ? String("") : extractTagContent(block, "res");
    found++;

    pos = closeEnd;
  }
  return found;
}

DlnaBrowseResult DlnaSoap::browse(const String& controlUrl, const String& objectId, uint32_t startingIndex, uint16_t requestedCount) {
  DlnaBrowseResult result;
  result.ok = false;
  result.error = "";
  result.count = 0;
  result.totalMatches = 0;

  if (controlUrl.length() == 0) {
    result.error = "no control url";
    return result;
  }

  // SOAP-конверт ContentDirectory#Browse (UPnP AV спецификация).
  // BrowseDirectChildren — просим только прямых детей objectId, без рекурсии по подпапкам.
  // Filter=* — все доступные поля ответа; SortCriteria пустой — сортировка на стороне сервера.
  String body = String(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body>"
      "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
      "<ObjectID>") + objectId + "</ObjectID>"
      "<BrowseFlag>BrowseDirectChildren</BrowseFlag>"
      "<Filter>*</Filter>"
      "<StartingIndex>" + String(startingIndex) + "</StartingIndex>"
      "<RequestedCount>" + String(requestedCount) + "</RequestedCount>"
      "<SortCriteria></SortCriteria>"
      "</u:Browse>"
      "</s:Body>"
      "</s:Envelope>";

  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(controlUrl)) {
    result.error = "http begin failed";
    return result;
  }
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  // SOAPAction обязателен для большинства UPnP-стеков (Cyber Garage, MiniDLNA и т.д.) —
  // без него сервер часто отвечает 400/500, даже если тело SOAP-конверта верное.
  http.addHeader("SOAPAction", "\"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"");
  http.addHeader("Connection", "close");

  const int httpCode = http.POST(body);
  String payload = "";
  if (httpCode == HTTP_CODE_OK) {
    payload = http.getString();
  }
  http.end();

  DLNA_LOG("soap.browse: POST %s -> httpCode=%d payloadLen=%u",
           controlUrl.c_str(), httpCode, (unsigned)payload.length());

  if (httpCode != HTTP_CODE_OK) {
    result.error = "soap post failed: " + String(httpCode);
    // Тело ответа при ошибке часто содержит SOAP Fault с человекочитаемой причиной —
    // печатаем срез, чтобы не листать серийный монитор в поисках истины.
    DLNA_LOG("soap.browse: error body head='%.200s'", payload.c_str());
    return result;
  }

  String resultField = extractTagContent(payload, "Result");
  if (resultField.length() == 0) {
    result.error = "no Result field in soap response";
    DLNA_LOG("soap.browse: no <Result> tag, raw payload head='%.200s'", payload.c_str());
    return result;
  }

  String didl = unescapeXmlEntities(resultField);
  result.count = parseDidl(didl, result.entries, DLNA_BROWSE_MAX_ENTRIES);

  String totalStr = extractTagContent(payload, "TotalMatches");
  result.totalMatches = (totalStr.length() > 0) ? (uint32_t)totalStr.toInt() : 0;

  DLNA_LOG("soap.browse: parsed %u entries (DIDL head='%.150s')", (unsigned)result.count, didl.c_str());

  result.ok = true; // 0 записей — просто пустая папка, это не ошибка
  return result;
}
