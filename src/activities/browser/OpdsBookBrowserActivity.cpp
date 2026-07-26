#include "OpdsBookBrowserActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <ScriptDetector.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <memory>

#include "CrossPointState.h"
#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "MappedInputManager.h"
#include "OpdsCoverCache.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/DebugLog.h"
#include "util/DebugLogging.h"
#include "util/GridNav.h"
#include "util/RollingSdLog.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
// Extra headroom above/below the tallest glyph in a row, matching the ~6px cushion the
// previous fixed 30px row height gave the Latin font (24px advanceY) -- see
// OpdsBookBrowserActivity::getListRowHeight().
constexpr int LIST_ROW_VERTICAL_PADDING = 6;

// Foulad eBooks' catalog feed writes each book entry's <id> as
// "urn:opds-library:book:<numeric id>" (see foulad-ebooks'
// resources/views/opds/books.blade.php) -- the same numeric id used for
// device reading-stats reporting (EINK_DEVICE_TRACKING_TASKS.md). Returns ""
// for anything that doesn't end in a run of digits (a non-Foulad OPDS
// server's own id convention, or a malformed entry) so callers can treat
// that as "no Foulad book id available" rather than a parse error.
std::string extractFouladBookId(const std::string& entryId) {
  size_t end = entryId.size();
  while (end > 0 && isdigit(static_cast<unsigned char>(entryId[end - 1]))) end--;
  if (end == entryId.size()) return "";  // no trailing digits at all
  return entryId.substr(end);
}

int moveHorizontalInGrid(const int currentIndex, const int totalItems, const bool moveRight) {
  return GridNav::moveHorizontal(currentIndex, totalItems, moveRight);
}

int moveVerticalInGrid(const int currentIndex, const int totalItems, const int columns, const int itemsPerPage,
                       const bool moveDown) {
  return GridNav::moveVertical(currentIndex, totalItems, columns, itemsPerPage, moveDown);
}

// Mirrors HalSystem::checkPanic's crash_report.txt mechanism, but for an OPDS feed
// fetch/parse failure instead of a device panic: dumps the same rolling log ring
// buffer (getLastLogs(), last 16 lines -- HttpDownloader's LOG_ERR lines survive in
// release builds since LOG_ERR is always compiled in) into the shared debug log
// (see util/DebugLog.h). Lets the user grab a diagnostic log via File
// Browser/File Transfer without needing a live serial connection, the same way
// they already can for a crash.
void saveOpdsDiagnosticLog(const std::string& context) {
  if (!DebugLogging::enabled()) return;
  std::string info = "[OPDS-ERROR] CrossPoint version: " CROSSPOINT_VERSION;
  info += " | Context: " + context;
  info += "\nLast logs:\n" + getLastLogs();

  RollingSdLog::append(DebugLog::PATH, info, DebugLog::MAX_LINES);
  LOG_INF("OPDS", "Saved diagnostic log to %s", DebugLog::PATH);
}

