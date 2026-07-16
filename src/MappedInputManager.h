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
    ScrollPrevious
  };

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
};
