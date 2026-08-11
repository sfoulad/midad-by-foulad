#pragma once

#include <OpdsParser.h>

#include <string>

#include "FouladOpdsHooksPure.h"
#include "OpdsServerStore.h"

// Foulad-eBooks-specific glue for OpdsBookBrowserActivity: device-tracking
// registration, and the handful of behaviors (news replace-not-accumulate,
// XTC/XTCH extension mapping, Foulad book-id tagging) that only apply to the
// Foulad eBooks catalog, not generic OPDS browsing. Kept out of that
// upstream-owned activity per docs/upstream-sync-architecture.md's Phase C.
//
// extractFouladBookId()/acquisitionExtension()/isNewsFeed() are declared in
// FouladOpdsHooksPure.h (included above) and implemented in
// FouladOpdsHooksPure.cpp, which has no HAL dependency -- see that header
// for why. Everything below needs HalStorage/Epub/FouladDeviceTracking, so
// it's implemented in FouladOpdsHooks.cpp instead.
namespace FouladOpdsHooks {

// Registers this device and flushes any locally-accumulated reading stats/
// crash report/debug log/device stats for the Foulad eBooks account. Called
// once per WiFi connect. KOReader Sync's flush runs unconditionally (its
// server is independently configured, not necessarily midad.one); everything
// else no-ops when `server` isn't Foulad eBooks.
void reportDeviceTrackingOnConnect(const OpdsServer& server);

// Periodic (~30s) re-registration while the catalog stays open, so a setting
// pushed from the "My Devices" web page while this activity is already open
// takes effect without the user having to leave and re-enter Foulad eBooks --
// the connect-time reportDeviceTrackingOnConnect() call above only fires
// once per session. Call this every loop() tick; it no-ops unless
// `isBrowsing` (so it never competes with an in-flight fetch/download for the
// same connection), `server` is Foulad eBooks, WiFi is connected, and roughly
// 30s have elapsed since the last check -- the timer itself is owned inside
// FouladOpdsHooks.cpp, not by the caller, so it persists for the life of the
// program rather than resetting each time a new browser activity is
// constructed (a minor, deliberate difference from the pre-extraction
// per-instance timer -- see docs/upstream-sync-architecture.md's Phase C).
void pollDeviceTracking(const OpdsServer& server, bool isBrowsing);

// News-specific replace-not-accumulate policy (EINK_NEWS_TASKS.md §4): a
// stale download at `filename`, and its render cache, are removed so the
// next download replaces it cleanly instead of the reader finding
// yesterday's cached pages still valid.
void removeExistingNewsDownload(const std::string& filename);

// Associates a just-opened/just-downloaded book at `filename` with its
// Foulad catalog id for later reading-stats reporting -- only when `server`
// is Foulad eBooks and `book.id` actually carries one.
void tagFouladBookId(const std::string& filename, const OpdsEntry& book, const OpdsServer& server);

}  // namespace FouladOpdsHooks
