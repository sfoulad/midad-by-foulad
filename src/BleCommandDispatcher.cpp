#include "BleCommandDispatcher.h"

#include <ArduinoJson.h>
#include <BlePeripheralManager.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "BleWifiScanCache.h"
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

// Headless post-provisioning connect-and-verify -- see docs/ble-module-tasks.md's
// resolved "Wi-Fi connect feedback" question: wifi.provision only SAVES a credential
// (handleWifiProvision() below never itself connects, since that would tear BLE down
// -- the doc's mutual-exclusion design -- before the "ok" reply could reach the
// phone). This is the deferred half: once the reply has had time to actually transmit,
// try the just-saved network for real, record ok/failed on BlePeripheralManager (which
// Status reports as wifi_last_attempt), and hand the radio back to WIFI_MODE_NULL
// either way so BLE's own bleAllowedNow gate (main.cpp) naturally resumes advertising.
// Not reusing WifiSelectionActivity::tryAutoConnectCredential() -- that's tightly
// coupled to its own screen's UI state (paints "Connecting to X" itself, tracks
// several member fields); this needs to run with no screen at all, from whatever
// activity happens to be showing when the timer fires.
enum class WifiVerifyState : uint8_t { Idle, PendingStart, Connecting };
WifiVerifyState g_verifyState = WifiVerifyState::Idle;
std::string g_verifySsid;
std::string g_verifyPassword;
// Grace delay before WiFi.begin() -- gives the BLE Command-characteristic notify()
// time to actually reach the phone before the radio switches away from it. NimBLE
// queues the notify near-instantly, but the real over-the-air delivery + the phone's
// own processing aren't instant either; 3s is generous against everything measured
// live this session (round-trips consistently well under 1s).
constexpr unsigned long kVerifyGraceMs = 3000;
unsigned long g_verifyReadyAtMs = 0;
unsigned long g_verifyStartMs = 0;
// Matches WifiSelectionActivity::AUTO_CONNECTION_TIMEOUT_MS -- this IS an
// auto-connect attempt, just one with no screen watching it.
constexpr unsigned long kVerifyTimeoutMs = 7000;

void finishWifiVerify(bool ok) {
  BlePeripheral.setWifiLastAttemptResult(ok);
  if (ok) WIFI_STORE.setLastConnectedSsid(g_verifySsid);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_MODE_NULL);
  g_verifyState = WifiVerifyState::Idle;
  LOG_DBG(TAG, "wifi verify: ssid=%s -> %s", g_verifySsid.c_str(), ok ? "ok" : "failed");
}

