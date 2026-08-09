#include "FouladDeviceTracking.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdlib>

#include "CrossPointSettings.h"
#include "FouladEbooksConfig.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "RecentBooksStore.h"
#include "network/HttpDownloader.h"
#include "network/OtaUpdater.h"
#include "reading/ReadingStatsStore.h"
#include "util/DebugLog.h"
#include "util/DebugLogging.h"
#include "util/RollingSdLog.h"

namespace FouladDeviceTracking {

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

namespace {
constexpr char TAG[] = "FDT";

std::string deviceEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/device"; }
std::string readingStatsEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/reading-stats"; }
std::string deviceLogEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/device-log"; }
std::string deviceStatsEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/device-stats"; }

// reportDeviceStats() skips the books/reading-days arrays this connect
// (device telemetry alone still sends) unless free heap comfortably covers
// BOTH the actual current data size AND this fixed margin for whatever OPDS
// browsing/WiFi/TLS is already holding -- scaled to the CURRENT book/day
// counts rather than the worst case (40 books + 750 days) once and for all,
// since a fixed worst-case threshold would never pass for a typical user
// with a handful of books, even though their real payload only needs a few
// KB. Per-entry estimates are deliberately generous (ArduinoJson's internal
// variant-slot overhead plus the final serialized string, per entry).
constexpr uint32_t READING_SNAPSHOT_SAFETY_MARGIN_BYTES = 40000;
constexpr uint32_t ESTIMATED_BYTES_PER_BOOK = 350;
constexpr uint32_t ESTIMATED_BYTES_PER_READING_DAY = 90;

// Last-reported READING_STATS total, to decide whether the (larger) reading
// snapshot needs resending -- device telemetry is cheap and always sent, but
// the reading snapshot is only worth another POST when something actually
// changed. Sentinel value ensures the first call after boot always sends it.
uint64_t lastReportedTotalReadingMs = UINT64_MAX;

// Matches the literal path HalSystem::checkPanic() writes to.
constexpr char CRASH_REPORT_PATH[] = "/crash_report.txt";

std::string modelName() { return gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4"; }
std::string displayName() { return std::string("Foulad eInk (") + (gpio.deviceIsX3() ? "X3" : "X4") + ")"; }

// Tagged into the shared on-SD debug log (see util/DebugLog.h) so a
// registration/reporting problem with the LIVE server can be diagnosed from
// the SD card, the same way OPDS browsing/downloading already can.
void diagLog(const std::string& line) { RollingSdLog::append(DebugLog::PATH, "[FDT] " + line, DebugLog::MAX_LINES); }

// Web (and on-device) Arabic font picker index -> actual stored
// ARABIC_FONT_FAMILY value. NOT a 1:1 mapping: AMIRI(=1) was dropped from
// flash and is skipped by both UIs (see SettingsList.h's
// buildArabicFontFamilySetting()/kSelectableFonts, which this must stay in
// sync with) -- picker index 1 ("Uthmani Hafs") is stored value 2, not 1.
// Applying the raw web index directly here would silently select the wrong
// font.
constexpr uint8_t kArabicFontDisplayToStored[] = {
    CrossPointSettings::NOTONASKHARABIC,  // 0: Noto Naskh Arabic
    CrossPointSettings::UTHMANICHAFS,     // 1: Uthmani Hafs
    CrossPointSettings::TAJAWAL,          // 2: Tajawal
};
constexpr uint8_t kArabicFontDisplayCount = sizeof(kArabicFontDisplayToStored) / sizeof(kArabicFontDisplayToStored[0]);

uint8_t arabicFontFamilyToDisplayIndex(const uint8_t stored) {
  for (uint8_t i = 0; i < kArabicFontDisplayCount; i++) {
    if (kArabicFontDisplayToStored[i] == stored) return i;
  }
  return 0;  // stale/legacy value (e.g. removed Amiri=1) or out of range
}

// Current settings, for the "My Devices" web editor's display only (EINK_SETTINGS_
// SYNC_TASKS.md PART 1) -- never affects what gets pushed back. Every key here must
// match CrossPointSettings'/KOReaderCredentialStore's own field name exactly; see
// applySettingsFromServer() below for the reverse direction and why arabicFontFamily
// needs translation but every other field doesn't.
void addSettingsReport(JsonObject settings) {
  const auto& s = SETTINGS;
  // Display
  settings["sleepScreen"] = s.sleepScreen;
  settings["sleepScreenCoverMode"] = s.sleepScreenCoverMode;
  settings["sleepScreenCoverFilter"] = s.sleepScreenCoverFilter;
  settings["quickResumeSleepScreen"] = s.quickResumeSleepScreen;
  settings["hideBatteryPercentage"] = s.hideBatteryPercentage;
  settings["refreshFrequency"] = s.refreshFrequency;
  settings["fadingFix"] = s.fadingFix;
  settings["darkModeEnabled"] = s.darkModeEnabled;
  // Reader
  settings["fontFamily"] = s.fontFamily;
  settings["fontSize"] = s.fontPointSize;
  settings["arabicFontFamily"] = arabicFontFamilyToDisplayIndex(s.arabicFontFamily);
  settings["arabicFontSize"] = s.arabicFontSize;
  settings["lineSpacing"] = s.lineSpacing;
  settings["screenMargin"] = s.screenMargin;
  settings["paragraphAlignment"] = s.paragraphAlignment;
  settings["orientation"] = s.orientation;
  settings["embeddedStyle"] = s.embeddedStyle;
  settings["focusReadingEnabled"] = s.focusReadingEnabled;
  settings["hyphenationEnabled"] = s.hyphenationEnabled;
  settings["extraParagraphSpacing"] = s.extraParagraphSpacing;
  settings["textAntiAliasing"] = s.textAntiAliasing;
  settings["imageRendering"] = s.imageRendering;
  settings["trackReadingStats"] = s.trackReadingStats;
  settings["dailyReadingGoal"] = s.dailyReadingGoal;
  // Controls
  settings["sideButtonLayout"] = s.sideButtonLayout;
  settings["frontButtonFollowOrientation"] = s.frontButtonFollowOrientation;
  settings["longPressButtonBehavior"] = s.longPressButtonBehavior;
  settings["longPressMenuFunction"] = s.longPressMenuFunction;
  settings["shortPwrBtn"] = s.shortPwrBtn;
  settings["pwrBtnFootnoteBack"] = s.pwrBtnFootnoteBack;
  settings["tiltPageTurn"] = s.tiltPageTurn;
  // Apps
  settings["quranEnabled"] = s.quranEnabled;
  settings["rssEnabled"] = s.rssEnabled;
  settings["gamesEnabled"] = s.gamesEnabled;
  settings["tasbihEnabled"] = s.tasbihEnabled;
  settings["stopwatchEnabled"] = s.stopwatchEnabled;
  settings["gymEnabled"] = s.gymEnabled;
  settings["gymWeightUnit"] = s.gymWeightUnit;
  settings["debugLoggingEnabled"] = s.debugLoggingEnabled;
  // System
  settings["sleepTimeoutMinutes"] = s.sleepTimeoutMinutes;
  settings["showHiddenFiles"] = s.showHiddenFiles;
  settings["removeReadBooksFromRecents"] = s.removeReadBooksFromRecents;
  settings["moveFinishedToReadFolder"] = s.moveFinishedToReadFolder;
  // Status Bar
  settings["statusBarChapterPageCount"] = s.statusBarChapterPageCount;
  settings["statusBarBookProgressPercentage"] = s.statusBarBookProgressPercentage;
  settings["statusBarProgressBar"] = s.statusBarProgressBar;
  settings["statusBarProgressBarThickness"] = s.statusBarProgressBarThickness;
  settings["statusBarTitle"] = s.statusBarTitle;
  settings["statusBarBattery"] = s.statusBarBattery;
  settings["xtcStatusBarMode"] = s.xtcStatusBarMode;
  settings["statusBarClock"] = s.statusBarClock;
  settings["clockUtcOffsetQ"] = s.clockUtcOffsetQ;
  settings["clockFormat"] = s.clockFormat;
  settings["clockHasBeenSynced"] = s.clockHasBeenSynced;
  // KOReader Sync -- never report koPassword (write-only from the web side).
  settings["koUsername"] = KOREADER_STORE.getUsername();
  settings["koServerUrl"] = KOREADER_STORE.getServerUrl();
  settings["koMatchMethod"] = static_cast<uint8_t>(KOREADER_STORE.getMatchMethod());
}

// Applies whatever the user configured via the "My Devices" web page (EINK_SETTINGS_
// SYNC_TASKS.md PART 2), from the same POST /opds/device response. `settings` is {}
// (no keys) until the user saves that page at least once -- every per-field lookup
// below then naturally misses and changes nothing, so no separate "is this empty"
// branch is needed. Unknown/out-of-range values are silently skipped, same tolerance
// as the server side. Persists via CrossPointSettings::saveToFile() (and
// KOReaderCredentialStore::saveToFile() for ko* keys) only if something actually
// changed -- mirrors this project's usual SPIFFS/SD write-throttling convention,
// even though this only runs once per Foulad eBooks connection, not per keystroke.
void applySettingsFromServer(JsonObjectConst settings) {
  if (settings.isNull()) return;

  auto& s = SETTINGS;
  using S = CrossPointSettings;
  bool changed = false;

  auto applyEnum = [&](uint8_t& field, const char* key, const uint8_t count) {
    const JsonVariantConst v = settings[key];
    if (v.isNull()) return;
    const uint8_t val = v.as<uint8_t>();
    if (val >= count) return;
    if (field != val) {
      field = val;
      changed = true;
    }
  };
  auto applyToggle = [&](uint8_t& field, const char* key) {
    const JsonVariantConst v = settings[key];
    if (v.isNull()) return;
    const uint8_t val = v.as<uint8_t>() != 0 ? 1 : 0;
    if (field != val) {
      field = val;
      changed = true;
    }
  };
  auto applyRange = [&](uint8_t& field, const char* key, const uint8_t lo, const uint8_t hi) {
    const JsonVariantConst v = settings[key];
    if (v.isNull()) return;
    const uint8_t val = v.as<uint8_t>();
    if (val < lo || val > hi) return;
    if (field != val) {
      field = val;
      changed = true;
    }
  };

  applyEnum(s.sleepScreen, "sleepScreen", S::SLEEP_SCREEN_MODE_COUNT);
  applyEnum(s.sleepScreenCoverMode, "sleepScreenCoverMode", S::SLEEP_SCREEN_COVER_MODE_COUNT);
  applyEnum(s.sleepScreenCoverFilter, "sleepScreenCoverFilter", S::SLEEP_SCREEN_COVER_FILTER_COUNT);
  applyEnum(s.quickResumeSleepScreen, "quickResumeSleepScreen", S::QUICK_RESUME_SLEEP_SCREEN_COUNT);
  applyEnum(s.hideBatteryPercentage, "hideBatteryPercentage", S::HIDE_BATTERY_PERCENTAGE_COUNT);
  applyEnum(s.refreshFrequency, "refreshFrequency", S::REFRESH_FREQUENCY_COUNT);
  applyToggle(s.fadingFix, "fadingFix");
  applyToggle(s.darkModeEnabled, "darkModeEnabled");

  // fontFamily/arabicFontFamily: the web only offers built-in options, which must
  // take priority over any SD font currently selected or the pushed change would
  // appear to silently do nothing (SD name wins when non-empty) -- clearing it
  // mirrors SettingsList.h's own valueSetter for these two fields.
  {
    const JsonVariantConst v = settings["fontFamily"];
    if (!v.isNull()) {
      const uint8_t val = v.as<uint8_t>();
      if (val < S::FONT_FAMILY_COUNT && (s.fontFamily != val || s.sdFontFamilyName[0] != '\0')) {
        s.fontFamily = val;
        s.sdFontFamilyName[0] = '\0';
        changed = true;
      }
    }
  }
  {
    const JsonVariantConst v = settings["arabicFontFamily"];
    if (!v.isNull()) {
      const uint8_t displayIdx = v.as<uint8_t>();
      if (displayIdx < kArabicFontDisplayCount) {
        const uint8_t val = kArabicFontDisplayToStored[displayIdx];
        if (s.arabicFontFamily != val || s.sdArabicFontFamilyName[0] != '\0') {
          s.arabicFontFamily = val;
          s.sdArabicFontFamilyName[0] = '\0';
          changed = true;
        }
      }
    }
  }

  // fontSize (wire key, unchanged) is now a point size, not a bounded enum slot --
  // an out-of-range/unavailable value is made safe at load/render time by
  // snapToNearestPointSize() (see SdCardFontSystem::ensureLoaded), so a plain
  // sanity range is enough here rather than an exact enum-count bound.
  applyRange(s.fontPointSize, "fontSize", 6, 72);
  applyEnum(s.arabicFontSize, "arabicFontSize", S::FONT_SIZE_COUNT);
  applyEnum(s.lineSpacing, "lineSpacing", S::LINE_COMPRESSION_COUNT);
  applyRange(s.screenMargin, "screenMargin", 5, 40);
  applyEnum(s.paragraphAlignment, "paragraphAlignment", S::PARAGRAPH_ALIGNMENT_COUNT);
  applyEnum(s.orientation, "orientation", S::ORIENTATION_COUNT);
  applyToggle(s.embeddedStyle, "embeddedStyle");
  applyToggle(s.focusReadingEnabled, "focusReadingEnabled");
  applyToggle(s.hyphenationEnabled, "hyphenationEnabled");
  applyToggle(s.extraParagraphSpacing, "extraParagraphSpacing");
  applyToggle(s.textAntiAliasing, "textAntiAliasing");
  applyEnum(s.imageRendering, "imageRendering", S::IMAGE_RENDERING_COUNT);
  applyToggle(s.trackReadingStats, "trackReadingStats");
  applyRange(s.dailyReadingGoal, "dailyReadingGoal", 0, 5);

  applyEnum(s.sideButtonLayout, "sideButtonLayout", S::SIDE_BUTTON_LAYOUT_COUNT);
  applyToggle(s.frontButtonFollowOrientation, "frontButtonFollowOrientation");
  applyEnum(s.longPressButtonBehavior, "longPressButtonBehavior", S::LONG_PRESS_BUTTON_BEHAVIOR_COUNT);
  applyEnum(s.longPressMenuFunction, "longPressMenuFunction", S::LONG_PRESS_MENU_FUNCTION_COUNT);
  applyEnum(s.shortPwrBtn, "shortPwrBtn", S::SHORT_PWRBTN_COUNT);
  applyToggle(s.pwrBtnFootnoteBack, "pwrBtnFootnoteBack");
  applyEnum(s.tiltPageTurn, "tiltPageTurn", S::TILT_PAGE_TURN_COUNT);

  applyToggle(s.quranEnabled, "quranEnabled");
  applyToggle(s.rssEnabled, "rssEnabled");
  applyToggle(s.gamesEnabled, "gamesEnabled");
  applyToggle(s.tasbihEnabled, "tasbihEnabled");
  applyToggle(s.stopwatchEnabled, "stopwatchEnabled");
  applyToggle(s.gymEnabled, "gymEnabled");
  applyEnum(s.gymWeightUnit, "gymWeightUnit", S::GYM_WEIGHT_UNIT_COUNT);
  applyToggle(s.debugLoggingEnabled, "debugLoggingEnabled");

  applyRange(s.sleepTimeoutMinutes, "sleepTimeoutMinutes", S::MIN_SLEEP_TIMEOUT_MINUTES, S::MAX_SLEEP_TIMEOUT_MINUTES);
  applyToggle(s.showHiddenFiles, "showHiddenFiles");
  applyToggle(s.removeReadBooksFromRecents, "removeReadBooksFromRecents");
  applyToggle(s.moveFinishedToReadFolder, "moveFinishedToReadFolder");

  applyToggle(s.statusBarChapterPageCount, "statusBarChapterPageCount");
  applyToggle(s.statusBarBookProgressPercentage, "statusBarBookProgressPercentage");
  applyEnum(s.statusBarProgressBar, "statusBarProgressBar", S::STATUS_BAR_PROGRESS_BAR_COUNT);
  applyEnum(s.statusBarProgressBarThickness, "statusBarProgressBarThickness",
            S::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT);
  applyEnum(s.statusBarTitle, "statusBarTitle", S::STATUS_BAR_TITLE_COUNT);
  applyToggle(s.statusBarBattery, "statusBarBattery");
  applyEnum(s.xtcStatusBarMode, "xtcStatusBarMode", S::XTC_STATUS_BAR_MODE_COUNT);
  applyEnum(s.statusBarClock, "statusBarClock", 3);  // STATUS_BAR_CLOCK_MODE has no _COUNT sentinel
  applyRange(s.clockUtcOffsetQ, "clockUtcOffsetQ", 0, 104);
  applyEnum(s.clockFormat, "clockFormat", 2);  // plain 0/1 field, not a named enum
  applyToggle(s.clockHasBeenSynced, "clockHasBeenSynced");

  if (changed) {
    s.saveToFile();
    diagLog("applied pushed settings from server");
  }

  // KOReader Sync -- separate store/file, each field applied independently so
  // an unrelated change (e.g. matchMethod) doesn't force-touch credentials.
  bool koChanged = false;
  {
    std::string koUser = KOREADER_STORE.getUsername();
    std::string koPass = KOREADER_STORE.getPassword();
    bool credentialsChanged = false;
    const JsonVariantConst userV = settings["koUsername"];
    if (!userV.isNull() && userV.as<std::string>() != koUser) {
      koUser = userV.as<std::string>();
      credentialsChanged = true;
    }
    // Empty/omitted koPassword means "leave unchanged" (EINK_SETTINGS_SYNC_TASKS.md) --
    // never let a blank value clear a stored password.
    const JsonVariantConst passV = settings["koPassword"];
    if (!passV.isNull()) {
      const std::string val = passV.as<std::string>();
      if (!val.empty() && val != koPass) {
        koPass = val;
        credentialsChanged = true;
      }
    }
    if (credentialsChanged) {
      KOREADER_STORE.setCredentials(koUser, koPass);
      koChanged = true;
    }
  }
  {
    const JsonVariantConst v = settings["koServerUrl"];
    if (!v.isNull() && v.as<std::string>() != KOREADER_STORE.getServerUrl()) {
      KOREADER_STORE.setServerUrl(v.as<std::string>());
      koChanged = true;
    }
  }
  {
    const JsonVariantConst v = settings["koMatchMethod"];
    if (!v.isNull()) {
      const uint8_t val = v.as<uint8_t>();
      if (val <= 1 && static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()) != val) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(val));
        koChanged = true;
      }
    }
  }
  if (koChanged) {
    KOREADER_STORE.saveToFile();
    diagLog("applied pushed KOReader settings from server");
  }
}

