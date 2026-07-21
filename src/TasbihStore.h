#pragma once
#include <PersistableStore.h>

#include <cstdint>

// Persisted daily tally for the Tasbih (dhikr counter) app. Kept in its own
// small file rather than folded into CrossPointSettings, same reasoning as
// GameHighScoresStore: the settings JSON loop only round-trips uint8_t
// fields, too narrow for a running count/year total, and this isn't a
// user-configurable setting anyway.
//
// Today's count resets once per calendar day (not per app visit -- reopening
// the app resumes today's count). "Top Tasbih" is the all-time best single
// day; "Total this year" sums every day's count within the current calendar
// year. Both figures fold in today's in-progress count live (see the
// getters) so they're accurate without waiting for the day to end.
class TasbihStore : public PersistableStore<TasbihStore> {
 private:
  uint32_t todayCount = 0;
  uint32_t todayDayOrdinal = 0;    // TimeUtils day ordinal; 0 = no day recorded yet
  uint32_t maxSingleDayCount = 0;  // best FULLY-ENDED day only; live getter adds today
  uint32_t yearTotal = 0;          // sum of ENDED days in yearCovered; live getter adds today
  uint32_t yearCovered = 0;        // calendar year yearTotal currently accumulates

  TasbihStore() = default;
  ~TasbihStore() = default;

  friend class PersistableStore<TasbihStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/tasbih.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Folds the day that just ended into the aggregates and resets today's
  // count if the calendar day (or year) has changed since last recorded.
  // No-op if the clock has never been synced (TimeUtils::todayOrdinal()==0)
  // -- never rolls over based on an invalid/epoch-0 read. Call on entering
  // the app (so a stale count from a previous day is never shown) and at the
  // top of increment() (covers a session left open across midnight).
  void ensureCurrentDay();

  void increment();
  // Manual "start over" -- zeroes today's count immediately (unlike
  // increment(), a deliberate, infrequent action so saving right away is
  // fine). Does NOT touch maxSingleDayCount/yearTotal: those only fold in a
  // day's count at rollover, so whatever the count is when the day actually
  // ends is what counts toward them -- resetting mid-day simply means the
  // pre-reset taps aren't carried forward, matching "start fresh" intent.
  void resetToday();

  uint32_t getTodayCount() const { return todayCount; }
  uint32_t getMaxSingleDayCount() const;
  uint32_t getYearTotal() const;
};

#define TASBIH TasbihStore::getInstance()
