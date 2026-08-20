#include "BleWifiAutoConnectCache.h"

#include <BlePeripheralManager.h>
#include <HalStorage.h>
#include <WiFi.h>

#include <optional>

#include "BleWifiScanCache.h"
#include "Logging.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"

extern ActivityManager activityManager;

namespace BleWifiAutoConnectCache {

namespace {
constexpr char TAG[] = "BLEWIFIAUTO";
// Matches WifiSelectionActivity::AUTO_CONNECTION_TIMEOUT_MS -- same "known-good
// network, fail fast rather than dragging BLE setup out" reasoning applies here.
constexpr uint32_t kConnectTimeoutMs = 7000;

State state = State::Idle;
uint32_t connectStartMillis = 0;

bool lastConnected = false;
std::string lastSsid;
int32_t lastRssi = 0;

// Mirrors handleWifiProvision()'s lazy-load: BLE has no guarantee any activity ever
// loaded WIFI_STORE's in-memory state this boot. Storage.exists() distinguishes "never
// saved any credential" (fine, nothing to load) from "file exists but failed to load"
// (real corruption -- bail rather than acting on stale/empty in-memory state).
bool ensureWifiStoreLoaded() {
  RenderLock lock;
  if (!Storage.exists(WifiCredentialStore::getFilePath())) return true;  // nothing saved yet
  if (!WIFI_STORE.loadFromFile()) {
    LOG_ERR(TAG, "wifi.json exists but failed to load");
    return false;
  }
  return true;
}

std::optional<WifiCredential> pickCredential() {
  const std::string lastConnectedSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastConnectedSsid.empty()) {
    if (auto cred = WIFI_STORE.findCredential(lastConnectedSsid)) {
      return cred;
    }
  }
  return WIFI_STORE.getCredentialAt(0);
}

// Always releases the radio back to BLE on the way out, whether the attempt
// succeeded or failed -- see this file's header comment for why device.info must
// read the cached outcome here rather than live WiFi.status().
void finishConnect(State result) {
  activityManager.requestUpdateAndWait();  // see tick()'s PendingStart case
  if (result != State::Done) {
    WiFi.disconnect(true, true);
  }
  WiFi.mode(WIFI_MODE_NULL);
  state = result;
}
}  // namespace

bool hasSavedCredential() {
  if (!ensureWifiStoreLoaded()) return false;
  return WIFI_STORE.getCredentialCount() > 0;
}

State currentState() { return state; }

void requestConnect() {
  // Mutual exclusion with BleWifiScanCache: both drive the same WiFi radio through
  // independent state machines ticked every main-loop iteration, and neither
  // originally checked the other. Confirmed live: a wifi.scan call landing while this
  // cache is Connecting reaches its PendingStart handler's WiFi.disconnect() call,
  // which aborts the in-flight connection to the saved network -- two consecutive
  // real hardware timeouts traced directly to this race (BLEWIFISCAN "starting scan"
  // logged inside the BLEWIFIAUTO Connecting window). Refusing to start while the
  // other cache is active is a strict startup gate, not a use of the radio itself, so
  // it's safe to check unconditionally.
  if (state != State::Idle) return;
  if (BleWifiScanCache::currentState() != BleWifiScanCache::State::Idle) return;
  state = State::PendingStart;
}

void consume() {
  if (state == State::Done || state == State::Failed) {
    state = State::Idle;
  }
}

bool lastKnownConnected() { return lastConnected; }
const std::string& lastKnownSsid() { return lastSsid; }
int32_t lastKnownRssi() { return lastRssi; }

void tick() {
  switch (state) {
    case State::Idle:
    case State::Done:
    case State::Failed:
      return;

    case State::PendingStart: {
      // Same render-lock and BLE-teardown-ordering rules as
      // BleWifiScanCache::tick()'s PendingStart case -- see that file's comments for
      // the full hardware-crash history both guards were built from. Repeating both
      // here rather than sharing code with the already hardware-validated wifi.scan
      // path, so this new command can't regress it.
      activityManager.requestUpdateAndWait();
      if (BlePeripheral.isActive()) {
        BlePeripheral.end();
      }

      if (!ensureWifiStoreLoaded()) {
        finishConnect(State::Failed);
        return;
      }
      const auto cred = pickCredential();
      if (!cred) {
        LOG_ERR(TAG, "requestConnect() with no saved credential -- caller should have checked hasSavedCredential()");
        finishConnect(State::Failed);
        return;
      }

      LOG_DBG(TAG, "connecting to saved network: %s", cred->ssid.c_str());
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
      WiFi.disconnect(true, true);
      delay(100);
      if (!cred->password.empty()) {
        WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
      } else {
        WiFi.begin(cred->ssid.c_str());
      }
      lastSsid = cred->ssid;
      connectStartMillis = millis();
      state = State::Connecting;
      return;
    }

    case State::Connecting: {
      if (WiFi.status() == WL_CONNECTED) {
        lastConnected = true;
        lastRssi = WiFi.RSSI();
        WIFI_STORE.setLastConnectedSsid(lastSsid);
        LOG_DBG(TAG, "connected: ssid=%s rssi=%d", lastSsid.c_str(), static_cast<int>(lastRssi));
        finishConnect(State::Done);
        return;
      }
      if (millis() - connectStartMillis > kConnectTimeoutMs) {
        LOG_DBG(TAG, "connect timed out after %u ms", static_cast<unsigned>(kConnectTimeoutMs));
        lastConnected = false;
        finishConnect(State::Failed);
        return;
      }
      return;
    }
  }
}

}  // namespace BleWifiAutoConnectCache