// Single attempt, no 404 handling -- reportReadingStats() (below) is the only
// caller and owns the retry-after-register decision.
bool postReadingStatsOnce(const std::string& username, const std::string& password, const std::string& fouladBookId,
                          const int progressPercent, const std::string& lastPosition, const uint32_t secondsRead) {
  JsonDocument doc;
  doc["serial_number"] = getSerialNumber();
  doc["book_id"] = atoi(fouladBookId.c_str());
  doc["progress_percent"] = progressPercent;
  if (!lastPosition.empty()) doc["last_position"] = lastPosition;
  doc["seconds_read"] = secondsRead;
  std::string body;
  serializeJson(doc, body);

  std::string response;
  const bool ok =
      HttpDownloader::postJson(readingStatsEndpoint(), body, username, password, response) == HttpDownloader::OK;
  diagLog("reading-stats book=" + fouladBookId + " progress=" + std::to_string(progressPercent) +
          " seconds=" + std::to_string(secondsRead) + " -> " +
          (ok ? "ok" : "FAIL status=" + std::to_string(HttpDownloader::getLastFailure().detail)));
  return ok;
}

// Single attempt, no 404 handling -- reportDeviceStats() owns the
// retry-after-register decision, same convention as postReadingStatsOnce().
// includeReading false sends `device` telemetry alone (unchanged since last
// send, or heap too tight for the larger arrays right now).
bool postDeviceStatsOnce(const std::string& username, const std::string& password, const bool includeReading) {
  JsonDocument doc;
  doc["serial_number"] = getSerialNumber();

  JsonObject device = doc["device"].to<JsonObject>();
  device["battery_percent"] = powerManager.getBatteryPercentage();
  device["wifi_rssi"] = WiFi.RSSI();
  device["free_heap_bytes"] = ESP.getFreeHeap();
  device["uptime_seconds"] = millis() / 1000;

  if (includeReading) {
    JsonObject reading = doc["reading"].to<JsonObject>();
    reading["current_streak_days"] = READING_STATS.getCurrentStreakDays();
    reading["max_streak_days"] = READING_STATS.getMaxStreakDays();
    reading["today_reading_seconds"] = static_cast<uint32_t>(READING_STATS.getTodayReadingMs() / 1000);
    reading["total_reading_seconds"] = static_cast<uint32_t>(READING_STATS.getTotalReadingMs() / 1000);
    reading["books_started"] = READING_STATS.getBooksStartedCount();
    reading["books_finished"] = READING_STATS.getBooksFinishedCount();

    // book.title empty fallback matches StatsActivity's own bookTitle().
    JsonArray books = reading["books"].to<JsonArray>();
    for (const auto& book : READING_STATS.getBooks()) {
      JsonObject b = books.add<JsonObject>();
      b["title"] = book.title.empty() ? book.path : book.title;
      b["author"] = book.author;
      b["total_reading_seconds"] = static_cast<uint32_t>(book.totalReadingMs / 1000);
      b["sessions"] = book.sessions;
      b["progress_percent"] = book.lastProgressPercent;
      b["completed"] = book.completed;
      b["last_read_at"] = book.lastReadAt;
      b["estimated_time_left_seconds"] = book.estimatedTimeLeftSeconds;
    }

    JsonArray days = reading["reading_days"].to<JsonArray>();
    for (const auto& day : READING_STATS.getReadingDays()) {
      JsonObject d = days.add<JsonObject>();
      d["day_ordinal"] = day.dayOrdinal;
      d["reading_seconds"] = day.readingMs / 1000;
    }
  }

  std::string body;
  serializeJson(doc, body);

  std::string response;
  const bool ok =
      HttpDownloader::postJson(deviceStatsEndpoint(), body, username, password, response) == HttpDownloader::OK;
  diagLog(std::string("device-stats upload (reading=") + (includeReading ? "yes" : "no") + ") -> " +
          (ok ? "ok" : "FAIL status=" + std::to_string(HttpDownloader::getLastFailure().detail)));
  return ok;
}
}  // namespace

