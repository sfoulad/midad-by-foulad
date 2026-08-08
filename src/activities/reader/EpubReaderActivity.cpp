#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
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
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderPomodoro.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/reader/MidadSyncActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookReaderSettings.h"
#include "util/BookmarkUtil.h"
#include "util/ReaderPerfLog.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
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

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
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

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
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

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

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
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
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

  loadCachedBookmarks();

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
  Activity::onExit();

  // The extractor holds a raw pointer to this activity's epub; drop it before
  // the activity (and the shared_ptr) goes away.
  ImageBlock::setExtractor(nullptr, nullptr);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Commit this session: bank the last page's dwell, cache the time-left estimate
  // and progress for the home hero card, then let the store apply its session
  // thresholds and save (single SD write; never per page turn).
  if (SETTINGS.trackReadingStats && epub) {
    accountPageDwellForStats(false);
    uint32_t timeLeft = 0;
    const bool haveEstimate = estimateTimeLeftSeconds(timeLeft);
    const uint8_t progressPercent = currentBookProgressPercent();
    const bool finishedBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
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

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
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
  // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts.
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
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
    // Should never happen
    finish();
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
    // Drive any in-progress incremental section build forward, off the page-turn critical path,
    // but only within a small window ahead of the reader: an unbounded build monopolized the
    // RenderLock and locked out page turns. The build follows the reader instead, and instant
    // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
    // Skip while the render mutex is busy so we never delay a pending render; re-check
    // isBuilding() under the lock since render() may have just finished it.
    // Heap gate: while a build is active, buildHeapPaused always reflects the CURRENT heap
    // reading, independent of whether this particular tick would otherwise attempt a chunk
    // (render-lock busy / already caught up to BUILD_WINDOW_AHEAD) -- skipLoopDelay() reads
    // this every frame, so a stale "paused" left over from a tick that skipped for an
    // unrelated reason would wrongly suppress the CPU race-to-idle once the real gate clears.
    buildHeapPaused = section && section->isBuilding() &&
                      (ESP.getFreeHeap() < BUILD_TICK_MIN_FREE_HEAP_BYTES ||
                       ESP.getMaxAllocHeap() < BUILD_TICK_MIN_LARGEST_BLOCK_BYTES);

    if (section && section->isBuilding() && !buildHeapPaused && !RenderLock::peek() &&
        static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) {
      RenderLock lock;
      // Re-check under the lock: render() (which also holds the RenderLock) may have finalized the
      // build between the outer isBuilding() check and acquiring the lock here, in which case
      // buildSomeMore() would fail and wrongly reset the section. cppcheck can't see the cross-task
      // mutation, so it flags this as always true.
      // cppcheck-suppress knownConditionTrueFalse
      if (section->isBuilding()) {
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

    // Idle glyph prewarm for the likely next page (currentPage + 1). The scan
    // pass draws nothing (FCM scan mode suppresses text pixels and ImageBlock
    // skips itself while scanning), so the displayed framebuffer is untouched;
    // endScanAndPrewarm loads only glyphs not already cached. Debounced past
    // rapid page-flipping, one attempt per position, and deferred while a
    // render/build owns the CPU or the heap is near the render floors.
    // Cross-chapter prewarm is deliberately out of scope (the next spine's
    // section isn't loaded). Mutually exclusive with the build tick above:
    // this only runs once the section has finished building.
    constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
    if (section && !section->isBuilding() && !RenderLock::peek() && lastRenderCompleteMs != 0 &&
        millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS && ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP &&
        ESP.getMaxAllocHeap() > RENDER_MIN_LARGEST_BLOCK &&
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
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
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

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block. A Confirm release after a long-press function (bookmark/sync) fired is left
  // to the regular Confirm handler below, which consumes it via ignoreNextConfirmRelease.
  if (atEndOfBook && endOfBookOptions.menuActive() &&
      !(ignoreNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    std::string openPath;
    switch (endOfBookOptions.handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        currentSpineIndex = std::max(epub->getSpineItemsCount() - 1, 0);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  // Enter reader menu activity on short-press Confirm. A long-press that fired a bound
  // function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      openReaderMenu();
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        // Hold ~1s launches KOReader sync. If sync can't run (no credentials stored), fall
        // through so the normal Confirm-release still opens the reader menu.
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if (launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;  // sync launched or error shown; suppress menu open
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
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

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (endOfBookOptions.menuActive()) {
      // Selection movement was handled above; absorb leftover page-turn triggers so
      // e.g. "previous" at the top of the list doesn't jump back into the book
      return;
    }
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      pendingPageJump.reset();  // see the invariant at its declaration
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
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

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }
  statsOnJump();

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPageJump.reset();  // see the invariant at its declaration
    pendingPercentJump = true;
    section.reset();
  }
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

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action, const MenuResult& menu) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
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

      if (currentSpineIndex != targetSpineIndex) {
        RenderLock lock(*this);
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        RenderLock lock(*this);
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
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
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
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
            if (!result.isCancelled) {
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
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
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
        RenderLock lock(*this);
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
      int marginLeft = 0, marginTop = 0;
      if (auto page = loadCurrentPageForLookup(marginLeft, marginTop)) {
        startActivityForResult(
            std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                           SETTINGS.getReaderFontId(), marginLeft, marginTop),
            [this](const ActivityResult&) { requestUpdate(); });
      } else {
        requestUpdate();
      }
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
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch

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

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    // Same dangling-context hazard as the Midad release: the extractor points
    // into the epub being freed here.
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
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

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  accountPageDwellForStats(isForwardTurn);
  if (isForwardTurn) {
    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump.reset();  // see the invariant at its declaration
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
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

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    // Sole load site: runs on the render task (serialized by RenderLock); the main
    // task only reads the suggestions once the loaded flag is published
    endOfBookOptions.loadOnce(epub->getPath());
    renderer.clearScreen();
    endOfBookOptions.render(renderer, mappedInput);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
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

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    const unsigned long chapterLoadStartMs = millis();
    // Zero SD-card font read/seek stats before a fresh build so the "built" log
    // line below reports THIS chapter's SD I/O, not whatever leaked over from
    // the previous page turn's render-time prewarm.
    if (auto* fcmForBuildStats = renderer.getFontCacheManager()) fcmForBuildStats->resetStats();
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
    const bool cacheLoaded = section->loadSectionFile(
        SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
        SETTINGS.effParagraphAlignment(), viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
        SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled);
    if (cacheLoaded) {
      // Matching render params means identical pagination, so the saved page number is valid
      // as-is: consume any pending settings-change reposition. Without this, a chapter total
      // saved while the section was still building (i.e. a watermark, not the real count)
      // would remap the resume page against the finalized count and teleport the reader.
      cachedChapterTotalPageCount = 0;
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
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
        // The popup's own refresh is a plain FAST, so force the page that replaces it onto the HALF
        // ghost-cleanup path -- otherwise the "INDEXING" text ghosts under the rendered page.
        pagesUntilFullRefresh = 1;
        // No popup redraws while the framebuffer is lent to the build below;
        // the panel holds the popup displayed above (e-ink is persistent).
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        // Lend the framebuffer's 48 KB to the blocking full build; restored
        // (white) at scope exit, and the page render below redraws everything.
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                        SETTINGS.extraParagraphSpacing, SETTINGS.effParagraphAlignment(), viewportWidth,
                                        viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();  // restore before anything draws
          showBuildError();
          return;
        }
        loan.end();
      } else {
        // Lay out just enough to show the landing page; loop() builds the rest behind it. Show the
        // indexing popup up front only when the build will actually be slow: a large spine (its
        // whole HTML must be inflated before page 1 can lay out -- the giant single-spine case), or
        // a deep resume/jump that must lay out many pages to reach the landing page. Tiny sections
        // build in a blink and stay popup-free.
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const size_t spineBytes = epub->getCumulativeSpineItemSize(currentSpineIndex) -
                                  (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
        // Popup only when the build will actually be slow: a big spine whose HTML still needs
        // inflating (the multi-second cost), or a deep page target. A reopen with cached HTML builds
        // fast, so no popup -- that's what made an already-indexed book look like it was reindexing.
        // A partial cache that already covers the target page shows it instantly: never popup.
        const bool willInflate = !section->hasHtmlCache();
        const bool anchorJump = !pendingAnchor.empty();
        bool showPopup;
        if (anchorJump) {
          // An anchor jump's cost is bounded by the anchor's page, not `target`. An anchor already
          // in the on-disk map (partial or finalized cache) lands instantly: no popup. Otherwise it
          // lies beyond the indexed watermark and the build may lay out the whole spine to find it,
          // so gate on spine size alone -- laying out a big spine takes seconds even with cached
          // HTML. Ordinary chapter-top TOC jumps resolve on page 0 and stay popup-free.
          showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
        } else {
          const bool targetAvailable = target < static_cast<int>(section->pageCount);
          showPopup = !targetAvailable &&
                      ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) || target > BUILD_POPUP_PAGE_THRESHOLD);
        }
        if (showPopup) {
          GUI.drawPopup(renderer, tr(STR_INDEXING));
          // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts under the page.
          pagesUntilFullRefresh = 1;
        }
        // Mid-build popup surfacing for slow builds the predictive gates can't
        // see (a whole-image extraction fallback inside a single page, or any
        // chunk overrunning the deadline). The parser fires the callback before
        // a slow image fallback; buildPopupPending gates it to this blocking
        // phase so a background build in loop() can never draw over a page.
        buildPopupPending = !showPopup;
        const unsigned long buildStartMs = millis();
        bool started;
        {
          // Lend the framebuffer's 48 KB to startBuild only (the spine HTML
          // inflation peak). The chunk loop below runs without it so the popup
          // can draw mid-build; background chunks never had the loan either.
          GfxRenderer::FrameBufferLoan loan(renderer);
          started =
              section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.effParagraphAlignment(), viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, [this] { showBuildPopup(); });
        }
        if (!started) {
          LOG_ERR("ERS", "Failed to start section build");
          section.reset();
          buildPopupPending = false;
          showBuildError();
          return;
        }
        while (!section->isBuildComplete() &&
               (anchorJump ? !section->findAnchor(pendingAnchor) : static_cast<int>(section->pageCount) <= target)) {
          // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
          // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
          // Otherwise: build until the target page exists. loop() builds the rest behind it.
          if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
            // The predictive gates guessed fast but the build blew the silent budget.
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
      // Arabic font, different from the reading font) is in play; these say
      // exactly how much of it was spent reading the font vs everything else
      // in layout (HTML/CSS parsing, line-breaking, page-splitting). Reports
      // both the reading font and its resolved Arabic font (if different and
      // SD-backed) since a chapter's layout can touch both.
      if (auto* fcmForBuildStats = renderer.getFontCacheManager()) {
        const int readerFontId = SETTINGS.getReaderFontId();
        const int arabicFontId = renderer.getArabicFontIdFor(readerFontId);
        uint32_t prewarmMs = 0, readMs = 0, seeks = 0, glyphs = 0, bytes = 0;
        fcmForBuildStats->getSdFontDiagStats(readerFontId, prewarmMs, readMs, seeks, glyphs, bytes);
        if (seeks > 0 || bytes > 0) {
          char part[96];
          snprintf(part, sizeof(part), " font_sd_ms=%lu font_seeks=%lu font_glyphs=%lu font_bytes=%lu",
                   (unsigned long)readMs, (unsigned long)seeks, (unsigned long)glyphs, (unsigned long)bytes);
          strncat(buf, part, sizeof(buf) - strlen(buf) - 1);
        }
        if (arabicFontId != readerFontId) {
          uint32_t aPrewarmMs = 0, aReadMs = 0, aSeeks = 0, aGlyphs = 0, aBytes = 0;
          fcmForBuildStats->getSdFontDiagStats(arabicFontId, aPrewarmMs, aReadMs, aSeeks, aGlyphs, aBytes);
          if (aSeeks > 0 || aBytes > 0) {
            char part[96];
            snprintf(part, sizeof(part), " arabic_sd_ms=%lu arabic_seeks=%lu arabic_glyphs=%lu arabic_bytes=%lu",
                     (unsigned long)aReadMs, (unsigned long)aSeeks, (unsigned long)aGlyphs, (unsigned long)aBytes);
            strncat(buf, part, sizeof(buf) - strlen(buf) - 1);
          }
        }
      }
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
      if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
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

  // Extend the build to the requested page if needed (for partials and in-progress builds).
  // This runs every render, so it covers both the first page and any forward turn that gets
  // ahead of the background builder; pages already built do no work here.
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    // Start a build to extend a partial toward the requested page.
    if (!section->isBuilding() &&
        !section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                             SETTINGS.extraParagraphSpacing, SETTINGS.effParagraphAlignment(), viewportWidth,
                             viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                             SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
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
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
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

  // The requested page is now as built as it will get. If it still lands past the end,
  // clamp to the last real page: the UINT16_MAX "last page" sentinel from backward chapter
  // navigation, an explicit jump beyond a finished chapter, or a stale saved position.
  // Guarded on !isBuilding() because a still-building section's pageCount is only the current
  // watermark (not the final count) and has already been driven far enough by the loops above.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  // Apply a deferred settings-change reposition now that the real page count is known (a no-op for
  // a plain resume / unchanged pagination). If still building, this defers to loop() on completion.
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
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      // Retrying rebuilds a transiently corrupt section and usually recovers, but a page that keeps
      // failing would loop forever on a blank screen, so bound the retries before giving up.
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      // Abandon (not suspend) any active build BEFORE clearing: clearCache deletes the files,
      // and the destructor's suspend would otherwise commit tables into a deleted handle.
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;  // Reset so a later user-initiated navigation can try afresh
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();  // Try again after clearing cache
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;  // Reset the retry counter once a page loads cleanly

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
  }
  // Only persist when the position actually changed. render() also runs on menu,
  // bookmark and screenshot re-renders, and writeAtomic is several FAT ops for 6 bytes.
  // Every real page turn changes currentPage, so progress durability is unaffected.
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
}

bool EpubReaderActivity::applyDeferredReposition() {
  if (cachedChapterTotalPageCount == 0 || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  // Only remap when the chapter actually re-paginated (e.g. after a settings change). A plain
  // resume has identical pagination, so section->pageCount == cachedChapterTotalPageCount and
  // nothing moves.
  if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
    const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
    int newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  cachedChapterTotalPageCount = 0;  // consumed; don't read cached progress again
  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
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
                     FontCacheManager* fcm, size_t preEndScanArabicBytes, int preEndScanArabicFontId) {
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
  // prewarm_glyphs stayed 0 even after fixing the scanText_.empty() early return that
  // was skipping the Arabic prewarmCache() call outright -- so this also logs WHERE
  // that call actually went: scan_bytes (how much Arabic text the scan pass recorded
  // -- 0 means the scan itself isn't seeing the page's text at all, upstream of
  // prewarm), and path (which branch FontCacheManager::prewarmCache() took for that
  // font id: sd=SD-card font tracked by separate stats getBitmap() never touches,
  // none=font id not found anywhere, compressed=the path FontDecompressor::Stats
  // above actually measures).
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
    const char* pathStr = "?";
    switch (fcm->getLastArabicPrewarmPath()) {
      case FontCacheManager::LastPrewarmPath::NotAttempted:
        pathStr = "not_attempted";
        break;
      case FontCacheManager::LastPrewarmPath::NoFontFound:
        pathStr = "no_font";
        break;
      case FontCacheManager::LastPrewarmPath::SdCardFont:
        pathStr = "sd";
        break;
      case FontCacheManager::LastPrewarmPath::Compressed:
        pathStr = "compressed";
        break;
    }
    // entries: how many times drawArabicText() was called during THIS page's scan
    // pass (counted before its own early return, so this can't be skipped by
    // whatever's causing scan_bytes=0). font_missing: how many of those calls found
    // the resolved Arabic font id absent from fontMap (the early-return path) --
    // if entries>0 and font_missing==entries, that early return is exactly why
    // nothing ever gets recorded. If entries==0, drawArabicText() isn't even being
    // reached during scan, and the bug is further upstream (TextBlock/drawText).
    // prewarm_fonts: how many DISTINCT Arabic fonts got their own prewarm call this
    // page (a Quran surah-header page legitimately has 3: the reading font for ayah
    // body text, plus the banner's calligraphy and label fonts) -- confirms every
    // font recorded during the scan actually got prewarmed, not just one.
    char pathPart[170];
    snprintf(pathPart, sizeof(pathPart),
             " scan_bytes=%lu scan_font=%d path=%s entries=%lu font_missing=%lu last_font=%d prewarm_fonts=%lu",
             (unsigned long)fcm->getLastArabicScanTextBytes(), fcm->getLastArabicPrewarmFontId(), pathStr,
             (unsigned long)fcm->getArabicScanEntries(), (unsigned long)fcm->getArabicScanFontMissing(),
             fcm->getArabicScanLastResolvedFontId(), (unsigned long)fcm->getLastArabicPrewarmFontCount());
    strncat(statsPart, pathPart, sizeof(statsPart) - strlen(statsPart) - 1);
    // hits/misses/decomp/calls/prewarm_glyphs/etc. above are FontDecompressor's stats --
    // meaningless on a path=sd turn, since that's the flash-font decompressor, not the
    // SD card read that's actually on the critical path for a path=sd font. Append the
    // SD font's own stats (SdCardFont::Stats, reset every page alongside everything else
    // in PrewarmScope's ctor) so a slow SD-font turn shows sd_read_ms/seeks/glyphs/bytes
    // instead of stale numbers from whatever flash font last ran.
    if (pathStr[0] == 's') {  // "sd"
      uint32_t sdPrewarmTotalMs = 0, sdReadTimeMs = 0, seekCount = 0, uniqueGlyphs = 0, bitmapBytes = 0;
      fcm->getSdFontDiagStats(fcm->getLastArabicPrewarmFontId(), sdPrewarmTotalMs, sdReadTimeMs, seekCount,
                              uniqueGlyphs, bitmapBytes);
      char sdPart[128];
      snprintf(sdPart, sizeof(sdPart), " sd_prewarm_ms=%lu sd_read_ms=%lu seeks=%lu sd_glyphs=%lu sd_bytes=%lu",
               (unsigned long)sdPrewarmTotalMs, (unsigned long)sdReadTimeMs, (unsigned long)seekCount,
               (unsigned long)uniqueGlyphs, (unsigned long)bitmapBytes);
      strncat(statsPart, sdPart, sizeof(statsPart) - strlen(statsPart) - 1);
    }
    // Diagnostics only: pre_bytes/pre_font are the SAME accumulator scan_bytes/scan_font
    // read above, but peeked immediately after the scan-pass render() call, before
    // endScanAndPrewarm() runs. If pre_bytes>0 while scan_bytes=0, something between the
    // peek and endScanAndPrewarm() clobbers scanArabicText_. If pre_bytes=0 too, the
    // accumulator was already empty at the earliest possible point, despite entries>0 --
    // meaning recordArabicText() itself isn't the one being called those `entries` times.
    char prePart[64];
    snprintf(prePart, sizeof(prePart), " pre_bytes=%lu pre_font=%d", (unsigned long)preEndScanArabicBytes,
             preEndScanArabicFontId);
    strncat(statsPart, prePart, sizeof(statsPart) - strlen(statsPart) - 1);
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

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  // Split point: the scan render (shaping + width measurement, no rasterization) vs
  // endScanAndPrewarm() (batched glyph prewarm) were previously lumped into one
  // "prewarm=" figure. On real-device SD-Arabic turns the scan render dominates
  // (prewarmTotalMs stays ~100ms), so time them separately to prove which phase the
  // multi-second cost is in before optimizing.
  const auto tScanRender = millis();
  // Diagnostics only: captured at the earliest possible point after the scan pass
  // returns, before endScanAndPrewarm() reads/clears the same accumulator a few lines
  // down -- see FontCacheManager::peekScanArabicTextSize().
  const size_t preEndScanArabicBytes = fcm->peekScanArabicTextSize();
  const int preEndScanArabicFontId = fcm->peekScanArabicFontId();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  // The reader starts with zero here, which means the normal refresh cycle would use
  // a HALF refresh for its first page. Keep that same clean base for image pages:
  // their double-FAST path otherwise runs directly over the retained frame after a
  // silent restart (e.g. returning from KOReader/Midad sync), leaving the old UI
  // mixed with the image.
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Whole-plane buffering only pays when the BW refresh genuinely runs async
  // underneath it; on a blocking panel it would just spend ~50KB for the
  // identical serial timing. Image pages take the blocking double-FAST path
  // below (no async refresh is ever started), so they'd spend the buffers
  // with nothing in flight to overlap.
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // Image pages intentionally bypass the regular refresh cadence. Preserve
      // a pending clean base before their double-FAST grayscale pipeline.
      if (cleanImageBasePending) {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    // Async form: start the waveform and return so the grayscale plane
    // rendering below overlaps the panel's refresh time instead of
    // following it.
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    // Render one plane band-by-band into a whole-plane buffer without
    // touching the controller, so it can run while the refresh is still in
    // flight.
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
    // inside the refresh wait; one hides the LSB render (its buffer is
    // reused for MSB after streaming); none falls back to the strip-scratch
    // flow below with no overlap. Each buffer is only attempted when it
    // leaves ~60KB free so the pass never starves concurrent allocations --
    // the next page re-render allocates through throwing std::string paths
    // that abort() on OOM under -fno-exceptions, so a plane buffer that
    // "fits" but eats the render headroom is worse than the strip fallback.
    // Blocking panels skip the buffers entirely: overlapRefresh is false, so
    // there is nothing in flight to hide the render behind.
    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    // Free-heap alone ignores fragmentation: taking the largest block for a
    // plane can leave only slivers behind even when total headroom looks
    // fine. Require the block to fit the plane with 16KB contiguous to
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

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
      logSlowPageTurn(t0, tScanRender, tPrewarm, tBwRender, tDisplay, tEnd, currentSpineIndex, epub->getTitle(), fcm,
                      preEndScanArabicBytes, preEndScanArabicFontId);
    } else {
      // Per-strip scratch tier: blocking panels and the OOM fallback. The
      // strip writes below need the panel idle, so wait out any pending
      // async refresh first (no-op when the BW push above was blocking).
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh) {
          // The BW refresh ran the shadow-free async path, so controller
          // RAM's differential baseline was never rebuilt. Even with AA
          // skipped it must be re-synced from the intact BW framebuffer, or
          // the next differential update diffs against stale contents.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        // Bands may be streamed in any order: X4 windows each via setRamArea, X3
        // via PTL.
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

        // MSB plane.
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

        // BW framebuffer is intact; re-sync controller RAM for the next
        // differential page turn directly from it.
        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
        logSlowPageTurn(t0, tScanRender, tPrewarm, tBwRender, tDisplay, tEnd, currentSpineIndex, epub->getTitle(), fcm,
                        preEndScanArabicBytes, preEndScanArabicFontId);
      }
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
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
      logSlowPageTurn(t0, tScanRender, tPrewarm, tBwRender, tDisplay, tEnd, currentSpineIndex, epub->getTitle(), fcm,
                      preEndScanArabicBytes, preEndScanArabicFontId);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
      logSlowPageTurn(t0, tScanRender, tPrewarm, tBwRender, tDisplay, tEnd, currentSpineIndex, epub->getTitle(), fcm,
                      preEndScanArabicBytes, preEndScanArabicFontId);
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
  // Calculate progress in book. Use the estimated total while a giant spine is still building so
  // "page X of Y" and the progress bar don't read off the small build watermark.
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->estimatedTotalPages();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

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

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = stripLeadingSurahWord(stripLeadingArabicIndicIndex(tocItem.title));
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section->isBuilding());
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;
  statsOnJump();

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
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
    RenderLock lock(*this);
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

  const std::string bmPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
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
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  const bool ok = JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str());
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
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
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
