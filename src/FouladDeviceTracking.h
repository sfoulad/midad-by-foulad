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

// True when a station connection is up. Anything that may reach HttpDownloader must
// check this first: driving esp_wifi with no station asserts inside IDF.
bool wifiConnected();

// Delivers a KOReader/MidadReader Sync progress upload queued by
// EpubReaderActivity::onExit() (see PendingKOReaderSync in
// KOReaderCredentialStore.h) -- reading never keeps WiFi connected, so this is
// the reliable moment for it. Uses KOReaderCredentialStore's own credentials, not the Foulad
// eBooks ones passed to every other function here: KOReader Sync's server can
// be any KOSync-compatible host, not just midad.one. No-op when nothing is
// queued, WiFi is down, or credentials were cleared since queueing (the queue
// entry is dropped in that last case rather than retried forever).
void flushPendingKOReaderSync();

// Uploads /crash_report.txt (see HalSystem::checkPanic) to the server as a
// "crash" type upload. Unlike every other function
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

// Refreshes firmware_version, device_name and a telemetry snapshot on Core.
// The only thing that updates firmware_version after pairing.
bool sendHeartbeat(const std::string& username, const std::string& password);

}  // namespace FouladDeviceTracking
