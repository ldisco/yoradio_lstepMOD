#include "options.h"

#if SPECTRUM_ANALYZER_ACTIVE

#include "spectrum_analyzer.h"
#include "config.h"
#include "display.h"
#include <dsps_fft2r.h>
#include <esp_heap_caps.h>
#include <math.h>

SpectrumAnalyzer spectrumAnalyzer;

namespace {
  /*
   * Границы 12 логарифмических полос для выходного потока 48 кГц.
   * Нижняя граница начинается с первого ненулевого FFT-bin (46,875 Гц),
   * верхняя ограничена 16 кГц, чтобы не рисовать малополезную ВЧ-область.
   */
  constexpr float BAND_EDGES_HZ[SpectrumAnalyzer::BAND_COUNT + 1] = {
    46.875f, 93.75f, 187.5f, 281.25f, 468.75f, 703.125f, 1054.688f,
    1582.031f, 2373.047f, 3559.570f, 5339.844f, 8000.0f, 16000.0f
  };
  constexpr float OUTPUT_SAMPLE_RATE = 48000.0f;
  constexpr float SILENCE_RMS = 0.00020f;
  constexpr float MIN_REFERENCE_DB = -36.0f;
  constexpr float MAX_REFERENCE_DB = -6.0f;
  constexpr float DISPLAY_RANGE_DB = 48.0f;

  /*
   * Лёгкая визуальная коррекция полос: низ чуть приглушён, верх приподнят.
   * FFT остаётся честным; меняется только отображаемый баланс «как у индикатора».
   */
  constexpr float BAND_WEIGHT[SpectrumAnalyzer::BAND_COUNT] = {
    0.88f, 0.92f, 0.96f, 1.00f, 1.02f, 1.05f,
    1.08f, 1.12f, 1.18f, 1.24f, 1.30f, 1.38f
  };

  void *allocSpectrumMemory(size_t bytes) {
    /*
     * Большие рабочие массивы сначала размещаем в PSRAM. Если внешняя память
     * временно недоступна, оставляем безопасный fallback во внутреннюю кучу.
     */
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    return ptr;
  }
}

bool SpectrumAnalyzer::begin() {
  if (_taskHandle) return true;

  for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
    _pcm[i] = static_cast<int16_t *>(allocSpectrumMemory(FFT_SIZE * sizeof(int16_t)));
  }
  _fft = static_cast<float *>(allocSpectrumMemory(FFT_SIZE * 2U * sizeof(float)));
  _window = static_cast<float *>(allocSpectrumMemory(FFT_SIZE * sizeof(float)));

  bool pcmOk = (_fft != nullptr) && (_window != nullptr);
  for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
    if (!_pcm[i]) pcmOk = false;
  }
  if (!pcmOk) {
    for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
      if (_pcm[i]) heap_caps_free(_pcm[i]);
      _pcm[i] = nullptr;
    }
    if (_fft) heap_caps_free(_fft);
    if (_window) heap_caps_free(_window);
    _fft = _window = nullptr;
    Serial.println("##ERROR#:\tSpectrum: недостаточно памяти для FFT");
    return false;
  }

  /*
   * Окно Hann уменьшает спектральную утечку между соседними полосами.
   * Оно вычисляется один раз, поэтому горячий аудиотракт не содержит sin/cos.
   */
  for (uint16_t i = 0; i < FFT_SIZE; ++i) {
    _window[i] = 0.5f - 0.5f * cosf((2.0f * PI * i) / (FFT_SIZE - 1));
  }

  esp_err_t fftInit = dsps_fft2r_init_fc32(nullptr, FFT_SIZE);
  if (fftInit != ESP_OK && fftInit != ESP_ERR_DSP_REINITIALIZED) {
    for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
      heap_caps_free(_pcm[i]);
      _pcm[i] = nullptr;
    }
    heap_caps_free(_fft);
    heap_caps_free(_window);
    _fft = _window = nullptr;
    Serial.printf("##ERROR#:\tSpectrum: ESP-DSP init=%d\n", static_cast<int>(fftInit));
    return false;
  }

  for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
    _bufferState[i] = BUFFER_FREE;
  }
  _bufferState[0] = BUFFER_WRITING;
  _writeBuffer = 0;
  _writePosition = 0;

  BaseType_t created = xTaskCreatePinnedToCore(
    _taskEntry,
    "Spectrum",
    4096,
    this,
    1,
    &_taskHandle,
    1
  );
  if (created != pdPASS) {
    _taskHandle = nullptr;
    for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
      heap_caps_free(_pcm[i]);
      _pcm[i] = nullptr;
    }
    heap_caps_free(_fft);
    heap_caps_free(_window);
    _fft = _window = nullptr;
    Serial.println("##ERROR#:\tSpectrum: задача анализа не создана");
    return false;
  }

