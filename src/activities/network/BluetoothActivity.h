#pragma once

#include "activities/Activity.h"

// Screen-scoped Midad BLE pairing screen (see docs/upstream-sync-architecture.md's
// BLE-R2 section, correction 2). This screen OWNS BLE's entire lifetime: onEnter()
// requests it, onExit() releases it -- there is no persistent "keep BLE running in
// the background" setting anymore. BLE is off everywhere else in the firmware
// (Home, Settings, File Transfer, etc.) by construction, which is the whole point:
// NimBLE's ~57-58KB resident cost no longer competes with those screens' own heap
// needs. See BlePeripheralManager::setUserRequested()/isUserRequested() and
// main.cpp's bleAllowedNow gate, which now reads that flag instead of a persisted
// setting.
//
// Two entry points reach this same activity (AppsActivity's "Midad BLE" tile, and
// HomeActivity's hold-Confirm-1s shortcut) -- both just navigate here via
// startActivityForResult(); neither touches BLE directly. Only this activity's own
// onEnter()/onExit() do.
//
// ActivityManager::prepareForActivityEnter() (BLE-R1) may tear BLE down immediately
// before this activity's onEnter() runs if it happened to be resident already (it
// won't be, now that nothing else requests it) -- harmless either way: onEnter()
// unconditionally requests it again right after.
class BluetoothActivity final : public Activity {
 public:
  explicit BluetoothActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Bluetooth", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "Bluetooth"; }
  // Mirrors CrossPointWebServerActivity: don't let the device sleep out from under a
  // pairing attempt the user is actively watching.
  bool preventAutoSleep() override { return true; }

 private:
  // BlePeripheralManager::State direct passthrough, minus Off (this screen always
  // requests BLE on entry, so Off only appears for the brief window before begin()
  // completes -- rendered as Starting, same as before the radio has advertised yet).
  enum class UiState : uint8_t { Starting, Advertising, Connected, PausedLowMemory };
  UiState currentUiState() const;

  // Last UiState this activity rendered -- render() redraws whenever it differs, the
  // same reasoning as the stale design's lastRenderedState_ (main.cpp's own global
  // state-transition nudge already calls requestUpdate() on the active activity, but
  // checking here too means this screen repaints correctly even if that nudge is ever
  // missed, and costs nothing when it isn't).
  int lastRenderedUiState_ = -1;
};
