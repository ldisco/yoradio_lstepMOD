#ifndef DLNA_WORKER_H
#define DLNA_WORKER_H
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Типы задач DLNA worker.
enum DlnaWorkerJobType : uint8_t {
  DLNA_JOB_NONE = 0,
  DLNA_JOB_INIT = 1,
  DLNA_JOB_BUILD = 2,
  DLNA_JOB_APPEND = 3
};

// Описание задания для worker.
struct DlnaWorkerJob {
  DlnaWorkerJobType type;
  char objectId[64];
  uint16_t limit;
};

// Текущий статус worker.
struct DlnaWorkerStatus {
  bool running;
  bool busy;
  uint32_t processedJobs;
  DlnaWorkerJobType lastJob;
  char lastError[96];
  // Сколько задач сейчас ждёт в очереди (для /dlna/status и отладки без Serial).
  uint8_t queueWaiting;
};

// Фоновая обработка DLNA-задач вне WS/HTTP callback.
class DlnaWorker {
 public:
  // Создать очередь и запустить задачу worker.
  bool begin();

  // Поставить задачу в очередь без блокировки UI.
  bool enqueue(const DlnaWorkerJob& job, uint32_t timeoutMs = 0);

  // Снимок текущего статуса worker (включая глубину очереди для /dlna/status).
  DlnaWorkerStatus status() const {
    DlnaWorkerStatus s = _status;
    if (_queue != NULL) {
      UBaseType_t w = uxQueueMessagesWaiting(_queue);
      s.queueWaiting = (w > 255U) ? 255U : (uint8_t)w;
    } else {
      s.queueWaiting = 0;
    }
    return s;
  }

 private:
  static void taskEntry(void* arg);
  void run();
  void process(const DlnaWorkerJob& job);

  QueueHandle_t _queue = NULL;
  TaskHandle_t _task = NULL;
  DlnaWorkerStatus _status = {false, false, 0, DLNA_JOB_NONE, "", 0};
};

extern DlnaWorker dlnaWorker;

#endif
