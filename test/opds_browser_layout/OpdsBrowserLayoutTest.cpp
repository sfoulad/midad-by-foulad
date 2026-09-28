#include <gtest/gtest.h>

#include <set>
#include <utility>
#include <vector>

#include "activities/browser/OpdsBrowserLayout.h"
#include "activities/browser/OpdsRowIcon.h"

// Coverage for OpdsBookBrowserActivity's catalogue geometry: the Library
// landing / category list rows, the cover grid's cells, the Prev/Next Page
// controls in the nav strips, and boundary taps at every edge of all three.
//
// These are the functions render() draws through and loop() hit-tests through.
// A separate hit-test computation drifting from the render math is the exact
// bug class this header exists to make impossible, so several tests below
// assert the agreement directly rather than only checking hitTest() in
// isolation.

namespace L = OpdsBrowserLayout;

namespace {

// X4 portrait: the panel the Library landing was photographed on.
constexpr int X4_W = 480;
constexpr int X4_H = 800;
// X3 portrait logical width.
constexpr int X3_W = 528;
constexpr int X3_H = 880;

// Live font metrics the activity measures and passes in: UI_10 line height + 6
// px padding for a list row, and the worst-case small-caption line height.
constexpr int ROW_H = 30;
constexpr int CAPTION_H = 20;

// The Library landing exactly as the server sends it, plus the synthetic Search
// row the browser inserts above everything: Search, English, My Books, By
// Author, All Books, Recently Added. Pure navigation -- no covers, so no grid.
constexpr int LANDING_ENTRIES = 6;

L::Grid landingGrid(const int width = X4_W, const int height = X4_H) {
  return L::computeGrid(width, height, ROW_H, CAPTION_H, LANDING_ENTRIES, -1, -1);
}

// A book page: `books` cover cells, optionally preceded by a Search row and a
// "Previous Page" row, optionally followed by a "Next Page" row.
L::Grid bookGrid(const int books, const int topNav, const int bottomNav, const int width = X4_W,
                 const int height = X4_H) {
  const int entryCount = topNav + books + bottomNav;
  return L::computeGrid(width, height, ROW_H, CAPTION_H, entryCount, topNav, topNav + books - 1);
}

L::Hit hit(const L::Grid& grid, const int x, const int y, const int entryCount, const int selectorIndex,
           const int slop = 0, const int width = X4_W, const int height = X4_H) {
  return L::hitTest(x, y, grid, width, height, ROW_H, CAPTION_H, entryCount, selectorIndex, slop);
}

}  // namespace

// ---------------------------------------------------------------------------
// Library landing / category list rows
// ---------------------------------------------------------------------------

TEST(OpdsBrowserLayoutLanding, PureNavigationFeedIsNotAGridPage) {
  const L::Grid grid = landingGrid();
  EXPECT_FALSE(grid.isGridPage);
  EXPECT_EQ(grid.bookCount, 0);
}

TEST(OpdsBrowserLayoutLanding, AllSixEntriesFitOnOnePage) { EXPECT_GE(L::listPageItems(X4_H, ROW_H), LANDING_ENTRIES); }

TEST(OpdsBrowserLayoutLanding, EveryLandingEntryIsTappableAtItsOwnRow) {
  const L::Grid grid = landingGrid();
  for (int i = 0; i < LANDING_ENTRIES; i++) {
    const L::Rect row = L::listRowRect(X4_W, i, ROW_H);
    const L::Hit h = hit(grid, X4_W / 2, row.y + ROW_H / 2, LANDING_ENTRIES, 0);
    EXPECT_EQ(h.kind, L::HitKind::ListRow) << "entry " << i;
    EXPECT_EQ(h.entryIndex, i) << "entry " << i;
  }
}

TEST(OpdsBrowserLayoutLanding, RowsTileTheContentBandWithoutOverlapOrGap) {
  const int pageItems = L::listPageItems(X4_H, ROW_H);
  for (int i = 1; i < pageItems; i++) {
    const L::Rect previous = L::listRowRect(X4_W, i - 1, ROW_H);
    const L::Rect current = L::listRowRect(X4_W, i, ROW_H);
    EXPECT_EQ(previous.y + previous.height, current.y) << "row " << i;
  }
}

TEST(OpdsBrowserLayoutLanding, RowsSpanTheFullScreenWidthSoEitherEdgeTaps) {
  const L::Grid grid = landingGrid();
  const int midRowY = L::listRowRect(X4_W, 2, ROW_H).y + ROW_H / 2;
  EXPECT_EQ(hit(grid, 0, midRowY, LANDING_ENTRIES, 0).entryIndex, 2);
  EXPECT_EQ(hit(grid, X4_W - 1, midRowY, LANDING_ENTRIES, 0).entryIndex, 2);
}

