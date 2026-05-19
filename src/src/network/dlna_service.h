#ifndef DLNA_SERVICE_H
#define DLNA_SERVICE_H
#pragma once

#include <Arduino.h>

// Сервис хранения текущего DLNA-контекста.
// Здесь нет тяжёлых сетевых операций: только состояние, нужное для worker/API.
class DlnaService {
 public:
  // Инициализация начального состояния.
  void begin();

  // Установить хост DLNA-сервера (ip/host:port).
  void setHost(const String& host);
  // Получить текущий хост.
  const String& host() const { return _host; }

  // Установить control URL (SOAP endpoint).
  void setControlUrl(const String& controlUrl);
  // Получить control URL.
  const String& controlUrl() const { return _controlUrl; }

  // Установить rootObjectId для Browse.
  void setRootObjectId(const String& objectId);
  // Получить rootObjectId.
  const String& rootObjectId() const { return _rootObjectId; }

  // Признак, что сервис инициализирован и готов к Browse.
  bool ready() const;

 private:
  String _host;
  String _controlUrl;
  String _rootObjectId;
};

extern DlnaService dlnaService;

#endif
