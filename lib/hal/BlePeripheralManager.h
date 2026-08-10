#pragma once

#include <stddef.h>
#include <stdint.h>

// Midad BLE peripheral: GATT server for phone control (Wi-Fi provisioning today;
// settings push, book-fetch, and firmware-update trigger in later phases -- see
// docs/ble-module-tasks.md). Opposite BLE role from freeink-sdk's BleKeyboardHost
// (central, HID page-turner remotes) -- the two are mutually exclusive at runtime on
// this chip (see the doc's Hardware reality section) but this class owns none of that
// arbitration itself; the caller decides when begin()/end() run (see the doc's device
// state machine: Idle -> BLE-peripheral, Wi-Fi active / Reading -> BLE off here).
//
// Memory: begin() refuses to start below the doc's Idle-state heap gate (~70 KB free)
// and enters a 30 s cool-down after any teardown poll() triggers for low memory, so a
// caller retrying begin() naively every loop tick can't spin the radio on/off (see the
// doc's "endless BT Connecting..." field-crash finding). NimBLE's own resident cost
// (~57 KB) is fixed and unavoidable; this class's own bookkeeping is a handful of
// scalars plus two small fixed buffers below, no heap allocation of its own.
//
// This header is deliberately NimBLE-free (mirrors BleKeyboardHost's own convention)
// so non-BLE callers (AppsActivity, status-bar rendering) don't pull in the BLE stack
// just to check State. lib/hal/ never includes src/ headers in this codebase (checked:
// no existing lib/*.cpp does) -- so command routing is NOT a direct call into a
// dispatcher. It's a poll/drain handoff instead, the same shape BleKeyboardHost already
// uses for its own NimBLE-host-task -> main-task boundary (there: a ring of decoded key
// events drained by popKey(); here: a single pending write drained by
// takePendingCommand()/takePendingAuth()). The src/-level caller that drives poll() is
// expected to drain these every tick and route them through the command dispatcher
// (src/BleCommandDispatcher.h), then push any reply back via sendCommandReply().
class BlePeripheralManager {
 public:
  enum class State : uint8_t {
    Off,              // NimBLE not initialized. Default, and where every teardown lands.
    Advertising,      // NimBLE up, advertising, no central connected yet.
    Connected,        // A phone is connected.
    PausedLowMemory,  // poll() tore this down because free heap dropped below the gate
                      // while running. Shown honestly in the UI rather than a silent
                      // drop -- see docs/ble-module-tasks.md's PAUSED state design.
  };

  // ~70 KB Idle-state PRE-FLIGHT gate from docs/ble-module-tasks.md's Hardware reality
  // section. Measured live 2026-08-10: NimBLE init itself consumes ~65 KB of heap
  // (77-80 KB free before begin() -> 12.7 KB free after), so this number is what
  // begin() needs to see BEFORE init for the device to still have a workable floor
  // left AFTER init. It is deliberately NOT the while-running floor -- using one
  // number for both jobs was the bug that made every successful start tear itself
  // down on the very next poll() (see the doc's second live-debug session).
  static constexpr uint32_t kHeapGateBytes = 70 * 1024;
  // While-RUNNING floor for poll(): once NimBLE is resident its ~65 KB is already
  // spent, and the question changes from "can we afford to start?" to "is the rest
  // of the system being starved?". Measured live: a healthy just-started session
  // sits at ~12.7 KB free, so the floor must be below that or nothing survives.
  // 8 KB leaves the Idle screens' small allocations (repaints, input, status JSON)
  // working while still catching a real downward spiral before malloc failures
  // cascade. Reading state never sees this floor -- the lifecycle in main.cpp tears
  // BLE down before the reader opens.
  static constexpr uint32_t kRunningFloorBytes = 8 * 1024;
  // Cool-down after a low-memory teardown, so a caller retrying begin() can't spin the
  // radio on/off (PR #2527's field-crash finding, see the doc).
  static constexpr uint32_t kCooldownMs = 30000;
  // How long poll()'s zombie check (state Advertising, radio idle, no connection)
  // must persist before teardown. Must comfortably exceed the controller-accepts-
  // CONNECT_IND -> host-delivers-connect-event window, or the guard kills real
  // inbound connections mid-handshake (live-debugged 2026-08-10).
  static constexpr uint32_t kZombiePersistMs = 3000;
  // JSON payload budget for the Auth/Command/Status characteristics -- see the doc's
  // GATT layout MTU note (185-byte target MTU, minus ATT header and JSON framing
  // overhead). A write larger than this is truncated at the GATT layer (NimBLE's own
  // characteristic value already capped by the negotiated MTU) before it ever reaches
  // these fixed buffers, so there's no overflow risk from a misbehaving/oversized
  // remote write -- see onAuthWritten()/onCommandWritten().
  static constexpr size_t kMaxPayloadLen = 160;

