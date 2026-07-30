#include "FouladReadingPosition.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip unless seen
// first. Same pinned order as OtaUpdater.cpp.
#include "network/HttpDownloader.h"
#include <ArduinoJson.h>
#include <Logging.h>
// clang-format on

#include <cstdlib>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"

namespace {
constexpr const char* TAG = "FRP";

// Fills `out` from a response body shared by both endpoints.
bool parsePosition(const std::string& body, FouladReadingPosition::Position& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    LOG_ERR(TAG, "Malformed reading-position response");
    return false;
  }

  out.hasPosition = doc["has_position"] | false;
  if (!out.hasPosition) return true;  // nothing else is meaningful

  // as<float>() on purpose: the server's JSON encoder writes a whole float without
  // a decimal point (80, not 80.0), so reading this as an int would silently work
  // for whole percentages and truncate every other one.
  out.progressPercent = doc["progress_percent"].as<float>();
  out.page = doc["page"].isNull() ? -1 : doc["page"].as<int>();
  out.totalPages = doc["total_pages"].isNull() ? -1 : doc["total_pages"].as<int>();
  out.source = doc["source"].isNull() ? "" : doc["source"].as<std::string>();
  out.deviceName = doc["device_name"].isNull() ? "" : doc["device_name"].as<std::string>();
  // Absent on the GET; missing reads as false rather than throwing.
  out.shouldJump = doc["should_jump"] | false;
  return true;
}

// True when the failure was a 404 naming an unregistered device, which is worth one
// retry after re-registering. An unknown_book 404 is not: the library changed under
// us and retrying cannot help.
bool isUnknownDeviceFailure(const std::string& response) {
  const auto failure = HttpDownloader::getLastFailure();
  if (failure.stage != HttpDownloader::FailStage::STATUS || failure.detail != 404) return false;
  return response.find("unknown_device") != std::string::npos;
}
}  // namespace

bool FouladReadingPosition::sync(const std::string& username, const std::string& password,
                                 const std::string& fouladBookId, const float progressPercent, const int page,
                                 const int totalPages, const uint32_t readAtEpochSeconds, Position& out) {
  if (username.empty() || password.empty() || fouladBookId.empty()) return false;

  JsonDocument doc;
  doc["serial_number"] = FouladDeviceTracking::getSerialNumber();
  doc["book_id"] = atoi(fouladBookId.c_str());
  doc["progress_percent"] = progressPercent;
  if (page >= 0) doc["page"] = page;
  if (totalPages > 0) doc["total_pages"] = totalPages;
  if (readAtEpochSeconds > 0) {
    // ISO-8601 UTC. Only sent when the caller has a clock it trusts -- see the
    // header, and HalClock on why that is rarer here than it sounds.
    char buf[32];
    const time_t t = static_cast<time_t>(readAtEpochSeconds);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    doc["read_at"] = buf;
  }

  std::string body;
  serializeJson(doc, body);

  std::string response;
  bool ok = HttpDownloader::postJson(FOULAD_EBOOKS_READING_POSITION_URL, body, username, password, response) ==
            HttpDownloader::OK;
  if (!ok && isUnknownDeviceFailure(response)) {
    FouladDeviceTracking::registerDevice(username, password);
    response.clear();
    ok = HttpDownloader::postJson(FOULAD_EBOOKS_READING_POSITION_URL, body, username, password, response) ==
         HttpDownloader::OK;
  }

  if (!ok) {
    LOG_ERR(TAG, "position sync failed (status=%d)", HttpDownloader::getLastFailure().detail);
    return false;
  }
  return parsePosition(response, out);
}

bool FouladReadingPosition::fetch(const std::string& username, const std::string& password,
                                  const std::string& fouladBookId, Position& out) {
  if (username.empty() || password.empty() || fouladBookId.empty()) return false;

  const std::string url = std::string(FOULAD_EBOOKS_READING_POSITION_URL) + "?book_id=" + fouladBookId;
  std::string response;
  bool ok = HttpDownloader::fetchUrl(url, response, username, password);
  if (!ok && isUnknownDeviceFailure(response)) {
    FouladDeviceTracking::registerDevice(username, password);
    response.clear();
    ok = HttpDownloader::fetchUrl(url, response, username, password);
  }

  if (!ok) {
    LOG_ERR(TAG, "position fetch failed (status=%d)", HttpDownloader::getLastFailure().detail);
    return false;
  }
  return parsePosition(response, out);
}
