# Update Survivability Gate

**Status: mandatory.** No X4 Pro release candidate may be handed to the owner
until a designated test device has completed every case in the matrix below and
the results are recorded and signed off.

Enforced in CI by [`.github/workflows/update-survivability-gate.yml`](../.github/workflows/update-survivability-gate.yml)
via [`scripts/update-survivability-gate.sh`](../scripts/update-survivability-gate.sh).

---

## 1. Why this gate exists

A UI/touch hardware-test RC also changed the firmware **installation path**.
It shipped without a two-slot update test. It installed **once**, successfully,
onto the owner's X4 Pro — and after that the device could never install again.

The mechanism:

- [`firmware_flash::runningPartitionChipId()`](../src/network/FirmwareFlasher.cpp)
  reads `chip_id` from offset 12 of the **running partition's** image header,
  caches it in a function-local static, and returns `0xFFFF` on any read
  failure.
- The SD install path (`validateImageFile()` in
  [`src/network/FirmwareFlasher.cpp`](../src/network/FirmwareFlasher.cpp))
  compares the candidate image's `chip_id` against that value and returns
  `Result::BAD_CHIP` on mismatch.
- The OTA path (`OtaUpdater::installUpdate()` in
  [`src/network/OtaUpdater.cpp`](../src/network/OtaUpdater.cpp)) buffers the
  first 14 bytes of the stream, does the same comparison, and aborts the
  transfer with `wrongChip = true`.

Both **fail closed on the same misread value**. Once the device was running
from a slot whose header the function misreads, every subsequent install — SD
*and* OTA — was rejected. The owner's only X4 Pro is update-locked with no
working recovery channel.

### What did not catch it

| Check | Ran | Result |
|---|---|---|
| Four board builds (`default`, `sticky`, `x4pro`, `papermono`) | yes | pass |
| 512 host tests | yes | pass |
| `cppcheck` (`pio check`) | yes | pass |
| CodeQL | yes | pass |
| CodeRabbit review | yes | pass |

**None of them runs on hardware. None of them exercises an install *cycle*.**
Adding more of the same class of check cannot close this gap. Only a real
device installing a second time can.

> ### The single most important rule on this page
>
> **A single successful installation is NOT sufficient evidence.**
>
> That is exactly what passed here. The bricking change installed cleanly the
> first time; the defect only appears on the *next* install, from the slot the
> first install landed in. Any validation that stops after one successful
> flash proves nothing about survivability.

---

## 2. Recovery precondition (do this before flashing anything)

Before an RC image is written to any device, **at least one recovery method
must be *proven*, not assumed.** Proven means demonstrated on that specific
device, in that specific state, before the risky flash — not "should work in
theory."

Accept exactly one of:

1. **ROM-loader USB connection proven.** The device enumerates in download
   mode and `esptool` can read the chip ID / flash ID over that connection.
   Demonstrate this *before* the RC is written, and record the `esptool`
   output.
2. **Rollback to the previous slot tested.** The device has been shown to boot
   the other OTA slot on demand (see
   [`src/network/OtaBootSwitch.cpp`](../src/network/OtaBootSwitch.cpp) and
   [`src/OtaRollbackRecoveryPlan.cpp`](../src/OtaRollbackRecoveryPlan.cpp)),
   and that path was exercised at least once in this session.
3. **Designated sacrificial unit.** The device under test is expendable, is
   not anyone's working reader, and professional flash recovery (external
   programmer / chip-off reflash) is available and arranged.

If none of the three holds, **stop.** Do not flash. This precondition is
case **US-0** in the matrix and it is not waivable.

### The device rule

> The test device must **never** be the owner's only working unit.

This is not a preference. The incident happened because the only X4 Pro in
existence was also the test device. See
[`docs/fix-bricked-xteink.md`](fix-bricked-xteink.md) for what recovery costs
once that rule is broken.

---

## 3. Flash geometry

From [`partitions.csv`](../partitions.csv):

| Name | Type | SubType | Offset | Size |
|---|---|---|---|---|
| `otadata` | data | ota | `0xe000` | `0x2000` |
| `app0` | app | ota_0 | `0x10000` | `0x640000` |
| `app1` | app | ota_1 | `0x650000` | `0x640000` |

