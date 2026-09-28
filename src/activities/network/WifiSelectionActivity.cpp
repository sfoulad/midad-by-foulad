#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HeapTrace.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/WifiDiagLog.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SCAN = 2;
constexpr fui::ActionId ACTION_PROMPT = 3;
}  // namespace

WifiSelectionActivity::WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const bool autoConnect)
    : Activity("WifiSelection", renderer, mappedInput), UiAppHost(renderer), allowAutoConnect(autoConnect) {}

void WifiSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<WifiSelectionActivity*>(user);
  if (self->state != WifiSelectionState::NETWORK_LIST) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->networks.size())) return;
  self->selectedNetworkIndex = static_cast<size_t>(event.value);
  // Long-press a saved network to forget it (mirrors the Left-button hold in loop()).
  if (event.longPress) {
    if (self->networks[self->selectedNetworkIndex].hasSavedPassword) {
      self->selectedSSID = self->networks[self->selectedNetworkIndex].ssid;
      self->state = WifiSelectionState::FORGET_PROMPT;
      self->forgetPromptSelection = 0;  // Default to "Cancel"
      self->app.clearTapFlash();
      self->requestUpdate();
    }
    return;
  }
  // Selection leaves this screen (password entry / connecting); a lingering
  // flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->selectNetwork(static_cast<int>(self->selectedNetworkIndex));
}

void WifiSelectionActivity::onScanEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<WifiSelectionActivity*>(user);
  if (self->state != WifiSelectionState::NETWORK_LIST) return;
  self->app.clearTapFlash();  // the scan screen replaces this one
  self->startWifiScan();
}

void WifiSelectionActivity::onPromptEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<WifiSelectionActivity*>(user);
  if (self->state == WifiSelectionState::SAVE_PROMPT) {
    self->savePromptSelection = event.value;
    self->app.clearTapFlash();  // the action leaves this screen
    if (self->savePromptSelection == 0) {
      RenderLock lock(*self);
      WIFI_STORE.addCredential(self->selectedSSID, self->enteredPassword);
    }
    self->onComplete(true);
    return;
  }
  if (self->state == WifiSelectionState::FORGET_PROMPT) {
    self->forgetPromptSelection = event.value;
    self->app.clearTapFlash();  // the action leaves this screen
    if (self->forgetPromptSelection == 1) {
      RenderLock lock(*self);
      WIFI_STORE.removeCredential(self->selectedSSID);
      const auto network = find_if(self->networks.begin(), self->networks.end(),
                                   [self](const WifiNetworkInfo& net) { return net.ssid == self->selectedSSID; });
      if (network != self->networks.end()) {
        network->hasSavedPassword = false;
      }
    }
    self->startWifiScan();
  }
}

#ifndef SIMULATOR
namespace {

// STA event counters. Written only by the Arduino network-event task, read only by the
// activity's loop() task: monotonic, 32-bit and naturally aligned, so a reader sees the
// old or the new value and never a torn one. A mutex would have to be taken inside the
// event callback to protect nothing.
//
// A plain function pointer, not a lambda: WiFi.onEvent()'s std::function overload
// heap-allocates a closure per registration (see CLAUDE.md on std::function), and
// nothing here needs to capture. File-static rather than per-activity for the same
// reason the callback cannot capture -- and because a counter that outlives the
// activity cannot be left dangling by one.
//
// They exist because ESP-IDF's events carry no application-level attempt id. Snapshotting
// a monotonic counter at each attempt boundary supplies the generation the event itself
// does not: a GOT_IP that did not advance the counter past an attempt's start belongs to
// an earlier attempt.
volatile uint32_t staGotIpEvents = 0;
volatile uint32_t staDisconnectEvents = 0;

void onStaNetworkEvent(arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    ++staGotIpEvents;
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    ++staDisconnectEvents;
  }
}

}  // namespace
#endif

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();

  // Load saved WiFi credentials - SD card operations need lock as we use SPI
  // for both
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

