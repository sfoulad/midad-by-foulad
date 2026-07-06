#include "TlsErrorClassifier.h"

bool isCertChainFailure(int tlsErrorCode) {
  constexpr int kMbedtlsErrX509FatalError = 0x3000;
  constexpr int kMbedtlsErrX509CertVerifyFailed = 0x2700;
  const int magnitude = tlsErrorCode < 0 ? -tlsErrorCode : tlsErrorCode;
  return magnitude == kMbedtlsErrX509FatalError || magnitude == kMbedtlsErrX509CertVerifyFailed;
}
