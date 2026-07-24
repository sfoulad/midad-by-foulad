#pragma once

#include <cstddef>

// Single shared SD-card file every rolling diagnostic logger writes into
// (/debug_log.txt), each line tagged with its subsystem (e.g. "[GYM] ...",
// "[SLEEP] ...") so a user asked to help diagnose an issue can pull ONE file
// off the card instead of hunting down 8+ separate *_log.txt files. Backed by
// the same RollingSdLog::append() read-modify-write used before consolidation
// -- concurrent subsystems appending to the same path is safe because each
// call re-reads the current file content rather than holding its own buffer.
namespace DebugLog {

constexpr char PATH[] = "/debug_log.txt";
// Was 5 separate line-bounded logs (100+80+120+80+80=460 lines total) plus
// 2 byte-bounded ones (~6KB each) and 2 one-shot report dumps, now sharing one
// budget -- generous enough that a normal debug session keeps entries from
// every subsystem, at the read-modify-write cost RollingSdLog already bounds.
constexpr size_t MAX_LINES = 500;

}  // namespace DebugLog
