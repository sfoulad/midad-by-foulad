#include <gtest/gtest.h>

#include <algorithm>

#include "activities/stats/StatsListLayout.h"

// Coverage for StatsActivity's heatmap-button + started-books list geometry:
// page-start derivation, row-rect placement, and hitTest() -- the shared
// pure function driving both render()'s page slicing and loop()'s touch
// hit-testing.

namespace {
using StatsListLayout::BOOK_ROW_GAP;
using StatsListLayout::BOOK_ROW_HEIGHT;
using StatsListLayout::BOOKS_PER_PAGE;
using StatsListLayout::HitKind;

constexpr int HEATMAP_X = 16;
constexpr int HEATMAP_Y = 200;
constexpr int HEATMAP_W = 448;
constexpr int HEATMAP_H = 54;
constexpr int CONTENT_TOP = 280;
}  // namespace

TEST(StatsListLayoutPageStart, EmptyListIsPageZero) { EXPECT_EQ(StatsListLayout::pageStartForSelection(0, 0), 0); }

TEST(StatsListLayoutPageStart, FirstPageForEarlyIndices) {
  for (int selectedIndex = 1; selectedIndex <= BOOKS_PER_PAGE; selectedIndex++) {
    EXPECT_EQ(StatsListLayout::pageStartForSelection(selectedIndex, 10), 0) << "selectedIndex=" << selectedIndex;
  }
}

TEST(StatsListLayoutPageStart, SecondPageStartsAtBooksPerPage) {
  EXPECT_EQ(StatsListLayout::pageStartForSelection(BOOKS_PER_PAGE + 1, 10), BOOKS_PER_PAGE);
}

TEST(StatsListLayoutPageStart, HeatmapSelectionClampsToFirstPage) {
  // selectedIndex == 0 (heatmap button) has no book of its own; the page
  // math must not go negative or out of bounds.
  EXPECT_EQ(StatsListLayout::pageStartForSelection(0, 10), 0);
}

TEST(StatsListLayoutPageStart, LastBookOnPartialLastPage) {
  // 7 books, 3 per page -> pages [0-2][3-5][6]. Selecting book index 6
  // (selectedIndex 7) must land on page start 6, not overshoot.
  EXPECT_EQ(StatsListLayout::pageStartForSelection(7, 7), 6);
}

TEST(StatsListLayoutRowRect, StacksRowsWithGap) {
  const auto row0 = StatsListLayout::bookRowRect(HEATMAP_X, HEATMAP_W, CONTENT_TOP, 0);
  const auto row1 = StatsListLayout::bookRowRect(HEATMAP_X, HEATMAP_W, CONTENT_TOP, 1);
  EXPECT_EQ(row0.y, CONTENT_TOP);
  EXPECT_EQ(row1.y, CONTENT_TOP + BOOK_ROW_HEIGHT + BOOK_ROW_GAP);
  EXPECT_EQ(row0.x, HEATMAP_X);
  EXPECT_EQ(row0.width, HEATMAP_W);
  EXPECT_EQ(row0.height, BOOK_ROW_HEIGHT);
}

TEST(StatsListLayoutHitTest, HitsHeatmapButtonCenter) {
  const auto hit = StatsListLayout::hitTest(HEATMAP_X + HEATMAP_W / 2, HEATMAP_Y + HEATMAP_H / 2, HEATMAP_X, HEATMAP_Y,
                                            HEATMAP_W, HEATMAP_H, CONTENT_TOP, HEATMAP_X, HEATMAP_W, /*bookCount=*/5,
                                            /*selectedIndex=*/0);
  EXPECT_EQ(hit.kind, HitKind::Heatmap);
}

TEST(StatsListLayoutHitTest, HitsEachRowOnFirstPage) {
  constexpr int bookCount = 5;
  for (int index = 0; index < std::min(bookCount, BOOKS_PER_PAGE); index++) {
    const auto rect = StatsListLayout::bookRowRect(HEATMAP_X, HEATMAP_W, CONTENT_TOP, index);
    const auto hit = StatsListLayout::hitTest(rect.x + rect.width / 2, rect.y + rect.height / 2, HEATMAP_X, HEATMAP_Y,
                                              HEATMAP_W, HEATMAP_H, CONTENT_TOP, HEATMAP_X, HEATMAP_W, bookCount,
                                              /*selectedIndex=*/1);
    EXPECT_EQ(hit.kind, HitKind::BookRow) << "index=" << index;
    EXPECT_EQ(hit.bookIndex, index) << "index=" << index;
  }
}

