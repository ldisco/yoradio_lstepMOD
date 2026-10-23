#include "cpu_load.h"
#include "options.h"
#include <cstring>  // strncmp — сравнение префикса имени задачи "IDLE*"
#include "Arduino.h" // millis() — окно кэша между NRSSI и DSPRSSI

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

// Кэш последнего процента: timekeeper шлёт NRSSI и DSPRSSI подряд — второй вызов без кэша
// видит дельту в миллисекунды и рисует ~0% на TFT при нормальной полоске в вебе.
static int s_lastPct = 0;
static uint32_t s_lastSampleMs = 0;
static const uint32_t WEBUI_CPU_SAMPLE_COALESCE_MS = 200;

int webui_freertos_cpu_percent(void) {
  const uint32_t now = millis();
  // Повтор в коротком окне — отдать кэш, счётчики FreeRTOS не трогать.
  if (s_lastSampleMs != 0u && (now - s_lastSampleMs) < WEBUI_CPU_SAMPLE_COALESCE_MS) {
    return s_lastPct;
  }

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
    s_lastPct = 0;
    s_lastSampleMs = now;
    return 0;
  }

  const uint32_t dTotal = totalRuntime - s_rtPrevTotal;
  const uint32_t dIdle = idleSum - s_rtPrevIdle;
  s_rtPrevTotal = totalRuntime;
  s_rtPrevIdle = idleSum;

  if (dTotal == 0u) {
    s_lastSampleMs = now;
    return s_lastPct;
  }

  // pulTotalRunTime — wall-clock (одна шкала), а ulRunTimeCounter у IDLE0+IDLE1
  // набираются параллельно на двух ядрах. Без деления на portNUM_PROCESSORS
  // «занятость» залипает около 100% (красная полная полоса), редко проваливаясь в зелёный.
  const uint32_t cores = (portNUM_PROCESSORS > 0) ? (uint32_t)portNUM_PROCESSORS : 1u;
  const uint32_t idleBudget = dTotal * cores;
  int pct = 100 - static_cast<int>((dIdle * 100u) / idleBudget);
  if (pct > 100) {
    pct = 100;
  }
  if (pct < 0) {
    pct = 0;
  }
  s_lastPct = pct;
  s_lastSampleMs = now;
  return pct;
}

#else

int webui_freertos_cpu_percent(void) {
  // Фича выключена в myoptions или сборка без CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS.
  return 0;
}

#endif
