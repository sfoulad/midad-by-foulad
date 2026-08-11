#include "FouladOpdsHooks.h"

#include <Epub.h>
#include <HalStorage.h>
#include <WiFi.h>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "RecentBooksStore.h"

namespace {
// Owned here, not by the caller (see FouladOpdsHooks.h's pollDeviceTracking()
// comment) -- persists for the program's lifetime rather than resetting each
// time a new OpdsBookBrowserActivity is constructed.
constexpr unsigned long DEVICE_TRACKING_RECHECK_MS = 30000;
unsigned long lastDeviceTrackingCheckMs = 0;
}  // namespace

namespace FouladOpdsHooks {

void reportDeviceTrackingOnConnect(const OpdsServer& server) {
  // Not gated on server.url below like the rest of this function: KOReader
  // Sync's server is independently configured (any KOSync-compatible host,
  // not necessarily midad.one), so this is worth attempting on WiFi coming up
  // for ANY OPDS server, Foulad eBooks or not.
  FouladDeviceTracking::flushPendingKOReaderSync();

  if (server.url != FOULAD_EBOOKS_URL) return;
  FouladDeviceTracking::registerDevice(server.username, server.password);
  // Reading itself never keeps WiFi connected, so this is the one reliable
  // moment to sync any progress accumulated since the last time the device
  // was online.
  FouladDeviceTracking::flushPendingReadingStats(server.username, server.password);
  // Crash reports go the same way, and for a stronger reason: until now the only
  // path was the user tapping Send on the crash screen. A device that panics and
  // reboots cleanly never shows that screen, so nobody taps it -- which is why
  // zero crash reports have ever reached the server while nine debug logs have.
  // The very failures worth seeing were the ones that reported nothing.
  //
  // Queued instead: the report sits on the SD card until the next time the device
  // is online with an account, then uploads unattended and is deleted so it is
  // sent once. Unconditional -- unlike the debug log below it, this is not gated
  // on Settings -> Apps -> Debug, because a crash is not routine logging and the
  // people whose devices crash are the least likely to have opted into anything.
  FouladDeviceTracking::flushPendingCrashReport(server.username, server.password);
  // Same reasoning: uploading the debug log here (rather than at the moment
  // a book is opened, when WiFi is already gone) is what actually delivers
  // it. No-op unless Settings -> Apps -> Debug is on.
  FouladDeviceTracking::uploadDebugLog(server.username, server.password);
  // Device/reading-stats snapshot for the "My Devices" web page's Device
  // Stats / Reading Stats tabs -- same reliable-moment reasoning.
  FouladDeviceTracking::reportDeviceStats(server.username, server.password);
}

void pollDeviceTracking(const OpdsServer& server, const bool isBrowsing) {
  const unsigned long nowMs = millis();
  const bool shouldPoll =
      shouldPollDeviceTracking(isBrowsing, server.url == FOULAD_EBOOKS_URL, WiFi.status() == WL_CONNECTED, nowMs,
                               lastDeviceTrackingCheckMs, DEVICE_TRACKING_RECHECK_MS);
  if (!shouldPoll) return;
  lastDeviceTrackingCheckMs = nowMs;
  FouladDeviceTracking::registerDevice(server.username, server.password);
}

void removeExistingNewsDownload(const std::string& filename) {
  if (!Storage.exists(filename.c_str())) return;
  Storage.remove(filename.c_str());
  // The cache directory is keyed on the FILE PATH (Epub.h: "epub_" +
  // hash(filepath)) and is only validated against the cache format version --
  // never against the file's size or contents. A feed keeps the same path
  // every time, so without this the reader would find yesterday's book.bin
  // and rendered sections still valid and read the old articles out of
  // cache, no matter how fresh the download was. Progress goes too, which is
  // correct: page 4 of yesterday's articles means nothing in today's.
  const std::string cacheDir = Epub(filename, "/.crosspoint").getCachePath();
  if (Storage.exists(cacheDir.c_str())) Storage.removeDir(cacheDir.c_str());
}

void tagFouladBookId(const std::string& filename, const OpdsEntry& book, const OpdsServer& server) {
  if (server.url != FOULAD_EBOOKS_URL) return;
  const std::string fouladBookId = extractFouladBookId(book.id);
  if (fouladBookId.empty()) return;
  RECENT_BOOKS.addBook(filename, book.title, book.author, "", fouladBookId);
}

}  // namespace FouladOpdsHooks
