#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <ScriptDetector.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>

#include "ArabicFontSystem.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryHistoryActivity.h"
#include "DictionaryStore.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "FouladReadingPosition.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderFontSizes.h"
#include "ReaderPomodoro.h"
#include "ReaderToolbarUi.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/reader/MidadSyncActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookReaderSettings.h"
#include "util/BookmarkFile.h"
#include "util/BookmarkUtil.h"
#include "util/ButtonNavigator.h"
#include "util/DebugLog.h"
#include "util/ReaderPerfLog.h"
#include "util/RollingSdLog.h"
#include "util/ScreenshotUtil.h"

namespace {
// The X4 Pro and X4 Classic carry the X4's panel but sit outside isXteinkDevice()
// (that helper also gates power management). Overlay refresh choices are per-panel:
// this family runs the grayscale anti-aliasing pass, so chrome painted over a
// fresh page needs the HALF ghost-cleanup and closing re-renders the page.
bool xteinkClassPanel() { return gpio.isXteinkDevice() || BoardConfig::isX4Pro() || BoardConfig::isX4Classic(); }

constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

EpubReaderActivity::~EpubReaderActivity() {
  ImageBlock::setExtractor(nullptr, nullptr);
  discardOverlayPage();  // free the overlay's page snapshot if one is held

  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

bool EpubReaderActivity::loadBook() {
  auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (!loaded) {
    LOG_ERR("ERS", "Failed to load EPUB");
    return false;
  }
  epub = std::move(loadedEpub);

  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  epub->setupCacheDir();

  // Lazy image extraction: section builds only header-probe images, so the first
  // render of an image page pulls the file out of the EPUB through this hook.
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  // Per-book reading overrides: load this book's sidecar into SETTINGS.book*
  // (cleared again in onExit). ReaderActivity already ensured the GLOBAL fonts,
  // so the font systems only need a second pass when this book overrides them.
  BookReaderSettings::applyToSettings(epub->getCachePath());
  if (SETTINGS.hasBookOverrides()) {
    sdFontSystem.ensureLoaded(renderer);
    arabicFontSystem.ensureLoaded(renderer);
  }

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  loadCachedBookmarks();
  return true;
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!Storage.exists(bookPath.c_str())) {
    LOG_ERR("ERS", "File does not exist: %s", bookPath.c_str());
    finish();
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  applyInitialOrientation();

  if (!loadBook()) {
    finish();
    return;
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  // News is deliberately not a recent book. The home screen's hero card is the most
  // recent entry, so a downloaded feed took over the "currently reading" slot with a
  // progress bar and an estimated time left -- for something that is replaced whole
  // the next time you open News. It is reached from its own tile, not from here.
  const bool isNews = isNewsBookPath(epub->getPath());
  if (!isNews) {
    RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  }

  // Reading-time tracking: open a session in the day-level stats store and start
  // the first page's dwell timer. Time is credited per page turn; the session is
  // committed and saved in onExit().
  // Nor does it count as reading. EINK_NEWS_TASKS.md is explicit that finishing a
  // feed is not finishing a book, and counting it would distort streaks and
  // books-finished -- the same reason the server refuses a feed id for stats.
  if (SETTINGS.trackReadingStats && !isNews) {
    READING_STATS.beginSession(epub->getPath(), epub->getTitle(), epub->getAuthor());
    // Capture the catalog id into the stats entry while RecentBooksStore still
    // holds it. That list caps at 10 and evicts, and the id used to live only
    // there -- so a catalog book read a while ago lost its id and could never be
    // reported to /opds/reading-stats again, leaving the server without a
    // library_reading row and the app without a cover (INSTRUCTIONS-14). Doing it
    // on open also backfills books that predate the v3 stats format, the moment
    // they are next read.
    {
      const RecentBook recent = RECENT_BOOKS.getDataFromBook(epub->getPath());
      if (!recent.fouladBookId.empty()) {
        READING_STATS.setFouladBookId(epub->getPath(), recent.fouladBookId);
      }
    }
    // One-time cleanup of the previous stats system's per-book sidecar file.
    Storage.remove((epub->getCachePath() + "/reading_stats.bin").c_str());
    paceWarmupPending = true;
    pageShownAtMs = millis();
  }

  // Consume a jump accepted on the sync screen -- but only READ it here. Applying it
  // is deferred to the first loop() iteration.
  //
  // Doing the jump inside onEnter() meant resetting `section` and taking a RenderLock
  // while the activity was still constructing itself, on the one boot that follows a
  // sync. That is exactly where a repeating panic lands: "assert failed:
  // xQueueSemaphoreTake queue.c:1709 (pxQueue)" ~791ms into the post-sync boot, on
  // two consecutive releases. A null handle with 129KB free is not an allocation
  // failure -- it is a handle taken before whatever owns it is ready.
  //
  // Every other reader action runs from loop(), after onEnter() has returned and the
  // render task is serving this activity. The jump has no reason to be the exception,
  // and one render frame later is imperceptible on e-ink.
  //
  // Still cleared and persisted here, not at apply time, so a crash between the two
  // cannot leave a device re-jumping on every open.
  if (APP_STATE.pendingSyncJumpPercent > 0) {
    pendingJumpPercent = APP_STATE.pendingSyncJumpPercent;
    pendingJumpSpine = APP_STATE.pendingSyncJumpSpine;
    APP_STATE.pendingSyncJumpPercent = 0;
    APP_STATE.pendingSyncJumpSpine = -1;
    APP_STATE.saveToFile();
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  // Bracketing the exit path, requested after three crashes whose last log line was
  // the per-book settings write -- leaving no way to tell which side of it died.
  LOG_INF("ERS", "Reader exit: begin");
  // Base resets orientation/readerActivityLoadCount/endOfBookOptions; the extractor
  // clear, footnote-safety save, and epub/section release now live in the destructor
  // (RAII, runs right after this on activity teardown -- see ~EpubReaderActivity()).
  ReaderActivity::onExit();

  // Commit this session: bank the last page's dwell, cache the time-left estimate
  // and progress for the home hero card, then let the store apply its session
  // thresholds and save (single SD write; never per page turn).
  if (SETTINGS.trackReadingStats && epub) {
    accountPageDwellForStats(false);
    uint32_t timeLeft = 0;
    const bool haveEstimate = estimateTimeLeftSeconds(timeLeft);
    const uint8_t progressPercent = currentBookProgressPercent();
    const bool finishedBook = isAtEndOfBook();
    READING_STATS.endSession(haveEstimate ? timeLeft : 0, progressPercent, finishedBook);

    // Foulad eInk device tracking (EINK_DEVICE_TRACKING_TASKS.md): opportunistic
    // report for Foulad eBooks books only. This is normally a no-op -- WiFi is
    // torn down before the reader even opens (see
    // OpdsBookBrowserActivity::onExit()), so the reliable moment this actually
    // sends is FouladDeviceTracking::flushPendingReadingStats(), called the next
    // time the device reconnects to Foulad eBooks. Attempted here too in case
    // WiFi happens to already be up (e.g. a future flow that doesn't tear it
    // down) -- cheap to check, harmless when it's not.
    const auto& recentBooksForStats = RECENT_BOOKS.getBooks();
    const auto recentIt = std::find_if(recentBooksForStats.begin(), recentBooksForStats.end(),
                                       [this](const RecentBook& b) { return b.path == epub->getPath(); });
    if (recentIt != recentBooksForStats.end() && !recentIt->fouladBookId.empty()) {
      const auto& servers = OPDS_STORE.getServers();
      const auto serverIt =
          std::find_if(servers.begin(), servers.end(), [](const OpdsServer& s) { return s.url == FOULAD_EBOOKS_URL; });
      if (serverIt != servers.end()) {
        char positionBuf[64];
        snprintf(positionBuf, sizeof(positionBuf), "spine=%d;page=%d", currentSpineIndex,
                 section ? section->currentPage : 0);
        const auto* bookStats = READING_STATS.findBook(epub->getPath());
        const uint32_t secondsRead = bookStats ? static_cast<uint32_t>(bookStats->totalReadingMs / 1000) : 0;
        FouladDeviceTracking::reportReadingStats(serverIt->username, serverIt->password, recentIt->fouladBookId,
                                                 progressPercent, positionBuf, secondsRead);

        // Cross-device position, alongside (not instead of) the per-device stats
        // above -- the two answer different questions and deliberately do not share
        // a row. Sending both on close is expected (EINK_PAGE_SYNC_TASKS.md §7).
        //
        // Closing is what makes this automatic: without it the phone only ever has
        // somewhere to sync to when a person remembers to press Sync in the drawer,
        // and the feature half-works.
        //
        // read_at only when the clock is trustworthy THIS boot. Neither device can
        // preserve a calendar date across a reboot -- the X3's DS3231 has no
        // calendar and the X4 has no RTC at all (HalClock.h) -- so after offline
        // reading there is usually nothing honest to send. Omitted, the server
        // stamps arrival; a guess would be worse, and the spec says so.
        const uint32_t readAt = HalClock::isSystemTimeValid() ? static_cast<uint32_t>(time(nullptr)) : 0;
        // Age is preferred over an absolute time (spec 4.1) and is the one number
        // this hardware can be sure of -- but only when a page was actually turned
        // while this instance was alive. Deep sleep is a full reboot here, so a
        // restored reader's pageShownAtMs marks when the book was reopened, not when
        // the person last read; sending that would understate the age badly.
        //
        // esp_sleep_get_time_in_deep_sleep() would close that gap, but it is not in
        // this ESP-IDF's esp_sleep.h, so there is nothing to measure the sleep with.
        // Rather than synthesise one, send no timing at all -- spec 4.2 makes that
        // correct now: a device supersedes its own earlier position without needing
        // a clock, which is exactly the read-offline-all-day case.
        const uint32_t ageSeconds =
            pageTurnedThisSession ? static_cast<uint32_t>((millis() - pageShownAtMs) / 1000UL) : 0;
        FouladReadingPosition::Position remote;
        FouladReadingPosition::sync(serverIt->username, serverIt->password, recentIt->fouladBookId,
                                    static_cast<float>(progressPercent), section ? section->currentPage : -1,
                                    section ? section->estimatedTotalPages() : -1, readAt, ageSeconds, remote);
      }
    }
  }

  // KOReader/MidadReader Sync: queue this session's position the same way the
  // manual "Sync" menu item uploads it (see launchKOReaderSync()), so a device
  // that's read but never pressed Sync still shows up server-side eventually.
  // Queued rather than sent live for the same reason as the Foulad reading-
  // position block above: WiFi is torn down before the reader even opens, so a
  // live attempt here would almost always no-op. FouladDeviceTracking::
  // flushPendingKOReaderSync() delivers it the next time the device reconnects
  // for any reason. Gated on pageTurnedThisSession so opening a book and
  // immediately backing out doesn't overwrite a real remote position with
  // nothing.
  if (epub && pageTurnedThisSession && KOREADER_STORE.hasCredentials()) {
    const CrossPointPosition localPos = getCurrentPosition();
    const SavedProgressPosition localProgress = ProgressMapper::toSavedProgress(epub, localPos);
    const std::string documentHash =
        KOReaderDocumentId::calculateForMatchMethod(epub->getPath(), KOREADER_STORE.getMatchMethod());
    if (!documentHash.empty()) {
      PendingKOReaderSync pending;
      pending.documentHash = documentHash;
      pending.xpath = localProgress.xpath;
      pending.percentage = localProgress.percentage;
      pending.spineIndex = static_cast<uint16_t>(localPos.spineIndex);
      pending.pageNumber = static_cast<uint16_t>(localPos.pageNumber);
      pending.totalPages = static_cast<uint16_t>(localPos.totalPages > 0 ? localPos.totalPages : 1);
      if (localPos.hasParagraphIndex) {
        pending.paragraphIndex = localPos.paragraphIndex;
      }
      KOREADER_STORE.setPendingSync(pending);
    }
  }

  // Drop this book's per-book overrides so the rest of the UI (and the next
  // book) resolves the global settings again. Cheap when the book had none:
  // ensureLoaded no-ops when the wanted family/size is already loaded.
  if (SETTINGS.hasBookOverrides()) {
    SETTINGS.clearBookOverrides();
    sdFontSystem.ensureLoaded(renderer);
    arabicFontSystem.ensureLoaded(renderer);
  }
  LOG_INF("ERS", "Reader exit: done");
}

void EpubReaderActivity::openReaderMenu() {
  pendingManualTurn = 0;
  if (usesToolbarMenu()) {
    // Reached from a child activity's result handler (footnotes, bookmarks,
    // go-to-percent... cancelled back to the menu), so the framebuffer holds
    // that screen, not the page: re-render the page and let renderBook() put
    // the toolbar on top. The in-reader fast path is openOverlay().
    overlay = Overlay::Toolbar;
    focusedTool = 0;
    panelHoldJumped = false;
    panelCursorShown = !mappedInput.hasTouch();
    if (!ensureToolbarUi()) {
      // Nothing to draw the menu with; leave the page as it is rather than aborting.
      overlay = Overlay::None;
      return;
    }
    toolbarUi->begin();
    discardOverlayPage();
    requestUpdate();
    return;
  }
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  // Script-aware menu: an Arabic book gets the Arabic font rows, anything else
  // the English ones. Title script is the primary signal (dc:language metadata
  // is often missing or wrong in Arabic EPUBs); Arabic-script language codes
  // cover Arabic-titled books published under a Latin title.
  const std::string& lang = epub->getLanguage();
  const bool isArabicBook = ScriptDetector::containsArabic(epub->getTitle().c_str()) || lang.rfind("ar", 0) == 0 ||
                            lang.rfind("fa", 0) == 0 || lang.rfind("ur", 0) == 0;
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                             renderer, mappedInput, epub.get(), currentSpineIndex, currentPage, totalPages,
                             bookProgressPercent, SETTINGS.orientation, !currentPageFootnotes.empty(),
                             !cachedBookmarks.empty(), isArabicBook, bookHasFouladId()),
                         [this](const ActivityResult& result) {
                           // Always apply orientation / auto-turn / per-book setting edits, even
                           // if the menu was cancelled (Back just closes the drawer).
                           const auto& menu = std::get<MenuResult>(result.data);
                           LOG_INF("ERS", "Drawer closed: action=%d cancelled=%d settingsChanged=%d chapterSpine=%d",
                                   menu.action, result.isCancelled ? 1 : 0, menu.bookSettingsChanged ? 1 : 0,
                                   menu.chapterSpineIndex);
                           applyOrientation(menu.orientation);
                           toggleAutoPageTurn(menu.pageTurnOption);
                           if (menu.bookSettingsChanged && epub) {
                             BookReaderSettings::saveFromSettings(epub->getCachePath());
                             {
                               // Same mid-book re-layout dance as applyOrientation(): preserve the
                               // position, reload the font systems for the new per-book values, and
                               // reset the section -- the changed fontId/arabicFontId/lineCompression/
                               // alignment in the section cache key forces a rebuild, and
                               // applyDeferredReposition() remaps the page once the count is known.
                               RenderLock lock(*this);
                               if (section) {
                                 cachedSpineIndex = currentSpineIndex;
                                 cachedChapterTotalPageCount = section->pageCount;
                                 nextPageNumber = section->currentPage;
                               }
                               sdFontSystem.ensureLoaded(renderer);
                               arabicFontSystem.ensureLoaded(renderer);
                               section.reset();
                             }
                             requestUpdate();
                           }
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action), menu);
                           }
                         });
}

