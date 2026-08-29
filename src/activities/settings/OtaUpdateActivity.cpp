#include "OtaUpdateActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/OtaUpdater.h"
#include "reading/ReadingStatsStore.h"
#include "util/FirmwareDiagLog.h"

// Compile-time guarantee that no update screen can ship without an operable
// control on either input surface (the host tests cover the same table
// case-by-case with the reasons).
namespace {
constexpr bool allScreensOperable() {
  for (uint8_t i = 0; i < ota_screen::SCREEN_COUNT; i++) {
    const auto actions = ota_screen::actionsFor(static_cast<ota_screen::Screen>(i));
    if (!ota_screen::operable(actions) || !ota_screen::touchOperable(actions)) return false;
  }
  return true;
}
static_assert(allScreensOperable(), "every firmware-update screen must expose a valid action on buttons and touch");
}  // namespace

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    FirmwareDiagLog::append("OTA", "aborted: WiFi connection failed");
    finish();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  const auto res = updater.checkForUpdate();
  // NO_UPDATE here means the release carries no firmware asset for this board
  // (expected until per-board assets are published) — not a failure.
  if (res == OtaUpdater::NO_UPDATE) {
    LOG_DBG("OTA", "No firmware asset for this board in latest release");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    requestUpdate();
    return;
  }
  if (res != OtaUpdater::OK) {
    const auto httpFailure = HttpDownloader::getLastFailure();
    LOG_DBG("OTA", "Update check failed: %d (free heap %u, largest block %u, http stage %u detail %d)", res,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), static_cast<unsigned>(httpFailure.stage), httpFailure.detail);
    FirmwareDiagLog::append("OTA", "check failed: res=" + std::to_string(res) +
                                       " free=" + std::to_string(ESP.getFreeHeap()) +
                                       " largest=" + std::to_string(ESP.getMaxAllocHeap()) +
                                       " httpStage=" + std::to_string(static_cast<unsigned>(httpFailure.stage)) +
                                       " httpDetail=" + std::to_string(httpFailure.detail));
    {
      RenderLock lock(*this);
      lastErrorCode = res;
      failureFreeHeap = ESP.getFreeHeap();
      failureMaxBlock = ESP.getMaxAllocHeap();
      failureHttpStage = static_cast<uint8_t>(httpFailure.stage);
      failureHttpDetail = httpFailure.detail;
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    // autoInstall (post silentRestartToOtaInstall boot): the user already
    // confirmed this update, so skip the WAITING_CONFIRMATION version page --
    // rendering it for one frame between the reboot and the install looked
    // like the updater bouncing back and forth. Show the progress screen
    // immediately; loop() starts the install from there.
    state = autoInstall ? UPDATE_IN_PROGRESS : WAITING_CONFIRMATION;
  }
  const char* options[] = {tr(STR_CANCEL), tr(STR_UPDATE)};
  // Default the selection to Update so the hardware Confirm button installs,
  // matching the pre-popup layout (Back = cancel, Confirm = update).
  confirmPopup.show(tr(STR_NEW_UPDATE), options, 2, 1, [this](const int idx) {
    if (idx == 1) {
      // Reboot into a fresh heap before the (memory-hungry) firmware download
      // rather than attempting it in this already-fragmented session -- see
      // silentRestartToOtaInstall(). Does not return.
      LOG_DBG("OTA", "Update confirmed, rebooting into auto-install...");
      silentRestartToOtaInstall();
    } else {
      finish();
    }
  });
  requestUpdate();
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // Free the two biggest discretionary DRAM consumers -- the reading-stats
  // store's in-RAM vectors (tens of KB after heavy reading) and the font glyph
  // cache -- as early as possible, before WiFi/TLS come up at all. A device
  // report confirmed this needs to happen here, not only right before
  // installUpdate(): reaching this screen normally means the user was just on
  // Home (which loads the stats store for the hero card) or browsing a library
  // (which fills the glyph cache with long/Arabic titles), so that memory is
  // already resident and fragmenting the heap for the version-CHECK request
  // too, not only the firmware download -- confirmed by a "code 2" (HTTP_ERROR)
  // failure on the check step with the fix only applied pre-install. Both
  // reload on demand afterward (stats from SD, glyphs from flash).
  READING_STATS.releaseMemory();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  LOG_DBG("OTA", "Pre-check heap: free %u, largest block %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Success path reboots via the SHUTTING_DOWN state's plain ESP.restart()
  // (loop() above) so the new firmware boots normally. Back-out paths land
  // here with wifi still active; silent-restart to free the LWIP/mbedTLS
  // fragmentation, same as the other wifi activities.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update progress: %d / %d", updater.getProcessedSize(), updater.getTotalSize());
    updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(updater.getTotalSize());
    // Only update every 2% at the most
    if (static_cast<int>(updaterProgress * 50) == lastUpdaterPercentage / 2) {
      return;
    }
    lastUpdaterPercentage = static_cast<int>(updaterProgress * 100);
  }

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    // Version info sits in the upper part of the screen so the centered
    // Cancel/Update popup doesn't cover it (same layout as ConfirmationActivity).
    const int infoTop = pageHeight / 6;
    // drawTextInWidth right-aligns Arabic strings within the row, so these two lines
    // anchor to the right edge under the Arabic UI language (proper RTL) while staying
    // left-anchored for English and other LTR languages.
    renderer.drawTextInWidth(UI_10_FONT_ID, metrics.contentSidePadding, infoTop,
                             pageWidth - metrics.contentSidePadding * 2,
                             (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    // GitHub tag names are conventionally "v1.6.24"; strip the leading 'v'/'V' so this
    // reads consistently next to "Current version: 1.6.24" above instead of looking like
    // two different version formats for what may be the same release.
    std::string latestVersionDisplay = updater.getLatestVersion();
    if (!latestVersionDisplay.empty() && (latestVersionDisplay[0] == 'v' || latestVersionDisplay[0] == 'V')) {
      latestVersionDisplay.erase(0, 1);
    }
    renderer.drawTextInWidth(UI_10_FONT_ID, metrics.contentSidePadding, infoTop + height + metrics.verticalSpacing,
                             pageWidth - metrics.contentSidePadding * 2,
                             (std::string(tr(STR_NEW_VERSION)) + latestVersionDisplay).c_str());

    if (confirmPopup.processRender(renderer, mappedInput)) return;
  } else if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING));

    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(updaterProgress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the bytes line below stays at the same Y it was at when the activity drew its own percent.
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(
        UI_10_FONT_ID, y,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
  } else if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    // A 403 on the version check is GitHub's rate limit, essentially always: the API
    // allows 60 unauthenticated requests an hour per public IP, shared with every
    // other device and tool behind it. Worth naming, because it is the one failure
    // here that is neither the device's fault nor fixed by retrying immediately --
    // "code 2 heap 79608" sent a user hunting a memory bug that wasn't there. The
    // conditional request in OtaUpdater::checkForUpdate should make reaching this
    // rare; it cannot help once the budget is already spent.
    const bool rateLimited =
        failureHttpStage == static_cast<uint8_t>(HttpDownloader::FailStage::STATUS) && failureHttpDetail == 403;

    int diagY = top + height + metrics.verticalSpacing;
    if (failedDetail != nullptr) {
      renderer.drawCenteredText(UI_10_FONT_ID, diagY, failedDetail);
      diagY += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
    }
    if (rateLimited) {
      // Wrapped, not single-line: it's a full sentence and overruns the panel at this
      // width, and drawCenteredText would clip it at both ends (same reasoning as
      // FouladQrLoginActivity's messages). Push the codes below however tall it lands.
      diagY += renderer.drawCenteredTextWrapped(UI_10_FONT_ID, diagY, pageWidth - metrics.contentSidePadding * 2,
                                                tr(STR_UPDATE_RATE_LIMITED), /*maxLines=*/3) +
               metrics.verticalSpacing;
    }

    // Diagnostic lines: error code + heap at failure, plus (when the failure came
    // from the HTTP version-check) which network stage failed and its detail code.
    // Not prose, so not translated; lets a failure be diagnosed without a USB
    // serial capture. Kept even when the friendly message above explains the cause --
    // it is what gets read back over chat when a report comes in.
    char diag[64];
    snprintf(diag, sizeof(diag), "code %d  heap %u  block %u", lastErrorCode, static_cast<unsigned>(failureFreeHeap),
             static_cast<unsigned>(failureMaxBlock));
    renderer.drawCenteredText(SMALL_FONT_ID, diagY, diag);
    if (failureHttpStage != 0) {
      char httpDiag[32];
      snprintf(httpDiag, sizeof(httpDiag), "http %u:%d", failureHttpStage, failureHttpDetail);
      renderer.drawCenteredText(SMALL_FONT_ID, diagY + renderer.getLineHeight(SMALL_FONT_ID) + 2, httpDiag);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, tr(STR_POWER_ON_HINT));
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::beginInstall() {
  LOG_DBG("OTA", "Starting download...");
  {
    RenderLock lock(*this);
    state = UPDATE_IN_PROGRESS;
  }
  requestUpdateAndWait();

  // Free heap again right before the firmware-download TLS handshake, on top
  // of the onEnter() clear: esp_https_ota_begin() needs a large *contiguous*
  // block for the mbedTLS session to GitHub's CDN, and the "Checking..." /
  // "Update available?" screens drawn since onEnter() will have refilled the
  // glyph cache somewhat (button hints, version strings). On an autoInstall
  // (post silentRestartToOtaInstall) boot this is close to a no-op -- nothing
  // has grown these yet -- but costs nothing to run unconditionally.
  READING_STATS.releaseMemory();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  LOG_DBG("OTA", "Pre-OTA heap: free %u, largest block %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  const auto res = updater.installUpdate(
      [](void* ctx) {
        // immediate=true notifies the render task directly. The default deferred path only
        // sets a flag consumed at the end of ActivityManager::loop(), which never runs while
        // installUpdate() blocks this task.
        static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
      },
      this);

  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update failed: %d (free heap %u, largest block %u)", res, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    FirmwareDiagLog::append("OTA", "install failed: res=" + std::to_string(res) +
                                       " free=" + std::to_string(ESP.getFreeHeap()) +
                                       " largest=" + std::to_string(ESP.getMaxAllocHeap()));
    {
      RenderLock lock(*this);
      lastErrorCode = res;
      failureFreeHeap = ESP.getFreeHeap();
      failureMaxBlock = ESP.getMaxAllocHeap();
      failedDetail = res == OtaUpdater::WRONG_DEVICE_ERROR ? tr(STR_FIRMWARE_WRONG_DEVICE) : nullptr;
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  FirmwareDiagLog::append("OTA", "install succeeded, rebooting");
  {
    RenderLock lock(*this);
    state = FINISHED;
  }
  requestUpdateAndWait();
  // Hold the completion screen briefly so the user sees it, then restart.
  delay(3000);
  {
    RenderLock lock(*this);
    state = SHUTTING_DOWN;
  }
}

void OtaUpdateActivity::loop() {
  // autoInstall means the user already confirmed this exact update before
  // silentRestartToOtaInstall() rebooted here -- start the install as soon as
  // the check has succeeded (state already shows the progress screen, see
  // onWifiSelectionComplete). One-shot: beginInstall blocks until done/failed.
  if (state == UPDATE_IN_PROGRESS && autoInstall) {
    autoInstall = false;
    beginInstall();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
    // Popup dismissed without a selection (Back button or tap outside): cancel.
    finish();
    return;
  }

  if (state == FAILED || state == NO_UPDATE) {
    // Exit affordances come from the shared state->actions contract: Back for
    // button hardware, tap-anywhere for touch hardware (where button hints
    // never render -- see BaseTheme::drawButtonHints).
    const ota_screen::Actions actions = ota_screen::actionsFor(screenFor(state));
    int x = 0;
    int y = 0;
    if ((actions.acceptBack && mappedInput.wasPressed(MappedInputManager::Button::Back)) ||
        (actions.acceptTap && mappedInput.wasScreenTapped(x, y))) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
