#ifndef AUDIOHANDLERS_H
#define AUDIOHANDLERS_H

//=============================================//
//              Audio handlers                 //
//=============================================//

// Буферы для ID3-тегов SD-треков (artist/title/album) — держим их здесь,
// чтобы собрать корректное название до того, как дернём дисплей и веб.
static char g_id3Artist[BUFLEN] = {0};
static char g_id3Title[BUFLEN]  = {0};
static char g_id3Album[BUFLEN]  = {0};

// Debounce для ID3 тегов — отправляем данные только через 500мс после последнего тега
static uint32_t g_id3LastUpdate = 0;
static bool g_id3Pending = false;

// DLNA-трек физически играет как обычный WEB-поток (HTTP GET файла с телефона/сервера,
// PM_WEB), но у него, как и у SD-файла, есть настоящие ID3-теги (Title/Artist/Album) —
// в отличие от радиостанций, где название приходит через ICY StreamTitle. Поэтому конвейер
// сборки ID3 (изначально писавшийся только для PM_SDCARD) должен работать и здесь, иначе
// WebUI/дисплей застревают на служебном "[соединение]" даже после getReady/PLAYING.
static inline bool isSdLikeId3Playback() {
  return config.getMode() == PM_SDCARD ||
         (config.getMode() == PM_WEB && config.getPlaylistSource() == PL_SRC_DLNA);
}

// Сброс всех ID3-буферов и debounce-таймера.
// Вызывается из _stop(), чтобы не всплывало старое название после стопа (debounce 500мс).
void clearId3State() {
  g_id3Pending = false;
  memset(g_id3Artist, 0, BUFLEN);
  memset(g_id3Title, 0, BUFLEN);
  memset(g_id3Album, 0, BUFLEN);
}

// Извлекает имя родительской папки из пути файла на SD.
// Пример: "/Music/Jazz/track.mp3" → "Jazz" (true); "/track.mp3" или "track.mp3" → false (корень).
static bool extractSdParentFolder(const char* path, char* out, size_t outLen) {
  if (!path || !path[0] || !out || outLen == 0) return false;
  out[0] = '\0';
  const char* lastSlash = strrchr(path, '/');
  if (!lastSlash) return false;           // нет каталога — только имя файла
  if (lastSlash == path) return false;    // "/file.mp3" — файл в корне
  // Ищем начало имени папки: символ после предыдущего '/' (или начало строки).
  const char* p = lastSlash - 1;
  while (p > path && *p != '/') p--;
  const char* folderStart = (*p == '/') ? (p + 1) : path;
  size_t len = (size_t)(lastSlash - folderStart);
  if (len == 0 || len >= outLen) return false;
  memcpy(out, folderStart, len);
  out[len] = '\0';
  return true;
}

// Функция для финальной отправки собранных ID3 данных (вызывается из timekeeper loop)
static void flushSdId3Meta() {
  if(!g_id3Pending) return;
  if(!isSdLikeId3Playback()) { g_id3Pending = false; return; }
  // Не отправляем устаревшие ID3-теги, если плеер уже остановлен.
  // Debounce (500мс) мог сработать ПОСЛЕ _stop() и вернуть старое название.
  if(player.status() != PLAYING) { g_id3Pending = false; return; }
  
  // Ждём 500мс после последнего ID3 тега чтобы собрать все
  if(millis() - g_id3LastUpdate < 500) return;
  g_id3Pending = false;
  
  const bool hasArtist = strlen(g_id3Artist) > 0;
  const bool hasAlbum  = strlen(g_id3Album) > 0;
  const bool hasTitle  = strlen(g_id3Title) > 0;
  
  // Низ (meta / station.title): всегда «Artist - Title», иначе только Title.
  char titleDisplay[BUFLEN] = {0};
  if(hasArtist && hasTitle) {
    snprintf(titleDisplay, sizeof(titleDisplay), "%s - %s", g_id3Artist, g_id3Title);
  } else if(hasTitle) {
    strlcpy(titleDisplay, g_id3Title, sizeof(titleDisplay));
  } else if(hasArtist) {
    // Редкий случай — есть только Artist, без Title: показываем хотя бы его.
    strlcpy(titleDisplay, g_id3Artist, sizeof(titleDisplay));
  }
  
  // DLNA: только нижняя строка meta. Верхнюю nameset НЕ трогаем —
  // там имя из playlist_dlna.csv (иначе путаница WebUI).
  const bool dlnaId3 = (config.getMode() == PM_WEB && config.getPlaylistSource() == PL_SRC_DLNA);

  // Верх (nameset / station.name) — только для SD:
  // папка (если не корень) → иначе Album → иначе Artist.
  // Имя файла (.mp3) наверх не ставим. Пустой setStation("") не делаем —
  // лишний раз дёргать дисплей/WS на старте не нужно.
  char stationDisplay[BUFLEN] = {0};
  if (!dlnaId3) {
    char folder[BUFLEN] = {0};
    if (extractSdParentFolder(config.station.url, folder, sizeof(folder))) {
      strlcpy(stationDisplay, folder, sizeof(stationDisplay));
    } else if (hasAlbum) {
      strlcpy(stationDisplay, g_id3Album, sizeof(stationDisplay));
    } else if (hasArtist) {
      strlcpy(stationDisplay, g_id3Artist, sizeof(stationDisplay));
    }
  }

  if (!dlnaId3 && strlen(stationDisplay) > 0) {
    config.setStation(stationDisplay);
  }
  
  // metaArtist для SD/DLNA — нужно TrackFacts
  {
    extern String metaArtist;
    metaArtist = String(g_id3Artist);
  }
  
  if(strlen(titleDisplay) > 0) {
    extern String metaTitle;
    metaTitle = String(titleDisplay);
    config.setTitle(titleDisplay);
  }
  
  // Отправляем обновление UI только ОДИН раз после сбора всех тегов
  if (dlnaId3) {
    // Только TITLE: meta уже ушёл через setTitle → requestOnChange(TITLE).
    // NEWSTATION/STATION не шлём — nameset остаётся из плейлиста.
  } else {
    display.putRequest(NEWSTATION);
    netserver.requestOnChange(STATION, 0);
  }
}