namespace {
// Current CPU clock for the perf log: distinguishes "layout is slow" from
// "layout ran at the power-saving clock" -- indistinguishable from timings
// alone (see EpubReaderActivity::skipLoopDelay).
unsigned cpuMhzNow() {
#ifdef SIMULATOR
  return 0;
#else
  return static_cast<unsigned>(getCpuFrequencyMhz());
#endif
}
}  // namespace

void EpubReaderActivity::showBuildPopup() {
  // Mid-build indexing popup: only during render()'s blocking build-to-target
  // phase (buildPopupPending), at most once, and only when the framebuffer
  // isn't on loan. If it fires while the loan is active (e.g. the parser's
  // size-based call during startBuild), pending stays set and the deadline
  // check retries after the loan. The parser's popup callback lives as long
  // as the build and keeps firing from background buildSomeMore ticks;
  // without the buildPopupPending gate it would draw over an already-
  // displayed page.
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (!DICTIONARIES.hasActiveDictionary()) {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  // A lookup ends back on the page no matter how it was opened (menu or
  // long-press): the user is mid-reading, not mid-menu.
  int marginLeft = 0, marginTop = 0;
  if (auto page = loadCurrentPageForLookup(marginLeft, marginTop)) {
    startActivityForResult(
        std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                       SETTINGS.getReaderFontId(), marginLeft, marginTop),
        [this](const ActivityResult&) { requestUpdate(); });
  } else {
    requestUpdate();
  }
}

