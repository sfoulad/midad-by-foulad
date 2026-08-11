#include "MidadAppSettings.h"

#include <Logging.h>

#include "CrossPointSettings.h"

namespace {
uint8_t clampToggle(uint8_t val, uint8_t fieldDefault) { return val < 2 ? val : fieldDefault; }
}  // namespace

void MidadAppSettings::toJson(JsonDocument& doc) const {
  doc["quranEnabled"] = quranEnabled;
  doc["rssEnabled"] = rssEnabled;
  doc["gamesEnabled"] = gamesEnabled;
  doc["tasbihEnabled"] = tasbihEnabled;
  doc["stopwatchEnabled"] = stopwatchEnabled;
  doc["pomodoroEnabled"] = pomodoroEnabled;
  doc["pomodoroFocusMin"] = pomodoroFocusMin;
  doc["pomodoroShortBreakMin"] = pomodoroShortBreakMin;
  doc["pomodoroLongBreakMin"] = pomodoroLongBreakMin;
  doc["gymEnabled"] = gymEnabled;
  doc["gymWeightUnit"] = gymWeightUnit;
  doc["debugLoggingEnabled"] = debugLoggingEnabled;
}

bool MidadAppSettings::fromJson(JsonVariantConst doc) {
  quranEnabled = clampToggle(doc["quranEnabled"] | quranEnabled, quranEnabled);
  rssEnabled = clampToggle(doc["rssEnabled"] | rssEnabled, rssEnabled);
  gamesEnabled = clampToggle(doc["gamesEnabled"] | gamesEnabled, gamesEnabled);
  tasbihEnabled = clampToggle(doc["tasbihEnabled"] | tasbihEnabled, tasbihEnabled);
  stopwatchEnabled = clampToggle(doc["stopwatchEnabled"] | stopwatchEnabled, stopwatchEnabled);
  pomodoroEnabled = clampToggle(doc["pomodoroEnabled"] | pomodoroEnabled, pomodoroEnabled);

  // Value fields -- clamp to the same ranges SettingsList.h's DynamicValue rows
  // enforce, since the bidirectional web/BLE settings sync can write anything.
  const uint8_t focus = doc["pomodoroFocusMin"] | pomodoroFocusMin;
  pomodoroFocusMin = (focus >= 5 && focus <= 90) ? focus : pomodoroFocusMin;
  const uint8_t shortBreak = doc["pomodoroShortBreakMin"] | pomodoroShortBreakMin;
  pomodoroShortBreakMin = (shortBreak >= 1 && shortBreak <= 30) ? shortBreak : pomodoroShortBreakMin;
  const uint8_t longBreak = doc["pomodoroLongBreakMin"] | pomodoroLongBreakMin;
  pomodoroLongBreakMin = (longBreak >= 5 && longBreak <= 60) ? longBreak : pomodoroLongBreakMin;

  gymEnabled = clampToggle(doc["gymEnabled"] | gymEnabled, gymEnabled);
  const uint8_t weightUnit = doc["gymWeightUnit"] | gymWeightUnit;
  gymWeightUnit = weightUnit < GYM_WEIGHT_UNIT_COUNT ? weightUnit : gymWeightUnit;
  debugLoggingEnabled = clampToggle(doc["debugLoggingEnabled"] | debugLoggingEnabled, debugLoggingEnabled);

  return true;
}

bool MidadAppSettings::loadFromFile() {
  if (PersistableStore<MidadAppSettings>::loadFromFile()) {
    return true;
  }

  // No midad_apps.json yet -- either a fresh device, or an existing one
  // upgrading from a firmware where these fields still lived in
  // CrossPointSettings' settings.json (same field names, same JSON keys, see
  // docs/upstream-sync-architecture.md's Phase B). Seed from there once so an
  // existing user's toggle/pomodoro/gym choices survive the migration, then
  // write our own file so this branch is never taken again.
  JsonDocument legacyDoc;
  if (!readDocFromFile(CrossPointSettings::getFilePath(), legacyDoc)) {
    return false;  // genuinely fresh device -- struct defaults above stand
  }

  const bool migrated = fromJson(legacyDoc.as<JsonVariantConst>());
  if (migrated) {
    if (saveToFile()) {
      LOG_DBG("MAS", "Migrated Apps settings from settings.json");
    } else {
      LOG_ERR("MAS", "Failed to save migrated Apps settings");
    }
  }
  return migrated;
}
