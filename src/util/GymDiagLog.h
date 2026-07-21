#pragma once

#include "RollingSdLog.h"

#include <string>

// Rolling diagnostic log for the Gym app (/gym_diag_log.txt): catalog sync
// results, per-exercise asset download outcomes, and workout session
// timing/errors. A user reporting "catalog won't download" or "workout
// screen looks wrong" has no serial cable to show us what actually happened
// -- this puts the same breakdown on the SD card, mirroring SleepDiagLog/
// BatteryDiagLog's approach (same shared RollingSdLog: debug-gated,
// heap-guarded, bounded line count).
namespace GymDiagLog {

constexpr char PATH[] = "/gym_diag_log.txt";
constexpr size_t MAX_LINES = 100;

inline void append(const std::string& line) { RollingSdLog::append(PATH, line, MAX_LINES); }

}  // namespace GymDiagLog