#if DSP_MODEL == DSP_ST7701
  Serial.println("##[BOOT]#\tSpectrum FFT-1024 / 12 bands ready (ST7701 20Hz ring3)");
#else
  Serial.println("##[BOOT]#\tSpectrum FFT-1024 / 12 bands ready");
#endif
  return true;
}

int8_t SpectrumAnalyzer::_findFreeBuffer(uint8_t exceptIndex) const {
  for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
    if (i != exceptIndex && _bufferState[i] == BUFFER_FREE) return static_cast<int8_t>(i);
  }
  return -1;
}

void SpectrumAnalyzer::feed(const int16_t *stereoFrames, int32_t frameCount) {
  if (!_taskHandle || !stereoFrames || frameCount <= 0) return;

  for (int32_t frame = 0; frame < frameCount; ++frame) {
    /* 255 = нет слота записи; для ring×3 нельзя проверять «> 1» — индекс 2 валиден */
    if (_writeBuffer >= PCM_BUFFER_COUNT) {
      portENTER_CRITICAL(&_bufferMux);
      int8_t freeIndex = _findFreeBuffer(255);
      if (freeIndex >= 0) {
        _writeBuffer = static_cast<uint8_t>(freeIndex);
        _bufferState[_writeBuffer] = BUFFER_WRITING;
        _writePosition = 0;
      }
      portEXIT_CRITICAL(&_bufferMux);
      if (_writeBuffer >= PCM_BUFFER_COUNT) return;
    }

    /*
     * Для индикации используем mono-сумму L/R после штатных EQ и Gain.
     * Деление выполняется в int32_t, чтобы сумма двух int16_t не переполнилась.
     */
    int32_t mono = static_cast<int32_t>(stereoFrames[frame * 2])
                 + static_cast<int32_t>(stereoFrames[frame * 2 + 1]);
    _pcm[_writeBuffer][_writePosition++] = static_cast<int16_t>(mono / 2);

    if (_writePosition < FFT_SIZE) continue;

    const uint32_t now = millis();
    const uint8_t completed = _writeBuffer;
    bool published = false;

    portENTER_CRITICAL(&_bufferMux);
    if ((now - _lastPublishedMs) >= ANALYSIS_INTERVAL_MS) {
      _bufferState[completed] = BUFFER_READY;
      published = true;

      int8_t freeIndex = _findFreeBuffer(completed);
      if (freeIndex >= 0) {
        _writeBuffer = static_cast<uint8_t>(freeIndex);
        _bufferState[_writeBuffer] = BUFFER_WRITING;
        _writePosition = 0;
      } else {
#if DSP_MODEL == DSP_ST7701
        /*
         * Opus 2.1 ring: не стопорить I2S-feed (writeBuffer=255 → голод FFT / miss).
         * Сбрасываем другой READY (ещё не взятый задачей) и пишем в него.
         * Если чужого READY нет — не трогаем только что опубликованный кадр:
         * пишем поверх completed и отменяем publish/notify.
         */
        int8_t steal = -1;
        for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
          if (i != completed && _bufferState[i] == BUFFER_READY) {
            steal = static_cast<int8_t>(i);
            break;
          }
        }
        if (steal >= 0) {
          _writeBuffer = static_cast<uint8_t>(steal);
          _bufferState[_writeBuffer] = BUFFER_WRITING;
          _writePosition = 0;
        } else {
          _bufferState[completed] = BUFFER_WRITING;
          _writeBuffer = completed;
          _writePosition = 0;
          published = false;
        }
#else
        _writeBuffer = 255;
        _writePosition = 0;
#endif
      }
      if (published) _lastPublishedMs = now;
    } else {
      /*
       * Полный блок, пришедший раньше лимита анализа, намеренно отбрасываем.
       * Это удерживает нагрузку FFT постоянной независимо от размера декодерных чанков.
       */
      _writePosition = 0;
    }
    portEXIT_CRITICAL(&_bufferMux);

    if (published) xTaskNotifyGive(_taskHandle);
  }
}

int8_t SpectrumAnalyzer::_takeReadyBuffer() {
  int8_t result = -1;
  portENTER_CRITICAL(&_bufferMux);
  for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
    if (_bufferState[i] == BUFFER_READY) {
      _bufferState[i] = BUFFER_PROCESSING;
      result = static_cast<int8_t>(i);
      break;
    }
  }
  portEXIT_CRITICAL(&_bufferMux);
  return result;
}

void SpectrumAnalyzer::_taskEntry(void *parameter) {
  static_cast<SpectrumAnalyzer *>(parameter)->_taskLoop();
}