#ifndef SIMULATOR
  // Registered for the whole activity, removed in onExit(): these supply the observable
  // disconnect boundary a cancellation waits on, and the generation stamp completion
  // is validated against.
  WiFi.onEvent(onStaNetworkEvent, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onStaNetworkEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif

  // Reset state
  selectedNetworkIndex = 0;
  networks.clear();
  networkStatuses.clear();
  networkRowItems.clear();
  realNetworkCount = 0;
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  savePromptSelection = 0;
  forgetPromptSelection = 0;
  autoConnecting = false;
  manualNetworkListRequested = false;
  awaitingTimeSync = false;
  cancelPending = false;
  cancelOutcome = PendingOutcome::None;
  associationMs = 0;
  autoAttemptedSsids.clear();
  const size_t savedCredentialCount = WIFI_STORE.getCredentialCount();
  autoAttemptedSsids.reserve(savedCredentialCount);

  // Read the hardware-derived station MAC directly. WiFi.macAddress() depends
  // on the STA netif already existing, but this screen is entered while WiFi
  // is often still off (notably after an X4 Pro WiFi session).
  uint8_t mac[6] = {};
  char macStr[64];
  const esp_err_t macResult = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (macResult == ESP_OK) {
    snprintf(macStr, sizeof(macStr), "%s %02x-%02x-%02x-%02x-%02x-%02x", tr(STR_MAC_ADDRESS), mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
  } else {
    LOG_ERR("WIFI", "Failed to read station MAC (err=%d)", static_cast<int>(macResult));
    snprintf(macStr, sizeof(macStr), "%s --", tr(STR_MAC_ADDRESS));
  }
  cachedMacAddress = std::string(macStr);

  // No requestUpdate() here: it only sets a flag that ActivityManager::loop() acts on
  // after onEnter() returns, so the paint landed *after* whichever radio path below ran,
  // not before it. startWifiScan() paints its own screen ahead of its blocking bring-up;
  // attemptConnection() requests one that lands as soon as the asynchronous
  // WiFi.begin() has been issued.
  listNav.reset();
  resetUi();
  app.on(ACTION_ROW, &WifiSelectionActivity::onRowEvent, this);
  app.on(ACTION_SCAN, &WifiSelectionActivity::onScanEvent, this);
  app.on(ACTION_PROMPT, &WifiSelectionActivity::onPromptEvent, this);
  app.setScreen(&WifiSelectionActivity::listScreen, this);

  // Trigger first update to show scanning message
  requestUpdate();

  // Attempt to auto-connect to known networks. Try the last successful
  // network first for speed, then scan and try any visible saved networks by
  // signal strength. The user can interrupt this and show the scan result.
  if (allowAutoConnect && savedCredentialCount != 0) {
    const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      const auto cred = WIFI_STORE.findCredential(lastSsid);
      if (cred && tryAutoConnectCredential(*cred)) {
        return;
      }
    }

    startWifiScan(true);
    return;
  }

  // Fallback to scanning
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();

#ifndef SIMULATOR
  // Before anything else: the callback must not outlive the activity that armed it.
  WiFi.removeEvent(onStaNetworkEvent, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.removeEvent(onStaNetworkEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif

  LOG_DBG("WIFI", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  // Stop any ongoing WiFi scan
  LOG_DBG("WIFI", "Deleting WiFi scan...");
  WiFi.scanDelete();
  LOG_DBG("WIFI", "Free heap after scanDelete: %d bytes", ESP.getFreeHeap());

  // Note: We do NOT disconnect WiFi here - the parent activity
  // (CrossPointWebServerActivity) manages WiFi connection state. We just clean
  // up the scan and task.

  LOG_DBG("WIFI", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void WifiSelectionActivity::startWifiScan(const bool autoScan) {
  // TEMP diagnostic: brackets WiFi stack bring-up (radio init + async scan start),
  // which is the first large allocator on the network path.
  HEAP_TRACE_SCOPE("WIFI.start");
  autoConnecting = autoScan;
  manualNetworkListRequested = false;
  listNav.reset();
  state = WifiSelectionState::SCANNING;
  networks.clear();
  // The cached rows point into the cleared networks' strings; drop them too.
  networkStatuses.clear();
  networkRowItems.clear();
  requestUpdate();

  // Set WiFi mode to station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Start async scan
  WiFi.scanNetworks(true);  // true = async scan
}

void WifiSelectionActivity::processWifiScanResults() {
  const int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING) {
    // Scan still in progress
    return;
  }

  if (scanResult == WIFI_SCAN_FAILED) {
    networks.clear();
    realNetworkCount = 0;
    appendHiddenNetworkEntry();
    rebuildNetworkRowItems();
    autoConnecting = false;
    state = WifiSelectionState::NETWORK_LIST;
    selectedNetworkIndex = 0;
    requestUpdate();
    return;
  }

  // Scan complete, process results — deduplicate in-place, keeping strongest signal
  networks.clear();
  networks.reserve(scanResult);

  for (int i = 0; i < scanResult; i++) {
    char ssid[33];
    strlcpy(ssid, WiFi.SSID(i).c_str(), sizeof(ssid));
    const int32_t rssi = WiFi.RSSI(i);

    // Skip hidden networks (empty SSID)
    if (ssid[0] == '\0') {
      continue;
    }

    auto it =
        std::find_if(networks.begin(), networks.end(), [&ssid](const WifiNetworkInfo& n) { return n.ssid == ssid; });
    if (it == networks.end()) {
      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = rssi;
      network.isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      network.hasSavedPassword = WIFI_STORE.hasSavedCredential(network.ssid);
      networks.push_back(std::move(network));
    } else if (rssi > it->rssi) {
      it->rssi = rssi;
      it->isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
  }

  // Sort: saved-password networks first, then by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    if (a.hasSavedPassword != b.hasSavedPassword) {
      return a.hasSavedPassword;
    }
    return a.rssi > b.rssi;
  });

  realNetworkCount = networks.size();
  appendHiddenNetworkEntry();
  rebuildNetworkRowItems();

  WiFi.scanDelete();

  if (autoConnecting && !manualNetworkListRequested && tryNextSavedNetworkFromScan()) {
    return;
  }

  autoConnecting = false;
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
}

void WifiSelectionActivity::appendHiddenNetworkEntry() {
  // Synthetic list entry that lets the user type an SSID that is not broadcast.
  // ESP32 can join hidden APs as long as the SSID is supplied to WiFi.begin().
  WifiNetworkInfo placeholder;
  placeholder.rssi = 0;
  placeholder.isEncrypted = true;  // Treated as encrypted; an empty password still connects open APs
  placeholder.hasSavedPassword = false;
  placeholder.isHiddenPlaceholder = true;
  networks.push_back(std::move(placeholder));
}

// Derives networkStatuses/networkRowItems from `networks`. Called whenever
// `networks` changes (both branches of processWifiScanResults()) so
// buildListScreen() reuses the cached rows on every repaint instead of
// re-deriving a "+ * ||||" status string per network each time.
void WifiSelectionActivity::rebuildNetworkRowItems() {
  networkStatuses.assign(networks.size(), std::string());
  networkRowItems.clear();
  networkRowItems.reserve(networks.size());
  for (size_t i = 0; i < networks.size(); i++) {
    const auto& network = networks[i];
    if (!network.isHiddenPlaceholder) {
      networkStatuses[i] = std::string(network.hasSavedPassword ? "+ " : "") + (network.isEncrypted ? "* " : "") +
                           getSignalStrengthIndicator(network.rssi);
    }
    fui::ListItem item;
    item.label = network.isHiddenPlaceholder ? tr(STR_ADD_HIDDEN_NETWORK) : network.ssid.c_str();
    if (!networkStatuses[i].empty()) item.value = networkStatuses[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    networkRowItems.push_back(item);
  }
}

void WifiSelectionActivity::selectNetwork(const int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) {
    return;
  }

  const auto& network = networks[index];

  // Synthetic "Add hidden network..." entry: prompt the user to type the SSID first
  if (network.isHiddenPlaceholder) {
    promptHiddenSsid();
    return;
  }

  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Check if we have saved credentials for this network
  const auto savedCred = WIFI_STORE.findCredential(selectedSSID);
  if (savedCred && !savedCred->password.empty()) {
    // Use saved password - connect directly
    enteredPassword = savedCred->password;
    usedSavedPassword = true;
    LOG_DBG("WiFi", "Using saved credentials for %s", selectedSSID.c_str());
    attemptConnection();
    return;
  }

  if (selectedRequiresPassword) {
    promptPasswordEntry();
  } else {
    // Connect directly for open networks
    attemptConnection();
  }
}

void WifiSelectionActivity::promptPasswordEntry() {
  // Show password entry
  state = WifiSelectionState::PASSWORD_ENTRY;
  // Don't allow screen updates while changing activity
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_WIFI_PASSWORD),
                                                                 "",  // No initial text
                                                                 64,  // Max password length
                                                                 InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             state = WifiSelectionState::NETWORK_LIST;
                           } else {
                             enteredPassword = std::get<KeyboardResult>(result.data).text;
                             // state will be updated in next loop iteration
                           }
                         });
}

