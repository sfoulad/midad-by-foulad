#include "HttpDownloader.h"

#include <Arduino.h>
#include <HeapTrace.h>
#include <Logging.h>
#include <Memory.h>
#include <UrlOrigin.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <strings.h>  // strcasecmp, for the case-insensitive response-header match

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>

#include "CoreCaCerts.h"
#include "HttpUrlParts.h"
#include "VerifiedTlsSession.h"

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#endif
// Always available, not only in the !FREEINK_NET_WOLFSSL branch above: runGet()/
// runPostFile()/postJson() below have no wolfSSL equivalent (conditional-GET via
// ETag, multipart POST) and stay on esp_http_client regardless of which backend
// runGetSecure() picks for plain fetches.
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

namespace {
// RX holds the response headers. Smaller buffers leave enough contiguous heap
// for mbedTLS on redirect-heavy OPDS feeds while still preserving the headers
// we read directly (Location, Content-Length). Used by runGet()/runPostFile()/
// postJson() below, which stay on esp_http_client regardless of FREEINK_NET_WOLFSSL
// (see the include block above).
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 512;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;
constexpr int MAX_REDIRECTS = 5;

// Identifies this device to Foulad eBooks on every request, reads included.
// Removing a device server-side can only cut off its reads if the read itself
// says which device it is: a reader holding the account password (the pre-QR
// manual login path) otherwise sends a request byte-for-byte identical to the
// owner's own browser, so the server cannot refuse one without refusing both.
// With this header present it answers a removed device 401, which the device
// already signs itself out on (see OpdsBookBrowserActivity's 401 handling).
// Contract: EINK_OPDS_SERIAL_HEADER_TASKS.md in the foulad-ebooks repo.
//
// Deliberately applied here, at the single choke point every HTTP request
// passes through, rather than at each OPDS call site -- the spec requires it on
// *every* request (feeds, search, downloads, covers, signed ?signature= links),
// and a per-call-site approach silently loses revocation for whichever path
// someone forgets, with no visible symptom until it matters.
//
// Value comes from FouladDeviceTracking::getSerialNumber(), the same source the
// `serial_number` POST bodies use. If the two ever disagreed the server would
// match neither and revocation would quietly stop working, so it is read from
// one place rather than formatted twice.
void setDeviceSerialHeader(esp_http_client_handle_t client, const std::string& url) {
  // Host-gated: a user-added third-party OPDS server must never be told this
  // device's identifier.
  if (!isFouladEbooksUrl(url)) return;
  const std::string serial = FouladDeviceTracking::getSerialNumber();
  if (serial.empty()) return;
  esp_http_client_set_header(client, "X-Device-Serial", serial.c_str());
}

// The simulator's esp_http_client.h stub (crosspoint-simulator, an external repo)
// doesn't implement esp_http_client_get_errno -- only real ESP-IDF has it.
#ifdef SIMULATOR
int getHttpClientErrno(esp_http_client_handle_t) { return 0; }
void logTlsError(esp_http_client_handle_t) {}
// crosspoint-simulator's esp_http_client.h stub only implements the GET/read
// path (matching every other simulator-buildable feature so far); it has no
// esp_http_client_write() at all. postFileMultipart() -- used only by the
// Convert Font web-portal relay -- has no simulator-testable counterpart
// anyway (no live foulad-ebooks endpoint in dev), so this just fails cleanly
// rather than needing a real stub.
int httpClientWrite(esp_http_client_handle_t, const char*, int) { return -1; }
#else
int getHttpClientErrno(esp_http_client_handle_t client) { return esp_http_client_get_errno(client); }
// A bare ESP_ERR_HTTP_CONNECT + errno=0 means the raw socket connect succeeded (or
// was never attempted) and something above it -- the TLS handshake or certificate
// verification -- is what actually failed; the plain socket errno can't distinguish
// that from DNS/TCP-level trouble. esp_tls_error_code is the underlying mbedtls
// error (a handshake failure); esp_tls_flags is mbedtls' cert verify bitmask
// (expired/untrusted/hostname-mismatch/etc. -- see mbedtls x509.h X509_BADCERT_*).
void logTlsError(esp_http_client_handle_t client) {
  int tlsErrorCode = 0;
  int tlsFlags = 0;
  esp_http_client_get_and_clear_last_tls_error(client, &tlsErrorCode, &tlsFlags);
  if (tlsErrorCode != 0 || tlsFlags != 0) {
    // esp_http_client reports this as the positive magnitude of the mbedTLS code
    // (e.g. 0x3000 for MBEDTLS_ERR_X509_FATAL_ERROR, which mbedtls's own headers define
    // as -0x3000) -- print it as mbedTLS's own negative convention for readability.
    LOG_ERR("HTTP", "tls detail: esp_tls_error_code=-0x%X esp_tls_flags=0x%X", tlsErrorCode, tlsFlags);
    // TEMP diagnostic: a single combined code hides which half is the real cause.
    // 0x4290 is MBEDTLS_ERROR_ADD(MBEDTLS_ERR_RSA_PUBLIC_FAILED, MBEDTLS_ERR_MPI_ALLOC_FAILED)
    // -- an out-of-memory inside the signature check, not a rejected certificate.
    // esp_tls_flags == 0 alongside it says the chain itself verified fine.
    HEAP_TRACE_REPORT_MBEDTLS("tls detail:", tlsErrorCode);
    HEAP_TRACE_REPORT_FAILURE("tls failure");
  }
}
int httpClientWrite(esp_http_client_handle_t client, const char* data, int len) {
  return esp_http_client_write(client, data, len);
}
#endif

// Set at every runGet() failure point (and cleared at the top of runGet), so a
// caller can read WHY the last HTTP_ERROR/FILE_ERROR happened -- see
// HttpDownloader::FailStage for what each stage means.
HttpDownloader::LastFailure gLastFailure;

void setLastFailure(const HttpDownloader::FailStage stage, const int detail) { gLastFailure = {stage, detail}; }

#if defined(FREEINK_NET_WOLFSSL)
// The client a HttpDownloader::KeepAliveScope holds open, or null when no scope is
// active. File-scope for the same reason gLastFailure above is: one network task, no
// concurrent requests. gKeepAliveDepth counts nesting so an inner scope cannot close
// the connection an outer one is still using.
std::unique_ptr<freeink::SecureHttpClient> gKeepAliveClient;
int gKeepAliveDepth = 0;
// The verified session a KeepAliveScope holds open, with the origin it was
// negotiated for. One handshake per cover-grid page instead of one per cover:
// the anchors are the same for every Midad Core request, so only the origin has
// to match for the session to be reusable.
std::unique_ptr<VerifiedTlsSession> gKeepAliveVerified;
std::string gKeepAliveVerifiedOrigin;
// Below this contiguous block a held session is dropped rather than reused. A
// kept connection is an optimisation; carrying one through a heap this tight is
// how a cover grid ends up doing TLS work with nothing left to recover with.
constexpr uint32_t KEEPALIVE_MIN_CONTIGUOUS_BYTES = 8192;

// Drops any held session. Safe to call when none is held.
void dropKeepAliveVerified() {
  gKeepAliveVerified.reset();
  gKeepAliveVerifiedOrigin.clear();
}
#endif

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  const bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// scheme://host[:port] prefix, i.e. everything before the first '/', '?', or
// '#' after the scheme separator (a URL with no path but a query/fragment --
// e.g. "https://host?key=value" -- has no '/' at all, so stopping at '/' alone
// would fold the query string into the "origin" and misclassify a same-origin
// redirect as cross-origin). Used to decide whether a redirect crosses trust
// boundaries -- see the Authorization/X-Device-Serial-stripping logic in
// runGet() and runGetWolf() below.
// Disables WiFi modem-sleep power-save for the duration of an HTTP operation,
// restoring the default on scope exit regardless of which return path is taken.
// OtaUpdater.cpp already does this for firmware downloads ("For better timing and
// connectivity, we disable power saving for WiFi"); OPDS feed/book fetches never
// did, despite being able to run just as long for a large category. Modem sleep
// periodically powers the radio down between DTIM beacon intervals, which can drop
// or stall packets mid-transfer -- more likely to be hit the longer a transfer
// takes, so small feeds mostly get away with it while a 200+ book category
// consistently doesn't. Matches a real device report: a 3-book category fetched
// fine while a 200+ book category on the same server/network failed every time
// with a bare ESP_ERR_HTTP_CONNECT (errno=0, i.e. failed before a socket-level
// error was even set -- consistent with the radio being asleep at connect time).
struct WifiPowerSaveGuard {
  WifiPowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_NONE); }
  ~WifiPowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
};

