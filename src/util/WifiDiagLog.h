#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// Rolling diagnostic log for WiFi connection attempts (manual and saved-network
// auto-connect), tagged into the shared /debug_log.txt (see DebugLog.h). WiFi connect
// failures are the single most common "it doesn't work" field report this project sees,
// and until now they left no durable trace at all -- WifiSelectionActivity.cpp's
// LOG_DBG("WIFI", ...) calls are compiled OUT entirely in release builds (LOG_LEVEL=1 in
// gh_release/gh_release_rc, platformio.ini) and even in a debug build only survive in a
// 16-line RTC ring buffer. Mirrors SleepDiagLog/BatteryDiagLog's rationale.
namespace WifiDiagLog {

inline void append(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[WIFI] " + line, DebugLog::MAX_LINES);
}

}  // namespace WifiDiagLog