std::string getSerialNumber() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return std::string("XTE-") + mac.c_str();
}

// Resolved once per registration: the build to offer, or empty for "nothing to say".
// A std::string rather than a pointer into the response -- responseDoc dies with the
// call and the Library reads this minutes later.
static std::string latestFirmwareOffer;

// Picks this device's channel out of {"stable", "rc"} and keeps the tag only if it
// beats the running build.
//
// Channel comes from Settings -> System -> Pre-release (otaPrereleaseEnabled), which is
// what "Check for Update" has always obeyed (OtaUpdater::checkForUpdate). This path used
// to ignore that toggle and infer the channel from whether the RUNNING build had "-rc" in
// its version instead, on the reasoning that a device running a release had opted out of
// pre-releases by doing so. That was wrong once an explicit opt-in existed: someone on a
// stable build who ticks Pre-release got RC offers from Check for Update and never from
// Library, so the toggle silently did nothing here -- reported as the Library check being
// broken.
//
// A device already running an -rc still gets rc offers whatever the toggle says: it is on
// that channel already, and the alternative is stranding it there with no route forward.
// Either way the stable tag remains the fallback, which is what carries an RC device off a
// pre-release once the equal-numbered release lands.
// Every outcome below is recorded to the SD debug log, not just the one that produces an
// offer. From outside the device "the server named nothing" and "the server named a build
// that is not newer than this one" are the same observation -- no popup -- but they are
// completely different problems, and telling them apart is otherwise impossible after the
// fact: the LOG_INF here reaches serial only, which nobody has attached while actually
// using the device. A report that the Library update check "stopped working" is
// unanswerable without this line.
static void captureLatestFirmware(JsonVariantConst latest) {
  latestFirmwareOffer.clear();

  const auto tagOf = [](JsonVariantConst v) -> const char* {
    return v.is<const char*>() ? v.as<const char*>() : nullptr;
  };
  const auto orNone = [](const char* s) { return s ? s : "(none)"; };

  if (latest.isNull()) {
    // Older server, or a current one that could not reach GitHub. Absent means "no
    // information", never "up to date".
    diagLog(std::string("firmware check: server sent no latest_firmware (running ") + CROSSPOINT_VERSION + ")");
    return;
  }

  const char* rcTag = tagOf(latest["rc"]);
  const char* stableTag = tagOf(latest["stable"]);

  const auto newerOrNull = [&tagOf](JsonVariantConst v) -> const char* {
    const char* tag = tagOf(v);
    return OtaUpdater::isVersionNewer(tag) ? tag : nullptr;
  };

  const bool wantsPrerelease = SETTINGS.otaPrereleaseEnabled != 0 || strstr(CROSSPOINT_VERSION, "-rc") != nullptr;

  const char* offer = nullptr;
  if (wantsPrerelease) offer = newerOrNull(latest["rc"]);
  if (offer == nullptr) offer = newerOrNull(latest["stable"]);
  if (offer == nullptr) {
    // The channel is logged too: "rc names a newer build but this device is on the stable
    // channel" and "rc names nothing" both end here, and only the toggle's state separates
    // them.
    diagLog(std::string("firmware check: rc=") + orNone(rcTag) + " stable=" + orNone(stableTag) +
            " channel=" + (wantsPrerelease ? "pre" : "stable") + " -- none newer than " + CROSSPOINT_VERSION);
    return;
  }

  latestFirmwareOffer = offer;
  diagLog(std::string("firmware check: offering ") + offer + " (rc=" + orNone(rcTag) + " stable=" + orNone(stableTag) +
          " channel=" + (wantsPrerelease ? "pre" : "stable") + ", running " + CROSSPOINT_VERSION + ")");
  LOG_INF(TAG, "Server reports newer firmware: %s (running %s)", offer, CROSSPOINT_VERSION);
}

