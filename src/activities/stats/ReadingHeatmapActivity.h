#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Monthly reading heatmap (Stats -> Reading Heatmap), ported from cpr-vcodex's
// ReadingHeatmapActivity: month summary cards, a 6x7 calendar grid shaded by
// daily reading time, goal-met badges, and a shade legend. Left/Right move the
// selected day (crossing month edges), Up/Down switch months, Confirm opens the
// selected day's detail. Improvements over the original: days with any reading
// under 15m still shade (level 1), a weekday header row, and RTL mirroring.
class ReadingHeatmapActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int viewedYear = 0;
  unsigned viewedMonth = 0;
  uint32_t selectedDayOrdinal = 0;
  bool waitForConfirmRelease = false;

  void goToAdjacentMonth(int delta);
  void resetSelectedDay();
  void moveSelection(int delta);

 public:
  explicit ReadingHeatmapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingHeatmap", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
