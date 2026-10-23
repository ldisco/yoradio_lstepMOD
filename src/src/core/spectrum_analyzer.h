#ifndef spectrum_analyzer_h
#define spectrum_analyzer_h

#include <Arduino.h>
#include "options.h"

/*
 * Анализатор компилируется только при SPECTRUM_ANALYZER_ACTIVE (options.h):
 * ST7789 / ILI9488/86 landscape / AXS15231B (+ флаг профиля) + I2S.
 * Остальные профили продолжают штатный VU без этого кода.
 *
 * DSP_ST7701 (Supremus): Opus 2.1 — 20 Гц, ring×3, decay между FFT;
 * остальные DSP — прежнее поведение (33 мс, 2 буфера).
 */
#if SPECTRUM_ANALYZER_ACTIVE

class SpectrumAnalyzer {
  public:
    static constexpr uint8_t BAND_COUNT = 12;
    static constexpr uint8_t SEGMENT_COUNT = 8;

    /*
     * Инициализирует FFT-буферы и отдельную задачу анализа.
     * Вызывается один раз из Player::init(), до начала воспроизведения.
     */
    bool begin();

    /*
     * Быстро принимает готовые стереофреймы I2S.
     * Метод не выполняет FFT и не ждёт освобождения занятого буфера.
     */
    void feed(const int16_t *stereoFrames, int32_t frameCount);

    /*
     * Копирует согласованный кадр уровней и пиков для дисплейной задачи.
     * Возвращает false, если после предыдущего чтения нового кадра ещё нет.
     * ST7701: между FFT крутит decay и отдаёт кадр, чтобы UI не «замирал» на miss.
     */
    bool snapshot(uint8_t *levels, uint8_t *peaks, uint32_t &sequence);

    /*
     * Очищает отображаемые уровни при остановке или выключении анализатора.
     * PCM-буферы при этом остаются выделенными и готовы к следующему старту.
     */
    void reset();

  private:
    static constexpr uint16_t FFT_SIZE = 1024;
#if DSP_MODEL == DSP_ST7701
    /* Supremus Opus 2.1: ~20 Гц — стабильнее под FLAC, чем 30 Гц при ~55% miss */
    static constexpr uint32_t ANALYSIS_INTERVAL_MS = 50;
    static constexpr uint8_t PCM_BUFFER_COUNT = 3;
    /* Спад сегмента между кадрами FFT (снижает «замер» при ровном widget loop) */
    static constexpr uint32_t INTER_FFT_DECAY_MS = 50;
#else
    /* ~30 Гц анализа — плотнее к ритму, без лишней нагрузки на Core 1 */
    static constexpr uint32_t ANALYSIS_INTERVAL_MS = 33;
    static constexpr uint8_t PCM_BUFFER_COUNT = 2;
#endif
    /* Peak-hold короче: 400 мс вместо 1 с — «пики» ближе к биту */
    static constexpr uint32_t PEAK_HOLD_MS = 400;
    /* После удержания пик падает заметно быстрее */
    static constexpr uint32_t PEAK_FALL_STEP_MS = 70;

    enum BufferState : uint8_t {
      BUFFER_FREE = 0,
      BUFFER_WRITING,
      BUFFER_READY,
      BUFFER_PROCESSING
    };

    int16_t *_pcm[PCM_BUFFER_COUNT] = {};
    float *_fft = nullptr;
    float *_window = nullptr;
    volatile uint8_t _bufferState[PCM_BUFFER_COUNT] = {};
    uint8_t _writeBuffer = 0;
    uint16_t _writePosition = 0;
    uint32_t _lastPublishedMs = 0;
#if DSP_MODEL == DSP_ST7701
    uint32_t _lastInterDecayMs = 0;
    bool _hasPublished = false;
#endif

    uint8_t _levels[BAND_COUNT] = {0};
    uint8_t _peaks[BAND_COUNT] = {0};
    uint32_t _peakHoldUntil[BAND_COUNT] = {0};
    uint32_t _peakLastFall[BAND_COUNT] = {0};
    uint32_t _sequence = 0;
    float _referenceDb = -18.0f;

    TaskHandle_t _taskHandle = nullptr;
    portMUX_TYPE _bufferMux = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE _resultMux = portMUX_INITIALIZER_UNLOCKED;

    static void _taskEntry(void *parameter);
    void _taskLoop();
    void _processBuffer(uint8_t index);
    int8_t _takeReadyBuffer();
    int8_t _findFreeBuffer(uint8_t exceptIndex) const;
    void _publishLevels(const uint8_t *rawLevels);
#if DSP_MODEL == DSP_ST7701
    /* Спад уровней/пиков без нового FFT; меняет _levels под mutex вызывающего */
    bool _decayBetweenFftUnlocked(uint32_t now);
#endif
};

extern SpectrumAnalyzer spectrumAnalyzer;

#endif
#endif
