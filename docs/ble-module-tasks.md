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
- Leaving a BLE remote connected into device idle/sleep crashes the device
  (CrumBLE [issue #44](https://github.com/imshentastic/CrumBLE/issues/44)).
  Tear BLE down as part of the sleep sequence, not after sleep entry trips
  over it.

None of this has been measured on this codebase specifically yet — it's
proven on CrossPoint/CrumBLE, and this firmware carries more features
(dictionary, two themes, games, gym tracker) than either of them. Log
`ESP.getFreeHeap()` through real sessions with BLE running before trusting
these exact thresholds here.

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
| Soak test before shipping phases 1–3 | Multi-hour idle-with-BLE-advertising run, zero heap-related aborts | Proves the ~100 KB gate and 30 s cool-down actually hold on this firmware's real feature set, not just CrossPoint's |
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

Two allowed connection states, nothing else:

- **Unclaimed device (no account bound):** any phone running Midad may
  connect, but only to provision — hand over Wi-Fi credentials and a claim
  request. Same trust boundary as today's QR sign-in; the account link is
  still approved server-side, BLE just replaces the camera as transport.
- **Claimed device:** only a phone signed into *that same* account may send
  commands. The phone presents the same `AppToken` it already uses over
  HTTPS as part of the BLE handshake; the device checks it against the
  account it believes it's bound to. No new credential system.

A command from a mismatched or missing token is dropped silently, same as
an unauthenticated HTTP request is refused today.

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

## What's actually new work here

No peripheral/GATT-server code exists anywhere in this ecosystem yet —
every BLE fork found (CrumBLE, `crosspoint-reader-ble`) only implements the
opposite role (central, for HID remotes). That part has no prior art to
port from; the rest does.

Suggested shape, following this codebase's existing conventions
(`Activity`/`ActivityManager` lifecycle, `lib/hal/` for hardware-facing
components):

1. **`lib/hal/BlePeripheralManager.{h,cpp}`** (new) — GATT server: Auth,
   Command, Status characteristics; owns NimBLE init/deinit, the heap gate,
   the 30 s cool-down, and the Idle/Paused state.
2. **A command dispatcher** — registry of `cmd` string → handler. First
   three handlers: `settings.push`, `book.fetch`, `wifi.provision`. A fourth,
   `firmware.check`/`firmware.update`, is cheap to add once the dispatcher
   exists — this firmware already does OTA over Wi-Fi
   (`README.md`), this just makes it phone-triggerable rather than only
   on the device's own schedule. (Real prior art: `crosspoint-reader` PR
   [#2119](https://github.com/crosspoint-reader/crosspoint-reader/pull/2119)
   shipped OTA-over-BLE, closed as out-of-scope for CrossPoint's roadmap but
   measured at only ~15 s slower than the serial path — the mechanism works.)
3. **Settings toggle** — BLE peripheral defaults **on**; add a disable
   toggle under `src/activities/settings/`.
4. **Remote page-turner** — separate track, can start in parallel. Port
   patterns (not necessarily code verbatim — check attribution either way,
   both sources are MIT) from `crosspoint-reader-ble`'s
   `lib/hal/BluetoothHIDManager.{h,cpp}`: singleton lifecycle, per-device
   profiles for known remotes, auto-reconnect to a bonded device, 5-minute
   inactivity timeout, heap-floor auto-retry on the parser side.

## Suggested order

1. Provisioning — the entry point, and the clearest standalone win (no more
   fumbling Wi-Fi credentials onto this screen with no keyboard).
2. Settings push — small, low-risk, proves the peripheral/GATT plumbing and
   the account-token check end to end.
3. Book-fetch command — exercises the Idle → Wi-Fi → Idle transition for
   the first time.
4. Remote page-turner — parallel track, real reference to build from.
5. Firmware update trigger — cheapest addition, once the dispatcher exists.

## What to expect from the phone side

Midad already has the pieces this depends on: `AppToken`-based auth (same
mechanism, not new), and a client architecture that can take a new BLE
dependency. The phone-side BLE integration is comparatively small and can
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
