#include "FouladEbooksSetupActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "FouladEbooksConfig.h"
#include "MappedInputManager.h"
#include "OpdsBookBrowserActivity.h"
#include "OpdsServerStore.h"
#include "activities/ActivityManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void FouladEbooksSetupActivity::onEnter() {
  Activity::onEnter();
  launchUsernameEntry();
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
    OPDS_STORE.addServer(server);

    activityManager.replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, server));
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
