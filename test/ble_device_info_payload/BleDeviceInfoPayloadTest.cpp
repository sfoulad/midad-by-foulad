#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <string>

#include "BleDeviceInfoPayload.h"

namespace {

using BleCommandDispatcher::buildDeviceInfoPayload;
using BleCommandDispatcher::DeviceInfoPayloadInput;

// Mirrors lib/hal/BlePeripheralManager.h's kMaxPayloadLen -- the real BLE GATT
// payload budget every device.info reply must fit inside.
constexpr size_t kMaxPayloadLen = 160;

DeviceInfoPayloadInput baseInput() {
  DeviceInfoPayloadInput in;
  in.serial = "XTE-AABBCCDDEEFF";
  in.model = "Xteink X3";
  in.firmwareVersion = "1.2.3";
  in.claimed = false;
  in.wifiSaved = true;
  in.wifiConnected = true;
  in.wifiSsid = "HomeNetwork";
  in.wifiRssi = -55;
  return in;
}

// Parses the reply and asserts it's syntactically valid JSON, returning the parsed
// document for further field assertions.
JsonDocument parseValid(const char* buf, size_t len) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, buf, len);
  EXPECT_EQ(err, DeserializationError::Ok) << "reply was not valid JSON: " << std::string(buf, len);
  return doc;
}

// A generously large outBufLen (unlike the real 160-byte GATT budget) should trigger
// no trimming at all -- both full Wi-Fi representations survive intact.
TEST(BuildDeviceInfoPayload, NoTrimmingWhenBufferIsGenerous) {
  char buf[512];
  const size_t len = buildDeviceInfoPayload(baseInput(), buf, sizeof(buf));
  ASSERT_GT(len, 0u);
  JsonDocument doc = parseValid(buf, len);
  EXPECT_TRUE(doc["wifi"]["connected"].is<bool>());
  EXPECT_TRUE(doc["wifi"]["saved"].is<bool>());
  EXPECT_TRUE(doc["wifi_connected"].is<bool>());
  EXPECT_TRUE(doc["wifi_saved"].is<bool>());
}

// The actual production call site always passes outBufLen == kMaxPayloadLen -- this
// is the case that must trim.
TEST(BuildDeviceInfoPayload, NeverExceeds160BytesAtRealBudgetConnected) {
  char buf[kMaxPayloadLen];
  const size_t len = buildDeviceInfoPayload(baseInput(), buf, kMaxPayloadLen);
  ASSERT_GT(len, 0u);
  EXPECT_LE(len, kMaxPayloadLen);
  JsonDocument doc = parseValid(buf, len);
  EXPECT_STREQ(doc["cmd"], "device.info");
}

TEST(BuildDeviceInfoPayload, NeverExceeds160BytesAtRealBudgetDisconnected) {
  DeviceInfoPayloadInput in = baseInput();
  in.wifiConnected = false;
  in.wifiSsid.clear();
  in.wifiRssi = 0;
  char buf[kMaxPayloadLen];
  const size_t len = buildDeviceInfoPayload(in, buf, kMaxPayloadLen);
  ASSERT_GT(len, 0u);
  EXPECT_LE(len, kMaxPayloadLen);
  parseValid(buf, len);
}

// This is the exact scenario CodeRabbit flagged: a budget tight enough that removing
// just ONE half of the connected pair (either wifi.connected or wifi_connected) would
// already fit, which is precisely what the old two-independent-`if`s code did wrong.
// We can't easily force that exact byte count from the real 160-byte constant with
// today's field sizes (confirmed today's real payload fits after firmware_version
// trimming alone), so this drives the budget down directly to probe the trimming
// logic's atomicity independent of what the current mandatory-field sizes happen to
// be -- verifying the *invariant* holds, not just today's specific measurements.
TEST(BuildDeviceInfoPayload, ConnectedPairNeverPartiallyPopulated) {
  DeviceInfoPayloadInput in = baseInput();
  in.wifiSsid = "AVeryLongNetworkNameThatTakesUpSpace";
  char buf[kMaxPayloadLen];
  // Sweep every budget from "everything fits" down to "almost nothing fits" and
  // check the invariant at each point, rather than guessing one magic byte count.
  for (size_t budget = kMaxPayloadLen; budget >= 40; --budget) {
    const size_t len = buildDeviceInfoPayload(in, buf, budget);
    if (len == 0) continue;  // budget too tight to produce anything -- fine
    JsonDocument doc = parseValid(buf, len);
    const bool nestedHasConnected = doc["wifi"].is<JsonObject>() && doc["wifi"]["connected"].is<bool>();
    const bool flatHasConnected = doc["wifi_connected"].is<bool>();
    EXPECT_EQ(nestedHasConnected, flatHasConnected)
        << "budget=" << budget << " nested/flat 'connected' pair is half-populated: " << std::string(buf, len);
  }
}

