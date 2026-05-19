#include "dlna_ssdp.h"
#include "dlna_http_guard.h"

DlnaSsdp dlnaSsdp;

DlnaSsdpResult DlnaSsdp::discover(uint32_t timeoutMs) {
  DlnaSsdpResult result;
  result.ok = false;
  result.locationUrl = "";
  result.server = "";

  // На этапе B оставляем безопасный каркас:
  // 1) сериализуем попытку discovery через HTTP guard,
  // 2) не выполняем реальные M-SEARCH пакеты до этапа C/D.
  DlnaHttpScopedLock lock(timeoutMs);
  if (!lock.locked()) {
    return result;
  }

  // Возвращаем "not ready" без побочных эффектов.
  return result;
}
