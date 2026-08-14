#pragma once

#include <cstdio>
#include <cstring>
#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    WRONG_DEVICE_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  // True when `candidate` (a GitHub tag, leading 'v' optional) names a build newer
  // than the running one. Public and static because the catalog server now reports
  // firmware versions too, and that path needs the identical answer -- a second
  // implementation of "is this newer" is how the two come to disagree about an -rc.
  //
  // Defined inline, below, deliberately: OtaUpdater.cpp is excluded from the
  // simulator build (it needs esp_https_ota), so a caller outside the OTA flow --
  // FouladDeviceTracking, reading the catalog's firmware report -- fails to link
  // against an out-of-line definition. Comparing two version strings needs nothing
  // from that translation unit anyway.
  static bool isVersionNewer(const char* candidate);
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);
};

inline bool OtaUpdater::isVersionNewer(const char* candidate) {
  if (candidate == nullptr || *candidate == '\0') return false;
  const std::string latestVersion(candidate);

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
