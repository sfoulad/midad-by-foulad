#pragma once
#include <string>

// Wraps esp_ota_get_last_invalid_partition()/esp_ota_get_partition_description()
// so the ESP-IDF dependency is isolated here rather than scattered into main.cpp.
// Returns true and fills outDigestHex (64 lowercase hex chars: the invalid
// partition's app_elf_sha256) if ESP-IDF's bootloader has ever marked an OTA
// slot invalid/aborted. This is a flash-persisted otadata signal, not a one-boot
// event -- it keeps returning the same result on every subsequent boot until
// that specific slot is freshly reflashed (e.g. via SD-card recovery).
//
// Future BLE-triggered OTA (not yet implemented) must consult this same
// function -- and OtaRollbackRecoveryPlan -- before acting, not add its own check.
bool captureLastInvalidOtaPartitionDigest(std::string& outDigestHex);
