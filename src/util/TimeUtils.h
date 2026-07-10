#pragma once
#include <cstddef>
#include <cstdint>

// Local-date helpers for reading statistics. All date math uses a flat
// "local days since 1970-01-01" day ordinal (0 = clock never valid), converted
// to/from civil dates with Howard Hinnant's civil-calendar algorithms -- no tm
// structs or timezone database. "Local" means UTC + SETTINGS.clockUtcOffsetQ,
// the same offset the clock display uses.
namespace TimeUtils {

// True when the system clock has been NTP-synced (epoch is plausibly current).
bool isClockValid();
bool isClockValid(uint32_t epochSeconds);

// UTC->local offset in seconds from SETTINGS.clockUtcOffsetQ (48 = UTC+0).
int32_t localOffsetSeconds();

// Local day ordinal for a UTC epoch timestamp; 0 when the input is invalid.
uint32_t getLocalDayOrdinal(uint32_t epochSeconds);

// Today's local day ordinal, or 0 when the clock has never been synced.
uint32_t todayOrdinal();

// Civil date <-> day ordinal (days since 1970-01-01, proleptic Gregorian).
uint32_t getDayOrdinalForDate(int year, unsigned month, unsigned day);
bool getDateFromDayOrdinal(uint32_t dayOrdinal, int& year, unsigned& month, unsigned& day);

// Weekday for a day ordinal, Monday = 0 (1970-01-01 was a Thursday = 3).
inline uint8_t weekdayForOrdinal(const uint32_t dayOrdinal) { return static_cast<uint8_t>((dayOrdinal + 3U) % 7U); }

unsigned daysInMonth(int year, unsigned month);

// "Mar 2026" (translated month name); buf must hold ~24 bytes.
void formatMonthYear(int year, unsigned month, char* buf, size_t len);
// "12 Mar 2026"; empty string when the ordinal is 0. buf must hold ~28 bytes.
void formatDayOrdinal(uint32_t dayOrdinal, char* buf, size_t len);

}  // namespace TimeUtils