void WifiSelectionActivity::promptHiddenSsid() {
  selectedSSID.clear();
  selectedRequiresPassword = true;  // Hidden networks are usually encrypted; empty password still joins open APs
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Suppress rendering during the activity transition (see render()).
  state = WifiSelectionState::HIDDEN_SSID_ENTRY;
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_WIFI_SSID),
                                                                 "",  // No initial text
                                                                 32,  // Max SSID length (IEEE 802.11: 32 bytes)
                                                                 InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             state = WifiSelectionState::NETWORK_LIST;
                             return;
                           }
                           selectedSSID = std::get<KeyboardResult>(result.data).text;
                           if (selectedSSID.empty()) {
                             state = WifiSelectionState::NETWORK_LIST;
                           }
                           // Otherwise stay in HIDDEN_SSID_ENTRY; loop() continues the flow.
                         });
}

bool WifiSelectionActivity::hasAttemptedAutoSsid(const std::string& ssid) const {
  return std::find(autoAttemptedSsids.begin(), autoAttemptedSsids.end(), ssid) != autoAttemptedSsids.end();
}

bool WifiSelectionActivity::tryAutoConnectCredential(const WifiCredential& cred) {
  if (hasAttemptedAutoSsid(cred.ssid)) {
    return false;
  }

  LOG_DBG("WIFI", "Attempting saved network: %s", cred.ssid.c_str());
  autoAttemptedSsids.push_back(cred.ssid);
  selectedSSID = cred.ssid;
  enteredPassword = cred.password;
  selectedRequiresPassword = !cred.password.empty();
  usedSavedPassword = true;
  autoConnecting = true;
  manualNetworkListRequested = false;
  attemptConnection();
  requestUpdate();
  return true;
}

bool WifiSelectionActivity::tryNextSavedNetworkFromScan() {
  for (const auto& network : networks) {
    if (!network.hasSavedPassword || hasAttemptedAutoSsid(network.ssid)) {
      continue;
    }

    const auto cred = WIFI_STORE.findCredential(network.ssid);
    if (cred && tryAutoConnectCredential(*cred)) {
      return true;
    }
  }
  return false;
}

void WifiSelectionActivity::handleAutoConnectFailure() {
  LOG_DBG("WIFI", "Saved network failed: %s", selectedSSID.c_str());
  WiFi.disconnect();

  if (!networks.empty()) {
    if (tryNextSavedNetworkFromScan()) {
      return;
    }
    autoConnecting = false;
    state = WifiSelectionState::NETWORK_LIST;
    selectedNetworkIndex = 0;
    requestUpdate();
    return;
  }

  startWifiScan(true);
}

void WifiSelectionActivity::showNetworkListFromAutoConnect() {
  LOG_DBG("WIFI", "User requested manual network list");
  WiFi.disconnect();
  autoConnecting = false;
  manualNetworkListRequested = true;

  if (networks.empty()) {
    startWifiScan(false);
    manualNetworkListRequested = true;
    return;
  }

  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
}

void WifiSelectionActivity::attemptConnection() {
  // TEMP diagnostic: brackets WiFi.begin() only. Association completes
  // asynchronously; the paired "WIFI.connected" mark below is the real end edge.
  HEAP_TRACE_SCOPE("WIFI.connect");
  state = autoConnecting ? WifiSelectionState::AUTO_CONNECTING : WifiSelectionState::CONNECTING;
  connectedIP.clear();
  connectionError.clear();
  // Cleared here rather than only where cancellation completes: the user can leave an
  // in-flight cancellation via Confirm (showNetworkListFromAutoConnect) or Back, and a
  // flag left set would make the next attempt cancel itself on its first tick.
  cancelPending = false;
  cancelOutcome = PendingOutcome::None;
  WifiDiagLog::append("connecting: ssid=" + selectedSSID + " auto=" + (autoConnecting ? "1" : "0") +
                      " savedPassword=" + (usedSavedPassword ? "1" : "0"));
  // requestUpdate(), not requestUpdateAndWait(): the panel paints as soon as
  // ActivityManager::loop() regains control, which is right after the asynchronous
  // WiFi.begin() below has been issued -- association then runs for seconds afterwards,
  // so "Connecting to <ssid>" is still up for effectively all of it. Waiting here put a
  // full e-ink repaint, plus the SD append above, in front of the radio work.
  requestUpdate();

  prepareRadio();
  beginConnection();
}

