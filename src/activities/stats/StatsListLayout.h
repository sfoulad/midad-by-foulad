#pragma once

#include <algorithm>

// Pure geometry/hit-testing for StatsActivity's heatmap-button + started-books
// list, shared between render() (drawing) and loop() (touch hit-testing) so a
// tap always lands on what's visually drawn -- same idiom as AppsGridLayout.h
// and ReadingHeatmapLayout.h. No GfxRenderer/MappedInputManager dependency, so
// it's host-testable.
namespace StatsListLayout {

constexpr int BOOK_ROW_HEIGHT = 78;
constexpr int BOOK_ROW_GAP = 8;
constexpr int BOOKS_PER_PAGE = 3;

// selectedIndex follows StatsActivity's own convention: 0 = heatmap button,
// 1..N = book rows (1-based). Returns the 0-based index of the first book on
// the page containing the current selection; 0 for an empty list.
inline int pageStartForSelection(int selectedIndex, int bookCount) {
  if (bookCount <= 0) return 0;
  const int bookIndex = std::max(0, selectedIndex - 1);
  const int clamped = std::min(bookIndex, bookCount - 1);
  return (clamped / BOOKS_PER_PAGE) * BOOKS_PER_PAGE;
}

struct RowRect {
  int x = 0, y = 0, width = 0, height = 0;
};

// rowIndexOnPage: 0-based position within the visible page (index - pageStart).
inline RowRect bookRowRect(int rowX, int rowWidth, int contentTop, int rowIndexOnPage) {
  return RowRect{rowX, contentTop + rowIndexOnPage * (BOOK_ROW_HEIGHT + BOOK_ROW_GAP), rowWidth, BOOK_ROW_HEIGHT};
}

enum class HitKind { None, Heatmap, BookRow };

struct Hit {
  HitKind kind = HitKind::None;
  int bookIndex = -1;  // 0-based; valid only when kind == BookRow
};

// heatmapX/Y/W/H + contentTop mirror StatsActivity::Layout (see
// computeLayout()); bookRowX/Width are the shared row band (same x/width as
// the heatmap button). bookCount/selectedIndex determine which page of rows
// is currently visible.
inline Hit hitTest(int x, int y, int heatmapX, int heatmapY, int heatmapW, int heatmapH, int contentTop,
                    int bookRowX, int bookRowWidth, int bookCount, int selectedIndex) {
  if (x >= heatmapX && x < heatmapX + heatmapW && y >= heatmapY && y < heatmapY + heatmapH) {
    return Hit{HitKind::Heatmap, -1};
  }
  const int pageStart = pageStartForSelection(selectedIndex, bookCount);
  const int pageEnd = std::min(bookCount, pageStart + BOOKS_PER_PAGE);
  for (int index = pageStart; index < pageEnd; index++) {
    const RowRect rect = bookRowRect(bookRowX, bookRowWidth, contentTop, index - pageStart);
    if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height) {
      return Hit{HitKind::BookRow, index};
    }
  }
  return Hit{};
}

}  // namespace StatsListLayout