// Сборка ID3 данных БЕЗ немедленной отправки - используем debounce
static void updateSdId3Meta() {
  if(!isSdLikeId3Playback()) return;
  g_id3LastUpdate = millis();
  g_id3Pending = true;
  // Фактическая отправка будет в flushSdId3Meta() после 500мс паузы
}

// Сброс буферов при начале нового файла на SD.
static void resetSdId3Buffers(){
  memset(g_id3Artist, 0, sizeof(g_id3Artist));
  memset(g_id3Title,  0, sizeof(g_id3Title));
  memset(g_id3Album,  0, sizeof(g_id3Album));
  g_id3Pending = false;
}

void audio_info(const char *info) {
  // Критично: формат потока обновлять ДО раннего return по lockOutput.
  // Иначе при lockOutput=true строка "format is flac" не доходит до setBitrateFormat,
  // конфиг остаётся BF_OGG (после "format is ogg"), и TrackFacts не видит FLAC
  // (не срабатывает блок авто-AI на FLAC → лишний DeepSeek + нагрузка на SSL).
  if (strstr(info, "format is aac")  != NULL) { g_audioFormatFlacActive = false; config.setBitrateFormat(BF_AAC); display.putRequest(DBITRATE); netserver.requestOnChange(BITRATE, 0); }
  if (strstr(info, "format is flac") != NULL) { g_audioFormatFlacActive = true; config.setBitrateFormat(BF_FLAC); display.putRequest(DBITRATE); netserver.requestOnChange(BITRATE, 0); }
  if (strstr(info, "format is mp3")  != NULL) { g_audioFormatFlacActive = false; config.setBitrateFormat(BF_MP3); display.putRequest(DBITRATE); netserver.requestOnChange(BITRATE, 0); }
  if (strstr(info, "format is wav")  != NULL) { g_audioFormatFlacActive = false; config.setBitrateFormat(BF_WAV); display.putRequest(DBITRATE); netserver.requestOnChange(BITRATE, 0); }
  if (strstr(info, "format is ogg")  != NULL) { g_audioFormatFlacActive = false; config.setBitrateFormat(BF_OGG); display.putRequest(DBITRATE); netserver.requestOnChange(BITRATE, 0); }
  // Битрейт обрабатываем до проверки lockOutput:
  // на ряде потоков событие "BitRate: ..." приходит только в начале соединения,
  // и если в этот момент lockOutput=true, UI навсегда остаётся с 0 kBits.
  /*char* iciEarly;
  char bEarly[20] = {0};
  if ((iciEarly = strstr(info, "BitRate: ")) != NULL) {
    strlcpy(bEarly, iciEarly + 9, sizeof(bEarly)); // Ограничиваем копирование размером буфера.
    audio_bitrate(bEarly);
  }*/

  if(player.lockOutput) return;
  // Сообщения восстановления декодера не в лог (Serial блокирует → фризы); формат/BitRate для UI оставляем.
  const bool isDecoderSpam = (strstr(info, "invalid frameheader") != NULL) ||
                             (strstr(info, "syncword found") != NULL) ||
                             (strstr(info, "BitRate:") != NULL) ||
                             (strstr(info, "Channels:") != NULL) ||
                             (strstr(info, "SampleRate:") != NULL) ||
                             (strstr(info, "BitsPerSample:") != NULL) ||
                             (strstr(info, "invalid Huffman") != NULL) ||
                             (strstr(info, "invalid sideinfo") != NULL);
  if (config.store.audioinfo && !isDecoderSpam) telnet.printf("##AUDIO.INFO#: %s\n", info);
  #ifdef USE_NEXTION
    nextion.audioinfo(info);
  #endif
  // format is * — см. блок в начале функции (до lockOutput)
  // "skip metadata": не в SD — title ← station.name; в SD метаданные через ID3 (flushSdId3Meta).
  // Системные статусы "[остановлено]"/"[готов]" при стопе не затираем колбэками после Stop().
  if (strstr(info, "skip metadata") != NULL && config.getMode() != PM_SDCARD) {
    config.setTitle(config.station.name);
  }
  if (strstr(info, "Account already in use") != NULL || strstr(info, "HTTP/1.0 401") != NULL) {
    player.setError(info);
    
  }
  char* ici; char b[20]={0};
  if ((ici = strstr(info, "BitRate: ")) != NULL) {
    strlcpy(b, ici + 9, sizeof(b)); // копирование ограничено размером буфера
    audio_bitrate(b);
  }
}

