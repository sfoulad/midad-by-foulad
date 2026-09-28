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

  // Pre-flight floor for starting a TLS transfer. Below this the session or
  // its ~17KB record buffer fails mid-stream (wolfSSL MEMORY_E) -- or an
  // interior allocation abort()s the device. Callers should check before
  // downloadToFile() and fail into their error UI instead.
  static constexpr uint32_t MIN_TLS_FREE_HEAP = 40000;
  static constexpr uint32_t MIN_TLS_MAX_ALLOC = 20000;

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

  /**
   * Keeps ONE connection -- and on https, ONE negotiated TLS session -- open
   * across a burst of requests, instead of handshaking per request.
   *
   * Construct one around a run of fetches and every fetchUrl/downloadToFile made
   * while it is alive shares the same client; the connection is dropped when the
   * outermost scope goes out of scope. Nothing else changes: a request to a
   * different scheme://host:port transparently reconnects, so a mixed burst is
   * still correct, it just saves nothing.
   *
   * What it saves. A wolfSSL handshake is the largest transient allocation on
   * this device's network path -- certificate chain buffers, a DecodedCert, and
   * the ECC verify's bignums, allocated and freed in a burst -- and on a heap a
   * long catalogue walk has already fragmented it is the step that fails. A grid
   * page of OPDS covers is six back-to-back GETs to one origin, so without this
   * it pays that six times over to fetch six thumbnails.
   *
   * Why a scope and not always-on: an idle session still holds a few KB, which a
   * caller doing heavy work BETWEEN requests (an image decode, a page layout)
   * would rather have back. Callers that fetch back-to-back opt in; everything
   * else keeps connection-per-request.
   *
   * Not thread-safe, by the same assumption getLastFailure() already makes: one
   * network task, no concurrent requests. Nesting is safe -- only the outermost
   * scope opens and closes the shared client. On a build without wolfSSL this is
   * an empty object; the esp_http_client path is unaffected.
   */
  class KeepAliveScope {
   public:
    KeepAliveScope();
    ~KeepAliveScope();
    KeepAliveScope(const KeepAliveScope&) = delete;
    KeepAliveScope& operator=(const KeepAliveScope&) = delete;
  };

  static bool fetchUrl(const std::string& url, const DataCallback& onData, ConditionalGet& conditional,
                       const std::string& username = "", const std::string& password = "");

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
   * Same as fetchUrl(DataCallback), but always over the verified esp_http_client/
   * mbedTLS path (TLS certificate chain checked against esp_crt_bundle_attach),
   * regardless of FREEINK_NET_WOLFSSL. For fetches where an unverified connection
   * is not an acceptable risk even though other traffic on this device currently
   * runs unverified over wolfSSL (see HttpDownloader.cpp's runGetWolf) -- notably
   * OTA firmware downloads, which don't need wolfSSL's TLS 1.3 support since
   * GitHub's release CDN doesn't require it.
   */
  /**
   * Verified GET against an explicit PEM trust anchor set: chain built to one of
   * those anchors and the certificate must name the host. https:// only. Used for
   * release metadata, where the response decides which firmware gets installed.
   */
  static bool fetchUrlPinned(const std::string& url, const DataCallback& onData, const char* anchorsPem);

  static bool fetchUrlVerified(const std::string& url, const DataCallback& onData, const std::string& username = "",
                               const std::string& password = "");

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const bool* cancelFlag = nullptr,
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
   * POST a small pre-serialized JSON body over the wolfSSL transport, verified
   * against caAnchorsPem -- one or more concatenated PEM certificates supplied
   * by the caller -- instead of ESP-IDF's certificate bundle. Same return
   * convention as postJson (any 2xx succeeds; getLastFailure()'s STATUS detail
   * carries the raw status code).
   *
   * What it buys over postJson, and why a caller would choose it: mbedTLS
   * anchors whatever the server sent at the TOP of the presented chain, so a
   * chain whose last link is a cross-certificate signed by an RSA root has to
   * be verified with an RSA modular exponentiation -- an allocation that fails
   * outright at the free heap this device has left mid-session. Handing wolfSSL
   * a trust store that terminates one link earlier lets the same chain verify
   * on pure ECDSA. Verification is stricter here, not weaker: the chain must
   * reach one of these anchors AND the certificate must name the URL's host
   * (see VerifiedTlsSession), with no bundle fallback and no plaintext path --
   * a non-https URL, an empty anchor set, or a build without wolfSSL is
   * refused rather than downgraded.
   *
   * Deliberately not the default for every POST: the anchor set is the
   * caller's to choose and get right, which only makes sense for an endpoint
   * whose operator this firmware knows.
   */
  static DownloadError postJsonWolf(const std::string& url, const std::string& jsonBody, const char* caAnchorsPem,
                                    std::string& outResponse, int timeoutMs = 15000, const std::string& username = {},
                                    const std::string& password = {}, const char* contentType = "application/json");

  /**
   * PUT with the same verified, pinned-anchor wolfSSL path as postJsonWolf.
   * Core's resume-position route is a PUT, and it carries a device credential --
   * so it must not fall back to the broad system CA bundle the plain postJson
   * path uses.
   */
  static DownloadError putJsonWolf(const std::string& url, const std::string& jsonBody, const char* caAnchorsPem,
                                   std::string& outResponse, int timeoutMs = 15000, const std::string& username = {},
                                   const std::string& password = {}, const char* contentType = "application/json");

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
