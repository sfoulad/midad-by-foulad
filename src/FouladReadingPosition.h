#pragma once

#include <cstdint>
#include <string>

/**
 * Cross-device reading position against Foulad eBooks (EINK_PAGE_SYNC_TASKS.md).
 *
 * Separate from FouladDeviceTracking's /opds/reading-stats, deliberately: that one
 * is PER DEVICE and answers "how long was this X3 read for", feeding the dashboard.
 * This one is PER ACCOUNT and answers "where am I in this book". They do not share
 * a row, and both are sent on close.
 *
 * The shared currency is progress_percent. A page number computed for a 5-inch
 * e-ink panel is not something a phone paginating with epub.js can act on, so page
 * and totalPages travel for display only and never drive a jump.
 */
namespace FouladReadingPosition {

struct Position {
  bool hasPosition = false;
  float progressPercent = 0.0f;
  // -1 when the other end set the position (a phone has no page number to give).
  int page = -1;
  int totalPages = -1;
  std::string source;      // "app" | "reader"
  std::string deviceName;  // safe to show in the prompt, and far more convincing
                           // than "another device"
  // Server's decision, not ours. It carries a slack allowance so it never proposes
  // a jump to where you already are -- two renderers computing a position from the
  // same place never agree to the decimal. Deliberately not re-derived here: if the
  // rule changes, a firmware release is a slow way to fix a float comparison.
  // Absent on a GET, so it reads as false there.
  bool shouldJump = false;
};

// Reports this device's position and returns the account's in one round trip.
// Use when the device has something honest to say -- the drawer's Sync item, and
// closing a book that was actually read.
//
// readAtEpochSeconds: when the person was ON that page, not when a connection was
// found. 0 omits the field, which makes the server stamp arrival time. Pass 0
// rather than a guess: neither device can preserve a calendar date across a reboot
// (HalClock.h), so an invented timestamp is worse than none.
// readAtAgeSeconds: how long ago the page turn happened. PREFERRED over the absolute
// time -- the server subtracts it from its own clock, so the result is indistinguishable
// from a real timestamp and the device never needs to know the date. If both are sent
// the age wins, which is why an unset RTC's 1970 alongside a good age does no harm.
// 0 omits it, and omitting is now correct rather than a silent fallback (spec 4.2).
bool sync(const std::string& username, const std::string& password, const std::string& fouladBookId,
          float progressPercent, int page, int totalPages, uint32_t readAtEpochSeconds, uint32_t readAtAgeSeconds,
          Position& out);

// Looks without reporting.
//
// This is what opening a book must use when the device has not been reading it.
// Posting page 1 there would drag the account back to the start of a book somebody
// is halfway through on their phone -- the one way this feature can actively make
// things worse.
bool fetch(const std::string& username, const std::string& password, const std::string& fouladBookId, Position& out);

}  // namespace FouladReadingPosition
