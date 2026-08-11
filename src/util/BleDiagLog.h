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
//
// Best-effort, not guaranteed: RollingSdLog::append() silently skips the write below
// RollingSdLog::MIN_SAFE_HEAP_BYTES (32 KB) free heap, and NimBLE's own resident cost
// can leave meaningfully less than that free while BLE is up -- a transition logged
// right after a successful begin() may not actually reach the SD card. That's accepted
// here rather than worked around: lowering the heap floor, forcing the write, or
// building a small pending-record-flushed-later mechanism would all trade a diagnostic
// nicety for real stability risk, which this log exists to help debug, not to add to.
namespace BleDiagLog {

inline void append(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[BLE] " + line, DebugLog::MAX_LINES);
}

}  // namespace BleDiagLog
