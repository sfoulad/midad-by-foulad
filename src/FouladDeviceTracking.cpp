#include "FouladDeviceTracking.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdlib>

#include "FouladEbooksConfig.h"
#include "RecentBooksStore.h"
#include "network/HttpDownloader.h"
#include "reading/ReadingStatsStore.h"
#include "util/DebugLog.h"
#include "util/DebugLogging.h"
#include "util/RollingSdLog.h"

namespace FouladDeviceTracking {

namespace {
constexpr char TAG[] = "FDT";

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

std::string deviceEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/device"; }
std::string readingStatsEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/reading-stats"; }
std::string deviceLogEndpoint() { return std::string(FOULAD_EBOOKS_URL) + "/device-log"; }

// Matches the literal path HalSystem::checkPanic() writes to.
constexpr char CRASH_REPORT_PATH[] = "/crash_report.txt";

std::string modelName() { return gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4"; }
std::string displayName() { return std::string("Foulad eInk (") + (gpio.deviceIsX3() ? "X3" : "X4") + ")"; }

// Tagged into the shared on-SD debug log (see util/DebugLog.h) so a
// registration/reporting problem with the LIVE server can be diagnosed from
// the SD card, the same way OPDS browsing/downloading already can.
void diagLog(const std::string& line) { RollingSdLog::append(DebugLog::PATH, "[FDT] " + line, DebugLog::MAX_LINES); }

// Single attempt, no 404 handling -- reportReadingStats() (below) is the only
// caller and owns the retry-after-register decision.
bool postReadingStatsOnce(const std::string& username, const std::string& password, const std::string& fouladBookId,
                          const int progressPercent, const std::string& lastPosition, const uint32_t secondsRead) {
  JsonDocument doc;
  doc["serial_number"] = getSerialNumber();
  doc["book_id"] = atoi(fouladBookId.c_str());
  doc["progress_percent"] = progressPercent;
  if (!lastPosition.empty()) doc["last_position"] = lastPosition;
  doc["seconds_read"] = secondsRead;
  std::string body;
  serializeJson(doc, body);

  std::string response;
  const bool ok =
      HttpDownloader::postJson(readingStatsEndpoint(), body, username, password, response) == HttpDownloader::OK;
  diagLog("reading-stats book=" + fouladBookId + " progress=" + std::to_string(progressPercent) +
          " seconds=" + std::to_string(secondsRead) + " -> " +
          (ok ? "ok" : "FAIL status=" + std::to_string(HttpDownloader::getLastFailure().detail)));
  return ok;
}
}  // namespace

std::string getSerialNumber() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return std::string("XTE-") + mac.c_str();
}

void registerDevice(const std::string& username, const std::string& password) {
  if (!wifiConnected() || username.empty() || password.empty()) return;

  JsonDocument doc;
  doc["serial_number"] = getSerialNumber();
  doc["name"] = displayName();
  doc["model"] = modelName();
  doc["firmware_version"] = CROSSPOINT_VERSION;
  std::string body;
  serializeJson(doc, body);

  std::string response;
  const auto result = HttpDownloader::postJson(deviceEndpoint(), body, username, password, response);
  if (result != HttpDownloader::OK) {
    const int status = HttpDownloader::getLastFailure().detail;
    LOG_ERR(TAG, "Device registration failed (status=%d)", status);
    diagLog("register serial=" + getSerialNumber() + " -> FAIL status=" + std::to_string(status));
    return;
  }
  LOG_DBG(TAG, "Device registered: %s", response.c_str());
  diagLog("register serial=" + getSerialNumber() + " -> ok " + response);
}

void reportReadingStats(const std::string& username, const std::string& password, const std::string& fouladBookId,
                        const int progressPercent, const std::string& lastPosition, const uint32_t secondsRead) {
  if (!wifiConnected() || username.empty() || password.empty() || fouladBookId.empty()) return;

  if (postReadingStatsOnce(username, password, fouladBookId, progressPercent, lastPosition, secondsRead)) {
    return;
  }

  const auto failure = HttpDownloader::getLastFailure();
  if (failure.stage == HttpDownloader::FailStage::STATUS && failure.detail == 404) {
    // Unknown device server-side -- register once, then retry exactly once.
    LOG_DBG(TAG, "Reading-stats 404 (device not registered yet), registering and retrying");
    diagLog("reading-stats book=" + fouladBookId + " got 404, registering and retrying");
    registerDevice(username, password);
    if (postReadingStatsOnce(username, password, fouladBookId, progressPercent, lastPosition, secondsRead)) return;
  }
  LOG_ERR(TAG, "Reading-stats report failed (status=%d)", HttpDownloader::getLastFailure().detail);
  diagLog("reading-stats book=" + fouladBookId +
          " gave up, status=" + std::to_string(HttpDownloader::getLastFailure().detail));
}

void flushPendingReadingStats(const std::string& username, const std::string& password) {
  if (!wifiConnected() || username.empty() || password.empty()) return;

  int flushed = 0;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.fouladBookId.empty()) continue;
    const ReadingBookStats* stats = READING_STATS.findBook(book.path);
    if (!stats || stats->totalReadingMs == 0) continue;
    reportReadingStats(username, password, book.fouladBookId, stats->lastProgressPercent, "",
                       static_cast<uint32_t>(stats->totalReadingMs / 1000));
    flushed++;
  }
  if (flushed > 0) diagLog("flush: reported " + std::to_string(flushed) + " book(s)");
}

