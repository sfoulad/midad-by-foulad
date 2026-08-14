#pragma once

#include <HalGPIO.h>

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
    ScrollPrevious,
    ScreenLeft,
    ScreenRight,
    ScreenUp,
    ScreenDown
  };
  enum class SwipeDir { None, Left, Right, Up, Down };
  // Which physical screen edge an edge-anchored swipe is measured against.
  enum class ScreenEdge : uint8_t { Left, Top, Bottom };

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
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  // Raw release edge, also true when the contact ended in a swipe or drag-off
  // (which wasScreenTapped never reports). InputSnapshot builders forward it
  // off-target so FreeInkUI routing clears its pressed-element state.
  bool wasScreenTouchReleased() const;
  bool wasTapInRect(int x, int y, int width, int height) const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers above do not
  // cover (custom row heights, option prompts, menus). Down = a held
  // tap-candidate is on a row (update the selection highlight); Tap = a tap
  // released on one (activate). rowHeight limits the hit to the top rowHeight
  // px of each step (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe() const;
  // Back = left-to-right swipe anchored at the left edge. Public so swipe-mode
  // page turns (reader) can exclude it from a plain SwipeDir::Right.
  bool wasBackGesture() const;
  // Home = bottom-edge up-swipe; reader menu = top-edge down-swipe.
  bool wasHomeGesture() const;
  bool wasMenuGesture() const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  const GfxRenderer& getRenderer() const { return renderer; }
  // rtlSwap: true for a genuinely horizontal previous/next pair (e.g. STR_DIR_LEFT/
  // STR_DIR_RIGHT) that should mirror sides under Arabic, matching NavNext/
  // NavPrevious's own RTL flip. Pass false for a vertical up/down pair (e.g.
  // STR_DIR_UP/STR_DIR_DOWN) paired with Button::ScrollNext/ScrollPrevious --
  // down is always down regardless of script direction, so only the
  // orientation-follow swap should apply, not the RTL one.
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next,
                   bool rtlSwap = true) const;
  // Maps four screen-direction labels onto the two physical front-button roles
  // using the same live-orientation transform as ScreenLeft/Right/Up/Down.
  Labels mapDirectionalLabels(const char* back, const char* confirm, const char* left, const char* right,
                              const char* up, const char* down) const;
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

  Button mapScreenDirection(Button button) const;
  Labels mapFrontLabels(const char* back, const char* confirm, const char* left, const char* right) const;
  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  // Edge-anchored swipe classification + the shared decode/held-time
  // bookkeeping; the wrappers below give each edge its board meaning.
  bool wasEdgeSwipe(ScreenEdge edge) const;
  bool wasTopEdgeDownSwipe() const;
  bool wasBottomEdgeUpSwipe() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
  void rememberTouchHeldTime() const;

  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
};
