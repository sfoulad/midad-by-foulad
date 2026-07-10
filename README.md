# Foulad eInk

**An Arabic-first e-reader firmware.** Foulad eInk is a personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), the open-source e-reader firmware for ESP32-C3-based Xteink devices, rebuilt around one goal: making Arabic reading on pocket e-ink hardware feel native — full Arabic UI, proper letter shaping in book text, a bundled Naskh reading font, and a direct line to a self-hosted [Foulad eBooks](https://github.com/sfoulad/foulad-ebooks) [OPDS](https://opds.io/) library. It updates itself over the air from this repository's own GitHub releases.

**Runs on:** ESP32C3-based Xteink [X4](https://www.xteink.com/products/xteink-x4) and [X3](https://www.xteink.com/products/xteink-x3).

![Reading an Arabic EPUB on Foulad eInk (Xteink X4)](./docs/images/reading-arabic.jpg)

## Arabic, first-class

Most e-reader firmware treats Arabic as an afterthought; here it's the point.

- **Full Arabic UI** — switch the device language to Arabic and every screen, menu, and setting is translated.
- **True RTL mirroring** — the home screen, settings rows, lists, and hint bars all flip layout in Arabic, and even the **physical page-turn buttons swap direction** so "forward" is always where your thumb expects it.
- **Correct Arabic text rendering everywhere** — contextual letter shaping (isolated/initial/medial/final forms, lam-alef and Allah ligatures) plus bidirectional text handling for numbers and mixed Arabic/Latin lines, in book body text, titles, chapter lists, and filenames alike.
- **Bundled Naskh reading font** — Noto Naskh Arabic ships in the firmware at all reading sizes (with word spacing tuned per size so large text never runs together), and Noto Sans Arabic covers UI text. No SD-card font setup needed to start reading Arabic.
- **Arabic-aware layout** — right-aligned Arabic titles, dynamic row heights so tall Arabic glyphs never clip, and Arabic-Indic digit support.

## What can it do?

Everything CrossPoint Reader can do, reshaped around this fork's additions:

- **Foulad home screen**: a hero card for the book you're reading (cover, progress bar, time read and estimated time left), a "My Books" row of recent covers, and a bottom icon menu with physical-button hints.

- **Reading statistics**: total and per-day reading time, a monthly heatmap of your reading activity, per-book time tracking, and a configurable daily reading goal.

- **Foulad eBooks**: a dedicated home-screen entry that opens a self-hosted OPDS catalog directly — cover-grid browsing, search, pagination, and one-tap downloads that keep the catalog's cover art. No manual server setup beyond a one-time username/password prompt on first use.

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, chapter navigation, footnotes, bookmarks, go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync and more.

- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

- **Custom fonts**: install your favorite fonts on the SD card.

- **Tilt page turn (X3 only)**.

- **Library workflow**: folder browser, hidden-file toggle, long-press delete, recent books with cover thumbnails, SD-cache management.

- **Wireless workflows**:

  - File transfer web UI
  - EPUB Optimizer
  - Web settings UI/API (edit many device settings from browser)
  - WebSocket fast uploads
  - WebDAV handler
  - AP mode (hotspot) and STA mode (join existing Wi-Fi), both with QR helpers
  - Calibre wireless connect flow
  - OPDS browser with saved servers (up to 8), search, pagination, and direct download
  - OTA update checks and installs from this repo's GitHub releases — the updater restarts the device into a clean-memory state before downloading, so updates install reliably even after long reading sessions

- **Lean firmware**: trimmed to the two languages it's actually for (English and Arabic) and a curated font set (Noto Serif for Latin reading, Noto Naskh Arabic for Arabic) — the whole image is ~4.2 MB, which keeps OTA updates fast.

- **Customization**: sleep screen modes, front/side button remapping, status bar controls, power-button behavior, refresh cadence, sunlight fading fix, and more.

- **Localization**: English and Arabic UI with full RTL support.

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory.
If your device is locked, you will need to use the **Xteink Unlocker** tool available at
https://crosspointreader.com/#unlock-tool before you can flash this firmware. This tool is maintained by the upstream
CrossPoint project, not this fork.

**You do not need this tool if you bought your device directly from xteink.com.** Those units are not locked.

**Not sure if your device is locked?** Power it on, connect the USB-C cable, and try flashing via the web flasher first (see
[Install firmware](#install-firmware) below). If the browser's serial device picker does not show your device, try a different
USB port or browser before assuming the device is locked. Only reach for the unlocker if the device still doesn't appear.

> ### ⚠️ WARNING: READ THIS BEFORE USING THE UNLOCKER ⚠️
>
> **The only officially supported firmwares in the unlock tool are CrossPoint and CrossInk** — this fork is not one of
> them, and must be flashed as a "Custom .bin" (see below).
>
> Flashing any other firmware on a USB-locked device may **permanently brick the device** or leave it **permanently
> stuck on that firmware with no recovery path**. Once USB flashing is re-locked, your only way back is via OTA, and if
> the firmware you flashed doesn't support OTA, **there is no way out**. This fork's OTA checks
> [this repo's releases](https://github.com/sfoulad/foulad-eink/releases), so once it's flashed once, future updates
> can be installed directly from the device.

## Install firmware

### Web installer

1. Connect your device to your computer via USB-C and wake/unlock the device.
2. Download `firmware.bin` from the [Releases](https://github.com/sfoulad/foulad-eink/releases) page.
3. Go to https://crosspointreader.com/#flash-tools (a generic browser-based ESP32 flasher), select device (X3 or X4), click "Custom .bin", and upload the downloaded `firmware.bin`.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download `firmware.bin` from the [Releases](https://github.com/sfoulad/foulad-eink/releases) page.
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

Adjust `/dev/ttyACM0` to match your system.

### Manual

See [Development quick start](#development-quick-start) below.

### After the first flash

Once this firmware is running, every future update can be installed directly from the device via Settings → OTA
Update — no cable needed — since it checks this repo's [releases](https://github.com/sfoulad/foulad-eink/releases).

---

## Custom SD-card fonts

Convert your own TTF/OTF files into `.cpfont` files that load from the SD card. No firmware reflash is needed.

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

### Arabic support

Book titles, author names, filenames, chapter titles, and full EPUB body text render Arabic script correctly
(contextual letter shaping, right-to-left word/line order, and bidi handling for numbers and mixed punctuation) out of
the box — no setup required. Two Arabic fonts ship built into the firmware:

- **Noto Naskh Arabic** — traditional Naskh book-printing style, used for reading at all four reading sizes
  (12/14/16/18pt). Word spacing is tuned per size so large text keeps clear word boundaries.
- **Noto Sans Arabic** — used for UI text (grid titles, chapter lists, menus) at the UI sizes (8/10/12pt),
  matching how the Latin reading font doesn't affect UI text either.

Pick the reading size from **Settings → Reader → Arabic Font Size**, independent of whichever Latin font/size you've
chosen — Arabic text always uses its own family and size.

If you'd prefer a different Arabic typeface for reading, you can override with your own SD-card font:

1. Download an Arabic-capable TTF/OTF (e.g. [IBM Plex Sans Arabic](https://fonts.google.com/specimen/IBM+Plex+Sans+Arabic)).
2. Convert it locally (the hosted web font builder doesn't know about this fork's Arabic support, so run the script directly):
   ```bash
   cd lib/EpdFont/scripts
   python3 fontconvert_sdcard.py --intervals reading,arabic --name "PlexArabic" /path/to/IBMPlexSansArabic-Regular.ttf
   ```
3. Copy the generated `.cpfont` files to your SD card under `/fonts/PlexArabic/` (or `/.fonts/PlexArabic/`).
4. On the device: **Settings → Reader → Arabic Font** → select the family you just installed (it appears alongside
   the built-in option). Selecting the built-in font again reverts to it.

Note: the built-in fonts cover every UI and reading size so Arabic matches whichever text it's replacing; a custom
SD-card override applies at one fixed size everywhere instead.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)

---

## Development quick start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/sfoulad/foulad-eink
cd foulad-eink

# if cloned without --recursive:
git submodule update --init --recursive
```

### Build / flash / monitor

```bash
pio run --target upload
```

### Pre-commit checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
pio run -t unit-tests
```

### Debugging

After flashing new changes, it's recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

This firmware is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card (an internal path inherited from upstream, unrelated
to this fork's name). The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Staying up to date with upstream

This fork tracks [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) as a
git remote named `upstream`. To pull in new upstream releases:

```bash
git fetch upstream
git merge upstream/develop
```

---

Not affiliated with Xteink or any device manufacturer. Based on
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), MIT licensed, which itself credits
[diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader) as its original inspiration.
