#pragma once

#include <Arduino.h>
#include <HalStorage.h>
#include <Memory.h>

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

// Hard ceiling on any rolling log, independent of the line count. A line cap alone is
// only as good as the assumption that lines are short; a subsystem emitting long lines
// (or few newlines) can outgrow it badly, and the file every hardware diagnosis depends
// on is also the one competing for the SD I/O and heap the reader needs. Enforced by the
// same streaming trim as the line cap, so it costs no extra pass.
constexpr size_t MAX_BYTES = 64 * 1024;

// Keeps only the last `maxLines` lines (and at most `maxBytes`) of the file, streaming
// through a small fixed buffer and rewriting via a temp file + rename.
//
// The previous implementation read the whole file into a String and rebuilt it as a
// std::string, so it guarded itself with `getMaxAllocHeap() < knownSize * 2 + 256`. That
// guard made the cap unenforceable by construction: trimming is only triggered once the
// file passes (maxLines + TRIM_SLACK_LINES) * ASSUMED_BYTES_PER_LINE bytes -- 54144 for
// the 500-line debug log -- while a device whose largest free block peaks near 49KB can
// only ever satisfy `knownSize * 2` for files under ~24KB. Every file large enough to
// need trimming was therefore too large to trim, and each append made the gap wider. One
// real device reached 16.8MB / 193782 lines that way.
//
// Streaming removes the file-sized allocations entirely, so the only heap this needs is
// the fixed buffer below and the cap holds at any file size.
inline void trim(const char* path, size_t maxLines, size_t maxBytes) {
  constexpr size_t CHUNK = 256;
  auto buf = makeUniqueNoThrow<char[]>(CHUNK);
  if (!buf) return;  // trimmed on a later, healthier call

  uint64_t keepFrom = 0;
  uint64_t size = 0;
  {
    HalFile in;
    if (!Storage.openFileForRead("RSDLOG", path, in)) return;
    size = in.fileSize64();
    if (size == 0) return;

    // Pass 1: count lines. A trailing line with no final newline still counts.
    uint64_t totalLines = 0;
    bool sawByte = false, endsWithNewline = false;
    for (;;) {
      const int n = in.read(buf.get(), CHUNK);
      if (n <= 0) break;
      sawByte = true;
      for (int i = 0; i < n; ++i) {
        if (buf[i] == '\n') totalLines++;
      }
      endsWithNewline = (buf[n - 1] == '\n');
    }
    if (!sawByte) return;
    if (!endsWithNewline) totalLines++;

    const bool overLines = totalLines > maxLines;
    const bool overBytes = maxBytes != 0 && size > maxBytes;
    if (!overLines && !overBytes) return;

    // Pass 2: byte offset of the first line to keep. Both bounds are applied, and the
    // byte bound is snapped forward to a line start so the survivor is never a partial
    // line -- a pathological writer emitting few newlines is bounded by bytes alone.
    const uint64_t skipLines = overLines ? totalLines - maxLines : 0;
    const uint64_t byteFloor = overBytes ? size - maxBytes : 0;

    if (!in.seek64(0)) return;
    uint64_t pos = 0, skipped = 0;
    bool lineBoundPassed = (skipLines == 0);
    for (;;) {
      const int n = in.read(buf.get(), CHUNK);
      if (n <= 0) break;
      for (int i = 0; i < n; ++i) {
        pos++;
        if (buf[i] != '\n') continue;
        if (!lineBoundPassed && ++skipped >= skipLines) {
          lineBoundPassed = true;
          keepFrom = pos;
        }
        // Once both bounds are satisfied the offset is final.
        if (lineBoundPassed && pos >= byteFloor) {
          if (pos > keepFrom) keepFrom = pos;
          goto found;
        }
      }
    }
  found:;
    if (keepFrom == 0 || keepFrom >= size) return;
  }

  // Rewrite through a sibling temp file, then swap. Nothing is destroyed until the new
  // content is fully written and closed.
  std::string tmpPath = std::string(path) + ".tmp";
  Storage.remove(tmpPath.c_str());
  {
    HalFile in;
    if (!Storage.openFileForRead("RSDLOG", path, in)) return;
    if (!in.seek64(keepFrom)) return;
    HalFile out = Storage.open(tmpPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (!out) return;
    for (;;) {
      const int n = in.read(buf.get(), CHUNK);
      if (n <= 0) break;
      if (out.write(reinterpret_cast<const uint8_t*>(buf.get()), static_cast<size_t>(n)) != static_cast<size_t>(n)) {
        out.close();
        Storage.remove(tmpPath.c_str());
        return;  // partial write: leave the original untouched
      }
    }
    out.close();
    in.close();
  }
  if (!Storage.remove(path)) {
    Storage.remove(tmpPath.c_str());
    return;
  }
  Storage.rename(tmpPath.c_str(), path);
}

// Fixed-buffer variant: writes exactly `len` bytes from a caller-owned buffer, so it
// allocates nothing of its own and is deliberately NOT subject to MIN_SAFE_HEAP_BYTES.
// That floor exists because a std::string growing under -fno-exceptions aborts, and
// there is no string on this path -- while the events most worth recording (the
// heap-guard trip points in OpdsBookBrowserActivity/OpdsCoverCache) fire precisely
// below it, so routing them through append() means the one line worth having is the
// one that never gets written. Format into a stack buffer with snprintf and call this.
//
// trim() streams, so it needs no file-sized allocation and cannot turn a guaranteed
// line into an abort.
inline void appendRaw(const char* path, const char* line, size_t len, size_t maxLines, bool force = false,
                      size_t maxBytes = MAX_BYTES) {
  if ((!force && !DebugLogging::enabled()) || maxLines == 0 || !line) return;

  HalFile file = Storage.open(path, O_WRITE | O_CREAT | O_APPEND);
  if (!file) return;
  file.write(line, len);
  file.write(static_cast<uint8_t>('\n'));
  const size_t sizeAfter = file.fileSize();
  // Close before trim() below reopens the same path for read then write (DESTRUCTOR_CLOSES_FILE
  // handles this file's own eventual scope exit, but that's too late for trim()'s reopen).
  file.close();

  const size_t trimTriggerBytes = (maxLines + TRIM_SLACK_LINES) * ASSUMED_BYTES_PER_LINE;
  if (sizeAfter > trimTriggerBytes || (maxBytes != 0 && sizeAfter > maxBytes)) {
    trim(path, maxLines, maxBytes);
  }
}

// force=true bypasses the debugLoggingEnabled gate (still subject to maxLines and the
// heap-safety floor above). Mirrors CrossPointSettings::debugLoggingEnabled's own
// carve-out for crash_report.txt: routine per-subsystem chatter stays opt-in so ordinary
// users don't get SD clutter, but a one-shot event that heads off or explains an actual
// crash must survive regardless -- those are exactly the sessions where nobody thought to
// turn debug logging on ahead of time. Use sparingly: reserved for panic-adjacent
// diagnostics (heap-guard trip points, crash summaries), not routine subsystem logging.
inline void append(const char* path, const std::string& line, size_t maxLines, bool force = false,
                   size_t maxBytes = MAX_BYTES) {
  if ((!force && !DebugLogging::enabled()) || maxLines == 0 || ESP.getFreeHeap() < MIN_SAFE_HEAP_BYTES) return;
  appendRaw(path, line.data(), line.size(), maxLines, force, maxBytes);
}

}  // namespace RollingSdLog
