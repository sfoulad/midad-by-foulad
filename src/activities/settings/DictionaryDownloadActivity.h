#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Foulad eBooks dictionary catalog (StarDict sets). Same conventions as the
// font store: plain http (see src/FouladEbooksConfig.h), version=1 schema,
// per-file crc32 verification. Overridable for local testing.
#ifndef FOULAD_DICTS_CATALOG_URL
#define FOULAD_DICTS_CATALOG_URL "http://foulad.one/api/dicts/catalog"
#endif

// Download store for StarDict dictionaries, modeled on FontDownloadActivity:
// fetch catalog -> list with sizes -> download all files of a set with
// progress + crc32 verify into /dictionaries/<direction>/ -> or delete an
// installed set (including the device-generated .cpridx cache).
class DictionaryDownloadActivity : public Activity {
 public:
  explicit DictionaryDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ != DICT_LIST; }
  const char* activityDebugName() const override { return "DictionaryDownload"; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_CATALOG,
    DICT_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct CatalogFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
  };

  struct CatalogDict {
    std::string slug;
    std::string name;
    std::string description;
    std::string direction;
    std::vector<CatalogFile> files;
    size_t totalSize = 0;
    bool installed = false;
  };

  State state_ = WIFI_SELECTION;
  ButtonNavigator buttonNavigator_;

  std::string baseUrl_;
  std::vector<CatalogDict> dicts_;
  int selectedIndex_ = 0;

  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingIndex_ = -1;
  std::string errorMessage_;
  bool cancelRequested_ = false;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseCatalog();
  void refreshInstalledFlags();
  std::string installDir(const CatalogDict& dict) const;
  void downloadDict(CatalogDict& dict);
  void deleteDict(const CatalogDict& dict);
  void promptDeleteSelected();
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  static std::string formatSize(size_t bytes);
};