TEST(OpdsBrowserLayoutLanding, TapAboveTheContentTopMisses) {
  const L::Grid grid = landingGrid();
  EXPECT_EQ(hit(grid, X4_W / 2, L::CONTENT_TOP - 1, LANDING_ENTRIES, 0).kind, L::HitKind::None);
}

TEST(OpdsBrowserLayoutLanding, TapBelowTheLastRealRowMisses) {
  // Six entries on a page that has room for many more: the empty remainder of
  // the band must not resolve to a seventh, non-existent entry.
  const L::Grid grid = landingGrid();
  const int belowLast = L::listRowRect(X4_W, LANDING_ENTRIES, ROW_H).y + 1;
  EXPECT_EQ(hit(grid, X4_W / 2, belowLast, LANDING_ENTRIES, 0).kind, L::HitKind::None);
}

TEST(OpdsBrowserLayoutLanding, EmptyFeedHasNothingToTap) {
  const L::Grid grid = L::computeGrid(X4_W, X4_H, ROW_H, CAPTION_H, 0, -1, -1);
  EXPECT_EQ(hit(grid, X4_W / 2, L::CONTENT_TOP + 5, 0, 0).kind, L::HitKind::None);
}

// ---------------------------------------------------------------------------
// List pagination: a long category page
// ---------------------------------------------------------------------------

TEST(OpdsBrowserLayoutListPaging, PageStartFollowsTheSelection) {
  const int pageItems = L::listPageItems(X4_H, ROW_H);
  EXPECT_EQ(L::listPageStart(0, pageItems), 0);
  EXPECT_EQ(L::listPageStart(pageItems - 1, pageItems), 0);
  EXPECT_EQ(L::listPageStart(pageItems, pageItems), pageItems);
  EXPECT_EQ(L::listPageStart(pageItems * 2 + 1, pageItems), pageItems * 2);
}

TEST(OpdsBrowserLayoutListPaging, TapsResolveAgainstTheVisiblePageNotTheWholeFeed) {
  const int pageItems = L::listPageItems(X4_H, ROW_H);
  const int entryCount = pageItems * 3;
  const L::Grid grid = L::computeGrid(X4_W, X4_H, ROW_H, CAPTION_H, entryCount, -1, -1);
  // Selection sits on page 2, so the top row on screen is entry `pageItems`.
  const int topRowY = L::listRowRect(X4_W, 0, ROW_H).y + ROW_H / 2;
  EXPECT_EQ(hit(grid, X4_W / 2, topRowY, entryCount, pageItems).entryIndex, pageItems);
}

TEST(OpdsBrowserLayoutListPaging, ShortLastPageRejectsTapsPastItsFinalRow) {
  const int pageItems = L::listPageItems(X4_H, ROW_H);
  const int entryCount = pageItems + 2;  // last page holds exactly 2 rows
  const L::Grid grid = L::computeGrid(X4_W, X4_H, ROW_H, CAPTION_H, entryCount, -1, -1);
  EXPECT_EQ(hit(grid, X4_W / 2, L::listRowRect(X4_W, 1, ROW_H).y + 1, entryCount, pageItems).entryIndex, pageItems + 1);
  EXPECT_EQ(hit(grid, X4_W / 2, L::listRowRect(X4_W, 2, ROW_H).y + 1, entryCount, pageItems).kind, L::HitKind::None);
}

// ---------------------------------------------------------------------------
// Cover grid cells
// ---------------------------------------------------------------------------

TEST(OpdsBrowserLayoutGrid, ThreeColumnsOnBothPortraitPanels) {
  EXPECT_EQ(bookGrid(25, 0, 0, X4_W, X4_H).columns, 3);
  EXPECT_EQ(bookGrid(25, 0, 0, X3_W, X3_H).columns, 3);
}

TEST(OpdsBrowserLayoutGrid, CoversFillTheScreenWidthEdgeToEdge) {
  const L::Grid grid = bookGrid(25, 0, 0);
  const int spanned = L::gridStartX(grid, X4_W) * 2 + grid.columns * (grid.coverWidth + L::GUTTER) - L::GUTTER;
  EXPECT_GE(spanned, X4_W - L::GUTTER * 2);
  EXPECT_LE(spanned, X4_W);
}

TEST(OpdsBrowserLayoutGrid, EveryCellOnThePageIsTappableAtItsCentre) {
  const L::Grid grid = bookGrid(25, 0, 0);
  const int entryCount = 25;
  for (int slot = 0; slot < grid.itemsPerPage; slot++) {
    const L::Rect cell = L::gridCellHitRect(grid, X4_W, ROW_H, CAPTION_H, slot);
    const L::Hit h = hit(grid, cell.x + cell.width / 2, cell.y + cell.height / 2, entryCount, 0);
    EXPECT_EQ(h.kind, L::HitKind::GridCell) << "slot " << slot;
    EXPECT_EQ(h.slot, slot) << "slot " << slot;
    EXPECT_EQ(h.entryIndex, slot) << "slot " << slot;
  }
}

