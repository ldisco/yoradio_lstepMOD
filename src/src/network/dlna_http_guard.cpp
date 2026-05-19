#include "dlna_http_guard.h"

SemaphoreHandle_t DlnaHttpGuard::_mutex = NULL;

void DlnaHttpGuard::begin() {
  // Создаём mutex только один раз.
  if (_mutex == NULL) {
    _mutex = xSemaphoreCreateMutex();
  }
}

bool DlnaHttpGuard::enter(uint32_t timeoutMs) {
  // Гарантируем, что mutex создан перед попыткой lock.
  begin();
  if (_mutex == NULL) {
    return false;
  }
  // Ждём lock ограниченное время, чтобы не зависнуть навсегда.
  return xSemaphoreTake(_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void DlnaHttpGuard::leave() {
  // Освобождаем только если mutex существует.
  if (_mutex != NULL) {
    xSemaphoreGive(_mutex);
  }
}

DlnaHttpScopedLock::DlnaHttpScopedLock(uint32_t timeoutMs) : _locked(false) {
  // Пытаемся занять guard в конструкторе.
  _locked = DlnaHttpGuard::enter(timeoutMs);
}

DlnaHttpScopedLock::~DlnaHttpScopedLock() {
  // Освобождаем guard только если он реально был получен.
  if (_locked) {
    DlnaHttpGuard::leave();
  }
}
