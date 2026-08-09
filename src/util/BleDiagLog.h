#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// Rolling diagnostic log for BlePeripheralManager's lifecycle (state transitions, and
// why it stays off when it wanted to start), tagged into the shared /debug_log.txt (see
// DebugLog.h). lib/hal/BlePeripheralManager.cpp itself can't write here directly --
// lib/hal/ never includes src/ headers in this codebase (see BlePeripheralManager.h's
// own comment) -- so main.cpp, which already calls begin()/poll() and has the heap
// figures on hand, logs on its behalf. Mirrors SleepDiagLog/WifiDiagLog's rationale: a
// user reporting "the BT indicator never showed up" has no serial cable to show us
// whether the radio ever actually started, or why not.
namespace BleDiagLog {

inline void append(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[BLE] " + line, DebugLog::MAX_LINES);
}

}  // namespace BleDiagLog
