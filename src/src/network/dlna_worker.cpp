#include "dlna_worker.h"

#include "dlna_desc.h"
#include "dlna_http_guard.h"
#include "dlna_index.h"
#include "dlna_service.h"
#include "dlna_soap.h"
#include "dlna_ssdp.h"
// options.h должен идти раньше config.h: именно активный myoptions.h теперь определяет USE_DLNA.
#include "../core/options.h"
#include "../core/config.h"
#include <HTTPClient.h>
#include <WiFi.h>  // нужен для WiFi.status()/WL_CONNECTED в лёгкой проверке Init/List
#include <esp_heap_caps.h> // Рекурсивный стек каталогов размещаем в PSRAM, не расходуя стек FreeRTOS-задачи.

// Сколько записей одним Browse для превью «Список» в WebUI.
// Потолок — DLNA_BROWSE_MAX_ENTRIES (см. dlna_soap.h): больше в фиксированный массив не влезет.
#ifndef DLNA_LIST_PREVIEW_COUNT
#define DLNA_LIST_PREVIEW_COUNT 40
#endif

// Жёсткий потолок на общее число треков за один Build/Append: сколько бы ни попросил
// пользователь через WebUI (?limit=...), сборка не уйдёт дальше этого числа страниц Browse
// (по DLNA_BROWSE_MAX_ENTRIES=40 каждая). Старт с 200 — безопасное значение для проверки на
// железе (несколько секунд последовательных SOAP-запросов); поднимать после теста стабильности.
#ifndef DLNA_BUILD_TOTAL_MAX
#define DLNA_BUILD_TOTAL_MAX 200
#endif

// Полная пользовательская синхронизация обходит подпапки, но имеет явные пределы по памяти и времени.
#ifndef DLNA_RECURSIVE_TRACK_MAX
#define DLNA_RECURSIVE_TRACK_MAX 5000
#endif
#ifndef DLNA_RECURSIVE_CONTAINER_MAX
#define DLNA_RECURSIVE_CONTAINER_MAX 256
#endif
#ifndef DLNA_RECURSIVE_DEPTH_MAX
#define DLNA_RECURSIVE_DEPTH_MAX 8
#endif

// Диагностика: до этого момента worker вообще не писал в Serial, поэтому в мониторе
// не было видно ни одного шага Init/Build/Append/Список — только конечный результат
// (или его отсутствие) через HTTP-эндпоинты. Макрос печатает с префиксом [DLNA], чтобы
// строки легко искать в общем логе среди других модулей (COVER, TrackFacts и т.д.).
#define DLNA_LOG(fmt, ...) Serial.printf("[DLNA] " fmt "\n", ##__VA_ARGS__)

DlnaWorker dlnaWorker;

