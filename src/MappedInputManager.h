#pragma once

#include <HalGPIO.h>

#include <cstdint>

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button {
    Back,
    Confirm,
    Left,
    Right,
    Up,
    Down,
    Power,
    PageBack,
    PageForward,
    NavNext,
    NavPrevious,
    // Like NavNext/NavPrevious, but for a purely VERTICAL up/down list-scroll
    // concept (front buttons doubling as list-scroll shortcuts alongside the
    // side Up/Down buttons, hinted with literal "Up"/"Down" labels) rather
    // than a generic left/right "next/previous item" -- see mapButton()'s
    // ScrollNext/ScrollPrevious cases for why these must NOT apply NavNext/
    // NavPrevious's RTL horizontal flip.
    ScrollNext,
    ScrollPrevious
  };
  enum class SwipeDir { None, Left, Right, Up, Down };
  // Combined touch interaction for a band of equal rows/columns with caller-supplied
  // geometry -- the shared hit-test for lists the theme helpers do not cover directly
  // (custom row heights, option prompts, menus). Down = a held tap-candidate is on a
  // row/col (update the selection highlight); Tap = a tap released on one (activate).
  enum class RowTouch : uint8_t { None, Down, Tap };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  void update() const { gpio.update(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;

  // --- Touch bridge (Path 2 of docs/contributing/touch-and-ui.md): adds tap/hold/swipe
  // support to hand-rolled theme rendering without restructuring the activity. All
  // coordinates are LOGICAL screen coordinates (orientation already applied via
  // GfxRenderer::tapToLogical) -- never touch raw normalized panel coordinates or the
  // SDK InputManager directly; go through HalGPIO/these helpers only. ---

  // True when the device has a touch panel. Rarely needed directly -- the helpers below
  // simply never fire without one.
  bool hasTouch() const;
  // A completed tap (press + release), with logical coords.
  bool wasScreenTapped(int& x, int& y) const;
  // Touch-down (held past a short debounce, not yet released): draw a selection highlight.
  bool wasScreenTouchDown(int& x, int& y) const;
  // Live contact position while the finger is down (drag tracking).
  bool isScreenTouchHeld(int& x, int& y) const;
  // One-off hit test on a rectangle (a single button, a banner).
  bool wasTapInRect(int x, int y, int width, int height) const;
  // Taps on a standard UITheme list; does the row/paging math for you.
  bool wasListItemTapped(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  // Same geometry as wasListItemTapped, touch-down phase (highlight before activate).
  bool wasListItemTouchedDown(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                              bool hasSubtitle) const;
  // rowHeight limits the hit to the top rowHeight px of each step (0 = the full step, no
  // gap band).
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;
  // Raw swipe direction, if a caller needs one beyond the global gestures below.
  SwipeDir wasSwipe() const;
  // Global gestures, handled once for every screen -- do not reimplement these in an
  // activity. Home: up-swipe starting in the bottom 14%, checked by ActivityManager::loop()
  // itself (pops to Home; override via Activity::handleHomeGesture()). Back: right-swipe
  // starting in the left 25%, folded directly into wasPressed(Button::Back)/
  // wasReleased(Button::Back) below, so existing button-era activities gain back-swipe
  // support with zero changes. Menu: down-swipe starting in the top 14%, exposed here for
  // activities that have a menu to check for themselves (the reader does this).
  bool wasHomeGesture() const;
  bool wasMenuGesture() const;
  // rtlSwap: true for a genuinely horizontal previous/next pair (e.g. STR_DIR_LEFT/
  // STR_DIR_RIGHT) that should mirror sides under Arabic, matching NavNext/
  // NavPrevious's own RTL flip. Pass false for a vertical up/down pair (e.g.
  // STR_DIR_UP/STR_DIR_DOWN) paired with Button::ScrollNext/ScrollPrevious --
  // down is always down regardless of script direction, so only the
  // orientation-follow swap should apply, not the RTL one.
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next,
                   bool rtlSwap = true) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the control axis is flipped relative to the physical buttons: the user opted into
  // orientation-following front buttons AND the screen is *currently rendered* rotated (INVERTED /
  // LANDSCAPE_CCW). Keyed on the live renderer orientation rather than the persisted reader setting,
  // so portrait UI (home, settings) never swaps while the reader and its menus do.
  [[nodiscard]] bool isNavDirectionSwapped() const;

 private:
  HalGPIO& gpio;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer& renderer;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;

  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords.
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
  bool wasBackGesture() const;
  bool listItemFromPoint(int x, int y, int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  // A gesture (back/home/menu) consumes the underlying touch-down/release edge without a
  // matching physical-button transition, so getHeldTime() would otherwise report a stale
  // value right after one fires. Latches the touch's held duration for a short window so
  // callers that check getHeldTime() immediately after a gesture-driven action (e.g. a
  // long-press vs short-press branch) still see a sensible number.
  void rememberTouchHeldTime() const;
  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
};