// Rolling browse trace, tagged into the shared debug log (see util/DebugLog.h):
// one line per feed fetch (URL, entry/book counts, the pagination links the
// parser captured) and per grid auto-advance decision. Diagnoses pagination
// misbehavior ("after page 4 it goes back to page 1") from the SD card:
// whether the next link was missing from the parse or the navigation itself
// picked the wrong URL.
void browseLog(const std::string& line) {
  LOG_INF("OPDS", "%s", line.c_str());

  char prefix[48];
  snprintf(prefix, sizeof(prefix), "[OPDS] [%lus heap=%u] ", millis() / 1000UL,
           static_cast<unsigned>(ESP.getFreeHeap()));
  RollingSdLog::append(DebugLog::PATH, prefix + line, DebugLog::MAX_LINES);
}
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  entries.clear();
  navigationHistory.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    if (!pendingReaderPath.empty()) {
      // Leaving to open a specific book (see downloadBook()) -- resume there after the
      // restart, not at Home. openEpubPath is only an in-memory field; it must be
      // explicitly persisted before ESP.restart() wipes RAM, or boot falls back to
      // whatever was last saved to disk (the book the user was already reading before
      // opening Foulad eBooks) instead of the one they just downloaded -- confirmed as
      // a real device bug: download a new book, and it silently reopens the old one.
      APP_STATE.openEpubPath = pendingReaderPath;
      APP_STATE.saveToFile();
      silentRestartToReader();
    } else {
      // NOT silentRestartToFouladEbooks() here even when server.url ==
      // FOULAD_EBOOKS_URL: that call is how Foulad eBooks is *entered* (see
      // HomeActivity::onFouladEbooksOpen(), FouladEbooksSetupActivity) --
      // calling it from this activity's own onExit() rebooted straight back
      // into itself, so pressing Back to leave (no download in flight)
      // reconnected WiFi and relaunched the same browser with no way out.
      // Home is this activity's actual caller in every case; exiting always
      // goes there, same as any other OPDS server.
      silentRestart();
    }
  }
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  // Re-check for a pushed settings change every ~30s while sitting on the
  // catalog -- BROWSING only (not LOADING/DOWNLOADING) so this never
  // competes with an in-flight fetch/download for the same connection.
  if (state == BrowserState::BROWSING && server.url == FOULAD_EBOOKS_URL && WiFi.status() == WL_CONNECTED) {
    const unsigned long nowMs = millis();
    if (nowMs - lastDeviceTrackingCheckMs >= DEVICE_TRACKING_RECHECK_MS) {
      lastDeviceTrackingCheckMs = nowMs;
      FouladDeviceTracking::registerDevice(server.username, server.password);
    }
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    const GridLayout layout = computeGridLayout();
    const bool onBook = !entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK;

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) && !onBook) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    if (entries.empty()) {
      // nothing to navigate
    } else if (!layout.isGridPage) {
      // Plain list navigation (category/navigation-only pages) — unchanged.
      buttonNavigator.onScrollNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onScrollPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onScrollNextContinuous([this] {
        const int pageItems = getListPageItems(getListRowHeight());
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), pageItems);
        requestUpdate();
      });
      buttonNavigator.onScrollPreviousContinuous([this] {
        const int pageItems = getListPageItems(getListRowHeight());
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), pageItems);
        requestUpdate();
      });
    } else if (onBook) {
      // Inside the cover grid: Left/Right cycle through books; Up/Down move by
      // row, escaping to the nav strip above/below at the grid's true edges.
      // Any movement that would WRAP around the in-memory books instead pages
      // the server feed (when it has a next/previous link). The first attempt
      // hooked only Left/Right on the exact first/last book, but users page
      // the grid with Up/Down, which wraps page-by-page without ever standing
      // on the last book -- the browse log showed 4 grid pages then a silent
      // wrap to page 1 with no auto-advance ever firing. Detecting the wrap
      // itself (candidate index jumping backwards on a forward move, or
      // forwards on a backward move) covers every direction and page shape.
      auto goToFeedPage = [this](const bool forward) {
        pendingGridSelect = forward ? PendingGridSelect::FirstBook : PendingGridSelect::LastBook;
        const std::string& url = forward ? feedNextUrl : feedPrevUrl;
        browseLog(std::string(forward ? "AUTO-NEXT -> " : "AUTO-PREV -> ") + url);
        navigateToEntry(OpdsEntry{OpdsEntryType::NAVIGATION, "", "", url, ""});
      };
      auto moveUp = [this, layout, goToFeedPage] {
        if (selectorIndex == layout.bookStart && layout.topNavCount > 0) {
          selectorIndex = 0;
          requestUpdate();
          return;
        }
        const int local = selectorIndex - layout.bookStart;
        const int candidate = moveVerticalInGrid(local, layout.bookCount, layout.columns, layout.itemsPerPage, false);
        if (candidate >= local && !feedPrevUrl.empty()) {
          goToFeedPage(false);
          return;
        }
        selectorIndex = layout.bookStart + candidate;
        requestUpdate();
      };
      auto moveDown = [this, layout, goToFeedPage] {
        if (selectorIndex == layout.bookStart + layout.bookCount - 1 &&
            layout.bottomNavStart < static_cast<int>(entries.size())) {
          selectorIndex = layout.bottomNavStart;
          requestUpdate();
          return;
        }
        const int local = selectorIndex - layout.bookStart;
        const int candidate = moveVerticalInGrid(local, layout.bookCount, layout.columns, layout.itemsPerPage, true);
        if (candidate <= local && !feedNextUrl.empty()) {
          goToFeedPage(true);
          return;
        }
        selectorIndex = layout.bookStart + candidate;
        requestUpdate();
      };
      auto moveLeft = [this, layout, goToFeedPage] {
        const int local = selectorIndex - layout.bookStart;
        const int candidate = moveHorizontalInGrid(local, layout.bookCount, false);
        if (candidate >= local && !feedPrevUrl.empty()) {
          goToFeedPage(false);
          return;
        }
        selectorIndex = layout.bookStart + candidate;
        requestUpdate();
      };
      auto moveRight = [this, layout, goToFeedPage] {
        const int local = selectorIndex - layout.bookStart;
        const int candidate = moveHorizontalInGrid(local, layout.bookCount, true);
        if (candidate <= local && !feedNextUrl.empty()) {
          goToFeedPage(true);
          return;
        }
        selectorIndex = layout.bookStart + candidate;
        requestUpdate();
      };

      buttonNavigator.onRelease({MappedInputManager::Button::Up}, moveUp);
      buttonNavigator.onRelease({MappedInputManager::Button::Down}, moveDown);
      buttonNavigator.onRelease({MappedInputManager::Button::Right}, moveRight);
      buttonNavigator.onRelease({MappedInputManager::Button::Left}, moveLeft);
      buttonNavigator.onContinuous({MappedInputManager::Button::Up}, moveUp);
      buttonNavigator.onContinuous({MappedInputManager::Button::Down}, moveDown);
      buttonNavigator.onContinuous({MappedInputManager::Button::Right}, moveRight);
      buttonNavigator.onContinuous({MappedInputManager::Button::Left}, moveLeft);
    } else {
      // Selection is in a nav strip (top or bottom) of a grid page: a small
      // flat list scoped to that strip; Up/Down hand off to/from the grid.
      const bool inTopStrip = selectorIndex < layout.bookStart;
      const int stripStart = inTopStrip ? 0 : layout.bottomNavStart;
      const int stripCount = inTopStrip ? layout.topNavCount : static_cast<int>(entries.size()) - layout.bottomNavStart;

      buttonNavigator.onRelease({MappedInputManager::Button::Right}, [this, stripStart, stripCount] {
        selectorIndex = stripStart + moveHorizontalInGrid(selectorIndex - stripStart, stripCount, true);
        requestUpdate();
      });
      buttonNavigator.onRelease({MappedInputManager::Button::Left}, [this, stripStart, stripCount] {
        selectorIndex = stripStart + moveHorizontalInGrid(selectorIndex - stripStart, stripCount, false);
        requestUpdate();
      });
      buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this, layout, inTopStrip] {
        if (inTopStrip) {
          selectorIndex = layout.bookStart;
          requestUpdate();
        }
      });
      buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this, layout, inTopStrip] {
        if (!inTopStrip) {
          selectorIndex = layout.bookStart + layout.bookCount - 1;
          requestUpdate();
        }
      });
    }
  }
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Show server name in header if available, otherwise generic title
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerTitle, true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    // Layout matches OtaUpdateActivity's progress display for a consistent look between
    // the two long-running download/transfer screens in the app.
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const int top = pageHeight / 2 - 60;

    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_DOWNLOADING));
    auto title =
        renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - metrics.contentSidePadding * 2);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, title.c_str());

    int y = top + (height + metrics.verticalSpacing) * 2;
    if (downloadTotal > 0) {
      GUI.drawProgressBar(
          renderer,
          Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
          downloadProgress, downloadTotal);

      y += metrics.progressBarHeight + metrics.verticalSpacing;
      // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally
      // empty so the bytes line below stays at the same Y as OtaUpdateActivity's layout.
      y += height + metrics.verticalSpacing;
      renderer.drawCenteredText(UI_10_FONT_ID, y,
                                (std::to_string(downloadProgress) + " / " + std::to_string(downloadTotal)).c_str());
    } else if (downloadProgress > 0) {
      // Total size is unknown (chunked transfer, no Content-Length): fill the bar against
      // an estimated ceiling so there's still a moving visual, but suppress the percentage
      // label (it would be precise-looking but not actually accurate) in favor of the real
      // byte count below.
      GUI.drawProgressBar(
          renderer,
          Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
          std::min(downloadProgress, ESTIMATED_DOWNLOAD_SIZE), ESTIMATED_DOWNLOAD_SIZE,
          /*showPercentage=*/false);

      y += metrics.progressBarHeight + metrics.verticalSpacing;
      renderer.drawCenteredText(UI_10_FONT_ID, y, (std::to_string(downloadProgress / 1024) + " KB downloaded").c_str());
    }
    renderer.displayBuffer();
    return;
  }

  const GridLayout layout = computeGridLayout();
  const bool onBook = !entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK;

  const char* confirmLabel = onBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const char* leftLabel;
  const char* rightLabel;
  // Grid-page book cells use genuinely horizontal Left/Right (mirror under RTL,
  // like any other horizontal grid); the plain-list and nav-strip cases below use
  // a vertical Up/Down pair instead (down is still down regardless of script
  // direction -- see mapLabels' rtlSwap parameter).
  const bool rtlSwapLabels = layout.isGridPage && onBook;
  if (rtlSwapLabels) {
    leftLabel = tr(STR_DIR_LEFT);
    rightLabel = tr(STR_DIR_RIGHT);
  } else {
    leftLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
    rightLabel = tr(STR_DIR_DOWN);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, leftLabel, rightLabel, rtlSwapLabels);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  int gridPageStart = 0;
  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else if (!layout.isGridPage) {
    // Plain text list (category/navigation-only pages). Row height is kept tight and
    // uniform (see getListRowHeight()) so Arabic and Latin rows match visually.
    const int rowHeight = getListRowHeight();
    const int pageItems = getListPageItems(rowHeight);
    const auto pageStartIndex = selectorIndex / pageItems * pageItems;
    // Extend the highlight downward only, not split symmetrically above/below: text draws
    // from a fixed top anchor (y = row top, baseline = y + font ascender), and Arabic's
    // larger ascender+descender sum extends the glyph bounds further DOWN from that same
    // top -- the top of the glyph doesn't move. Splitting the extra height evenly (an
    // earlier attempt) only gave half the needed room at the bottom and still clipped it.
    renderer.fillRect(0, GRID_CONTENT_TOP + (selectorIndex % pageItems) * rowHeight - 2, pageWidth - 1,
                      getListRowHighlightHeight(entries[selectorIndex].title));

    for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + pageItems); i++) {
      const auto& entry = entries[i];
      std::string displayText = (entry.type == OpdsEntryType::NAVIGATION) ? "> " + entry.title : entry.title;
      if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) displayText += " - " + entry.author;
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawTextInWidth(UI_10_FONT_ID, 20, GRID_CONTENT_TOP + (i % pageItems) * rowHeight, pageWidth - 40,
                               item.c_str(), i != static_cast<size_t>(selectorIndex));
    }
  } else {
    // Nav strip above the grid (e.g. a "Prev Page" entry). Same tight row height as the
    // plain list above.
    const int rowHeight = getListRowHeight();
    const int titleHeight = getGridTitleHeight();
    int y = GRID_CONTENT_TOP;
    for (int i = 0; i < layout.topNavCount; i++) {
      const auto& entry = entries[i];
      auto item = renderer.truncatedText(UI_10_FONT_ID, ("> " + entry.title).c_str(), pageWidth - 40);
      if (i == selectorIndex) {
        // Extends downward only -- see the plain-list highlight above for why.
        renderer.fillRect(0, y - 2, pageWidth - 1, getListRowHighlightHeight(entry.title));
      }
      renderer.drawTextInWidth(UI_10_FONT_ID, 20, y, pageWidth - 40, item.c_str(), i != selectorIndex);
      y += rowHeight;
    }
    const int gridTop = y + (layout.topNavCount > 0 ? 10 : 0);

    const bool selectionInGrid = onBook;
    const int localSelector = selectionInGrid ? selectorIndex - layout.bookStart : 0;
    gridPageStart = (localSelector / layout.itemsPerPage) * layout.itemsPerPage;
    const int pageCount = std::min(layout.itemsPerPage, layout.bookCount - gridPageStart);
    const int totalGridWidth = layout.columns * (layout.coverWidth + GRID_GUTTER) - GRID_GUTTER;
    const int gridStartX = std::max(0, (pageWidth - totalGridWidth) / 2);

    for (int i = 0; i < pageCount; i++) {
      const int bookIdx = layout.bookStart + gridPageStart + i;
      const auto& entry = entries[bookIdx];
      const int col = i % layout.columns;
      const int row = i / layout.columns;
      const int cellX = gridStartX + col * (layout.coverWidth + GRID_GUTTER);
      const int cellY = gridTop + row * (layout.coverHeight + titleHeight + GRID_GUTTER);

      bool drawn = false;
      if (!entry.coverUrl.empty()) {
        const std::string coverPath = getOpdsCoverCachePath(entry.id, layout.coverWidth, layout.coverHeight);
        if (Storage.exists(coverPath.c_str())) {
          HalFile file;
          if (Storage.openFileForRead("OPDS", coverPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              // The cache file may be smaller than the cell (OpdsCoverCache never upscales a
              // source cover below the target size) -- center it instead of pinning to the
              // top-left corner. drawBitmap only ever scales down, never up, so this stays a
              // no-op for the common case where the cache file already matches the cell.
              const int offsetX = std::max(0, (layout.coverWidth - bitmap.getWidth()) / 2);
              const int offsetY = std::max(0, (layout.coverHeight - bitmap.getHeight()) / 2);
              renderer.drawBitmap(bitmap, cellX + offsetX, cellY + offsetY, layout.coverWidth, layout.coverHeight);
              drawn = true;
            }
          }
        }
      }
      renderer.drawRect(cellX, cellY, layout.coverWidth, layout.coverHeight);
      if (!drawn) {
        renderer.drawIcon(BookIcon, cellX + (layout.coverWidth - 32) / 2, cellY + (layout.coverHeight - 32) / 2, 32);
      }
      if (bookIdx == selectorIndex) {
        // A 1px outline was hard to spot at a glance on e-ink, especially across a
        // multi-column grid where the eye has to search for it -- a thick (4px) border
        // reads as a deliberate, high-contrast selection frame instead.
        renderer.drawRect(cellX - 4, cellY - 4, layout.coverWidth + 8, layout.coverHeight + 8, 4, true);
      }

      const auto titleLines =
          renderer.wrappedText(SMALL_FONT_ID, entry.title.c_str(), layout.coverWidth, GRID_TITLE_LINES);
      // Was previously widened per-title for an Arabic title (using the taller Arabic line
      // height between its two lines). Explicit user feedback preferred the gap between
      // lines matching the Latin/English spacing exactly over the extra clipping headroom
      // -- always uses the tight Latin height now; revisit if Arabic titles start clipping.
      const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      int titleY = cellY + layout.coverHeight + GRID_TITLE_TOP_GAP;
      for (const auto& line : titleLines) {
        renderer.drawTextInWidth(SMALL_FONT_ID, cellX, titleY, layout.coverWidth, line.c_str());
        titleY += titleLineHeight;
      }
    }

    // Nav strip below the grid (e.g. a "Next Page" entry). Same tight row height.
    const int bottomCount = static_cast<int>(entries.size()) - layout.bottomNavStart;
    if (bottomCount > 0) {
      int by = pageHeight - GRID_BOTTOM_MARGIN - bottomCount * rowHeight;
      for (int i = layout.bottomNavStart; i < static_cast<int>(entries.size()); i++) {
        const auto& entry = entries[i];
        auto item = renderer.truncatedText(UI_10_FONT_ID, ("> " + entry.title).c_str(), pageWidth - 40);
        if (i == selectorIndex) {
          // Extends downward only -- see the plain-list highlight above for why.
          renderer.fillRect(0, by - 2, pageWidth - 1, getListRowHighlightHeight(entry.title));
        }
        renderer.drawTextInWidth(UI_10_FONT_ID, 20, by, pageWidth - 40, item.c_str(), i != selectorIndex);
        by += rowHeight;
      }
    }
  }
  renderer.displayBuffer();

  if (layout.isGridPage && !entries.empty() && loadedGridPageStart != gridPageStart) {
    loadGridPageCovers(layout, gridPageStart);
  }
}

