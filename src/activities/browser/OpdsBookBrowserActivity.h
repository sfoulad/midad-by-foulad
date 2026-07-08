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
  // foulad-ebooks' download endpoints respond with Transfer-Encoding: chunked (no
  // Content-Length), so downloadTotal is 0 for every real download today. Rather than show
  // no bar at all in that case, render() fills the bar against this generous EPUB-sized
  // ceiling instead -- it under-promises (a smaller book finishes before the bar looks
  // "full", jumping straight to completion) rather than over-promises, and still gives the
  // user visible proof the transfer is progressing instead of a static label with no motion.
  static constexpr size_t ESTIMATED_DOWNLOAD_SIZE = 8 * 1024 * 1024;
  // Last redrawn percentage during download, so the progress callback (fired once per
  // HTTP chunk) only forces an e-ink refresh on ~1% steps instead of on every chunk.
  // Only meaningful when downloadTotal > 0 -- foulad-ebooks' download endpoints respond
  // with Transfer-Encoding: chunked (no Content-Length), so downloadTotal is 0 for every
  // real download today; see lastDownloadBytesShown below for that case.
  int lastDownloadPercentage = -1;
  // Last redrawn byte count during a download of unknown total size (downloadTotal == 0).
  // Redraws every ~32KB instead of on every 2KB HTTP chunk, for the same reason as
  // lastDownloadPercentage -- forcing an e-ink refresh per chunk made a several-MB
  // download look stuck for minutes.
  size_t lastDownloadBytesShown = 0;
  // Set just before calling activityManager.goToReader() from downloadBook(), so onExit()
  // can silentRestartToReader() instead of the default silentRestart()-to-Home if WiFi is
  // still connected when this activity is torn down (the deferred Replace action runs
  // onExit() before the new ReaderActivity is constructed, so a plain silentRestart()
  // would silently discard the pending "open this book" transition).
  std::string pendingReaderPath;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  // Book-listing pages (any page with at least one BOOK entry) render as a
  // cover grid instead of a text list. NAVIGATION entries on the same page
  // (the synthetic PREV_PAGE/NEXT_PAGE entries fetchFeed() may insert) render
  // as slim strips above/below the grid instead of grid cells.
  //
  // Cover size is NOT a fixed constant: covers must fill the actual screen
  // width edge-to-edge (minus gutters), which differs by device (X3 portrait
  // logical width 528, X4 portrait 480) and orientation. GRID_MIN_CELL_WIDTH
  // only decides how many columns fit; computeGridLayout() derives the real
  // per-cover pixel size from renderer.getScreenWidth() every time, so cover
  // size and column count can never drift apart the way two independent
  // fixed constants did previously (covers rendering much smaller than the
  // grid cells they sat in, leaving dead space).
  // 140 reliably yields 3 columns on both X3 portrait (528px logical width) and
  // X4 portrait (480px) -- 150 only reached 3 columns on the X3, giving just 2 on
  // the X4 (verified by computing actual column counts for both device widths).
  static constexpr int GRID_MIN_CELL_WIDTH = 140;  // decides column count, not the rendered cover size
  static constexpr int GRID_GUTTER = 12;
  static constexpr float GRID_COVER_ASPECT = 1.5f;  // coverHeight = coverWidth * aspect
  static constexpr int GRID_TITLE_LINES = 2;        // caption below the cover wraps up to this many lines
  static constexpr int GRID_TITLE_TOP_GAP = 4;      // gap between cover bottom and first title line
  static constexpr int GRID_CONTENT_TOP = 60;       // matches the existing list's content start y
  static constexpr int GRID_BOTTOM_MARGIN = 40;     // reserved for button hints
  static constexpr int NO_GRID_PAGE_LOADED = -1;
  int loadedGridPageStart = NO_GRID_PAGE_LOADED;

  // Total space reserved below a cover for its (up to GRID_TITLE_LINES-line) title caption.
  // Computed from actual font metrics rather than a fixed constant: the Arabic font's line
  // height runs noticeably taller than SMALL_FONT_ID's Latin metrics (same reasoning as
  // getListRowHeight() below), so a fixed reservation sized for Latin text would clip an
  // Arabic title's second line. Uses the worst case unconditionally (not per-page detection)
  // since the grid's row spacing must stay stable regardless of which page is showing.
  int getGridTitleHeight() const;

  struct GridLayout {
    bool isGridPage = false;
    int topNavCount = 0;  // entries[0, topNavCount) -> nav strip above the grid
    int bookStart = 0;    // entries[bookStart, bookStart+bookCount) -> the grid
    int bookCount = 0;
    int bottomNavStart = 0;  // entries[bottomNavStart, entries.size()) -> nav strip below the grid
    int columns = 1;
    int itemsPerPage = 1;
    int coverWidth = 0;  // fills the actual screen width -- see GRID_MIN_CELL_WIDTH comment above
    int coverHeight = 0;
  };
  GridLayout computeGridLayout() const;
  void loadGridPageCovers(const GridLayout& layout, int pageStart);

  // Row height in pixels for plain text-list rows (category/navigation entries, both the
  // pure-list page and the nav strips above/below a book grid). Arabic titles need
  // noticeably more vertical space than Latin ones (the Arabic font's ascender+descender
  // is roughly double), so a Latin-sized row clips Arabic glyph tops/tails -- same issue
  // already fixed for XtcReaderChapterSelectionActivity's chapter list. Checks whether any
  // entry on the current page has an Arabic title and sizes every row for the tallest font
  // actually in use, so pagination (getListPageItems) and rendering always agree.
  int getListRowHeight() const;
  // How many list rows fit in the available content area for a given row height --
  // replaces a fixed item-per-page constant so a taller Arabic row height doesn't overflow
  // the screen, and so this adapts across orientations/devices instead of assuming one
  // fixed screen height.
  int getListPageItems(int rowHeight) const;

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
