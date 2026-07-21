#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Gym app entry point: the 7-day workout overview, opened from the "Gym" tile
// in My Books (see GYM_PSEUDO_PATH in RecentBooksActivity.cpp). If no
// exercise catalog has been downloaded yet, shows a prompt to sync it
// (GymCatalogSyncActivity) instead of an empty day list -- the day/exercise
// data model works with zero catalog data (GymPlanStore caches exercise
// name/bodyPart directly), but adding NEW exercises needs the catalog.
//
// Two long-press actions, deliberately on different buttons so neither
// conflicts with day-list scrolling (which already claims Up/Down/Left/Right
// via ScrollNext/Previous):
//   - Long-press Confirm: toggle the SELECTED day as a Rest Day (only when
//     it has zero exercises -- a rest day is empty by definition).
//   - Long-press Back: opens GymAssetSyncActivity, downloading any missing
//     exercise images/instructions across all 7 days in one explicit pass
//     (a global action, not tied to the selected day).
class GymActivity final : public Activity {
 public:
  explicit GymActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "Gym"; }

 private:
  static constexpr unsigned long LONG_PRESS_MS = 1000;

  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;
  bool hasCatalog_ = false;
  bool longPressConfirmFired_ = false;
  bool longPressBackFired_ = false;

  void refreshCatalogStatus();
  void openSelectedDay();
  void launchCatalogSync();
  void launchAssetSync();
};
