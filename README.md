<h1 align="center">Foulad eInk</h1>
<p align="center"><b>An e-reader built around Arabic.</b></p>
<p align="center">
  Foulad eInk is a free, open-source firmware for Xteink e-ink readers — a fork of
  <a href="https://github.com/crosspoint-reader/crosspoint-reader">CrossPoint Reader</a>, rebuilt so Arabic, and the
  languages that share its script, read the way they're supposed to. It updates itself over the air, straight from
  this repository.
</p>
<p align="center">
  <i>Runs on Xteink <a href="https://www.xteink.com/products/xteink-x4">X4</a> and
  <a href="https://www.xteink.com/products/xteink-x3">X3</a>.</i>
</p>

<p align="center">
  <a href="https://github.com/sfoulad/foulad-eink/releases"><img src="https://img.shields.io/github/v/release/sfoulad/foulad-eink?label=latest%20release&color=black" alt="Latest release"></a>
  <a href="./LICENSE"><img src="https://img.shields.io/github/license/sfoulad/foulad-eink?color=black" alt="License"></a>
</p>

<p align="center">
  <sub>
    <a href="#highlights">Highlights</a> ·
    <a href="#what-can-it-do">Full feature list</a> ·
    <a href="#install-firmware">Install</a> ·
    <a href="#custom-sd-card-fonts">Fonts</a> ·
    <a href="#development-quick-start">Development</a>
  </sub>
</p>

<br>

<p align="center">
  <img src="./docs/images/home.png" width="380" alt="Foulad eInk home screen">
</p>
<h3 align="center">Everything, from one screen.</h3>
<p align="center">
  Pick up where you left off, browse your library, and check your reading stats — a home screen designed to get out
  of your way.
</p>

<br>

<p align="center">
  <img src="./docs/images/reading.png" width="380" alt="Reading the Quran on Foulad eInk">
</p>
<h3 align="center">Reads Arabic like it was made for it.</h3>
<p align="center">
  Correct letter shaping, right-to-left layout, and a bundled Quran in full Uthmani script with proper ayah markers.
  The same engine reads Persian, Ottoman Turkish, and Kurdish text correctly too.
</p>

<br>

<p align="center">
  <img src="./docs/images/stats.png" width="380" alt="Reading statistics on Foulad eInk">
</p>
<h3 align="center">See your reading, at a glance.</h3>
<p align="center">
  Daily streaks, a monthly heatmap, and per-book progress — set a daily goal and watch it add up.
</p>

<br>

<p align="center">
  <img src="./docs/images/games.png" width="380" alt="Sudoku on Foulad eInk">
</p>
<h3 align="center">Take a break.</h3>
<p align="center">
  Snake, Tetris, Sudoku, and Maze — built right into the device, no download required.
</p>

<br>

<p align="center">
  <img src="./docs/images/devices.png" width="700" alt="Foulad eInk running on Xteink X4 and X3">
</p>
<h3 align="center">One firmware, both devices.</h3>
<p align="center">
  Foulad eInk runs natively on the Xteink X4 and X3, adapting to each device's screen and buttons automatically.
</p>

<br>

---

## Highlights

