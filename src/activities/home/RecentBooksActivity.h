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
  // grid's cell size instead of the home tile's size.
  static constexpr int GRID_COVER_WIDTH = 150;
  static constexpr int GRID_COVER_HEIGHT = 225;
  static constexpr int GRID_CELL_WIDTH = 180;   // target cell width incl. spacing -> derives column count (3 cols)
  static constexpr int GRID_CELL_HEIGHT = 270;  // target cell height incl. spacing + title row
  static constexpr int NO_GRID_PAGE_LOADED = -1;
  int loadedGridPageStart = NO_GRID_PAGE_LOADED;

  struct GridGeometry {
    int columns = 1;
    int itemsPerPage = 1;
  };
  GridGeometry computeGridGeometry() const;
  void loadGridPageCovers(int pageStart);

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