#if defined(FREEINK_NET_WOLFSSL)
HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password, Sink& sink) {
  setLastFailure(HttpDownloader::FailStage::NONE, 0);
  WifiPowerSaveGuard psGuard;

  // One client for the whole request, redirect hops included -- and, inside a
  // HttpDownloader::KeepAliveScope, for every request the scope spans.
  // SecureHttpClient keeps its connection open between begin()/GET() pairs to the
  // same scheme://host:port, so each of those rides the TLS session the first one
  // negotiated instead of paying another handshake. A per-hop client (what this
  // used to build) threw that away even for a same-origin redirect, which is the
  // common shape of a signed cover URL.
  std::unique_ptr<freeink::SecureHttpClient> owned;
  freeink::SecureHttpClient* http = gKeepAliveClient.get();
  if (!http) {
    owned = makeUniqueNoThrow<freeink::SecureHttpClient>();
    if (!owned) {
      LOG_ERR("HTTP", "wolfSSL client alloc failed (free heap: %u bytes)", static_cast<unsigned>(ESP.getFreeHeap()));
      setLastFailure(HttpDownloader::FailStage::CLIENT_INIT, static_cast<int>(ESP.getFreeHeap()));
      return HttpDownloader::HTTP_ERROR;
    }
    http = owned.get();
  }

  // Hop 0 gets the same admission check every redirect target gets. A cover or
  // acquisition href arrives absolute, straight out of the feed (OpdsParser.cpp),
  // so "https://api.midad.one@evil.example/cover.jpg" can reach here without any
  // redirect being involved at all.
  const RedirectVerdict startVerdict = checkRequestUrl(startUrl);
  if (startVerdict != RedirectVerdict::Follow) {
    // Deliberately does not log the URL: this is the one case where it may carry
    // credentials in its authority.
    LOG_ERR("HTTP", "wolfSSL refusing request URL: %s", describeRedirectVerdict(startVerdict));
    setLastFailure(HttpDownloader::FailStage::OPEN, 0);
    return HttpDownloader::HTTP_ERROR;
  }

  std::string url = startUrl;
  // Views startUrl, which outlives this loop -- deliberately not `url`, which is
  // rewritten on every redirect. See RedirectOriginGuard for the rule itself.
  RedirectOriginGuard originGuard(startUrl);

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    http->setTimeout(HTTP_TIMEOUT_MS);
    http->setInsecure();
    // Redirects are this loop's to follow, never the client's. SecureHttpClient's
    // own follow loop would move between origins INSIDE one GET() call, where the
    // origin check below cannot see it and the credentials set for this hop would
    // still be attached. Zero is its default, but begin() does not reset it and the
    // client can now outlive a single request, so the invariant is stated per hop
    // instead of inherited.
    http->setFollowRedirects(0);
    if (!http->begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      setLastFailure(HttpDownloader::FailStage::OPEN, 0);
      return HttpDownloader::HTTP_ERROR;
    }
    // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
    // append a second User-Agent header, which strict servers reject (aiohttp
    // answers 400 "Duplicate 'User-Agent' header found").
    http->setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
    // setBasicAuth rather than a hand-built Authorization header: the client
    // formats it straight into the request line buffer, where the manual version
    // churned a credentials string, an Arduino String from base64::encode, the
    // "Basic " concatenation and an entry in the header vector -- per hop, per
    // request, on the heap this path is trying not to fragment.
    //
    // Set or CLEARED on every hop, never just omitted: begin() clears _headers but
    // not _authUser/_authPass (SecureHttpClient.h), so a credential set on an
    // earlier hop -- or, now that the client can be shared, on an earlier request --
    // would otherwise still be written into the next request's headers.
    if (originGuard.credentialsAllowed() && !username.empty() && !password.empty()) {
      http->setBasicAuth(username, password);
    } else {
      http->clearBasicAuth();
    }

    LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
    const int status = http->GET(
        [http, &sink](const uint8_t* data, size_t len) {
          if (http->getStatus() != 200) return true;
          if (sink.total == 0 && http->hasContentLength()) sink.total = http->getContentLength();
          if (!sink.write(data, len)) return false;
          sink.downloaded += len;
          if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
          return true;
        },
        [&sink]() { return sink.cancelFlag && *sink.cancelFlag; });

    if (http->aborted()) return HttpDownloader::ABORTED;
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed: %s", url.c_str());
      setLastFailure(HttpDownloader::FailStage::OPEN, status);
      return HttpDownloader::HTTP_ERROR;
    }
    if (isRedirect(status)) {
      // Every rejection below is a hard stop, never a "follow it anyway without
      // credentials": a redirect this device refuses to trust is a server that
      // should not be getting the request either. None of them log the URL --
      // a rejected Location is exactly the one that may carry credentials.
      const std::string location = http->getHeader("location");
      const RedirectVerdict locationVerdict = checkRedirectLocation(location);
      if (locationVerdict != RedirectVerdict::Follow) {
        LOG_ERR("HTTP", "wolfSSL refusing redirect (%d): %s", status, describeRedirectVerdict(locationVerdict));
        setLastFailure(HttpDownloader::FailStage::REDIRECT, status);
        return HttpDownloader::HTTP_ERROR;
      }
      // Resolved into its own string, not over `url`: `url` is still needed as the
      // previous hop for the downgrade check, and nothing should advance to a
      // target that has not passed it.
      std::string next;
      if (!freeink::SecureHttpClient::resolveUrl(url, location, next)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        setLastFailure(HttpDownloader::FailStage::REDIRECT, status);
        return HttpDownloader::HTTP_ERROR;
      }
      const RedirectVerdict targetVerdict = checkRedirectTarget(url, next);
      if (targetVerdict != RedirectVerdict::Follow) {
        LOG_ERR("HTTP", "wolfSSL refusing redirect (%d): %s", status, describeRedirectVerdict(targetVerdict));
        setLastFailure(HttpDownloader::FailStage::REDIRECT, status);
        return HttpDownloader::HTTP_ERROR;
      }
      url = std::move(next);
      originGuard.onHop(url);
      continue;
    }
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      setLastFailure(HttpDownloader::FailStage::STATUS, status);
      return HttpDownloader::HTTP_ERROR;
    }
    if (http->callbackAborted()) return HttpDownloader::FILE_ERROR;
    if (!http->responseComplete()) {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      setLastFailure(HttpDownloader::FailStage::INCOMPLETE,
                     static_cast<int>(std::min<size_t>(sink.downloaded, INT32_MAX)));
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  setLastFailure(HttpDownloader::FailStage::REDIRECT, MAX_REDIRECTS);
  return HttpDownloader::HTTP_ERROR;
}
#endif

#if defined(FREEINK_NET_WOLFSSL)
// --- verified-wolfSSL JSON POST ---------------------------------------------
//
// A second POST path, deliberately NOT a variant of runPostJson: it never
// touches esp_http_client or the ESP-IDF certificate bundle. The chain is
// checked against the anchors the caller hands in, on wolfSSL, whose ECDSA
// verification runs through the single-precision P-256/P-384 code
// (WOLFSSL_HAVE_SP_ECC + WOLFSSL_SP_384) instead of mbedTLS's bignum RSA path.
//
// Only what a small JSON exchange needs is implemented: one request, no
// redirect following (a redirect off the pinned host could not be verified
// against these anchors anyway, so it is surfaced to the caller as its 3xx
// status), no connection reuse, no streaming.
constexpr size_t WOLF_JSON_READ_CHUNK = 512;
constexpr size_t WOLF_JSON_MAX_RESPONSE = 16384;
constexpr size_t WOLF_JSON_MAX_LINE = 1024;

// Reads one CRLF-terminated line (CR stripped). False on timeout or a closed
// connection with nothing pending.
bool wolfReadLine(VerifiedTlsSession& tls, std::string& line, const uint32_t timeoutMs) {
  line.clear();
  uint32_t deadline = millis() + timeoutMs;
  uint8_t ch = 0;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    const int got = tls.read(&ch, 1);
    if (got < 0) return false;
    if (got == 0) {
      delay(2);
      continue;
    }
    deadline = millis() + timeoutMs;
    if (ch == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
    if (line.size() >= WOLF_JSON_MAX_LINE) return false;
    line += static_cast<char>(ch);
  }
  return false;
}

// Appends exactly `count` bytes to out. False on timeout, close or the cap.
bool wolfReadFixed(VerifiedTlsSession& tls, const size_t count, std::string& out, uint8_t* buf,
                   const uint32_t timeoutMs) {
  size_t remaining = count;
  uint32_t deadline = millis() + timeoutMs;
  while (remaining > 0) {
    const size_t want = remaining < WOLF_JSON_READ_CHUNK ? remaining : WOLF_JSON_READ_CHUNK;
    const int got = tls.read(buf, want);
    if (got < 0) return false;
    if (got == 0) {
      if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
      delay(2);
      continue;
    }
    deadline = millis() + timeoutMs;
    out.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(got));
    if (out.size() > WOLF_JSON_MAX_RESPONSE) return false;
    remaining -= static_cast<size_t>(got);
  }
  return true;
}

// Reads until the peer closes: the framing a response with neither
// Content-Length nor chunked encoding uses.
bool wolfReadUntilClose(VerifiedTlsSession& tls, std::string& out, uint8_t* buf, const uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while (true) {
    const int got = tls.read(buf, WOLF_JSON_READ_CHUNK);
    if (got < 0) return !tls.connected();  // clean end of stream
    if (got == 0) {
      if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
      delay(2);
      continue;
    }
    deadline = millis() + timeoutMs;
    out.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(got));
    if (out.size() > WOLF_JSON_MAX_RESPONSE) return false;
  }
}

bool wolfReadChunked(VerifiedTlsSession& tls, std::string& out, uint8_t* buf, const uint32_t timeoutMs) {
  std::string line;
  while (true) {
    if (!wolfReadLine(tls, line, timeoutMs)) return false;
    const size_t ext = line.find(';');  // chunk extensions are ignored
    const std::string sizeText = ext == std::string::npos ? line : line.substr(0, ext);
    char* parseEnd = nullptr;
    const unsigned long size = strtoul(sizeText.c_str(), &parseEnd, 16);
    if (parseEnd == sizeText.c_str()) return false;
    if (size == 0) {
      while (wolfReadLine(tls, line, timeoutMs) && !line.empty()) {  // drain trailers
      }
      return true;
    }
    if (!wolfReadFixed(tls, size, out, buf, timeoutMs)) return false;
    if (!wolfReadLine(tls, line, timeoutMs)) return false;  // chunk's trailing CRLF
  }
}

