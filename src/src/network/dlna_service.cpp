#include "dlna_service.h"

DlnaService dlnaService;

void DlnaService::begin() {
  // На старте очищаем весь контекст, чтобы не тащить старые значения.
  _host = "";
  _controlUrl = "";
  _rootObjectId = "0";
}

void DlnaService::setHost(const String& host) {
  // Сохраняем host в trimmed-виде для единообразия.
  _host = host;
  _host.trim();
}

void DlnaService::setControlUrl(const String& controlUrl) {
  // Сохраняем SOAP endpoint как есть, но без лишних пробелов.
  _controlUrl = controlUrl;
  _controlUrl.trim();
}

void DlnaService::setRootObjectId(const String& objectId) {
  // Root object id по спецификации обычно "0", но не ограничиваем только им.
  _rootObjectId = objectId;
  _rootObjectId.trim();
  if (_rootObjectId.length() == 0) {
    _rootObjectId = "0";
  }
}

bool DlnaService::ready() const {
  // Минимальный критерий готовности: задан host и control URL.
  return _host.length() > 0 && _controlUrl.length() > 0;
}