void registerDevice(const std::string& username, const std::string& password) {
  if (!wifiConnected() || username.empty() || password.empty()) return;

  JsonDocument doc;
  doc["serial_number"] = getSerialNumber();
  doc["name"] = displayName();
  doc["model"] = modelName();
  doc["firmware_version"] = CROSSPOINT_VERSION;
  addSettingsReport(doc["settings"].to<JsonObject>());
  std::string body;
  serializeJson(doc, body);

  std::string response;
  const auto result = HttpDownloader::postJson(deviceEndpoint(), body, username, password, response);
  if (result != HttpDownloader::OK) {
    const int status = HttpDownloader::getLastFailure().detail;
    LOG_ERR(TAG, "Device registration failed (status=%d)", status);
    diagLog("register serial=" + getSerialNumber() + " -> FAIL status=" + std::to_string(status));
    return;
  }
  LOG_DBG(TAG, "Device registered: %s", response.c_str());
  diagLog("register serial=" + getSerialNumber() + " -> ok");

  JsonDocument responseDoc;
  if (!deserializeJson(responseDoc, response)) {
    applySettingsFromServer(responseDoc["settings"].as<JsonObjectConst>());
    captureLatestFirmware(responseDoc["latest_firmware"]);
  }
}

void reportReadingStats(const std::string& username, const std::string& password, const std::string& fouladBookId,
                        const int progressPercent, const std::string& lastPosition, const uint32_t secondsRead) {
  if (!wifiConnected() || username.empty() || password.empty() || fouladBookId.empty()) return;

  if (postReadingStatsOnce(username, password, fouladBookId, progressPercent, lastPosition, secondsRead)) {
    return;
  }

  const auto failure = HttpDownloader::getLastFailure();
  if (failure.stage == HttpDownloader::FailStage::STATUS && failure.detail == 404) {
    // Unknown device server-side -- register once, then retry exactly once.
    LOG_DBG(TAG, "Reading-stats 404 (device not registered yet), registering and retrying");
    diagLog("reading-stats book=" + fouladBookId + " got 404, registering and retrying");
    registerDevice(username, password);
    if (postReadingStatsOnce(username, password, fouladBookId, progressPercent, lastPosition, secondsRead)) return;
  }
  LOG_ERR(TAG, "Reading-stats report failed (status=%d)", HttpDownloader::getLastFailure().detail);
  diagLog("reading-stats book=" + fouladBookId +
          " gave up, status=" + std::to_string(HttpDownloader::getLastFailure().detail));
}

