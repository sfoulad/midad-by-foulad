#pragma once

#include <string>

#include "FouladDeviceLogin.h"
#include "activities/Activity.h"

/**
 * Sign in to Foulad eBooks by scanning a QR code with the Foulad One phone app.
 *
 * Opens a pairing session (FouladDeviceLogin::start), renders the returned URL
 * as a QR code with the pairing code printed underneath, and polls until the
 * user approves it. On approval the issued per-device token is stored as the
 * OPDS password and the device silently restarts into the catalog -- the same
 * fresh-heap handoff the typed-password path uses (see
 * SilentRestart.h).
 *
 * Requires WiFi, which the typed-password path does not: credentials there are
 * just stored for later, whereas this must talk to the server before it has any
 * credential at all. So this connects first, then starts the session.
 *
 * Confirm exits to manual username/password entry -- the documented fallback for
 * users without the phone app, and the recovery path if this flow is unavailable
 * (EINK_QR_LOGIN_TASKS.md, PART 4). That is reported to the parent as
 * MenuResult::action == ACTION_MANUAL_LOGIN rather than handled here, so the
 * existing typed-password code stays entirely untouched and stays easy to delete
 * when sign-in eventually becomes QR-only.
 */
class FouladQrLoginActivity final : public Activity {
 public:
  // Reported via MenuResult::action when the user asks for manual entry.

  explicit FouladQrLoginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FouladQrLogin", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t {
    ConnectingWifi,  // WifiSelectionActivity is up
    Starting,        // opening a pairing session
    ShowingQr,       // QR on screen, polling
    Denied,          // user rejected it in the app
    Failed,          // couldn't open a session (offline / server unreachable)
  };

  State state = State::ConnectingWifi;
  FouladDeviceLogin::StartResult session;
  unsigned long lastPollMs = 0UL;
  unsigned long sessionStartedMs = 0UL;
  // Consecutive transport failures. The session stays valid across these, so
  // they're retried silently rather than thrown at the user on the first blip.
  uint8_t consecutivePollErrors = 0;
  static constexpr uint8_t MAX_POLL_ERRORS = 5;

  void onWifiSelectionComplete(bool success);
  void beginSession();
  void pollOnce();
};