void WifiSelectionActivity::prepareRadio() {
  WiFi.persistent(false);  // Credentials are managed by WifiCredentialStore; suppress SDK NVS auto-connect
  WiFi.mode(WIFI_STA);
  // eraseap, but not wifioff: aborting an in-progress SDK auto-connect and clearing the
  // NVS-saved SSID is the point here, whereas also powering the radio down made the
  // following WiFi.begin() pay a full driver bring-up on every single attempt. The
  // settle delay stays because esp_wifi_disconnect() completes asynchronously and an
  // esp_wifi_connect() issued inside that window is rejected with ESP_ERR_WIFI_CONN.
  WiFi.disconnect(false, true);
  delay(100);

  // Scan all channels so networks with multiple APs use the strongest matching
  // BSSID instead of the first match found by the framework's default fast scan.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  // Set hostname so routers show "CrossPoint-Reader-AABBCCDDEEFF" instead of "esp32-XXXXXXXXXXXX"
  uint8_t mac[6] = {};
  const esp_err_t macResult = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (macResult == ESP_OK) {
    char hostname[sizeof("CrossPoint-Reader-") + 12];
    snprintf(hostname, sizeof(hostname), "CrossPoint-Reader-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    WiFi.setHostname(hostname);
  } else {
    LOG_ERR("WIFI", "Failed to read station MAC for hostname (err=%d)", static_cast<int>(macResult));
  }

  if (selectedRequiresPassword && !enteredPassword.empty()) {
    WiFi.begin(selectedSSID.c_str(), enteredPassword.c_str());
  } else {
    LOG_ERR("WIFI", "Failed to read station MAC for hostname (err=%d)", static_cast<int>(macResult));
  }
}

void WifiSelectionActivity::beginConnection() {
  const char* password = (selectedRequiresPassword && !enteredPassword.empty()) ? enteredPassword.c_str() : nullptr;

  // New generation for this attempt, with the GOT_IP counter snapshotted against it: a
  // WL_CONNECTED that does not advance that counter past this point was produced by an
  // earlier attempt (see completionBelongsToActiveAttempt).
  ++attemptId;
#ifndef SIMULATOR
  gotIpAtAttemptStart = staGotIpEvents;
#endif

  // Last statement before WiFi.begin(): every timeout in wifi_connect_policy is a budget
  // for the radio, so display and SD work must not be charged against it.
  connectionStartTime = millis();
  WiFi.begin(selectedSSID.c_str(), password);
}

void WifiSelectionActivity::requestAttemptCancellation(const bool hardFail, const PendingOutcome outcome) {
  cancelPending = true;
  cancelOutcome = outcome;
  cancelStartMs = millis();
#ifndef SIMULATOR
  disconnectsAtCancel = staDisconnectEvents;
#endif
  WiFi.disconnect(false, false);
}

WifiSelectionActivity::CancelProgress WifiSelectionActivity::advanceCancellation() {
#ifdef SIMULATOR
  return CancelProgress::Confirmed;
#else
  const unsigned long waitedMs = millis() - cancelStartMs;

  if (staDisconnectEvents != disconnectsAtCancel) {
    // The superseded attempt is definitively finished, so any later GOT_IP can only
    // belong to whatever is started next.
    return CancelProgress::Confirmed;
  }

  if (waitedMs < wifi_connect_policy::CANCEL_CONFIRM_TIMEOUT_MS) {
    return CancelProgress::Waiting;
  }

  // Silence is not confirmation. With no disconnect event the superseded attempt may
  // still be alive, and issuing WiFi.begin() here would put two attempts in flight --
  // the exact race this confirmation exists to prevent, and one the event counters
  // cannot untangle afterwards, because a GOT_IP landing after the new attempt's
  // snapshot is indistinguishable from that attempt's own. So escalate to a stop that
  // can be observed, rather than waiting a second time and hoping.

  // WiFi.mode(WIFI_OFF) runs esp_wifi_stop() and then esp_wifi_deinit() inline and
  // reports whether they succeeded; getMode() then reads WIFI_MODE_NULL back out of the
  // driver's own started flag. Both are synchronous, so this is an observed stopped
  // state and not another timeout -- and a stopped, deinitialised driver has no station
  // left to deliver a callback for the old attempt.
  const bool stopped = WiFi.mode(WIFI_OFF) && WiFi.getMode() == WIFI_MODE_NULL;
  if (!stopped) {
    // A definite negative, not an expired clock: the radio will not say it stopped, so
    // there is no point at which starting another attempt would be sound.
    return CancelProgress::Unrecoverable;
  }
  return CancelProgress::ConfirmedAfterRadioStop;
#endif
}

bool WifiSelectionActivity::completionBelongsToActiveAttempt() {
#ifdef SIMULATOR
  return true;
#else
  // A GOT_IP newer than this attempt's start is the only thing that makes a
  // WL_CONNECTED this attempt's own. Withheld rather than failed: the counter is bumped
  // by the same event dispatch that sets WL_CONNECTED, so a genuine completion resolves
  // on the very next tick, while a superseded one never advances the counter and is
  // left to the ordinary connection timeout.
  if (staGotIpEvents != gotIpAtAttemptStart) {
    return true;
  }
  return false;
#endif
}

void WifiSelectionActivity::checkConnectionStatus() {
  if (state != WifiSelectionState::CONNECTING && state != WifiSelectionState::AUTO_CONNECTING) {
    return;
  }

  const wl_status_t status = WiFi.status();

  // A superseded attempt is still being torn down: nothing may complete, and whatever
  // replaces it may not start, until that teardown has actually been observed.
  if (cancelPending) {
    const CancelProgress progress = advanceCancellation();
    if (progress == CancelProgress::Waiting) {
      return;
    }
    cancelPending = false;
    const PendingOutcome outcome = cancelOutcome;
    cancelOutcome = PendingOutcome::None;

    if (progress == CancelProgress::Unrecoverable) {
      // Nothing here may start another attempt. Fail this connection through the
      // ordinary failure path instead: auto-connect moves on to the next saved network
      // (or the manual list), and a user-initiated connect gets the usual failure
      // screen, from which Confirm or Back offers a retry. Reuses the existing timeout
      // string rather than adding one: the attempt did time out, and inventing an
      // untranslated message for a rare device fault would be worse than reusing an
      // accurate one.
      WifiDiagLog::append("cancel-abort: ssid=" + selectedSSID + " radio did not confirm stop");
      state = WifiSelectionState::CONNECTION_FAILED;
      requestUpdate();
      return;
    }

    if (progress == CancelProgress::ConfirmedAfterRadioStop) {
      // The escalation stopped and deinitialised the radio, so it has to be brought
      // back up before anything can associate. Untouched on the ordinary
      // event-confirmed path, which needs none of this.
      prepareRadio();
    }

    // Only now, on an observed teardown, may the superseded attempt's outcome run --
    // handleAutoConnectFailure() starts the next attempt, and it must not overlap this
    // one.
    if (outcome == PendingOutcome::AutoConnectFailure) {
      handleAutoConnectFailure();
      return;
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }

  if (status == WL_CONNECTED && !completionBelongsToActiveAttempt()) {
    return;  // Withheld; the ordinary connection timeout still bounds this.
  }

  if (status == WL_CONNECTED) {
    // Successfully connected
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;
    autoConnecting = false;
    HEAP_TRACE_MARK("WIFI.connected", "EVENT");
    // elapsedMs is association only -- WiFi.begin() to WL_CONNECTED/GOT_IP. Total
    // "WiFi ready" is reported separately once the clock sync has also settled, since
    // that is no longer part of this measurement.
    associationMs = millis() - connectionStartTime;
    WifiDiagLog::append("connected: ssid=" + selectedSSID + " ip=" + std::string(ipStr) +
                        " elapsedMs=" + std::to_string(associationMs));
#ifndef SIMULATOR
    {
      uint8_t traceBssid[6] = {};
      WiFi.BSSID(traceBssid);
    }
#endif

#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
    uint8_t connectedBssid[6] = {};
    WiFi.BSSID(connectedBssid);
    LOG_DBG("WIFI", "Connected BSSID: %02x:%02x:%02x:%02x:%02x:%02x, channel: %d, RSSI: %d dBm",
            static_cast<unsigned>(connectedBssid[0]), static_cast<unsigned>(connectedBssid[1]),
            static_cast<unsigned>(connectedBssid[2]), static_cast<unsigned>(connectedBssid[3]),
            static_cast<unsigned>(connectedBssid[4]), static_cast<unsigned>(connectedBssid[5]), WiFi.channel(),
            WiFi.RSSI());
#endif

    // System time must be roughly correct for TLS certificate validation -- OPDS
    // fetches, OTA update checks, and font downloads are all HTTPS. Neither the
    // DS3231 (X3 -- hour/minute only, no calendar date) nor X4 (no RTC at all) can
    // preserve the date across a reboot, so without re-syncing here every HTTPS
    // request silently fails cert validation after every restart (confirmed via a
    // real device: "Foulad eBooks" repeatedly failed with a bare ESP_ERR_HTTP_CONNECT
    // right after boot, with healthy heap and a healthy server -- the system clock
    // was the missing piece). Cheap to check on every connection: skipped entirely
    // once time is already valid, so this doesn't add the ~5s SNTP wait on every
    // WiFi connect, only right after a cold boot/OTA restart.
    // Started, not waited on: nothing on this screen needs the clock, so the up-to-5s
    // SNTP wait belongs in loop() (see finishWhenClockSynced) rather than in front of
    // onComplete(). Guarded out of the simulator build: crosspoint-simulator's own
    // HalClock stub doesn't implement these (its network stack isn't real
    // ESP-IDF/mbedTLS, so the cert-validation clock issue this works around doesn't
    // apply there).
#ifndef SIMULATOR
    if (!HalClock::isSystemTimeValid() && HalClock::beginSystemTimeSync()) {
      awaitingTimeSync = true;
      timeSyncStartMs = millis();
    }
#endif

    // Save this as the last connected network, plus where it was actually joined so the
    // next attempt can skip the scan - SD card operations need lock as we use SPI for both
    {
      RenderLock lock(*this);
      WIFI_STORE.setLastConnectedSsid(selectedSSID);
    }

    // If we entered a new password, ask if user wants to save it
    // Otherwise fall through to CONNECTED, which completes as soon as the clock is ready
    if (!usedSavedPassword && !enteredPassword.empty()) {
      state = WifiSelectionState::SAVE_PROMPT;
      savePromptSelection = 0;  // Default to "Yes"
      requestUpdate();
    } else {
      LOG_DBG("WIFI", "Connected with saved/open credentials");
      state = WifiSelectionState::CONNECTED;
      // Only worth a repaint when there is actually a wait to show; otherwise loop()
      // completes on the very next tick and the paint would never be seen.
      if (awaitingTimeSync) {
        requestUpdate();
      }
    }
    return;
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    connectionError = tr(STR_ERROR_GENERAL_FAILURE);
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = tr(STR_ERROR_NETWORK_NOT_FOUND);
    }
    WifiDiagLog::append("failed: ssid=" + selectedSSID +
                        " status=" + (status == WL_NO_SSID_AVAIL ? "NO_SSID_AVAIL" : "CONNECT_FAILED") +
                        " auto=" + (autoConnecting ? "1" : "0"));
    requestAttemptCancellation(/*hardFail=*/true,
                               autoConnecting ? PendingOutcome::AutoConnectFailure : PendingOutcome::ConnectionFailed);
    return;
  }

  // Check for timeout. Auto-connect uses the shorter budget so a saved network that's
  // merely out of range doesn't eat 15s before moving on to the next one; see
  // wifi_connect_policy for the measurements both budgets are sized against.
  const unsigned long timeoutMs =
      autoConnecting ? wifi_connect_policy::AUTO_CONNECTION_TIMEOUT_MS : wifi_connect_policy::CONNECTION_TIMEOUT_MS;
  if (millis() - connectionStartTime > timeoutMs) {
    connectionError = tr(STR_ERROR_CONNECTION_TIMEOUT);
    WifiDiagLog::append("timeout: ssid=" + selectedSSID + " afterMs=" + std::to_string(timeoutMs) + " auto=" +
                        (autoConnecting ? "1" : "0") + " lastStatus=" + std::to_string(static_cast<int>(status)));
    // The attempt is out of budget but may still be associating, so it is torn down
    // under confirmation rather than with a bare disconnect: auto-connect's next saved
    // network would otherwise begin while this one is still in flight.
    requestAttemptCancellation(/*hardFail=*/false,
                               autoConnecting ? PendingOutcome::AutoConnectFailure : PendingOutcome::ConnectionFailed);
    return;
  }
}

void WifiSelectionActivity::finishWhenClockSynced() {
  if (awaitingTimeSync) {
    const bool synced = HalClock::isSystemTimeSyncComplete();
    if (!synced && millis() - timeSyncStartMs < wifi_connect_policy::TIME_SYNC_TIMEOUT_MS) {
      return;  // Poll again next tick; the connected screen is already up.
    }
    awaitingTimeSync = false;
    if (!synced) {
      LOG_ERR("WIFI", "NTP sync did not land; HTTPS cert validation may fail until it does");
    }
  }

  // Sync RTC from NTP on the first successful WiFi connection only. The DS3231
  // drifts ~2 ppm so one sync is enough; users can force a re-sync from
  // Settings > Customise Status Bar > Sync clock now.
  if (halClock.isAvailable() && !SETTINGS.clockHasBeenSynced) {
    if (halClock.syncFromNTP()) {
      SETTINGS.clockHasBeenSynced = 1;
      SETTINGS.saveToFile();
    }
  }

  // Association and total WiFi-ready are separate figures now that the clock sync runs
  // asynchronously: assocMs ends at GOT_IP, readyMs at the point the parent may use the
  // connection for TLS.
  onComplete(true);
}

void WifiSelectionActivity::loop() {
  // Check scan progress
  if (state == WifiSelectionState::SCANNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      WiFi.scanDelete();
      onComplete(false);
      return;
    }
    if (autoConnecting && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      autoConnecting = false;
      manualNetworkListRequested = true;
      requestUpdate();
    }
    processWifiScanResults();
    return;
  }

  // Check connection progress
  if (state == WifiSelectionState::CONNECTING || state == WifiSelectionState::AUTO_CONNECTING) {
    if (state == WifiSelectionState::AUTO_CONNECTING) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        WiFi.disconnect();
        onComplete(false);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        showNetworkListFromAutoConnect();
        return;
      }
    }
    checkConnectionStatus();
    return;
  }

  // Reached once the hidden-network SSID has been entered (and was non-empty).
  if (state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
    const auto savedCred = WIFI_STORE.findCredential(selectedSSID);
    if (savedCred && !savedCred->password.empty()) {
      // We already know this hidden network - connect with the saved password
      enteredPassword = savedCred->password;
      usedSavedPassword = true;
      LOG_DBG("WiFi", "Using saved password for hidden network %s", selectedSSID.c_str());
      attemptConnection();
    } else {
      // Prompt for the password (empty password connects to open hidden APs)
      promptPasswordEntry();
    }
    return;
  }

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    // Reach here once password entry finished in subactivity
    attemptConnection();
    return;
  }

  // Handle save prompt state
  if (state == WifiSelectionState::SAVE_PROMPT) {
    // Touch goes through the FreeInkApp: render() registered the dialog
    // button hit rects; route the snapshot and let onPromptEvent dispatch.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onPromptEvent

    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (savePromptSelection > 0) {
        savePromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (savePromptSelection < 1) {
        savePromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (savePromptSelection == 0) {
        // User chose "Yes" - save the password
        RenderLock lock(*this);
        WIFI_STORE.addCredential(selectedSSID, enteredPassword);
      }
      // Parent will start the web server, once the clock is usable for TLS
      state = WifiSelectionState::CONNECTED;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip saving, complete anyway
      state = WifiSelectionState::CONNECTED;
    }
    return;
  }

  // Handle forget prompt state (connection failed with saved credentials)
  if (state == WifiSelectionState::FORGET_PROMPT) {
    // Touch goes through the FreeInkApp: render() registered the dialog
    // button hit rects; route the snapshot and let onPromptEvent dispatch.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onPromptEvent

    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (forgetPromptSelection > 0) {
        forgetPromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        RenderLock lock(*this);
        // User chose "Forget network" - forget the network
        WIFI_STORE.removeCredential(selectedSSID);
        // Update the network list to reflect the change
        const auto network = find_if(networks.begin(), networks.end(),
                                     [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
        if (network != networks.end()) {
          network->hasSavedPassword = false;
        }
      }
      // Go back to network list (whether Cancel or Forget network was selected)
      startWifiScan();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip forgetting, go back to network list
      startWifiScan();
    }
    return;
  }

  // Connected: hand the result to the parent as soon as the clock is usable for TLS.
  if (state == WifiSelectionState::CONNECTED) {
    finishWhenClockSynced();
    return;
  }

  // Handle connection failed state
  if (state == WifiSelectionState::CONNECTION_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // If we were auto-connecting or using a saved credential, offer to forget
      // the network
      if (autoConnecting || usedSavedPassword) {
        autoConnecting = false;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
      } else {
        // Go back to network list on failure for non-saved credentials
        state = WifiSelectionState::NETWORK_LIST;
      }
      requestUpdate();
      return;
    }
  }

  // Handle network list state
  if (state == WifiSelectionState::NETWORK_LIST) {
    if (manualNetworkListRequested) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
        manualNetworkListRequested = false;
      }
      return;
    }

    // Check for Back button to exit (cancel)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onComplete(false);
      return;
    }

    // Check for Confirm button to select network or rescan
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!networks.empty()) {
        selectNetwork(selectedNetworkIndex);
      } else {
        startWifiScan();
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      startWifiScan();
      return;
    }

    const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
    if (leftPressed) {
      const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
      if (hasSavedPassword) {
        selectedSSID = networks[selectedNetworkIndex].ssid;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
        requestUpdate();
        return;
      }
    }

    // Touch goes through the FreeInkApp: render() registered the row hit
    // rects; route the snapshot and let onRowEvent dispatch. Long-press on a
    // network row fires "forget" while the finger is down.
    const auto route = routeTouch(mappedInput, /*withLongPress=*/true);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onRowEvent

    if (!networks.empty()) {
      // Swipes scroll the viewport; the selection stays put and button
      // navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? listNav.visibleRows : -listNav.visibleRows;
        if (listNav.scrollBy(delta, static_cast<int>(networks.size()))) requestUpdate();
        return;
      }
    }

    const auto moveSelection = [this](const int index) {
      selectedNetworkIndex = static_cast<size_t>(index);
      listNav.selected = index;
      listNav.follow(static_cast<int>(networks.size()));
      requestUpdate();
    };
    buttonNavigator.onNext(
        [this, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectedNetworkIndex, networks.size())); });
    buttonNavigator.onPrevious([this, &moveSelection] {
      moveSelection(ButtonNavigator::previousIndex(selectedNetworkIndex, networks.size()));
    });
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return " |||";  // Good
  }
  if (rssi >= -70) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void WifiSelectionActivity::render(RenderLock&&) {
  // Don't render if we're in a keyboard-entry state - we're just transitioning
  // from the keyboard subactivity back to the main activity
  if (state == WifiSelectionState::PASSWORD_ENTRY || state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
    return;
  }

  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  // Draw header
  // STR_NETWORKS_FOUND is ~37 bytes once the Arabic translation is substituted,
  // so 32 truncated it. See ClockSyncActivity for the same class of bug.
  char countStr[64];
  snprintf(countStr, sizeof(countStr), tr(STR_NETWORKS_FOUND), realNetworkCount);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_WIFI_NETWORKS), countStr);
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      cachedMacAddress.c_str());

  switch (state) {
    case WifiSelectionState::AUTO_CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::SCANNING:
      renderConnecting(&screen, &metrics);  // Reuse connecting screen with different message
      break;
    case WifiSelectionState::NETWORK_LIST:
      renderNetworkList(&screen, &metrics);
      break;
    case WifiSelectionState::HIDDEN_SSID_ENTRY:
      // Transitioning to/from the SSID keyboard subactivity - nothing to draw
      break;
    case WifiSelectionState::CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTED:
      renderConnected(&screen, &metrics);
      break;
    case WifiSelectionState::SAVE_PROMPT:
    case WifiSelectionState::FORGET_PROMPT: {
      // The app's screen builder draws the option dialog panel itself.
      renderUi();
      const auto labels =
          mappedInput.mapLabels(state == WifiSelectionState::SAVE_PROMPT ? tr(STR_CANCEL) : tr(STR_BACK),
                                tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case WifiSelectionState::CONNECTION_FAILED:
      renderConnectionFailed(&screen, &metrics);
      break;
  }

  renderer.displayBuffer();
}

void WifiSelectionActivity::listScreen(UiScreen& screen, void* user) {
  static_cast<WifiSelectionActivity*>(user)->buildListScreen(screen);
}

void WifiSelectionActivity::buildListScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content below the header + MAC sub-band, above the legend line.
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                           metrics.verticalSpacing),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing * 2),
      static_cast<int16_t>(safe.x)});

  if (state == WifiSelectionState::SAVE_PROMPT || state == WifiSelectionState::FORGET_PROMPT) {
    buildPromptDialog(screen);
    return;
  }

  if (networks.empty()) {
    screen.centeredText(tr(STR_NO_NETWORKS), screen.theme().bodyText);
    if (mappedInput.hasTouch()) {
      // Touch has no OK button to rescan with; offer the retry on screen instead
      // of the "Press OK" hint renderNetworkList draws for button boards.
      const auto& theme = screen.theme();
      const fui::Rect body = screen.body();
      const int16_t buttonWidth = static_cast<int16_t>(body.width / 2);
      const fui::Rect buttonRect{static_cast<int16_t>(body.x + (body.width - buttonWidth) / 2),
                                 static_cast<int16_t>(body.y + body.height * 2 / 3), buttonWidth, theme.rowHeight};
      fui::ButtonProps scan;
      scan.label = tr(STR_RETRY);
      scan.action = ACTION_SCAN;
      scan.inputMask = fui::InputTouch;
      scan.text = theme.bodyText;
      fui::button(screen.frame(), buttonRect, scan);
    }
    return;
  }

  // networkStatuses/networkRowItems are built once per processWifiScanResults()
  // call (see rebuildNetworkRowItems()) and reused here on every repaint.
  fui::ListProps props;
  props.items = networkRowItems.data();
  props.count = static_cast<uint16_t>(networkRowItems.size());
  props.action = ACTION_ROW;
  // Tap opens; long-press a saved network forgets it (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;  // air between the signal bars and the row edge
  // Long SSIDs grow their row to a second line; the trailing value is
  // just the short status glyphs, so skip the balanced 60%-band wrap cap.
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.balanceWrappedLabelWithValue = false;
  listNav.selected = static_cast<int>(selectedNetworkIndex);
  props.partialTrailingRow = true;
  screen.syncListViewport(listNav, props, static_cast<int>(networks.size()));
  screen.list(props);
}

