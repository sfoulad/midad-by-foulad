#include "BleCommandDispatcher.h"

#include <ArduinoJson.h>
#include <BlePeripheralManager.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "BleWifiAutoConnectCache.h"
#include "BleWifiScanCache.h"
#include "FouladEbooksConfig.h"
#include "OpdsServerStore.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"

namespace BleCommandDispatcher {

namespace {
constexpr char TAG[] = "BLECMD";

// "Claimed" mirrors docs/ble-module-tasks.md's Security section: this device already
// has a saved Foulad eBooks credential (the same std::find_if-on-FOULAD_EBOOKS_URL
// pattern OpdsBookBrowserActivity/ActivityManager/etc. already use throughout this
// codebase to locate that one entry among OPDS_STORE's servers). Phase 1 only needs
// the "unclaimed" side of this check -- wifi.provision is for a brand-new reader with
// no account yet; a claimed device has nothing in this phase's scope to authorize
// against, so it's refused outright rather than half-implementing the Auth-credential
// verification that's actually Phase 2 (settings.push)'s job to build out.
bool deviceIsClaimed() {
  const auto& servers = OPDS_STORE.getServers();
  return std::find_if(servers.begin(), servers.end(), [](const OpdsServer& s) { return s.url == FOULAD_EBOOKS_URL; }) !=
         servers.end();
}

size_t formatReply(char* outBuf, size_t outBufLen, const char* cmd, const char* state, const char* reason) {
  int written;
  if (reason) {
    written = snprintf(outBuf, outBufLen, "{\"cmd\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\"}", cmd, state, reason);
  } else {
    written = snprintf(outBuf, outBufLen, "{\"cmd\":\"%s\",\"state\":\"%s\"}", cmd, state);
  }
  return (written > 0 && static_cast<size_t>(written) < outBufLen) ? static_cast<size_t>(written) : 0;
}

// Hands Wi-Fi credentials to WifiCredentialStore -- see docs/ble-module-tasks.md's
// "Suggested order" item 1. Deliberately does NOT itself drive a connection attempt:
// doing so would tear down BLE (the doc's mutual-exclusion design) before a reply could
// reach the phone, and this codebase's existing auto-connect-saved-networks flow
// (WifiSelectionActivity::tryAutoConnectCredential/tryNextSavedNetworkFromScan) already
// picks up newly-saved credentials the next time Wi-Fi is needed. The reply here means
// "credential saved," not "verified reachable" -- flagged in
// docs/ble-module-tasks.md as an open question (how the phone learns definitive
// connect success once BLE is unreachable during the Wi-Fi-active state).
size_t handleWifiProvision(JsonVariantConst payload, char* outBuf, size_t outBufLen) {
  constexpr char kCmd[] = "wifi.provision";
  if (deviceIsClaimed()) {
    LOG_DBG(TAG, "wifi.provision refused: device already claimed");
    return formatReply(outBuf, outBufLen, kCmd, "failed", "unauthorized");
  }

  const char* ssid = payload["ssid"] | "";
  const char* password = payload["password"] | "";
  if (!ssid || ssid[0] == '\0') {
    return formatReply(outBuf, outBufLen, kCmd, "failed", "invalid_payload");
  }

  // BLE has no guarantee WifiSelectionActivity ever ran this boot to populate
  // WIFI_STORE's in-memory state; without this, addCredential() below would
  // save over wifi.json with only this one credential, discarding every
  // previously-saved network. fromJson() fully replaces in-memory state from
  // disk, so this is safe to call unconditionally. Storage.exists() decides
  // "missing" (fine, first boot) vs. "exists but failed to load" (real
  // corruption -- bail rather than saving over it) the same way
  // MidadAppSettings::loadFromFile() does, since loadFromFile()'s plain bool
  // return collapses both cases into false.
  //
  // Mirrors WifiSelectionActivity::onEnter(): SD and the e-ink display share
  // one SPI bus, so a task doing blocking SD I/O off the render task holds
  // RenderLock for the duration.
  RenderLock lock;
  const bool wifiStoreExists = Storage.exists(WifiCredentialStore::getFilePath());
  if (wifiStoreExists && !WIFI_STORE.loadFromFile()) {
    LOG_ERR(TAG, "wifi.provision: wifi.json exists but failed to load");
    return formatReply(outBuf, outBufLen, kCmd, "failed", "storage_error");
  }

  if (!WIFI_STORE.addCredential(ssid, password)) {
    LOG_ERR(TAG, "wifi.provision: addCredential failed for ssid=%s", ssid);
    return formatReply(outBuf, outBufLen, kCmd, "failed", "storage_error");
  }

  LOG_DBG(TAG, "wifi.provision: saved credential for ssid=%s", ssid);
  return formatReply(outBuf, outBufLen, kCmd, "ok", nullptr);
}

// account.claim/device.challenge -- the BLE account-claim flow from foulad-ebooks'
// docs/BLE_ACCOUNT_CLAIM_PROPOSAL.md, ported from the hardware-validated commit
// 85a23164 (branch docs/ble-phase1-hardware-validated; never merged to develop --
// confirmed hardware-hung on the real Foulad One app's setup flow without it, see
// HANDOFF FE-P3-RC-BLE-PAIRING-001). Like wifi.provision, these run entirely on the
// unclaimed-device path: physical possession of the reader is the security model,
// not a credential check -- there is nothing to check against yet on a device with
// no account. Once claimed, everything past this point needs the real
// Auth-characteristic check (still Phase 2 work, see BleCommandDispatcher::pump()).

size_t handleAccountClaim(JsonVariantConst payload, char* outBuf, size_t outBufLen) {
  constexpr char kCmd[] = "account.claim";
  // Same unclaimed-device gate as wifi.provision -- a device that already has a
  // Foulad eBooks account must not have it silently overwritten by anyone who
  // picks it up and holds Confirm.
  if (deviceIsClaimed()) {
    LOG_DBG(TAG, "account.claim refused: device already claimed");
    return formatReply(outBuf, outBufLen, kCmd, "failed", "unauthorized");
  }

  const char* username = payload["username"] | "";
  const char* token = payload["token"] | "";
  if (!username || username[0] == '\0' || !token || token[0] == '\0') {
    return formatReply(outBuf, outBufLen, kCmd, "failed", "invalid_payload");
  }

  // Same sink the QR flow writes to (FouladQrLoginActivity.cpp's Approved case) --
  // every existing OPDS call keeps working unchanged, no new credential type.
  // RenderLock, same as handleWifiProvision(): SD and the e-ink display share one
  // SPI bus, so a task doing blocking SD I/O (addServer() persists to opds.json) off
  // the render task holds this for the duration.
  RenderLock lock;
  OpdsServer server;
  server.name = FOULAD_EBOOKS_NAME;
  server.url = FOULAD_EBOOKS_URL;
  server.username = username;
  server.password = token;
  server.isDeviceToken = true;
  if (!OPDS_STORE.addServer(server)) {
    // Username only, never here either -- see the success log below for why.
    LOG_ERR(TAG, "account.claim: addServer failed");
    return formatReply(outBuf, outBufLen, kCmd, "failed", "storage_error");
  }

  // Deliberately no restart here (unlike the QR flow's silentRestartToFouladEbooks()):
  // that flow restarts because it's mid-screen-transition into browsing the catalog;
  // this is a BLE command mid-session, and forcing a reboot here would kill the reply
  // notify and any further commands the phone still means to send (e.g. this is
  // commonly followed by wifi.provision in the same session). The device picks the
  // credential up naturally next time it navigates to Foulad eBooks.
  // Outcome only, not the username: pump()'s own policy (see its comment) is no
  // identifier/credential data in the serial/debug log for any command past this
  // dispatcher, and username is an account identifier by that same standard.
  LOG_INF(TAG, "account.claim: ok");
  return formatReply(outBuf, outBufLen, kCmd, "ok", nullptr);
}

size_t handleDeviceChallenge(JsonVariantConst payload, char* outBuf, size_t outBufLen) {
  constexpr char kCmd[] = "device.challenge";
  const char* nonce = payload["nonce"] | "";
  if (!nonce || nonce[0] == '\0') {
    return formatReply(outBuf, outBufLen, kCmd, "failed", "invalid_payload");
  }
  // Pure proof-of-possession -- no crypto, just echo the server-issued nonce back
  // verbatim so the phone can forward it to claim-by-serial. See foulad-ebooks'
  // docs/BLE_ACCOUNT_CLAIM_PROPOSAL.md for why this alone is a sufficient check
  // (the nonce is single-use, 60s TTL, and only reaches this device over an
  // already-connected BLE session with whoever is physically holding it). The
  // real challenge is a fixed 32 chars (DeviceClaimChallenge::issue()), but a
  // malformed/oversized nonce from a non-conforming caller must not silently
  // truncate into invalid JSON the same way device.info's reply once did --
  // measure before serializing rather than assume it fits.
  JsonDocument doc;
  doc["cmd"] = kCmd;
  doc["state"] = "ok";
  doc["echo"] = nonce;
  if (measureJson(doc) > outBufLen) {
    return formatReply(outBuf, outBufLen, kCmd, "failed", "invalid_payload");
  }
  return serializeJson(doc, outBuf, outBufLen);
}

// Read-only device identification -- no side effects, no state mutation, works
// whether the device is claimed or not. Physical possession of the reader (it's
// sitting there, in BluetoothActivity, advertising) is the security model for this
// and every other unclaimed-device BLE command, not a credential check.
size_t handleDeviceInfo(char* outBuf, size_t outBufLen) {
  JsonDocument doc;
  doc["cmd"] = "device.info";
  doc["state"] = "ok";
  // Not FouladDeviceTracking::getSerialNumber() -- it derives from WiFi.macAddress(),
  // which reads the STA netif's MAC and is only valid while that netif exists.
  // esp_efuse_mac_get_default() reads the same base MAC directly from eFuse, with no
  // netif dependency, and on ESP32-C3 the STA MAC equals the base eFuse MAC unmodified
  // -- same value, no WiFi-state dependency to get wrong.
  {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    char serial[18];
    snprintf(serial, sizeof(serial), "XTE-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    doc["serial"] = serial;
  }
  doc["model"] = gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4";
  doc["firmware_version"] = CROSSPOINT_VERSION;
  doc["claimed"] = deviceIsClaimed();

  // HANDOFF FE-P3-RC-BLE-PROTOCOL-COMPAT-001: two representations of the exact same
  // Wi-Fi state, generated from one source (BleWifiAutoConnectCache's cached outcome,
  // NOT live WiFi.status() -- by the time any BLE caller can ask, WiFi has already
  // been torn back down to WIFI_MODE_NULL to let BLE resume advertising, so a live
  // read would almost always show disconnected regardless of what the last attempt
  // actually did). Never includes the password.
  //   - "wifi": {...} -- the preferred nested form for the app version that reads it.
  //   - top-level "wifi_saved"/"wifi_connected"/"wifi_ssid"/"wifi_rssi" -- flat
  //     compatibility fields for the currently-shipped app (foulad-one @ main,
  //     lib/data/ble/midad_ble_client.dart's DeviceInfo.fromReply()), which only
  //     knows these flat keys. Deprecate the flat fields once that app ships a build
  //     reading the nested form instead -- not yet.
  const bool wifiSaved = BleWifiAutoConnectCache::hasSavedCredential();
  const bool wifiConnected = BleWifiAutoConnectCache::lastKnownConnected();
  const std::string& wifiSsid = BleWifiAutoConnectCache::lastKnownSsid();
  const int32_t wifiRssi = BleWifiAutoConnectCache::lastKnownRssi();

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["saved"] = wifiSaved;
  wifi["connected"] = wifiConnected;
  if (wifiConnected) {
    wifi["ssid"] = wifiSsid;
    wifi["rssi"] = wifiRssi;
  }
  doc["wifi_saved"] = wifiSaved;
  doc["wifi_connected"] = wifiConnected;
  if (wifiConnected) {
    doc["wifi_ssid"] = wifiSsid;
    doc["wifi_rssi"] = wifiRssi;
  }

  // Fit into the BLE payload budget (kMaxPayloadLen, 160 bytes). Measured live: even
  // with a short release-style firmware_version, cmd/state/serial/model/claimed plus
  // BOTH full Wi-Fi representations runs to ~250+ bytes -- there is no way to always
  // send everything, so this trims in priority order, least-consumed-today first.
  // serializeJson() below has no bounds awareness of its own: given a doc that
  // doesn't fit, it silently writes a truncated (and therefore unparseable) prefix
  // rather than erroring, which is exactly what shipped here once wifi{} first
  // pushed a real dev-build firmware_version over the edge -- confirmed live: a
  // reply cut off mid-object at "wifi":{"saved":. Order below: drop what nothing
  // shipped reads yet (nested saved/rssi/ssid, flat saved) before touching what the
  // currently-shipped app actually depends on (flat connected/ssid/rssi), and drop
  // nested "connected" -- the one nested field with real near-term value -- only
  // once firmware_version is already fully shrunk and there's truly nothing else
  // left to give up.
  if (measureJson(doc) > outBufLen) wifi.remove("saved");
  if (measureJson(doc) > outBufLen) doc.remove("wifi_saved");
  if (wifiConnected && measureJson(doc) > outBufLen) wifi.remove("rssi");
  if (wifiConnected && measureJson(doc) > outBufLen) wifi.remove("ssid");
  while (measureJson(doc) > outBufLen) {
    std::string fw = doc["firmware_version"].as<std::string>();
    // Must stop at size()==0, not size()<=1: fw.size() is unsigned, and resize(size()-1)
    // on an already-empty string underflows to SIZE_MAX rather than a negative number.
    // A size-1 firmware_version legitimately needs one more shrink to empty -- confirmed
    // live: a real build's version string ("1") left the whole reply exactly 1 byte over
    // budget with nothing else left to trim, and stopping here at size<=1 silently
    // shipped a truncated, unparseable JSON reply instead of fixing the one byte.
    if (fw.empty()) break;  // give up rather than loop forever
    fw.resize(fw.size() - 1);
    doc["firmware_version"] = fw;
  }
  // Below this point, none of the remaining removals are gated on wifiConnected --
  // confirmed live that the disconnected case (wifi:{"connected":false} plus flat
  // wifi_saved/wifi_connected, no ssid/rssi to have dropped yet) is the tighter one,
  // not the connected case: with firmware_version already empty there was still
  // nothing left to trim and the reply truncated ("wifi_connect...). Order: flat
  // rssi/ssid first (only reachable with an unusually long SSID even after
  // firmware_version is empty) -- these are what the shipped app actually depends
  // on, so kept as long as possible; then nested "connected" and finally the whole
  // nested "wifi" object, since flat "wifi_connected" (never touched here) is the
  // one field this whole compat layer exists to guarantee reaches the app.
  if (measureJson(doc) > outBufLen) doc.remove("wifi_rssi");
  if (measureJson(doc) > outBufLen) doc.remove("wifi_ssid");
  if (measureJson(doc) > outBufLen) wifi.remove("connected");
  if (measureJson(doc) > outBufLen) doc.remove("wifi");

  return serializeJson(doc, outBuf, outBufLen);
}

// Async, cross-reconnect -- same shape as handleWifiScan() below (see its comment
// for the full mutual-exclusion rationale this mirrors). A phone call sequence is:
// call 1 (with a saved credential available) kicks off the attempt and gets
// "started" (BLE disconnects shortly after); once WiFi mode returns to NULL BLE
// re-advertises; call 2 (after reconnect) gets "ok"/"failed", or the phone can skip
// straight to a plain device.info call and read wifi.connected there instead --
// either reads the same BleWifiAutoConnectCache outcome.
size_t handleWifiAutoconnect(char* outBuf, size_t outBufLen) {
  constexpr char kCmd[] = "wifi.autoconnect";
  switch (BleWifiAutoConnectCache::currentState()) {
    case BleWifiAutoConnectCache::State::Idle:
      if (!BleWifiAutoConnectCache::hasSavedCredential()) {
        return formatReply(outBuf, outBufLen, kCmd, "skipped", "no_saved_credentials");
      }
      BleWifiAutoConnectCache::requestConnect();
      // requestConnect() silently no-ops (state stays Idle) when BleWifiScanCache is
      // active -- confirmed live this mutual-exclusion guard is real, not
      // theoretical: an earlier version without it let an overlapping wifi.scan call
      // abort an in-flight saved-network connection. Report it honestly rather than
      // claiming "started" for a request that didn't actually take.
      if (BleWifiAutoConnectCache::currentState() == BleWifiAutoConnectCache::State::Idle) {
        return formatReply(outBuf, outBufLen, kCmd, "failed", "busy");
      }
      return formatReply(outBuf, outBufLen, kCmd, "started", nullptr);

    case BleWifiAutoConnectCache::State::PendingStart:
    case BleWifiAutoConnectCache::State::Connecting:
      return formatReply(outBuf, outBufLen, kCmd, "in_progress", nullptr);

    case BleWifiAutoConnectCache::State::Done: {
      JsonDocument doc;
      doc["cmd"] = kCmd;
      doc["state"] = "ok";
      doc["ssid"] = BleWifiAutoConnectCache::lastKnownSsid();
      doc["rssi"] = BleWifiAutoConnectCache::lastKnownRssi();
      BleWifiAutoConnectCache::consume();
      return serializeJson(doc, outBuf, outBufLen);
    }

    case BleWifiAutoConnectCache::State::Failed:
      BleWifiAutoConnectCache::consume();
      return formatReply(outBuf, outBufLen, kCmd, "failed", nullptr);
  }
  return 0;
}

// Async, cross-reconnect: the actual scan runs entirely in BleWifiScanCache::tick()
// (src/main.cpp's loop, never here) because starting it changes WiFi mode away from
// WIFI_MODE_NULL, which drops BLE via main.cpp's existing bleAllowedNow mutual-
// exclusion gate before a scan could ever finish on this same connection. See
// BleWifiScanCache.h for why. A phone call sequence is: call 1 kicks off the scan and
// gets "started" (BLE disconnects shortly after); once WiFi mode returns to NULL BLE
// re-advertises; call 2 (after reconnect) gets "ok" with the cached results, or
// "failed" if the scan didn't find anything usable -- either way the cache resets to
// Idle so a further call starts a fresh scan rather than replaying the same list.
size_t handleWifiScan(char* outBuf, size_t outBufLen) {
  constexpr char kCmd[] = "wifi.scan";
  switch (BleWifiScanCache::currentState()) {
    case BleWifiScanCache::State::Idle:
      BleWifiScanCache::requestScan();
      // Same honesty check as handleWifiAutoconnect(): requestScan() silently no-ops
      // when BleWifiAutoConnectCache is active.
      if (BleWifiScanCache::currentState() == BleWifiScanCache::State::Idle) {
        return formatReply(outBuf, outBufLen, kCmd, "failed", "busy");
      }
      return formatReply(outBuf, outBufLen, kCmd, "started", nullptr);

    case BleWifiScanCache::State::PendingStart:
    case BleWifiScanCache::State::Scanning:
      return formatReply(outBuf, outBufLen, kCmd, "in_progress", nullptr);

    case BleWifiScanCache::State::Failed:
      BleWifiScanCache::consume();
      return formatReply(outBuf, outBufLen, kCmd, "failed", nullptr);

    case BleWifiScanCache::State::Done: {
      JsonDocument doc;
      doc["cmd"] = kCmd;
      doc["state"] = "ok";
      JsonArray arr = doc["networks"].to<JsonArray>();
      for (const auto& n : BleWifiScanCache::networks()) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = n.ssid;
        obj["rssi"] = n.rssi;
        obj["sec"] = n.encrypted;
      }
      // kMaxPayloadLen (160 bytes) can't fit every scanned network with room for
      // 32-byte SSIDs -- drop from the weakest-signal end (the array is already
      // sorted strongest-first) until it fits, same shrink-to-fit approach
      // handleDeviceInfo() uses for its version string.
      while (measureJson(doc) > outBufLen && arr.size() > 0) {
        arr.remove(arr.size() - 1);
      }
      BleWifiScanCache::consume();
      return serializeJson(doc, outBuf, outBufLen);
    }
  }
  return 0;
}

size_t dispatch(const char* cmd, JsonVariantConst payload, char* outBuf, size_t outBufLen) {
  if (strcmp(cmd, "wifi.provision") == 0) {
    return handleWifiProvision(payload, outBuf, outBufLen);
  }
  if (strcmp(cmd, "device.info") == 0) {
    return handleDeviceInfo(outBuf, outBufLen);
  }
  if (strcmp(cmd, "account.claim") == 0) {
    return handleAccountClaim(payload, outBuf, outBufLen);
  }
  if (strcmp(cmd, "device.challenge") == 0) {
    return handleDeviceChallenge(payload, outBuf, outBufLen);
  }
  if (strcmp(cmd, "wifi.scan") == 0) {
    return handleWifiScan(outBuf, outBufLen);
  }
  if (strcmp(cmd, "wifi.autoconnect") == 0) {
    return handleWifiAutoconnect(outBuf, outBufLen);
  }
  // Explicit reply, not silence -- an older reader talking to a newer phone app
  // should fail as "needs a firmware update," not hang. See docs/ble-module-tasks.md's
  // command-protocol section.
  return formatReply(outBuf, outBufLen, cmd, "unsupported", nullptr);
}
}  // namespace

void pump() {
  uint8_t authBuf[BlePeripheralManager::kMaxPayloadLen];
  size_t authLen = 0;
  if (BlePeripheral.takePendingAuth(authBuf, sizeof(authBuf), authLen)) {
    // Stored/verified starting in Phase 2 (settings.push, the doc's own "proves the
    // account-token check end to end" phase) -- Phase 1's wifi.provision runs entirely
    // on the unclaimed-device path and doesn't consult this. Logged length only, not
    // content: it's a credential.
    LOG_DBG(TAG, "auth write received, %u bytes (not yet verified -- Phase 2 work)", static_cast<unsigned>(authLen));
  }

  uint8_t cmdBuf[BlePeripheralManager::kMaxPayloadLen];
  size_t cmdLen = 0;
  if (!BlePeripheral.takePendingCommand(cmdBuf, sizeof(cmdBuf), cmdLen)) return;
  if (cmdLen == 0) return;

  // Null-terminate into a stack buffer for ArduinoJson -- the pending-command buffer
  // itself is exactly kMaxPayloadLen with no room reserved for a terminator.
  char jsonBuf[BlePeripheralManager::kMaxPayloadLen + 1];
  memcpy(jsonBuf, cmdBuf, cmdLen);
  jsonBuf[cmdLen] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, jsonBuf) != DeserializationError::Ok) {
    LOG_ERR(TAG, "command write: malformed JSON (%u bytes)", static_cast<unsigned>(cmdLen));
    return;  // no reply -- nothing we can even echo a `cmd` name back for
  }

  const char* cmd = doc["cmd"] | "";
  char replyBuf[BlePeripheralManager::kMaxPayloadLen];
  const size_t replyLen = dispatch(cmd, doc["payload"], replyBuf, sizeof(replyBuf));
  if (replyLen > 0) {
    // Log delivery, not just intent: notify() returns false when no client has
    // subscribed to the Command characteristic's CCCD -- diagnostics-only, this
    // doesn't retry or change protocol behavior, just distinguishes "device never
    // replied" from "client never listened" for whoever reads the serial/debug log.
    // Logs the command name only, never the reply body: this dispatcher is the
    // shared path for every future command (settings, firmware, book, sync), and a
    // later reply may carry identifiers/tokens/account data that don't belong in a
    // serial or debug log.
    const bool delivered = BlePeripheral.sendCommandReply(reinterpret_cast<const uint8_t*>(replyBuf), replyLen);
    LOG_DBG(TAG, "reply for cmd=%s: %s", cmd, delivered ? "notified" : "NOT delivered (no subscriber?)");
  }
}

}  // namespace BleCommandDispatcher
