#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Day-level reading statistics, modeled on cpr-vcodex's ReadingStatsStore
// (github.com/franssjz/cpr-vcodex) with deliberate slimming for the ESP32-C3
// RAM/battery budget:
//
// - One compact binary file (/.crosspoint/reading_stats.bin) via Serialization.h,
//   not JSON: no ArduinoJson document allocation, smaller on SD, faster to load.
// - No session log, book-id aliases, auto-backups, or import/export machinery.
// - The aggregated per-day vector is rebuilt from the per-book days at load time
//   instead of being persisted.
// - Hard caps (books / days-per-book / aggregate days) bound worst-case RAM; the
//   pruned days fold into the per-book lifetime total so no reading time is lost.
// - SD writes only on session end plus a 5-minute deferred safety save while
//   reading (cpr-vcodex saved every 30s) -- battery over crash-window.
// - Keeps this fork's pace EMA + estimated-time-left fields, which the Foulad
//   home hero card displays.

// Reading-time accounting constants (seconds), used by the reader's dwell timer.
// A page interval longer than the idle threshold means the reader was left open
// (device asleep, set aside); the interval is discarded rather than counted.
constexpr uint32_t READING_IDLE_THRESHOLD_SECONDS = 300;
// Dwell shorter than this is page-flipping/skimming, not reading -- too noisy as a
// words-per-page pace sample for the time-left estimate.
constexpr uint32_t MIN_PACE_SAMPLE_SECONDS = 5;

// Per-day reading time. dayOrdinal = local days since 1970-01-01 (TimeUtils).
struct ReadingDayStats {
  uint32_t dayOrdinal = 0;
  uint32_t readingMs = 0;
};

struct ReadingBookStats {
  std::string path;
  std::string title;
  std::string author;
  // Ascending dayOrdinal; capped at MAX_DAYS_PER_BOOK (oldest pruned).
  std::vector<ReadingDayStats> readingDays;
  uint64_t totalReadingMs = 0;
  uint32_t sessions = 0;
  uint32_t lastSessionMs = 0;
  uint32_t firstReadAt = 0;  // epoch seconds; 0 = clock never valid while reading
  uint32_t lastReadAt = 0;
  uint32_t completedAt = 0;
  // Pace EMA + cached time-left estimate for the home hero card (this fork's
  // enhancement; cpr-vcodex has neither).
  uint32_t estimatedTimeLeftSeconds = 0;
  uint16_t avgSecondsPerForwardPage = 0;
  uint8_t lastProgressPercent = 0;
  bool completed = false;
};

// Compact duration for UI readouts: "<1m", "45m", "2h 30m".
void formatReadingDuration(uint32_t seconds, char* buf, size_t len);

class ReadingStatsStore {
  static constexpr size_t MAX_BOOKS = 40;
  static constexpr size_t MAX_DAYS_PER_BOOK = 90;
  static constexpr size_t MAX_AGGREGATE_DAYS = 750;
  static constexpr unsigned long DEFERRED_SAVE_INTERVAL_MS = 5UL * 60UL * 1000UL;
  static constexpr uint32_t MIN_SESSION_SECONDS = 60;

  struct SessionState {
    bool active = false;
    uint64_t accumulatedMs = 0;
    bool startCompleted = false;
  };

  // Newest-first: books[0] is the active/most recent book.
  std::vector<ReadingBookStats> books;
  // Aggregate across books, ascending dayOrdinal; rebuilt on load.
  std::vector<ReadingDayStats> readingDays;
  uint16_t maxStreakDays = 0;  // persisted: survives aggregate-day pruning
  SessionState session;
  bool loaded = false;
  bool dirty = false;
  unsigned long lastSaveMs = 0;

  ReadingStatsStore() = default;

  size_t findBookIndex(const std::string& path) const;
  // Find-or-create at index 0 (moves an existing entry to the front).
  void touchBook(const std::string& path, const std::string& title, const std::string& author);
  void addDayMs(std::vector<ReadingDayStats>& days, uint32_t dayOrdinal, uint32_t ms, size_t maxDays);
  void rebuildAggregatedDays();
  void updateStreaksAfterWrite();
  void saveDeferred();
  bool save();

 public:
  static ReadingStatsStore& getInstance() {
    static ReadingStatsStore instance;
    return instance;
  }

  // Loads /.crosspoint/reading_stats.bin on first call (and deletes the legacy
  // v1-format global_stats.bin if present). Cheap no-op afterwards.
  void ensureLoaded();

  // --- Reader session hooks (all no-ops unless a session is active) ---
  void beginSession(const std::string& path, const std::string& title, const std::string& author);
  // Credits actively-read seconds to the current book + today's day buckets.
  void addReadingTime(uint32_t seconds);
  // Feeds the forward-page pace EMA (reader pre-filters warmup/idle samples).
  void recordForwardPage(uint32_t seconds);
  uint16_t activeBookPace() const;
  // Commits session count/threshold bookkeeping and saves. estimatedTimeLeft of 0
  // means "unknown".
  void endSession(uint32_t estimatedTimeLeftSeconds, uint8_t progressPercent, bool bookCompleted);

  // --- Read accessors for stats/heatmap/home screens ---
  const std::vector<ReadingBookStats>& getBooks() const { return books; }
  const std::vector<ReadingDayStats>& getReadingDays() const { return readingDays; }
  const ReadingBookStats* findBook(const std::string& path) const;
  uint32_t readingMsOnDay(uint32_t dayOrdinal) const;
  uint64_t getTotalReadingMs() const;
  uint64_t getTodayReadingMs() const;
  uint64_t getRecentReadingMs(uint32_t days) const;
  uint32_t getCurrentStreakDays() const;
  uint32_t getMaxStreakDays() const { return maxStreakDays; }
  uint32_t getBooksStartedCount() const { return static_cast<uint32_t>(books.size()); }
  uint32_t getBooksFinishedCount() const;
  // Newest day ordinal with any reading; 0 when there is none.
  uint32_t getLatestReadingDayOrdinal() const;

  bool removeBook(const std::string& path);
};

#define READING_STATS ReadingStatsStore::getInstance()
