#include "VerifiedTlsSession.h"

#if defined(FREEINK_NET_WOLFSSL)

#include <Arduino.h>
#include <HeapTrace.h>
#include <Logging.h>
#include <wolfssl/ssl.h>

#include <cstring>

namespace {

// wolfSSL's BIO bridged onto the WiFiClient transport. Non-blocking: the
// handshake loop below retries on WANT_READ/WANT_WRITE rather than blocking a
// task for the whole round trip.
int sessionSend(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
  // wolfSSL can call back into the BIO during teardown (close_notify on
  // wolfSSL_free). If the session has already been poisoned by a fatal error its
  // transport is stopped first, so report the connection closed rather than
  // writing through a socket nothing owns any more.
  if (!ctx || !buf || sz <= 0) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
  auto* transport = static_cast<WiFiClient*>(ctx);
  const int written = transport->write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(sz));
  if (written <= 0) {
    // A dead transport must surface as CONN_CLOSE; reporting WANT_WRITE makes
    // the handshake spin to its deadline instead of failing fast.
    if (!transport->connected()) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WOLFSSL_CBIO_ERR_WANT_WRITE;
  }
  return written;
}

int sessionRecv(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
  if (!ctx || !buf || sz <= 0) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
  auto* transport = static_cast<WiFiClient*>(ctx);
  if (!transport->connected() && transport->available() == 0) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
  if (transport->available() == 0) return WOLFSSL_CBIO_ERR_WANT_READ;
  const int got = transport->read(reinterpret_cast<uint8_t*>(buf), static_cast<size_t>(sz));
  if (got <= 0) return WOLFSSL_CBIO_ERR_WANT_READ;
  return got;
}

// Only the wolfSSL_get_error() codes: the WOLFSSL_CBIO_ERR_* callback returns
// never come back out of wolfSSL_get_error and collide with fatal wolfCrypt
// errors (MP_MEM is -2 == WOLFSSL_CBIO_ERR_WANT_READ), which would turn an
// out-of-memory handshake into a spin until the deadline.
bool isWantIo(const int err) { return err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE; }

}  // namespace

VerifiedTlsSession::~VerifiedTlsSession() { stop(); }

bool VerifiedTlsSession::connect(const char* host, const uint16_t port, const char* anchorsPem,
                                 const uint32_t timeoutMs) {
  stop();
  lastError_ = 0;
  if (!host || !*host || !anchorsPem || !*anchorsPem) {
    LOG_ERR("VTLS", "refusing to connect without a host and a trust anchor set");
    return false;
  }

  const uint32_t started = millis();
  transport_.setTimeout(timeoutMs);
  transport_.setConnectionTimeout(timeoutMs);
  if (!transport_.connect(host, port)) {
    LOG_ERR("VTLS", "TCP connect failed: %s:%u", host, static_cast<unsigned>(port));
    return false;
  }

  // Negotiate the highest mutually supported version rather than pinning 1.3:
  // the same choice freeink::SecureClient makes, for the same reason (TLS
  // 1.2-only origins exist). Verification is above version negotiation and is
  // identical either way.
  HEAP_TRACE_MARK("TLS.ctxInit", "BEGIN");
  auto* ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
  if (!ctx) {
    HEAP_TRACE_MARK("TLS.ctxInit", "END-FAIL");
    HEAP_TRACE_REPORT_FAILURE("wolf CTX alloc");
    LOG_ERR("VTLS", "CTX alloc failed (free heap: %u bytes)", static_cast<unsigned>(ESP.getFreeHeap()));
    transport_.stop();
    return false;
  }
  ctx_ = ctx;

  // Explicit, though a wolfSSL client context verifies by default: this is the
  // property the whole class exists for, so it is stated rather than inherited.
  wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, nullptr);
  // A concatenated multi-certificate PEM is loaded whole: wolfSSL's
  // ProcessChainBuffer keeps parsing consecutive BEGIN CERTIFICATE blocks out
  // of the same buffer, so every anchor in the bundle lands in the trust store.
  const int loaded = wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(anchorsPem),
                                                    static_cast<long>(strlen(anchorsPem)), WOLFSSL_FILETYPE_PEM);
  HEAP_TRACE_MARK("TLS.ctxInit", loaded == WOLFSSL_SUCCESS ? "END" : "END-FAIL");
  if (loaded != WOLFSSL_SUCCESS) {
    LOG_ERR("VTLS", "trust anchors rejected: %d", loaded);
    lastError_ = loaded;
    stop();
    return false;
  }
  wolfSSL_SetIORecv(ctx, sessionRecv);
  wolfSSL_SetIOSend(ctx, sessionSend);

  auto* ssl = wolfSSL_new(ctx);
  if (!ssl) {
    HEAP_TRACE_REPORT_FAILURE("wolf SSL alloc");
    LOG_ERR("VTLS", "SSL alloc failed (free heap: %u bytes)", static_cast<unsigned>(ESP.getFreeHeap()));
    stop();
    return false;
  }
  ssl_ = ssl;
  wolfSSL_SetIOReadCtx(ssl, &transport_);
  wolfSSL_SetIOWriteCtx(ssl, &transport_);
  wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, static_cast<unsigned short>(strlen(host)));
  // SNI tells the server which certificate to send; this is what makes the
  // certificate it sent have to be for the host we asked for. Without it
  // wolfSSL checks the chain and nothing else (internal.c only runs the
  // SAN/CN match when ssl->buffers.domainName is set).
  if (wolfSSL_check_domain_name(ssl, host) != WOLFSSL_SUCCESS) {
    LOG_ERR("VTLS", "could not arm the hostname check for %s", host);
    stop();
    return false;
  }