Two application slots. `esp_ota_get_next_update_partition()` alternates between
them, so a healthy device ping-pongs `app0 → app1 → app0 → app1`. **The whole
point of this matrix is that each direction of that ping-pong is a distinct
code path and must be tested separately.** The bricking change passed the first
hop and failed the second.

---

## 4. The survivability matrix

Build three distinct, individually identifiable images before starting:

- **A** — the known-good baseline currently shipping.
- **B** — the release candidate under test.
- **C** — any third build distinguishable from B (a trivial version-string
  bump on top of B is fine). C exists so the `app1 → app0` hop can be tested
  with an image that is not already resident.

Record the **SHA-256 of every image file** before flashing it, and again as
read back from the device where the path allows it.

For **every transition**, record: **running slot**, **target slot**, **image
SHA-256**, and **boot result**.

| ID | Case | What must happen |
|---|---|---|
| **US-0** | Recovery precondition proven (§2) | One of the three recovery methods demonstrated on this device *before* the first RC flash |
| **US-1** | Known-good **A** running from `app0` → install **B** into `app1` | Install succeeds; device boots B from `app1` |
| **US-2** | **B** running from `app1` → install **C** into `app0` | Install succeeds; device boots C from `app0`. **This is the hop the incident failed.** |
| **US-3** | **C** running from `app0` → install **B** into `app1` | Install succeeds; device boots B from `app1`. Proves the cycle closes, not just that two hops work |
| **US-4** | Self-reinstall, where supported | Installing the currently running version again either succeeds cleanly or is refused with a clear message — never leaves the device unbootable. Mark `N/A` if the build refuses same-version installs by design (see `isUpdateNewer()` in [`OtaUpdater.cpp`](../src/network/OtaUpdater.cpp)) |
| **US-5** | Wrong-**chip** image offered | Rejected, **and neither slot is erased**. The running slot still boots and the inactive slot still holds its previous image |
| **US-6** | Wrong-**board** image offered (same chip family, different board tag — see [`FirmwareBoardTag.h`](../src/network/FirmwareBoardTag.h)) | Rejected, **and never selected as bootable**. Partial bytes may land in the inactive slot; `esp_ota_set_boot_partition()` must not be reached |
| **US-7** | Corrupted / truncated image offered | Rejected **before erase**. Truncate a valid image mid-file and offer it via SD; the fallback slot must be intact afterwards |
| **US-8** | HTTP failure **before** the download starts (unreachable host, DNS failure, TLS rejection) | Update aborts; **the fallback slot is not erased** |
| **US-9** | HTTP failure **during** the download (connection dropped mid-stream) | Update aborts; **the fallback slot is not erased** |
| **US-10** | Power loss at each documented safe stage (§6) | Device boots afterwards, from either slot |
| **US-11** | After **every** failure case above | Confirm the previous slot still boots before moving on |

### Why US-8 and US-9 are required cases, grounded in the code

[`OtaUpdater::installUpdate()`](../src/network/OtaUpdater.cpp) calls:

```cpp
esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
```

and only *then* starts the HTTP fetch (`HttpDownloader::fetchUrlVerified`).

`esp_ota_begin` with `OTA_SIZE_UNKNOWN` erases the **entire** target partition
up front, because it cannot know how much of it the incoming image will use.
So the ordering is:

1. erase the whole fallback slot
2. open the network connection
3. download

The fallback image is destroyed **before the first byte arrives**. Any network
failure at step 2 or step 3 therefore happens with one slot already blank —
which is survivable only if the *running* slot is still good. US-8 and US-9
exist to prove that this erase-before-fetch ordering never costs the device its
only bootable image. They are not hypothetical robustness checks; they are a
direct consequence of that one line.

The same ordering is why US-7 says "rejected **before** erase": the SD path
(`validateImageFile()`) validates the whole file from the card *before* any
partition write, and that property must be preserved. Reordering validation
after erase would turn a rejected image into a bricked device.

### Why US-5 and US-6 are separate cases

`chip_id` cannot distinguish S3 boards from each other — `sticky`, `x4pro` and
`papermono` all share it. That is why
[`FirmwareBoardTag.h`](../src/network/FirmwareBoardTag.h) exists and why the
board check has different semantics from the chip check: the chip check runs
against the *header* and rejects before any write; the board check runs against
the *stream* and may abort mid-write, relying on `esp_ota_abort()` so the
partial image is never made bootable. Two different guarantees, two different
test cases.