void audio_bitrate(const char *info)
{
  const int newRate = atoi(info) / 1000;
  // Не шлём в лог/дисплей при том же битрейте — меньше нагрузки на нестабильном потоке.
  if (newRate == config.station.bitrate) return;
  config.station.bitrate = newRate;
  if(config.store.audioinfo) telnet.printf("%s %s\n", "##AUDIO.BITRATE#:", info);
  display.putRequest(DBITRATE);
  #ifdef USE_NEXTION
    nextion.bitrate(config.station.bitrate);
  #endif
  netserver.requestOnChange(BITRATE, 0);
}

bool printable(const char *info) {
  if(L10N_LANGUAGE!=RU) return true;
  bool p = true;
  for (int c = 0; c < strlen(info); c++)
  {
    if ((uint8_t)info[c] > 0x7e || (uint8_t)info[c] < 0x20) p = false;
  }
  if (!p) p = (uint8_t)info[0] >= 0xC2 && (uint8_t)info[1] >= 0x80 && (uint8_t)info[1] <= 0xBF;
  return p;
}

void audio_showstation(const char *info) {
  // В SD режиме не используем эту функцию - метаданные из ID3 тегов
  if (config.getMode() == PM_SDCARD) return;
  bool p = printable(info) && (strlen(info) > 0);(void)p;
  if(player.remoteStationName){
    config.setStation(p?info:config.station.name);
    display.putRequest(NEWSTATION);
    netserver.requestOnChange(STATION, 0);
  }
}

void audio_showstreamtitle(const char *info) {
  // В SD режиме метаданные приходят через ID3 теги, StreamTitle не используем.
  if (config.getMode() == PM_SDCARD) return;
  // Как в референсе: просто принимаем StreamTitle и кладём в setTitle.
  // setTitle сам решит, можно ли перезаписать текущий заголовок.
  if (strstr(info, "Account already in use") != NULL || strstr(info, "HTTP/1.0 401") != NULL || strstr(info, "HTTP/1.1 401") != NULL) player.setError(info);
  bool p = printable(info) && (strlen(info) > 0);
  #ifdef DEBUG_TITLES
    config.setTitle(DEBUG_TITLES);
  #else
    config.setTitle(p?info:config.station.name);
  #endif
}

void audio_error(const char *info) {
  // Как в референсе: передаём ошибку в player (заголовок/дисплей).
  player.setError(info);
  // Всплывающее уведомление внизу веб-интерфейса (тот же toast, что у таймера сна).
  if (info && strcmp(info, "timeout") == 0) {
    netserver.sendToast("Станция не отвечает", true);
  }
}

// Fallback для icy-description: используем описание станции как тег,
// только если НЕТ нормальных метаданных трека.
void audio_icydescription(const char *info) {
  if(player.lockOutput) return;
  if (!info || !info[0]) return; // пустую строку игнорируем

  // Работаем только в WEB-режиме для реально играющего потока.
  if (config.getMode() != PM_WEB) return;
  if (player.status() != PLAYING) return;

  // Если уже есть нормальный тег (не служебный статус), не трогаем его.
  const char* cur = config.station.title;
  if (cur && cur[0]) {
    // Служебные статусы, которые можно перезаписать: "[соединение]", "[остановлено]", "[готов]".
    bool isService =
      (strstr(cur, "[соедин") != nullptr) ||   // "[соединение]"
      (strstr(cur, "[остановлено]") != nullptr) ||
      (strstr(cur, "[готов]") != nullptr);
    if (!isService) return;
  }

  // Слишком короткие description не используем.
  if (strlen(info) < 3) return;

  // Используем icy-description как нижний визуальный тег станции.
  // ВАЖНО: metaTitle не трогаем, чтобы TrackFacts не путали описание станции с названием трека.
  config.setTitle(info);
}

