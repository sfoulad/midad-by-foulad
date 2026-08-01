#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <strings.h>  // strcasecmp, for the case-insensitive response-header match

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "MidadRootCa.h"

namespace {
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 2048;

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

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

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
  // One pinned root for Midad, the full bundle for everything else. The bundle costs
  // ~250KB of contiguous allocation during the handshake -- the reason every Midad
  // endpoint was on plain http, with account credentials in clear on every request.
  // ISRG Root X1 alone verifies midad.one's cross-signed chain at ~1.9KB, which a
  // device observed at 18.7KB free can actually afford.
  //
  // GitHub is on a Sectigo chain this root cannot verify, so OTA and font downloads
  // keep the bundle. Selection is by host, matching the same helper that gates the
  // device-serial header.
  if (isFouladEbooksUrl(url)) {
    config.cert_pem = MIDAD_ROOT_CA_PEM;
  } else {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }
  config.keep_alive_enable = true;
  if (conditional) {
    config.event_handler = captureResponseEtag;
    config.user_data = conditional;
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
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
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    // ESP_ERR_HTTP_CONNECT alone doesn't say whether this was heap pressure, DNS
    // failure, TCP-level rejection/timeout, or a TLS handshake problem, so also
    // surface the underlying socket errno -- ETIMEDOUT/ECONNREFUSED/EHOSTUNREACH/etc.
    // point at the network path itself; an mbedTLS-range negative value points at the
    // TLS handshake instead (see logTlsError).
    LOG_ERR("HTTP", "open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(err), getHttpClientErrno(client),
            ESP.getFreeHeap());
    logTlsError(client);
    setLastFailure(HttpDownloader::FailStage::OPEN, static_cast<int>(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < 5; ++hop) {
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
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(err),
              getHttpClientErrno(client), ESP.getFreeHeap());
      logTlsError(client);
      setLastFailure(HttpDownloader::FailStage::REDIRECT, static_cast<int>(err));
      esp_http_client_cleanup(client);
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
  // One pinned root for Midad, the full bundle for everything else. The bundle costs
  // ~250KB of contiguous allocation during the handshake -- the reason every Midad
  // endpoint was on plain http, with account credentials in clear on every request.
  // ISRG Root X1 alone verifies midad.one's cross-signed chain at ~1.9KB, which a
  // device observed at 18.7KB free can actually afford.
  //
  // GitHub is on a Sectigo chain this root cannot verify, so OTA and font downloads
  // keep the bundle. Selection is by host, matching the same helper that gates the
  // device-serial header.
  if (isFouladEbooksUrl(url)) {
    config.cert_pem = MIDAD_ROOT_CA_PEM;
  } else {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
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
  // One pinned root for Midad, the full bundle for everything else. The bundle costs
  // ~250KB of contiguous allocation during the handshake -- the reason every Midad
  // endpoint was on plain http, with account credentials in clear on every request.
  // ISRG Root X1 alone verifies midad.one's cross-signed chain at ~1.9KB, which a
  // device observed at 18.7KB free can actually afford.
  //
  // GitHub is on a Sectigo chain this root cannot verify, so OTA and font downloads
  // keep the bundle. Selection is by host, matching the same helper that gates the
  // device-serial header.
  if (isFouladEbooksUrl(url)) {
    config.cert_pem = MIDAD_ROOT_CA_PEM;
  } else {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }
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
  // One pinned root for Midad, the full bundle for everything else. The bundle costs
  // ~250KB of contiguous allocation during the handshake -- the reason every Midad
  // endpoint was on plain http, with account credentials in clear on every request.
  // ISRG Root X1 alone verifies midad.one's cross-signed chain at ~1.9KB, which a
  // device observed at 18.7KB free can actually afford.
  //
  // GitHub is on a Sectigo chain this root cannot verify, so OTA and font downloads
  // keep the bundle. Selection is by host, matching the same helper that gates the
  // device-serial header.
  if (isFouladEbooksUrl(url)) {
    config.cert_pem = MIDAD_ROOT_CA_PEM;
  } else {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
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

  esp_err_t err = esp_http_client_open(client, static_cast<int>(jsonBody.size()));
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "postJson open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(err),
            getHttpClientErrno(client), ESP.getFreeHeap());
    logTlsError(client);
    setLastFailure(HttpDownloader::FailStage::OPEN, static_cast<int>(err));
    esp_http_client_cleanup(client);
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

  esp_http_client_cleanup(client);
  return HttpDownloader::OK;
}
}  // namespace

HttpDownloader::LastFailure HttpDownloader::getLastFailure() { return gLastFailure; }

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGet(url, username, password, sink) == OK;
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
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
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
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

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

  const DownloadError result = runGet(url, username, password, sink);
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

HttpDownloader::DownloadError HttpDownloader::deleteRequest(const std::string& url, const std::string& username,
                                                            const std::string& password, std::string& outResponse,
                                                            int timeoutMs) {
  LOG_DBG("HTTP", "DELETE: %s", url.c_str());
  return runDelete(url, username, password, outResponse, timeoutMs);
}
