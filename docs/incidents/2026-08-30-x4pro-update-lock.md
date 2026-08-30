# Incident: X4 Pro update lock

**Date:** 2026-08-30
**Severity:** Critical — the owner's only working X4 Pro cannot install firmware through any channel.
**Status:** Root cause identified and fixed in code; the fix is undeliverable to the affected device.

Unless stated otherwise, source citations are against `a94d3d31` (the firmware image currently
running on the device). `src/network/FirmwareFlasher.cpp` is byte-identical between `a94d3d31`
and the `release/x4pro-convergence-rc` tip, so its line numbers hold on both.

---

## Summary

An X4 Pro is update-locked. It reads books normally, but every firmware installation path
refuses every image — including an image byte-identical to the firmware already running.

The cause is a pre-flight chip-identity check that derives the *device's* identity by reading
bytes out of SPI flash rather than from the build target. On this device that read returns a
value that is neither the correct `0x0009` (ESP32-S3) nor the `0xFFFF` "unknown" sentinel, and
both install paths compare against it and fail **closed**.

This was a release-process failure, not a user error and not bad luck. Two specific process
decisions caused it:

1. **Upstream code that changes the installation path was adopted into a hardware bring-up
   branch without ever being validated on that hardware.** The guard was written for boards
   that already existed; X4 Pro support arrived in this tree *after* it, and nothing re-checked
   the guard against the new board.
2. **An updater change rode along in a UI-focused RC.** The X4 Pro convergence RC is a
   touch/UI convergence branch, and it carries commits that modify the network OTA path. It was
   handed to the owner without a two-slot install-survivability test, because nobody classified
   it as an RC that could affect updating at all.

A test that would have caught this — install, reboot, install again from the other slot — did
not exist. It exists now only as a mandate.

---

## Timeline