#if defined(WOLFSSL_TLS13) && defined(HAVE_CURVE25519)
  // Pin the TLS 1.3 key_share to X25519: wolfSSL's default P-256 share is
  // generated with fast-math bignums that WOLFSSL_SMALL_STACK heap-allocates at
  // ~4KB apiece, which fails at the free heap a reading session leaves.
  wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_X25519);
#endif
#ifdef HAVE_MAX_FRAGMENT
  // Cap TLS records at 2KB (RFC 6066): wolfSSL sizes its receive buffer to the
  // incoming record, and a default 16KB record demands a ~17KB contiguous
  // allocation this device cannot spare mid-session.
  wolfSSL_UseMaxFragment(ssl, WOLFSSL_MFL_2_11);
#endif

  HEAP_TRACE_MARK("TLS.handshake", "BEGIN");
  const uint32_t deadline = millis() + timeoutMs;
  int ret;
  while ((ret = wolfSSL_connect(ssl)) != WOLFSSL_SUCCESS) {
    const int err = wolfSSL_get_error(ssl, ret);
    if (!isWantIo(err)) {
      HEAP_TRACE_MARK("TLS.handshake", "END-FAIL");
      // wolfSSL_get_verify_result is an OPENSSL_EXTRA symbol this build does not
      // compile; the get_error code already names the failure.
      HEAP_TRACE_EVENT("TLS.handshake", "wolfSSL_connect err=%d", err);
      HEAP_TRACE_REPORT_FAILURE("wolf handshake");
      // -188 ASN_NO_SIGNER_E (chain does not reach an anchor), -155
      // ASN_SIG_CONFIRM_E (bad signature), -185 DOMAIN_NAME_MISMATCH,
      // -125 MEMORY_E (out of heap inside the handshake).
      LOG_ERR("VTLS", "handshake failed: %s err=%d (free heap: %u bytes)", host, err,
              static_cast<unsigned>(ESP.getFreeHeap()));
      lastError_ = err;
      stop();
      return false;
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      HEAP_TRACE_MARK("TLS.handshake", "END-FAIL");
      HEAP_TRACE_REPORT_FAILURE("wolf handshake timeout");
      LOG_ERR("VTLS", "handshake timeout: %s (last err %d, transport %s)", host, err,
              transport_.connected() ? "up" : "down");
      lastError_ = err;
      stop();
      return false;
    }
    delay(5);
  }
  HEAP_TRACE_MARK("TLS.handshake", "END-OK");

  connected_ = true;
  LOG_INF("VTLS", "verified handshake: %s %s / %s in %lu ms", host, wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl),
          static_cast<unsigned long>(millis() - started));
  return true;
}

void VerifiedTlsSession::failFatally(const int err) {
  lastError_ = err;
  poisoned_ = true;
  connected_ = false;
  // Transport first, deliberately. After a wolfCrypt failure -- MEMORY_E from a
  // refused allocation, or AES_GCM_AUTH_E from a record that failed its tag --
  // wolfSSL's own state is no longer trustworthy, and wolfSSL_free() may try to
  // emit a close_notify through the BIO on the way out. Stopping the socket
  // first makes that write a no-op (sessionSend reports CONN_CLOSE) instead of a
  // send driven by corrupted internals. A real device took a Load access fault
  // inside sessionSend exactly this way, after MEMORY_E then AES_GCM_AUTH_E.
  transport_.stop();
  if (ssl_) {
    wolfSSL_free(static_cast<WOLFSSL*>(ssl_));
    ssl_ = nullptr;
  }
  if (ctx_) {
    wolfSSL_CTX_free(static_cast<WOLFSSL_CTX*>(ctx_));
    ctx_ = nullptr;
  }
}

int VerifiedTlsSession::write(const uint8_t* data, const size_t len) {
  if (!connected_ || poisoned_ || !ssl_ || !data) return -1;
  auto* ssl = static_cast<WOLFSSL*>(ssl_);
  size_t off = 0;
  while (off < len) {
    const int written = wolfSSL_write(ssl, data + off, static_cast<int>(len - off));
    if (written > 0) {
      off += static_cast<size_t>(written);
      continue;
    }
    const int err = wolfSSL_get_error(ssl, written);
    if (isWantIo(err)) {
      delay(2);
      continue;
    }
    LOG_ERR("VTLS", "write failed: %d", err);
    failFatally(err);
    return -1;
  }
  return static_cast<int>(len);
}

int VerifiedTlsSession::read(uint8_t* data, const size_t len) {
  if (!connected_ || poisoned_ || !ssl_ || !data) return -1;
  auto* ssl = static_cast<WOLFSSL*>(ssl_);
  const int got = wolfSSL_read(ssl, data, static_cast<int>(len));
  if (got > 0) return got;

  const int err = wolfSSL_get_error(ssl, got);
  if (isWantIo(err)) return 0;
  if (err == WOLFSSL_ERROR_ZERO_RETURN) {  // clean close_notify: end of stream
    connected_ = false;
    return -1;
  }
  LOG_ERR("VTLS", "read failed: %d (free heap: %u bytes)", err, static_cast<unsigned>(ESP.getFreeHeap()));
  failFatally(err);
  return -1;
}

bool VerifiedTlsSession::connected() { return connected_ && !poisoned_ && ssl_ && transport_.connected(); }

void VerifiedTlsSession::stop() {
  if (ssl_) {
    wolfSSL_free(static_cast<WOLFSSL*>(ssl_));
    ssl_ = nullptr;
  }
  if (ctx_) {
    wolfSSL_CTX_free(static_cast<WOLFSSL_CTX*>(ctx_));
    ctx_ = nullptr;
  }
  transport_.stop();
  connected_ = false;
}

#endif  // FREEINK_NET_WOLFSSL
