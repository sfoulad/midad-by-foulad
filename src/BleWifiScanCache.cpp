#include "BleWifiScanCache.h"

#include <WiFi.h>

#include <algorithm>

#include "Logging.h"
// ActivityManager.h's inline constructor default-constructs a
// std::unique_ptr<Activity> member; Activity.h must be visible here too or
// that unique_ptr's implicit destructor instantiation fails against an
// incomplete type (this TU has no other reason to see Activity's definition).
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

extern ActivityManager activityManager;

namespace BleWifiScanCache {

namespace {
constexpr char TAG[] = "BLEWIFISCAN";
constexpr uint32_t kScanTimeoutMs = 15000;
constexpr size_t kMaxCachedNetworks = 8;

State state = State::Idle;
std::vector<Network> cachedNetworks;
uint32_t scanStartMillis = 0;

void finishScan(State result) {
  WiFi.scanDelete();
  WiFi.mode(WIFI_MODE_NULL);
  state = result;
}
}  // namespace

void requestScan() {
  if (state == State::Idle) {
    state = State::PendingStart;
  }
}

State currentState() { return state; }

const std::vector<Network>& networks() { return cachedNetworks; }

void consume() {
  if (state == State::Done || state == State::Failed) {
    state = State::Idle;
  }
}

void tick() {
  switch (state) {
    case State::Idle:
    case State::Done:
    case State::Failed:
      return;

    case State::PendingStart: {
      // The render task holds a HalPowerManager::Lock (forces full CPU speed)
      // for the whole duration of every e-ink refresh; a WiFi.mode() call
      // issued while it's still ramping down after a refresh reliably hung the
      // device in the past (~50% of attempts -- see
      // docs/history/ble-module-tasks-pre-thin-fork.md's documented fix).
      // requestUpdateAndWait() blocks (bounded, ~5s worst case) until the
      // render task's own notification confirms that Lock has released, the
      // same call WifiSelectionActivity::startWifiScan() already makes before
      // touching WiFi. Safe from here: this tick() runs on the same task as
      // main.cpp's own loop() (confirmed via ActivityManager.cpp), not the
      // render task itself, so the "don't call from the render task" guard
      // ActivityManager::requestUpdateAndWait() asserts on doesn't apply.
      activityManager.requestUpdateAndWait();

      LOG_DBG(TAG, "starting scan");
      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
      delay(100);
      WiFi.scanNetworks(true);  // async -- see docs/ble-recovery-plan.md's
                                 // BLE-R3 entry for why the sync form hung
      scanStartMillis = millis();
      state = State::Scanning;
      return;
    }

    case State::Scanning: {
      if (millis() - scanStartMillis > kScanTimeoutMs) {
        LOG_ERR(TAG, "scan timed out after %u ms", static_cast<unsigned>(kScanTimeoutMs));
        finishScan(State::Failed);
        return;
      }

      const int16_t result = WiFi.scanComplete();
      if (result == WIFI_SCAN_RUNNING) {
        return;
      }
      if (result == WIFI_SCAN_FAILED) {
        LOG_ERR(TAG, "scan failed");
        finishScan(State::Failed);
        return;
      }

      cachedNetworks.clear();
      cachedNetworks.reserve(std::min(static_cast<size_t>(result), kMaxCachedNetworks));
      for (int i = 0; i < result; i++) {
        std::string ssid = WiFi.SSID(i).c_str();
        if (ssid.empty()) continue;  // hidden networks -- nothing to show
        const int32_t rssi = WiFi.RSSI(i);
        const bool encrypted = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        auto it = std::find_if(cachedNetworks.begin(), cachedNetworks.end(),
                                [&ssid](const Network& n) { return n.ssid == ssid; });
        if (it != cachedNetworks.end()) {
          if (rssi > it->rssi) {
            it->rssi = rssi;
            it->encrypted = encrypted;
          }
          continue;
        }
        cachedNetworks.push_back({std::move(ssid), rssi, encrypted});
      }
      std::sort(cachedNetworks.begin(), cachedNetworks.end(),
                [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
      if (cachedNetworks.size() > kMaxCachedNetworks) {
        cachedNetworks.resize(kMaxCachedNetworks);
      }

      LOG_DBG(TAG, "scan complete, %u network(s) cached", static_cast<unsigned>(cachedNetworks.size()));
      finishScan(State::Done);
      return;
    }
  }
}

}  // namespace BleWifiScanCache