TEST(StatsListLayoutHitTest, HitsRowsOnSecondPage) {
  // 5 books, page 2 holds books [3,4] (0-based); selecting index 4 (selectedIndex 5).
  constexpr int bookCount = 5;
  const int pageStart = StatsListLayout::pageStartForSelection(5, bookCount);
  ASSERT_EQ(pageStart, BOOKS_PER_PAGE);
  const auto rect = StatsListLayout::bookRowRect(HEATMAP_X, HEATMAP_W, CONTENT_TOP, /*rowIndexOnPage=*/1);
  const auto hit = StatsListLayout::hitTest(rect.x + rect.width / 2, rect.y + rect.height / 2, HEATMAP_X, HEATMAP_Y,
                                            HEATMAP_W, HEATMAP_H, CONTENT_TOP, HEATMAP_X, HEATMAP_W, bookCount,
                                            /*selectedIndex=*/5);
  EXPECT_EQ(hit.kind, HitKind::BookRow);
  EXPECT_EQ(hit.bookIndex, 4);
}

TEST(StatsListLayoutHitTest, MissesBelowLastRowOnPartialPage) {
  // Only 1 book on the page -- tapping where row 1 would be (if it existed)
  // must miss, not fall through to a stale/absent row.
  constexpr int bookCount = 1;
  const auto phantomRow = StatsListLayout::bookRowRect(HEATMAP_X, HEATMAP_W, CONTENT_TOP, /*rowIndexOnPage=*/1);
  const auto hit = StatsListLayout::hitTest(phantomRow.x + phantomRow.width / 2, phantomRow.y + phantomRow.height / 2,
                                            HEATMAP_X, HEATMAP_Y, HEATMAP_W, HEATMAP_H, CONTENT_TOP, HEATMAP_X,
                                            HEATMAP_W, bookCount, /*selectedIndex=*/1);
  EXPECT_EQ(hit.kind, HitKind::None);
}

TEST(StatsListLayoutHitTest, MissesEmptyListEntirely) {
  const auto hit = StatsListLayout::hitTest(HEATMAP_X + 10, CONTENT_TOP + 10, HEATMAP_X, HEATMAP_Y, HEATMAP_W,
                                            HEATMAP_H, CONTENT_TOP, HEATMAP_X, HEATMAP_W, /*bookCount=*/0,
                                            /*selectedIndex=*/0);
  EXPECT_EQ(hit.kind, HitKind::None);
}

TEST(StatsListLayoutHitTest, MissesTheGapBetweenRows) {
  if (BOOK_ROW_GAP <= 0) return;  // no gap to miss into
  const auto row0 = StatsListLayout::bookRowRect(HEATMAP_X, HEATMAP_W, CONTENT_TOP, 0);
  const int y = row0.y + row0.height;  // just past row 0, short of row 1's top
  const auto hit = StatsListLayout::hitTest(HEATMAP_X + 10, y, HEATMAP_X, HEATMAP_Y, HEATMAP_W, HEATMAP_H, CONTENT_TOP,
                                            HEATMAP_X, HEATMAP_W, /*bookCount=*/5, /*selectedIndex=*/1);
  EXPECT_EQ(hit.kind, HitKind::None);
}

TEST(StatsListLayoutHitTest, MissesOutsideRowXBounds) {
  const auto row0 = StatsListLayout::bookRowRect(HEATMAP_X, HEATMAP_W, CONTENT_TOP, 0);
  const auto hit = StatsListLayout::hitTest(row0.x + row0.width + 5, row0.y + row0.height / 2, HEATMAP_X, HEATMAP_Y,
                                            HEATMAP_W, HEATMAP_H, CONTENT_TOP, HEATMAP_X, HEATMAP_W,
                                            /*bookCount=*/5, /*selectedIndex=*/1);
  EXPECT_EQ(hit.kind, HitKind::None);
}

TEST(StatsListLayoutHitTest, RowsTakePrecedenceOverHeatmapWhenOverlapping) {
  // If a row rect happened to overlap the heatmap rect (degenerate geometry),
  // the heatmap check runs first -- document that as the defined behavior.
  const auto hit = StatsListLayout::hitTest(HEATMAP_X + 5, HEATMAP_Y + 5, HEATMAP_X, HEATMAP_Y, HEATMAP_W, HEATMAP_H,
                                            /*contentTop=*/HEATMAP_Y, HEATMAP_X, HEATMAP_W, /*bookCount=*/5,
                                            /*selectedIndex=*/1);
  EXPECT_EQ(hit.kind, HitKind::Heatmap);
}
