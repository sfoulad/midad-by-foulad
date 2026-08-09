# BLE module: phone control + remote page-turner

Implementation handoff, written 2026-08-09. This is the foulad-eink side of a
plan being worked out with Midad (the phone app) — that conversation is
ongoing and this doc is a snapshot of it, not the final word. If something
here turns out wrong once you're actually building it, that's expected;
write it back as an update to this doc (or a reply doc) rather than treating
it as fixed.

## The requirement

Two features, sharing one radio:

1. **Phone control.** From Midad: push settings to a nearby reader instantly,
   provision a brand-new reader's Wi-Fi without touching its screen, tell a
   reader to fetch a specific book, trigger a firmware update check — all
   over BLE, without needing Wi-Fi already configured.
2. **Remote page-turner.** Pair a BLE HID remote (GameBrick, Free2, similar)
   to turn pages hands-free. Not new in this ecosystem — CrumBLE and
   `thedrunkpenguin/crosspoint-reader-ble` both already ship it on this exact
   hardware — but new to this firmware.

These use **opposite BLE roles** (device-as-peripheral for #1, device-as-
central for #2) and run at **different times** (idle/menus for #1, actively
reading for #2). Treat them as two tracks sharing infrastructure, not one
feature.

## Why this needed real research before a spec, not just a design

The obvious approach — "just enable BLE alongside Wi-Fi, use standard
coexistence" — doesn't match what's actually been shipped on this chip.
Two real, MIT-licensed forks of CrossPoint Reader (this firmware's own
upstream) already fought this exact battle in production, and the numbers
below come from their code and their field crash reports, not from a
datasheet.

## Hardware reality

- ESP32-C3, no PSRAM, ~380 KB usable RAM — same ceiling this firmware
  already documents extensively (`README.md:450`, ~370+ `ESP.getFreeHeap()`
  gate sites).
- NimBLE resident cost: **~57–58 KB**. Confirmed independently by both
  CrumBLE's changelog and `crosspoint-reader` PR
  [#2527](https://github.com/crosspoint-reader/crosspoint-reader/pull/2527)
  (merged, written after real field `abort()` crashes).
- **Wi-Fi and BLE are mutually exclusive in practice on this firmware
  stack**, not just time-sliced. `crosspoint-reader-ble`'s
  `BluetoothHIDManager::enable()` forcibly calls `WiFi.mode(WIFI_OFF)`
  before `NimBLEDevice::init()`, with the comment *"ESP32-C3 cannot have
  both WiFi and BLE enabled simultaneously."* Espressif's own docs describe
  coexistence as supported and automatic — that may be true of the raw SDK,
  but it is not what either shipped fork relies on. Design around mutual
  exclusion.
- PR #2527's numbers, which we're adopting as the starting design target:
  - Refuse to start BLE below **~100 KB free** (57 KB stack + 40 KB EPUB
    section-build headroom) — starting it below that just re-triggers the
    recovery path that tears it back down, seen in the field as an endless
    "BT Connecting…" loop.
  - **30-second cool-down** after any low-memory teardown, so it can't
    oscillate.
  - A third BLE state beyond on/off: **PAUSED (low memory)**, shown
    honestly in the UI rather than silently dropping the connection.
  - **Split by device-state section below, not applied as one flat number:**
    the 40 KB of the 100 KB figure is EPUB section-build headroom — margin
    for a chapter render to still succeed with BLE resident. Per this doc's
    own device state machine (below), that scenario is **Reading** only
    (BLE-central, Phase 4's page-turner, connected through active reading).
    **Idle** state (Phases 1–3, BLE-peripheral, no book open) has no EPUB
    pressure to leave headroom for at all. Proposed split, to verify once
    real numbers come in:
    - Idle → BLE-peripheral start gate: **~70 KB free** (57 KB NimBLE + a
      smaller flat safety margin, no EPUB term).
    - Reading → BLE-central start gate: the full **~100 KB**, matching
      PR #2527's own measured scenario exactly.
    This is reasoned from the numbers already in this doc, not independently
    measured — treat it the same as every other threshold here: log real
    `ESP.getFreeHeap()` sessions before trusting it, and tighten this note
    once that data exists. Only the ~70 KB Idle figure is load-bearing for
    Phase 1; the Reading-state figure is Phase 4's concern.
- Leaving a BLE remote connected into device idle/sleep crashes the device
  (CrumBLE [issue #44](https://github.com/imshentastic/CrumBLE/issues/44)).
  Tear BLE down as part of the sleep sequence, not after sleep entry trips
  over it.

None of this has been measured on this codebase specifically yet — it's
proven on CrossPoint/CrumBLE, and this firmware carries more features
(dictionary, two themes, games, gym tracker) than either of them. Log
`ESP.getFreeHeap()` through real sessions with BLE running before trusting
these exact thresholds here.

**Correction (2026-08-09, verified against this repo): the BLE library
dependency already exists here, unused.** `freeink-sdk/libs/network/BleKeyboardHost`
is a complete, production NimBLE **central**-role integration (BLE HID host —
pairs with page-turner/keyboard peripherals), capability-gated behind
`FREEINK_CAP_BLE_HID_HOST` and currently **disabled** in this firmware (grep
confirms zero references to that flag or to NimBLE anywhere in `src/`/`lib/`).
Its own `library.json` pins the exact dependency to add:
`h2zero/NimBLE-Arduino@^2.3.8` — **not** `esp-nimble-cpp`; the SDK's note
explains this version "carries the arduino-esp32 3.3.7 controller-init fix
and the Arduino BLE init path." This firmware's `platformio.ini` already
builds against the same platform release the SDK's own sample config
verifies against (`pioarduino/platform-espressif32` `55.03.37`, Arduino core
3.3.x / ESP-IDF 5.5.x — confirmed by our own `pio run` output), so that
pinned version applies directly here, not just "probably compatible."
Two things this changes:

- **Phase 4 (page-turner) is far cheaper than "port patterns from
  `crosspoint-reader-ble`."** `BleKeyboardHost` already does exactly that
  job — HID central role, Just-Works bonding, auto-reconnect, a `SpecialKey`
  enum that already includes `PageUp`/`PageDown` — and it's MIT-licensed
  in-tree. Phase 4 becomes "enable the capability, add the lib_dep, wire it
  into an Activity + the Reading-state lifecycle," not "write a new NimBLE
  central integration." See the corrected item 4 under "What's actually new
  work here" below.
- **A real, load-bearing constraint surfaces from reading its lifecycle
  code, not present anywhere in this doc before:** `BleKeyboardHost::end()`
  doc comment: *"Must run at normal CPU frequency (controller deinit), like
  begin()."* NimBLE init/deinit apparently doesn't tolerate running under
  this firmware's power-saving reduced CPU clock (`HalPowerManager`,
  `IDLE_POWER_SAVING_MS` — kicks in after 3s idle, exactly the state the BLE
  peripheral spends most of its life in). `BlePeripheralManager`'s
  `begin()`/`end()` need to wrap NimBLE init/deinit in a
  `HalPowerManager::Lock` (the same RAII guard `ActivityManager::renderTaskLoop()`
  already takes around `render()`), not the advertising/connected period as
  a whole — that would defeat the point of Idle power-saving for however
  long BLE just sits there advertising.
- Also worth carrying over: `BleKeyboardHost::begin()` sets
  `NimBLEDevice::setMTU(23)` (the BLE legacy minimum) specifically to avoid
  large per-connection GATT buffers — relevant to the MTU decision below,
  since our Command/Status characteristics carry JSON, not 23-byte HID
  reports.

## RAM and performance are goals, not side effects

Two things to hold onto through every part of this, not just the NimBLE
integration itself:

- **Our own code should cost close to nothing beyond NimBLE's fixed ~57 KB.**
  The dispatcher, the command structs, the state machine — keep these
  fixed-size and stack-allocated where at all possible. This codebase
  already treats `std::vector`/`std::string` heap churn as dangerous under
  `-fno-exceptions` (an OOM there is an instant `abort()`, per PR #2527's
  own diagnosis) — the BLE module should not add a second source of the
  same failure. Prefer fixed-size buffers and avoid dynamic allocation on
  the command hot path.
- **Performance is part of the spec, not a nice-to-have.** A command that
  takes several seconds to acknowledge will read as broken even if it
  eventually works. Budget for it explicitly — see targets below.

### Targets

| What | Target | Why this number |
|---|---|---|
| Heap added by our own code (dispatcher, state, buffers), beyond NimBLE's own ~57 KB | ≤ ~5 KB | NimBLE's cost is fixed and already tight against the ~100 KB start-gate; our own bookkeeping shouldn't eat further into the 40 KB the EPUB renderer needs |
| Advertising current draw at rest | ≤ ~0.5 mA (favor a longer interval, e.g. 1 s+, over faster discovery) | "As small as possible" — this runs all day in Idle, not just during an active session |
| Time from advertising visible → first command acknowledged | < 1 s at normal proximity | Below this, phone control reads as instant; above it, it reads as unreliable |
| Soak test before shipping phases 1–3 | Multi-hour idle-with-BLE-advertising run, zero heap-related aborts | Proves the ~70 KB Idle-state gate and 30 s cool-down actually hold on this firmware's real feature set, not just CrossPoint's |
| Soak test before shipping phase 4 (page-turner) | Multi-hour active reading session with a bonded remote connected, zero crashes, heap-floor auto-retry observed working at least once | Mirrors the exact scenario PR #2527 was written to fix |

These are starting numbers, not settled ones — tighten or loosen them once
the first real measurements come back, and write the actual numbers into
this doc when they do.

## Device state machine

Because Wi-Fi and BLE don't run together, the device is always in exactly
one mode:

- **Idle** — home, menus, sleep screen. BLE peripheral, advertising,
  reachable by the phone. Wi-Fi off.
- **Wi-Fi active** — downloading a book, syncing position, provisioning
  handoff. BLE off. Brief and task-scoped; returns to Idle or Reading after.
- **Reading** — a book is open. BLE central, connected to a bonded remote if
  one is paired. Wi-Fi off. Not reachable by the phone in this mode. Drops
  to **Paused (low memory)** below the heap gate; torn down before sleep.

A book-fetch command received in Idle makes the device briefly unreachable
over BLE while it steps into Wi-Fi mode. That's expected — the phone side
will show this as "sending," not "disconnected."

## Security

**Resolved (2026-08-09, phone/server conversation): a new, device-scoped
BLE token — not `AppToken`, and not the account password.** The correction
below (this firmware has no `AppToken` concept) stands as the finding that
forced the decision; this is the decision.

Sending the raw account password over BLE was considered and rejected: this
project specifically moved the phone *off* raw passwords onto `AppToken` so
the real password never has to live on a device repeatedly, and reusing the
password here would undo that for the same device class it was meant to
protect. Reusing `AppToken` itself doesn't work either, since nothing here
can currently mint, store, or validate one.

The resolution: foulad-ebooks mints a **separate, device-scoped BLE
token** at provisioning time — same shape as `AppToken` (scoped,
revocable, independent of the account password) but issued *to the
device*, not the phone. The device persists it the same way it already
persists its username/password today (new field, same storage pattern —
no new persistence mechanism needed). The server also exposes the current
value to the account's other phones (via `/api/app/devices` or
equivalent), so a phone presenting it over the Auth characteristic is
comparing against a value the device already has locally — **no live
server round-trip needed at connect time**, which matters since BLE is
supposed to work before Wi-Fi does. Foulad-ebooks work to track: mint +
expose this token; not yet scoped or scheduled.

**Original correction, preserved for context:** this firmware has no
`AppToken` concept — grep confirms zero references anywhere in
`src/`/`lib/`. The device's *actual* existing HTTPS credential, used
everywhere it talks to the Foulad eBooks/Midad server
(`FouladDeviceTracking.cpp`'s `registerDevice()`, `reportReadingStats()`,
`uploadDebugLog()`, and every OPDS fetch in `OpdsBookBrowserActivity.cpp`),
is a plain **username/password pair**, persisted on-device and passed
straight through `HttpDownloader::postJson(url, body, username, password,
response)`. That pattern is exactly what the new BLE token reuses —
same storage shape, different credential.

Two allowed connection states, nothing else:

- **Unclaimed device (no account bound):** any phone running Midad may
  connect, but only to provision — hand over Wi-Fi credentials and a claim
  request. Same trust boundary as today's QR sign-in; the account link is
  still approved server-side, BLE just replaces the camera as transport.
- **Claimed device:** only a phone signed into *that same* account may send
  commands. The phone presents the device-scoped BLE token (see Security
  resolution above) over the BLE Auth characteristic; the device compares
  it against the value it persisted at provisioning, locally, no server
  round-trip needed. No plaintext-over-the-air concern beyond what HTTPS
  already accepts today regardless: BLE Secure Connections pairing (LE
  Secure Connections, not Just Works — this exchange needs to resist a
  passive eavesdropper, unlike the page-turner's HID reports) encrypts the
  link before Auth is ever written.

A command from a mismatched or missing credential is dropped silently, same
as an unauthenticated HTTP request is refused today.

## Command protocol — designed to grow

One characteristic for commands, not one per feature. A command is:

```json
// phone → device
{ "cmd": "book.fetch", "v": 1, "payload": { "book_id": 835 } }

// device → phone (notify), in progress
{ "cmd": "book.fetch", "state": "downloading", "progress": 0.4 }

// device → phone, failed
{ "cmd": "book.fetch", "state": "failed", "reason": "no_internet" }

// device → phone, unrecognized command
{ "cmd": "shelf.reorder", "state": "unsupported" }
```

One dispatcher on the device owns this characteristic and routes by `cmd`
string to a registered handler. Anything it doesn't recognize gets an
explicit `unsupported` reply, not silence — an older reader talking to a
newer phone app should fail as "needs a firmware update," not hang. `v`
exists so a command's payload can change shape later without breaking
readers still on the old one. Failure replies share one small fixed
vocabulary of `reason` codes (`no_internet`, `unauthorized`, `timed_out`,
`unsupported`, …) rather than each handler inventing its own — a future
command reuses the set instead of adding a new shape.

**A new use case, later, should be a new `cmd` name and a new handler
function — never a new characteristic, never a phone-side BLE plumbing
change, never a re-pair.**

### GATT layout (concrete, added 2026-08-09 — needed before writing code)

The doc named three characteristics but never pinned down UUIDs, ATT
properties, or the MTU/fragmentation question. This is what Phase 1 is
built against; **the UUIDs below need to be agreed with the Midad phone
side before Phase 1 is usable end-to-end** — they're proposed, not
negotiated yet, and can change freely until a phone build depends on them.

- **Service UUID:** `fa01ade0-b1e0-4d1d-a000-000000000001` (placeholder,
  custom 128-bit — no registration needed for a private app-specific
  service, but the phone app must use the identical value or it can't find
  the device while scanning).
- **Auth characteristic** — `...0002`. Write-only (no read, no notify: a
  credential has no reason to be readable back, and nothing async happens
  here — the result comes back over Status). Payload:
  `{"username": "...", "password": "..."}` today (see Security section's
  correction above re: `AppToken`).
- **Command characteristic** — `...0003`. Write (phone → device commands)
  + Notify (device → phone: progress and final state, per the JSON shapes
  above). One characteristic both directions, matching "one characteristic
  for commands, not one per feature."
- **Status characteristic** — `...0004`. Read + Notify. Current
  `{"state": "idle" | "advertising" | "connected" | "paused_low_memory"}`
  — lets the phone poll on connect (Read) without waiting for the next
  Notify, and lets it show the doc's honest PAUSED state rather than a
  silent drop.

**MTU:** `BleKeyboardHost` deliberately minimizes to 23 bytes (HID reports
are tiny, and every byte of GATT buffer matters on this chip — see the
Hardware reality correction above). Our JSON payloads don't fit: even the
example `book.fetch` command is already ~50 bytes, and a longer
`settings.push` payload will exceed the legacy 20-byte ATT payload (23-byte
MTU minus the 3-byte ATT header) by a wide margin. Negotiate a larger MTU
(`NimBLEDevice::setMTU(185)` is a common middle ground — fits comfortably
inside one radio packet on BLE 4.2+ controllers without reserving the full
517-byte NimBLE maximum) and cap outgoing JSON at whatever that leaves
after the ATT header, rather than implementing multi-write fragmentation
for Phase 1. If a payload doesn't fit (a very long book title in a
`book.fetch` failure reason, say), that is itself a "make this command
`unsupported`-adjacent" case, not a reason to build a reassembly protocol
before there's a real command that needs it.

## What's actually new work here

No peripheral/GATT-server code exists anywhere in this ecosystem yet —
every BLE fork found (CrumBLE, `crosspoint-reader-ble`), and this
firmware's own in-tree `freeink-sdk/libs/network/BleKeyboardHost`, only
implement the opposite role (central, for HID remotes). The peripheral
side (Phases 1–3, phone control) has no prior art to port from and is the
real new work. The central side (Phase 4, page-turner) is a different
story than originally written here — see the corrected item 4 below.

Suggested shape, following this codebase's existing conventions
(`Activity`/`ActivityManager` lifecycle, `lib/hal/` for hardware-facing
components):

1. **`lib/hal/BlePeripheralManager.{h,cpp}`** (new) — GATT server: Auth,
   Command, Status characteristics (concrete UUIDs/properties/MTU in the
   GATT layout section above); owns NimBLE init/deinit, the heap gate
   (~70 KB Idle-state, see Hardware reality above), the 30 s cool-down, and
   the Idle/Paused state. `begin()`/`end()` wrap NimBLE init/deinit in a
   `HalPowerManager::Lock` (same RAII guard `ActivityManager::renderTaskLoop()`
   already uses around `render()`), per the CPU-frequency constraint found
   in `BleKeyboardHost::end()`'s own doc comment — see Hardware reality
   correction above. NimBLE callbacks run on the NimBLE host task, not the
   main/render task (mirrors `BleKeyboardHost`'s own spinlock-guarded-ring
   pattern for crossing that boundary) — any UI-visible state this manager
   updates needs the same `RenderLock` discipline the rest of this codebase
   already uses for cross-task activity state.
2. **A command dispatcher** — registry of `cmd` string → handler. First
   three handlers: `settings.push`, `book.fetch`, `wifi.provision`. A fourth,
   `firmware.check`/`firmware.update`, is cheap to add once the dispatcher
   exists — this firmware already does OTA over Wi-Fi
   (`README.md`), this just makes it phone-triggerable rather than only
   on the device's own schedule. (Real prior art: `crosspoint-reader` PR
   [#2119](https://github.com/crosspoint-reader/crosspoint-reader/pull/2119)
   shipped OTA-over-BLE, closed as out-of-scope for CrossPoint's roadmap but
   measured at only ~15 s slower than the serial path — the mechanism works.)
3a. **Status-bar indicator** — added 2026-08-09, per direction from the
    Midad side: a small icon next to the battery indicator in
    `BaseTheme::drawHeader()` (used by every non-reader screen — Apps,
    Settings, Home, …; the reader's own `drawStatusBar()` is separate and
    doesn't need this, since Reading state is BLE-central for the
    page-turner, not the peripheral role this icon represents) whenever the
    peripheral is actually advertising or connected. Tracks
    `BlePeripheralManager`'s live state, not just
    `CrossPointSettings::bleEnabled` — the setting can be on while the
    radio is torn down (WiFi-active state, cool-down, Paused-low-memory),
    and showing the icon then would contradict this doc's own "show PAUSED
    honestly" principle. No `UIIcon` bitmap asset exists for Bluetooth;
    hand-draw a small glyph the same way `drawBatteryLightningBolt()`
    already hand-draws the charging bolt, rather than adding a new bitmap
    to the icon asset pipeline for one small header glyph.
3. **Settings toggle** — corrected 2026-08-09, per direction from the
   Midad side: goes under **Apps** (`AppsActivity`, `src/activities/apps/`),
   not general Settings — labeled **"Midad BLE"**, defaults **on**
   (`CrossPointSettings::bleEnabled`, `uint8_t = 1`, matching the existing
   `xEnabled` field convention — see `gymEnabled`/`tasbihEnabled`/etc.).
   Shape-wise this doesn't fit `AppsActivity`'s existing `AppEntry` pattern
   cleanly: every current entry (Files, Quran, Games, …) is a *launcher* —
   pressing it opens a sub-activity, enabling the app first if needed. "Midad
   BLE" is a live radio switch, not a destination — pressing it should just
   flip `bleEnabled` and repaint in place, no navigation. Needs either a
   small special case in `AppsActivity::launch()`/`render()` (compute the
   tile label dynamically — `"Midad BLE: On"` / `"Midad BLE: Off"` — for
   this one entry instead of the static `I18N.get(entry.label)` every other
   entry uses), or a distinct `AppEntry` variant if a second toggle-style
   tile shows up later. Don't build the general mechanism speculatively —
   one special case is fine until there's a second one.
4. **Remote page-turner — corrected 2026-08-09, cheaper than originally
   scoped.** `freeink-sdk/libs/network/BleKeyboardHost` (see the Hardware
   reality correction above) already **is** this — HID central role,
   Just-Works bonding, auto-reconnect to a bonded device, and a
   `SpecialKey` enum that already has `PageUp`/`PageDown`. This phase is now:
   enable `FREEINK_CAP_BLE_HID_HOST`, add `h2zero/NimBLE-Arduino@^2.3.8` to
   `lib_deps`, wire `BleKeyboardHost::begin()`/`end()` into the Reading-state
   lifecycle (device state machine above: BLE-central only while a book is
   open, torn down on sleep per the CrumBLE issue #44 finding), and map
   `KeyEvent`/`SpecialKey::PageUp`/`PageDown` onto the existing page-turn
   input handling. `crosspoint-reader-ble`'s `BluetoothHIDManager` is still
   worth a skim for its 5-minute inactivity timeout and heap-floor
   auto-retry behavior (this doc's own state-machine section already covers
   the heap-floor part via PAUSED), but it's no longer the thing to port
   code *from* — `BleKeyboardHost` is.

## Suggested order

1. Provisioning — the entry point, and the clearest standalone win (no more
   fumbling Wi-Fi credentials onto this screen with no keyboard).
2. Settings push — small, low-risk, proves the peripheral/GATT plumbing and
   the account-token check end to end.
3. Book-fetch command — exercises the Idle → Wi-Fi → Idle transition for
   the first time.
4. Remote page-turner — parallel track; mostly enabling and wiring up the
   SDK's existing `BleKeyboardHost`, not new BLE code (see corrected item 4
   above).
5. Firmware update trigger — cheapest addition, once the dispatcher exists.

## What to expect from the phone side

Midad already has the pieces this depends on: `AppToken`-based auth on the
phone/server side, and a client architecture that can take a new BLE
dependency. Whether that `AppToken` becomes what's sent over the Auth
characteristic, or the device keeps presenting the username/password it
already uses over HTTPS (see the Security section's correction above), is
the actual open question to resolve in that conversation — "same mechanism,
not new" is true of the phone side today, not yet confirmed as true
end-to-end. The phone-side BLE integration is comparatively small and can
follow once there's a device to actually connect to — it doesn't block you
starting on #1–3 above.

## Open questions to flag back

- Real heap measurements on this codebase specifically, with BLE running
  and the dictionary/theme/game features also present — do the CrossPoint
  numbers actually hold here, or does this firmware's larger feature set
  move the gate?
- Whether `book.transfer_direct` (small text-only books straight over BLE,
  as a fallback when Wi-Fi has failed repeatedly) is worth adding later —
  not in scope now, but the command envelope already has room for it if
  Wi-Fi reliability on this hardware turns out to be as much of a problem
  in the field as `crosspoint-reader` PR
  [#2119](https://github.com/crosspoint-reader/crosspoint-reader/pull/2119)'s
  discussion suggests it was for at least one user.
- A second future direction, same shape as the one above but the other
  data: `sync.push` — the device hands its current reading position/stats
  to the phone over BLE, and the phone relays them to the server, for a
  device that has BLE-range to a phone but no working Wi-Fi of its own at
  that moment. Not in scope now; the command/dispatcher design already
  accommodates a device-initiated or phone-polled variant without changing
  the envelope.

**Added 2026-08-09, from reviewing this doc against the real codebase
before starting Phase 1** (see the inline corrections above for the
reasoning behind each):

- **Service/characteristic UUIDs are placeholders** (`fa01ade0-...0001`
  through `...0004` in the new GATT layout section) — need sign-off from
  the Midad phone side before Phase 1 is connectable end-to-end. Cheap to
  change on the device side until then; expensive once a phone build ships
  with them baked in.
- ~~`AppToken` vs username/password for the Auth characteristic~~ —
  **resolved 2026-08-09**, see Security section: neither. A new
  device-scoped BLE token, minted by foulad-ebooks at provisioning. That
  server-side work isn't scoped or scheduled yet — flagging it as a real
  dependency for Phase 1 to actually be usable end-to-end, not just for
  the device side to compile.
- **The Idle-state (~70 KB) vs Reading-state (~100 KB) heap-gate split**
  proposed in Hardware reality is reasoned from this doc's own numbers, not
  measured — flagging it explicitly so it doesn't quietly become "the"
  number without a real soak test backing it up.
- **MTU target (185 bytes, proposed in the GATT layout section)** is a
  starting guess balancing JSON payload size against NimBLE's per-connection
  buffer cost — untested on this specific radio/controller combination.
