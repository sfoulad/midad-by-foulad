#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Reading statistics overview (Home -> Stats), modeled on cpr-vcodex's
// ReadingStatsActivity: six metric cards (streak / max streak / daily goal /
// total time / finished / started), a Reading Heatmap button, and a paged list
// of started books. Confirm on a book opens it in the reader; a long-press
// Confirm removes its stats entry.
class StatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  // 0 = heatmap button, 1..N = book rows.
  int selectedIndex = 0;
  bool waitForConfirmRelease = false;
  bool waitForBackRelease = false;

  void openSelectedEntry();
  void confirmRemoveSelectedBook();

 public:
  explicit StatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Stats", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
