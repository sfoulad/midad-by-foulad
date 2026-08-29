#include <gtest/gtest.h>

#include "activities/apps/AppsGridLayout.h"

// Coverage for the Apps launcher's static 2x3 grid: pagination, RTL column
// mirroring, touch hit-testing (incl. the touch-slop enlarged hit target),
// selection-index clamping (the persistence-across-reconstruction guard), and
// the page/row/col math AppsActivity's navigation is built from.
//
// Deliberately does NOT exercise util/GridNav.h directly (the wraparound/
// page-crossing move functions AppsActivity::loop() actually calls): GridNav.h
// transitively includes Logging.h, which pulls in Arduino.h and isn't
// host-compilable, and giving it a host-testable seam is a change to shared
// navigation infra RecentBooksActivity/OpdsBookBrowserActivity also depend on
// -- out of scope for this launcher-only redesign. GridNav's own move
// functions are exercised on-device (see the PR's screenshot/manual
// verification pass); what's tested here is the page/row/col arithmetic this
// grid feeds into those calls.

namespace {
using AppsGridLayout::COLUMNS;
using AppsGridLayout::ITEMS_PER_PAGE;
using AppsGridLayout::ROWS;
}  // namespace

TEST(AppsGridLayoutPagination, SinglePageWhenSixOrFewer) {
  EXPECT_EQ(AppsGridLayout::pageCount(0), 1);
  EXPECT_EQ(AppsGridLayout::pageCount(1), 1);
  EXPECT_EQ(AppsGridLayout::pageCount(6), 1);
}

TEST(AppsGridLayoutPagination, SecondPageStartsAtSeven) {
  EXPECT_EQ(AppsGridLayout::pageCount(7), 2);
  EXPECT_EQ(AppsGridLayout::pageCount(8), 2);  // the real registry size today
  EXPECT_EQ(AppsGridLayout::pageCount(12), 2);
  EXPECT_EQ(AppsGridLayout::pageCount(13), 3);
}

TEST(AppsGridLayoutPagination, PageIndexAndStartTrackSelector) {
  EXPECT_EQ(AppsGridLayout::pageIndexOf(0), 0);
  EXPECT_EQ(AppsGridLayout::pageIndexOf(5), 0);
  EXPECT_EQ(AppsGridLayout::pageIndexOf(6), 1);
  EXPECT_EQ(AppsGridLayout::pageIndexOf(7), 1);
  EXPECT_EQ(AppsGridLayout::pageStartOf(6), 6);
  EXPECT_EQ(AppsGridLayout::pageStartOf(11), 6);
}

TEST(AppsGridLayoutPagination, JumpPageNoOpOnSinglePage) {
  EXPECT_EQ(AppsGridLayout::jumpPage(2, 6, 1), 2);
  EXPECT_EQ(AppsGridLayout::jumpPage(2, 6, -1), 2);
  EXPECT_EQ(AppsGridLayout::jumpPage(0, 0, 1), 0);  // empty list
}

TEST(AppsGridLayoutPagination, JumpPagePreservesIndexInPage) {
  // 8 items = 2 pages (6 + 2). Index-in-page 1 exists on both pages.
  EXPECT_EQ(AppsGridLayout::jumpPage(1, 8, 1), 7);   // page 0 -> page 1, same slot
  EXPECT_EQ(AppsGridLayout::jumpPage(7, 8, -1), 1);  // page 1 -> page 0, same slot
}

TEST(AppsGridLayoutPagination, JumpPageClampsOnPartialLastPage) {
  // Page 1 only has 2 items (indices 6-7); index-in-page 4 (from page 0's
  // index 4) has no counterpart there, so it lands on the last real item.
  EXPECT_EQ(AppsGridLayout::jumpPage(4, 8, 1), 7);
}

TEST(AppsGridLayoutPagination, JumpPageWrapsAround) {
  // 13 items = 3 pages. From the last page, +1 wraps to page 0; from page 0,
  // -1 wraps to the last page.
  EXPECT_EQ(AppsGridLayout::pageIndexOf(AppsGridLayout::jumpPage(12, 13, 1)), 0);
  EXPECT_EQ(AppsGridLayout::pageIndexOf(AppsGridLayout::jumpPage(0, 13, -1)), 2);
}

TEST(AppsGridLayoutPagination, RowAndColWithinPage) {
  // Index-in-page 0..5 across a 2-col x 3-row page.
  EXPECT_EQ(AppsGridLayout::rowInPage(0), 0);
  EXPECT_EQ(AppsGridLayout::colInPage(0), 0);
  EXPECT_EQ(AppsGridLayout::rowInPage(1), 0);
  EXPECT_EQ(AppsGridLayout::colInPage(1), 1);
  EXPECT_EQ(AppsGridLayout::rowInPage(5), 2);
  EXPECT_EQ(AppsGridLayout::colInPage(5), 1);
}

