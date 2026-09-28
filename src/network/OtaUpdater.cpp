#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include "GithubCaCerts.h"
#include "HttpUrlParts.h"
#include <mbedtls/sha256.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

namespace {
// Host only -- never the path or query. A release asset URL is a signed, time-limited
// CDN link, so logging it whole would put a credential in the serial log and on the SD
// card. The host is what identifies the failing stage (api.github.com vs the CDN) and
// carries nothing secret.
std::string sanitizedHost(const std::string& url) {
  HttpUrlParts parts;
  if (!parseHttpUrl(url, parts) || parts.host.empty()) return "unparseable";
  return parts.host;
}

// Turns HttpDownloader's last failure into "stage/detail" for one log line. The detail
// is an HTTP status when the stage is STATUS, and an errno/internal code otherwise.
// Accepts exactly "sha256:" + 64 lowercase-or-uppercase hex, nothing else. Anything
// shorter, longer, differently prefixed or non-hex is refused rather than repaired:
// an unparseable digest must fail closed, not fall through to an install.
bool parseSha256Digest(const std::string& digest, uint8_t out[32]) {
  constexpr char kPrefix[] = "sha256:";
  constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
  if (digest.size() != kPrefixLen + 64) return false;
  if (strncasecmp(digest.c_str(), kPrefix, kPrefixLen) != 0) return false;
  for (size_t i = 0; i < 32; ++i) {
    const char hi = digest[kPrefixLen + i * 2];
    const char lo = digest[kPrefixLen + i * 2 + 1];
    const auto nib = [](const char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int h = nib(hi), l = nib(lo);
    if (h < 0 || l < 0) return false;
    out[i] = static_cast<uint8_t>((h << 4) | l);
  }
  return true;
}

const char* failStageName(const HttpDownloader::FailStage stage) {
  switch (stage) {
    case HttpDownloader::FailStage::NONE:
      return "none";
    case HttpDownloader::FailStage::CLIENT_INIT:
      return "client-init";
    case HttpDownloader::FailStage::OPEN:
      return "open";
    case HttpDownloader::FailStage::STATUS:
      return "status";
    case HttpDownloader::FailStage::READ:
      return "read";
    case HttpDownloader::FailStage::REDIRECT:
      return "redirect";
    case HttpDownloader::FailStage::BUFFER_OOM:
      return "buffer-oom";
    default:
      return "other";
  }
}
}  // namespace

#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"

namespace {
// Excludes pre-releases/drafts -- GitHub's own documented behavior for this
// endpoint. Used when Settings -> System -> Pre-release is off (the default).
// Repository the update channels read from. Overridable ONLY so an isolated
// hardware OTA test can run against a dedicated test repository instead of the
// production one -- publishing a test pre-release to production would expose it
// to every other X4 Pro with pre-release enabled.
//
// THE OVERRIDE CHANGES NOTHING BUT THIS STRING. Transport (pinned,
// hostname-verified fetchUrlPinned), metadata parsing, asset-name resolution,
// the SHA-256 digest gate and the installation path are the production code in
// both cases -- that is the point of overriding the repo rather than stubbing
// the channel.
//
// Release builds never define it: the #ifndef below defaults to production, only
// [env:x4pro-otatest] overrides it, and test/ota_test_repo_guard asserts that no
// gh_release* environment does. A build that overrides it also carries
// "-otatest" in CROSSPOINT_VERSION, so a device cannot silently masquerade as a
// release build in a test report.
#ifndef CROSSPOINT_OTA_REPO
#define CROSSPOINT_OTA_REPO "sfoulad/midad-by-foulad"
#endif

constexpr char latestReleaseUrl[] = "https://api.github.com/repos/" CROSSPOINT_OTA_REPO "/releases/latest";
// Every release, newest-first, pre-releases included (GitHub does not surface
// unpublished drafts to an unauthenticated request like this one either way).
// Used when Settings -> System -> Pre-release is on.
//
// per_page=1 because ReleaseJsonParser reads only the FIRST (newest) array
// element and discards everything after it. Without the parameter GitHub sends
// its default page of 30 full release objects -- measured at 114,887 bytes
// against this repo's 190 releases, versus 3,908 with it. That whole payload was
// being streamed over TLS and parsed on a 160MHz single core so that 96% of it
// could be thrown away, which is most of the wait between pressing Check for
// updates and getting an answer. The stable channel never had this:
// /releases/latest is a single object.
constexpr char allReleasesUrl[] = "https://api.github.com/repos/" CROSSPOINT_OTA_REPO "/releases?per_page=1";

// Survives the reboot the OTA flow performs on the way in (silentRestartToOtaCheck),
// which rules out keeping any of this in RAM. Four lines: ETag, then the release the
// ETag was read alongside, so a 304 can be answered without the body.
constexpr char checkCachePath[] = "/.crosspoint/ota_check.txt";
// Bumped if the line layout below changes; a mismatch just discards the cache and
// costs one ordinary conditional-less fetch.
constexpr char checkCacheVersion[] = "2";

struct CheckCache {
  std::string etag;
  std::string tag;
  std::string url;
  size_t size = 0;
  bool usable() const { return !etag.empty() && !tag.empty() && !url.empty(); }
};

CheckCache loadCheckCache(bool prerelease) {
  CheckCache cache;
  const String raw = Storage.readFile(checkCachePath);
  if (raw.isEmpty()) return cache;

  // version / channel / etag / tag / url / size, one per line.
  std::string text(raw.c_str());
  std::vector<std::string> lines;
  size_t start = 0;
  while (lines.size() < 6) {
    const size_t nl = text.find('\n', start);
    lines.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  if (lines.size() < 6 || lines[0] != checkCacheVersion) return cache;
  // The two channels return different documents from different URLs, so an ETag
  // from one is meaningless against the other. Toggling Pre-release discards it.
  if (lines[1] != (prerelease ? "pre" : "stable")) return cache;

  cache.etag = lines[2];
  cache.tag = lines[3];
  cache.url = lines[4];
  cache.size = static_cast<size_t>(strtoul(lines[5].c_str(), nullptr, 10));
  return cache;
}

void saveCheckCache(bool prerelease, const std::string& etag, const std::string& tag, const std::string& url,
                    size_t size) {
  if (etag.empty()) return;  // nothing to condition on next time; don't write a half-cache
  String out;
  out += checkCacheVersion;
  out += "\n";
  out += prerelease ? "pre" : "stable";
  out += "\n";
  out += etag.c_str();
  out += "\n";
  out += tag.c_str();
  out += "\n";
  out += url.c_str();
  out += "\n";
  out += String(static_cast<uint32_t>(size));
  out += "\n";
  Storage.mkdir("/.crosspoint");
  if (!Storage.writeFile(checkCachePath, out)) {
    LOG_ERR("OTA", "Failed to write release check cache");
  }
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  const bool prerelease = SETTINGS.otaPrereleaseEnabled != 0;
  const char* const url = prerelease ? allReleasesUrl : latestReleaseUrl;
  if (std::strcmp(CROSSPOINT_OTA_REPO, "sfoulad/midad-by-foulad") != 0) {
    // Cannot happen in a release build (see CROSSPOINT_OTA_REPO above).
    LOG_ERR("OTA", "TEST BUILD: update channel is %s, NOT the production repository", CROSSPOINT_OTA_REPO);
  }
  LOG_DBG("OTA", "Checking for update (current: %s, channel: %s)", CROSSPOINT_VERSION,
          prerelease ? "pre-release" : "stable");

  // Stream the release JSON straight into the parser as it arrives (the
  // pre-release channel's /releases list runs well past the ~32KB a single
  // /releases/latest object does, since it lists every past release). Buffering
  // the whole body in a std::string would add a growing allocation on top of
  // the TLS session's heap during the fetch; with -fno-exceptions an OOM there
  // aborts. fetchUrl handles the verified-https GET, redirects, and User-Agent
  // (see HttpDownloader).
  // Conditional GET. Unauthenticated GitHub API calls share a 60-per-hour budget
  // with every other device and tool behind the same public IP, and exhausting it
  // answers 403 -- reported from the field as "Update failed ... http 5:403",
  // intermittent because the window resets hourly. A 304 does not count against
  // that budget, so an unchanged release list makes repeat checks effectively free.
  // Only condition when the cache can actually answer a 304 (see usable()),
  // otherwise a 304 would leave nothing to report.
  const CheckCache cache = loadCheckCache(prerelease);
  HttpDownloader::ConditionalGet conditional;
  if (cache.usable()) conditional.ifNoneMatch = cache.etag;

  ReleaseJsonParser releaseParser;
  releaseParser.setFirmwareAssetName("");
  // Each board updates from crosspoint-<version>-<device>.bin. The combined
  // C3 image uses x3-x4; other asset suffixes match their firmware board tag.
  const bool isX4 = board_tag::boardNameLen() == 2 && memcmp(board_tag::boardName(), "x4", 2) == 0;
  char assetSuffix[20] = "-x3-x4";
  if (!isX4) {
    snprintf(assetSuffix, sizeof(assetSuffix), "-%.*s", static_cast<int>(board_tag::boardNameLen()),
             board_tag::boardName());
  }
  // Asset name is only knowable once tag_name has been seen, so the body is fed
  // one byte at a time until then, and in bulk afterwards.
  char assetName[48] = {};
  bool assetNameSet = false;
  auto feed = [&](const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (!assetNameSet && offset < len) {
      releaseParser.feed(reinterpret_cast<const char*>(data + offset), 1);
      offset++;
      if (releaseParser.foundTag()) {
        // Assets are named from the version, not the tag: the release workflow
        // writes crosspoint-${TAG#v}-<device>.bin, so a "v" prefix must be dropped
        // here too. No-op for tags that already start with a digit.
        const char* version = releaseParser.getTagName();
        if (*version == 'v') ++version;
        snprintf(assetName, sizeof(assetName), "crosspoint-%s%s.bin", version, assetSuffix);
        releaseParser.setFirmwareAssetName(assetName);
        assetNameSet = true;
      }
    }
    if (offset < len) releaseParser.feed(reinterpret_cast<const char*>(data + offset), len - offset);
    return true;
  };

  // Release metadata decides WHICH image gets installed, so it runs pinned and
  // hostname-verified against the measured Sectigo ECDSA root (GithubCaCerts.h).
  // The conditional/ETag path is given up deliberately: it only exists on the
  // esp_http_client transport, which aborts rather than erroring when it cannot
  // allocate (http_utils.c:72 asserts on a failed realloc, observed killing this
  // device mid-check). A re-fetched JSON body is a cheap price for a path that
  // fails gracefully.
  const bool ok = HttpDownloader::fetchUrlPinned(url, feed, github_ca::kGithubApiCaAnchors);
  conditional.notModified = false;

  if (!ok) {
    const auto f = HttpDownloader::getLastFailure();
    LOG_ERR("OTA", "update FAILED stage=metadata host=%s fail=%s detail=%d heap=%u block=%u",
            sanitizedHost(url).c_str(), failStageName(f.stage), f.detail, static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return HTTP_ERROR;
  }

  // cppcheck-suppress knownConditionTrueFalse // deliberately unreachable, see below
  if (conditional.notModified) {
    LOG_DBG("OTA", "Release list unchanged (304), using cached result: tag=%s", cache.tag.c_str());
    latestVersion = cache.tag;
    otaUrl = cache.url;
    otaSize = cache.size;
    // The on-disk cache predates the digest and cannot supply one. Cleared rather
    // than left stale: a stale digest belongs to a different asset, and
    // installUpdate() must refuse an asset it cannot authenticate. Unreachable while
    // the pinned fetch above forces notModified = false, kept correct regardless.
    otaDigest.clear();
    totalSize = otaSize;
    updateAvailable = true;
    return OK;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_INF("OTA", "No %s asset in latest release", assetName);
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  otaDigest = releaseParser.getFirmwareDigest();
  totalSize = otaSize;
  updateAvailable = true;

  // Store alongside the ETag so the next check can be conditional. Written only on
  // a fully parsed response, so the cache never describes a release we couldn't read.
  saveCheckCache(prerelease, conditional.etag, latestVersion, otaUrl, otaSize);

  LOG_INF("OTA", "update available: tag=%s size=%zu host=%s", latestVersion.c_str(), otaSize,
          sanitizedHost(otaUrl).c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }
  return isVersionNewer(latestVersion.c_str());
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // Drive the OTA partition ourselves and stream the firmware through
  // HttpDownloader::fetchUrlVerified, reusing its redirect handling for the
  // GitHub -> CDN hop. Deliberately the *verified* entry point, not fetchUrl:
  // GitHub's release CDN doesn't need wolfSSL's TLS 1.3 support (that's only
  // needed for some KOSync/OPDS servers), so the firmware binary -- the one
  // download on this device where an unverified TLS connection is a full
  // remote-code-execution risk -- goes over esp_http_client/mbedTLS with the
  // certificate chain checked against esp_crt_bundle_attach instead.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  // The image streams in chunks; only the first bytes carry the header. Buffer
  // the first 14 bytes so we can read chip_id (esp_image_header_t offset 12)
  // and reject a wrong-MCU image before it overwrites the OTA partition.
  uint8_t hdr[14];
  size_t hdrLen = 0;
  bool wrongChip = false;
  // All S3 boards share a chip_id, so also scan the stream for the embedded
  // board tag (FirmwareBoardTag.h). An untagged image passes; a tag naming a
  // different board aborts the download. The wrong image may partially land in
  // the inactive OTA slot, but esp_ota_abort() below means it never becomes
  // the boot target.
  board_tag::Scanner tagScanner;
  // The asset host (objects.githubusercontent.com) chains to an RSA root, and an
  // RSA modexp is what exhausted this device once already (0x4290 =
  // RSA_PUBLIC_FAILED + MPI_ALLOC_FAILED). So the CDN is NOT pinned; the asset is
  // authenticated by the SHA-256 digest that the pinned metadata channel above
  // delivered. Transport stays https-only and bounded.
  if (otaDigest.empty()) {
    LOG_ERR("OTA", "update FAILED stage=digest-missing host=%s -- refusing unauthenticated asset",
            sanitizedHost(otaUrl).c_str());
    esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }
  uint8_t expectedDigest[32];
  if (!parseSha256Digest(otaDigest, expectedDigest)) {
    LOG_ERR("OTA", "update FAILED stage=digest-malformed host=%s", sanitizedHost(otaUrl).c_str());
    esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }
  if (strncasecmp(otaUrl.c_str(), "https://", 8) != 0) {
    LOG_ERR("OTA", "update FAILED stage=asset-scheme host=%s", sanitizedHost(otaUrl).c_str());
    esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  const bool fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    mbedtls_sha256_update(&sha, data, len);
    if (hdrLen < sizeof(hdr)) {
      const size_t take = std::min(len, sizeof(hdr) - hdrLen);
      std::memcpy(hdr + hdrLen, data, take);
      hdrLen += take;
      if (hdrLen == sizeof(hdr)) {
        uint16_t imageChip;
        std::memcpy(&imageChip, hdr + 12, sizeof(imageChip));
        const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
        if (deviceChip != 0xFFFF && imageChip != deviceChip) {
          LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
          wrongChip = true;
          return false;  // abort the transfer
        }
      }
    }
    tagScanner.feed(data, len);
    if (tagScanner.mismatch()) {
      LOG_ERR("OTA", "wrong board: image=%s device=%.*s", tagScanner.foundName(),
              static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
      return false;  // abort the transfer
    }
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (wrongChip || tagScanner.mismatch()) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk) {
    const auto f = HttpDownloader::getLastFailure();
    LOG_ERR("OTA", "update FAILED stage=%s host=%s fail=%s detail=%d written=%u/%u heap=%u block=%u",
            flashOk ? "asset-download" : "flash-write", sanitizedHost(otaUrl).c_str(), failStageName(f.stage), f.detail,
            static_cast<unsigned>(processedSize), static_cast<unsigned>(otaSize),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    mbedtls_sha256_free(&sha);
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  // Authenticity gate, before esp_ota_end() and before any boot-partition change.
  // esp_ota_end() and the chip/board checks prove the image is intact and belongs on
  // this device; only this digest -- delivered over the pinned, hostname-verified
  // metadata channel -- proves it is the image GitHub actually published.
  {
    uint8_t actualDigest[32];
    mbedtls_sha256_finish(&sha, actualDigest);
    mbedtls_sha256_free(&sha);

    if (processedSize != otaSize) {
      LOG_ERR("OTA", "update FAILED stage=size-mismatch got=%u want=%u host=%s", static_cast<unsigned>(processedSize),
              static_cast<unsigned>(otaSize), sanitizedHost(otaUrl).c_str());
      esp_ota_abort(otaHandle);
      return HTTP_ERROR;
    }
    if (memcmp(actualDigest, expectedDigest, sizeof(actualDigest)) != 0) {
      LOG_ERR("OTA", "update FAILED stage=digest-mismatch host=%s bytes=%u", sanitizedHost(otaUrl).c_str(),
              static_cast<unsigned>(processedSize));
      esp_ota_abort(otaHandle);
      return HTTP_ERROR;
    }
    LOG_INF("OTA", "asset authenticated: %u bytes, sha256 matches release metadata",
            static_cast<unsigned>(processedSize));
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
