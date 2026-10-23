#ifndef SLEEPTIMER_PLUGIN_H
#define SLEEPTIMER_PLUGIN_H

#include <Arduino.h>
#include "../../pluginsManager/pluginsManager.h"

// Флаг сценария таймера сна / softSleep: блокирует авто-retry и запись в подсветку.
extern volatile bool g_sleepTimerShutdown;

class SleepTimer : public Plugin {
public:
  SleepTimer();

  // Инициализация плагина при старте системы.
  void on_setup() override;

  // Тик раз в секунду: основной цикл таймера и обратного отсчета.
  void on_ticker() override;

  // Циклическое переключение пресетов: 0 -> 15 -> 30 -> 45 -> 60 -> 90 -> 120 -> 0.
  void cyclePreset();

  // Включение/выключение режима "ALL OFF" (аппаратное отключение питания).
  void setAllOffEnabled(bool enabled);

  // Текущее состояние для Web UI.
  uint16_t selectedMinutes() const { return _selectedMinutes; }
  uint32_t remainingSeconds() const;
  bool isActive() const { return _deadlineMs != 0; }
  bool isAllOffEnabled() const { return _allOffEnabled; }
  bool hasPowerPin() const;

  // Явная отправка состояния в WebSocket (используется при getindex/getsystem).
  // clientId==0 — textAll всем; иначе только запросившему клиенту.
  void pushState(uint8_t clientId = 0);

private:
  uint16_t _selectedMinutes;
  uint32_t _deadlineMs;
  bool _allOffEnabled;
  uint8_t _lastMinuteBucket;

  uint8_t _powerActiveLevel() const;
  uint8_t _powerInactiveLevel() const;
  void _applyPowerPinInactive();
  void _handleExpired();
  /* CST820: light sleep + опрос тача по I2C (INT не нужен). Возврат после касания/GPIO/таймаута. */
  void _enterLightSleepTouchWake();
  uint16_t _nextPreset(uint16_t current) const;
  void _showToast(const String& text);
};

extern SleepTimer sleepTimerPlugin;

#endif
