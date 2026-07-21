#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // Deletion
  bool removeDirFile(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;

  Mode mode = Mode::Books;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

  // Synthetic "File Transfer" row shown ABOVE the real file/folder listing,
  // only at the SD card root in normal browsing (not the firmware picker,
  // and not once you've navigated into a subfolder). Kept out of `files`
  // (real filesystem entries) rather than prepended into it, so delete/
  // navigate logic never has to special-case a fake entry -- callers instead
  // add 1 to indices into `files` (see hasTransferRow()'s call sites).
  bool hasTransferRow() const { return mode == Mode::Books && basepath == "/"; }

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books)
      : Activity("FileBrowser", renderer, mappedInput),
        mode(mode),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
