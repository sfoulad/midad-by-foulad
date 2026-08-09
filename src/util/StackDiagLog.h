#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// Periodic stack high-water-mark breadcrumb for the app's two FreeRTOS tasks (the main
// Arduino loop task and ActivityManager's render task), tagged into the shared
// /debug_log.txt. Zero stack-margin visibility existed anywhere in this firmware before
// this -- a classic silent-crash cause (stack overflow corrupting adjacent heap, see
// CLAUDE.md's Stack Safety section) with no instrumentation at all.
// uxTaskGetStackHighWaterMark() returns the LOWEST-EVER free stack margin for the calling
// task, not the current value -- exactly what's needed to catch a rare deep-recursion
// spike that a point-in-time sample would miss. On ESP-IDF's FreeRTOS port StackType_t is
// uint8_t, so the returned count is already bytes (see CLAUDE.md's own debugging section:
// "uxTaskGetStackHighWaterMark(nullptr) (< 512 bytes -> increase stack)") -- no word-to-
// byte conversion needed here, unlike the vanilla FreeRTOS API contract on other ports.
namespace StackDiagLog {

inline void append(const char* taskName, uint32_t freeBytes) {
  RollingSdLog::append(DebugLog::PATH,
                       "[STACK] " + std::string(taskName) + " min free=" + std::to_string(freeBytes) + " bytes",
                       DebugLog::MAX_LINES);
}

}  // namespace StackDiagLog
