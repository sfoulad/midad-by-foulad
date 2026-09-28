#include "FouladDeviceTracking.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <cstdlib>

#include "CrossPointSettings.h"
#include "FouladEbooksConfig.h"
#include "FouladTelemetryGate.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MidadAppSettings.h"
#include "RecentBooksStore.h"
#include "network/CoreCaCerts.h"
#include "network/HttpDownloader.h"
#include "network/OtaUpdater.h"
#include "reading/ReadingStatsStore.h"
#include "util/DebugLog.h"
#include "util/DebugLogging.h"
#include "util/RollingSdLog.h"

namespace FouladDeviceTracking {

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

namespace {
constexpr char TAG[] = "FDT";

// Reserved for the device-stats upload returning on /devices/v1/heartbeat: skip
// the books/reading-days arrays unless free heap comfortably covers
// BOTH the actual current data size AND this fixed margin for whatever OPDS
// browsing/WiFi/TLS is already holding -- scaled to the CURRENT book/day
// counts rather than the worst case (40 books + 750 days) once and for all,
// since a fixed worst-case threshold would never pass for a typical user
// with a handful of books, even though their real payload only needs a few
// KB. Per-entry estimates are deliberately generous (ArduinoJson's internal
// variant-slot overhead plus the final serialized string, per entry).
constexpr uint32_t READING_SNAPSHOT_SAFETY_MARGIN_BYTES = 40000;
constexpr uint32_t ESTIMATED_BYTES_PER_BOOK = 350;
constexpr uint32_t ESTIMATED_BYTES_PER_READING_DAY = 90;

// Last-reported READING_STATS total, to decide whether the (larger) reading
// snapshot needs resending -- device telemetry is cheap and always sent, but
// the reading snapshot is only worth another POST when something actually
// changed. Sentinel value ensures the first call after boot always sends it.
uint64_t lastReportedTotalReadingMs = UINT64_MAX;

// Matches the literal path HalSystem::checkPanic() writes to.
constexpr char CRASH_REPORT_PATH[] = "/crash_report.txt";

std::string modelName() { return gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4"; }
std::string displayName() { return std::string("Foulad eInk (") + (gpio.deviceIsX3() ? "X3" : "X4") + ")"; }

// Tagged into the shared on-SD debug log (see util/DebugLog.h) so a
// registration/reporting problem with the LIVE server can be diagnosed from
// the SD card, the same way OPDS browsing/downloading already can.
void diagLog(const std::string& line) { RollingSdLog::append(DebugLog::PATH, "[FDT] " + line, DebugLog::MAX_LINES); }

// For the crash_report.txt lifecycle specifically -- forces through RollingSdLog's
// debugLoggingEnabled gate. Routine FDT chatter stays opt-in via diagLog(), but these
// lines are the only local record of an actual panic once crash_report.txt is deleted,
// and per CrossPointSettings::debugLoggingEnabled's own carve-out, crash reporting must
// survive regardless of the debug-logging preference.
// Single point every network-touching entry below returns through when the gate refuses.
// Logs the reason at most once per reason per boot: the whole point of this change is that
// a refused upload must not become its own source of SD writes and heap churn.
bool telemetryRefused(const char* what) {
  const FouladTelemetryGate::Block b = FouladTelemetryGate::check(millis());
  if (b == FouladTelemetryGate::Block::None) return false;
  static uint8_t loggedReasons = 0;
  const uint8_t bit = static_cast<uint8_t>(1U << static_cast<uint8_t>(b));
  if ((loggedReasons & bit) == 0) {
    loggedReasons = static_cast<uint8_t>(loggedReasons | bit);
    LOG_DBG(TAG, "%s skipped: %s", what, FouladTelemetryGate::describe(b));
  }
  return true;
}

void criticalDiagLog(const std::string& line) {
  RollingSdLog::append(DebugLog::PATH, "[FDT] " + line, DebugLog::MAX_LINES, /*force=*/true);
}

// Web (and on-device) Arabic font picker index -> actual stored
// ARABIC_FONT_FAMILY value. NOT a 1:1 mapping: AMIRI(=1) was dropped from
// flash and is skipped by both UIs (see SettingsList.h's
// buildArabicFontFamilySetting()/kSelectableFonts, which this must stay in
// sync with) -- picker index 1 ("Uthmani Hafs") is stored value 2, not 1.
// Applying the raw web index directly here would silently select the wrong
// font.
constexpr uint8_t kArabicFontDisplayToStored[] = {
    CrossPointSettings::NOTONASKHARABIC,  // 0: Noto Naskh Arabic
    CrossPointSettings::UTHMANICHAFS,     // 1: Uthmani Hafs
    CrossPointSettings::TAJAWAL,          // 2: Tajawal
};
constexpr uint8_t kArabicFontDisplayCount = sizeof(kArabicFontDisplayToStored) / sizeof(kArabicFontDisplayToStored[0]);

uint8_t arabicFontFamilyToDisplayIndex(const uint8_t stored) {
  for (uint8_t i = 0; i < kArabicFontDisplayCount; i++) {
    if (kArabicFontDisplayToStored[i] == stored) return i;
  }
  return 0;  // stale/legacy value (e.g. removed Amiri=1) or out of range
}

}  // namespace

std::string getSerialNumber() {
  // Read the MAC from efuse, NOT via WiFi.macAddress().
  //
  // WiFi.macAddress() returns all zeros until the WiFi driver has been
  // initialised, so anything asking for the serial before the radio comes up got
  // "XTE-000000000000". That is not hypothetical: KOSync's device identity is set
  // once at boot (main.cpp), long before any WiFi, and every progress upload went
  // out carrying the zero serial while pairing -- which happens with WiFi already
  // up -- reported the real one. The account then had two different identifiers
  // for one device, which is exactly what Core's position bridge keys on.
  //
  // esp_read_mac() reads the burned-in address directly and works at any point
  // after boot, with or without a radio.
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[20];
  snprintf(buf, sizeof(buf), "XTE-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

void flushPendingKOReaderSync() {
  if (!wifiConnected()) return;
  if (!KOREADER_STORE.hasPendingSync()) return;
  // Credentials may have been cleared (sign-out, re-pairing) since the item was
  // queued; a stale queue entry would otherwise retry forever with NO_CREDENTIALS.
  if (!KOREADER_STORE.hasCredentials()) {
    KOREADER_STORE.clearPendingSync();
    return;
  }

  const PendingKOReaderSync& pending = *KOREADER_STORE.getPendingSync();

  KOReaderProgress progress;
  progress.document = pending.documentHash;
  progress.progress = pending.xpath;
  progress.percentage = pending.percentage;

  // Rich CrossPoint position, same as the manual sync's live upload -- see
  // KOReaderSyncActivity::performUpload().
  KOReaderRichPosition pos;
  const float pct = pending.percentage < 0.0f ? 0.0f : pending.percentage > 1.0f ? 1.0f : pending.percentage;
  pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
  pos.spineIndex = pending.spineIndex;
  pos.pageNumber = pending.pageNumber;
  pos.totalPages = pending.totalPages;
  pos.paragraphIndex = pending.paragraphIndex;
  pos.xpath = pending.xpath;
  progress.position = std::move(pos);

  const auto result = KOReaderSyncClient::updateProgress(progress);
  if (result == KOReaderSyncClient::OK) {
    KOREADER_STORE.clearPendingSync();
    diagLog("KOReader sync flushed (doc=" + pending.documentHash + ")");
  }
  // Failure (offline mid-request, server error): leave it queued for the next
  // reconnect, same as every other flushPending* function here.
}

// First line of crash_report.txt is "CrossPoint version: X" (HalSystem::checkPanic).
// Empty when absent -- an older report, or a truncated write -- and the caller falls
// back to the running version rather than sending nothing.
std::string readCrashReportVersion() {
  HalFile f;
  if (!Storage.openFileForRead(TAG, CRASH_REPORT_PATH, f)) return "";
  char buf[96] = {};
  const int read = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
  if (read <= 0) return "";
  const std::string head(buf, static_cast<size_t>(read));
  constexpr const char* kPrefix = "CrossPoint version: ";
  const size_t at = head.find(kPrefix);
  if (at == std::string::npos) return "";
  const size_t start = at + strlen(kPrefix);
  const size_t end = head.find_first_of("\r\n", start);
  return head.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Condenses the "Panic reason" and "Heap before panic" lines out of crash_report.txt
// for the debug log -- readCrashReportVersion()'s technique (small bounded read, not
// the full multi-KB report with its backtrace/last-logs sections). This is what lets
// a "crash happened but I can't find crash_report.txt" report get diagnosed at all:
// the SD copy is deleted once an upload succeeds, which can be a LATER boot than the
// one that crashed -- so a user checking the SD card afterward finds nothing, even
// though the crash was real. Logging this summary into debug_log.txt (the file users
// already pull off the device) keeps the panic reason visible locally.
std::string readCrashReportSummary() {
  HalFile f;
  if (!Storage.openFileForRead(TAG, CRASH_REPORT_PATH, f)) return "(unreadable)";
  char buf[512] = {};
  const int read = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
  if (read <= 0) return "(unreadable)";
  const std::string head(buf, static_cast<size_t>(read));

  auto extractLine = [&](const char* prefix) -> std::string {
    const size_t at = head.find(prefix);
    if (at == std::string::npos) return "?";
    const size_t start = at + strlen(prefix);
    const size_t end = head.find_first_of("\r\n", start);
    return head.substr(start, end == std::string::npos ? std::string::npos : end - start);
  };

  return "reason=[" + extractLine("Panic reason: ") + "] heap=[" + extractLine("Heap before panic: ") + "]";
}

bool uploadCrashReport(const std::string& username, const std::string& password) {
  if (telemetryRefused("crash-report upload")) return false;

  if (!wifiConnected() || username.empty() || password.empty()) return false;
  if (!Storage.exists(CRASH_REPORT_PATH)) return false;

  // text/plain, not multipart: multipart only exists on the esp_http_client path,
  // which verifies through the certificate bundle and cannot complete an ECDSA
  // handshake to api.midad.one without attempting an RSA modexp it has no heap
  // for (0x4290). The pinned wolfSSL transport has no multipart encoder, and the
  // contract offers text/plain with ?type= precisely for callers like this.
  HalFile f;
  if (!Storage.openFileForRead(TAG, CRASH_REPORT_PATH, f)) return false;
  const size_t size = f.size();
  if (size == 0) return false;
  if (size > FOULAD_CORE_LOG_MAX_BYTES) {
    // 413 is checked against the declared length before a byte is buffered, so
    // sending it would only waste a handshake. Crash reports are a few KB; this is
    // a guard, not an expected path.
    LOG_ERR(TAG, "Crash report too large to upload (%u bytes, cap %u)", (unsigned)size,
            (unsigned)FOULAD_CORE_LOG_MAX_BYTES);
    criticalDiagLog("crash report upload -> SKIPPED too large " + std::to_string(size));
    return false;
  }
  std::string body;
  body.resize(size);
  if (f.read(&body[0], size) != static_cast<int>(size)) {
    LOG_ERR(TAG, "Crash report read failed");
    return false;
  }

  // The version that CRASHED is already the first line of the report (HalSystem
  // writes it at panic time), which is what the contract asks for -- a log row
  // carries no firmware field of its own, and stamping the RUNNING version would
  // attribute an old panic to a new release. Observed: a 1.8.1 panic arriving
  // labelled 1.8.5-rc.
  std::string response;
  const auto err =
      HttpDownloader::postJsonWolf(std::string(FOULAD_CORE_LOGS_URL) + "?type=crash", body,
                                   core_ca::kMidadCoreCaGtsRootR4, response, 20000, username, password, "text/plain");
  const int status = HttpDownloader::getLastFailure().detail;
  if (err == HttpDownloader::OK) {
    // duplicate:true is success too -- byte-identical re-upload, nothing written.
    criticalDiagLog("crash report upload -> ok");
    return true;
  }
  LOG_ERR(TAG, "Crash report upload failed (status=%d)", status);
  criticalDiagLog("crash report upload -> FAIL status=" + std::to_string(status));
  return false;
}

// Refreshes what Core knows about this device. Since registerDevice was deleted,
// this is the ONLY thing that updates device_tokens.firmware_version after
// pairing -- without it the account shows whatever version was paired with,
// forever.
//
// All fields optional, at least one required, body capped at 2 KB server-side.
// Deliberately modest: no library listing, no settings dump, nothing that grows
// with use.
bool sendHeartbeat(const std::string& username, const std::string& password) {
  if (telemetryRefused("heartbeat")) return false;
  if (!wifiConnected() || username.empty() || password.empty()) return false;

  JsonDocument doc;
  doc["firmwareVersion"] = CROSSPOINT_VERSION;
  doc["deviceName"] = displayName();
  doc["freeHeapBytes"] = ESP.getFreeHeap();
  doc["uptimeSeconds"] = millis() / 1000UL;
  const int rssi = WiFi.RSSI();
  if (rssi < 0 && rssi >= -127) doc["wifiRssi"] = rssi;

  std::string body;
  serializeJson(doc, body);

  std::string response;
  const auto err = HttpDownloader::postJsonWolf(FOULAD_CORE_HEARTBEAT_URL, body, core_ca::kMidadCoreCaGtsRootR4,
                                                response, 15000, username, password);
  if (err == HttpDownloader::OK) {
    LOG_INF(TAG, "Heartbeat accepted");
    diagLog("heartbeat -> ok");
    return true;
  }
  const int status = HttpDownloader::getLastFailure().detail;
  LOG_ERR(TAG, "Heartbeat failed (status=%d)", status);
  diagLog("heartbeat -> FAIL status=" + std::to_string(status));
  return false;
}

}  // namespace FouladDeviceTracking