TEST(OpdsBrowserLayoutGrid, TappingACoverAndTappingItsCaptionOpenTheSameBook) {
  const L::Grid grid = bookGrid(25, 0, 0);
  const L::Rect cover = L::gridCoverRect(grid, X4_W, ROW_H, CAPTION_H, 4);
  const int captionY = cover.y + cover.height + L::titleHeight(CAPTION_H) / 2;
  EXPECT_EQ(hit(grid, cover.x + 2, cover.y + 2, 25, 0).entryIndex, 4);
  EXPECT_EQ(hit(grid, cover.x + 2, captionY, 25, 0).entryIndex, 4);
}

TEST(OpdsBrowserLayoutGrid, CellsDoNotOverlap) {
  const L::Grid grid = bookGrid(25, 0, 0);
  std::set<std::pair<int, int>> seen;
  for (int slot = 0; slot < grid.itemsPerPage; slot++) {
    const L::Rect cell = L::gridCellHitRect(grid, X4_W, ROW_H, CAPTION_H, slot);
    for (int other = 0; other < slot; other++) {
      const L::Rect prior = L::gridCellHitRect(grid, X4_W, ROW_H, CAPTION_H, other);
      const bool overlapX = cell.x < prior.x + prior.width && prior.x < cell.x + cell.width;
      const bool overlapY = cell.y < prior.y + prior.height && prior.y < cell.y + cell.height;
      EXPECT_FALSE(overlapX && overlapY) << "slot " << slot << " overlaps " << other;
    }
    EXPECT_TRUE(seen.insert({cell.x, cell.y}).second) << "duplicate origin at slot " << slot;
  }
}

TEST(OpdsBrowserLayoutGrid, TapsResolveAgainstTheVisibleGridPage) {
  const L::Grid grid = bookGrid(25, 0, 0);
  ASSERT_GT(grid.itemsPerPage, 0);
  // Selection on page 2: the top-left cell is book `itemsPerPage`, not book 0.
  const L::Rect cell = L::gridCellHitRect(grid, X4_W, ROW_H, CAPTION_H, 0);
  const L::Hit h = hit(grid, cell.x + 1, cell.y + 1, 25, grid.itemsPerPage);
  EXPECT_EQ(h.slot, 0);
  EXPECT_EQ(h.entryIndex, grid.itemsPerPage);
}

TEST(OpdsBrowserLayoutGrid, ShortLastPageRejectsTapsOnEmptySlots) {
  // 25 books over pages of 6 leaves 1 book on the last page: slots 1..5 are
  // empty and must not open the wrong book (or anything at all).
  const L::Grid grid = bookGrid(25, 0, 0);
  const int lastPageStart = L::gridPageStart(24, grid.itemsPerPage);
  ASSERT_EQ(L::gridCellsOnPage(grid, lastPageStart), 25 - lastPageStart);
  for (int slot = L::gridCellsOnPage(grid, lastPageStart); slot < grid.itemsPerPage; slot++) {
    const L::Rect cell = L::gridCellHitRect(grid, X4_W, ROW_H, CAPTION_H, slot);
    EXPECT_EQ(hit(grid, cell.x + cell.width / 2, cell.y + cell.height / 2, 25, 24).kind, L::HitKind::None)
        << "empty slot " << slot;
  }
}

TEST(OpdsBrowserLayoutGrid, TheGridNeverReachesIntoTheBottomStrip) {
  // The strips are drawn at the same rowHeight as the list, so a taller touch
  // row shortens the grid's band. The last cover row must still finish above
  // the "Next Page" control on every panel and both row heights.
  for (const int rowHeight : {ROW_H, 48}) {
    for (const auto& panel : std::vector<std::pair<int, int>>{{X4_W, X4_H}, {X3_W, X3_H}, {X4_H, X4_W}, {X3_H, X3_W}}) {
      const int w = panel.first;
      const int h = panel.second;
      const int books = 60;
      const int entryCount = 2 + books + 1;
      const L::Grid grid = L::computeGrid(w, h, rowHeight, CAPTION_H, entryCount, 2, 2 + books - 1);
      // A band too short for even one whole row is not a grid page at all any
      // more -- it renders as a plain list, which is what keeps the promise
      // below absolute (see the OpdsBrowserLayoutShortBand tests). Landscape at
      // the 48px touch row height is exactly that case.
      if (!grid.isGridPage) continue;
      const int lastSlot = grid.itemsPerPage - 1;
      const L::Rect cell = L::gridCellHitRect(grid, w, rowHeight, CAPTION_H, lastSlot);
      const int stripTop = L::bottomStripTop(grid, h, entryCount, rowHeight);
      // Unconditional now: the "one row is the floor" carve-out this test used
      // to need was the bug -- it drew a cell across the strip and let the strip
      // steal its taps.
      EXPECT_LE(cell.y + cell.height, stripTop) << w << "x" << h << " rowHeight=" << rowHeight;
    }
  }
}

