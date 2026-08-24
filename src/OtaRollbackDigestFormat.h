#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Pure hex formatting for an OTA rollback digest -- split out of
// OtaRollbackDetection.cpp so it's host-testable without any esp_ota_ops.h
// dependency. See test/ota_rollback_digest_format/.

// Encodes bytes as a lowercase hex string, two chars per byte. Used to format
// esp_app_desc_t::app_elf_sha256 for OtaRollbackRecoveryPlan's digest comparison.
std::string formatOtaRollbackDigestHex(const uint8_t* bytes, size_t len);
