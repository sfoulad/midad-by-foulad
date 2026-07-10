#include "ReadingStats.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>

#include "CrossPointSettings.h"

namespace {
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
constexpr char BOOK_STATS_FILENAME[] = "/reading_stats.bin";
constexpr uint8_t BOOK_STATS_VERSION = 1;
constexpr uint8_t GLOBAL_STATS_VERSION = 1;
}  // namespace

std::string readingStatsCachePathForBook(const std::string& bookPath) {
  auto endsWithCi = [&](const char* ext) {
    const size_t n = strlen(ext);
    if (bookPath.size() < n) return false;
    for (size_t i = 0; i < n; i++) {
      char a = bookPath[bookPath.size() - n + i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (a != ext[i]) return false;
    }
    return true;
  };
  const bool isXtc = endsWithCi(".xtc") || endsWithCi(".xtch");
  return std::string("/.crosspoint/") + (isXtc ? "xtc_" : "epub_") + std::to_string(std::hash<std::string>{}(bookPath));
}

ReadingLocalDateTime getCurrentLocalReadingDateTime() {
  ReadingLocalDateTime out;
  if (!HalClock::isSystemTimeValid()) {
    return out;
  }
  const time_t utcNow = time(nullptr);
  // clockUtcOffsetQ is a biased quarter-hour offset: 48 = UTC+0.
  const int offsetMinutes = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  const int64_t localEpoch = static_cast<int64_t>(utcNow) + static_cast<int64_t>(offsetMinutes) * 60;
  if (localEpoch <= 0) {
    return out;
  }
  out.dayIndex = static_cast<uint32_t>(localEpoch / 86400);
  out.hour = static_cast<uint8_t>((localEpoch % 86400) / 3600);
  // 1970-01-01 was a Thursday; Monday = 0 -> Thursday's index is 3.
  out.dayOfWeek = static_cast<uint8_t>((out.dayIndex + 3) % 7);
  out.valid = true;
  return out;
}

uint8_t readingTimeBucketForHour(const uint8_t hour) {
  if (hour >= 5 && hour <= 11) return 0;   // morning
  if (hour >= 12 && hour <= 16) return 1;  // afternoon
  if (hour >= 17 && hour <= 21) return 2;  // evening
  return 3;                                // night
}

void formatReadingDuration(const uint32_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "<1m");
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lum", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  BookReadingStats stats;
  HalFile f;
  if (!Storage.openFileForRead("RSTAT", cachePath + BOOK_STATS_FILENAME, f)) {
    return stats;
  }
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != BOOK_STATS_VERSION) {
    LOG_DBG("RSTAT", "Book stats version mismatch (%u), starting fresh", version);
    return BookReadingStats{};
  }
  serialization::readPod(f, stats.sessionCount);
  serialization::readPod(f, stats.totalReadingSeconds);
  serialization::readPod(f, stats.totalPagesTurned);
  serialization::readPod(f, stats.avgSecondsPerForwardPage);
  serialization::readPod(f, stats.estimatedTimeLeftSeconds);
  serialization::readPod(f, stats.lastProgressPercent);
  serialization::readPod(f, stats.firstReadDayIndex);
  serialization::readPod(f, stats.lastReadDayIndex);
  return stats;
}

void BookReadingStats::save(const std::string& cachePath) const {
  HalFile f;
  if (!Storage.openFileForWrite("RSTAT", cachePath + BOOK_STATS_FILENAME, f)) {
    LOG_ERR("RSTAT", "Could not write book reading stats");
    return;
  }
  serialization::writePod(f, BOOK_STATS_VERSION);
  serialization::writePod(f, sessionCount);
  serialization::writePod(f, totalReadingSeconds);
  serialization::writePod(f, totalPagesTurned);
  serialization::writePod(f, avgSecondsPerForwardPage);
  serialization::writePod(f, estimatedTimeLeftSeconds);
  serialization::writePod(f, lastProgressPercent);
  serialization::writePod(f, firstReadDayIndex);
  serialization::writePod(f, lastReadDayIndex);
}

void BookReadingStats::recordForwardPageRead(const uint32_t seconds) {
  const uint16_t sample = static_cast<uint16_t>(std::min<uint32_t>(seconds, UINT16_MAX));
  if (avgSecondsPerForwardPage == 0) {
    avgSecondsPerForwardPage = sample;
  } else {
    // EMA with 1/8 weight: smooth enough to ignore one distracted page, responsive
    // enough to track a genuine pace change within a couple dozen pages.
    avgSecondsPerForwardPage =
        static_cast<uint16_t>((static_cast<uint32_t>(avgSecondsPerForwardPage) * 7 + sample + 4) / 8);
    if (avgSecondsPerForwardPage == 0) avgSecondsPerForwardPage = 1;
  }
}

