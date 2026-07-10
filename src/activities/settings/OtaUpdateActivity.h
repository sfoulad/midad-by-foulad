#pragma once

#include "activities/Activity.h"
#include "network/OtaUpdater.h"

class OtaUpdateActivity : public Activity {
  enum State {
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };

  // Can't initialize this to 0 or the first render doesn't happen
  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  State state = WIFI_SELECTION;
  unsigned int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  OtaUpdater updater;
  // Captured on failure so the on-screen message can show what went wrong
  // (otherwise the error code is only in the USB serial log).
  int lastErrorCode = 0;
  uint32_t failureFreeHeap = 0;
  uint32_t failureMaxBlock = 0;
  // Only meaningful when the failure came from checkForUpdate() (the version-check
  // GET), which goes through HttpDownloader -- installUpdate() uses esp_https_ota_*
  // directly and doesn't set these. 0/0 (FailStage::NONE) otherwise.
  uint8_t failureHttpStage = 0;
  int failureHttpDetail = 0;
  // True only on the fresh boot landed on by silentRestartToOtaInstall(): the
  // user already confirmed this update before that reboot, so WAITING_CONFIRMATION
  // proceeds straight to install instead of waiting for another button press.
  bool autoInstall = false;

  void onWifiSelectionComplete(bool success);
  void beginInstall();

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoInstall = false)
      : Activity("OtaUpdate", renderer, mappedInput), updater(), autoInstall(autoInstall) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS; }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
