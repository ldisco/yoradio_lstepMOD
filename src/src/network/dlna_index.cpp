#include "dlna_index.h"

#include <LittleFS.h>
#include "../core/config.h"

DlnaIndex dlnaIndex;

DlnaBuildResult DlnaIndex::build(const DlnaItem* items, uint16_t count) {
  // Полная пересборка: пишем временный файл и атомарно переименовываем.
  DlnaBuildResult result = writeCsvAtomic(items, count);
  if (!result.ok) return result;
  if (!lockLittleFS(1000)) {
    result.ok = false;
    result.error = "lock failed after build";
    return result;
  }
  bool idxOk = rebuildIndexFileLocked();
  unlockLittleFS();
  if (!idxOk) {
    result.ok = false;
    result.error = "index rebuild failed";
  }
  return result;
}

DlnaBuildResult DlnaIndex::append(const DlnaItem* items, uint16_t count) {
  // Append выполняется под mutex LittleFS.
  DlnaBuildResult result = appendCsvLocked(items, count);
  if (!result.ok) return result;
  if (!lockLittleFS(1000)) {
    result.ok = false;
    result.error = "lock failed after append";
    return result;
  }
  bool idxOk = rebuildIndexFileLocked();
  unlockLittleFS();
  if (!idxOk) {
    result.ok = false;
    result.error = "index rebuild failed";
  }
  return result;
}

bool DlnaIndex::beginStagedBuild(String& error) {
  error = "";
  if (!lockLittleFS(1500)) {
    error = "staged build lock failed";
    return false;
  }

  const char* tmpPath = "/data/playlist_dlna.tmp";
  LittleFS.remove(tmpPath);
  File tmp = LittleFS.open(tmpPath, "w");
  if (!tmp) {
    unlockLittleFS();
    error = "staged build open failed";
    return false;
  }
  tmp.close();
  unlockLittleFS();
  return true;
}

DlnaBuildResult DlnaIndex::appendStaged(const DlnaItem* items, uint16_t count) {
  DlnaBuildResult result = {false, 0, ""};
  if (items == nullptr || count == 0) {
    result.ok = true;
    return result;
  }
  if (!lockLittleFS(1500)) {
    result.error = "staged append lock failed";
    return result;
  }

  const char* tmpPath = "/data/playlist_dlna.tmp";
  File tmp = LittleFS.open(tmpPath, "a");
  if (!tmp) {
    unlockLittleFS();
    result.error = "staged append open failed";
    return result;
  }
  for (uint16_t i = 0; i < count; ++i) {
    // Формат совпадает с WEB-плейлистом: имя, URL и поправка громкости разделены табуляцией.
    String line = items[i].title + "\t" + items[i].url + "\t0";
    if (tmp.println(line) == 0) {
      tmp.close();
      unlockLittleFS();
      result.error = "staged append write failed";
      return result;
    }
    result.written++;
  }
  tmp.flush();
  tmp.close();
  unlockLittleFS();
  result.ok = true;
  return result;
}

bool DlnaIndex::commitStagedBuild(String& error) {
  error = "";
  if (!lockLittleFS(2000)) {
    error = "staged commit lock failed";
    return false;
  }

  const char* tmpPath = "/data/playlist_dlna.tmp";
  const char* backupPath = "/data/playlist_dlna.bak";
  if (!LittleFS.exists(tmpPath)) {
    unlockLittleFS();
    error = "staged playlist missing";
    return false;
  }

  File tmp = LittleFS.open(tmpPath, "r");
  const size_t tmpSize = tmp ? tmp.size() : 0;
  if (tmp) tmp.close();
  if (tmpSize == 0) {
    unlockLittleFS();
    error = "staged playlist empty";
    return false;
  }

  // Сначала сохраняем прежний рабочий файл: при ошибке rename его можно вернуть без потери медиатеки.
  LittleFS.remove(backupPath);
  const bool hadOldPlaylist = LittleFS.exists(PLAYLIST_DLNA_PATH);
  if (hadOldPlaylist && !LittleFS.rename(PLAYLIST_DLNA_PATH, backupPath)) {
    unlockLittleFS();
    error = "playlist backup failed";
    return false;
  }
  if (!LittleFS.rename(tmpPath, PLAYLIST_DLNA_PATH)) {
    if (hadOldPlaylist) LittleFS.rename(backupPath, PLAYLIST_DLNA_PATH);
    unlockLittleFS();
    error = "staged playlist rename failed";
    return false;
  }

  if (!rebuildIndexFileLocked()) {
    LittleFS.remove(PLAYLIST_DLNA_PATH);
    if (hadOldPlaylist) LittleFS.rename(backupPath, PLAYLIST_DLNA_PATH);
    rebuildIndexFileLocked();
    unlockLittleFS();
    error = "staged index rebuild failed";
    return false;
  }

  LittleFS.remove(backupPath);
  unlockLittleFS();
  return true;
}

