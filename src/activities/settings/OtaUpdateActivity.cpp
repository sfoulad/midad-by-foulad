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

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
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
  if (res != OtaUpdater::OK) {
    const auto httpFailure = HttpDownloader::getLastFailure();
    LOG_DBG("OTA", "Update check failed: %d (free heap %u, largest block %u, http stage %u detail %d)", res,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), static_cast<unsigned>(httpFailure.stage), httpFailure.detail);
    {
      RenderLock lock(*this);
      lastErrorCode = res;
      failureFreeHeap = ESP.getFreeHeap();
      failureMaxBlock = ESP.getMaxAllocHeap();
      failureHttpStage = static_cast<uint8_t>(httpFailure.stage);
      failureHttpDetail = httpFailure.detail;
      state = FAILED;
    }
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state = WAITING_CONFIRMATION;
  }
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
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    // drawTextInWidth right-aligns Arabic strings within the row, so these two lines
    // anchor to the right edge under the Arabic UI language (proper RTL) while staying
    // left-anchored for English and other LTR languages.
    renderer.drawTextInWidth(UI_10_FONT_ID, metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                             pageWidth - metrics.contentSidePadding * 2,
                             (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    // GitHub tag names are conventionally "v1.6.24"; strip the leading 'v'/'V' so this
    // reads consistently next to "Current version: 1.6.24" above instead of looking like
    // two different version formats for what may be the same release.
    std::string latestVersionDisplay = updater.getLatestVersion();
    if (!latestVersionDisplay.empty() && (latestVersionDisplay[0] == 'v' || latestVersionDisplay[0] == 'V')) {
      latestVersionDisplay.erase(0, 1);
    }
    renderer.drawTextInWidth(UI_10_FONT_ID, metrics.contentSidePadding, top + height * 2 + metrics.verticalSpacing * 2,
                             pageWidth - metrics.contentSidePadding * 2,
                             (std::string(tr(STR_NEW_VERSION)) + latestVersionDisplay).c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
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
    // Diagnostic lines: error code + heap at failure, plus (when the failure came
    // from the HTTP version-check) which network stage failed and its detail code.
    // Not prose, so not translated; lets a failure be diagnosed without a USB
    // serial capture.
    char diag[64];
    snprintf(diag, sizeof(diag), "code %d  heap %u  block %u", lastErrorCode, static_cast<unsigned>(failureFreeHeap),
             static_cast<unsigned>(failureMaxBlock));
    renderer.drawCenteredText(SMALL_FONT_ID, top + height + metrics.verticalSpacing, diag);
    if (failureHttpStage != 0) {
      char httpDiag[32];
      snprintf(httpDiag, sizeof(httpDiag), "http %u:%d", failureHttpStage, failureHttpDetail);
      renderer.drawCenteredText(
          SMALL_FONT_ID, top + height + metrics.verticalSpacing + renderer.getLineHeight(SMALL_FONT_ID) + 2, httpDiag);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, tr(STR_POWER_ON_HINT));
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::loop() {
  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("OTA", "New update available, starting download...");
      {
        RenderLock lock(*this);
        state = UPDATE_IN_PROGRESS;
      }
      requestUpdateAndWait();

      // Free heap again right before the firmware-download TLS handshake, on top
      // of the onEnter() clear: esp_https_ota_begin() needs a large *contiguous*
      // block for the mbedTLS session to GitHub's CDN, and the "Checking..." /
      // "Update available?" screens drawn since onEnter() will have refilled the
      // glyph cache somewhat (button hints, version strings).
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
        LOG_DBG("OTA", "Update failed: %d (free heap %u, largest block %u)", res, ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
        {
          RenderLock lock(*this);
          lastErrorCode = res;
          failureFreeHeap = ESP.getFreeHeap();
          failureMaxBlock = ESP.getMaxAllocHeap();
          state = FAILED;
        }
        requestUpdate();
        return;
      }

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

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }

    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
