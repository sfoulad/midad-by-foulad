#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// Rolling diagnostic log for chapter indexing/build time, tagged into the
// shared /debug_log.txt (see DebugLog.h). The in-app serial log ("Building
// section...", "Page render (tiled): ...") already carries this, but a user
// reporting slowness by photo has no way to pull serial output -- this puts
// the same numbers on the SD card so a report like "Quran page turns feel
// slow" comes back with real timings instead of requiring a repro session
// over USB.
namespace ReaderPerfLog {

inline void append(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[READERPERF] " + line, DebugLog::MAX_LINES);
}

}  // namespace ReaderPerfLog