int OpdsBookBrowserActivity::getListRowHeight() const {
  // Explicit user feedback preferred visually consistent, tighter row-to-row spacing matching
  // the Latin rows over extra clipping headroom, so this always uses the tight Latin height
  // regardless of script -- see getListRowHighlightHeight() for where an Arabic-tall selection
  // highlight is still handled separately from this pitch.
  return renderer.getLineHeight(UI_10_FONT_ID) + LIST_ROW_VERTICAL_PADDING;
}

int OpdsBookBrowserActivity::getListRowHighlightHeight(const std::string& title) const {
  const int rowHeight = getListRowHeight();
  if (!ScriptDetector::containsArabic(title.c_str())) return rowHeight;
  // A selected Arabic row inverts to white-on-black; unlike the unselected case (where a
  // slightly-too-tall glyph just quietly overlaps the next row's white background), any part
  // of the glyph taller than the tight Latin pitch gets visibly sliced off at the highlight
  // rectangle's edge. Widen just the highlight for this one row instead of the pitch itself.
  // Since UI Arabic is baseline-matched to the Latin font (see GfxRenderer's
  // arabicBaselineMatchFontIds_), only the deeper Arabic descenders overshoot -- a few px,
  // matching BaseTheme::drawList's identical bump, not the full Arabic line height (which
  // produced a comically oversized 50px highlight on a 30px row once baselines matched).
  return rowHeight + 4;
}

