#pragma once

#include "CrossPointSettings.h"

// Single gate checked by every rolling SD diagnostic log's write path (see
// CrossPointSettings::debugLoggingEnabled's own comment for the full list and
// rationale). Deliberately does NOT gate crash_report.txt -- that's a one-shot
// panic capture, not routine verbose logging.
namespace DebugLogging {

inline bool enabled() { return SETTINGS.debugLoggingEnabled != 0; }

}  // namespace DebugLogging