void flushPendingReadingStats(const std::string& username, const std::string& password) {
  if (!wifiConnected() || username.empty() || password.empty()) return;

  // Driven by the stats store, not RecentBooksStore. Recents caps at 10 and
  // evicts, so iterating it silently skipped every catalog book that had been
  // read but pushed out of the top 10 -- those never produced a library_reading
  // row on the server no matter how often the device reconnected
  // (INSTRUCTIONS-14). The stats store holds up to MAX_BOOKS and now carries the
  // id itself, so the two lists can no longer disagree about what is reportable.
  READING_STATS.ensureLoaded();
  int flushed = 0;
  for (const auto& stats : READING_STATS.getBooks()) {
    if (stats.fouladBookId.empty()) continue;  // side-loaded, or read before v3
    if (stats.totalReadingMs == 0) continue;
    reportReadingStats(username, password, stats.fouladBookId, stats.lastProgressPercent, "",
                       static_cast<uint32_t>(stats.totalReadingMs / 1000));
    flushed++;
  }
  if (flushed > 0) diagLog("flush: reported " + std::to_string(flushed) + " book(s)");
}

void flushPendingKOReaderSync() {
  if (!wifiConnected()) return;
  if (!KOREADER_STORE.hasPendingSync()) return;
  // Credentials may have been cleared (sign-out, re-pairing) since the item was
  // queued; a stale queue entry would otherwise retry forever with NO_CREDENTIALS.
  if (!KOREADER_STORE.hasCredentials()) {
    KOREADER_STORE.clearPendingSync();
    return;
  }

  const PendingKOReaderSync& pending = *KOREADER_STORE.getPendingSync();

  KOReaderProgress progress;
  progress.document = pending.documentHash;
  progress.progress = pending.xpath;
  progress.percentage = pending.percentage;

  // Rich CrossPoint position, same as the manual sync's live upload -- see
  // KOReaderSyncActivity::performUpload().
  KOReaderRichPosition pos;
  const float pct = pending.percentage < 0.0f ? 0.0f : pending.percentage > 1.0f ? 1.0f : pending.percentage;
  pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
  pos.spineIndex = pending.spineIndex;
  pos.pageNumber = pending.pageNumber;
  pos.totalPages = pending.totalPages;
  pos.paragraphIndex = pending.paragraphIndex;
  pos.xpath = pending.xpath;
  progress.position = std::move(pos);

  const auto result = KOReaderSyncClient::updateProgress(progress);
  if (result == KOReaderSyncClient::OK) {
    KOREADER_STORE.clearPendingSync();
    diagLog("KOReader sync flushed (doc=" + pending.documentHash + ")");
  }
  // Failure (offline mid-request, server error): leave it queued for the next
  // reconnect, same as every other flushPending* function here.
}