HttpDownloader::DownloadError runPostJsonWolf(const std::string& url, const std::string& jsonBody,
                                              const char* caAnchorsPem, std::string& outResponse, const int timeoutMs,
                                              const std::string& username, const std::string& password,
                                              const char* contentType, const char* method) {
  setLastFailure(HttpDownloader::FailStage::NONE, 0);
  outResponse.clear();
  WifiPowerSaveGuard psGuard;

  HttpUrlParts parts;
  if (!parseHttpUrl(url, parts)) {
    LOG_ERR("HTTP", "wolfSSL postJson: unparseable URL");
    setLastFailure(HttpDownloader::FailStage::OPEN, 0);
    return HttpDownloader::HTTP_ERROR;
  }
  // No plaintext path out of this function: a caller asking for pinned
  // verification over http:// has asked for something that cannot be delivered.
  if (!parts.https) {
    LOG_ERR("HTTP", "wolfSSL postJson refuses a non-https URL");
    setLastFailure(HttpDownloader::FailStage::OPEN, 0);
    return HttpDownloader::HTTP_ERROR;
  }
  if (!caAnchorsPem || !*caAnchorsPem) {
    LOG_ERR("HTTP", "wolfSSL postJson called without trust anchors");
    setLastFailure(HttpDownloader::FailStage::OPEN, 0);
    return HttpDownloader::HTTP_ERROR;
  }

  // One reused read buffer for the whole response; the line reader works a byte
  // at a time out of wolfSSL's already-decrypted record, so only the body needs it.
  auto buf = makeUniqueNoThrow<uint8_t[]>(WOLF_JSON_READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer (free heap: %u bytes)", (unsigned)WOLF_JSON_READ_CHUNK,
            ESP.getFreeHeap());
    setLastFailure(HttpDownloader::FailStage::BUFFER_OOM, static_cast<int>(ESP.getFreeHeap()));
    return HttpDownloader::HTTP_ERROR;
  }

  VerifiedTlsSession tls;
  if (!tls.connect(parts.host.c_str(), parts.port, caAnchorsPem, static_cast<uint32_t>(timeoutMs))) {
    setLastFailure(HttpDownloader::FailStage::OPEN, tls.lastError());
    return HttpDownloader::HTTP_ERROR;
  }

  std::string request;
  request.reserve(256 + jsonBody.size());
  request += std::string(method) + " " + parts.path + " HTTP/1.1\r\n";
  request += "Host: " + parts.host;
  if (parts.port != 443) request += ":" + std::to_string(parts.port);
  request += "\r\n";
  request += "User-Agent: CrossPoint-ESP32-" CROSSPOINT_VERSION "\r\n";
  request += std::string("Content-Type: ") + (contentType ? contentType : "application/json") + "\r\n";
  request += "Accept: application/json\r\n";
  // Same host-gated device identifier every other request carries, so
  // server-side revocation keeps working on this path too.
  if (isFouladEbooksUrl(url)) {
    const std::string serial = FouladDeviceTracking::getSerialNumber();
    if (!serial.empty()) request += "X-Device-Serial: " + serial + "\r\n";
  }
  if (!username.empty() && !password.empty()) {
    const std::string credentials = username + ":" + password;
    request += "Authorization: Basic " + std::string(base64::encode(credentials.c_str()).c_str()) + "\r\n";
  }
  request += "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += jsonBody;

  if (tls.write(reinterpret_cast<const uint8_t*>(request.data()), request.size()) < 0) {
    LOG_ERR("HTTP", "wolfSSL postJson write failed");
    setLastFailure(HttpDownloader::FailStage::OPEN, tls.lastError());
    return HttpDownloader::HTTP_ERROR;
  }
  request.clear();
  request.shrink_to_fit();

  std::string line;
  if (!wolfReadLine(tls, line, static_cast<uint32_t>(timeoutMs)) || line.size() < 12) {
    LOG_ERR("HTTP", "wolfSSL postJson: no status line");
    setLastFailure(HttpDownloader::FailStage::READ, 0);
    return HttpDownloader::HTTP_ERROR;
  }
  const int status = atoi(line.c_str() + 9);  // "HTTP/1.1 201 Created"

  size_t contentLength = 0;
  bool haveContentLength = false;
  bool chunked = false;
  while (wolfReadLine(tls, line, static_cast<uint32_t>(timeoutMs))) {
    if (line.empty()) break;  // end of headers
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    if (strcasecmp(name.c_str(), "content-length") == 0) {
      contentLength = static_cast<size_t>(strtoul(value.c_str(), nullptr, 10));
      haveContentLength = true;
    } else if (strcasecmp(name.c_str(), "transfer-encoding") == 0) {
      std::transform(value.begin(), value.end(), value.begin(),
                     [](unsigned char c) { return static_cast<char>(tolower(c)); });
      chunked = value.find("chunked") != std::string::npos;
    }
  }

  // Any 2xx: /device-login/start answers 201 Created with the pairing payload.
  if (status < 200 || status > 299) {
    // Not necessarily worth logging loudly -- a 401 from /device-login/poll is
    // the documented "this session is gone". Callers read getLastFailure().detail.
    LOG_DBG("HTTP", "wolfSSL postJson non-2xx status: %d", status);
    setLastFailure(HttpDownloader::FailStage::STATUS, status);
    HEAP_TRACE_MARK("HTTP.cleanup", "BEGIN");
    tls.stop();
    HEAP_TRACE_MARK("HTTP.cleanup", "END");
    return HttpDownloader::HTTP_ERROR;
  }

  bool bodyOk;
  if (chunked)
    bodyOk = wolfReadChunked(tls, outResponse, buf.get(), static_cast<uint32_t>(timeoutMs));
  else if (haveContentLength)
    bodyOk = wolfReadFixed(tls, contentLength, outResponse, buf.get(), static_cast<uint32_t>(timeoutMs));
  else
    bodyOk = wolfReadUntilClose(tls, outResponse, buf.get(), static_cast<uint32_t>(timeoutMs));

  HEAP_TRACE_MARK("HTTP.cleanup", "BEGIN");
  tls.stop();
  HEAP_TRACE_MARK("HTTP.cleanup", "END");

  if (!bodyOk) {
    LOG_ERR("HTTP", "wolfSSL postJson body incomplete after %zu bytes", outResponse.size());
    setLastFailure(HttpDownloader::FailStage::INCOMPLETE,
                   static_cast<int>(std::min<size_t>(outResponse.size(), INT32_MAX)));
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

// --- Verified GET ---------------------------------------------------------
//
// The SDK's SecureHttpClient cannot serve this path. SecureClient::connect()
// loads a CA when one is set, but never raises WOLFSSL_VERIFY_PEER and never
// calls wolfSSL_check_domain_name() -- and it exposes no WOLFSSL handle for a
// caller to add the check on top (VerifiedTlsSession.h states the same). SNI
// only tells the server which certificate to send; it proves nothing. So every
// request to a Midad Core host runs here, on a session whose verification
// cannot be switched off, rather than on setInsecure().
//
// Bodies stream straight to the sink. A cover is tens of KB and this device has
// neither the free heap nor the contiguous block to hold one as a std::string.

bool wolfStreamFixed(VerifiedTlsSession& tls, const size_t count, Sink& sink, uint8_t* buf, const uint32_t timeoutMs) {
  size_t remaining = count;
  uint32_t deadline = millis() + timeoutMs;
  while (remaining > 0) {
    if (sink.cancelFlag && *sink.cancelFlag) return false;
    const size_t want = remaining < WOLF_JSON_READ_CHUNK ? remaining : WOLF_JSON_READ_CHUNK;
    const int got = tls.read(buf, want);
    if (got < 0) return false;
    if (got == 0) {
      if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
      delay(1);
      continue;
    }
    if (!sink.write(buf, static_cast<size_t>(got))) return false;
    sink.downloaded += static_cast<size_t>(got);
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
    remaining -= static_cast<size_t>(got);
    deadline = millis() + timeoutMs;
  }
  return true;
}

bool wolfStreamUntilClose(VerifiedTlsSession& tls, Sink& sink, uint8_t* buf, const uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  for (;;) {
    if (sink.cancelFlag && *sink.cancelFlag) return false;
    const int got = tls.read(buf, WOLF_JSON_READ_CHUNK);
    if (got < 0) return true;  // clean end of stream: the close IS the terminator
    if (got == 0) {
      if (!tls.connected()) return true;
      if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
      delay(1);
      continue;
    }
    if (!sink.write(buf, static_cast<size_t>(got))) return false;
    sink.downloaded += static_cast<size_t>(got);
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
    deadline = millis() + timeoutMs;
  }
}

bool wolfStreamChunked(VerifiedTlsSession& tls, Sink& sink, uint8_t* buf, const uint32_t timeoutMs) {
  std::string line;
  for (;;) {
    if (!wolfReadLine(tls, line, timeoutMs)) return false;
    if (line.empty()) continue;  // trailing CRLF of the previous chunk
    const size_t size = static_cast<size_t>(strtoul(line.c_str(), nullptr, 16));
    if (size == 0) return true;  // last chunk; trailers are not read back
    if (!wolfStreamFixed(tls, size, sink, buf, timeoutMs)) return false;
    if (!wolfReadLine(tls, line, timeoutMs)) return false;  // chunk terminator
  }
}

// --- Ranged download ------------------------------------------------------
//
// A whole-file GET fails mid-stream on this device. Measured: a book download died at
// 49% with wolfSSL MEMORY_E, and the instrumented run showed why -- MaxAlloc was 10,740
// when the transfer started and 3,444 at its lowest DURING it. The transfer itself
// erodes the contiguous block, so no amount of freeing beforehand can rescue it, and
// wolfSSL sizes its receive buffer to whatever record arrives.
//
// So the response is bounded at the source instead of hoping the peer negotiates a
// small record: each request asks for RANGE_CHUNK_BYTES and nothing larger can come
// back. A 206 and a well-formed Content-Range are REQUIRED -- a server that answers 200
// would stream the whole body and put us back where we started, so that is a failure
// here, not a fallback.
//
// Bytes land in a temp file and the destination is only created by rename() once the
// byte count matches the total the server declared. A partial download therefore never
// appears as a book.
constexpr size_t RANGE_CHUNK_BYTES = 4096;
constexpr int RANGE_MAX_CHUNK_RETRIES = 2;

// Parses "bytes <start>-<end>/<total>". Returns false on any shape that is not exactly
// that, including "*" totals -- an unknown total cannot be validated against.
bool parseContentRange(const std::string& value, size_t& outStart, size_t& outEnd, size_t& outTotal) {
  const char* p = value.c_str();
  while (*p == ' ') p++;
  if (strncasecmp(p, "bytes", 5) != 0) return false;
  p += 5;
  while (*p == ' ') p++;
  char* end = nullptr;
  const unsigned long start = strtoul(p, &end, 10);
  if (end == p || *end != '-') return false;
  p = end + 1;
  const unsigned long last = strtoul(p, &end, 10);
  if (end == p || *end != '/') return false;
  p = end + 1;
  if (*p == '*') return false;
  const unsigned long total = strtoul(p, &end, 10);
  if (end == p || total == 0 || last < start || last >= total) return false;
  outStart = start;
  outEnd = last;
  outTotal = total;
  return true;
}

// rangeHeader, when non-null, is sent as `Range:` and makes a 206 with a well-formed
// Content-Range MANDATORY -- a 200 means the peer ignored the range and would stream the
// whole body, which is the exact failure this exists to prevent, so it is refused.
HttpDownloader::DownloadError runGetVerified(const std::string& startUrl, const std::string& username,
                                             const std::string& password, Sink& sink, const char* anchorsPem,
                                             const char* rangeHeader = nullptr, size_t* outRangeStart = nullptr,
                                             size_t* outRangeEnd = nullptr, size_t* outRangeTotal = nullptr) {
  setLastFailure(HttpDownloader::FailStage::NONE, 0);
  WifiPowerSaveGuard psGuard;

  if (!anchorsPem || !*anchorsPem) {
    LOG_ERR("HTTP", "verified GET called without trust anchors");
    setLastFailure(HttpDownloader::FailStage::OPEN, 0);
    return HttpDownloader::HTTP_ERROR;
  }
  // Hop 0 gets the same admission check every redirect target gets: a cover href
  // arrives absolute out of the feed, so a userinfo authority can reach here with
  // no redirect involved at all.
  const RedirectVerdict startVerdict = checkRequestUrl(startUrl);
  if (startVerdict != RedirectVerdict::Follow) {
    LOG_ERR("HTTP", "verified GET refusing request URL: %s", describeRedirectVerdict(startVerdict));
    setLastFailure(HttpDownloader::FailStage::OPEN, 0);
    return HttpDownloader::HTTP_ERROR;
  }

  auto buf = makeUniqueNoThrow<uint8_t[]>(WOLF_JSON_READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer (free heap: %u bytes)", (unsigned)WOLF_JSON_READ_CHUNK,
            ESP.getFreeHeap());
    setLastFailure(HttpDownloader::FailStage::BUFFER_OOM, static_cast<int>(ESP.getFreeHeap()));
    return HttpDownloader::HTTP_ERROR;
  }

  std::string url = startUrl;
  RedirectOriginGuard originGuard(startUrl);
  const uint32_t timeoutMs = static_cast<uint32_t>(HTTP_TIMEOUT_MS);
  // The hop a reused session has already been retried on, so one stale keep-alive
  // costs one extra handshake and never loops.
  int retriedHop = -1;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    HttpUrlParts parts;
    if (!parseHttpUrl(url, parts)) {
      LOG_ERR("HTTP", "verified GET: unparseable URL");
      setLastFailure(HttpDownloader::FailStage::OPEN, 0);
      return HttpDownloader::HTTP_ERROR;
    }
    // There is no plaintext way out of this function. An https -> http redirect
    // is refused by checkRedirectTarget below, and a caller asking for pinned
    // verification over http:// has asked for something undeliverable.
    if (!parts.https) {
      LOG_ERR("HTTP", "verified GET refuses a non-https URL");
      setLastFailure(HttpDownloader::FailStage::OPEN, 0);
      return HttpDownloader::HTTP_ERROR;
    }

    const std::string origin = parts.host + ":" + std::to_string(parts.port);
    const bool keepAlive = gKeepAliveDepth > 0;

    // Reuse the held session only for the same origin. A different origin gets a
    // fresh handshake -- a TLS session is never carried across origins.
    VerifiedTlsSession* tls = nullptr;
    std::unique_ptr<VerifiedTlsSession> owned;
    bool reusedSession = false;
    // A held session is reusable only when it is for this exact origin, has
    // never taken a fatal error, is still connected, AND the heap can still
    // afford the fresh handshake a failed reuse would need.
    if (keepAlive && gKeepAliveVerified && gKeepAliveVerifiedOrigin == origin && !gKeepAliveVerified->poisoned() &&
        gKeepAliveVerified->connected() && ESP.getMaxAllocHeap() >= KEEPALIVE_MIN_CONTIGUOUS_BYTES) {
      tls = gKeepAliveVerified.get();
      reusedSession = true;
    } else {
      if (keepAlive) dropKeepAliveVerified();
      owned = makeUniqueNoThrow<VerifiedTlsSession>();
      if (!owned) {
        LOG_ERR("HTTP", "verified session alloc failed (free heap: %u bytes)",
                static_cast<unsigned>(ESP.getFreeHeap()));
        setLastFailure(HttpDownloader::FailStage::CLIENT_INIT, static_cast<int>(ESP.getFreeHeap()));
        return HttpDownloader::HTTP_ERROR;
      }
      if (!owned->connect(parts.host.c_str(), parts.port, anchorsPem, timeoutMs)) {
        LOG_ERR("HTTP", "verified handshake failed: %d (free heap: %u bytes)", owned->lastError(),
                static_cast<unsigned>(ESP.getFreeHeap()));
        setLastFailure(HttpDownloader::FailStage::OPEN, owned->lastError());
        return HttpDownloader::HTTP_ERROR;
      }
      if (keepAlive) {
        gKeepAliveVerified = std::move(owned);
        gKeepAliveVerifiedOrigin = origin;
        tls = gKeepAliveVerified.get();
      } else {
        tls = owned.get();
      }
    }

    std::string request;
    request.reserve(320);
    request += "GET " + parts.path + " HTTP/1.1\r\n";
    request += "Host: " + parts.host;
    if (parts.port != 443) request += ":" + std::to_string(parts.port);
    request += "\r\n";
    request += "User-Agent: CrossPoint-ESP32-" CROSSPOINT_VERSION "\r\n";
    // Credentials follow the same rule as every other path: never written once the
    // chain has left the origin they were issued for.
    if (originGuard.credentialsAllowed() && !username.empty() && !password.empty()) {
      request +=
          "Authorization: Basic " + std::string(base64::encode((username + ":" + password).c_str()).c_str()) + "\r\n";
      if (isFouladEbooksUrl(url)) {
        const std::string serial = FouladDeviceTracking::getSerialNumber();
        if (!serial.empty()) request += "X-Device-Serial: " + serial + "\r\n";
      }
    }
    if (rangeHeader != nullptr) {
      request += "Range: ";
      request += rangeHeader;
      request += "\r\n";
    }
    request += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";

    if (tls->write(reinterpret_cast<const uint8_t*>(request.data()), request.size()) < 0) {
      // A held connection the server closed while idle fails here, on a socket
      // that looked alive. That is not a request failure, so a REUSED session
      // gets one fresh handshake before the request is given up on. A session
      // this function opened itself has no such excuse.
      if (reusedSession && retriedHop != hop) {
        retriedHop = hop;
        dropKeepAliveVerified();
        --hop;
        continue;
      }
      LOG_ERR("HTTP", "verified GET write failed");
      setLastFailure(HttpDownloader::FailStage::OPEN, tls->lastError());
      if (keepAlive) dropKeepAliveVerified();
      return HttpDownloader::HTTP_ERROR;
    }
    request.clear();
    request.shrink_to_fit();

    std::string line;
    if (!wolfReadLine(*tls, line, timeoutMs) || line.size() < 12) {
      // Same one-shot retry as the write above: a closed keep-alive connection
      // most often surfaces here, with the request written into a dead socket
      // and no status line coming back.
      if (reusedSession && retriedHop != hop) {
        retriedHop = hop;
        dropKeepAliveVerified();
        --hop;
        continue;
      }
      LOG_ERR("HTTP", "verified GET: no status line");
      setLastFailure(HttpDownloader::FailStage::READ, 0);
      if (keepAlive) dropKeepAliveVerified();
      return HttpDownloader::HTTP_ERROR;
    }
    const int status = atoi(line.c_str() + 9);

    std::string location;
    std::string contentRange;
    size_t contentLength = 0;
    bool haveContentLength = false;
    bool chunked = false;
    bool serverCloses = false;
    while (wolfReadLine(*tls, line, timeoutMs)) {
      if (line.empty()) break;
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      const std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ') value.erase(value.begin());
      if (strcasecmp(name.c_str(), "content-length") == 0) {
        contentLength = static_cast<size_t>(strtoul(value.c_str(), nullptr, 10));
        haveContentLength = true;
      } else if (strcasecmp(name.c_str(), "content-range") == 0) {
        contentRange = value;
      } else if (strcasecmp(name.c_str(), "location") == 0) {
        location = value;
      } else if (strcasecmp(name.c_str(), "transfer-encoding") == 0) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(tolower(c)); });
        chunked = value.find("chunked") != std::string::npos;
      } else if (strcasecmp(name.c_str(), "connection") == 0) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(tolower(c)); });
        serverCloses = value.find("close") != std::string::npos;
      }
    }

    if (isRedirect(status)) {
      if (location.empty()) {
        LOG_ERR("HTTP", "verified GET: %d with no Location", status);
        setLastFailure(HttpDownloader::FailStage::REDIRECT, status);
        return HttpDownloader::HTTP_ERROR;
      }
      const RedirectVerdict locationVerdict = checkRedirectLocation(location);
      if (locationVerdict != RedirectVerdict::Follow) {
        LOG_ERR("HTTP", "verified GET refusing redirect (%d): %s", status, describeRedirectVerdict(locationVerdict));
        setLastFailure(HttpDownloader::FailStage::REDIRECT, status);
        return HttpDownloader::HTTP_ERROR;
      }
      std::string resolved;
      if (!freeink::SecureHttpClient::resolveUrl(url, location, resolved)) {
        LOG_ERR("HTTP", "verified GET: bad redirect target (%d)", status);
        setLastFailure(HttpDownloader::FailStage::REDIRECT, status);
        return HttpDownloader::HTTP_ERROR;
      }
      const RedirectVerdict targetVerdict = checkRedirectTarget(url, resolved);
      if (targetVerdict != RedirectVerdict::Follow) {
        LOG_ERR("HTTP", "verified GET refusing redirect (%d): %s", status, describeRedirectVerdict(targetVerdict));
        setLastFailure(HttpDownloader::FailStage::REDIRECT, status);
        return HttpDownloader::HTTP_ERROR;
      }
      // Drain the redirect body so a kept-alive connection stays in sync.
      if (haveContentLength && contentLength > 0) {
        Sink discard;
        discard.write = [](const uint8_t*, size_t) { return true; };
        wolfStreamFixed(*tls, contentLength, discard, buf.get(), timeoutMs);
      } else if (chunked) {
        Sink discard;
        discard.write = [](const uint8_t*, size_t) { return true; };
        wolfStreamChunked(*tls, discard, buf.get(), timeoutMs);
      } else if (keepAlive) {
        dropKeepAliveVerified();  // body length unknown: the session cannot be reused
      }
      url = std::move(resolved);
      originGuard.onHop(url);
      continue;
    }

    if (rangeHeader != nullptr) {
      if (status != 206) {
        LOG_ERR("HTTP", "ranged request answered %d, not 206", status);
        setLastFailure(HttpDownloader::FailStage::STATUS, status);
        if (keepAlive) dropKeepAliveVerified();
        return HttpDownloader::HTTP_ERROR;
      }
      size_t rs = 0, re = 0, rt = 0;
      if (!parseContentRange(contentRange, rs, re, rt)) {
        LOG_ERR("HTTP", "ranged request: unusable Content-Range");
        setLastFailure(HttpDownloader::FailStage::STATUS, status);
        if (keepAlive) dropKeepAliveVerified();
        return HttpDownloader::HTTP_ERROR;
      }
      if (outRangeStart) *outRangeStart = rs;
      if (outRangeEnd) *outRangeEnd = re;
      if (outRangeTotal) *outRangeTotal = rt;
    } else if (status != 200) {
      LOG_DBG("HTTP", "verified GET non-200 status: %d", status);
      setLastFailure(HttpDownloader::FailStage::STATUS, status);
      if (keepAlive && !haveContentLength && !chunked) dropKeepAliveVerified();
      return HttpDownloader::HTTP_ERROR;
    }

    if (haveContentLength) sink.total = contentLength;
    bool ok;
    if (chunked) {
      ok = wolfStreamChunked(*tls, sink, buf.get(), timeoutMs);
    } else if (haveContentLength) {
      ok = wolfStreamFixed(*tls, contentLength, sink, buf.get(), timeoutMs);
    } else {
      ok = wolfStreamUntilClose(*tls, sink, buf.get(), timeoutMs);
      serverCloses = true;
    }
    if (!ok) {
      setLastFailure(HttpDownloader::FailStage::READ, 0);
      if (keepAlive) dropKeepAliveVerified();
      return HttpDownloader::HTTP_ERROR;
    }
    if (keepAlive && serverCloses) dropKeepAliveVerified();
    return HttpDownloader::OK;
  }

  LOG_ERR("HTTP", "verified GET: too many redirects");
  setLastFailure(HttpDownloader::FailStage::REDIRECT, MAX_REDIRECTS);
  return HttpDownloader::HTTP_ERROR;
}

