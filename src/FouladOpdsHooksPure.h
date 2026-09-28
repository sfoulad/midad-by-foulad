#pragma once

#include <string>

// The subset of FouladOpdsHooks with zero OpdsParser/OpdsServerStore/HAL
// dependency, split into its own header so it's host-testable without any
// library include paths beyond <string> -- see test/foulad_opds_hooks_pure/.
// Implemented in FouladOpdsHooksPure.cpp; declarations re-exported from
// FouladOpdsHooks.h so callers only ever need to include that one header.
namespace FouladOpdsHooks {

// Foulad eBooks' catalog feed writes each book entry's <id> as
// "urn:opds-library:book:<numeric id>" (see foulad-ebooks'
// resources/views/opds/books.blade.php) -- the same numeric id used for
// device reading-stats reporting (EINK_DEVICE_TRACKING_TASKS.md). Returns ""
// for anything that doesn't end in a run of digits (a non-Foulad OPDS
// server's own id convention, a News entry's "urn:midad:feed:<id>", or a
// malformed entry) so callers can treat that as "no Foulad book id
// available" rather than a parse error.
std::string extractFouladBookId(const std::string& entryId);

// File extension to save an acquisition link's target under, based on
// foulad-ebooks' one-format-per-book MIME convention (XTC-only books are
// Arabic/PDF-sourced and recognized as downloadable BOOK entries too).
// Defaults to ".epub" for anything else.
std::string acquisitionExtension(const std::string& acquisitionType);

// Pure gating decision for a periodic device-tracking call: true only when
// actually browsing (not loading/downloading), the server is Foulad eBooks,
// WiFi is connected, and at least `intervalMs` have elapsed since
// `lastCheckMs` (unsigned subtraction, so this is correct across a millis()
// rollover). Answers only "should it fire right now", so it stays
// host-testable without HAL.
//
// Currently unwired: the registration poll it used to gate is gone, since Core
// has no such endpoint. Kept because the device heartbeat Core is building
// needs exactly this cadence decision, and the tests below pin the rollover
// behaviour that is easy to get wrong when rewriting it from scratch.
bool shouldPollDeviceTracking(bool isBrowsing, bool isFouladServer, bool wifiConnected, unsigned long nowMs,
                              unsigned long lastCheckMs, unsigned long intervalMs);

}  // namespace FouladOpdsHooks
