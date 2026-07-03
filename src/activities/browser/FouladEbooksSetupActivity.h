#pragma once

#include "activities/Activity.h"

/**
 * First-run credential collection for the built-in "Foulad eBooks" home menu entry.
 * Prompts for username then password (catalog URL is fixed, see FouladEbooksConfig.h),
 * saves the result into OpdsServerStore, then hands off to OpdsBookBrowserActivity.
 * Modeled on KOReaderAuthActivity, which also launches a child activity immediately
 * from onEnter() rather than waiting for input on its own screen.
 */
class FouladEbooksSetupActivity final : public Activity {
 public:
  explicit FouladEbooksSetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FouladEbooksSetup", renderer, mappedInput) {}

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  std::string username;

  void launchUsernameEntry();
  void launchPasswordEntry();
};
