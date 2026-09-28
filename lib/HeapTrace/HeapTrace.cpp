// TEMPORARY DIAGNOSTIC INSTRUMENTATION -- REMOVE BEFORE ANY RELEASE ARTIFACT.
// See HeapTrace.h. Whole translation unit is empty unless X3_HEAP_TRACE is set.

#include "HeapTrace.h"

#if defined(X3_HEAP_TRACE) && X3_HEAP_TRACE

#include <Arduino.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>

#include <cstdarg>
#include <cstdio>

namespace {

constexpr uint32_t kTraceCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

HeapTrace::FailedAlloc gLast = {};
volatile uint32_t gFailureCount = 0;
const char* gPhase = "boot";
volatile bool gInCallback = false;

// Invoked by ESP-IDF's heap_caps_alloc_failed() the moment an allocation is
// refused, before the caller has had any chance to free anything -- which is the
// whole point: a largest-free-block reading taken after esp_http_client_cleanup()
// describes a heap that has already been given the evidence back.
//
// Constraints honoured here:
//   - No allocation of any kind. No printf/snprintf into a heap buffer, no
//     std::string, no LOG_* (logPrintf appends to a std::string ring).
//     esp_rom_printf writes straight to the ROM UART with a fixed internal path.
//   - No mutex take, no blocking call.
//   - Re-entrancy guarded, in case anything below ever allocates indirectly.
//
// Deliberately NOT marked IRAM_ATTR: ESP-IDF forbids allocation from an ISR or
// with the flash cache disabled, so this always runs in ordinary task context,
// and marking it IRAM_ATTR while it calls flash-resident heap_caps_* helpers
// would advertise a safety property it does not have. heap_caps_alloc_failed()
// is called AFTER heap_caps_malloc_base() returns, so no heap lock is held and
// heap_caps_get_largest_free_block() cannot deadlock here.
void onAllocFailed(size_t size, uint32_t caps, const char* functionName) {
  if (gInCallback) return;
  gInCallback = true;

  const uint32_t largest = heap_caps_get_largest_free_block(kTraceCaps);
  const uint32_t freeNow = heap_caps_get_free_size(kTraceCaps);
  const uint32_t seq = ++gFailureCount;

  gLast.requestedBytes = static_cast<uint32_t>(size);
  gLast.caps = caps;
  gLast.caller = functionName;
  gLast.phase = gPhase;
  gLast.largestBlock = largest;
  gLast.freeHeap = freeNow;
  gLast.atMillis = millis();
  gLast.seq = seq;

  esp_rom_printf("[HTRACE-OOM] #%u phase=%s caller=%s req=%u caps=0x%x largest=%u free=%u ms=%u\n", seq,
                 gPhase ? gPhase : "?", functionName ? functionName : "?", static_cast<uint32_t>(size), caps, largest,
                 freeNow, gLast.atMillis);

  gInCallback = false;
}

}  // namespace

namespace HeapTrace {

void begin() {
  const esp_err_t err = heap_caps_register_failed_alloc_callback(&onAllocFailed);
  LOG_INF("HTRACE", "failed-alloc callback %s (esp_err=%d); free=%u min=%u maxalloc=%u",
          err == ESP_OK ? "registered" : "NOT registered", static_cast<int>(err),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void mark(const char* tag, const char* edge) {
  LOG_INF("HTRACE", "%s %s free=%u min=%u maxalloc=%u", tag, edge, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void event(const char* tag, const char* format, ...) {
  char buf[160];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  LOG_INF("HTRACE", "%s %s", tag, buf);
}

void setPhase(const char* newPhase) { gPhase = newPhase; }

const char* phase() { return gPhase; }

FailedAlloc lastFailure() { return gLast; }

uint32_t failureCount() { return gFailureCount; }

void reportLastFailure(const char* why) {
  if (gLast.seq == 0) {
    LOG_INF("HTRACE", "%s: no allocation has been refused since boot (count=0)", why);
    return;
  }
  LOG_ERR("HTRACE",
          "%s: last refusal #%u at %ums -- phase=%s caller=%s req=%u caps=0x%X largest=%u free=%u (total "
          "refusals=%u)",
          why, static_cast<unsigned>(gLast.seq), static_cast<unsigned>(gLast.atMillis), gLast.phase ? gLast.phase : "?",
          gLast.caller ? gLast.caller : "?", static_cast<unsigned>(gLast.requestedBytes),
          static_cast<unsigned>(gLast.caps), static_cast<unsigned>(gLast.largestBlock),
          static_cast<unsigned>(gLast.freeHeap), static_cast<unsigned>(gFailureCount));
}

void reportMbedtlsError(const char* tag, const int combined) {
  if (combined == 0) return;
  // mbedTLS packs an outer (module) error and the inner error that produced it
  // into one value via MBEDTLS_ERROR_ADD: high-level errors occupy 0xFF80, the
  // low-level ones 0x007F. Printed as mbedTLS's own negative convention.
  // 0x4290 -> outer -0x4280 (MBEDTLS_ERR_RSA_PUBLIC_FAILED, rsa.h) plus inner
  // -0x0010 (MBEDTLS_ERR_MPI_ALLOC_FAILED, bignum.h): an out-of-memory, not a
  // trust failure.
  const int high = combined & 0xFF80;
  const int low = combined & 0x007F;
  LOG_ERR("HTRACE", "%s mbedtls -0x%04X = outer -0x%04X + inner -0x%04X%s", tag, combined, high, low,
          low == 0x0010 ? "  (inner = MBEDTLS_ERR_MPI_ALLOC_FAILED: heap, not trust)" : "");
}

Scope::Scope(const char* tag) : tag_(tag), previousPhase_(gPhase), minFreeAtEntry_(ESP.getMinFreeHeap()) {
  gPhase = tag;
  mark(tag_, "BEGIN");
}

Scope::~Scope() {
  const uint32_t minFreeNow = ESP.getMinFreeHeap();
  LOG_INF("HTRACE", "%s END   free=%u min=%u maxalloc=%u dmin=-%u", tag_, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(minFreeNow), static_cast<unsigned>(ESP.getMaxAllocHeap()),
          static_cast<unsigned>(minFreeAtEntry_ - minFreeNow));
  gPhase = previousPhase_;
}

}  // namespace HeapTrace

#endif  // X3_HEAP_TRACE
