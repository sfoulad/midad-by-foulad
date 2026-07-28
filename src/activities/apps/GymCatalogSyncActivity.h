#pragma once

#include <string>

#include "activities/Activity.h"

// Foulad eBooks gym/exercise catalog (see ~/Desktop/Claude/foulad-ebooks/GYM_STORE_TASKS.md
// for the server contract). Overridable for local testing, same convention as
// FOULAD_DICTS_CATALOG_URL/FOULAD_FONTS_CATALOG_URL.
#ifndef FOULAD_GYM_CATALOG_URL
#define FOULAD_GYM_CATALOG_URL "http://midad.one/api/gym/catalog"
#endif

// Downloads the full exercise catalog (metadata only, no images/instructions
// -- those are fetched lazily, per-exercise, from the exercise browser) and
// caches it at GymCatalog::CATALOG_PATH. Unlike FontDownloadActivity/
// DictionaryDownloadActivity there's nothing to pick from a list here: the
// catalog is one file, always fetched in full and overwritten wholesale on
// each sync (simpler than a per-item install/update flow since there's only
// one item).
class GymCatalogSyncActivity final : public Activity {
 public:
  explicit GymCatalogSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == SYNCING; }
  const char* activityDebugName() const override { return "GymCatalogSync"; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    SYNCING,
    COMPLETE,
    ERROR,
  };

  State state_ = WIFI_SELECTION;
  std::string errorMessage_;
  int exerciseCount_ = 0;

  void onWifiSelectionComplete(bool success);
  void syncCatalog();
};
