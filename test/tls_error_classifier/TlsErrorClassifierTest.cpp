#include <gtest/gtest.h>

#include "TlsErrorClassifier.h"

// esp_http_client_get_and_clear_last_tls_error reports the POSITIVE magnitude of the
// mbedTLS error code, not mbedTLS's own negative convention -- confirmed against two real
// device logs (v1.6.6, v1.6.7). This is the actual runtime shape isCertChainFailure must
// handle; a prior version of this function only matched the negative constants and never
// triggered the fallback-root retry in production despite passing code review, because
// nothing exercised it against the real value's sign.
TEST(TlsErrorClassifier, PositiveMagnitudeX509FatalError) { EXPECT_TRUE(isCertChainFailure(0x3000)); }

TEST(TlsErrorClassifier, PositiveMagnitudeX509CertVerifyFailed) { EXPECT_TRUE(isCertChainFailure(0x2700)); }

// mbedTLS's own headers define these negative (MBEDTLS_ERR_X509_FATAL_ERROR = -0x3000,
// MBEDTLS_ERR_X509_CERT_VERIFY_FAILED = -0x2700). A future ESP-IDF version could plausibly
// switch back to that convention, so both signs must keep matching.
TEST(TlsErrorClassifier, NegativeMbedtlsConventionX509FatalError) { EXPECT_TRUE(isCertChainFailure(-0x3000)); }

TEST(TlsErrorClassifier, NegativeMbedtlsConventionX509CertVerifyFailed) { EXPECT_TRUE(isCertChainFailure(-0x2700)); }

TEST(TlsErrorClassifier, ZeroIsNotAChainFailure) { EXPECT_FALSE(isCertChainFailure(0)); }

TEST(TlsErrorClassifier, UnrelatedTlsErrorIsNotAChainFailure) {
  // MBEDTLS_ERR_SSL_ALLOC_FAILED (0x7F00) -- seen once in a real device log alongside the
  // chain failures. A real error, but not a chain-resolution problem: retrying against the
  // fallback root wouldn't fix an allocation failure, so this must not be misclassified.
  EXPECT_FALSE(isCertChainFailure(0x7F00));
  EXPECT_FALSE(isCertChainFailure(-0x7F00));
}