// ---------------------------------------------------------------------------
// A band too short for one whole cover row
// ---------------------------------------------------------------------------

namespace {

// A landscape touch board: 800x480 with the taller icon rows the touch builds
// use for the Search / Previous Page / Next Page strips.
constexpr int TOUCH_ROW_H = 44;
constexpr int TOUCH_TOP_NAV = 2;     // Search + Previous Page
constexpr int TOUCH_BOTTOM_NAV = 1;  // Next Page

L::Grid touchGrid(const int books, const int width, const int height) {
  const int entryCount = TOUCH_TOP_NAV + books + TOUCH_BOTTOM_NAV;
  return L::computeGrid(width, height, TOUCH_ROW_H, CAPTION_H, entryCount, TOUCH_TOP_NAV, TOUCH_TOP_NAV + books - 1);
}

// Everything the band's height is spent on besides the grid itself, so a test
// can name the screen height at which exactly one row fits.
constexpr int bandOverhead() {
  return L::CONTENT_TOP + TOUCH_TOP_NAV * TOUCH_ROW_H + L::TOP_STRIP_GAP + L::BOTTOM_MARGIN +
         TOUCH_BOTTOM_NAV * TOUCH_ROW_H;
}

}  // namespace

TEST(OpdsBrowserLayoutShortBand, NoCompleteRowFallsBackToTheList) {
  // The reported case: landscape touch board, Search + Previous Page above the
  // grid and Next Page below it, all at the 44px icon-row height. That leaves
  // less than one cover pitch, and forcing a row anyway drew the cell across
  // the bottom strip -- where hitTest() gives the strip priority, so tapping a
  // VISIBLE part of the book activated "Next Page" instead of opening it.
  const L::Grid grid = touchGrid(12, X4_H, X4_W);  // 800x480
  EXPECT_FALSE(grid.isGridPage);
  EXPECT_EQ(grid.bookCount, 0);

  // The page still works -- it is simply a plain list now, so every entry
  // (including the Next Page control) is reachable at its own row.
  const int entryCount = TOUCH_TOP_NAV + 12 + TOUCH_BOTTOM_NAV;
  const L::Rect firstRow = L::listRowRect(X4_H, 0, TOUCH_ROW_H);
  const L::Hit h = L::hitTest(firstRow.x + 5, firstRow.y + TOUCH_ROW_H / 2, grid, X4_H, X4_W, TOUCH_ROW_H, CAPTION_H,
                              entryCount, 0, 0);
  EXPECT_EQ(h.kind, L::HitKind::ListRow);
  EXPECT_EQ(h.entryIndex, 0);
}

TEST(OpdsBrowserLayoutShortBand, ABandExactlyOnePitchTallStillGetsItsRow) {
  // Boundary. Measure the pitch off a screen tall enough to be a grid page
  // (cover size depends only on width), then name the exact height at which the
  // band is one pitch: that height must keep its single row, and one pixel less
  // must fall back to the list.
  const L::Grid tall = touchGrid(12, X4_H, 900);
  ASSERT_TRUE(tall.isGridPage);
  const int pitch = tall.coverHeight + L::titleHeight(CAPTION_H) + L::GUTTER;
  const int exactHeight = pitch + bandOverhead();

  const L::Grid exact = touchGrid(12, X4_H, exactHeight);
  ASSERT_TRUE(exact.isGridPage) << "a band of exactly one pitch must keep its row";
  EXPECT_EQ(exact.itemsPerPage, exact.columns) << "exactly one row, not two";

  const L::Grid oneShort = touchGrid(12, X4_H, exactHeight - 1);
  EXPECT_FALSE(oneShort.isGridPage) << "one pixel short of a whole row is not a grid page";
}

