#include "GymPlanStore.h"

#include <algorithm>

void GymPlanStore::toJson(JsonDocument& doc) const {
  doc["currentDayIndex"] = currentDayIndex;
  JsonArray daysArr = doc["days"].to<JsonArray>();
  for (const auto& day : days) {
    JsonObject dayObj = daysArr.add<JsonObject>();
    dayObj["isRestDay"] = day.isRestDay;
    JsonArray exArr = dayObj["exercises"].to<JsonArray>();
    for (const auto& ex : day.exercises) {
      JsonObject exObj = exArr.add<JsonObject>();
      exObj["slug"] = ex.slug;
      exObj["name"] = ex.name;
      exObj["bodyPart"] = ex.bodyPart;
      exObj["targetSets"] = ex.targetSets;
      exObj["targetReps"] = ex.targetReps;
    }
  }
}

bool GymPlanStore::fromJson(JsonVariantConst doc) {
  for (auto& day : days) {
    day.exercises.clear();
  }
  currentDayIndex = static_cast<uint8_t>((doc["currentDayIndex"] | 0) % DAY_COUNT);

  JsonArrayConst daysArr = doc["days"].as<JsonArrayConst>();
  size_t dayIndex = 0;
  for (JsonObjectConst dayObj : daysArr) {
    if (dayIndex >= DAY_COUNT) break;
    days[dayIndex].isRestDay = dayObj["isRestDay"] | false;
    JsonArrayConst exArr = dayObj["exercises"].as<JsonArrayConst>();
    auto& exercises = days[dayIndex].exercises;
    exercises.reserve(std::min(exArr.size(), MAX_EXERCISES_PER_DAY));
    for (JsonObjectConst exObj : exArr) {
      if (exercises.size() >= MAX_EXERCISES_PER_DAY) break;
      PlannedExercise ex;
      ex.slug = exObj["slug"] | "";
      ex.name = exObj["name"] | "";
      ex.bodyPart = exObj["bodyPart"] | "";
      ex.targetSets = exObj["targetSets"] | 3;
      ex.targetReps = exObj["targetReps"] | 10;
      exercises.push_back(std::move(ex));
    }
    ++dayIndex;
  }
  return true;
}

bool GymPlanStore::addExerciseToDay(const size_t dayIndex, const PlannedExercise& exercise) {
  auto& day = days[dayIndex % DAY_COUNT];
  if (day.exercises.size() >= MAX_EXERCISES_PER_DAY) return false;
  day.exercises.push_back(exercise);
  day.isRestDay = false;
  saveToFile();
  return true;
}

bool GymPlanStore::toggleRestDay(const size_t dayIndex) {
  auto& day = days[dayIndex % DAY_COUNT];
  if (!day.isRestDay && !day.exercises.empty()) return false;
  day.isRestDay = !day.isRestDay;
  saveToFile();
  return true;
}

void GymPlanStore::removeExerciseFromDay(const size_t dayIndex, const size_t exerciseIndex) {
  auto& exercises = days[dayIndex % DAY_COUNT].exercises;
  if (exerciseIndex >= exercises.size()) return;
  exercises.erase(exercises.begin() + static_cast<std::ptrdiff_t>(exerciseIndex));
  saveToFile();
}

void GymPlanStore::updateExerciseTargets(const size_t dayIndex, const size_t exerciseIndex, uint8_t targetSets,
                                         uint8_t targetReps) {
  auto& exercises = days[dayIndex % DAY_COUNT].exercises;
  if (exerciseIndex >= exercises.size()) return;
  exercises[exerciseIndex].targetSets = std::clamp<uint8_t>(targetSets, 1, 20);
  exercises[exerciseIndex].targetReps = std::clamp<uint8_t>(targetReps, 1, 100);
  saveToFile();
}