void WifiSelectionActivity::buildPromptDialog(UiScreen& screen) {
  const bool isForget = state == WifiSelectionState::FORGET_PROMPT;

  // Owned for the duration of the draw; the dialog wraps long SSIDs itself.
  const std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;

  const int selection = isForget ? forgetPromptSelection : savePromptSelection;
  fui::DialogOption options[2];
  options[0].label = isForget ? tr(STR_CANCEL) : tr(STR_YES);
  options[1].label = isForget ? tr(STR_FORGET_NETWORK) : tr(STR_NO);
  for (int i = 0; i < 2; i++) {
    options[i].action = ACTION_PROMPT;
    options[i].value = static_cast<int16_t>(i);
    options[i].state = selection == i ? fui::StateFocused : fui::StateNormal;
  }

  fui::OptionDialogProps props;
  props.title = isForget ? tr(STR_FORGET_NETWORK) : tr(STR_CONNECTED);
  props.headline = ssidInfo.c_str();
  props.message = isForget ? tr(STR_FORGET_AND_REMOVE) : tr(STR_SAVE_PASSWORD);
  props.options = options;
  props.optionCount = 2;
  // Stacked full-width options (matching OptionPopup): side-by-side halves
  // truncate the long "Forget network" label.
  props.verticalOptions = true;
  props.titleText = screen.theme().smallText;
  props.titleText.bold = true;
  // TextStyle defaults to maxLines=1 (ellipsis truncation); let the SSID
  // headline and the question wrap. optionDialogHeight measures with the same
  // styles, so the dialog grows to fit the wrapped lines.
  props.headlineText = screen.theme().bodyText;
  props.headlineText.maxLines = 2;
  props.messageText = screen.theme().smallText;
  props.messageText.maxLines = 3;
  props.buttonText = screen.theme().smallText;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // defaultPopupStyles() (fui::optionDialog's fallback when styles is left
  // unset) has no border; opt one in explicitly using the theme's popup frame
  // metrics, matching OptionPopup::render().
  const auto& metrics = UITheme::getInstance().getMetrics();
  props.styles = fui::defaultPopupStyles();
  props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
  props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
  props.styles.selected = props.styles.normal;
  props.styles.focused = props.styles.normal;
  props.styles.active = props.styles.normal;
  props.styles.disabled = props.styles.normal;

  const fui::Rect body = screen.body();
  int16_t width = static_cast<int16_t>(renderer.getScreenWidth() * 3 / 4);
  if (width > body.width) width = body.width;
  const int16_t height = fui::optionDialogHeight(screen.target(), props, width);
  fui::optionDialog(screen.frame(), fui::centeredRect(body, fui::Size{width, height}), props);
}

