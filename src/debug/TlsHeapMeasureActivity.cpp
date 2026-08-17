#include "TlsHeapMeasureActivity.h"
#ifdef TLS_HEAP_MEASURE_HARNESS

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "debug/TlsHeapMeasureCaBundles.h"
#include "fontIds.h"

namespace {

// Samples ESP.getFreeHeap() at ~15ms intervals from a low-priority task while
// a blocking connect() call is in flight, approximating the heap trough/peak
// reached mid-handshake -- something a synchronous before/after read on the
// caller's own task cannot observe. Best-effort only: FreeRTOS gives no
// guarantee the sampler actually runs during the exact millisecond the
// handshake's largest transient allocation is live, so treat min/max as an
// approximation, not an exact instrument.
struct HandshakeHeapSampler {
  static void taskFn(void* arg) {
    auto* self = static_cast<HandshakeHeapSampler*>(arg);
    while (self->running) {
      const uint32_t freeNow = ESP.getFreeHeap();
      if (freeNow < self->minHeap) self->minHeap = freeNow;
      if (freeNow > self->maxHeap) self->maxHeap = freeNow;
      self->samples++;
      vTaskDelay(pdMS_TO_TICKS(15));
    }
    xSemaphoreGive(self->doneSem);
    vTaskDelete(nullptr);
  }

  void start() {
    minHeap = UINT32_MAX;
    maxHeap = 0;
    samples = 0;
    doneSem = xSemaphoreCreateBinary();
    if (!doneSem) {
      LOG_ERR("TLSHEAP", "OOM creating sampler semaphore; skipping in-handshake sampling this cycle");
      return;
    }
    running = true;
    if (xTaskCreate(&taskFn, "tls-heap-sample", 2048, this, 1, &task) != pdPASS) {
      LOG_ERR("TLSHEAP", "OOM creating sampler task; skipping in-handshake sampling this cycle");
      running = false;
      vSemaphoreDelete(doneSem);
      doneSem = nullptr;
    }
  }

  // Signals the task to stop and blocks until it confirms exit, so the
  // caller never reads minHeap/maxHeap while the task might still be
  // writing them.
  void stop() {
    if (!doneSem) return;
    running = false;
    xSemaphoreTake(doneSem, pdMS_TO_TICKS(500));
    vSemaphoreDelete(doneSem);
    doneSem = nullptr;
  }

  bool hasSamples() const { return samples > 0; }

  volatile bool running = false;
  volatile uint32_t minHeap = UINT32_MAX;
  volatile uint32_t maxHeap = 0;
  volatile uint32_t samples = 0;
  TaskHandle_t task = nullptr;
  SemaphoreHandle_t doneSem = nullptr;
};

void logHeapCheckpoint(const char* tag, int cycle, const char* checkpoint) {
  LOG_DBG("MEM", "[%s c%d] %-28s free=%u largest=%u psramSize=%u psramFree=%u", tag, cycle, checkpoint,
          static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(ESP.getPsramSize()), static_cast<unsigned>(ESP.getFreePsram()));
}

}  // namespace

void TlsHeapMeasureActivity::BundleStats::recordPostCycle(const uint32_t free, const uint32_t largest) {
  minFree = std::min(minFree, free);
  maxFree = std::max(maxFree, free);
  finalFree = free;
  minLargest = std::min(minLargest, largest);
  maxLargest = std::max(maxLargest, largest);
  finalLargest = largest;
}

void TlsHeapMeasureActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("TLSHEAP", "TLS heap measurement harness: bringing up WiFi...");
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void TlsHeapMeasureActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    // Internal diagnostic tool, not polished UX: no saved WiFi credentials
    // (or a failed auto-connect) just stops here -- see WifiSelectionActivity
    // for the auto-connect/scan logic this reuses.
    LOG_ERR("TLSHEAP", "WiFi connection failed; TLS heap measurement harness cannot run");
    {
      RenderLock lock(*this);
      state = State::FAILED;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::RUNNING;
  }
  requestUpdateAndWait();

  runAllBundles();

  {
    RenderLock lock(*this);
    state = State::DONE;
  }
  requestUpdate();
}

