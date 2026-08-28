#pragma once
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

// Reading statistics overview (Home -> Stats), modeled on cpr-vcodex's
// ReadingStatsActivity: six metric cards (streak / max streak / daily goal /
// total time / finished / started), a Reading Heatmap button, and a paged list
// of started books. Confirm on a book opens it in the reader; a long-press
// Confirm removes its stats entry. On touch boards: tapping the heatmap
// button or a book row does the same thing as Confirm on it; holding a book
// row does the same thing as a long-press Confirm (X4 Pro has no physical
// Confirm button, so this is the only way to reach removal there); swiping
// up/down pages the book list, same direction convention as scrolling any
// other list (up = forward).
class StatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  // 0 = heatmap button, 1..N = book rows.
  int selectedIndex = 0;
  bool waitForConfirmRelease = false;
  bool waitForBackRelease = false;

  void openSelectedEntry();
  void confirmRemoveSelectedBook();

  // Geometry shared between render() (drawing) and loop() (touch
  // hit-testing) so the two can never drift apart -- same idiom as
  // AppsActivity::computeGeometry() and ReadingHeatmapActivity::
  // computeGridGeometry().
  struct Layout {
    Rect heatmapRect;
    int contentTop;  // top of the current page's first book row
  };
  Layout computeLayout() const;

 public:
  explicit StatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Stats", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