void uploadDebugLog(const std::string& username, const std::string& password) {
  if (!DebugLogging::enabled() || !wifiConnected() || username.empty() || password.empty()) return;
  if (!Storage.exists(DebugLog::PATH)) return;

  const std::vector<std::pair<std::string, std::string>> files = {{"log", DebugLog::PATH}};
  const std::vector<std::pair<std::string, std::string>> fields = {{"serial_number", getSerialNumber()},
                                                                   {"firmware_version", CROSSPOINT_VERSION}};
  std::string response;
  bool ok = HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);

  if (!ok) {
    const auto failure = HttpDownloader::getLastFailure();
    if (failure.stage == HttpDownloader::FailStage::STATUS && failure.detail == 404) {
      // Unknown device server-side -- register once, then retry exactly once,
      // same convention as reportReadingStats().
      registerDevice(username, password);
      ok = HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);
    }
  }

  // Neither outcome is diagLog()'d, because diagLog() writes to DebugLog::PATH --
  // the very file this function just uploaded (see diagLog's definition above).
  //
  // On failure that would get picked up on the next attempt, and a persistent
  // failure (e.g. no signal) would spam a line into the log on every reconnect.
  //
  // On success it defeats server-side deduplication outright: the upload
  // endpoint skips storing a body whose hash matches the last one it holds for
  // this device, so an unchanged reader costs nothing to re-report. Appending
  // "log upload -> ok" after each successful send made every subsequent body
  // byte-different, so the hash never matched and every OPDS browse stored a
  // fresh ~50KB copy (see foulad-ebooks EINK_DEVICE_LOG_TASKS.md 2.4/5.4).
  //
  // If a success marker is ever wanted, it has to live somewhere other than the
  // file being uploaded.
  if (!ok) {
    LOG_ERR(TAG, "Debug log upload failed (status=%d)", HttpDownloader::getLastFailure().detail);
  }
}

