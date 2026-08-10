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
  // False until loop()'s first tick kicks off BleWifiScanCache::startScan() -- NOT
  // called from onEnter() itself. A hang reproduced live 2026-08-10, intermittently,
  // specifically when WiFi.mode(WIFI_STA) ran synchronously inside the activity-
  // transition call chain (ActivityManager::loop()'s pendingActivity handling ->
  // onEnter()) -- the identical call reliably works from ordinary loop()-tick timing
  // (matches BleCommandDispatcher's pumpWifiVerify(), proven reliable all session).
  // Root cause not confirmed (a timing race between WiFi driver init and whatever
  // else runs during an activity transition is the leading hypothesis), but
  // deferring the scan to the first loop() tick sidesteps it either way.
  bool scanStarted_ = false;
  // False until BleWifiScanCache::update() reports the wifi.scan cache scan has
  // finished -- loop() polls it every tick and only requests BLE
  // (BlePeripheral.setUserRequested(true)) once it flips true, since WiFi and BLE
  // can't run at once on this single-radio SoC. See BleWifiScanCache.h for why this
  // is async/polled rather than a blocking call.
  bool bleRequested_ = false;
};
