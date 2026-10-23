#ifndef DLNA_HTTP_GUARD_H
#define DLNA_HTTP_GUARD_H
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Глобальный guard для сериализации DLNA HTTP/SOAP запросов.
// Цель: не допускать параллельных тяжёлых сетевых операций DLNA,
// чтобы снизить пики нагрузки на heap и сетевой стек.
class DlnaHttpGuard {
 public:
  // Инициализация mutex. Безопасно вызывать повторно.
  static void begin();

  // Вход в критическую секцию HTTP guard.
  // Возвращает true, если lock получен за timeoutMs.
  static bool enter(uint32_t timeoutMs = 5000);

  // Выход из критической секции HTTP guard.
  static void leave();

 private:
  // Mutex разделяется всеми DLNA-модулями.
  static SemaphoreHandle_t _mutex;
};

// RAII-обёртка: автоматически освобождает guard при выходе из scope.
class DlnaHttpScopedLock {
 public:
  explicit DlnaHttpScopedLock(uint32_t timeoutMs = 5000);
  ~DlnaHttpScopedLock();
  bool locked() const { return _locked; }

 private:
  bool _locked;
};

#endif