void SpectrumAnalyzer::_taskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    int8_t readyIndex;
    while ((readyIndex = _takeReadyBuffer()) >= 0) {
      _processBuffer(static_cast<uint8_t>(readyIndex));

      portENTER_CRITICAL(&_bufferMux);
      _bufferState[readyIndex] = BUFFER_FREE;
      portEXIT_CRITICAL(&_bufferMux);
    }
  }
}

void SpectrumAnalyzer::_processBuffer(uint8_t index) {
  float mean = 0.0f;
  float squareSum = 0.0f;
  for (uint16_t i = 0; i < FFT_SIZE; ++i) mean += _pcm[index][i];
  mean /= FFT_SIZE;

  /*
   * ESP-DSP принимает комплексный массив Re/Im. Мнимая часть входа равна нулю,
   * действительная — нормализованный PCM без DC и с окном Hann.
   */
  for (uint16_t i = 0; i < FFT_SIZE; ++i) {
    float sample = (static_cast<float>(_pcm[index][i]) - mean) / 32768.0f;
    squareSum += sample * sample;
    _fft[i * 2] = sample * _window[i];
    _fft[i * 2 + 1] = 0.0f;
  }

  const float rms = sqrtf(squareSum / FFT_SIZE);
  uint8_t rawLevels[BAND_COUNT] = {0};
  if (rms < SILENCE_RMS) {
    _publishLevels(rawLevels);
    return;
  }

  if (dsps_fft2r_fc32(_fft, FFT_SIZE) != ESP_OK
      || dsps_bit_rev_fc32(_fft, FFT_SIZE) != ESP_OK) {
    return;
  }

  float bandDb[BAND_COUNT];
  float frameMaxDb = -96.0f;
  constexpr float binHz = OUTPUT_SAMPLE_RATE / FFT_SIZE;
  constexpr float hannAmplitudeScale = 4.0f / FFT_SIZE;

  for (uint8_t band = 0; band < BAND_COUNT; ++band) {
    uint16_t firstBin = static_cast<uint16_t>(ceilf(BAND_EDGES_HZ[band] / binHz));
    uint16_t lastBin = static_cast<uint16_t>(floorf(BAND_EDGES_HZ[band + 1] / binHz));
    if (firstBin < 1) firstBin = 1;
    if (lastBin >= FFT_SIZE / 2) lastBin = FFT_SIZE / 2 - 1;
    if (lastBin < firstBin) lastBin = firstBin;

    float energy = 0.0f;
    uint16_t bins = 0;
    for (uint16_t bin = firstBin; bin <= lastBin; ++bin) {
      float re = _fft[bin * 2];
      float im = _fft[bin * 2 + 1];
      float amplitude = sqrtf(re * re + im * im) * hannAmplitudeScale;
      energy += amplitude * amplitude;
      ++bins;
    }

    float rmsAmplitude = bins ? sqrtf(energy / bins) : 0.0f;
    /* Косметический вес полосы — до перевода в дБ */
    rmsAmplitude *= BAND_WEIGHT[band];
    bandDb[band] = 20.0f * log10f(rmsAmplitude + 1.0e-9f);
    if (bandDb[band] > frameMaxDb) frameMaxDb = bandDb[band];
  }

  float targetReference = constrain(frameMaxDb, MIN_REFERENCE_DB, MAX_REFERENCE_DB);
  if (targetReference > _referenceDb) {
    /*
     * Плавный подъём AGC: один сильный кик не мгновенно «схлопывает»
     * остальные полосы к нулю на следующем кадре.
     */
    _referenceDb = fminf(targetReference, _referenceDb + 0.9f);
  } else {
    /* Медленнее отпускаем потолок — картинка стабильнее между ударами */
    _referenceDb = fmaxf(targetReference, _referenceDb - 0.28f);
  }

  const float floorDb = _referenceDb - DISPLAY_RANGE_DB;
  for (uint8_t band = 0; band < BAND_COUNT; ++band) {
    float normalized = (bandDb[band] - floorDb) / DISPLAY_RANGE_DB;
    normalized = constrain(normalized, 0.0f, 1.0f);
    rawLevels[band] = normalized < 0.030f
      ? 0
      : static_cast<uint8_t>(ceilf(normalized * SEGMENT_COUNT));
  }

  _publishLevels(rawLevels);
}

