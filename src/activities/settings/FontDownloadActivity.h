#pragma once

#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// JSON schema version of the fonts.json manifest. The canonical version for
// the build tooling lives in lib/EpdFont/scripts/cpfont_version.py. This
// firmware-side copy must be bumped manually when the firmware is updated to
// support a new manifest schema.
#define FONTS_MANIFEST_VERSION 1

// Primary font source: the Foulad eBooks catalog (same manifest schema as the
// legacy source, plus a per-family "language" field driving the Arabic/English
// filter). Plain http for the same reason as FOULAD_EBOOKS_URL -- see
// src/FouladEbooksConfig.h. Overridable for local testing.
#ifndef FOULAD_FONTS_CATALOG_URL
#define FOULAD_FONTS_CATALOG_URL "http://midad.one/api/fonts/catalog"
#endif

#ifndef FONT_MANIFEST_URL
// LEGACY FALLBACK, tried only when the Foulad eBooks catalog is unreachable:
// manifest + .cpfont assets published by .github/workflows/release-fonts.yml
// to the crosspoint-fonts repo under the "sd-fonts-m<META>-b<BIN>" tag. The tag
// pattern must stay in sync with the workflow; it derives its version numbers
// from lib/EpdFont/scripts/cpfont_version.py.
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#define FONT_MANIFEST_URL                                                                                           \
  "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  const char* activityDebugName() const override { return "FontDownloadActivity"; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct ManifestFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    std::vector<std::string> styles;
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
    // From the catalog's per-family "language" field ("arabic"/"english",
    // missing = english). Drives the list's language filter.
    bool arabic = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // Manifest data
  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  int selectedIndex_ = 0;
  // Language filter: a fixed tab bar above the list (same widget/idiom as
  // SettingsActivity's own category tabs), switched with Left/Right
  // (NavNext/NavPrevious) -- free on this screen since the list already owns
  // Up/Down (ScrollNext/ScrollPrevious) for scrolling. Unlike the list's
  // rows, the tab bar isn't part of the ButtonNavigator index space at all,
  // so there's no virtual "row 0" to keep in sync with the real list size.
  // visible_ holds indices into families_ matching the filter.
  bool showArabic_ = true;
  std::vector<int> visible_;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  std::string errorMessage_;
  bool cancelRequested_ = false;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  bool fetchManifestFrom(const char* url);
  void rebuildVisibleList();
  void downloadFamily(ManifestFamily& family);
  void downloadAll();
  void updateAll();
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  // Maps a list row to a families_ index through the language filter, or -1
  // for the filter row / special rows / out of range.
  int familyIndexFromList(int listIndex) const;
  int listItemCount() const;
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};
