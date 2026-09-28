#pragma once

#include <cstdint>

// Pure state -> actions contract for every screen the firmware-update surfaces
// can show: OtaUpdateActivity's states plus the Library update offer
// (OpdsBookBrowserActivity's UPDATE_PROMPT). The activities consult this table
// and the host tests (test/firmware_update_policy) assert on it, so a screen
// can never again ship with no operable control -- the X4 Pro rendered the
// Library offer with button hints alone, which BaseTheme::drawButtonHints
// suppresses entirely on touch hardware, leaving no Update or Cancel control
// at all.
namespace ota_screen {

enum class Screen : uint8_t {
  WIFI_SELECTION,  // WifiSelectionActivity owns the screen
  CHECKING,        // release-check GET in flight
  CONFIRMING,      // version info + Cancel/Update popup
  INSTALLING,      // firmware streaming to the OTA partition
  NO_UPDATE,       // nothing newer / no asset for this board
  FAILED,          // check or install failed, diagnostics on screen
  FINISHED,        // install done, about to restart
  SHUTTING_DOWN,   // restart imminent
  LIBRARY_OFFER,   // catalog-server offer over the Library catalog
};
inline constexpr uint8_t SCREEN_COUNT = 9;

struct Actions {
  bool delegated;     // a sub-activity owns rendering and input
  bool confirmPopup;  // Cancel/Update OptionPopup is the control surface (touch-tappable and button-navigable)
  bool acceptBack;    // physical Back exits the screen
  bool acceptTap;     // any screen tap exits -- the touch counterpart of acceptBack
  bool busy;          // in-flight or terminal: no user action expected, the screen advances on its own
};

constexpr Actions actionsFor(const Screen screen) {
  switch (screen) {
    case Screen::WIFI_SELECTION:
      return {true, false, false, false, false};
    case Screen::CHECKING:
      return {false, false, false, false, true};
    case Screen::CONFIRMING:
      return {false, true, true, false, false};
    case Screen::INSTALLING:
      return {false, false, false, false, true};
    case Screen::NO_UPDATE:
      return {false, false, true, true, false};
    case Screen::FAILED:
      return {false, false, true, true, false};
    case Screen::FINISHED:
      return {false, false, false, false, true};
    case Screen::SHUTTING_DOWN:
      return {false, false, false, false, true};
    case Screen::LIBRARY_OFFER:
      return {false, true, true, false, false};
  }
  return {false, false, false, false, false};
}

// Operable with physical buttons (or no input expected at all).
constexpr bool operable(const Actions& a) {
  return a.delegated || a.confirmPopup || a.acceptBack || a.acceptTap || a.busy;
}

// Operable on touch-only hardware: button hints don't render there
// (BaseTheme::drawButtonHints), so a bare acceptBack is not enough -- the
// screen needs a popup, a tap target, or no expected input.
constexpr bool touchOperable(const Actions& a) { return a.delegated || a.confirmPopup || a.acceptTap || a.busy; }

}  // namespace ota_screen
