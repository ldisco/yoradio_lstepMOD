#ifndef DLNA_INDEX_H
#define DLNA_INDEX_H
#pragma once

#include <Arduino.h>

// Элемент DLNA-контейнера для последующей записи в playlist_dlna.csv.
struct DlnaItem {
  String title;
  String url;
};

// Результат операции сборки/добавления DLNA-плейлиста.
struct DlnaBuildResult {
  bool ok;
  uint16_t written;
  String error;
};

// Индексация/запись DLNA-плейлиста в LittleFS.
class DlnaIndex {
 public:
  // Полная пересборка DLNA-плейлиста.
  DlnaBuildResult build(const DlnaItem* items, uint16_t count);

  // Добавление элементов в конец DLNA-плейлиста.
  DlnaBuildResult append(const DlnaItem* items, uint16_t count);

  // Начинает транзакционную сборку полного DLNA-плейлиста во временный файл.
  // Старый рабочий плейлист остаётся нетронутым до успешного вызова commitStagedBuild().
  bool beginStagedBuild(String& error);

  // Добавляет одну уже разобранную страницу SOAP Browse во временный плейлист.
  // LittleFS блокируется только на короткое время записи, сетевых операций под mutex здесь нет.
  DlnaBuildResult appendStaged(const DlnaItem* items, uint16_t count);

  // Атомарно делает полностью собранный временный файл рабочим и перестраивает бинарный индекс.
  bool commitStagedBuild(String& error);

  // Удаляет незавершённый временный файл, сохраняя предыдущий рабочий плейлист.
  void abortStagedBuild();

  // Публичная пересборка бинарного INDEX_DLNA из PLAYLIST_DLNA_PATH (как index.dat для WEB).
  // Берёт LittleFS-lock внутри; нужна при смене источника PL_SRC_DLNA и после импорта DLNA CSV.
  bool rebuildIndexFromPlaylist();

 private:
  DlnaBuildResult writeCsvAtomic(const DlnaItem* items, uint16_t count);
  DlnaBuildResult appendCsvLocked(const DlnaItem* items, uint16_t count);
  bool rebuildIndexFileLocked();
};

extern DlnaIndex dlnaIndex;

#endif