TEST(OpdsBrowserLayoutShortBand, NoDrawnCellEverOverlapsTheBottomStripAtAnyHeight) {
  // The invariant the fix exists to hold, swept rather than spot-checked: at
  // every screen height, either the page is not a grid at all, or every drawn
  // cell (cover PLUS its caption -- what the user sees as "the book") finishes
  // above the strip that would otherwise steal its taps.
  constexpr int BOOKS = 12;
  const int entryCount = TOUCH_TOP_NAV + BOOKS + TOUCH_BOTTOM_NAV;
  for (int height = 200; height <= 900; height++) {
    const L::Grid grid = touchGrid(BOOKS, X4_H, height);
    if (!grid.isGridPage) continue;

    const int stripTop = L::bottomStripTop(grid, height, entryCount, TOUCH_ROW_H);
    const int cells = L::gridCellsOnPage(grid, 0);
    ASSERT_GT(cells, 0) << "height=" << height;
    for (int slot = 0; slot < cells; slot++) {
      const L::Rect cell = L::gridCellHitRect(grid, X4_H, TOUCH_ROW_H, CAPTION_H, slot);
      EXPECT_LE(cell.y + cell.height, stripTop) << "height=" << height << " slot=" << slot;
    }
  }
}

TEST(OpdsBrowserLayoutGrid, ButtonBoardRowCountsAreUnchangedByTheBandMeasurement) {
  // X3/X4 keep the 30px text row, and their grid must hold exactly what it held
  // before the band measurement was introduced: two cover rows in portrait, one
  // in landscape, with or without nav strips.
  struct Case {
    int w, h, topNav, bottomNav, expectedRows;
  };
  for (const Case& c : std::vector<Case>{{X4_W, X4_H, 0, 0, 2},
                                         {X4_W, X4_H, 2, 1, 2},
                                         {X3_W, X3_H, 0, 0, 2},
                                         {X3_W, X3_H, 2, 1, 2},
                                         {X4_H, X4_W, 0, 0, 1},
                                         {X4_H, X4_W, 2, 1, 1},
                                         {X3_H, X3_W, 0, 0, 1},
                                         {X3_H, X3_W, 2, 1, 1}}) {
    const int books = 60;
    const int entryCount = c.topNav + books + c.bottomNav;
    const L::Grid grid = L::computeGrid(c.w, c.h, ROW_H, CAPTION_H, entryCount, c.topNav, c.topNav + books - 1);
    ASSERT_GT(grid.columns, 0);
    EXPECT_EQ(grid.itemsPerPage / grid.columns, c.expectedRows)
        << c.w << "x" << c.h << " strips=" << c.topNav << "/" << c.bottomNav;
  }
}

TEST(OpdsBrowserLayoutGrid, GridTopClearsANonEmptyTopStrip) {
  const L::Grid withStrip = bookGrid(25, 2, 0);
  const L::Grid noStrip = bookGrid(25, 0, 0);
  EXPECT_EQ(L::gridTop(noStrip, ROW_H), L::CONTENT_TOP);
  EXPECT_EQ(L::gridTop(withStrip, ROW_H), L::CONTENT_TOP + 2 * ROW_H + L::TOP_STRIP_GAP);
}

// ---------------------------------------------------------------------------
// Page controls: the Search / Previous Page / Next Page nav strips
// ---------------------------------------------------------------------------

TEST(OpdsBrowserLayoutPageControls, SearchAndPrevPageRowsSitAboveTheGrid) {
  // Entry 0 = Search, entry 1 = "« Previous Page", entries 2..26 = books,
  // entry 27 = "Next Page »".
  const int books = 25;
  const L::Grid grid = bookGrid(books, 2, 1);
  const int entryCount = 2 + books + 1;
  ASSERT_EQ(grid.topNavCount, 2);
  ASSERT_EQ(grid.bookStart, 2);
  ASSERT_EQ(grid.bottomNavStart, 27);

  for (int i = 0; i < 2; i++) {
    const L::Rect row = L::topStripRowRect(X4_W, i, ROW_H);
    const L::Hit h = hit(grid, X4_W / 2, row.y + ROW_H / 2, entryCount, grid.bookStart);
    EXPECT_EQ(h.kind, L::HitKind::TopNavRow) << "top strip row " << i;
    EXPECT_EQ(h.entryIndex, i) << "top strip row " << i;
  }
}

TEST(OpdsBrowserLayoutPageControls, NextPageRowIsTappableAtTheBottomOfTheGridPage) {
  const int books = 25;
  const L::Grid grid = bookGrid(books, 1, 1);
  const int entryCount = 1 + books + 1;
  const L::Rect row = L::bottomStripRowRect(grid, X4_W, X4_H, entryCount, 0, ROW_H);
  const L::Hit h = hit(grid, X4_W / 2, row.y + ROW_H / 2, entryCount, grid.bookStart);
  EXPECT_EQ(h.kind, L::HitKind::BottomNavRow);
  EXPECT_EQ(h.entryIndex, grid.bottomNavStart);
}

