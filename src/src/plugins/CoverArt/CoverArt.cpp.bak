// CoverArt.cpp
#include "CoverArt.h"
#include <Arduino.h>

// ВРЕМЕННО: Используем глобальную переменную из проекта
// Вместо сложной структуры, просто объявим внешнюю переменную для title
extern "C" {
    // Объявляем, что где-то есть эта переменная
    extern const char* currentMetadataTitle;
}

CoverArt::CoverArt() {
    registerPlugin();
    // ЛОГ: сокращено — подавляем сообщение регистрации плагина
    // Serial.println("[CoverArt] Plugin registered");
    lastTitle = "";
}

void CoverArt::on_station_change() {
    // ЛОГ: сокращено — сообщения об изменении станции подавлены
    // Serial.println("[CoverArt] Station changed!");
    lastTitle = "";
}

void CoverArt::on_ticker() {
    // ВРЕМЕННАЯ ЗАГЛУШКА: просто выводим сообщение
    static int counter = 0;
    if (counter++ % 5 == 0) { // Каждые 5 секунд
        // ЛОГ: сокращено — периодический heartbeat тикера подавлён
        // Serial.printf("[CoverArt] Ticker is alive! Count: %d\n", counter);
        
        // TODO: Позже получим доступ к реальным метаданным
        // Если currentMetadataTitle доступен
        // if (currentMetadataTitle) {
        //     processMetadata(currentMetadataTitle);
        // }
    }
}

void CoverArt::processMetadata(const char* title) {
    // ЛОГ: сокращено — лог обработки метаданных подавлён
    // Serial.printf("[CoverArt] Would process: %s\n", title);
}