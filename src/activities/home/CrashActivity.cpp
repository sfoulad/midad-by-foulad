#include "CrashActivity.h"

#include <GfxRenderer.h>
#include <HalSystem.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "OpdsServerStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void CrashActivity::onEnter() {
  Activity::onEnter();

  panicMessage = HalSystem::getPanicInfo(false);
  if (panicMessage.empty()) {
    panicMessage = tr(STR_CRASH_NO_REASON);
  }
  HalSystem::clearPanic();

  requestUpdateAndWait();
}

void CrashActivity::loop() {
  // WifiSelectionActivity owns the screen/input while it's on top.
  if (sendState == SendState::Connecting) return;

  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // wasPressed, not wasReleased. Reported as needing two presses to take: the
  // release edge is cleared by every gpio.update(), including the ones the render
  // wait performs mid-frame, so a release landing during a repaint is simply lost.
  // The press edge does not have that problem, and it is what every other
  // confirm-and-go screen here uses (OtaUpdateActivity, MidadSyncActivity) -- this
  // was the odd one out.
  if ((sendState == SendState::Idle || sendState == SendState::Failed) &&
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    startSendLog();
  }
}

void CrashActivity::startSendLog() {
  const auto& servers = OPDS_STORE.getServers();
  const auto it =
      std::find_if(servers.begin(), servers.end(), [](const OpdsServer& s) { return s.url == FOULAD_EBOOKS_URL; });
  if (it == servers.end()) {
    sendState = SendState::NoAccount;
    requestUpdate();
    return;
  }
  sendUsername = it->username;
  sendPassword = it->password;

  if (WiFi.status() == WL_CONNECTED) {
    performSend();
    return;
  }

  sendState = SendState::Connecting;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void CrashActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    sendState = SendState::Failed;
    requestUpdate();
    return;
  }
  performSend();
}

void CrashActivity::performSend() {
  sendState = SendState::Sending;
  requestUpdate(true);  // force an immediate redraw before the blocking network call below

  const bool ok = FouladDeviceTracking::uploadCrashReport(sendUsername, sendPassword);
  sendState = ok ? SendState::Sent : SendState::Failed;
  requestUpdate();
}

void CrashActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CRASH_TITLE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto descLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_CRASH_DESCRIPTION), contentWidth, 10);
  for (const auto& line : descLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_CRASH_REASON));
  y += lineHeight + metrics.verticalSpacing;

  auto panicLines = renderer.wrappedText(UI_10_FONT_ID, panicMessage.c_str(), contentWidth, 5);
  for (const auto& line : panicLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  if (sendState != SendState::Idle) {
    y += metrics.verticalSpacing * 2;
    const char* statusText = "";
    switch (sendState) {
      case SendState::Connecting:
      case SendState::Sending:
        statusText = tr(STR_CRASH_SENDING);
        break;
      case SendState::Sent:
        statusText = tr(STR_CRASH_SEND_SUCCESS);
        break;
      case SendState::Failed:
        statusText = tr(STR_CRASH_SEND_FAILED);
        break;
      case SendState::NoAccount:
        statusText = tr(STR_CRASH_SEND_NO_ACCOUNT);
        break;
      case SendState::Idle:
        break;
    }
    auto statusLines = renderer.wrappedText(UI_10_FONT_ID, statusText, contentWidth, 3);
    for (const auto& line : statusLines) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
      y += lineHeight;
    }
  }

  const bool canSend = sendState == SendState::Idle || sendState == SendState::Failed;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canSend ? tr(STR_CRASH_SEND_LOG) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
