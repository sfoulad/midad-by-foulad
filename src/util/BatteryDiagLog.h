#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// Rolling diagnostic log correlating battery level with WiFi radio state and
// CPU power-saving mode over time, tagged into the shared /debug_log.txt (see
// DebugLog.h), sampled periodically (not per-call) so a long session doesn't
// crowd out other subsystems' entries in minutes. A user reporting unusually
// fast battery drain has no serial cable to show us whether WiFi was left on
// or the CPU was stuck at full frequency -- this puts the same breakdown on
// the SD card instead.
namespace BatteryDiagLog {

inline void append(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[BATTERY] " + line, DebugLog::MAX_LINES);
}

}  // namespace BatteryDiagLog
