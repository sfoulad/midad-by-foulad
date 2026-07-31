#include "MidadSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointState.h"
#include "FouladEbooksConfig.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void MidadSyncActivity::onEnter() {
  Activity::onEnter();

  if (fouladBookId.empty()) {
    // Answer this without bringing the radio up: no id means nothing to sync
    // against, and making someone sit through a WiFi connect to be told so would
    // be worse than the silent row it replaces.
    state = State::NotInLibrary;
    requestUpdate();
    return;
  }

  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void MidadSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    // Backing out of WiFi selection is a decision, not a failure -- go straight
    // back to the book rather than showing an error for something deliberate.
    returnToReader();
    return;
  }
  {
    RenderLock lock(*this);
    state = State::Syncing;
  }
  requestUpdateAndWait();
  performSync();
}

void MidadSyncActivity::performSync() {
  const auto& servers = OPDS_STORE.getServers();
  const auto it =
      std::find_if(servers.begin(), servers.end(), [](const OpdsServer& s) { return s.url == FOULAD_EBOOKS_URL; });
  if (it == servers.end()) {
    RenderLock lock(*this);
    state = State::Failed;
    requestUpdate();
    return;
  }

  FouladReadingPosition::Position fetched;
  const bool ok = FouladReadingPosition::sync(it->username, it->password, fouladBookId, progressPercent, page,
                                              totalPages, /*readAtEpochSeconds=*/0, readAtAgeSeconds, fetched);

  RenderLock lock(*this);
  if (!ok) {
    state = State::Failed;
  } else {
    remote = fetched;
    // should_jump is the server's decision and carries its own slack allowance, so
    // it never proposes a jump to where you already are. Deliberately not
    // second-guessed here by comparing percentages: two renderers computing a
    // position from the same place never agree to the decimal.
    state = remote.shouldJump ? State::Prompt : State::UpToDate;
  }
  requestUpdate();
}

void MidadSyncActivity::acceptJump() {
  // Handed to the reader rather than applied here: resolving a percentage to a
  // spine and page needs the Epub, which was released before this activity started
  // so the handshake could fit in RAM. Rounded to whole percent because that is
  // what the handoff carries and what jumpToPercent() takes.
  APP_STATE.pendingSyncJumpPercent = static_cast<uint8_t>(remote.progressPercent + 0.5f);
  APP_STATE.saveToFile();
  LOG_INF("SYNC", "Accepted jump to %u%% (from %s)", APP_STATE.pendingSyncJumpPercent,
          remote.deviceName.empty() ? "another device" : remote.deviceName.c_str());
  returnToReader();
}

void MidadSyncActivity::returnToReader() {
  // Silent restart rather than a plain activity swap: the reader is about to reload
  // a book into a heap that has just held a TLS session, which is the fragmentation
  // KOReaderSyncActivity reboots for as well.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  silentRestartToReader();
}

void MidadSyncActivity::loop() {
  if (state == State::Prompt) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (promptSelection > 0) {
        promptSelection--;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (promptSelection < 1) {
        promptSelection++;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (promptSelection == 0) {
        acceptJump();
      } else {
        returnToReader();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToReader();  // declining is the same as staying put
    }
    return;
  }

  if (state == State::UpToDate || state == State::Failed || state == State::NotInLibrary) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToReader();
    }
  }
}

void MidadSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textWidth = pageWidth - metrics.contentSidePadding * 2;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYNC_MIDAD));

  const int top = (pageHeight - lineHeight) / 2;

  switch (state) {
    case State::Connecting:
    case State::Syncing:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SYNCING));
      break;

    case State::UpToDate:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SYNC_UP_TO_DATE), true, EpdFontFamily::BOLD);
      break;

    case State::Failed:
      renderer.drawCenteredTextWrapped(UI_10_FONT_ID, top, textWidth, tr(STR_SYNC_FAILED), /*maxLines=*/3, true,
                                       EpdFontFamily::BOLD);
      break;

    case State::NotInLibrary:
      renderer.drawCenteredTextWrapped(UI_10_FONT_ID, top, textWidth, tr(STR_SYNC_NOT_IN_LIBRARY), /*maxLines=*/4, true,
                                       EpdFontFamily::BOLD);
      break;

    case State::Prompt: {
      // Naming the other device is far more convincing than "another device", which
      // is why the server sends device_name at all.
      char msg[128];
      snprintf(msg, sizeof(msg), tr(STR_SYNC_CONTINUE_FROM), static_cast<int>(remote.progressPercent + 0.5f),
               remote.deviceName.empty() ? tr(STR_SYNC_ANOTHER_DEVICE) : remote.deviceName.c_str());
      const int msgHeight =
          renderer.drawCenteredTextWrapped(UI_10_FONT_ID, top - lineHeight, textWidth, msg, /*maxLines=*/3);

      const int buttonY = top - lineHeight + msgHeight + metrics.verticalSpacing * 2;
      constexpr int buttonWidth = 120;
      constexpr int buttonSpacing = 24;
      const int startX = (pageWidth - (buttonWidth * 2 + buttonSpacing)) / 2;
      const auto drawChoice = [&](const int slot, const char* label) {
        const int x = startX + slot * (buttonWidth + buttonSpacing);
        if (promptSelection == slot) {
          const std::string framed = "[" + std::string(label) + "]";
          renderer.drawText(UI_10_FONT_ID, x, buttonY, framed.c_str());
        } else {
          renderer.drawText(UI_10_FONT_ID, x + 4, buttonY, label);
        }
      };
      drawChoice(0, tr(STR_SYNC_JUMP));
      drawChoice(1, tr(STR_SYNC_STAY_HERE));
      break;
    }
  }

  const auto labels = state == State::Prompt
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                          : mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
