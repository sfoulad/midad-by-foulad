#pragma once

// ============================================================================
// TEMPORARY DIAGNOSTIC INSTRUMENTATION -- REMOVE BEFORE ANY RELEASE ARTIFACT.
// ============================================================================
//
// Answers two questions on a real X3 and nothing else:
//   1. Which phase of the boot -> Home -> WiFi -> QR-login sequence is the one
//      that drives ESP.getMinFreeHeap() from ~47KB down toward ~200 bytes.
//   2. What exactly the allocator refused during the TLS handshake -- the
//      requested size, the capability flags, the ESP-IDF function that asked,
//      and the largest contiguous block AT THAT MOMENT, not after cleanup has
//      already handed the evidence back.
//
// Every symbol here compiles to nothing unless X3_HEAP_TRACE is defined, which
// only [env:x3_heap_trace] in platformio.ini does. No release, board or default
// environment enables it.
//
// Sizes, counts and heap figures only: nothing in this file logs a credential,
// a pairing secret, a device token, a URL query string or certificate material.

#if defined(X3_HEAP_TRACE) && X3_HEAP_TRACE

#include <cstdint>

namespace HeapTrace {

// Registers the ESP-IDF failed-allocation callback. Call once, early in setup().
void begin();

// Emits one line: "[HTRACE] <tag> <edge> free=.. min=.. maxalloc=..".
// `edge` is "BEGIN"/"END" for a bracketed phase, or a bare event name.
void mark(const char* tag, const char* edge);

// printf-style free-form diagnostic line under the same [HTRACE] prefix.
void event(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));

// Names the phase a subsequent allocation failure is attributed to. The pointer
// is stored, never copied -- pass a string literal only.
void setPhase(const char* phase);
const char* phase();

// Filled in by the failed-allocation callback. `seq` is 0 until something has
// actually failed.
struct FailedAlloc {
  uint32_t requestedBytes;
  uint32_t caps;
  const char* caller;  // ESP-IDF's own __func__ (a .rodata literal)
  const char* phase;   // active HEAP_TRACE phase when the refusal happened
  uint32_t largestBlock;
  uint32_t freeHeap;
  uint32_t atMillis;
  uint32_t seq;
};

FailedAlloc lastFailure();
uint32_t failureCount();

// Re-prints the recorded failure (or "none since boot") from task context, so a
// call site can attribute a returned error code to the refusal that caused it.
void reportLastFailure(const char* why);

// Splits an mbedTLS error into its high-level and low-level halves. mbedTLS's
// MBEDTLS_ERROR_ADD packs an outer module error and the inner error that
// produced it into one value, so a single printed code hides the real cause.
// `combined` is the POSITIVE magnitude esp_http_client reports.
void reportMbedtlsError(const char* tag, int combined);

// Brackets a phase: BEGIN on construction, END plus the min-free delta on
// destruction, and names the phase for the failed-allocation callback in between.
class Scope {
 public:
  explicit Scope(const char* tag);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  const char* tag_;
  const char* previousPhase_;
  uint32_t minFreeAtEntry_;
};

}  // namespace HeapTrace

#define HEAP_TRACE_CONCAT_INNER(a, b) a##b
#define HEAP_TRACE_CONCAT(a, b) HEAP_TRACE_CONCAT_INNER(a, b)

#define HEAP_TRACE_BEGIN() ::HeapTrace::begin()
#define HEAP_TRACE_MARK(tag, edge) ::HeapTrace::mark((tag), (edge))
#define HEAP_TRACE_EVENT(tag, ...) ::HeapTrace::event((tag), __VA_ARGS__)
#define HEAP_TRACE_SCOPE(tag) ::HeapTrace::Scope HEAP_TRACE_CONCAT(heapTraceScope_, __LINE__)((tag))
#define HEAP_TRACE_REPORT_FAILURE(why) ::HeapTrace::reportLastFailure((why))
#define HEAP_TRACE_REPORT_MBEDTLS(tag, combined) ::HeapTrace::reportMbedtlsError((tag), (combined))

#else  // X3_HEAP_TRACE

#define HEAP_TRACE_BEGIN() ((void)0)
#define HEAP_TRACE_MARK(tag, edge) ((void)0)
#define HEAP_TRACE_EVENT(tag, ...) ((void)0)
#define HEAP_TRACE_SCOPE(tag) ((void)0)
#define HEAP_TRACE_REPORT_FAILURE(why) ((void)0)
#define HEAP_TRACE_REPORT_MBEDTLS(tag, combined) ((void)0)

#endif  // X3_HEAP_TRACE
