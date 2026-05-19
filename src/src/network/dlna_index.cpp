#include "dlna_index.h"

#include <LittleFS.h>
#include "../core/config.h"

DlnaIndex dlnaIndex;

// Локальный helper: экранирование CSV-значения.
static String csvEscape(const String& in) {
  String out = in;
  out.replace("\"", "\"\"");
  return "\"" + out + "\"";
}

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
    // Формат совместим с существующим CSV-плейлистом:
    // "name"\t"url"\t0
    String line = csvEscape(items[i].title) + "\t" + csvEscape(items[i].url) + "\t0";
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
    String line = csvEscape(items[i].title) + "\t" + csvEscape(items[i].url) + "\t0";
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
