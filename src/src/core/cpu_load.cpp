#include "cpu_load.h"
#include "options.h"
#include <cstring>  // strncmp — сравнение префикса имени задачи "IDLE*"

#if WEBUI_CPU_BAR_ENABLE
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#if WEBUI_CPU_BAR_ENABLE && defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

// Максимум задач для одного снимка uxTaskGetSystemState (статический буфер без malloc в net-loop).
#define WEBUI_CPU_TASKSTAT_CAP 48

// Буфер под снимок состояния задач: один на всю прошивку, вызывается не чаще периода NRSSI/WebSocket.
static TaskStatus_t s_taskStatBuf[WEBUI_CPU_TASKSTAT_CAP];

// Предыдущие значения «общего» времени и суммы IDLE*-задач для расчёта дельты между опросами.
static uint32_t s_rtPrevTotal = 0;
static uint32_t s_rtPrevIdle = 0;

int webui_freertos_cpu_percent(void) {
  // Снимок всех задач; pulTotalRunTime — счётчик порта (монотонно растёт между вызовами).
  uint32_t totalRuntime = 0;
  const UBaseType_t n = uxTaskGetSystemState(s_taskStatBuf, WEBUI_CPU_TASKSTAT_CAP, &totalRuntime);

  // Суммируем время только idle-задач (на ESP32 обычно IDLE0, IDLE1 — префикс "IDLE").
  uint32_t idleSum = 0;
  for (UBaseType_t i = 0; i < n; i++) {
    const char *name = s_taskStatBuf[i].pcTaskName;
    if (name && strncmp(name, "IDLE", 4) == 0) {
      idleSum += s_taskStatBuf[i].ulRunTimeCounter;
    }
  }

  // Первый вызов после сброса: запоминаем базу, процент не показываем (избегаем выброса).
  if (s_rtPrevTotal == 0u) {
    s_rtPrevTotal = totalRuntime;
    s_rtPrevIdle = idleSum;
    return 0;
  }

  const uint32_t dTotal = totalRuntime - s_rtPrevTotal;
  const uint32_t dIdle = idleSum - s_rtPrevIdle;
  s_rtPrevTotal = totalRuntime;
  s_rtPrevIdle = idleSum;

  if (dTotal == 0u) {
    return 0;
  }

  // Занятость ≈ 100% − доля idle; clamp 0..100.
  const uint32_t used = (dTotal > dIdle) ? (dTotal - dIdle) : 0u;
  int pct = static_cast<int>((used * 100u) / dTotal);
  if (pct > 100) {
    pct = 100;
  }
  if (pct < 0) {
    pct = 0;
  }
  return pct;
}

#else

int webui_freertos_cpu_percent(void) {
  // Фича выключена в myoptions или сборка без CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS.
  return 0;
}

#endif
