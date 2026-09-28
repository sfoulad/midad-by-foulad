#include "FouladTelemetryGate.h"

#include <Arduino.h>

namespace FouladTelemetryGate {
namespace {

// The routes now exist. Core R18 (version c1173627-bcb9-47d3-8f78-8c5f6e5672eb,
// deployed 2026-09-04 17:56 UTC, migrations 0035+0036) serves
// POST /devices/v1/heartbeat and POST /devices/v1/logs, both verified live --
// they answer 401 unauthenticated and appear in the OpenAPI listing.
//
// The four routes this comment used to name are gone rather than fixed: the three
// legacy telemetry posts were removed outright, and only the heartbeat and the
// crash-report upload remain, both repointed at the routes above.
constexpr bool ROUTES_IMPLEMENTED = true;

// Both remaining posts now run on the pinned wolfSSL transport, which is what this
// flag was waiting for -- not an edit to the constant. sendHeartbeat() and
// uploadCrashReport() both call HttpDownloader::postJsonWolf() against
// core_ca::kMidadCoreCaGtsRootR4, the same verified path as device-login.
//
// That was not a preference. api.midad.one's chain is ECDSA, and verifying it
// through esp_crt_bundle makes mbedTLS attempt an RSA modexp it cannot allocate --
// observed as PK verify error 0x4290 with free heap driven to 128 bytes, which is
// what broke sign-out until it moved to this transport.
constexpr bool TLS_VERIFIED = true;

uint8_t consecutiveFailures = 0;
unsigned long nextAttemptAtMs = 0;

}  // namespace

Block check(const unsigned long nowMs) {
  if (!ROUTES_IMPLEMENTED) return Block::RoutesUnimplemented;
  if (!TLS_VERIFIED) return Block::UnverifiedTls;
  if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) return Block::Backoff;
  if (nextAttemptAtMs != 0 && static_cast<long>(nowMs - nextAttemptAtMs) < 0) return Block::Backoff;
  if (ESP.getMaxAllocHeap() < MIN_CONTIGUOUS_BYTES) return Block::LowContiguousHeap;
  return Block::None;
}

bool allow(const unsigned long nowMs) { return check(nowMs) == Block::None; }

void noteFailure(const unsigned long nowMs) {
  if (consecutiveFailures < MAX_CONSECUTIVE_FAILURES) consecutiveFailures++;
  unsigned long delay = BACKOFF_BASE_MS;
  for (uint8_t i = 1; i < consecutiveFailures && delay < BACKOFF_MAX_MS; ++i) delay *= 2;
  if (delay > BACKOFF_MAX_MS) delay = BACKOFF_MAX_MS;
  nextAttemptAtMs = nowMs + delay;
}

void noteSuccess() {
  consecutiveFailures = 0;
  nextAttemptAtMs = 0;
}

void resetForNewConnection() { noteSuccess(); }

const char* describe(const Block b) {
  switch (b) {
    case Block::None:
      return "ok";
    case Block::RoutesUnimplemented:
      return "routes-unimplemented";
    case Block::UnverifiedTls:
      return "unverified-tls";
    case Block::LowContiguousHeap:
      return "low-contiguous-heap";
    case Block::Backoff:
      return "backoff";
  }
  return "unknown";
}

}  // namespace FouladTelemetryGate