// First line of crash_report.txt is "CrossPoint version: X" (HalSystem::checkPanic).
// Empty when absent -- an older report, or a truncated write -- and the caller falls
// back to the running version rather than sending nothing.
std::string readCrashReportVersion() {
  HalFile f;
  if (!Storage.openFileForRead(TAG, CRASH_REPORT_PATH, f)) return "";
  char buf[96] = {};
  const int read = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
  if (read <= 0) return "";
  const std::string head(buf, static_cast<size_t>(read));
  constexpr const char* kPrefix = "CrossPoint version: ";
  const size_t at = head.find(kPrefix);
  if (at == std::string::npos) return "";
  const size_t start = at + strlen(kPrefix);
  const size_t end = head.find_first_of("\r\n", start);
  return head.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Condenses the "Panic reason" and "Heap before panic" lines out of crash_report.txt
// for the debug log -- readCrashReportVersion()'s technique (small bounded read, not
// the full multi-KB report with its backtrace/last-logs sections). This is what lets
// a "crash happened but I can't find crash_report.txt" report get diagnosed at all:
// flushPendingCrashReport() deletes the SD copy once it's uploaded, which can be a
// LATER boot than the one that crashed -- so a user checking the SD card afterward
// finds nothing, even though the crash was real and the report made it to the server.
// Logging this summary into debug_log.txt (the file users already pull off the device)
// before deletion keeps the panic reason visible locally too.
std::string readCrashReportSummary() {
  HalFile f;
  if (!Storage.openFileForRead(TAG, CRASH_REPORT_PATH, f)) return "(unreadable)";
  char buf[512] = {};
  const int read = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
  if (read <= 0) return "(unreadable)";
  const std::string head(buf, static_cast<size_t>(read));

  auto extractLine = [&](const char* prefix) -> std::string {
    const size_t at = head.find(prefix);
    if (at == std::string::npos) return "?";
    const size_t start = at + strlen(prefix);
    const size_t end = head.find_first_of("\r\n", start);
    return head.substr(start, end == std::string::npos ? std::string::npos : end - start);
  };

  return "reason=[" + extractLine("Panic reason: ") + "] heap=[" + extractLine("Heap before panic: ") + "]";
}

bool uploadCrashReport(const std::string& username, const std::string& password) {
  if (!wifiConnected() || username.empty() || password.empty()) return false;
  if (!Storage.exists(CRASH_REPORT_PATH)) return false;

  const std::vector<std::pair<std::string, std::string>> files = {{"log", CRASH_REPORT_PATH}};
  // The version that CRASHED, not the one uploading. A crash report queues on the SD
  // card until the device is next online, by which time it may have updated -- so
  // stamping CROSSPOINT_VERSION here attributes an old panic to a new release, which
  // is exactly backwards for "did yesterday's RC break this". Observed: a 1.8.1 panic
  // arriving labelled 1.8.5-rc. HalSystem writes the real one into the report at
  // panic time; read it back out.
  const std::string crashedVersion = readCrashReportVersion();
  const std::vector<std::pair<std::string, std::string>> fields = {
      {"serial_number", getSerialNumber()},
      {"type", "crash"},
      {"firmware_version", crashedVersion.empty() ? CROSSPOINT_VERSION : crashedVersion}};
  std::string response;
  bool ok = HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);

  if (!ok) {
    const auto failure = HttpDownloader::getLastFailure();
    if (failure.stage == HttpDownloader::FailStage::STATUS && failure.detail == 404) {
      // Unknown device server-side -- register once, then retry exactly once,
      // same convention as reportReadingStats()/uploadDebugLog().
      registerDevice(username, password);
      ok = HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);
    }
  }

  if (!ok) {
    LOG_ERR(TAG, "Crash report upload failed (status=%d)", HttpDownloader::getLastFailure().detail);
  }
  // Unlike uploadDebugLog(), diagLog() here is safe either way: this writes to
  // /debug_log.txt, a different file than the one just uploaded.
  diagLog(ok ? "crash report upload -> ok"
             : "crash report upload -> FAIL status=" + std::to_string(HttpDownloader::getLastFailure().detail));
  return ok;
}

