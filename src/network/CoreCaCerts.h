#pragma once

// Trust anchors for api.midad.one (Midad Core), and nothing else.
//
// Live chain, captured 2026-08-31 with
// `openssl s_client -connect api.midad.one:443 -servername api.midad.one -showcerts`:
//
//   0 CN=midad.one                                    (EC P-256, ecdsa-with-SHA256)
//       SAN: midad.one, api.midad.one, *.api.midad.one
//     <- 1 C=US/O=Google Trust Services/CN=WE1        (EC P-256, ecdsa-with-SHA384)
//       <- 2 C=US/O=Google Trust Services LLC/CN=GTS Root R4
//                                                     (EC P-384, sha256WithRSAEncryption)
//            issued by C=BE/O=GlobalSign nv-sa/OU=Root CA/CN=GlobalSign Root CA
//
// Certificate 2 is the CROSS-SIGNED GTS Root R4, not the self-signed one: same
// subject and same P-384 key, but signed by GlobalSign Root CA. That makes two
// legitimate paths to a trust anchor, and both are anchored here:
//
//   served path    leaf <- WE1 <- GTS Root R4 (cross) <- [anchor] GlobalSign Root CA
//   alternate path leaf <- WE1 <- [anchor] GTS Root R4 (self-signed)
//
// BOTH anchors are the minimum, established by verification rather than
// assumption (test/core_ca_anchors re-runs all of it):
//
//  - GlobalSign Root CA alone does NOT verify the alternate path: with only WE1
//    presented, `openssl verify -CAfile <GlobalSign> -untrusted <WE1> <leaf>`
//    fails at depth 1. Google can stop serving the cross-certificate at any
//    time, and must by 2028-01-28, when both GlobalSign Root CA and the cross
//    certificate expire.
//  - GTS Root R4 alone is enough only for a verifier that will discard a
//    redundant trailing cross-certificate (OpenSSL by default; wolfSSL with
//    WOLFSSL_ALT_CERT_CHAINS). A verifier that anchors the TOP of whatever the
//    server sent -- ESP-IDF's esp_crt_bundle callback is exactly that, it looks
//    up the issuer of the last chain certificate and nothing else -- needs
//    GlobalSign Root CA for today's served chain.
//
// PEM copied byte-for-byte from the host system trust store (/etc/ssl/cert.pem,
// Mozilla-derived), confirmed against the CAs' published SHA-256 certificate
// fingerprints, and additionally confirmed to be the SAME key material this
// firmware's own ESP-IDF certificate bundle already carries: each root's
// SubjectPublicKeyInfo DER here is byte-identical to the corresponding entry in
// the compiled x509_crt_bundle (GTS Root R4 at index 31, GlobalSign Root CA at
// index 77 of 150).
//   GTS Root R4:
//     71:CC:A5:39:1F:9E:79:4B:04:80:25:30:B3:63:E1:21:DA:8A:30:43:BB:26:66:2F:EA:4D:CA:7F:C9:51:A4:BD
//   GlobalSign Root CA:
//     EB:D4:10:40:E4:BB:3E:C7:42:C9:E3:81:D3:1E:F2:A4:1A:48:B6:68:5C:96:E7:CE:F3:C1:DF:6C:D4:33:1C:99
//
// ROOTS, not the served intermediates: WE1 is a five-year intermediate that
// rotates, the roots do not.
//
// NOT a fix for the mbedTLS path: nothing here changes what
// HttpDownloader::postJson does. The `esp-x509-crt-bundle: Certificate matched but
// signature verification failed` seen on device is not a missing anchor -- the
// anchor was FOUND (that is what "matched" means) and the failure code 0x4290
// decodes as MBEDTLS_ERR_RSA_PUBLIC_FAILED + MBEDTLS_ERR_MPI_ALLOC_FAILED, an
// allocation failure inside the RSA-2048 verification of the cross certificate.
// HttpDownloader.cpp's redirect-header block records the same signature from an
// earlier incident ("PK verify failed with an MPI/RSA allocation error ... with
// the largest contiguous block down to ~21KB by then"). What this anchor set is
// for is the path that has no bundle at all: wolfSSL carries no default trust
// store, and verifying the alternate path against GTS Root R4 is pure ECDSA --
// no RSA modular exponentiation anywhere in the chain, which is precisely the
// allocation the mbedTLS path fails on. That path is
// HttpDownloader::postJsonWolf, which loads kMidadCoreCaGtsRootR4 (below).
//
// static constexpr: ~2.0KB stays in flash (CLAUDE.md "constexpr First"), never
// copied to DRAM.
namespace core_ca {

// GTS Root R4 + GlobalSign Root CA, concatenated PEM.
inline constexpr char kMidadCoreCaAnchors[] = R"PEMCERT(-----BEGIN CERTIFICATE-----
MIICCjCCAZGgAwIBAgIQbkepyIuUtui7OyrYorLBmTAKBggqhkjOPQQDAzBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQA
IgNiAATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzu
hXyiQHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/l
xKvRHYqjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud
DgQWBBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNnADBkAjBqUFJ0
CMRw3J5QdCHojXohw0+WbhXRIjVhLfoIN+4Zba3bssx9BzT1YBkstTTZbyACMANx
sbqjYAuG7ZoIapVon+Kz4ZNkfF6Tpt95LY2F45TPI11xzPKwTdb+mciUqXWi4w==
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIDdTCCAl2gAwIBAgILBAAAAAABFUtaw5QwDQYJKoZIhvcNAQEFBQAwVzELMAkG
A1UEBhMCQkUxGTAXBgNVBAoTEEdsb2JhbFNpZ24gbnYtc2ExEDAOBgNVBAsTB1Jv
b3QgQ0ExGzAZBgNVBAMTEkdsb2JhbFNpZ24gUm9vdCBDQTAeFw05ODA5MDExMjAw
MDBaFw0yODAxMjgxMjAwMDBaMFcxCzAJBgNVBAYTAkJFMRkwFwYDVQQKExBHbG9i
YWxTaWduIG52LXNhMRAwDgYDVQQLEwdSb290IENBMRswGQYDVQQDExJHbG9iYWxT
aWduIFJvb3QgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDaDuaZ
jc6j40+Kfvvxi4Mla+pIH/EqsLmVEQS98GPR4mdmzxzdzxtIK+6NiY6arymAZavp
xy0Sy6scTHAHoT0KMM0VjU/43dSMUBUc71DuxC73/OlS8pF94G3VNTCOXkNz8kHp
1Wrjsok6Vjk4bwY8iGlbKk3Fp1S4bInMm/k8yuX9ifUSPJJ4ltbcdG6TRGHRjcdG
snUOhugZitVtbNV4FpWi6cgKOOvyJBNPc1STE4U6G7weNLWLBYy5d4ux2x8gkasJ
U26Qzns3dLlwR5EiUWMWea6xrkEmCMgZK9FGqkjWZCrXgzT/LCrBbBlDSgeF59N8
9iFo7+ryUp9/k5DPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8E
BTADAQH/MB0GA1UdDgQWBBRge2YaRQ2XyolQL30EzTSo//z9SzANBgkqhkiG9w0B
AQUFAAOCAQEA1nPnfE920I2/7LqivjTFKDK1fPxsnCwrvQmeU79rXqoRSLblCKOz
yj1hTdNGCbM+w6DjY1Ub8rrvrTnhQ7k4o+YviiY776BQVvnGCv04zcQLcFGUl5gE
38NflNUVyRRBnMRddWQVDf9VMOyGj/8N7yy5Y0b2qvzfvGn9LhJIZJrglfCm7ymP
AbEVtQwdpf5pLGkkeB6zpxxxYu7KyJesF12KwvhHhm4qxFYxldBniYUr+WymXUad
DKqC5JlR3XC321Y9YeRq4VzW9v493kHMB65jUr9TU/Qr6cf9tveCX4XSQRjbgbME
HMUfpIBvFSDJ3gyICh3WZlXi/EjJKSZp4A==
-----END CERTIFICATE-----
)PEMCERT";

