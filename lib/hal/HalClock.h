#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // True if the system clock (time(nullptr)) looks like a real calendar date rather
  // than the ESP-IDF post-boot default (epoch 0 / Jan 1 1970). Device-independent --
  // works whether or not a DS3231 is present.
  static bool isSystemTimeValid();

  // Sets the system clock via SNTP, independent of any RTC hardware. Blocks for up
  // to ~5s while waiting for SNTP response. Requires WiFi to be connected.
  // Needed because neither the DS3231 (hour/minute only, no calendar date) nor X4
  // (no RTC at all) can preserve the date across a reboot -- without re-syncing,
  // mbedTLS certificate validation silently fails on every HTTPS request (OPDS,
  // OTA checks, font downloads) until something calls this. Safe/cheap to call
  // whenever isSystemTimeValid() is false; a no-op if WiFi isn't connected yet.
  static bool quickSyncSystemTime();

 private:
  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
};
