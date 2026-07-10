#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <ScriptDetector.h>
#include <Xtc.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "fontIds.h"
#include "util/CoverThumbs.h"
#include "util/GridNav.h"

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;

// SD library scan bounds: enough for any realistic personal library on these
// devices while keeping the path vector's RAM cost trivial (~50 bytes/entry).
constexpr size_t MAX_LIBRARY_BOOKS = 200;
constexpr size_t MAX_SCAN_DIRS = 64;
constexpr size_t NAME_BUF_SIZE = 256;

// Filename without directory or extension -- the caption for books that have
// never been opened (no metadata in the recents store yet). OPDS downloads are
// named "Author - Title.ext", so this reads naturally.
std::string filenameStem(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  const size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;
  return path.substr(start, end - start);
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  // "My Books" = every book on the SD card, not just the recently-opened list.
  // Downloaded-but-unopened books used to be invisible here (user request:
  // "recent books showing 3 books while in the sd card 6 books"). Recents come
  // first (most recently read first, preserving their stored title/author),
  // then every other .epub/.xtc found on the card, alphabetically.
  recentBooks = RECENT_BOOKS.getBooks();

  std::vector<std::string> dirs{"/"};
  std::vector<RecentBook> discovered;
  size_t dirsScanned = 0;
  char nameBuf[NAME_BUF_SIZE];

  // On-SD scan report (/mybooks_scan_log.txt): every directory visited and how
  // each entry was classified, so "a book on the card isn't showing up" can be
  // diagnosed from the SD card without a serial capture. Rewritten per scan.
  std::string report = "My Books scan -- CrossPoint version " CROSSPOINT_VERSION "\n";
  report += "recents in store: " + std::to_string(recentBooks.size()) + "\n";

  while (!dirs.empty() && dirsScanned < MAX_SCAN_DIRS &&
         recentBooks.size() + discovered.size() < MAX_LIBRARY_BOOKS) {
    const std::string dirPath = dirs.back();
    dirs.pop_back();
    dirsScanned++;

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      report += "OPEN-FAIL " + dirPath + "\n";
      continue;
    }
    dir.rewindDirectory();
    report += "DIR " + dirPath + "\n";

    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(nameBuf, sizeof(nameBuf));
      if (nameBuf[0] == '\0' || nameBuf[0] == '.') continue;  // hidden + .crosspoint/.fonts
      const std::string entryPath = (dirPath == "/") ? "/" + std::string(nameBuf) : dirPath + "/" + nameBuf;
      if (entry.isDirectory()) {
        if (strcmp(nameBuf, "System Volume Information") != 0 && strcmp(nameBuf, "fonts") != 0) {
          dirs.push_back(entryPath);
        }
        continue;
      }
      if (recentBooks.size() + discovered.size() >= MAX_LIBRARY_BOOKS) break;

      const bool isEpub = FsHelpers::hasEpubExtension(entryPath);
      const bool isXtc = FsHelpers::hasXtcExtension(entryPath);
      if (!isEpub && !isXtc) {
        if (report.size() < 4096) report += "  skip(ext) " + std::string(nameBuf) + "\n";
        continue;
      }

      const bool inRecents = std::any_of(recentBooks.begin(), recentBooks.end(),
                                         [&entryPath](const RecentBook& b) { return b.path == entryPath; });
      if (inRecents) {
        report += "  book(recent) " + std::string(nameBuf) + "\n";
        continue;
      }
      report += "  book(new) " + std::string(nameBuf) + "\n";

      RecentBook book;
      book.path = entryPath;
      book.title = filenameStem(entryPath);
      // Same [HEIGHT]-templated thumb path addBook stores for opened books, so
      // the grid's cover pipeline (incl. the build-cache-if-missing fallback)
      // works identically for never-opened books.
      book.coverBmpPath =
          isEpub ? Epub(entryPath, "/.crosspoint").getThumbBmpPath() : Xtc(entryPath, "/.crosspoint").getThumbBmpPath();
      discovered.push_back(std::move(book));
    }
  }

  report += "TOTAL recents=" + std::to_string(RECENT_BOOKS.getBooks().size()) +
            " new=" + std::to_string(discovered.size()) + " dirs=" + std::to_string(dirsScanned) + "\n";
  {
    HalFile logFile;
    if (Storage.openFileForWrite("MYBOOKS", "/mybooks_scan_log.txt", logFile)) {
      logFile.write(reinterpret_cast<const uint8_t*>(report.data()), report.size());
    }
  }
  LOG_INF("MYBOOKS", "scan: recents=%u new=%u dirs=%u", (unsigned)RECENT_BOOKS.getBooks().size(),
          (unsigned)discovered.size(), (unsigned)dirsScanned);

  std::sort(discovered.begin(), discovered.end(),
            [](const RecentBook& a, const RecentBook& b) { return a.title < b.title; });
  recentBooks.insert(recentBooks.end(), std::make_move_iterator(discovered.begin()),
                     std::make_move_iterator(discovered.end()));
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  loadedGridPageStart = NO_GRID_PAGE_LOADED;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: prompt to remove it from the list.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (!recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (recentBooks.empty()) return;

  const int listSize = static_cast<int>(recentBooks.size());
  const GridGeometry geometry = computeGridGeometry();

  auto moveUp = [this, listSize, geometry] {
    selectorIndex = GridNav::moveVertical(static_cast<int>(selectorIndex), listSize, geometry.columns,
                                          geometry.itemsPerPage, false);
    requestUpdate();
  };
  auto moveDown = [this, listSize, geometry] {
    selectorIndex =
        GridNav::moveVertical(static_cast<int>(selectorIndex), listSize, geometry.columns, geometry.itemsPerPage, true);
    requestUpdate();
  };
  auto moveLeft = [this, listSize] {
    selectorIndex = GridNav::moveHorizontal(static_cast<int>(selectorIndex), listSize, false);
    requestUpdate();
  };
  auto moveRight = [this, listSize] {
    selectorIndex = GridNav::moveHorizontal(static_cast<int>(selectorIndex), listSize, true);
    requestUpdate();
  };

  buttonNavigator.onRelease({MappedInputManager::Button::Up}, moveUp);
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, moveDown);
  buttonNavigator.onRelease({MappedInputManager::Button::Left}, moveLeft);
  buttonNavigator.onRelease({MappedInputManager::Button::Right}, moveRight);
  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, moveUp);
  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, moveDown);
  buttonNavigator.onContinuous({MappedInputManager::Button::Left}, moveLeft);
  buttonNavigator.onContinuous({MappedInputManager::Button::Right}, moveRight);
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= recentBooks.size()) {
        selectorIndex = recentBooks.size() - 1;
      }
      loadedGridPageStart = NO_GRID_PAGE_LOADED;
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

