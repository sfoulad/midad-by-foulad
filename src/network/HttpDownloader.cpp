#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>

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

// The simulator's esp_http_client.h stub (crosspoint-simulator, an external repo)
// doesn't implement esp_http_client_get_errno -- only real ESP-IDF has it.
#ifdef SIMULATOR
int getHttpClientErrno(esp_http_client_handle_t) { return 0; }
void logTlsError(esp_http_client_handle_t) {}
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
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
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
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    setLastFailure(HttpDownloader::FailStage::CLIENT_INIT, 0);
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
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
