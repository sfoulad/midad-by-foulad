#pragma once

#include <memory>
#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/UiListActivity.h"

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
// Midad Core's font catalogue. "v4" is a served version in Core's
// CPFONT_VERSIONS: there is deliberately no default and no "latest", so the
// segment must name a published version or the request fails. v5 is accepted
// but publishes nothing yet and returns 503.
//
// The file URLs are NOT built from this constant -- parseCatalog() takes
// baseUrl from the response body and appends each file name, so the server
// chooses where files come from.
#define FOULAD_FONTS_CATALOG_URL "https://api.midad.one/fonts/v4/catalog"
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

class FontDownloadActivity final : public UiListActivity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
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
    GROUP_LIST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  // Byte offset into stringArena_; 0 is the empty string.
  using StrRef = uint32_t;

  struct ManifestFile {
    StrRef name = 0;
    uint32_t size = 0;
    uint32_t crc32 = 0;
  };

  struct ManifestFamily {
    StrRef name = 0;
    StrRef description = 0;
    // Range into files_, which holds every family's files back to back.
    uint32_t fileStart = 0;
    uint32_t fileCount = 0;
    uint32_t totalSize = 0;
    uint32_t scriptMask = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  static constexpr size_t MAX_SCRIPT_GROUPS = 32;

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;

  // Manifest data
  std::string baseUrl_;
  // Reused for every file of every family: downloadToFile takes a std::string,
  // so a char buffer would just build a temporary per call.
  std::string downloadUrl_;
  // Manifest strings, null-terminated and packed back to back.
  std::unique_ptr<char[]> stringArena_;
  uint32_t arenaUsed_ = 0;
  uint32_t arenaCapacity_ = 0;
  std::vector<ManifestFamily> families_;
  // Every family's files back to back; sized once from the manifest, so it is
  // allocated nothrow like the arena rather than through vector::reserve.
  std::unique_ptr<ManifestFile[]> files_;
  uint32_t fileEntryCount_ = 0;
  // Manifest-defined labels are dynamic; cap them at the 32-bit membership
  // mask and retain only labels after parsing so group tags consume no steady-state heap.
  std::vector<StrRef> scriptGroupLabels_;
  // One 4-byte index per manifest family, allocated once and reused for every group.
  std::vector<int> filteredIndices_;
  freeink::ui::ListNav groupNav_;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  // Set when the cancel came from the home gesture (consumed by the download
  // callback's own input pump); exit to home after the abort unwinds.
  bool goHomeRequested_ = false;

  // Shared cache for group and family rows. It is rebuilt only when the visible
  // list changes, never for cursor movement or tap flash repaints.
  std::vector<std::string> rowLabels_;
  std::vector<freeink::ui::ListItem> rowItems_;
  bool rowsDirty_ = true;
  void rebuildRowItems();
  void rebuildGroupRowItems();
  void rebuildFamilyRowItems();

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  freeink::ui::ListNav& activeNav() override;
  void onBackButton() override;
  // Non-list states (loading, downloading, complete, error) consume the loop
  // pass here; the group and family lists use the base list protocol.
  bool handleCustomInput() override;

  void activateSelected();

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  // One manifest source. languageSchema enables the Foulad catalog's
  // per-family "language" extension (mapped onto synthesized script groups);
  // the legacy manifest uses scriptGroups/scripts natively.
  bool fetchManifestFrom(const char* url, bool languageSchema);
  // cppcheck-suppress arithOperationsOnVoidPointer // unique_ptr<char[]>::get() is char*, not void*
  const char* str(StrRef ref) const { return stringArena_ ? stringArena_.get() + ref : ""; }
  // Returns false if the string does not fit the arena reserved for the manifest.
  bool internString(const char* text, StrRef& outRef);
  void clearManifest();
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
  int familyIndexFromList(int listIndex) const;
  int listItemCount() const;
  bool hasGroupScreen() const { return !scriptGroupLabels_.empty(); }
  int groupListItemCount() const { return 1 + static_cast<int>(scriptGroupLabels_.size()); }
  int groupMemberCount(int scriptGroupIndex) const;
  void buildFilteredIndices(int groupListIndex);
  void enterGroup(int groupListIndex);
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};