void SpectrumAnalyzer::_publishLevels(const uint8_t *rawLevels) {
  const uint32_t now = millis();

  portENTER_CRITICAL(&_resultMux);
  for (uint8_t band = 0; band < BAND_COUNT; ++band) {
    uint8_t requested = min<uint8_t>(rawLevels[band], SEGMENT_COUNT);

    if (requested >= _levels[band]) {
      /* Attack мгновенный — столбик поднимается в такт удару */
      _levels[band] = requested;
    } else if (_levels[band] > 0) {
      /*
       * Release: обычный спад 1 сегмент/кадр; при резком провале (−3 и больше)
       * падаем на 2 — визуально «дышит» вместе с битом, без залипания наверху.
       */
      const uint8_t drop = (_levels[band] - requested >= 3) ? 2 : 1;
      if (_levels[band] > drop) _levels[band] = static_cast<uint8_t>(_levels[band] - drop);
      else _levels[band] = 0;
      if (_levels[band] < requested) _levels[band] = requested;
    }

    if (_levels[band] >= _peaks[band]) {
      _peaks[band] = _levels[band];
      _peakHoldUntil[band] = now + PEAK_HOLD_MS;
      _peakLastFall[band] = now;
    } else if (_peaks[band] > 0
               && static_cast<int32_t>(now - _peakHoldUntil[band]) >= 0
               && (now - _peakLastFall[band]) >= PEAK_FALL_STEP_MS) {
      --_peaks[band];
      _peakLastFall[band] = now;
    }

    if (_peaks[band] < _levels[band]) _peaks[band] = _levels[band];
  }
  ++_sequence;
#if DSP_MODEL == DSP_ST7701
  _hasPublished = true;
  _lastInterDecayMs = now;
#endif
  portEXIT_CRITICAL(&_resultMux);
}

#if DSP_MODEL == DSP_ST7701
bool SpectrumAnalyzer::_decayBetweenFftUnlocked(uint32_t now) {
  if ((now - _lastInterDecayMs) < INTER_FFT_DECAY_MS) return false;
  _lastInterDecayMs = now;
  bool changed = false;
  for (uint8_t band = 0; band < BAND_COUNT; ++band) {
    if (_levels[band] > 0) {
      --_levels[band];
      changed = true;
    }
    if (_peaks[band] > 0
        && static_cast<int32_t>(now - _peakHoldUntil[band]) >= 0
        && (now - _peakLastFall[band]) >= PEAK_FALL_STEP_MS) {
      --_peaks[band];
      _peakLastFall[band] = now;
      changed = true;
    }
    if (_peaks[band] < _levels[band]) {
      _peaks[band] = _levels[band];
      changed = true;
    }
  }
  if (changed) ++_sequence;
  return changed;
}
#endif

bool SpectrumAnalyzer::snapshot(uint8_t *levels, uint8_t *peaks, uint32_t &sequence) {
  if (!levels || !peaks) return false;

  portENTER_CRITICAL(&_resultMux);
#if DSP_MODEL == DSP_ST7701
  /*
   * Между кадрами FFT виджет раньше получал miss≈55% → визуальный «замер».
   * Decay поднимает sequence при изменении уровней; иначе отдаём только fresh FFT.
   */
  if (!_hasPublished) {
    portEXIT_CRITICAL(&_resultMux);
    return false;
  }
  if (sequence == _sequence) {
    _decayBetweenFftUnlocked(millis());
  }
  if (sequence == _sequence) {
    portEXIT_CRITICAL(&_resultMux);
    return false;
  }
#else
  if (sequence == _sequence) {
    portEXIT_CRITICAL(&_resultMux);
    return false;
  }
#endif
  memcpy(levels, _levels, BAND_COUNT);
  memcpy(peaks, _peaks, BAND_COUNT);
  sequence = _sequence;
  portEXIT_CRITICAL(&_resultMux);
  return true;
}

void SpectrumAnalyzer::reset() {
  portENTER_CRITICAL(&_resultMux);
  memset(_levels, 0, sizeof(_levels));
  memset(_peaks, 0, sizeof(_peaks));
  memset(_peakHoldUntil, 0, sizeof(_peakHoldUntil));
  memset(_peakLastFall, 0, sizeof(_peakLastFall));
  ++_sequence;
#if DSP_MODEL == DSP_ST7701
  _hasPublished = false;
  _lastInterDecayMs = 0;
#endif
  portEXIT_CRITICAL(&_resultMux);
}

/*
 * Штатный weak-hook аудиобиблиотеки вызывается после EQ/Gain/ресемплинга.
 * continueI2S всегда устанавливается в true: анализатор наблюдает поток,
 * но никогда не перехватывает и не останавливает вывод на PCM5102.
 */
void audio_process_i2s(int16_t *outBuff, int32_t validSamples, bool *continueI2S) {
  if (continueI2S) *continueI2S = true;
  if (!outBuff || validSamples <= 0) return;
  if (!config.store.vumeter || display.mode() != PLAYER) return;
  spectrumAnalyzer.feed(outBuff, validSamples);
}

#endif
