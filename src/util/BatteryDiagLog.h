#pragma once

#include "RollingSdLog.h"

#include <string>

// Rolling diagnostic log correlating battery level with WiFi radio state and CPU
// power-saving mode over time (/battery_diag_log.txt), sampled periodically (not
// per-call) so a long session doesn't blow through MAX_LINES in minutes. A user
// reporting unusually fast battery drain has no serial cable to show us whether
// WiFi was left on or the CPU was stuck at full frequency -- this puts the same
// breakdown on the SD card instead, mirroring SleepDiagLog's approach.
namespace BatteryDiagLog {

constexpr char PATH[] = "/battery_diag_log.txt";
constexpr size_t MAX_LINES = 120;

inline void append(const std::string& line) { RollingSdLog::append(PATH, line, MAX_LINES); }

}  // namespace BatteryDiagLog
