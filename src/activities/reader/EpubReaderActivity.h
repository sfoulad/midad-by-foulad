#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "BookmarkEntry.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "reading/ReadingStatsStore.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
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
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action, const MenuResult& menu);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  // --- Reading-time tracking (see src/reading/ReadingStatsStore.h) ---
  // millis() when the current page became visible; 0 = timer not running.
  unsigned long pageShownAtMs = 0UL;
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

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  // Full performance while a background section build is in flight: after just
  // IDLE_POWER_SAVING_MS (3s) without a button press -- i.e. always, while the
  // user reads quietly -- the main loop drops the CPU clock and inserts a 50ms
  // tick delay, stretching each (input-blind) build chunk 3-4x and the post-open
  // watermark-rebuild storm to a minute-plus of degraded responsiveness. Race to
  // idle instead: build at full clock, then let power saving resume. Auto-sleep
  // is unaffected (its timer doesn't consult this).
  bool skipLoopDelay() override { return section && section->isBuilding(); }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
