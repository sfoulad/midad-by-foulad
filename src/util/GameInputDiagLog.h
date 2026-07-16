#pragma once

#include "RollingSdLog.h"

#include <string>

// Rolling diagnostic log for in-game button input (/game_input_diag_log.txt).
// A user reporting "buttons not working in Snake" (game keeps auto-stepping, but
// direction/pause presses do nothing) has no serial cable to show us whether
// wasPressed() is even seeing the press, or whether it fires but a turn's guard
// condition silently rejects it -- this puts that breakdown on the SD card instead.
namespace GameInputDiagLog {

constexpr char PATH[] = "/game_input_diag_log.txt";
constexpr size_t MAX_LINES = 80;  // bounds the read-modify-write cost below

inline void append(const std::string& line) { RollingSdLog::append(PATH, line, MAX_LINES); }

}  // namespace GameInputDiagLog