void DlnaIndex::abortStagedBuild() {
  if (!lockLittleFS(1000)) return;
  LittleFS.remove("/data/playlist_dlna.tmp");
  unlockLittleFS();
}

DlnaBuildResult DlnaIndex::writeCsvAtomic(const DlnaItem* items, uint16_t count) {
  DlnaBuildResult result;
  result.ok = false;
  result.written = 0;
  result.error = "";

  if (!lockLittleFS(1500)) {
    result.error = "lock failed";
    return result;
  }

  // Пишем во временный файл, чтобы не оставлять битый playlist при сбое.
  const char* tmpPath = "/data/playlist_dlna.tmp";
  File tmp = LittleFS.open(tmpPath, "w");
  if (!tmp) {
    unlockLittleFS();
    result.error = "tmp open failed";
    return result;
  }

  for (uint16_t i = 0; i < count; ++i) {
    // Как WEB playlist.csv (script.js submitPlaylist): name\turl\tovol — без кавычек.
    // Иначе WebUI показывает "Title" в кавычках (сырой /api/playlist).
    String line = items[i].title + "\t" + items[i].url + "\t0";
    if (tmp.println(line) == 0) {
      tmp.close();
      LittleFS.remove(tmpPath);
      unlockLittleFS();
      result.error = "tmp write failed";
      return result;
    }
    result.written++;
  }
  tmp.flush();
  tmp.close();

  // Атомарный switch на новый файл.
  LittleFS.remove(PLAYLIST_DLNA_PATH);
  if (!LittleFS.rename(tmpPath, PLAYLIST_DLNA_PATH)) {
    LittleFS.remove(tmpPath);
    unlockLittleFS();
    result.error = "rename failed";
    return result;
  }
  unlockLittleFS();

  result.ok = true;
  return result;
}

DlnaBuildResult DlnaIndex::appendCsvLocked(const DlnaItem* items, uint16_t count) {
  DlnaBuildResult result;
  result.ok = false;
  result.written = 0;
  result.error = "";

  if (!lockLittleFS(1500)) {
    result.error = "lock failed";
    return result;
  }

  File csv = LittleFS.open(PLAYLIST_DLNA_PATH, "a");
  if (!csv) {
    unlockLittleFS();
    result.error = "playlist open failed";
    return result;
  }

  for (uint16_t i = 0; i < count; ++i) {
    // Append — тот же формат без кавычек, что и build.
    String line = items[i].title + "\t" + items[i].url + "\t0";
    if (csv.println(line) == 0) {
      csv.close();
      unlockLittleFS();
      result.error = "append write failed";
      return result;
    }
    result.written++;
  }
  csv.flush();
  csv.close();
  unlockLittleFS();

  result.ok = true;
  return result;
}

bool DlnaIndex::rebuildIndexFromPlaylist() {
  // Внешняя точка входа: lock + пересборка (этап D — корректный формат как у WEB index.dat).
  if (!lockLittleFS(1500)) {
    return false;
  }
  bool ok = rebuildIndexFileLocked();
  unlockLittleFS();
  return ok;
}

bool DlnaIndex::rebuildIndexFileLocked() {
  // Формат идентичен Config::indexPlaylist(): для каждой валидной строки CSV пишем uint32_t смещение в файле.
  // Иначе playlistLength()/loadStation() (шаг 4 байта) не согласованы с плейлистом DLNA.
  if (!LittleFS.exists(PLAYLIST_DLNA_PATH)) {
    File idx = LittleFS.open(INDEX_DLNA_PATH, "w");
    if (!idx) {
      return false;
    }
    idx.close();
    return true;
  }

  File playlist = LittleFS.open(PLAYLIST_DLNA_PATH, "r");
  if (!playlist) {
    return false;
  }
  int sOvol = 0;
  File index = LittleFS.open(INDEX_DLNA_PATH, "w");
  if (!index) {
    playlist.close();
    return false;
  }
  while (playlist.available()) {
    uint32_t pos = (uint32_t)playlist.position();
    if (config.parseCSV(playlist.readStringUntil('\n').c_str(), config.tmpBuf, config.tmpBuf2, sOvol)) {
      index.write((uint8_t*)&pos, 4);
    }
  }
  index.close();
  playlist.close();
  return true;
}