TEST(OpdsBrowserLayoutPageControls, BottomStripIsAnchoredAboveTheButtonHints) {
  const int books = 25;
  const L::Grid grid = bookGrid(books, 0, 1);
  const int entryCount = books + 1;
  const L::Rect row = L::bottomStripRowRect(grid, X4_W, X4_H, entryCount, 0, ROW_H);
  EXPECT_EQ(row.y + row.height, X4_H - L::BOTTOM_MARGIN);
}

TEST(OpdsBrowserLayoutPageControls, TwoBottomRowsStackUpwardsFromTheSameAnchor) {
  const int books = 25;
  const L::Grid grid = bookGrid(books, 0, 2);
  const int entryCount = books + 2;
  const L::Rect first = L::bottomStripRowRect(grid, X4_W, X4_H, entryCount, 0, ROW_H);
  const L::Rect second = L::bottomStripRowRect(grid, X4_W, X4_H, entryCount, 1, ROW_H);
  EXPECT_EQ(first.y + first.height, second.y);
  EXPECT_EQ(second.y + second.height, X4_H - L::BOTTOM_MARGIN);
}

TEST(OpdsBrowserLayoutPageControls, SelectionOnAStripRowKeepsTheGridOnItsFirstPage) {
  // The renderer shows grid page 0 whenever the selection is not on a cell (a
  // "Next Page" row names no grid page). The hit test has to agree, or tapping
  // a cover while the Next Page row is selected would open a book from a page
  // that is not on screen.
  const int books = 60;
  const L::Grid grid = bookGrid(books, 1, 1);
  const int entryCount = 1 + books + 1;
  EXPECT_EQ(L::gridLocalSelector(grid, grid.bottomNavStart), 0);
  EXPECT_EQ(L::gridLocalSelector(grid, 0), 0);  // the top strip's Search row
  EXPECT_EQ(L::gridLocalSelector(grid, grid.bookStart + 7), 7);

  const L::Rect cell = L::gridCellHitRect(grid, X4_W, ROW_H, CAPTION_H, 0);
  const L::Hit h = hit(grid, cell.x + 2, cell.y + 2, entryCount, grid.bottomNavStart);
  EXPECT_EQ(h.kind, L::HitKind::GridCell);
  EXPECT_EQ(h.entryIndex, grid.bookStart);
}

TEST(OpdsBrowserLayoutPageControls, PageControlsWinOverASlopExpandedCell) {
  // With touch slop on, a bottom-row cover's hit zone grows into the gutter
  // beneath it. Stealing the "Next Page" control would strand the user on the
  // page, so the strips are tested first, with exact bounds.
  const int books = 25;
  const L::Grid grid = bookGrid(books, 0, 1);
  const int entryCount = books + 1;
  const L::Rect row = L::bottomStripRowRect(grid, X4_W, X4_H, entryCount, 0, ROW_H);
  const L::Hit h = hit(grid, X4_W / 2, row.y, entryCount, grid.bookStart, L::GUTTER / 2);
  EXPECT_EQ(h.kind, L::HitKind::BottomNavRow);
}

// ---------------------------------------------------------------------------
// Boundary taps
// ---------------------------------------------------------------------------

TEST(OpdsBrowserLayoutBoundary, ListRowIncludesItsFirstPixelAndExcludesTheNextRowsFirst) {
  const L::Grid grid = landingGrid();
  const L::Rect row = L::listRowRect(X4_W, 3, ROW_H);
  EXPECT_EQ(hit(grid, X4_W / 2, row.y, LANDING_ENTRIES, 0).entryIndex, 3);
  EXPECT_EQ(hit(grid, X4_W / 2, row.y + row.height - 1, LANDING_ENTRIES, 0).entryIndex, 3);
  EXPECT_EQ(hit(grid, X4_W / 2, row.y + row.height, LANDING_ENTRIES, 0).entryIndex, 4);
}

TEST(OpdsBrowserLayoutBoundary, GridCellIncludesItsTopLeftPixelAndExcludesItsBottomRight) {
  const L::Grid grid = bookGrid(25, 0, 0);
  const L::Rect cell = L::gridCellHitRect(grid, X4_W, ROW_H, CAPTION_H, 0);
  EXPECT_EQ(hit(grid, cell.x, cell.y, 25, 0).slot, 0);
  EXPECT_EQ(hit(grid, cell.x + cell.width - 1, cell.y + cell.height - 1, 25, 0).slot, 0);
  // One past the cover's right edge is gutter, and with no slop that is a miss.
  EXPECT_EQ(hit(grid, cell.x + grid.coverWidth, cell.y + 1, 25, 0).kind, L::HitKind::None);
}

