#include <gtest/gtest.h>

#include "OtaRollbackDigestFormat.h"

TEST(OtaRollbackDigestFormat, FormatsSha256DigestAsLowercaseHex) {
  // A real esp_app_desc_t::app_elf_sha256 is 32 bytes -> 64 lowercase hex chars.
  const uint8_t digest[32] = {
      0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xa5, 0x5a, 0x10,
      0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90,
  };
  EXPECT_EQ(formatOtaRollbackDigestHex(digest, sizeof(digest)),
            "deadbeef000102030405060708090a0b0c0d0e0fffa55a1020304050607080"
            "90");
}

TEST(OtaRollbackDigestFormat, EmptyInputProducesEmptyString) {
  EXPECT_EQ(formatOtaRollbackDigestHex(nullptr, 0), "");
}

TEST(OtaRollbackDigestFormat, AllZeroBytesFormatAsZeros) {
  const uint8_t digest[4] = {0, 0, 0, 0};
  EXPECT_EQ(formatOtaRollbackDigestHex(digest, sizeof(digest)), "00000000");
}

TEST(OtaRollbackDigestFormat, DifferentDigestsNeverCompareEqual) {
  // Regression guard for OtaRollbackRecoveryPlan's digest comparison: two
  // digests differing in a single byte must produce different hex strings.
  const uint8_t a[2] = {0x12, 0x34};
  const uint8_t b[2] = {0x12, 0x35};
  EXPECT_NE(formatOtaRollbackDigestHex(a, sizeof(a)), formatOtaRollbackDigestHex(b, sizeof(b)));
}

TEST(OtaRollbackDigestFormat, InputLongerThanCapIsTruncatedNotOverflowed) {
  // Documents the fixed-buffer cap in OtaRollbackDigestFormat.cpp (64 bytes)
  // rather than growing the buffer -- this guards against a future caller
  // accidentally passing an oversized/unsanitized length and overflowing a
  // fixed stack buffer instead of just truncating.
  uint8_t oversized[100];
  for (size_t i = 0; i < sizeof(oversized); i++) {
    oversized[i] = static_cast<uint8_t>(i);
  }
  std::string result = formatOtaRollbackDigestHex(oversized, sizeof(oversized));
  EXPECT_EQ(result.size(), 128u);  // 64-byte cap * 2 hex chars
  EXPECT_EQ(result.substr(0, 6), "000102");
}
