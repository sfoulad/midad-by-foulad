#pragma once

// Hardware-independent state machine for X4 Pro's overloaded power button: a
// single short click must dispatch the user's configured SHORT_PWRBTN action
// (FORCE_REFRESH/PAGE_TURN/FOOTNOTES/PWR_CONFIRM/...) exactly once, but two
// short clicks within a window instead toggle the frontlight and must NOT
// also dispatch the configured action. See test/x4pro_power_button_gesture/
// for the timing/state coverage this exists to make possible without hardware.
//
// The caller is responsible for the "was this a short click, not a hold"
// filtering (a long-press already means something else, handled separately)
// and for calling update() exactly once per input-polling frame, in frame
// order, whether or not a release happened that frame -- expiry of the
// double-click window is itself a frame event, not tied to any button edge.
class X4ProPowerButtonGesture {
 public:
  enum class Event {
    None,             // Nothing to dispatch this frame.
    ShortClick,       // Dispatch the configured SHORT_PWRBTN action now.
    FrontlightToggle  // Two clicks landed inside the window; toggle the light.
  };

  static constexpr unsigned long DOUBLE_CLICK_WINDOW_MS = 500;

  Event update(unsigned long nowMs, bool shortReleaseThisFrame);

 private:
  bool clickPending = false;
  unsigned long pendingClickAt = 0;
};
