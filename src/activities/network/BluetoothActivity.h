#pragma once

#include "activities/Activity.h"

// Dedicated screen for Midad BLE phone pairing (see docs/ble-module-tasks.md). This
// is the ONLY place BLE is ever requested -- there is no persistent "always on"
// setting; entering this screen sets BlePeripheralManager's userRequested_ flag,
// leaving it (Back, or navigating anywhere else) clears it and the radio tears down
// via main.cpp's existing bleAllowedNow lifecycle. Reached by holding Confirm on
// Home (see HomeActivity's kBleLongPressMs), not a menu item -- the whole point is a
// screen small enough to reliably clear BlePeripheralManager::kHeapGateBytes even
// right after leaving a heavier screen, so onEnter() also aggressively frees every
// reclaimable cache elsewhere in the firmware (reading stats, font glyph cache) that
// this simple a page has no use for, maximizing the odds BLE actually fits.
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
  // Last BlePeripheral::state() this activity rendered -- render() redraws whenever
  // it differs, so a state change reaches the screen even between the periodic
  // repaint nudge main.cpp's own loop() already provides on every transition.
  int lastRenderedState_ = -1;
};
