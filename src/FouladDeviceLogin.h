#pragma once

#include <cstdint>
#include <string>

// Sign in to Midad by QR code -- implements the device half of Midad Core's
// device-pairing contract (docs/device-auth-contract.md in midad-core).
//
// The device asks the server to open a pairing session, renders the returned
// URL as a QR code, and polls until the user approves it in the Foulad One
// phone app. On approval the server issues a per-device token, which is then
// used as the HTTP Basic Auth *password* for every existing OPDS call -- see
// FouladDeviceTracking, OpdsBookBrowserActivity, etc. Nothing downstream needs
// to know a token is being used rather than the account password.
//
// Why a token rather than the account password: it is per-device, independently
// revocable from the phone, reaches only the OPDS surface plus its own
// self-revoke (Core refuses it everywhere else), and is useless for signing into
// the website. The token is issued exactly once, in the body of the poll that
// redeems the session, and is never returned again -- a device that mis-reads
// that one response can only re-pair, never recover it.
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
  Pending,   // keep the QR on screen and keep polling
  Approved,  // credential issued -- stop polling, save it
  Denied,    // user rejected the request in the app
  Expired,   // session timed out, or the session token is unknown; start over
  // This session's approval was already redeemed and its one-time token handed
  // out. Terminal: the token is never reissued, so polling on can only ever
  // return this again. Distinct from Expired so the log says which happened,
  // but the caller's move is the same -- open a fresh session.
  Redeemed,
  NetworkError,  // transport failure; caller may retry, session is still valid
};

struct PollResult {
  PollStatus status = PollStatus::NetworkError;
  // The HTTP Basic *user* half. Core discards it on verification (it has no
  // account passwords and no username namespace), so its only job is to be
  // stable and non-empty -- never treat a missing one as a failed pairing.
  std::string username;
  // The credential. HTTP Basic *password* for every authenticated request
  // afterwards: feeds, covers, acquisition, self-revoke.
  std::string token;
  // user.name from the response, e.g. "Sameh" -- for a "Signed in as ..."
  // confirmation. Empty if the server didn't send one.
  std::string displayName;

  // KOSync provisioning, from the response's optional `kosync` object. Core mints
  // the KOSync credential at pairing (the password IS the device token), because
  // there is no other channel: the firmware's only other writer of these is the
  // Settings UI, and the server-pushed-settings path died with registerDevice.
  // All empty when the server sends no `kosync` block, which is the pre-R19 case
  // and must stay harmless.
  std::string koUsername;
  std::string koPassword;
  std::string koServerUrl;
  int koMatchMethod = -1;  // -1 = not sent; 0 = filename, 1 = binary
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
