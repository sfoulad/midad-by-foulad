#pragma once

// Host-test stand-in for lib/Logging/Logging.h that CAPTURES instead of
// discarding. CoverThumbs::probeThumb() is required to emit exactly one
// "[COVER] <FAULT-NAME> ..." line per fault -- no more (a caller logging the
// same fault again doubles every cover failure in a capture) and no fewer
// (CLAUDE.md: always log before an error return). That is a property about the
// NUMBER of lines, so the tests need to count them, which a no-op sink cannot
// support.

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace test_log {

struct Line {
  std::string level;   // "ERR" / "INF" / "DBG"
  std::string origin;  // the subsystem tag, e.g. "COVER"
  std::string text;    // the formatted message
};

inline std::vector<Line>& lines() {
  static std::vector<Line> captured;
  return captured;
}

inline void clear() { lines().clear(); }

__attribute__((format(printf, 3, 4))) inline void record(const char* level, const char* origin, const char* format,
                                                         ...) {
  char buf[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  lines().push_back(Line{level, origin, buf});
}

// True when `needle` appears in the one and only captured line.
inline bool onlyLineContains(const std::string& needle) {
  return lines().size() == 1 && lines()[0].text.find(needle) != std::string::npos;
}

}  // namespace test_log

#define LOG_ERR(origin, ...) test_log::record("ERR", origin, __VA_ARGS__)
#define LOG_INF(origin, ...) test_log::record("INF", origin, __VA_ARGS__)
#define LOG_DBG(origin, ...) test_log::record("DBG", origin, __VA_ARGS__)
