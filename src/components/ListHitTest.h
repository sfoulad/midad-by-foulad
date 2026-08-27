#pragma once

// Pure, renderer-independent hit-test for BaseTheme::drawList()'s
// page-relative row layout (selected row's page determines which page is on
// screen; rows stack top-to-bottom with no gap). BaseTheme::listIndexFromPoint()
// resolves rowHeight from the active theme metrics and delegates here, so the
// geometry math itself is host-testable without pulling in the graphics/HAL
// stack that the rest of BaseTheme.cpp depends on.
struct ListHitTestResult {
  bool hit = false;
  int index = -1;
};

inline ListHitTestResult listHitTest(int rectX, int rectY, int rectWidth, int rectHeight, int itemCount,
                                     int selectedIndex, int rowHeight, int x, int y) {
  if (itemCount <= 0 || rowHeight <= 0) return {};
  if (x < rectX || x >= rectX + rectWidth) return {};
  if (y < rectY) return {};

  const int pageItems = rectHeight / rowHeight > 0 ? rectHeight / rowHeight : 1;
  const int pageStartIndex = selectedIndex / pageItems * pageItems;
  const int remaining = itemCount - pageStartIndex;
  const int rowsOnPage = remaining < pageItems ? remaining : pageItems;

  const int row = (y - rectY) / rowHeight;
  if (row >= rowsOnPage) return {};

  return ListHitTestResult{true, pageStartIndex + row};
}
