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
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["claimed"] = deviceIsClaimed();

  // CROSSPOINT_VERSION embeds the branch name + short SHA on dev/RC builds
  // (scripts/git_branch.py) and can run long enough to blow the BLE payload budget
  // together with the other fields. Truncate just this field rather than dropping
  // the whole reply -- the phone only needs enough to identify the base version, not
  // the exact dev tag, in this fallback case.
  while (measureJson(doc) > outBufLen) {
    std::string fw = doc["firmware_version"].as<std::string>();
    if (fw.size() <= 1) break;  // give up rather than loop forever
    fw.resize(fw.size() - 1);
    doc["firmware_version"] = fw;
  }

  return serializeJson(doc, outBuf, outBufLen);
}

size_t dispatch(const char* cmd, JsonVariantConst payload, char* outBuf, size_t outBufLen) {
  if (strcmp(cmd, "wifi.provision") == 0) {
    return handleWifiProvision(payload, outBuf, outBufLen);
  }
  if (strcmp(cmd, "device.info") == 0) {
    return handleDeviceInfo(outBuf, outBufLen);
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