- **A fully Arabic interface** — mirrored, translated, and with page-turn buttons that swap direction automatically so "forward" is always where your thumb expects it.
- **Reads Arabic, Persian, Ottoman Turkish, and Kurdish correctly** — proper contextual letter shaping and right-to-left text, in full book text, titles, and filenames alike.
- **A bundled Quran** — Uthmani script, ayah and surah markers, and justified lines that stretch the way a real mushaf does.
- **Foulad eBooks** — connect to a self-hosted library and browse it as a cover grid, right from the home screen.
- **Reading stats** — streaks, a monthly heatmap, and per-book progress.
- **Four built-in games** — Snake, Tetris, Sudoku, and Maze.
- **Everything else CrossPoint Reader does** — EPUB/XTC/TXT support, custom fonts, Wi-Fi file transfer, OPDS library browsing, and more. See [the full feature list](#what-can-it-do) below.

---

## What can it do?

Everything CrossPoint Reader can do, reshaped around this fork's additions.

**Home & library**

- A hero card for the book you're reading (cover, progress bar, time read, and estimated time left), a "My Books" row of recent covers, and a bottom icon menu with physical-button hints.
- Folder browser, hidden-file toggle, long-press delete, recent books with cover thumbnails, SD-cache management.
- Native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

**Reading**

- EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, chapter navigation, footnotes, bookmarks, go-to-percent, auto page turn, orientation control, focus reading, and KOReader progress sync.
- Custom fonts — install your favorite fonts on the SD card.
- Tilt page turn (X3 only).

**Reading statistics**

- Total and per-day reading time, a monthly heatmap of your reading activity, per-book time tracking, and a configurable daily reading goal.

**Games**

- Snake, Tetris, Sudoku, and Maze, pinned as their own tile in your library — no setup, nothing to install.

**Foulad eBooks**

- A dedicated home-screen entry that opens a self-hosted OPDS catalog directly — cover-grid browsing, search, pagination, and one-tap downloads that keep the catalog's cover art. No manual server setup beyond a one-time username/password prompt on first use.

**Wireless**

- File transfer web UI
- EPUB Optimizer
- Web settings UI/API (edit many device settings from a browser)
- WebSocket fast uploads
- WebDAV handler
- AP mode (hotspot) and STA mode (join existing Wi-Fi), both with QR helpers
- Calibre wireless connect flow
- OPDS browser with saved servers (up to 8), search, pagination, and direct download
- OTA update checks and installs from this repo's GitHub releases — the updater restarts the device into a clean-memory state before downloading, so updates install reliably even after long reading sessions

**Customization**

- Sleep screen modes, front/side button remapping, status bar controls, power-button behavior, refresh cadence, sunlight fading fix, and more.

**Under the hood**

- Lean firmware, trimmed to the two languages it's actually for (English and Arabic) and a curated font set (Noto Serif for Latin reading, Noto Naskh Arabic for Arabic) — the whole image is a few MB, which keeps OTA updates fast.
- Full English and Arabic UI localization with complete RTL support.

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory.
If your device is locked, you'll need the **Xteink Unlocker** tool at https://crosspointreader.com/#unlock-tool before
you can flash this firmware. It's maintained by the upstream CrossPoint project, not this fork.

> [!NOTE]
> **You don't need this tool if you bought your device directly from xteink.com** — those units aren't locked.
>
> **Not sure if yours is locked?** Power it on, connect USB-C, and try the web flasher first (see
> [Install firmware](#install-firmware) below). If the browser's device picker doesn't see it, try a different USB
> port or browser before assuming it's locked.

> [!WARNING]
> **Read this before using the unlocker.** The only officially supported firmwares in the unlock tool are CrossPoint
> and CrossInk — this fork isn't one of them, and must be flashed as a **"Custom .bin"** (see below).
>
> Flashing any other firmware on a USB-locked device can **permanently brick it** or leave it **stuck on that
> firmware with no way back**. Once USB flashing is re-locked, OTA is your only path forward — and if the firmware
> you flashed doesn't support OTA, there is no way out. This fork's OTA checks
> [this repo's releases](https://github.com/sfoulad/foulad-eink/releases), so once it's flashed once, every future
> update can be installed straight from the device.

---

## Install firmware

### Web installer

1. Connect your device via USB-C and wake/unlock it.
2. Download `firmware.bin` from the [Releases](https://github.com/sfoulad/foulad-eink/releases) page.
3. Go to https://crosspointreader.com/#flash-tools (a generic browser-based ESP32 flasher), pick your device (X3 or
   X4), click **Custom .bin**, and upload the file you downloaded.

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

5. Flash it:

   ```bash
   esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
   ```

   Adjust `/dev/ttyACM0` to match your system.

### Manual

See [Development quick start](#development-quick-start) below.

> [!TIP]
> **After the first flash**, every future update installs directly from the device — Settings → OTA Update, no
> cable needed — since it checks this repo's [releases](https://github.com/sfoulad/foulad-eink/releases).

---

## Custom SD-card fonts

Convert your own TTF/OTF files into `.cpfont` files that load from the SD card — no firmware reflash needed.

1. Go to https://crosspointreader.com/fonts and open the **SD-card font builder** form.
2. Upload up to four styles (regular, bold, italic, bold-italic), and set the family name, point sizes, and Unicode
   range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so the output
matches a local host build.

### Arabic support

Book titles, author names, filenames, chapter titles, and full EPUB body text render Arabic script correctly out of
the box — no setup required — including contextual letter shaping, right-to-left word and line order, and the extra
letters Persian, Ottoman Turkish, and Kurdish add on top of the Arabic alphabet. Two Arabic fonts ship built into the
firmware:

- **Noto Naskh Arabic** — traditional Naskh book-printing style, used for reading at all four reading sizes
  (12/14/16/18pt). Word spacing is tuned per size so large text keeps clear word boundaries.
- **Noto Sans Arabic** — used for UI text (grid titles, chapter lists, menus) at the UI sizes (8/10/12pt), the same
  way the Latin reading font doesn't affect UI text either.

Pick the reading size from **Settings → Reader → Arabic Font Size**, independent of whichever Latin font/size you've
chosen — Arabic text always uses its own family and size.

If you'd rather use a different Arabic typeface for reading, you can override it with your own SD-card font:

1. Download an Arabic-capable TTF/OTF (e.g. [IBM Plex Sans Arabic](https://fonts.google.com/specimen/IBM+Plex+Sans+Arabic)).
2. Convert it locally — the hosted web font builder doesn't know about this fork's Arabic support, so run the script
   directly:

   ```bash
   cd lib/EpdFont/scripts
   python3 fontconvert_sdcard.py --intervals reading,arabic --name "PlexArabic" /path/to/IBMPlexSansArabic-Regular.ttf
   ```

3. Copy the generated `.cpfont` files to your SD card under `/fonts/PlexArabic/` (or `/.fonts/PlexArabic/`).
4. On the device: **Settings → Reader → Arabic Font** → select the family you just installed (it appears alongside
   the built-in option). Selecting the built-in font again reverts to it.

> [!NOTE]
> The built-in fonts cover every UI and reading size, so Arabic always matches whichever text it's replacing; a
> custom SD-card override applies at one fixed size everywhere instead.

---

## Documentation

| | |
|---|---|
| [User Guide](./USER_GUIDE.md) | How to use the device day to day |
| [Web server usage](./docs/webserver.md) | Using the built-in file-transfer/settings web UI |
| [Web server endpoints](./docs/webserver-endpoints.md) | API reference for the web server |
| [Project scope](./SCOPE.md) | What this fork will and won't take on |

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

After flashing new changes, it's worth capturing detailed logs from the serial port. First, install the required
Python packages:

```bash
python3 -m pip install pyserial colorama matplotlib
```

Then run the monitor script:

```bash
# Linux (tested on Debian, should work on most distros)
python3 scripts/debugging_monitor.py

# macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be needed on Windows.

---

## Internals

This firmware is aggressive about caching data down to the SD card to keep RAM usage low — the ESP32-C3 only has
~380KB of usable RAM, so most of the internal design works around that constraint.

### Data caching

The first time a book's chapters are loaded, they're cached to the SD card; subsequent loads are served from the
cache. The cache directory lives at `.crosspoint` on the SD card (an internal path inherited from upstream, unrelated
to this fork's name):

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

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes,
overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may
leave stale cache directories behind.

For more detail on the internal file formats, see the [file formats document](./docs/file-formats.md).

---

## Staying up to date with upstream

This fork tracks [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) as a
git remote named `upstream`. To pull in new upstream releases:

```bash
git fetch upstream
git merge upstream/develop
```

---

<p align="center">
  <sub>
    Not affiliated with Xteink or any device manufacturer. Based on
    <a href="https://github.com/crosspoint-reader/crosspoint-reader">CrossPoint Reader</a>, MIT licensed, which
    itself credits <a href="https://github.com/atomic14/diy-esp32-epub-reader">diy-esp32-epub-reader</a> as its
    original inspiration.
  </sub>
</p>