#endif  // FREEINK_NET_WOLFSSL

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
//
// Only ever verifies against the default crt_bundle -- a prior version of this
// function fell back to a single pinned root (Let's Encrypt's newer "ISRG Root YE"
// hierarchy, not yet in ESP-IDF's embedded bundle) when the default bundle couldn't
// resolve a chain. That fix was cryptographically correct but dangerous in practice:
// a real device log showed completing that fallback handshake drove free heap from
// a ~57KB baseline down to just 5.4KB, and the device froze shortly after -- verifying
// against a single non-bundled root apparently forces mbedTLS to process the entire
// certificate chain the server sends (4 certificates), which is far more memory-hungry
// than the default bundle's fast lookup-and-reject path, and this device doesn't have
// the headroom to safely absorb that. Removed rather than risk another freeze; hosts
// whose certificate chain the default bundle can't resolve will fail cleanly with
// ESP_ERR_HTTP_CONNECT instead, same as unmodified upstream crosspoint-reader.
// Response headers are only reachable through the event stream --
// esp_http_client_get_header() reads back a REQUEST header, not the server's reply
// (see its own doc comment in esp_http_client.h). Fires once per header line;
// header_key is compared case-insensitively because the field name is not
// case-sensitive and GitHub's is "ETag" while a CDN in front of it may differ.
esp_err_t captureResponseEtag(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data) return ESP_OK;
  if (!evt->header_key || !evt->header_value) return ESP_OK;
  if (strcasecmp(evt->header_key, "ETag") != 0) return ESP_OK;
  // Last one wins: a redirect hop reopens the client and replays its own headers,
  // so the final response's ETag is the one still standing when runGet returns.
  static_cast<HttpDownloader::ConditionalGet*>(evt->user_data)->etag = evt->header_value;
  return ESP_OK;
}

HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink, HttpDownloader::ConditionalGet* conditional = nullptr) {
  setLastFailure(HttpDownloader::FailStage::NONE, 0);
  WifiPowerSaveGuard psGuard;

  // Reserve the read buffer before opening the connection, not after. An active TLS
  // session holds its own sizable internal buffers (mbedTLS's handshake/record buffers)
  // for as long as the connection stays open; confirmed on a real device that even with
  // a healthy free-heap reading right before connecting, a completed TLS handshake left
  // too little *contiguous* heap for this one small allocation afterward. Grabbing this
  // buffer first -- before the handshake's own larger allocations can fragment the
  // heap -- avoids losing the race for a small block after a big one has already carved
  // up the free space.
  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer (free heap: %u bytes)", (unsigned)READ_CHUNK, ESP.getFreeHeap());
    setLastFailure(HttpDownloader::FailStage::BUFFER_OOM, static_cast<int>(ESP.getFreeHeap()));
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  // Verify HTTPS against the bundled CA roots. This build has esp-tls
  // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
  // up at all; the model is public servers over verified https and local
  // servers over plain http (esp_http_client picks the transport from the URL
  // scheme, so http:// needs no cert config). The prior setInsecure() worked
  // only because Arduino's ssl_client drives mbedtls directly.
  // TEMP diagnostic: see the matching note in runPostJson -- the bundle is parsed
  // inside the handshake, not here.
  HEAP_TRACE_MARK("HTTP.bundleAttach", "BEGIN");
  config.crt_bundle_attach = esp_crt_bundle_attach;
  HEAP_TRACE_MARK("HTTP.bundleAttach", "END");
  config.keep_alive_enable = true;
  if (conditional) {
    config.event_handler = captureResponseEtag;
    config.user_data = conditional;
  }

  HEAP_TRACE_MARK("HTTP.clientInit", "BEGIN");
  esp_http_client_handle_t client = esp_http_client_init(&config);
  HEAP_TRACE_MARK("HTTP.clientInit", "END");
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    setLastFailure(HttpDownloader::FailStage::CLIENT_INIT, 0);
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  // Set on the handle, so it survives the manual redirect hops below (which
  // reopen the same client rather than building a new one).
  setDeviceSerialHeader(client, url);
  if (conditional && !conditional->ifNoneMatch.empty()) {
    esp_http_client_set_header(client, "If-None-Match", conditional->ifNoneMatch.c_str());
  }
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  HEAP_TRACE_MARK("TLS.handshake", "BEGIN");
  esp_err_t err = esp_http_client_open(client, 0);
  HEAP_TRACE_MARK("TLS.handshake", err == ESP_OK ? "END-OK" : "END-FAIL");
  if (err != ESP_OK) {
    // ESP_ERR_HTTP_CONNECT alone doesn't say whether this was heap pressure, DNS
    // failure, TCP-level rejection/timeout, or a TLS handshake problem, so also
    // surface the underlying socket errno -- ETIMEDOUT/ECONNREFUSED/EHOSTUNREACH/etc.
    // point at the network path itself; an mbedTLS-range negative value points at the
    // TLS handshake instead (see logTlsError).
    LOG_ERR("HTTP", "open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(err), getHttpClientErrno(client),
            ESP.getFreeHeap());
    logTlsError(client);
    HEAP_TRACE_REPORT_FAILURE("get open");
    setLastFailure(HttpDownloader::FailStage::OPEN, static_cast<int>(err));
    HEAP_TRACE_MARK("HTTP.cleanup", "BEGIN");
    esp_http_client_cleanup(client);
    HEAP_TRACE_MARK("HTTP.cleanup", "END");
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < MAX_REDIRECTS; ++hop) {
    // Close before following. The redirect response carries a body of its own
    // (GitHub's API answers a renamed repository with 301 + a 215-byte JSON
    // payload), and keep_alive_enable means the socket is reused -- so reopening
    // without consuming that body left it queued in front of the next response.
    // The header parser then read the leftovers instead of the status line,
    // fetch_headers() failed, and the status code was never populated: the caller
    // saw FailStage::STATUS with a nonsensical detail of -1.
    //
    // That path had simply never run before. Nothing this firmware talks to
    // redirected on the check endpoint until the GitHub repository was renamed,
    // at which point every device on a build predating the new URL began failing
    // its update check outright, reporting "http 5:-1".
    //
    // Closing rather than draining is deliberate: it costs a fresh handshake on a
    // redirect (rare, and unavoidable anyway when the host changes, as it does on
    // the release CDN hop), and in exchange the next response is parsed on a
    // connection that cannot be carrying anything stale -- correct regardless of
    // whether a body was present, chunked, or truncated.
    LOG_DBG("HTTP", "following redirect %d (status %d)", hop + 1, status);
    esp_http_client_close(client);
    if (esp_http_client_set_redirection(client) != ESP_OK) break;

    // esp_http_client_set_redirection() only refuses an HTTPS-to-HTTP downgrade;
    // it does not refuse a same-scheme redirect to a different host. Both
    // Authorization (Basic-auth callers) and X-Device-Serial (any Foulad
    // eBooks request, see setDeviceSerialHeader() above) stay on this reused
    // client handle across the hop, so a compromised or malicious server could
    // otherwise redirect to attacker infrastructure and receive either. Runs
    // for every request, not just credentialed ones -- a serial-only request
    // needs the same origin check. Strips once the origin changes; neither
    // header comes back even if a later hop redirects back to the original
    // origin.
    {
      // Reuses `buf` (already allocated above, READ_CHUNK bytes, idle until the
      // body-read loop after this one) instead of a fresh heap allocation here.
      // This runs right before esp_http_client_open() below re-establishes the
      // TLS connection to the redirect target -- on the CDN hop, that connection
      // needs a large contiguous block for RSA signature verification
      // (crt_bundle_attach), so an extra alloc/free cycle in this exact window
      // is itself a fragmentation risk, not just a cost. A real device hit
      // exactly this: PK verify failed with an MPI/RSA allocation error on the
      // CDN hop, with the largest contiguous block down to ~21KB by then.
      const bool sameOrigin =
          esp_http_client_get_url(client, buf.get(), READ_CHUNK) == ESP_OK && urlOrigin(buf.get()) == urlOrigin(url);
      if (!sameOrigin) {
        LOG_DBG("HTTP", "redirect changed origin -- dropping Authorization/X-Device-Serial headers");
        esp_http_client_delete_header(client, "Authorization");
        esp_http_client_delete_header(client, "X-Device-Serial");
      }
    }

    esp_http_client_close(client);
    HEAP_TRACE_MARK("TLS.handshake.redirect", "BEGIN");
    err = esp_http_client_open(client, 0);
    HEAP_TRACE_MARK("TLS.handshake.redirect", err == ESP_OK ? "END-OK" : "END-FAIL");
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(err),
              getHttpClientErrno(client), ESP.getFreeHeap());
      logTlsError(client);
      HEAP_TRACE_REPORT_FAILURE("redirect open");
      setLastFailure(HttpDownloader::FailStage::REDIRECT, static_cast<int>(err));
      HEAP_TRACE_MARK("HTTP.cleanup", "BEGIN");
      esp_http_client_cleanup(client);
      HEAP_TRACE_MARK("HTTP.cleanup", "END");
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  // 304 is a success, not a failure: the caller asked "has this changed?" and got a
  // definitive no. There is no body to read, so return before the read loop and let
  // the caller fall back to whatever it cached alongside the ETag.
  if (conditional && status == 304) {
    conditional->notModified = true;
    LOG_DBG("HTTP", "304 Not Modified (cached copy still current)");
    esp_http_client_cleanup(client);
    return HttpDownloader::OK;
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    setLastFailure(HttpDownloader::FailStage::STATUS, status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // Visibility into how much heap an established TLS session actually leaves behind --
  // added after a real device connected successfully but then failed the (now
  // pre-reserved, see above) read buffer allocation, implying the active session's own
  // buffers eat deeply into what looked like healthy free heap right before connecting.
  LOG_INF("HTTP", "connected, free heap: %u bytes", ESP.getFreeHeap());

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      setLastFailure(HttpDownloader::FailStage::READ, static_cast<int>(std::min<size_t>(sink.downloaded, INT32_MAX)));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    setLastFailure(HttpDownloader::FailStage::INCOMPLETE,
                   static_cast<int>(std::min<size_t>(sink.downloaded, INT32_MAX)));
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

// Streams a file as a multipart/form-data POST body (never buffers the whole
// file into RAM) plus plain string fields, then reads a small, expected-JSON
// response body into outResponse. Mirrors runGet()'s manual open/read
// structure rather than esp_http_client_perform() (same reasons as runGet's
// comment), plus streaming the outbound body ourselves via
// esp_http_client_write() between esp_http_client_open()'s write_len and
// esp_http_client_fetch_headers() -- the standard esp_http_client pattern for
// a POST with a known Content-Length.
HttpDownloader::DownloadError runPostFile(const std::string& url,
                                          const std::vector<std::pair<std::string, std::string>>& files,
                                          const std::vector<std::pair<std::string, std::string>>& fields,
                                          std::string& outResponse, int timeoutMs, const std::string& username,
                                          const std::string& password) {
  setLastFailure(HttpDownloader::FailStage::NONE, 0);
  outResponse.clear();
  WifiPowerSaveGuard psGuard;

  // Pass 1: size every file up front -- the total Content-Length must be
  // known before opening the connection (esp_http_client_open()'s write_len
  // needs the real total). Files are re-opened one at a time while streaming,
  // so at most one SD handle is held at once.
  std::vector<size_t> fileSizes;
  fileSizes.reserve(files.size());
  for (const auto& f : files) {
    HalFile file;
    if (!Storage.openFileForRead("HTTP", f.second.c_str(), file)) {
      LOG_ERR("HTTP", "postFileMultipart: failed to open %s", f.second.c_str());
      return HttpDownloader::FILE_ERROR;
    }
    fileSizes.push_back(file.size());
  }

  const std::string boundary = "----FouladEInkBoundary7f3c9a";

  // Build every part's header text up front (see Content-Length note above) --
  // only the files' own bytes stream in READ_CHUNK pieces, never held whole
  // in RAM. Each file part ends with its own "\r\n" terminator; the closing
  // boundary no longer carries it.
  std::vector<std::string> fieldParts;
  fieldParts.reserve(fields.size());
  for (const auto& field : fields) {
    fieldParts.push_back("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + field.first + "\"\r\n\r\n" +
                         field.second + "\r\n");
  }
  std::vector<std::string> filePartHeaders;
  filePartHeaders.reserve(files.size());
  for (const auto& f : files) {
    // Basename only -- multipart filename= doesn't need the full SD path.
    std::string fileName = f.second;
    const size_t slash = fileName.find_last_of('/');
    if (slash != std::string::npos) fileName = fileName.substr(slash + 1);
    filePartHeaders.push_back("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + f.first +
                              "\"; filename=\"" + fileName + "\"\r\nContent-Type: application/octet-stream\r\n\r\n");
  }
  const std::string closing = "--" + boundary + "--\r\n";

  size_t contentLength = closing.size();
  for (const auto& p : fieldParts) contentLength += p.size();
  for (size_t i = 0; i < files.size(); i++) {
    contentLength += filePartHeaders[i].size() + fileSizes[i] + 2;  // +2: the part's "\r\n"
  }

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer (free heap: %u bytes)", (unsigned)READ_CHUNK, ESP.getFreeHeap());
    setLastFailure(HttpDownloader::FailStage::BUFFER_OOM, static_cast<int>(ESP.getFreeHeap()));
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = timeoutMs;
  // TEMP diagnostic: assigning the hook costs nothing; esp-tls parses the bundle
  // inside the handshake, so the cost shows up under TLS.handshake, not here.
  HEAP_TRACE_MARK("HTTP.bundleAttach", "BEGIN");
  config.crt_bundle_attach = esp_crt_bundle_attach;
  HEAP_TRACE_MARK("HTTP.bundleAttach", "END");
  config.keep_alive_enable = true;

  HEAP_TRACE_MARK("HTTP.clientInit", "BEGIN");
  esp_http_client_handle_t client = esp_http_client_init(&config);
  HEAP_TRACE_MARK("HTTP.clientInit", "END");
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    setLastFailure(HttpDownloader::FailStage::CLIENT_INIT, 0);
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  setDeviceSerialHeader(client, url);
  const std::string contentType = "multipart/form-data; boundary=" + boundary;
  esp_http_client_set_header(client, "Content-Type", contentType.c_str());
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, same convention as runGet/runPostJson.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  esp_err_t err = esp_http_client_open(client, static_cast<int>(contentLength));
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "postFileMultipart open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(err),
            getHttpClientErrno(client), ESP.getFreeHeap());
    logTlsError(client);
    setLastFailure(HttpDownloader::FailStage::OPEN, static_cast<int>(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  auto writeAll = [&](const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
      const int written = httpClientWrite(client, data + off, static_cast<int>(len - off));
      if (written < 0) return false;
      off += static_cast<size_t>(written);
    }
    return true;
  };

  bool writeOk = true;
  for (const auto& p : fieldParts) {
    if (!writeAll(p.data(), p.size())) {
      writeOk = false;
      break;
    }
  }

  // Stream each file's body in READ_CHUNK pieces, same size as the download
  // path's read buffer -- never holds more than one chunk (or one open SD
  // handle) in RAM. Reset the watchdog per chunk, matching
  // handleFontUploadData()'s existing pattern for a comparably long SD
  // read/write loop.
  for (size_t i = 0; writeOk && i < files.size(); i++) {
    if (!writeAll(filePartHeaders[i].data(), filePartHeaders[i].size())) {
      writeOk = false;
      break;
    }
    HalFile file;
    if (!Storage.openFileForRead("HTTP", files[i].second.c_str(), file)) {
      LOG_ERR("HTTP", "postFileMultipart: reopen failed: %s", files[i].second.c_str());
      writeOk = false;
      break;
    }
    size_t remaining = fileSizes[i];
    while (remaining > 0) {
      esp_task_wdt_reset();
      const size_t want = std::min(remaining, READ_CHUNK);
      const int got = file.read(reinterpret_cast<uint8_t*>(buf.get()), want);
      if (got <= 0) {
        writeOk = false;
        break;
      }
      if (!writeAll(buf.get(), static_cast<size_t>(got))) {
        writeOk = false;
        break;
      }
      remaining -= static_cast<size_t>(got);
    }
    if (writeOk) writeOk = writeAll("\r\n", 2);
  }

  if (writeOk) writeOk = writeAll(closing.data(), closing.size());

  if (!writeOk) {
    LOG_ERR("HTTP", "postFileMultipart write failed (errno=%d)", getHttpClientErrno(client));
    setLastFailure(HttpDownloader::FailStage::OPEN, getHttpClientErrno(client));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    LOG_ERR("HTTP", "postFileMultipart unexpected status: %d", status);
    setLastFailure(HttpDownloader::FailStage::STATUS, status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "postFileMultipart read error after %zu bytes", outResponse.size());
      setLastFailure(HttpDownloader::FailStage::READ,
                     static_cast<int>(std::min<size_t>(outResponse.size(), INT32_MAX)));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;
    outResponse.append(buf.get(), static_cast<size_t>(read));
    // Response is expected to be a small JSON object (see postFileMultipart's
    // doc comment) -- cap it generously so an unexpected huge body can't
    // accumulate without bound.
    if (outResponse.size() > 16384) {
      LOG_ERR("HTTP", "postFileMultipart response too large, aborting");
      setLastFailure(HttpDownloader::FailStage::INCOMPLETE, static_cast<int>(outResponse.size()));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
  }

  esp_http_client_cleanup(client);
  return HttpDownloader::OK;
}

// Small in-memory JSON body POST (never streamed -- callers of postJson pass
// a pre-serialized string expected to be at most a few hundred bytes, e.g.
// device-registration/reading-stats payloads). Mirrors runPostFile's manual
// open/write/fetch_headers/read structure but skips the multipart framing
// and the size-everything-up-front pass multipart needs, since the whole
// body is already a single in-memory std::string.
// DELETE with no request body. Same open/fetch/read shape as runPostJson below, minus
// the write phase -- which also means it needs none of the SIMULATOR httpClientWrite
// handling, so this path works in the simulator as well as on device.
HttpDownloader::DownloadError runDelete(const std::string& url, const std::string& username,
                                        const std::string& password, std::string& outResponse, int timeoutMs) {
  setLastFailure(HttpDownloader::FailStage::NONE, 0);
  outResponse.clear();
  WifiPowerSaveGuard psGuard;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer (free heap: %u bytes)", (unsigned)READ_CHUNK, ESP.getFreeHeap());
    setLastFailure(HttpDownloader::FailStage::BUFFER_OOM, static_cast<int>(ESP.getFreeHeap()));
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_DELETE;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = timeoutMs;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    setLastFailure(HttpDownloader::FailStage::CLIENT_INIT, 0);
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  setDeviceSerialHeader(client, url);
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, same convention as runGet -- don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // write_len 0: no request body.
  const esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "delete open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(err),
            getHttpClientErrno(client), ESP.getFreeHeap());
    logTlsError(client);
    setLastFailure(HttpDownloader::FailStage::OPEN, static_cast<int>(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  // Any 2xx: a delete legitimately answers 204 No Content as well as 200, and this
  // returns before reading the body, so a bare 200 check would discard a successful
  // response (same trap as the 201 from /api/device-login/start -- see runPostJson).
  // 404 is left to the caller: "already gone" and "wrong URL" are the same status.
  if (status < 200 || status > 299) {
    LOG_DBG("HTTP", "delete non-2xx status: %d", status);
    setLastFailure(HttpDownloader::FailStage::STATUS, status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "delete read error after %zu bytes", outResponse.size());
      setLastFailure(HttpDownloader::FailStage::READ,
                     static_cast<int>(std::min<size_t>(outResponse.size(), INT32_MAX)));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;
    outResponse.append(buf.get(), static_cast<size_t>(read));
    if (outResponse.size() > 16384) {
      LOG_ERR("HTTP", "delete response too large, aborting");
      setLastFailure(HttpDownloader::FailStage::INCOMPLETE, static_cast<int>(outResponse.size()));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
  }

  esp_http_client_cleanup(client);
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError runPostJson(const std::string& url, const std::string& jsonBody,
                                          const std::string& username, const std::string& password,
                                          std::string& outResponse, int timeoutMs) {
  setLastFailure(HttpDownloader::FailStage::NONE, 0);
  outResponse.clear();
  WifiPowerSaveGuard psGuard;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer (free heap: %u bytes)", (unsigned)READ_CHUNK, ESP.getFreeHeap());
    setLastFailure(HttpDownloader::FailStage::BUFFER_OOM, static_cast<int>(ESP.getFreeHeap()));
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = timeoutMs;
  // TEMP diagnostic: assigning the hook costs nothing; esp-tls parses the bundle
  // inside the handshake, so the cost shows up under TLS.handshake, not here.
  HEAP_TRACE_MARK("HTTP.bundleAttach", "BEGIN");
  config.crt_bundle_attach = esp_crt_bundle_attach;
  HEAP_TRACE_MARK("HTTP.bundleAttach", "END");
  config.keep_alive_enable = true;

  HEAP_TRACE_MARK("HTTP.clientInit", "BEGIN");
  esp_http_client_handle_t client = esp_http_client_init(&config);
  HEAP_TRACE_MARK("HTTP.clientInit", "END");
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    setLastFailure(HttpDownloader::FailStage::CLIENT_INIT, 0);
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  setDeviceSerialHeader(client, url);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, same convention as runGet -- don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // TEMP diagnostic: esp_http_client_open() is where the TCP connect, the CA
  // bundle parse and the whole TLS handshake happen. Bracketed BEFORE and AFTER
  // deliberately -- a heap reading taken after esp_http_client_cleanup() describes
  // a heap that has already been handed the evidence back, which is exactly what
  // made an out-of-memory look like a trust failure.
  HEAP_TRACE_MARK("TLS.handshake", "BEGIN");
  esp_err_t err = esp_http_client_open(client, static_cast<int>(jsonBody.size()));
  HEAP_TRACE_MARK("TLS.handshake", err == ESP_OK ? "END-OK" : "END-FAIL");
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "postJson open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(err),
            getHttpClientErrno(client), ESP.getFreeHeap());
    logTlsError(client);
    HEAP_TRACE_REPORT_FAILURE("postJson open");
    setLastFailure(HttpDownloader::FailStage::OPEN, static_cast<int>(err));
    HEAP_TRACE_MARK("HTTP.cleanup", "BEGIN");
    esp_http_client_cleanup(client);
    HEAP_TRACE_MARK("HTTP.cleanup", "END");
    return HttpDownloader::HTTP_ERROR;
  }

  size_t off = 0;
  bool writeOk = true;
  while (off < jsonBody.size()) {
    const int written = httpClientWrite(client, jsonBody.data() + off, static_cast<int>(jsonBody.size() - off));
    if (written < 0) {
      writeOk = false;
      break;
    }
    off += static_cast<size_t>(written);
  }
  if (!writeOk) {
    LOG_ERR("HTTP", "postJson write failed (errno=%d)", getHttpClientErrno(client));
    setLastFailure(HttpDownloader::FailStage::OPEN, getHttpClientErrno(client));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  // Any 2xx, not just 200: POSTing to a creation endpoint legitimately answers
  // 201 Created, and this function returns before reading the body, so treating
  // that as a failure discards a perfectly good response. /api/device-login/start
  // does exactly this -- it answers 201 with the pairing code and qr_payload, so
  // requiring a bare 200 here made QR sign-in fail every time and drop the user on
  // the error screen instead of a QR (see FouladDeviceLogin::start).
  if (status < 200 || status > 299) {
    // Not necessarily an error the caller should log loudly -- e.g. a 404 from
    // /opds/reading-stats just means "register the device first" (see
    // FouladDeviceTracking). Callers inspect getLastFailure().detail for the
    // status code rather than this function surfacing it as a return value.
    LOG_DBG("HTTP", "postJson non-2xx status: %d", status);
    setLastFailure(HttpDownloader::FailStage::STATUS, status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "postJson read error after %zu bytes", outResponse.size());
      setLastFailure(HttpDownloader::FailStage::READ,
                     static_cast<int>(std::min<size_t>(outResponse.size(), INT32_MAX)));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;
    outResponse.append(buf.get(), static_cast<size_t>(read));
    // Response is expected to be a small JSON object -- cap it generously so
    // an unexpected huge body can't accumulate without bound.
    if (outResponse.size() > 16384) {
      LOG_ERR("HTTP", "postJson response too large, aborting");
      setLastFailure(HttpDownloader::FailStage::INCOMPLETE, static_cast<int>(outResponse.size()));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
  }

  HEAP_TRACE_MARK("HTTP.cleanup", "BEGIN");
  esp_http_client_cleanup(client);
  HEAP_TRACE_MARK("HTTP.cleanup", "END");
  return HttpDownloader::OK;
}

// All HTTP(S) fetches go through wolfSSL when it is the active TLS stack: it
// speaks TLS 1.3 and reads large bodies from servers where the esp_http_client/
// mbedTLS path fails to connect or stalls mid-stream. Plain-http URLs still use a
// WiFiClient inside runGetWolf, so this is safe for non-TLS targets too.
// Downloads `url` to `destPath` in RANGE_CHUNK_BYTES pieces over one verified session.
// Every response is bounded by the range we asked for, so no single TLS record can grow
// past it. Returns OK only after the written byte count matches the server's declared
// total and the temp file has been renamed into place.
HttpDownloader::DownloadError runDownloadRangedVerified(const std::string& url, const std::string& username,
                                                        const std::string& password, const std::string& destPath,
                                                        const HttpDownloader::ProgressCallback& progress,
                                                        const bool* cancelFlag, const char* anchorsPem) {
  const std::string tmpPath = destPath + ".part";
  Storage.remove(tmpPath.c_str());

  // One chunk staged in RAM, then appended only once the slice has been validated. The
  // file therefore never holds bytes from a failed attempt, which matters because
  // HalStorage has no truncate() to roll one back.
  auto chunk = makeUniqueNoThrow<uint8_t[]>(RANGE_CHUNK_BYTES);
  if (!chunk) {
    LOG_ERR("HTTP", "ranged: OOM staging %u bytes", static_cast<unsigned>(RANGE_CHUNK_BYTES));
    return HttpDownloader::HTTP_ERROR;
  }

  // One scope for the whole file: every chunk rides the session the first one opened,
  // otherwise a 450KB book would pay ~110 handshakes.
  const HttpDownloader::KeepAliveScope rangeSession;

  size_t offset = 0;
  size_t total = 0;
  bool haveTotal = false;
  uint32_t minMaxAlloc = ESP.getMaxAllocHeap();

  while (!haveTotal || offset < total) {
    if (cancelFlag && *cancelFlag) {
      Storage.remove(tmpPath.c_str());
      return HttpDownloader::ABORTED;
    }

    char rangeHeader[64];
    snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%u-%u", static_cast<unsigned>(offset),
             static_cast<unsigned>(offset + RANGE_CHUNK_BYTES - 1));

    size_t staged = 0;
    size_t cStart = 0, cEnd = 0, cTotal = 0;
    HttpDownloader::DownloadError r = HttpDownloader::HTTP_ERROR;

    for (int attempt = 0; attempt <= RANGE_MAX_CHUNK_RETRIES; ++attempt) {
      staged = 0;
      Sink sink;
      sink.write = [&chunk, &staged](const uint8_t* data, const size_t len) {
        if (staged + len > RANGE_CHUNK_BYTES) return false;  // more than we asked for
        memcpy(chunk.get() + staged, data, len);
        staged += len;
        return true;
      };
      r = runGetVerified(url, username, password, sink, anchorsPem, rangeHeader, &cStart, &cEnd, &cTotal);
      if (r == HttpDownloader::OK) break;
    }

    if (r != HttpDownloader::OK) {
      LOG_ERR("HTTP", "ranged: chunk at %u failed after retries", static_cast<unsigned>(offset));
      Storage.remove(tmpPath.c_str());
      return r;
    }
    if (cStart != offset || staged != cEnd - cStart + 1) {
      LOG_ERR("HTTP", "ranged: wrong slice (want %u, got %u..%u, %u bytes)", static_cast<unsigned>(offset),
              static_cast<unsigned>(cStart), static_cast<unsigned>(cEnd), static_cast<unsigned>(staged));
      Storage.remove(tmpPath.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    if (haveTotal && cTotal != total) {
      LOG_ERR("HTTP", "ranged: total changed mid-download");
      Storage.remove(tmpPath.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    total = cTotal;
    haveTotal = true;

    HalFile out = Storage.open(tmpPath.c_str(), O_WRITE | O_CREAT | O_APPEND);
    if (!out || out.write(chunk.get(), staged) != staged) {
      LOG_ERR("HTTP", "ranged: temp write failed at %u", static_cast<unsigned>(offset));
      Storage.remove(tmpPath.c_str());
      return HttpDownloader::FILE_ERROR;
    }
    out.close();

    offset += staged;
    const uint32_t nowMaxAlloc = ESP.getMaxAllocHeap();
    if (nowMaxAlloc < minMaxAlloc) minMaxAlloc = nowMaxAlloc;
    if (progress) progress(offset, total);
  }

  // Only now does the destination exist. Anything short never gets the name.
  Storage.remove(destPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    LOG_ERR("HTTP", "ranged: rename failed");
    Storage.remove(tmpPath.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  LOG_INF("HTTP", "ranged ok: %u bytes / %u chunks, minMaxAllocDuring=%u", static_cast<unsigned>(total),
          static_cast<unsigned>((total + RANGE_CHUNK_BYTES - 1) / RANGE_CHUNK_BYTES),
          static_cast<unsigned>(minMaxAlloc));
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink) {
#if defined(FREEINK_NET_WOLFSSL)
  // Every Midad Core request -- feed, cover, download -- runs verified, anchored
  // at GTS Root R4. Host-gated, not path-gated: covers live outside /opds
  // (/library/items/:id/cover, /store/books/:id/cover) and must not fall through
  // to the unverified path because of their path prefix.
  if (isFouladEbooksUrl(url)) {
    return runGetVerified(url, username, password, sink, core_ca::kMidadCoreCaGtsRootR4);
  }
  return runGetWolf(url, username, password, sink);
#else
  return runGet(url, username, password, sink);
#endif
}
}  // namespace

HttpDownloader::LastFailure HttpDownloader::getLastFailure() { return gLastFailure; }

HttpDownloader::KeepAliveScope::KeepAliveScope() {
#if defined(FREEINK_NET_WOLFSSL)
  if (gKeepAliveDepth++ > 0) return;  // an inner scope rides the outer one's client
  gKeepAliveVerifiedOrigin.clear();
  gKeepAliveClient = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!gKeepAliveClient) {
    // Not an error the caller has to handle: with no shared client every request
    // falls back to building its own, i.e. connection-per-request as before.
    LOG_ERR("HTTP", "keep-alive client alloc failed, falling back (free heap: %u bytes)",
            static_cast<unsigned>(ESP.getFreeHeap()));
  }
#endif
}

HttpDownloader::KeepAliveScope::~KeepAliveScope() {
#if defined(FREEINK_NET_WOLFSSL)
  if (--gKeepAliveDepth > 0) return;
  gKeepAliveClient.reset();  // closes the socket and frees the TLS session
  gKeepAliveVerified.reset();
  gKeepAliveVerifiedOrigin.clear();
#endif
}

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrlPinned(const std::string& url, const DataCallback& onData, const char* anchorsPem) {
#if defined(FREEINK_NET_WOLFSSL)
  Sink sink;
  sink.write = onData;
  // runGetVerified refuses a non-https URL and every non-https redirect target
  // itself, and never forwards credentials once the chain leaves its start origin.
  return runGetVerified(url, "", "", sink, anchorsPem) == OK;
#else
  (void)url;
  (void)onData;
  (void)anchorsPem;
  LOG_ERR("HTTP", "pinned fetch needs the wolfSSL stack");
  return false;
#endif
}

bool HttpDownloader::fetchUrlVerified(const std::string& url, const DataCallback& onData, const std::string& username,
                                      const std::string& password) {
  // "Verified" means TLS too, not just certificate-chain checking once connected --
  // reject plaintext up front rather than relying on every caller to only ever pass
  // an https:// URL. esp_http_client_set_redirection() already refuses an
  // HTTPS-to-HTTP downgrade mid-request, so this closes the one gap it doesn't
  // cover: a caller-supplied URL that starts as http:// in the first place.
  // Case-insensitive: esp_http_client itself parses the scheme that way.
  constexpr char kHttpsScheme[] = "https://";
  if (url.size() < sizeof(kHttpsScheme) - 1 || strncasecmp(url.c_str(), kHttpsScheme, sizeof(kHttpsScheme) - 1) != 0) {
    LOG_ERR("HTTP", "Refusing unverified fetch of non-HTTPS URL");
    setLastFailure(HttpDownloader::FailStage::OPEN, 0);
    return false;
  }
  LOG_DBG("HTTP", "Fetching (verified): %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, ConditionalGet& conditional,
                              const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Fetching (conditional): %s", url.c_str());
  conditional.notModified = false;
  conditional.etag.clear();
  Sink sink;
  sink.write = onData;
  return runGet(url, username, password, sink, &conditional) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const bool* cancelFlag,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

#if defined(FREEINK_NET_WOLFSSL)
  // Midad Core downloads go out in bounded ranges. A whole-file GET died at 49% with
  // MEMORY_E because the transfer itself erodes the contiguous block (MaxAlloc 10,740 at
  // the start, 3,444 at its lowest during), and wolfSSL sizes its receive buffer to
  // whatever record arrives -- so the response is capped at the request instead.
  // Third-party hosts keep the streaming path: they are not guaranteed to honour Range,
  // and a 200 there is normal rather than a failure.
  // Book acquisitions only. Core's cover route serves the object body with a plain 200
  // and implements no Range at all, so sending one there makes every cover fail the
  // mandatory-206 check -- measured as 18 refusals and 6 lost covers. Covers are ~4KB
  // and were never the problem: a single response that size cannot produce the oversized
  // record this exists to prevent.
  const bool isCoverUrl = url.find("/cover") != std::string::npos;
  if (isFouladEbooksUrl(url) && !isCoverUrl) {
    return runDownloadRangedVerified(url, username, password, destPath, progress, cancelFlag,
                                     core_ca::kMidadCoreCaGtsRootR4);
  }
#endif

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGetSecure(url, username, password, sink);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.close();

  if (result != OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}

bool HttpDownloader::postFileMultipart(const std::string& url, const std::string& filePath,
                                       const std::vector<std::pair<std::string, std::string>>& fields,
                                       std::string& outResponse, int timeoutMs, const std::string& username,
                                       const std::string& password) {
  return postFilesMultipart(url, {{"font", filePath}}, fields, outResponse, timeoutMs, username, password);
}

bool HttpDownloader::postFilesMultipart(const std::string& url,
                                        const std::vector<std::pair<std::string, std::string>>& files,
                                        const std::vector<std::pair<std::string, std::string>>& fields,
                                        std::string& outResponse, int timeoutMs, const std::string& username,
                                        const std::string& password) {
  for (const auto& f : files) {
    LOG_DBG("HTTP", "Posting %s: %s -> %s", f.first.c_str(), f.second.c_str(), url.c_str());
  }
  return runPostFile(url, files, fields, outResponse, timeoutMs, username, password) == OK;
}

HttpDownloader::DownloadError HttpDownloader::postJson(const std::string& url, const std::string& jsonBody,
                                                       const std::string& username, const std::string& password,
                                                       std::string& outResponse, int timeoutMs) {
  LOG_DBG("HTTP", "Posting JSON: %s", url.c_str());
  return runPostJson(url, jsonBody, username, password, outResponse, timeoutMs);
}

HttpDownloader::DownloadError HttpDownloader::postJsonWolf(const std::string& url, const std::string& jsonBody,
                                                           const char* caAnchorsPem, std::string& outResponse,
                                                           int timeoutMs, const std::string& username,
                                                           const std::string& password, const char* contentType) {
#if defined(FREEINK_NET_WOLFSSL)
  LOG_DBG("HTTP", "Posting (wolfSSL, pinned anchors): %s", url.c_str());
  return runPostJsonWolf(url, jsonBody, caAnchorsPem, outResponse, timeoutMs, username, password, contentType, "POST");
#else
  // Fail closed. Falling back to the bundle path would silently answer a
  // request for these anchors with a different trust decision.
  (void)jsonBody;
  (void)caAnchorsPem;
  (void)timeoutMs;
  (void)username;
  (void)password;
  (void)contentType;
  outResponse.clear();
  LOG_ERR("HTTP", "wolfSSL transport not built in; refusing pinned POST: %s", url.c_str());
  return HTTP_ERROR;
#endif
}

HttpDownloader::DownloadError HttpDownloader::putJsonWolf(const std::string& url, const std::string& jsonBody,
                                                          const char* caAnchorsPem, std::string& outResponse,
                                                          int timeoutMs, const std::string& username,
                                                          const std::string& password, const char* contentType) {
#if defined(FREEINK_NET_WOLFSSL)
  LOG_DBG("HTTP", "PUT (wolfSSL, pinned anchors): %s", url.c_str());
  return runPostJsonWolf(url, jsonBody, caAnchorsPem, outResponse, timeoutMs, username, password, contentType, "PUT");
#else
  // Fail closed. Falling back to the bundle path would silently answer a
  // request for these anchors with a different trust decision.
  (void)jsonBody;
  (void)caAnchorsPem;
  (void)timeoutMs;
  (void)username;
  (void)password;
  (void)contentType;
  outResponse.clear();
  LOG_ERR("HTTP", "wolfSSL transport not built in; refusing pinned POST: %s", url.c_str());
  return HTTP_ERROR;
#endif
}

HttpDownloader::DownloadError HttpDownloader::deleteRequest(const std::string& url, const std::string& username,
                                                            const std::string& password, std::string& outResponse,
                                                            int timeoutMs) {
  LOG_DBG("HTTP", "DELETE: %s", url.c_str());
  return runDelete(url, username, password, outResponse, timeoutMs);
}
