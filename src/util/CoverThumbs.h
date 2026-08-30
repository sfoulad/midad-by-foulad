#pragma once
#include <cstddef>
#include <string>

#include "util/CoverDiagnostics.h"

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

// Appends a "[COVER] ..." line to the shared on-SD debug log (see
// util/DebugLog.h). Cheap and rare (only called around generation attempts,
// which are themselves throttled); the file lets a cover failure be diagnosed
// from the SD card without a USB serial capture. Lines carry a millis()
// timestamp and free-heap/largest-block readings.
void diagLog(const std::string& line);

// Opens and validates one cached thumbnail for `expectedHeight`, returning the
// SPECIFIC reason it is unusable (CoverDiag::Fault::None when it is fine) and
// writing a short human-readable explanation into `detail` (e.g. the BMP reader
// error, or the size actually found). `expectedHeight <= 0` skips the size check.
//
// Probing is quiet by design -- "this thumb has not been generated yet" is the
// normal first-visit state, not a fault worth an error line. Screens that are
// about to fall back to the placeholder call reportFault() instead.
CoverDiag::Fault probeThumb(const std::string& thumbPath, int expectedHeight, char* detail, size_t detailLen);

// The "should I (re)generate this?" gate. A size mismatch is LOGGED but still
// counts as usable -- see the implementation for why invalidating on size would
// loop -- so this returns false only for a missing, corrupt, or empty-marker file.
bool isUsableThumb(const std::string& thumbPath, int expectedHeight);

// The one greppable line a cover failure leaves behind, so the next report can
// be read off a serial capture instead of guessed at from a photo:
//   [COVER] MISSING-COVER /.crosspoint/epub_123/thumb_300.bmp want=300 (...)
// `module` names the screen (HOME, MYBOOKS, ...). Mirrored into the shared SD
// debug log through diagLog() so a user without a serial cable can still send it.
void reportFault(const char* module, CoverDiag::Fault fault, const std::string& thumbPath, int expectedHeight,
                 const char* detail);

}  // namespace CoverThumbs
