#pragma once

#include <stddef.h>
#include <stdint.h>

// Routes BLE Command-characteristic writes to a handler by `cmd` string and produces
// the JSON reply -- see docs/ble-module-tasks.md's "Command protocol" and GATT layout
// sections. Sits above BlePeripheralManager (lib/hal/), which owns the actual GATT
// server/transport and knows nothing about command routing -- lib/hal/ never includes
// src/ headers in this codebase (see BlePeripheralManager.h), so this dispatcher is the
// src/-level code that drains BlePeripheral's pending-write handoff and pushes replies
// back through it. Call pump() once per main-loop iteration, alongside
// BlePeripheral.poll() -- see the Idle-state lifecycle wiring in ActivityManager.cpp.
//
// Phase 1 implements exactly one command, wifi.provision -- see
// docs/ble-module-tasks.md's "Suggested order". settings.push, book.fetch, and
// firmware.check/update are later phases; adding one is a new `cmd` string and a new
// handler function here, never a new characteristic (per the doc's own design goal).
namespace BleCommandDispatcher {

void pump();

}  // namespace BleCommandDispatcher
