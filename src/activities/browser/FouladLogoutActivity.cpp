#include "FouladLogoutActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "FouladDeviceLogout.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void FouladLogoutActivity::onEnter() {
  Activity::onEnter();

  // Refused rather than degraded: without a network there is no way to tell the
  // server to drop this device, and clearing the credential anyway would leave the
  // unit listed under "My Devices" forever with no way back to it from here.
  if (WiFi.status() != WL_CONNECTED) {
    state = State::NoWifi;
    requestUpdate();
    return;
  }

  // Paint "Signing out..." and wait for it to land BEFORE the blocking calls start.
  // A plain requestUpdate() would only be serviced after this function returns, by
  // which time the work is already done and the user has stared at the previous
  // screen for the whole of it.
  state = State::Working;
  requestUpdateAndWait();

  const auto result = FouladDeviceLogout::removeThisDevice(username, password);
  if (result == FouladDeviceLogout::Result::Failed) {
    state = State::Failed;
    requestUpdate();
    return;
  }

  // Removed, or already absent from the account -- either way the desired end state
  // holds, so the caller may now clear the credential.
  LOG_INF("LOGOUT", "sign-out confirmed by server");
  ActivityResult res;
  res.isCancelled = false;
  setResult(std::move(res));
  finish();
}

void FouladLogoutActivity::loop() {
  // Only the two failure screens are interactive; the working screen owns the device
  // for the duration of the calls and has nothing to offer.
  if (state == State::Working) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    ActivityResult res;
    res.isCancelled = true;  // credential stays put
    setResult(std::move(res));
    finish();
  }
}

void FouladLogoutActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FOULAD_EBOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // Wrapped: these are full sentences and the two failure ones are wider than the
  // panel, which drawCenteredText would overrun and clip at both edges.
  const int textWidth = pageWidth - metrics.contentSidePadding * 2;

  switch (state) {
    case State::Working:
      renderer.drawCenteredTextWrapped(UI_12_FONT_ID, contentTop + 40, textWidth, tr(STR_LOGOUT_IN_PROGRESS),
                                       /*maxLines=*/2);
      break;
    case State::NoWifi:
      renderer.drawCenteredTextWrapped(UI_12_FONT_ID, contentTop + 40, textWidth, tr(STR_LOGOUT_NEEDS_WIFI),
                                       /*maxLines=*/5);
      break;
    case State::Failed:
      renderer.drawCenteredTextWrapped(UI_12_FONT_ID, contentTop + 40, textWidth, tr(STR_LOGOUT_FAILED),
                                       /*maxLines=*/5);
      break;
  }

  if (state != State::Working) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
