#include "OpdsBookBrowserActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "OpdsCoverCache.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/GridNav.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;

int moveHorizontalInGrid(const int currentIndex, const int totalItems, const bool moveRight) {
  return GridNav::moveHorizontal(currentIndex, totalItems, moveRight);
}

int moveVerticalInGrid(const int currentIndex, const int totalItems, const int columns, const int itemsPerPage,
                       const bool moveDown) {
  return GridNav::moveVertical(currentIndex, totalItems, columns, itemsPerPage, moveDown);
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
    silentRestart();
  }
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
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
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
    } else if (onBook) {
      // Inside the cover grid: Left/Right cycle through books; Up/Down move by
      // row, escaping to the nav strip above/below at the grid's true edges.
      auto moveUp = [this, layout] {
        if (selectorIndex == layout.bookStart && layout.topNavCount > 0) {
          selectorIndex = 0;
        } else {
          selectorIndex = layout.bookStart + moveVerticalInGrid(selectorIndex - layout.bookStart, layout.bookCount,
                                                                layout.columns, layout.itemsPerPage, false);
        }
        requestUpdate();
      };
      auto moveDown = [this, layout] {
        if (selectorIndex == layout.bookStart + layout.bookCount - 1 &&
            layout.bottomNavStart < static_cast<int>(entries.size())) {
          selectorIndex = layout.bottomNavStart;
        } else {
          selectorIndex = layout.bookStart + moveVerticalInGrid(selectorIndex - layout.bookStart, layout.bookCount,
                                                                layout.columns, layout.itemsPerPage, true);
        }
        requestUpdate();
      };
      auto moveLeft = [this, layout] {
        selectorIndex =
            layout.bookStart + moveHorizontalInGrid(selectorIndex - layout.bookStart, layout.bookCount, false);
        requestUpdate();
      };
      auto moveRight = [this, layout] {
        selectorIndex =
            layout.bookStart + moveHorizontalInGrid(selectorIndex - layout.bookStart, layout.bookCount, true);
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
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  const GridLayout layout = computeGridLayout();
  const bool onBook = !entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK;

  const char* confirmLabel = onBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const char* leftLabel;
  const char* rightLabel;
  if (layout.isGridPage && onBook) {
    leftLabel = tr(STR_DIR_LEFT);
    rightLabel = tr(STR_DIR_RIGHT);
  } else {
    leftLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
    rightLabel = tr(STR_DIR_DOWN);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, leftLabel, rightLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  int gridPageStart = 0;
  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else if (!layout.isGridPage) {
    // Plain text list (category/navigation-only pages) — unchanged.
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

    for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
      const auto& entry = entries[i];
      std::string displayText = (entry.type == OpdsEntryType::NAVIGATION) ? "> " + entry.title : entry.title;
      if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) displayText += " - " + entry.author;
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  } else {
    // Nav strip above the grid (e.g. a "Prev Page" entry).
    int y = GRID_CONTENT_TOP;
    for (int i = 0; i < layout.topNavCount; i++) {
      const auto& entry = entries[i];
      auto item = renderer.truncatedText(UI_10_FONT_ID, ("> " + entry.title).c_str(), pageWidth - 40);
      if (i == selectorIndex) renderer.fillRect(0, y - 2, pageWidth - 1, 30);
      renderer.drawText(UI_10_FONT_ID, 20, y, item.c_str(), i != selectorIndex);
      y += 30;
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
      const int cellY = gridTop + row * (layout.coverHeight + GRID_TITLE_ROW_HEIGHT + GRID_GUTTER);

      bool drawn = false;
      if (!entry.coverUrl.empty()) {
        const std::string coverPath = getOpdsCoverCachePath(entry.id, layout.coverWidth, layout.coverHeight);
        if (Storage.exists(coverPath.c_str())) {
          HalFile file;
          if (Storage.openFileForRead("OPDS", coverPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              renderer.drawBitmap(bitmap, cellX, cellY, layout.coverWidth, layout.coverHeight);
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
        renderer.drawRect(cellX - 3, cellY - 3, layout.coverWidth + 6, layout.coverHeight + 6, true);
      }

      auto title = renderer.truncatedText(SMALL_FONT_ID, entry.title.c_str(), layout.coverWidth);
      renderer.drawTextInWidth(SMALL_FONT_ID, cellX, cellY + layout.coverHeight + 4, layout.coverWidth, title.c_str());
    }

    // Nav strip below the grid (e.g. a "Next Page" entry).
    const int bottomCount = static_cast<int>(entries.size()) - layout.bottomNavStart;
    if (bottomCount > 0) {
      int by = pageHeight - GRID_BOTTOM_MARGIN - bottomCount * 30;
      for (int i = layout.bottomNavStart; i < static_cast<int>(entries.size()); i++) {
        const auto& entry = entries[i];
        auto item = renderer.truncatedText(UI_10_FONT_ID, ("> " + entry.title).c_str(), pageWidth - 40);
        if (i == selectorIndex) renderer.fillRect(0, by - 2, pageWidth - 1, 30);
        renderer.drawText(UI_10_FONT_ID, 20, by, item.c_str(), i != selectorIndex);
        by += 30;
      }
    }
  }
  renderer.displayBuffer();

  if (layout.isGridPage && !entries.empty() && loadedGridPageStart != gridPageStart) {
    loadGridPageCovers(layout, gridPageStart);
  }
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
  const int rowHeight = layout.coverHeight + GRID_TITLE_ROW_HEIGHT + GRID_GUTTER;
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

  for (int i = pageStart; i < pageEnd; i++) {
    const auto& entry = entries[layout.bookStart + i];
    if (!entry.coverUrl.empty() &&
        !Storage.exists(getOpdsCoverCachePath(entry.id, layout.coverWidth, layout.coverHeight).c_str())) {
      if (!showingLoading) {
        showingLoading = true;
        popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      }
      GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
      ensureOpdsCoverCached(entry, server.username, server.password, layout.coverWidth, layout.coverHeight);
    }
    processedCount++;
  }

  loadedGridPageStart = pageStart;
  if (showingLoading) {
    requestUpdate();
  }
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  loadedGridPageStart = NO_GRID_PAGE_LOADED;

  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  entries = std::move(parser).getEntries();

  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }

  selectorIndex = 0;
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
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
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  std::string filename =
      "/" + StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title) + ".epub";
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      },
      nullptr, server.username, server.password);

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    state = BrowserState::BROWSING;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
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

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
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
