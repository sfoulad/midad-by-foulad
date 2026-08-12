#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>

// Persisted state for the Apps screen (AppsActivity) and the diagnostic
// logging toggle -- Midad features with no CrossPoint upstream equivalent.
// Split out of CrossPointSettings/SettingsList.h so a `git merge` of
// upstream's develop never has to touch this file: see
// docs/upstream-sync-architecture.md's Phase B (Settings extraction).
//
// Every field here used to live on CrossPointSettings with the same name and
// the same JSON key -- see loadFromFile() for the one-time migration that
// carries an existing user's values over from settings.json the first time
// this store's own file doesn't exist yet.
class MidadAppSettings : public PersistableStore<MidadAppSettings> {
 private:
  MidadAppSettings() = default;
  ~MidadAppSettings() = default;

  friend class PersistableStore<MidadAppSettings>;

 public:
  static const char* getFilePath() { return "/.crosspoint/midad_apps.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Overridden to seed a first-time store from CrossPointSettings' legacy
  // keys before falling back to the struct defaults below -- see the .cpp.
  bool loadFromFile();

  // Settings -> Apps -> Quran: the firmware-embedded Quran EPUB is extracted
  // to SD and pinned as the first book in My Books (see QuranBook.h).
  uint8_t quranEnabled = 0;

  // Apps -> News: pins a synthetic "News" tile that browses /opds/news. The
  // tile itself was removed (News is app-only now -- see FouladEbooksConfig.h);
  // this field stays purely for settings.json/web-sync compatibility, mirroring
  // its prior life on CrossPointSettings.
  uint8_t rssEnabled = 1;

  // Settings -> Apps -> Games: pins a synthetic "Games" tile that opens a
  // picker for the built-in Snake/Tetris activities. No SD extraction needed.
  uint8_t gamesEnabled = 1;

  // Settings -> Apps -> Tasbih: pins a synthetic "Tasbih" tile that opens the
  // built-in dhikr counter (TasbihActivity). No SD extraction needed.
  uint8_t tasbihEnabled = 1;

  // Settings -> Apps -> Stop Watch: pins a synthetic "Stop Watch" tile that
  // opens the built-in stopwatch (StopwatchActivity).
  uint8_t stopwatchEnabled = 1;

  // Settings -> Apps -> Pomodoro: pins a synthetic "Pomodoro" tile that opens
  // StopwatchActivity with its Pomodoro mode preselected.
  uint8_t pomodoroEnabled = 1;

  // Phase lengths in minutes. Read live at each phase change by
  // ReaderPomodoro/StopwatchActivity rather than cached, so an edit mid-cycle
  // applies at the next phase instead of needing a restart. Ranges in
  // SettingsList.h exclude 0, but these persist to JSON that the bidirectional
  // web settings sync could write anything into, so consumers clamp a 0 to 1.
  uint8_t pomodoroFocusMin = 25;
  uint8_t pomodoroShortBreakMin = 5;
  uint8_t pomodoroLongBreakMin = 15;
  // Focus phases completed before a long break replaces the short one.
  static constexpr uint8_t POMODORO_CYCLES_BEFORE_LONG_BREAK = 4;

  // Settings -> Apps -> Gym: pins a synthetic "Gym" tile that opens the
  // built-in 7-day workout planner.
  uint8_t gymEnabled = 1;
  // Display unit for logged weights: 0 = kg, 1 = lb. Weight is always stored
  // internally in kg (GymLogStore) regardless of this setting.
  enum GYM_WEIGHT_UNIT { GYM_WEIGHT_KG = 0, GYM_WEIGHT_LB = 1, GYM_WEIGHT_UNIT_COUNT };
  uint8_t gymWeightUnit = GYM_WEIGHT_KG;

  // Settings -> Apps -> Debug: gates whether ANY diagnostic entries get
  // written to the single shared SD log (see src/util/DebugLogging.h). Off by
  // default. Does NOT gate crash_report.txt (HalSystem::checkPanic) -- that's
  // a one-shot panic capture on an actual crash, not routine verbose logging.
  uint8_t debugLoggingEnabled = 0;

  // Midad BLE used to have a persisted on/off field here (bleEnabled), read by
  // main.cpp's background bleAllowedNow gate so the radio could run unattended on
  // Home/Settings/etc. BLE-R2 correction 2 removed it: NimBLE's ~57-58KB resident
  // cost was competing with every other screen's own heap needs (root cause of a
  // real Settings-screen OOM abort), so BLE is now purely screen-scoped --
  // BluetoothActivity's onEnter()/onExit() request/release it directly via
  // BlePeripheralManager::setUserRequested(), nothing to persist. Confirmed via
  // audit before removal: this field was never part of FouladDeviceTracking's
  // settings.push/report wire contract, and the BLE settings.push command itself
  // isn't implemented yet -- its only consumer was this struct's own local SPIFFS
  // file, which degrades safely (unknown/missing JSON keys are silently ignored,
  // same as every other field here).
};

#define MIDAD_APP_SETTINGS MidadAppSettings::getInstance()
