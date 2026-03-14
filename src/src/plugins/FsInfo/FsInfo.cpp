// FsInfo.cpp — Плагин файловой системы: информация + файловый менеджер
// API: /api/fs-info, /api/fs-list, /api/fs-download, /api/fs-delete, /api/fs-upload, /api/fs-auth
// Защита паролем через FM_PASSWORD в myoptions.h

#include "FsInfo.h"
#include <Arduino.h>
#include <LittleFS.h>
#include "../../AsyncWebServer/ESPAsyncWebServer.h"
#include "../../core/config.h"

extern AsyncWebServer webserver;
extern bool littleFsReady;

FsInfo::FsInfo()
  : _routeRegistered(false) {
  registerPlugin();
}

bool FsInfo::_isPathSafe(const String &path) {
  if (path.length() == 0 || path.indexOf("..") >= 0) return false;
  if (path.charAt(0) != '/') return false;
  return true;
}

bool FsInfo::_isCriticalFile(const String &path) {
  return path == "/data/playlist.csv"
      || path == "/data/wifi.csv"
      || path == "/data/index.dat"
      || path == "/data/indexsd.dat" // Защищаем индекс SD-плейлиста от удаления.
      || path == "/data/playlistsd.csv" // Защищаем SD-плейлист от удаления.
      || path.startsWith("/www/");
}

// Проверка пароля FM (если FM_PASSWORD определён)
bool FsInfo::_checkAuth(AsyncWebServerRequest *request) {
#ifdef FM_PASSWORD
  if (!request->hasParam("key")) return false;
  return request->getParam("key")->value() == FM_PASSWORD;
#else
  return true;
#endif
}