---

## 5. Results table (fill this in)

Copy this block into the PR description. CI reads it: every `US-` row must
carry a `PASS`, `FAIL` or `N/A` verdict, `Test device:` must name a unit, and
`Build SHAs:` must contain real commit hashes.

```markdown
## Hardware validation

Test device: <model, serial/MAC, and why it is NOT the only working unit>
Recovery method proven: <ROM loader | tested rollback | sacrificial unit + recovery arranged>
Build SHAs: A=<commit> B=<commit> C=<commit>
Image SHA-256: A=<sha256> B=<sha256> C=<sha256>
Firmware channel(s) exercised: <SD | OTA | both>

### Survivability matrix

| ID    | Running slot | Target slot | Image SHA-256 | Boot result | Verdict | Notes |
|-------|--------------|-------------|---------------|-------------|---------|-------|
| US-0  |              |             |               |             |         |       |
| US-1  | app0         | app1        |               |             |         |       |
| US-2  | app1         | app0        |               |             |         |       |
| US-3  | app0         | app1        |               |             |         |       |
| US-4  |              |             |               |             |         |       |
| US-5  |              |             |               |             |         |       |
| US-6  |              |             |               |             |         |       |
| US-7  |              |             |               |             |         |       |
| US-8  |              |             |               |             |         |       |
| US-9  |              |             |               |             |         |       |
| US-10 |              |             |               |             |         |       |
| US-11 |              |             |               |             |         |       |
```

`N/A` is acceptable only for **US-4**, and only where the build refuses
same-version installs by design. `FAIL` or `BLOCKED` on any row blocks the RC;
so does a row left without a verdict. Both are CI failures, not warnings.

A recorded `FAIL` is a *good* outcome for the process — it is the gate working.
Fix the firmware, re-run the matrix, update the report.

---

## 6. Power-loss stages (US-10)

Cut power at each of these points, then confirm the device boots afterwards:

| Stage | Where | Expectation |
|---|---|---|
| P1 | After `esp_ota_begin()` erase, before any write | Running slot still boots; inactive slot blank |
| P2 | Mid-write, roughly 50% through | Running slot still boots; `otadata` still points at it |
| P3 | After the last `esp_ota_write()`, before `esp_ota_end()` | Running slot still boots; the incomplete image is never selected |
| P4 | After `esp_ota_end()`, before `esp_ota_set_boot_partition()` | Running slot still boots |
| P5 | Immediately after `esp_ota_set_boot_partition()`, before reboot | Device boots the NEW slot; if it does not, rollback recovery engages (see [`src/OtaRollbackDetection.cpp`](../src/OtaRollbackDetection.cpp)) |

P5 is the only stage where the boot target legitimately changes. Any *other*
stage that changes the boot target is a defect.

---

## 7. Sign-off

An RC is not releasable until this block is complete. It goes in the PR
description alongside the results table, or in a report file under
`docs/hardware-validation/`.

```markdown
## Update survivability sign-off

- [ ] The test device is NOT the owner's only working unit (§2, device rule)
- [ ] US-0 recovery precondition was proven BEFORE the first RC flash
- [ ] Every matrix row US-0..US-11 has a recorded verdict
- [ ] Running slot, target slot, image SHA-256 and boot result are recorded
      for every transition — not just the successful ones
- [ ] At least THREE consecutive successful installs across BOTH slots were
      observed (US-1, US-2, US-3), not one
- [ ] After each failure case, the previous slot was confirmed bootable (US-11)
- [ ] Both install channels in scope for this RC were exercised (SD and/or OTA)

Tested by: <name>
Date: <YYYY-MM-DD>
Firmware under test: <build SHA>
Baseline it replaces: <build SHA>
Verdict: <RELEASABLE | BLOCKED>
```

---

## 8. What CI checks, and what it does not

The workflow verifies the report was **written down**: that a protected file
was touched, that a test device is named, that build SHAs are real hashes and
not `TBD`, that every matrix row carries a verdict, and that no row failed.

It cannot verify the results are **true**. Nobody can automate that — the whole
premise of this gate is that no CI job runs on hardware. The reviewer owns
judging whether the recorded slots, hashes and boot results describe a test
that actually happened.

