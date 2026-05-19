#include "dlna_desc.h"

DlnaDesc dlnaDesc;

// Локальный helper: извлечь содержимое первого тега.
static String extractTag(const String& xml, const char* tagName) {
  String openTag = String("<") + tagName + ">";
  String closeTag = String("</") + tagName + ">";
  int from = xml.indexOf(openTag);
  if (from < 0) return "";
  from += openTag.length();
  int to = xml.indexOf(closeTag, from);
  if (to < 0 || to <= from) return "";
  String value = xml.substring(from, to);
  value.trim();
  return value;
}

DlnaDescResult DlnaDesc::parse(const String& xml) {
  DlnaDescResult result;
  result.ok = false;
  result.controlUrl = "";
  result.friendlyName = "";
  result.udn = "";

  // Минимальный набор полей, который нужен для следующей интеграции.
  result.controlUrl = extractTag(xml, "controlURL");
  result.friendlyName = extractTag(xml, "friendlyName");
  result.udn = extractTag(xml, "UDN");

  // Считаем парсинг успешным, если найден controlURL.
  result.ok = result.controlUrl.length() > 0;
  return result;
}
