#include "dlna_service.h"

DlnaService dlnaService;

void DlnaService::begin() {
  reset();
}

void DlnaService::reset() {
  // На старте и при ошибках очищаем весь контекст
  _host = "";
  _controlUrl = "";
  _rootObjectId = "0";
}

void DlnaService::setHostFromLocation(const String& locationUrl) {
  // Извлекаем хост (ip:port или dns:port) из location URL
  // http://192.168.1.1:8200/rootDesc.xml -> 192.168.1.1:8200
  int from = locationUrl.indexOf("://");
  if (from < 0) {
    _host = "";
    return;
  }
  from += 3; // Пропускаем "://"
  int to = locationUrl.indexOf('/', from);
  if (to < 0) {
    _host = locationUrl.substring(from);
  } else {
    _host = locationUrl.substring(from, to);
  }
  _host.trim();
}

void DlnaService::setHost(const String& host) {
  // Прямая установка хоста (ip/host:port) без разбора URL.
  _host = host;
  _host.trim();
}

void DlnaService::setControlUrl(const String& controlUrl) {
  // Сохраняем SOAP endpoint как есть, но без лишних пробелов
  _controlUrl = controlUrl;
  _controlUrl.trim();
}

void DlnaService::setRootObjectId(const String& objectId) {
  // Root object id по спецификации обычно "0", но не ограничиваем только им
  _rootObjectId = objectId;
  _rootObjectId.trim();
  if (_rootObjectId.length() == 0) {
    _rootObjectId = "0";
  }
}

bool DlnaService::ready() const {
  // Минимальный критерий готовности: задан host и control URL
  return _host.length() > 0 && _controlUrl.length() > 0;
}
