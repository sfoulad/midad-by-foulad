#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class FileBrowserActivity final : public UiListActivity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // Deletion
  bool removeDirFile(const std::string& fullPath);

  Mode mode = Mode::Books;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;

  // Per-row render buffers, derived from `files` and rebuilt only when it
  // changes (loadFiles()) rather than on every repaint — buildScreen() used to
  // rebuild a name/extension string and a ListItem per file on every render
  // (cursor move, tap flash, ...), which meant a 500-file directory allocated
  // 500 strings per repaint instead of once per directory load.
  std::vector<std::string> rowNames;
  std::vector<std::string> rowExtensions;
  std::vector<freeink::ui::ListItem> rowItems;
  // getFileName()'s "[folder]" bracket formatting depends on the active
  // theme's showsFileIcons(); tracked so a theme change while this activity is
  // paused underneath (e.g. a Settings screen reached via a picker flow)
  // invalidates the cached rows on return instead of rendering stale ones.
  bool rowsUseFileIcons = false;

  void rebuildRowItems();

  // Swallows a Confirm release left over from before this activity opened
  // (see the comment in onEnter()) so it doesn't immediately activate a row.
  bool lockNextConfirmRelease = false;

  int listCount() const override { return static_cast<int>(files.size()) + (hasTransferRow() ? 1 : 0); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Long-press BACK goes to root; short Back goes up a directory (home/cancel at
  // root), and Confirm activates on RELEASE (a hold is "delete").
  bool handleCustomInput() override;
  bool handleButtons() override;
  // Header shows the current folder name (battery indicator via GUI.drawHeader);
  // footer labels depend on path depth and picker mode.
  void drawChrome() override;
  void drawFooter() override;
  // forceDelete routes the touch long-press to the delete branch; button
  // navigation leaves it false and relies on getHeldTime() instead.
  void activateSelected(bool forceDelete = false);

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
                               Mode mode = Mode::Books);
  void onEnter() override;
  void onExit() override;
};