TEST(OpdsBrowserLayoutBoundary, GutterIsADeadZoneWithoutSlopAndBridgedWithIt) {
  const L::Grid grid = bookGrid(25, 0, 0);
  const L::Rect first = L::gridCoverRect(grid, X4_W, ROW_H, CAPTION_H, 0);
  const int gutterMidX = first.x + grid.coverWidth + L::GUTTER / 2;
  const int cellMidY = first.y + grid.coverHeight / 2;
  EXPECT_EQ(hit(grid, gutterMidX, cellMidY, 25, 0).kind, L::HitKind::None);
  const L::Hit slopped = hit(grid, gutterMidX, cellMidY, 25, 0, L::GUTTER / 2);
  EXPECT_EQ(slopped.kind, L::HitKind::GridCell);
  // Exactly halfway belongs to the neighbour on the far side (the lane's own
  // slop is a strict `<`, the next lane's is `<=`), which is the same split
  // AppsGridLayout::resolveGridLane makes.
  EXPECT_EQ(slopped.slot, 1);
}

TEST(OpdsBrowserLayoutBoundary, SlopNeverInventsAColumnPastTheLast) {
  const L::Grid grid = bookGrid(25, 0, 0);
  const L::Rect last = L::gridCoverRect(grid, X4_W, ROW_H, CAPTION_H, grid.columns - 1);
  const int pastRight = last.x + grid.coverWidth + L::GUTTER / 2 + 1;
  EXPECT_EQ(hit(grid, pastRight, last.y + 5, 25, 0, L::GUTTER / 2).kind, L::HitKind::None);
}

TEST(OpdsBrowserLayoutBoundary, TapLeftOfTheGridOriginMisses) {
  const L::Grid grid = bookGrid(25, 0, 0);
  const int originX = L::gridStartX(grid, X4_W);
  ASSERT_GT(originX, 0);
  EXPECT_EQ(hit(grid, originX - 1, L::gridTop(grid, ROW_H) + 5, 25, 0).kind, L::HitKind::None);
}

TEST(OpdsBrowserLayoutBoundary, TapAboveTheGridTopMisses) {
  const L::Grid grid = bookGrid(25, 0, 0);
  const L::Rect cell = L::gridCoverRect(grid, X4_W, ROW_H, CAPTION_H, 0);
  EXPECT_EQ(hit(grid, cell.x + 5, L::gridTop(grid, ROW_H) - 1, 25, 0).kind, L::HitKind::None);
}

TEST(OpdsBrowserLayoutBoundary, NegativeCoordinatesMissEverywhere) {
  const L::Grid list = landingGrid();
  const L::Grid grid = bookGrid(25, 1, 1);
  EXPECT_EQ(hit(list, -1, 100, LANDING_ENTRIES, 0).kind, L::HitKind::None);
  EXPECT_EQ(hit(list, 100, -1, LANDING_ENTRIES, 0).kind, L::HitKind::None);
  EXPECT_EQ(hit(grid, -5, -5, 27, 1, L::GUTTER / 2).kind, L::HitKind::None);
}

TEST(OpdsBrowserLayoutBoundary, ZeroRowHeightIsRejectedRatherThanDividingByZero) {
  const L::Grid grid = landingGrid();
  EXPECT_EQ(L::hitTest(10, 100, grid, X4_W, X4_H, 0, CAPTION_H, LANDING_ENTRIES, 0, 0).kind, L::HitKind::None);
}

// ---------------------------------------------------------------------------
// Render / hit-test agreement -- the invariant this header exists for
// ---------------------------------------------------------------------------

TEST(OpdsBrowserLayoutAgreement, EveryDrawnCellRectHitTestsBackToItsOwnSlot) {
  for (const auto& panel : std::vector<std::pair<int, int>>{{X4_W, X4_H}, {X3_W, X3_H}, {X4_H, X4_W}, {X3_H, X3_W}}) {
    const int w = panel.first;
    const int h = panel.second;
    const L::Grid grid = bookGrid(60, 1, 1, w, h);
    ASSERT_TRUE(grid.isGridPage) << w << "x" << h;
    const int entryCount = 1 + 60 + 1;
    for (int slot = 0; slot < grid.itemsPerPage; slot++) {
      const L::Rect cell = L::gridCellHitRect(grid, w, ROW_H, CAPTION_H, slot);
      const L::Hit h1 = L::hitTest(cell.x, cell.y, grid, w, h, ROW_H, CAPTION_H, entryCount, grid.bookStart, 0);
      const L::Hit h2 = L::hitTest(cell.x + cell.width - 1, cell.y + cell.height - 1, grid, w, h, ROW_H, CAPTION_H,
                                   entryCount, grid.bookStart, 0);
      EXPECT_EQ(h1.slot, slot) << w << "x" << h << " slot " << slot << " top-left";
      EXPECT_EQ(h2.slot, slot) << w << "x" << h << " slot " << slot << " bottom-right";
    }
  }
}

