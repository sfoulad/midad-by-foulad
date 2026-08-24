#include "OtaRollbackDigestFormat.h"

#include <algorithm>
#include <cstdio>

namespace {
// Fixed stack buffer, not a heap allocation -- the only real caller passes a
// 32-byte SHA-256 digest (esp_app_desc_t::app_elf_sha256); this cap is a
// generous ceiling for that, kept well under the 256-byte stack-variable
// guideline. Longer input is truncated rather than growing the buffer.
constexpr size_t kMaxDigestBytes = 64;
}  // namespace

std::string formatOtaRollbackDigestHex(const uint8_t* bytes, size_t len) {
  len = std::min(len, kMaxDigestBytes);
  char hex[kMaxDigestBytes * 2 + 1];
  for (size_t i = 0; i < len; i++) {
    snprintf(hex + i * 2, 3, "%02x", bytes[i]);
  }
  return std::string(hex, len * 2);
}
