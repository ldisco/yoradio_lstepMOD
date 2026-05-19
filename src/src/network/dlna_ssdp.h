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
  DlnaSsdpResult discover(uint32_t timeoutMs = 2000);
};

extern DlnaSsdp dlnaSsdp;

#endif
