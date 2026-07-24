#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// Rolling diagnostic log for what keeps resetting the auto-sleep inactivity
// timer, tagged into the shared /debug_log.txt (see DebugLog.h). A user
// reporting "auto-sleep never triggers" has no serial cable to show us which
// of button/tilt/activity-block is actually firing -- this puts the same
// breakdown on the SD card instead.
namespace SleepDiagLog {

inline void append(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[SLEEP] " + line, DebugLog::MAX_LINES);
}

}  // namespace SleepDiagLog
