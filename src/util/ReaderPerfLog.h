#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

// Rolling diagnostic log for chapter indexing/build time (/reader_perf_log.txt).
// The in-app serial log ("Building section...", "Page render (tiled): ...")
// already carries this, but a user reporting slowness by photo has no way to
// pull serial output -- this puts the same numbers on the SD card so a report
// like "Quran page turns feel slow" comes back with real timings instead of
// requiring a repro session over USB.
namespace ReaderPerfLog {

constexpr char PATH[] = "/reader_perf_log.txt";
constexpr size_t MAX_LINES = 80;  // bounds the read-modify-write cost below

// Appends one line (chapter switch: cache hit, or build with elapsed ms and
// heap headroom). Read-modify-write since HalStorage has no append mode;
// bounded by MAX_LINES so the cost stays small even after a long session.
inline void append(const std::string& line) {
  String existing = Storage.readFile(PATH);
  std::vector<std::string> lines;
  {
    std::string s(existing.c_str());
    size_t start = 0;
    while (start < s.size()) {
      const size_t nl = s.find('\n', start);
      if (nl == std::string::npos) {
        if (start < s.size()) lines.push_back(s.substr(start));
        break;
      }
      lines.push_back(s.substr(start, nl - start));
      start = nl + 1;
    }
  }
  lines.push_back(line);
  if (lines.size() > MAX_LINES) {
    lines.erase(lines.begin(), lines.begin() + (lines.size() - MAX_LINES));
  }
  std::string out;
  out.reserve(line.size() * lines.size());
  for (const auto& l : lines) {
    out += l;
    out += '\n';
  }
  Storage.writeFile(PATH, out.c_str());
}

}  // namespace ReaderPerfLog
