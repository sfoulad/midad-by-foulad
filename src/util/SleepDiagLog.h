#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

// Rolling diagnostic log for what keeps resetting the auto-sleep inactivity timer
// (/sleep_diag_log.txt). A user reporting "auto-sleep never triggers" has no serial
// cable to show us which of button/tilt/activity-block is actually firing -- this
// puts the same breakdown on the SD card instead.
namespace SleepDiagLog {

constexpr char PATH[] = "/sleep_diag_log.txt";
constexpr size_t MAX_LINES = 80;  // bounds the read-modify-write cost below

// Appends one line. Read-modify-write since HalStorage has no append mode;
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

}  // namespace SleepDiagLog
