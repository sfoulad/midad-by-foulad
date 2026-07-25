#include "FouladDeviceLogin.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <WiFi.h>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "network/HttpDownloader.h"

namespace {

// Mirrors FouladDeviceTracking's own naming so the phone app's confirmation
// prompt shows exactly what the "My Devices" page already shows for this unit.
std::string modelName() { return gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4"; }
std::string displayName() { return std::string("Foulad eInk (") + (gpio.deviceIsX3() ? "X3" : "X4") + ")"; }

// Responses are a handful of short flat fields (the largest, an approval, is
// ~200 bytes). 1KB leaves generous room without being worth a heap-pressure
// check the way FouladDeviceTracking's multi-KB stats payloads are.
constexpr size_t RESPONSE_DOC_BYTES = 1024;

}  // namespace

FouladDeviceLogin::StartResult FouladDeviceLogin::start() {
  StartResult result;
  if (WiFi.status() != WL_CONNECTED) {
    LOG_DBG("QRLOGIN", "start skipped: WiFi not connected");
    return result;
  }

  JsonDocument requestDoc;
  requestDoc["device_name"] = displayName();
  requestDoc["serial_number"] = FouladDeviceTracking::getSerialNumber();
  requestDoc["model"] = modelName();
  requestDoc["firmware_version"] = CROSSPOINT_VERSION;

  std::string body;
  serializeJson(requestDoc, body);

  std::string response;
  const auto err = HttpDownloader::postJson(FOULAD_EBOOKS_DEVICE_LOGIN_START_URL, body, "", "", response);
  if (err != HttpDownloader::OK) {
    const auto failure = HttpDownloader::getLastFailure();
    LOG_ERR("QRLOGIN", "start failed: err=%d stage=%d detail=%d", static_cast<int>(err),
            static_cast<int>(failure.stage), failure.detail);
    return result;
  }

  JsonDocument doc;
  const DeserializationError jsonErr = deserializeJson(doc, response);
  if (jsonErr) {
    LOG_ERR("QRLOGIN", "start: bad JSON (%s)", jsonErr.c_str());
    return result;
  }

  // ArduinoJson unescapes the response for us, which matters more than it looks
  // here: the server is PHP, so qr_payload arrives on the wire as
  // "https:\/\/foulad.one\/link\/CODE". Encoding that raw into the QR would put
  // literal backslashes in the URL and every scan would silently fail.
  result.sessionToken = doc["session_token"].as<std::string>();
  result.pairingCode = doc["pairing_code"].as<std::string>();
  result.qrPayload = doc["qr_payload"].as<std::string>();
  result.expiresInSeconds = doc["expires_in"] | 300U;
  result.pollIntervalSeconds = doc["poll_interval"] | 3U;

  // A session with no token can never be collected, and one with no payload has
  // nothing to render -- treat either as a failure rather than showing a dead QR.
  if (result.sessionToken.empty() || result.qrPayload.empty()) {
    LOG_ERR("QRLOGIN", "start: response missing session_token/qr_payload");
    result.sessionToken.clear();
    return result;
  }

  // Guard against a server-side change making us hammer a rate-limited endpoint
  // (start 10/min, poll 120/min per IP).
  if (result.pollIntervalSeconds == 0) result.pollIntervalSeconds = 3;

  result.ok = true;
  LOG_INF("QRLOGIN", "Pairing session open: code=%s expires_in=%us", result.pairingCode.c_str(),
          static_cast<unsigned>(result.expiresInSeconds));
  return result;
}

FouladDeviceLogin::PollResult FouladDeviceLogin::poll(const std::string& sessionToken) {
  PollResult result;
  if (sessionToken.empty()) {
    result.status = PollStatus::Expired;
    return result;
  }
  if (WiFi.status() != WL_CONNECTED) {
    result.status = PollStatus::NetworkError;
    return result;
  }

  JsonDocument requestDoc;
  requestDoc["session_token"] = sessionToken;
  std::string body;
  serializeJson(requestDoc, body);

  std::string response;
  const auto err = HttpDownloader::postJson(FOULAD_EBOOKS_DEVICE_LOGIN_POLL_URL, body, "", "", response);
  if (err != HttpDownloader::OK) {
    // 404 and 410 both mean "this session is gone" and are the documented,
    // expected end of an unclaimed session -- not transport failures. Everything
    // else leaves the session valid, so the caller can simply poll again.
    const auto failure = HttpDownloader::getLastFailure();
    if (failure.stage == HttpDownloader::FailStage::STATUS && (failure.detail == 404 || failure.detail == 410)) {
      result.status = PollStatus::Expired;
      return result;
    }
    LOG_DBG("QRLOGIN", "poll transport error: err=%d stage=%d detail=%d", static_cast<int>(err),
            static_cast<int>(failure.stage), failure.detail);
    result.status = PollStatus::NetworkError;
    return result;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    result.status = PollStatus::NetworkError;
    return result;
  }

  const std::string status = doc["status"].as<std::string>();
  if (status == "approved") {
    result.username = doc["username"].as<std::string>();
    result.token = doc["token"].as<std::string>();
    result.displayName = doc["user"]["name"].as<std::string>();
    // An "approved" with nothing usable in it would otherwise store empty
    // credentials and leave the user apparently signed in but broken.
    if (result.username.empty() || result.token.empty()) {
      LOG_ERR("QRLOGIN", "approved but username/token missing");
      result.status = PollStatus::NetworkError;
      return result;
    }
    result.status = PollStatus::Approved;
    LOG_INF("QRLOGIN", "Approved for user '%s'", result.username.c_str());
    return result;
  }
  if (status == "denied") {
    result.status = PollStatus::Denied;
    return result;
  }
  if (status == "expired") {
    result.status = PollStatus::Expired;
    return result;
  }
  result.status = PollStatus::Pending;
  return result;
}
