#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Owns the wifi.scan BLE command's async scan lifecycle. See
// docs/ble-recovery-plan.md's BLE-R3 entry for the historical hangs this design
// avoids and the requirements it satisfies.
//
// The actual WiFi.mode()/scanNetworks() calls can only safely happen from
// src/main.cpp's own loop() tick -- never from a BLE command callback -- and
// BLE tears itself down the instant WiFi leaves WIFI_MODE_NULL (main.cpp's
// bleAllowedNow gate: BLE and WiFi STA are already mutually exclusive by
// construction). So a scan necessarily outlives the BLE connection that
// requested it: wifi.scan starts a scan and replies "started", the phone
// reconnects once the scan finishes and WiFi mode returns to NULL, and a
// second wifi.scan call reads back the cached result.
namespace BleWifiScanCache {

struct Network {
  std::string ssid;
  int32_t rssi = 0;
  bool encrypted = false;
};

enum class State { Idle, PendingStart, Scanning, Done, Failed };

// Call once per main-loop tick (src/main.cpp), after BleCommandDispatcher::pump().
void tick();

State currentState();

// Only meaningful when Idle -- starts the scan on the next tick(). A no-op if a
// scan is already pending/in-flight (idempotent against a phone retry), AND a no-op
// if BleWifiAutoConnectCache is mid-connect (see requestScan()'s .cpp comment --
// confirmed live that without this guard, an overlapping wifi.scan call kills an
// in-flight saved-network connection attempt). Check currentState() after calling to
// see whether the request actually took.
void requestScan();

// Valid only right after currentState() == Done.
const std::vector<Network>& networks();

// Resets Done/Failed back to Idle. Call after the dispatcher has read the
// result (or the failure) so the next wifi.scan call starts a fresh scan
// rather than replaying the same cached list forever.
void consume();

}  // namespace BleWifiScanCache
