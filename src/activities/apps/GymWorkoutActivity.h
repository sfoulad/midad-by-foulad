#pragma once

#include <cstddef>

#include "GymPlanStore.h"
#include "activities/Activity.h"

// Perform-the-workout screen: walks through a day's exercises in order, one
// set at a time. Up/Down (physical side buttons) adjust weight, Left/Right
// (front buttons) adjust reps, Confirm logs the set and advances. Shows the
// exercise's image if it's already been downloaded (no network access is
// ever triggered from this screen -- see Phase 6 for the explicit "download
// missing assets" action; forcing a WiFi connect mid-workout would be a bad
// interruption, so this screen is offline-safe and just falls back to a
// text-only view when an image isn't cached yet).
class GymWorkoutActivity final : public Activity {
 public:
  explicit GymWorkoutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t dayIndex);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Mid-workout, an auto-sleep would strand the user needing to wake the
  // device just to keep logging sets.
  bool preventAutoSleep() override { return true; }
  const char* activityDebugName() const override { return "GymWorkout"; }

 private:
  enum State { EXERCISING, COMPLETE };

  static constexpr float WEIGHT_STEP_KG = 2.5f;

  size_t dayIndex_;
  State state_ = EXERCISING;
  size_t exerciseIndex_ = 0;
  int currentSet_ = 1;
  float weightKg_ = 0.0f;
  int reps_ = 0;
  bool hasImage_ = false;
  unsigned long sessionStartMs_ = 0;
  int setsLoggedThisSession_ = 0;

  const PlannedExercise* currentExercise() const;
  void beginExercise();
  void logSetAndAdvance();
  void promptEndWorkout();
  void finishWorkoutComplete();
  // Draws the header, set-progress dots, weight/reps, last-performance line,
  // and hints -- everything except the image itself, which render() decodes
  // and draws separately (only on the one-time initial render per exercise).
  void drawExercisingScreen(const PlannedExercise& ex);
};
