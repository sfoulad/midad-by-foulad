#pragma once

#include <cstdint>

// One gate every FouladDeviceTracking upload must pass before it touches the network.
//
// Background: on 2026-09-01 a reference X3 was reported "slow and stuck". Within ~17s of
// WiFi reaching ready, device registration, the crash-report upload and the debug-log
// upload all fired together, each needing a ~16.7KB contiguous TLS buffer. They drove the
// heap to 3264 bytes free with the largest block collapsing from 49140 to 23540 -- and
// then retried unconditionally every 30s, so the device never recovered.
//
// The uploads could not have succeeded in any case: Core serves no /opds/device,
// /opds/reading-stats, /opds/device-log or /opds/device-stats route. The module was
// written against the foulad-ebooks contract and FOULAD_EBOOKS_URL was later repointed at
// api.midad.one/opds without those routes being ported, so every attempt was doomed
// before it allocated anything.
namespace FouladTelemetryGate {

// Why an upload is refused. Ordered most-fundamental first; check() reports the first
// that applies, so a log line names the real reason rather than a downstream symptom.
enum class Block : uint8_t {
  None,
  RoutesUnimplemented,
  UnverifiedTls,
  LowContiguousHeap,
  Backoff,
};

// Largest contiguous block an upload must see before it is allowed to start. Above the
// ~16.7KB the TLS path actually requested, with margin: free heap is not the constraint
// here -- the device had ~40KB free while the largest block was too small.
inline constexpr uint32_t MIN_CONTIGUOUS_BYTES = 24576;

// Bounded exponential backoff, replacing the unconditional 30s retry.
inline constexpr unsigned long BACKOFF_BASE_MS = 30000;
inline constexpr unsigned long BACKOFF_MAX_MS = 3600000;  // 1 hour, then stop doubling
inline constexpr uint8_t MAX_CONSECUTIVE_FAILURES = 5;    // then wait for a successful connect

Block check(unsigned long nowMs);
bool allow(unsigned long nowMs);

// Records the outcome so the backoff can widen or reset. noteFailure() is what stops the
// 30s loop; noteSuccess() (and a fresh connection) clears it.
void noteFailure(unsigned long nowMs);
void noteSuccess();
void resetForNewConnection();

// Human-readable reason, for the on-SD diagnostic log.
const char* describe(Block b);

}  // namespace FouladTelemetryGate
