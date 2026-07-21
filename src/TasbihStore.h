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

  // Commits todayCount into maxSingleDayCount/yearTotal (same fold rule
  // ensureCurrentDay() uses for a real day rollover). Shared by
  // ensureCurrentDay() (day/year change) and resetToday() (manual reset) so
  // a mid-day reset can't make the live getters' displayed figures drop --
  // see resetToday()'s own comment for why that matters.
  void foldTodayIntoAggregates();

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
  // Manual "start over" -- folds today's count-so-far into
  // maxSingleDayCount/yearTotal (same as a real day rollover would), then
  // zeroes today's count. Folding first is required: getMaxSingleDayCount()/
  // getYearTotal() add the live todayCount on top of the persisted fields, so
  // simply zeroing it without folding first made those figures visibly drop
  // the instant the user pressed reset -- read as "reset also reset Best
  // Day/Total this year" even though the persisted fields themselves never
  // changed (user report). Folding first means a reset only restarts the
  // on-screen tally; credit already tapped today is never lost.
  void resetToday();

  uint32_t getTodayCount() const { return todayCount; }
  uint32_t getMaxSingleDayCount() const;
  uint32_t getYearTotal() const;
};

#define TASBIH TasbihStore::getInstance()