TEST(AppsGridLayoutRtl, MirroredColumnFlipsUnderRtl) {
  EXPECT_EQ(AppsGridLayout::mirroredColumn(0, false), 0);
  EXPECT_EQ(AppsGridLayout::mirroredColumn(1, false), 1);
  EXPECT_EQ(AppsGridLayout::mirroredColumn(0, true), 1);
  EXPECT_EQ(AppsGridLayout::mirroredColumn(1, true), 0);
}

TEST(AppsGridLayoutRtl, MirroringIsSelfInverse) {
  for (const bool rtl : {false, true}) {
    for (int col = 0; col < COLUMNS; col++) {
      EXPECT_EQ(AppsGridLayout::mirroredColumn(AppsGridLayout::mirroredColumn(col, rtl), rtl), col);
    }
  }
}

TEST(AppsGridLayoutSelection, ClampsNegativeToZero) { EXPECT_EQ(AppsGridLayout::clampSelection(-1, 8), 0); }

TEST(AppsGridLayoutSelection, ClampsOutOfRangeToLastValidIndex) { EXPECT_EQ(AppsGridLayout::clampSelection(99, 8), 7); }

TEST(AppsGridLayoutSelection, PreservesInRangeIndex) {
  // The persistence contract itself: a remembered index that's still valid
  // comes back unchanged -- this is what makes "return from Files/Quran keeps
  // your place" work (see AppsActivity::onEnter/onExit).
  EXPECT_EQ(AppsGridLayout::clampSelection(3, 8), 3);
}

TEST(AppsGridLayoutSelection, EmptyListSelectsZero) {
  EXPECT_EQ(AppsGridLayout::clampSelection(0, 0), 0);
  EXPECT_EQ(AppsGridLayout::clampSelection(5, 0), 0);
}

namespace {
// A page's worth of geometry matching what AppsActivity::computeGeometry()
// would produce on a plausible screen -- exact pixel values don't matter,
// only that hitTestTile's math is exercised against realistic proportions.
constexpr int GRID_START_X = 20;
constexpr int CONTENT_TOP = 100;
constexpr int TILE_WIDTH = 200;
constexpr int TILE_HEIGHT = 150;
constexpr int GUTTER = 12;

int tileCenterX(const int col) { return GRID_START_X + col * (TILE_WIDTH + GUTTER) + TILE_WIDTH / 2; }
int tileCenterY(const int row) { return CONTENT_TOP + row * (TILE_HEIGHT + GUTTER) + TILE_HEIGHT / 2; }
}  // namespace

TEST(AppsGridLayoutTouch, HitsCorrectTileAtTileCenter) {
  for (int row = 0; row < ROWS; row++) {
    for (int col = 0; col < COLUMNS; col++) {
      const int hit = AppsGridLayout::hitTestTile(tileCenterX(col), tileCenterY(row), GRID_START_X, CONTENT_TOP,
                                                  TILE_WIDTH, TILE_HEIGHT, GUTTER, /*rtl=*/false, ITEMS_PER_PAGE);
      EXPECT_EQ(hit, row * COLUMNS + col) << "row=" << row << " col=" << col;
    }
  }
}

TEST(AppsGridLayoutTouch, MissesGutterWithoutSlop) {
  // Squarely in the gutter between column 0 and column 1, no slop: a miss.
  const int gutterX = GRID_START_X + TILE_WIDTH + GUTTER / 2;
  const int hit = AppsGridLayout::hitTestTile(gutterX, tileCenterY(0), GRID_START_X, CONTENT_TOP, TILE_WIDTH,
                                              TILE_HEIGHT, GUTTER, /*rtl=*/false, ITEMS_PER_PAGE, /*touchSlop=*/0);
  EXPECT_EQ(hit, -1);
}

TEST(AppsGridLayoutTouch, TouchSlopClaimsTheNearerNeighborAcrossTheGutter) {
  // Half the gutter is claimed by whichever tile is closer -- the "larger hit
  // target" the X4 Pro touch layout gets (AppsActivity passes gutter/2).
  const int slop = GUTTER / 2;
  const int justPastCol0 = GRID_START_X + TILE_WIDTH + 1;             // 1px into the gutter, column 0's side
  const int justBeforeCol1 = GRID_START_X + TILE_WIDTH + GUTTER - 1;  // 1px into the gutter, column 1's side

  EXPECT_EQ(AppsGridLayout::hitTestTile(justPastCol0, tileCenterY(0), GRID_START_X, CONTENT_TOP, TILE_WIDTH,
                                        TILE_HEIGHT, GUTTER, false, ITEMS_PER_PAGE, slop),
            0);
  EXPECT_EQ(AppsGridLayout::hitTestTile(justBeforeCol1, tileCenterY(0), GRID_START_X, CONTENT_TOP, TILE_WIDTH,
                                        TILE_HEIGHT, GUTTER, false, ITEMS_PER_PAGE, slop),
            1);
}

