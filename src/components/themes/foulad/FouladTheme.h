#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// "Foulad" home theme: hero card for the current book (cover + title/author +
// progress bar + Read / Est. Left readouts from the reading stats), a Recents
// cover row with a +N count badge, and a bottom icon bar for the home menu.
// Everything outside the home screen inherits the Lyra look (the previous
// device default, restored by user request after Classic felt like a
// downgrade for Settings/lists).
namespace FouladMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeTopPadding = 10;
  v.homeCoverHeight = 300;      // aalu hero cover height
  v.homeCoverTileHeight = 640;  // status line + hero + Recents divider + thumb row
  v.homeRecentBooksCount = 4;   // books[0] = hero, books[1..3] = recents row
  // Same reasoning as Lyra3Covers: the selector walks all recent covers before the
  // menu, which only lines up when Continue Reading isn't ALSO a menu row.
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  return v;
}();
}  // namespace FouladMetrics

class FouladTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
};
