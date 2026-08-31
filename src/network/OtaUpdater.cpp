#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
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
#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"

namespace {
// Excludes pre-releases/drafts -- GitHub's own documented behavior for this
// endpoint. Used when Settings -> System -> Pre-release is off (the default).
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/sfoulad/midad-by-foulad/releases/latest";
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
constexpr char allReleasesUrl[] = "https://api.github.com/repos/sfoulad/midad-by-foulad/releases?per_page=1";

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
  // Each board updates from its own release asset: plain firmware.bin for the
  // C3 X4/X3 binary (pre-existing releases), firmware-<board>.bin otherwise.
  const bool isX4 = board_tag::boardNameLen() == 2 && memcmp(board_tag::boardName(), "x4", 2) == 0;
  char assetName[48] = "firmware.bin";
  if (!isX4) {
    snprintf(assetName, sizeof(assetName), "firmware-%.*s.bin", static_cast<int>(board_tag::boardNameLen()),
             board_tag::boardName());
  }
  releaseParser.setFirmwareAssetName(assetName);
  auto feed = [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  };

  const bool ok = HttpDownloader::fetchUrl(url, feed, conditional);

  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  if (conditional.notModified) {
    LOG_DBG("OTA", "Release list unchanged (304), using cached result: tag=%s", cache.tag.c_str());
    latestVersion = cache.tag;
    otaUrl = cache.url;
    otaSize = cache.size;
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
  totalSize = otaSize;
  updateAvailable = true;

  // Store alongside the ETag so the next check can be conditional. Written only on
  // a fully parsed response, so the cache never describes a release we couldn't read.
  saveCheckCache(prerelease, conditional.etag, latestVersion, otaUrl, otaSize);

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
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
  const bool fetchOk = HttpDownloader::fetchUrlVerified(otaUrl, [&](const uint8_t* data, size_t len) {
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
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
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

// Disposable gate-wiring proof B: comment-only, never merged.
