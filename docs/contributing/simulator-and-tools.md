# Companion repos: simulator, tools site, community SDK

Notes from surveying three sibling repos in the `crosspoint-reader` org, kept here
for reference when we want to test or extend this fork without hardware.

Local checkout (this session, sibling to the firmware repo, not a submodule):

```text
/home/user/workspace-tools/crosspoint-tools/       (SoFriendly/crosspoint-tools on GitHub)
/home/user/workspace-tools/crosspoint-simulator/
/home/user/workspace-tools/community-sdk/
```

## crosspoint-simulator — desktop testing without an X4

Ships as a PlatformIO library (`lib_deps` entry, not a standalone binary). Add an
`[env:simulator]` env with `platform = native` and `-DSIMULATOR`, and it compiles
this firmware as a host binary that renders the e-ink framebuffer into an SDL2
window. This is the main lever for "test and improve" without physical hardware.

**Already wired up locally**: `platformio.local.ini` (gitignored, not committed)
in this repo now has a working `[env:simulator]` pointing at
`simulator=symlink:///home/user/workspace-tools/crosspoint-simulator`, copied
from the repo's `sample-platformio-linux-wsl.ini`. `libsdl2-dev` and
`libssl-dev` are installed.

```sh
pio run -e simulator                 # build only
pio run -e simulator -t run_simulator  # build + launch SDL window
rm -rf ./fs_/.crosspoint/            # clear stale simulator cache after storage/cache changes
```

Books go in `./fs_/books/` (maps to `/books/` on the real SD card).

Key facts:
- **HAL stub rule**: every `Hal*` class here mirrors a `lib/hal/Hal*` in the firmware. If firmware adds a method to a HAL class, the simulator build breaks (link error) until a matching stub (usually one-line no-op) is added on the simulator side. This is the most common simulator breakage after pulling firmware changes.
- Host-backed network: OPDS/catalog downloads and KOReader sync shim through the host's `curl`; `CROSSPOINT_SIM_HTTP_MOCK_ROOT` can serve local fixture files instead of hitting the real network (useful for SD-font testing).
- File transfer: firmware's port-80 web server / port-81 WebSocket server are exposed at `http://127.0.0.1:8080/` and `ws://127.0.0.1:8081/` on the host.
- OTA / SD firmware flashing are non-destructive no-ops in the simulator (UI opens, nothing actually flashes).
- Image previews decode JPEG/PNG on the host for a rough grayscale preview — doesn't simulate real e-ink refresh/ghosting or on-device memory pressure.
- `-DSIMULATOR_DEVICE_X3` switches to the X3's 792x528 landscape panel + tilt sensor.
- Controls: arrows = page/front buttons, Return = confirm, Escape = back, P = power, S = simulate sleep.
- Deep architecture notes + bug-fix history: `crosspoint-simulator/.claude/CONTEXT-sim-notes.md` (read before non-trivial simulator-side changes).

**Known limitation in this remote sandbox**: the simulator build could not actually
be verified here. `pio run -e simulator` needs PlatformIO's package registry
(`api.registry.platformio.org` and mirrors) to fetch the `native` platform and
`lib_deps` (ArduinoJson, QRCode, WebSockets), and this session's egress policy
blocks that host outright (confirmed via the agent-proxy status endpoint — an
org policy denial, not something to retry or route around). The regular
hardware build (`pio run`) is blocked for the same class of reason: it needs
`github.com/pioarduino/platform-espressif32` release assets, and this
session's GitHub scope is locked to `sfoulad/foulad-eink` only (cross-owner
repos can't be added once a session already has a repo from another owner).
**Both should work fine on an unrestricted machine** (a real laptop) — the
`platformio.local.ini` setup here is ready to use as-is once network access
allows it.

## crosspoint-tools — the web flasher/build/font infra (crosspointreader.com)

Cloudflare Worker + GitHub Actions, not something we run locally day-to-day, but
useful to know about:

- Browser-based firmware flashing over WebSerial (stable/insider/beta channels, custom-font builds, SD-card `.cpfont` builder, stock firmware restore for X3/X4).
- `/debug` page identifies a connected device's partition layout (CrossPoint / CrossInk / stock X3 / unknown) and can repair a corrupted boot region (bad partition table at `0x8000`) by reflashing bootloader + partition table + blank NVS/otadata, optionally landing straight on stable CrossPoint.
- Nightly insider builds come from `crosspoint-reader/crosspoint-reader` `master` via `pioarduino` PlatformIO, uploaded to R2.
- The SD-card font builder here is the same generator our firmware vendors at `lib/EpdFont/scripts/fontconvert_sdcard.py`, kept in sync manually.
- `unlocker-tool/` (Tauri desktop app) is a separate concern: it repoints a device's OTA update check via a local Wi-Fi-hotspot MITM (DNS + HTTPS spoofing of the Xteink update API and GitHub releases API) to let stock/locked X3/X4 units install CrossPoint-family firmware. Relevant only if we ever need to help a user unbrick/unlock a device, not for day-to-day firmware dev.

## community-sdk (OpenX4 E-Paper Community SDK)

Community-maintained, MIT-licensed sibling to `freeink-sdk` — same shape
(`libs/display`, `libs/hardware`, `libs/graphics`, `tools/`), meant to be
pulled in as a submodule + `lib_deps` symlinks (`BatteryMonitor`,
`InputManager`, `SDCardManager`, `EInkDisplay`, etc.) for X4 firmware projects.
Worth checking here first if `freeink-sdk` is missing a hardware helper before
writing one from scratch — same license, same PlatformIO-friendly shape.

## Suggested next step

Since this sandbox can't reach PlatformIO's registry or fetch the ESP32
toolchain, hands-on testing (simulator or real-device flashing) needs to
happen on your actual laptop. `platformio.local.ini` here already has the
`[env:simulator]` config ready — clone `crosspoint-simulator` next to this
repo (or adjust the `symlink://` path) and `pio run -e simulator -t
run_simulator` should just work there. This session can keep doing static
review, code changes, and `pio check`-style analysis in the meantime.