void EpubReaderActivity::loop() {
  // Deferred cross-device jump (see onEnter). Runs once, on the first iteration
  // after the activity is fully up and the render task is serving it.
  if (pendingJumpPercent > 0) {
    const int target = pendingJumpPercent;
    const int targetSpine = pendingJumpSpine;
    pendingJumpPercent = 0;
    pendingJumpSpine = -1;
    // Spine only when documents are fine-grained enough to beat the percentage.
    // Each covers roughly 100/spineCount percent: on a 103-document book that is ~1%
    // and the spine wins, but on a three-document book "open document 3" lands near
    // 67% for a position 22% in -- worse than the percentage it replaced.
    const int spineCount = epub ? epub->getSpineItemsCount() : 0;
    if (targetSpine >= 0 && spineCount >= 30) {
      LOG_INF("SYNC", "Applying cross-device jump to spine=%d of %d (%d%%)", targetSpine, spineCount, target);
      jumpToSpine(targetSpine);
    } else if (targetSpine >= 0) {
      LOG_INF("SYNC", "Spine anchor %d of %d too coarse; using %d%% instead", targetSpine, spineCount, target);
      jumpToPercent(target);
    } else {
      LOG_INF("SYNC", "Applying cross-device jump to %d%% (no spine anchor)", target);
      jumpToPercent(target);
    }
    // Spine only; both jump paths reset `section` for the next render to rebuild, so
    // pagination is always unresolved at this point. The previous version of this log
    // printed "page=-1 of -1" every time and was reasonably read as evidence of a race.
    LOG_INF("SYNC", "Jump applied: spine=%d, pagination resolves on next render", currentSpineIndex);
    return;  // let the jump's own reload land before handling input this frame
  }

  if (!epub) {
    finish();
    return;
  }

  // Someone else turned the screen while this reader was stacked (the control
  // center's orientation tile). Reflow before the next render, or the page
  // would be drawn with a layout built for the previous frame size.
  if (appliedOrientation != SETTINGS.orientation) {
    applyOrientation(SETTINGS.orientation);
    requestUpdate();
    return;
  }

  // Background section build tick. Runs only on ticks with NO button input:
  // a page-turn (or any) press is handled first and never waits behind a
  // build chunk -- pressing forward through a long, still-indexing surah used
  // to queue each turn behind layout work (user report: Quran page flips
  // "very slow").
  bool anyButtonEvent = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  for (const auto b :
       {MappedInputManager::Button::Back, MappedInputManager::Button::Confirm, MappedInputManager::Button::Left,
        MappedInputManager::Button::Right, MappedInputManager::Button::Up, MappedInputManager::Button::Down,
        MappedInputManager::Button::PageBack, MappedInputManager::Button::PageForward,
        MappedInputManager::Button::NavNext, MappedInputManager::Button::NavPrevious}) {
    // isPressed too (not just edges): a HELD button must never wait behind a
    // build chunk -- auto-repeat page turns and long-presses stay responsive.
    anyButtonEvent =
        anyButtonEvent || mappedInput.wasPressed(b) || mappedInput.wasReleased(b) || mappedInput.isPressed(b);
  }
  if (!anyButtonEvent) {
    // Idle glyph prewarm for the likely next page (currentPage + 1). The scan
    // pass draws nothing (FCM scan mode suppresses text pixels and ImageBlock
    // skips itself while scanning), so the displayed framebuffer is untouched;
    // endScanAndPrewarm loads only glyphs not already cached. Debounced past
    // rapid page-flipping, one attempt per position, and deferred while a
    // render/build owns the CPU or the heap is near the render floors.
    // Cross-chapter prewarm is deliberately out of scope (the next spine's
    // section isn't loaded). Mutually exclusive with the build ticks below:
    // this only runs once the section has finished building.
    constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
    if (section && !section->isBuilding() && !RenderLock::peek() && renderer.hasFrameBuffer() &&
        lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
        ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > RENDER_MIN_LARGEST_BLOCK &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      RenderLock lock;  // the page table must not change under the scan
      // Re-check under the lock: peek() and acquisition are not atomic, so the render
      // task may have reset/replaced the section or moved the page in between. cppcheck
      // can't see the cross-task mutation, so it flags this as always true.
      // cppcheck-suppress knownConditionTrueFalse
      if (section && !section->isBuilding() &&
          (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
        idlePrewarmSpine = currentSpineIndex;
        idlePrewarmPage = section->currentPage;
        const int nextPage = section->currentPage + 1;
        if (nextPage < static_cast<int>(section->pageCount)) {
          if (const auto p = section->loadPage(nextPage)) {
            if (auto* fcm = renderer.getFontCacheManager()) {
              const auto t0 = millis();
              auto scope = fcm->createPrewarmScope();
              p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);  // scan only, no pixels
              scope.endScanAndPrewarm();
              LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
            }
          }
        }
      }
    }

    // Lazily resume a partial's extension build once the reader nears its watermark. Far from
    // it the rebuild is all cost (whole-chapter re-layout from page 0) and no benefit this
    // session, so reopening a partial deliberately does NOT start it (see the deferral in
    // render()); crossing this margin is the signal that the reader will actually need pages
    // past the watermark soon. Uses the last render's viewport so pagination matches the
    // partial being extended.
    if (section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 &&
        !partialRebuildStartFailed &&
        section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
      RenderLock lock;
      // Reuse the last render's viewport so the extension paginates identically to the partial.
      const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
      if (!section->startBuild(buildSpec)) {
        // Not fatal: the partial keeps serving its pages; crossing the watermark falls back to
        // the blocking extension in render(). Don't retry every tick.
        partialRebuildStartFailed = true;
        LOG_ERR("ERS", "Failed to start deferred partial extension build");
      } else {
        LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
                section->pageCount);
      }
    }

    // Drive any in-progress incremental section build forward, off the page-turn critical path,
    // but only within a small window ahead of the reader: an unbounded build monopolized the
    // RenderLock and locked out page turns. The build follows the reader instead, and instant
    // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
    // Skip while the render mutex is busy so we never delay a pending render; re-check
    // isBuilding() under the lock since render() may have just finished it.
    // While extending a partial (rebuild from a previous session), pageCount is pinned at the
    // partial's watermark until the build catches up, so the window check would wrongly read
    // "far enough ahead" and stall the build at 0 pages -- then the first turn past the
    // watermark re-parses the whole chapter synchronously. Keep ticking until it finalizes.
    // Heap gate: while a build is active, buildHeapPaused always reflects the CURRENT heap
    // reading, independent of whether this particular tick would otherwise attempt a chunk
    // (render-lock busy / already caught up to BUILD_WINDOW_AHEAD) -- skipLoopDelay() reads
    // this every frame, so a stale "paused" left over from a tick that skipped for an
    // unrelated reason would wrongly suppress the CPU race-to-idle once the real gate clears.
    buildHeapPaused = section && section->isBuilding() &&
                      (ESP.getFreeHeap() < BUILD_TICK_MIN_FREE_HEAP_BYTES ||
                       ESP.getMaxAllocHeap() < BUILD_TICK_MIN_LARGEST_BLOCK_BYTES);

    if (section && section->isBuilding() && !buildHeapPaused && !RenderLock::peek() &&
        (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD)) {
      RenderLock lock;
      // Re-check under the lock: render() (which also holds the RenderLock) may have finalized the
      // build between the outer isBuilding() check and acquiring the lock here, in which case
      // buildSomeMore() would fail and wrongly reset the section. The heap gate must be re-read
      // too: a render that won the lock race can expand retained glyph buffers, invalidating the
      // pre-lock heap reading. cppcheck can't see the cross-task mutation, so it flags this as
      // always true.
      // cppcheck-suppress knownConditionTrueFalse
      if (section->isBuilding() && !buildHeapPaused) {
        // Diagnostic: while this chunk runs, loop() is input-blind (see the
        // BUILD_PAGES_PER_CHUNK comment in the header). Log any chunk long
        // enough to swallow a quick tap so a "buttons went dead mid-book"
        // report lines up against concrete blocked windows in the perf log.
        const unsigned long bgChunkStart = millis();
        if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK, BACKGROUND_BUILD_BUDGET_MS)) {
          LOG_ERR("ERS", "Background section build failed");
          section.reset();
          requestUpdate();
        } else if (const unsigned long bgChunkMs = millis() - bgChunkStart; bgChunkMs > 500) {
          // With the 250ms budget a healthy chunk stays well under this; anything
          // logged here means one parseStep (a single paragraph/image) overshot
          // the budget by itself -- worth seeing in a device log.
          char buf[144];
          snprintf(buf, sizeof(buf), "%lu bg_chunk=%lums spine=%d pages=%u heap=%u max=%u cpu=%u", millis(), bgChunkMs,
                   currentSpineIndex, section ? (unsigned)section->pageCount : 0u, (unsigned)ESP.getFreeHeap(),
                   (unsigned)ESP.getMaxAllocHeap(), cpuMhzNow());
          ReaderPerfLog::append(buf);
        }
        if (section && section->isBuildComplete() && applyDeferredReposition()) {
          // The chapter re-paginated since the saved progress (settings changed): we now know the
          // real page count, so re-render at the remapped page. No-op for an unchanged resume.
          requestUpdate();
        }
      }
    }
  }

  const bool atEndOfBook = isAtEndOfBook();
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  // Pomodoro expiry. consumeJustExpired() is a millis() comparison and latches, so this
  // costs nothing on every other tick and fires exactly once -- the ONLY refresh the
  // countdown ever asks for. Everything else about it rides along on repaints the reader
  // was already doing.
  if (READER_POMODORO.consumeJustExpired()) {
    flashPomodoroAlert();
    // Repaint the page so the footer swaps the countdown for the phase-done message.
    // The page content is unchanged, so this is a plain refresh, not a re-layout.
    requestUpdate();
    return;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  // The toolbar reader menu owns all input while shown, ahead of the automatic page turn
  // below: the More panel's rate popup switches automatic turning on and leaves the panel
  // open, so the timer must neither flip the page under it nor eat the panel's next
  // Confirm/Back release.
  if (overlay != Overlay::None) {
    if (usesToolbarMenu()) {
      // Hold the interval at zero elapsed so closing the panel starts a fresh one.
      lastPageTurnTime = millis();
      handleOverlayInput();
      return;
    }
    // The style was switched off while an overlay was up (Settings reached via
    // the More panel); fall back to the clean page.
    overlay = Overlay::None;
    discardOverlayPage();
    requestUpdate();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      requestUpdate();
      return;
    }
  }

  // While the end-of-book suggestion menu is up it owns Confirm/Back/navigation, so it
  // gets this tick's input first and the long-press shortcuts below stay inert behind it
  // -- a hold there must not drop a bookmark onto the suggestion screen or paint the
  // dictionary word picker over it. Anything the menu does not handle (long-press Back to
  // the file browser, say) still falls through to the regular handlers.
  if (handleEndOfBookMenu()) {
    return;
  }
  const bool endOfBookMenuOpen = endOfBookMenuActive();

  const unsigned long confirmHoldMs = confirmLongPressThreshold();
  // wasLongPressed() suppresses the release that follows it, so leave it unpolled while
  // the end-of-book menu owns Confirm -- otherwise the menu never sees that release.
  const bool confirmLongPressed = !endOfBookMenuOpen && confirmHoldMs != 0 &&
                                  mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, confirmHoldMs);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmLongPressed) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        addBookmark();
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (launchKOReaderSync()) {
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        openDictionaryWordSelect();
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Home-key boards have no front Confirm button, so a Home-key hold runs the
  // same user-selected long-press action. The SDK emits this event once per
  // hold and suppresses the short Home tap for the same contact.
  if (mappedInput.wasHomeKeyHold() && !endOfBookMenuOpen) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case CrossPointSettings::LP_MENU_KOSYNC:
        launchKOReaderSync();
        return;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (!showDictionaryMessage) {
          openDictionaryWordSelect();
        }
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
        if (usesToolbarMenu() && section) {
          openOverlay(Overlay::Toolbar);
        } else {
          openReaderMenu();
        }
        return;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  if (confirmReleased || ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    // Toolbar style: the page is on screen and in the framebuffer, so paint the
    // toolbar over it (one refresh) instead of pushing a full-screen menu.
    if (usesToolbarMenu() && section) {
      pendingManualTurn = 0;
      openOverlay(Overlay::Toolbar);
    } else {
      openReaderMenu();
    }
  }

  // Short press Back restores position when viewing a footnote (takes priority over navigation)
  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES && mappedInput.wasShortPowerClick() &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section) {
      pendingManualTurn = 0;
      return;
    }
    const bool forward = pendingManualTurn > 0;
    pendingManualTurn = 0;
    pageTurn(forward);
    requestUpdate();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs >= ReaderUtils::SKIP_HOLD_MS;
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    skipPages(nextTriggered ? 1 : -1);
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (!section) {
    requestUpdate();
    return;
  }

  if (turnGuardActive) {
    pendingManualTurn = prevTriggered ? -1 : 1;
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
  requestUpdate();
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }
  statsOnJump();

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return;

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) targetSize = bookSize - 1;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPageJump.reset();  // see the invariant at its declaration
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::jumpToSpine(const int spineIndex) {
  if (!epub) return;
  const int spineCount = epub->getSpineItemsCount();
  // Out of range means the anchor does not describe this copy of the book -- a
  // different edition, or a feed and a file that disagree. Refuse rather than clamp:
  // landing at the end of the wrong book is not better than not jumping.
  if (spineIndex < 0 || spineIndex >= spineCount) {
    LOG_ERR("SYNC", "Spine anchor %d outside this book's %d items; ignoring", spineIndex, spineCount);
    return;
  }
  statsOnJump();
  // Start of the document. The percentage could position within it, but on a book
  // where each document is a page that is already exact, and on one with long
  // chapters the chapter opening is the honest answer -- a percentage of the WHOLE
  // book says nothing about where inside a chapter to stop.
  pendingSpineProgress = 0.0f;
  {
    RenderLock lock(*this);
    currentSpineIndex = spineIndex;
    nextPageNumber = 0;
    pendingPageJump.reset();  // see the invariant at its declaration
    pendingPercentJump = true;
    section.reset();
  }
}

std::shared_ptr<Page> EpubReaderActivity::loadCurrentPageForLookup(int& outMarginLeft, int& outMarginTop) {
  if (!section) return nullptr;

  // Same oriented-margin computation render() uses (see its comment for the
  // per-orientation/status-bar reasoning) -- duplicated rather than shared
  // because render()'s locals aren't otherwise exposed, and this is a small,
  // stable block.
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  marginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  outMarginLeft = marginLeft;
  outMarginTop = marginTop;

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) return nullptr;
  return section->loadPage(section->currentPage);
}

