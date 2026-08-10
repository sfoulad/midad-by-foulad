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
  here — the result comes back over Status). Payload, per the Security
  section's resolution: `{"ble_token": "..."}` for a claimed device (the
  device-scoped token minted by foulad-ebooks at provisioning, compared
  locally against what the device persisted then). **Not yet meaningful for
  Phase 1**, which only ever runs against an unclaimed device (no token
  exists to check yet) -- this characteristic exists in the GATT layout
  from day one per the doc's own "never a new characteristic" design goal,
  but Phase 1's dispatcher doesn't consume a write to it (see the
  implementation-status section at the bottom).
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
   fumbling Wi-Fi credentials onto this screen with no keyboard). **Code
   complete, compile-verified, not device-tested** — see "Phase 1
   implementation status" below. The connect-feedback field this needs
   (`wifi_last_attempt` on Status) is resolved but not yet built; add it
   before calling Phase 1 finished, not as part of Phase 2.
2. Settings push — small, low-risk, proves the peripheral/GATT plumbing and
   the account-token check end to end.
3. Book-fetch command — exercises the Idle → Wi-Fi → Idle transition for
   the first time.
4. Remote page-turner — parallel track; mostly enabling and wiring up the
   SDK's existing `BleKeyboardHost`, not new BLE code (see corrected item 4
   above).
5. Firmware update trigger — cheapest addition, once the dispatcher exists.

## What to expect from the phone side

