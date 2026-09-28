#include "FouladDeviceLogin.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <WiFi.h>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "network/CoreCaCerts.h"
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
  const auto err = HttpDownloader::postJsonWolf(FOULAD_EBOOKS_DEVICE_LOGIN_START_URL, body,
                                                core_ca::kMidadCoreCaGtsRootR4, response);
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

  // ArduinoJson unescapes the response for us, which matters more than it looks:
  // a server that emits "https:\/\/midad.one\/link\/CODE" would otherwise put
  // literal backslashes in the QR and every scan would silently fail.
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
  const auto err =
      HttpDownloader::postJsonWolf(FOULAD_EBOOKS_DEVICE_LOGIN_POLL_URL, body, core_ca::kMidadCoreCaGtsRootR4, response);
  if (err != HttpDownloader::OK) {
    // 401 is Core's "invalid_session"; 404/410 are the legacy server's shape for
    // the same thing. All three mean this session no longer exists, which is the
    // documented end of an unclaimed session, not a transport failure. Retrying
    // any of them can only repeat the answer, so end the session here and let the
    // caller open a fresh one. Everything else leaves the session valid.
    const auto failure = HttpDownloader::getLastFailure();
    if (failure.stage == HttpDownloader::FailStage::STATUS &&
        (failure.detail == 401 || failure.detail == 404 || failure.detail == 410)) {
      LOG_DBG("QRLOGIN", "poll: session no longer valid (HTTP %d)", failure.detail);
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
    // Core's own name for the credential, with the legacy server's `token` as the
    // fallback. Redemption is single-use: if this response is misread the token is
    // gone for good, so read both spellings rather than one.
    result.token = doc["device_token"].as<std::string>();
    if (result.token.empty()) result.token = doc["token"].as<std::string>();

    // The Basic user half. Core sends the account id as `username` and repeats it
    // as `user.id`; either identifies the account well enough, and neither is a
    // secret. It is NEVER a reason to reject an approval -- Core discards this half
    // on verification, and treating an absent one as a failure is how a device ends
    // up polling a session it has already redeemed until it expires.
    result.username = doc["username"].as<std::string>();
    if (result.username.empty()) result.username = doc["user"]["id"].as<std::string>();
    if (result.username.empty()) result.username = "device";
    result.displayName = doc["user"]["name"].as<std::string>();

    // Only the token can fail this. Without it there is nothing to store, and the
    // session is spent either way -- so this is terminal, not a retry.
    if (result.token.empty()) {
      LOG_ERR("QRLOGIN", "approved but no device token in the response");
      result.status = PollStatus::Redeemed;
      return result;
    }
    // Optional: absence is not an error, and every field is read independently so
    // a partial block provisions what it can rather than nothing.
    JsonObjectConst kosync = doc["kosync"].as<JsonObjectConst>();
    if (!kosync.isNull()) {
      result.koUsername = kosync["username"].as<std::string>();
      result.koPassword = kosync["password"].as<std::string>();
      result.koServerUrl = kosync["server_url"].as<std::string>();
      if (kosync["match_method"].is<int>()) result.koMatchMethod = kosync["match_method"].as<int>();
      // Never logged: koPassword is the device token by construction.
      LOG_INF("QRLOGIN", "KOSync provisioning received (match_method=%d)", result.koMatchMethod);
    }

    result.status = PollStatus::Approved;
    LOG_INF("QRLOGIN", "Approved for account '%s'", result.username.c_str());
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
  // Terminal, and the one status a "not pending means keep waiting" client gets
  // catastrophically wrong: the approval was already redeemed and the one-time
  // token issued, so no later poll can ever produce it. Treated as Pending, this
  // is a reader that polls a dead session until expiry and silently never signs in.
  if (status == "redeemed") {
    LOG_ERR("QRLOGIN", "session already redeemed -- the token cannot be reissued");
    result.status = PollStatus::Redeemed;
    return result;
  }
  // "pending" and anything unrecognised. The pairing session's own deadline (see
  // FouladQrLoginActivity) is what stops an unknown status polling forever.
  result.status = PollStatus::Pending;
  return result;
}