// Потолок очереди: иначе десятки build подряд выжимают heap на разборе ответов.
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

  // Mutex для снимка "Список" (DLNA_JOB_LIST) — защищает _lastList от гонки между
  // worker-задачей (пишет) и HTTP-обработчиком /dlna/listresult (читает).
  if (_listMutex == NULL) {
    _listMutex = xSemaphoreCreateMutex();
  }

  // Отдельная задача на core 0, чтобы не нагружать UI-поток.
  // Стек 12288 (было 4096): SOAP Browse + DIDL; DlnaBrowseResult держит фиксированный
  // массив DlnaBrowseEntry на стеке — несколько КБ на каждый Browse.
  BaseType_t ok = xTaskCreatePinnedToCore(
      DlnaWorker::taskEntry,
      "DlnaWorker",
      12288,
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
  // Backpressure: при забитой очереди не принимаем новые (клиент получит queued=0).
  if (uxQueueMessagesWaiting(_queue) >= DLNA_QUEUE_WAITING_MAX) {
    return false;
  }
  return xQueueSend(_queue, &job, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

// Сброс снимка перед новым LIST — WebUI ждёт именно НОВЫЙ ready=1, а не прошлый.
void DlnaWorker::invalidateListSnapshot() {
  if (_listMutex != NULL && xSemaphoreTake(_listMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    _lastList.ready = false;
    _lastList.error = "";
    _lastList.count = 0;
    xSemaphoreGive(_listMutex);
  }
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
      _status.lastOk = false;
      _status.itemsBuilt = 0;
      _status.containersScanned = 0;
      strlcpy((char*)_status.phase,
              job.type == DLNA_JOB_INIT ? "init" :
              job.type == DLNA_JOB_LIST ? "browse" : "build",
              sizeof(_status.phase));
      process(job);
      _status.lastOk = (_status.lastError[0] == '\0');
      strlcpy((char*)_status.phase, _status.lastOk ? "ready" : "error", sizeof(_status.phase));
      // Успешная пересборка плейлиста: сброс позиции и сигнал WebUI (если уже в режиме DLNA).
      if (_status.lastOk && (job.type == DLNA_JOB_BUILD || job.type == DLNA_JOB_APPEND)) {
        config.saveValue(&config.store.lastDlnaStation, (uint16_t)1);
        config.notifyDlnaPlaylistUpdated();
      }
      _status.processedJobs++;
      _status.busy = false;
      // Короткая пауза между задачами: снижаем пиковую нагрузку на TCP/heap подряд.
      vTaskDelay(pdMS_TO_TICKS(DLNA_INTER_JOB_DELAY_MS));
    }
  }
}

// Воркер использует HTTP-клиент для получения description (include в начале файла).
void DlnaWorker::process(const DlnaWorkerJob& job) {
  // Сбрасываем прошлую ошибку перед обработкой новой задачи
  strlcpy((char*)_status.lastError, "", sizeof(_status.lastError));

  DLNA_LOG("process: type=%u objectId='%s' limit=%u start=%u",
           (unsigned)job.type, job.objectId, (unsigned)job.limit, (unsigned)job.start);

  // Раньше при раннем выходе (SD-режим / unsafe window) для DLNA_JOB_LIST снимок
  // для /dlna/listresult вообще не писался — WebUI видел ready:0 бесконечно, хотя
  // текст ошибки уже лежал в /dlna/status. Локальный helper чинит именно этот разрыв.
  auto reportListEarlyExit = [&](const char* errText) {
    if (job.type != DLNA_JOB_LIST) return;
    DlnaListSnapshot snap;
    snap.ready = true;
    snap.objectId = (job.objectId[0] != '\0') ? String(job.objectId) : String("0");
    snap.error = errText;
    snap.count = 0;
    storeListSnapshot(snap);
  };

  // В SD-режиме DLNA worker не должен трогать сеть/плейлисты WEB
  if (config.getMode() == PM_SDCARD) {
    strlcpy((char*)_status.lastError, "DLNA off in SD mode", sizeof(_status.lastError));
    reportListEarlyExit("DLNA off in SD mode");
    DLNA_LOG("abort: SD mode active, DLNA worker skipped");
    return;
  }

  // Init/List — лёгкие операции (один HTTP GET description или один SOAP Browse).
  // Полный isSafeForDlnaHeavyWork() при играющем WEB-потоке почти всегда false:
  // freeHeap ~60–70K < порога 75K (MIN_HEAP_FOR_SSL/2), плюс 20 с quarantine после
  // смены станции. Из-за этого WebUI показывает «нет ответа от worker (таймаут)».
  // Build/Append по-прежнему ждут тяжёлое окно (много страниц SOAP + запись LittleFS).
  const bool lightJob = (job.type == DLNA_JOB_INIT || job.type == DLNA_JOB_LIST);
  if (lightJob) {
    if (WiFi.status() != WL_CONNECTED) {
      strlcpy((char*)_status.lastError, "wifi not connected", sizeof(_status.lastError));
      reportListEarlyExit("wifi not connected");
      DLNA_LOG("abort: light job, wifi not connected");
      return;
    }
    // Мягкий порог: хватит на один HTTP/SOAP без конкуренции с Build.
    if (ESP.getFreeHeap() < 40000) {
      strlcpy((char*)_status.lastError, "low heap", sizeof(_status.lastError));
      reportListEarlyExit("low heap");
      DLNA_LOG("abort: light job, freeHeap=%u < 40000", (unsigned)ESP.getFreeHeap());
      return;
    }
  } else if (!waitDlnaHeavySafe(DLNA_SAFE_WINDOW_WAIT_MS)) {
    strlcpy((char*)_status.lastError, "unsafe window timeout", sizeof(_status.lastError));
    reportListEarlyExit("unsafe window timeout");
    DLNA_LOG("abort: unsafe window timeout after %u ms (isSafeForDlnaHeavyWork() never returned true)",
             (unsigned)DLNA_SAFE_WINDOW_WAIT_MS);
    return;
  }

  switch (job.type) {
    case DLNA_JOB_INIT: {
      // Явный host из WebUI (?host=...) передаём в SSDP как фильтр по IP — иначе первым
      // ответом часто оказывается роутер (192.168.88.1), а не телефон пользователя.
      String explicitHost;
      if (job.objectId[0] != '\0') {
        explicitHost = String(job.objectId);
        explicitHost.trim();
      }
      const char* ssdpFilter = explicitHost.length() > 0 ? explicitHost.c_str() : nullptr;

      // Шаг 1: SSDP-поиск сервера (один цикл M-SEARCH, без фонового перебора портов).
      DlnaSsdpResult ssdp = dlnaSsdp.discover(3000, ssdpFilter);
      DLNA_LOG("init: ssdp.ok=%d locationUrl='%s' filter='%s'",
               (int)ssdp.ok, ssdp.locationUrl.c_str(), ssdpFilter ? ssdpFilter : "");

      String locationUrl = ssdp.locationUrl;

      // Флаг: нужно ли перебирать типовые пути description.xml (только для fallback по host,
      // когда пользователь ввёл просто IP или IP:порт, а не готовую ссылку).
      bool useFallbackPaths = false;
      // Базовый URL вида "http://host[:порт]" без хвостового пути — используется только
      // если useFallbackPaths == true, для подстановки разных вариантов пути description.
      String fallbackBaseUrl;

      // Fallback: SSDP не нашёл сервер — пробуем HTTP по host из WebUI/EEPROM.
      // Разные UPnP-библиотеки на Android отдают device description по разным путям
      // (rootDesc.xml / description.xml / DeviceDescription.xml), поэтому дальше по коду
      // эти варианты перебираются, а не бьём только в один жёстко заданный путь.
      if (!ssdp.ok || locationUrl.length() == 0) {
        String cfgHost;
        if (explicitHost.length() > 0) {
          cfgHost = explicitHost;
        } else if (job.objectId[0] != '\0') {
          cfgHost = String(job.objectId);
        } else {
          cfgHost = String(config.store.dlnaHost);
        }
        cfgHost.trim();
        if (cfgHost.length() == 0) {
          strlcpy((char*)_status.lastError, "ssdp discover failed & no host", sizeof(_status.lastError));
          dlnaService.reset();
          DLNA_LOG("init abort: ssdp failed and no host configured");
          return;
        }
        if (cfgHost.indexOf("://") < 0) {
          // Без схемы и без порта — добавляем типовой порт UPnP на Android (8200).
          // Без схемы, но с портом — используем порт как есть (его пользователь взял из настроек DLNA-приложения).
          if (cfgHost.indexOf(':') < 0) {
            fallbackBaseUrl = String("http://") + cfgHost + ":8200";
          } else {
            fallbackBaseUrl = String("http://") + cfgHost;
          }
          useFallbackPaths = true;
        } else {
          // Пользователь ввёл готовую ссылку со схемой (http://...) — считаем её точным адресом
          // description.xml и путь не перебираем.
          locationUrl = cfgHost;
        }
      }

      // Типовые пути device description для перебора в fallback-режиме по host.
      static const char* const kDlnaDescPaths[] = {"/rootDesc.xml", "/description.xml", "/DeviceDescription.xml"};
      const size_t kDlnaDescPathsCount = sizeof(kDlnaDescPaths) / sizeof(kDlnaDescPaths[0]);

      // Шаг 2: получаем XML device description.
      // Если SSDP нашёл сервер или пользователь ввёл готовую ссылку — один запрос по locationUrl.
      // Если это fallback по host — перебираем типовые пути, пока не получим 200 OK
      // (или пока не закончится список — тогда в статус попадёт код последней попытки).
      HTTPClient http;
      http.setTimeout(3000);
      int httpCode = -1;
      String payload = "";

      if (useFallbackPaths) {
        for (size_t i = 0; i < kDlnaDescPathsCount; i++) {
          String candidateUrl = fallbackBaseUrl + kDlnaDescPaths[i];
          http.begin(candidateUrl);
          httpCode = http.GET();
          if (httpCode == HTTP_CODE_OK) {
            payload = http.getString();
            locationUrl = candidateUrl; // запоминаем реально сработавший адрес — от него берём host
            http.end();
            break;
          }
          http.end();
          // Небольшая пауза между попытками, чтобы не долбить сервер подряд без передышки.
          vTaskDelay(pdMS_TO_TICKS(50));
        }
      } else {
        http.begin(locationUrl);
        httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
          payload = http.getString();
        }
        http.end();
      }

      DLNA_LOG("init: desc fetch httpCode=%d url='%s' payloadLen=%u",
               httpCode, locationUrl.c_str(), (unsigned)payload.length());

      if (httpCode == HTTP_CODE_OK) {
        // Шаг 3: Парсим XML и сохраняем controlUrl
        DlnaDescResult desc = dlnaDesc.parse(payload);
        if (desc.ok) {
          dlnaService.setHostFromLocation(locationUrl); // Извлекаем хост из URL
          
          // Склейка относительного controlUrl с base URL
          String ctrl = desc.controlUrl;
          if (!ctrl.startsWith("http://")) {
            if (!ctrl.startsWith("/")) ctrl = "/" + ctrl;
            ctrl = "http://" + dlnaService.host() + ctrl;
          }
          dlnaService.setControlUrl(ctrl);
          
          dlnaService.setRootObjectId(desc.rootObjectId); // Сохраняем и root object ID
          // Сохраняем хост в EEPROM (saveValue сам копирует строку и коммитит).
          config.saveValue(config.store.dlnaHost, dlnaService.host().c_str(), sizeof(config.store.dlnaHost));
          DLNA_LOG("init OK: host='%s' controlUrl='%s' rootObjectId='%s'",
                   dlnaService.host().c_str(), ctrl.c_str(), desc.rootObjectId.c_str());
        } else {
          strlcpy((char*)_status.lastError, "desc parse failed", sizeof(_status.lastError));
          DLNA_LOG("init abort: desc parse failed, payload head='%.120s'", payload.c_str());
        }
      } else {
        String errorMsg = "http get failed: " + String(httpCode);
        strlcpy((char*)_status.lastError, errorMsg.c_str(), sizeof(_status.lastError));
        DLNA_LOG("init abort: http get failed, httpCode=%d", httpCode);
      }
      return;
    }
    case DLNA_JOB_BUILD:
    case DLNA_JOB_APPEND: {
      // Пользовательская команда «Использовать эту папку» выполняет полную транзакционную
      // сборку выбранного каталога: старый плейлист заменяется только после успешного обхода.
      if (job.type == DLNA_JOB_BUILD && job.recursive) {
        if (!dlnaService.ready()) {
          strlcpy((char*)_status.lastError, "dlna not ready, connect first", sizeof(_status.lastError));
          return;
        }

        String stageError;
        if (!dlnaIndex.beginStagedBuild(stageError)) {
          strlcpy((char*)_status.lastError, stageError.c_str(), sizeof(_status.lastError));
          return;
        }

        // Узел DFS не содержит String: фиксированная структура безопасно хранится в PSRAM и не фрагментирует heap.
        struct DlnaFolderNode {
          char id[64];
          uint8_t depth;
        };
        DlnaFolderNode* folders = static_cast<DlnaFolderNode*>(
            heap_caps_calloc(DLNA_RECURSIVE_CONTAINER_MAX, sizeof(DlnaFolderNode),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (folders == nullptr) {
          folders = static_cast<DlnaFolderNode*>(
              heap_caps_calloc(DLNA_RECURSIVE_CONTAINER_MAX, sizeof(DlnaFolderNode), MALLOC_CAP_8BIT));
        }
        if (folders == nullptr) {
          dlnaIndex.abortStagedBuild();
          strlcpy((char*)_status.lastError, "folder stack alloc failed", sizeof(_status.lastError));
          return;
        }

        const String rootId = (job.objectId[0] != '\0')
                                  ? String(job.objectId)
                                  : dlnaService.rootObjectId();
        strlcpy(folders[0].id, rootId.length() ? rootId.c_str() : "0", sizeof(folders[0].id));
        folders[0].depth = 0;
        uint16_t folderCount = 1;
        uint32_t totalWritten = 0;
        uint32_t trackLimit = job.limit ? job.limit : DLNA_RECURSIVE_TRACK_MAX;
        if (trackLimit > DLNA_RECURSIVE_TRACK_MAX) trackLimit = DLNA_RECURSIVE_TRACK_MAX;
        bool failed = false;

        while (folderCount > 0 && totalWritten < trackLimit && !failed) {
          // LIFO-обход экономит память: после извлечения узла его место можно занять найденной подпапкой.
          const DlnaFolderNode current = folders[--folderCount];
          _status.containersScanned++;
          uint32_t startingIndex = 0;

          while (totalWritten < trackLimit) {
            const uint16_t requested = (trackLimit - totalWritten < DLNA_BROWSE_MAX_ENTRIES)
                                           ? (uint16_t)(trackLimit - totalWritten)
                                           : (uint16_t)DLNA_BROWSE_MAX_ENTRIES;
            DlnaBrowseResult page = dlnaSoap.browse(
                dlnaService.controlUrl(), String(current.id), startingIndex, requested);
            if (!page.ok) {
              stageError = page.error.length() ? page.error : String("recursive browse failed");
              failed = true;
              break;
            }

            DlnaItem pageItems[DLNA_BROWSE_MAX_ENTRIES];
            uint16_t pageItemCount = 0;
            for (uint16_t i = 0; i < page.count; ++i) {
              const DlnaBrowseEntry& entry = page.entries[i];
              if (entry.isContainer) {
                if (current.depth < DLNA_RECURSIVE_DEPTH_MAX) {
                  if (folderCount >= DLNA_RECURSIVE_CONTAINER_MAX) {
                    stageError = "too many nested folders";
                    failed = true;
                    break;
                  }
                  strlcpy(folders[folderCount].id, entry.id.c_str(), sizeof(folders[folderCount].id));
                  folders[folderCount].depth = current.depth + 1;
                  folderCount++;
                }
                continue;
              }
              if (entry.resUrl.length() == 0) continue;
              if (entry.upnpClass.length() > 0 && entry.upnpClass.indexOf("audioItem") < 0) continue;
              pageItems[pageItemCount].title = entry.title.length() ? entry.title : entry.id;
              pageItems[pageItemCount].url = entry.resUrl;
              pageItemCount++;
            }
            if (failed) break;

            if (pageItemCount > 0) {
              if (totalWritten + pageItemCount > trackLimit) {
                pageItemCount = (uint16_t)(trackLimit - totalWritten);
              }
              DlnaBuildResult writeResult = dlnaIndex.appendStaged(pageItems, pageItemCount);
              if (!writeResult.ok) {
                stageError = writeResult.error;
                failed = true;
                break;
              }
              totalWritten += writeResult.written;
              _status.itemsBuilt = totalWritten;
            }

            if (page.count == 0) break;
            startingIndex += page.count;
            if (page.count < requested) break;
            if (page.totalMatches > 0 && startingIndex >= page.totalMatches) break;
            vTaskDelay(pdMS_TO_TICKS(5));
          }
        }

        free(folders);
        if (failed || totalWritten == 0) {
          dlnaIndex.abortStagedBuild();
          if (!stageError.length()) stageError = "no playable audio in selected folder";
          strlcpy((char*)_status.lastError, stageError.c_str(), sizeof(_status.lastError));
          return;
        }
        if (!dlnaIndex.commitStagedBuild(stageError)) {
          dlnaIndex.abortStagedBuild();
          strlcpy((char*)_status.lastError, stageError.c_str(), sizeof(_status.lastError));
          return;
        }

        _status.itemsBuilt = totalWritten;
        DLNA_LOG("recursive build ready: root='%s' folders=%u tracks=%lu",
                 rootId.c_str(), (unsigned)_status.containersScanned, (unsigned long)totalWritten);
        return;
      }

      // Пагинация Browse: страницы по DLNA_BROWSE_MAX_ENTRIES (StartingIndex 0, 40, 80…),
      // пока не наберём job.limit (≤ DLNA_BUILD_TOTAL_MAX) или сервер не скажет «конец».
      // BUILD: первая страница перезаписывает playlist_dlna.csv; дальше и весь APPEND — допись.
      // Только прямые дети (BrowseDirectChildren) с непустым <res> URL; папки в плейлист не пишем.
      if (!dlnaService.ready()) {
        strlcpy((char*)_status.lastError, "dlna not ready, run Init first", sizeof(_status.lastError));
        DLNA_LOG("build/append abort: dlnaService not ready (run Init first)");
        return;
      }
      String objectId = (job.objectId[0] != '\0') ? String(job.objectId) : String("0");
      uint16_t targetTotal = job.limit;
      if (targetTotal == 0 || targetTotal > DLNA_BUILD_TOTAL_MAX) {
        targetTotal = DLNA_BUILD_TOTAL_MAX;
      }

      DLNA_LOG("build/append: controlUrl='%s' objectId='%s' targetTotal=%u",
               dlnaService.controlUrl().c_str(), objectId.c_str(), (unsigned)targetTotal);

      uint16_t totalWritten = 0;
      uint32_t startingIndex = 0;
      bool firstPage = true;
      String lastErr = "";

      while (totalWritten < targetTotal) {
        uint16_t remaining = targetTotal - totalWritten;
        uint16_t soapCount = (remaining < DLNA_BROWSE_MAX_ENTRIES) ? remaining : DLNA_BROWSE_MAX_ENTRIES;

        DlnaBrowseResult browseResult = dlnaSoap.browse(dlnaService.controlUrl(), objectId, startingIndex, soapCount);
        DLNA_LOG("build/append: page start=%u browse ok=%d error='%s' count=%u totalMatches=%u",
                 (unsigned)startingIndex, (int)browseResult.ok, browseResult.error.c_str(),
                 (unsigned)browseResult.count, (unsigned)browseResult.totalMatches);
        if (!browseResult.ok) {
          lastErr = browseResult.error.length() ? browseResult.error : String("browse failed");
          break;
        }

        DlnaItem items[DLNA_BROWSE_MAX_ENTRIES];
        uint16_t itemsCount = 0;
        for (uint16_t i = 0; i < browseResult.count; i++) {
          const DlnaBrowseEntry& e = browseResult.entries[i];
          // Лог до фильтра: видно, почему Build на корне objectId=0 даёт 0 треков
          // (там обычно только DIR/container, а не audioItem с <res>).
          DLNA_LOG("build/append: raw[%u] %s id='%s' title='%s' class='%s' resLen=%u",
                   (unsigned)i,
                   e.isContainer ? "DIR" : "FILE",
                   e.id.c_str(),
                   e.title.c_str(),
                   e.upnpClass.c_str(),
                   (unsigned)e.resUrl.length());
          if (e.isContainer) continue;               // папки — не аудио-поток
          if (e.resUrl.length() == 0) continue;       // нечего проигрывать без прямого URL
          if (e.upnpClass.length() > 0 && e.upnpClass.indexOf("audioItem") < 0) continue; // видео/фото — пропускаем
          items[itemsCount].title = e.title.length() ? e.title : e.id;
          items[itemsCount].url = e.resUrl;
          itemsCount++;
        }

        if (itemsCount > 0) {
          DlnaBuildResult r = (firstPage && job.type == DLNA_JOB_BUILD) ? dlnaIndex.build(items, itemsCount)
                                                                          : dlnaIndex.append(items, itemsCount);
          DLNA_LOG("build/append: writeCsv page ok=%d written=%u error='%s' totalWritten=%u",
                   (int)r.ok, (unsigned)r.written, r.error.c_str(), (unsigned)(totalWritten + r.written));
          if (!r.ok) {
            lastErr = r.error;
            break;
          }
          totalWritten += r.written;
          firstPage = false; // build() перезаписывает файл только на первой странице, где реально что-то нашли
        }

        if (browseResult.count == 0) break;                 // пустая страница — записей больше нет
        startingIndex += browseResult.count;
        if (browseResult.count < soapCount) break;           // сервер отдал меньше запрошенного — это последняя страница
        if (browseResult.totalMatches > 0 && startingIndex >= browseResult.totalMatches) break; // дошли до конца каталога
      }

      DLNA_LOG("build/append: done totalWritten=%u targetTotal=%u lastErr='%s'",
               (unsigned)totalWritten, (unsigned)targetTotal, lastErr.c_str());

      if (totalWritten == 0) {
        String err = lastErr.length() ? lastErr : String("no playable audio items in this container (try another objectId)");
        strlcpy((char*)_status.lastError, err.c_str(), sizeof(_status.lastError));
        return;
      }
      if (lastErr.length() > 0) {
        // Часть страниц записали, но обрыв случился раньше targetTotal — сообщаем частичный успех,
        // а не тихо делаем вид, что всё прошло гладко.
        char buf[96];
        snprintf(buf, sizeof(buf), "partial (%u items): %s", (unsigned)totalWritten, lastErr.c_str());
        strlcpy((char*)_status.lastError, buf, sizeof(_status.lastError));
      }
      return;
    }
    case DLNA_JOB_LIST: {
      // Просмотр каталога без записи в плейлист — реальный Browse, результат кладём в снимок
      // для /dlna/listresult (WebUI сам его подхватит после статуса busy=0).
      DlnaListSnapshot snap;
      snap.ready = true;
      snap.objectId = (job.objectId[0] != '\0') ? String(job.objectId) : String("0");
      snap.error = "";
      snap.count = 0;

      if (!dlnaService.ready()) {
        snap.error = "dlna not ready, run Init first";
        storeListSnapshot(snap);
        strlcpy((char*)_status.lastError, snap.error.c_str(), sizeof(_status.lastError));
        DLNA_LOG("list abort: dlnaService not ready (run Init first)");
        return;
      }

      uint16_t soapCount = job.limit;
      if (soapCount == 0 || soapCount > DLNA_BROWSE_MAX_ENTRIES) {
        soapCount = DLNA_LIST_PREVIEW_COUNT;
      }

      DLNA_LOG("list: controlUrl='%s' objectId='%s' start=%u soapCount=%u",
               dlnaService.controlUrl().c_str(), snap.objectId.c_str(), (unsigned)job.start, (unsigned)soapCount);

      DlnaBrowseResult browseResult = dlnaSoap.browse(dlnaService.controlUrl(), snap.objectId, job.start, soapCount);
      DLNA_LOG("list: browse ok=%d error='%s' count=%u totalMatches=%u",
               (int)browseResult.ok, browseResult.error.c_str(),
               (unsigned)browseResult.count, (unsigned)browseResult.totalMatches);
      if (!browseResult.ok) {
        snap.error = browseResult.error.length() ? browseResult.error : String("browse failed");
        storeListSnapshot(snap);
        strlcpy((char*)_status.lastError, snap.error.c_str(), sizeof(_status.lastError));
        return;
      }

      uint16_t n = browseResult.count;
      if (n > DLNA_LIST_SNAPSHOT_MAX) n = DLNA_LIST_SNAPSHOT_MAX;
      for (uint16_t i = 0; i < n; i++) {
        snap.items[i].id = browseResult.entries[i].id;
        snap.items[i].title = browseResult.entries[i].title.length() ? browseResult.entries[i].title
                                                                      : browseResult.entries[i].id;
        snap.items[i].isContainer = browseResult.entries[i].isContainer;
        // Печатаем каждую запись: в корне почти всегда только папки (Music/Video/...),
        // без этого лога непонятно, куда именно надо зайти следующим Browse.
        DLNA_LOG("list: [%u] %s id='%s' title='%s' class='%s'",
                 (unsigned)i,
                 snap.items[i].isContainer ? "DIR" : "FILE",
                 snap.items[i].id.c_str(),
                 snap.items[i].title.c_str(),
                 browseResult.entries[i].upnpClass.c_str());
      }
      snap.count = n;
      storeListSnapshot(snap);
      DLNA_LOG("list: snapshot ready, count=%u", (unsigned)snap.count);
      return;
    }
    default:
      // Неизвестный тип задачи: фиксируем ошибку и выходим
      strlcpy((char*)_status.lastError, "unknown job type", sizeof(_status.lastError));
      return;
  }
}

// Сохраняет снимок результата "Список" под mutex — вызывается только из потока worker (после DLNA_JOB_LIST).
void DlnaWorker::storeListSnapshot(const DlnaListSnapshot& snap) {
  if (_listMutex != NULL && xSemaphoreTake(_listMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    _lastList = snap;
    xSemaphoreGive(_listMutex);
  }
}

// Возвращает копию последнего снимка "Список" — вызывается из HTTP-обработчика /dlna/listresult
// (другой поток/ядро), поэтому копирование тоже идёт под тем же mutex.
DlnaListSnapshot DlnaWorker::lastListSnapshot() const {
  DlnaListSnapshot copy;
  copy.ready = false;
  copy.count = 0;
  if (_listMutex != NULL && xSemaphoreTake(_listMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    copy = _lastList;
    xSemaphoreGive(_listMutex);
  }
  return copy;
}

