#pragma once
#include <string>

// Once-per-boot throttle for cover thumbnail generation retries.
//
// generateThumbBmp() writes an empty marker BMP when a cover can't be produced;
// treating that marker as permanent blacklisted books forever (fixed in
// a3821bb4), but treating it as freely retryable made every Home/My Books
// entry re-attempt (and re-show the loading popup for) books whose covers
// genuinely cannot generate -- e.g. OPDS downloads from before the
// external-cover fallback existed, which have no embedded cover art at all.
//
// Middle ground: retry a previously-failed thumb at most once per boot. A fix
// that lands in a firmware update still gets its retry on the first boot after
// updating, while a genuinely coverless book costs one attempt per boot
// instead of one per screen entry.
namespace CoverThumbs {

// True if generation for this exact thumb path already ran (and failed --
// success makes the cached-BMP validity check pass, short-circuiting the retry
// gate before this is consulted) since boot.
bool wasAttemptedThisBoot(const std::string& thumbPath);

// Record that generation for this thumb path is being attempted.
void markAttempted(const std::string& thumbPath);

// Appends a line to the on-SD cover diagnostic log (/cover_diag_log.txt) and
// rewrites the file. Cheap and rare (only called around generation attempts,
// which are themselves throttled); the file lets a cover failure be diagnosed
// from the SD card without a USB serial capture, like /opds_error_log.txt.
// Lines carry a millis() timestamp and free-heap/largest-block readings.
void diagLog(const std::string& line);

}  // namespace CoverThumbs
