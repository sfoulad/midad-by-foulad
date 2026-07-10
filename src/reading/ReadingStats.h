#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Reading-time statistics, modeled on CrossInk's (github.com/uxjulia/crossink)
// BookReadingStats/GlobalReadingStats design with two deliberate departures:
//
// 1. Dates come from the NTP-synced SYSTEM clock (time() + SETTINGS.clockUtcOffsetQ)
//    instead of DS3231 calendar registers. This fork already keeps the system clock
//    valid for TLS certificate validation (HalClock::quickSyncSystemTime), so date
//    attribution works on BOTH X3 and X4 -- CrossInk's date features are X3-only.
//    When the clock isn't valid yet (fresh boot, no WiFi), time still accumulates;
//    only the date-bucketed attribution (streaks, time-of-day) is skipped.
//
// 2. All date math uses a flat "days since Unix epoch, in local time" day index
//    instead of y/m/d structs -- no leap-year/month-length arithmetic to maintain.

constexpr size_t READING_TIME_BUCKET_COUNT = 4;  // morning / afternoon / evening / night
constexpr size_t READING_DAY_OF_WEEK_COUNT = 7;  // Monday = 0
constexpr size_t READING_HISTORY_DAYS = 730;     // 2 years of did-read-today bits
constexpr size_t READING_HISTORY_BYTES = (READING_HISTORY_DAYS + 7) / 8;

// Reading-time accounting constants (seconds).
// A page interval longer than the idle threshold means the reader was left open
// (device asleep, set aside); the interval is discarded rather than counted.
constexpr uint32_t READING_IDLE_THRESHOLD_SECONDS = 300;
// Dwell shorter than this is page-flipping/skimming, not reading -- too noisy as a
// words-per-page pace sample for the time-left estimate.
constexpr uint32_t MIN_PACE_SAMPLE_SECONDS = 5;
// Session commit thresholds (matching CrossInk): sessions under 10s add no time at
// all; sessions under 60s add time but don't count as a "session".
constexpr uint32_t MIN_SESSION_SECONDS_FOR_TIME = 10;
constexpr uint32_t MIN_SESSION_SECONDS_FOR_SESSION = 60;

struct ReadingLocalDateTime {
  uint32_t dayIndex = 0;  // local days since 1970-01-01
  uint8_t hour = 0;       // local hour 0-23
  uint8_t dayOfWeek = 0;  // Monday = 0
  bool valid = false;
};

// Cache directory for a book path ("/.crosspoint/epub_<hash>" / "xtc_<hash>"), mirroring
// the Epub/Xtc constructors, so home-screen surfaces can read a book's stats file
// without constructing the full book object.
std::string readingStatsCachePathForBook(const std::string& bookPath);

// Current local date/time from the system clock; valid=false when the system clock
// hasn't been NTP-synced since boot (HalClock::isSystemTimeValid()).
ReadingLocalDateTime getCurrentLocalReadingDateTime();

// 0=morning(5-11) 1=afternoon(12-16) 2=evening(17-21) 3=night(22-4)
uint8_t readingTimeBucketForHour(uint8_t hour);

// Compact duration for UI readouts: "<1m", "45m", "2h 30m".
void formatReadingDuration(uint32_t seconds, char* buf, size_t len);

// Per-book stats, persisted to <book cache dir>/reading_stats.bin. Loaded when the
// reader opens the book and by the Foulad home theme's hero card; saved only on
// reader exit (SD write-throttling: never written per page turn).
struct BookReadingStats {
  uint16_t sessionCount = 0;
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesTurned = 0;
  // Smoothed seconds-per-page pace used for the time-left estimate. Exponential
  // moving average (7/8 old + 1/8 new) instead of CrossInk's windowed mean: one
  // sample of state, naturally favors recent pace, immune to count overflow.
  uint16_t avgSecondsPerForwardPage = 0;
  // Cached on reader exit so the home hero card can show "Est. Left" without
  // opening the book. 0 = unavailable.
  uint32_t estimatedTimeLeftSeconds = 0;
  // Cached book progress (0-100) on reader exit, for the hero/recents progress bars.
  uint8_t lastProgressPercent = 0;
  uint32_t firstReadDayIndex = 0;  // 0 = unknown (clock never valid while reading)
  uint32_t lastReadDayIndex = 0;

  static BookReadingStats load(const std::string& cachePath);
  void save(const std::string& cachePath) const;

  void recordForwardPageRead(uint32_t seconds);
};

// Cross-book stats, persisted to /.crosspoint/global_stats.bin. Same load/save
// cadence as the per-book file.
struct GlobalReadingStats {
  uint32_t totalSessions = 0;
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesTurned = 0;
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
  // Rolling 730-day did-read bitmap. anchorDay is the day index of bit 0; the
  // window slides forward (dropping oldest bits) when a newer day is marked.
  uint32_t historyAnchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> historyBits{};
  uint16_t longestStreak = 0;

  static GlobalReadingStats load();
  void save() const;

  // Attributes a reading span to the date/time buckets and marks the day in the
  // history bitmap (spans are attributed entirely to their start day).
  void recordReadingSpan(const ReadingLocalDateTime& start, uint32_t seconds);

  bool wasDayRead(uint32_t dayIndex) const;
  uint16_t currentStreak(uint32_t todayIndex) const;
  // Days with any reading within the last `days` days ending at todayIndex.
  uint16_t daysReadInLast(uint16_t days, uint32_t todayIndex) const;
};