// Menu-launched sub-screens return one level to the reader menu when
// cancelled (Back button or back gesture, identical on touch and button
// devices), instead of dropping to the reading surface. Home-gesture exits
// never run these handlers (ActivityManager replaces the stack), so they
// cannot re-open the menu.
void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action, const MenuResult& menu) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
    } else {
      const auto& sync = std::get<ProgressChangeResult>(result.data);

      if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
        RenderLock lock;
        clearDeferredReposition();
        if (section && currentSpineIndex == sync.spineIndex) {
          const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
          section->currentPage = page.value_or(std::max(0, sync.page));
        } else {
          currentSpineIndex = sync.spineIndex;
          pendingOffsetJump = sync.visibleTextOffset;
          nextPageNumber = std::max(0, sync.page);
          section.reset();
        }
        requestUpdate();
        return;
      }

      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      RenderLock lock;
      clearDeferredReposition();

      if (currentSpineIndex != targetSpineIndex) {
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
      requestUpdate();
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::POMODORO: {
      // One row, three meanings, matching the label the drawer showed: idle starts a
      // session, a finished phase is acknowledged into the next one, and a running
      // session stops. No requestUpdate() -- the drawer closing already repaints the
      // page, and that repaint picks up the new footer state for free.
      auto& pomodoro = READER_POMODORO;
      if (!pomodoro.isActive()) {
        pomodoro.start();
        reserveStatusBarSpaceIfHidden();
      } else if (pomodoro.isFinished()) {
        pomodoro.advancePhase();
      } else {
        pomodoro.stop();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      // The chapter was already picked from the in-drawer TOC list.
      if (menu.chapterSpineIndex >= 0) {
        statsOnJump();
        RenderLock lock(*this);

        currentSpineIndex = menu.chapterSpineIndex;

        // If anchor is not empty, it will be used later to calculate the page number.
        pendingAnchor = menu.chapterAnchor;

        // Otherwise page 0 will be used.
        nextPageNumber = 0;
        pendingPageJump.reset();  // see the invariant at its declaration

        section.reset();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 openReaderMenu();
                                 return;
                               }
                               const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                               navigateToHref(footnoteResult.href, true);
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult&) { openReaderMenu(); });
          break;
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock;
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
          // The per-book settings sidecar lived in the cleared cache dir; the
          // overrides are still in RAM, so re-persist them like progress above.
          BookReaderSettings::saveFromSettings(epub->getCachePath());
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock;
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::MIDAD_SYNC: {
      launchMidadSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP_WORD: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP_HISTORY: {
      int marginLeft = 0, marginTop = 0;
      // History's own definition viewer needs a page purely as render context
      // (for images-in-background etc.); nullptr is acceptable if the page
      // fails to load -- history/definitions still work without it.
      auto page = loadCurrentPageForLookup(marginLeft, marginTop);
      startActivityForResult(
          std::make_unique<DictionaryHistoryActivity>(renderer, mappedInput, std::move(page),
                                                      SETTINGS.getReaderFontId(), marginLeft, marginTop),
          [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    default:
      // FONT_SIZE / FONT_NAME / TEXT_ALIGN / LINE_SPACING / RESET_BOOK_SETTINGS /
      // MORE are handled inside the drawer and never arrive here.
      break;
  }
}

unsigned long EpubReaderActivity::confirmLongPressThreshold() const {
  switch (SETTINGS.longPressMenuFunction) {
    case CrossPointSettings::LP_MENU_BOOKMARK:
    case CrossPointSettings::LP_MENU_DICTIONARY:
      return ReaderUtils::BOOKMARK_HOLD_MS;
    case CrossPointSettings::LP_MENU_KOSYNC:
      return KOREADER_STORE.hasCredentials() ? ReaderUtils::GO_HOME_MS : 0;
    case CrossPointSettings::LP_MENU_READER_MENU:
    case CrossPointSettings::LP_MENU_DISABLED:
    default:
      return 0;
  }
}

std::string EpubReaderActivity::currentBookFouladId() const {
  if (!epub) return "";
  // Stats store first, recents second. The id lives in ReadingBookStats precisely
  // because RECENT_BOOKS caps at 10 and evicts (see INSTRUCTIONS-14) -- looking it
  // up only in recents would make Sync vanish for a catalog book read a while ago,
  // which is the same bug that hid reading from library_reading rows.
  READING_STATS.ensureLoaded();
  if (const auto* stats = READING_STATS.findBook(epub->getPath()); stats && !stats->fouladBookId.empty()) {
    return stats->fouladBookId;
  }
  const auto& recents = RECENT_BOOKS.getBooks();
  const auto it =
      std::find_if(recents.begin(), recents.end(), [this](const RecentBook& b) { return b.path == epub->getPath(); });
  return it != recents.end() ? it->fouladBookId : "";
}

bool EpubReaderActivity::bookHasFouladId() const {
  // The Sync row is offered only for a book the catalog can be told about.
  // launchMidadSync() requires the same id and returns silently without one, so
  // gating the row on anything weaker (an account existing, say) produces a menu
  // entry that does nothing at all when pressed -- which is exactly what shipped
  // in v1.8.5-rc and was reported as "nothing happens".
  //
  // Empty for a side-loaded file, and also for a catalog book downloaded before
  // the id was recorded or since evicted from the 10-entry recents list.
  return !currentBookFouladId().empty();
}

void EpubReaderActivity::launchMidadSync() {
  if (!epub) return;

  // Empty id is not an early return any more: the sync screen says why. The id is
  // only recorded when a book is opened THROUGH Library (see
  // OpdsBookBrowserActivity), so a catalog book that has only ever been opened from
  // Home legitimately has none -- indistinguishable, from the outside, from a
  // side-loaded file or a broken feature.
  const std::string bookId = currentBookFouladId();

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  const float percent = static_cast<float>(currentBookProgressPercent());
  // Only when a page was genuinely turned while this instance was alive -- see the
  // close-sync block in onExit() for why a restored reader's timestamp is not an age.
  const uint32_t ageSeconds = pageTurnedThisSession ? static_cast<uint32_t>((millis() - pageShownAtMs) / 1000UL) : 0;
  const std::string savedEpubPath = epub->getPath();
  // Read before the Epub is released below; the resolver needs them to search.
  const std::string savedTitle = epub->getTitle();
  const std::string savedAuthor = epub->getAuthor();

  // Persist first: the reader is replaced below and resumes from this file, so a
  // failed write would silently lose the reader's place. Same guard as KOReader's.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("SYNC", "Aborting Midad sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return;
  }

  // Release Epub and Section (~65KB) before the handshake, exactly as the KOReader
  // path does. A TLS session cannot be afforded alongside a loaded book, and this is
  // the whole reason sync is a separate activity rather than a call from here.
  LOG_DBG("SYNC", "Releasing epub for Midad sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    // The image extractor holds a raw pointer into this epub (see onEnter);
    // clear it before the early release, mirroring onExit(), or a later image
    // render would call through a dangling context.
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }
  LOG_DBG("SYNC", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<MidadSyncActivity>(renderer, mappedInput, savedEpubPath, bookId,
                                                                      savedTitle, savedAuthor, percent, currentPage,
                                                                      totalPages, ageSeconds));
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock;
    if (section) {
      nextPageNumber = section->currentPage;
    }
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;
}

void EpubReaderActivity::applyInitialOrientation() {
  ReaderActivity::applyInitialOrientation();
  appliedOrientation = SETTINGS.orientation;
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // Also runs when SETTINGS already holds the new value but this layout was
  // built for the old one — that is what an external change looks like here.
  if (SETTINGS.orientation == orientation && appliedOrientation == orientation) {
    return;
  }

  RenderLock lock(*this);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  if (SETTINGS.orientation != orientation) {
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  appliedOrientation = orientation;
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  reserveStatusBarSpaceIfHidden();
}

void EpubReaderActivity::flashPomodoroAlert() {
  // The screen is the only alert channel this hardware has -- no buzzer, speaker or
  // vibration motor exists anywhere in the firmware or the SDK (see
  // StopwatchActivity::flashAlert, which hit the same ceiling).
  //
  // Two passes where the Pomodoro app uses three, and shorter ones: that app is alerting
  // someone who is watching a timer, this interrupts someone mid-sentence. The footer
  // keeps saying the phase is done afterwards, so the flash only has to pull the eye up,
  // not carry the whole message. FULL_REFRESH each way because a partial pass leaves the
  // inversion streaked rather than clean.
  //
  // Holds the render lock for the duration: this drives the framebuffer directly from
  // loop(), and the render task painting a page turn into the middle of the sequence
  // would leave the screen inverted.
  RenderLock lock(*this);
  for (int i = 0; i < 2; ++i) {
    renderer.invertScreen();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    delay(150);
    renderer.invertScreen();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    delay(150);
  }
}

// A status bar that is hidden, or drawn as a bare progress bar, has no text lane for an
// indicator to live in -- the page's text is laid out over that space. Anything that
// needs to write there (the auto-page-turn rate, the Pomodoro countdown) has to give the
// lane back first, which means a re-layout. Only done when the lane is genuinely absent:
// with a normal status bar this is a no-op and the indicator costs nothing at all.
void EpubReaderActivity::reserveStatusBarSpaceIfHidden() {
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight != 0 && statusBarHeight != UITheme::getInstance().getProgressBarHeight()) {
    return;
  }
  // Preserve current reading position so we can restore after reflow.
  RenderLock lock(*this);
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  section.reset();
}

void EpubReaderActivity::accountPageDwellForStats(const bool isForwardTurn) {
  if (!SETTINGS.trackReadingStats || pageShownAtMs == 0UL) {
    return;
  }
  const uint32_t elapsed = static_cast<uint32_t>((millis() - pageShownAtMs) / 1000UL);
  pageShownAtMs = millis();
  // A page was genuinely turned while this instance was alive, so pageShownAtMs now
  // marks a real reading moment rather than the time the book happened to be opened.
  // That is the difference between an age we can send and one we cannot -- see the
  // close-sync block in onExit().
  pageTurnedThisSession = true;
  if (elapsed == 0 || elapsed > READING_IDLE_THRESHOLD_SECONDS) {
    // Zero-second flicks aren't reading; anything past the idle threshold means the
    // reader was set aside with the page open -- discard rather than inflate stats.
    return;
  }
  READING_STATS.addReadingTime(elapsed);
  if (isForwardTurn) {
    if (paceWarmupPending) {
      // First dwell after open/jump includes setup, not just reading.
      paceWarmupPending = false;
    } else if (elapsed >= MIN_PACE_SAMPLE_SECONDS) {
      READING_STATS.recordForwardPage(elapsed);
    }
  }
}

void EpubReaderActivity::statsOnJump() {
  accountPageDwellForStats(false);
  paceWarmupPending = true;
}

uint8_t EpubReaderActivity::currentBookProgressPercent() const {
  if (!epub || epub->getBookSize() == 0 || !section || section->pageCount == 0) {
    return 0;
  }
  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
  const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  const int pct = static_cast<int>(bookProgress + 0.5f);
  return static_cast<uint8_t>(std::clamp(pct, 0, 100));
}

bool EpubReaderActivity::estimateTimeLeftSeconds(uint32_t& seconds) const {
  seconds = 0;
  const uint32_t pace = READING_STATS.activeBookPace();
  if (pace == 0 || !epub || !section || section->pageCount == 0) {
    return false;
  }
  if (currentSpineIndex >= epub->getSpineItemsCount()) {
    return false;  // end-of-book screen
  }
  // Pages left in the current section, then the rest of the spine is estimated by
  // scaling its remaining bytes with this section's bytes-per-page, since later
  // sections haven't been paginated yet. Must use estimatedTotalPages(), not the
  // raw pageCount: while a chapter is still incrementally building, pageCount is
  // only "pages laid out so far" (a watermark trailing currentPage), not the
  // chapter's true total -- using it directly collapsed both sectionPagesLeft and
  // bytesPerPage toward the current chapter alone, making the estimate ignore the
  // rest of the book entirely.
  const uint32_t sectionTotalPages = section->estimatedTotalPages();
  const uint32_t sectionPagesLeft =
      section->currentPage < sectionTotalPages ? (sectionTotalPages - section->currentPage - 1) : 0;
  const uint32_t cumulativeEnd = epub->getSpineItem(currentSpineIndex).cumulativeSize;
  const uint32_t cumulativeStart = currentSpineIndex > 0 ? epub->getSpineItem(currentSpineIndex - 1).cumulativeSize : 0;
  const uint32_t sectionBytes = cumulativeEnd > cumulativeStart ? cumulativeEnd - cumulativeStart : 0;
  const uint32_t bookSize = static_cast<uint32_t>(epub->getBookSize());
  const uint32_t remainingBytes = bookSize > cumulativeEnd ? bookSize - cumulativeEnd : 0;
  float remainingPages = static_cast<float>(sectionPagesLeft);
  if (sectionBytes > 0 && remainingBytes > 0) {
    const float bytesPerPage = static_cast<float>(sectionBytes) / static_cast<float>(sectionTotalPages);
    if (bytesPerPage > 0.0f) {
      remainingPages += static_cast<float>(remainingBytes) / bytesPerPage;
    }
  }
  if (remainingPages <= 0.0f) {
    return false;
  }
  const float estimate = remainingPages * static_cast<float>(pace);
  seconds = static_cast<uint32_t>(std::min(estimate + 0.5f, 4294967040.0f));
  return seconds > 0;
}

bool EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (!section) return false;
  accountPageDwellForStats(isForwardTurn);
  // A page turn is authoritative: do not let a resume/reflow position captured
  // at session start snap the reader back after the incremental build completes.
  {
    RenderLock lock;
    clearDeferredReposition();
  }
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
      lastPageTurnTime = millis();
      return true;
    } else {
      // Reached the end of the book: release this chapter's Section (~tens of KB) --
      // nothing re-reads it while the end-of-book screen is showing.
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump.reset();  // see the invariant at its declaration
      currentSpineIndex++;
      section.reset();
      lastPageTurnTime = millis();
      return true;
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      currentSpineIndex--;
      section.reset();
      lastPageTurnTime = millis();
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::skipPages(int amount) {
  if (!section) return false;
  if (amount > 0) {
    RenderLock lock;
    nextPageNumber = 0;
    pendingPageJump.reset();  // see the invariant at its declaration
    currentSpineIndex++;
    section.reset();
    return true;
  } else {
    if (section->currentPage > 0) {
      section->currentPage = 0;
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump.reset();  // see the invariant at its declaration
      currentSpineIndex--;
      section.reset();
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (epub && epub->getSpineItemsCount() > 0) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
  }
}

bool EpubReaderActivity::skipLoopDelay() {
  return section && section->isBuilding() && !buildHeapPaused &&
         (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
}

namespace {
// Blocked-window diagnostic: while a blocking section build runs, loop() isn't
// polling buttons, so a quick tap whose press AND release both land inside the
// window is silently lost -- reads as "buttons dead mid-book" on device. Log any
// window long enough to swallow a tap so such a report lines up against concrete
// entries in the SD perf log (debug-gated + heap-guarded by RollingSdLog).
void logSlowBlockingBuild(const char* tag, unsigned long startMs, int spineIndex, const Section* section) {
  const unsigned long ms = millis() - startMs;
  if (ms <= 750) return;
  char buf[144];
  snprintf(buf, sizeof(buf), "%lu %s=%lums spine=%d pages=%u heap=%u max=%u cpu=%u", millis(), tag, ms, spineIndex,
           section ? (unsigned)section->pageCount : 0u, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
           cpuMhzNow());
  ReaderPerfLog::append(buf);
}
}  // namespace

void EpubReaderActivity::renderBook() {
  if (!epub) return;

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for the automatic page turn indicator (or the Pomodoro countdown --
  // both write into the status bar's text lane) when no status bar or progress bar only
  if ((automaticPageTurnActive || READER_POMODORO.isActive()) &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    const unsigned long chapterLoadStartMs = millis();
    // Zero SD-card font read/seek stats before a fresh build so the "built" log
    // line below reports THIS chapter's SD I/O, not whatever leaked over from
    // the previous page turn's render-time prewarm.
    if (auto* fcmForBuildStats = renderer.getFontCacheManager()) fcmForBuildStats->resetStats();
    section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer);
    if (!section) {
      // Bare `new` under -fno-exceptions calls abort() on OOM instead of returning nullptr
      // (see CLAUDE.md) -- this is the primary chapter-load path, crossed on every chapter
      // transition, so it gets the same makeUniqueNoThrow treatment as every other fallible
      // allocation. Leaving `section` null and returning lets the next render() tick retry
      // once whatever fragmented the heap has cleared, the same trade already made by
      // hasHeapForNavigation()/hasHeapForCoverWork() in the OPDS browser.
      LOG_ERR("ERS", "OOM building Section for spine index %d: free=%u largest=%u", currentSpineIndex,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      RollingSdLog::append(DebugLog::PATH,
                           "[ERS] OOM building Section spine=" + std::to_string(currentSpineIndex) + " free=" +
                               std::to_string(ESP.getFreeHeap()) + " largest=" + std::to_string(ESP.getMaxAllocHeap()),
                           DebugLog::MAX_LINES, /*force=*/true);
      return;
    }
    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;

    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      // Pagination of a large chapter is the reader's peak-RAM moment (expat +
      // CSS + hyphenation + page assembly); an on-device crash report showed
      // abort() during a rebuild right after a corrupt section file was
      // discarded. Drop the decompressed-glyph cache first (tens of KB after
      // browsing; repopulates with exactly the working set as pagination
      // measures text) -- same relief valve the OTA flow uses. Reading stats
      // stay put: releaseMemory() is a mid-session no-op by design.
      if (renderer.getFontCacheManager() != nullptr) {
        renderer.getFontCacheManager()->clearCache();
      }
      LOG_INF("ERS", "Building section (free heap: %u, largest block: %u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

      // Jumps that need the final pagination or the anchor map -- explicit page jumps,
      // fragment anchors, percent jumps, and cross-setting progress repositioning -- can't
      // resolve their landing page until the whole chapter is laid out, so they take the full
      // (blocking) build with the indexing popup. Everything else -- plain forward reads, resume,
      // and explicit page jumps -- only needs a specific page, so it builds incrementally to that
      // page and finishes the rest in loop(). The settings-change reposition (cachedChapterTotal*)
      // is NOT a full-build trigger: it's deferred to applyDeferredReposition() once the real page
      // count is known, so it never blocks the first page.
      // Only a percent jump truly needs the whole chapter up front (percent -> page needs the final
      // page count). Anchor jumps (TOC / chapter select / footnotes) resolve incrementally below --
      // the anchor is recorded as its page is laid out, so a chapter-top anchor lands on page 0
      // without indexing the whole chapter.
      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();
          showBuildError();
          return;
        }
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup();
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
        buildPopupPending = false;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    {
      // "cache" = pagination was already on disk (finalized or partial-then-
      // extended); "built" = this open ran the HTML/CSS/layout pipeline from
      // scratch. A run of "built" entries for the SAME book is the signature
      // of a section cache that never sticks (e.g. a settings/font mismatch
      // re-triggering a rebuild every open) rather than genuinely slow layout.
      char buf[320] = "";
      snprintf(buf, sizeof(buf), "%lu spine=%d %s elapsed=%lums heap=%u max=%u cpu=%u pages=%u", millis(),
               currentSpineIndex, cacheComplete ? "cache" : "built", millis() - chapterLoadStartMs,
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), cpuMhzNow(), (unsigned)section->pageCount);
      // SD-card font read/seek stats accumulated during THIS build (see the
      // resetStats() call above) -- a "built" line's elapsed time is dominated
      // by SD I/O when a custom SD-card font (esp. a separately-configured
      char titlePart[64];
      snprintf(titlePart, sizeof(titlePart), " \"%s\"", epub->getTitle().c_str());
      strncat(buf, titlePart, sizeof(buf) - strlen(buf) - 1);
      ReaderPerfLog::append(buf);
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        clearDeferredReposition();
      }
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }

    // Every branch above (pending jump, resume, anchor, percent jump) is meant to land on a
    // valid page, but a stale/corrupt saved position or an unclamped upstream computation can
    // still hand us a negative value here -- which would otherwise flow straight into the
    // footer ("-6/87") and into loadPage(). This is the single chokepoint all of them funnel
    // through before anything reads currentPage, so floor it here once rather than re-guarding
    // every call site above.
    if (section->currentPage < 0) {
      section->currentPage = 0;
    }
  }

  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    // Extend until either the target page exists or the build completes.
    const unsigned long extendStart = millis();
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
    logSlowBlockingBuild("extend_build", extendStart, currentSpineIndex, section.get());
  }
  if (section->isBuilding()) {
    const unsigned long turnBuildStart = millis();
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
    logSlowBlockingBuild("turn_build", turnBuildStart, currentSpineIndex, section.get());
  }

  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
  }

  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }

  // Toolbar menu: overlay the toolbar / panel on top of the freshly rendered page.
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    discardOverlayPage();
    overlayPageStored = renderer.storeBwBuffer();
    renderOverlay();
    // An open option picker rides on top of the freshly drawn panel.
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    // HALF on the Xteink grayscale panels: the page render above just ran the
    // anti-aliasing waveform, and a FAST differential leaves the covered text
    // ghosting gray through the chrome background (see openOverlay).
    renderer.displayBuffer(xteinkClassPanel() ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  }
}

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  std::optional<uint32_t> offset;
  if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                 ? currentPageVisibleOffset
                 : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, offset);
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

