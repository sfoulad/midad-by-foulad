#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

/**
 * Full-screen trap entered when main.cpp's OTA routing detects an
 * unacknowledged bootloader rollback to a signature-unaware old firmware
 * image (see OtaRollbackRecoveryPlan.h). Blocks only the normal OTA-update
 * entry point -- Home, Reader, Settings, networking, and BLE all remain
 * reachable normally, since this activity is never entered on those paths.
 *
 * The user picks one of two options, presented via the standard OptionPopup:
 *  - Recover via SD card: replaces this activity with SdFirmwareUpdateActivity
 *    in recovery mode, the same flash path used by the physical UP+POWER
 *    recovery boot.
 *  - Continue anyway: records this specific image's digest as acknowledged
 *    (see CrossPointState::acknowledgedOtaRollbackDigestHex) and goes Home.
 *    A future genuine rollback carries a different digest and is never
 *    silently treated as already-acknowledged.
 *
 * Entered via activityManager.replaceActivity() with nothing on the stack
 * beneath it, so there is no "Back" destination -- dismissing the popup
 * without a selection re-traps the user in the same choice.
 */
class OtaRollbackRecoveryActivity final : public Activity {
 public:
  OtaRollbackRecoveryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string invalidImageDigestHex)
      : Activity("OtaRollbackRecovery", renderer, mappedInput),
        invalidImageDigestHex(std::move(invalidImageDigestHex)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "OtaRollbackRecoveryActivity"; }

 private:
  std::string invalidImageDigestHex;
  OptionPopup optionPopup;

  void showOptions();
  void onOptionSelected(int idx);
};
