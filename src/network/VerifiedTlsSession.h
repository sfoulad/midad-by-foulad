#pragma once

// A wolfSSL TLS session whose verification cannot be switched off.
//
// Two properties are structural here rather than optional, because every
// caller of this class is a request that must fail rather than proceed
// unverified:
//   1. the peer chain must build to one of the PEM anchors the caller passes
//      in (WOLFSSL_VERIFY_PEER, an explicit trust store, no bundle, no
//      fallback), and
//   2. the certificate must name the host we asked for
//      (wolfSSL_check_domain_name -- SNI alone proves nothing, it only tells
//      the server which certificate to send).
// There is no insecure mode and no way to reach one: the class exposes no
// setInsecure(), and connect() refuses a null/empty anchor set.
//
// Separate from the SDK's freeink::SecureClient rather than a change to it:
// that class runs the same wolfSSL engine but calls setInsecure() at its only
// call site today and never calls wolfSSL_check_domain_name(), and it exposes
// no handle to the WOLFSSL session for a caller to add the check on top. The
// handshake configuration below (v23 client method, X25519 key share, 2KB max
// fragment, non-blocking BIO callbacks) is deliberately identical to its
// measured-good one -- those are the MEMFIX-PORT settings that make a
// handshake fit in this device's free heap at all.
//
// Only compiled when wolfSSL is the active TLS stack; callers guard on the
// same flag.

#if defined(FREEINK_NET_WOLFSSL)

#include <WiFiClient.h>

#include <cstddef>
#include <cstdint>

class VerifiedTlsSession {
 public:
  VerifiedTlsSession() = default;
  ~VerifiedTlsSession();
  VerifiedTlsSession(const VerifiedTlsSession&) = delete;
  VerifiedTlsSession& operator=(const VerifiedTlsSession&) = delete;

  // anchorsPem: one or more concatenated PEM certificates (flash-resident;
  // wolfSSL keeps only its parsed Signer entries on the heap). host is used for
  // BOTH the SNI extension and the certificate name check. Returns false on any
  // failure -- transport, handshake, chain or hostname; lastError() then carries
  // wolfSSL's own error code (negative), or 0 if the failure was below TLS.
  bool connect(const char* host, uint16_t port, const char* anchorsPem, uint32_t timeoutMs);

  // Returns bytes written, or -1. Short writes are retried internally.
  int write(const uint8_t* data, size_t len);
  // >0: bytes read. 0: nothing available yet (retry). -1: closed or failed --
  // check connected() to tell a clean end-of-stream from an error.
  int read(uint8_t* data, size_t len);
  bool connected();
  void stop();
  // True once a fatal wolfSSL error has torn this session down. A poisoned
  // session is never usable again and must never be reused by a keep-alive
  // holder -- reusing one is what crashed a real device.
  bool poisoned() const { return poisoned_; }

  int lastError() const { return lastError_; }

 private:
  WiFiClient transport_;
  void* ssl_ = nullptr;  // WOLFSSL*, opaque to keep wolfSSL headers out of here
  void* ctx_ = nullptr;  // WOLFSSL_CTX*
  bool connected_ = false;
  bool poisoned_ = false;
  int lastError_ = 0;

  // Tears the session down on a fatal wolfSSL error, transport first.
  void failFatally(int err);
};

#endif  // FREEINK_NET_WOLFSSL