namespace {
// A "built"/"cache" chapter-load line only shows the cost of opening a chapter, not turning
// pages within it -- and real-device logs showed heap dropping between two page turns on an
// already-cached chapter. Log the slow turns themselves (threshold avoids spamming the bounded
// log with every ordinary ~50-80ms turn) so the next report can tell chapter-open cost apart
// from per-page-turn cost/leakage. Was 150ms while chasing the Arabic-prewarm bug (fixed in
// v1.6.80); left that low, a heavy Arabic-font page (~780-800ms even when everything is
// working correctly) tripped this on literally every turn, and the resulting every-page SD
// read-modify-write in ReaderPerfLog::append() was itself a contributor to the heap pressure
// that crashed a real device (see RollingSdLog.h). 3000ms sits above every currently-observed
// good case (including the periodic ~2.5-2.7s full e-ink refresh) while still catching a
// regression back toward multi-second turns.
constexpr unsigned long SLOW_PAGE_TURN_MS = 3000;
// prewarm/bw_render/display are common to all three renderContents() exit paths (tiled
// strip grayscale, non-strip grayscale, and no-AA/no-images); everything after display
// (grayscale planes + cleanup, when present) is lumped into "rest" so one signature
// covers all three call sites without duplicating per-path breakdown fields. The
// simulator's host-filesystem "SD" access is far faster than a real microSD card, so a
// stall here (e.g. hundreds of per-glyph decompressions each doing a real SD read)
// never showed up in simulator testing -- this is the only way to see which phase a
// real multi-second turn is actually spending time in.
void logSlowPageTurn(unsigned long t0, unsigned long tScanRender, unsigned long tPrewarm, unsigned long tBwRender,
                     unsigned long tDisplay, unsigned long tEnd, int spineIndex, const std::string& title,
                     FontCacheManager* fcm) {
  const unsigned long total = tEnd - t0;
  // A failed malloc() inside FontDecompressor::getBitmap() (real-device memory
  // pressure only -- never reproduced against the simulator's effectively-unlimited
  // heap) returns near-instantly, not slowly: the glyph is silently dropped and the
  // render moves on. Such a turn can finish well under SLOW_PAGE_TURN_MS even though
  // it left visibly blank text on the page, so bitmapAllocFailures must force logging
  // independent of the timing gate below, or exactly the pages that need investigating
  // leave zero trace in this log.
  const uint32_t bitmapAllocFailures =
      (fcm && fcm->getDecompressor()) ? fcm->getDecompressor()->getStats().bitmapAllocFailures : 0;
  if (total < SLOW_PAGE_TURN_MS && bitmapAllocFailures == 0) return;
  // Cache hits/misses/decompress time: PrewarmScope resets these at the top of every
  // page render, so this reflects just this one page. hits+misses were still ~500 vs
  // ~90 hits on the Quran even after fixing the marker scan/render mismatch, while a
  // plain Arabic novel on the same device hit ~90%: the gap is too big to be markers
  // alone (a page has a few dozen ayah numbers at most, not hundreds of missed
  // glyphs). prewarm_glyphs (how many DISTINCT glyphs the batched prewarm pass
  // actually cached for this page, from pageGlyphsBytes/12 -- PageGlyphEntry is 12
  // bytes) says whether the scan pass is only capturing a small fraction of the
  // page's text in the first place, upstream of anything getBitmap() does.
  //
  char statsPart[512] = "";
  if (fcm) {
    FontDecompressor* decompressor = fcm->getDecompressor();
    if (decompressor) {
      const auto& s = decompressor->getStats();
      snprintf(statsPart, sizeof(statsPart),
               " hits=%lu misses=%lu decomp=%lums calls=%lu prewarm_glyphs=%lu prewarm_bytes=%lu prewarm_groups=%u "
               "bitmap_fail=%lu fail_bytes=%lu",
               (unsigned long)s.cacheHits, (unsigned long)s.cacheMisses, (unsigned long)s.decompressTimeMs,
               (unsigned long)s.getBitmapCalls, (unsigned long)(s.pageGlyphsBytes / 12),
               (unsigned long)s.pageBufferBytes, (unsigned)s.uniqueGroupsAccessed, (unsigned long)s.bitmapAllocFailures,
               (unsigned long)s.firstFailedAllocBytes);
    }
  }
  char buf[640];
  snprintf(buf, sizeof(buf),
           "%lu turn spine=%d scan=%lums prewarm=%lums bw_render=%lums display=%lums rest=%lums total=%lums "
           "heap=%u max=%u cpu=%u%s \"%s\"",
           millis(), spineIndex, tScanRender - t0, tPrewarm - tScanRender, tBwRender - tPrewarm, tDisplay - tBwRender,
           tEnd - tDisplay, total, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), cpuMhzNow(), statsPart,
           title.c_str());
  ReaderPerfLog::append(buf);
}
}  // namespace

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  // Split point: the scan render (shaping + width measurement, no rasterization) vs
  // endScanAndPrewarm() (batched glyph prewarm) were previously lumped into one
  // "prewarm=" figure. On real-device SD-Arabic turns the scan render dominates
  // (prewarmTotalMs stays ~100ms), so time them separately to prove which phase the
  // multi-second cost is in before optimizing.
  const auto tScanRender = millis();
  // Scan the status bar too: a CJK book/chapter title redirected to the SD
  // fallback font joins the page's single batch prewarm instead of triggering
  // its own SD pass after the scope ends. After the diagnostic captures above,
  // so it doesn't pollute the isolated scan-render timing.
  renderStatusBar();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  // The reader starts with zero here, which means the normal refresh cycle
  // would use a HALF refresh for its first page. Keep that same clean base for
  // image pages: their double-FAST path otherwise runs directly over the
  // retained frame after a silent restart (for example, when returning from
  // KOReader/Midad sync), leaving the old UI mixed with the image.
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Paper Mono only (no other panel combines): defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase = tiledGrayscale && !pageHasImages && renderer.combinesGrayscaleBase();
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Image pages use one base refresh before the grayscale pass. FAST leaves
    // the panel receptive to the gray waveform; pending cleanup still honors
    // the scheduled/manual HALF refresh.
    renderer.displayBuffer(cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh = 1;
  } else if (combinedGrayscaleBase) {
    // Stash the base without activating; displayGrayBuffer() below commits
    // base + grays as one waveform.
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
  }
  const auto tDisplay = millis();

  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    // Tiered on heap pressure: two plane buffers hide both plane renders
    // inside the refresh wait; one hides the LSB render (its buffer is reused
    // for MSB after streaming); none falls back to the strip-scratch flow with
    // no overlap. Each buffer is only attempted when it leaves ~60 KB free so
    // the pass never starves concurrent allocations: the next page re-render
    // allocates through throwing std::string paths that abort() on OOM under
    // -fno-exceptions, so a plane buffer that "fits" but eats the render
    // headroom is worse than the strip fallback. Blocking panels skip the
    // buffers entirely: overlapRefresh is false, so there is nothing in
    // flight to hide the render behind.
    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    // Free-heap alone ignores fragmentation: taking the largest block for a
    // plane can leave only slivers behind even when total headroom looks
    // fine. Require the block to fit the plane with 16 KB contiguous to
    // spare -- both floors sit comfortably above this file's own
    // RENDER_MIN_FREE_HEAP (24KB), so a plane buffer can never itself starve
    // the render it is part of.
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
      logSlowPageTurn(t0, tScanRender, tPrewarm, tBwRender, tDisplay, tEnd, currentSpineIndex, epub->getTitle(), fcm);
    } else {
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
        logSlowPageTurn(t0, tScanRender, tPrewarm, tBwRender, tDisplay, tEnd, currentSpineIndex, epub->getTitle(), fcm);
      }
    }
  } else {
    if (needsAnyGrayscale) {
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
      logSlowPageTurn(t0, tScanRender, tPrewarm, tBwRender, tDisplay, tEnd, currentSpineIndex, epub->getTitle(), fcm);
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
      logSlowPageTurn(t0, tScanRender, tPrewarm, tBwRender, tDisplay, tEnd, currentSpineIndex, epub->getTitle(), fcm);
    }
  }
}

