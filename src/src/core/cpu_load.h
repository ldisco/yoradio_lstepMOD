#ifndef cpu_load_h
#define cpu_load_h

#include <stdint.h>

// Оценка загрузки CPU 0..100 для полоски WebUI / TFT (см. WEBUI_CPU_BAR_ENABLE в options.h).
// Реализация — разность накопленных run-time счётчиков FreeRTOS между вызовами.
// Повторный вызов в окне ~200 мс возвращает кэш (NRSSI и DSPRSSI на одном тике не ломают дельту).
// При выключенной фиче или без статистики в прошивке возвращает 0.
int webui_freertos_cpu_percent(void);

#endif
