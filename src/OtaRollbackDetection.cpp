#include "OtaRollbackDetection.h"

#ifndef SIMULATOR
#include <esp_ota_ops.h>

#include <cstdio>

bool captureLastInvalidOtaPartitionDigest(std::string& outDigestHex) {
  const esp_partition_t* invalidPartition = esp_ota_get_last_invalid_partition();
  if (!invalidPartition) {
    return false;
  }

  esp_app_desc_t appDesc;
  if (esp_ota_get_partition_description(invalidPartition, &appDesc) != ESP_OK) {
    return false;
  }

  char hex[sizeof(appDesc.app_elf_sha256) * 2 + 1];
  for (size_t i = 0; i < sizeof(appDesc.app_elf_sha256); i++) {
    snprintf(hex + i * 2, 3, "%02x", appDesc.app_elf_sha256[i]);
  }
  outDigestHex.assign(hex, sizeof(hex) - 1);
  return true;
}

#else  // SIMULATOR

// The simulator's esp_ota_ops.h stub doesn't implement
// esp_ota_get_last_invalid_partition()/esp_ota_get_partition_description() --
// there is no real bootloader rollback to detect on the host build.
bool captureLastInvalidOtaPartitionDigest(std::string& /*outDigestHex*/) { return false; }

#endif  // SIMULATOR
