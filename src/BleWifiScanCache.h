#pragma once

#include <cstddef>
#include <cstdint>

// Cache of the strongest WiFi networks seen the last time BluetoothActivity scanned,
// before switching the radio over to BLE (see docs/ble-module-tasks.md's wifi.scan
// spec). BLE and WiFi can't run at once on this single-radio SoC, so wifi.scan reads
// back this snapshot on request rather than scanning live.
//
// Async, poll-driven -- matches WifiSelectionActivity's own proven scan pattern
// (scanNetworks(true) + poll scanComplete() from loop()). An earlier version used the
// synchronous WiFi.scanNetworks(false) instead, called once from onEnter(): it hung
// the device solid on real hardware (confirmed live 2026-08-10 -- no watchdog reset,
// no serial response, for minutes). WiFiScanClass's sync path waits on an event-group
// bit with a 60s internal timeout (WiFiScanClass::_scanTimeout in the Arduino core),
// which blocks whichever task calls it for however long that takes -- unlike a tight
// busy-loop, this doesn't starve other tasks or trip the watchdog, so nothing forces
// it to give up. Every other WiFi-scan use in this codebase already uses the async
// form for exactly this reason; this module should have matched it from the start.
//
// A second, different intermittent hang showed up even after that fix: WiFi.mode()
// racing the render task's HalPowerManager::Lock (which changes CPU frequency for
// the E-ink refresh) when the scan started too soon after an activity transition.
// Fixed in BluetoothActivity.cpp's onEnter() (requestUpdateAndWait(), not this
// file) -- see that comment for the full story.
namespace BleWifiScanCache {

constexpr size_t kMaxNetworks = 6;

struct Network {
  char ssid[33] = {};
  int8_t rssi = 0;
};

// Non-blocking -- kicks off WiFi.mode(WIFI_STA) + an async scanNetworks(). Call once
// before polling update(). Never call while BLE is active (single shared radio).
void startScan();

// Call every loop() tick after startScan(), until it returns true. Returns true once
// the scan has finished (successfully, failed, or given up after an internal timeout)
// and the cache is ready -- at that point WiFi has already been returned to
// WIFI_MODE_NULL, safe to request BLE. Returns false while still in progress.
bool update();

size_t count();
const Network* networks();

}  // namespace BleWifiScanCache
