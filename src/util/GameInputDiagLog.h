#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// Rolling diagnostic log for in-game button input, tagged into the shared
// /debug_log.txt (see DebugLog.h). A user reporting "buttons not working in
// Snake" (game keeps auto-stepping, but direction/pause presses do nothing)
// has no serial cable to show us whether wasPressed() is even seeing the
// press, or whether it fires but a turn's guard condition silently rejects it
// -- this puts that breakdown on the SD card instead.
namespace GameInputDiagLog {

inline void append(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[GAMEINPUT] " + line, DebugLog::MAX_LINES);
}

}  // namespace GameInputDiagLog