void pumpWifiVerify() {
  if (g_verifyState == WifiVerifyState::PendingStart) {
    if (millis() < g_verifyReadyAtMs) return;
    // Something else already grabbed the radio in the meantime (e.g. the user
    // navigated to a screen that needs WiFi on its own before this timer fired) --
    // abandon rather than fight over it. Leaves wifi_last_attempt as it was (still
    // "null" if this was the first-ever attempt) since a connection genuinely
    // wasn't tried this time, not "failed".
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      LOG_DBG(TAG, "wifi verify: abandoned, radio already in use (mode=%d)", static_cast<int>(WiFi.getMode()));
      g_verifyState = WifiVerifyState::Idle;
      return;
    }
    // Tear BLE down synchronously here rather than trusting main.cpp's bleAllowedNow
    // gate to notice next tick -- that check runs BEFORE this pump() in loop(), so on
    // this exact iteration it still sees the old WIFI_MODE_NULL and leaves BLE up.
    // WiFi.mode(WIFI_STA) below would then grab the single shared 2.4GHz radio while
    // NimBLE is still mid-advertise/teardown, which hangs the radio driver long enough
    // to trip the task watchdog -- confirmed live 2026-08-10 (crash_report.txt: empty
    // panic reason, meaning a WDT reset, not a real abort -- see
    // HalSystem::isRebootFromPanic()). Same root cause as the sleep-entry BLE backstop
    // above in main.cpp (CrumBLE issue #44): the gate reacting one loop late isn't
    // fast enough when something is about to touch the radio in this same tick.
    if (BlePeripheral.isActive()) {
      LOG_DBG(TAG, "wifi verify: tearing down BLE before grabbing the radio for WiFi");
      BlePeripheral.end();
    }
    WiFi.mode(WIFI_STA);
    if (!g_verifyPassword.empty()) {
      WiFi.begin(g_verifySsid.c_str(), g_verifyPassword.c_str());
    } else {
      WiFi.begin(g_verifySsid.c_str());
    }
    g_verifyStartMs = millis();
    g_verifyState = WifiVerifyState::Connecting;
    return;
  }

  if (g_verifyState != WifiVerifyState::Connecting) return;

  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    finishWifiVerify(true);
  } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
             millis() - g_verifyStartMs > kVerifyTimeoutMs) {
    finishWifiVerify(false);
  }
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
// "Suggested order" item 1. Deliberately does NOT itself drive a connection attempt
// (doing so would tear down BLE -- the doc's mutual-exclusion design -- before this
// reply could reach the phone): it schedules pumpWifiVerify() to try it a few seconds
// later instead. The reply here means "credential saved, verifying" -- the phone
// polls Status's wifi_last_attempt (formatStatusJson(), BlePeripheralManager.cpp)
// after reconnecting to learn whether it actually worked, per the doc's resolved
// "Wi-Fi connect feedback" question.
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
  g_verifySsid = ssid;
  g_verifyPassword = password;
  g_verifyReadyAtMs = millis() + kVerifyGraceMs;
  g_verifyState = WifiVerifyState::PendingStart;
  return formatReply(outBuf, outBufLen, kCmd, "ok", nullptr);
}

// device.info/account.claim/device.challenge -- the BLE account-claim flow from
// docs/ble-module-tasks.md's "New phase, scoped after real use" section, contract
// pinned in foulad-ebooks' docs/BLE_ACCOUNT_CLAIM_PROPOSAL.md. Like wifi.provision,
// these run entirely on the unclaimed-device path: physical possession of the reader
// (it's sitting there, in BluetoothActivity, advertising) is the security model, not
// a credential check -- there is nothing to check against yet on a device with no
// account. Once claimed, everything past this point needs the real Auth-characteristic
// check this doc's "Prerequisite: command authentication" section specs (not yet
// built -- deliberately out of scope here).

