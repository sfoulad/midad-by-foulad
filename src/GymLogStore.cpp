#include "GymLogStore.h"

#include <ctime>

#include "util/TimeUtils.h"

void GymLogStore::toJson(JsonDocument& doc) const {
  doc["lifetimeSetsLogged"] = lifetimeSetsLogged;
  doc["lifetimeSessionsCompleted"] = lifetimeSessionsCompleted;
  JsonArray arr = doc["performances"].to<JsonArray>();
  for (const auto& p : performances) {
    JsonObject obj = arr.add<JsonObject>();
    obj["slug"] = p.slug;
    obj["lastWeightKg"] = p.lastWeightKg;
    obj["lastReps"] = p.lastReps;
    obj["lastPerformedAt"] = p.lastPerformedAt;
  }
}

bool GymLogStore::fromJson(JsonVariantConst doc) {
  lifetimeSetsLogged = doc["lifetimeSetsLogged"] | 0;
  lifetimeSessionsCompleted = doc["lifetimeSessionsCompleted"] | 0;

  performances.clear();
  JsonArrayConst arr = doc["performances"].as<JsonArrayConst>();
  performances.reserve(std::min(arr.size(), MAX_TRACKED_EXERCISES));
  for (JsonObjectConst obj : arr) {
    if (performances.size() >= MAX_TRACKED_EXERCISES) break;
    ExercisePerformance p;
    p.slug = obj["slug"] | "";
    p.lastWeightKg = obj["lastWeightKg"] | 0.0f;
    p.lastReps = obj["lastReps"] | 0;
    p.lastPerformedAt = obj["lastPerformedAt"] | 0;
    performances.push_back(std::move(p));
  }
  return true;
}

size_t GymLogStore::findIndex(const std::string& slug) const {
  for (size_t i = 0; i < performances.size(); i++) {
    if (performances[i].slug == slug) return i;
  }
  return performances.size();
}

const ExercisePerformance* GymLogStore::findPerformance(const std::string& slug) const {
  const size_t index = findIndex(slug);
  return index < performances.size() ? &performances[index] : nullptr;
}

void GymLogStore::recordSet(const std::string& slug, const float weightKg, const uint8_t reps) {
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  const uint32_t timestamp = (TimeUtils::isClockValid() && TimeUtils::isClockValid(now)) ? now : 0;

  size_t index = findIndex(slug);
  if (index >= performances.size()) {
    if (performances.size() >= MAX_TRACKED_EXERCISES) {
      // Evict the least-recently-performed tracked exercise -- its lifetime
      // contribution is already folded into lifetimeSetsLogged below, so
      // nothing is lost except the "last time" reference for that exercise.
      size_t oldest = 0;
      for (size_t i = 1; i < performances.size(); i++) {
        if (performances[i].lastPerformedAt < performances[oldest].lastPerformedAt) {
          oldest = i;
        }
      }
      performances.erase(performances.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    ExercisePerformance p;
    p.slug = slug;
    performances.insert(performances.begin(), std::move(p));
    index = 0;
  }

  performances[index].lastWeightKg = weightKg;
  performances[index].lastReps = reps;
  performances[index].lastPerformedAt = timestamp;
  ++lifetimeSetsLogged;
}

void GymLogStore::recordSessionCompleted() {
  ++lifetimeSessionsCompleted;
  saveToFile();
}
