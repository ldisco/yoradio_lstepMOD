// CoverArt.h
#ifndef CoverArt_H
#define CoverArt_H

#include "../../pluginsManager/pluginsManager.h"


class CoverArt : public Plugin {
public:
    CoverArt();
    void on_ticker() override; // Будет вызываться каждую секунду
    void on_station_change() override;
    
private:
    String lastTitle; // Запоминаем последний заголовок
    void processMetadata(const char* title);
};

#endif