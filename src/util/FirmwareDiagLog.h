#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// One-shot diagnostic log for firmware update outcomes -- OTA-over-WiFi (OtaUpdater.cpp,
// OtaUpdateActivity.cpp) and manual SD-card update (SdFirmwareUpdateActivity.cpp), both
// funneling through the same low-level writer (FirmwareFlasher.cpp) -- tagged into the
// shared /debug_log.txt (see DebugLog.h). The existing LOG_ERR/LOG_DBG calls across these
// files are compiled OUT entirely in release builds (LOG_LEVEL=1 in gh_release/gh_release_rc,
// platformio.ini) and even in a debug build only survive a 16-line RTC ring buffer -- a
// failed update (bricking risk, the highest-consequence failure class in this firmware)
// has had no durable trace at all.
//
// force=true unconditionally (see RollingSdLog::append): every call site here is a one-shot
// terminal outcome (validation/flash success or failure, not routine per-chunk progress),
// so it survives regardless of the debug-logging toggle -- mirrors the crash-report/
// heap-guard carve-out, for the same reason: this is exactly the kind of event a user
// hasn't turned debug logging on ahead of expecting.
namespace FirmwareDiagLog {

inline void append(const char* tag, const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[" + std::string(tag) + "] " + line, DebugLog::MAX_LINES, /*force=*/true);
}

}  // namespace FirmwareDiagLog
