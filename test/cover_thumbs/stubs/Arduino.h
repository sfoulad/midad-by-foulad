#pragma once

// Host-test stand-in for the Arduino core, providing only what CoverThumbs.cpp
// touches: millis() and the ESP heap accessors used to tag SD debug-log lines.
// Fixed, generous heap readings -- nothing under test branches on them.

#include <cstdint>

inline unsigned long millis() { return 0; }

struct EspStub {
  [[nodiscard]] uint32_t getFreeHeap() const { return 200000; }
  [[nodiscard]] uint32_t getMaxAllocHeap() const { return 100000; }
};

inline EspStub ESP;
