#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;

  // Recent tab state
  std::vector<RecentBook> recentBooks;

  // Cover grid layout (matches OpdsBookBrowserActivity's grid sizing for visual
  // consistency between the two "grid" screens). Covers come from the SAME
  // per-book thumbnail cache file HomeActivity's hero/recents row uses
  // (Epub::generateThumbBmp / Xtc::generateThumbBmp at the theme's fixed
  // homeCoverHeight -- see GridGeometry::thumbHeight in computeGridGeometry()),
  // then drawBitmap scales that one shared bitmap down to this grid's own
  // cell size. Requesting a second, grid-specific generation height here used
  // to mean two independent cache files and two independent success/failure
  // outcomes for the same cover.
  //
  // The DISPLAY cell size (coverWidth/coverHeight) is NOT a fixed constant:
  // covers must fill the actual screen
  // width edge-to-edge (minus gutters), which differs by device (X3 portrait
  // logical width 528, X4 portrait 480) and orientation. GRID_MIN_CELL_WIDTH
  // only decides how many columns fit; computeGridGeometry() derives the
  // real per-cover pixel size from renderer.getScreenWidth() every time.
  // 140 reliably yields 3 columns on both X3 portrait (528px logical width) and
  // X4 portrait (480px) -- 150 only reached 3 columns on the X3, giving just 2 on
  // the X4 (verified by computing actual column counts for both device widths).
  static constexpr int GRID_MIN_CELL_WIDTH = 140;
  static constexpr int GRID_GUTTER = 12;
  static constexpr float GRID_COVER_ASPECT = 1.5f;
  static constexpr int GRID_TITLE_LINES = 2;    // caption below the cover wraps up to this many lines
  static constexpr int GRID_TITLE_TOP_GAP = 4;  // gap between cover bottom and first title line
  static constexpr int NO_GRID_PAGE_LOADED = -1;
  int loadedGridPageStart = NO_GRID_PAGE_LOADED;

  struct GridGeometry {
    int columns = 1;
    int itemsPerPage = 1;
    int coverWidth = 0;
    int coverHeight = 0;  // grid cell display size -- varies with device/orientation
    int thumbHeight = 0;  // cache-key height for the underlying thumb bmp -- see
                          // computeGridGeometry()'s comment for why this must stay
                          // fixed at the theme's canonical hero height
  };
  GridGeometry computeGridGeometry() const;
  void loadGridPageCovers(int pageStart);
  // Total space reserved below a cover for its (up to GRID_TITLE_LINES-line) title caption --
  // see OpdsBookBrowserActivity::getGridTitleHeight() for why this is computed from font
  // metrics rather than a fixed constant (Arabic titles need noticeably more line height).
  int getGridTitleHeight() const;

  // Data loading
  void loadRecentBooks();

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
