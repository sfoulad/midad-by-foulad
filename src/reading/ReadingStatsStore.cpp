#include "ReadingStatsStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <numeric>

#include "util/TimeUtils.h"

namespace {
constexpr char STATS_PATH[] = "/.crosspoint/reading_stats.bin";
constexpr char LEGACY_GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
constexpr uint8_t STATS_FILE_VERSION = 1;
// Sanity bounds for a corrupt file (counts read before any allocation).
constexpr uint16_t MAX_FILE_BOOKS = 512;
constexpr uint16_t MAX_FILE_DAYS = 4096;
}  // namespace

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

void ReadingStatsStore::ensureLoaded() {
  if (loaded) {
    return;
  }
  loaded = true;

  // One-time cleanup of the previous stats system's file (per-book sidecars are
  // removed lazily when each book is next opened).
  Storage.remove(LEGACY_GLOBAL_STATS_PATH);

  HalFile f;
  if (!Storage.openFileForRead("RSTAT", STATS_PATH, f)) {
    return;  // first run: empty store
  }
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != STATS_FILE_VERSION) {
    LOG_DBG("RSTAT", "Stats file version mismatch (%u), starting fresh", version);
    return;
  }
  serialization::readPod(f, maxStreakDays);
  uint16_t bookCount = 0;
  serialization::readPod(f, bookCount);
  if (bookCount > MAX_FILE_BOOKS) {
    LOG_ERR("RSTAT", "Corrupt stats file (%u books), starting fresh", bookCount);
    maxStreakDays = 0;
    return;
  }
  books.reserve(std::min<size_t>(bookCount, MAX_BOOKS));
  for (uint16_t i = 0; i < bookCount; i++) {
    ReadingBookStats book;
    serialization::readString(f, book.path);
    serialization::readString(f, book.title);
    serialization::readString(f, book.author);
    serialization::readPod(f, book.totalReadingMs);
    serialization::readPod(f, book.sessions);
    serialization::readPod(f, book.lastSessionMs);
    serialization::readPod(f, book.firstReadAt);
    serialization::readPod(f, book.lastReadAt);
    serialization::readPod(f, book.completedAt);
    serialization::readPod(f, book.estimatedTimeLeftSeconds);
    serialization::readPod(f, book.avgSecondsPerForwardPage);
    serialization::readPod(f, book.lastProgressPercent);
    serialization::readPod(f, book.completed);
    uint16_t dayCount = 0;
    serialization::readPod(f, dayCount);
    if (dayCount > MAX_FILE_DAYS) {
      LOG_ERR("RSTAT", "Corrupt stats file (%u days), starting fresh", dayCount);
      books.clear();
      maxStreakDays = 0;
      return;
    }
    book.readingDays.reserve(std::min<size_t>(dayCount, MAX_DAYS_PER_BOOK));
    for (uint16_t d = 0; d < dayCount; d++) {
      ReadingDayStats day;
      serialization::readPod(f, day.dayOrdinal);
      serialization::readPod(f, day.readingMs);
      if (book.readingDays.size() < MAX_DAYS_PER_BOOK) {
        book.readingDays.push_back(day);
      }
    }
    if (books.size() < MAX_BOOKS) {
      books.push_back(std::move(book));
    }
  }
  rebuildAggregatedDays();
  LOG_DBG("RSTAT", "Loaded stats: %u books, %u aggregate days", static_cast<unsigned>(books.size()),
          static_cast<unsigned>(readingDays.size()));
}

size_t ReadingStatsStore::findBookIndex(const std::string& path) const {
  for (size_t i = 0; i < books.size(); i++) {
    if (books[i].path == path) {
      return i;
    }
  }
  return books.size();
}

const ReadingBookStats* ReadingStatsStore::findBook(const std::string& path) const {
  const size_t index = findBookIndex(path);
  return index < books.size() ? &books[index] : nullptr;
}

