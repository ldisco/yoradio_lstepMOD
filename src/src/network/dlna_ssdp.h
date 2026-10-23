#ifndef DLNA_SSDP_H
#define DLNA_SSDP_H
#pragma once

#include <Arduino.h>

// Результат SSDP-discovery (минимальный каркас для этапа B).
struct DlnaSsdpResult {
  bool ok;
  String locationUrl;
  String server;
};

// SSDP-клиент. На этапе B даём безопасный каркас вызовов.
class DlnaSsdp {
 public:
  // Выполнить M-SEARCH и вернуть базовый результат.
  // expectedHost — необязательный фильтр (IP или IP:порт из WebUI): принимаем только LOCATION с этим IP,
  // чтобы SSDP не подхватил чужой UPnP (например роутер), когда пользователь указал телефон/Android-сервер.
  DlnaSsdpResult discover(uint32_t timeoutMs = 2000, const char* expectedHost = nullptr);
};

extern DlnaSsdp dlnaSsdp;

#endif
