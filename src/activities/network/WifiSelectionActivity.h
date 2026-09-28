#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "network/WifiConnectPolicy.h"
#include "util/ButtonNavigator.h"

struct Rect;
struct ThemeMetrics;
struct WifiCredential;

// Structure to hold WiFi network information
struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi;
  bool isEncrypted;
  bool hasSavedPassword;             // Whether we have saved credentials for this network
  bool isHiddenPlaceholder = false;  // Synthetic "Add hidden network..." list entry
};

// WiFi selection states
enum class WifiSelectionState {
  AUTO_CONNECTING,    // Trying to connect to the last known network
  SCANNING,           // Scanning for networks
  NETWORK_LIST,       // Displaying available networks
  HIDDEN_SSID_ENTRY,  // Entering SSID for a hidden network
  PASSWORD_ENTRY,     // Entering password for selected network
  CONNECTING,         // Attempting to connect
  CONNECTED,          // Successfully connected
  SAVE_PROMPT,        // Asking user if they want to save the password
  CONNECTION_FAILED,  // Connection failed
  FORGET_PROMPT       // Asking user if they want to forget the network
};

/**
 * WifiSelectionActivity is responsible for scanning WiFi APs and connecting to them.
 * It will:
 * - Enter scanning mode on entry
 * - List available WiFi networks
 * - Allow selection and launch KeyboardEntryActivity for password if needed
 * - Save the password if requested
 * - Call onComplete callback when connected or cancelled
 *
 * The onComplete callback receives true if connected successfully, false if cancelled.
 */
class WifiSelectionActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  WifiSelectionState state = WifiSelectionState::SCANNING;
  size_t selectedNetworkIndex = 0;
  std::vector<WifiNetworkInfo> networks;
  // Number of real (scanned) networks, excluding the synthetic hidden-network entry
  size_t realNetworkCount = 0;

  // Row buffers derived from `networks`, rebuilt only when it changes
  // (processWifiScanResults()) instead of on every repaint — buildListScreen()
  // used to re-derive a "+ * ||||" status string per network on every render
  // (cursor move, tap flash, ...).
  std::vector<std::string> networkStatuses;
  std::vector<freeink::ui::ListItem> networkRowItems;
  void rebuildNetworkRowItems();

  // Selected network for connection
  std::string selectedSSID;
  bool selectedRequiresPassword = false;

  // Connection result
  std::string connectedIP;
  std::string connectionError;

  // Password to potentially save (from keyboard or saved credentials)
  std::string enteredPassword;

  // Cached MAC address string for display
  std::string cachedMacAddress;

  // Whether network was connected using a saved password (skip save prompt)
  bool usedSavedPassword = false;

  // Whether to attempt auto-connect on entry
  const bool allowAutoConnect;

  // Whether we are attempting to auto-connect or auto-scan saved networks.
  bool autoConnecting = false;

  // True from the Confirm press that stops auto-connect until that button is released.
  bool manualNetworkListRequested = false;

  // Saved SSIDs already attempted during the current auto-connect session.
  std::vector<std::string> autoAttemptedSsids;

  // Save/forget prompt selection (0 = Yes, 1 = No)
  int savePromptSelection = 0;
  int forgetPromptSelection = 0;

  // Set immediately before WiFi.begin(), so the budgets in wifi_connect_policy measure
  // the radio's own work and nothing else.
  unsigned long connectionStartTime = 0;

  // Set while an SNTP sync started at association is still outstanding. The activity
  // stays in CONNECTED and polls it from loop() rather than blocking the connect path.
  bool awaitingTimeSync = false;
  unsigned long timeSyncStartMs = 0;

  // Monotonic attempt generation. Every WiFi.begin() takes a new id, and the STA event
  // counters are snapshotted against it, so a completion can be attributed to the
  // attempt that actually produced it rather than to whichever attempt is current.
  uint32_t attemptId = 0;
  uint32_t gotIpAtAttemptStart = 0;
  // Association time of the accepted attempt, kept so the total "WiFi ready" figure can
  // be reported separately from it once the clock sync has also finished.
  unsigned long associationMs = 0;
  // How many times onComplete() has run. A duplicate has to be visible, not silent.
  uint8_t completionCount = 0;

  // Cancellation of a superseded attempt, in flight. Whatever replaces it -- the next
  // saved network, a scan, or the failure screen -- does not start until an STA
  // disconnect event has actually been observed (or the bound in wifi_connect_policy
  // expires), so two attempts never overlap.
  bool cancelPending = false;
  uint32_t disconnectsAtCancel = 0;
  unsigned long cancelStartMs = 0;
  // What to do once the in-flight cancellation resolves.
  enum class PendingOutcome { None, AutoConnectFailure, ConnectionFailed };
  PendingOutcome cancelOutcome = PendingOutcome::None;

  // Connection timeout
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;
  static constexpr unsigned long AUTO_CONNECTION_TIMEOUT_MS = 7000;

  // The UiAppHost app hosts the network list and the save/forget prompts
  // (themed rows and dialogs, touch routing); every other state keeps its
  // legacy centered-text rendering.
  // Viewport memory (top/visibleRows) for the network list; `selected` is
  // mirrored from selectedNetworkIndex at build/move time.
  freeink::ui::ListNav listNav;

  static void listScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onScanEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onPromptEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiScreen& screen);
  void buildPromptDialog(UiScreen& screen);

  void renderNetworkList(const Rect* screen, const ThemeMetrics* metrics);
  void renderPasswordEntry(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnected(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const;

  void startWifiScan(bool autoScan = false);
  void processWifiScanResults();
  void appendHiddenNetworkEntry();
  void selectNetwork(int index);
  void promptHiddenSsid();
  void promptPasswordEntry();
  void attemptConnection();
  // Puts the radio into a known state for a connect attempt. Everything slow or
  // display-bound belongs here, ahead of beginConnection()'s clock.
  void prepareRadio();
  // Issues a full-scan WiFi.begin() and starts the timeout clock.
  void beginConnection();
  void checkConnectionStatus();
  // Asks a superseded attempt to stop, records the event-counter baseline the
  // confirmation is measured against, and states what runs once it resolves.
  void requestAttemptCancellation(bool hardFail, PendingOutcome outcome);
  // Progress of an in-flight cancellation. The deferred outcome may run only on the two
  // Confirmed outcomes, each of which rests on an observed state rather than on elapsed
  // time; ConfirmedAfterRadioStop additionally requires the radio to be brought back up,
  // having been stopped and deinitialised.
  enum class CancelProgress { Waiting, Confirmed, ConfirmedAfterRadioStop, Unrecoverable };
  CancelProgress advanceCancellation();
  // Whether an observed WL_CONNECTED was produced by the attempt currently in flight,
  // rather than by an earlier one whose GOT_IP predates this attempt's start.
  bool completionBelongsToActiveAttempt();
  // Completes the activity once the SNTP sync started at association lands or times out.
  void finishWhenClockSynced();
  // Auto-connect helpers: try a saved credential, fall through the scan result for
  // the next untried saved network, or give up and drop to the manual list.
  bool hasAttemptedAutoSsid(const std::string& ssid) const;
  bool tryAutoConnectCredential(const WifiCredential& cred);
  bool tryNextSavedNetworkFromScan();
  void handleAutoConnectFailure();
  void showNetworkListFromAutoConnect();
  static std::string getSignalStrengthIndicator(int32_t rssi);

  void onComplete(bool connected);

 public:
  explicit WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoConnect = true);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