  static BlePeripheralManager& getInstance();

  // Starts advertising if the heap gate and cool-down both allow it. Returns false
  // (and logs why) otherwise -- the caller must not retry in a tight loop; back off and
  // let the caller's own poll cadence try again later. Safe to call when already
  // running (no-op, returns true). Wraps NimBLE init in a HalPowerManager::Lock (NimBLE
  // init/deinit needs normal CPU frequency -- see the doc's BleKeyboardHost finding).
  bool begin();

  // Tears down the NimBLE stack, freeing its RAM back to the heap. Safe to call when
  // not running (no-op). Does NOT start the low-memory cool-down -- that only applies
  // when poll() itself detects and reacts to memory pressure, not an ordinary caller-
  // requested stop (e.g. stepping into Wi-Fi-active state).
  void end();

  // Pump per main-loop iteration: while running, checks the heap gate and tears down
  // (entering PausedLowMemory + the cool-down) if it's been breached. Cheap; never
  // blocks. Recovery is NOT this method's job -- the caller's own periodic begin()
  // calls naturally succeed again once the gate and cool-down both clear. Callers
  // should also drain takePendingAuth()/takePendingCommand() at the same cadence.
  void poll();

  State state() const { return state_; }
  bool isActive() const { return state_ == State::Advertising || state_ == State::Connected; }

  // Copies the most recent Auth-characteristic write into `outBuf` (capacity
  // `maxLen`), sets `outLen`, and returns true -- or returns false if nothing is
  // pending. A write that arrives before the previous one is drained overwrites it
  // (GATT writes here are request/response, not a queue -- Phase 1 has exactly one
  // outstanding provisioning attempt at a time).
  bool takePendingAuth(uint8_t* outBuf, size_t maxLen, size_t& outLen);
  // Same shape, for the Command characteristic.
  bool takePendingCommand(uint8_t* outBuf, size_t maxLen, size_t& outLen);
  // Sends `data`/`len` back over the Command characteristic's Notify property (the
  // dispatcher's JSON reply -- state/progress/failure, per the doc's command
  // protocol). No-op (returns false) if nothing is connected to notify.
  bool sendCommandReply(const uint8_t* data, size_t len);

  // --- Internal: called by the NimBLE backend (not for app use). Keeps this header
  // free of NimBLE types, the same convention BleKeyboardHost uses. NimBLE callbacks
  // run on the NimBLE host task, not the main/render task; onAuthWritten()/
  // onCommandWritten() take a spinlock around the fixed pending-write buffers since
  // those are genuinely correctness-critical (a torn copy would corrupt a command),
  // unlike `state_` below which is a coarse volatile flag for UI/log decisions only,
  // matching BleKeyboardHost's own split between its spinlock-guarded key ring and its
  // plain volatile connection-state flags. ------------------------------------------
  void onConnected();
  void onDisconnected();
  void onAuthWritten(const uint8_t* data, size_t len);
  void onCommandWritten(const uint8_t* data, size_t len);

 private:
  BlePeripheralManager() = default;
  BlePeripheralManager(const BlePeripheralManager&) = delete;
  BlePeripheralManager& operator=(const BlePeripheralManager&) = delete;

  volatile State state_ = State::Off;
  uint32_t lastLowMemoryTeardownMs_ = 0;
  bool coolingDown_ = false;
  // millis() when poll() first saw the zombie signature; 0 = not currently seen.
  unsigned long zombieSinceMs_ = 0;

  uint8_t pendingAuth_[kMaxPayloadLen] = {};
  size_t pendingAuthLen_ = 0;
  bool pendingAuthReady_ = false;

  uint8_t pendingCommand_[kMaxPayloadLen] = {};
  size_t pendingCommandLen_ = 0;
  bool pendingCommandReady_ = false;
};

#define BlePeripheral BlePeripheralManager::getInstance()
