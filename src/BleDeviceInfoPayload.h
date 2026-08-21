#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Hardware-independent half of device.info's reply: given already-resolved field
// values, builds and budget-trims the JSON payload. Split out from
// BleCommandDispatcher.cpp (which gathers the values from eFuse/HalGPIO/
// BleWifiAutoConnectCache) so this trimming logic -- the part that has twice shipped
// a real bug (dropping "saved" first, then leaving nested/flat pairs half-populated)
// -- can be exercised by host-side unit tests without any ESP-IDF dependency. See
// test/ble_device_info_payload/.
namespace BleCommandDispatcher {

struct DeviceInfoPayloadInput {
  std::string serial;
  std::string model;
  std::string firmwareVersion;
  bool claimed = false;
  bool wifiSaved = false;
  bool wifiConnected = false;
  std::string wifiSsid;
  int32_t wifiRssi = 0;
};

// Writes the device.info JSON reply into outBuf (capacity outBufLen) and returns the
// serialized length, or 0 if it could not be made to fit even after trimming
// everything trimmable. Never writes a truncated/unparseable prefix -- see the
// priority-trim comment in the .cpp for why serializeJson() alone can't be trusted to
// respect outBufLen.
size_t buildDeviceInfoPayload(const DeviceInfoPayloadInput& in, char* outBuf, size_t outBufLen);

}  // namespace BleCommandDispatcher
