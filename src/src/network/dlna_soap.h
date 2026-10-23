#ifndef DLNA_SOAP_H
#define DLNA_SOAP_H
#pragma once

#include <Arduino.h>

// SOAP ContentDirectory#Browse: спросить список objectId и вернуть разобранный результат.
// Без LittleFS/очередей — только сеть+парс (запись плейлиста в dlna_worker).

// Потолок записей из одного Browse: защита heap + размер DlnaBrowseResult на стеке worker
// (массив по значению; больше лимит → нужен больший стек DlnaWorker).
#ifndef DLNA_BROWSE_MAX_ENTRIES
#define DLNA_BROWSE_MAX_ENTRIES 40
#endif

struct DlnaBrowseEntry {
  String id;         // ObjectID (следующий Browse в папку)
  String title;       // dc:title
  String upnpClass;   // upnp:class — аудио vs видео/фото/папка
  String resUrl;      // <res> URL потока (у container пусто)
  bool isContainer;   // true — папка
};

struct DlnaBrowseResult {
  bool ok;                 // false — сеть/битый ответ; ok+count==0 — пустая папка
  String error;             // пусто при ok
  uint16_t count;           // распарсено (<= DLNA_BROWSE_MAX_ENTRIES)
  uint32_t totalMatches;    // TotalMatches с сервера (0 = не сообщил)
  DlnaBrowseEntry entries[DLNA_BROWSE_MAX_ENTRIES];
};

class DlnaSoap {
 public:
  // BrowseDirectChildren: блокирующий HTTP POST — только из фонового dlna_worker, не из HTTP/WS.
  DlnaBrowseResult browse(const String& controlUrl, const String& objectId, uint32_t startingIndex, uint16_t requestedCount);
};

extern DlnaSoap dlnaSoap;

#endif