void flushPendingCrashReport(const std::string& username, const std::string& password) {
  if (!Storage.exists(CRASH_REPORT_PATH)) return;  // nothing waiting
  if (!uploadCrashReport(username, password)) return;
  // See readCrashReportSummary(): capture the panic reason locally before the SD
  // copy is deleted below, since the upload+delete can happen on a boot after the
  // one the user actually has eyes on.
  diagLog("crash summary: " + readCrashReportSummary());
  // Delivered: drop the local copy so the next connection does not resend it.
  // Deliberately only on success -- a failed upload leaves the file for the next
  // attempt, which is the whole point of queueing it rather than requiring the
  // user to be looking at the crash screen at the moment they have WiFi.
  Storage.remove(CRASH_REPORT_PATH);
  diagLog("crash report flushed and cleared");
}

void reportDeviceStats(const std::string& username, const std::string& password) {
  if (!wifiConnected() || username.empty() || password.empty()) return;

  READING_STATS.ensureLoaded();
  const uint64_t totalMs = READING_STATS.getTotalReadingMs();
  const bool readingChanged = totalMs != lastReportedTotalReadingMs;
  const uint32_t estimatedReadingBytes =
      static_cast<uint32_t>(READING_STATS.getBooks().size()) * ESTIMATED_BYTES_PER_BOOK +
      static_cast<uint32_t>(READING_STATS.getReadingDays().size()) * ESTIMATED_BYTES_PER_READING_DAY;
  const bool includeReading =
      readingChanged && ESP.getFreeHeap() >= estimatedReadingBytes + READING_SNAPSHOT_SAFETY_MARGIN_BYTES;
  if (readingChanged && !includeReading) {
    diagLog("device-stats reading snapshot skipped: free heap=" + std::to_string(ESP.getFreeHeap()) +
            " need=" + std::to_string(estimatedReadingBytes + READING_SNAPSHOT_SAFETY_MARGIN_BYTES));
  }

  bool ok = postDeviceStatsOnce(username, password, includeReading);
  if (!ok) {
    const auto failure = HttpDownloader::getLastFailure();
    if (failure.stage == HttpDownloader::FailStage::STATUS && failure.detail == 404) {
      // Unknown device server-side -- register once, then retry exactly once,
      // same convention as every other upload here.
      registerDevice(username, password);
      ok = postDeviceStatsOnce(username, password, includeReading);
    }
  }

  // Only remember success -- a failed send (including one that skipped the
  // reading snapshot for heap reasons) should retry next connect, not be
  // treated as "already reported."
  if (ok && includeReading) lastReportedTotalReadingMs = totalMs;
}

const std::string& pendingFirmwareUpdate() { return latestFirmwareOffer; }

}  // namespace FouladDeviceTracking
