#pragma once

// Trust anchors for the OTA update path, measured from the live hosts on
// 2026-09-03 rather than assumed.
//
//   api.github.com
//     leaf  CN=*.github.com                    (SAN: *.github.com, github.com)
//     int   Sectigo Public Server Authentication CA DV E36
//     root  Sectigo Public Server Authentication Root E46   ECDSA secp384r1
//
// Both anchors below are ECDSA. That is not a preference, it is a hard
// constraint on this device: verifying against an RSA root runs a modular
// exponentiation that has already exhausted an X3 once -- the original pairing
// failure decoded to RSA_PUBLIC_FAILED + MPI_ALLOC_FAILED (0x4290), which is why
// Midad Core is anchored at the pure-ECDSA GTS Root R4 and why GlobalSign stays
// out of that trust store. The same rule applies here.
//
// This is why the CDN (objects.githubusercontent.com) is NOT pinned: it chains to
// ISRG Root YR, which is RSA. Its asset is authenticated by the SHA-256 digest
// GitHub publishes in the release metadata, which this pinned, hostname-verified
// api.github.com channel delivers. Transport trust for the metadata; content
// authenticity for the asset.
//
// The chain served by api.github.com ends in a Root E46 CROSS-SIGNED by USERTrust
// ECC, not the self-signed root. The self-signed root is pinned here instead
// (same subject and key, valid to 2046 rather than 2038), exactly as
// CoreCaCerts.h pins self-signed GTS Root R4 while the served chain carries the
// GlobalSign cross-certificate. Verified before pinning:
//   openssl verify -CApath /dev/null -CAfile <this root> -untrusted <int> <leaf>
//     -> OK, and a missing-intermediate control correctly fails.
//
// USERTrust ECC is included for rotation overlap: it is the issuer of the
// cross-signed Root E46 in the currently served chain, so a chain that stops at
// the cross-certificate still verifies while Sectigo rotates. Also ECDSA.
namespace github_ca {

// Sectigo Public Server Authentication Root E46 (self-signed, ECDSA secp384r1,
// notAfter 2046-03-21), followed by USERTrust ECC Certification Authority
// (ECDSA, notAfter 2038-01-18) for rotation overlap.
inline constexpr char kGithubApiCaAnchors[] = R"PEMCERT(
-----BEGIN CERTIFICATE-----
MIICOjCCAcGgAwIBAgIQQvLM2htpN0RfFf51KBC49DAKBggqhkjOPQQDAzBfMQsw
CQYDVQQGEwJHQjEYMBYGA1UEChMPU2VjdGlnbyBMaW1pdGVkMTYwNAYDVQQDEy1T
ZWN0aWdvIFB1YmxpYyBTZXJ2ZXIgQXV0aGVudGljYXRpb24gUm9vdCBFNDYwHhcN
MjEwMzIyMDAwMDAwWhcNNDYwMzIxMjM1OTU5WjBfMQswCQYDVQQGEwJHQjEYMBYG
A1UEChMPU2VjdGlnbyBMaW1pdGVkMTYwNAYDVQQDEy1TZWN0aWdvIFB1YmxpYyBT
ZXJ2ZXIgQXV0aGVudGljYXRpb24gUm9vdCBFNDYwdjAQBgcqhkjOPQIBBgUrgQQA
IgNiAAR2+pmpbiDt+dd34wc7qNs9Xzjoq1WmVk/WSOrsfy2qw7LFeeyZYX8QeccC
WvkEN/U0NSt3zn8gj1KjAIns1aeibVvjS5KToID1AZTc8GgHHs3u/iVStSBDHBv+
6xnOQ6OjQjBAMB0GA1UdDgQWBBTRItpMWfFLXyY4qp3W7usNw/upYTAOBgNVHQ8B
Af8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNnADBkAjAn7qRa
qCG76UeXlImldCBteU/IvZNeWBj7LRoAasm4PdCkT0RHlAFWovgzJQxC36oCMB3q
4S6ILuH5px0CMk7yn2xVdOOurvulGu7t0vzCAxHrRVxgED1cf5kDW21USAGKcw==
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICjzCCAhWgAwIBAgIQXIuZxVqUxdJxVt7NiYDMJjAKBggqhkjOPQQDAzCBiDEL
MAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNl
eSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMT
JVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTAwMjAx
MDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMCVVMxEzARBgNVBAgT
Ck5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVUaGUg
VVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBFQ0MgQ2VydGlm
aWNhdGlvbiBBdXRob3JpdHkwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAQarFRaqflo
I+d61SRvU8Za2EurxtW20eZzca7dnNYMYf3boIkDuAUU7FfO7l0/4iGzzvfUinng
o4N+LZfQYcTxmdwlkWOrfzCjtHDix6EznPO/LlxTsV+zfTJ/ijTjeXmjQjBAMB0G
A1UdDgQWBBQ64QmG1M8ZwpZ2dEl23OA1xmNjmjAOBgNVHQ8BAf8EBAMCAQYwDwYD
VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNoADBlAjA2Z6EWCNzklwBBHU6+4WMB
zzuqQhFkoJ2UOQIReVx7Hfpkue4WQrO/isIJxOzksU0CMQDpKmFHjFJKS04YcPbW
RNZu9YO6bVi9JNlWSOrvxKJGgYhqOkbRqZtNyWHa0V1Xahg=
-----END CERTIFICATE-----
)PEMCERT";

}  // namespace github_ca
