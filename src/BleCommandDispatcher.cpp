#include "BleCommandDispatcher.h"

#include <ArduinoJson.h>
#include <BlePeripheralManager.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "FouladEbooksConfig.h"
#include "OpdsServerStore.h"
#include "WifiCredentialStore.h"

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

  if (!WIFI_STORE.addCredential(ssid, password)) {
    LOG_ERR(TAG, "wifi.provision: addCredential failed for ssid=%s", ssid);
    return formatReply(outBuf, outBufLen, kCmd, "failed", "storage_error");
  }

  LOG_DBG(TAG, "wifi.provision: saved credential for ssid=%s", ssid);
  return formatReply(outBuf, outBufLen, kCmd, "ok", nullptr);
}

size_t dispatch(const char* cmd, JsonVariantConst payload, char* outBuf, size_t outBufLen) {
  if (strcmp(cmd, "wifi.provision") == 0) {
    return handleWifiProvision(payload, outBuf, outBufLen);
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
    // subscribed to the Command characteristic's CCCD -- exactly the phone-side bug
    // seen live 2026-08-10 (app wrote the command without subscribing first, its
    // "Something went wrong" was a reply it never arranged to receive). This line is
    // what distinguishes "device never replied" from "app never listened".
    const bool delivered = BlePeripheral.sendCommandReply(reinterpret_cast<const uint8_t*>(replyBuf), replyLen);
    LOG_DBG(TAG, "reply %s: %.*s", delivered ? "notified" : "NOT delivered (no subscriber?)",
            static_cast<int>(replyLen), replyBuf);
  }
}

}  // namespace BleCommandDispatcher
