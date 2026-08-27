#pragma once

// Pure grid math for the Apps launcher's static 2-column x 3-row layout.
// Header-only and GfxRenderer/MappedInputManager-free (same convention as
// util/GridNav.h) so it can be exercised directly by a host gtest target --
// AppsActivity itself needs HAL/renderer mocks this test harness doesn't have.
namespace AppsGridLayout {

constexpr int COLUMNS = 2;
constexpr int ROWS = 3;
constexpr int ITEMS_PER_PAGE = COLUMNS * ROWS;

inline int pageCount(const int totalItems) {
  if (totalItems <= 0) return 1;
  return (totalItems + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
}

inline int pageIndexOf(const int selectorIndex) { return selectorIndex > 0 ? selectorIndex / ITEMS_PER_PAGE : 0; }

inline int pageStartOf(const int selectorIndex) { return pageIndexOf(selectorIndex) * ITEMS_PER_PAGE; }

inline int rowInPage(const int indexInPage) { return indexInPage / COLUMNS; }
inline int colInPage(const int indexInPage) { return indexInPage % COLUMNS; }

// Self-inverse: applying it twice returns the original column, so the same
// function maps logical->visual (render) and visual->logical (touch hit-test).
inline int mirroredColumn(const int col, const bool rtl) { return rtl ? COLUMNS - 1 - col : col; }

// Keeps a remembered selection valid if the app registry's size ever changes
// (defensive -- the registry is a fixed 8 entries today, so this floor/ceiling
// clamp is not a live-resize path, just a guard). Empty list selects index 0.
inline int clampSelection(const int previousIndex, const int totalItems) {
  if (totalItems <= 0) return 0;
  if (previousIndex < 0) return 0;
  if (previousIndex >= totalItems) return totalItems - 1;
  return previousIndex;
}

// Resolves one axis (row or column) of a tap: which lane index (0-based) a
// coordinate `rel` (already relative to the grid's origin) falls in, given
// each lane is `tileExtent` px wide/tall followed by a `step - tileExtent` px
// gutter. `touchSlop` > 0 grows each lane's hit zone into the surrounding
// gutter (split evenly with the neighboring lane, capped by whichever runs
// out first) instead of leaving the gutter a dead zone -- the "larger hit
// target" case for touch boards; button-only boards pass 0 and get the exact
// tile bounds. Returns -1 for a miss (dead zone, or past the last lane).
inline int resolveGridLane(const int rel, const int step, const int tileExtent, const int slop, const int laneCount) {
  if (rel < 0 || step <= 0) return -1;
  const int lane = rel / step;
  if (lane >= laneCount) return -1;
  const int posInLane = rel % step;
  if (posInLane < tileExtent) return lane;
  if (slop <= 0) return -1;
  if (posInLane - tileExtent < slop) return lane;                         // slop past this lane's own edge
  if (step - posInLane <= slop && lane + 1 < laneCount) return lane + 1;  // slop before the next lane
  return -1;
}

// Hit-tests a tap against the current page's fixed 2x3 grid. Geometry mirrors
// what render() computes: gridStartX/contentTop is the page's origin,
// tileWidth/tileHeight is one cell, gutter is the spacing between cells.
// touchSlop extends each tile's hit zone into the gutter (see
// resolveGridLane) -- pass 0 for exact-tile-bounds hit-testing, or up to
// gutter/2 to eliminate dead zones between tiles for touch input. Returns the
// tapped item's index-in-page (0..ITEMS_PER_PAGE-1), or -1 for a miss or a
// slot with no real item (the last page can hold fewer than ITEMS_PER_PAGE
// apps).
inline int hitTestTile(const int tapX, const int tapY, const int gridStartX, const int contentTop, const int tileWidth,
                       const int tileHeight, const int gutter, const bool rtl, const int itemsOnPage,
                       const int touchSlop = 0) {
  if (tileWidth <= 0 || tileHeight <= 0) return -1;
  const int relX = tapX - gridStartX;
  const int relY = tapY - contentTop;

  const int visualCol = resolveGridLane(relX, tileWidth + gutter, tileWidth, touchSlop, COLUMNS);
  const int row = resolveGridLane(relY, tileHeight + gutter, tileHeight, touchSlop, ROWS);
  if (visualCol < 0 || row < 0) return -1;

  const int col = mirroredColumn(visualCol, rtl);
  const int indexInPage = row * COLUMNS + col;
  return indexInPage < itemsOnPage ? indexInPage : -1;
}

}  // namespace AppsGridLayout
