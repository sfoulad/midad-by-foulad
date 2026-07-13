#pragma once

#include "RollingSdLog.h"

#include <string>

// Rolling diagnostic log for what keeps resetting the auto-sleep inactivity timer
// (/sleep_diag_log.txt). A user reporting "auto-sleep never triggers" has no serial
// cable to show us which of button/tilt/activity-block is actually firing -- this
// puts the same breakdown on the SD card instead.
namespace SleepDiagLog {

constexpr char PATH[] = "/sleep_diag_log.txt";
constexpr size_t MAX_LINES = 80;  // bounds the read-modify-write cost below

inline void append(const std::string& line) { RollingSdLog::append(PATH, line, MAX_LINES); }

}  // namespace SleepDiagLog
