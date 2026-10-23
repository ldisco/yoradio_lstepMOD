#include "options.h"
#include "transient_notifier.h"
#include "config.h"
#include "display.h"
#include "netserver.h"

/*
 * Нижняя строка уже существует только у landscape cover-SS.
 * Новых CSS-классов, координат и display widgets здесь не создаём:
 * переиспользуем проверенный showFactStatusToast().
 */
#if DSP_MODEL==DSP_ST7789 || DSP_MODEL==DSP_ST7789_240 || DSP_MODEL==DSP_ST7789_76
  #include "../displays/displayST7789.h"
  #define TRANSIENT_COVER_TOAST_NS st7789_cover
#elif (DSP_MODEL==DSP_ILI9488 || DSP_MODEL==DSP_ILI9486) && !(defined(ILI9488_PORTRAIT) && ILI9488_PORTRAIT)
  #include "../displays/displayILI9488.h"
  #define TRANSIENT_COVER_TOAST_NS ili9488_cover
#elif DSP_MODEL==DSP_AXS15231B
  #include "../displays/displayAXS15231B.h"
  #define TRANSIENT_COVER_TOAST_NS axs_cover
#endif

TransientNotifier transientNotifier;

void TransientNotifier::show(transientNoticeCategory_e category,
                             const char* message,
                             bool isError,
                             uint32_t durationMs) {
  if (category >= TN_NOTICE_CATEGORY_COUNT || message == nullptr || message[0] == '\0') return;

  const uint32_t now = millis();
  /*
   * active подавляет повтор на каждом тике, а короткий cooldown
   * защищает от дребезга active→clear→active при пограничном уровне буфера.
   * Запись bool/uint32_t атомарна на ESP32; ложный однократный пропуск безопаснее,
   * чем блокировка WebSocket/display под общей критической секцией.
   */
  if (_active[category]) return;
  if (_lastShownMs[category] != 0
      && (uint32_t)(now - _lastShownMs[category]) < 15000u) {
    _active[category] = true;
    return;
  }
  _active[category] = true;
  _lastShownMs[category] = now;

  netserver.sendToast(message, isError);

#if defined(TRANSIENT_COVER_TOAST_NS)
  const displayMode_e m = display.mode();
  if (config.isScreensaver && m == SCREENBLANK) {
    TRANSIENT_COVER_TOAST_NS::showFactStatusToast(message, durationMs, false);
  } else if (m == PLAYER && display.ready()) {
    /* PLAYER Faster: не до display.ready() — canvas ещё нет (boot crash). */
    TRANSIENT_COVER_TOAST_NS::showFactStatusToast(message, durationMs, false);
  }
#else
  (void)durationMs;
#endif
}

void TransientNotifier::clear(transientNoticeCategory_e category) {
  if (category >= TN_NOTICE_CATEGORY_COUNT) return;
  _active[category] = false;
}

bool TransientNotifier::isActive(transientNoticeCategory_e category) const {
  if (category >= TN_NOTICE_CATEGORY_COUNT) return false;
  return _active[category];
}
