#pragma once

#include <string_view>

// Non-secret catalog identity for the built-in "Foulad eBooks" home menu entry.
// Credentials are issued by QR sign-in on first use (FouladQrLoginActivity)
// and stored via OpdsServerStore — never hardcoded here, since this repo is public.
constexpr char FOULAD_EBOOKS_NAME[] = "Midad";
// http://, and Basic Auth credentials travel in cleartext as a result. That is a
// known, unresolved exposure -- not a settled design -- and it should not reach a
// production release unrevisited.
//
// The original reason recorded here was that ESP-IDF's trust bundle could not
// resolve the certificate chain. That was wrong, and worth correcting so nobody
// re-derives it: the server presents the full cross-signed chain, so ISRG Root X1
// alone verifies it (checked with `openssl s_client -CAfile isrgrootx1.pem`:
// return code 0). At ~1.9KB pinned, versus the ~250KB bundle that genuinely did
// drive free heap to ~5KB, the memory objection does not apply either.
//
// https was nevertheless tried and reverted (v1.8.14-rc): the first device to run
// it could not fetch the feed. All twelve endpoints were switched at once, so the
// failure took out catalog, sign-in, covers, fonts and gym together with nothing to
// compare against -- the shape of the change was as wrong as anything in it.
//
// For the next attempt: switch ONE endpoint, and suspect the clock first.
// Certificate validity checking needs a roughly correct date, and neither the X3's
// DS3231 (hour/minute, no calendar) nor the X4 (no RTC) keeps one across a reboot.
// http never cared; https does. That is testable before writing any code -- compare
// a freshly NTP-synced device against one that has been offline.
constexpr char FOULAD_EBOOKS_URL[] = "http://midad.one/opds";
// News feeds (EINK_NEWS_TASKS.md). A normal OPDS acquisition feed, one entry per
// subscription, browsed by the same activity as the catalog -- News is a different
// root, not a different browser. Entry ids live in their own namespace
// ("urn:midad:feed:<id>"), which is why extractFouladBookId checks the prefix.
constexpr char FOULAD_EBOOKS_NEWS_URL[] = "http://midad.one/opds/news";

// Downloaded news lives in its own folder rather than beside the books at the SD
// root. It is not a book: it has no author, it is replaced wholesale every time you
// open the feed, and finishing it is not finishing anything. Keeping it in one place
// is what lets the home screen, My Books and the reading stats leave it alone
// without having to guess from a filename.
constexpr char FOULAD_NEWS_DIR[] = "/News";

inline bool isNewsBookPath(const std::string_view path) { return path.rfind("/News/", 0) == 0; }

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
// Cross-device reading position (EINK_PAGE_SYNC_TASKS.md). POST reports where this
// device is and returns where the ACCOUNT is in one round trip; GET only looks, for
// when there is nothing honest to report yet. Same opds.auth Basic Auth as the feed.
//
// The shared currency is progress_percent, not page: the phone paginates with
// epub.js at its own font size and cannot act on a page number computed for a
// 5-inch panel. page/total_pages are sent for display only.
constexpr char FOULAD_EBOOKS_READING_POSITION_URL[] = "http://midad.one/opds/reading-position";

constexpr char FOULAD_EBOOKS_APP_DEVICES_URL[] = "http://midad.one/api/app/devices";

// BLE book.fetch's lookup step (FouladDeviceTracking::flushPendingBleBookFetch):
// GET this + book id -> {title, author, format, download_url}. download_url is a
// 30-day Laravel temporary signed URL, not derivable from the id alone -- there is
// no fixed download path a device can hand-build (confirmed with foulad-ebooks
// 2026-08-11; /api/app/books/{id} returns the same shape but is walled off from
// device tokens by RejectDeviceTokenAuth). format/download_url are both null if
// the book has no acquisition link yet (e.g. still converting).
constexpr char FOULAD_EBOOKS_BOOK_LOOKUP_URL_PREFIX[] = "http://midad.one/opds/books/";

// The one host every URL above points at. Matched on the host rather than on a
// URL prefix because the requests that must carry the device serial header are
// spread across several path roots -- /opds/*, /opds/books/{id}/download, /xtc,
// cover images -- and the acquisition/cover links additionally arrive as absolute
// URLs straight out of the feed (some carrying ?signature=), so no single prefix
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
