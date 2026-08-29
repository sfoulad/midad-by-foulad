#include <gtest/gtest.h>

#include "activities/stats/ReadingHeatmapLayout.h"

// Coverage for ReadingHeatmapActivity's calendar-grid geometry: the pixel
// layout computeGrid() derives (shared between render() and touch
// hit-testing), and cellIndexAt()'s round-trip through columnX() in both LTR
// and RTL.

namespace {
using ReadingHeatmapLayout::GRID_COLS;
using ReadingHeatmapLayout::GRID_ROWS;

// Representative on-device dimensions (X4 Pro-ish); the geometry math doesn't
// depend on any particular board, so these are just plausible sample values.
constexpr int PAGE_WIDTH = 480;
constexpr int PAGE_HEIGHT = 800;
constexpr int CONTENT_TOP = 100;
constexpr int SIDE_PADDING = 16;
constexpr int BUTTON_HINTS_HEIGHT = 40;

ReadingHeatmapLayout::Grid sampleGrid() {
  return ReadingHeatmapLayout::computeGrid(PAGE_WIDTH, PAGE_HEIGHT, CONTENT_TOP, SIDE_PADDING, BUTTON_HINTS_HEIGHT);
}
}  // namespace

TEST(ReadingHeatmapLayoutGrid, ProducesPositiveCellDimensions) {
  const auto grid = sampleGrid();
  EXPECT_GT(grid.cellWidth, 0);
  EXPECT_GT(grid.cellHeight, 0);
  EXPECT_GT(grid.gridTop, CONTENT_TOP);
}

TEST(ReadingHeatmapLayoutGrid, WiderPageWidensEachCell) {
  const auto narrow =
      ReadingHeatmapLayout::computeGrid(400, PAGE_HEIGHT, CONTENT_TOP, SIDE_PADDING, BUTTON_HINTS_HEIGHT);
  const auto wide = ReadingHeatmapLayout::computeGrid(800, PAGE_HEIGHT, CONTENT_TOP, SIDE_PADDING, BUTTON_HINTS_HEIGHT);
  EXPECT_GT(wide.cellWidth, narrow.cellWidth);
}

TEST(ReadingHeatmapLayoutColumn, RtlReversesColumnOrder) {
  const auto grid = sampleGrid();
  // Column 0 in LTR sits at the grid's left edge; column 0 in RTL sits where
  // LTR's last column (GRID_COLS-1) would be, and vice versa.
  EXPECT_EQ(ReadingHeatmapLayout::columnX(grid, 0, SIDE_PADDING, false),
            ReadingHeatmapLayout::columnX(grid, GRID_COLS - 1, SIDE_PADDING, true));
  EXPECT_EQ(ReadingHeatmapLayout::columnX(grid, GRID_COLS - 1, SIDE_PADDING, false),
            ReadingHeatmapLayout::columnX(grid, 0, SIDE_PADDING, true));
}

TEST(ReadingHeatmapLayoutTouch, HitsEveryCellCenterLtr) {
  const auto grid = sampleGrid();
  for (int row = 0; row < GRID_ROWS; row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      const int x = ReadingHeatmapLayout::columnX(grid, col, SIDE_PADDING, false) + grid.cellWidth / 2;
      const int y = grid.gridTop + row * (grid.cellHeight + ReadingHeatmapLayout::GRID_GAP) + grid.cellHeight / 2;
      const int expected = row * GRID_COLS + col;
      EXPECT_EQ(ReadingHeatmapLayout::cellIndexAt(grid, x, y, SIDE_PADDING, false), expected)
          << "row=" << row << " col=" << col;
    }
  }
}

TEST(ReadingHeatmapLayoutTouch, HitsEveryCellCenterRtl) {
  const auto grid = sampleGrid();
  for (int row = 0; row < GRID_ROWS; row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      const int x = ReadingHeatmapLayout::columnX(grid, col, SIDE_PADDING, true) + grid.cellWidth / 2;
      const int y = grid.gridTop + row * (grid.cellHeight + ReadingHeatmapLayout::GRID_GAP) + grid.cellHeight / 2;
      const int expected = row * GRID_COLS + col;
      EXPECT_EQ(ReadingHeatmapLayout::cellIndexAt(grid, x, y, SIDE_PADDING, true), expected)
          << "row=" << row << " col=" << col;
    }
  }
}

TEST(ReadingHeatmapLayoutTouch, MissesAboveGrid) {
  const auto grid = sampleGrid();
  EXPECT_EQ(ReadingHeatmapLayout::cellIndexAt(grid, SIDE_PADDING + 5, grid.gridTop - 1, SIDE_PADDING, false), -1);
}

TEST(ReadingHeatmapLayoutTouch, MissesBelowLastRow) {
  const auto grid = sampleGrid();
  const int y = grid.gridTop + GRID_ROWS * (grid.cellHeight + ReadingHeatmapLayout::GRID_GAP) + 1;
  EXPECT_EQ(ReadingHeatmapLayout::cellIndexAt(grid, SIDE_PADDING + 5, y, SIDE_PADDING, false), -1);
}

TEST(ReadingHeatmapLayoutTouch, MissesTheGapBetweenRows) {
  const auto grid = sampleGrid();
  // Just past the first row's cell height, still short of the second row --
  // lands in the GRID_GAP dead zone.
  const int y = grid.gridTop + grid.cellHeight;
  if (ReadingHeatmapLayout::GRID_GAP <= 0) return;  // no gap to miss into
  EXPECT_EQ(ReadingHeatmapLayout::cellIndexAt(grid, SIDE_PADDING + 5, y, SIDE_PADDING, false), -1);
}

TEST(ReadingHeatmapLayoutTouch, MissesBeforeGridOrigin) {
  const auto grid = sampleGrid();
  EXPECT_EQ(ReadingHeatmapLayout::cellIndexAt(grid, -10, grid.gridTop + 5, SIDE_PADDING, false), -1);
}

TEST(ReadingHeatmapLayoutTouch, DegenerateGridNeverHits) {
  const ReadingHeatmapLayout::Grid empty{0, 0, 0};
  EXPECT_EQ(ReadingHeatmapLayout::cellIndexAt(empty, 0, 0, SIDE_PADDING, false), -1);
}
