#pragma once

#include <Arduino.h>
#include <HalStorage.h>

#include <cstring>
#include <string>

#include "util/DebugLogging.h"

// Shared read-modify-write implementation behind ReaderPerfLog, SleepDiagLog,
// and GameInputDiagLog. Read-modify-write since HalStorage has no append mode;
// bounded by maxLines so the cost stays small even after a long session.
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

// force=true bypasses the debugLoggingEnabled gate (still subject to maxLines and the
// heap-safety floor above). Mirrors CrossPointSettings::debugLoggingEnabled's own
// carve-out for crash_report.txt: routine per-subsystem chatter stays opt-in so ordinary
// users don't get SD clutter, but a one-shot event that heads off or explains an actual
// crash must survive regardless -- those are exactly the sessions where nobody thought to
// turn debug logging on ahead of time. Use sparingly: reserved for panic-adjacent
// diagnostics (heap-guard trip points, crash summaries), not routine subsystem logging.
inline void append(const char* path, const std::string& line, size_t maxLines, bool force = false) {
  if ((!force && !DebugLogging::enabled()) || maxLines == 0 || ESP.getFreeHeap() < MIN_SAFE_HEAP_BYTES) return;

  const String existing = Storage.readFile(path);
  const char* data = existing.c_str();
  const size_t len = existing.length();

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

  // Pass 2: find the byte offset of the first line to keep, so the new line
  // still fits within maxLines total without ever materializing per-line copies.
  const size_t keepExisting = totalLines > maxLines - 1 ? maxLines - 1 : totalLines;
  const size_t skipLines = totalLines - keepExisting;
  size_t keepFrom = 0;
  if (skipLines > 0) {
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

  const size_t keptLen = len - keepFrom;
  std::string out;
  out.reserve(keptLen + line.size() + 1);
  out.append(data + keepFrom, keptLen);
  if (!out.empty() && out.back() != '\n') out += '\n';
  out += line;
  out += '\n';

  Storage.writeFile(path, out.c_str());
}

}  // namespace RollingSdLog
