#include "dlna_worker.h"

#include "dlna_desc.h"
#include "dlna_http_guard.h"
#include "dlna_index.h"
#include "dlna_service.h"
#include "dlna_ssdp.h"
#include "../core/config.h"
#include "../core/options.h"

DlnaWorker dlnaWorker;

// Этап F: не накапливать больше N задач в очереди — иначе браузер/скрипты могут
// поставить десятки build подряд и выжать heap при разборе ответов.
#ifndef DLNA_QUEUE_WAITING_MAX
#define DLNA_QUEUE_WAITING_MAX 12
#endif
// Максимум ожидания «безопасного окна» перед отменой задачи (мс).
#ifndef DLNA_SAFE_WINDOW_WAIT_MS
#define DLNA_SAFE_WINDOW_WAIT_MS 20000
#endif
// Пауза между задачами — отдаём сети/аудио тик (мс).
#ifndef DLNA_INTER_JOB_DELAY_MS
#define DLNA_INTER_JOB_DELAY_MS 50
#endif

// Ждём, пока isSafeForDlnaHeavyWork() станет true, с ограничением по времени.
static bool waitDlnaHeavySafe(uint32_t maxWaitMs) {
  const uint32_t t0 = millis();
  while ((millis() - t0) < maxWaitMs) {
    if (isSafeForDlnaHeavyWork()) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

bool DlnaWorker::begin() {
  // Повторный вызов begin не должен создавать второй worker.
  if (_queue != NULL && _task != NULL) {
    return true;
  }

  // Очередь делаем короткой (16), чтобы не накапливать долгий хвост задач.
  _queue = xQueueCreate(16, sizeof(DlnaWorkerJob));
  if (_queue == NULL) {
    strlcpy((char*)_status.lastError, "queue create failed", sizeof(_status.lastError));
    return false;
  }

  // Инициализация зависимостей DLNA-каркаса.
  DlnaHttpGuard::begin();
  dlnaService.begin();

  // Отдельная задача на core 0, чтобы не нагружать UI-поток.
  BaseType_t ok = xTaskCreatePinnedToCore(
      DlnaWorker::taskEntry,
      "DlnaWorker",
      4096,
      this,
      1,
      &_task,
      0);
  if (ok != pdPASS) {
    strlcpy((char*)_status.lastError, "task create failed", sizeof(_status.lastError));
    return false;
  }

  _status.running = true;
  return true;
}

bool DlnaWorker::enqueue(const DlnaWorkerJob& job, uint32_t timeoutMs) {
  // Без очереди worker не может принять задачу.
  if (_queue == NULL) return false;
  // Этап F: backpressure — при забитой очереди не принимаем новые задания (клиент получит queued=0).
  if (uxQueueMessagesWaiting(_queue) >= DLNA_QUEUE_WAITING_MAX) {
    return false;
  }
  return xQueueSend(_queue, &job, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void DlnaWorker::taskEntry(void* arg) {
  DlnaWorker* self = static_cast<DlnaWorker*>(arg);
  if (self != NULL) {
    self->run();
  }
  vTaskDelete(NULL);
}

void DlnaWorker::run() {
  DlnaWorkerJob job;
  while (true) {
    // Ждём задачу бесконечно: worker "спит", пока нет событий.
    if (xQueueReceive(_queue, &job, portMAX_DELAY) == pdTRUE) {
      _status.busy = true;
      _status.lastJob = job.type;
      process(job);
      _status.processedJobs++;
      _status.busy = false;
      // Короткая пауза между задачами: снижаем пиковую нагрузку на TCP/heap подряд.
      vTaskDelay(pdMS_TO_TICKS(DLNA_INTER_JOB_DELAY_MS));
    }
  }
}

void DlnaWorker::process(const DlnaWorkerJob& job) {
  // Сбрасываем прошлую ошибку перед обработкой новой задачи.
  strlcpy((char*)_status.lastError, "", sizeof(_status.lastError));

  // В SD-режиме DLNA worker не должен трогать сеть/плейлисты WEB (контракт этапа F).
  if (config.getMode() == PM_SDCARD) {
    strlcpy((char*)_status.lastError, "DLNA off in SD mode", sizeof(_status.lastError));
    return;
  }
  // Этап F: ждём безопасное окно (WiFi, heap, вне cooldown смены режима/станции).
  if (!waitDlnaHeavySafe(DLNA_SAFE_WINDOW_WAIT_MS)) {
    strlcpy((char*)_status.lastError, "unsafe window timeout", sizeof(_status.lastError));
    return;
  }

  switch (job.type) {
    case DLNA_JOB_INIT: {
      // Шаг INIT: пока только базовый discovery-каркас.
      DlnaSsdpResult ssdp = dlnaSsdp.discover(2000);
      if (!ssdp.ok) {
        strlcpy((char*)_status.lastError, "ssdp discover failed", sizeof(_status.lastError));
        return;
      }
      dlnaService.setHost(ssdp.locationUrl);
      return;
    }
    case DLNA_JOB_BUILD: {
      // Шаг BUILD: на этапе B пишем безопасный placeholder-плейлист (objectId/limit — для будущего Browse).
      DlnaItem items[1];
      if (job.objectId[0] != '\0') {
        items[0].title = String("DLNA build ") + job.objectId;
      } else {
        items[0].title = "DLNA placeholder";
      }
      items[0].url = "http://127.0.0.1/dlna-placeholder";
      (void)job.limit;
      DlnaBuildResult r = dlnaIndex.build(items, 1);
      if (!r.ok) {
        strlcpy((char*)_status.lastError, r.error.c_str(), sizeof(_status.lastError));
      }
      return;
    }
    case DLNA_JOB_APPEND: {
      // Шаг APPEND: placeholder; objectId пробрасываем в title для отладки API.
      DlnaItem items[1];
      if (job.objectId[0] != '\0') {
        items[0].title = String("DLNA append ") + job.objectId;
      } else {
        items[0].title = "DLNA placeholder append";
      }
      items[0].url = "http://127.0.0.1/dlna-placeholder-append";
      (void)job.limit;
      DlnaBuildResult r = dlnaIndex.append(items, 1);
      if (!r.ok) {
        strlcpy((char*)_status.lastError, r.error.c_str(), sizeof(_status.lastError));
      }
      return;
    }
    default:
      // Неизвестный тип задачи: фиксируем ошибку и выходим.
      strlcpy((char*)_status.lastError, "unknown job type", sizeof(_status.lastError));
      return;
  }
}
