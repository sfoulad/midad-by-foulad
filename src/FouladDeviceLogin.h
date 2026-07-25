#pragma once

#include <cstdint>
#include <string>

// Sign in to Foulad eBooks by QR code -- implements the device half of the
// contract in EINK_QR_LOGIN_TASKS.md (foulad-ebooks repo).
//
// The device asks the server to open a pairing session, renders the returned
// URL as a QR code, and polls until the user approves it in the Foulad One
// phone app. On approval the server issues a per-device token, which is then
// used as the HTTP Basic Auth *password* for every existing OPDS call -- see
// FouladDeviceTracking, OpdsBookBrowserActivity, etc. Nothing downstream needs
// to know a token is being used rather than the account password.
//
// Why this exists at all: OPDS runs over plain HTTP on this server (the device's
// mbedTLS build can't validate foulad.one's certificate chain -- see
// FouladEbooksConfig.h), so a typed account password travels in cleartext on
// every request. A device token is per-device, independently revocable, and
// useless for signing into the website.
//
// Both calls are synchronous and block for up to their HTTP timeout, so callers
// must drive them from a context where that is acceptable and must not call
// poll() faster than the server's advertised interval (it is rate limited:
// start 10/min, poll 120/min per IP).
namespace FouladDeviceLogin {

struct StartResult {
  bool ok = false;
  // Secret. Identifies THIS device's claim on the pairing session; never render
  // it and never put it in the QR code. The pairing code is public (anyone can
  // photograph it off the screen) -- only the device holding this token can
  // collect the issued credential.
  std::string sessionToken;
  // 8 chars from ABCDEFGHJKLMNPQRSTUVWXYZ23456789 (no O/0/I/1), safe to display
  // large and to read aloud if a camera won't focus on the e-ink screen.
  std::string pairingCode;
  // The exact string to encode as the QR. A URL, so a scan with an ordinary
  // camera app lands on an explanatory page rather than showing gibberish.
  std::string qrPayload;
  uint32_t expiresInSeconds = 0;
  uint32_t pollIntervalSeconds = 3;
};

enum class PollStatus : uint8_t {
  Pending,       // keep the QR on screen and keep polling
  Approved,      // credential issued -- stop polling, save it
  Denied,        // user rejected the request in the app
  Expired,       // session timed out (or the token is unknown); start over
  NetworkError,  // transport failure; caller may retry, session is still valid
};

struct PollResult {
  PollStatus status = PollStatus::NetworkError;
  std::string username;
  std::string token;
  // user.name from the response, e.g. "Sameh" -- for a "Signed in as ..."
  // confirmation. Empty if the server didn't send one.
  std::string displayName;
};

// Opens a pairing session. Sends this device's name/serial/model/firmware so
// the phone app can show them for confirmation before the user approves -- the
// serial is printed on the device, so it is what actually lets a user verify
// they are approving their own reader. None of it grants anything server-side.
StartResult start();

// One poll. Caller is responsible for honouring StartResult::pollIntervalSeconds
// between calls and for stopping as soon as the status is not Pending.
PollResult poll(const std::string& sessionToken);

}  // namespace FouladDeviceLogin
