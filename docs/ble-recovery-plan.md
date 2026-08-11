# BLE Recovery Plan

**Status: planning only. No BLE recovery code has been written yet.** This
document is the current source of truth for how BLE work on the stale branch
`docs/ble-phase1-hardware-validated` (head `50e0fc64` as of this writing) will
be brought onto `develop`, now that `develop` contains Phases A, B, and C of
the thin-fork architecture (`docs/upstream-sync-architecture.md`).

For the historical design/debugging narrative this plan is derived from, see
[`docs/history/ble-module-tasks-pre-thin-fork.md`](history/ble-module-tasks-pre-thin-fork.md)
— preserved verbatim from the stale branch, but **not** current architecture.

This plan is the output of a read-only audit (no code changes, no merges, no
cherry-picks, no rebases) of the stale branch against `develop`. Nothing in
this document has been implemented yet.

## 1. What already exists on `develop`

The BLE Phase 1 foundation is already merged and is the common ancestor
(`a52a97d4`, PR #132) between `develop` and the stale branch:

- `lib/hal/BlePeripheralManager.h/.cpp` — the BLE peripheral HAL component.
- `src/BleCommandDispatcher.h/.cpp` — the command dispatcher, with
  `wifi.provision` implemented.
- NimBLE-Arduino dependency, wired into `platformio.ini`.
- The Idle-state BLE lifecycle gate in `main.cpp`'s `loop()`
  (`bleAllowedNow` — WiFi off, not the reader activity).
- A live BT BLE status indicator drawn near the battery by
  `BaseTheme::drawHeader()` when `BlePeripheralManager` is
  advertising/connected.

Confirmed byte-identical between the merge base and current `develop` for
`BlePeripheralManager.*`/`BleCommandDispatcher.*` — Phases A, B, and C never
touched the BLE mechanics themselves. Phase B's only BLE-related change was
relocating `bleEnabled`'s storage (see §2).

## 2. What exists only on the stale branch

50 commits unique to `docs/ble-phase1-hardware-validated` (merge base
`a52a97d4`), none merged via any PR. In descending order of what's actually
new relative to §1:

- 4 bug fixes hardening the Phase 1 foundation already on `develop` (retry
  from `PausedLowMemory`, four stacked root causes including a NimBLE
  build-flag/`sdkconfig.h` conflict, a missing GATT `WRITE` property bit that
  silently broke phone provisioning, repaint-lag + SD-persisted diagnostics).
- A full redesign of the BLE UX: a dedicated `BluetoothActivity` pairing
  screen (reached by holding Confirm on Home) that requests the radio only
  while it's on screen, **replacing** the persistent `bleEnabled` toggle and
  the `AppsActivity` "Midad BLE" live-toggle tile entirely.
- `device.info`, `account.claim`, `device.challenge`, `wifi.scan` BLE
  commands, plus a `BleWifiScanCache` module (WiFi/BLE share one radio, so
  `wifi.scan` reads back a cache taken before the radio switched to BLE).
- Auth-characteristic verification (session-scoped, gates every command past
  `wifi.provision`/`device.info`/`account.claim`/`device.challenge`) and
  `settings.push` over BLE, reusing `FouladDeviceTracking::applySettingsFromServer()`.
- `firmware.update` over BLE.
- `book.fetch` over BLE: phone queues a Foulad book id, device downloads it
  over its own WiFi on the next natural reconnect. Iterated three times
  (initial implementation, a real-lookup-endpoint fix, a duplicate-download
  fix) and live-verified against production.
- `sync.pull`/`sync.ack` over BLE: phone relays reading-stats/position sync
  on the reader's behalf when it authenticates for any other reason.
- ~10 permanent/temporary `BLE_*`/`BLE_TEST_*` serial debug commands, all
  inlined into `main.cpp`'s existing serial-command dispatch.

## 3. Architecture changes made by Phases A/B/C that BLE recovery must respect

- **`bleEnabled` now belongs to `MidadAppSettings`, not `CrossPointSettings`.**
  Phase B relocated it there (field #13) because it has no CrossPoint upstream
  equivalent. `CrossPointSettings.h` and `SettingsList.h` both currently
  contain **zero** Midad-specific lines related to BLE (or anything else) —
  confirmed by direct inspection during the audit.
- **BLE settings rows belong in `MidadSettingsList.cpp`, not `SettingsList.h`.**
  Any settings-list row for BLE must go through
  `appendMidadAppSettings()`/`getCombinedSettingsList()`, the same pattern
  Phase B established for all other Midad Apps settings.
- **The stale branch's own `CrossPointSettings`/`SettingsList.h` BLE
  implementation must not be restored.** It predates Phase B and represents
  an architecture `develop` has already moved past. Note this is doubly true
  for BLE specifically: the stale branch's *own later commit*
  (`8fa4ba27`) independently deleted `bleEnabled` from those files too, in
  favor of the dedicated pairing-screen redesign — same direction as Phase B,
  arrived at for an unrelated reason. Net effect: recovering the redesign
  requires **zero new lines** in `CrossPointSettings.h`/`SettingsList.h`.
- **Midad product logic stays in Midad-owned modules**, connected to
  upstream-owned files via the smallest possible stable hook — the rule
  Phases A/B/C were built around (`docs/upstream-sync-architecture.md`).
  BLE recovery follows the same rule; see §6 for the specific files this
  constrains.
- **Generic upstream fixes come through the normal upstream-merge/convergence
  path, not stale manual ports.** 12 of the 50 stale-branch commits are
  upstream CrossPoint cherry-picks already present on `develop` under
  different SHAs (same PR numbers, independent pulls). None of those should
  be manually re-applied from the stale branch — see §5.

## 4. Recovery sequence

Each phase below starts from current `develop`, is its own PR, preserves the
Phase A/B/C architecture, and requires hardware verification where marked.
**The stale branch is never merged wholesale — nothing here is a merge,
rebase, or bulk cherry-pick of `docs/ble-phase1-hardware-validated`.**

### BLE-R1 — Phase 1 hardening
Recover the bug fixes atop the foundation already on `develop`:
- `0592d385` — BLE state repaint lag + SD-persisted lifecycle diagnostics
- `28562591` — `BlePeripheralManager::begin()` couldn't retry from `PausedLowMemory`
- `90d19895` — four stacked root causes (NimBLE build-flag/`sdkconfig.h`
  conflict, `ActivityManager` freeing BLE's ~65KB before the next activity's
  `onEnter()`, plus two more)
- `835dacbc` — Auth characteristic missing its `WRITE` property bit
- the per-device-name portion of `804cfacd`

Lowest risk of any phase: bug fixes to code already on `develop` and already
X3-hardware-validated, no settings/architecture surface touched.

### BLE-R2 — Dedicated pairing screen + status presentation
Reimplement the `BluetoothActivity` redesign and BLE status icon against
current architecture — not ported as-is. Specifically:
- `bleEnabled` should likely be **removed** from `MidadAppSettings`/
  `MidadSettingsList` entirely rather than carried forward: the dedicated
  screen has nothing to persist (radio only runs while the screen is open).
- `main.cpp`'s BLE lifecycle/debug-command additions must land as calls into
  new Midad-owned modules, not inlined into `loop()` (see §6).
- Status icon in `FouladTheme`/`LyraTheme` needs a minimal-hook design — see
  §6's conflict-budget note before assuming a direct port.

### BLE-R3 — Discovery/claim commands
`device.info`, `account.claim`, `device.challenge`, `wifi.scan` (including
`BleWifiScanCache`), plus the headless-verify portion of `804cfacd`.

### BLE-R4 — Auth + settings.push
Lowest re-architecture cost of the feature phases: `applySettingsFromServer()`
already exists on `develop`, unchanged in shape since Phase B, and is already
the correct integration point.

### BLE-R5 — firmware.update + book.fetch
`firmware.update`, plus `book.fetch` (including the real-lookup-endpoint and
duplicate-download fixes). **The OPDS integration must land through
`FouladOpdsHooks`** (`src/FouladOpdsHooks.h/.cpp`, added in Phase C) —
specifically, `flushPendingBleBookFetch()`'s call site belongs inside
`FouladOpdsHooks::reportDeviceTrackingOnConnect()`, not back inside
`OpdsBookBrowserActivity.cpp`. Phase C actually makes this easier than the
original stale-branch implementation, which predates `FouladOpdsHooks` and
called it directly from what was then a private `OpdsBookBrowserActivity`
method.

### BLE-R6 — sync.pull/sync.ack
Requires real-data device testing: every real connect on the stale branch
hit an empty `sync.pull` snapshot, so the relay's actual payload-handling
path has never been exercised against hardware, only its empty-snapshot
path.

### BLE-R7 — BLE-central/page-turner (optional)
Design-only on the stale branch — zero code exists to rescue. A fresh
scoping exercise if ever pursued, not a recovery of existing work.

## 5. Independent fixes discovered during audit

These are **not** BLE work and must not be bundled into any BLE-R phase.
They were found only because auditing the stale branch required diffing its
full history against `develop`; they happen to live on that branch but are
unrelated to it. Each should be evaluated and implemented as its own
independent PR, off current `develop`, unconnected to the BLE recovery
sequence above.

- **`e02fb457` — SDK/display-controller soft-brick safety fix.** Bumps the
  `freeink-sdk` pin and fixes `HalGPIO.cpp`'s controller-detection call
  ordering so newer-production-batch X3/X4 units (UC8253→UC8279d,
  SSD1677→UC8179/UC8279 controller swaps) aren't driven with the wrong
  controller's command set. This is the actual soft-brick mechanism it
  closes. **Highest priority of the four**, subject to confirming — before
  implementing — that current `develop`/current upstream CrossPoint still
  lacks the equivalent change (confirmed missing as of this audit: `develop`
  is still pinned at the older `freeink-sdk` SHA `e62f6c16`, bumped there
  independently and unrelated to this fix).
- **`50e0fc64` — `FOOTNOTE_HREF_LEN` correctness fix.** Bumps 96→256 so
  Calibre-generated EPUBs with long, URL-encoded footnote hrefs don't get
  truncated. Also bumps `SECTION_FILE_VERSION` (cache-format change, not
  purely additive). Confirmed still `96` on `develop` as of this audit.
- **`0a0e8c3f`, non-BLE half — `WifiCredentialStore` lazy-load-on-read fix.**
  `WIFI_STORE` never auto-loads from disk except via
  `WifiSelectionActivity::onEnter()` — confirmed via `main.cpp`'s boot
  sequence that this is still true on `develop` today. Any code path that
  reads `WIFI_STORE` before that activity has ever been entered in the
  current boot session sees an empty in-RAM store regardless of what's
  actually saved on disk. Real, currently-reproducible, unrelated to BLE.
- **`8efdaef2` — Home cover-cache heap-fragmentation fix.** `HomeActivity`'s
  cover-image cache was one ~42KB buffer for the whole
  status-line+hero+divider+thumbs tile; a single large malloc failing under
  any heap pressure (BLE being only the trigger that surfaced it, not the
  cause) forced a full SD re-decode on every selector move. Splits into
  independent per-region cache slots so a heap squeeze only costs whichever
  slot doesn't fit. Generic heap-fragmentation-avoidance fix, valuable
  independent of whether BLE is ever recovered.

## 6. Conflict-budget rules

These constraints are binding for every BLE-R phase above, derived directly
from the audit's file-by-file comparison against the current 158-file
fixed-upstream conflict baseline (`docs/upstream-sync-architecture.md`):

- **`CrossPointSettings.h`/`SettingsList.h` must remain free of Midad BLE
  settings behavior.** Both are currently at zero Midad-specific lines
  (post-Phase-B); no BLE-R phase should add any.
- **The `AppsActivity` BLE live-toggle implementation from the old branch is
  superseded** by the planned dedicated-screen design (BLE-R2). Note that
  `develop` *currently* still carries that live-toggle tile (pointed at
  `MIDAD_APP_SETTINGS.bleEnabled` post-Phase-B) — `AppsActivity.cpp`/`.h` are
  currently clean against the conflict baseline despite this. BLE-R2 should
  delete this code, not extend it; doing so takes `AppsActivity`'s Midad-BLE
  footprint to zero and leaves the file at least as upstream-similar as it is
  today.
- **Avoid turning currently-clean `FouladTheme.cpp`/`LyraTheme.cpp` into new
  conflict files merely for a BLE status icon.** Both are currently absent
  from the 158-file baseline. Design a Midad-owned/minimal-hook solution for
  the status icon in BLE-R2 before assuming a direct port of the stale
  branch's theme changes — this is the one place in the whole recovery where
  a naive port would most likely create a new conflicting file that doesn't
  exist today.
- **`main.cpp` should contain only minimal BLE lifecycle hooks.** The stale
  branch grew `main.cpp` by ~240 net lines (lifecycle gate, diagnostics, ~10
  serial debug commands, all inlined into `loop()`). Recovery should extract
  this into Midad-owned modules (e.g. a lifecycle-pump function and a
  debug-command handler function), leaving `main.cpp` with a small, bounded
  number of hook call sites rather than the inlined block.
- **`OpdsBookBrowserActivity` should receive no new BLE/Foulad
  implementation.** Route through `FouladOpdsHooks` instead (see BLE-R5).
  `OpdsBookBrowserActivity.cpp` is already on the conflict baseline; routing
  through `FouladOpdsHooks` means this phase adds zero new lines to it.
- **HAL-level generic BLE infrastructure may live in HAL when genuinely
  platform-level** — e.g. `BlePeripheralManager` itself, already correctly
  placed in `lib/hal/`. The distinction is device/platform infrastructure
  (HAL-appropriate) versus Midad product behavior (Midad-module-appropriate);
  apply it phase by phase rather than defaulting either direction.

## 7. Hardware-validation matrix

Preserved from the audit. Every item below needs real hardware verification
before its corresponding BLE-R phase can be considered done — the simulator
cannot substitute for any of it (both `BleCommandDispatcher.cpp` and
`BluetoothActivity.cpp` are excluded from simulator builds via
`build_src_filter`, and the simulator's flat 1MB heap cannot reproduce
fragmentation/OOM behavior regardless).

| Item | X3 | X4 | Notes |
|---|---|---|---|
| NimBLE init/deinit, heap gate, cooldown, retry-from-paused | Previously validated (stale branch) | Never validated | Re-verify on X3 during BLE-R1; X4 is new ground |
| CPU-frequency locking (WiFi mode change racing `HalPowerManager::Lock`) | Previously reproduced + fixed | Never validated | Real intermittent hang found and fixed on the stale branch — re-verify the fix still holds |
| Runtime/static heap (BLE's ~65KB resident cost) | Previously measured | Never validated | Directly motivates the BLE-R0-adjacent `8efdaef2` fix (§5) |
| WiFi/BLE handoff (single-radio mutual exclusion) | Previously validated | Never validated | |
| Advertising + GATT connection lifecycle (incl. Auth `WRITE_ENC`) | Previously validated (real iPhone + serial-injected fallback) | Never validated | |
| Deep sleep/wake interaction | **Never validated on either device** | **Never validated on either device** | Gap on both platforms, not just X4 |
| Phone + XTEINK interaction (`wifi.provision` handshake specifically) | **Never physically tested end-to-end** | Never validated | An earlier stale-branch doc entry claimed this was validated; a later entry in the same document explicitly retracts that claim — see the preserved history doc |
| Firmware/book transfer (`firmware.update`, `book.fetch`) | `book.fetch` live-verified against production | Never validated | `firmware.update` itself less thoroughly verified than `book.fetch` |
| Real sync data (`sync.pull`/`sync.ack` with actual queued entries) | **Never validated** | Never validated | Every real connect during stale-branch testing hit an empty snapshot; only the empty-snapshot code path has ever run on hardware |
| OTA/recovery interaction | Never validated | Never validated | Not exercised anywhere in the stale branch's history |

**Bottom line, unchanged from the audit**: every hardware claim on the stale
branch is X3-only. X4 has zero physical BLE validation. Deep sleep, OTA
interaction, and `sync.pull`/`sync.ack` with real data are gaps on both
platforms.

## 8. Discarded / superseded work

- **`171c7418`** ("Phase 1 BLE provisioning validated end-to-end on a real
  Xteink X3") — discard. Self-superseded by the very next commit
  (`d6543d3e`), which corrects the claim: only firmware-side performance and
  the toggle were tested, not the actual phone-to-device handshake.
- **`cd95e186`** ("EndOfBookOptions fails to compile") — discard. Confirmed
  during the audit that `EndOfBookOptions.cpp` compiles clean on current
  `develop` without this commit's extra include; whatever include-ordering
  issue it fixed was specific to the stale branch's state at the time, not a
  standing bug on `develop`.
- **12 upstream-equivalent cherry-picks** — do not manually rescue any of
  these; equivalent changes are already on `develop` under different SHAs
  (confirmed by matching upstream PR numbers):

  | Stale-branch SHA | Subject (PR #) | `develop` SHA with the same change |
  |---|---|---|
  | `7a6f1d31` | fix: keep list item bullet inline with nested paragraph text (#2589) | `0756d9f7` |
  | `ac0f3528` | feat(i18n): add Norwegian Bokmål translation (#2113) | `94ae750e` |
  | `fe1a3004` | feat(i18n): add Bosnian translation (#2616) | `b9867b0d` |
  | `e9b54780` | feat: remember web upload settings & rename pattern (#2534) | `35a45f9c` |
  | `0d4d39b6` | add Bahasa Indonesia Translation (#2666) | `aacdd7e9` |
  | `21aa60de` | fix: STR_RESTARTING_HINT text overflow bug (#2692) | `66abde5f` |
  | `52ffe091` | fix: preserve custom font ligatures (#2673) | `9ec77671` |
  | `618873c2` | test: repair malformed section-break EPUB fixture (#2728) | `b25e57da` |
  | `7285eecc` | docs: bricked-Xteink recovery instructions (#2622) (#2682) | `f39d35d7` |
  | `5d71aaae` | perf: cache cumulative spine sizes in RAM (#2441) | `6af9a049` |
  | `07fe942c` | fix(ui): wrap button hint labels that overflow their button (#2928) | `81028d58` |
  | `0ddc42f8` | fix: MAC address reading when WiFi is off (#2960) | `9b090266` |

  A thirteenth, `05cd9cd1` (a revert of `#2692`), is also already reflected
  on `develop` — upstream reverted the same change there too.

## 9. Branch lifecycle

`docs/ble-phase1-hardware-validated` **must remain available** throughout
the recovery sequence in §4. It is not merged, rebased, or force-pushed by
any of this work.

It can be archived or deleted only after **all** of the following:

1. All BLE-R phases selected for recovery (not necessarily all of R1–R7 —
   see §4) have been individually reimplemented against current `develop`
   and merged via their own PRs.
2. The historical design/debugging record is preserved — done by this PR
   (`docs/history/ble-module-tasks-pre-thin-fork.md`).
3. A final branch audit, run the same way as the one this plan is based on,
   confirms no unique valuable implementation or design work remains
   uncaptured on the stale branch.

Until then, the branch stays as the canonical reference for what still needs
re-architecting, alongside this plan and the preserved history document.
