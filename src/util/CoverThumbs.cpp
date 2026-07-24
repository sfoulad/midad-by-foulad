#include "CoverThumbs.h"

#include <Arduino.h>
#include <Logging.h>

#include <cstdio>
#include <set>

#include "util/DebugLog.h"
#include "util/RollingSdLog.h"

namespace {
// Bounded in practice by the recents list (10 books) x the screens that
// generate thumbs -- a handful of short strings, not a growth concern.
std::set<std::string>& attemptedSet() {
  static std::set<std::string> attempted;
  return attempted;
}
}  // namespace

namespace CoverThumbs {

bool wasAttemptedThisBoot(const std::string& thumbPath) { return attemptedSet().count(thumbPath) > 0; }

void markAttempted(const std::string& thumbPath) { attemptedSet().insert(thumbPath); }

void diagLog(const std::string& line) {
  LOG_INF("COVER", "%s", line.c_str());

  char prefix[80];
  snprintf(prefix, sizeof(prefix), "[COVER] [%lus heap=%u block=%u] ", millis() / 1000UL,
           static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  RollingSdLog::append(DebugLog::PATH, prefix + line, DebugLog::MAX_LINES);
}

}  // namespace CoverThumbs
