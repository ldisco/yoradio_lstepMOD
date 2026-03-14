// FsInfo.h — Плагин файловой системы: информация + файловый менеджер
// Эндпойнты: /api/fs-info, /api/fs-list, /api/fs-download, /api/fs-delete, /api/fs-upload
// Защита паролем через FM_PASSWORD в myoptions.h
#ifndef FSINFO_H
#define FSINFO_H

#include "../../pluginsManager/pluginsManager.h"

class AsyncWebServerRequest;

class FsInfo : public Plugin {
public:
  FsInfo();
  void on_setup() override;

private:
  bool _routeRegistered;
  static bool _isPathSafe(const String &path);
  static bool _isCriticalFile(const String &path);
  static bool _checkAuth(AsyncWebServerRequest *request);
};

#endif // FSINFO_H
