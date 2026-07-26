#include "GymAssetSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "GymCatalog.h"
#include "GymPlanStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/GymDiagLog.h"

GymAssetSyncActivity::GymAssetSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("GymAssetSync", renderer, mappedInput) {}

void GymAssetSyncActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void GymAssetSyncActivity::onExit() {
  Activity::onExit();
  // Targeted restart (not bare silentRestart()) so the user lands back in
  // the Gym app they were using, not on Home.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToGym();
  }
}

void GymAssetSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }
  {
    RenderLock lock(*this);
    state_ = SYNCING;
  }
  requestUpdateAndWait();
  syncMissingAssets();
}

void GymAssetSyncActivity::syncMissingAssets() {
  const unsigned long startMs = millis();
  // Distinct slugs across all 7 days, first-seen order -- a slug assigned to
  // more than one day only ever needs downloading once.
  std::vector<std::string> slugs;
  for (size_t d = 0; d < GymPlanStore::DAY_COUNT; d++) {
    for (const auto& ex : GYM_PLAN.getDay(d).exercises) {
      if (std::find(slugs.begin(), slugs.end(), ex.slug) == slugs.end()) {
        slugs.push_back(ex.slug);
      }
    }
  }

  Storage.mkdir("/gym");
  Storage.mkdir("/gym/images");
  Storage.mkdir("/gym/instructions");

  totalMissing_ = 0;
  for (const auto& slug : slugs) {
    if (!Storage.exists(GymCatalog::imagePath(slug).c_str()) ||
        !Storage.exists(GymCatalog::instructionsPath(slug).c_str())) {
      ++totalMissing_;
    }
  }

  downloadedCount_ = 0;
  for (const auto& slug : slugs) {
    const std::string imgPath = GymCatalog::imagePath(slug);
    const std::string instrPath = GymCatalog::instructionsPath(slug);
    const bool neededSomething = !Storage.exists(imgPath.c_str()) || !Storage.exists(instrPath.c_str());
    bool ok = true;

    if (!Storage.exists(imgPath.c_str())) {
      const std::string url = std::string(FOULAD_GYM_IMAGE_BASE_URL) + slug + ".jpg";
      const auto result = HttpDownloader::downloadToFile(url, imgPath, nullptr);
      if (result != HttpDownloader::OK) {
        Storage.remove(imgPath.c_str());
        ok = false;
        char buf[96];
        snprintf(buf, sizeof(buf), "%lu gym asset_sync slug=%s asset=image result=failed httpErr=%d", millis(),
                 slug.c_str(), static_cast<int>(result));
        GymDiagLog::append(buf);
      }
    }
    if (!Storage.exists(instrPath.c_str())) {
      const std::string url = std::string(FOULAD_GYM_EXERCISE_BASE_URL) + slug;
      const auto result = HttpDownloader::downloadToFile(url, instrPath, nullptr);
      if (result != HttpDownloader::OK) {
        Storage.remove(instrPath.c_str());
        ok = false;
        char buf[96];
        snprintf(buf, sizeof(buf), "%lu gym asset_sync slug=%s asset=instructions result=failed httpErr=%d", millis(),
                 slug.c_str(), static_cast<int>(result));
        GymDiagLog::append(buf);
      }
    }

    if (neededSomething && ok) {
      ++downloadedCount_;
    }
    requestUpdate();
  }

  char summaryBuf[112];
  snprintf(summaryBuf, sizeof(summaryBuf), "%lu gym asset_sync result=done downloaded=%d missing=%d elapsed=%lums",
           millis(), downloadedCount_, totalMissing_, millis() - startMs);
  GymDiagLog::append(summaryBuf);

  RenderLock lock(*this);
  state_ = COMPLETE;
}

void GymAssetSyncActivity::loop() {
  if (state_ == COMPLETE || state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

void GymAssetSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GYM));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == SYNCING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight - metrics.verticalSpacing, tr(STR_LOADING));
    GUI.drawProgressBar(renderer,
                        Rect{metrics.contentSidePadding, centerY, pageWidth - metrics.contentSidePadding * 2,
                             metrics.progressBarHeight},
                        static_cast<size_t>(downloadedCount_), static_cast<size_t>(std::max(totalMissing_, 1)));
  } else if (state_ == COMPLETE) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s (%d)", tr(STR_GYM_ASSETS_READY), downloadedCount_);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, buf, true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DOWNLOAD_FAILED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
