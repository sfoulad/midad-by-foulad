#pragma once

#include <string>

#include "activities/Activity.h"

// Downloads the image + instructions for every exercise currently assigned
// to ANY of the 7 days, skipping ones already cached locally. Deliberately a
// separate, explicit, user-triggered action (long-press Confirm from
// GymActivity) rather than something GymWorkoutActivity or
// GymExerciseBrowserActivity triggers automatically -- forcing a WiFi connect
// at add-exercise or mid-workout time would be a disruptive interruption; the
// user syncs assets deliberately, same as Font/Dictionary "Manage" screens.
#ifndef FOULAD_GYM_IMAGE_BASE_URL
#define FOULAD_GYM_IMAGE_BASE_URL "http://foulad.one/api/gym/image/"
#endif
#ifndef FOULAD_GYM_EXERCISE_BASE_URL
#define FOULAD_GYM_EXERCISE_BASE_URL "http://foulad.one/api/gym/exercise/"
#endif

class GymAssetSyncActivity final : public Activity {
 public:
  explicit GymAssetSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == SYNCING; }
  const char* activityDebugName() const override { return "GymAssetSync"; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    SYNCING,
    COMPLETE,
    ERROR,
  };

  State state_ = WIFI_SELECTION;
  int downloadedCount_ = 0;
  int totalMissing_ = 0;
  std::string errorMessage_;

  void onWifiSelectionComplete(bool success);
  void syncMissingAssets();
};