void ReadingStatsStore::touchBook(const std::string& path, const std::string& title, const std::string& author) {
  size_t index = findBookIndex(path);
  if (index >= books.size()) {
    if (books.size() >= MAX_BOOKS) {
      // Evict the least recently read book (its time is already reflected in the
      // aggregate day entries, which stay).
      size_t oldest = 0;
      for (size_t i = 1; i < books.size(); i++) {
        if (books[i].lastReadAt < books[oldest].lastReadAt) {
          oldest = i;
        }
      }
      books.erase(books.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    ReadingBookStats book;
    book.path = path;
    books.insert(books.begin(), std::move(book));
    index = 0;
    dirty = true;
  }
  if (index != 0) {
    std::rotate(books.begin(), books.begin() + static_cast<std::ptrdiff_t>(index),
                books.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    dirty = true;
  }
  auto& book = books[0];
  if (!title.empty() && book.title != title) {
    book.title = title;
    dirty = true;
  }
  if (!author.empty() && book.author != author) {
    book.author = author;
    dirty = true;
  }
}

void ReadingStatsStore::addDayMs(std::vector<ReadingDayStats>& days, const uint32_t dayOrdinal, const uint32_t ms,
                                 const size_t maxDays) {
  // Days arrive in (nearly) ascending order, so search from the back.
  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    if (it->dayOrdinal == dayOrdinal) {
      it->readingMs += ms;
      return;
    }
    if (it->dayOrdinal < dayOrdinal) {
      break;
    }
  }
  const auto pos = std::lower_bound(days.begin(), days.end(), dayOrdinal,
                                    [](const ReadingDayStats& d, const uint32_t v) { return d.dayOrdinal < v; });
  days.insert(pos, ReadingDayStats{dayOrdinal, ms});
  if (days.size() > maxDays) {
    days.erase(days.begin());  // prune oldest; lifetime totals keep the time
  }
}

void ReadingStatsStore::rebuildAggregatedDays() {
  readingDays.clear();
  const size_t total =
      std::accumulate(books.begin(), books.end(), static_cast<size_t>(0),
                      [](const size_t sum, const ReadingBookStats& book) { return sum + book.readingDays.size(); });
  readingDays.reserve(std::min(total, MAX_AGGREGATE_DAYS));
  for (const auto& book : books) {
    for (const auto& day : book.readingDays) {
      addDayMs(readingDays, day.dayOrdinal, day.readingMs, MAX_AGGREGATE_DAYS);
    }
  }
}

void ReadingStatsStore::updateStreaksAfterWrite() {
  const uint32_t streak = getCurrentStreakDays();
  if (streak > maxStreakDays) {
    maxStreakDays = static_cast<uint16_t>(std::min<uint32_t>(streak, UINT16_MAX));
  }
}

void ReadingStatsStore::beginSession(const std::string& path, const std::string& title, const std::string& author) {
  ensureLoaded();
  if (path.empty()) {
    return;
  }
  if (session.active) {
    endSession(0, books.empty() ? 0 : books[0].lastProgressPercent, false);
  }
  touchBook(path, title, author);
  session.active = true;
  session.accumulatedMs = 0;
  session.startCompleted = books[0].completed;
}

void ReadingStatsStore::addReadingTime(const uint32_t seconds) {
  if (!session.active || books.empty() || seconds == 0) {
    return;
  }
  const uint32_t ms = seconds * 1000U;
  auto& book = books[0];
  book.totalReadingMs += ms;
  session.accumulatedMs += ms;

  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (TimeUtils::isClockValid() && TimeUtils::isClockValid(now)) {
    const uint32_t today = TimeUtils::getLocalDayOrdinal(now);
    if (today != 0) {
      addDayMs(book.readingDays, today, ms, MAX_DAYS_PER_BOOK);
      addDayMs(readingDays, today, ms, MAX_AGGREGATE_DAYS);
      updateStreaksAfterWrite();
    }
    if (book.firstReadAt == 0) {
      book.firstReadAt = now;
    }
    book.lastReadAt = now;
  }
  dirty = true;
  saveDeferred();
}

void ReadingStatsStore::recordForwardPage(const uint32_t seconds) {
  if (!session.active || books.empty()) {
    return;
  }
  auto& book = books[0];
  const uint16_t sample = static_cast<uint16_t>(std::min<uint32_t>(seconds, UINT16_MAX));
  if (book.avgSecondsPerForwardPage == 0) {
    book.avgSecondsPerForwardPage = sample;
  } else {
    // EMA with 1/8 weight: smooth enough to ignore one distracted page, responsive
    // enough to track a genuine pace change within a couple dozen pages.
    book.avgSecondsPerForwardPage =
        static_cast<uint16_t>((static_cast<uint32_t>(book.avgSecondsPerForwardPage) * 7 + sample + 4) / 8);
    if (book.avgSecondsPerForwardPage == 0) {
      book.avgSecondsPerForwardPage = 1;
    }
  }
  dirty = true;
}

uint16_t ReadingStatsStore::activeBookPace() const {
  return (session.active && !books.empty()) ? books[0].avgSecondsPerForwardPage : 0;
}

void ReadingStatsStore::endSession(const uint32_t estimatedTimeLeftSeconds, const uint8_t progressPercent,
                                   const bool bookCompleted) {
  if (!session.active || books.empty()) {
    session = {};
    return;
  }
  auto& book = books[0];
  if (session.accumulatedMs >= static_cast<uint64_t>(MIN_SESSION_SECONDS) * 1000ULL) {
    book.sessions++;
    book.lastSessionMs = static_cast<uint32_t>(std::min<uint64_t>(session.accumulatedMs, UINT32_MAX));
    dirty = true;
  }
  const uint8_t pct = std::min<uint8_t>(progressPercent, 100);
  if (book.lastProgressPercent != pct) {
    book.lastProgressPercent = pct;
    dirty = true;
  }
  if (book.estimatedTimeLeftSeconds != estimatedTimeLeftSeconds) {
    book.estimatedTimeLeftSeconds = estimatedTimeLeftSeconds;
    dirty = true;
  }
  if ((bookCompleted || pct >= 100) && !book.completed) {
    book.completed = true;
    if (book.completedAt == 0) {
      book.completedAt = book.lastReadAt;
    }
    dirty = true;
  }
  session = {};
  if (dirty) {
    save();
  }
}

uint32_t ReadingStatsStore::readingMsOnDay(const uint32_t dayOrdinal) const {
  const auto it = std::lower_bound(readingDays.begin(), readingDays.end(), dayOrdinal,
                                   [](const ReadingDayStats& d, const uint32_t v) { return d.dayOrdinal < v; });
  return (it != readingDays.end() && it->dayOrdinal == dayOrdinal) ? it->readingMs : 0;
}

uint64_t ReadingStatsStore::getTotalReadingMs() const {
  return std::accumulate(books.begin(), books.end(), static_cast<uint64_t>(0),
                         [](const uint64_t sum, const ReadingBookStats& book) { return sum + book.totalReadingMs; });
}

uint64_t ReadingStatsStore::getTodayReadingMs() const {
  const uint32_t today = TimeUtils::todayOrdinal();
  return today != 0 ? readingMsOnDay(today) : 0;
}

uint64_t ReadingStatsStore::getRecentReadingMs(const uint32_t days) const {
  const uint32_t today = TimeUtils::todayOrdinal();
  if (today == 0 || days == 0) {
    return 0;
  }
  const uint32_t from = today >= days - 1 ? today - (days - 1) : 0;
  uint64_t total = 0;
  for (auto it = readingDays.rbegin(); it != readingDays.rend(); ++it) {
    if (it->dayOrdinal < from) {
      break;
    }
    if (it->dayOrdinal <= today) {
      total += it->readingMs;
    }
  }
  return total;
}

uint32_t ReadingStatsStore::getCurrentStreakDays() const {
  if (readingDays.empty()) {
    return 0;
  }
  // A streak is alive if today OR yesterday was read (today may not have started
  // yet). Without a valid clock, count back from the newest recorded day.
  const uint32_t today = TimeUtils::todayOrdinal();
  uint32_t day = readingDays.back().dayOrdinal;
  if (today != 0 && day + 1 < today) {
    return 0;  // newest reading is older than yesterday
  }
  uint32_t streak = 0;
  auto it = readingDays.rbegin();
  while (it != readingDays.rend() && it->dayOrdinal == day - streak) {
    if (it->readingMs > 0) {
      streak++;
      ++it;
    } else {
      break;
    }
  }
  return streak;
}

uint32_t ReadingStatsStore::getBooksFinishedCount() const {
  return static_cast<uint32_t>(
      std::count_if(books.begin(), books.end(), [](const ReadingBookStats& book) { return book.completed; }));
}

uint32_t ReadingStatsStore::getLatestReadingDayOrdinal() const {
  return readingDays.empty() ? 0 : readingDays.back().dayOrdinal;
}

bool ReadingStatsStore::removeBook(const std::string& path) {
  ensureLoaded();
  const size_t index = findBookIndex(path);
  if (index >= books.size()) {
    return false;
  }
  if (session.active && index == 0) {
    session = {};
  }
  books.erase(books.begin() + static_cast<std::ptrdiff_t>(index));
  rebuildAggregatedDays();
  dirty = true;
  save();
  return true;
}

void ReadingStatsStore::saveDeferred() {
  // Crash-protection save while a long session is running. 5 minutes keeps SD
  // writes rare (battery); everything is saved unconditionally on session end.
  if (!dirty || (lastSaveMs != 0 && millis() - lastSaveMs < DEFERRED_SAVE_INTERVAL_MS)) {
    return;
  }
  save();
}

bool ReadingStatsStore::save() {
  HalFile f;
  if (!Storage.openFileForWrite("RSTAT", STATS_PATH, f)) {
    LOG_ERR("RSTAT", "Could not write reading stats");
    return false;
  }
  serialization::writePod(f, STATS_FILE_VERSION);
  serialization::writePod(f, maxStreakDays);
  const uint16_t bookCount = static_cast<uint16_t>(books.size());
  serialization::writePod(f, bookCount);
  for (const auto& book : books) {
    serialization::writeString(f, book.path);
    serialization::writeString(f, book.title);
    serialization::writeString(f, book.author);
    serialization::writePod(f, book.totalReadingMs);
    serialization::writePod(f, book.sessions);
    serialization::writePod(f, book.lastSessionMs);
    serialization::writePod(f, book.firstReadAt);
    serialization::writePod(f, book.lastReadAt);
    serialization::writePod(f, book.completedAt);
    serialization::writePod(f, book.estimatedTimeLeftSeconds);
    serialization::writePod(f, book.avgSecondsPerForwardPage);
    serialization::writePod(f, book.lastProgressPercent);
    serialization::writePod(f, book.completed);
    const uint16_t dayCount = static_cast<uint16_t>(book.readingDays.size());
    serialization::writePod(f, dayCount);
    for (const auto& day : book.readingDays) {
      serialization::writePod(f, day.dayOrdinal);
      serialization::writePod(f, day.readingMs);
    }
  }
  dirty = false;
  lastSaveMs = millis();
  return true;
}
