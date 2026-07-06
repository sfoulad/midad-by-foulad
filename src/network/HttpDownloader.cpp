#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <TlsErrorClassifier.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_wifi.h>

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
void logTlsError(esp_http_client_handle_t, int* outTlsErrorCode) {
  if (outTlsErrorCode) *outTlsErrorCode = 0;
}
#else
int getHttpClientErrno(esp_http_client_handle_t client) { return esp_http_client_get_errno(client); }
// A bare ESP_ERR_HTTP_CONNECT + errno=0 means the raw socket connect succeeded (or
// was never attempted) and something above it -- the TLS handshake or certificate
// verification -- is what actually failed; the plain socket errno can't distinguish
// that from DNS/TCP-level trouble. esp_tls_error_code is the underlying mbedtls
// error (a handshake failure); esp_tls_flags is mbedtls' cert verify bitmask
// (expired/untrusted/hostname-mismatch/etc. -- see mbedtls x509.h X509_BADCERT_*).
// outTlsErrorCode (if non-null) receives the raw code so the caller can decide whether
// this looks like a chain-resolution problem worth retrying against the fallback root.
void logTlsError(esp_http_client_handle_t client, int* outTlsErrorCode) {
  int tlsErrorCode = 0;
  int tlsFlags = 0;
  esp_http_client_get_and_clear_last_tls_error(client, &tlsErrorCode, &tlsFlags);
  if (tlsErrorCode != 0 || tlsFlags != 0) {
    // esp_http_client reports this as the positive magnitude of the mbedTLS code
    // (e.g. 0x3000 for MBEDTLS_ERR_X509_FATAL_ERROR, which mbedtls's own headers define
    // as -0x3000) -- print it as mbedTLS's own negative convention for readability, but
    // do NOT rely on the sign when comparing (see isCertChainFailure's abs() comparison).
    LOG_ERR("HTTP", "tls detail: esp_tls_error_code=-0x%X esp_tls_flags=0x%X", tlsErrorCode, tlsFlags);
  }
  if (outTlsErrorCode) *outTlsErrorCode = tlsErrorCode;
}
#endif

// Let's Encrypt's certificate hierarchy introduced in Sept 2025 for some of their issuance
// (confirmed live on foulad.one via `openssl s_client -showcerts`: leaf -> "YE2" -> this
// "ISRG Root YE" -> cross-signed back to the classic ISRG Root X2/X1). Browsers and OpenSSL
// do path-building across multiple candidate parents and resolve this fine; mbedTLS does a
// simple linear chain walk and can't, failing with MBEDTLS_ERR_X509_FATAL_ERROR (-0x3000)
// even though the chain is entirely legitimate. This root postdates ESP-IDF's embedded
// crt_bundle snapshot, so esp_crt_bundle_attach doesn't have it either. Trusting it directly
// short-circuits the ambiguity: the leaf's issuer (YE2) is itself issued by this cert, so
// mbedTLS resolves the chain in one hop without ever needing the cross-sign detour.
// Confirmed against a real device failure log (esp_tls_error_code=-0x3000, esp_tls_flags=0x0
// -- no cert-verify flags set, consistent with "couldn't build a path" rather than "untrusted
// cert"). Background: https://community.letsencrypt.org/t/chain-validation-issues-with-ye-yr-under-linux-distributions/247836
constexpr char kIsrgRootYePem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICpjCCAiugAwIBAgIRAIchZfw0tuX7qK3Vs3BftTowCgYIKoZIzj0EAwMwTzEL\n"
    "MAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2VhcmNo\n"
    "IEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDIwHhcNMjYwNTEzMDAwMDAwWhcN\n"
    "MzIwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsGA1UEChMESVNSRzEQMA4G\n"
    "A1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAiA2IABDwS/6vhrcVqcbBo\n"
    "+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7XyFjWsPYhIPt/wzOqxTd2\n"
    "b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW87j77GaOB6zCB6DAOBgNV\n"
    "HQ8BAf8EBAMCAQYwEwYDVR0lBAwwCgYIKwYBBQUHAwEwDwYDVR0TAQH/BAUwAwEB\n"
    "/zAdBgNVHQ4EFgQUo8gmWo6hTNA1Y/ybI8g6rlbzT1YwHwYDVR0jBBgwFoAUfEKW\n"
    "rt5LSDv6kviejM9ti6lyN5UwMgYIKwYBBQUHAQEEJjAkMCIGCCsGAQUFBzAChhZo\n"
    "dHRwOi8veDIuaS5sZW5jci5vcmcvMBMGA1UdIAQMMAowCAYGZ4EMAQIBMCcGA1Ud\n"
    "HwQgMB4wHKAaoBiGFmh0dHA6Ly94Mi5jLmxlbmNyLm9yZy8wCgYIKoZIzj0EAwMD\n"
    "aQAwZgIxAMU19WCtmxVND8UHBZRoma49Z7jPs64Dma0eTu1OChVbB/2J7GV3nvYK\n"
    "Ax54uk1G9QIxAO0miLVJu8PLNiXXXkiE/gsK3CTRTF/aeo4bMX42Zw40csRU6AC2\n"
    "6hSW1/IWaas6dg==\n"
    "-----END CERTIFICATE-----\n";

