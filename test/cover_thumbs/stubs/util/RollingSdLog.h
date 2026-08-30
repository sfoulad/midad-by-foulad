#pragma once

// Host-test stand-in for src/util/RollingSdLog.h. The real header is
// header-only and pulls in Arduino String, the full HalStorage write surface
// and the debug-logging settings gate; none of that is under test here. The
// serial line is what the fault-count tests assert on (see stubs/Logging.h),
// and diagLog() only mirrors that same line to SD through this call, so a no-op
// keeps the mirror from needing an SD card without changing what is counted.

#include <cstddef>
#include <string>

namespace RollingSdLog {

inline void append(const char*, const std::string&, size_t, bool = false) {}

}  // namespace RollingSdLog
