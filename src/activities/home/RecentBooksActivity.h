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
  // consistency between the two "grid" screens). Covers come from the same
  // per-book thumbnail cache HomeActivity's Continue Reading tile already uses
  // (Epub::generateThumbBmp / Xtc::generateThumbBmp), just requested at this
  // grid's computed cell size instead of the home tile's size.
  //
  // Cover size is NOT a fixed constant: covers must fill the actual screen
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
  // Base space reserved below the cover for the title, sized for Latin text. Actual
  // reserved height comes from getGridTitleRowHeight(), which grows this when an Arabic
  // title is present -- use that, not this constant directly, anywhere layout is computed.
  static constexpr int GRID_TITLE_ROW_HEIGHT = 36;
  static constexpr int NO_GRID_PAGE_LOADED = -1;
  int loadedGridPageStart = NO_GRID_PAGE_LOADED;

  struct GridGeometry {
    int columns = 1;
    int itemsPerPage = 1;
    int coverWidth = 0;
    int coverHeight = 0;
  };
  GridGeometry computeGridGeometry() const;
  void loadGridPageCovers(int pageStart);

  // GRID_TITLE_ROW_HEIGHT reserves space below the cover for one line of title text at
  // SMALL_FONT_ID. Arabic line height at the same nominal size runs noticeably taller
  // (no descender clearance built in for it), so an Arabic book title clips its
  // tails/tashkeel against the next row -- same bug class already fixed for
  // OpdsBookBrowserActivity's list rows. Grows the reserved height by exactly the extra
  // an Arabic title needs; returns the unmodified constant (zero regression) when the
  // current recent-books list has no Arabic titles.
  int getGridTitleRowHeight() const;

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
