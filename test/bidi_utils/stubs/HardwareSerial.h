#pragma once
// Minimal host-only stand-in for Arduino's HardwareSerial.h so BidiUtils.cpp (via
// Logging.h) can be compiled for the host gtest build without pulling in the ESP32 HAL.
// Never used by firmware builds -- this directory is only added to the include path for
// this test. LOG_DBG/LOG_ERR/LOG_INF compile to no-ops here (ENABLE_SERIAL_LOG is not
// defined for this build), so logSerial/Serial are never actually invoked -- they only
// need to exist for Logging.h's `static HWCDC& logSerial = Serial;` to compile.
// Real ESP32 Arduino core's HardwareSerial.h transitively brings in Print.h (for the
// Print base class Logging.h's MySerialImpl derives from) -- mirrored here.
#include "Print.h"

class HWCDC {
 public:
  operator bool() const { return false; }
  void begin(unsigned long) {}
};

inline HWCDC Serial;
