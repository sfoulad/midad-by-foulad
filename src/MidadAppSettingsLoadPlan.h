#pragma once

// Pure decision logic for MidadAppSettings::loadFromFile()'s migration
// choice, split out of MidadAppSettings.cpp so it's host-testable without any
// HalStorage/ArduinoJson dependency -- see test/midad_app_settings_load_plan/.
// The actual file I/O lives in MidadAppSettings.cpp; this function only
// decides what to do given three already-known facts about its outcome.
enum class MidadAppSettingsLoadPlan {
  UseOwnFile,     // own file existed and loaded successfully
  ReportCorrupt,  // own file exists but failed to load -- must NOT migrate
  Migrate,        // own file doesn't exist, legacy settings.json does
  FreshDefaults,  // own file doesn't exist, neither does legacy -- new device
};

MidadAppSettingsLoadPlan planMidadAppSettingsLoad(bool ownFileExists, bool ownFileLoadedOk, bool legacyFileExists);
