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