Midad already has the pieces Phase 1 depends on: `AppToken`-based auth for
its own HTTPS calls, and a client architecture that can take a new BLE
dependency. Per the Security section's resolution, the BLE Auth
characteristic itself uses neither `AppToken` nor the account
password -- a new device-scoped BLE token, which foulad-ebooks needs to
mint and expose (e.g. via `/api/app/devices` or equivalent) before a
*claimed*-device flow (Phase 2's `settings.push` and beyond) is usable
end-to-end. That server-side work isn't scoped or scheduled yet. It does
NOT block Phase 1: provisioning only ever runs against an *unclaimed*
device, which the Security section's two-state design already exempts from
any Auth check at all. The phone-side BLE integration is comparatively
small and can follow once there's a device to actually connect to — it
doesn't block starting on #1–3 above.

## Open questions to flag back

- Real heap measurements on this codebase specifically, with BLE running
  and the dictionary/theme/game features also present — do the CrossPoint
  numbers actually hold here, or does this firmware's larger feature set
  move the gate?
- **The phone as a relay, generally — not just two special cases.**
  `book.transfer_direct` (small text-only books straight over BLE, as a
  fallback when Wi-Fi has failed repeatedly) and `sync.push` (the device
  hands reading position/stats to the phone, which relays them to the
  server) were noted separately below, but they're the same shape: a
  device with BLE-range to a phone but no working Wi-Fi of its own right
  then, using the phone's already-working connection instead of its own.
  The BLE-token auth mechanism above is already the first real instance of
  this — the phone fetches a server-verified value and hands it to the
  device over BLE — so the pattern isn't hypothetical, it's already load-
  bearing for Phase 1. Worth considering, later, as one general capability
  (e.g. a `relay.request` command carrying an HTTP method/path/body for the
  phone to execute against the server on the device's behalf and hand the
  response back) rather than a growing list of one-off relay commands —
  not scoped now, and needs its own security thinking before it is (a
  general relay is a bigger trust surface than either specific case), but
  worth naming as the direction rather than rediscovering it piecemeal
  each time a new "device has no Wi-Fi" case shows up.
  - `book.transfer_direct`: not in scope now, but the command envelope
    already has room for it if Wi-Fi reliability on this hardware turns
    out to be as much of a problem in the field as `crosspoint-reader` PR
    [#2119](https://github.com/crosspoint-reader/crosspoint-reader/pull/2119)'s
    discussion suggests it was for at least one user.
  - `sync.push`: not in scope now; the command/dispatcher design already
    accommodates a device-initiated or phone-polled variant without
    changing the envelope.

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

## Phase 1 implementation status (2026-08-09)

Code complete and **compile-verified only** — `pio run -e default` (0
warnings/errors), `pio check` (cppcheck, no defects), `pio run -e simulator`
(the X4 desktop target) all clean. **Not device-tested**: no real BLE
hardware or a Midad phone build to connect against exists in this session,
so nothing about actual advertising behavior, connection handling, the heap
gate/cool-down under real fragmentation, or the phone-side protocol has
been exercised. Same discipline this repo already applies to PR #122/#123/
#126 (ESP32-S3 foundation, SDK bump, touch input) — do not treat this as
shippable without a real device + phone pass.

**What shipped**: `lib/hal/BlePeripheralManager.{h,cpp}` (GATT server,
heap gate, cool-down, Off/Advertising/Connected/PausedLowMemory state),
`src/BleCommandDispatcher.{h,cpp}` (routes Command writes by `cmd`,
currently just `wifi.provision`), `CrossPointSettings::bleEnabled` (Apps ->
Midad BLE toggle, on by default, also a live-toggle `AppsActivity` tile),
a "BT" status-bar indicator in `BaseTheme::drawHeader()`, and lifecycle
wiring in `main.cpp` (start/stop gated on `WiFi.getMode() == WIFI_MODE_NULL`
+ not-reading, checked every loop tick rather than hooking every WiFi-
activation call site; teardown-before-sleep in `enterDeepSleep()`, mirroring
the existing WiFi backstop-disconnect pattern there).

**Real findings from actually building this, not present anywhere above
before now:**

- **`^2.3.8` resolved to NimBLE-Arduino 2.5.1** (a real `pio pkg install`,
  not assumed) — the caret range works as expected, no pin conflict.
  Verified the real v2.5.1 API directly from the fetched library source
  before writing any dependent code (`NimBLEServerCallbacks::onConnect/
  onDisconnect` and `NimBLECharacteristicCallbacks::onWrite` all take a
  `NimBLEConnInfo&` in this version; `NIMBLE_PROPERTY::WRITE_ENC` exists and
  is what Auth uses to require an encrypted link at the GATT layer, not just
  an app-level check; `getValue()` returns a `NimBLEAttValue` with
  `data()`/`size()`; advertising interval setters are 0.625ms units).
- **A previously-unmeasured static RAM cost, separate from NimBLE's own
  documented ~57 KB *dynamic* (runtime `init()`) cost**: merely linking
  NimBLE-Arduino into the firmware — before it's ever initialized, even
  with `bleEnabled` off — adds a **fixed +~27.6 KB to link-time DRAM**
  (bss+data, measured via `pio run`'s own memory summary: 116219 -> 143885
  bytes total DRAM used, before vs. after this branch) and **+~240 KB to
  flash** (4486335 -> 4716573 bytes). This is a permanent tax on every
  build that includes the dependency at all, regardless of whether any
  given user ever turns BLE on — worth weighing if a build variant
  (`slim`?) should exclude it later; not done here since `slim` already
  builds clean with it included and nothing in this doc asked for that
  split.
- **`lib/hal/` never includes `src/` headers anywhere in this codebase**
  (checked directly: zero matches). `BlePeripheralManager` initially tried
  to call `src/BleCommandDispatcher.h` directly from its NimBLE write
  callbacks — wrong layering, and likely wouldn't have resolved under this
  project's default (non-simulator) LDF mode anyway. Redesigned as a poll/
  drain handoff instead (`takePendingAuth()`/`takePendingCommand()`/
  `sendCommandReply()`, spinlock-guarded fixed buffers), the same shape
  `BleKeyboardHost` already uses for its own NimBLE-host-task ->
  main-task boundary (there: a ring of decoded key events drained by
  `popKey()`; here: a single pending write, since Auth/Command are
  request/response, not a continuous stream).
- **The simulator needs an explicit exclusion, not just a guarded
  include.** `src/BleCommandDispatcher.cpp` has no host-backed shim (unlike
  `network/CrossPointWebServer.cpp` etc., which fall back to the
  simulator's own implementations) — BLE peripheral advertising has no
  meaningful desktop equivalent to shim. Added
  `-<BleCommandDispatcher.cpp>` to `[env:simulator]`/`[env:simulator_x3]`'s
  `build_src_filter`, matching the existing network-file exclusion pattern.
  `main.cpp` and `BaseTheme.cpp` each need their own `#ifndef SIMULATOR`
  around the `<BlePeripheralManager.h>` include specifically (unlike
  `HalStorage.h`/`HalGPIO.h`/etc., the simulator has no vendored
  counterpart for this brand-new HAL header under the same name, so
  `lib_ignore=hal` alone isn't enough — it would just fail to resolve).
- **`[env:simulator_x3]` currently fails to build for an unrelated,
  pre-existing reason**: `HalClock::isSystemTimeValid()` is missing from
  that environment's own vendored simulator copy, surfacing in
  `EpubReaderActivity.cpp` (a file this work never touched).
  `[env:simulator]` (X4, the CLAUDE.md testing checklist's primary
  simulator target) builds clean and was used for verification here; the
  X3 simulator gap needs its own local patch per the existing "Local
  simulator patches" convention and is not a BLE regression.
- **"Claimed device" needed a concrete definition Phase 1 could compile
  against**: implemented as "a saved Foulad eBooks `OpdsServer` entry
  exists" (`std::find_if` on `FOULAD_EBOOKS_URL` — the exact pattern
  `OpdsBookBrowserActivity`/`ActivityManager`/etc. already use throughout
  this codebase to locate that one entry). `wifi.provision` refuses
  outright (`reason: unauthorized`) on a claimed device rather than
  attempting the BLE-token check the Security section's resolution
  describes — that check is Phase 2 (`settings.push`) work, not built
  here, and half-building it against a token foulad-ebooks hasn't minted
  yet would be worse than an honest refusal.
- **`wifi.provision`'s payload is `{"ssid", "password"}` only** — it does
  NOT yet carry the device-scoped BLE token the Security section's
  resolution describes minting "at provisioning time," since foulad-ebooks
  doesn't mint one yet (that work is explicitly "not yet scoped or
  scheduled" per that section). Once it exists server-side, the payload
  likely needs a third field (e.g. `ble_token`) for the device to persist
  alongside the Wi-Fi credential in the same write. Flagged here rather
  than guessed at now.
- **`wifi.provision` only saves the credential; it does not drive a
  connection attempt.** Actually testing it would tear down BLE (this
  doc's mutual-exclusion design) before a reply could reach the phone —
  there's no way to report "connected" back over a radio that's already
  off. The reply means "saved," not "verified reachable." Relies on this
  codebase's existing `WifiSelectionActivity::tryAutoConnectCredential`/
  `tryNextSavedNetworkFromScan` auto-connect-saved-networks flow to
  actually use the credential the next time Wi-Fi comes up. How the phone
  ever learns definitive connect success (vs. a bad password that fails
  silently later) is a real open question, added below.
- **The status-bar indicator is plain "BT" text, not a hand-drawn
  Bluetooth glyph**, despite the "hand-draw it like `drawBatteryLightningBolt()`"
  note added earlier in this doc. Reason found while actually implementing
  it: that bolt glyph is a simple triangle, hand-verifiable by eye in the
  diff; the Bluetooth bind-rune is intricate enough (merged Hagall + Berkanan
  runes) that getting it subtly wrong produces an unrecognizable zigzag
  instead of a slightly-off logo, and nothing in this session can render
  and visually inspect it (no hardware, and the simulator's framebuffer
  isn't accessible from here). Text is unambiguous and correct by
  construction; a proper glyph is a fine follow-up once someone can
  actually look at it on a screen.
- **`bleEnabled` is registered in `SettingsList.h`'s generic settings
  table** (`SettingInfo::Toggle`), not just as the special-cased
  `AppsActivity` tile — same as every other app's enable flag
  (`gymEnabled`, `tasbihEnabled`, etc.), for the same free persistence and
  server-settings-push handling (`FouladDeviceTracking.cpp`'s
  `applyToggle`) those already get. The live-toggle Apps tile is
  additional UI on top of that, not a replacement for it.

**Resolved (2026-08-10), from the phone/server conversation, against the
three questions this implementation pass raised:**

1. **Wi-Fi connect feedback:** build the re-advertise-and-poll version, not
   silent "saved." The device goes back to Idle → BLE-peripheral after a
   connect attempt either way (success or failure both return it to Idle,
   per the state machine); Status should carry the outcome of the last
   attempt (e.g. add a `wifi_last_attempt` field: `"ok"` / `"failed"` /
   `null` if none yet) so the phone can poll after reconnecting rather than
   guessing from silence. A provisioning flow that regresses below what
   today's QR flow already does (a live poll-until-approved loop) isn't
   worth shipping to save the extra state field. Phase 2 work, not a
   Phase 1 blocker — Phase 1's "saved" reply is a legitimate stepping
   stone, not a dead end to redo.
2. **`ble_token` delivery:** not through `wifi.provision`. Once the device
   is online (the whole point of provisioning), account-claiming and the
   token go through the *existing* HTTP device-login flow — the same one
   today's QR sign-in already uses, `/api/device-login/start` +
   `/poll`/`approve` (see foulad-eink's own `FouladDeviceLogin.cpp`). No
   new BLE payload field, no new `device.claim` command. `wifi.provision`
   stays exactly what's already built: get the device onto Wi-Fi, nothing
   more. The token becomes part of whatever that existing exchange already
   returns once foulad-ebooks mints it — a foulad-ebooks/foulad-eink
   question for whenever that work is scoped, not a BLE-protocol one.
3. **The +27.6 KB DRAM / +240 KB flash static tax:** accept it fleet-wide
   for now, not a build-variant split. BLE-by-default was already a
   deliberate call — "reachability is the point of the feature" — and
   ~7% of the ~380 KB RAM ceiling, while real, isn't disqualifying on its
   own. Explicitly **not settled with confidence**: this is a judgment
   call made without real device flash/RAM headroom numbers in hand.
   Revisit if a hardware pass shows the margin is tighter than this
   reasoning assumes — this is the one of the three most likely to need
   walking back.

## Phone side (foulad-one), Phase 1 — built 2026-08-10

Against the real, committed `BlePeripheralManager.h`/`BleCommandDispatcher.cpp`
from `e0a832bc` (read in full, not assumed), not the earlier draft protocol.

- **New screen**: `BleProvisionScreen` (`lib/features/devices/ble_provision_screen.dart`).
  Scans for the service UUID, lists nearby readers, takes SSID/password,
  sends `wifi.provision`, shows the device's `ok`/`failed` reply (with the
  three known `reason` codes — `unauthorized`, `invalid_payload`,
  `storage_error` — turned into real sentences, and an honest fallback for
  anything else). Entry point: `DevicesScreen`'s existing "Add device" FAB
  now opens a chooser sheet (QR vs. Bluetooth) instead of jumping straight
  to the QR scanner.
- **Matches the resolved question above exactly**: this does *not* try to
  claim the device or carry a `ble_token`. On success it just offers
  "Scan its QR code now," handing off to the existing `ScanDeviceScreen`/
  `/api/device-login` flow once the reader is online — same conclusion
  reached independently on this side before rereading the resolution above
  and finding it already written down.
- **Low-level client**: `lib/data/ble/midad_ble_client.dart`, built on
  `flutter_blue_plus`. Enforces `kMaxPayloadLen` (160 bytes) client-side
  before writing, so an oversized SSID+password pair fails with a clear
  message instead of silently truncating at the GATT layer.
- **Finding, not yet a question**: Command (`...0003`) is
  `WRITE | NOTIFY` — no `_ENC` — while Auth (`...0002`) is `WRITE_ENC`.
  Nothing stops a central from writing `wifi.provision` (SSID + Wi-Fi
  password) to Command over an unencrypted link if it never touches Auth
  first. Phase 1's Auth write is unverified server-side either way, so
  this isn't gated on any dispatcher change — the phone can just always
  write a throwaway payload (`{}`) to Auth before its first Command write,
  which is enough to make CoreBluetooth/BlueZ/Android negotiate pairing
  and an encrypted link before the credentials go over the air. Implemented
  that way in `MidadBleClient.connect()` (see `_forceEncryptedLink()`).
  Mentioning it here in case there's a firmware-side reason to eventually
  make Command `WRITE_ENC` outright rather than relying on the phone
  behaving — no action needed on this repo's side to unblock testing.

Not yet tested against real hardware — this is the phone half being ready,
per the ping in the last relayed message. Ping back when there's a build
to test against.

## Hardware validation (2026-08-10) — device side only, corrected

**Originally logged here as "validated end-to-end," which overclaimed —
correcting before anyone builds on it.** What was actually tested on a
real Xteink X3: the firmware's own side only — device performance with
BLE running, and the "Midad BLE" on/off toggle (`AppsActivity`'s live
tile / `bleEnabled` setting). Both reported as working normally.

**Not yet tested, on any hardware:** the phone side at all.
`MidadBleClient`/`BleProvisionScreen` in foulad-one have not been run
against a real device — no scan, no connect, no `wifi.provision` send, no
`ok`/`failed` reply, nothing. The "Phase 1 validated end-to-end" claim
above was wrong; strike it. Phase 1's phone↔firmware handshake is still
unverified beyond code review of both sides.

Open next: the actual phone-to-device test (foulad-one's Add device → Set
up over Bluetooth screen, against an X3 with no saved Wi-Fi), then the
Phase 2 work above (`wifi_last_attempt` status field, the device-scoped
BLE token once foulad-ebooks mints it).

## New task: per-device advertised name (multi-device households)

Raised by Sameh: with more than one Xteink nearby, foulad-one's scan list
is currently useless for telling them apart. Confirmed why —
`NimBLEDevice::init("Midad")` at `lib/hal/BlePeripheralManager.cpp:141`
advertises the literal string `"Midad"`, identical on every device. No
serial, no MAC, nothing — and the Status characteristic (`...0004`)
doesn't carry one either; nothing in Phase 1 writes to it with identifying
info. Two readers in range render as two indistinguishable "Midad" rows.
This is a real regression from the QR flow, which shows the actual serial
for confirmation (`ConfirmDeviceSheet`) before pairing — over BLE right
now there's no equivalent check, so tapping the wrong entry (e.g. a
neighbor's unclaimed reader in an apartment) would hand it your Wi-Fi
password.

**Decided (Sameh, 2026-08-10): fix it at the source — advertise a unique
per-device name**, not a phone-side workaround (RSSI-sorting was on the
table and explicitly turned down in favor of the real fix).

Suggested approach, anchored to what already exists rather than inventing
a new scheme: `FouladDeviceTracking.cpp:443`'s `getSerialNumber()` already
derives `"XTE-" + WiFi.macAddress()` (colons stripped) for the QR-confirm
serial and server registration. Advertise `"Midad-" + <last 4-6 hex chars
of that same MAC>` instead of the bare `"Midad"` literal, so the BLE name
correlates with the serial a user may already recognize from Devices/QR
elsewhere, rather than being a second, unrelated identifier.

One thing to verify while building this, not assumed here: whether
`WiFi.macAddress()` reads cleanly from `begin()`'s context when the device
is in Idle → BLE-peripheral state with Wi-Fi intentionally off (this
doc's mutual-exclusion design). On ESP32 the MAC is normally readable from
efuse regardless of radio state, but that's worth confirming against this
codebase's actual `WiFi.macAddress()` behavior rather than assumed.

No phone-side change needed to consume this — `BleProvisionScreen`
already prefers `device.platformName` / `advertisementData.advName` over
a hardcoded fallback, so a real per-device name shows up automatically
once advertised. Once names are unique, tapping the correct list entry
functions as the confirmation step itself (comparable to pointing the
camera at the right QR code) — no separate confirm-sheet needed unless a
later review decides otherwise.

## Live hardware debugging session (2026-08-10) — real bug found and fixed

First real end-to-end debugging pass, on a real Xteink X3 connected via USB
(`pio run -t upload` + a raw pyserial reader script, since `pio device
monitor` needs an interactive TTY it doesn't have when scripted). Added
`CMD:BLE_ON` / `CMD:BLE_OFF` / `CMD:BLE_STATUS` to `main.cpp`'s existing
serial command handler (alongside `CMD:SCREENSHOT`) so `bleEnabled` and
`BlePeripheralManager`'s live state can be driven and inspected over
serial without touching the device's physical buttons — kept as permanent
tooling, not removed after this session.

**Symptom reported:** the Apps "Midad BLE" tile toggled on/off/on
correctly, but the "BT" status-bar indicator never appeared.

**Real bug found, not a false alarm:** `BlePeripheralManager::begin()`'s
guard clause was `if (state_ != State::Off) return true;` — this treated
`PausedLowMemory` (the state `poll()` enters after tearing down for low
memory) the same as "already actively running," so it returned early
without ever re-attempting. Once BLE paused for low memory once, it could
**never retry again**, regardless of how long the cool-down ran or how
much heap recovered afterward. Confirmed live: `CMD:BLE_STATUS` reported
`bleState=3` (PausedLowMemory) sitting there indefinitely across multiple
checks, heap unchanged, zero further attempts.

**Fixed:** the guard now only short-circuits on `Advertising`/`Connected`
(genuinely already running); `Off` and `PausedLowMemory` both fall through
to the existing cool-down + heap-gate checks. Verified live, full cycle,
after the fix: boot → brief early-boot success (heap momentarily above
the gate before Home finishes loading its own resources) → `poll()`
correctly tears down for low memory → `begin()` correctly refuses during
the 30s cool-down (`[DBG] [BLE] begin() refused: cool-down active`,
repeating every tick) → cool-down expires → `begin()` correctly falls
through to the heap-gate check and refuses there instead
(`[DBG] [BLE] begin() refused: heap gate not met (free=45136 < 71680)`) —
every stage of the state machine now reachable and behaving as designed.
No crash anywhere in this cycle; the whole point of the gate/cool-down/
PAUSED design held up under real fragmentation, once the retry bug itself
was fixed.

Second real finding, answering this doc's own "unmeasured" heap-gate
numbers for the first time: **on this X3, idle free heap depends heavily
on which screen is showing.** Measured directly, not estimated:
- **Home screen** (cover art + font caches resident): **~45 KB** free,
  settled/stable across 100+ seconds of observation. Below both the
  proposed ~70 KB Idle-state gate *and* NimBLE's own bare ~57 KB
  requirement — BLE cannot run here except in the brief pre-load window
  right after boot.
- **Settings/Apps screens** (after navigating away from Home): **~77 KB**
  free, also stable. Above the gate — BLE can run and stay up here.

This means the ~70 KB gate isn't wrong in general, but it's not
sufficient *on Home specifically* on this device — the "Idle state" this
doc's state machine describes isn't heap-uniform across the screens that
make it up. Not yet decided what to do about this (leave as-is, since
"Midad BLE" itself lives on the Apps screen and a user toggling it is
almost certainly not sitting on Home at that exact moment; or investigate
whether Home's own resident footprint can shrink; or split the gate
further by screen rather than just by Idle/Reading) — flagging it rather
than picking one silently.

The originally-reported symptom (no icon) is now fully explained by the
combination of both findings: on a **fresh boot with `bleEnabled` already
persisted true**, BLE started, immediately got torn down by the Home-heap
finding above, and — before this fix — got permanently stuck in
`PausedLowMemory` with no way back, so the icon (which only shows on
`Advertising`/`Connected`) correctly never appeared. It wasn't the
repaint-lag fix from earlier this same day that was insufficient; it was
this deeper bug preventing the state from ever reaching `Advertising`
again after the first pause.

## Second live debugging session (2026-08-10) — the real blocker: NimBLE's own init cost

Follow-up session, same USB/serial setup, this time navigating deliberately
to Settings > Apps (per the Home-vs-Settings finding above) and attempting
an actual phone pairing test from foulad-one on iPhone. Symptom: still no
"BT" icon, and the iPhone's Bluetooth scan never found the device at all —
a different, deeper problem than the `PausedLowMemory` retry bug fixed
earlier today.

**Root cause, confirmed from the live log** (two occurrences, identical):

```
[466625] [DBG] [BLE] begin(): advertising (free heap=12768)
[466625] [ERR] [BLE] poll(): heap dropped below gate while running (free=12768), tearing down
[466635] [DBG] [BLE] end(): torn down (free heap=71412)
```

`begin()`'s heap-gate check (`BlePeripheralManager.cpp:128`) runs *before*
`NimBLEDevice::init()`. At that point free heap was ~77-80 KB (Settings/Apps,
per the finding above) — comfortably over the 71680-byte gate, so the check
passes. But everything `begin()` does next —
`NimBLEDevice::init()` + `createServer()` + `createService()` + three
characteristics + `advertising->start()` — itself allocates roughly
**65 KB of heap**, which is not accounted for anywhere in the pre-flight
check. By the time `state_` is set to `Advertising` and the "advertising"
log line fires, free heap has already collapsed to ~12.8 KB. The very next
`poll()` call, same tick (10ms later per the timestamps above), sees that
12.8 KB is under the gate and tears everything down immediately.

So the state machine's own logic worked exactly as designed at every step
— the problem is that `Advertising` never lasts longer than a single main
loop iteration. That's under any realistic BLE scan window on either
platform, which is why the phone never saw the device, and why the "BT"
icon (drawn only on a repaint, which for e-ink takes 1-2s) never had a
chance to appear before the state reverted.

**Why the earlier +27.6 KB estimate didn't catch this**: that number (see
"Phase 1 implementation status" below) was measured as NimBLE-Arduino's
*static link-time* footprint (`.data`/`.bss` added to the binary), not its
*runtime heap* allocation once `init()` actually spins up the host task,
GATT tables, ATT buffers, and advertising payload. Those are two different
costs; only the second one matters for this gate, and it was never
directly measured until this session.

**The problem this creates for the whole gate design**: `kHeapGateBytes`
(71680) is used both as the pre-flight threshold in `begin()` and as the
"still safe while running" floor in `poll()` — but a single number can't
correctly serve both roles once there's a ~65 KB step-cost in between them.
Raising the constant doesn't fix this by itself: the pre-flight check would
need something on the order of *(safe running floor) + (NimBLE's own
~65 KB init cost) + margin* — call it 100 KB+ free heap ISO *before*
`begin()` even runs. The highest free heap measured anywhere on this
device so far, including Settings/Apps, is ~77-80 KB. There is currently
no screen on this device where a correctly-sized pre-flight check would
ever pass.

**Not yet attempted**: whether NimBLE-Arduino's ~65 KB runtime footprint
can be reduced via `sdkconfig` tuning (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS`,
bonding/pairing store size, disabling unused extended-advertising or mesh
options, ATT buffer counts) rather than by finding 100 KB+ of free heap
elsewhere on a 380 KB-RAM device, which is very unlikely to be won back
from other subsystems. This is flagged here rather than fixed blind —
shrinking NimBLE's own memory use is a real investigation, not a
one-line constant change, and deserves its own session before more device
time is spent on it.

## Research: NimBLE-Arduino memory tuning options (2026-08-10, from Midad side)

Requested by Sameh -- searched for what's actually available to attack the
~65 KB runtime init cost above, since the `MAX_CONNECTIONS=1` /
`ATT_PREFERRED_MTU=185` change already in flight in `platformio.ini` is
real but partial (connection-context and MTU buffers are only part of
NimBLE's allocation). One correction up front: **the option names in raw
ESP-IDF's `Kconfig.in` (`idf.py menuconfig`) aren't all directly usable
here.** This project builds NimBLE through `h2zero/NimBLE-Arduino`'s own
config surface (`src/nimconfig.h`, overridable via `-D` in
`platformio.ini`'s `build_flags` -- same mechanism as the two flags
already added), which exposes a narrower set of the same-named
`CONFIG_BT_NIMBLE_*` macros. Everything below is checked against that
actual file
([nimconfig.h](https://github.com/h2zero/NimBLE-Arduino/blob/release/1.4/src/nimconfig.h)),
not the raw ESP-IDF Kconfig -- a couple of Kconfig options that looked
promising (`BT_NIMBLE_MEMPOOL_RUNTIME_ALLOC`, which per its own ESP-IDF
help text "can significantly reduce memory consumption after mempool
initialization" -- almost exactly this bug) aren't exposed through
NimBLE-Arduino's wrapper at all, and it's unconfirmed whether they're
reachable some other way in this build. Worth a search of its own if the
options below don't get far enough.

**Additional candidates, layered on top of the two flags already added:**

- **`CONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT`** (default 12, ~256 B/block ≈
  3 KB total). Per NimBLE-Arduino's own comment, this pool backs "prepare
  write & prepare responses" and only needs raising for large data over a
  low MTU. Neither BLE role here does that -- `wifi.provision`'s payload
  (≤160 B) fits in a single MTU-185 write, no GATT long-write chunking
  anywhere in this protocol. A candidate to shrink (e.g. to 2-4), though
  the safe floor isn't documented -- NimBLE may use msys_1 for other
  internal purposes beyond app-level prepare-writes, so this needs testing
  down incrementally, not dropped to the minimum blind.
- **`CONFIG_BT_NIMBLE_ROLE_OBSERVER_DISABLED`** / **`_BROADCASTER_DISABLED`**
  / **`_CENTRAL_DISABLED`** -- per-role compile-outs, each documented by
  the library itself as a **flash** saving (~26 KB / ~5 KB / ~7 KB
  respectively) rather than a confirmed heap one, though disabling unused
  subsystems (e.g. observer's scan-result handling) plausibly trims some
  runtime buffers too -- unconfirmed, would need measuring on-device
  either way. **Real tension worth flagging before touching these**:
  `BlePeripheralManager` only needs Peripheral+Broadcaster (advertising),
  but this firmware's *other* BLE consumer -- `BleKeyboardHost`
  (freeink-sdk, Phase 4's page-turner, currently capability-gated off) --
  needs Central+Observer, and Kconfig/nimconfig role flags are compiled
  into one binary, not switchable per the runtime Idle-vs-Reading state
  this doc's design already splits BLE into. Disabling Central/Observer
  now (while Phase 4 is dormant) is a real, honest win today, but it's
  borrowed against Phase 4 -- re-enabling both roles when
  `FREEINK_CAP_BLE_HID_HOST` actually turns on will give this exact
  runtime-heap number back. Don't let a win measured with Phase 4 disabled
  quietly become the assumed number once Phase 4 ships.
- **`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE`** (default 4096 B) -- a
  FreeRTOS task stack allocated once at `NimBLEDevice::init()`, so it's
  part of the exact ~65 KB step-cost this bug is about, not a separate
  concern. Real candidate, but the riskiest one on this list to get wrong:
  under-sizing it doesn't fail cleanly like the other options here, it
  stack-overflows at some unpredictable point during operation -- worse
  than the current honest teardown. If tried, needs incremental reduction
  with real headroom margin, not a single guessed cut.
- **`CONFIG_BT_NIMBLE_MAX_BONDS`** / **`_MAX_CCCDS`** (defaults 3 / 8) --
  small, NVS-backed, minor expected win. Note `MAX_BONDS` shouldn't drop
  to 1 blindly: multiple phones on the same Midad account are expected to
  each bond with a device over time (per the "same account" security
  model earlier in this doc), even though only one is connected at once.
- **`CONFIG_BT_NIMBLE_LOG_LEVEL`** -- already defaults to `5` (NONE) in
  NimBLE-Arduino's own header. Already at the minimum; no further win
  available here, mentioning only so it's not re-investigated.
- **PSRAM-backed allocation** (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`)
  -- moot, this doc's own Hardware Reality section already established no
  PSRAM on this board.

None of this is measured against real hardware yet -- these are candidates
to test incrementally (one flag, one measurement, repeat) against the
same live serial rig the two findings above already used, not a batch of
changes to land at once.

## Third live debugging session (2026-08-10): end-to-end WORKING — full root-cause chain

Same USB rig, plus a new capability: **the Mac itself acted as the phone**
(python + `bleak` over CoreBluetooth), so the entire client side ran
scripted with zero phone involvement. Outcome first: **the complete Phase 1
flow now works end-to-end on real hardware** — scan finds "Midad" with the
correct service UUID, connect succeeds, Status characteristic reads
`{"state":"connected"}`, a `wifi.provision` command written to the Command
characteristic gets a real dispatcher-built JSON reply notified back, and
disconnect returns the device to advertising (`advertiseOnDisconnect`
confirmed working). Verified against both a mid-session `begin()` and a
normal boot-time session.

It took **four stacked root causes** to get there, each masking the next.
In the order found:

1. **One heap number doing two jobs** (fixed earlier this session, above):
   `poll()` now judges a running session against `kRunningFloorBytes`
   (8 KB), not the 70 KB pre-flight gate. Without this, every successful
   start tore itself down within 10 ms.

2. **10 MHz idle clock kills BLE RF while the controller keeps claiming
   it's advertising.** `HalPowerManager::LOW_POWER_FREQ` is 10 MHz;
   Espressif requires ≥80 MHz for RF. After 3 s of no buttons the main
   loop dropped the CPU to 10 MHz and the radio went silent — but
   `ble_gap_adv_active()` still returned true, so nothing device-side
   looked wrong. This is why the device was visible to a phone only while
   someone was actively pressing buttons, and invisible to every scan run
   while it sat idle. Fix: `main.cpp`'s idle branch never engages
   power-saving while `BlePeripheral.isActive()`. (Upstream
   `crosspoint-reader-ble` has the same 10 MHz constant but is
   central-role — scanning/connected during active reading — so they
   never had an idle advertising session to lose.)

3. **NimBLE 2.x does not put the device name in the advertisement by
   itself.** `NimBLEDevice::init("Midad")` only sets the GATT device-name
   characteristic. Until `advertising->setName("Midad")` was added, the
   advertisement carried no name — early phone sightings showed an
   anonymous device. Also added `NimBLEDevice::setPower(ESP_PWR_LVL_P9)`
   (upstream parity; not proven load-bearing on its own but kept).

4. **The zombie guard added to catch #2 then killed real connections.**
   A central's CONNECT_IND stops advertising instantly, milliseconds
   before the host task delivers the connect event that moves `state_` to
   Connected. The first guard saw "Advertising + radio idle" in that
   window and tore the stack down mid-handshake (NimBLE:
   `ble_hs_stop_terminate_timeout_cb, 1 connection(s) still up`). Fix:
   zombie requires **no live connections AND 3 s persistence**
   (`kZombiePersistMs`) before teardown.

Supporting changes in the same pass:
- **`ActivityManager` tears BLE down before every activity `onEnter()`**
  (guarded `#ifndef SIMULATOR`) so a heavy screen (Home's cover art)
  never allocates against a heap NimBLE has already claimed; `main.cpp`'s
  existing lifecycle restarts BLE on the new screen if its heap clears
  the gate. Measured consequence, by design: on Home with BLE resident,
  one 42 KB cover buffer fails gracefully (`[HOME] OOM: cover buffer`,
  logged and skipped) — same graceful-degradation stance upstream takes
  in its reader (`EpubReaderActivity.cpp`'s AA-fallback comment).
- **`CMD:RESTART` serial command** (permanent, alongside
  `BLE_ON/OFF/STATUS`): scripted reboot so boot-path behavior is testable
  without physical access.
- **`platformio.ini`: the `-DCONFIG_BT_NIMBLE_*` flags were removed** —
  measured zero effect (post-init 12752 vs 12768–12916 baseline). Root
  cause confirmed in source: the framework's pre-generated `sdkconfig.h`
  redefines those macros after any command-line `-D` (the build even
  warns), and NimBLE-Arduino's `nimconfig.h` includes `sdkconfig.h`
  before its own `#ifndef` defaults, so framework values win everywhere.
  The research section above stands, but every candidate routed through
  `-D` flags on this framework will be clobbered the same way — a
  different delivery mechanism (framework sdkconfig rebuild) would be
  needed, which changes that cost/benefit sharply.

**Known issues left open, deliberately:**
- **Flash-reset boots only (dev bench, not field):** a boot immediately
  following `esptool`'s RTS reset comes up with the same
  claims-advertising-but-silent radio signature. A clean `ESP.restart()`
  or normal power-on boots into a fully visible session. Not chased
  further — field devices never boot via RTS reset.
- **`deviceIsClaimed()` returned false on the test device** so
  `wifi.provision` accepted and saved a test credential
  (`e2e-test-ssid` — Sameh: forget it in Wi-Fi settings). The device
  appears genuinely logged out of Foulad eBooks, so this is likely
  *correct* behavior, but the claimed-refusal path has therefore never
  been exercised on hardware — test it after logging the device back in.
- **Home cover-OOM repaint loop:** when a cover buffer fails, Home
  repaints roughly every 1.6 s indefinitely (e-ink wear + battery).
  Needs a give-up-after-first-failure latch in HomeActivity.
- **Wi-Fi stays on after leaving network screens**, which keeps BLE off
  (`bleAllowedNow` requires `WIFI_MODE_NULL`) until reboot. Pre-existing
  behavior, now user-visible because BLE advertises its absence.
- **Gate margin is razor-thin on this device:** post-teardown free heap
  on Settings/Apps measured 71.1–83 KB against a 71680-byte gate — one
  refusal was observed at 540 bytes short. If field devices flap here,
  shave the gate a couple KB.
- Phone-side (foulad-one) pairing against this now-working device side:
  still the next real test; the Mac client proved the device end only.

## Phone-validated (2026-08-10, later the same day): real foulad-one flow completed

The real app on a real iPhone completed the full Phase 1 provisioning flow
against the device: found "Midad" in its scan, connected, wrote the auth
token over the encrypted link, sent `wifi.provision` with the user's real
home SSID, and the device parsed and saved the credential
(`saved credential for ssid=green` in the live serial log). One firmware
bug surfaced and was fixed to get there, and one app bug remains:

- **Fixed (firmware):** the Auth characteristic was created with
  `NIMBLE_PROPERTY::WRITE_ENC` alone. That's only the requires-encryption
  flag — without the `WRITE` property bit, the characteristic declaration
  tells clients writing is unsupported, and the app failed with "The WRITE
  property is not supported by this BLE characteristic" before ever
  attempting. Now `WRITE | WRITE_ENC`. (The Mac-client e2e test missed
  this because it only wrote to the Command characteristic; the app
  correctly writes Auth first, per this doc's own protocol.)
- **Fixed (foulad-one side, from the Midad conversation):** the app was
  already subscribing to `...0003` before writing -- that part was right.
  The actual bug was *which* stream it read the reply from.
  `flutter_blue_plus`'s `lastValueStream` merges two event sources: real
  notifications, **and an echo of the characteristic's own outgoing
  writes**. The app's own `wifi.provision` request (written to the same
  Command characteristic it was listening on) satisfied its reply filter
  -- valid JSON, matching `cmd` -- and `.first` resolved on that self-echo
  before the device's real notification ever arrived over the air. Since
  the echoed request has no `state` field, it decoded as
  `state: "", reason: null`, which is exactly "Something went wrong: ?".
  Fix: listen on `onValueReceived` instead (notifications/reads only, no
  write-echo). So the dispatcher's new "reply notified" log should in fact
  show up clean on the next app test -- the device side was never the
  problem here, confirming what the Mac-client comparison already
  suggested.
- Also relay to Midad as UX context: the SSID field stays manual-entry by
  necessity — iOS doesn't allow apps to scan for nearby Wi-Fi networks
  (pre-filling the phone's *current* SSID via NEHotspotNetwork is the
  realistic improvement), and the reader can't scan Wi-Fi while a BLE
  session is up (one radio). Also, the reader intentionally does not
  appear in iOS Settings > Bluetooth > My Devices — the session is
  app-managed and transient, like most BLE accessories.
  **Done, same session**: current-network SSID prefill is now built
  (`network_info_plus` + a Location permission prompt, since iOS ties the
  two together for this specific API) — password still has to be typed;
  there's no API to read or share a saved Wi-Fi password with a
  third-party app on either platform, so that half of the original ask
  isn't achievable at all, not just deferred.

## New phase, scoped after real use: BLE-driven account claim (no QR)

After actually using Phase 1, Sameh's reaction: why does claiming the
device still need a separate QR scan, when the phone already has a live
BLE session to it? Fair question — this was a deliberate Phase 1 scope
cut (see "Resolved (2026-08-10)" #2 above: `wifi.provision` only gets the
reader online, account-claiming stays on the existing QR/HTTP flow), not
an oversight. Revisiting it now that the friction is real. **Decided:
plan it as a real phase**, not build it silently.

**Checked foulad-ebooks' actual claiming flow before designing this**,
rather than assume the shape from memory (an earlier note in this doc
claimed the device only has raw username/password with zero token
concept — that was wrong, corrected below):

- Current QR flow: `DeviceLoginController::start()` mints a
  `DeviceLoginSession` (`pairing_code` for the QR, `session_token` for the
  device's own poll loop). The phone approves
  (`POST /api/app/device-login/approve`), which calls
  `DeviceToken::issue($user, $name, $serialNumber)` — a 48-char random
  string, stored only sha256-hashed, **already a device-scoped credential
  distinct from both the account password and the phone's own
  `AppToken`**. The device ends up storing `{username, token}` and sends
  it as HTTP Basic Auth (`OpdsBasicAuth` checks
  `DeviceToken::findByPlainToken()` before falling back to a real
  password). So the "device-scoped BLE token, not AppToken, not raw
  password" security requirement from earlier in this doc is **already
  built** as `DeviceToken` — no new credential type needed, just a new way
  to deliver one.
- `serial_number` is the device's global identity server-side (unique DB
  constraint, looked up the same way in both the OPDS device-registration
  path and the QR-approval path). Format is barely validated
  (`^[A-Za-z0-9_-]+$`, max 100 chars) — this codebase's own `XTE-<mac>`
  format already satisfies it, nothing to change there.
- `GET /api/app/devices` does **not** expose the DeviceToken (an earlier
  note in this doc implied it might — wrong, corrected). Not relevant to
  this design either way, just flagging the correction.

**Proposed shape**, reusing `DeviceToken::issue()` rather than a new
credential path:

1. **foulad-eink**: two more commands on the existing envelope, no new
   characteristics.
   - `device.info` — replies `{"serial": "...", "model": "...",
     "firmware_version": "..."}`. Doubles as this flow's confirmation
     step (see below) *and* as a second, independent fix for the
     multi-device scan-list problem above: even without the per-device
     advertised-name fix, once connected the phone can show the real
     serial, not just "Midad". The two fixes overlap in value but solve
     different moments (before connecting vs. after) — worth doing both,
     but this one alone makes BLE claiming reasonably safe even if the
     advertised-name fix slips.
   - `account.claim`, payload `{"username": "...", "token": "..."}` —
     saved exactly the way manually-entered or QR-issued credentials
     already are today. Nothing new to build here; it's the same sink.
2. **foulad-ebooks**: one new authenticated endpoint,
   `POST /api/app/devices/claim-by-serial`, body
   `{serial_number, model?, firmware_version?, device_name?}`. Does what
   `DeviceLoginController::approve()` already does (claim-uniqueness
   checks, `Device` row upsert, `DeviceToken::issue()`), just
   phone-initiated instead of session/poll-initiated — because BLE lets
   the phone hand the result straight to the device itself, so there's no
   async device-side polling to design around. Returns
   `{username, token}` synchronously.
3. **foulad-one**: after `wifi.provision` (same connection, before
   disconnecting — the device is about to lose BLE the moment it joins
   Wi-Fi, so both need to happen in one session): send `device.info`,
   show the user "Register [model] · [serial] to your account?" as the
   explicit confirmation step — the BLE-sourced replacement for QR's
   implicit "you're looking at the right physical device" check, not a
   dropped safeguard — then call `claim-by-serial`, then send
   `account.claim` with what it returns.

**Not yet built anywhere** — this is the plan, not the implementation.
Sequencing question left open: whether foulad-ebooks' side gets built by
whichever session/person picks up that repo, coordinated through this
same doc the way Phase 1 was. No firmware or server code should change
for this without confirming the shape here first, given it touches
account-claiming security.
