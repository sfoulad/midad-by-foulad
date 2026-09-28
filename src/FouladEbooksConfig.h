#pragma once

#include <string_view>

// Non-secret catalog identity for the built-in "Foulad eBooks" home menu entry.
// Credentials are issued by QR sign-in on first use (FouladQrLoginActivity)
// and stored via OpdsServerStore — never hardcoded here, since this repo is public.
constexpr char FOULAD_EBOOKS_NAME[] = "Midad";
// Midad Core's OPDS root. HTTPS only: Core is a Cloudflare Worker on
// api.midad.one and does not answer plain http, and a POST would not follow the
// redirect that answered it anyway.
//
// Two feeds hang off this one root and the device needs no knowledge of either.
// Anonymous, the root is the public Store catalogue, byte-for-byte what shipped.
// With a device credential it is a pure-navigation feed offering "My Library"
// (/opds/library), "Midad Store" (/opds/store) and, for a reader who has any,
// "Shelves" (/opds/library/collections) -- all absolute hrefs, all reached by
// the ordinary browser. Library search, categories, pagination, covers and
// acquisition are advertised on those feeds as ordinary OPDS links, so nothing
// below this line has to know Library exists.
//
// Page size is the server's to choose and is never assumed here: an
// authenticated device feed pages at 12 while the public Store stays at 25, and
// the browser only ever follows rel="next"/rel="previous".
constexpr char FOULAD_EBOOKS_URL[] = "https://api.midad.one/opds";

// Downloaded news lives in its own folder rather than beside the books at the SD
// root. It is not a book: it has no author, it is replaced wholesale every time you
// open the feed, and finishing it is not finishing anything. Keeping it in one place
// is what lets the home screen, My Books and the reading stats leave it alone
// without having to guess from a filename.
constexpr char FOULAD_NEWS_DIR[] = "/News";

inline bool isNewsBookPath(const std::string_view path) { return path.rfind("/News/", 0) == 0; }

// Font-conversion relay endpoint (Settings/File Transfer portal -> Fonts ->
// Convert a Font): the device uploads a raw TTF/OTF plus a language choice,
// the legacy host converts it into the device's .cpfont SD-format (4 sizes)
// and responds with download URLs. Open endpoint, no auth -- rate-limited
// server-side. Stays on the legacy host deliberately: the converter is a Python
// service, so Core has no equivalent and will not gain one.
constexpr char FOULAD_EBOOKS_FONT_CONVERT_URL[] = "https://api.midad.one/api/fonts/convert";

// QR-code sign-in (see FouladDeviceLogin.h). Unauthenticated JSON POSTs, no
// cookies/CSRF. Nothing secret is sent TO these endpoints; the token they return
// is what replaces the account password on the wire.
//
// Both run through HttpDownloader::postJsonWolf: wolfSSL, verified against the
// pinned api.midad.one anchor (src/network/CoreCaCerts.h) rather than the
// esp_crt_bundle, with the certificate's hostname checked against the URL's.
// Every other endpoint here still uses postJson/esp_http_client unchanged.
// Certificate validity needs a roughly correct clock, which is why the WiFi
// connect path syncs NTP before any HTTPS call.
constexpr char FOULAD_EBOOKS_DEVICE_LOGIN_START_URL[] = "https://api.midad.one/device-login/start";
constexpr char FOULAD_EBOOKS_DEVICE_LOGIN_POLL_URL[] = "https://api.midad.one/device-login/poll";

// Device-facing sign-out: "remove me". The request names no target at all -- Core
// revokes the token that authenticated the call -- so a stolen reader can destroy
// exactly the access it already had and not a sibling's. It is the one write a
// device credential may perform; every other account mutation answers it 401.
// Answers 204 on success and 401 when the token is already dead.
constexpr char FOULAD_EBOOKS_DEVICE_SIGNOUT_URL[] = "https://api.midad.one/device-login/self-revoke";

// Crash-report upload. Legacy, like reading-position above and for the same
// reason: the route lives on midad.one and was never carried over to Core, so
// building it from FOULAD_EBOOKS_URL (now api.midad.one) addressed a host that
// has no such endpoint and 404'd every time. Core replaces it with
// /devices/v1/logs; repointed here rather than removed so crash diagnostics keep
// working in the meantime.
// Core device telemetry (R18, live 2026-09-04, contract version
// c1173627-bcb9-47d3-8f78-8c5f6e5672eb). Both take the device credential as Basic
// auth and both go over the PINNED wolfSSL transport, never the certificate
// bundle: api.midad.one's chain is ECDSA, and verifying it through the bundle
// makes mbedTLS attempt an RSA modexp it cannot allocate (0x4290) -- the failure
// that broke sign-out. Logs deliberately use the text/plain envelope rather than
// multipart for the same reason: multipart only exists on the bundle path.
// Core resume position (B1). PUT/GET /resume/v1/items/{ref}/position, where {ref}
// is the Store book id the reader downloaded -- Core resolves it to the caller's
// own library item. Accepts the device token, so it replaces legacy
// /opds/reading-position, which resolves device credentials in a store Core's
// tokens have never been written to.
constexpr char FOULAD_CORE_RESUME_ITEM_URL[] = "https://api.midad.one/resume/v1/items/";

constexpr char FOULAD_CORE_HEARTBEAT_URL[] = "https://api.midad.one/devices/v1/heartbeat";
constexpr char FOULAD_CORE_LOGS_URL[] = "https://api.midad.one/devices/v1/logs";
// Server-side cap; a larger body is rejected 413 before it is read.
constexpr size_t FOULAD_CORE_LOG_MAX_BYTES = 65536;

// Midad's own hosts. Matched on the host rather than on a
// URL prefix because the requests that must carry the device serial header are
// spread across several path roots -- /opds/*, /books/{id}/download, /xtc, cover
// images -- and the acquisition/cover links additionally arrive as absolute URLs
// straight out of the feed (some carrying ?signature=), so no single prefix
// covers them. Kept scheme-agnostic so a legacy http:// entry still on an SD card
// is recognised until OpdsServerStore::fromJson rewrites it.
constexpr char FOULAD_EBOOKS_HOST[] = "midad.one";

// Midad Core's API host. A separate hostname from the site above, not a path on
// it, so recognising only FOULAD_EBOOKS_HOST would silently drop the device
// serial header from every catalog, cover and download request and leave remote
// removal reporting nothing.
constexpr char FOULAD_CORE_HOST[] = "api.midad.one";

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
  return hostEquals(host, FOULAD_CORE_HOST) || hostEquals(host, FOULAD_EBOOKS_HOST) ||
         hostEquals(host, FOULAD_EBOOKS_LEGACY_HOST);
}