size_t handleDeviceInfo(char* outBuf, size_t outBufLen) {
  JsonDocument doc;
  doc["cmd"] = "device.info";
  doc["state"] = "ok";
  // Deliberately NOT FouladDeviceTracking::getSerialNumber() -- it derives from
  // WiFi.macAddress(), which reads the STA netif's MAC via esp_netif_get_mac() and
  // is only valid while that netif actually exists. It doesn't just start invalid
  // (the bug the advertised-BLE-name fix hit) -- WiFi.mode(WIFI_MODE_NULL) tears the
  // STA netif down again, which is exactly what BleWifiScanCache::finalizeScan()
  // does once its scan completes, right before BLE starts advertising. Confirmed
  // live 2026-08-10: device.info returned serial "XTE-000000000000" over a real BLE
  // connection. esp_efuse_mac_get_default() reads the same base MAC directly from
  // eFuse, with no netif dependency -- and on ESP32-C3 the STA MAC equals the base
  // eFuse MAC unmodified (offset 0 in Espressif's per-interface MAC scheme), so this
  // produces the exact same "XTE-<mac>" value FouladDeviceTracking::getSerialNumber()
  // would when WiFi is genuinely online -- not a different identity, just a reliable
  // way to compute the same one.
  {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    char serial[18];
    snprintf(serial, sizeof(serial), "XTE-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    doc["serial"] = serial;
  }
  doc["model"] = gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4";
  doc["firmware_version"] = CROSSPOINT_VERSION;

  // CROSSPOINT_VERSION embeds the branch name + short SHA on dev/RC builds
  // (scripts/git_branch.py) and can run long enough to blow the 160-byte BLE
  // payload budget together with serial+model -- measured 158/160 live with a
  // real branch name this session. Truncate just this field rather than dropping
  // the whole reply: the phone only needs enough to identify the base version,
  // not the exact dev tag, in this fallback case.
  while (measureJson(doc) > outBufLen) {
    std::string fw = doc["firmware_version"].as<std::string>();
    if (fw.size() <= 1) break;  // give up rather than loop forever
    fw.resize(fw.size() - 1);
    doc["firmware_version"] = fw;
  }

  return serializeJson(doc, outBuf, outBufLen);
}

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
  OpdsServer server;
  server.name = FOULAD_EBOOKS_NAME;
  server.url = FOULAD_EBOOKS_URL;
  server.username = username;
  server.password = token;
  server.isDeviceToken = true;
  if (!OPDS_STORE.addServer(server)) {
    LOG_ERR(TAG, "account.claim: addServer failed for username=%s", username);
    return formatReply(outBuf, outBufLen, kCmd, "failed", "storage_error");
  }

  // Deliberately no restart here (unlike the QR flow's silentRestartToFouladEbooks()):
  // that flow restarts because it's mid-screen-transition into browsing the catalog;
  // this is a BLE command mid-session, and forcing a reboot here would kill the reply
  // notify and any further commands the phone still means to send (e.g. this is
  // commonly followed by wifi.provision in the same session). The device picks the
  // credential up naturally next time it navigates to Foulad eBooks.
  LOG_INF(TAG, "account.claim: signed in as '%s'", username);
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
  // already-connected BLE session with whoever is physically holding it).
  JsonDocument doc;
  doc["cmd"] = kCmd;
  doc["state"] = "ok";
  doc["echo"] = nonce;
  return serializeJson(doc, outBuf, outBufLen);
}

size_t handleWifiScan(char* outBuf, size_t outBufLen) {
  JsonDocument doc;
  doc["cmd"] = "wifi.scan";
  doc["state"] = "ok";
  JsonArray networks = doc["networks"].to<JsonArray>();

  const size_t available = BleWifiScanCache::count();
  const BleWifiScanCache::Network* cached = BleWifiScanCache::networks();
  for (size_t i = 0; i < available; i++) {
    JsonObject entry = networks.add<JsonObject>();
    entry["ssid"] = cached[i].ssid;
    entry["rssi"] = cached[i].rssi;
    // Stop as soon as adding one more would exceed the characteristic's payload
    // budget, rather than guessing a safe network count up front -- SSID length
    // varies too much for a fixed cap to be both safe and not wasteful. cached[]
    // is already strongest-first (BleWifiScanCache::scanAndCache()), so this keeps
    // the strongest networks that fit, dropping only the weakest tail.
    if (measureJson(doc) > outBufLen) {
      networks.remove(networks.size() - 1);
      break;
    }
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
  if (strcmp(cmd, "account.claim") == 0) {
    return handleAccountClaim(payload, outBuf, outBufLen);
  }
  if (strcmp(cmd, "device.challenge") == 0) {
    return handleDeviceChallenge(payload, outBuf, outBufLen);
  }
  if (strcmp(cmd, "wifi.scan") == 0) {
    return handleWifiScan(outBuf, outBufLen);
  }
  // Explicit reply, not silence -- an older reader talking to a newer phone app
  // should fail as "needs a firmware update," not hang. See docs/ble-module-tasks.md's
  // command-protocol section.
  return formatReply(outBuf, outBufLen, cmd, "unsupported", nullptr);
}
}  // namespace

void pump() {
  pumpWifiVerify();

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
