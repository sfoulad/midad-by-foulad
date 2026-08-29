#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  // Which stage of the last failed request produced its HTTP_ERROR/FILE_ERROR,
  // plus a stage-specific detail code -- lets a caller-facing diagnostic (e.g. the
  // OTA "Update Failed" screen) show what actually went wrong (DNS/connect/TLS vs.
  // a bad HTTP status vs. a mid-transfer read failure) without a USB serial
  // capture, since DownloadError alone collapses all of these into one bucket.
  enum class FailStage : uint8_t {
    NONE = 0,
    BUFFER_OOM,   // couldn't allocate the read buffer; detail = free heap
    CLIENT_INIT,  // esp_http_client_init returned null
    OPEN,         // esp_http_client_open failed; detail = esp_err_t
    REDIRECT,     // esp_http_client_open failed while following a redirect; detail = esp_err_t
    STATUS,       // non-200 final status; detail = HTTP status code
    READ,         // esp_http_client_read returned < 0; detail = bytes read so far (clamped)
    INCOMPLETE,   // stream ended before is_complete_data_received(); detail = bytes received (clamped)
  };
  struct LastFailure {
    FailStage stage = FailStage::NONE;
    int detail = 0;
  };
  // Reflects the most recent runGet() call across ANY fetchUrl/downloadToFile
  // caller (single network task, no concurrent requests), valid to read
  // immediately after that call returned an error.
  static LastFailure getLastFailure();

  // Conditional-GET state, for a caller that re-fetches the same URL repeatedly and
  // usually gets the same bytes back. Pass the ETag stored from last time in
  // ifNoneMatch; if the resource is unchanged the server answers 304 and sends no
  // body, so onData is never called and notModified comes back true -- the caller
  // must then serve its own cached copy of whatever it parsed before.
  //
  // The point is not bandwidth. GitHub's REST API allows 60 requests per hour per
  // IP unauthenticated, and documents 304 responses as NOT counting against that
  // budget, so this is what keeps a device (and everything else behind the same
  // public IP) from being rate-limited into a 403 by ordinary update checks --
  // see OtaUpdater::checkForUpdate.
  struct ConditionalGet {
    std::string ifNoneMatch;   // in: ETag from the previous successful fetch, or empty to skip
    std::string etag;          // out: ETag of this response, empty if the server sent none
    bool notModified = false;  // out: server answered 304, no body was delivered
  };

  // Conditional GETs exist for the OTA release check alone, and that check must
  // never run over an unverified connection, so the conditional entry point IS
  // the verified one -- see fetchUrlVerified below for what caPem means. There
  // is deliberately no unverified conditional overload to fall back to.
  static bool fetchUrlVerified(const std::string& url, const DataCallback& onData, ConditionalGet& conditional,
                               const char* caPem);

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Same as fetchUrl(DataCallback), but with the TLS peer actually verified --
   * for fetches where an unverified connection is not an acceptable risk even
   * though ordinary catalog/book traffic currently runs unverified over
   * wolfSSL (see HttpDownloader.cpp's runGetWolf). The OTA flow is the primary
   * caller: with firmware signing retired, the TLS layer is the only thing
   * standing between the flasher and an attacker-substituted image.
   *
   * caPem names the trust anchors (concatenated PEM roots, e.g.
   * ota_ca::kGithubOtaCaAnchors) and is REQUIRED on wolfSSL builds: wolfSSL
   * carries no default CA bundle, so a null/empty caPem is refused rather than
   * silently downgraded to setInsecure(). The chain is verified against caPem
   * and the hostname against the leaf certificate (SecureClient's
   * check_domain_name); http:// URLs are refused up front and an https->http
   * redirect is refused mid-flight. On non-wolfSSL builds (simulator) the
   * fetch runs over esp_http_client/mbedTLS verified against
   * esp_crt_bundle_attach, and caPem is unused.
   */
  static bool fetchUrlVerified(const std::string& url, const DataCallback& onData, const char* caPem,
                               const std::string& username = "", const std::string& password = "");

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");

  /**
   * POST a file (streamed from SD, never buffered whole into RAM) as one
   * multipart/form-data part named "font", alongside plain string fields, to
   * url. Response body (expected to be small JSON) is accumulated into
   * outResponse. Used for the font-conversion relay to foulad-ebooks, whose
   * response can legitimately take much longer than a normal fetch -- pass a
   * generous timeoutMs (this does NOT reuse the GET path's tuned-for-downloads
   * timeout constant). username/password add preemptive Basic Auth (empty by
   * default, matching the font relay's no-auth convention). Returns false on
   * any network/file error; check getLastFailure() for detail.
   */
  static bool postFileMultipart(const std::string& url, const std::string& filePath,
                                const std::vector<std::pair<std::string, std::string>>& fields,
                                std::string& outResponse, int timeoutMs = 120000, const std::string& username = "",
                                const std::string& password = "");

  /**
   * Multi-file variant of postFileMultipart: each (fieldName, sdPath) pair
   * becomes its own streamed multipart file part. Used by the Convert Font
   * relay to send optional bold/italic/bolditalic style fonts alongside the
   * required regular one ("font", "font_bold", "font_italic",
   * "font_bolditalic" -- matching the foulad-ebooks endpoint), and by
   * FouladDeviceTracking to stream the on-SD debug log with Basic Auth.
   */
  static bool postFilesMultipart(const std::string& url, const std::vector<std::pair<std::string, std::string>>& files,
                                 const std::vector<std::pair<std::string, std::string>>& fields,
                                 std::string& outResponse, int timeoutMs = 120000, const std::string& username = "",
                                 const std::string& password = "");

  /**
   * POST a small pre-serialized JSON body (Content-Type: application/json)
   * with optional Basic Auth, buffering the (expected-small) JSON response
   * into outResponse. Unlike postFilesMultipart, returns the DownloadError
   * enum directly (not collapsed to bool): callers that need to distinguish
   * e.g. a 404 from other failures should check getLastFailure() after a
   * non-OK result -- FailStage::STATUS's detail is the raw HTTP status code.
   */
  static DownloadError postJson(const std::string& url, const std::string& jsonBody, const std::string& username,
                                const std::string& password, std::string& outResponse, int timeoutMs = 15000);

  /**
   * HTTP DELETE with optional Basic Auth, buffering any (expected-small) response
   * body into outResponse.
   *
   * Deliberately does not interpret the status beyond "2xx succeeded": a 404 means
   * different things to different callers (already gone and therefore fine, versus a
   * genuinely wrong URL), so that judgement stays with the caller. Same convention as
   * postJson -- check getLastFailure() after a non-OK result, where FailStage::STATUS
   * carries the raw HTTP status code.
   */
  static DownloadError deleteRequest(const std::string& url, const std::string& username, const std::string& password,
                                     std::string& outResponse, int timeoutMs = 15000);
};
