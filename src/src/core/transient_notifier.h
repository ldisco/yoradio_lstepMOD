#ifndef transient_notifier_h
#define transient_notifier_h

#include <Arduino.h>

// Категории кратких системных toast; у каждой своё active — defer не льётся потоком.
enum transientNoticeCategory_e : uint8_t {
  TN_AUDIO_RECOVERY = 0,
  TN_AUDIO_RECOVERED,
  TN_FACT_DEFERRED,
  TN_COVER_DEFERRED,
  TN_NETWORK_RECOVERED,
  TN_NOTICE_CATEGORY_COUNT
};

class TransientNotifier {
  public:
    /** Показать сообщение один раз при переходе категории в active. */
    void show(transientNoticeCategory_e category,
              const char* message,
              bool isError = false,
              uint32_t durationMs = 4000u);
    /** Разрешить следующее уведомление этой категории после завершения состояния. */
    void clear(transientNoticeCategory_e category);
    bool isActive(transientNoticeCategory_e category) const;

  private:
    volatile bool _active[TN_NOTICE_CATEGORY_COUNT] = {};
    volatile uint32_t _lastShownMs[TN_NOTICE_CATEGORY_COUNT] = {};
};

extern TransientNotifier transientNotifier;

#endif
