#include "CoverThumbs.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <set>

namespace {
// Bounded in practice by the recents list (10 books) x the screens that
// generate thumbs -- a handful of short strings, not a growth concern.
std::set<std::string>& attemptedSet() {
  static std::set<std::string> attempted;
  return attempted;
}

constexpr char DIAG_PATH[] = "/cover_diag_log.txt";
constexpr size_t DIAG_MAX_BYTES = 6 * 1024;  // drop the oldest half beyond this

std::string& diagBuffer() {
  static std::string buffer;
  return buffer;
}
}  // namespace

namespace CoverThumbs {

bool wasAttemptedThisBoot(const std::string& thumbPath) { return attemptedSet().count(thumbPath) > 0; }

void markAttempted(const std::string& thumbPath) { attemptedSet().insert(thumbPath); }

void diagLog(const std::string& line) {
  std::string& buffer = diagBuffer();
  if (buffer.empty()) {
    buffer = "Cover diagnostic log -- CrossPoint version " CROSSPOINT_VERSION "\n";
  }

  char prefix[64];
  snprintf(prefix, sizeof(prefix), "[%lus heap=%u block=%u] ", millis() / 1000UL,
           static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  buffer += prefix;
  buffer += line;
  buffer += '\n';

  if (buffer.size() > DIAG_MAX_BYTES) {
    buffer.erase(0, buffer.size() / 2);
  }

  HalFile file;
  if (Storage.openFileForWrite("COVER", DIAG_PATH, file)) {
    file.write(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size());
  }
  LOG_INF("COVER", "%s", line.c_str());
}

}  // namespace CoverThumbs
