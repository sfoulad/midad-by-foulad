// Implements FouladOpdsHooksPure.h -- see that header for why this has zero
// HalStorage/Epub dependency. See test/foulad_opds_hooks_pure/.
#include "FouladOpdsHooksPure.h"

#include <cctype>

#include "FouladEbooksConfig.h"

namespace FouladOpdsHooks {

std::string extractFouladBookId(const std::string& entryId) {
  // The prefix is checked, not just the trailing digits. News entries are
  // "urn:midad:feed:<id>" in a DIFFERENT namespace, and a bare trailing-digit
  // parse turns feed 3 into book 3 -- which is not a missing id, it is somebody
  // else's book. That id is sent to /opds/reading-position and /opds/reading-stats,
  // so the failure mode is a news article silently overwriting the saved position
  // of an unrelated real book. Anything that is not a book entry returns "".
  static constexpr char kBookPrefix[] = "urn:opds-library:book:";
  static constexpr size_t kBookPrefixLen = sizeof(kBookPrefix) - 1;
  if (entryId.compare(0, kBookPrefixLen, kBookPrefix) != 0) return "";

  const std::string tail = entryId.substr(kBookPrefixLen);
  if (tail.empty()) return "";
  for (const char c : tail) {
    if (!isdigit(static_cast<unsigned char>(c))) return "";
  }
  return tail;
}

std::string acquisitionExtension(const std::string& acquisitionType) {
  // foulad-ebooks derives this MIME type from the stored file's own
  // extension, so mirror it back exactly (.xtch vs .xtc) rather than
  // assuming one -- both are recognized identically for local file-type
  // detection (FsHelpers::hasXtcExtension), so either is safe to save as.
  if (acquisitionType == "application/x-xtch") return ".xtch";
  if (acquisitionType == "application/x-xtc") return ".xtc";
  return ".epub";
}

bool isNewsFeed(const std::string& serverUrl) { return serverUrl == FOULAD_EBOOKS_NEWS_URL; }

}  // namespace FouladOpdsHooks
