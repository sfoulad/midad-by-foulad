#pragma once

#include <algorithm>
#include <cstdint>

// Pure calendar-grid geometry for ReadingHeatmapActivity, factored out of
// render()'s local layout math so touch hit-testing can share the exact same
// numbers (a tap always lands on the cell it visually overlaps) and so the
// geometry is host-testable without GfxRenderer/UITheme -- same convention as
// activities/apps/AppsGridLayout.h.
namespace ReadingHeatmapLayout {

constexpr int GRID_ROWS = 6;
constexpr int GRID_COLS = 7;
constexpr int GRID_CELLS = GRID_ROWS * GRID_COLS;
constexpr int SECTION_GAP = 10;
constexpr int MONTH_HEADER_HEIGHT = 34;
constexpr int SUMMARY_CARD_HEIGHT = 64;
constexpr int SUMMARY_CARD_GAP = 8;
constexpr int GRID_GAP = 6;
constexpr int WEEKDAY_ROW_HEIGHT = 24;
constexpr int LEGEND_HEIGHT = 30;

// Ordinal of the grid's top-left cell (the Monday on or before the 1st), in
// signed arithmetic. TimeUtils::getDayOrdinalForDate() clamps any date at or
// before the 1970-01-01 epoch to 0, so for January 1970 this start legitimately
// goes negative. Computing it in uint32_t wrapped through UINT32_MAX instead,
// which rendered garbage day numbers and, on a tap, opened a day-detail screen
// for a date millions of years out -- reachable on a device whose clock has
// never been set, since that reports January 1970.
inline int64_t gridStartOrdinal(const uint32_t firstDayOrdinal, const int firstWeekday) {
  return static_cast<int64_t>(firstDayOrdinal) - firstWeekday;
}

// Day ordinals are meaningful from 1 up: 0 is the codebase's "no date" sentinel
// (TimeUtils::getDateFromDayOrdinal() rejects it) and negatives are pre-epoch
// leading cells, which render blank and open nothing.
inline bool isValidDayOrdinal(const int64_t ordinal) { return ordinal >= 1; }

struct Grid {
  int gridTop;
  int cellWidth;
  int cellHeight;
};

// Mirrors render()'s summaryTop/weekdayTop/gridTop/cellWidth/cellHeight chain
// exactly -- contentTop is metrics.topPadding + headerHeight + verticalSpacing,
// buttonHintsHeight is metrics.buttonHintsHeight (the legend sits just above
// the button-hint row).
inline Grid computeGrid(const int pageWidth, const int pageHeight, const int contentTop, const int sidePadding,
                        const int buttonHintsHeight) {
  const int summaryTop = contentTop + MONTH_HEADER_HEIGHT + 4;
  const int weekdayTop = summaryTop + (SUMMARY_CARD_HEIGHT + SUMMARY_CARD_GAP) * 2 + SECTION_GAP;
  const int gridTop = weekdayTop + WEEKDAY_ROW_HEIGHT;
  const int legendTop = pageHeight - buttonHintsHeight - LEGEND_HEIGHT - 4;
  const int gridHeight = std::max(120, legendTop - gridTop - SECTION_GAP);
  const int cellWidth = (pageWidth - sidePadding * 2 - GRID_GAP * (GRID_COLS - 1)) / GRID_COLS;
  const int cellHeight = (gridHeight - GRID_GAP * (GRID_ROWS - 1)) / GRID_ROWS;
  return Grid{gridTop, cellWidth, cellHeight};
}

// Self-inverse: the same mapping renders a logical column to its visual X
// (render()) and resolves a tap's visual column back to a logical one
// (cellIndexAt() below) -- same idiom as AppsGridLayout::mirroredColumn.
inline int columnX(const Grid& grid, const int col, const int sidePadding, const bool rtl) {
  const int slot = rtl ? GRID_COLS - 1 - col : col;
  return sidePadding + slot * (grid.cellWidth + GRID_GAP);
}

// Resolves a tap to a grid cell index (0..GRID_CELLS-1, row-major), or -1 if
// the point is outside the grid (including in the inter-cell gaps -- no
// touch-slop widening here, unlike AppsGridLayout::hitTestTile, since
// calendar cells are already close to minimum comfortable touch size and
// slop would make adjacent-day mistaps more likely, not less).
inline int cellIndexAt(const Grid& grid, const int x, const int y, const int sidePadding, const bool rtl) {
  if (grid.cellWidth <= 0 || grid.cellHeight <= 0) return -1;
  const int relY = y - grid.gridTop;
  if (relY < 0) return -1;
  const int row = relY / (grid.cellHeight + GRID_GAP);
  if (row >= GRID_ROWS || relY % (grid.cellHeight + GRID_GAP) >= grid.cellHeight) return -1;

  // Invert columnX(): search the (small, fixed) column count for the slot the
  // tap's X falls within, then unmirror it back to a logical column.
  for (int visualCol = 0; visualCol < GRID_COLS; visualCol++) {
    const int cellX = sidePadding + visualCol * (grid.cellWidth + GRID_GAP);
    if (x >= cellX && x < cellX + grid.cellWidth) {
      const int logicalCol = rtl ? GRID_COLS - 1 - visualCol : visualCol;
      return row * GRID_COLS + logicalCol;
    }
  }
  return -1;
}

}  // namespace ReadingHeatmapLayout