namespace {
// build_quran_epub_kfgqpc.py's build_toc_ncx() bakes an "<Arabic-Indic digits> - "
// index prefix into each surah's TOC navLabel (e.g. "18 - سورة الكهف") -- useful
// context in the "Select Chapter" list, but redundant clutter in the reader's own
// chapter-title status bar (the page's own surah banner already shows the name).
// Byte-level check rather than full UTF-8 decoding since Arabic-Indic digits
// (U+0660-0669) are a fixed 2-byte UTF-8 sequence (0xD9 0xA0-0xA9); a no-op for any
// other book's TOC titles, which won't start with this exact byte pattern.
std::string stripLeadingArabicIndicIndex(const std::string& title) {
  size_t i = 0;
  while (i + 1 < title.size() && static_cast<uint8_t>(title[i]) == 0xD9 && static_cast<uint8_t>(title[i + 1]) >= 0xA0 &&
         static_cast<uint8_t>(title[i + 1]) <= 0xA9) {
    i += 2;
  }
  if (i == 0 || i + 3 > title.size() || title[i] != ' ' || title[i + 1] != '-' || title[i + 2] != ' ') {
    return title;
  }
  return title.substr(i + 3);
}

// Every Quran chapter title starts with the literal word "سورة " ("Surah "), which
// carries zero information (all 114 are surahs) but adds ~9 bytes / 4 glyphs to
// every single title. drawStatusBar()'s truncatedText() shortens from the LOGICAL
// end of the string, which for this RTL text means it strips the actual surah name
// (which comes after "سورة ") and keeps "سورة" itself for any title too wide for the
// available space -- e.g. "سورة الدخان"/"سورة محمد"/"سورة الفتح" showing as just
// "سورة" in the footer, real-device report. Rather than teach truncation about RTL
// (which title to keep depends on the string's own semantics, not something generic
// truncation logic can know), strip this literal, known prefix before it's ever a
// truncation candidate -- shorter titles are also simply less likely to need
// truncating at all. Byte-match, same convention as the digit-prefix strip above;
// a no-op for any non-Quran book's title, which won't start with this exact phrase.
std::string stripLeadingSurahWord(const std::string& title) {
  constexpr char kSurahWord[] = "\xd8\xb3\xd9\x88\xd8\xb1\xd8\xa9 ";  // "سورة " (9 bytes)
  constexpr size_t kSurahWordLen = sizeof(kSurahWord) - 1;
  if (title.size() >= kSurahWordLen && title.compare(0, kSurahWordLen, kSurahWord) == 0) {
    return title.substr(kSurahWordLen);
  }
  return title;
}
}  // namespace

void EpubReaderActivity::renderStatusBar() const {
  const int currentPage = section ? section->currentPage + 1 : 1;
  const float pageCount = section ? section->estimatedTotalPages() : 1;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub ? (epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100) : 0;

  std::string title;
  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  // Pomodoro takes the lane ahead of the auto-turn rate when both are on: the rate is a
  // fixed value you set once and can re-read from the drawer, while this is the number
  // that is actually changing and the reason the reader asked for a footer readout.
  const auto& pomodoro = READER_POMODORO;
  if (pomodoro.isActive()) {
    if (pomodoro.isFinished()) {
      title = I18N.get(pomodoro.currentPhase() == ReaderPomodoro::Phase::Focus ? StrId::STR_POMODORO_FOCUS_DONE
                                                                               : StrId::STR_POMODORO_BREAK_DONE);
    } else {
      // Phase name alongside the clock -- "12:04" alone doesn't say whether you are
      // reading or on a break, and the break phases are short enough to misread as a
      // nearly-finished focus block.
      const StrId phaseLabel = pomodoro.currentPhase() == ReaderPomodoro::Phase::Focus ? StrId::STR_POMODORO_FOCUS
                               : pomodoro.currentPhase() == ReaderPomodoro::Phase::LongBreak
                                   ? StrId::STR_POMODORO_LONG_BREAK
                                   : StrId::STR_POMODORO_SHORT_BREAK;
      title = std::string(I18N.get(phaseLabel)) + " " + formatPomodoroRemaining(pomodoro.remainingMs());
    }
    // Same offset the auto-turn indicator needs for the same reason: with the text lane
    // reserved rather than native, the baseline sits high without it.
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    if (epub) {
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex != -1) {
        const auto tocItem = epub->getTocItem(tocIndex);
        title = stripLeadingSurahWord(stripLeadingArabicIndicIndex(tocItem.title));
      }
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub ? epub->getTitle() : "";
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section ? section->isBuilding() : false);
}

// ---------------------------------------------------------------------------
// Toolbar reader menu
// ---------------------------------------------------------------------------

namespace {
constexpr StrId kTextRowNames[] = {StrId::STR_FONT, StrId::STR_FONT_SIZE, StrId::STR_LINE_SPACING,
                                   StrId::STR_PARA_ALIGNMENT, StrId::STR_FOCUS_READING};
constexpr StrId kSpacingIds[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
constexpr StrId kAlignIds[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                               StrId::STR_BOOK_S_STYLE};
constexpr int kTextRowCount = static_cast<int>(std::size(kTextRowNames));
static_assert(std::size(kSpacingIds) == CrossPointSettings::LINE_COMPRESSION_COUNT, "line spacing labels");
static_assert(std::size(kAlignIds) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, "alignment labels");
}  // namespace

bool EpubReaderActivity::usesToolbarMenu() const {
  return SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

std::string EpubReaderActivity::currentChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex != -1) {
    return epub->getTocItem(tocIndex).title;
  }
  return tr(STR_UNNAMED);
}

