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
