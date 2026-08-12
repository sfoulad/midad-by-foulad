#include "MidadSettingsList.h"

#include "MidadAppSettings.h"
#include "SettingsList.h"

void appendMidadAppSettings(std::vector<SettingInfo>& appsSettings) {
  // Quran toggle: SettingsActivity::toggleCurrentSetting() extracts the
  // firmware-embedded EPUB to SD when this turns on (QuranBook::ensureExtracted).
  appsSettings.push_back(SettingInfo::DynamicToggle(
      StrId::STR_QURAN, [] { return MIDAD_APP_SETTINGS.quranEnabled; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.quranEnabled = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      "quranEnabled", StrId::STR_CAT_APPS));
  // Games toggle: pins a "Games" tile in My Books that opens a Snake/Tetris
  // picker. No extraction step needed -- unlike Quran, nothing but the
  // toggle itself is required.
  appsSettings.push_back(SettingInfo::DynamicToggle(
      StrId::STR_GAMES, [] { return MIDAD_APP_SETTINGS.gamesEnabled; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.gamesEnabled = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      "gamesEnabled", StrId::STR_CAT_APPS));
  // Tasbih toggle: pins a "Tasbih" tile in My Books that opens the built-in
  // dhikr counter. No extraction step needed, same as Games.
  appsSettings.push_back(SettingInfo::DynamicToggle(
      StrId::STR_TASBIH, [] { return MIDAD_APP_SETTINGS.tasbihEnabled; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.tasbihEnabled = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      "tasbihEnabled", StrId::STR_CAT_APPS));
  // News toggle: pins a "News" tile in My Books that browses the account's feed
  // subscriptions (EINK_NEWS_TASKS.md). Off by default, and the Midad app can turn
  // it on remotely -- subscriptions are managed there, never here, because adding
  // one means typing a URL on an on-screen keyboard.
  // News toggle removed with the tile -- see AppsActivity::entries(). The
  // rssEnabled field itself stays (settings.json + web-sync compatibility).
  // Stop Watch toggle: pins a "Stop Watch" tile in My Books that opens the
  // built-in stopwatch. No extraction step needed, same as Games.
  appsSettings.push_back(SettingInfo::DynamicToggle(
      StrId::STR_STOPWATCH, [] { return MIDAD_APP_SETTINGS.stopwatchEnabled; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.stopwatchEnabled = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      "stopwatchEnabled", StrId::STR_CAT_APPS));
  // Pomodoro toggle: pins a "Pomodoro" tile in My Books opening
  // StopwatchActivity in Pomodoro mode. The three durations below sit
  // directly under it so the group reads as one feature. Ranges cover the
  // common variants -- 25/5/15 classic, 52/17, 90-minute deep work -- and
  // start above 0, since a zero-length phase would expire instantly.
  appsSettings.push_back(SettingInfo::DynamicToggle(
      StrId::STR_POMODORO, [] { return MIDAD_APP_SETTINGS.pomodoroEnabled; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.pomodoroEnabled = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      "pomodoroEnabled", StrId::STR_CAT_APPS));
  appsSettings.push_back(SettingInfo::DynamicValue(
      StrId::STR_POMODORO_FOCUS_MIN, [] { return MIDAD_APP_SETTINGS.pomodoroFocusMin; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.pomodoroFocusMin = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      {5, 90, 5}, "pomodoroFocusMin", StrId::STR_CAT_APPS));
  appsSettings.push_back(SettingInfo::DynamicValue(
      StrId::STR_POMODORO_SHORT_BREAK_MIN, [] { return MIDAD_APP_SETTINGS.pomodoroShortBreakMin; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.pomodoroShortBreakMin = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      {1, 30, 1}, "pomodoroShortBreakMin", StrId::STR_CAT_APPS));
  appsSettings.push_back(SettingInfo::DynamicValue(
      StrId::STR_POMODORO_LONG_BREAK_MIN, [] { return MIDAD_APP_SETTINGS.pomodoroLongBreakMin; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.pomodoroLongBreakMin = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      {5, 60, 5}, "pomodoroLongBreakMin", StrId::STR_CAT_APPS));
  // Gym toggle: pins a "Gym" tile in My Books that opens the built-in
  // workout planner.
  appsSettings.push_back(SettingInfo::DynamicToggle(
      StrId::STR_GYM, [] { return MIDAD_APP_SETTINGS.gymEnabled; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.gymEnabled = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      "gymEnabled", StrId::STR_CAT_APPS));
  appsSettings.push_back(SettingInfo::DynamicEnum(
      StrId::STR_GYM_WEIGHT_UNIT, {StrId::STR_GYM_UNIT_KG, StrId::STR_GYM_UNIT_LB},
      [] { return MIDAD_APP_SETTINGS.gymWeightUnit; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.gymWeightUnit = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      "gymWeightUnit", StrId::STR_CAT_APPS));
  // Midad BLE used to also be registered here as a persisted Settings->Apps toggle
  // (see MidadAppSettings.h's own removal comment) -- removed along with the field
  // itself under BLE-R2 correction 2. Opening BluetoothActivity (Apps tile or
  // Home's hold-Confirm shortcut) is now the only way to turn BLE on, and there's
  // nothing left to persist.
  // Gates whether the rolling SD diagnostic logs get written at all -- see
  // MidadAppSettings::debugLoggingEnabled and src/util/DebugLogging.h for the
  // full list and rationale. Last in this function by design (user request):
  // it should always land after KOReader Sync, which SettingsActivity appends
  // immediately before calling appendMidadAppSettings().
  appsSettings.push_back(SettingInfo::DynamicToggle(
      StrId::STR_DEBUG_LOGGING, [] { return MIDAD_APP_SETTINGS.debugLoggingEnabled; },
      [](uint8_t val) {
        MIDAD_APP_SETTINGS.debugLoggingEnabled = val;
        MIDAD_APP_SETTINGS.saveToFile();
      },
      "debugLoggingEnabled", StrId::STR_CAT_APPS));
}

std::vector<SettingInfo> getCombinedSettingsList(const SdCardFontRegistry* registry) {
  std::vector<SettingInfo> settings = getSettingsList(registry);
  appendMidadAppSettings(settings);
  return settings;
}
