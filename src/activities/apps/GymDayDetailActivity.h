#pragma once

#include <cstddef>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Exercises assigned to one day of the 7-day plan (opened from GymActivity).
// Two synthetic rows sit ahead of the real exercise list -- same technique
// FileBrowserActivity uses for its "File Transfer" row: kept out of
// GymPlanStore's data so remove/edit logic never special-cases a fake entry,
// callers just offset by rowOffset() into the real list.
//   - "Start Workout" (only shown when the day has >=1 exercise) -- index 0
//   - "Add Exercise" (always shown) -- index 0 or 1 depending on the above
//
// Two interaction modes on a real exercise row: short Confirm enters "adjust
// targets" mode for that row (Up/Down change target sets, Left/Right change
// target reps, Confirm/Back exits back to normal browsing -- values apply
// live via GymPlanStore, no separate save step); long-press Confirm prompts
// to remove it from the day.
class GymDayDetailActivity final : public Activity {
 public:
  explicit GymDayDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t dayIndex);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "GymDayDetail"; }

 private:
  static constexpr unsigned long LONG_PRESS_MS = 1000;

  ButtonNavigator buttonNavigator_;
  size_t dayIndex_;
  int selectedIndex_ = 0;
  bool editingTargets_ = false;
  bool longPressFired_ = false;

  bool hasStartRow() const;
  int rowOffset() const { return hasStartRow() ? 2 : 1; }

  void promptRemoveSelected();
  void addExercisePressed();
  void startWorkoutPressed();
};
