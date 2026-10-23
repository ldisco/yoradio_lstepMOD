#ifndef sdmanager_h
#define sdmanager_h

class SDManager : public SDFS {
  public:
    bool ready;
  public:
    SDManager(FSImplPtr impl) : SDFS(impl) {}
    bool start();
    // Быстрая попытка mount при смене режима: одна инициализация без повторных retry (~200 мс вместо ~3 с).
    bool startQuick();
    void stop();
    bool cardPresent();
    // Отпечаток SD-карты по CID (MID+OID+PSN) — для обнаружения смены носителя.
    bool readCardFingerprint(uint64_t *out);
    void listSD(File &plSDfile, File &plSDindex, const char * dirname, uint8_t levels);
    void indexSDPlaylist();
  private:
    uint32_t _sdFCount;
  private:
    bool _checkNoMedia(const char* path);
    bool _endsWith (const char* base, const char* str);
    bool _endsWithCI (const char* base, const char* str);
};

extern SDManager sdman;
#if defined(SD_SPIPINS) || SD_HSPI
extern SPIClass  SDSPI;
#endif
#endif
