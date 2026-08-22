#include "X4ProPowerButtonGesture.h"

X4ProPowerButtonGesture::Event X4ProPowerButtonGesture::update(const unsigned long nowMs,
                                                               const bool shortReleaseThisFrame) {
  if (shortReleaseThisFrame) {
    if (clickPending && nowMs - pendingClickAt <= DOUBLE_CLICK_WINDOW_MS) {
      // Second click landed inside the window: consume the pending first
      // click so it never fires, and toggle the light instead.
      clickPending = false;
      return Event::FrontlightToggle;
    }
    if (clickPending) {
      // The prior click's window already expired (polling was delayed past
      // it) -- dispatch the expired click now instead of silently dropping
      // it, and retain this release as the first click of a new gesture.
      pendingClickAt = nowMs;
      return Event::ShortClick;
    }
    // First click: arm the window and wait to see if a second click follows.
    clickPending = true;
    pendingClickAt = nowMs;
    return Event::None;
  }

  if (clickPending && nowMs - pendingClickAt > DOUBLE_CLICK_WINDOW_MS) {
    // No second click arrived in time: the original click was a real,
    // single short click. Dispatch it now, exactly once.
    clickPending = false;
    return Event::ShortClick;
  }
  return Event::None;
}
