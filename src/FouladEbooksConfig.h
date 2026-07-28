#pragma once

#include <string_view>

// Non-secret catalog identity for the built-in "Foulad eBooks" home menu entry.
// Username/password are entered on-device on first use (FouladEbooksSetupActivity)
// and stored via OpdsServerStore — never hardcoded here, since this repo is public.
constexpr char FOULAD_EBOOKS_NAME[] = "Midad";
// Temporarily http:// (was https://), during beta only: Let's Encrypt rotated
// foulad.one onto a certificate hierarchy ("ISRG Root YE") that ESP-IDF's embedded
// trust bundle doesn't recognize, and the on-device fallback that could otherwise
// verify it was removed after it drove free heap down to ~5KB and froze the device
// (see HttpDownloader.cpp). The server has been reconfigured to serve OPDS over
// plain HTTP without redirecting to HTTPS, specifically so this can keep working
// during beta testing until the certificate chain is fixed server-side or ESP-IDF
// updates its bundle. Basic Auth credentials travel in cleartext over this
// connection as a result -- an accepted, explicit tradeoff for beta, not something
// to carry into a production release without revisiting.
constexpr char FOULAD_EBOOKS_URL[] = "http://midad.one/opds";

// Font-conversion relay endpoint (Settings/File Transfer portal -> Fonts ->
// Convert a Font): the device uploads a raw TTF/OTF plus a language choice,
// foulad-ebooks converts it into the device's .cpfont SD-font format (4 sizes)
// and responds with download URLs. Open endpoint, no auth -- foulad-ebooks is
// expected to rate-limit this server-side. Plain http:// for the same reason
// as FOULAD_EBOOKS_URL above (foulad.one's current certificate chain isn't in
// ESP-IDF's trust bundle); revisit alongside that fix.
constexpr char FOULAD_EBOOKS_FONT_CONVERT_URL[] = "http://midad.one/api/fonts/convert";

// QR-code sign-in (see FouladDeviceLogin.h). Unauthenticated JSON POSTs, no
// cookies/CSRF. Plain http:// is deliberate and expected here for the same
// certificate-chain reason as FOULAD_EBOOKS_URL above -- these two endpoints sit
// outside the server's HTTPS-forced group precisely so the device can reach them.
// Nothing secret is sent TO these endpoints; the token they return is what
// replaces the account password on the wire, so this flow strictly reduces what
// is exposed rather than adding to it.
constexpr char FOULAD_EBOOKS_DEVICE_LOGIN_START_URL[] = "http://midad.one/api/device-login/start";
constexpr char FOULAD_EBOOKS_DEVICE_LOGIN_POLL_URL[] = "http://midad.one/api/device-login/poll";

// Device-facing sign-out: "remove me", identified by this device's own serial.
// On the OPDS surface rather than the app API below, because that one sits behind
// RejectDeviceTokenAuth and answers a QR-issued device token 403 -- deliberately, since
// it can delete any device, change settings and manage fonts. Removing only itself is a
// far narrower capability, so a token may reach this. Contract:
// docs/ebooks-device-signout-endpoint.md.
constexpr char FOULAD_EBOOKS_DEVICE_SIGNOUT_URL[] = "http://midad.one/opds/device/signout";

// Foulad One's app JSON API, used by signing out to remove this device from the
// account (see FouladDeviceLogout). Behind the SAME opds.auth Basic-Auth middleware
// as the OPDS routes above, so the credential already stored for Foulad eBooks
// authenticates here unchanged -- no separate login, and no new server endpoint was
// needed for the device to remove itself.
//
// GET  <base>       -> this account's devices, each carrying id and serial_number
// DELETE <base>/{id} -> remove one
constexpr char FOULAD_EBOOKS_APP_DEVICES_URL[] = "http://midad.one/api/app/devices";

// The one host every URL above points at. Matched on the host rather than on a
// URL prefix because the requests that must carry the device serial header are
// spread across several path roots -- /opds/*, /books/{id}/download, /xtc, cover
// images -- and the acquisition/cover links additionally arrive as absolute URLs
// straight out of the feed (some carrying ?signature=), so no single prefix
// covers them. Kept scheme-agnostic so this keeps working when the beta http://
// above goes back to https:// (see FOULAD_EBOOKS_URL).
constexpr char FOULAD_EBOOKS_HOST[] = "midad.one";

// The pre-rename host. Every device paired before Midad has "http://foulad.one/opds"
// written into its own OpdsServerStore entry, and that entry -- not the constant above
// -- is what its requests actually use until the migration in OpdsServerStore::fromJson
// rewrites it. Between an update landing and that rewrite (and for any absolute feed
// link still carrying the old host), requests must keep being recognised as ours, or
// they silently lose the device-serial header and remote removal stops working for
// them. foulad.one redirects to midad.one server-side, so both resolve.
//
// Removable once no device in the field predates the rename -- not before.
constexpr char FOULAD_EBOOKS_LEGACY_HOST[] = "foulad.one";

// Case-insensitive hostname compare, per RFC 3986.
constexpr bool hostEquals(std::string_view host, std::string_view expected) {
  if (host.size() != expected.size()) return false;
  for (size_t i = 0; i < host.size(); ++i) {
    const char c = host[i] >= 'A' && host[i] <= 'Z' ? static_cast<char>(host[i] - 'A' + 'a') : host[i];
    if (c != expected[i]) return false;
  }
  return true;
}

// True when `url` targets Midad, under either its current or pre-rename host. Used
// to decide whether a request may
// carry this device's serial: a third-party OPDS server the user added must
// never receive it, so anything that is not this host is excluded by default.
//
// string_view only -- this never reaches a C API, so the non-null-terminated
// substrings taken below are safe (see CLAUDE.md on string_view).
constexpr bool isFouladEbooksUrl(std::string_view url) {
  const auto schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return false;
  url.remove_prefix(schemeEnd + 3);

  // Authority runs to the first '/', '?' or '#'.
  const auto authorityEnd = url.find_first_of("/?#");
  std::string_view host = authorityEnd == std::string_view::npos ? url : url.substr(0, authorityEnd);

  // Strip any userinfo ("user:pass@host") and port, leaving the bare hostname.
  const auto at = host.rfind('@');
  if (at != std::string_view::npos) host.remove_prefix(at + 1);
  const auto colon = host.find(':');
  if (colon != std::string_view::npos) host = host.substr(0, colon);

  // Hostnames are case-insensitive per RFC 3986; ours are lowercase in practice,
  // but a mismatch here would silently disable revocation rather than fail
  // loudly, so don't rely on that.
  return hostEquals(host, FOULAD_EBOOKS_HOST) || hostEquals(host, FOULAD_EBOOKS_LEGACY_HOST);
}
