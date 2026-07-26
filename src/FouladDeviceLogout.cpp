#include "FouladDeviceLogout.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <WiFi.h>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "network/HttpDownloader.h"

namespace {

// "Remove me", by serial. The only path a QR-paired device can use: the app API that
// the fallback in removeThisDevice() calls sits behind RejectDeviceTokenAuth and
// answers a device token 403, so for every QR install -- the primary sign-in path --
// that fallback can never succeed.
//
// Returns Failed on a 404 so the caller can fall back, since a 404 here means the
// endpoint is not deployed yet rather than "no such device". The two are told apart by
// the caller: see removeThisDevice().
FouladDeviceLogout::Result signOutViaDeviceEndpoint(const std::string& username, const std::string& password,
                                                    const std::string& serial, int& outStatus) {
  outStatus = 0;

  JsonDocument requestDoc;
  requestDoc["serial_number"] = serial;
  std::string body;
  serializeJson(requestDoc, body);

  std::string response;
  const auto err = HttpDownloader::postJson(FOULAD_EBOOKS_DEVICE_SIGNOUT_URL, body, username, password, response);
  if (err == HttpDownloader::OK) {
    LOG_INF("LOGOUT", "signed out via /opds/device/signout");
    return FouladDeviceLogout::Result::Removed;
  }

  const auto failure = HttpDownloader::getLastFailure();
  if (failure.stage == HttpDownloader::FailStage::STATUS) {
    outStatus = failure.detail;
  }
  return FouladDeviceLogout::Result::Failed;
}

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

  const std::string ownSerial = FouladDeviceTracking::getSerialNumber();

  // Preferred path. Tried first because it is the only one a device token can use.
  int status = 0;
  if (signOutViaDeviceEndpoint(username, password, ownSerial, status) == Result::Removed) {
    return Result::Removed;
  }
  if (status == 404) {
    // Two different 404s share this status: the endpoint not being deployed yet, and the
    // device already being absent. Falling back distinguishes them without guessing --
    // the list below will simply not contain this serial in the second case, which the
    // caller already treats as success.
    LOG_DBG("LOGOUT", "signout endpoint unavailable; falling back to the app API");
  } else if (status != 0) {
    // A real refusal (401 bad credential, 403 serial mismatch). The fallback authenticates
    // identically, so retrying it would only repeat the rejection.
    LOG_ERR("LOGOUT", "signout refused with status %d", status);
    return Result::Failed;
  } else {
    LOG_ERR("LOGOUT", "signout transport failure");
    return Result::Failed;
  }

  // FALLBACK -- removable once every install is past the release that added the endpoint
  // above. Only reachable for account-password logins; a device token gets 403 here.
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

  long deviceId = -1;
  for (JsonObject device : doc["data"].as<JsonArray>()) {
    if (ownSerial == device["serial_number"].as<std::string>()) {
      deviceId = device["id"] | -1L;
      break;
    }
  }

  if (deviceId < 0) {
    // Already removed elsewhere (phone app, web). The end state the caller wants
    // already holds, so this is success, not an error.
    LOG_INF("LOGOUT", "serial %s not listed on the account; nothing to remove", ownSerial.c_str());
    return Result::NotFound;
  }

  char url[160];
  snprintf(url, sizeof(url), "%s/%ld", FOULAD_EBOOKS_APP_DEVICES_URL, deviceId);

  if (HttpDownloader::deleteRequest(url, username, password, response) == HttpDownloader::OK) {
    LOG_INF("LOGOUT", "device %ld (%s) removed from the account", deviceId, ownSerial.c_str());
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