int OpdsBookBrowserActivity::getListPageItems(const int rowHeight) const {
  const int available = renderer.getScreenHeight() - GRID_CONTENT_TOP - GRID_BOTTOM_MARGIN;
  return std::max(1, available / rowHeight);
}

int OpdsBookBrowserActivity::getGridTitleHeight() const {
  const int lineHeight =
      std::max(renderer.getLineHeight(SMALL_FONT_ID), renderer.getLineHeight(NOTOSANSARABIC_8_FONT_ID));
  return GRID_TITLE_TOP_GAP + lineHeight * GRID_TITLE_LINES;
}

OpdsBookBrowserActivity::GridLayout OpdsBookBrowserActivity::computeGridLayout() const {
  GridLayout layout;

  int firstBookIndex = -1;
  int lastBookIndex = -1;
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].type == OpdsEntryType::BOOK) {
      if (firstBookIndex < 0) firstBookIndex = static_cast<int>(i);
      lastBookIndex = static_cast<int>(i);
    }
  }

  if (firstBookIndex < 0) {
    return layout;  // isGridPage stays false: pure navigation/category page
  }

  layout.isGridPage = true;
  layout.topNavCount = firstBookIndex;
  layout.bookStart = firstBookIndex;
  layout.bookCount = lastBookIndex - firstBookIndex + 1;
  layout.bottomNavStart = lastBookIndex + 1;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // Column count first, from a target minimum cell width -- then the cover
  // size is derived to fill the resulting columns edge-to-edge, so covers
  // are always as big as the actual screen allows rather than a fixed size
  // that may be much smaller than the cell it sits in.
  layout.columns = std::max(1, (pageWidth - GRID_GUTTER) / (GRID_MIN_CELL_WIDTH + GRID_GUTTER));
  layout.coverWidth = (pageWidth - GRID_GUTTER * (layout.columns + 1)) / layout.columns;
  layout.coverHeight = static_cast<int>(layout.coverWidth * GRID_COVER_ASPECT);

  const int contentHeight = pageHeight - GRID_CONTENT_TOP - GRID_BOTTOM_MARGIN;
  const int rowHeight = layout.coverHeight + getGridTitleHeight() + GRID_GUTTER;
  const int rows = std::max(1, contentHeight / rowHeight);
  layout.itemsPerPage = layout.columns * rows;

  return layout;
}