void audio_id3artist(const char *info){
  if(player.lockOutput) return;
  if(!info || !info[0]) return;  // не затирать fallback пустой строкой
  if(!printable(info)) return;
  strlcpy(g_id3Artist, info, sizeof(g_id3Artist));
  updateSdId3Meta();
}

void audio_id3album(const char *info){
  if(player.lockOutput) return;
  if(!info || !info[0]) return;  // не затирать fallback пустой строкой
  if(!printable(info)) return;
  strlcpy(g_id3Album, info, sizeof(g_id3Album));
  updateSdId3Meta();
}

void audio_id3title(const char *info){
  if(player.lockOutput) return;
  if(!info || !info[0]) return;  // не затирать fallback пустой строкой
  if(!printable(info)) return;
  strlcpy(g_id3Title, info, sizeof(g_id3Title));
  updateSdId3Meta();
}

void audio_beginSDread(){
  // Новый трек на SD — обнуляем кэш тегов
  resetSdId3Buffers();
  config.setTitle("");
  // Сбрасываем file-key предыдущего трека чтобы не показывать старую обложку
  extern String g_sdFileKey;
  g_sdFileKey = "";

  // ВАЖНО: здесь НЕ вызываем setStation/NEWSTATION/requestOnChange.
  // audio_beginSDread идёт из PlayerTask (Core 0) во время открытия файла на SD —
  // лишние WS/дисплей в этот момент конкурируют с Wi‑Fi (ppTask на Core 0)
  // и давали INT-WDT сразу после «Ready in SD Mode» / SmartStart.
  // Верх (папка/Album/Artist) выставляется позже в flushSdId3Meta() с loop (Core 1).

  // Имя файла как fallback в g_id3Title (низ), без расширения; ID3 перезапишет позже.
  if(isSdLikeId3Playback() && strlen(config.station.url) > 0) {
    const char* filename = strrchr(config.station.url, '/');
    if(filename) {
      filename++;
    } else {
      filename = config.station.url;
    }
    // Убираем расширение
    char tempName[BUFLEN];
    strlcpy(tempName, filename, sizeof(tempName));
    char* ext = strrchr(tempName, '.');
    if(ext) *ext = '\0';
    
    // Парсим "Artist - Title" из имени файла
    char* dash = strstr(tempName, " - ");
    if(dash) {
      *dash = '\0';
      strlcpy(g_id3Artist, tempName, sizeof(g_id3Artist));
      strlcpy(g_id3Title, dash + 3, sizeof(g_id3Title));
    } else {
      // Нет разделителя - используем всё имя как title
      strlcpy(g_id3Title, tempName, sizeof(g_id3Title));
    }
    
    // Запускаем debounce - данные отправятся через 500мс если не придут ID3 теги
    g_id3LastUpdate = millis();
    g_id3Pending = true;
  }
}

void audio_id3data(const char *info){  //id3 metadata
    if(player.lockOutput) return;
    telnet.printf("##AUDIO.ID3#: %s\n", info);
}

void audio_eof_mp3(const char *info){  //end of file
    config.sdResumePos = 0;
    player.next();
}

void audio_eof_stream(const char *info){
#ifdef USE_DLNA
  // DLNA-файл — конечный HTTP-поток: после EOF переходим к следующему треку, как на SD.
  if (config.getMode() == PM_WEB && config.getPlaylistSource() == PL_SRC_DLNA) {
    config.sdResumePos = 0;
    player.next();
    return;
  }
#endif
  player.sendCommand({PR_STOP, 0});
  if(!player.resumeAfterUrl && config.getMode()!=PM_WEB) return;
  if (config.getMode()==PM_WEB){
    player.sendCommand({PR_PLAY, config.lastStation()});
  }else{
    player.setResumeFilePos( config.sdResumePos==0?0:config.sdResumePos-player.sd_min);
    player.sendCommand({PR_PLAY, config.lastStation()});
  }
}

void audio_progress(uint32_t startpos, uint32_t endpos){
  player.sd_min = startpos;
  player.sd_max = endpos;
  netserver.requestOnChange(SDLEN, 0);
}

#endif
