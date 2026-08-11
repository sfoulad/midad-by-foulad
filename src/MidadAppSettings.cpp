#include "MidadAppSettings.h"

#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MidadAppSettingsLoadPlan.h"

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
  doc["bleEnabled"] = bleEnabled;
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
  bleEnabled = clampToggle(doc["bleEnabled"] | bleEnabled, bleEnabled);

  return true;
}

bool MidadAppSettings::loadFromFile() {
  // Decided up front via Storage.exists() rather than inferring "missing" from
  // PersistableStore::loadFromFile()'s plain bool return -- that return value
  // collapses "file doesn't exist" and "file exists but is corrupt/unparseable"
  // into the same false, and those two cases must NOT be treated the same:
  // see MidadAppSettingsLoadPlan.h.
  const bool ownFileExists = Storage.exists(getFilePath());
  const bool ownFileLoadedOk = ownFileExists && PersistableStore<MidadAppSettings>::loadFromFile();
  const bool legacyFileExists = Storage.exists(CrossPointSettings::getFilePath());

  switch (planMidadAppSettingsLoad(ownFileExists, ownFileLoadedOk, legacyFileExists)) {
    case MidadAppSettingsLoadPlan::UseOwnFile:
      return true;

    case MidadAppSettingsLoadPlan::ReportCorrupt:
      // readDocFromFile() already LOG_ERR'd the specific parse/read failure.
      // Do not fall back to legacy settings.json here: migration is a
      // one-time act, and resurrecting stale pre-migration values behind a
      // torn write would be silently wrong, not a safe recovery.
      LOG_ERR("MAS", "midad_apps.json exists but failed to load; not migrating from legacy settings.json");
      return false;

    case MidadAppSettingsLoadPlan::FreshDefaults:
      return false;  // genuinely fresh device -- struct defaults above stand

    case MidadAppSettingsLoadPlan::Migrate: {
      // No midad_apps.json yet, but CrossPointSettings' settings.json still
      // has these fields under the same names (see docs/upstream-sync-
      // architecture.md's Phase B). Seed from there once so an existing
      // user's toggle/pomodoro/gym choices survive the split, then write our
      // own file so this branch is never taken again.
      JsonDocument legacyDoc;
      if (!readDocFromFile(CrossPointSettings::getFilePath(), legacyDoc)) {
        // legacyFileExists was true a moment ago but the read failed anyway
        // (e.g. corrupt legacy file) -- fall back to defaults rather than
        // migrating from an empty document.
        return false;
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
  }
  return false;
}