void WifiSelectionActivity::renderNetworkList(const Rect* screen, const ThemeMetrics* metrics) {
  renderUi();
  if (networks.empty() && !mappedInput.hasTouch()) {
    // Below the centered "no networks" line the app drew. Touch boards get an
    // on-screen Retry button from the screen builder instead of this hint.
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = screen->y + (screen->height - height) / 2;
    UITheme::drawCenteredText(renderer, *screen, SMALL_FONT_ID, top + height + 10, tr(STR_PRESS_OK_SCAN));
  }

  GUI.drawHelpText(renderer,
                   Rect{screen->x, screen->y + screen->height - metrics->contentSidePadding - 15, screen->width, 20},
                   tr(STR_NETWORK_LEGEND));

  const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
  const char* forgetLabel = hasSavedPassword ? tr(STR_FORGET_BUTTON) : "";

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), forgetLabel, tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const {
  constexpr int MAX_STATUS_LINES = 2;
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height) / 2;
  const int statusX = screen->x + metrics->contentSidePadding;
  const int statusWidth = screen->width - metrics->contentSidePadding * 2;

  if (state == WifiSelectionState::SCANNING) {
    const char* statusText = autoConnecting ? tr(STR_FINDING_SAVED_WIFI) : tr(STR_SCANNING);
    const Rect statusBounds{statusX, screen->y, statusWidth, screen->height};
    UITheme::drawCenteredWrappedText(renderer, statusBounds, UI_10_FONT_ID, statusText, MAX_STATUS_LINES);
    if (autoConnecting) {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SHOW_NETWORKS), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else {
    const char* statusText = autoConnecting ? tr(STR_CONNECTING_SAVED_WIFI) : tr(STR_CONNECTING);
    const Rect statusBounds{statusX, screen->y, statusWidth, top - metrics->verticalSpacing - screen->y};
    UITheme::drawCenteredWrappedText(renderer, statusBounds, UI_12_FONT_ID, statusText, MAX_STATUS_LINES, true,
                                     EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::BOTTOM);

    std::string ssidInfo = std::string(tr(STR_TO_PREFIX)) + selectedSSID;
    if (ssidInfo.length() > 25) {
      ssidInfo.replace(22, ssidInfo.length() - 22, "...");
    }
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());
    if (autoConnecting) {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SHOW_NETWORKS), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }
}

void WifiSelectionActivity::renderConnected(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 4) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 30, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 10, ssidInfo.c_str());

  const std::string ipInfo = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, ipInfo.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 2) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 20, tr(STR_CONNECTION_FAILED), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 20, connectionError.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::onComplete(const bool connected) {
  // Counted, not just logged: a single "completed" line proves nothing about
  // exactly-once, whereas a second line carrying n=2 would be the failure itself.
  ++completionCount;

  ActivityResult result;
  result.isCancelled = !connected;
  if (connected) {
    result.data = WifiResult{true, selectedSSID, connectedIP};
  }
  setResult(std::move(result));
  finish();
}
