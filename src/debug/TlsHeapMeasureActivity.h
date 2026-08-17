#pragma once
#ifdef TLS_HEAP_MEASURE_HARNESS

#include <cstddef>
#include <cstdint>

#include "activities/Activity.h"
#include "activities/ActivityResult.h"

// Debug-only diagnostic: measures wolfSSL's real per-connection heap cost
// (CTX creation + CA-bundle parse + TLS 1.3 handshake, see
// freeink::SecureClient::connectWithMethod()) at three CA-bundle sizes, to
// ground a future setInsecure() -> real certificate verification decision.
// Compiled only by the default_tls_measure PlatformIO env, which is the only
// place that defines TLS_HEAP_MEASURE_HARNESS -- never present in any normal
// or release build. Boots straight into this activity (see main.cpp).
class TlsHeapMeasureActivity final : public Activity {
  struct BundleStats {
    uint32_t minFree = UINT32_MAX;
    uint32_t maxFree = 0;
    uint32_t finalFree = 0;
    uint32_t minLargest = UINT32_MAX;
    uint32_t maxLargest = 0;
    uint32_t finalLargest = 0;
    int successCount = 0;
    int failCount = 0;

    void recordPostCycle(uint32_t free, uint32_t largest);
  };

  enum class State { WAITING_WIFI, RUNNING, DONE, FAILED };
  State state = State::WAITING_WIFI;

  void onWifiSelectionComplete(bool success);
  void runAllBundles();
  void runBundle(const char* tag, const char* pemBundle, size_t certCount, size_t bundleBytes, const char* host,
                 uint16_t port);
  void runCycle(const char* tag, const char* pemBundle, const char* host, uint16_t port, int cycle, BundleStats& stats);

  // 10 connect/destroy cycles per bundle, per the measurement plan -- enough
  // to see fragmentation drift settle (or not) without a multi-minute run.
  static constexpr int kCyclesPerBundle = 10;

 public:
  explicit TlsHeapMeasureActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TlsHeapMeasure", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#endif  // TLS_HEAP_MEASURE_HARNESS