RecentBooksActivity::GridGeometry RecentBooksActivity::computeGridGeometry() const {
  GridGeometry geometry;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Column count first, from a target minimum cell width -- then the cover
  // size is derived to fill the resulting columns edge-to-edge.
  geometry.columns = std::max(1, (pageWidth - GRID_GUTTER) / (GRID_MIN_CELL_WIDTH + GRID_GUTTER));
  geometry.coverWidth = (pageWidth - GRID_GUTTER * (geometry.columns + 1)) / geometry.columns;
  geometry.coverHeight = static_cast<int>(geometry.coverWidth / 0.6f);

  // Thumbnail CACHE height stays fixed at the theme's canonical hero height
  // (matching HomeActivity::loadRecentCovers) instead of this grid's own
  // dynamically-computed cell height. Two different generateThumbBmp() heights
  // per book meant two independent cache files, two independent generation
  // attempts, and two independent success/failure outcomes for the exact same
  // source cover -- confirmed on a real device as the reason a book could show
  // its cover in this grid but not on Home (or vice versa) even after both
  // screens' cover logic was otherwise identical. drawBitmap already scales
  // this fixed-height source down to fit the grid's (device/orientation
  // dependent) cell box, same as FouladTheme's hero/thumbnail-row covers do.
  geometry.thumbHeight = metrics.homeCoverHeight;

  const int rowHeight = geometry.coverHeight + getGridTitleHeight() + GRID_GUTTER;
  const int rows = std::max(1, contentHeight / rowHeight);
  geometry.itemsPerPage = geometry.columns * rows;
  return geometry;
}