TEST(AppsGridLayoutTouch, RtlMirrorsWhichTileATapHits) {
  // Same tap position (visually column 0, the leftmost slot), opposite logical
  // hit under RTL -- column 0 visually is logical column 1 when mirrored.
  const int hitLtr = AppsGridLayout::hitTestTile(tileCenterX(0), tileCenterY(0), GRID_START_X, CONTENT_TOP, TILE_WIDTH,
                                                 TILE_HEIGHT, GUTTER, /*rtl=*/false, ITEMS_PER_PAGE);
  const int hitRtl = AppsGridLayout::hitTestTile(tileCenterX(0), tileCenterY(0), GRID_START_X, CONTENT_TOP, TILE_WIDTH,
                                                 TILE_HEIGHT, GUTTER, /*rtl=*/true, ITEMS_PER_PAGE);
  EXPECT_EQ(hitLtr, 0);
  EXPECT_EQ(hitRtl, 1);
}

TEST(AppsGridLayoutTouch, MissesPastGridBounds) {
  const int farX = GRID_START_X + COLUMNS * (TILE_WIDTH + GUTTER) + 500;
  const int hit = AppsGridLayout::hitTestTile(farX, tileCenterY(0), GRID_START_X, CONTENT_TOP, TILE_WIDTH, TILE_HEIGHT,
                                              GUTTER, false, ITEMS_PER_PAGE);
  EXPECT_EQ(hit, -1);
}

TEST(AppsGridLayoutTouch, MissesBeforeGridOrigin) {
  const int hit = AppsGridLayout::hitTestTile(GRID_START_X - 50, CONTENT_TOP - 50, GRID_START_X, CONTENT_TOP,
                                              TILE_WIDTH, TILE_HEIGHT, GUTTER, false, ITEMS_PER_PAGE);
  EXPECT_EQ(hit, -1);
}

TEST(AppsGridLayoutTouch, RejectsSlotsWithNoRealItemOnAPartialLastPage) {
  // 8 apps, page 1 (indices 6,7) holds only 2 of the page's 6 slots. A tap on
  // the tile that WOULD be index-in-page 2 (bottom row) must miss, even though
  // it lands squarely on a real tile rect -- there's no app there.
  constexpr int itemsOnLastPage = 2;  // 8 total, 6 on page 0, 2 on page 1
  const int hitFilledSlot = AppsGridLayout::hitTestTile(tileCenterX(1), tileCenterY(0), GRID_START_X, CONTENT_TOP,
                                                        TILE_WIDTH, TILE_HEIGHT, GUTTER, false, itemsOnLastPage);
  const int hitEmptySlot = AppsGridLayout::hitTestTile(tileCenterX(0), tileCenterY(1), GRID_START_X, CONTENT_TOP,
                                                       TILE_WIDTH, TILE_HEIGHT, GUTTER, false, itemsOnLastPage);
  EXPECT_EQ(hitFilledSlot, 1);
  EXPECT_EQ(hitEmptySlot, -1);
}

// Navigation math: the page/row/col arithmetic AppsActivity::loop() combines
// with GridNav's move functions (see the file header comment for why GridNav
// itself isn't exercised here).
TEST(AppsGridLayoutNavigation, LastRowOfPageZeroPrecedesPageOneStart) {
  // Index 5 (row 2, col 1) is the bottom-right of page 0 -- one more "down"
  // press must land somewhere on page 1, whose first index is pageStartOf(6).
  EXPECT_EQ(AppsGridLayout::rowInPage(5), ROWS - 1);
  EXPECT_EQ(AppsGridLayout::pageStartOf(6), ITEMS_PER_PAGE);
  EXPECT_EQ(AppsGridLayout::pageIndexOf(5), 0);
  EXPECT_EQ(AppsGridLayout::pageIndexOf(6), 1);
}

TEST(AppsGridLayoutNavigation, EveryIndexInPageMapsToAUniqueCell) {
  // No two of the 6 slots on a page collide on the same (row, col).
  bool seen[ROWS][COLUMNS] = {};
  for (int i = 0; i < ITEMS_PER_PAGE; i++) {
    const int row = AppsGridLayout::rowInPage(i);
    const int col = AppsGridLayout::colInPage(i);
    ASSERT_FALSE(seen[row][col]) << "collision at i=" << i;
    seen[row][col] = true;
  }
}