TEST(OpdsBrowserLayoutAgreement, EveryDrawnListRowHitTestsBackToItsOwnEntry) {
  for (const auto& panel : std::vector<std::pair<int, int>>{{X4_W, X4_H}, {X3_W, X3_H}, {X4_H, X4_W}}) {
    const int w = panel.first;
    const int h = panel.second;
    const int pageItems = L::listPageItems(h, ROW_H);
    const int entryCount = pageItems * 2;
    const L::Grid grid = L::computeGrid(w, h, ROW_H, CAPTION_H, entryCount, -1, -1);
    for (int k = 0; k < pageItems; k++) {
      const L::Rect row = L::listRowRect(w, k, ROW_H);
      EXPECT_EQ(L::hitTest(row.x, row.y, grid, w, h, ROW_H, CAPTION_H, entryCount, 0, 0).entryIndex, k)
          << w << "x" << h << " row " << k;
      EXPECT_EQ(
          L::hitTest(row.x + row.width - 1, row.y + row.height - 1, grid, w, h, ROW_H, CAPTION_H, entryCount, 0, 0)
              .entryIndex,
          k)
          << w << "x" << h << " row " << k;
    }
  }
}

// ---------------------------------------------------------------------------
// Landing row icons -- keyed on the href, never the (server-localised) title
// ---------------------------------------------------------------------------

namespace {
OpdsRowIcon::Kind iconFor(const char* href) {
  return OpdsRowIcon::classify(href, /*isSearchRow=*/false, /*isBook=*/false, /*isPageControl=*/false);
}
}  // namespace

TEST(OpdsRowIconClassify, SearchRowWinsOverEverythingElse) {
  EXPECT_EQ(OpdsRowIcon::classify("", true, false, false), OpdsRowIcon::Kind::Search);
}

TEST(OpdsRowIconClassify, PageControlsGetNoIcon) {
  EXPECT_EQ(OpdsRowIcon::classify("/opds/books?page=2", false, false, true), OpdsRowIcon::Kind::None);
}

TEST(OpdsRowIconClassify, LandingEntriesMapToTheirOwnIcons) {
  EXPECT_EQ(iconFor("/opds/books"), OpdsRowIcon::Kind::AllBooks);
  EXPECT_EQ(iconFor("/opds/recent"), OpdsRowIcon::Kind::Recent);
  EXPECT_EQ(iconFor("/opds/category/english"), OpdsRowIcon::Kind::Collection);
  EXPECT_EQ(iconFor("/opds/category/arabic"), OpdsRowIcon::Kind::Collection);
  EXPECT_EQ(iconFor("/opds/category/fiction/all"), OpdsRowIcon::Kind::AllBooks);
  EXPECT_EQ(iconFor("/opds/category/fiction/recent"), OpdsRowIcon::Kind::Recent);
}

TEST(OpdsRowIconClassify, AbsoluteUrlsQueryStringsAndTrailingSlashesAllMatch) {
  EXPECT_EQ(iconFor("http://midad.one/opds/recent"), OpdsRowIcon::Kind::Recent);
  EXPECT_EQ(iconFor("http://midad.one/opds/recent/"), OpdsRowIcon::Kind::Recent);
  EXPECT_EQ(iconFor("/opds/recent?page=3"), OpdsRowIcon::Kind::Recent);
  EXPECT_EQ(iconFor("/opds/books?page=12"), OpdsRowIcon::Kind::AllBooks);
}

TEST(OpdsRowIconClassify, UnrecognisedNavigationStillReadsAsAShelf) {
  // A row this firmware has never heard of is still something you can open.
  EXPECT_EQ(iconFor("/opds/whatever-the-server-adds-next"), OpdsRowIcon::Kind::Collection);
  EXPECT_EQ(iconFor(""), OpdsRowIcon::Kind::Collection);
}

TEST(OpdsRowIconClassify, ABookRowGetsTheBookIcon) {
  EXPECT_EQ(OpdsRowIcon::classify("/opds/books/935/download", false, true, false), OpdsRowIcon::Kind::Book);
}

TEST(OpdsBrowserLayoutAgreement, GridPageStartMatchesTheSlicingRenderUses) {
  const L::Grid grid = bookGrid(60, 0, 0);
  for (int book = 0; book < 60; book++) {
    const int pageStart = L::gridPageStart(book, grid.itemsPerPage);
    EXPECT_EQ(pageStart % grid.itemsPerPage, 0) << "book " << book;
    EXPECT_LE(pageStart, book);
    EXPECT_GT(pageStart + grid.itemsPerPage, book);
  }
}
