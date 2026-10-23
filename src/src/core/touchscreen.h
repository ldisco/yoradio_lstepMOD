#ifndef touchscreen_h
#define touchscreen_h

enum tsDirection_e { TSD_STAY, TSD_LEFT, TSD_RIGHT, TSD_UP, TSD_DOWN, TDS_REQUEST };

class TouchScreen {
  public:
    TouchScreen() {}
    void init(uint16_t w, uint16_t h);
    void loop();
    void flip();
    /* Подготовка тача к пробуждению ESP из deep sleep (CST820: INT/RST). Для других моделей — пустышка. */
    void prepareDeepSleepWake();
    /* Подготовка CST820 к light sleep: I2C-poll без авто-Standby. Для других моделей — пустышка. */
    void prepareLightSleepWake();
    /* true — палец на экране (CST820 по I2C). Для light-sleep пробуждения. */
    bool pollFingerPresent();
    /* Диагностика линии INT→WAKE_PIN без ухода в deep sleep: проверяет наличие провода,
     * тип выхода INT (push-pull / открытый коллектор) и ловит импульсы прерывания
     * в том же режиме чипа (Standby), в котором он находится во время сна ESP. */
    void startWakeIntTest();
  private:
    /* Запуск теста INT в контексте основного цикла (I2C + ожидание Standby). */
    void _wakeIntTestBegin();
    /* Обслуживание активного теста INT (печать счётчиков, финальный отчёт). */
    void _wakeIntTestService();
    uint16_t _oldTouchX, _oldTouchY, _width, _height;
    uint32_t _touchdelay;
    bool _checklpdelay(int m, uint32_t &tstamp);
    tsDirection_e _tsDirection(uint16_t x, uint16_t y);
    bool _istouched();
    /* Тап начался в верхней полосе meta — не трактовать микро-свайп как VOL/плейлист. */
    bool _inTopBar(uint16_t touchY) const;
    bool _touchTopBarStart = false;
};

extern TouchScreen touchscreen;

#endif