// isCertChainFailure() lives in lib/TlsErrorClassifier (dependency-free, host-testable --
// see TlsErrorClassifierTest.cpp for the regression test that would have caught the v1.6.7
// sign-comparison bug where the fallback-root retry below silently never triggered).

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
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  WifiPowerSaveGuard psGuard;

  // Reserve the read buffer before opening any connection, not after. An active TLS
  // session holds its own sizable internal buffers (mbedTLS's handshake/record buffers)
  // for as long as the connection stays open; confirmed on a real device that even with
  // ~59KB reported free right before connecting, a successful TLS handshake (via the
  // fallback-root retry below) left too little *contiguous* heap for this one small
  // allocation afterward, failing with "OOM: 2048 byte read buffer" despite the
  // connection itself having worked. Grabbing this buffer first -- before the
  // handshake's own larger allocations can fragment the heap -- avoids losing the race
  // for a small block after a big one has already carved up the free space.
  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer (free heap: %u bytes)", (unsigned)READ_CHUNK, ESP.getFreeHeap());
    return HttpDownloader::HTTP_ERROR;
  }

  // Try the default crt_bundle first (covers the vast majority of servers, including
  // any user-configured OPDS server or GitHub for OTA); only on a chain-resolution
  // failure (see isCertChainFailure) retry once against the fallback root above,
  // which is scoped to exactly the one CA the default bundle is missing.
  esp_http_client_handle_t client = nullptr;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool useFallbackRoot = attempt == 1;
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
    if (useFallbackRoot) {
      config.cert_pem = kIsrgRootYePem;
    } else {
      config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    config.keep_alive_enable = true;

    client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "client init failed");
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
    const esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) break;  // connected -- fall through below with this client

    // ESP_ERR_HTTP_CONNECT alone doesn't say whether this was heap pressure, DNS
    // failure, TCP-level rejection/timeout, or a TLS handshake problem. Free heap
    // ruled out (confirmed a healthy ~59KB free on a real device that still hit this),
    // so also surface the underlying socket errno -- ETIMEDOUT/ECONNREFUSED/
    // EHOSTUNREACH/etc. point at the network path itself; an mbedTLS-range negative
    // value points at the TLS handshake instead.
    LOG_ERR("HTTP", "open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(err), getHttpClientErrno(client),
            ESP.getFreeHeap());
    int tlsErrorCode = 0;
    logTlsError(client, &tlsErrorCode);
    esp_http_client_cleanup(client);
    client = nullptr;

    if (!useFallbackRoot && isCertChainFailure(tlsErrorCode)) {
      LOG_INF("HTTP", "cert chain unresolved against default bundle -- retrying with fallback root");
      continue;
    }
    return HttpDownloader::HTTP_ERROR;
  }

  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < 5; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    const esp_err_t redirectErr = esp_http_client_open(client, 0);
    if (redirectErr != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s (errno=%d, free heap: %u bytes)", esp_err_to_name(redirectErr),
              getHttpClientErrno(client), ESP.getFreeHeap());
      logTlsError(client, nullptr);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
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
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}
}  // namespace

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
