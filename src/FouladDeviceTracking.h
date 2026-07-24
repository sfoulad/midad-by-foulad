#pragma once

#include <cstdint>
#include <string>

// Foulad eInk device tracking (Phase 1) -- implements the contract in
// EINK_DEVICE_TRACKING_TASKS.md (foulad-ebooks repo): registering this
// physical device under a Foulad eBooks account, and reporting per-book
// reading progress. Both endpoints reuse the exact same OPDS Basic Auth
// credentials already configured for the Foulad eBooks server -- there is no
// separate authentication to set up.
//
// Every function here is fire-and-forget: offline or a server error is only
// logged, never surfaced to the caller or allowed to block browsing/reading.
// Nothing here ever initiates a WiFi connection itself.
namespace FouladDeviceTracking {

// This device's stable identifier, sent as `serial_number` on every call --
// the WiFi station MAC address, colon-stripped. Always present (unlike the
// optional manufacturing eFuse serial some units lack), and stable across
// reboots/firmware updates for the same physical unit.
std::string getSerialNumber();

// Registers (or re-registers -- the server upserts by serial_number) this
// device under the given Foulad eBooks account. Call once per connection,
// before browsing (e.g. from OpdsBookBrowserActivity right before its first
// fetchFeed()). No-op if WiFi isn't currently connected.
//
// Bidirectional settings sync (EINK_SETTINGS_SYNC_TASKS.md) piggybacks on
// this same call: the request reports this device's current CrossPointSettings/
// KOReaderCredentialStore values (display-only, for the "My Devices" web
// editor), and the response may carry back whatever the user configured
// there -- applied immediately to local settings.json (and koreader.json for
// ko* keys) if the user has ever saved that page for this device, otherwise
// {} and nothing changes. See FouladDeviceTracking.cpp for the exact
// key/type mapping.
void registerDevice(const std::string& username, const std::string& password);

// Reports current reading progress for one (device, book) pair.
// progressPercent and secondsRead are ABSOLUTE totals for this book on this
// device, never deltas -- a retried/duplicated request must not double-count
// reading time. lastPosition is optional and opaque (not interpreted
// server-side). No-op if WiFi isn't connected, fouladBookId is empty, or
// credentials are empty. If the server reports the device isn't registered
// yet (404), this registers it once and retries the report -- callers don't
// need to sequence registerDevice() themselves.
void reportReadingStats(const std::string& username, const std::string& password, const std::string& fouladBookId,
                        int progressPercent, const std::string& lastPosition, uint32_t secondsRead);

// Scans RECENT_BOOKS for entries with a non-empty fouladBookId, looks up each
// one's current totals in READING_STATS, and reports them. Reading itself
// never keeps WiFi connected (OpdsBookBrowserActivity::onExit() tears it down
// before opening the reader), so this is the actually-reliable moment to
// sync progress: whenever the device reconnects to Foulad eBooks for some
// other reason (e.g. browsing/downloading another book). Call this alongside
// registerDevice(); same no-op-when-offline behavior.
void flushPendingReadingStats(const std::string& username, const std::string& password);

// Uploads the on-SD shared debug log (see util/DebugLog.h) to the server,
// where it's visible to Foulad eBooks admins only -- never to the device's
// own owner -- so a report of "the reader crashed" or "covers won't load"
// can be investigated proactively without asking the user for a USB serial
// capture. No-op unless Settings -> Apps -> Debug is enabled (nothing is
// ever sent from a device with debug logging off), WiFi is connected, and
// /debug_log.txt exists. Streamed directly from the SD file (never buffered
// whole into RAM, see HttpDownloader::postFilesMultipart) since the file can
// run to tens of KB. Call this alongside registerDevice(); same
// no-op-when-offline behavior and 404-retry-after-register handling as
// reportReadingStats().
void uploadDebugLog(const std::string& username, const std::string& password);

// Uploads /crash_report.txt (see HalSystem::checkPanic) to the server as a
// "crash" type upload -- stored separately from the debug log snapshot (see
// uploadDebugLog()), same admin-only visibility. Unlike every other function
// here, this is NOT gated on Settings -> Apps -> Debug (a crash report is
// exactly the kind of one-off diagnostic a user should be able to send
// regardless of that setting) and is never called automatically -- only from
// CrashActivity's explicit "Send Log" action, after the caller has already
// ensured WiFi is connected (this does not itself trigger a WiFi connect
// flow, same as every other function here). Returns true on success
// (including a register-and-retry that succeeds) so the caller can show the
// user a result; false if WiFi isn't connected, credentials are empty, the
// crash report file doesn't exist, or the upload ultimately fails.
bool uploadCrashReport(const std::string& username, const std::string& password);

}  // namespace FouladDeviceTracking
