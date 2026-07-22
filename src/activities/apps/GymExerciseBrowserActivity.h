#pragma once

#include <cstddef>
#include <deque>
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
// Reads the catalog fresh from SD each time, but never all of it at once:
// onEnter() only loads distinct body-part names + counts
// (GymCatalog::loadBodyPartCounts, bounded to ~17 entries), and selecting a
// body part loads just that subset (GymCatalog::loadExercisesForBodyPart,
// worst case ~148 entries for Quadriceps). An earlier version loaded the
// full ~873-entry catalog into one container up front -- first std::vector,
// then std::deque -- and BOTH failed with a real device crash (operator new
// throwing bad_alloc from the container's own internal growth, against a
// fragmented real-hardware heap the simulator's flat 1MB heap never
// reproduces). Nothing here is a persistent in-RAM catalog store; state is
// held only for this activity's lifetime.
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

  std::vector<std::string> bodyParts_;  // distinct values actually present in the catalog
  std::vector<int> bodyPartCounts_;     // exercise count per bodyParts_[i], parallel array
  // Only the selected body part's exercises -- never the full catalog (see
  // class comment). deque, not vector, so even this bounded subset never
  // needs one large contiguous allocation.
  std::deque<GymCatalogEntry> currentBodyPartExercises_;

  int bodyPartIndex_ = 0;
  int exerciseIndex_ = 0;

  void filterByBodyPart(const std::string& bodyPart);
  void addSelectedExercise();
};
