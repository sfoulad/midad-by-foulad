#pragma once

#include <cstdint>
#include <string>

// Owns the wifi.autoconnect BLE command's async connect lifecycle -- same shape and
// safety rules as BleWifiScanCache (see that header's comment for the full rationale;
// this mirrors it deliberately rather than sharing a base, to avoid touching the
// already hardware-validated wifi.scan path). See HANDOFF FE-P3-RC-BLE-WIFI-COEXIST-001.
//
// WiFi.begin()/WiFi.mode() can only safely happen from src/main.cpp's own loop() tick,
// never from a BLE command callback, and BLE tears itself down the instant WiFi leaves
// WIFI_MODE_NULL (main.cpp's bleAllowedNow gate). So a connect attempt necessarily
// outlives the BLE connection that requested it: wifi.autoconnect replies "started",
// the phone reconnects once WiFi mode returns to NULL (whether the attempt succeeded or
// failed -- this cache always releases the radio back to BLE), and a second
// wifi.autoconnect call (or device.info, via lastKnownConnected()/lastKnownSsid())
// reads back the cached outcome.
namespace BleWifiAutoConnectCache {

enum class State { Idle, PendingStart, Connecting, Done, Failed };

// NoCredential and StoreLoadFailed both mean "nothing to attempt," but for different
// reasons the phone shouldn't see as the same thing: a genuinely unconfigured reader
// vs. a corrupted/unreadable wifi.json. Collapsing them (an earlier version of this
// function just returned bool) made a real storage error look identical to "nothing
// saved yet" over BLE, masking it from the client entirely.
enum class CredentialCheckResult { NoCredential, HasCredential, StoreLoadFailed };

// Whether a saved credential exists to even attempt -- cheap, synchronous, lazy-loads
// WIFI_STORE from SD if not already loaded this boot (RenderLock-guarded, mirrors
// BleCommandDispatcher's own wifi.provision lazy-load pattern). Safe to call anytime.
CredentialCheckResult checkSavedCredential();

// Call once per main-loop tick (src/main.cpp), after BleWifiScanCache::tick().
void tick();

State currentState();

// Only meaningful when Idle -- starts the connect attempt on the next tick(). A no-op
// if an attempt is already pending/in-flight (idempotent against a phone retry), AND
// a no-op if BleWifiScanCache is mid-scan (see requestConnect()'s .cpp comment --
// confirmed live that without this guard, an overlapping wifi.scan call kills an
// in-flight connect attempt via WiFi.disconnect()). Check currentState() after
// calling to see whether the request actually took.
void requestConnect();

// Resets Done/Failed back to Idle, mirroring BleWifiScanCache::consume().
void consume();

// Most recent verified outcome. Valid immediately after a Done/Failed result and
// persists until the next requestConnect() -- device.info reads these directly rather
// than live WiFi.status(), since by the time any BLE caller can ask, WiFi has already
// been torn back down to WIFI_MODE_NULL to let BLE resume. A snapshot of the last
// attempt, not "is WiFi connected right now."
bool lastKnownConnected();
const std::string& lastKnownSsid();
int32_t lastKnownRssi();

}  // namespace BleWifiAutoConnectCache
