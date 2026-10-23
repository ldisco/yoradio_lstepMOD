#ifndef DLNA_WORKER_H
#define DLNA_WORKER_H
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

enum DlnaWorkerJobType : uint8_t {
  DLNA_JOB_NONE = 0,
  DLNA_JOB_INIT = 1,
  DLNA_JOB_BUILD = 2,
  DLNA_JOB_APPEND = 3,
  DLNA_JOB_LIST = 4 // просмотр каталога (SOAP Browse) без записи в плейлист
};

struct DlnaWorkerJob {
  DlnaWorkerJobType type;
  char objectId[64];
  uint16_t limit;
  // StartingIndex для SOAP Browse (только DLNA_JOB_LIST; BUILD/APPEND = 0).
  // В конце структуры — старые инициализаторы {type, "", 0} остаются валидны.
  uint32_t start;
  // true = рекурсивная сборка выбранной папки и всех подпапок.
  bool recursive;
};

// Одна запись каталога, которую видит WebUI в панели "Список" (упрощённая проекция DlnaBrowseEntry
// из dlna_soap.h — здесь без upnpClass/resUrl, они не нужны для отображения списка).
struct DlnaListItem {
  String id;
  String title;
  bool isContainer;
};

// Лимит снимка «Список» для /dlna/listresult (≤ DLNA_BROWSE_MAX_ENTRIES в soap;
// заголовок намеренно не тянет dlna_soap.h).
#ifndef DLNA_LIST_SNAPSHOT_MAX
#define DLNA_LIST_SNAPSHOT_MAX 40
#endif

// Результат последнего выполненного DLNA_JOB_LIST — то, что реально отдаёт /dlna/listresult.
struct DlnaListSnapshot {
  bool ready;                     // true после хотя бы одного завершённого JOB_LIST
  String objectId;                 // какой каталог был показан
  String error;                    // текст ошибки последнего list (пусто — всё ок)
  uint16_t count;
  DlnaListItem items[DLNA_LIST_SNAPSHOT_MAX];
};

struct DlnaWorkerStatus {
  bool running;
  bool busy;
  uint32_t processedJobs;
  DlnaWorkerJobType lastJob;
  char lastError[96];
  // Сколько задач сейчас ждёт в очереди (для /dlna/status и отладки без Serial).
  uint8_t queueWaiting;
  // Число треков, уже записанных текущей/последней рекурсивной сборкой.
  uint32_t itemsBuilt;
  // Число обработанных каталогов помогает WebUI показать живой прогресс без технических деталей SOAP.
  uint16_t containersScanned;
  // Короткая человекочитаемая фаза: idle/init/browse/build/ready/error.
  char phase[16];
  // Итог последней завершённой задачи; во время busy значение относится к предыдущей задаче.
  bool lastOk;
};

class DlnaWorker {
 public:
  bool begin();

  bool enqueue(const DlnaWorkerJob& job, uint32_t timeoutMs = 0);

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

  // Копия снимка «Список» для /dlna/listresult (mutex: worker vs AsyncWebServer).
  DlnaListSnapshot lastListSnapshot() const;

  // Сброс снимка перед новым DLNA_JOB_LIST — иначе WebUI цепляет старый ready=1
  // или ложный таймаут, пока waitDlnaHeavySafe (до 20 с).
  void invalidateListSnapshot();

 private:
  static void taskEntry(void* arg);
  void run();
  void process(const DlnaWorkerJob& job);
  void storeListSnapshot(const DlnaListSnapshot& snap);

  QueueHandle_t _queue = NULL;
  TaskHandle_t _task = NULL;
  DlnaWorkerStatus _status = {false, false, 0, DLNA_JOB_NONE, "", 0, 0, 0, "idle", false};

  SemaphoreHandle_t _listMutex = NULL;
  DlnaListSnapshot _lastList = {false, "", "", 0, {}};
};

extern DlnaWorker dlnaWorker;

#endif
