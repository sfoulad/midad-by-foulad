#pragma once

// True for the mbedTLS error codes that indicate "couldn't resolve/verify the certificate
// chain" as opposed to a non-TLS network failure (DNS/TCP) or a genuinely bad cert (expired,
// hostname mismatch -- those set esp_tls_flags bits instead, not this code). Used by
// HttpDownloader.cpp to decide whether a TLS open() failure is worth retrying against the
// fallback trusted root, or whether retrying wouldn't fix anything.
//
// mbedTLS's own headers define these as negative (MBEDTLS_ERR_X509_FATAL_ERROR = -0x3000,
// MBEDTLS_ERR_X509_CERT_VERIFY_FAILED = -0x2700), but esp_http_client_get_and_clear_last_tls_error
// returns the POSITIVE magnitude instead -- confirmed on a real device: a logged
// "esp_tls_error_code=-0xFFFFD000" (HttpDownloader.cpp's own log line negates the raw value
// before printing) decodes to a raw stored value of +0x3000, not -0x3000. A first version of
// this function compared against the negative constants and never matched anything, silently
// making the fallback-root retry dead code in v1.6.7 (confirmed: v1.6.7 behaved identically to
// v1.6.6 on device, no retry ever attempted -- see TlsErrorClassifierTest.cpp for the regression
// test this bug should have been caught by). Comparing the absolute value is robust to either
// sign convention, so a future ESP-IDF version flipping it back can't silently break this again
// the same way.
bool isCertChainFailure(int tlsErrorCode);
