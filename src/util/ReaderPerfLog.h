#pragma once

#include "RollingSdLog.h"

#include <string>

// Rolling diagnostic log for chapter indexing/build time (/reader_perf_log.txt).
// The in-app serial log ("Building section...", "Page render (tiled): ...")
// already carries this, but a user reporting slowness by photo has no way to
// pull serial output -- this puts the same numbers on the SD card so a report
// like "Quran page turns feel slow" comes back with real timings instead of
// requiring a repro session over USB.
namespace ReaderPerfLog {

constexpr char PATH[] = "/reader_perf_log.txt";
constexpr size_t MAX_LINES = 80;  // bounds the read-modify-write cost below

inline void append(const std::string& line) { RollingSdLog::append(PATH, line, MAX_LINES); }

}  // namespace ReaderPerfLog
