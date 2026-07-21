#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "GymCatalog.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// "Add Exercise" flow: pick a body part, then an exercise within it. Adding
// an exercise keeps the user in the exercise list (same body part) rather
// than finishing back to GymDayDetailActivity -- building out a day usually
// means adding several exercises in a row, often from the same muscle group.
// Back from the exercise list returns to the body-part list; Back again
// exits to Day Detail. Two-step list navigation (not a tab bar) since there
// are 17 body-part categories -- too many to fit legibly as tabs, but a
// perfectly normal list either way.
//
// Reads the catalog fresh from SD each time (GymCatalog::loadFromSd), same
// DOM-parse-and-hold-for-this-activity's-lifetime convention as
// FontDownloadActivity/DictionaryDownloadActivity -- no persistent in-RAM
// catalog store.
class GymExerciseBrowserActivity final : public Activity {
 public:
  explicit GymExerciseBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t dayIndex);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "GymExerciseBrowser"; }

 private:
  enum State { BODY_PART_LIST, EXERCISE_LIST };

  ButtonNavigator buttonNavigator_;
  size_t dayIndex_;
  State state_ = BODY_PART_LIST;

  std::vector<GymCatalogEntry> allExercises_;
  std::vector<std::string> bodyParts_;      // distinct values actually present in the catalog
  std::vector<int> bodyPartCounts_;         // exercise count per bodyParts_[i], parallel array
  std::vector<size_t> filteredIndices_;     // indices into allExercises_ for the selected body part

  int bodyPartIndex_ = 0;
  int exerciseIndex_ = 0;

  void filterByBodyPart(const std::string& bodyPart);
  void addSelectedExercise();
};
