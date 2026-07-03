#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "fontIds.h"
#include "util/GridNav.h"

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;
}  // namespace

void RecentBooksActivity::loadRecentBooks() { recentBooks = RECENT_BOOKS.getBooks(); }

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

  geometry.columns = std::max(1, pageWidth / GRID_CELL_WIDTH);
  const int rows = std::max(1, contentHeight / GRID_CELL_HEIGHT);
  geometry.itemsPerPage = geometry.columns * rows;
  return geometry;
}

void RecentBooksActivity::loadGridPageCovers(const int pageStart) {
  const GridGeometry geometry = computeGridGeometry();
  const int pageEnd = std::min(pageStart + geometry.itemsPerPage, static_cast<int>(recentBooks.size()));

  bool needsGeneration = false;
  for (int i = pageStart; i < pageEnd; i++) {
    const RecentBook& book = recentBooks[i];
    if (book.coverBmpPath.empty()) continue;
    if (!Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, GRID_COVER_HEIGHT).c_str())) {
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
      const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, GRID_COVER_HEIGHT);
      if (!Storage.exists(coverPath.c_str())) {
        bool success = false;
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          if (epub.load(false, true)) {
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
            success = epub.generateThumbBmp(GRID_COVER_HEIGHT);
          }
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
            success = xtc.generateThumbBmp(GRID_COVER_HEIGHT);
          }
        }
        if (!success) {
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
          book.coverBmpPath = "";
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  int gridPageStart = 0;
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    const GridGeometry geometry = computeGridGeometry();
    gridPageStart = (static_cast<int>(selectorIndex) / geometry.itemsPerPage) * geometry.itemsPerPage;
    const int pageCount = std::min(geometry.itemsPerPage, static_cast<int>(recentBooks.size()) - gridPageStart);
    const int totalGridWidth = geometry.columns * (GRID_COVER_WIDTH + 10) - 10;
    const int gridStartX = std::max(0, (static_cast<int>(pageWidth) - totalGridWidth) / 2);

    for (int i = 0; i < pageCount; i++) {
      const int bookIdx = gridPageStart + i;
      const auto& book = recentBooks[bookIdx];
      const int col = i % geometry.columns;
      const int row = i / geometry.columns;
      const int cellX = gridStartX + col * (GRID_COVER_WIDTH + 10);
      const int cellY = contentTop + row * (GRID_COVER_HEIGHT + 40);

      bool drawn = false;
      if (!book.coverBmpPath.empty()) {
        const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, GRID_COVER_HEIGHT);
        if (Storage.exists(coverPath.c_str())) {
          HalFile file;
          if (Storage.openFileForRead("RBA", coverPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              renderer.drawBitmap(bitmap, cellX, cellY, GRID_COVER_WIDTH, GRID_COVER_HEIGHT);
              drawn = true;
            }
          }
        }
      }
      renderer.drawRect(cellX, cellY, GRID_COVER_WIDTH, GRID_COVER_HEIGHT);
      if (!drawn) {
        renderer.drawIcon(BookIcon, cellX + (GRID_COVER_WIDTH - 32) / 2, cellY + (GRID_COVER_HEIGHT - 32) / 2, 32);
      }
      if (bookIdx == static_cast<int>(selectorIndex)) {
        renderer.drawRect(cellX - 3, cellY - 3, GRID_COVER_WIDTH + 6, GRID_COVER_HEIGHT + 6, true);
      }

      auto title = renderer.truncatedText(SMALL_FONT_ID, book.title.c_str(), GRID_COVER_WIDTH);
      renderer.drawText(SMALL_FONT_ID, cellX, cellY + GRID_COVER_HEIGHT + 4, title.c_str());
    }
  }

  renderer.displayBuffer();

  if (!recentBooks.empty() && loadedGridPageStart != gridPageStart) {
    loadGridPageCovers(gridPageStart);
  }
}
