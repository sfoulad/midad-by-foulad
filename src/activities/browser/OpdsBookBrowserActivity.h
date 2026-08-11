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
  enum class BrowserState {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    DOWNLOADING,
    ERROR,
    SEARCH_INPUT,
    // A newer firmware build was named by the catalog server. Not a step in
    // browsing -- an offer laid over it, which Cancel returns from.
    UPDATE_PROMPT
  };

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
  // Feed-level pagination links of the currently loaded page (empty when the
  // feed has none). Kept as members so grid navigation can auto-advance: moving
  // Right past the last cover fetches the next OPDS page, Left before the first
  // cover fetches the previous one -- without them, the grid just wrapped
  // around its 25 in-memory books and the catalog appeared to "repeat".
  std::string feedNextUrl;
  std::string feedPrevUrl;
  // Where to land the selection after the next fetchFeed() (auto-advance only):
  // page N+1 should start on its first cover, page N-1 on its last.
  enum class PendingGridSelect { None, FirstBook, LastBook };
  PendingGridSelect pendingGridSelect = PendingGridSelect::None;
  bool consumeConfirm = false;
  bool consumeBack = false;  // Added missing member
  // Periodic re-registration while sitting on the BROWSING screen, so a
  // setting pushed from the "My Devices" web page while this activity is
  // already open takes effect without the user having to leave and
  // re-enter Foulad eBooks (see loop()) -- the original connect-time-only
  // registerDevice() call left a live session with no way to pick up a
  // mid-session web change short of a full exit/re-enter.
  unsigned long lastDeviceTrackingCheckMs = 0;
  static constexpr unsigned long DEVICE_TRACKING_RECHECK_MS = 30000;
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

  // Cell geometry, shared by the full render and the selector fast path so the two can
  // never disagree about where a cell sits.
  int gridStartXFor(const GridLayout& layout) const;
  int gridTopFor(const GridLayout& layout) const;
  // Draws one cell (cover/fallback, selection frame, title). `slot` is the cell's position
  // on the page, 0-based. `eraseFirst` clears the cell's footprint before drawing, which
  // the fast path needs because it paints over a live frame instead of a cleared one.
  void drawGridCell(const GridLayout& layout, int pageStart, int slot, bool eraseFirst) const;

  // Selector fast path helper: toggles only the 4px selection-ring border around a cell --
  // no SD read, no BMP decode, cover art and title untouched. The ring lives entirely in
  // the GRID_GUTTER=12 gap around a cell (same reasoning as RecentBooksActivity's identical
  // ringRect idiom), so drawing white then black over it never touches the cover or caption.
  // Real-device testing showed the previous drawGridCell-based fast path (full erase + SD
  // read + decode for both the old and new cell) still cost ~1.2-1.4s per selector move --
  // this replaces that cost with a single rect outline toggle.
  void drawGridSelectionRing(const GridLayout& layout, int slot, bool black) const;

  // Selector fast path (see render()). A full repaint re-reads and re-decodes every cover
  // on the page from the SD card, which is what made moving the selector feel slow, so a
  // pure selection move within an already-drawn page redraws only the two cells' ring
  // borders. These track what the framebuffer currently holds; FB_GRID_NONE means
  // it holds something the fast path must not touch up.
  static constexpr int FB_GRID_NONE = -1;
  int fbGridPageStart = FB_GRID_NONE;
  int fbSelectorIndex = -1;
  bool canFastRepaintSelector() const;
  // Consecutive ring-only touch-ups since the last full grid repaint. Real-device
  // testing (holding Down/Right so onContinuous fires the move handlers faster than
  // the ~560ms e-ink refresh cycle can drain them) found a stuck double-ring: two
  // cells' borders lit simultaneously, persisting across many subsequent moves,
  // eventually followed by a device reset. A long unbroken chain of partial ring
  // updates with no intervening full clearScreen()+redraw is exactly the class of
  // problem HalDisplay's forceCleanBaseOnHalf exists for elsewhere in this codebase
  // -- cap it the same way: force a full repaint (which re-clears and redraws
  // everything, self-correcting any accumulated state) every FAST_REPAINT_CAP
  // touch-ups instead of letting the chain run unbounded.
  static constexpr int FAST_REPAINT_CAP = 5;
  int fastRepaintStreak = 0;

  // Row height (pitch) in pixels for plain text-list rows (category/navigation entries, both
  // the pure-list page and the nav strips above/below a book grid). Kept tight/Latin-sized
  // unconditionally so row-to-row spacing looks identical for English and Arabic pages -- see
  // getListRowHighlightHeight() below for the one place that tightness isn't safe on its own.
  int getListRowHeight() const;
  // Height of the selection-highlight bar drawn behind one specific row's title -- can be
  // taller than getListRowHeight() itself. The row pitch stays tight/Latin-sized (see above)
  // for visual consistency, but an Arabic title's ascenders/descenders can be taller than that
  // tight pitch; unselected, the overflow quietly overlaps the next row's whitespace and is
  // barely noticeable, but a *selected* row inverts to white-on-black, so the same overflow
  // gets visibly sliced off at the highlight rectangle's hard edge. Only the highlight itself
  // grows -- and only downward from the row's existing top edge, not split symmetrically
  // above/below: text draws from a fixed top anchor (baseline = top + font ascender), and
  // Arabic's larger ascender+descender sum extends the glyph bounds further down from that
  // same top, not upward, so a symmetric split (an earlier attempt) only gave half the
  // needed room at the bottom and still clipped it.
  int getListRowHighlightHeight(const std::string& title) const;
  // How many list rows fit in the available content area for a given row height --
  // replaces a fixed item-per-page constant so a taller Arabic row height doesn't overflow
  // the screen, and so this adapts across orientations/devices instead of assuming one
  // fixed screen height.
  int getListPageItems(int rowHeight) const;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  // Raises the update offer if registration came back naming a newer build. Reads a
  // cached string; makes no request. An earlier version of this asked GitHub from
  // here and aborted the device -- see the implementation.
  void maybeOfferFirmwareUpdate();
  // Counts painted BROWSING frames, so the offer waits until the catalog it covers
  // is actually on screen.
  uint8_t browsingFramesRendered = 0;
  void fetchFeed(const std::string& path);
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  // Only actually in-flight network work should block auto-sleep -- a page
  // sitting open in BROWSING/SEARCH_INPUT/ERROR is idle, no different from any
  // other screen the user walked away from. The previous unconditional `true`
  // kept the device (and its WiFi radio) awake for as long as Foulad
  // eBooks/OPDS stayed the foreground activity, even fully idle -- a real
  // battery-drain path (user report).
  bool preventAutoSleep() override {
    // UPDATE_PROMPT holds the device awake for the same reason OtaUpdateActivity
    // does: sleeping on an unanswered question loses the answer.
    return state == BrowserState::LOADING || state == BrowserState::DOWNLOADING || state == BrowserState::UPDATE_PROMPT;
  }
  const char* activityDebugName() const override { return "OpdsBookBrowserActivity"; }
};
