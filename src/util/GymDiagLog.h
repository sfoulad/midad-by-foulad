#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// Rolling diagnostic log for the Gym app, tagged into the shared
// /debug_log.txt (see DebugLog.h): catalog sync results, per-exercise asset
// download outcomes, and workout session timing/errors. A user reporting
// "catalog won't download" or "workout screen looks wrong" has no serial
// cable to show us what actually happened -- this puts the same breakdown on
// the SD card.
namespace GymDiagLog {

inline void append(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[GYM] " + line, DebugLog::MAX_LINES);
}

}  // namespace GymDiagLog