int RecentBooksActivity::getGridTitleHeight() const {
  const int lineHeight =
      std::max(renderer.getLineHeight(SMALL_FONT_ID), renderer.getLineHeight(NOTOSANSARABIC_8_FONT_ID));
  return GRID_TITLE_TOP_GAP + lineHeight * GRID_TITLE_LINES;
}

void RecentBooksActivity::loadGridPageCovers(const int pageStart) {
  const GridGeometry geometry = computeGridGeometry();
  const int pageEnd = std::min(pageStart + geometry.itemsPerPage, static_cast<int>(recentBooks.size()));

  bool needsGeneration = false;
  for (int i = pageStart; i < pageEnd; i++) {
    const RecentBook& book = recentBooks[i];
    if (book.coverBmpPath.empty()) continue;
    // isValidCachedBmp, not a plain existence check -- see the comment on the
    // matching check below for why a stale failure marker must not block a retry.
    // Unlike Home, the grid does NOT consult CoverThumbs::wasAttemptedThisBoot:
    // opening this screen is an explicit user action (its loading popup is
    // expected), and the grid is the memory-favorable context that has
    // historically succeeded where Home's attempt failed -- gating it behind
    // Home's failed attempt turned "grid rescues the covers" into "nothing
    // does" (confirmed on device after a cache clear).
    if (!Bitmap::isValidCachedBmp(UITheme::getCoverThumbPath(book.coverBmpPath, geometry.thumbHeight))) {
      needsGeneration = true;
      break;
    }
  }
  if (!needsGeneration) {
    loadedGridPageStart = pageStart;
    return;
  }

  bool showingLoading = false;
  Rect popupRect;
  const int totalToProcess = pageEnd - pageStart;
  int processedCount = 0;

  for (int i = pageStart; i < pageEnd; i++) {
    RecentBook& book = recentBooks[i];
    if (!book.coverBmpPath.empty()) {
      const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, geometry.thumbHeight);
      // Bitmap::isValidCachedBmp (not a plain existence check) so a stale marker
      // from a PRIOR failed attempt doesn't permanently block a retry once the
      // underlying failure is fixed. The grid always attempts (see the pre-scan
      // comment above) but still records the attempt so HOME's once-per-boot
      // gate stays quiet for a book the grid itself couldn't generate. Failure
      // leaves book.coverBmpPath untouched (not cleared to "") for the same reason.
      if (!Bitmap::isValidCachedBmp(coverPath)) {
        CoverThumbs::markAttempted(coverPath);
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          bool loaded = epub.load(false, true);
          bool built = false;
          if (!loaded) {
            // Metadata cache missing (never opened / cache cleared): build it
            // now behind the loading popup -- see HomeActivity::loadRecentCovers.
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
            loaded = built = epub.load(true, true);
          }
          bool generated = false;
          if (loaded) {
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
            generated = epub.generateThumbBmp(geometry.thumbHeight);
            // Never-opened books enter the list with a filename-stem caption;
            // now that the metadata is loaded anyway, use the real title.
            const std::string title = epub.getTitle();
            if (!title.empty()) book.title = title;
          }
          CoverThumbs::diagLog(std::string("GRID epub load=") + (loaded ? "1" : "0") + " built=" +
                               (built ? "1" : "0") + " gen=" + (generated ? "1" : "0") + " " + book.path);
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          Xtc xtc(book.path, "/.crosspoint");
          const bool loaded = xtc.load();
          bool generated = false;
          if (loaded) {
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
            generated = xtc.generateThumbBmp(geometry.thumbHeight);
          }
          CoverThumbs::diagLog(std::string("GRID xtc load=") + (loaded ? "1" : "0") +
                               " gen=" + (generated ? "1" : "0") + " " + book.path);
        }
      }
    }
    processedCount++;
  }

  loadedGridPageStart = pageStart;
  if (showingLoading) {
    requestUpdate();
  }
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // "My Books" (STR_RECENTS), matching the home screen's divider label: this
  // page lists every book on the SD card now, not just recently-opened ones.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_RECENTS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  int gridPageStart = 0;
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    const GridGeometry geometry = computeGridGeometry();
    const int titleHeight = getGridTitleHeight();
    gridPageStart = (static_cast<int>(selectorIndex) / geometry.itemsPerPage) * geometry.itemsPerPage;
    const int pageCount = std::min(geometry.itemsPerPage, static_cast<int>(recentBooks.size()) - gridPageStart);
    const int totalGridWidth = geometry.columns * (geometry.coverWidth + GRID_GUTTER) - GRID_GUTTER;
    const int gridStartX = std::max(0, (static_cast<int>(pageWidth) - totalGridWidth) / 2);

    for (int i = 0; i < pageCount; i++) {
      const int bookIdx = gridPageStart + i;
      const auto& book = recentBooks[bookIdx];
      const int col = i % geometry.columns;
      const int row = i / geometry.columns;
      const int cellX = gridStartX + col * (geometry.coverWidth + GRID_GUTTER);
      const int cellY = contentTop + row * (geometry.coverHeight + titleHeight + GRID_GUTTER);

      bool drawn = false;
      if (!book.coverBmpPath.empty()) {
        // Read from the shared canonical-height thumb (see computeGridGeometry);
        // drawBitmap scales it down to this grid cell's own display size.
        const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, geometry.thumbHeight);
        if (Storage.exists(coverPath.c_str())) {
          HalFile file;
          if (Storage.openFileForRead("RBA", coverPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              renderer.drawBitmap(bitmap, cellX, cellY, geometry.coverWidth, geometry.coverHeight);
              drawn = true;
            }
          }
        }
      }
      renderer.drawRect(cellX, cellY, geometry.coverWidth, geometry.coverHeight);
      if (!drawn) {
        renderer.drawIcon(BookIcon, cellX + (geometry.coverWidth - 32) / 2, cellY + (geometry.coverHeight - 32) / 2,
                          32);
      }
      if (bookIdx == static_cast<int>(selectorIndex)) {
        // A 1px outline was hard to spot at a glance on e-ink, especially across a
        // multi-column grid where the eye has to search for it -- a thick (4px) border
        // reads as a deliberate, high-contrast selection frame instead.
        renderer.drawRect(cellX - 4, cellY - 4, geometry.coverWidth + 8, geometry.coverHeight + 8, 4, true);
      }

      const auto titleLines =
          renderer.wrappedText(SMALL_FONT_ID, book.title.c_str(), geometry.coverWidth, GRID_TITLE_LINES);
      // Was previously widened per-title for an Arabic title (using the taller Arabic line
      // height between its two lines). Explicit user feedback preferred the gap between
      // lines matching the Latin/English spacing exactly over the extra clipping headroom
      // -- always uses the tight Latin height now; revisit if Arabic titles start clipping.
      const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      int titleY = cellY + geometry.coverHeight + GRID_TITLE_TOP_GAP;
      for (const auto& line : titleLines) {
        renderer.drawTextInWidth(SMALL_FONT_ID, cellX, titleY, geometry.coverWidth, line.c_str());
        titleY += titleLineHeight;
      }
    }
  }

  renderer.displayBuffer();

  if (!recentBooks.empty() && loadedGridPageStart != gridPageStart) {
    loadGridPageCovers(gridPageStart);
  }
}
