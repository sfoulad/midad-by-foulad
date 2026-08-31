#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderToolbarUi.h"
#include "components/OptionPopup.h"
#include "reading/ReadingStatsStore.h"

class EpubReaderActivity final : public ReaderActivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  // Absolute page to land on when the NEXT section loads (uint16 max = last page).
  // Armed by backward-across-boundary turns and the end-of-book "last page" paths;
  // consumed exactly once at section load. INVARIANT: every other navigation that
  // resets `section` (forward turns, chapter select, jumps, href/footnote moves) must
  // clear this, or a stale arm left by a failed/preempted load overrides that
  // navigation's own landing page -- real-device report: finishing a chapter and
  // turning forward landed on the LAST page of the next chapter.
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  int8_t pendingManualTurn = 0;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool currentPageBookmarked = false;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, More };
  Overlay overlay = Overlay::None;
  int focusedTool = 0;  // toolbar tool focus: 0=Contents, 1=Text, 2=More
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font size / line spacing / alignment / orientation /
  // auto page turn. Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup overlayPopup;
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool partialRebuildStartFailed = false;

  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  // Small chunks on purpose: while buildSomeMore() runs, loop() is blocked and
  // polled buttons are input-blind -- a quick tap whose press AND release both
  // land inside the window is lost entirely. Image-heavy pages decode their
  // images during layout, stretching a chunk to seconds on-device, which reads
  // as "buttons dead" mid-book. 2 (was 8) bounds the blocking-turn overshoot
  // past the target page; 1 (was 2) halves the background tick's blind window.
  static constexpr int BUILD_PAGES_PER_CHUNK = 2;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 1;
  // Hard cap on one background tick's input-blind window. Real-device perf logs
  // showed page-count pacing alone letting a single chunk run 1-3.6s (heavy
  // page layout, further stretched by the power-saving CPU clock while the user
  // reads quietly) -- long enough to swallow a quick page-turn tap entirely.
  // buildSomeMore() yields mid-page once this elapses and resumes next tick.
  static constexpr unsigned long BACKGROUND_BUILD_BUDGET_MS = 250;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Background build ticks pause below these thresholds instead of attempting a chunk: layout
  // (CSS rule storage, TextBlock/word arrays, image decode buffers) makes real allocations, and
  // running it while the heap is already this tight is how a background tick becomes the straw
  // that triggers an OOM abort for something unrelated (e.g. a WiFi/TLS operation started right
  // after). Matches the free-heap/largest-block gate idiom used elsewhere in this codebase (see
  // OpdsBookBrowserActivity.cpp, DictionaryStore.cpp). Foreground paths (page turns, initial
  // build) are NOT gated -- the reader must still open the book it's showing.
  static constexpr uint32_t BUILD_TICK_MIN_FREE_HEAP_BYTES = 32 * 1024;
  static constexpr uint32_t BUILD_TICK_MIN_LARGEST_BLOCK_BYTES = 16 * 1024;
  // Set when a background tick was skipped for the heap gate above. Read by skipLoopDelay() so
  // a paused build doesn't spin the reader at full CPU/no-delay for nothing -- see its comment.
  bool buildHeapPaused = false;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Deadline backstop for the predictive gates above: if the blocking build-to-target still
  // hasn't produced the landing page this long after the build started, surface the popup
  // mid-build. Builds that finish under the deadline stay popup-free.
  // 2500, not 1000: real-device serial logs (X3, ordinary chapters, no images) showed
  // "Rendered page" landing at 1000-1200ms routinely -- an initial 1000ms deadline fired
  // on essentially every book open, showing "Indexing" for ordinary landings the predictive
  // gates above were specifically designed to keep popup-free. 2500ms gives real hardware
  // headroom over that baseline while still catching genuinely slow builds.
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 2500;
  // True only during render()'s blocking build-to-target phase, until the popup has been
  // drawn. Gates showBuildPopup() so the parser's popup callback (which persists into
  // background buildSomeMore ticks) can never draw over a displayed page.
  bool buildPopupPending = false;
  // Draw the indexing popup mid-build (parser image-probe callback and deadline backstop).
  void showBuildPopup();
  // Heap floors for optional render-adjacent work (idle prewarm). Page
  // deserialization (TextBlock word arenas/strings) and glyph caching allocate
  // through throwing paths that abort() on OOM under -fno-exceptions; skip
  // deferrable work below them. The largest-block floor exists because free
  // heap alone ignores fragmentation (same lesson as the OPDS cover gate).
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  static constexpr size_t RENDER_MIN_LARGEST_BLOCK = 16 * 1024;
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void jumpToPercent(int percent);
  // Spine-anchored jump, preferred over the percentage when the server supplies one.
  void jumpToSpine(int spineIndex);
  // `menu` carries the drawer's extra payload (chapter pick, per-book setting
  // edits). The toolbar's More panel dispatches leaf actions with no payload,
  // so it defaults -- every case that reads `menu` is filtered out there.
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action, const MenuResult& menu = MenuResult{});
  // Reloads the current page fresh from the section (mirrors the loadPage()
  // call in render()) as a shared_ptr for DictionaryWordSelectActivity /
  // DictionaryHistoryActivity, which outlive the menu-confirm call. Also
  // computes the same oriented margins render() uses. Returns nullptr if the
  // page can't be loaded (caller should no-op rather than launch with no text).
  std::shared_ptr<Page> loadCurrentPageForLookup(int& outMarginLeft, int& outMarginTop);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  std::string currentChapterTitle() const;
  // Text panel rows (font, size, line spacing, alignment, focus reading).
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  void openDictionaryWordSelect();
  bool launchKOReaderSync();
  unsigned long confirmLongPressThreshold() const;
  // Midad equivalent of launchKOReaderSync(): same save-release-replace shape,
  // see the implementation.
  void launchMidadSync();
  // True when this book carries a Foulad eBooks catalog id, i.e. Sync can do
  // something. Gates the drawer row so it is never shown-and-inert.
  bool bookHasFouladId() const;
  // Catalog id for the open book: stats store first, recents as fallback.
  std::string currentBookFouladId() const;
  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  // Gives the status bar's text lane back when it is hidden or progress-bar-only, so a
  // footer indicator has somewhere to draw. Costs a re-layout, so it is only called when
  // one is actually starting. No-op with a normal status bar.
  void reserveStatusBarSpaceIfHidden();
  // Inverted full-screen passes when a Pomodoro phase runs out. The only refresh the
  // reading Pomodoro ever costs; see ReaderPomodoro.h.
  void flashPomodoroAlert();
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  // --- Reading-time tracking (see src/reading/ReadingStatsStore.h) ---
  // millis() when the current page became visible; 0 = timer not running.
  unsigned long pageShownAtMs = 0UL;
  // True once a page has actually been turned in this activity instance -- see the
  // close-sync block in onExit() for why an age is only honest after that.
  bool pageTurnedThisSession = false;
  // A cross-device jump read out of APP_STATE in onEnter() and applied on the first
  // loop() iteration -- see the comment there for why it is not applied immediately.
  // 0 / -1 mean nothing pending.
  uint8_t pendingJumpPercent = 0;
  int16_t pendingJumpSpine = -1;
  // First page dwell after opening/jumping includes orientation time, not reading --
  // skip it as a pace sample (it still counts toward session time).
  bool paceWarmupPending = true;
  // Folds the current page's dwell into the stats store (and, for a qualifying
  // forward turn, into the pace average), then restarts the page timer.
  void accountPageDwellForStats(bool isForwardTurn);
  // Call on any non-linear navigation (chapter/percent/bookmark/footnote jump): banks
  // the dwell so far and re-arms the pace warmup so the jump doesn't pollute the pace.
  void statsOnJump();
  // Pace x estimated remaining pages (current section pages + remaining spine bytes
  // scaled by this section's bytes-per-page). False when no pace is known yet.
  bool estimateTimeLeftSeconds(uint32_t& seconds) const;
  // Current whole-book progress percent (0-100), same math as the reader menu header.
  uint8_t currentBookProgressPercent() const;

  bool loadBook() override;
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

  // Overrides ReaderActivity::onEnter()/onExit() rather than layering on top of them: the
  // isNews exclusion (below) must run BEFORE the base's unconditional RECENT_BOOKS.addBook(),
  // which the base's own onEnter() has no hook for. onExit() calls ReaderActivity::onExit()
  // first, then adds the reading-stats/KOReader-sync-queue/per-book-settings cleanup the base
  // doesn't know about.
  void onEnter() override;
  void onExit() override;
  void loop() override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