Protected paths, from
[`scripts/update-survivability-gate.sh`](../scripts/update-survivability-gate.sh):

- `src/network/FirmwareFlasher.*`, `src/network/OtaUpdater.*`,
  `src/network/OtaBootSwitch.*`, `src/network/FirmwareBoardTag.*`,
  `src/network/FirmwareImageIdentity.h`, `src/network/FirmwareUpdatePolicy.h`
- `src/OtaRollback*`
- `src/activities/settings/SdFirmwareUpdateActivity.*`,
  `src/activities/settings/OtaUpdateActivity.*`
- `partitions.csv`
- `.github/workflows/release*.yml`
- Broad conventions so newly added files are caught without a list update:
  `src/network/Firmware*`, `src/network/Ota*`, `src/Ota*`,
  `src/activities/settings/Ota*`, `src/activities/settings/SdFirmware*`,
  and any `scripts/*sign*`, `scripts/*hash*`, `scripts/*checksum*`,
  `scripts/*digest*`

One documented carve-out: `.github/workflows/release-fonts.yml` matches
`release*.yml` by name but publishes SD-card font packs — it touches no
firmware image, partition or boot slot, so no install testing could say
anything about a change to it. It is excluded explicitly rather than left as a
false positive, because a gate that hard-blocks work it has nothing to say
about is a gate people learn to route around. Any new exclusion must clear the
same bar.

Renaming a protected file does not get you out of the gate. Both the **source**
and the **destination** path of every rename are matched against the list
above, so all three of these block:

- `src/network/OtaUpdater.cpp` → `src/network/Updater.cpp` — the file that was
  the updater still is the updater; matching only the destination would report
  this PR as "Not applicable" while the install path was rewritten under a new
  name.
- `src/network/Updater.cpp` → `src/network/OtaUpdater.cpp` — whatever content
  lands at a protected pathname *is* the update path from that commit on.
- A rename through the `release-fonts.yml` carve-out in either direction — the
  exclusion is evaluated per path, not per rename.

A rename that changes not one byte is classified identically to a rewrite; the
gate reads paths, never patch size. When a rename is what triggered the gate,
the failure message names both paths and which side matched, because the
blocking path will not be in the diff you are looking at.

The gate is triggered by the **diff**, not by a label. A PR cannot opt out of
it by omitting or removing a label — see the workflow header for that decision
and its residual failure mode.

### Branches that do not carry this workflow

The gate runs on `pull_request_target`, so GitHub executes the **base branch's**
copy of the workflow, not the pull request's. A pull request opened against a
branch that predates this file therefore gets no gate run at all — and it gets
none *silently*. There is no red check, no skipped check, and nothing in the
PR's check list to notice; the absence looks exactly like a clean sheet. That
is the same shape of failure the gate exists to prevent, so it is called out
here rather than left to be discovered.

Two rules follow.

**Create every future `release/*` and `sync/*` branch from `develop` once
`develop` carries this workflow.** A branch cut from protected `develop` carries
the gate with it, and every PR into it is checked automatically. Existing
release and sync branches are deliberately **not** retrofitted: rewriting the
history of a branch that has already produced a tested artifact would cost the
provenance of that artifact for no gain the manual route below does not
already provide.

**A PR into a branch without the workflow needs a trusted manual gate result
attached before it merges.** Run
[`scripts/update-survivability-gate.sh`](../scripts/update-survivability-gate.sh)
from a commit on a protected branch — never from the PR's own head, which the
PR author controls — against that PR's exact base and head SHAs, and attach the
complete output as a comment. To count as trusted, the comment must state:

- the branch and commit the gate script itself came from;
- the PR's base SHA and head SHA, verbatim;
- the changed-record count, and whether the API's 3000-file cap was hit;
- the gate's **complete** output, untruncated;
- the exit code and its meaning — and note that exit 2 means the gate could not
  determine an answer, which is a failure, never a pass;
- that the gate's own self-test suite was run and passed.

A manual result is weaker evidence than an automatic one, because a human chose
when to run it and against what. Attaching one is a reviewer-facing obligation,
not a formality — the reviewer is the enforcement mechanism, and should refuse a
PR that lacks it exactly as CI would.
