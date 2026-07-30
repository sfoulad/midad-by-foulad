#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen before esp_http_client (which includes lwip). Pin this
// order; clang-format would otherwise sort the local header last and break the
// build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_wifi.h>
// clang-format on

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

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

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
}

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
    LOG_ERR("OTA", "No firmware.bin asset found");
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

  // GitHub tag names are conventionally "v1.6.24" while CROSSPOINT_VERSION (from
  // platformio.ini) is the bare "1.6.24" -- strip a leading 'v'/'V' before comparing so the
  // two aren't treated as different versions just because of the tag prefix. This also
  // matters for the sscanf below: handing it a string that starts with a non-digit character
  // leaves its output variables uninitialized rather than parsed, which previously made this
  // function's result depend on stack garbage whenever the tag prefix caused a mismatch here.
  const char* latestVersionStr = latestVersion.c_str();
  if (*latestVersionStr == 'v' || *latestVersionStr == 'V') latestVersionStr++;

  const auto currentVersion = CROSSPOINT_VERSION;
  if (strcmp(latestVersionStr, currentVersion) == 0) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersionStr, "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 15000,
      // 4096 holds the github->CDN redirect headers (the 512 default truncates
      // them); TX only carries our GET. Both are contiguous blocks contending
      // with the TLS handshake on a tight internal arena, so keep them minimal.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  int lastReportedPct = -1;
  do {
    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);
    // Fire the callback only on whole-percent change. Without this it fired
    // every ~100ms perform iteration, waking the render task whose framebuffer
    // work contends with TLS on the same internal arena. E-ink can't repaint
    // faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    // esp_https_ota_perform() is itself a blocking call -- it already reads one
    // buffer_size'd chunk over the network and writes it to flash before returning,
    // so there's no protocol reason to wait afterward. This delay exists only to yield
    // to the scheduler/watchdog between iterations (see CLAUDE.md's own guidance: a
    // tight loop needs a small vTaskDelay to avoid tripping the watchdog, not a long
    // one). The previous 100ms here throttled the whole download to roughly one 4KB
    // chunk per 100ms+ (~40KB/s ceiling) regardless of how fast the network/flash
    // actually were -- a real, reported slowdown ("update very slow") with no upside,
    // since perform()'s own blocking I/O already provides the natural pacing.
    delay(1);
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
