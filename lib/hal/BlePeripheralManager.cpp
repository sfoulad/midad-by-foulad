#include "BlePeripheralManager.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <NimBLEDevice.h>

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

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*pServer*/, NimBLEConnInfo& /*connInfo*/) override {
    LOG_DBG(TAG, "central connected");
    BlePeripheral.onConnected();
    publishStatus();
  }
  void onDisconnect(NimBLEServer* /*pServer*/, NimBLEConnInfo& /*connInfo*/, int reason) override {
    LOG_DBG(TAG, "central disconnected, reason=%d", reason);
    BlePeripheral.onDisconnected();
    publishStatus();
  }
};

class AuthCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& /*connInfo*/) override {
    const NimBLEAttValue value = pCharacteristic->getValue();
    LOG_DBG(TAG, "auth write, %u bytes", static_cast<unsigned>(value.size()));
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

bool BlePeripheralManager::begin() {
  // Real-device bug, found live: this used to be `if (state_ != State::Off) return
  // true;`, which treated PausedLowMemory the same as "already active" and returned
  // early without ever re-attempting -- so once poll() paused for low memory, begin()
  // could NEVER retry again, no matter how long the cool-down ran or how much heap
  // recovered. Only Advertising/Connected are genuinely "already running, no-op";
  // PausedLowMemory needs to fall through to the cool-down + heap-gate checks below,
  // same as a fresh Off, so a real retry can actually happen once both clear.
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

  if (!NimBLEDevice::init("Midad")) {
    LOG_ERR(TAG, "begin(): NimBLEDevice::init() failed");
    return false;
  }
  NimBLEDevice::setMTU(kTargetMtu);
  // Explicit TX power, same as crosspoint-reader-ble's enable() (+9 dBm). Do not rely
  // on the default: live-debugged 2026-08-10 -- with no setPower() call the controller
  // reported advertising-active while two independent scanners at desk range saw
  // nothing over the air.
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

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

  // WRITE_ENC: requires an encrypted link before a write is accepted -- see
  // docs/ble-module-tasks.md's Security section (LE Secure Connections, not Just
  // Works, since this exchange needs to resist a passive eavesdropper).
  NimBLECharacteristic* authChar = service->createCharacteristic(kAuthCharUuid, NIMBLE_PROPERTY::WRITE_ENC);
  authChar->setCallbacks(&g_authCallbacks);

  g_commandChar = service->createCharacteristic(kCommandCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  g_commandChar->setCallbacks(&g_commandCallbacks);

  g_statusChar = service->createCharacteristic(kStatusCharUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  service->start();

  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/true, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setName("Midad");
  advertising->setMinInterval(kAdvIntervalUnits);
  advertising->setMaxInterval(kAdvIntervalUnits);
  // start() has real failure paths (GATT server start, adv-data set, ble_gap_adv_start
  // rc) -- ignoring its result is how this class once sat in Advertising state with a
  // silent radio for a whole session (live-debugged 2026-08-10: phone and a Mac-side
  // scanner both saw nothing while bleState said 1). Fail begin() honestly instead.
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
  // transmitting (seen live 2026-08-10 -- phone + Mac scanners both blind while
  // bleState claimed Advertising). Tear down so the caller's normal begin() cadence
  // brings it back for real; no cool-down, this isn't memory pressure.
  //
  // MUST be lazy about it: adv-inactive is also the perfectly normal signature of a
  // central mid-connect (the controller stops advertising the instant it accepts
  // CONNECT_IND, milliseconds before the host delivers the connect event that moves
  // state_ to Connected). First version of this guard had no tolerance and killed a
  // real inbound connection mid-handshake, live-debugged 2026-08-10 (NimBLE:
  // "ble_hs_stop_terminate_timeout_cb, 1 connection(s) still up"). So: never a
  // zombie if a connection exists, and the radio-idle condition must persist
  // kZombiePersistMs before teardown.
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

  // kRunningFloorBytes, NOT kHeapGateBytes: init already spent its ~65 KB, so
  // judging a running session against the pre-flight number tears down every
  // successful start on the next poll -- the exact live-debugged failure in
  // docs/ble-module-tasks.md's second session.
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
