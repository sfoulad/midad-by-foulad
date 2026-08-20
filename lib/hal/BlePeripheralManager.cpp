#include "BlePeripheralManager.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>

#include <cstring>

namespace {
constexpr char TAG[] = "BLE";

// Placeholders pending sign-off from the Midad phone side -- see
// docs/ble-module-tasks.md's GATT layout section and "Open questions to flag back".
constexpr char kServiceUuid[] = "fa01ade0-b1e0-4d1d-a000-000000000001";
constexpr char kAuthCharUuid[] = "fa01ade0-b1e0-4d1d-a000-000000000002";
constexpr char kCommandCharUuid[] = "fa01ade0-b1e0-4d1d-a000-000000000003";
constexpr char kStatusCharUuid[] = "fa01ade0-b1e0-4d1d-a000-000000000004";

// See docs/ble-module-tasks.md's GATT layout MTU note: BleKeyboardHost minimizes to
// 23 (HID reports are tiny); our JSON payloads need more room than that, but the full
// 517-byte NimBLE maximum reserves more per-connection GATT buffer than this chip can
// spare. 185 fits inside one radio packet on BLE 4.2+ controllers without needing
// multi-packet reassembly.
constexpr uint16_t kTargetMtu = 185;
// 0.625ms units (NimBLEAdvertising's own convention) -- 1600 * 0.625ms = 1.0s, matching
// the doc's "favor a longer interval, e.g. 1 s+" advertising-current target.
constexpr uint16_t kAdvIntervalUnits = 1600;

// Guards the pending Auth/Command write buffers, written from the NimBLE host task's
// characteristic callbacks and read from the main task's poll()/takePending*() --
// unlike `state_` (a coarse volatile flag), a torn copy here would corrupt a command.
portMUX_TYPE g_pendingMux = portMUX_INITIALIZER_UNLOCKED;

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_commandChar = nullptr;
NimBLECharacteristic* g_statusChar = nullptr;

// Writes the current state as a compact JSON status line into `outBuf` (fixed-size,
// caller-owned -- see docs/ble-module-tasks.md's "no heap churn on the command hot
// path" principle) and returns its length, or 0 if it didn't fit.
size_t formatStatusJson(BlePeripheralManager::State state, char* outBuf, size_t outBufLen) {
  const char* stateStr = "idle";
  switch (state) {
    case BlePeripheralManager::State::Off:
      stateStr = "idle";
      break;
    case BlePeripheralManager::State::Advertising:
      stateStr = "advertising";
      break;
    case BlePeripheralManager::State::Connected:
      stateStr = "connected";
      break;
    case BlePeripheralManager::State::PausedLowMemory:
      stateStr = "paused_low_memory";
      break;
  }
  const int written = snprintf(outBuf, outBufLen, "{\"state\":\"%s\"}", stateStr);
  return (written > 0 && static_cast<size_t>(written) < outBufLen) ? static_cast<size_t>(written) : 0;
}

void publishStatus() {
  if (!g_statusChar) return;
  char buf[64];
  const size_t len = formatStatusJson(BlePeripheral.state(), buf, sizeof(buf));
  if (len == 0) return;
  g_statusChar->setValue(reinterpret_cast<const uint8_t*>(buf), len);
  g_statusChar->notify();
}

// FE-P3-RC-BLE-PAIRING-001 diagnostic instrumentation: logs the full SMP/GAP security
// path NimBLEServerCallbacks exposes, to see what the pairing procedure actually does
// on the peripheral side rather than inferring it from a central-side ATT timeout.
// Read-only logging only -- no behavior changes. The passkey/confirm hooks below
// shouldn't ever fire with mitm=false (Just Works needs none of them); logging their
// invocation is itself diagnostic signal if they do. Each still calls through to the
// exact NimBLEServerCallbacks base-class default so behavior is unchanged either way.
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*pServer*/, NimBLEConnInfo& connInfo) override {
    LOG_DBG(TAG, "central connected: addr=%s connHandle=%u bondsBefore=%d", connInfo.getAddress().toString().c_str(),
            connInfo.getConnHandle(), NimBLEDevice::getNumBonds());
    BlePeripheral.onConnected();
    publishStatus();
  }
  void onDisconnect(NimBLEServer* /*pServer*/, NimBLEConnInfo& connInfo, int reason) override {
    LOG_DBG(TAG,
            "central disconnected, reason=%d: encrypted=%d authenticated=%d bonded=%d "
            "keySize=%u bondsAfter=%d",
            reason, connInfo.isEncrypted(), connInfo.isAuthenticated(), connInfo.isBonded(), connInfo.getSecKeySize(),
            NimBLEDevice::getNumBonds());
    BlePeripheral.onDisconnected();
    publishStatus();
  }
  // Fired on BLE_GAP_EVENT_ENC_CHANGE -- the actual pairing/encryption-establishment
  // outcome, success or failure. NimBLEConnInfo doesn't expose the raw SMP/HCI status
  // code (not surfaced by this library's public callback API); encrypted/authenticated/
  // bonded state here is the closest observable proxy for "did it work."
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    // NimBLE's public API doesn't expose a separate "was this SC (vs legacy) pairing"
    // flag; keySize==16 is a reasonable proxy (SC pairing always negotiates 128-bit
    // keys) but not a direct signal, so it's omitted here rather than faked.
    LOG_DBG(TAG,
            "auth complete: addr=%s connHandle=%u encrypted=%d authenticated=%d bonded=%d "
            "keySize=%u bondsAfter=%d",
            connInfo.getAddress().toString().c_str(), connInfo.getConnHandle(), connInfo.isEncrypted(),
            connInfo.isAuthenticated(), connInfo.isBonded(), connInfo.getSecKeySize(), NimBLEDevice::getNumBonds());
  }
  void onIdentity(NimBLEConnInfo& connInfo) override {
    LOG_DBG(TAG, "peer identity resolved: idAddr=%s connHandle=%u", connInfo.getIdAddress().toString().c_str(),
            connInfo.getConnHandle());
    NimBLEServerCallbacks::onIdentity(connInfo);
  }
  uint32_t onPassKeyDisplay() override {
    LOG_DBG(TAG, "onPassKeyDisplay invoked (unexpected with mitm=false)");
    return NimBLEServerCallbacks::onPassKeyDisplay();
  }
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    LOG_DBG(TAG, "onPassKeyEntry invoked (unexpected with mitm=false)");
    NimBLEServerCallbacks::onPassKeyEntry(connInfo);
  }
  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    LOG_DBG(TAG, "onConfirmPassKey invoked (unexpected with mitm=false)");
    NimBLEServerCallbacks::onConfirmPassKey(connInfo, pin);
  }
};

class AuthCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    const NimBLEAttValue value = pCharacteristic->getValue();
    LOG_DBG(TAG, "auth write, %u bytes: encrypted=%d authenticated=%d bonded=%d", static_cast<unsigned>(value.size()),
            connInfo.isEncrypted(), connInfo.isAuthenticated(), connInfo.isBonded());
    BlePeripheral.onAuthWritten(value.data(), value.size());
  }
};

class CommandCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& /*connInfo*/) override {
    const NimBLEAttValue value = pCharacteristic->getValue();
    LOG_DBG(TAG, "command write, %u bytes", static_cast<unsigned>(value.size()));
    BlePeripheral.onCommandWritten(value.data(), value.size());
  }
};

ServerCallbacks g_serverCallbacks;
AuthCharCallbacks g_authCallbacks;
CommandCharCallbacks g_commandCallbacks;
}  // namespace

BlePeripheralManager& BlePeripheralManager::getInstance() {
  static BlePeripheralManager instance;
  return instance;
}

void BlePeripheralManager::getAdvertisedName(char* outBuf, size_t outBufLen) {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  snprintf(outBuf, outBufLen, "Midad-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

int BlePeripheralManager::getBondCount() {
  // NimBLEDevice::init() sets up the underlying NimBLE host task and its port-layer
  // mutexes (including the ones the NVS-backed bond store depends on) -- before that's
  // ever run this boot (fresh boot, BluetoothActivity never entered yet),
  // getNumBonds()'s call chain dereferences a mutex handle that doesn't exist yet and
  // crashes (confirmed on hardware: "assert failed: npl_freertos_mutex_pend ...
  // (mu->handle)"). end() calls deinit(true), which clears m_initialized right back to
  // false, so this same guard also covers "BLE was on, user left the screen" -- not
  // just "never started this boot." Correct outcome either way: no bond count is
  // meaningfully available when the host stack isn't currently running.
  if (!NimBLEDevice::isInitialized()) return 0;
  return NimBLEDevice::getNumBonds();
}

bool BlePeripheralManager::clearAllBonds() {
  if (!NimBLEDevice::isInitialized()) return false;

  // ble_gap_unpair() (what deleteAllBonds() calls per-bond) refuses to remove a bond
  // that distributed an IRK while advertising is active -- BLE_HS_EBUSY, since it can't
  // safely drop the peer from the controller's resolving list mid-advertisement. Any
  // bond formed under this device's old mitm=true config did distribute an IRK, so
  // clearing needs advertising paused for the duration.
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  const bool wasAdvertising = advertising != nullptr && advertising->isAdvertising();
  if (wasAdvertising) advertising->stop();

  const bool cleared = NimBLEDevice::deleteAllBonds();

  if (wasAdvertising) advertising->start();
  return cleared;
}

bool BlePeripheralManager::begin() {
  // Only Advertising/Connected are genuinely "already running, no-op". PausedLowMemory
  // used to be folded into this same early return (state_ != State::Off), which meant
  // once poll() paused for low memory, begin() could never retry again -- no matter how
  // long the cool-down ran or how much heap recovered. Off and PausedLowMemory both
  // fall through to the cool-down + heap-gate checks below instead, so a genuine retry
  // can actually happen once both clear.
  if (state_ == State::Advertising || state_ == State::Connected) return true;

  if (coolingDown_) {
    if (millis() - lastLowMemoryTeardownMs_ < kCooldownMs) {
      LOG_DBG(TAG, "begin() refused: cool-down active");
      return false;
    }
    coolingDown_ = false;
  }

  if (ESP.getFreeHeap() < kHeapGateBytes) {
    LOG_DBG(TAG, "begin() refused: heap gate not met (free=%u < %u)", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(kHeapGateBytes));
    return false;
  }

  // NimBLE init/deinit needs normal CPU frequency -- see docs/ble-module-tasks.md's
  // BleKeyboardHost::end() finding ("Must run at normal CPU frequency (controller
  // deinit)"). Scoped to just the init sequence below, not the whole advertising
  // period -- that would defeat Idle power-saving for however long BLE just sits there.
  HalPowerManager::Lock powerLock;

  // Self-heal a partial teardown, same defensive pattern BleKeyboardHost::begin() uses:
  // if a previous deinit raced and didn't complete, init() would no-op and hand back a
  // dead stack.
  if (NimBLEDevice::isInitialized()) {
    LOG_DBG(TAG, "begin(): NimBLE was already initialized; forcing deinit first");
    NimBLEDevice::deinit(true);
  }

  char advertisedName[24];
  getAdvertisedName(advertisedName, sizeof(advertisedName));

  if (!NimBLEDevice::init(advertisedName)) {
    LOG_ERR(TAG, "begin(): NimBLEDevice::init() failed");
    return false;
  }
  NimBLEDevice::setMTU(kTargetMtu);
  // Explicit TX power (+9 dBm), not left at whatever default the controller happens to
  // pick: an earlier hardware session found the controller reporting advertising-active
  // while two independent scanners at desk range saw nothing over the air with no
  // explicit setPower() call. NimBLE-Arduino's setPower(int8_t dbm, ...) is the current
  // API surface (h2zero/NimBLE-Arduino ^2.3.8) -- not the esp_power_level_t-enum-based
  // setPower(ESP_PWR_LVL_P9) shape an earlier, now-superseded version of this fix used.
  NimBLEDevice::setPower(9);

  g_server = NimBLEDevice::createServer();
  if (!g_server) {
    LOG_ERR(TAG, "begin(): createServer() returned null");
    NimBLEDevice::deinit(true);
    return false;
  }
  g_server->setCallbacks(&g_serverCallbacks, /*deleteCallbacks=*/false);
  // Resume advertising automatically after a disconnect, so a second phone (or the
  // same one reconnecting) can find the device without this class re-driving it.
  g_server->advertiseOnDisconnect(true);

  NimBLEService* service = g_server->createService(kServiceUuid);
  if (!service) {
    LOG_ERR(TAG, "begin(): createService() returned null");
    NimBLEDevice::deinit(true);
    g_server = nullptr;
    return false;
  }

  // WRITE | WRITE_ENC: WRITE_ENC alone is only the requires-encryption flag -- without
  // the plain WRITE property bit the characteristic declaration advertises no write
  // support at all, and a real phone BLE stack refuses to even attempt a write ("The
  // WRITE property is not supported by this BLE characteristic", confirmed against a
  // real client on an earlier hardware session). Encryption is still fully enforced:
  // WRITE_ENC gates the write on an encrypted link per docs/ble-module-tasks.md's
  // Security section -- this only adds the missing "writable at all" declaration.
  NimBLECharacteristic* authChar =
      service->createCharacteristic(kAuthCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  authChar->setCallbacks(&g_authCallbacks);

  g_commandChar = service->createCharacteristic(kCommandCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  g_commandChar->setCallbacks(&g_commandCallbacks);

  g_statusChar = service->createCharacteristic(kStatusCharUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  service->start();

  // mitm=false: this hardware has no display/keyboard (BLE_HS_IO_NO_INPUT_OUTPUT), so
  // it cannot perform passkey entry or numeric comparison. Requesting mitm=true with
  // NoInputNoOutput is unsatisfiable per the BLE spec's pairing method selection --
  // confirmed on real hardware to leave the Auth characteristic's encrypted write
  // hanging until the central times out ("Encryption is insufficient"). bonding and sc
  // stay on: the link is still encrypted (LE Secure Connections) and bonded, just via
  // unauthenticated Just Works, which is standard for headless IoT peripherals.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  // Scan response, NOT the primary advertising payload: the 128-bit service UUID
  // above already consumes 18 of the primary payload's 31-byte legacy AD budget: the
  // per-device name ("Midad-XXXXXX", up to 14 bytes with its AD header) doesn't fit
  // alongside it. NimBLEAdvertisementData::addData() silently no-ops when a field
  // doesn't fit rather than failing loudly, so without this the name was verified
  // live to never reach the actual over-the-air advertisement at all (confirmed via
  // a real scan: local_name absent from the raw AD payload) -- any name a scanner
  // showed was a stale OS-level cache from an earlier static-name advertisement, not
  // a live read. Scan response is a second, separate 31-byte payload sent in reply to
  // an active scan request, with its own budget -- enabling it and letting setName()
  // land there (its own fallback order, see NimBLEAdvertising::setName()) fits both.
  advertising->enableScanResponse(true);
  advertising->setName(advertisedName);
  advertising->setMinInterval(kAdvIntervalUnits);
  advertising->setMaxInterval(kAdvIntervalUnits);
  // start() has real failure paths (GATT server start, adv-data set, ble_gap_adv_start
  // rc) -- ignoring its result is how this class could sit in Advertising state with a
  // silent radio for a whole session. Fail begin() honestly instead, tearing everything
  // back down rather than leaving state_ claiming a radio that isn't actually running.
  if (!advertising->start() || !advertising->isAdvertising()) {
    LOG_ERR(TAG, "begin(): advertising failed to start (free heap=%u)", static_cast<unsigned>(ESP.getFreeHeap()));
    NimBLEDevice::deinit(true);
    g_server = nullptr;
    g_commandChar = nullptr;
    g_statusChar = nullptr;
    return false;
  }

  state_ = State::Advertising;
  publishStatus();
  LOG_DBG(TAG, "begin(): advertising (free heap=%u)", static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

void BlePeripheralManager::end() {
  if (state_ != State::Advertising && state_ != State::Connected) return;  // nothing to tear down

  HalPowerManager::Lock powerLock;  // see begin()'s comment on this

  if (NimBLEDevice::getAdvertising()) NimBLEDevice::getAdvertising()->stop();
  if (g_server && g_server->getConnectedCount() > 0) {
    const auto peers = g_server->getPeerDevices();
    for (const uint16_t connHandle : peers) {
      g_server->disconnect(connHandle);
    }
  }
  NimBLEDevice::deinit(true);
  g_server = nullptr;
  g_commandChar = nullptr;
  g_statusChar = nullptr;

  state_ = State::Off;
  LOG_DBG(TAG, "end(): torn down (free heap=%u)", static_cast<unsigned>(ESP.getFreeHeap()));
}

void BlePeripheralManager::poll() {
  if (state_ != State::Advertising && state_ != State::Connected) return;

  // Zombie guard: state says Advertising but the controller isn't actually
  // transmitting. Tear down so the caller's normal begin() cadence brings it back for
  // real; no cool-down, this isn't memory pressure.
  //
  // MUST be lazy about it: adv-inactive is also the perfectly normal signature of a
  // central mid-connect (the controller stops advertising the instant it accepts
  // CONNECT_IND, milliseconds before the host delivers the connect event that moves
  // state_ to Connected). An eager, no-tolerance version of this check would kill a
  // real inbound connection mid-handshake. So: never a zombie if a connection exists,
  // and the radio-idle condition must persist kZombiePersistMs before teardown.
  if (state_ == State::Advertising) {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    const bool radioIdle = advertising && !advertising->isAdvertising();
    const bool hasConnection = g_server && g_server->getConnectedCount() > 0;
    if (radioIdle && !hasConnection) {
      if (zombieSinceMs_ == 0) {
        zombieSinceMs_ = millis();
      } else if (millis() - zombieSinceMs_ >= kZombiePersistMs) {
        LOG_ERR(TAG, "poll(): zombie state -- Advertising but radio idle for %lus, tearing down for retry",
                static_cast<unsigned long>(kZombiePersistMs / 1000));
        zombieSinceMs_ = 0;
        end();
        return;
      }
    } else {
      zombieSinceMs_ = 0;
    }
  }

  // kRunningFloorBytes, NOT kHeapGateBytes: init already spent kHeapGateBytes-and-then-
  // some, so judging a running session against the pre-flight number tears down every
  // successful start on its very next poll().
  if (ESP.getFreeHeap() < kRunningFloorBytes) {
    LOG_ERR(TAG, "poll(): heap dropped below running floor (free=%u), tearing down",
            static_cast<unsigned>(ESP.getFreeHeap()));
    end();
    state_ = State::PausedLowMemory;
    coolingDown_ = true;
    lastLowMemoryTeardownMs_ = millis();
  }
}

void BlePeripheralManager::onConnected() {
  if (state_ == State::Advertising) state_ = State::Connected;
}

void BlePeripheralManager::onDisconnected() {
  if (state_ == State::Connected) state_ = State::Advertising;
}

void BlePeripheralManager::onAuthWritten(const uint8_t* data, size_t len) {
  const size_t copyLen = len < kMaxPayloadLen ? len : kMaxPayloadLen;
  taskENTER_CRITICAL(&g_pendingMux);
  memcpy(pendingAuth_, data, copyLen);
  pendingAuthLen_ = copyLen;
  pendingAuthReady_ = true;
  taskEXIT_CRITICAL(&g_pendingMux);
}

void BlePeripheralManager::onCommandWritten(const uint8_t* data, size_t len) {
  const size_t copyLen = len < kMaxPayloadLen ? len : kMaxPayloadLen;
  taskENTER_CRITICAL(&g_pendingMux);
  memcpy(pendingCommand_, data, copyLen);
  pendingCommandLen_ = copyLen;
  pendingCommandReady_ = true;
  taskEXIT_CRITICAL(&g_pendingMux);
}

bool BlePeripheralManager::takePendingAuth(uint8_t* outBuf, size_t maxLen, size_t& outLen) {
  bool ready;
  taskENTER_CRITICAL(&g_pendingMux);
  ready = pendingAuthReady_;
  if (ready) {
    outLen = pendingAuthLen_ < maxLen ? pendingAuthLen_ : maxLen;
    memcpy(outBuf, pendingAuth_, outLen);
    pendingAuthReady_ = false;
  }
  taskEXIT_CRITICAL(&g_pendingMux);
  return ready;
}

bool BlePeripheralManager::takePendingCommand(uint8_t* outBuf, size_t maxLen, size_t& outLen) {
  bool ready;
  taskENTER_CRITICAL(&g_pendingMux);
  ready = pendingCommandReady_;
  if (ready) {
    outLen = pendingCommandLen_ < maxLen ? pendingCommandLen_ : maxLen;
    memcpy(outBuf, pendingCommand_, outLen);
    pendingCommandReady_ = false;
  }
  taskEXIT_CRITICAL(&g_pendingMux);
  return ready;
}

bool BlePeripheralManager::sendCommandReply(const uint8_t* data, size_t len) {
  if (!g_commandChar || state_ != State::Connected) return false;
  g_commandChar->setValue(data, len);
  return g_commandChar->notify();
}