std::string EpubReaderActivity::textRowName(int row) const {
  return row >= 0 && row < kTextRowCount ? I18N.get(kTextRowNames[row]) : "";
}

std::string EpubReaderActivity::textRowValue(int row) const {
  static constexpr StrId kFamily[] = {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS};
  switch (row) {
    case 0:
      if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
      return I18N.get(kFamily[SETTINGS.fontFamily % CrossPointSettings::FONT_FAMILY_COUNT]);
    case 1:
      return std::to_string(SETTINGS.fontPointSize) + " pt";
    case 2:
      return I18N.get(kSpacingIds[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
    case 3:
      return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
    case 4:
      return SETTINGS.focusReadingEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

// Live apply: persist, re-paginate, and let renderBook() redraw the page with
// the open panel back on top -- the book itself is the preview.
void EpubReaderActivity::applyTextSettingLive() {
  applyReaderTextSettings();
  discardOverlayPage();  // the stored page is laid out with the old settings
  requestUpdate();
}

// Settings-style option pickers for the Text panel's enum rows. Every
// selection applies immediately to the page under the sheet.
void EpubReaderActivity::showTextRowPopup(const int row) {
  switch (row) {
    case 1: {
      // The point sizes the active family actually ships.
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      if (sizes.empty()) return;
      std::vector<std::string> labels;
      labels.reserve(sizes.size());
      for (const uint8_t size : sizes) labels.push_back(std::to_string(size) + " pt");
      const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      int curIdx = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == cur) curIdx = static_cast<int>(i);
      }
      overlayPopup.show(StrId::STR_FONT_SIZE, labels, curIdx, [this, sizes](int idx) {
        if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
        SETTINGS.fontPointSize = sizes[idx];
        applyTextSettingLive();
      });
      break;
    }
    case 2:
      overlayPopup.show(StrId::STR_LINE_SPACING, kSpacingIds, static_cast<int>(std::size(kSpacingIds)),
                        SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT, [this](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case 3:
      overlayPopup.show(StrId::STR_PARA_ALIGNMENT, kAlignIds, static_cast<int>(std::size(kAlignIds)),
                        SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, [this](int idx) {
                          SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    default:
      return;
  }
  paintOverlayPopup();
}

void EpubReaderActivity::discardOverlayPage() {
  if (!overlayPageStored) return;
  renderer.discardStoredBwBuffer();
  overlayPageStored = false;
}

bool EpubReaderActivity::ensureToolbarUi() {
  // ~2 KB claimed only while the menu is open, which is peak heap pressure with an EPUB
  // section resident. A throwing new would abort() here rather than return null.
  if (!toolbarUi) {
    toolbarUi = makeUniqueNoThrow<ReaderToolbarUi>(renderer);
    if (!toolbarUi) LOG_ERR("EPUBREADER", "OOM: ReaderToolbarUi");
  }
  return toolbarUi != nullptr;
}

void EpubReaderActivity::openOverlay(Overlay target) {
  const Overlay previous = overlay;
  overlay = target;
  if (!ensureToolbarUi()) {
    // Nothing to draw the panel with; stay on the page instead of resetting the device.
    overlay = previous;
    return;
  }
  if (previous == Overlay::None) toolbarUi->begin();
  // Buttons show a cursor from the start; touch boards only once a button moves it.
  panelCursorShown = !mappedInput.hasTouch();
  switch (target) {
    case Overlay::Toolbar:
      focusedTool = 0;
      break;
    case Overlay::Contents:
      panelIndex = std::max(0, epub->getTocIndexForSpineIndex(currentSpineIndex));
      // Fresh viewport opening on the current chapter, cursor shown or not.
      toolbarUi->nav().reset(panelIndex);
      toolbarUi->nav().top = panelIndex;
      break;
    case Overlay::Text:
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::More:
      panelIndex = 0;
      buildMoreActions();
      toolbarUi->nav().reset();
      break;
    default:
      break;
  }
  panelHoldJumped = false;

  // The page is already on screen and still in the framebuffer, so paint the
  // chrome straight onto it and push one refresh. requestUpdate() would
  // re-render the whole page first: slow, and visibly wrong, since that repaint
  // lands before the overlay does.
  //
  // Refresh mode: FAST for every overlay paint, first open included. The AA
  // pass only grays glyph edges, and residue a FAST differential leaves under
  // the sheet has not shown in practice; it also self-heals on the
  // Xteink-class panels, whose close path re-renders the page. If text or
  // images ever visibly ghost through the chrome, restore a HALF cleanup on
  // the first open (see #2190 for the mechanism).
  if (section) {
    // Serialize against the render task: renderBook may be mid-page (status
    // bar included) in the shared framebuffer, and painting the chrome from
    // the loop task at the same time interleaves the two frames.
    RenderLock lock;
    if (previous == Overlay::None) {
      // Snapshot the clean page so stepping back from a panel to the toolbar
      // (and closing, where supported) can restore it without a re-render.
      overlayPageStored = renderer.storeBwBuffer();
    } else if (overlayPageStored) {
      // Overlay -> overlay: wipe the previous chrome (toolbar header, sheet,
      // progress row) back to the clean page so none of it shows around or
      // through the new sheet; re-store for the next transition. No baseline
      // resync: the glass still shows the old chrome, and the differential
      // must keep diffing against it to erase it.
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    requestUpdate();  // no page yet: renderBook() draws the overlay once it is
  }
}

// Close the overlay back to the reading page. Boards without the Xteink
// grayscale-AA pass restore the page snapshot and push one FAST refresh -- no
// re-render, no flash; Xteink boards re-render to restore the AA planes.
void EpubReaderActivity::closeOverlayToPage() {
  overlay = Overlay::None;
  overlayPopup.dismiss();  // an option picker cannot outlive its panel
  toolbarUi.reset();       // ~1 KB of interaction table + props, only needed while open
  if (!xteinkClassPanel() && overlayPageStored) {
    RenderLock lock;  // the render task shares the framebuffer
    // No baseline resync: the glass shows the chrome, and erasing it needs
    // the differential to keep diffing against the last pushed frame.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  discardOverlayPage();
  requestUpdate();  // redraw the clean page
}

void EpubReaderActivity::renderOverlay() {
  if (!epub || !section || !toolbarUi) return;

  ReaderToolbarUi::Model model;
  // The toolbar's tool pill is the button-navigation cursor: tap-first (same
  // convention as the panel lists), it only shows once a button has moved it.
  // Panels override below: there the pill marks the open panel on every board.
  model.activeTool = (overlay == Overlay::Toolbar && !panelCursorShown) ? -1 : focusedTool;
  // Strings the model points at live here until render() returns.
  std::string chapterTitle, pageInfo;

  if (overlay == Overlay::Toolbar) {
    chapterTitle = currentChapterTitle();
    const int pageCount = section->estimatedTotalPages();
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(section->currentPage + 1) / static_cast<float>(pageCount) : 0.0f;
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress);
    pageInfo = std::to_string(section->currentPage + 1) + "/" + std::to_string(pageCount) + "   " +
               std::to_string(clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f))) + "%";
    model.chapterTitle = chapterTitle.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = static_cast<int>(bookProgress * 1000.0f + 0.5f);
    toolbarUi->setModel(model);
    toolbarUi->render();
    return;
  }

  // Panels (Contents / Text / More): a bottom sheet over the page + button hints.
  model.panel = true;
  if (!mappedInput.hasTouch()) {
    model.bottomReserve = UITheme::getInstance().getMetrics().buttonHintsHeight;
    model.denseRows = true;
  }
  // Tap-first: the cursor is only drawn once a button has moved it, so a
  // tapped row does not stay inverted after its action.
  model.selectedIndex = panelCursorShown ? panelIndex : -1;
  if (overlay == Overlay::Contents) {
    model.panelTitle = tr(STR_TOOL_CONTENTS);
    model.itemCount = epub->getTocItemsCount();
    model.rowText = [this](int i) {
      const auto item = epub->getTocItem(i);
      const int depth = item.level > 1 ? (item.level - 1) * 2 : 0;
      return std::string(depth, ' ') + item.title;
    };
  } else if (overlay == Overlay::Text) {
    model.panelTitle = tr(STR_TOOL_TEXT);
    model.itemCount = kTextRowCount;
    model.rowText = [this](int i) { return textRowName(i); };
    model.rowValue = [this](int i) { return textRowValue(i); };
  } else {
    model.panelTitle = tr(STR_TOOL_MORE);
    model.itemCount = static_cast<int>(moreItems.size());
    model.rowText = [this](int i) { return moreRowName(i); };
    model.rowValue = [this](int i) { return moreRowValue(i); };
  }
  toolbarUi->setModel(model);
  toolbarUi->render();

  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void EpubReaderActivity::handleOverlayInput() {
  if (!toolbarUi) return;

  // A modal option picker over the panel owns all input while open.
  if (overlayPopup.isActive()) {
    overlayPopup.handleInput(mappedInput, [this] {
      if (overlayPopup.isActive()) {
        paintOverlayPopup();  // highlight moved
        return;
      }
      // Dismissed or selected: erase the dialog -- clean page back, then the
      // panel over it (the dialog can overhang the sheet onto the page).
      RenderLock lock;
      if (overlayPageStored) {
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      } else {
        requestUpdate();
      }
    });
    return;
  }
  const auto fastRedraw = [this] {
    RenderLock lock;  // the render task shares the framebuffer
    renderOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  };

  // Jump to another spine item (chapter scrub). The overlay stays up and is
  // re-drawn over the new page by renderBook().
  const auto gotoSpine = [this](int target) {
    const int spineCount = epub->getSpineItemsCount();
    target = std::clamp(target, 0, spineCount - 1);
    if (target != currentSpineIndex) {
      statsOnJump();  // non-linear navigation: bank the dwell, re-arm the pace warmup
      RenderLock lock;
      clearDeferredReposition();
      nextPageNumber = 0;
      currentSpineIndex = target;
      section.reset();
    }
    requestUpdate();
  };
  const auto toolOverlay = [](int tool) {
    return tool == 0 ? Overlay::Contents : (tool == 1 ? Overlay::Text : Overlay::More);
  };

  // Touch first: FreeInkUI routes the frame against the tap targets the last
  // render registered and hands back the action it mapped to.
  const auto routed = toolbarUi->route(mappedInput);

  // --- Toolbar ---
  if (overlay == Overlay::Toolbar) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeOverlayToPage();
        return;
      case ReaderToolbarUi::Event::Tool:
        focusedTool = routed.value;
        openOverlay(toolOverlay(focusedTool));
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        gotoSpine(currentSpineIndex - 1);
        return;
      case ReaderToolbarUi::Event::NextChapter:
        gotoSpine(currentSpineIndex + 1);
        return;
      case ReaderToolbarUi::Event::Scrub:
        gotoSpine(static_cast<int>((static_cast<float>(routed.permille) / 1000.0f) *
                                       static_cast<float>(epub->getSpineItemsCount() - 1) +
                                   0.5f));
        return;
      default:
        break;
    }
    if (routed.routed) return;  // a touch frame the chrome consumed (or dead space)

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeOverlayToPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + 2) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openOverlay(toolOverlay(focusedTool));
      return;
    }
    const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (prev || next) {
      gotoSpine(currentSpineIndex + (next ? 1 : -1));
    }
    return;
  }

  // --- Panels (Contents / Text / More) ---
  const int count = overlay == Overlay::Contents ? epub->getTocItemsCount()
                    : overlay == Overlay::Text   ? kTextRowCount
                                                 : static_cast<int>(moreItems.size());
  const int pageRows = std::max(1, toolbarUi->visibleRows());

  // Activate the highlighted row: change a value / jump to a chapter / run an
  // action. Shared by the Confirm button and a row tap.
  const auto activateRow = [this, count, &fastRedraw] {
    if (panelIndex < 0 || panelIndex >= count) return;
    if (overlay == Overlay::Text) {
      if (panelIndex == 0) {
        // Full font picker (built-in + SD fonts, live preview) -- the same
        // screen Settings uses; a popup cannot scroll a long font list.
        overlay = Overlay::None;
        overlayPopup.dismiss();
        discardOverlayPage();
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 applyReaderTextSettings();
                                 overlay = Overlay::Text;  // back to the Text panel
                                 panelIndex = 0;
                                 if (toolbarUi) toolbarUi->begin();  // the picker drew its own FUI screen
                                 requestUpdate();                    // re-render page + Text panel
                               });
      } else if (panelIndex == 4) {
        // Focus Reading is a genuine on/off: a tap toggles and applies live.
        SETTINGS.focusReadingEnabled = SETTINGS.focusReadingEnabled ? 0 : 1;
        applyTextSettingLive();
      } else {
        // Enum rows open the Settings-style option picker.
        showTextRowPopup(panelIndex);
      }
    } else if (overlay == Overlay::Contents) {
      const auto item = epub->getTocItem(panelIndex);
      if (item.spineIndex != -1) {
        statsOnJump();  // non-linear navigation: bank the dwell, re-arm the pace warmup
        RenderLock lock;
        clearDeferredReposition();
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
        nextPageNumber = 0;
        section.reset();
      }
      overlay = Overlay::None;
      discardOverlayPage();
      requestUpdate();
    } else if (overlay == Overlay::More) {
      activateMoreRow(panelIndex);
    }
  };

  // Steps up to the toolbar -- the Back button and a tap on the page above
  // the sheet.
  const auto dismissPanel = [this, &fastRedraw] {
    overlay = Overlay::Toolbar;
    // Restore the snapshotted page under the toolbar instead of re-rendering
    // it (2+ refreshes -> one FAST). Re-store right away so another panel
    // round-trip can restore again.
    if (overlayPageStored) {
      {
        RenderLock lock;  // the render task shares the framebuffer
        // No baseline resync: the glass shows the panel, and erasing it needs
        // the differential to keep diffing against the last pushed frame.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      fastRedraw();  // takes its own RenderLock
      return;
    }
    requestUpdate();
  };

  // Pages the list by one screen of rows through the nav (measured page size,
  // no-op at the ends). A shown cursor rides along so the buttons continue
  // from what is visible; on touch boards only the viewport moves.
  const auto pageList = [this, count, pageRows, &fastRedraw](int direction) {
    if (count <= 0) return;
    const bool moved = toolbarUi->nav().scrollBy(direction * pageRows, count);
    if (panelCursorShown) {
      panelIndex = std::clamp(panelIndex + direction * pageRows, 0, count - 1);
      fastRedraw();
      return;
    }
    if (moved) fastRedraw();
  };

  switch (routed.event) {
    case ReaderToolbarUi::Event::Dismiss:
      dismissPanel();
      return;
    case ReaderToolbarUi::Event::Tool: {
      // Sheet-bottom tool switcher: hop straight to another panel.
      const Overlay target = toolOverlay(routed.value);
      if (target != overlay) {
        focusedTool = routed.value;
        openOverlay(target);
      }
      return;
    }
    case ReaderToolbarUi::Event::Row:
      // A tap on the right-edge strip pages the sheet instead (upper half =
      // previous page, lower half = next): swipes are unreliable on etched
      // glass, and a long contents list needs a fast way through.
      if (routed.x >= renderer.getScreenWidth() - 44) {
        pageList(routed.y >= renderer.getScreenHeight() - (renderer.getScreenHeight() * 62) / 200 ? 1 : -1);
        return;
      }
      panelIndex = routed.value;
      panelCursorShown = false;
      activateRow();
      return;
    default:
      break;
  }
  // Swipe up/down pages the list. Checked before the routed-frame return:
  // FUI routes every touch frame over the sheet, so a swipe's frames count as
  // routed (without dispatching -- too much travel for a tap) and the gesture
  // would otherwise never be seen.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageList(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  if (routed.routed) return;  // consumed by the chrome (title band, dead space)

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    dismissPanel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }

  // Up/Down (side) and Left/Right (front) move the cursor: a tap steps one
  // row, holding past PANEL_HOLD_MS jumps PANEL_HOLD_STEP rows in one go, which
  // is how you cross a hundreds-of-chapters contents list without a press per
  // row. The jump fires once on the hold and swallows the release that ends it,
  // so it never doubles up with the tap step.
  if (count > 0) {
    const bool up = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                    mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool down = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                      mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!panelHoldJumped && (up || down) && mappedInput.getHeldTime() >= PANEL_HOLD_MS) {
      const int step = down ? PANEL_HOLD_STEP : -PANEL_HOLD_STEP;
      panelIndex = std::clamp(panelIndex + step, 0, count - 1);
      panelHoldJumped = true;
      panelCursorShown = true;
      fastRedraw();
      return;
    }

    const bool releasedUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool releasedDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (releasedUp || releasedDown) {
      if (!panelHoldJumped) {
        panelIndex = releasedUp ? ButtonNavigator::previousIndex(panelIndex, count)
                                : ButtonNavigator::nextIndex(panelIndex, count);
        panelCursorShown = true;
        fastRedraw();
      }
      panelHoldJumped = false;
    }
  }
}

