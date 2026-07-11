#include "TimeUtils.h"

#include <HalClock.h>
#include <I18n.h>

#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"

namespace {
// Anything before this (2020-09-13) means the clock was never NTP-synced.
constexpr uint32_t MIN_PLAUSIBLE_EPOCH = 1600000000U;

const char* monthName(const unsigned month) {
  static constexpr StrId kMonths[12] = {StrId::STR_MONTH_JAN, StrId::STR_MONTH_FEB, StrId::STR_MONTH_MAR,
                                        StrId::STR_MONTH_APR, StrId::STR_MONTH_MAY, StrId::STR_MONTH_JUN,
                                        StrId::STR_MONTH_JUL, StrId::STR_MONTH_AUG, StrId::STR_MONTH_SEP,
                                        StrId::STR_MONTH_OCT, StrId::STR_MONTH_NOV, StrId::STR_MONTH_DEC};
  return (month >= 1 && month <= 12) ? I18N.get(kMonths[month - 1]) : "";
}
}  // namespace

namespace TimeUtils {

#ifdef SIMULATOR
// The simulator's HalClock stub has no isSystemTimeValid(); the host clock is
// always trustworthy.
bool isClockValid() { return true; }
#else
bool isClockValid() { return HalClock::isSystemTimeValid(); }
#endif

bool isClockValid(const uint32_t epochSeconds) { return epochSeconds >= MIN_PLAUSIBLE_EPOCH; }

int32_t localOffsetSeconds() {
  // clockUtcOffsetQ is a biased quarter-hour offset: 48 = UTC+0.
  return (static_cast<int32_t>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60;
}

uint32_t getLocalDayOrdinal(const uint32_t epochSeconds) {
  if (!isClockValid(epochSeconds)) {
    return 0;
  }
  const int64_t localEpoch = static_cast<int64_t>(epochSeconds) + localOffsetSeconds();
  return localEpoch > 0 ? static_cast<uint32_t>(localEpoch / 86400) : 0;
}

uint32_t todayOrdinal() {
  if (!isClockValid()) {
    return 0;
  }
  return getLocalDayOrdinal(static_cast<uint32_t>(time(nullptr)));
}

// Howard Hinnant's days_from_civil / civil_from_days (public domain algorithms).
uint32_t getDayOrdinalForDate(int year, const unsigned month, const unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
  return days > 0 ? static_cast<uint32_t>(days) : 0;
}

bool getDateFromDayOrdinal(const uint32_t dayOrdinal, int& year, unsigned& month, unsigned& day) {
  if (dayOrdinal == 0) {
    return false;
  }
  // z is always positive here (dayOrdinal >= 1), so the negative-era branch of
  // the original algorithm is dropped.
  const int64_t z = static_cast<int64_t>(dayOrdinal) + 719468;
  const int64_t era = z / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year = static_cast<int>(y + (month <= 2));
  return true;
}

unsigned daysInMonth(const int year, const unsigned month) {
  static constexpr unsigned DAYS_PER_MONTH[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29U : 28U;
  }
  return (month >= 1 && month <= 12) ? DAYS_PER_MONTH[month - 1] : 30U;
}

void formatMonthYear(const int year, const unsigned month, char* buf, const size_t len) {
  snprintf(buf, len, "%s %d", monthName(month), year);
}

void formatDayOrdinal(const uint32_t dayOrdinal, char* buf, const size_t len) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (!getDateFromDayOrdinal(dayOrdinal, year, month, day)) {
    if (len > 0) buf[0] = '\0';
    return;
  }
  snprintf(buf, len, "%u %s %d", day, monthName(month), year);
}

}  // namespace TimeUtils
