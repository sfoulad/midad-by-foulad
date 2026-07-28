#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "activities/stats/StatsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "reading/ReadingStatsStore.h"
#include "util/CoverThumbs.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // eBooks, Stats, Files, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  // If any cover actually needs generating, release the ~38KB home-cover frame
  // snapshot (storeCoverBuffer) first: the JPEG/PNG cover converters need large
  // contiguous heap blocks (the PNG inflate ring buffer alone is 32KB), and with
  // the snapshot held, generation reliably failed HERE while succeeding for the
  // SAME book at the SAME size from the My Books grid, which holds no such
  // buffer -- confirmed on device (Home blank -> open grid, covers appear ->
  // back Home, covers now show, because Home could then just read the files the
  // grid had converted). The snapshot is rebuilt on the next render from the
  // freshly cached thumb files.
  bool anyToGenerate = false;      // invalid thumb (missing or failure marker), not yet attempted this boot
  bool anyNeverAttempted = false;  // thumb file absent entirely -- never attempted since the cache was cleared
  for (const RecentBook& book : recentBooks) {
    if (book.coverBmpPath.empty()) continue;
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
    if (Bitmap::isValidCachedBmp(thumbPath) || CoverThumbs::wasAttemptedThisBoot(thumbPath)) continue;
    anyToGenerate = true;
    if (!Storage.exists(thumbPath.c_str())) anyNeverAttempted = true;
  }

  if (anyToGenerate) {
    // Free the ~38KB frame snapshot and the reading-stats vectors before
    // converting: after a cache clear, generation means a FULL EPUB metadata
    // re-parse (content.opf/expat/BookMetadataCache) on top of the image decode
    // (PNG's inflate ring buffer alone is 32KB), and Home needs every spare
    // block for it. Both are rebuilt/reloaded on demand afterward.
    freeCoverBuffer();
    coverRendered = false;
    READING_STATS.releaseMemory();
  }

  // Fresh-boot rule, learned the hard way across three on-device iterations:
  // generation in a used session fails on Home in ways no amount of freeing our
  // own caches fixed (a heap-threshold heuristic in v1.6.45 also missed), while
  // generation right after a silent reboot demonstrably succeeds for every book
  // ("go to Update [which reboots] and back -> covers load everywhere"). So:
  // never-attempted covers are only generated on a fresh boot -- reboot to get
  // one if needed. Freshness is TIME SINCE BOOT only -- an earlier version also
  // counted bootWasSilentRestart(), but that flag stays true for the whole boot,
  // so a session that STARTED as a silent restart (e.g. the post-download
  // auto-open-reader path) still counted as fresh after a long reading session
  // and generated inline in a used heap, failing until the grid rescued it.
  // Cycle guards: (a) the fresh boot's inline attempt writes the empty marker
  // BMP on genuine failure; (b) marker-bearing thumbs (attempted on a fresh heap
  // and truly impossible, e.g. no cover art in the EPUB) never justify a reboot,
  // only the cheap once-per-boot inline retry.
  constexpr unsigned long kFreshBootWindowMs = 60000;
  const bool freshBoot = millis() < kFreshBootWindowMs;
  if (anyToGenerate) {
    CoverThumbs::diagLog(std::string("HOME scan: neverAttempted=") + (anyNeverAttempted ? "1" : "0") + " fresh=" +
                         (freshBoot ? "1" : "0") + (anyNeverAttempted && !freshBoot ? " -> reboot" : " -> inline"));
  }
  if (anyNeverAttempted && !freshBoot) {
    silentRestart();  // does not return
  }

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      // Bitmap::isValidCachedBmp (not a plain existence check) so a stale marker
      // from a PRIOR failed attempt doesn't permanently block a retry once the
      // underlying failure is fixed; CoverThumbs bounds those retries to once
      // per boot so a genuinely coverless book doesn't re-attempt (and re-show
      // the loading popup) on every Home entry.
      if (!Bitmap::isValidCachedBmp(coverPath) && !CoverThumbs::wasAttemptedThisBoot(coverPath)) {
        CoverThumbs::markAttempted(coverPath);
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here. Only generate if
          // the metadata actually loaded (matching the My Books grid) --
          // generateThumbBmp can't do anything without it.
          bool loaded = epub.load(false, true);
          bool built = false;
          if (!loaded) {
            // No metadata cache: the book was downloaded but never opened, or
            // the user cleared the SD cache -- the confirmed "covers never come
            // back until each book is reopened" case from /cover_diag_log.txt.
            // Build it now: a one-time OPF/TOC indexing pass per book, behind
            // the same loading popup the thumb conversion already shows.
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            loaded = built = epub.load(true, true);
          }
          bool generated = false;
          if (loaded) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            // Failure leaves book.coverBmpPath untouched (not cleared to "") --
            // see the isValidCachedBmp comment above for why a retry must stay
            // possible instead of being disabled forever.
            generated = epub.generateThumbBmp(coverHeight);
            coverRendered = false;
            requestUpdate();
          }
          CoverThumbs::diagLog(std::string("HOME epub load=") + (loaded ? "1" : "0") + " built=" + (built ? "1" : "0") +
                               " gen=" + (generated ? "1" : "0") + " " + book.path);
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          const bool loaded = xtc.load();
          bool generated = false;
          if (loaded) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            generated = xtc.generateThumbBmp(coverHeight);
            coverRendered = false;
            requestUpdate();
          }
          CoverThumbs::diagLog(std::string("HOME xtc load=") + (loaded ? "1" : "0") +
                               " gen=" + (generated ? "1" : "0") + " " + book.path);
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // Drop entries whose files were deleted since the store was loaded, so the
  // "+N" stack badge and stack-tile logic count only books that still exist.
  if (RECENT_BOOKS.pruneMissing() && !RECENT_BOOKS.saveToFile()) {
    LOG_ERR("HOME", "Failed to persist pruned recent books");
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem);

  lastIdleWhitenMs = millis();

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  // Anti-drift whitening: a static e-ink image slowly drifts gray over minutes
  // even with the Sunlight Fading Fix ON (confirmed on-device by photo) -- the
  // pigment relaxes regardless of the panel gate being powered down. A FAST
  // redraw of identical content drives NO pixels (FAST only pushes changed
  // ones), so the previous quiet-redraw version here did nothing for the
  // fade. Full-drive the panel instead (HALF refresh, ~1.7s blink). Only
  // fires after minutes of true idleness on this screen, never mid-use.
  constexpr unsigned long kIdleWhitenIntervalMs = 3UL * 60UL * 1000UL;
  if (millis() - lastIdleWhitenMs >= kIdleWhitenIntervalMs) {
    lastIdleWhitenMs = millis();
    idleWhitenPending = true;
    requestUpdate();
  }

  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < recentBooks.size()) {
      // The last recents tile becomes a "+N more" stack when the store holds more
      // books than the row shows (see FouladTheme); confirming it opens the full
      // Recent Books grid instead of silently opening one arbitrary book.
      const int totalRecents = static_cast<int>(RECENT_BOOKS.getBooks().size());
      const bool stackTile = selectorIndex == 3 && recentBooks.size() == 4 && totalRecents > 4;
      if (stackTile) {
        onRecentsOpen();
      } else {
        onSelectBook(recentBooks[selectorIndex].path);
      }
    } else {
      const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
      switch (indexToMenuItem(menuIndex)) {
        case HomeMenuItem::FOULAD_EBOOKS:
          // Signed out is not a separate destination: goToFouladEbooks() sends an
          // unconfigured device to FouladEbooksSetupActivity, whose onEnter() opens
          // the QR sign-in, and a configured one straight to the catalog.
          onFouladEbooksOpen();
          break;
        case HomeMenuItem::SETTINGS_MENU:
          onSettingsOpen();
          break;
        case HomeMenuItem::FILE_BROWSER:
          activityManager.goToFileBrowser("/");
          break;
        case HomeMenuItem::STATS:
          onStatsOpen();
          break;
        default:
          break;
      }
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // No GUI.drawHeader here: the Foulad theme draws its own status line (clock /
  // app name / battery) inside the cover tile. Drawing the standard header too
  // duplicated the battery indicator at the top of the screen.

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Order: eBooks, Stats, Files, Settings -- matches menuItemToIndex/indexToMenuItem.
  // Short labels chosen to fit the bottom icon bar tiles. Recent Books has no
  // menu entry anymore: the recents covers row (and its stacked +N tile, which
  // opens the full grid) took over that job.
  // Fixed, not login-dependent (user-specified). The slots used to swap on Foulad
  // eBooks login state -- eBooks/Files in slot 0, Files/Update in slot 2 -- which
  // moved two icons the moment an account was added or removed, and made eBooks
  // undiscoverable to exactly the people who had not signed in yet. Signed out,
  // eBooks now opens the QR sign-in (see loop()). Update keeps its permanent home
  // under Settings > System, where it already lived for signed-in devices.
  std::vector<const char*> menuItems = {tr(STR_EBOOKS), tr(STR_STATS), tr(STR_FILES), tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Library, Stats, Folder, Settings};

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  // No button-hints bar on the home screen (matching aalu): the icon menu is
  // self-explanatory, and dropping the bar frees the bottom strip for the menu.

  if (idleWhitenPending) {
    // Idle anti-fade pass: full-drive every pixel (FAST would skip unchanged
    // ones and leave the gray drift in place) -- see loop().
    idleWhitenPending = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer();
  }

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onStatsOpen() {
  startActivityForResult(std::make_unique<StatsActivity>(renderer, mappedInput), [](const ActivityResult&) {});
}

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFouladEbooksOpen() {
  // Reboot into the browser on a fresh heap (see SilentRestart.h): OPDS
  // browsing from a fragmented session heap ended in OOM aborts on-device.
  // Does not return.
  silentRestartToFouladEbooks();
}
