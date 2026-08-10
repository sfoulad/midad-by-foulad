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
#include "CrossPointSettings.h"
#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "OpdsServerStore.h"
#include "WifiCredentialStore.h"
#include "network/OtaUpdater.h"

namespace BleCommandDispatcher {

namespace {
constexpr char TAG[] = "BLECMD";

// Same std::find_if-on-FOULAD_EBOOKS_URL pattern OpdsBookBrowserActivity/
// ActivityManager/etc. already use throughout this codebase to locate that one
// entry among OPDS_STORE's servers. Shared by deviceIsClaimed() (below) and
// checkAuth() (further down): "is there a claimed account" and "does this
// credential match it" are the same lookup.
auto findFouladEbooksServer() {
  const auto& servers = OPDS_STORE.getServers();
  return std::find_if(servers.begin(), servers.end(), [](const OpdsServer& s) { return s.url == FOULAD_EBOOKS_URL; });
}

// "Claimed" mirrors docs/ble-module-tasks.md's Security section: this device already
// has a saved Foulad eBooks credential. wifi.provision/device.info/account.claim/
// device.challenge only need this "unclaimed" side -- they're for a brand-new reader
// with no account yet; a claimed device has nothing in that phase's scope to
// authorize against, so they're refused outright. Everything else needs the real
// Auth-characteristic check (checkAuth() below), added once account.claim existed to
// authorize against.
bool deviceIsClaimed() { return findFouladEbooksServer() != OPDS_STORE.getServers().end(); }

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

// Headless firmware.update -- see docs/ble-module-tasks.md's spec: "Triggers the
// *existing* OtaUpdater::checkForUpdate() path... so BLE is only ever the trigger,
// never a new update mechanism." Deliberately calls ONLY checkForUpdate() (a release-
// metadata fetch), never installUpdate() -- the actual flash-and-reboot stays behind
// the existing on-device confirmation UI (OtaUpdateActivity), unchanged. This state
// machine only gets the device online long enough for that one metadata check, on
// whatever network it already has saved (WIFI_STORE's last-connected SSID) -- there's
// no credential in firmware.update's payload the way wifi.provision has one.
// Same shape as WifiVerifyState above (grace delay, then connect, then act, then hand
// the radio back) -- see that state machine's comments for the BLE-teardown-race
// rationale, identical here.
enum class FirmwareCheckState : uint8_t { Idle, PendingStart, Connecting, Checking };
FirmwareCheckState g_fwCheckState = FirmwareCheckState::Idle;
// Set from the {"channel":"stable"|"rc"} payload field, if present -- applied to
// SETTINGS.otaPrereleaseEnabled only in-memory for the duration of this one check,
// then restored, never SETTINGS.saveToFile()'d. "Override... for just this check"
// (the doc's own wording) means exactly that: never touching the persisted setting.
bool g_fwCheckHasChannelOverride = false;
bool g_fwCheckForcePrerelease = false;
constexpr unsigned long kFwCheckGraceMs = 3000;  // matches kVerifyGraceMs's own rationale
unsigned long g_fwCheckReadyAtMs = 0;
unsigned long g_fwCheckConnectStartMs = 0;
// Generous connect timeout (matches CONNECTION_TIMEOUT_MS in WifiSelectionActivity,
// not the shorter auto-connect one -- this is the device's own known-good network,
// not an untried saved credential, so it's worth waiting the longer timeout for).
constexpr unsigned long kFwCheckConnectTimeoutMs = 15000;

void finishFirmwareCheck() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_MODE_NULL);
  g_fwCheckState = FirmwareCheckState::Idle;
}

