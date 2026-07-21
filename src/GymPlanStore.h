#pragma once
#include <PersistableStore.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// A single exercise assigned to a workout day. name/bodyPart are cached here
// (not looked up from the downloaded catalog each time) so the day-detail
// screen never needs to DOM-parse the full ~500-800 entry catalog just to
// show the handful of exercises assigned to one day -- same reasoning as
// RecentBooksStore caching title/author instead of re-parsing the EPUB.
struct PlannedExercise {
  std::string slug;
  std::string name;
  std::string bodyPart;
  uint8_t targetSets = 3;
  uint8_t targetReps = 10;
};

struct WorkoutDay {
  std::vector<PlannedExercise> exercises;
  // A deliberate rest/break day -- distinct from "empty because nothing's
  // been added yet" so the UI can say "Rest Day" instead of nagging the user
  // to add exercises. Only meaningful while exercises is empty (see
  // GymPlanStore::toggleRestDay/addExerciseToDay's own comments for why).
  bool isRestDay = false;
};

// The user's 7-day workout split. NOT tied to the calendar -- a repeating
// Day 1..7 cycle (user's choice over a fixed Mon-Sun mapping), so it works for
// any split style (push/pull/legs, upper/lower, etc.) regardless of which
// real-world weekday the user actually trains on.
class GymPlanStore : public PersistableStore<GymPlanStore> {
 public:
  static constexpr size_t DAY_COUNT = 7;
  // Advisory cap per day -- keeps a single day's list on-screen without
  // scrolling machinery beyond GUI.drawList's own paging, and bounds the
  // plan file size regardless of how enthusiastically a user adds exercises.
  static constexpr size_t MAX_EXERCISES_PER_DAY = 20;

 private:
  std::array<WorkoutDay, DAY_COUNT> days;
  // 0-based index into days[] -- "today's" workout in the repeating cycle.
  // Advances (wrapping) when a workout session is completed, not on any
  // calendar/clock basis.
  uint8_t currentDayIndex = 0;

  GymPlanStore() = default;
  ~GymPlanStore() = default;

  friend class PersistableStore<GymPlanStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/gym_plan.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const WorkoutDay& getDay(size_t dayIndex) const { return days[dayIndex % DAY_COUNT]; }
  uint8_t getCurrentDayIndex() const { return currentDayIndex; }
  void advanceToNextDay() { currentDayIndex = static_cast<uint8_t>((currentDayIndex + 1) % DAY_COUNT); }

  // Returns false (no-op) if dayIndex's list is already at MAX_EXERCISES_PER_DAY.
  // Clears isRestDay on success -- adding an exercise to a day means it's no
  // longer a pure rest day, no separate toggle-off step needed.
  bool addExerciseToDay(size_t dayIndex, const PlannedExercise& exercise);
  // Flips isRestDay. Only allowed while the day has zero exercises (a rest
  // day is empty by definition) -- returns false (no-op) otherwise, so the
  // caller can tell the user to remove exercises first.
  bool toggleRestDay(size_t dayIndex);
  void removeExerciseFromDay(size_t dayIndex, size_t exerciseIndex);
  // Clamped to [1, 20] sets / [1, 100] reps -- both plainly nonsensical outside
  // that range and this avoids needing a popup error for a fat-fingered value.
  void updateExerciseTargets(size_t dayIndex, size_t exerciseIndex, uint8_t targetSets, uint8_t targetReps);
};

#define GYM_PLAN GymPlanStore::getInstance()
