#pragma once

#include <string>

#include "DebugLog.h"
#include "RollingSdLog.h"

// Rolling diagnostic log for the on-device web server (file browser, WebDAV, settings
// API) and its file-upload path, tagged into the shared /debug_log.txt (see DebugLog.h).
// The existing LOG_DBG("WEB", ...)/LOG_ERR("WEB", ...) call sites in CrossPointWebServer.cpp
// are compiled OUT entirely in release builds (LOG_LEVEL=1 in gh_release/gh_release_rc,
// platformio.ini) and even in a debug build only survive in a 16-line RTC ring buffer --
// neither reaches a user's SD card, so "the upload froze"/"WebDAV won't start" reports have
// had no durable trace at all. This mirrors SleepDiagLog/BatteryDiagLog's rationale.
namespace WebDiagLog {

inline void append(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[WEB] " + line, DebugLog::MAX_LINES);
}

// force=true bypasses the debugLoggingEnabled gate (see RollingSdLog::append). Reserved for
// server-start/upload allocation failures -- the same OOM class already guarded elsewhere
// (hasHeapForNavigation() etc.) -- not routine request/response chatter.
inline void appendCritical(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[WEB] " + line, DebugLog::MAX_LINES, /*force=*/true);
}

}  // namespace WebDiagLog
