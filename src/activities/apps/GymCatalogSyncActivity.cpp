#include "GymCatalogSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstdio>

#include "GymCatalog.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/GymDiagLog.h"

namespace {
constexpr const char* CATALOG_TMP = "/gym_catalog.tmp";
}

GymCatalogSyncActivity::GymCatalogSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("GymCatalogSync", renderer, mappedInput) {}

void GymCatalogSyncActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void GymCatalogSyncActivity::onExit() {
  Activity::onExit();
  // Same reasoning as Dictionary/Font download: tear down WiFi and restart on
  // a fresh heap rather than risk running the rest of the session on a
  // TLS-fragmented one.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void GymCatalogSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }
  {
    RenderLock lock(*this);
    state_ = SYNCING;
  }
  requestUpdateAndWait();
  syncCatalog();
}

void GymCatalogSyncActivity::syncCatalog() {
  const unsigned long startMs = millis();
  Storage.remove(CATALOG_TMP);
  auto result = HttpDownloader::downloadToFile(FOULAD_GYM_CATALOG_URL, CATALOG_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("GYM", "Failed to fetch catalog from %s", FOULAD_GYM_CATALOG_URL);
    char buf[96];
    snprintf(buf, sizeof(buf), "%lu gym catalog_sync result=fetch_failed httpErr=%d", millis(),
             static_cast<int>(result));
    GymDiagLog::append(buf);
    Storage.remove(CATALOG_TMP);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to fetch exercise catalog";
    return;
  }

  // Validate before committing over the existing (working) catalog -- a
  // truncated/malformed download must never clobber a good local copy.
  std::vector<GymCatalogEntry> parsed;
  if (!GymCatalog::loadFromPath(CATALOG_TMP, parsed) || parsed.empty()) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lu gym catalog_sync result=parse_failed", millis());
    GymDiagLog::append(buf);
    Storage.remove(CATALOG_TMP);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Invalid exercise catalog";
    return;
  }

  Storage.mkdir("/gym");
  Storage.remove(GymCatalog::CATALOG_PATH);
  if (!Storage.rename(CATALOG_TMP, GymCatalog::CATALOG_PATH)) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lu gym catalog_sync result=rename_failed", millis());
    GymDiagLog::append(buf);
    Storage.remove(CATALOG_TMP);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to save exercise catalog";
    return;
  }

  exerciseCount_ = static_cast<int>(parsed.size());
  char buf[96];
  snprintf(buf, sizeof(buf), "%lu gym catalog_sync result=ok count=%d elapsed=%lums", millis(), exerciseCount_,
           millis() - startMs);
  GymDiagLog::append(buf);
  RenderLock lock(*this);
  state_ = COMPLETE;
}

void GymCatalogSyncActivity::loop() {
  if (state_ == COMPLETE || state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

void GymCatalogSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYNC_GYM_CATALOG));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == SYNCING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING));
  } else if (state_ == COMPLETE) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s (%d)", tr(STR_GYM_CATALOG_READY), exerciseCount_);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, buf, true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DOWNLOAD_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
