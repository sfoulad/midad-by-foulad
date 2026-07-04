

#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

namespace Lyra3CoversMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverTileHeight = 300;
  v.homeRecentBooksCount = 3;
  // Explicitly false (not just inherited): HomeActivity's selectorIndex is shared
  // between the recent-cover strip and the menu-row highlight when this flag is on,
  // which only lines up correctly when homeRecentBooksCount == 1 (true for every
  // other theme). With 3 covers here, forcing this on would highlight the wrong
  // menu row while browsing covers 2/3.
  v.homeContinueReadingInMenu = false;
  return v;
}();
}  // namespace Lyra3CoversMetrics

class Lyra3CoversTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
};
