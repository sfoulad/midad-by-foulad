#include "CoverThumbs.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <HalStorage.h>
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

// Emits the single greppable line for a probe fault and hands the fault back,
// so every `return` in probeThumb() logs exactly once and no caller has to
// remember to do it. Missing stays at debug level: "this thumb has not been
// generated yet" is the normal first-visit state that the generation pass this
// feeds then heals, not an error worth a red line on every fresh book.
CoverDiag::Fault logProbeFault(const CoverDiag::Fault fault, const std::string& thumbPath, const int expectedHeight,
                               const char* reason) {
  if (fault == CoverDiag::Fault::Missing) {
    LOG_DBG("COVER", "%s %s want=%d", CoverDiag::faultName(fault), thumbPath.c_str(), expectedHeight);
  } else {
    LOG_ERR("COVER", "%s %s want=%d (%s)", CoverDiag::faultName(fault), thumbPath.c_str(), expectedHeight,
            reason != nullptr ? reason : "");
  }
  return fault;
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

CoverDiag::Fault probeThumb(const std::string& thumbPath, const int expectedHeight, char* detail,
                            const size_t detailLen) {
  if (detail != nullptr && detailLen > 0) detail[0] = '\0';
  // Built once per fault and then copied into the caller's optional `detail`,
  // so the logged reason and the returned reason can never disagree.
  char reason[48] = {0};
  const auto publish = [&](const CoverDiag::Fault fault) {
    if (detail != nullptr && detailLen > 0) snprintf(detail, detailLen, "%s", reason);
    return logProbeFault(fault, thumbPath, expectedHeight, reason);
  };

  if (thumbPath.empty()) return publish(CoverDiag::Fault::NoPath);

  HalFile file;
  if (!Storage.openFileForRead("COVER", thumbPath, file)) {
    return publish(CoverDiag::Fault::Missing);
  }
  Bitmap bitmap(file);
  const BmpReaderError err = bitmap.parseHeaders();
  if (err != BmpReaderError::Ok) {
    snprintf(reason, sizeof(reason), "%s", Bitmap::errorToString(err));
    return publish(CoverDiag::Fault::Invalid);
  }
  if (bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    snprintf(reason, sizeof(reason), "degenerate dimensions %dx%d", bitmap.getWidth(), bitmap.getHeight());
    return publish(CoverDiag::Fault::Invalid);
  }
  if (expectedHeight > 0 && bitmap.getHeight() != expectedHeight) {
    snprintf(reason, sizeof(reason), "found %dx%d", bitmap.getWidth(), bitmap.getHeight());
    return publish(CoverDiag::Fault::StaleSize);
  }
  return CoverDiag::Fault::None;
}

bool isUsableThumb(const std::string& thumbPath, const int expectedHeight) {
  // probeThumb() has already emitted the one line naming whichever fault it
  // found, so this only decides usability -- logging here too would double every
  // cover fault in the capture.
  switch (probeThumb(thumbPath, expectedHeight, nullptr, 0)) {
    case CoverDiag::Fault::None:
      return true;
    case CoverDiag::Fault::StaleSize:
      // Reported but NOT rejected. The thumb converters are height-driven yet crop
      // to whichever axis binds, so a legitimate thumb can land a pixel short of
      // (or taller than) the requested height, and XTC's small-page path copies
      // cover.bmp at its native size on purpose. The cover box already scales any
      // height to fit (CoverDiag::fitCoverIntoBox), so rejecting on size would
      // re-run generation -- loading popup included -- on every boot for a file
      // that draws correctly. Naming it in the log is what separates a genuinely
      // stale cache from a missing or corrupt one.
      return true;
    default:
      return false;
  }
}

void reportFault(const char* module, const CoverDiag::Fault fault, const std::string& thumbPath,
                 const int expectedHeight, const char* detail) {
  if (fault == CoverDiag::Fault::None) return;
  const char* reason = detail != nullptr ? detail : "";
  LOG_ERR("COVER", "%s %s %s want=%d (%s)", module, CoverDiag::faultName(fault), thumbPath.c_str(), expectedHeight,
          reason);
  // Same line into the shared SD debug log, so a user who cannot attach a serial
  // cable can still hand over the reason a cover fell back to the placeholder.
  char buf[96];
  snprintf(buf, sizeof(buf), "%s %s want=%d (%s) ", module, CoverDiag::faultName(fault), expectedHeight, reason);
  diagLog(std::string(buf) + thumbPath);
}

}  // namespace CoverThumbs