// First paint of the option picker over the panel (and highlight repaints).
// The dialog draws over the current framebuffer without clearing; erasing it
// on dismissal is the popup gate's restore in handleOverlayInput().
void EpubReaderActivity::paintOverlayPopup() {
  RenderLock lock;
  overlayPopup.render(renderer);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void EpubReaderActivity::applyReaderTextSettings() {
  SETTINGS.saveToFile();
  // (Re)load or unload the selected SD-card font for the current family/size.
  // The reader otherwise only loads SD fonts on book open, so without this an
  // in-reader font change wouldn't take effect until re-opening the book.
  sdFontSystem.ensureLoaded(renderer);
  RenderLock lock;
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  section.reset();  // force re-pagination with the new settings
}

// The More panel carries everything the classic list menu offers except the
// two entries that have their own tool (chapters -> Contents, text -> Text).
void EpubReaderActivity::buildMoreActions() {
  using MA = EpubReaderMenuActivity::MenuAction;
  EpubReaderMenuActivity::buildMenuItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty());
  moreItems.erase(std::remove_if(moreItems.begin(), moreItems.end(),
                                 [](const auto& item) {
                                   return item.action == MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS;
                                 }),
                  moreItems.end());
}

std::string EpubReaderActivity::moreRowName(int row) const {
  return row >= 0 && row < static_cast<int>(moreItems.size()) ? I18N.get(moreItems[row].labelId) : "";
}

std::string EpubReaderActivity::moreRowValue(int row) const {
  using MA = EpubReaderMenuActivity::MenuAction;
  static constexpr StrId kOrient[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(std::size(kOrient) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return "";
  switch (moreItems[row].action) {
    case MA::ROTATE_SCREEN:
      return I18N.get(kOrient[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]);
    case MA::AUTO_PAGE_TURN:
      return (autoTurnOption == 0 || autoTurnOption >= static_cast<int>(std::size(PAGE_TURN_RATES)))
                 ? std::string(tr(STR_STATE_OFF))
                 : std::to_string(PAGE_TURN_RATES[autoTurnOption]);
    case MA::NIGHT_MODE:
      return SETTINGS.screenInverted ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MA::FRONTLIGHT:
      return Frontlight.isOn() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

void EpubReaderActivity::activateMoreRow(int row) {
  using MA = EpubReaderMenuActivity::MenuAction;
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return;
  const auto action = moreItems[row].action;
  // In-place toggles keep the panel open and re-render the page beneath it.
  switch (action) {
    case MA::ROTATE_SCREEN: {
      static constexpr StrId kOrientIds[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
      static_assert(std::size(kOrientIds) == CrossPointSettings::ORIENTATION_COUNT, "orientation options");
      overlayPopup.show(StrId::STR_ORIENTATION, kOrientIds, static_cast<int>(std::size(kOrientIds)),
                        SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT, [this](int idx) {
                          if (idx == SETTINGS.orientation) return;
                          applyOrientation(static_cast<uint8_t>(idx));
                          // The stored page is laid out for the old orientation.
                          discardOverlayPage();
                          requestUpdate();
                        });
      paintOverlayPopup();
      return;
    }
    case MA::AUTO_PAGE_TURN: {
      std::vector<std::string> labels;
      labels.reserve(std::size(PAGE_TURN_RATES));
      labels.emplace_back(tr(STR_STATE_OFF));
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) labels.push_back(std::to_string(PAGE_TURN_RATES[i]));
      overlayPopup.show(StrId::STR_AUTO_TURN_PAGES_PER_MIN, labels, autoTurnOption, [this](int idx) {
        autoTurnOption = idx;
        toggleAutoPageTurn(static_cast<uint8_t>(idx));
      });
      paintOverlayPopup();
      return;
    }
    case MA::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      discardOverlayPage();
      requestUpdate();
      return;
    case MA::FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      {
        RenderLock lock;  // the render task shares the framebuffer
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      return;
    }
    default:
      break;
  }
  // Leaf actions open their own screen / perform the action; close the overlay first.
  overlay = Overlay::None;
  discardOverlayPage();
  if (action == MA::TOGGLE_BOOKMARK) {
    // No child activity here to trigger the re-render the list menu relies on:
    // show the same confirmation popup the long-press path does.
    addBookmark();
    showBookmarkMessage = true;
    bookmarkMessageTime = millis();
    requestUpdate();
    return;
  }
  onReaderMenuConfirm(action);
  // Actions that neither open a screen nor leave the reader (a sync with no
  // credentials, say) would otherwise leave the closed panel on screen.
  if (action != MA::GO_HOME && action != MA::DELETE_CACHE) requestUpdate();
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;
  statsOnJump();

  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;
    return;
  }

  {
    RenderLock lock;
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPageJump.reset();  // see the invariant at its declaration
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  statsOnJump();
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    pendingPageJump.reset();  // see the invariant at its declaration
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock;
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
