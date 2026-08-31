#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "FirmwareUpdatePolicy.h"

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
  // True when `candidate` (a GitHub tag, leading 'v' optional) names a build
  // newer than the running one. The comparison itself lives in
  // FirmwareUpdatePolicy.h -- shared with the Library's catalog-reported offer
  // path (FouladDeviceTracking) and the host tests, because a second
  // implementation of "is this newer" is how the two came to disagree about an
  // -rc. Inline so the simulator build (which excludes OtaUpdater.cpp) links.
  static bool isVersionNewer(const char* candidate) {
    return firmware_update_policy::isVersionNewer(candidate, CROSSPOINT_VERSION);
  }
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);
};
