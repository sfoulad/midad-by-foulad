#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server)
      : Activity("OpdsBookBrowser", renderer, mappedInput), buttonNavigator(), server(std::move(server)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;  // Added missing member
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  // Book-listing pages (any page with at least one BOOK entry) render as a
  // cover grid instead of a text list. NAVIGATION entries on the same page
  // (the synthetic PREV_PAGE/NEXT_PAGE entries fetchFeed() may insert) render
  // as slim strips above/below the grid instead of grid cells.
  static constexpr int GRID_COVER_WIDTH = 150;
  static constexpr int GRID_COVER_HEIGHT = 225;
  static constexpr int GRID_CELL_WIDTH = 180;    // target cell width incl. spacing -> derives column count (3 cols)
  static constexpr int GRID_CELL_HEIGHT = 270;   // target cell height incl. spacing + title row
  static constexpr int GRID_CONTENT_TOP = 60;    // matches the existing list's content start y
  static constexpr int GRID_BOTTOM_MARGIN = 40;  // reserved for button hints
  static constexpr int NO_GRID_PAGE_LOADED = -1;
  int loadedGridPageStart = NO_GRID_PAGE_LOADED;

  struct GridLayout {
    bool isGridPage = false;
    int topNavCount = 0;  // entries[0, topNavCount) -> nav strip above the grid
    int bookStart = 0;    // entries[bookStart, bookStart+bookCount) -> the grid
    int bookCount = 0;
    int bottomNavStart = 0;  // entries[bottomNavStart, entries.size()) -> nav strip below the grid
    int columns = 1;
    int itemsPerPage = 1;
  };
  GridLayout computeGridLayout() const;
  void loadGridPageCovers(const GridLayout& layout, int pageStart);

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override { return true; }
};