void uploadDebugLog(const std::string& username, const std::string& password) {
  if (!DebugLogging::enabled() || !wifiConnected() || username.empty() || password.empty()) return;
  if (!Storage.exists(DebugLog::PATH)) return;

  const std::vector<std::pair<std::string, std::string>> files = {{"log", DebugLog::PATH}};
  const std::vector<std::pair<std::string, std::string>> fields = {{"serial_number", getSerialNumber()}};
  std::string response;
  bool ok = HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);

  if (!ok) {
    const auto failure = HttpDownloader::getLastFailure();
    if (failure.stage == HttpDownloader::FailStage::STATUS && failure.detail == 404) {
      // Unknown device server-side -- register once, then retry exactly once,
      // same convention as reportReadingStats().
      registerDevice(username, password);
      ok = HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);
    }
  }

  if (!ok) {
    LOG_ERR(TAG, "Debug log upload failed (status=%d)", HttpDownloader::getLastFailure().detail);
    // Deliberately not diagLog()'d: appending a failure line to the very file
    // that just failed to upload would just get picked up on next attempt,
    // and a persistent failure (e.g. no signal) would otherwise spam a line
    // into the log on every single reconnect.
    return;
  }
  diagLog("log upload -> ok");
}

bool uploadCrashReport(const std::string& username, const std::string& password) {
  if (!wifiConnected() || username.empty() || password.empty()) return false;
  if (!Storage.exists(CRASH_REPORT_PATH)) return false;

  const std::vector<std::pair<std::string, std::string>> files = {{"log", CRASH_REPORT_PATH}};
  const std::vector<std::pair<std::string, std::string>> fields = {{"serial_number", getSerialNumber()},
                                                                   {"type", "crash"}};
  std::string response;
  bool ok = HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);

  if (!ok) {
    const auto failure = HttpDownloader::getLastFailure();
    if (failure.stage == HttpDownloader::FailStage::STATUS && failure.detail == 404) {
      // Unknown device server-side -- register once, then retry exactly once,
      // same convention as reportReadingStats()/uploadDebugLog().
      registerDevice(username, password);
      ok = HttpDownloader::postFilesMultipart(deviceLogEndpoint(), files, fields, response, 20000, username, password);
    }
  }

  if (!ok) {
    LOG_ERR(TAG, "Crash report upload failed (status=%d)", HttpDownloader::getLastFailure().detail);
  }
  // Unlike uploadDebugLog(), diagLog() here is safe either way: this writes to
  // /debug_log.txt, a different file than the one just uploaded.
  diagLog(ok ? "crash report upload -> ok"
             : "crash report upload -> FAIL status=" + std::to_string(HttpDownloader::getLastFailure().detail));
  return ok;
}

}  // namespace FouladDeviceTracking
