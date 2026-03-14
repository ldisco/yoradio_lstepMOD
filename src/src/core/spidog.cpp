#include "spidog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

SPIDog sdog;

SPIDog::SPIDog() {
  _busy = false;
}

bool SPIDog::begin(){
  if(_spiMutex == NULL){
    _spiMutex = xSemaphoreCreateMutex();
    if(_spiMutex == NULL) return false;
  }
  return true;
}

bool SPIDog::takeMutex(){
  if(_spiMutex == NULL) {
    return false;
  }
  // [Gemini3Pro] Пытаемся захватить мьютекс с таймаутом (1000мс), чтобы избежать бесконечного зависания (deadlock)
  if(xSemaphoreTake(_spiMutex, pdMS_TO_TICKS(1000)) != pdPASS) {
      return false; 
  }
  _busy = true;
  return true;
}

void SPIDog::giveMutex(){
  if(_spiMutex != NULL) xSemaphoreGive(_spiMutex);
  _busy = false;
}

bool SPIDog::canTake(){
  if(_spiMutex == NULL) {
    return false;
  }
  return xSemaphoreTake(_spiMutex, 0) == pdPASS;
}

bool SPIDog::breakMutex(uint8_t ticks){
  if(!_busy){
    giveMutex();
    vTaskDelay(ticks);
    return takeMutex();
  }
  return false;
}
