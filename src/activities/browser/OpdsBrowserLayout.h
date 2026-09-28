#pragma once

#include <algorithm>

// Pure geometry and hit-testing for OpdsBookBrowserActivity's catalogue pages:
// the plain navigation list (the Library landing and every category page), the
// cover grid, and the Prev/Next Page nav strips above and below it.
//
// Same idiom as AppsGridLayout.h, StatsListLayout.h and ReadingHeatmapLayout.h:
// render() draws through these functions and loop() hit-tests through the same
// ones, so a tap can never land on something other than what is on screen. No
// GfxRenderer/MappedInputManager dependency, so it is exercised directly by a
// host gtest target -- OpdsBookBrowserActivity itself needs HAL, network and
// renderer mocks this harness does not have.
//
// Everything the renderer derives from font metrics or the panel (row height,
// title-caption height, screen size) is passed in rather than recomputed here;
// this header owns only the arithmetic that turns those into rectangles.
namespace OpdsBrowserLayout {

// Content band. CONTENT_TOP clears the centred header; BOTTOM_MARGIN reserves
// the button-hint strip.
constexpr int CONTENT_TOP = 60;
constexpr int BOTTOM_MARGIN = 40;
// Spacing between cover cells, and the gap the 4px selection ring lives in.
constexpr int GUTTER = 12;
// Decides the column count only -- the rendered cover size is derived from the
// real screen width so covers always fill it edge to edge (see computeGrid).
constexpr int MIN_CELL_WIDTH = 140;
constexpr float COVER_ASPECT = 1.5f;  // coverHeight = coverWidth * aspect
constexpr int TITLE_LINES = 2;        // caption below a cover wraps to this many lines
constexpr int TITLE_TOP_GAP = 4;      // gap between cover bottom and the first title line
// Gap between a non-empty top nav strip and the first grid row.
constexpr int TOP_STRIP_GAP = 10;
// Text inset for list rows; the row band itself spans the full screen width.
constexpr int LIST_TEXT_INSET = 20;
// Selection highlight starts this many px above the row's text top.
constexpr int LIST_HIGHLIGHT_LIFT = 2;

struct Rect {
  int x = 0, y = 0, width = 0, height = 0;
};

inline bool contains(const Rect& r, const int x, const int y) {
  return r.width > 0 && r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

// Total height reserved below a cover for its wrapped title caption.
// `captionLineHeight` is the worst-case line height across the scripts the
// caption may use, measured by the caller from live font metrics.
inline int titleHeight(const int captionLineHeight) { return TITLE_TOP_GAP + captionLineHeight * TITLE_LINES; }

// Page shape of the currently loaded feed. `firstGridIndex`/`lastGridIndex` are
// the first and last entry indices that earn a cover cell (books, and
// navigation entries carrying cover art); pass -1 for both when the feed has
// none, which makes this a pure navigation page rendered as a text list.
struct Grid {
  bool isGridPage = false;
  int topNavCount = 0;     // entries [0, topNavCount) -> nav strip above the grid
  int bookStart = 0;       // entries [bookStart, bookStart + bookCount) -> the grid
  int bookCount = 0;       //
  int bottomNavStart = 0;  // entries [bottomNavStart, entryCount) -> nav strip below
  int columns = 1;
  int itemsPerPage = 1;
  int coverWidth = 0;
  int coverHeight = 0;
};

// --- plain navigation list (the Library landing, and every category page) ----

inline int listPageItems(const int screenHeight, const int rowHeight) {
  if (rowHeight <= 0) return 1;
  return std::max(1, (screenHeight - CONTENT_TOP - BOTTOM_MARGIN) / rowHeight);
}

inline int listPageStart(const int selectorIndex, const int pageItems) {
  if (pageItems <= 0 || selectorIndex <= 0) return 0;
  return selectorIndex / pageItems * pageItems;
}

// Full-width band for one row of the plain list. `rowIndexOnPage` is 0-based
// within the visible page (entryIndex - listPageStart()).
inline Rect listRowRect(const int screenWidth, const int rowIndexOnPage, const int rowHeight) {
  return Rect{0, CONTENT_TOP + rowIndexOnPage * rowHeight, screenWidth, rowHeight};
}

// --- nav strips on a grid page ----------------------------------------------

// Row band for entry `index` of the strip above the grid (index < topNavCount).
inline Rect topStripRowRect(const int screenWidth, const int index, const int rowHeight) {
  return Rect{0, CONTENT_TOP + index * rowHeight, screenWidth, rowHeight};
}

inline int bottomStripCount(const Grid& grid, const int entryCount) {
  return std::max(0, entryCount - grid.bottomNavStart);
}

// First y of the strip below the grid; it is bottom-anchored above the button
// hints, so its top moves with how many rows it holds.
inline int bottomStripTop(const Grid& grid, const int screenHeight, const int entryCount, const int rowHeight) {
  return screenHeight - BOTTOM_MARGIN - bottomStripCount(grid, entryCount) * rowHeight;
}

// Row band for the `indexInStrip`-th (0-based) row of the bottom strip.
inline Rect bottomStripRowRect(const Grid& grid, const int screenWidth, const int screenHeight, const int entryCount,
                               const int indexInStrip, const int rowHeight) {
  return Rect{0, bottomStripTop(grid, screenHeight, entryCount, rowHeight) + indexInStrip * rowHeight, screenWidth,
              rowHeight};
}

// --- cover grid --------------------------------------------------------------

inline int gridStartX(const Grid& grid, const int screenWidth) {
  const int totalGridWidth = grid.columns * (grid.coverWidth + GUTTER) - GUTTER;
  return std::max(0, (screenWidth - totalGridWidth) / 2);
}

// Mirrors where the top nav strip leaves the render cursor.
inline int gridTop(const Grid& grid, const int rowHeight) {
  return CONTENT_TOP + grid.topNavCount * rowHeight + (grid.topNavCount > 0 ? TOP_STRIP_GAP : 0);
}

// Resolves a loaded feed into a page shape. Defined here, below gridTop() and
// bottomStripTop(), because the grid's row count is measured against the band
// those two leave it.
inline Grid computeGrid(const int screenWidth, const int screenHeight, const int rowHeight, const int captionHeight,
                        const int entryCount, const int firstGridIndex, const int lastGridIndex) {
  Grid grid;
  if (firstGridIndex < 0 || lastGridIndex < firstGridIndex || entryCount <= 0) return grid;

  grid.isGridPage = true;
  grid.topNavCount = firstGridIndex;
  grid.bookStart = firstGridIndex;
  grid.bookCount = lastGridIndex - firstGridIndex + 1;
  grid.bottomNavStart = lastGridIndex + 1;

  // Column count first, from a target minimum cell width; the cover size is
  // then derived to fill those columns, so cover size and column count can
  // never drift apart the way two independent fixed constants did.
  grid.columns = std::max(1, (screenWidth - GUTTER) / (MIN_CELL_WIDTH + GUTTER));
  grid.coverWidth = (screenWidth - GUTTER * (grid.columns + 1)) / grid.columns;
  grid.coverHeight = static_cast<int>(grid.coverWidth * COVER_ASPECT);

  // The grid gets the band actually left between the two nav strips, not the
  // whole content area: strip rows are drawn at the same rowHeight, and a
  // taller row (the touch boards' icon rows) would otherwise push the last
  // cover row down over the "Next Page" control.
  const int bandTop = gridTop(grid, rowHeight);
  const int bandBottom = bottomStripTop(grid, screenHeight, entryCount, rowHeight);
  const int gridRowPitch = grid.coverHeight + titleHeight(captionHeight) + GUTTER;
  const int rows = gridRowPitch > 0 ? (bandBottom - bandTop) / gridRowPitch : 0;
  // No complete row fits (a landscape touch board carrying Search + Prev Page
  // above and Next Page below can leave a band shorter than one cover cell).
  // Claiming a row anyway drew a cover across the bottom strip, and since
  // hitTest() gives the strips priority, a tap on the visible part of that
  // cover activated "Next Page" instead of opening the book. Falling back to
  // the plain list -- the same shape a coverless navigation feed already gets
  // -- is the one move that keeps render(), swipePage() and hitTest() in
  // agreement, because all three branch on this single isGridPage flag. It
  // also always terminates, which shrinking the cover to fit does not: a band
  // thinner than the caption plus gutter has no positive cover height to find.
  if (rows <= 0) return Grid{};
  grid.itemsPerPage = grid.columns * rows;
  return grid;
}

// First book index (relative to bookStart) of the page holding `localSelector`.
inline int gridPageStart(const int localSelector, const int itemsPerPage) {
  if (itemsPerPage <= 0 || localSelector <= 0) return 0;
  return (localSelector / itemsPerPage) * itemsPerPage;
}

// The selection's position within the grid, or 0 when the selection is not on a
// cell at all. A selection sitting on a nav-strip row (Search, Prev Page, Next
// Page) does not name a grid page, and the grid keeps showing its first page
// in that case -- so both the renderer and the hit test must resolve it to 0,
// not to whatever gridPageStart() would make of an out-of-grid index.
inline int gridLocalSelector(const Grid& grid, const int selectorIndex) {
  const int local = selectorIndex - grid.bookStart;
  return (local >= 0 && local < grid.bookCount) ? local : 0;
}

// How many cells the page starting at `pageStart` actually holds; the last page
// can be short.
inline int gridCellsOnPage(const Grid& grid, const int pageStart) {
  return std::max(0, std::min(grid.itemsPerPage, grid.bookCount - pageStart));
}

inline int gridRowsOnPage(const Grid& grid, const int cellsOnPage) {
  if (grid.columns <= 0 || cellsOnPage <= 0) return 0;
  return (cellsOnPage + grid.columns - 1) / grid.columns;
}

// Cover rectangle for cell `slot` (0-based position on the current page). The
// caption is drawn immediately below this rect; see gridCellHitRect() for the
// combined tappable area.
inline Rect gridCoverRect(const Grid& grid, const int screenWidth, const int rowHeight, const int captionHeight,
                          const int slot) {
  if (grid.columns <= 0) return Rect{};
  const int col = slot % grid.columns;
  const int row = slot / grid.columns;
  const int cellPitchY = grid.coverHeight + titleHeight(captionHeight) + GUTTER;
  return Rect{gridStartX(grid, screenWidth) + col * (grid.coverWidth + GUTTER),
              gridTop(grid, rowHeight) + row * cellPitchY, grid.coverWidth, grid.coverHeight};
}

// Cover plus its caption -- what a user sees as "the book", and therefore what
// a tap must land on.
inline Rect gridCellHitRect(const Grid& grid, const int screenWidth, const int rowHeight, const int captionHeight,
                            const int slot) {
  Rect r = gridCoverRect(grid, screenWidth, rowHeight, captionHeight, slot);
  r.height += titleHeight(captionHeight);
  return r;
}

// Resolves one axis of a tap: which lane a coordinate `rel` (relative to the
// grid origin) falls in, given each lane is `extent` px followed by a
// `step - extent` px gutter. `slop > 0` grows each lane into the surrounding
// gutter instead of leaving it a dead zone -- the larger-hit-target case for
// touch boards; button-only boards pass 0 and get exact cell bounds. Returns
// -1 for a miss. Same contract as AppsGridLayout::resolveGridLane.
inline int resolveGridLane(const int rel, const int step, const int extent, const int slop, const int laneCount) {
  if (rel < 0 || step <= 0 || laneCount <= 0) return -1;
  const int lane = rel / step;
  if (lane >= laneCount) return -1;
  const int posInLane = rel % step;
  if (posInLane < extent) return lane;
  if (slop <= 0) return -1;
  if (posInLane - extent < slop) return lane;                             // slop past this lane's own edge
  if (step - posInLane <= slop && lane + 1 < laneCount) return lane + 1;  // slop before the next lane
  return -1;
}

// --- unified hit test --------------------------------------------------------

enum class HitKind { None, ListRow, TopNavRow, GridCell, BottomNavRow };

struct Hit {
  HitKind kind = HitKind::None;
  // Index into the activity's `entries` vector -- what loop() assigns to
  // selectorIndex before activating, so touch and button selection converge on
  // exactly one code path.
  int entryIndex = -1;
  // Cell position on the current grid page; only meaningful for GridCell.
  int slot = -1;
};

// Hit-tests a tap against whatever the page currently shows.
//
// The nav strips are tested before the grid and always with exact bounds: a
// slop-expanded cell may reach into a strip row's band, and stealing the
// "Next Page" control is worse than a slightly smaller cover target.
//
// Columns are NOT mirrored under RTL here, because the renderer does not mirror
// them either (unlike the Apps launcher) -- OPDS cell order follows feed order
// left-to-right on every locale.
inline Hit hitTest(const int tapX, const int tapY, const Grid& grid, const int screenWidth, const int screenHeight,
                   const int rowHeight, const int captionHeight, const int entryCount, const int selectorIndex,
                   const int touchSlop = 0) {
  if (entryCount <= 0 || rowHeight <= 0) return Hit{};

  if (!grid.isGridPage) {
    const int pageItems = listPageItems(screenHeight, rowHeight);
    const int pageStart = listPageStart(selectorIndex, pageItems);
    const int rowsOnPage = std::min(pageItems, entryCount - pageStart);
    for (int k = 0; k < rowsOnPage; k++) {
      if (contains(listRowRect(screenWidth, k, rowHeight), tapX, tapY)) {
        return Hit{HitKind::ListRow, pageStart + k, -1};
      }
    }
    return Hit{};
  }

  for (int i = 0; i < grid.topNavCount; i++) {
    if (contains(topStripRowRect(screenWidth, i, rowHeight), tapX, tapY)) {
      return Hit{HitKind::TopNavRow, i, -1};
    }
  }

  const int bottomCount = bottomStripCount(grid, entryCount);
  for (int i = 0; i < bottomCount; i++) {
    if (contains(bottomStripRowRect(grid, screenWidth, screenHeight, entryCount, i, rowHeight), tapX, tapY)) {
      return Hit{HitKind::BottomNavRow, grid.bottomNavStart + i, -1};
    }
  }

  const int pageStart = gridPageStart(gridLocalSelector(grid, selectorIndex), grid.itemsPerPage);
  const int cellsOnPage = gridCellsOnPage(grid, pageStart);
  const int rowsOnPage = gridRowsOnPage(grid, cellsOnPage);
  if (cellsOnPage <= 0) return Hit{};

  const int cellHeight = grid.coverHeight + titleHeight(captionHeight);
  const int col = resolveGridLane(tapX - gridStartX(grid, screenWidth), grid.coverWidth + GUTTER, grid.coverWidth,
                                  touchSlop, grid.columns);
  const int row =
      resolveGridLane(tapY - gridTop(grid, rowHeight), cellHeight + GUTTER, cellHeight, touchSlop, rowsOnPage);
  if (col < 0 || row < 0) return Hit{};

  const int slot = row * grid.columns + col;
  if (slot >= cellsOnPage) return Hit{};
  return Hit{HitKind::GridCell, grid.bookStart + pageStart + slot, slot};
}

}  // namespace OpdsBrowserLayout
