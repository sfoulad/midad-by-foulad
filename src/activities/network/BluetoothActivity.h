#pragma once

#include "activities/Activity.h"

// Observability/pairing screen for Midad BLE (see docs/upstream-sync-architecture.md's
// BLE-R2 section). Deliberately does NOT own BLE's lifetime -- unlike an earlier
// design explored on a since-abandoned branch, this screen never calls anything like
// setUserRequested(); no such API exists. BLE stays governed entirely by the existing
// MIDAD_APP_SETTINGS.bleEnabled setting and main.cpp's bleAllowedNow gate (BLE-R1),
// exactly as it already works from the Apps tile today -- this screen only shows
// what that machinery is currently doing, and lets Confirm flip the same persisted
// setting the old tile did. Opening this screen does not turn Bluetooth on; closing
// it does not turn Bluetooth off.
//
// ActivityManager::prepareForActivityEnter() (BLE-R1) may tear BLE down immediately
// before this activity's onEnter() if it happened to be resident already -- that's
// fine and expected: bleAllowedNow's normal check on the very next main-loop tick
// restarts it (assuming bleEnabled/WiFi/reader conditions still allow), now with
// this screen's own freed memory (see onEnter()) adding to the available headroom.
class BluetoothActivity final : public Activity {
 public:
  explicit BluetoothActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Bluetooth", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "Bluetooth"; }
  // Mirrors CrossPointWebServerActivity/the stale design: don't let the device sleep
  // out from under a pairing attempt the user is actively watching.
  bool preventAutoSleep() override { return true; }

 private:
  // Combined UI state this screen actually cares about -- MIDAD_APP_SETTINGS.bleEnabled
  // (the persisted intent) folded together with BlePeripheralManager::State (the live
  // radio, when enabled) into the five states the design calls for. Computed fresh
  // every loop()/render() call from the two real sources of truth; never cached beyond
  // one tick, so there is nothing here that itself needs invalidating.
  enum class UiState : uint8_t { Disabled, Starting, Advertising, Connected, PausedLowMemory };
  UiState currentUiState() const;

  // Last UiState this activity rendered -- render() redraws whenever it differs, the
  // same reasoning as the stale design's lastRenderedState_ (main.cpp's own global
  // state-transition nudge already calls requestUpdate() on the active activity, but
  // checking here too means this screen repaints correctly even if that nudge is ever
  // missed, and costs nothing when it isn't).
  int lastRenderedUiState_ = -1;
};
