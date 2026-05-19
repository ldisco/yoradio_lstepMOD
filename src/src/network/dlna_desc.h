#ifndef DLNA_DESC_H
#define DLNA_DESC_H
#pragma once

#include <Arduino.h>

// Результат парсинга device description.
struct DlnaDescResult {
  bool ok;
  String controlUrl;
  String friendlyName;
  String udn;
};

// Парсер XML описания устройства.
// На этапе B поддерживается минимальный extraction по тегам.
class DlnaDesc {
 public:
  DlnaDescResult parse(const String& xml);
};

extern DlnaDesc dlnaDesc;

#endif
