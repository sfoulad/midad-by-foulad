#include "CoverThumbs.h"

#include <set>

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

}  // namespace CoverThumbs
