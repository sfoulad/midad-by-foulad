#include "BleWifiScanCache.h"

#include <WiFi.h>

#include <algorithm>
#include <cstring>

#include "Logging.h"

namespace BleWifiScanCache {

namespace {
constexpr char TAG[] = "BLEWIFISCAN";
Network g_networks[kMaxNetworks];
size_t g_count = 0;
bool g_scanning = false;
unsigned long g_scanStartMs = 0;
// Generous margin over how long a real scan takes -- a scan that hasn't finished by
// here is abandoned (empty cache) rather than left to block BLE indefinitely. Well
// under WiFiScanClass's own default max_ms_per_chan * channel-count budget, so a
// scan this slow is already abnormal.
constexpr unsigned long kScanTimeoutMs = 8000;

// Index of the weakest cached entry -- used once the cache is full to decide
// whether a newly-seen network displaces it.
size_t weakestIndex() {
  size_t idx = 0;
  for (size_t i = 1; i < g_count; i++) {
    if (g_networks[i].rssi < g_networks[idx].rssi) idx = i;
  }
  return idx;
}

void finalizeScan(int16_t found) {
  g_count = 0;
  if (found > 0) {
    // Same "strongest signal per SSID" dedup as WifiSelectionActivity, capped to the
    // handful that fit wifi.scan's 160-byte reply budget (BleCommandDispatcher.cpp
    // trims further at serialize time, but no need to cache more than this).
    for (int i = 0; i < found; i++) {
      const String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) continue;  // hidden network, nothing to show
      const int32_t rssi = WiFi.RSSI(i);

      Network* existing = nullptr;
      for (size_t j = 0; j < g_count; j++) {
        if (strncmp(g_networks[j].ssid, ssid.c_str(), sizeof(g_networks[j].ssid)) == 0) {
          existing = &g_networks[j];
          break;
        }
      }
      if (existing) {
        if (rssi > existing->rssi) existing->rssi = static_cast<int8_t>(rssi);
        continue;
      }

      if (g_count < kMaxNetworks) {
        strlcpy(g_networks[g_count].ssid, ssid.c_str(), sizeof(g_networks[g_count].ssid));
        g_networks[g_count].rssi = static_cast<int8_t>(rssi);
        g_count++;
      } else {
        const size_t weakest = weakestIndex();
        if (rssi > g_networks[weakest].rssi) {
          strlcpy(g_networks[weakest].ssid, ssid.c_str(), sizeof(g_networks[weakest].ssid));
          g_networks[weakest].rssi = static_cast<int8_t>(rssi);
        }
      }
    }
    std::sort(g_networks, g_networks + g_count, [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
  }
  LOG_DBG(TAG, "scan cached %u networks (found=%d)", static_cast<unsigned>(g_count), found);

  WiFi.scanDelete();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_MODE_NULL);
  g_scanning = false;
}
}  // namespace

void startScan() {
  g_count = 0;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.scanNetworks(/*async=*/true);
  g_scanStartMs = millis();
  g_scanning = true;
}

bool update() {
  if (!g_scanning) return true;

  const int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    if (millis() - g_scanStartMs > kScanTimeoutMs) {
      LOG_DBG(TAG, "scan timed out after %lums, giving up", kScanTimeoutMs);
      finalizeScan(0);
      return true;
    }
    return false;
  }

  // WIFI_SCAN_FAILED or an actual network count -- either way the scan has finished.
  finalizeScan(result > 0 ? result : 0);
  return true;
}

size_t count() { return g_count; }
const Network* networks() { return g_networks; }

}  // namespace BleWifiScanCache
