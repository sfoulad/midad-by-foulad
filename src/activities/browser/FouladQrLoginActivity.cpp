#include "FouladQrLoginActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "FouladEbooksConfig.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void FouladQrLoginActivity::onEnter() {
  Activity::onEnter();
  state = State::ConnectingWifi;
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FouladQrLoginActivity::onExit() { Activity::onExit(); }

void FouladQrLoginActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    // No WiFi means no QR sign-in at all, but typing a password still works
    // offline (it's only stored, not verified), so send the user there rather
    // than dead-ending them.
    finishWithManualRequest();
    return;
  }
  state = State::Starting;
  requestUpdate();
  beginSession();
}

void FouladQrLoginActivity::beginSession() {
  session = FouladDeviceLogin::start();
  if (!session.ok) {
    state = State::Failed;
    requestUpdate();
    return;
  }
  sessionStartedMs = millis();
  // Stagger the first poll by one interval: the user cannot possibly have
  // scanned and approved in the time it took to draw the screen, and the poll
  // endpoint is rate limited.
  lastPollMs = millis();
  consecutivePollErrors = 0;
  state = State::ShowingQr;
  requestUpdate();
}

void FouladQrLoginActivity::finishWithManualRequest() {
  MenuResult menu;
  menu.action = ACTION_MANUAL_LOGIN;
  setResult(ActivityResult{std::move(menu)});
  finish();
}

void FouladQrLoginActivity::pollOnce() {
  const auto poll = FouladDeviceLogin::poll(session.sessionToken);

  switch (poll.status) {
    case FouladDeviceLogin::PollStatus::Pending:
      consecutivePollErrors = 0;
      return;

    case FouladDeviceLogin::PollStatus::Approved: {
      OpdsServer server;
      server.name = FOULAD_EBOOKS_NAME;
      server.url = FOULAD_EBOOKS_URL;
      server.username = poll.username;
      // The device token goes in the password field: every existing OPDS call
      // (feed, covers, downloads, FouladDeviceTracking's endpoints) keeps using
      // HTTP Basic Auth exactly as before and needs no knowledge of this flow.
      server.password = poll.token;
      // Flagged so a later migration can tell an issued token from a typed
      // account password without guessing by length -- see the deferred
      // "migrate existing password logins" task. Retrofitting this after the
      // fact would leave every pre-existing install unclassifiable.
      server.isDeviceToken = true;
      OPDS_STORE.addServer(server);  // persists to SD before the restart below

      LOG_INF("QRLOGIN", "Signed in as '%s'", poll.username.c_str());
      // Same fresh-heap handoff the typed-password path uses: boot routes into
      // goToFouladEbooks(), which finds the account just stored.
      silentRestartToFouladEbooks();
      return;
    }

    case FouladDeviceLogin::PollStatus::Denied:
      state = State::Denied;
      requestUpdate();
      return;

    case FouladDeviceLogin::PollStatus::Expired:
      // Auto-restart rather than making the user press something: the code is
      // dead and a fresh one is one request away. beginSession() sets Failed if
      // that request fails, so this cannot spin.
      LOG_DBG("QRLOGIN", "Pairing code expired, requesting a new one");
      beginSession();
      return;

    case FouladDeviceLogin::PollStatus::NetworkError:
      // The session survives a transport blip, so keep the QR up and retry on
      // the next tick. Only give up after several in a row.
      if (++consecutivePollErrors >= MAX_POLL_ERRORS) {
        LOG_ERR("QRLOGIN", "Giving up after %u consecutive poll failures", consecutivePollErrors);
        state = State::Failed;
        requestUpdate();
      }
      return;
  }
}

void FouladQrLoginActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    setResult(ActivityResult{});
    result.isCancelled = true;
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finishWithManualRequest();
    return;
  }

  if (state != State::ShowingQr) return;

  // Poll on a millis() gate rather than blocking, so the two buttons above stay
  // responsive across the whole five-minute window.
  const unsigned long intervalMs = static_cast<unsigned long>(session.pollIntervalSeconds) * 1000UL;
  if (millis() - lastPollMs < intervalMs) return;
  lastPollMs = millis();
  pollOnce();
}

void FouladQrLoginActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FOULAD_EBOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  switch (state) {
    case State::ConnectingWifi:
    case State::Starting:
      renderer.drawCenteredText(UI_12_FONT_ID, contentTop + 40, tr(STR_QR_LOGIN_PREPARING), true);
      break;

    case State::ShowingQr: {
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop, tr(STR_QR_LOGIN_SCAN_HINT), true);

      // Leave room under the QR for the pairing code and its caption.
      const int codeBlockHeight = renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(UI_10_FONT_ID) + 24;
      const int qrTop = contentTop + renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
      const int qrHeight = pageHeight - qrTop - codeBlockHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
      const int qrWidth = pageWidth - 40;
      if (qrHeight > 40) {
        QrUtils::drawQrCode(renderer, Rect{20, qrTop, qrWidth, qrHeight}, session.qrPayload);
      }

      // The pairing code is the fallback when a phone camera won't focus on an
      // e-ink panel, so it is drawn large and readable rather than as a footnote.
      const int codeY = qrTop + std::max(qrHeight, 0) + 8;
      renderer.drawCenteredText(UI_10_FONT_ID, codeY, tr(STR_QR_LOGIN_OR_ENTER_CODE), true);
      renderer.drawCenteredText(UI_12_FONT_ID, codeY + renderer.getLineHeight(UI_10_FONT_ID) + 4,
                                session.pairingCode.c_str(), true, EpdFontFamily::BOLD);
      // Deliberately no live countdown. E-ink cannot repaint fast enough for a
      // ticking second to look like anything but a stutter (the same reason the
      // Stop Watch shows centiseconds only when frozen), and an expired code
      // silently replaces itself anyway.
      break;
    }

    case State::Denied:
      renderer.drawCenteredText(UI_12_FONT_ID, contentTop + 40, tr(STR_QR_LOGIN_DENIED), true);
      break;

    case State::Failed:
      renderer.drawCenteredText(UI_12_FONT_ID, contentTop + 40, tr(STR_QR_LOGIN_FAILED), true);
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", tr(STR_QR_LOGIN_MANUAL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
