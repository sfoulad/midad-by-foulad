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
#include <memory>

#include "CrossPointState.h"
#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "FouladOpdsHooks.h"
#include "MappedInputManager.h"
#include "OpdsCoverCache.h"
#include "OpdsServerStore.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TileCover.h"
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

// What earns a cell in the cover grid. Books always do. A navigation entry does when
// it carries cover art -- a collection is a shelf with a picture on it, and rendering
// it as a "> Summer reading" text row beside a grid of book covers would make the one
// thing the user is meant to press look like the page furniture.
//
// Navigation entries WITHOUT art stay text rows on purpose: that is the Prev/Next page
// strip and any plain category link, none of which are things to show a cell for.
bool isGridItem(const OpdsEntry& entry) {
  return entry.type == OpdsEntryType::BOOK || (entry.type == OpdsEntryType::NAVIGATION && !entry.coverUrl.empty());
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
//
// force=true bypasses the debugLoggingEnabled gate (see RollingSdLog::append) --
// reserved for the heap-guard trip points below (hasHeapForCoverWork/
// hasHeapForNavigation), which are the exact defense that heads off the
// throwing-`new`-under-`-fno-exceptions` crash class documented at
// hasHeapForNavigation()'s definition. A near-miss save is as diagnostically
// valuable as the crash it prevented, and the sessions that need it most are
// the ones where nobody thought to turn debug logging on ahead of time. Routine
// fetch/parse/auth failures below stay opt-in.
void saveOpdsDiagnosticLog(const std::string& context, bool force = false) {
  if (!force && !DebugLogging::enabled()) return;
  std::string info = "[OPDS-ERROR] CrossPoint version: " CROSSPOINT_VERSION;
  info += " | Context: " + context;
  info += "\nLast logs:\n" + getLastLogs();

  RollingSdLog::append(DebugLog::PATH, info, DebugLog::MAX_LINES, force);
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
      // HomeActivity::onFouladEbooksOpen(), FouladQrLoginActivity) --
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
  if (state == BrowserState::UPDATE_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Reboots into the OTA flow rather than downloading here: this session has an
      // OPDS feed and its cover cache resident, and the firmware download needs more
      // contiguous heap than that leaves. Does not return.
      silentRestartToOtaCheck();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = BrowserState::BROWSING;  // declined; carry on browsing
      requestUpdate();
      return;
    }
    return;  // nothing else acts while the offer is up
  }

  maybeOfferFirmwareUpdate();
  // The offer can go up mid-frame; hand this frame to it rather than letting a
  // keypress fall through to the catalog underneath.
  if (state == BrowserState::UPDATE_PROMPT) return;

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
    // Grid-vs-strip dispatch has to key off "is this a cell in the cover grid", not "is
    // this a book" -- a collection tile is a NAVIGATION entry with cover art (isGridItem()
    // says so too; see computeGridLayout()), and needs the same Left/Right/Up/Down grid
    // movement a book cell gets. Checking type == BOOK here left every collection landing
    // in the nav-strip branch below even though computeGridLayout() had already counted it
    // as part of the grid, so Left/Right walked off the end of an empty "strip" and the
    // selector vanished.
    const bool onGridItem = !entries.empty() && isGridItem(entries[selectorIndex]);

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        if (entry.type == OpdsEntryType::SEARCH) {
          launchSearch();
        } else if (entry.type == OpdsEntryType::BOOK) {
          downloadBook(entry);
        } else {
          navigateToEntry(entry);
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
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
    } else if (onGridItem) {
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
          // The row directly above the grid, not the top of the strip -- with a search
          // row now sitting above Prev Page, jumping to 0 would skip past it.
          selectorIndex = layout.topNavCount - 1;
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
  if (state == BrowserState::BROWSING && browsingFramesRendered < 2) browsingFramesRendered++;

  if (state == BrowserState::UPDATE_PROMPT) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int top = (renderer.getScreenHeight() - lineHeight * 3) / 2;
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   tr(STR_UPDATE));
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(
        UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing,
        (std::string(tr(STR_NEW_VERSION)) + FouladDeviceTracking::pendingFirmwareUpdate()).c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Selector fast path. Moving the selection one cell changes two frames' worth of pixels,
  // but a full repaint re-opens and re-decodes EVERY cover on the page from the SD card
  // (see drawGridCell) -- on a page of covers that re-read is essentially the entire cost
  // of a keypress, and it is what makes the selector feel slow. When the framebuffer
  // already holds this exact grid page and only the selection moved, redraw just the two
  // cells whose frame changed and leave the rest of the frame standing.
  if (canFastRepaintSelector() && fastRepaintStreak < FAST_REPAINT_CAP) {
    const GridLayout layout = computeGridLayout();
    drawGridSelectionRing(layout, fbSelectorIndex - layout.bookStart - fbGridPageStart, /*black=*/false);
    drawGridSelectionRing(layout, selectorIndex - layout.bookStart - fbGridPageStart, /*black=*/true);
    renderer.displayBuffer();
    fbSelectorIndex = selectorIndex;
    fastRepaintStreak++;
    return;
  }
  fastRepaintStreak = 0;
  // Anything that is not a full grid render leaves the framebuffer in a state the fast
  // path must not touch up. Invalidated here and re-armed only at the end of a grid
  // render, so every other path (list, error, loading, popups) is covered by default.
  fbGridPageStart = FB_GRID_NONE;

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
  // Kept strictly type == BOOK: this only decides the confirm-button label (Download vs
  // Open), which is correct for a book regardless of grid placement -- a collection tile
  // is still something you navigate INTO, not download, even though it now shares the
  // grid layout with books. See onGridItem below for the "is this cell in the grid"
  // question that governs layout math instead.
  const bool onBook = !entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK;
  const bool onGridItem = !entries.empty() && isGridItem(entries[selectorIndex]);

  // "Open" is wrong on the search row -- nothing opens, a keyboard appears. The hint
  // names what the button does, the same rule the font browser's tab row follows.
  const bool onSearchRow = !entries.empty() && entries[selectorIndex].type == OpdsEntryType::SEARCH;
  const char* confirmLabel = onSearchRow ? tr(STR_SEARCH) : (onBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN));
  const char* leftLabel;
  const char* rightLabel;
  // Grid-page cells (books and collection tiles alike) use genuinely horizontal
  // Left/Right (mirror under RTL, like any other horizontal grid); the plain-list and
  // nav-strip cases below use a vertical Up/Down pair instead (down is still down
  // regardless of script direction -- see mapLabels' rtlSwap parameter).
  const bool rtlSwapLabels = layout.isGridPage && onGridItem;
  if (rtlSwapLabels) {
    leftLabel = tr(STR_DIR_LEFT);
    rightLabel = tr(STR_DIR_RIGHT);
  } else {
    leftLabel = tr(STR_DIR_UP);
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
      std::string displayText = (entry.type != OpdsEntryType::BOOK) ? "> " + entry.title : entry.title;
      if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) displayText += " - " + entry.author;
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawTextInWidth(UI_10_FONT_ID, 20, GRID_CONTENT_TOP + (i % pageItems) * rowHeight, pageWidth - 40,
                               item.c_str(), i != static_cast<size_t>(selectorIndex));
    }
  } else {
    // Nav strip above the grid (e.g. a "Prev Page" entry). Same tight row height as the
    // plain list above.
    const int rowHeight = getListRowHeight();
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
    const int gridTop = gridTopFor(layout);

    const bool selectionInGrid = onGridItem;
    const int localSelector = selectionInGrid ? selectorIndex - layout.bookStart : 0;
    gridPageStart = (localSelector / layout.itemsPerPage) * layout.itemsPerPage;
    const int pageCount = std::min(layout.itemsPerPage, layout.bookCount - gridPageStart);

    for (int i = 0; i < pageCount; i++) {
      // Screen was just cleared, so no per-cell erase is needed here -- only the
      // selector fast path (which draws over a live frame) asks for one.
      drawGridCell(layout, gridPageStart, i, /*eraseFirst=*/false);
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

  // Arm the selector fast path: the framebuffer now holds this grid page in full, so the
  // next pure selection move can touch up two cells instead of repainting all of them.
  // Only for a grid page -- the list/error paths left it invalidated above.
  if (layout.isGridPage && !entries.empty()) {
    fbGridPageStart = gridPageStart;
    fbSelectorIndex = selectorIndex;
  }

  if (layout.isGridPage && !entries.empty() && loadedGridPageStart != gridPageStart) {
    loadGridPageCovers(layout, gridPageStart);
  }
}

// Every condition that has to hold before render() may touch up the standing frame instead
// of redrawing it. Deliberately strict: the cost of being wrong is a visibly corrupt screen,
// while the cost of a false negative is one ordinary full repaint.
bool OpdsBookBrowserActivity::canFastRepaintSelector() const {
  if (state != BrowserState::BROWSING) return false;
  // No frame we put there, or nothing actually moved.
  if (fbGridPageStart == FB_GRID_NONE || fbSelectorIndex == selectorIndex) return false;
  if (entries.empty()) return false;
  if (selectorIndex < 0 || selectorIndex >= static_cast<int>(entries.size())) return false;
  if (fbSelectorIndex < 0 || fbSelectorIndex >= static_cast<int>(entries.size())) return false;
  // Covers for this page must already be on screen. While they are still downloading,
  // loadGridPageCovers() is going to request a full render anyway, and touching up a page
  // that is about to gain artwork would show a frame around a placeholder.
  if (loadedGridPageStart != fbGridPageStart) return false;

  const GridLayout layout = computeGridLayout();
  if (!layout.isGridPage || layout.itemsPerPage <= 0) return false;
  // Both ends of the move must be cells (not nav-strip rows) on the page already drawn --
  // a move onto a strip row, or onto another grid page, changes more than two cells.
  if (!isGridItem(entries[selectorIndex]) || !isGridItem(entries[fbSelectorIndex])) return false;
  const int newLocal = selectorIndex - layout.bookStart;
  const int oldLocal = fbSelectorIndex - layout.bookStart;
  if (newLocal < 0 || oldLocal < 0) return false;
  return (newLocal / layout.itemsPerPage) * layout.itemsPerPage == fbGridPageStart &&
         (oldLocal / layout.itemsPerPage) * layout.itemsPerPage == fbGridPageStart;
}

void OpdsBookBrowserActivity::drawGridSelectionRing(const GridLayout& layout, const int slot, const bool black) const {
  const int col = slot % layout.columns;
  const int row = slot / layout.columns;
  const int titleHeight = getGridTitleHeight();
  const int cellX = gridStartXFor(layout) + col * (layout.coverWidth + GRID_GUTTER);
  const int cellY = gridTopFor(layout) + row * (layout.coverHeight + titleHeight + GRID_GUTTER);
  renderer.drawRect(cellX - 4, cellY - 4, layout.coverWidth + 8, layout.coverHeight + 8, 4, black);
}

int OpdsBookBrowserActivity::gridStartXFor(const GridLayout& layout) const {
  const int totalGridWidth = layout.columns * (layout.coverWidth + GRID_GUTTER) - GRID_GUTTER;
  return std::max(0, (renderer.getScreenWidth() - totalGridWidth) / 2);
}

int OpdsBookBrowserActivity::gridTopFor(const GridLayout& layout) const {
  // Mirrors where render()'s nav-strip loop leaves its cursor: one row height per strip
  // entry from the content top, plus a gap when the strip is non-empty.
  return GRID_CONTENT_TOP + layout.topNavCount * getListRowHeight() + (layout.topNavCount > 0 ? 10 : 0);
}

// One grid cell: cover art (or its fallback), the selection frame, and the wrapped title.
// Extracted so the full-page render and the selector fast path draw a cell through exactly
// the same code -- two copies of this would drift, and a cell that renders differently
// depending on which path touched it is the kind of bug that only shows up on device.
void OpdsBookBrowserActivity::drawGridCell(const GridLayout& layout, const int pageStart, const int slot,
                                           const bool eraseFirst) const {
  const int bookIdx = layout.bookStart + pageStart + slot;
  if (bookIdx < 0 || bookIdx >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[bookIdx];

  const int titleHeight = getGridTitleHeight();
  const int col = slot % layout.columns;
  const int row = slot / layout.columns;
  const int cellX = gridStartXFor(layout) + col * (layout.coverWidth + GRID_GUTTER);
  const int cellY = gridTopFor(layout) + row * (layout.coverHeight + titleHeight + GRID_GUTTER);

  if (eraseFirst) {
    // Clear the cell's whole footprint, selection frame included, before redrawing over a
    // live frame. Bounded to stay inside the gutter: the frame reaches 4px out of a 12px
    // gap, so this stops 4px short of a neighbour's frame on either axis and 4px short of
    // the next row's.
    renderer.fillRect(cellX - 4, cellY - 4, layout.coverWidth + 8, layout.coverHeight + 8 + titleHeight, false);
  }

  bool drawn = false;
  if (!entry.coverUrl.empty()) {
    const std::string coverPath = getOpdsCoverCachePath(entry.id, layout.coverWidth, layout.coverHeight);
    // No exists() probe before opening: on FAT each path lookup is a linear scan of a
    // directory that accumulates one file per cover ever fetched, and openFileForRead
    // already fails cleanly when the file is missing. Probing first paid for that scan
    // twice per cover per render.
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
  renderer.drawRect(cellX, cellY, layout.coverWidth, layout.coverHeight);
  if (!drawn && (server.url == FOULAD_EBOOKS_NEWS_URL || entry.type == OpdsEntryType::NAVIGATION)) {
    // News entries carry no cover art -- there is no image link in the feed, so
    // nothing failed to download and nothing is going to appear later. The generic
    // book icon reads as a book that did not load; the feed's own name on a
    // designed tile reads as the thing it is, which is what the app tiles already
    // do for the same reason.
    drawTileCover(renderer, cellX, cellY, layout.coverWidth, layout.coverHeight, entry.title.c_str());
  } else if (!drawn) {
    renderer.drawIcon(BookIcon, cellX + (layout.coverWidth - 32) / 2, cellY + (layout.coverHeight - 32) / 2, 32);
  }
  if (bookIdx == selectorIndex) {
    // A 1px outline was hard to spot at a glance on e-ink, especially across a
    // multi-column grid where the eye has to search for it -- a thick (4px) border
    // reads as a deliberate, high-contrast selection frame instead.
    renderer.drawRect(cellX - 4, cellY - 4, layout.coverWidth + 8, layout.coverHeight + 8, 4, true);
  }

  const auto titleLines = renderer.wrappedText(SMALL_FONT_ID, entry.title.c_str(), layout.coverWidth, GRID_TITLE_LINES);
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
    if (isGridItem(entries[i])) {
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

// Cover work is the largest transient allocation a catalogue walk makes: an HTTP
// download plus a JPEG/PNG decode, repeated once per book on every grid page. The
// feed itself is not the pressure -- OpdsParserStream parses it as it arrives and
// only title/author/id text is ever retained (OpdsParser.cpp:227) -- but the covers
// land on a heap that a long walk has already fragmented. Crash 28 walked to page 2
// of 57 with the floor sinking 41,448 -> 37,072 bytes and aborted there.
//
// Below the floor, covers are skipped and the placeholder icon is drawn. A grid with
// grey boxes in it is a bad page; an abort is a lost session and a crash report. The
// same trade is already made for the reading-stats snapshot in FouladDeviceTracking.
static bool hasHeapForCoverWork() {
  constexpr uint32_t COVER_MIN_FREE_HEAP = 32 * 1024;
  constexpr uint32_t COVER_MIN_FREE_BLOCK = 12 * 1024;
  return ESP.getFreeHeap() > COVER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > COVER_MIN_FREE_BLOCK;
}

// Crash report from 1.8.40-rc: confirming a collection tile aborted with a BLANK panic
// reason -- no "abort() was called at PC X" the way a normal abort() gets, because this
// wasn't one. Stack: OpdsBookBrowserActivity::loop() (Confirm) -> navigateToEntry() ->
// UrlUtils::buildUrl() -> operator new. buildUrl() copies its input through plain
// std::string construction, which allocates via the THROWING global operator new -- there
// is no nothrow equivalent for ordinary std::string/std::vector growth the way
// makeUniqueNoThrow covers explicit heap buffers. Under -fno-exceptions, that failed `new`
// calls std::terminate() directly, which is why the report carries no message at all.
// Free heap alone was 55,952 bytes at the time (noteHeap() samples once a loop tick) --
// largest block was only 21,492, so fragmentation, not raw exhaustion, did it. Same
// defense as hasHeapForCoverWork() above: refuse the risky path below this floor rather
// than let a plain string copy take the device down.
static bool hasHeapForNavigation() {
  constexpr uint32_t NAV_MIN_FREE_HEAP = 32 * 1024;
  return ESP.getFreeHeap() > NAV_MIN_FREE_HEAP;
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
      if (!hasHeapForCoverWork()) {
        // Stop for this page rather than skipping one and trying the next: heap this
        // low does not recover within a loop, and each further attempt is another
        // chance to abort. The uncached covers stay uncached and are picked up on a
        // later visit, when the walk that fragmented the heap is behind us.
        saveOpdsDiagnosticLog("Cover fetch stopped early, low heap: free=" + std::to_string(ESP.getFreeHeap()) +
                                  " largest=" + std::to_string(ESP.getMaxAllocHeap()) + " at book " +
                                  std::to_string(i - pageStart + 1) + "/" + std::to_string(totalToProcess),
                              /*force=*/true);
        LOG_ERR("OPDS", "Cover fetch stopped early (free heap %u)", static_cast<unsigned>(ESP.getFreeHeap()));
        break;
      }
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

    if (server.url == FOULAD_EBOOKS_URL) {
      // Removing this device from the Foulad One app revokes its credential, so the
      // next fetch 401s and arrives here. Ending on the message alone left the user
      // to work out for themselves that signing in again meant backing out to Home
      // and re-entering Foulad eBooks -- reported as the device looking broken after
      // a removal that was deliberate. Take them to the QR sign-in instead.
      //
      // The message is painted first, and held long enough to read, so the sign-in
      // prompt doesn't simply appear with no explanation of what happened.
      //
      // Restart rather than a direct transition, for the same reason
      // ActivityManager::goToFouladEbooks() restarts on the way in: the QR flow runs
      // WiFi, TLS and polling, and this session's heap has already been fragmented by
      // a browse. removeServer() above persists immediately, so the reboot cannot
      // bring back the credential just dropped and loop straight back here.
      //
      // Only for Foulad eBooks. A user-added OPDS server has no QR flow to send
      // anyone to, so it keeps the plain message.
      requestUpdateAndWait();
      delay(1500);
      silentRestartToFouladEbooks();  // does not return
      return;
    }

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
  // On the News root the advertised search is the BOOK search (/opds/search), which
  // would put a row inside News that returns books. Wrong answer to the wrong
  // question, so News does not offer it.
  if (server.url == FOULAD_EBOOKS_NEWS_URL) searchTemplate.clear();
  feedNextUrl = parser->getNextPageUrl();
  feedPrevUrl = parser->getPrevPageUrl();
  entries = std::move(*parser).getEntries();

  // Whether the FEED was empty, captured before the synthetic rows go in. The empty
  // state has to be decided on what the server actually sent: a page carrying only a
  // Search row is an empty feed wearing a button, and treating it as populated is how
  // "Add news feeds in the Midad app" stopped appearing at all.
  const bool feedHadEntries = !entries.empty();

  if (!feedPrevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", feedPrevUrl, ""});
  }
  if (!feedNextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", feedNextUrl, ""});
  }
  // Search goes in as a row, above everything, on every feed that advertises it.
  //
  // It was already implemented and already worked -- but it lived on the Left button,
  // and only while the selection happened to be on the top row. Nothing said so. A
  // catalogue that has grown to 679 books behind a 57-page walk is unusable if the one
  // feature that finds a book by name is a binding you have to be told about, so it
  // stops being a binding.
  if (!searchTemplate.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::SEARCH, tr(STR_SEARCH), "", "", ""});
  }

  // Past the search row: it is the first thing you can reach by pressing Up, not the
  // thing you land on every time a page loads.
  selectorIndex = (!entries.empty() && entries[0].type == OpdsEntryType::SEARCH && entries.size() > 1) ? 1 : 0;
  if (pendingSelect != PendingGridSelect::None) {
    // Auto-advance landing spot: continue the reading direction seamlessly
    // instead of parking the selection on the "> Prev Page" strip row.
    int firstBook = -1;
    int lastBook = -1;
    for (size_t i = 0; i < entries.size(); i++) {
      if (isGridItem(entries[i])) {
        if (firstBook < 0) firstBook = static_cast<int>(i);
        lastBook = static_cast<int>(i);
      }
    }
    if (firstBook >= 0) {
      selectorIndex = (pendingSelect == PendingGridSelect::FirstBook) ? firstBook : lastBook;
    }
  }

  state = feedHadEntries ? BrowserState::BROWSING : BrowserState::ERROR;
  if (!feedHadEntries) {
    // An empty feed is the normal, successful answer to "this account has no
    // subscriptions" -- not an error (EINK_NEWS_TASKS.md §2). Say what to do next
    // instead, because the thing to do is on the phone and nothing here can do it.
    errorMessage = (server.url == FOULAD_EBOOKS_NEWS_URL) ? tr(STR_NEWS_EMPTY) : tr(STR_NO_ENTRIES);
  }

  // Crash report: a heap-corruption abort (multi_heap assert_valid_block) landed inside
  // this block's temporary std::string destructors, after an extended low-heap OPDS
  // session -- the same throwing-`new`-under-`-fno-exceptions` hazard hasHeapForNavigation()
  // guards against below, just one call site earlier. This block is pure diagnostics
  // (RollingSdLog), so skipping it under memory pressure costs nothing but a log line.
  if (hasHeapForNavigation()) {
    int bookCount = 0;
    for (const auto& e : entries) {
      if (e.type == OpdsEntryType::BOOK) bookCount++;
    }
    browseLog("FETCH " + url + " entries=" + std::to_string(entries.size()) + " books=" + std::to_string(bookCount) +
              " next=" + (feedNextUrl.empty() ? "-" : feedNextUrl) +
              " prev=" + (feedPrevUrl.empty() ? "-" : feedPrevUrl));
  } else {
    LOG_ERR("OPDS", "Skipped FETCH diagnostic log, low heap: free=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    saveOpdsDiagnosticLog(
        "Skipped FETCH diagnostic log, low heap: free=" + std::to_string(ESP.getFreeHeap()) + " url=" + url,
        /*force=*/true);
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  // See hasHeapForNavigation() -- everything below here allocates ordinary std::strings
  // (navigationHistory.push_back, both buildUrl() calls), which has no OOM-safe fallback
  // the way explicit buffers do. Declining up front beats a silent abort mid-navigation.
  if (!hasHeapForNavigation()) {
    LOG_ERR("OPDS", "Navigation stopped early, low heap: free=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    saveOpdsDiagnosticLog(
        "Navigation stopped early, low heap: free=" + std::to_string(ESP.getFreeHeap()) + " entry=" + entry.id,
        /*force=*/true);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

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
  // Extension/folder/replace policy is Foulad eBooks catalog-specific (XTC-only
  // books, News' own folder and replace-not-accumulate rule) -- see FouladOpdsHooks.h.
  const std::string extension = FouladOpdsHooks::acquisitionExtension(book.acquisitionType);
  const bool isNewsFeed = FouladOpdsHooks::isNewsFeed(server.url);
  if (isNewsFeed) Storage.mkdir(FOULAD_NEWS_DIR);
  const std::string filename =
      (isNewsFeed ? std::string(FOULAD_NEWS_DIR) + "/" : std::string("/")) +
      StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title) + extension;

  if (isNewsFeed) FouladOpdsHooks::removeExistingNewsDownload(filename);

  if (!isNewsFeed && Storage.exists(filename.c_str())) {
    // Already downloaded -- open it directly rather than downloading again.
    FouladOpdsHooks::tagFouladBookId(filename, book, server);
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
    FouladOpdsHooks::tagFouladBookId(filename, book, server);
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

  // Opens on the Arabic panel when the interface is Arabic: this catalog is mostly
  // Arabic books, and making an Arabic reader press Mode twice before typing a title
  // is the same discoverability failure the search row itself just fixed.
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), "", 0, InputType::Text,
                                                          "", /*numericOnly=*/false,
                                                          /*preferArabic=*/I18N.isRtl());
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

// Once per boot, not once per Library entry: declining should stick for the session
// rather than being asked again on every visit. File-scope because the activity is
// destroyed and rebuilt each time.
static bool gFirmwareOfferShownThisBoot = false;

void OpdsBookBrowserActivity::maybeOfferFirmwareUpdate() {
  if (gFirmwareOfferShownThisBoot) return;
  if (state != BrowserState::BROWSING || browsingFramesRendered == 0) return;
  // The catalog goes up first and the offer follows. Not a memory precaution -- this
  // costs nothing -- simply that a popup arriving before the list has painted hides
  // the thing the user opened Library to see.
  if (FouladDeviceTracking::pendingFirmwareUpdate().empty()) return;

  // Note what this does NOT do: contact GitHub. The version was parsed out of the
  // registerDevice() response earlier in this same connect, so raising the offer is
  // a string comparison. The previous attempt ran OtaUpdater::checkForUpdate() here
  // and the TLS handshake aborted the device at ~52KB free (v1.8.17-rc); the ~32KB
  // of mbedTLS record buffers do not fit beside a loaded feed. Nothing in this
  // function may become a network call without moving it off this path entirely.
  gFirmwareOfferShownThisBoot = true;
  state = BrowserState::UPDATE_PROMPT;
  requestUpdate();
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    FouladOpdsHooks::reportDeviceTrackingOnConnect(server);
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
    FouladOpdsHooks::reportDeviceTrackingOnConnect(server);
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
