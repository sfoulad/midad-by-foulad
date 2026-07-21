#include "TasbihStore.h"

#include <algorithm>

#include "util/TimeUtils.h"

void TasbihStore::toJson(JsonDocument& doc) const {
  doc["todayCount"] = todayCount;
  doc["todayDayOrdinal"] = todayDayOrdinal;
  doc["maxSingleDayCount"] = maxSingleDayCount;
  doc["yearTotal"] = yearTotal;
  doc["yearCovered"] = yearCovered;
}

bool TasbihStore::fromJson(JsonVariantConst doc) {
  todayCount = doc["todayCount"] | 0;
  todayDayOrdinal = doc["todayDayOrdinal"] | 0;
  maxSingleDayCount = doc["maxSingleDayCount"] | 0;
  yearTotal = doc["yearTotal"] | 0;
  yearCovered = doc["yearCovered"] | 0;
  return true;
}

void TasbihStore::ensureCurrentDay() {
  const uint32_t today = TimeUtils::todayOrdinal();
  if (today == 0 || today == todayDayOrdinal) return;

  // Fold the day that just ended into the aggregates -- but only if a real
  // (clock-valid) day was actually recorded; todayDayOrdinal==0 means "no day
  // recorded yet" (fresh install), never a phantom day to fold in.
  if (todayDayOrdinal != 0 && todayCount > 0) {
    int oldYear = 0;
    unsigned oldMonth = 0, oldDay = 0;
    if (TimeUtils::getDateFromDayOrdinal(todayDayOrdinal, oldYear, oldMonth, oldDay) &&
        static_cast<uint32_t>(oldYear) == yearCovered) {
      yearTotal += todayCount;
    }
    maxSingleDayCount = std::max(maxSingleDayCount, todayCount);
  }

  todayDayOrdinal = today;
  todayCount = 0;

  int newYear = 0;
  unsigned newMonth = 0, newDay = 0;
  if (TimeUtils::getDateFromDayOrdinal(today, newYear, newMonth, newDay) &&
      static_cast<uint32_t>(newYear) != yearCovered) {
    yearTotal = 0;
    yearCovered = static_cast<uint32_t>(newYear);
  }

  saveToFile();
}

void TasbihStore::increment() {
  ensureCurrentDay();
  ++todayCount;
  // Deliberately NOT saved here -- a dhikr session can mean dozens of taps a
  // minute, and writing SD on every single one is exactly what the SPIFFS
  // write-throttling rule warns against. TasbihActivity::onExit() persists
  // once when the app is actually left; ensureCurrentDay() above still saves
  // immediately on a day/year rollover, since those are rare (~daily).
}

void TasbihStore::resetToday() {
  todayCount = 0;
  saveToFile();
}

uint32_t TasbihStore::getMaxSingleDayCount() const { return std::max(maxSingleDayCount, todayCount); }

uint32_t TasbihStore::getYearTotal() const {
  int year = 0;
  unsigned month = 0, day = 0;
  if (todayDayOrdinal != 0 && TimeUtils::getDateFromDayOrdinal(todayDayOrdinal, year, month, day) &&
      static_cast<uint32_t>(year) == yearCovered) {
    return yearTotal + todayCount;
  }
  return yearTotal;
}
