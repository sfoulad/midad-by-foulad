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

  // Apps -> Midad BLE: whether the BLE peripheral (phone control -- Wi-Fi
  // provisioning today, more in later phases; see docs/ble-module-tasks.md) is
  // allowed to advertise while the device is Idle. Defaults on: it only ever runs
  // in Idle state (home/menus/sleep screen, never while reading or on Wi-Fi), is
  // gated by its own heap floor and cool-down (BlePeripheralManager) regardless of
  // this setting, and the whole point of Phase 1 is a phone finding the device
  // without the user doing anything first -- an opt-in toggle would defeat that.
  // Unlike the other AppsActivity launcher tiles, this is a live radio switch,
  // not something to "open": AppsActivity special-cases it to flip this flag
  // directly rather than navigating anywhere. Not part of FouladDeviceTracking's
  // settings.push/report wire contract -- it never was, even before this field
  // lived here, so this move doesn't add new remote wire behavior.
  uint8_t bleEnabled = 1;
};

#define MIDAD_APP_SETTINGS MidadAppSettings::getInstance()