void OpdsBookBrowserActivity::loadGridPageCovers(const GridLayout& layout, const int pageStart) {
  const int pageEnd = std::min(pageStart + layout.itemsPerPage, layout.bookCount);

  bool needsFetch = false;
  for (int i = pageStart; i < pageEnd; i++) {
    const auto& entry = entries[layout.bookStart + i];
    if (entry.coverUrl.empty()) continue;
    if (!Storage.exists(getOpdsCoverCachePath(entry.id, layout.coverWidth, layout.coverHeight).c_str())) {
      needsFetch = true;
      break;
    }
  }
  if (!needsFetch) {
    loadedGridPageStart = pageStart;
    return;
  }

  bool showingLoading = false;
  Rect popupRect;
  const int totalToProcess = pageEnd - pageStart;
  int processedCount = 0;
  // Cover downloads have no user-visible error state (a failed cover just falls back to
  // the placeholder icon), so without this there's no way to diagnose a real device
  // report after the fact -- e.g. confirming whether cover/download links are still
  // https:// even after the main feed was switched to http:// (see FouladEbooksConfig.h).
  // OpdsCoverCache already LOG_ERRs the failing coverUrl into the ring buffer; this just
  // makes sure that ring buffer actually reaches an SD file the user can retrieve, the
  // same way a feed fetch failure already does via saveOpdsDiagnosticLog().
  int coverFailures = 0;

  for (int i = pageStart; i < pageEnd; i++) {
    const auto& entry = entries[layout.bookStart + i];
    if (!entry.coverUrl.empty() &&
        !Storage.exists(getOpdsCoverCachePath(entry.id, layout.coverWidth, layout.coverHeight).c_str())) {
      if (!showingLoading) {
        showingLoading = true;
        popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      }
      GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
      if (!ensureOpdsCoverCached(entry, server.username, server.password, layout.coverWidth, layout.coverHeight)) {
        coverFailures++;
      }
    }
    processedCount++;
  }

  if (coverFailures > 0) {
    saveOpdsDiagnosticLog("Cover download failed for " + std::to_string(coverFailures) + " of " +
                          std::to_string(totalToProcess) + " book(s) on this page");
  }

  loadedGridPageStart = pageStart;
  if (showingLoading) {
    requestUpdate();
  }
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  loadedGridPageStart = NO_GRID_PAGE_LOADED;
  // Consume the auto-advance landing hint up front so a failed fetch can't
  // leave it armed for a later, unrelated navigation.
  const PendingGridSelect pendingSelect = pendingGridSelect;
  pendingGridSelect = PendingGridSelect::None;

  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());

  // Larger catalogs (e.g. a category with hundreds of books, even paginated) take longer
  // to transfer over WiFi than a handful of KB, which makes a one-off mid-transfer hiccup
  // more likely purely by taking more time -- not necessarily a real fault. Retry the whole
  // fetch a couple of times before surfacing an error, rather than failing on the first blip.
  // Each attempt gets its own fresh OpdsParser -- reusing one across a failed attempt would
  // leave it permanently in its post-failure error state (clear() doesn't reset that).
  constexpr int MAX_FETCH_ATTEMPTS = 3;
  std::unique_ptr<OpdsParser> parser;
  bool fetchOk = false;
  bool authFailed = false;
  for (int attempt = 1; attempt <= MAX_FETCH_ATTEMPTS && !fetchOk; attempt++) {
    if (attempt > 1) {
      LOG_DBG("OPDS", "Retrying feed fetch (attempt %d/%d): %s", attempt, MAX_FETCH_ATTEMPTS, url.c_str());
      delay(500);
    }
    parser = std::make_unique<OpdsParser>();
    OpdsParserStream stream{*parser};
    fetchOk = HttpDownloader::fetchUrl(url, stream, server.username, server.password);
    if (!fetchOk) {
      const auto failure = HttpDownloader::getLastFailure();
      if (failure.stage == HttpDownloader::FailStage::STATUS && failure.detail == 401) {
        // Credential rejected. Retrying cannot help and the spec is explicit
        // about not looping on this (EINK_QR_LOGIN_TASKS.md, "Handling a
        // revoked token"), so stop immediately and handle it below.
        authFailed = true;
        break;
      }
    }
  }

  if (authFailed) {
    // The account was signed out from the phone app or the web (or the device
    // token was revoked). Drop the stored credential so the next entry lands on
    // the sign-in screen rather than failing the same way forever -- for Foulad
    // eBooks that is now the QR screen. Only the matching entry is removed; any
    // other configured OPDS server is left alone.
    saveOpdsDiagnosticLog("Auth rejected (401), clearing stored credential: " + url);
    LOG_ERR("OPDS", "401 from %s -- signing this device out", server.url.c_str());
    const auto& servers = OPDS_STORE.getServers();
    for (size_t i = 0; i < servers.size(); ++i) {
      if (servers[i].url == server.url && servers[i].username == server.username) {
        OPDS_STORE.removeServer(i);
        break;
      }
    }
    state = BrowserState::ERROR;
    errorMessage = tr(STR_OPDS_SIGNED_OUT);
    requestUpdate();
    return;
  }

  if (!fetchOk) {
    saveOpdsDiagnosticLog("Fetch failed after " + std::to_string(MAX_FETCH_ATTEMPTS) + " attempts: " + url);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_FETCH_FEED_FAILED);
    requestUpdate();
    return;
  }

  if (!*parser) {
    saveOpdsDiagnosticLog("Parse failed: " + url);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  searchTemplate = parser->getSearchTemplate();
  feedNextUrl = parser->getNextPageUrl();
  feedPrevUrl = parser->getPrevPageUrl();
  entries = std::move(*parser).getEntries();

  if (!feedPrevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", feedPrevUrl, ""});
  }
  if (!feedNextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", feedNextUrl, ""});
  }

  selectorIndex = 0;
  if (pendingSelect != PendingGridSelect::None) {
    // Auto-advance landing spot: continue the reading direction seamlessly
    // instead of parking the selection on the "> Prev Page" strip row.
    int firstBook = -1;
    int lastBook = -1;
    for (size_t i = 0; i < entries.size(); i++) {
      if (entries[i].type == OpdsEntryType::BOOK) {
        if (firstBook < 0) firstBook = static_cast<int>(i);
        lastBook = static_cast<int>(i);
      }
    }
    if (firstBook >= 0) {
      selectorIndex = (pendingSelect == PendingGridSelect::FirstBook) ? firstBook : lastBook;
    }
  }

  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);

  {
    int bookCount = 0;
    for (const auto& e : entries) {
      if (e.type == OpdsEntryType::BOOK) bookCount++;
    }
    browseLog("FETCH " + url + " entries=" + std::to_string(entries.size()) + " books=" + std::to_string(bookCount) +
              " next=" + (feedNextUrl.empty() ? "-" : feedNextUrl) +
              " prev=" + (feedPrevUrl.empty() ? "-" : feedPrevUrl));
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    entries.clear();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  // Pick the file extension from the acquisition link's actual format instead of always
  // assuming EPUB -- matters now that XTC-only books (Arabic/PDF-sourced, per foulad-ebooks'
  // one-format-per-book rule) are recognized as downloadable BOOK entries too. foulad-ebooks
  // derives this MIME type from the stored file's own extension, so mirror it back exactly
  // (.xtch vs .xtc) rather than assuming one -- both are recognized identically for local
  // file-type detection (FsHelpers::hasXtcExtension), so either is safe to save as.
  std::string extension = ".epub";
  if (book.acquisitionType == "application/x-xtch") {
    extension = ".xtch";
  } else if (book.acquisitionType == "application/x-xtc") {
    extension = ".xtc";
  }
  const std::string filename =
      "/" + StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title) + extension;

  if (Storage.exists(filename.c_str())) {
    // Already downloaded -- open it directly rather than downloading again.
    if (server.url == FOULAD_EBOOKS_URL) {
      const std::string fouladBookId = extractFouladBookId(book.id);
      if (!fouladBookId.empty()) {
        RECENT_BOOKS.addBook(filename, book.title, book.author, "", fouladBookId);
      }
    }
    pendingReaderPath = filename;
    activityManager.goToReader(filename);
    return;
  }

  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  lastDownloadPercentage = -1;
  lastDownloadBytesShown = 0;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  // foulad-ebooks' download endpoints respond with Transfer-Encoding: chunked (no
  // Content-Length), so `total` is 0 for every real download today -- redraw by a byte-count
  // step in that case instead of skipping the throttle entirely (which forced an e-ink
  // refresh on every single 2KB HTTP chunk, making a multi-MB book look stuck for minutes).
  constexpr size_t UNKNOWN_TOTAL_REDRAW_STEP = 32 * 1024;

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        if (total > 0) {
          const int percentage = static_cast<int>(downloaded * 100 / total);
          if (percentage == lastDownloadPercentage) return;
          lastDownloadPercentage = percentage;
        } else {
          if (downloaded - lastDownloadBytesShown < UNKNOWN_TOTAL_REDRAW_STEP) return;
          lastDownloadBytesShown = downloaded;
        }
        requestUpdate(true);
      },
      nullptr, server.username, server.password);

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    // Save the catalog cover next to the book (after clearBookCache, which wipes the
    // cache dir) so it shows a thumbnail on Home/My Books even when the EPUB has no
    // embedded cover art -- common for Foulad eBooks. Best-effort; a missing cover
    // just falls back to the placeholder as before. Only EPUBs have the external
    // cover fallback in generateThumbBmp(); XTC covers come from the file itself.
    if (!book.coverUrl.empty() && FsHelpers::hasEpubExtension(filename)) {
      Epub epub(filename, "/.crosspoint");
      Storage.mkdir(epub.getCachePath().c_str());
      // Mirror OpdsCoverCache's own format dispatch: the catalog serves either JPEG
      // or PNG covers, and saving/decoding under the wrong assumed format silently
      // fails the later thumbnail conversion, leaving the cover blank.
      const bool isPng = book.coverType.find("png") != std::string::npos;
      if (HttpDownloader::downloadToFile(book.coverUrl, epub.getExternalCoverPath(isPng), nullptr, nullptr,
                                         server.username, server.password) != HttpDownloader::OK) {
        LOG_DBG("OPDS", "External cover download failed (non-fatal): %s", book.coverUrl.c_str());
      }
    }
    if (server.url == FOULAD_EBOOKS_URL) {
      const std::string fouladBookId = extractFouladBookId(book.id);
      if (!fouladBookId.empty()) {
        RECENT_BOOKS.addBook(filename, book.title, book.author, "", fouladBookId);
      }
    }
    pendingReaderPath = filename;
    activityManager.goToReader(filename);
    return;
  }
  state = BrowserState::ERROR;
  errorMessage = tr(STR_DOWNLOAD_FAILED);
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);  // <-- add this
  currentPath = url;                         // <-- add this

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::reportDeviceTrackingOnConnect() {
  if (server.url != FOULAD_EBOOKS_URL) return;
  FouladDeviceTracking::registerDevice(server.username, server.password);
  // Reading itself never keeps WiFi connected (see onExit() below), so this
  // is the one reliable moment to sync any progress accumulated since the
  // last time the device was online.
  FouladDeviceTracking::flushPendingReadingStats(server.username, server.password);
  // Same reasoning: uploading the debug log here (rather than at the moment
  // a book is opened, when WiFi is already gone) is what actually delivers
  // it. No-op unless Settings -> Apps -> Debug is on.
  FouladDeviceTracking::uploadDebugLog(server.username, server.password);
  // Device/reading-stats snapshot for the "My Devices" web page's Device
  // Stats / Reading Stats tabs -- same reliable-moment reasoning.
  FouladDeviceTracking::reportDeviceStats(server.username, server.password);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    reportDeviceTrackingOnConnect();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    reportDeviceTrackingOnConnect();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