void pumpFirmwareCheck() {
  if (g_fwCheckState == FirmwareCheckState::PendingStart) {
    if (millis() < g_fwCheckReadyAtMs) return;
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      LOG_DBG(TAG, "firmware check: abandoned, radio already in use (mode=%d)", static_cast<int>(WiFi.getMode()));
      g_fwCheckState = FirmwareCheckState::Idle;
      return;
    }
    const std::string& ssid = WIFI_STORE.getLastConnectedSsid();
    const WifiCredential* cred = ssid.empty() ? nullptr : WIFI_STORE.findCredential(ssid);
    if (!cred) {
      LOG_DBG(TAG, "firmware check: no saved network to connect to, abandoning");
      g_fwCheckState = FirmwareCheckState::Idle;
      return;
    }
    // Same synchronous BLE teardown as pumpWifiVerify() above, same reason.
    if (BlePeripheral.isActive()) {
      LOG_DBG(TAG, "firmware check: tearing down BLE before grabbing the radio for WiFi");
      BlePeripheral.end();
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    g_fwCheckConnectStartMs = millis();
    g_fwCheckState = FirmwareCheckState::Connecting;
    return;
  }

  if (g_fwCheckState == FirmwareCheckState::Connecting) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      g_fwCheckState = FirmwareCheckState::Checking;
    } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
               millis() - g_fwCheckConnectStartMs > kFwCheckConnectTimeoutMs) {
      LOG_DBG(TAG, "firmware check: wifi connect failed/timed out");
      finishFirmwareCheck();
    }
    return;
  }

  if (g_fwCheckState != FirmwareCheckState::Checking) return;

  // checkForUpdate() is a single blocking HTTPS fetch (not itself async/polled --
  // matches how OtaUpdateActivity calls it) -- fine to call inline here since WiFi is
  // already confirmed connected; this isn't a repeated poll like the Connecting state
  // above.
  const uint8_t savedPrerelease = SETTINGS.otaPrereleaseEnabled;
  if (g_fwCheckHasChannelOverride) SETTINGS.otaPrereleaseEnabled = g_fwCheckForcePrerelease ? 1 : 0;
  OtaUpdater updater;
  const auto result = updater.checkForUpdate();
  if (g_fwCheckHasChannelOverride) SETTINGS.otaPrereleaseEnabled = savedPrerelease;
  LOG_DBG(TAG, "firmware check: result=%d newer=%d latest=%s", static_cast<int>(result), updater.isUpdateNewer(),
          updater.getLatestVersion().c_str());
  finishFirmwareCheck();
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

// Command authentication -- see docs/ble-module-tasks.md's "Prerequisite: command
// authentication" section. The device already holds its own {username, token}
// (DeviceToken, via OPDS_STORE's Foulad eBooks entry) in plaintext locally -- that's
// what it sends as HTTP Basic Auth today, so no new crypto is needed here, just a
// comparison. A phone writes the same {"username","token"} shape to the Auth
// characteristic; g_authenticated gates every command except the unclaimed-device
// ones (wifi.provision/device.info/account.claim/device.challenge), which keep their
// existing physical-possession rules unchanged.
//
// Session-scoped, not persisted: reset to false whenever the connection isn't in the
// Connected state (pump()'s first line, every tick) so a phone must re-auth on every
// new connection -- no stale auth carried across a disconnect/reconnect.
bool g_authenticated = false;

void checkAuth(const uint8_t* authBuf, size_t authLen) {
  char jsonBuf[BlePeripheralManager::kMaxPayloadLen + 1];
  memcpy(jsonBuf, authBuf, authLen);
  jsonBuf[authLen] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, jsonBuf) != DeserializationError::Ok) {
    LOG_ERR(TAG, "auth write: malformed JSON (%u bytes)", static_cast<unsigned>(authLen));
    return;
  }
  const char* username = doc["username"] | "";
  const char* token = doc["token"] | "";

  const auto& servers = OPDS_STORE.getServers();
  const auto it = findFouladEbooksServer();
  // Password field holds the DeviceToken, not a real account password -- see
  // FouladQrLoginActivity.cpp's Approved case / account.claim's handler, both of
  // which write it there the same way.
  const bool matches = it != servers.end() && it->username == username && it->password == token;
  g_authenticated = matches;
  // Logged length only, not content -- it's a credential either way.
  LOG_DBG(TAG, "auth write: %u bytes -> %s", static_cast<unsigned>(authLen), matches ? "verified" : "rejected");
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
  // Added after foulad-one's real wizard build flagged the gap (2026-08-10, see doc):
  // the wizard skips its own Wi-Fi/claim steps when these say so
  // (DeviceInfo.wifiConnected == true, etc.), already coded null-safe against their
  // absence -- so this was never blocking, just picked up free once added.
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["claimed"] = deviceIsClaimed();

  // CROSSPOINT_VERSION embeds the branch name + short SHA on dev/RC builds
  // (scripts/git_branch.py) and can run long enough to blow the 160-byte BLE
  // payload budget together with serial+model -- measured 158/160 live with a
  // real branch name this session, before wifi_connected/claimed existed to make it
  // tighter still. Truncate just this field rather than dropping the whole reply:
  // the phone only needs enough to identify the base version, not the exact dev tag,
  // in this fallback case.
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