| When | What |
| --- | --- |
| 2026-08-07 | Upstream CrossPoint lands `e00f5958` "feat: guard against cross-chip firmware installs" (PR #2880, v1.5.0), introducing `runningPartitionChipId()`. |
| 2026-08-14 | The guard enters this tree via the upstream sync merge `8285959a` (CrossPoint develop @ `48e39eb0`). No X4 Pro exists in the tree yet. |
| 2026-08-15 | `bbca4886` "feat: Add support for x4pro & papermono devices (#2983)" adds the X4 Pro board. The chip guard is already an ancestor at this point and is not re-examined against the new board. |
| 2026-08-22 | Sync merge `e56da65e` carries the same guard forward unchanged. |
| — | The X4 Pro convergence RC is assembled: predominantly touch/UI work, but it also carries `b5e1be55` ("OTA update-offer correctness and touch-operable update screens") and `fef9ed0e` ("verify OTA TLS against pinned GitHub trust anchors over wolfSSL"), both of which modify `src/network/OtaUpdater.cpp`. |
| — | The RC is handed to the owner. No two-slot install matrix is run. |
| — | The device, running from **app1**, installs the RC successfully into **app0**. `/debug_log.txt` records `dest=app0 size=4958448` — the format emitted at `src/network/FirmwareFlasher.cpp:370`. |
| — | Running from app0, the device refuses every subsequent image on every channel. |
| 2026-08-30 | Root cause identified. Fix raised upstream as CrossPoint PR #3311; a local bridge build (`bridge/x4pro-3311` @ `018cc72b`) is produced and verified, and found to be undeliverable. |

---

## Root cause

`firmware_flash::runningPartitionChipId()` — `src/network/FirmwareFlasher.cpp:73`:

```cpp
uint16_t runningPartitionChipId() {
  static uint16_t cached = [] {
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (!run) return static_cast<uint16_t>(0xFFFF);
    uint16_t id = 0xFFFF;
    if (esp_partition_read(run, 12, &id, sizeof(id)) != ESP_OK) return static_cast<uint16_t>(0xFFFF);
    return id;
  }();
  return cached;
}
```

The device's own chip identity is inferred by reading two bytes at offset 12 of the **running app
partition's image header in SPI flash** (`FirmwareFlasher.cpp:83`), memoised in a function-local
`static`. On this device that read yields a value that is neither `0x0009` nor `0xFFFF`.

The design error is structural, independent of why the read misbehaves: the device's identity is a
property of the build target, and it is being recovered from mutable storage. The only escape
hatch is `0xFFFF`, and a value that is merely *wrong* looks exactly like a valid identity to both
callers.

Both install channels then compare against it and fail closed:

- **SD card** — `src/network/FirmwareFlasher.cpp:147-153`:

  ```cpp
  uint16_t imageChip;
  std::memcpy(&imageChip, header + 12, sizeof(imageChip));
  const uint16_t deviceChip = runningPartitionChipId();
  if (deviceChip != 0xFFFF && imageChip != deviceChip) {
    LOG_ERR("FLASH", "validate: wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
    file.close();
    return Result::BAD_CHIP;
  }
  ```

- **Network OTA** — `src/network/OtaUpdater.cpp:262-265`, the same comparison against the same
  memoised value, inside the download callback.

Because the correct image chip id for this board is `0x0009` and `deviceChip` is neither that nor
the sentinel, the predicate is true for every well-formed X4 Pro image. The device rejects the
firmware it is already running.

---

## Provenance

The defect is upstream CrossPoint's, introduced whole in `e00f5958` ("feat: guard against
cross-chip firmware installs", PR #2880, v1.5.0, Uri Tauber, 2026-08-07).

The function body is byte-identical at every point in the chain. SHA-256 of the extracted
`runningPartitionChipId()` body:

| Ref | Body SHA-256 |
| --- | --- |
| `e00f5958` (upstream, introduction) | `daf5d4b3f0d027f75bdcf6d8684c7063670872b4f80b4321ecd50e98427aad61` |
| `crosspoint-reader/develop@790a0817` | `daf5d4b3f0d027f75bdcf6d8684c7063670872b4f80b4321ecd50e98427aad61` |
| `a94d3d31` (the shipped image) | `daf5d4b3f0d027f75bdcf6d8684c7063670872b4f80b4321ecd50e98427aad61` |
| `release/x4pro-convergence-rc` | `daf5d4b3f0d027f75bdcf6d8684c7063670872b4f80b4321ecd50e98427aad61` |

The entire downstream delta on `src/network/FirmwareFlasher.cpp` between `790a0817` and the RC is
`FirmwareDiagLog::append(...)` calls plus the `#include "util/FirmwareDiagLog.h"` they need. Not
one line of the guard's logic is ours.

That is the point, and it is the finding, not the excuse. **This was adopted from upstream without
hardware validation on X4 Pro.** A thin fork inherits upstream's code *and* upstream's validation
gaps; upstream had no X4 Pro to test the guard against when it was written — `e00f5958` predates
X4 Pro board support in this tree by eight days — and neither the sync that carried it in nor the
board-support commit that followed re-checked it. Adopting a change to the installation path is
exactly the case where "upstream tested it" is not a substitute for testing it here.

---

## Why the existing checks missed it

Everything the project runs, passed:

| Check | Where | Why it could not catch this |
| --- | --- | --- |
| Four board builds (`default`, `sticky`, `x4pro`, `papermono`) | `.github/workflows/ci.yml:104-110` | Compiles the code; never executes it. |
| Host unit tests (ctest over the `test/` suites) | `.github/workflows/ci.yml:169-196` | 24 suites, none covering the updater's identity logic. At the time of the incident, the chip comparison had no host-testable seam at all — it was hand-inlined into both install paths. |
| `cppcheck` | `.github/workflows/ci.yml:57` | The code is well-formed; the bug is a wrong premise, not a defect pattern. |
| `clang-format` | `.github/workflows/pr-formatting-check.yml` | Formatting only. |
| CodeQL | `.github/workflows/codeql.yml` | Security dataflow; no rule models "device identity read from mutable storage". |
| CodeRabbit review | PR review | Read the diff, not the device. |

None of these executes on hardware, and none exercises an app0 → app1 → app0 install cycle. The
failure is observable only on a device that has actually installed once and then attempts a second
install from the other slot. No gate in the pipeline has that shape.

---

## The untested transition

`/debug_log.txt` shows the device installed successfully while running from **app1**, landing the
image in **app0**: `dest=app0 size=4958448`, the line emitted at `FirmwareFlasher.cpp:370` on the
success path. After that reboot the device ran from app0 and rejected everything.

The same byte-identical code read the chip id correctly from app1 and incorrectly from app0.
Whatever differs between the two partition images at offset 12, it is a per-slot property — which
is precisely what a memoised read of "the running partition's own header" is blind to.

The partition layout makes the two slots symmetric on paper (`partitions.csv`): `app0` at
`0x10000`, `app1` at `0x650000`, both `0x640000`. They were not symmetric in practice, and only a
directional test would have shown it. The app1 → app0 direction was validated implicitly by the
install that succeeded. The **app0 → app1 direction was never tested at all**, and that is the
direction the device was left in.

---

## Why every channel closed

| Channel | Outcome |
| --- | --- |
| **SD card update** | Blocked by the gate. `SdFirmwareUpdateActivity.cpp:100` calls `firmware_flash::validateImageFile()` unconditionally; `BAD_CHIP` returns at `FirmwareFlasher.cpp:153`. The activity's `recoveryMode` flag changes only headers and navigation (`SdFirmwareUpdateActivity.cpp:35, 137, 206, 224, 268`) — it does **not** bypass validation. |
| **Network OTA** | Blocked by the same gate, at `OtaUpdater.cpp:262-265`. |
| **Xteink Unlocker** | Attempted once; failed before installation (see below). |
| **OEM SD `/update.bin` bootloader route** | Does not exist for the X4 Pro. This route works on X3/X4, but the CrossPoint maintainers state on crosspointreader.com's unlock page that the X4 Pro has "no known way to SD flash it — you must use the OTA Unlocker Tool". |
| **USB** | Has never enumerated on this unit. |

An exhaustive audit of the shipped `a94d3d3` tree found no SD-loaded executable mechanism that
could sidestep the gate. Ten files in `src/` reference `esp_partition_*` or `esp_ota_*` —
`OtaRollbackDetection.{cpp,h}`, `OtaRollbackDigestFormat.h`, `OtaRollbackRecoveryPlan.h`,
`SdFirmwareUpdateActivity.cpp`, `FirmwareFlasher.{cpp,h}`, `OtaBootSwitch.{cpp,h}`,
`OtaUpdater.cpp` — and every path that can write an app partition is downstream of
`validateImageFile()` or of the OTA callback, i.e. downstream of the same comparison.

One further path exists and should be named for completeness: `docs/fix-bricked-xteink.md`
documents recovery by writing the SPI flash chip directly with an external programmer (CH341a or
equivalent). It is physically invasive — dissolving the screen adhesive, cutting a battery wire —
and the guide as written targets the ESP32-C3 devices. It is a last resort on the owner's only
working unit, not a delivery channel.

---

## Status of the app1 slot

**Assessed as most likely intact.** This corrects the working assumption that app1 should be
treated as erased.

The concern was well founded on the shape of the code. In `a94d3d31`,
`esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle)` is at `OtaUpdater.cpp:228` and the
HTTP fetch does not start until `OtaUpdater.cpp:252` — the erase precedes the transfer, and
`esp_ota_abort()` (`:300`, `:306`) does not un-erase. And `HTTP_ERROR` is returned by *both*
`checkForUpdate()` (`:152`, no erase) and `installUpdate()` (`:307`, after the erase), so the error
code alone cannot distinguish them.

The observed failure, however, reported **two** values: `code 2` and `http 3:-1`. Those come from
the firmware's own failure screen, and the second one is decisive:

- `code 2` is `OtaUpdater::HTTP_ERROR` (`OtaUpdater.h:20-29`; `OK=0`, `NO_UPDATE=1`,
  `HTTP_ERROR=2`) — ambiguous on its own, as above.
- `http 3:-1` is `FailStage::OPEN` with `esp_err_t` detail `-1` (`HttpDownloader.h:33-43`;
  `NONE=0`, `BUFFER_OOM=1`, `CLIENT_INIT=2`, `OPEN=3`).
- The `http %u:%d` line is drawn **only when `failureHttpStage != 0`**
  (`OtaUpdateActivity.cpp:265-268`).
- `failureHttpStage` / `failureHttpDetail` default to `0` (`OtaUpdateActivity.h:59-60`) and are
  assigned in exactly one place: the **check**-failure branch, `OtaUpdateActivity.cpp:76-77`.
- The **install**-failure branch (`OtaUpdateActivity.cpp:311-325`) sets `lastErrorCode`,
  `failureFreeHeap`, `failureMaxBlock` and `failedDetail` — and never touches
  `failureHttpStage`/`failureHttpDetail`.
- Within one activity instance the two branches are mutually exclusive: a failed check returns
  early to `FAILED` and never reaches the install, and the `FAILED` screen offers only Back — no
  in-instance retry that could leave a stale stage behind. Activities are heap-allocated and
  deleted on exit, so the fields start at `0` on every visit.

A displayed `http 3:-1` therefore means the failure occurred inside `checkForUpdate()`, at
`esp_http_client_open` — **before** `esp_ota_begin()` ran, and so before any erase. The attempt did
not reach the code that erases app1.

Residual uncertainty, stated plainly: this reasoning depends on `code 2` and `http 3:-1` having
been read off the same failure screen. The `/debug_log.txt` line that would settle it
independently — `OtaUpdateActivity.cpp:66` writes "check failed", `:312` writes "install failed" —
was never read before it rotated out. `/debug_log.txt` is a 500-line rolling log
(`src/util/DebugLog.h:14,19`) shared by roughly fifteen subsystems (BLE, WiFi, battery, sleep,
reader-perf, covers, web, and others), so firmware-update entries are churned by ordinary use.

Practical consequence: app1 is probably a bootable fallback, but nothing in the current state
allows us to *select* it — the boot-slot switch is itself downstream of a successful install.

---

## Remediation

**CrossPoint PR #3311** (open upstream, not merged). Verified locally on `bridge/x4pro-3311`:

1. **Chip identity from the build target.** `deviceChipId()` derives the device's family from
   `CONFIG_IDF_FIRMWARE_CHIP_ID` — the same constant the bootloader compares every image against
   on every boot — with an explicit `esp_chip_model_t` → `esp_chip_id_t` switch as fallback
   (`018cc72b:src/network/FirmwareFlasher.cpp:108-126`).
2. **The flash read is removed.** `runningPartitionChipId()` no longer exists at `018cc72b`;
   nothing derives device identity from mutable storage.
3. **Validation moved before `esp_ota_begin`.** In the reworked OTA path the chip verdict is taken
   at `018cc72b:src/network/OtaUpdater.cpp:293-294` and `esp_ota_begin()` is deferred to `:313`,
   guarded by an `otaBegun` flag (`:240`, `:319`) so `esp_ota_abort()` is only called on a handle
   that was actually opened (`:356-371`). A rejected image no longer erases the fallback slot.
4. **Explicit unknown-identity handling that fails open.** The shared, host-testable
   `firmware_identity::compareChipId()` returns `Match` / `Mismatch` /
   `UnknownDeviceIdentity` (`018cc72b:src/network/FirmwareImageIdentity.h`). A known mismatch
   fails closed; an *unknown device identity* fails **open**, on the reasoning recorded in that
   header: a guard that refuses on "unknown" bricks the update path itself, and the bootloader —
   not this pre-flight check — is the authority that keeps a wrong-family image from running.
5. **The user strings are split.** `STR_FIRMWARE_WRONG_CHIP` is added alongside
   `STR_FIRMWARE_WRONG_DEVICE` (`018cc72b:lib/I18n/translations/english.yaml:659-660`). In the
   shipped firmware both `BAD_CHIP` and `WRONG_BOARD` collapse into the single
   `STR_FIRMWARE_WRONG_DEVICE` message (`a94d3d31:src/activities/settings/SdFirmwareUpdateActivity.cpp:109-110`
   and `:180`), so the screen could not tell the owner which of the two guards had fired.
6. **A host-testable seam.** The comparison moves into a header-only, Arduino/IDF-free namespace
   with its own suite (`test/firmware_image_identity/`), so the identical code the firmware runs is
   exercised on the host. Previously the two install paths carried hand-copied instances of the
   same comparison — which is how they came to be able to drift.

---

## Recovery status

A minimal bridge image is built and fully verified: `bridge/x4pro-3311` @ `018cc72b`, which is
exactly `a94d3d31` (the currently-booting firmware) plus two commits — `de5fb880` (the #3311 fix)
and `018cc72b` (a local addition that records the chip verdict to `/debug_log.txt`, so the next
attempt on a device with no usable serial port leaves a trace). Confirmed by `git log
a94d3d31..018cc72b`; `a94d3d31` is an ancestor of `018cc72b`.

Reported build artefact properties (from the build, not reproducible from the repository):

- SHA-256 `4b68cdda05e40cb7e2d7c8ff2a7662db6d6e1502618b30a2779f1afc74abb648`
- 4,959,280 bytes (well within the 0x640000 = 6,553,600-byte app partition)
- chip id `0x0009` (ESP32-S3)
- exactly one `x4pro` board tag
- valid appended SHA-256

**It is undeliverable.** No channel on the device accepts it, and USB has never enumerated on this
unit. Recovery is blocked pending a confirmed data-capable X4 Pro adapter for ESP32-S3 ROM download
mode. The SPI-flash-programmer route in `docs/fix-bricked-xteink.md` remains a last resort.

---

## Preventive gates now mandated

Being implemented on `chore/update-survivability-gate`.

1. **Update Survivability Gate.** Before any RC reaches the owner, a designated device must pass a
   full two-slot install matrix: install from app0 → app1, reboot, install from app1 → app0,
   reboot, and re-install an image identical to the running one from each slot. The last case is
   the one that catches an identity source that fails closed, and it is cheap. "It installed once"
   is not evidence that it can install again.
2. **Strict separation of UI RCs from updater changes.** A change to the installation path gets its
   own RC, with its own survivability run. It does not ride along in a touch/UI convergence branch
   because it happened to be in the same sync window.
3. **A recovery method proven to exist before testing.** No RC goes onto a device unless a working,
   *demonstrated* recovery path for that specific device exists first — demonstrated on that unit,
   not assumed from a sibling board. The X4 Pro's lack of the OEM SD `/update.bin` route was
   knowable before the RC was installed.
4. **CI label/path guard.** A PR labelled as UI-only that touches updater, partition, boot or
   validator files fails CI. The candidate path set is the ten files identified above plus
   `partitions.csv` and the new identity header. The guard's purpose is to force the
   reclassification the convergence RC never got, not to block the change.

---

## Lessons

**A guard that fails closed on its own uncertainty is a brick.** The original code has exactly one
escape hatch, `0xFFFF`, and no way to express "this identity source returned something I should not
trust". A pre-flight check that can refuse an image identical to the running firmware is not a
safety feature; it is a single point of failure sitting in front of the only mechanism that could
repair it. The bootloader already re-checks `chip_id` on every boot — the guard was never the
authority it was written as if it were.

**Never derive a device's identity from mutable storage.** What chip this is, is a property of the
build. Reading it back out of flash makes a compile-time constant into a runtime variable that can
be wrong, and the failure mode is silent because a wrong value is indistinguishable from a right
one.

**A thin fork inherits upstream's blind spots along with its code.** `e00f5958` was sound review
work against the boards upstream could test. It arrived here eight days before X4 Pro support did,
and nothing in our process re-asked the question after the board landed. Adopting upstream changes
to the installation path is precisely where "upstream tested it" stops being sufficient.

**Classify RCs by what they can break, not by what they were for.** The convergence RC was about
touch and UI. It also carried `b5e1be55` and `fef9ed0e`, both modifying `OtaUpdater.cpp`. Nobody
was hiding anything; the RC simply was never classified as one that could affect updating, so the
test that would have caught this was never even considered.

**Instrument the failure you cannot observe.** The one log line that would have resolved the app1
question — "check failed" versus "install failed" — was written, and was then rotated out of a
500-line log shared with fifteen chattier subsystems before anyone read it. On a device with no
serial port, the update path's diagnostics need their own durable record, not a share of a general
one.

**This was preventable by a test that did not exist.** Not by more review, not by a stricter
reading of the diff. Install, reboot, install again from the other slot.
