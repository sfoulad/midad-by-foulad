#include "BleDeviceInfoPayload.h"

#include <ArduinoJson.h>

namespace BleCommandDispatcher {

size_t buildDeviceInfoPayload(const DeviceInfoPayloadInput& in, char* outBuf, size_t outBufLen) {
  JsonDocument doc;
  doc["cmd"] = "device.info";
  doc["state"] = "ok";
  doc["serial"] = in.serial;
  doc["model"] = in.model;
  doc["firmware_version"] = in.firmwareVersion;
  doc["claimed"] = in.claimed;

  // HANDOFF FE-P3-RC-BLE-PROTOCOL-COMPAT-001: two representations of the exact same
  // Wi-Fi state.
  //   - "wifi": {...} -- the nested form.
  //   - top-level "wifi_saved"/"wifi_connected"/"wifi_ssid"/"wifi_rssi" -- flat
  //     compatibility fields.
  // The shipped app (foulad-one @ main, lib/data/ble/midad_ble_client.dart's
  // DeviceInfo.fromReply()) prefers the nested "wifi" object whenever it's present at
  // all, and only falls back to the flat fields when "wifi" is entirely absent -- it
  // does NOT fall back per-field. If "wifi" is present but missing a key (e.g.
  // "connected" trimmed out while "wifi_connected" survives), fromReply() reads that
  // key as null/unknown from the nested object; it never reaches for the flat
  // sibling. That makes the nested and flat pair for each field a single unit from
  // the client's point of view -- trimming must drop both halves of a pair together
  // or never drop either, see the atomic removals below.
  // device.info's wifi.saved has no room for a third "couldn't check" state (unlike
  // wifi.autoconnect's dedicated reply, which does) -- callers report a failed saved-
  // credential check as wifiSaved=false, the conservative choice: claiming "saved"
  // when the store couldn't actually be read would be worse than under-reporting it.
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["saved"] = in.wifiSaved;
  wifi["connected"] = in.wifiConnected;
  if (in.wifiConnected) {
    wifi["ssid"] = in.wifiSsid;
    wifi["rssi"] = in.wifiRssi;
  }
  doc["wifi_saved"] = in.wifiSaved;
  doc["wifi_connected"] = in.wifiConnected;
  if (in.wifiConnected) {
    doc["wifi_ssid"] = in.wifiSsid;
    doc["wifi_rssi"] = in.wifiRssi;
  }

  // Fit into the BLE payload budget (kMaxPayloadLen, 160 bytes). Measured live: even
  // with a short release-style firmware_version, cmd/state/serial/model/claimed plus
  // BOTH full Wi-Fi representations runs to ~250+ bytes -- there is no way to always
  // send everything, so this trims in priority order. serializeJson() below has no
  // bounds awareness of its own: given a doc that doesn't fit, it silently writes a
  // truncated (and therefore unparseable) prefix rather than erroring, which is
  // exactly what shipped here once wifi{} first pushed a real dev-build
  // firmware_version over the edge -- confirmed live: a reply cut off mid-object at
  // "wifi":{"saved":.
  //
  // Order below is corrected from an earlier version that dropped "saved" first on
  // the assumption nothing shipped read it yet -- confirmed live (HANDOFF
  // MIDAD-E2E-TF168-001) that assumption was wrong: the real shipped app's
  // BleProvisionScreen decides whether to show the Wi-Fi entry form at all from
  // wifi.saved, and a trimmed-away "saved" made a reader with saved credentials look
  // like it had none, sending every setup attempt through the manual Wi-Fi form
  // regardless of what was actually on the SD card. "saved" and "connected" (both
  // booleans, a few bytes each) now survive as long as possible; the cosmetic
  // ssid/rssi strings/ints (nested and flat) and firmware_version are what actually
  // give the budget back, so those go first.
  //
  // Each pair's nested and flat halves are removed inside the SAME `if`, gated on one
  // measurement -- not two independent `if`s each re-measuring after the other's
  // removal. Two independent ifs can each individually fit the doc after removing
  // just one half (e.g. dropping only wifi.rssi already brings it under budget),
  // leaving the other half's sibling behind -- exactly the half-populated nested/flat
  // mismatch the client can't tolerate (see the comment above).
  if (in.wifiConnected && measureJson(doc) > outBufLen) {
    wifi.remove("rssi");
    doc.remove("wifi_rssi");
  }
  if (in.wifiConnected && measureJson(doc) > outBufLen) {
    wifi.remove("ssid");
    doc.remove("wifi_ssid");
  }
  while (measureJson(doc) > outBufLen) {
    std::string fw = doc["firmware_version"].as<std::string>();
    // Must stop at size()==0, not size()<=1: fw.size() is unsigned, and resize(size()-1)
    // on an already-empty string underflows to SIZE_MAX rather than a negative number.
    // A size-1 firmware_version legitimately needs one more shrink to empty -- confirmed
    // live: a real build's version string ("1") left the whole reply exactly 1 byte over
    // budget with nothing else left to trim, and stopping here at size<=1 silently
    // shipped a truncated, unparseable JSON reply instead of fixing the one byte.
    if (fw.empty()) break;  // give up rather than loop forever
    fw.resize(fw.size() - 1);
    doc["firmware_version"] = fw;
  }
  // "connected" before "saved": saved is the earlier, more foundational routing
  // decision (whether to attempt provisioning at all vs. skip straight to the
  // account-claim step), so it stays if only one of the two can survive.
  if (measureJson(doc) > outBufLen) {
    wifi.remove("connected");
    doc.remove("wifi_connected");
  }
  if (measureJson(doc) > outBufLen) {
    wifi.remove("saved");
    doc.remove("wifi_saved");
  }
  if (measureJson(doc) > outBufLen) doc.remove("wifi");

  if (measureJson(doc) > outBufLen) return 0;
  return serializeJson(doc, outBuf, outBufLen);
}

}  // namespace BleCommandDispatcher