void TlsHeapMeasureActivity::runAllBundles() {
  LOG_INF("TLSHEAP",
          "=== TLS heap measurement harness starting: free=%u largest=%u ===", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  // Hosts were chosen by directly inspecting each candidate's live
  // certificate chain (openssl s_client -showcerts), not by guessing from
  // PEM comments -- see TlsHeapMeasureCaBundles.h for the verified chains.
  runBundle("1ROOT", kOneRootCaBundle, kOneRootCertCount, kOneRootBundleBytes, "www.globalsign.com", 443);
  runBundle("PROD", kProductionCaBundle, kProductionCertCount, kProductionBundleBytes, "example.com", 443);
  runBundle("STRESS", kStressCaBundle, kStressCertCount, kStressBundleBytes, "example.com", 443);

  LOG_INF("TLSHEAP", "=== TLS heap measurement harness complete ===");
}

void TlsHeapMeasureActivity::runBundle(const char* tag, const char* pemBundle, const size_t certCount,
                                       const size_t bundleBytes, const char* host, const uint16_t port) {
  LOG_INF("TLSHEAP", "--- bundle %s: %u cert(s), %u PEM bytes, host=%s:%u, %d cycles ---", tag,
          static_cast<unsigned>(certCount), static_cast<unsigned>(bundleBytes), host, port, kCyclesPerBundle);

  BundleStats stats;
  for (int cycle = 1; cycle <= kCyclesPerBundle; ++cycle) {
    runCycle(tag, pemBundle, host, port, cycle, stats);
    delay(50);  // let the previous cycle's TCP socket fully release before the next connect()
  }

  // Summary is computed from the post-teardown checkpoint of each cycle --
  // the steady-state heap once CTX/SSL/transport are all freed -- so drift
  // across cycles shows fragmentation that persists between connections,
  // not just a snapshot mid-handshake.
  LOG_INF("TLSHEAP",
          "--- bundle %s summary (post-teardown, %d/%d ok): free min=%u max=%u final=%u | largest min=%u max=%u "
          "final=%u ---",
          tag, stats.successCount, kCyclesPerBundle, static_cast<unsigned>(stats.minFree),
          static_cast<unsigned>(stats.maxFree), static_cast<unsigned>(stats.finalFree),
          static_cast<unsigned>(stats.minLargest), static_cast<unsigned>(stats.maxLargest),
          static_cast<unsigned>(stats.finalLargest));
}

void TlsHeapMeasureActivity::runCycle(const char* tag, const char* pemBundle, const char* host, const uint16_t port,
                                      const int cycle, BundleStats& stats) {
  logHeapCheckpoint(tag, cycle, "before-construct");
  auto client = makeUniqueNoThrow<freeink::SecureClient>();
  if (!client) {
    LOG_ERR("TLSHEAP", "[%s c%d] OOM constructing SecureClient; skipping cycle", tag, cycle);
    stats.failCount++;
    return;
  }
  logHeapCheckpoint(tag, cycle, "after-construct");

  logHeapCheckpoint(tag, cycle, "before-setCACert");
  client->setCACert(pemBundle);
  // setCACert() (SecureClient.cpp) only stores the pointer; the real
  // wolfSSL_CTX_load_verify_buffer() parse happens fused with CTX creation
  // and the handshake itself inside connect() (connectWithMethod()), so this
  // checkpoint and "before-connect" below are expected to read the same
  // value -- SecureClient's public API has no separate hook for "CA parsed,
  // handshake not yet started" without editing the freeink-sdk submodule.
  logHeapCheckpoint(tag, cycle, "after-setCACert");

  logHeapCheckpoint(tag, cycle, "before-connect");

  HandshakeHeapSampler sampler;
  sampler.start();
  const int ok = client->connect(host, port);
  sampler.stop();

  logHeapCheckpoint(tag, cycle, ok ? "after-handshake-ok" : "after-handshake-FAILED");
  if (sampler.hasSamples()) {
    LOG_DBG("MEM", "[%s c%d] in-handshake approx (~15ms sampling, best-effort) free min=%u max=%u samples=%u", tag,
            cycle, static_cast<unsigned>(sampler.minHeap), static_cast<unsigned>(sampler.maxHeap),
            static_cast<unsigned>(sampler.samples));
  }

  client.reset();  // ~SecureClient() -> stop(): frees WOLFSSL*/WOLFSSL_CTX* and the TCP transport
  logHeapCheckpoint(tag, cycle, "after-teardown");

  if (ok) {
    stats.successCount++;
  } else {
    stats.failCount++;
    LOG_ERR("TLSHEAP", "[%s c%d] handshake failed against %s:%u", tag, cycle, host, port);
  }
  stats.recordPostCycle(ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void TlsHeapMeasureActivity::loop() {
  if ((state == State::DONE || state == State::FAILED) && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void TlsHeapMeasureActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  // Hardcoded, not tr(): this screen exists only in the default_tls_measure
  // debug build (never shipped, never localized) -- see platformio.ini.
  const char* message;
  switch (state) {
    case State::WAITING_WIFI:
      message = "TLS Heap Measure: connecting WiFi...";
      break;
    case State::RUNNING:
      message = "TLS Heap Measure: running -- see serial log";
      break;
    case State::DONE:
      message = "TLS Heap Measure: done -- see serial log";
      break;
    case State::FAILED:
    default:
      message = "TLS Heap Measure: WiFi failed";
      break;
  }
  renderer.drawCenteredText(UI_10_FONT_ID, top, message);
  renderer.displayBuffer();
}

#endif  // TLS_HEAP_MEASURE_HARNESS
