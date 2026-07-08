#pragma once
// Minimal host-only stand-in for Arduino's Print.h so BidiUtils.cpp (via Logging.h) can
// be compiled for the host gtest build without pulling in the ESP32 HAL. Never used by
// firmware builds -- this directory is only added to the include path for this test.
#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) { return 0; }
  virtual size_t write(const uint8_t*, size_t n) { return n; }
  virtual void flush() {}
};
