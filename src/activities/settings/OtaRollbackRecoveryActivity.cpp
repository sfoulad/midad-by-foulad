#include "OtaRollbackRecoveryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void OtaRollbackRecoveryActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("OTA", "Rollback recovery: unacknowledged invalid image digest=%.8s...", invalidImageDigestHex.c_str());
  showOptions();
  requestUpdate();
}

void OtaRollbackRecoveryActivity::showOptions() {
  const char* options[] = {tr(STR_OTA_ROLLBACK_RECOVER_SD), tr(STR_OTA_ROLLBACK_CONTINUE)};
  optionPopup.show(tr(STR_OTA_ROLLBACK_TITLE), options, 2, 0, [this](const int idx) { onOptionSelected(idx); });
}

void OtaRollbackRecoveryActivity::onOptionSelected(const int idx) {
  if (idx == 0) {
    LOG_INF("OTA", "Rollback recovery: user chose SD card recovery");
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput, /*recoveryMode=*/true));
    return;
  }

  LOG_INF("OTA", "Rollback recovery: user acknowledged and continues on current firmware");
  APP_STATE.acknowledgedOtaRollbackDigestHex = invalidImageDigestHex;
  APP_STATE.saveToFile();
  activityManager.goHome();
}

void OtaRollbackRecoveryActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  // Dismissed without a selection (Back button or tap outside). There is no
  // "Back" destination -- re-trap the user in the same choice.
  showOptions();
  requestUpdate();
}

void OtaRollbackRecoveryActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OTA_ROLLBACK_TITLE));

  const int msgTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const Rect msgBounds{metrics.contentSidePadding, msgTop, pageWidth - metrics.contentSidePadding * 2,
                       pageHeight - msgTop};
  UITheme::drawCenteredWrappedText(renderer, msgBounds, UI_10_FONT_ID, tr(STR_OTA_ROLLBACK_MESSAGE), 8, true,
                                   EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);

  if (optionPopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}
