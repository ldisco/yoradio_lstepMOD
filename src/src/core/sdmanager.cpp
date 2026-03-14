#include "options.h"
#if SDC_CS!=255
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "vfs_api.h"
#include "sd_diskio.h"
//#define USE_SD
#include "config.h"
#include "sdmanager.h"
#include "display.h"
#include "player.h"
// Для переноса индексного файла SD (indexsd.dat) на LittleFS
// нам нужен прямой доступ к LittleFS из менеджера SD.
#include <LittleFS.h>

#if defined(SD_SPIPINS) || SD_HSPI
SPIClass  SDSPI(HSPI);
#define SDREALSPI SDSPI
#else
  #define SDREALSPI SPI
#endif

#ifndef SDSPISPEED
  #define SDSPISPEED 20000000
#endif

SDManager sdman(FSImplPtr(new VFSImpl()));

bool SDManager::start(){
  ready = begin(SDC_CS, SDREALSPI, SDSPISPEED);
  vTaskDelay(10);
  if(!ready) ready = begin(SDC_CS, SDREALSPI, SDSPISPEED);
  vTaskDelay(20);
  if(!ready) ready = begin(SDC_CS, SDREALSPI, SDSPISPEED);
  vTaskDelay(50);
  if(!ready) ready = begin(SDC_CS, SDREALSPI, SDSPISPEED);
  return ready;
}

void SDManager::stop(){
  end();
  ready = false;
}
#include "diskio_impl.h"
bool SDManager::cardPresent() {

  if(!ready) return false;
  if(sectorSize()<1) {
    return false;
  }
  uint8_t buff[sectorSize()] = { 0 };
  bool bread = readRAW(buff, 1);
  if(sectorSize()>0 && !bread) return false;
  return bread;
}

bool SDManager::_checkNoMedia(const char* path){
  if (path[strlen(path) - 1] == '/')
    snprintf(config.tmpBuf, sizeof(config.tmpBuf), "%s%s", path, ".nomedia");
  else
    snprintf(config.tmpBuf, sizeof(config.tmpBuf), "%s/%s", path, ".nomedia");
  bool nm = exists(config.tmpBuf);
  return nm;
}

bool SDManager::_endsWith (const char* base, const char* str) {
  int slen = strlen(str) - 1;
  const char *p = base + strlen(base) - 1;
  while(p > base && isspace(*p)) p--;
  p -= slen;
  if (p < base) return false;
  return (strncmp(p, str, slen) == 0);
}

// [FIX] Case-insensitive вариант для сравнения расширений без порчи оригинального имени файла.
bool SDManager::_endsWithCI (const char* base, const char* str) {
  int slen = strlen(str) - 1;
  const char *p = base + strlen(base) - 1;
  while(p > base && isspace(*p)) p--;
  p -= slen;
  if (p < base) return false;
  return (strncasecmp(p, str, slen) == 0);
}

void SDManager::listSD(File &plSDfile, File &plSDindex, const char* dirname, uint8_t levels) {
    File root = sdman.open(dirname);
    if (!root) {
        Serial.println("##[ERROR]#\tFailed to open directory");
        return;
    }
    if (!root.isDirectory()) {
        Serial.println("##[ERROR]#\tNot a directory");
        return;
    }

    uint32_t pos = 0;
    char* filePath;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5));  // [FIX] 5мс вместо 2 тиков — даём WiFi-стеку больше времени
        player.loop();
        bool isDir;
        String fileName = root.getNextFileName(&isDir);
        if (fileName.isEmpty()) break;
        filePath = (char*)malloc(fileName.length() + 1);
        if (filePath == NULL) {
            Serial.println("Memory allocation failed");
            break;
        }
        strcpy(filePath, fileName.c_str());
        const char* fn = strrchr(filePath, '/') + 1;
        if (isDir) {
            if (levels && !_checkNoMedia(filePath)) {
                listSD(plSDfile, plSDindex, filePath, levels - 1);
            }
        } else {
            // [FIX] Убрали strlwr — имя файла сохраняется как есть (оригинальный регистр).
            // Сравнение расширений делаем без учёта регистра через _endsWithCI.
            if (_endsWithCI(fn, ".mp3") || _endsWithCI(fn, ".m4a") || _endsWithCI(fn, ".aac") ||
                _endsWithCI(fn, ".wav") || _endsWithCI(fn, ".flac") || _endsWithCI(fn, ".ogg")) {
                pos = plSDfile.position();
                plSDfile.printf("%s\t%s\t0\n", fn, filePath);
                plSDindex.write((uint8_t*)&pos, 4);

                // if(display.mode()==SDCHANGE) display.putRequest(SDFILEINDEX, _sdFCount+1); 
                _sdFCount++;
                // if (_sdFCount % 64 == 0) Serial.println();
            }
        }
        free(filePath);
    }
    root.close();
}

void SDManager::indexSDPlaylist() {
  _sdFCount = 0;
  // [FIX v2] КЛЮЧЕВОЕ ИЗМЕНЕНИЕ: Плейлист CSV теперь тоже хранится на LittleFS,

  //   - playlistsd.csv  → LittleFS (быстрая внутренняя флеш)
  //   - indexsd.dat      → LittleFS (уже было перенесено ранее)
  //   - аудио MP3        → SD-карта (единственный потребитель SPI)
  //
  // Удаляем старые версии и на SD, и на LittleFS для чистой миграции.
  if(exists(PLAYLIST_SD_PATH)) remove(PLAYLIST_SD_PATH);
  if(LittleFS.exists(PLAYLIST_SD_PATH)) LittleFS.remove(PLAYLIST_SD_PATH);
  if(exists(INDEX_SD_PATH)) remove(INDEX_SD_PATH);
  if(LittleFS.exists(INDEX_SD_PATH)) LittleFS.remove(INDEX_SD_PATH);
  
  // [FIX v2] Плейлист создаём на LittleFS — веб-сервер, display-виджеты
  // и loadStation() будут читать его оттуда, не конкурируя с аудио за SD.
  vTaskDelay(pdMS_TO_TICKS(10));  // [FIX] Пауза после удаления файлов — даём WiFi обработать таймеры
  File playlist = LittleFS.open(PLAYLIST_SD_PATH, "w", true);
  if (!playlist) {
    Serial.println("[indexSD] ОШИБКА: не удалось создать playlist на LittleFS");
    return;
  }
  
  // Индекс тоже на LittleFS (как и раньше).
  File index = LittleFS.open(INDEX_SD_PATH, "w", true);
  if (!index) {
    Serial.println("[indexSD] ОШИБКА: не удалось создать index на LittleFS");
    playlist.close();
    return;
  }
  
  // Убраны все вызовы display.putRequest и Serial.print внутри рекурсии, 
  // чтобы избежать LoadProhibited на ESP32-S3 при тяжелом I/O.
  listSD(playlist, index, "/", SD_MAX_LEVELS);
  
  index.flush();
  index.close();
  playlist.flush();
  playlist.close();
  Serial.printf("[indexSD] Найдено файлов: %d, playlist=%s, index=%s\n", _sdFCount, PLAYLIST_SD_PATH, INDEX_SD_PATH);
  delay(50);
}
#endif