GlobalReadingStats GlobalReadingStats::load() {
  GlobalReadingStats stats;
  HalFile f;
  if (!Storage.openFileForRead("RSTAT", GLOBAL_STATS_PATH, f)) {
    return stats;
  }
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != GLOBAL_STATS_VERSION) {
    LOG_DBG("RSTAT", "Global stats version mismatch (%u), starting fresh", version);
    return GlobalReadingStats{};
  }
  serialization::readPod(f, stats.totalSessions);
  serialization::readPod(f, stats.totalReadingSeconds);
  serialization::readPod(f, stats.totalPagesTurned);
  for (auto& v : stats.timeOfDaySeconds) serialization::readPod(f, v);
  for (auto& v : stats.dayOfWeekSeconds) serialization::readPod(f, v);
  serialization::readPod(f, stats.historyAnchorDay);
  for (auto& b : stats.historyBits) serialization::readPod(f, b);
  serialization::readPod(f, stats.longestStreak);
  return stats;
}

void GlobalReadingStats::save() const {
  HalFile f;
  if (!Storage.openFileForWrite("RSTAT", GLOBAL_STATS_PATH, f)) {
    LOG_ERR("RSTAT", "Could not write global reading stats");
    return;
  }
  serialization::writePod(f, GLOBAL_STATS_VERSION);
  serialization::writePod(f, totalSessions);
  serialization::writePod(f, totalReadingSeconds);
  serialization::writePod(f, totalPagesTurned);
  for (const auto& v : timeOfDaySeconds) serialization::writePod(f, v);
  for (const auto& v : dayOfWeekSeconds) serialization::writePod(f, v);
  serialization::writePod(f, historyAnchorDay);
  for (const auto& b : historyBits) serialization::writePod(f, b);
  serialization::writePod(f, longestStreak);
}

void GlobalReadingStats::recordReadingSpan(const ReadingLocalDateTime& start, const uint32_t seconds) {
  if (!start.valid || seconds == 0) {
    return;
  }
  timeOfDaySeconds[readingTimeBucketForHour(start.hour)] += seconds;
  dayOfWeekSeconds[start.dayOfWeek % READING_DAY_OF_WEEK_COUNT] += seconds;

  // Mark the day in the rolling history bitmap.
  if (historyAnchorDay == 0) {
    // First mark ever: anchor the window so today sits near the start.
    historyAnchorDay = start.dayIndex;
  }
  if (start.dayIndex < historyAnchorDay) {
    return;  // clock moved backwards past the window start; don't corrupt the bitmap
  }
  uint32_t bit = start.dayIndex - historyAnchorDay;
  if (bit >= READING_HISTORY_DAYS) {
    // Slide the window forward so the new day becomes the last bit. Whole-byte
    // shifts keep this simple; a few dropped oldest days at the boundary is fine.
    const uint32_t slideDays = bit - READING_HISTORY_DAYS + 1;
    const uint32_t slideBytes = (slideDays + 7) / 8;
    if (slideBytes >= READING_HISTORY_BYTES) {
      historyBits.fill(0);
      historyAnchorDay = start.dayIndex;
      bit = 0;
    } else {
      for (size_t i = 0; i + slideBytes < READING_HISTORY_BYTES; i++) {
        historyBits[i] = historyBits[i + slideBytes];
      }
      for (size_t i = READING_HISTORY_BYTES - slideBytes; i < READING_HISTORY_BYTES; i++) {
        historyBits[i] = 0;
      }
      historyAnchorDay += slideBytes * 8;
      bit = start.dayIndex - historyAnchorDay;
    }
  }
  historyBits[bit / 8] |= static_cast<uint8_t>(1U << (bit % 8));

  const uint16_t streak = currentStreak(start.dayIndex);
  if (streak > longestStreak) {
    longestStreak = streak;
  }
}

bool GlobalReadingStats::wasDayRead(const uint32_t dayIndex) const {
  if (historyAnchorDay == 0 || dayIndex < historyAnchorDay) {
    return false;
  }
  const uint32_t bit = dayIndex - historyAnchorDay;
  if (bit >= READING_HISTORY_DAYS) {
    return false;
  }
  return (historyBits[bit / 8] & (1U << (bit % 8))) != 0;
}

uint16_t GlobalReadingStats::currentStreak(const uint32_t todayIndex) const {
  // A streak is alive if today OR yesterday was read (today may simply not have
  // started yet); count consecutive read days backwards from the newest read day.
  uint32_t day = todayIndex;
  if (!wasDayRead(day)) {
    if (day == 0 || !wasDayRead(day - 1)) {
      return 0;
    }
    day = day - 1;
  }
  uint16_t streak = 0;
  while (wasDayRead(day)) {
    streak++;
    if (day == 0 || streak == UINT16_MAX) break;
    day--;
  }
  return streak;
}

uint16_t GlobalReadingStats::daysReadInLast(const uint16_t days, const uint32_t todayIndex) const {
  uint16_t count = 0;
  for (uint16_t i = 0; i < days; i++) {
    if (todayIndex < i) break;
    if (wasDayRead(todayIndex - i)) count++;
  }
  return count;
}
