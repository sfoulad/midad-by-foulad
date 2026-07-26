#include "FouladDeviceLogout.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <WiFi.h>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "network/HttpDownloader.h"

namespace {

// The device list is the fattest response this firmware asks for: the server sends
// every device with its full settings and reading-stats blobs (six devices on the
// test account came to well over a KB each). Deserializing all of that on a 380KB
// part to read two fields would be reckless, so ArduinoJson is given a filter and
// keeps only id and serial_number -- everything else is skipped while streaming and
// never allocated.
constexpr size_t DEVICE_LIST_DOC_BYTES = 4096;

}  // namespace

FouladDeviceLogout::Result FouladDeviceLogout::removeThisDevice(const std::string& username,
                                                               const std::string& password) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("LOGOUT", "no WiFi; refusing to sign out");
    return Result::Failed;
  }
  if (username.empty() || password.empty()) {
    LOG_ERR("LOGOUT", "no stored credential to authenticate with");
    return Result::Failed;
  }

  std::string response;
  if (!HttpDownloader::fetchUrl(FOULAD_EBOOKS_APP_DEVICES_URL, response, username, password)) {
    const auto failure = HttpDownloader::getLastFailure();
    LOG_ERR("LOGOUT", "device list failed: stage=%d detail=%d", static_cast<int>(failure.stage), failure.detail);
    return Result::Failed;
  }

  JsonDocument filter;
  filter["data"][0]["id"] = true;
  filter["data"][0]["serial_number"] = true;

  JsonDocument doc;
  const DeserializationError jsonErr = deserializeJson(doc, response, DeserializationOption::Filter(filter));
  if (jsonErr) {
    LOG_ERR("LOGOUT", "device list: bad JSON (%s)", jsonErr.c_str());
    return Result::Failed;
  }
  // Free the (potentially multi-KB) raw body before the delete call allocates its own
  // buffers -- this runs on a heap that is already carrying an active WiFi/TLS session.
  response.clear();
  response.shrink_to_fit();

  const std::string serial = FouladDeviceTracking::getSerialNumber();
  long deviceId = -1;
  for (JsonObject device : doc["data"].as<JsonArray>()) {
    if (serial == device["serial_number"].as<std::string>()) {
      deviceId = device["id"] | -1L;
      break;
    }
  }

  if (deviceId < 0) {
    // Already removed elsewhere (phone app, web). The end state the caller wants
    // already holds, so this is success, not an error.
    LOG_INF("LOGOUT", "serial %s not listed on the account; nothing to remove", serial.c_str());
    return Result::NotFound;
  }

  char url[160];
  snprintf(url, sizeof(url), "%s/%ld", FOULAD_EBOOKS_APP_DEVICES_URL, deviceId);

  if (HttpDownloader::deleteRequest(url, username, password, response) == HttpDownloader::OK) {
    LOG_INF("LOGOUT", "device %ld (%s) removed from the account", deviceId, serial.c_str());
    return Result::Removed;
  }

  // 404 here means the row went away between the list and the delete -- a race with a
  // removal from the phone app or web, and the same desired end state either way.
  const auto failure = HttpDownloader::getLastFailure();
  if (failure.stage == HttpDownloader::FailStage::STATUS && failure.detail == 404) {
    LOG_INF("LOGOUT", "device %ld already gone", deviceId);
    return Result::NotFound;
  }

  LOG_ERR("LOGOUT", "delete failed: stage=%d detail=%d", static_cast<int>(failure.stage), failure.detail);
  return Result::Failed;
}
