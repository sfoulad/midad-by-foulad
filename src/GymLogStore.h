#pragma once
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// Last-known weight/reps for one exercise, so the workout screen can show
// "last time: 60kg x 8" as a reference while logging a new set.
struct ExercisePerformance {
  std::string slug;
  float lastWeightKg = 0.0f;
  uint8_t lastReps = 0;
  uint32_t lastPerformedAt = 0;  // epoch seconds; 0 = clock was never valid when logged
};

// Workout history: per-exercise last performance (bounded, evictable -- like
// ReadingStatsStore's per-book tracking) PLUS lifetime totals that are
// incremented once at log time and never recomputed/decremented, so they
// can't regress when an old exercise gets evicted to make room for a new one.
// This mirrors the fix applied to ReadingStatsStore's getBooksFinishedCount()
// (see that store's own comment) -- applying the lesson up front here instead
// of shipping the same bug twice.
class GymLogStore : public PersistableStore<GymLogStore> {
 public:
  // Advisory cap: distinct exercises with a tracked "last performance" at
  // once. A real routine touches a few dozen exercises at most; this bounds
  // worst-case RAM/file size the same way ReadingStatsStore::MAX_BOOKS does.
  static constexpr size_t MAX_TRACKED_EXERCISES = 60;

 private:
  // Newest-touched first, like ReadingStatsStore::books.
  std::vector<ExercisePerformance> performances;
  uint32_t lifetimeSetsLogged = 0;         // persisted, never decremented
  uint32_t lifetimeSessionsCompleted = 0;  // persisted, never decremented

  GymLogStore() = default;
  ~GymLogStore() = default;

  friend class PersistableStore<GymLogStore>;

  size_t findIndex(const std::string& slug) const;

 public:
  static const char* getFilePath() { return "/.crosspoint/gym_log.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // nullptr if this exercise has never been logged.
  const ExercisePerformance* findPerformance(const std::string& slug) const;

  // Updates (or creates) this exercise's last-performance record, evicting
  // the least-recently-performed tracked exercise if at capacity. Also
  // increments the lifetime set counter. Does NOT save -- callers batch a
  // whole session's sets and call saveToFile() once (see GymWorkoutActivity).
  void recordSet(const std::string& slug, float weightKg, uint8_t reps);

  // Call once when a workout session is marked done (not per set).
  void recordSessionCompleted();

  uint32_t getLifetimeSetsLogged() const { return lifetimeSetsLogged; }
  uint32_t getLifetimeSessionsCompleted() const { return lifetimeSessionsCompleted; }
};

#define GYM_LOG GymLogStore::getInstance()
