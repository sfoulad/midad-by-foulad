#pragma once

#include <ArduinoJson.h>

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

// Applies a settings-push payload through the exact same code path
// registerDevice()'s server response already uses (see FouladDeviceTracking.cpp)
// -- exposed here so BleCommandDispatcher.cpp's settings.push command can reuse it
// rather than duplicating the key/type mapping. Silently skips unknown/out-of-range
// keys (same tolerance as the server-push path); persists only if something
// actually changed.
void applySettingsFromServer(JsonObjectConst settings);

// True when a station connection is up. Anything that may reach HttpDownloader must
// check this first: driving esp_wifi with no station asserts inside IDF.
bool wifiConnected();

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

// Delivers a KOReader/MidadReader Sync progress upload queued by
// EpubReaderActivity::onExit() (see PendingKOReaderSync in
// KOReaderCredentialStore.h) -- reading never keeps WiFi connected, so this is
// the reliable moment for it too, same reasoning as flushPendingReadingStats()
// above. Uses KOReaderCredentialStore's own credentials, not the Foulad
// eBooks ones passed to every other function here: KOReader Sync's server can
// be any KOSync-compatible host, not just midad.one. No-op when nothing is
// queued, WiFi is down, or credentials were cleared since queueing (the queue
// entry is dropped in that last case rather than retried forever).
void flushPendingKOReaderSync();

// Delivers a book queued by a BLE book.fetch command (see
// BleCommandDispatcher::handleBookFetch(), CrossPointState::pendingBleBookFetchId)
// -- BLE only ever records the intent; this does the actual download, the same
// "device reconnected for some other reason" moment as flushPendingReadingStats()
// above. Downloads directly by ID, not through the catalog-browsing download path,
// so v1 always assumes .epub and the RECENT_BOOKS entry gets a generic title, not
// the real one -- see FouladDeviceTracking.cpp for the full reasoning. No-op when
// nothing is queued or WiFi is down; left queued (retried every future reconnect,
// no attempt cap) on failure.
void flushPendingBleBookFetch(const std::string& username, const std::string& password);

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

// Uploads a queued crash report on reconnect, then deletes it so it is sent once.
// No-op when none is waiting. Unlike uploadCrashReport() this is not user-initiated:
// a device that panics and reboots cleanly never shows the crash screen, so the
// Send button there delivers nothing for exactly the failures worth seeing.
void flushPendingCrashReport(const std::string& username, const std::string& password);

// Reports device telemetry (battery/RSSI/free heap/uptime) and a reading-
// stats snapshot (streaks, totals, per-book history, reading-day heatmap)
// for the "My Devices" web page's Device Stats / Reading Stats tabs -- see
// EINK_STATS_SYNC_TASKS.md. A separate endpoint from registerDevice(),
// deliberately: the reading snapshot can run to several KB (up to 40 books,
// up to 750 heatmap days), not worth paying on every single connect. Call
// this alongside flushPendingReadingStats() (same "device reconnected for
// some other reason" moment); same no-op-when-offline and
// 404-retry-after-register behavior as every other function here.
//
// `device` telemetry is sent every call (cheap); the larger `reading`
// snapshot is only included when READING_STATS's total has changed since
// the last successful send, and is skipped entirely (this call still sends
// `device` alone) when free heap is below a safety floor -- building the
// full books+reading-days JSON is the single largest allocation this
// module makes, so it backs off under memory pressure rather than risking
// the OOM aborts a prior Gym-app bug already demonstrated on this device.
void reportDeviceStats(const std::string& username, const std::string& password);

// The newest firmware build the catalog server named in its last registerDevice()
// response, if it is newer than the one running; empty otherwise. Read-only, free,
// and always safe to call: it is a string parsed out of a reply the device already
// fetched, so consulting it costs no request, no TLS handshake and no heap.
//
// That is the entire point of routing this through registration. The Library once
// asked GitHub directly and aborted the device doing it -- a TLS handshake needs
// ~32KB of mbedTLS record buffers, and the heap after Home/library browsing has
// nothing like that contiguous (see SettingsActivity's Check for updates, which
// reboots first for exactly this reason).
//
// Empty is the normal state: an older server omits the key, and so does a current
// one that could not reach GitHub. Absent means "no information", never "up to date".
const std::string& pendingFirmwareUpdate();

}  // namespace FouladDeviceTracking
