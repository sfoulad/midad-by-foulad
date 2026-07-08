---
name: testing-and-verification
description: Choosing and running the right verification tier for a firmware change. Use when finishing a change and deciding how to test it, when claiming something "works" or "is fixed," when adding new test coverage, or when a task needs the host unit tests, the desktop simulator, a full pio build/check, or on-device confirmation. Covers the four CI gates, what each verification tier actually proves, when host gtest suites are the right (and cheap) answer, HAL-stub obligations for the simulator, and what must stay flagged for a human with hardware.
---

# Testing and Verification

CrossPoint has four independent CI gates (`.github/workflows/ci.yml`):
`clang-format`, `cppcheck` (via `pio check`), `build` (`pio run`), and
`unit-tests` (host gtest via CMake/CTest). None of them substitutes for the
others, and none of them proves a UI/activity change actually behaves —
match the verification tier to what changed, and say plainly which tiers you
ran.

## The four tiers, cheapest first

1. **Host unit tests** (`test/`, CMake + GoogleTest, `ctest --test-dir
   build/test`) — pure host binary, no ESP-IDF/Arduino toolchain, no
   PlatformIO registry. Seconds to build and run. This is the right tier for
   anything that is a pure function of its inputs: parsers (`ReleaseJsonParser`,
   `StreamingJsonParser`), text/font math (`Utf8`, `EpdFont` kerning/metrics,
   `Fp4Math`), hyphenation, layout math. If the change lives in `lib/` and
   doesn't touch a `Hal*` singleton, it belongs here before anything heavier.
2. **Desktop simulator** (`crosspoint-simulator`, native SDL2 build) —
   exercises real Activities, rendering, input mapping, and orientation on a
   host binary. This is the tier for UI/activity/rendering/button-flow
   changes you can't express as a pure function. See "Simulator specifics"
   below before touching it.
3. **`pio check` + `pio run`** — real cppcheck static analysis and a real
   ESP32-C3 cross-compile against the actual HAL/SDK. Catches type/include/
   toolchain issues host tests and the simulator can't see. Required before a
   PR is postable as done, but a clean build is not behavioral proof.
4. **On-device** — flashing a real X4/X3 and exercising it. The only tier
   that proves timing, real heap fragmentation, e-ink refresh/ghosting,
   physical button mapping, and SD card behavior. Cannot be delegated to CI
   or simulated away; flag it explicitly for a human with hardware rather
   than implying it happened.

Picking the tier is a judgment call, not a ritual: a one-line change to a
hyphenation table needs tier 1, not a device flash; a new Activity's button
handling needs at least tier 2 before you call it done.

## When to add a new host gtest suite

Add one (mirror an existing suite's `CMakeLists.txt`: an `add_executable`
pulling in only the `.cpp` files it needs from `REPO_ROOT`, linked against
`crosspoint_test_common` and `GTest::gtest_main`, ending in
`gtest_discover_tests`) when the code:

- has no `Hal*`/SDK/Arduino dependency, or the dependency is trivial to stub, and
- has edge cases worth pinning down (truncated input, boundary rounding,
  empty/malformed data) that are tedious to hit by hand on a device.

Don't add one for code that's a thin wrapper over a `Hal*` call — that's a
simulator or on-device concern, and a host stub would just test the stub.

## Simulator specifics

- It's a PlatformIO `lib_dep`, not a standalone app: firmware compiles as a
  native host binary with `-DSIMULATOR`, e-ink framebuffer renders into SDL2.
- **The HAL stub rule**: every `Hal*` class in `lib/hal/` has a mirror in the
  simulator repo. If a change adds a method to a `Hal*` class and calls it,
  the simulator fails to link until a matching stub exists there — usually a
  one-line no-op, but it does mean a firmware-side HAL change is not fully
  "tested" via the simulator until that stub lands too (separate repo/PR).
- OTA/firmware-flash paths are simulator no-ops by design — never treat
  "the update UI opened in the simulator" as proof an OTA path works.
- `rm -rf ./fs_/.crosspoint/` after any storage/cache-format change, or
  stale on-disk cache will mask the fix.
- Image previews and e-ink refresh are host-decoded approximations, not real
  e-ink timing/ghosting/memory pressure — don't cite simulator visuals as
  proof of on-device rendering quality.

## Reporting results

State which tier(s) you actually ran and what passed/failed — "ran the host
suite, 85/85 passing" is verifiable; "tested and it works" is not. If a tier
was unavailable (no hardware, no network to fetch a toolchain, no simulator
checkout), say that explicitly rather than letting the gap read as
"verified." Never claim on-device behavior (heap headroom, refresh quality,
button feel) you did not actually observe on a device.

## Self-review before calling something tested

- [ ] Named the tier(s) run, matched to what actually changed.
- [ ] Pure-logic changes have a host gtest suite exercising their edge cases,
      not just the happy path.
- [ ] A `Hal*` signature change has a corresponding simulator-side stub, or
      that gap is flagged, not silently left to break the next simulator build.
- [ ] Stale `.crosspoint` cache ruled out before trusting a cache/layout fix.
- [ ] Claims about device behavior (heap, refresh, buttons) are backed by an
      actual device run, or explicitly flagged as unverified.