void FsInfo::on_setup() {
  if (_routeRegistered) return;
  _routeRegistered = true;

  // --- GET /api/fs-info — информация о ФС (без пароля) ---
  webserver.on("/api/fs-info", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!littleFsReady) {
      request->send(503, "application/json", "{\"error\":\"LittleFS not ready\"}");
      return;
    }
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes  = LittleFS.usedBytes();
    size_t freeBytes  = totalBytes - usedBytes;
    size_t psramTotal = ESP.getPsramSize();
    size_t psramFree  = ESP.getFreePsram();
    size_t psramUsed  = psramTotal - psramFree;
    char buf[400];
    snprintf(buf, sizeof(buf),
      "{\"totalBytes\":%u,\"usedBytes\":%u,\"freeBytes\":%u,"
      "\"totalKB\":%u,\"usedKB\":%u,\"freeKB\":%u,"
      "\"totalMB\":%.1f,\"usedMB\":%.1f,\"freeMB\":%.1f,"
      "\"psramTotal\":%u,\"psramFree\":%u,\"psramUsed\":%u,"
      "\"psramTotalMB\":%.1f,\"psramFreeMB\":%.1f,\"psramUsedMB\":%.1f}",
      (unsigned)totalBytes, (unsigned)usedBytes, (unsigned)freeBytes,
      (unsigned)(totalBytes/1024), (unsigned)(usedBytes/1024), (unsigned)(freeBytes/1024),
      totalBytes/1048576.0, usedBytes/1048576.0, freeBytes/1048576.0,
      (unsigned)psramTotal, (unsigned)psramFree, (unsigned)psramUsed,
      psramTotal/1048576.0, psramFree/1048576.0, psramUsed/1048576.0
    );
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", buf);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    request->send(response);
  });

  // --- GET /api/fs-auth?key=... — проверка пароля ---
  webserver.on("/api/fs-auth", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (_checkAuth(request)) {
      request->send(200, "application/json", "{\"ok\":true}");
    } else {
      request->send(403, "application/json", "{\"error\":\"wrong password\"}");
    }
  });

  // --- GET /api/fs-list?dir=/path&key=... ---
  webserver.on("/api/fs-list", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_checkAuth(request)) { request->send(403, "application/json", "{\"error\":\"auth\"}"); return; }
    if (!littleFsReady) { request->send(503, "application/json", "{\"error\":\"FS not ready\"}"); return; }
    String dir = "/";
    if (request->hasParam("dir")) dir = request->getParam("dir")->value();
    if (!_isPathSafe(dir)) { request->send(400, "application/json", "{\"error\":\"bad path\"}"); return; }
    if (!lockLittleFS(2000)) { request->send(503, "application/json", "{\"error\":\"FS busy\"}"); return; }
    File root = LittleFS.open(dir);
    if (!root || !root.isDirectory()) {
      unlockLittleFS();
      request->send(404, "application/json", "{\"error\":\"not found\"}");
      return;
    }
    String json = "[";
    bool first = true;
    File entry = root.openNextFile();
    while (entry) {
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"";
      json += entry.name();
      json += "\",\"size\":";
      json += String((unsigned long)entry.size());
      json += ",\"isDir\":";
      json += entry.isDirectory() ? "true" : "false";
      json += "}";
      entry = root.openNextFile();
    }
    json += "]";
    unlockLittleFS();
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // --- GET /api/fs-download?path=...&key=... — скачивание файла ---
  webserver.on("/api/fs-download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_checkAuth(request)) { request->send(403, "text/plain", "auth"); return; }
    if (!littleFsReady) { request->send(503, "text/plain", "FS not ready"); return; }
    if (!request->hasParam("path")) { request->send(400, "text/plain", "missing path"); return; }
    String path = request->getParam("path")->value();
    if (!_isPathSafe(path)) { request->send(400, "text/plain", "bad path"); return; }
    if (!LittleFS.exists(path)) { request->send(404, "text/plain", "not found"); return; }

    String fname = path;
    int sl = path.lastIndexOf('/');
    if (sl >= 0) fname = path.substring(sl + 1);

    // Определяем Content-Type по расширению
    String contentType = "application/octet-stream";
    String lower = fname;
    lower.toLowerCase();
    if (lower.endsWith(".txt") || lower.endsWith(".csv") || lower.endsWith(".md"))
      contentType = "text/plain; charset=utf-8";
    else if (lower.endsWith(".html") || lower.endsWith(".htm"))
      contentType = "text/html; charset=utf-8";
    else if (lower.endsWith(".json"))
      contentType = "application/json";
    else if (lower.endsWith(".jpg") || lower.endsWith(".jpeg"))
      contentType = "image/jpeg";
    else if (lower.endsWith(".png"))
      contentType = "image/png";
    else if (lower.endsWith(".css"))
      contentType = "text/css";
    else if (lower.endsWith(".js"))
      contentType = "text/javascript";

    AsyncWebServerResponse *response = request->beginResponse(LittleFS, path, contentType, true);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // --- GET /api/fs-delete?path=...&key=... — удаление ---
  webserver.on("/api/fs-delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!_checkAuth(request)) { request->send(403, "application/json", "{\"error\":\"auth\"}"); return; }
    if (!littleFsReady) { request->send(503, "application/json", "{\"error\":\"FS not ready\"}"); return; }
    if (!request->hasParam("path")) { request->send(400, "application/json", "{\"error\":\"missing path\"}"); return; }
    String path = request->getParam("path")->value();
    if (!_isPathSafe(path)) { request->send(400, "application/json", "{\"error\":\"bad path\"}"); return; }
    if (_isCriticalFile(path)) { request->send(403, "application/json", "{\"error\":\"protected\"}"); return; }
    if (!lockLittleFS(2000)) { request->send(503, "application/json", "{\"error\":\"FS busy\"}"); return; }
    bool ok = false;
    if (LittleFS.exists(path)) {
      File f = LittleFS.open(path);
      bool isDir = f.isDirectory();
      f.close();
      ok = isDir ? LittleFS.rmdir(path) : LittleFS.remove(path);
    }
    unlockLittleFS();
    if (ok) {
      request->send(200, "application/json", "{\"ok\":true}");
    } else {
      request->send(500, "application/json", "{\"error\":\"failed\"}");
    }
  });

  // --- POST /api/fs-upload — загрузка файла (multipart) ---
  webserver.on("/api/fs-upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!_checkAuth(request)) { request->send(403, "application/json", "{\"error\":\"auth\"}"); return; }
      request->send(200, "application/json", "{\"ok\":true}");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      static String uploadPath;
      if (!index) {
#ifdef FM_PASSWORD
        bool authed = false;
        if (request->hasParam("key", true)) authed = (request->getParam("key", true)->value() == FM_PASSWORD);
        else if (request->hasParam("key")) authed = (request->getParam("key")->value() == FM_PASSWORD);
        if (!authed) return;
#endif
        String dir = "/data/";
        if (request->hasParam("dir", true)) dir = request->getParam("dir", true)->value();
        else if (request->hasParam("dir")) dir = request->getParam("dir")->value();
        if (!dir.endsWith("/")) dir += "/";
        String safeName = filename;
        int sl = safeName.lastIndexOf('/');
        if (sl >= 0) safeName = safeName.substring(sl + 1);
        sl = safeName.lastIndexOf('\\');
        if (sl >= 0) safeName = safeName.substring(sl + 1);
        uploadPath = dir + safeName;
        if (!_isPathSafe(uploadPath)) return;
        if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
        request->_tempFile = LittleFS.open(uploadPath, "w");
        Serial.printf("[FM] Upload: %s\n", uploadPath.c_str());
      }
      if (len && request->_tempFile) request->_tempFile.write(data, len);
      if (final && request->_tempFile) {
        request->_tempFile.close();
        Serial.printf("[FM] Done: %s (%u B)\n", uploadPath.c_str(), index + len);
      }
    }
  );

  Serial.println("[FsInfo] API: fs-info/auth/list/download/delete/upload");
}