// GTS Root R4 alone: the anchor set the wolfSSL pairing path loads.
//
// Byte-identical to the first certificate of kMidadCoreCaAnchors above (proved
// by test/core_ca_anchors, which compares them) -- the same audited key
// material, not a second capture.
//
// It exists because WHICH anchor a verifier terminates at decides what
// arithmetic it must do. With GlobalSign Root CA also in the store, wolfSSL
// verifies the trailing cross-certificate's own RSA-2048 signature: internal.c's
// ProcessPeerCerts parses and verifies every chain certificate before it asks
// whether that certificate was already a known signer (ProcessPeerCertParse
// computes alreadySigner AFTER ParseCertRelative), so being trusted twice does
// not skip the work. With only GTS Root R4 loaded, that certificate finds no
// issuer, fails with ASN_NO_SIGNER_E without performing any signature check at
// all, and WOLFSSL_ALT_CERT_CHAINS (set by scripts/patch_wolfssl_midad.py)
// tolerates exactly that for a CA certificate -- leaving the real path,
// leaf <- WE1 <- GTS Root R4, verified end to end on ECDSA P-256 and P-384.
// That is the whole point: no RSA modular exponentiation anywhere in the walk.
//
// Compatibility cost of dropping GlobalSign Root CA HERE: none for this
// verifier. The cross-certificate is redundant when its subject is already
// trusted, and the day Google stops sending it (it must by 2028-01-28) the
// served chain becomes the alternate path this anchor already covers.
// kMidadCoreCaAnchors keeps both for verifiers that anchor at the top of the
// presented chain instead, ESP-IDF's bundle callback among them.
inline constexpr char kMidadCoreCaGtsRootR4[] = R"PEMCERT(-----BEGIN CERTIFICATE-----
MIICCjCCAZGgAwIBAgIQbkepyIuUtui7OyrYorLBmTAKBggqhkjOPQQDAzBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQA
IgNiAATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzu
hXyiQHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/l
xKvRHYqjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud
DgQWBBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNnADBkAjBqUFJ0
CMRw3J5QdCHojXohw0+WbhXRIjVhLfoIN+4Zba3bssx9BzT1YBkstTTZbyACMANx
sbqjYAuG7ZoIapVon+Kz4ZNkfF6Tpt95LY2F45TPI11xzPKwTdb+mciUqXWi4w==
-----END CERTIFICATE-----
)PEMCERT";

}  // namespace core_ca
