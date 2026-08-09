#pragma once

#include <Arduino.h>
#include <HalStorage.h>

#include <cstring>
#include <string>

#include "util/DebugLogging.h"

// Shared logger behind ReaderPerfLog, SleepDiagLog, GameInputDiagLog, and the rest of
// DebugLog.h's subsystems. The common case is a cheap O_APPEND write of one line; trimming
// back to maxLines needs a read-modify-write of the whole file and only runs occasionally
// (see trim() below), not on every call.
namespace RollingSdLog {

// Skip the write (rather than crash) when free heap is too tight. Under
// -fno-exceptions, std::string's internal allocation failing calls abort()
// instead of throwing -- a diagnostic log must never be the thing that takes
// the whole device down. Real-device crash report: this append() aborted mid
// std::string growth with heap at ~21KB, driven down by the same page turn's
// font decompression buffers; the previous implementation also made it worse
// by allocating one std::string per existing line (up to 80 of them) on every
// single call, fragmenting the heap further before the growth that aborted.
constexpr uint32_t MIN_SAFE_HEAP_BYTES = 32768;

// A second, later real-device crash report (blank panic reason, free heap a healthy-looking
// 54428 bytes but largest contiguous block only 22516) had this exact function's rebuilt
// std::string sitting in the panic stack dump. MIN_SAFE_HEAP_BYTES above only checks total
// free heap; it says nothing about whether a SINGLE CONTIGUOUS block big enough for the
// whole file (readFile()'s String plus the rebuilt std::string, both roughly file-sized) is
// actually available, which is exactly what a read-modify-write on every call needs and
// exactly what fragmentation starves first. Appending is now a plain O_APPEND write (no
// read, no file-sized allocation); trim() below still does the expensive rewrite, but only
// once the file has grown TRIM_SLACK_LINES past target, and skips itself on a still-tight
// contiguous block rather than risking the abort -- the file is simply left to trim on a
// later, healthier call.
constexpr size_t TRIM_SLACK_LINES = 64;
// Conservative estimate of a tagged log line ("[SUBSYSTEM] ..." + newline), used only to
// decide WHEN trim() should run from a cheap fileSize() check -- not exact byte accounting.
constexpr size_t ASSUMED_BYTES_PER_LINE = 96;

// Re-reads the file, keeps only its last `maxLines` lines, and rewrites it. `knownSize` is
// the file size the caller already measured via fileSize() moments ago (append()'s own
// write) -- checked BEFORE calling readFile() below, since readFile() is itself the first
// of the two large allocations this guard exists to avoid making blind. Trimming only ever
// drops TRIM_SLACK_LINES worth of bytes off a file of roughly knownSize, so `existing` and
// the rebuilt `out` are both roughly knownSize -- two separate allocations that need to
// coexist, hence the 2x (getMaxAllocHeap() only reports the single largest free block, not
// whether a second, similarly-sized block also exists, so this is a heuristic, not a
// guarantee). See TRIM_SLACK_LINES above for why this runs rarely instead of on every
// append(), and why it bails out (leaving the file to trim on a later call) rather than
// force the read-modify-write through a contiguous block too small for it.
inline void trim(const char* path, size_t maxLines, size_t knownSize) {
  if (ESP.getMaxAllocHeap() < knownSize * 2 + 256) return;

  const String existing = Storage.readFile(path);
  const char* data = existing.c_str();
  const size_t len = existing.length();
  if (len == 0) return;

  // Pass 1: count existing lines (a trailing line with no final '\n' still counts).
  size_t totalLines = 0;
  {
    size_t pos = 0;
    while (pos < len) {
      totalLines++;
      const char* nl = static_cast<const char*>(memchr(data + pos, '\n', len - pos));
      if (!nl) break;
      pos = static_cast<size_t>(nl - data) + 1;
    }
  }
  if (totalLines <= maxLines) return;  // grew past the trim trigger but not past maxLines itself

  // Pass 2: find the byte offset of the first line to keep.
  const size_t skipLines = totalLines - maxLines;
  size_t keepFrom = 0;
  {
    size_t pos = 0, skipped = 0;
    while (pos < len && skipped < skipLines) {
      const char* nl = static_cast<const char*>(memchr(data + pos, '\n', len - pos));
      if (!nl) {
        pos = len;
        break;
      }
      pos = static_cast<size_t>(nl - data) + 1;
      skipped++;
    }
    keepFrom = pos;
  }

  std::string out;
  out.reserve(len - keepFrom);
  out.append(data + keepFrom, len - keepFrom);
  Storage.writeFile(path, out.c_str());
}

// force=true bypasses the debugLoggingEnabled gate (still subject to maxLines and the
// heap-safety floor above). Mirrors CrossPointSettings::debugLoggingEnabled's own
// carve-out for crash_report.txt: routine per-subsystem chatter stays opt-in so ordinary
// users don't get SD clutter, but a one-shot event that heads off or explains an actual
// crash must survive regardless -- those are exactly the sessions where nobody thought to
// turn debug logging on ahead of time. Use sparingly: reserved for panic-adjacent
// diagnostics (heap-guard trip points, crash summaries), not routine subsystem logging.
inline void append(const char* path, const std::string& line, size_t maxLines, bool force = false) {
  if ((!force && !DebugLogging::enabled()) || maxLines == 0 || ESP.getFreeHeap() < MIN_SAFE_HEAP_BYTES) return;

  HalFile file = Storage.open(path, O_WRITE | O_CREAT | O_APPEND);
  if (!file) return;
  file.write(line.data(), line.size());
  file.write(static_cast<uint8_t>('\n'));
  const size_t sizeAfter = file.fileSize();
  // Close before trim() below reopens the same path for read then write (DESTRUCTOR_CLOSES_FILE
  // handles this file's own eventual scope exit, but that's too late for trim()'s reopen).
  file.close();

  const size_t trimTriggerBytes = (maxLines + TRIM_SLACK_LINES) * ASSUMED_BYTES_PER_LINE;
  if (sizeAfter > trimTriggerBytes) {
    trim(path, maxLines, sizeAfter);
  }
}

}  // namespace RollingSdLog