TEST(BuildDeviceInfoPayload, SavedPairNeverPartiallyPopulated) {
  DeviceInfoPayloadInput in = baseInput();
  in.wifiSsid = "AVeryLongNetworkNameThatTakesUpSpace";
  char buf[kMaxPayloadLen];
  for (size_t budget = kMaxPayloadLen; budget >= 40; --budget) {
    const size_t len = buildDeviceInfoPayload(in, buf, budget);
    if (len == 0) continue;
    JsonDocument doc = parseValid(buf, len);
    const bool nestedHasSaved = doc["wifi"].is<JsonObject>() && doc["wifi"]["saved"].is<bool>();
    const bool flatHasSaved = doc["wifi_saved"].is<bool>();
    EXPECT_EQ(nestedHasSaved, flatHasSaved)
        << "budget=" << budget << " nested/flat 'saved' pair is half-populated: " << std::string(buf, len);
  }
}

TEST(BuildDeviceInfoPayload, SsidRssiPairsNeverPartiallyPopulated) {
  DeviceInfoPayloadInput in = baseInput();
  in.wifiSsid = "AVeryLongNetworkNameThatTakesUpSpace";
  char buf[kMaxPayloadLen];
  for (size_t budget = kMaxPayloadLen; budget >= 40; --budget) {
    const size_t len = buildDeviceInfoPayload(in, buf, budget);
    if (len == 0) continue;
    JsonDocument doc = parseValid(buf, len);
    const bool nestedHasSsid = doc["wifi"].is<JsonObject>() && doc["wifi"]["ssid"].is<const char*>();
    const bool flatHasSsid = doc["wifi_ssid"].is<const char*>();
    EXPECT_EQ(nestedHasSsid, flatHasSsid)
        << "budget=" << budget << " nested/flat 'ssid' pair is half-populated: " << std::string(buf, len);

    const bool nestedHasRssi = doc["wifi"].is<JsonObject>() && doc["wifi"]["rssi"].is<int>();
    const bool flatHasRssi = doc["wifi_rssi"].is<int>();
    EXPECT_EQ(nestedHasRssi, flatHasRssi)
        << "budget=" << budget << " nested/flat 'rssi' pair is half-populated: " << std::string(buf, len);
  }
}

// "saved" is the most-protected field: it must survive trimming of the cosmetic
// ssid/rssi fields and of firmware_version, and only get dropped (if ever) after
// "connected" is already gone. Force this by giving firmware_version enough length
// to guarantee those earlier trims are exercised before "saved" is at risk.
TEST(BuildDeviceInfoPayload, SavedSurvivesAfterCosmeticAndFirmwareVersionTrim) {
  DeviceInfoPayloadInput in = baseInput();
  in.firmwareVersion = "1.2.3-a-much-longer-development-build-string-than-shipped-versions-use";
  char buf[kMaxPayloadLen];
  const size_t len = buildDeviceInfoPayload(in, buf, kMaxPayloadLen);
  ASSERT_GT(len, 0u);
  EXPECT_LE(len, kMaxPayloadLen);
  JsonDocument doc = parseValid(buf, len);
  // firmware_version must actually have been shortened (proves the trim ran) while
  // "saved" is still present somewhere (nested and/or flat).
  const std::string fw = doc["firmware_version"] | std::string();
  EXPECT_LT(fw.size(), in.firmwareVersion.size());
  const bool savedPresent =
      (doc["wifi"].is<JsonObject>() && doc["wifi"]["saved"].is<bool>()) || doc["wifi_saved"].is<bool>();
  EXPECT_TRUE(savedPresent);
}

// "connected" is dropped before "saved" when only one can survive.
TEST(BuildDeviceInfoPayload, ConnectedDroppedBeforeSavedUnderExtremePressure) {
  DeviceInfoPayloadInput in = baseInput();
  in.serial = "XTE-0011223344556677889900";  // deliberately oversized to force heavy trimming
  in.model = "Xteink X3 Extended Model Name For Testing";
  in.firmwareVersion = "1.2.3-a-much-longer-development-build-string-than-shipped-versions-use";
  char buf[kMaxPayloadLen];
  // Find a budget tight enough that at most one of connected/saved survives.
  for (size_t budget = kMaxPayloadLen; budget >= 40; --budget) {
    const size_t len = buildDeviceInfoPayload(in, buf, budget);
    if (len == 0) continue;
    JsonDocument doc = parseValid(buf, len);
    const bool hasConnected =
        (doc["wifi"].is<JsonObject>() && doc["wifi"]["connected"].is<bool>()) || doc["wifi_connected"].is<bool>();
    const bool hasSaved =
        (doc["wifi"].is<JsonObject>() && doc["wifi"]["saved"].is<bool>()) || doc["wifi_saved"].is<bool>();
    if (hasSaved && !hasConnected) {
      // Found the priority boundary: saved survived, connected didn't. This is the
      // required order -- test passes.
      SUCCEED();
      return;
    }
    ASSERT_FALSE(hasConnected && !hasSaved)
        << "budget=" << budget
        << " 'connected' survived while 'saved' was dropped -- wrong priority order: " << std::string(buf, len);
  }
}

TEST(BuildDeviceInfoPayload, TooTightToFitReturnsZeroNotTruncatedJson) {
  DeviceInfoPayloadInput in = baseInput();
  char buf[8];
  const size_t len = buildDeviceInfoPayload(in, buf, sizeof(buf));
  EXPECT_EQ(len, 0u);
}

}  // namespace
