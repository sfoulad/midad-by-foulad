#include "FouladEbooksSetupActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "FouladEbooksConfig.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "OpdsServerStore.h"
#include "activities/ActivityManager.h"
#include "activities/browser/FouladQrLoginActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void FouladEbooksSetupActivity::onEnter() {
  Activity::onEnter();
  launchQrLogin();
}

// QR is the primary path; typed credentials are the documented fallback for
// users without the phone app and the recovery path when the sign-in endpoints
// are unreachable (EINK_QR_LOGIN_TASKS.md, PART 4). Kept as a separate branch
// into the original, unmodified entry flow rather than merged into the QR
// screen, so that when sign-in eventually becomes QR-only this is a small
// deletion instead of an unpicking job.
void FouladEbooksSetupActivity::launchQrLogin() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) {
      finish();
      return;
    }
    const auto* menu = std::get_if<MenuResult>(&result.data);
    if (menu && menu->action == FouladQrLoginActivity::ACTION_MANUAL_LOGIN) {
      launchUsernameEntry();
      return;
    }
    // A successful QR sign-in never returns here -- it stores the credential and
    // silently restarts into the catalog. Anything else reaching this point is
    // unexpected, so fall back to typed entry rather than stranding the user on
    // a blank screen.
    launchUsernameEntry();
  };
  startActivityForResult(std::make_unique<FouladQrLoginActivity>(renderer, mappedInput), handler);
}

void FouladEbooksSetupActivity::launchUsernameEntry() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) {
      finish();
      return;
    }
    username = std::get<KeyboardResult>(result.data).text;
    launchPasswordEntry();
  };
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME), "", 63, InputType::Text,
                                              tr(STR_FOULAD_EBOOKS_LOGIN_HINT), /*numericOnly=*/true),
      handler);
}

void FouladEbooksSetupActivity::launchPasswordEntry() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) {
      finish();
      return;
    }

    OpdsServer server;
    server.name = FOULAD_EBOOKS_NAME;
    server.url = FOULAD_EBOOKS_URL;
    server.username = username;
    server.password = std::get<KeyboardResult>(result.data).text;
    // Typed by the user, so it is their real account password, not an issued
    // device token. Recorded explicitly -- see OpdsServer::isDeviceToken.
    server.isDeviceToken = false;
    OPDS_STORE.addServer(server);  // persists to SD before the reboot below

    // Reboot into the catalog on a fresh heap instead of opening it in this
    // session -- same OOM protection as the Home eBooks entry (see
    // SilentRestart.h). Boot routes into goToFouladEbooks(), which finds the
    // account just stored and opens the browser directly.
    silentRestartToFouladEbooks();
  };
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD), "", 63, InputType::Password,
                                              tr(STR_FOULAD_EBOOKS_LOGIN_HINT), /*numericOnly=*/true),
      handler);
}

void FouladEbooksSetupActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FOULAD_EBOOKS));

  renderer.displayBuffer();
}
