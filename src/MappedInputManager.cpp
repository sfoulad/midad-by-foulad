#include "MappedInputManager.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"

bool MappedInputManager::isNavDirectionSwapped() const {
  // Key the swap on the orientation the screen is *actually* rendered at, not the persisted reader
  // setting. The reader (and its modal menus) render rotated, so navigation/labels flip there; the
  // home and settings UI render in portrait, so they never flip even when a rotated reader is configured.
  const auto orientation = renderer.getOrientation();
  return SETTINGS.frontButtonFollowOrientation &&
         (orientation == GfxRenderer::PortraitInverted || orientation == GfxRenderer::LandscapeCounterClockwise);
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::NavNext: {
      // Logical "next item" navigation: side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW (frontButtonFollowOrientation) so it matches the rotated hint labels.
      // In an RTL UI language only the HORIZONTAL half swaps (Left advances, Right goes back):
      // mirrored layouts flow right-to-left, while vertical flow is unchanged.
      const bool swapped = isNavDirectionSwapped();
      const Button vertical = swapped ? Button::Up : Button::Down;
      Button horizontal = swapped ? Button::Left : Button::Right;
      if (I18N.isRtl()) horizontal = horizontal == Button::Right ? Button::Left : Button::Right;
      return mapButton(vertical, fn) || mapButton(horizontal, fn);
    }
    case Button::NavPrevious: {
      // Logical "previous item" navigation: side Up + front Left, axis-flipped the same ways.
      const bool swapped = isNavDirectionSwapped();
      const Button vertical = swapped ? Button::Down : Button::Up;
      Button horizontal = swapped ? Button::Right : Button::Left;
      if (I18N.isRtl()) horizontal = horizontal == Button::Right ? Button::Left : Button::Right;
      return mapButton(vertical, fn) || mapButton(horizontal, fn);
    }
    case Button::ScrollNext: {
      // Same as NavNext (side Down + front Right, orientation-flipped) but deliberately
      // WITHOUT the RTL horizontal flip: this is a purely vertical "scroll the list down"
      // concept (front buttons doubling as shortcuts for the side Up/Down buttons, hinted
      // with literal "Up"/"Down" labels), not a generic left/right "next item" -- down is
      // still down regardless of Arabic's horizontal reading direction. Mirroring the front
      // button here would just make the "Down" hint label lie about which physical button
      // actually scrolls down.
      const bool swapped = isNavDirectionSwapped();
      const Button vertical = swapped ? Button::Up : Button::Down;
      const Button horizontal = swapped ? Button::Left : Button::Right;
      return mapButton(vertical, fn) || mapButton(horizontal, fn);
    }
    case Button::ScrollPrevious: {
      // See ScrollNext -- same as NavPrevious, minus the RTL flip.
      const bool swapped = isNavDirectionSwapped();
      const Button vertical = swapped ? Button::Down : Button::Up;
      const Button horizontal = swapped ? Button::Right : Button::Left;
      return mapButton(vertical, fn) || mapButton(horizontal, fn);
    }
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next, const bool rtlSwap) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  // RTL adds one more swap for a genuinely horizontal previous/next pair: NavNext/NavPrevious
  // flip the horizontal buttons for Arabic (see their handlers above), so the front Left/Right
  // hint labels must flip with them -- otherwise the button that now navigates the RTL
  // "forward" direction still wears the "wrong side" label (reported on-device as "أسفل
  // going up" back when this was unconditionally applied to an Up/Down pair too). Callers
  // pairing a vertical Up/Down label with Button::ScrollNext/ScrollPrevious pass
  // rtlSwap=false, since down is still down regardless of script direction.
  const bool swapLabels = isNavDirectionSwapped() != (rtlSwap && I18N.isRtl());
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return leftLabel;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return rightLabel;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