// settings.push -- see docs/ble-module-tasks.md's spec. Reuses whatever shape the
// existing server-driven settings push already uses
// (FouladDeviceTracking::applySettingsFromServer(), FouladDeviceTracking.cpp's
// `applyToggle`, SettingsList.h's SettingInfo::Toggle), applied through the exact
// same code path -- this is a new *source* for values that code already knows how
// to apply, not new apply logic. Requires the Auth check (dispatch() below).
size_t handleSettingsPush(JsonVariantConst payload, char* outBuf, size_t outBufLen) {
  constexpr char kCmd[] = "settings.push";
  const JsonVariantConst settings = payload["settings"];
  if (!settings.is<JsonObjectConst>()) {
    return formatReply(outBuf, outBufLen, kCmd, "failed", "invalid_payload");
  }
  FouladDeviceTracking::applySettingsFromServer(settings.as<JsonObjectConst>());
  return formatReply(outBuf, outBufLen, kCmd, "ok", nullptr);
}

// firmware.update -- see docs/ble-module-tasks.md's spec and pumpFirmwareCheck()'s own
// comment above for the full design. Payload {} or {"channel":"stable"|"rc"}. Reply
// means "trigger accepted," not "update finished" -- see the doc.
size_t handleFirmwareUpdate(JsonVariantConst payload, char* outBuf, size_t outBufLen) {
  constexpr char kCmd[] = "firmware.update";
  const char* channel = payload["channel"] | "";
  if (channel[0] != '\0') {
    if (strcmp(channel, "stable") == 0) {
      g_fwCheckHasChannelOverride = true;
      g_fwCheckForcePrerelease = false;
    } else if (strcmp(channel, "rc") == 0) {
      g_fwCheckHasChannelOverride = true;
      g_fwCheckForcePrerelease = true;
    } else {
      return formatReply(outBuf, outBufLen, kCmd, "failed", "invalid_payload");
    }
  } else {
    g_fwCheckHasChannelOverride = false;
  }
  g_fwCheckReadyAtMs = millis() + kFwCheckGraceMs;
  g_fwCheckState = FirmwareCheckState::PendingStart;
  return formatReply(outBuf, outBufLen, kCmd, "ok", nullptr);
}

size_t dispatch(const char* cmd, JsonVariantConst payload, char* outBuf, size_t outBufLen) {
  // Unclaimed-device commands -- physical-possession security model, no Auth check
  // (there's no credential to check against yet on a device with no account). See
  // docs/ble-module-tasks.md's "Prerequisite: command authentication" section.
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

  // Everything past this point acts on an already-claimed device and requires the
  // real Auth-characteristic check (checkAuth(), set from the Auth write in pump()).
  if (!g_authenticated) {
    return formatReply(outBuf, outBufLen, cmd, "failed", "unauthorized");
  }
  if (strcmp(cmd, "settings.push") == 0) {
    return handleSettingsPush(payload, outBuf, outBufLen);
  }
  if (strcmp(cmd, "firmware.update") == 0) {
    return handleFirmwareUpdate(payload, outBuf, outBufLen);
  }
  // Explicit reply, not silence -- an older reader talking to a newer phone app
  // should fail as "needs a firmware update," not hang. See docs/ble-module-tasks.md's
  // command-protocol section.
  return formatReply(outBuf, outBufLen, cmd, "unsupported", nullptr);
}
}  // namespace

void pump() {
  pumpWifiVerify();
  pumpFirmwareCheck();

  // Session-scoped auth (see checkAuth()'s comment): cleared on every tick the
  // connection isn't Connected, so a phone must write Auth again on every new
  // connection. Checked before takePendingAuth() so a disconnect mid-command-
  // sequence clears it even on the very next connect's first tick, before any new
  // Auth write has had a chance to arrive.
  if (BlePeripheral.state() != BlePeripheralManager::State::Connected) {
    g_authenticated = false;
  }

  uint8_t authBuf[BlePeripheralManager::kMaxPayloadLen];
  size_t authLen = 0;
  if (BlePeripheral.takePendingAuth(authBuf, sizeof(authBuf), authLen)) {
    checkAuth(authBuf, authLen);
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
