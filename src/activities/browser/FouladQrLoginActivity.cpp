#include "FouladQrLoginActivity.h"

#include <GfxRenderer.h>
#include <HeapTrace.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "FouladEbooksConfig.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void FouladQrLoginActivity::onEnter() {
  HEAP_TRACE_SCOPE("QRLOGIN.construct");
  Activity::onEnter();
  state = State::ConnectingWifi;
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

// Cancelling leaves WiFi initialised, and the user feels it immediately: the
// radio holds ~20KB, dropping the largest contiguous block below the 42,768
// bytes Home needs for its cover snapshot, so Home re-renders every cover on
// every paint -- measured at 1,478 ms per repaint against ~15 ms with the
// snapshot. The success path already escapes this by silent-restarting into the
// catalog; cancel had no equivalent and returned to a Home it had just made slow.
//
// Restart rather than WiFi.mode(WIFI_OFF): freeing the radio does not
// de-fragment what its allocations left behind (upstream issue #3343). Only a
// reboot restores the contiguous block.
void FouladQrLoginActivity::onExit() {
  Activity::onExit();
  if (result.isCancelled && WiFi.getMode() != WIFI_MODE_NULL) {
    LOG_DBG("QRLOGIN", "Cancelled with WiFi up; restarting to give Home a clean heap");
    silentRestart();  // does not return
  }
}

void FouladQrLoginActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    // QR sign-in is the only way in now, and it needs the network end to end --
    // start the session, then poll until the phone approves it. Without WiFi
    // there is nothing to fall back to, so say so on the failure screen rather
    // than closing silently and leaving the user wondering what happened.
    state = State::Failed;
    requestUpdate();
    return;
  }
  state = State::Starting;
  requestUpdate();
  beginSession();
}

void FouladQrLoginActivity::beginSession() {
  // TEMP diagnostic: everything the HTTPS session-start costs -- client init, CA
  // bundle attach, TLS handshake, response read, cleanup -- falls inside here.
  HEAP_TRACE_SCOPE("QRLOGIN.beginSession");
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

      // KOSync, when Core provisioned it in the approval. Written here rather than
      // inside the login module so that module stays a pure "get a credential"
      // step. Self-revoke kills both credentials at once server-side, so they are
      // deliberately established together.
      if (!poll.koPassword.empty() && !poll.koServerUrl.empty()) {
        KOREADER_STORE.setCredentials(poll.koUsername, poll.koPassword);
        KOREADER_STORE.setServerUrl(poll.koServerUrl);
        if (poll.koMatchMethod == 0 || poll.koMatchMethod == 1) {
          KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(poll.koMatchMethod));
        }
        KOREADER_STORE.saveToFile();
        LOG_INF("QRLOGIN", "KOSync configured for this account");
      }

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
    case FouladDeviceLogin::PollStatus::Redeemed:
      // Auto-restart rather than making the user press something: the code is
      // dead and a fresh one is one request away. beginSession() sets Failed if
      // that request fails, so this cannot spin. Redeemed lands here for the same
      // reason -- its one-time token is spent and will never be reissued, so
      // polling on would wait out the full window for an answer that cannot come.
      LOG_DBG("QRLOGIN", "Pairing session is spent, requesting a new one");
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

  if (mappedInput.hasTouch()) {
    const Rect backRect = touchBackButtonRect(renderer);
    if (mappedInput.wasTapInRect(backRect.x, backRect.y, backRect.width, backRect.height)) {
      setResult(ActivityResult{});
      result.isCancelled = true;
      finish();
      return;
    }
  }

  // No Confirm binding: the typed-password route is no longer offered from this
  // screen, and a live button with no footer label is worse than either choice --
  // Confirm is the natural "I've scanned it" press, and it would have silently
  // dropped the user into a username prompt instead. The route itself still
  // exists for the one case that would otherwise dead-end: cancelling out of WiFi
  // selection, where QR sign-in is impossible but a password can still be stored
  // (see onWifiSelectionComplete).

  if (state != State::ShowingQr) return;

  // The session's own deadline, enforced locally. The server expires a pairing
  // session after `expires_in` and answers "expired" from then on, but only if it
  // is reachable and only if it recognises the session at all -- a status this
  // build does not know is treated as Pending, so without a local clock a single
  // unexpected response shape means polling a dead code forever. `expires_in` is
  // the server's own figure, plus one poll interval of slack so the server's
  // "expired" (which auto-restarts with a fresh code) normally wins the race.
  const unsigned long intervalMs = static_cast<unsigned long>(session.pollIntervalSeconds) * 1000UL;
  const unsigned long deadlineMs = static_cast<unsigned long>(session.expiresInSeconds) * 1000UL + intervalMs;
  if (session.expiresInSeconds > 0 && millis() - sessionStartedMs >= deadlineMs) {
    LOG_DBG("QRLOGIN", "Pairing window elapsed locally, requesting a new code");
    beginSession();
    return;
  }

  // Poll on a millis() gate rather than blocking, so the two buttons above stay
  // responsive across the whole window.
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
  // Every message on this screen is a full sentence, and the two failure ones are wider
  // than the panel at UI_12. drawCenteredText would overrun and, because it centers,
  // clip them at *both* edges -- the middle of the sentence survives and the start and
  // end are lost. So they wrap instead. See GfxRenderer::drawCenteredTextWrapped.
  const int textWidth = pageWidth - metrics.contentSidePadding * 2;

  switch (state) {
    case State::ConnectingWifi:
    case State::Starting:
      renderer.drawCenteredTextWrapped(UI_12_FONT_ID, contentTop + 40, textWidth, tr(STR_QR_LOGIN_PREPARING),
                                       /*maxLines=*/3);
      break;

    case State::ShowingQr: {
      // Height is measured rather than assumed: the hint fits one line in English but
      // wraps in longer translations, and the QR has to start below wherever it ends.
      const int hintHeight =
          renderer.drawCenteredTextWrapped(UI_10_FONT_ID, contentTop, textWidth, tr(STR_QR_LOGIN_SCAN_HINT),
                                           /*maxLines=*/2);

      // Leave room under the QR for the pairing code and its caption.
      const int codeBlockHeight = renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(UI_10_FONT_ID) + 24;
      const int qrTop = contentTop + hintHeight + metrics.verticalSpacing;
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
      renderer.drawCenteredTextWrapped(UI_12_FONT_ID, contentTop + 40, textWidth, tr(STR_QR_LOGIN_DENIED),
                                       /*maxLines=*/4);
      break;

    case State::Failed:
      renderer.drawCenteredTextWrapped(UI_12_FONT_ID, contentTop + 40, textWidth, tr(STR_QR_LOGIN_FAILED),
                                       /*maxLines=*/4);
      break;
  }

  if (mappedInput.hasTouch()) {
    const Rect backRect = touchBackButtonRect(renderer);
    renderer.fillRect(backRect.x, backRect.y, backRect.width, backRect.height, false);
    renderer.drawRect(backRect.x, backRect.y, backRect.width, backRect.height, true);
    const int backTextWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_BACK));
    const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, backRect.x + (backRect.width - backTextWidth) / 2,
                      backRect.y + (backRect.height - textHeight) / 2, tr(STR_BACK));
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

Rect FouladQrLoginActivity::touchBackButtonRect(const GfxRenderer& renderer) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int buttonWidth = 100;
  constexpr int buttonHeight = 36;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  return Rect{(pageWidth - buttonWidth) / 2, pageHeight - metrics.buttonHintsHeight, buttonWidth, buttonHeight};
}
