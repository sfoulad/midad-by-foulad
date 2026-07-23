# Foulad eInk User Guide

Welcome to **Foulad eInk**, a free firmware for Xteink e-ink readers built around Arabic reading — a fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). This guide covers the hardware controls,
navigation, and features of the device on both supported models, the **Xteink X4** and **Xteink X3**.

> [!TIP]
> For a feature overview and installation instructions, see the [README](./README.md). This guide is the day-to-day
> reference for using the device once it's flashed.

- [Foulad eInk User Guide](#foulad-eink-user-guide)
  - [1. Hardware Overview](#1-hardware-overview)
    - [Button Layout](#button-layout)
    - [Taking a Screenshot](#taking-a-screenshot)
  - [2. Power \& Startup](#2-power--startup)
    - [Power On / Off](#power-on--off)
    - [First Launch](#first-launch)
  - [3. Screens](#3-screens)
    - [3.1 Home Screen](#31-home-screen)
    - [3.2 Reading Mode](#32-reading-mode)
    - [3.3 Browse Files Screen](#33-browse-files-screen)
    - [3.4 My Books \& Apps Screen](#34-my-books--apps-screen)
    - [3.5 File Transfer Screen](#35-file-transfer-screen)
    - [3.5.1 Calibre Wireless Transfers](#351-calibre-wireless-transfers)
      - [Installing the Plugin in Calibre](#installing-the-plugin-in-calibre)
      - [Configuring the Plugin in Calibre](#configuring-the-plugin-in-calibre)
      - [Uploading Books](#uploading-books)
      - [Removing a Book](#removing-a-book)
    - [3.6 Foulad eBooks](#36-foulad-ebooks)
    - [3.7 Built-in Apps](#37-built-in-apps)
      - [Gym](#gym)
      - [Tasbih](#tasbih)
      - [Stop Watch](#stop-watch)
      - [Games](#games)
    - [3.8 Dictionary](#38-dictionary)
    - [3.9 Settings](#39-settings)
      - [3.9.1 Display](#391-display)
      - [3.9.2 Reader](#392-reader)
      - [3.9.3 Controls](#393-controls)
      - [3.9.4 Apps](#394-apps)
      - [3.9.5 System](#395-system)
      - [3.9.6 OPDS Servers (Multiple Libraries)](#396-opds-servers-multiple-libraries)
      - [3.9.7 Web Settings (Wi-Fi + OPDS)](#397-web-settings-wi-fi--opds)
      - [3.9.8 KOReader Sync Quick Setup](#398-koreader-sync-quick-setup)
        - [Option A: Free Public Server (`sync.koreader.rocks`)](#option-a-free-public-server-synckoreaderrocks)
        - [Option B: Self-Hosted Server (Docker Compose)](#option-b-self-hosted-server-docker-compose)
      - [3.9.9 Customise Status Bar](#399-customise-status-bar)
    - [3.10 Sleep Screen](#310-sleep-screen)
      - [Cover settings](#cover-settings)
      - [Custom images](#custom-images)
    - [3.11 Custom Fonts (SD Card)](#311-custom-fonts-sd-card)
  - [4. Reading Mode](#4-reading-mode)
    - [Page Turning](#page-turning)
    - [Chapter Navigation](#chapter-navigation)
    - [Auto Page Turn](#auto-page-turn)
    - [Tilt Page Turn (X3 only)](#tilt-page-turn-x3-only)
    - [Footnote Navigation](#footnote-navigation)
    - [System Navigation](#system-navigation)
    - [Arabic \& Right-to-Left Reading](#arabic--right-to-left-reading)
    - [Supported Languages](#supported-languages)
  - [5. Reader Drawer](#5-reader-drawer)
    - [5.1 Reading Tab](#51-reading-tab)
    - [5.2 Settings Tab](#52-settings-tab)
    - [5.3 Chapter Selection](#53-chapter-selection)
    - [5.4 Bookmarks](#54-bookmarks)
    - [5.5 Dictionary Lookup](#55-dictionary-lookup)
  - [6. Current Limitations \& Roadmap](#6-current-limitations--roadmap)
  - [7. Troubleshooting Issues \& Escaping Bootloop](#7-troubleshooting-issues--escaping-bootloop)

## 1. Hardware Overview

Foulad eInk runs on two devices with slightly different physical button layouts. Both share the same four front
buttons; only the placement of the side (page-turn/volume-style) buttons differs.

### Button Layout

| Location            | Xteink X4                                   | Xteink X3                                            |
| -------------------- | -------------------------------------------- | ----------------------------------------------------- |
| **Bottom Edge**      | **Back**, **Confirm**, **Left**, **Right**  | **Back**, **Confirm**, **Left**, **Right**            |
| **Side buttons**     | **Up** and **Down**, stacked on the right edge | **Up** on the left edge, **Down** on the right edge — one on each side of the screen |
| **Power / Reset**    | Right side                                   | Right side                                             |

Button layout can be customized in the **[Controls Settings](#393-controls)** — the front Back/Confirm/Left/Right
buttons can be remapped to different logical functions; the side Up/Down buttons are fixed.

> [!NOTE]
> Only the **X3** has a real-time clock chip and a gyroscope. This means the Home screen clock and the
> **[Dashboard sleep screen](#310-sleep-screen)**'s clock only appear on X3, and **[Tilt Page Turn](#tilt-page-turn-x3-only)**
> is X3-exclusive. Everything else works identically on both devices.

### Taking a Screenshot

When the Power Button and Down button are pressed at the same time, it will take a screenshot and save it in the
folder `screenshots/`.

Alternatively, while reading a book, open the **[Reader Drawer](#5-reader-drawer)** and select **Take Screenshot**
from the Settings tab.

---

## 2. Power & Startup

### Power On / Off

To turn the device on or off, **press and hold the Power button for approximately half a second**.
In the **[Controls Settings](#393-controls)** you can configure the power button to turn the device off with a short
press instead of a long one.

To reboot the device (for example after a firmware update or if it's frozen), press and release the Reset button, and
then quickly press and hold the Power button for a few seconds.

### First Launch

Upon turning the device on for the first time, you will be placed on the **[Home](#31-home-screen)** screen.

> [!NOTE]
> On subsequent restarts, the firmware will automatically reopen the last book you were reading.

---

## 3. Screens

### 3.1 Home Screen

The Home screen is the main entry point to the firmware: a hero card for the book you're currently reading (cover,
title, author, progress bar, time read, and estimated time left), a "My Books" row of recent covers below it, and a
bottom icon menu for **My Books & Apps**, **Stats**, **Files** (or **eBooks**, once [Foulad eBooks](#36-foulad-ebooks)
is set up), and **Settings**.

Setting the UI **Language** to Arabic mirrors this entire screen right-to-left — the hero cover, recents row, menu
order, and status bar all flip to match. See [Arabic & Right-to-Left Reading](#arabic--right-to-left-reading) for
details.

### 3.2 Reading Mode

See [Reading Mode](#4-reading-mode) below for more information.

### 3.3 Browse Files Screen

The Browse Files screen acts as a file and folder browser. The full path to the current directory is shown at the top
of the screen. File extensions are displayed alongside each filename, and directories are shown with brackets (e.g.
`[folder-name]`). Hidden directories (those beginning with `.`) are also visible when enabled in Settings.

* **Navigate List:** Use **Left** (or **Up**), or **Right** (or **Down**) to move the selection cursor up and down
  through folders and books. You can also long-press these buttons to scroll a full page up or down.
* **Open Selection:** Press **Confirm** to open a folder or start reading a selected book. Selecting a `.bmp` file
  will open the image viewer.
* **Delete Files or Folders:** Hold and release **Confirm** to delete the selected file or folder. You will be given
  an option to either confirm or cancel. Multiple files can be selected for deletion in a single operation.
* **Rename or Move:** Files can be renamed or moved to a different folder from within the browse screen.

### 3.4 My Books & Apps Screen

Reached from the Home screen's **My Books & Apps** icon, this screen shows every book on your SD card as a cover
grid, alongside a pinned tile for each built-in app you've enabled in **[Settings → Apps](#394-apps)** — Quran,
Tasbih, Stop Watch, Gym, and Games, in that order, followed by your real books. Pinned app tiles show a solid black
cover with the app's name instead of a book cover, and open their app directly instead of the reader.

* Use **Up/Down/Left/Right** to move the selection across the grid.
* Press **Confirm** to open a book, or to launch a pinned app.
* Long-press **Confirm** on a real book to remove it from this view (pinned app tiles can't be removed this way —
  disable them from Settings instead).

### 3.5 File Transfer Screen

The File Transfer screen allows you to upload and manage files on the device. When you enter the screen, choose **Join
a Network**, **Calibre Wireless**, or **Create Hotspot**. The reader then starts the web server for the selected mode.

See the [web server docs](./docs/webserver.md) for more information on how to connect to the web server and upload
files.

The web interface also supports **WebDAV**, allowing you to mount the device as a network drive and manage files
directly from your computer's file manager.

Download links for files already on the device are available in the web interface, so you can retrieve books or
screenshots over Wi-Fi without connecting a cable.

A **Wi-Fi signal strength indicator** (dBm) is displayed on-screen during joined-network web server sessions.

> [!TIP]
> Advanced users can also manage files programmatically or via the command line using `curl`. See the [web server docs](./docs/webserver.md) for details.
> [!TIP]
> If your EPUBs have compatibility issues, you can run the built-in **EPUB Optimizer** directly from the device to clean up and reprocess books for better rendering.

### 3.5.1 Calibre Wireless Transfers

Foulad eInk supports sending books from Calibre using the CrossPoint Reader device plugin.

#### Installing the Plugin in Calibre

If you don't already have the plugin installed:

1. Head to https://github.com/crosspoint-reader/calibre-plugins/releases to download the latest version of the crosspoint_reader plugin.
2. Download the zip file.
3. Open Calibre → Preferences → Plugins → Load plugin from file → Select the zip file.
4. Restart Calibre.

#### Configuring the Plugin in Calibre
1. In Calibre select Preferences.
2. In the Preferences dialog select Plugins.
3. In Plugins search for "crosspoint".
4. Click on "Customize plugin".
5. Update the value for "Host" to match the IP for your device.
6. Leave the other settings as they are.
7. [optional] Modify the "Upload path" to point to a subfolder other than the root "/" folder. Enter this as a path relative to the root folder. Example: `/mybooks`
8. Restart Calibre.

<img width="420" height="385" alt="Image" src="https://github.com/user-attachments/assets/01fc7e33-a9a7-48ba-9e26-2e68d1f9daec" />

#### Uploading Books

To upload a book using the plugin in Calibre:

1. On the device: File Transfer -> Calibre Wireless, then join a network.
2. Select one or more books.
3. Right-click on that selection.
4. Select "Send to Device" > "Send to main memory"

The plugin will connect to your device, create a folder for the book's author in the root folder (or the folder you
configured for the plugin), then copy the book into that folder.

<img width="783" height="310" alt="Image" src="https://github.com/user-attachments/assets/741b0909-2e1d-4f16-8af0-2c43fbda5ce6" />

#### Removing a Book

Books cannot be removed from your device through Calibre. Use the web interface instead.

### 3.6 Foulad eBooks

Foulad eBooks is a self-hosted OPDS catalog with its own dedicated Home-screen entry point, separate from the general
[OPDS Servers](#396-opds-servers-multiple-libraries) feature.

**First-time setup:**

1. Go to **Settings → System → Log In to Foulad eBooks**.
2. Enter your username, then your password. Both fields use a **numeric-only keypad** — Foulad eBooks accounts use
   numeric credentials, so this isn't a mistake if you were expecting a full keyboard.
3. The device automatically reboots into the catalog once you're logged in.

Once logged in, the Home screen's **Files** icon becomes an **eBooks** icon that opens the catalog directly.

**Browsing and downloading:**

* Books are shown as a cover grid, same navigation as [My Books & Apps](#34-my-books--apps-screen).
* If the catalog supports search, a **Search** tile appears in the grid — press **Confirm** on it to type a query.
* Press **Confirm** on a book to download and open it in one step. Re-selecting a book you've already downloaded opens
  it immediately without downloading again.

**Logging out:** Settings → System → **Log Out of Foulad eBooks** (with a confirmation prompt). This reverts the Home
screen's eBooks icon back to **Files**.

### 3.7 Built-in Apps

Snake, Tetris, Sudoku, Maze, a Gym workout planner, a Tasbih dhikr counter, and a Stop Watch are all built into the
firmware — no download required. Each one is pinned as its own tile in **[My Books & Apps](#34-my-books--apps-screen)**
once enabled in **Settings → Apps** (see [3.9.4 Apps](#394-apps)).

#### Gym

A 7-day workout planner and set logger.

**Home grid:** Opening Gym shows a 2-column grid — one card per day (a fixed weekday label, a big day number, and
either the body parts trained that day, an exercise count, or "Rest Day"), plus an **Update** tile in the last slot.
Use **Up/Down/Left/Right** to move between cards and **Confirm** to open one.

* The first time you open Gym, there's no exercise catalog yet — press **Confirm** to sync it over Wi-Fi.
* Selecting the **Update** tile (or holding **Back** on the home grid) downloads any missing exercise photos and
  instructions for exercises already in your plan.
* Holding **Confirm** on a day toggles it as a **Rest Day** — only possible while that day has no exercises assigned.

**Inside a day:** select **Add Exercise** to browse by body part and add exercises to that day (you can add several in
a row without leaving the list); select an existing exercise and use **Up/Down** to adjust its target sets and
**Left/Right** to adjust its target reps; hold **Confirm** on an exercise to remove it from the day.

**Running a workout:** select **Start Workout** to begin. Each screen shows the current exercise, a set-progress
indicator, the exercise photo (if already downloaded), and your current weight × reps.

* **Up/Down** adjust the weight in 2.5 kg steps (or lb, per your [Weight Unit setting](#394-apps)).
* **Confirm** logs the current set and advances to the next set or exercise, remembering your last weight/reps for
  next time.
* **Back** asks to end the workout early; confirming saves your progress and returns to the Gym home grid.

Reps are fixed per set (pre-filled from your last performance or target) and aren't adjustable mid-workout.

#### Tasbih

A dhikr (remembrance) counter.

* **Either Up or Down** increments today's count by one — either button works, so it doesn't matter which hand you
  use.
* **Confirm** asks to reset today's count (your all-time best day and yearly total are untouched).
* **Back** exits the app.

The counter flashes inverted (white-on-black) whenever you reach **33, 99, or 100** for the day. Two stat cards show
your best single-day count ever and your running total for the year.

#### Stop Watch

A simple stopwatch with lap tracking, displayed as `MM:SS:CS` (minutes:seconds:hundredths).

* **Up** starts or pauses the timer.
* **Down** records a lap while running, or resets the timer (and clears all laps) while paused.
* **Back** exits the app. The timer always starts back at zero the next time you open it.

#### Games

Selecting the Games tile opens a picker for the four built-in games: Snake, Tetris, Sudoku, and Maze.

### 3.8 Dictionary

Reached from **Settings → Apps → Dictionary**, or directly from the [Reader Drawer](#5-reader-drawer) while reading
(see [5.5 Dictionary Lookup](#55-dictionary-lookup)).

The Dictionary screen lists:

* **Look up a word** — type a word on the on-device keyboard and look it up.
* **Lookup History** — your past lookups.
* **Download Dictionaries** — browse and install StarDict-format dictionary sets over Wi-Fi. Selecting an installed
  set removes it (along with its generated lookup index); selecting one not yet installed downloads it.
* **Definition Text Size** — Small or Large.
* **Clear History**.
* One row per installed dictionary — selecting it makes that dictionary the active one used for lookups.

No dictionary ships installed by default; download at least one before word lookup works.

### 3.9 Settings

The Settings screen is organized into five tabs: **Display**, **Reader**, **Controls**, **Apps**, and **System**.

#### 3.9.1 Display

- **Sleep Screen**: Which sleep screen to display when the device sleeps (see [Sleep Screen](#310-sleep-screen) for
  full detail on each):

  - "Dashboard" (default) — Clock, battery, current book/progress, reading streak, and a rotating Quran ayah (if
    Quran is enabled).
  - "Dark" — The default dark logo sleep screen.
  - "Light" — The same default sleep screen, on a white background.
  - "Custom" — Custom images from the SD card.
  - "Cover" — The current book's cover image.
  - "Cover + Custom" — The book cover while actively reading, falling back to "Custom" behavior otherwise.
  - "None" — A blank screen.
  - "Quick Resume" — The text of the last page read is shown, with a moon icon at the screen edge. Waking the device
    returns straight to that page, for quickly resuming without a full reload.

- **Sleep Screen Cover Mode**: How to display the book cover when "Cover" or "Cover + Custom" is selected:

  - "Fit" (default) - Scale the image down to fit centered on the screen, padding with white borders as necessary
  - "Crop" - Scale the image down and crop as necessary to try to fill the screen

- **Sleep Screen Cover Filter**: What filter will be applied to the book cover when "Cover" or "Cover + Custom" is selected:

  - "None" (default) - The cover image will be converted to a grayscale image and displayed as it is
  - "Contrast" - The image will be displayed as a black & white image without grayscale conversion
  - "Inverted" - The image will be inverted as in white & black and will be displayed without grayscale conversion

- **Quick Resume on Timeout**: Whether to enable the "Quick Resume" sleep screen when the device goes to sleep due to inactivity (System > Time to Sleep). This overrides the normal Sleep Screen setting when enabled.

- **Status Bar**: Configure the status bar displayed while reading — see [3.9.9 Customise Status Bar](#399-customise-status-bar) for the full set of controls.

- **Hide Battery %**: Configure where to suppress the battery percentage display in the status bar; the battery icon will still be shown:

  - "Never" (default) - Always show battery percentage
  - "In Reader" - Show battery percentage everywhere except in reading mode
  - "Always" - Always hide battery percentage

- **Refresh Frequency**: Set how often the screen does a full refresh while reading to reduce ghosting; options are every 1, 5, 10, 15, or 30 pages.

- **Sunlight Fading Fix**: Configure whether to enable a software-fix for the issue where white X4 models may fade when used in direct sunlight:

  - "OFF" (default) - Disable the fix
  - "ON" - Enable the fix

- **Dark Mode**: Inverts the entire UI (reader text, menus, games) to white-on-black.

> [!NOTE]
> A battery charging indicator is shown on the battery icon whenever the device is actively charging.

> [!NOTE]
> Foulad eInk ships a single, fixed UI theme — there's no separate theme picker.

#### 3.9.2 Reader

- **Manage Fonts**: Browse, download, and manage custom font families installed from the SD card. See [Custom Fonts (SD Card)](#311-custom-fonts-sd-card) for more information.

- **English Font**: Choose the font used for Latin-script reading — "Bitter" (default) or "Lexend Deca", plus any
  custom fonts installed from the SD card.

- **English Font Size**: Adjust the text size; options are "Small", "Medium" (default), "Large", or "X Large".

- **Arabic Font**: Choose the font used for Arabic-script reading — "Noto Naskh Arabic" (default), "Uthmanic Hafs",
  or "Tajawal", plus any custom Arabic fonts installed from the SD card.

- **Arabic Font Size**: Adjust the Arabic text size independently of the English size.

- **Reader Line Spacing**: Adjust the spacing between lines; options are "Tight", "Normal" (default), or "Wide".

- **Reader Screen Margin**: Controls the screen margins in Reading Mode between 5 and 40 pixels in 5-pixel increments.

- **Reader Paragraph Alignment**: Set the alignment of paragraphs; options are "Justified" (default), "Left", "Center", "Right", or "Book's Style" (use the EPUB's own alignment).

- **Embedded Style**: Whether to use the EPUB file's embedded HTML/CSS styling and formatting (default ON).

- **Focus Reading**: Bolds the first part of each word to create visual fixation points, similar to Bionic Reading. This can help improve reading speed and focus (default OFF).

- **Hyphenation**: Whether to hyphenate text in Reading Mode.

- **Track Reading Stats**: Whether to record reading time/streaks for the Stats screen and Dashboard sleep screen.

- **Daily Reading Goal**: Set a daily reading-time target — 15 min, 30 min, 45 min, 1 hour, 1.5 hours, or 2 hours —
  tracked on the Stats screen and shown as a progress ring on the Dashboard sleep screen.

- **Reading Orientation**: Set the screen orientation for reading EPUB files:

  - "Portrait" (default) - Standard portrait orientation
  - "Landscape CW" - Landscape, rotated clockwise
  - "Inverted" - Portrait, upside down
  - "Landscape CCW" - Landscape, rotated counter-clockwise

- **Extra Paragraph Spacing**: Set how to handle paragraph breaks:

  - "ON" - Vertical space will be added between paragraphs in Reading Mode
  - "OFF" - Paragraphs will not have vertical space added, but will have first-line indentation

- **Text Anti-Aliasing**: Whether to show smooth grey edges (anti-aliasing) on text in reading mode. Note this slows down page turns slightly.

- **Images**: How to handle embedded images (JPG/PNG) found in EPUB files:

  - "Display" (default) - Render images normally
  - "Placeholder" - Show an `[Image]` placeholder instead of rendering
  - "Suppress" - Skip images entirely

- **Customise Status Bar**: Opens a dedicated sub-screen — see [3.9.9 Customise Status Bar](#399-customise-status-bar).

#### 3.9.3 Controls

- **Remap Front Buttons**: A menu for customising the function of each bottom edge button.

- **Side Button Layout (reader)**: Swap the order of the Up and Down side buttons from "Prev/Next" (default) to "Next/Prev". You can also disable them entirely. This change is only in effect when reading.

- **Orient front buttons**: Whether the front buttons' logical direction follows the current [Reading Orientation](#392-reader) (so "forward" always matches the rotated layout).

- **Long-press button behavior**: What holding a page-turn button does:

  - "OFF" (default) - No special behavior
  - "Chapter skip" - Holding skips to the next/previous chapter
  - "Orientation change" - Holding cycles the reading orientation

- **Long-press Menu**: Selects the function bound to holding the Confirm button while reading an EPUB. A short press of Confirm always opens the [Reader Drawer](#5-reader-drawer) as normal:

  - "Bookmark" (default) - Hold Confirm (~0.4 second) to drop a bookmark at the current page.
  - "KOSync" - Hold Confirm (~1 second) to launch KOReader sync directly.
  - "Disabled" - Long-press is ignored; only short-press opens the reader drawer.

- **Short Power Button Click**: Controls the effect of a short click of the power button:

  - "Ignore" (default) - Require a long press to turn off the device
  - "Sleep" - A short press puts the device into sleep mode
  - "Page Turn" - A short press in reading mode turns to the next page; a long press turns the device off
  - "Refresh Screen" - A short press triggers a manual full-screen refresh, useful for clearing ghosting
  - "Footnotes" - A short press in reading mode opens the footnotes submenu; if only one footnote is present on the page, the referenced page is opened directly. Once this is selected, a **Quick-return from footnotes** toggle appears, letting the short power-button press also act as a back button from the footnotes page.

- **Tilt Page Turn (X3 only)**: Turn pages by tilting the device, using the X3's built-in gyroscope. Options are OFF (default), Normal, or Inverted. This setting only appears on X3 hardware.

#### 3.9.4 Apps

- **The Holy Quran**: Extracts the bundled Quran (Uthmani script) to the SD card and pins it in [My Books & Apps](#34-my-books--apps-screen).
- **Games**: Pins a Games tile — see [Games](#games).
- **Tasbih**: Pins a Tasbih tile — see [Tasbih](#tasbih).
- **Stop Watch**: Pins a Stop Watch tile — see [Stop Watch](#stop-watch).
- **Gym**: Pins a Gym tile — see [Gym](#gym).
- **Weight Unit**: Kilograms (kg, default) or Pounds (lb) — used throughout the Gym workout screens.
- **Dictionary**: Opens the [Dictionary](#38-dictionary) screen. Always available; no toggle to hide it.
- **KOReader Sync**: Opens the KOReader Sync sub-screen — see [3.9.8 KOReader Sync Quick Setup](#398-koreader-sync-quick-setup).
- **Debug**: Enables rolling diagnostic logs written to the SD card, useful when reporting an issue.

#### 3.9.5 System

- **Browse Files**: Opens the [Browse Files](#33-browse-files-screen) screen.
- **File Transfer**: Opens the [File Transfer](#35-file-transfer-screen) screen.
- **Time to Sleep**: Set the duration of inactivity before the device automatically goes to sleep; from 1 to 30 minutes, or "Never".
- **Show Hidden Files**: Whether hidden (dot-prefixed) files/folders appear in Browse Files.
- **Clear Read Books from Recent List**: Automatically remove finished books from the recents row.
- **Move Finished Books to Read Folder**: Automatically move finished books into a "Read" folder on the SD card.
- **Wi-Fi Networks**: Connect to Wi-Fi networks for file transfers, firmware updates, and OPDS/Foulad eBooks browsing.
- **OPDS Servers**: Manage one or more OPDS [(Open Publication Distribution System)](https://en.wikipedia.org/wiki/Open_Publication_Distribution_System) libraries for browsing and downloading books. See [OPDS Servers (Multiple Libraries)](#396-opds-servers-multiple-libraries) below.
- **Clear Reading Cache**: Clear the internal SD card cache.
- **Check for updates**: Check for firmware updates over Wi-Fi. Firmware can also be updated without a USB connection by placing a `firmware.bin` file on the SD card.
- **SD Card Firmware Update**: Install a firmware update from a `firmware.bin` file placed on the SD card, without needing Wi-Fi.
- **Language**: Set the UI language. Foulad eInk ships **English and Arabic** — a deliberately small, fully-translated
  set rather than a long list of partial translations. See [Arabic & Right-to-Left Reading](#arabic--right-to-left-reading).
- **Log In to Foulad eBooks** / **Log Out of Foulad eBooks**: See [3.6 Foulad eBooks](#36-foulad-ebooks).

#### 3.9.6 OPDS Servers (Multiple Libraries)

Foulad eInk supports saving multiple OPDS servers and switching between them when browsing catalogs. This is separate
from [Foulad eBooks](#36-foulad-ebooks), which has its own dedicated login flow.

1. Open **Settings -> System -> OPDS Servers**.

2. Select **Add Server** to create a new entry, or select an existing server to edit it.

3. Configure these fields:

   - **Server Name**: Optional display name (for example, "Home Calibre" or "Public Catalog").

   - **OPDS Server URL**: Full catalog root URL (for Calibre Content Server, usually ends with `/opds`).

   - **Username / Password**: Optional credentials for authenticated servers.

4. Use **Delete Server** inside a server entry to remove it.

Behavior notes:

- You can store up to 8 OPDS servers.
- OPDS authentication supports HTTP Basic auth. If you use Calibre Content Server with authentication enabled, set it to Basic (not Digest).

You can also manage OPDS servers from the web interface while in File Transfer mode:

1. Connect to the device web UI.
2. Open `http://<device-ip>/settings`.
3. Use the **OPDS Servers** card to add, edit, or delete entries.

For web-based Wi-Fi network management, see [Web Settings (Wi-Fi + OPDS)](#397-web-settings-wi-fi--opds).

#### 3.9.7 Web Settings (Wi-Fi + OPDS)

While in **File Transfer** mode, the web settings page includes management cards for both **Wi-Fi Networks** and **OPDS Servers**.

1. On device: open **File Transfer** and connect through **Join a Network** or **Create Hotspot**.
2. In a browser, open `http://<device-ip>/settings`.
3. In **Wi-Fi Networks**, add, edit, or delete saved network entries (SSID + optional password).
4. In **OPDS Servers**, add, edit, or delete OPDS catalogs.

Behavior notes:

- Passwords are never shown back in the web UI after saving.
- Leaving Password blank while editing keeps the existing saved password unchanged.
- The web UI can save hidden-network SSIDs, but connecting to hidden networks still depends on the device-side Wi-Fi connection flow.

#### 3.9.8 KOReader Sync Quick Setup

Foulad eInk can sync reading progress with KOReader-compatible sync servers.
It also interoperates with KOReader apps/devices when they use the same server and credentials.

##### Option A: Free Public Server (`sync.koreader.rocks`)

1. Register a user once (only if needed):

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "https://sync.koreader.rocks/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

Already have KOReader Sync credentials? Skip registration; basic sync only requires using the same existing username/password on all devices.

When this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, pick a different username or use that existing account.

2. On each device:

   - Go to **Settings -> Apps -> KOReader Sync**.

   - Set **Username** and **Password** (enter the plain password; the device computes MD5 internally, and use the same values on all devices).

   - Set **Sync Server URL** to `https://sync.koreader.rocks`, or leave it empty (both use the same default KOReader sync server).

   - Run **Authenticate**.

3. While reading, open the **[Reader Drawer](#5-reader-drawer)** and select **Sync Progress** from the Settings tab.

   - Choose **Apply Remote** to jump to remote progress.

   - Choose **Upload Local** to push current progress.

##### Option B: Self-Hosted Server (Docker Compose)

1. Start a sync server:

```bash
mkdir -p kosync-quickstart
cd kosync-quickstart

cat > compose.yaml <<'YAML'
services:
  kosync:
    image: koreader/kosync:latest
    ports:
      - "7200:7200"
      - "17200:17200"
    volumes:
      - ./data/redis:/var/lib/redis
    environment:
      - ENABLE_USER_REGISTRATION=true
    restart: unless-stopped
YAML

# Docker
docker compose up -d

# Podman (alternative)
podman compose up -d
```

> [!NOTE]
> `ENABLE_USER_REGISTRATION=true` is convenient for first setup. After creating your users, set it to `false` (or remove it) to avoid unexpected registrations.

2. Verify the server:

```bash
curl -H "Accept: application/vnd.koreader.v1+json" "http://<server-ip>:17200/healthcheck"
# Expected: {"state":"OK"}
```

3. Register a user once.
   The device authenticates against KOReader Sync (`koreader/kosync`) using an MD5 key, so register using the MD5 of your password:

> [!WARNING]
> Sending a reusable MD5-derived password over plain HTTP is insecure.
> Create unique sync-only credentials and do not reuse main account passwords.
> Prefer `https://<server-ip>:7200` whenever traffic leaves a fully trusted LAN or when using untrusted networks.
> Use `curl -k` only for self-signed certificate testing.

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "http://<server-ip>:17200/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

If this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, the account already exists.

4. On each device:

   - Go to **Settings -> Apps -> KOReader Sync**.

   - Set **Username** and **Password** (enter the plain password; the device computes MD5 internally, and use the same values on all devices).

   - Set **Sync Server URL** to `http://<server-ip>:17200`.

   - Run **Authenticate**.

If you use the HTTPS listener, use `https://<server-ip>:7200` (`curl -k` only for self-signed certificate testing).

5. While reading, open the **[Reader Drawer](#5-reader-drawer)** and select **Sync Progress** from the Settings tab.

   - Choose **Apply Remote** to jump to remote progress.

   - Choose **Upload Local** to push current progress.

#### 3.9.9 Customise Status Bar

Reached from **Settings → Reader → Customise Status Bar**, this sub-screen controls exactly what appears in the
status bar while reading:

- **Chapter Page Count**: Show the current page number within the chapter.
- **Book Progress Percentage**: Show overall book progress as a percentage.
- **Progress Bar**: Book, Chapter, or Hide.
- **Progress Bar Thickness**: Thin, Medium, or Thick.
- **Title**: Book, Chapter, or Hide.
- **Battery**: Toggle the battery indicator.
- **XTC Status Bar**: For `.xtc`/`.xtch` files specifically — Hide, Bottom, or Top.
- **Clock**: Hide, Left, or Right (X3 only — X4 has no real-time clock).
- **Clock Format**: 24-hour or 12-hour.

### 3.10 Sleep Screen

The **Sleep Screen** setting controls what is displayed when the device goes to sleep:

| Mode                  | Behavior                                                                                                                     |
| ---------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| **Dashboard** (default) | Clock (X3 only) and battery, your current book's cover/title/author/progress, a 2×2 stats grid (time left, streak, total reading time, books finished), a daily-goal progress ring, a reader-level badge, and a rotating Quran ayah if Quran is enabled. Falls back to the plain logo screen if no book is open. |
| **Dark**               | The logo on a dark background.                                                                                              |
| **Light**              | The logo on a white background.                                                                                             |
| **Custom**             | A custom image from the SD card (see below). Falls back to **Dark** if no custom image is found.                             |
| **Cover**              | The cover of the currently open book. Falls back to **Dark** if no book is open.                                             |
| **Cover + Custom**     | The cover of the currently open book, shown only while actively reading. Falls back to **Custom** behavior when not reading. |
| **None**               | A blank screen.                                                                                                              |
| **Quick Resume**       | The text of the last page read, for near-instant resume.                                                                     |

#### Cover settings

When using **Cover** or **Cover + Custom**, two additional settings apply:

- **Sleep Screen Cover Mode**: **Fit** (scale to fit, white borders) or **Crop** (scale and crop to fill the screen).
- **Sleep Screen Cover Filter**: **None** (grayscale), **Contrast** (black & white), or **Inverted** (inverted black & white).

#### Custom images

To use custom sleep images, set the sleep screen mode to **Custom** or **Cover + Custom**, then place images on the SD card:

- **Multiple Images (recommended):** Create a `.sleep` directory in the root of the SD card and place any number of `.bmp` images inside. One will be randomly selected each time the device sleeps. (A directory named `sleep` is also accepted as a fallback.)
- **Single Image:** Place a file named `sleep.bmp` in the root directory. This is used as a fallback if no valid images are found in the `.sleep`/`sleep` directory.

> [!TIP]
> For best results:
>
> - Use uncompressed BMP files with 24-bit color depth
> - X4: Use a resolution of 480x800 pixels to match the device's screen resolution.
> - X3: Use a resolution of 528x792 pixels to match the device's screen resolution.

> [!TIP]
> You can set an image as the sleep screen cover directly from the BMP image viewer in the **[Browse Files](#33-browse-files-screen)** screen.

---

### 3.11 Custom Fonts (SD Card)

Foulad eInk supports loading additional fonts from the SD card, extending beyond the built-in families (Bitter and
Lexend Deca for Latin script; Noto Naskh Arabic, Uthmanic Hafs, and Tajawal for Arabic script). Custom fonts can
include extended Unicode coverage, enabling CJK (Chinese, Japanese, Korean) and other scripts.

There are three ways to install fonts:

1. **Download from device (recommended):** Go to **Settings -> Reader -> Manage Fonts**, browse the available font families, and select one to download over Wi-Fi.
2. **Upload via web interface:** While in **File Transfer** mode, open the web UI in a browser and navigate to the **Fonts** tab to upload `.cpfont` files.
3. **Manual SD card copy:** Copy `.cpfont` files to `/.fonts/` (preferred) or `/fonts/` on your SD card.

Once installed, custom fonts appear in **Settings → Reader → English Font** or **Arabic Font**, alongside the built-in
fonts.

See [docs/sd-card-fonts.md](./docs/sd-card-fonts.md) for full installation details and SD card folder structure.

---

## 4. Reading Mode

Once you have opened a book, the button layout changes to facilitate reading.

### Page Turning

| Action            | Buttons                     |
| ----------------- | ----------------------------- |
| **Previous Page** | Press **Left** _or_ **Up**   |
| **Next Page**     | Press **Right** _or_ **Down** |

The role of the side buttons can be swapped in the **[Controls Settings](#393-controls)**.

If the **Short Power Button Click** setting is set to "Page Turn", you can also turn to the next page by briefly pressing the Power button.

> [!NOTE]
> Page-turn button direction does **not** automatically swap based on whether a book is right-to-left — see
> [Arabic & Right-to-Left Reading](#arabic--right-to-left-reading) for what actually changes for RTL books.

### Chapter Navigation

* **Next Chapter:** Press and **hold** the **Right** (or **Down**) button briefly, then release.
* **Previous Chapter:** Press and **hold** the **Left** (or **Up**) button briefly, then release.

This feature is controlled by the **Long-press button behavior** setting in **[Controls Settings](#393-controls)**.

### Auto Page Turn

Auto Page Turn automatically advances pages at a set interval, useful for hands-free reading. This feature can be enabled and configured from the **[Reader Drawer](#5-reader-drawer)**'s Settings tab while reading an EPUB.

### Tilt Page Turn (X3 only)

On the **Xteink X3**, the built-in gyroscope can be used to turn pages by tilting the device. This feature is available in the Controls settings, and only appears on X3 hardware.

### Footnote Navigation

When reading an EPUB that contains footnotes, you can navigate to the footnote text by selecting the footnote reference in the book. From the footnote, you can return to your original reading position.

If the device goes to sleep or you close the book while viewing a footnote, the book reopens to your original reading position, not the footnote.

### System Navigation

* **Return to Home:** Press the **Back** button to close the book and return to the **[Home](#31-home-screen)** screen.
* **Return to Browse Files:** Press and hold the **Back** button to close the book and return to the **[Browse Files](#33-browse-files-screen)** screen.
* **Reader Drawer:** Press **Confirm** to open the **[Reader Drawer](#5-reader-drawer)**, which includes chapter navigation, reading options, dictionary lookup, and more.
* **Long-press Confirm (configurable):** Holding **Confirm** runs the function chosen by the **Long-press Menu** setting in **[Controls Settings](#393-controls)** — "Bookmark" (default) drops a bookmark, "KOSync" launches KOReader Sync, "Disabled" does nothing. A short press always opens the Reader Drawer.

### Arabic & Right-to-Left Reading

Setting the UI **Language** to Arabic (Settings → System → Language) does two distinct things:

1. **The entire app UI mirrors right-to-left** — the Home screen's cover position, text alignment, menu order, and
   status bar all flip, and front-button **Left/Right now navigate menus and lists** in the RTL direction (so "next
   item" is still the button your thumb expects). This applies to menus, settings, and list navigation throughout the
   firmware.
2. **Arabic-script book text renders correctly** — proper letter shaping, right-to-left layout, and justified lines
   that stretch the way a real page does, regardless of the UI language setting. This also covers Persian, Ottoman
   Turkish, and Kurdish text, which share the Arabic script.

> [!NOTE]
> The reader's actual **page-turn side buttons** ([Page Turning](#page-turning) above) do **not** automatically swap
> direction based on a book's script — "next page" is always the same physical button unless you change it yourself
> via **Side Button Layout** or **Reading Orientation** in Controls Settings. The RTL swap only affects menu/list
> navigation (chapters, bookmarks, settings), not the reader's own forward/backward page turn.

### Supported Languages

Foulad eInk's **UI** ships in English and Arabic only — a deliberate choice to keep both fully and correctly
translated rather than offering many partial translations (see [Language](#395-system) setting).

For **book content**, the built-in reader fonts render:

* **Arabic script** (and the Persian, Ottoman Turkish, and Kurdish letters built on it) — Noto Naskh Arabic, Uthmanic Hafs, and Tajawal.
* **Latin script** (Basic, Supplement, Extended-A/B) — covering English, German, French, Spanish, Portuguese, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Polish, Czech, Hungarian, Romanian, Slovak, Slovenian, Turkish, Catalan, and others.
* **Cyrillic script** (Standard and Extended) — covering Russian, Ukrainian, Belarusian, Bulgarian, Serbian, Macedonian, Kazakh, Kyrgyz, Mongolian, and others.
* **Vietnamese** — via extended Latin glyph coverage.

What is not supported with built-in reader fonts: Chinese, Japanese, Korean, Greek, and Hebrew. **These, and any other
extended script, can be enabled by installing custom SD card fonts** — see [Custom Fonts (SD Card)](#311-custom-fonts-sd-card).

---

## 5. Reader Drawer

Press **Confirm** while reading to open the Reader Drawer — a two-tab menu (**Reading** and **Settings**) for
in-book utilities and navigation, without leaving the page. Press **Back** at any time to close it and return to
your current page.

### 5.1 Reading Tab

Most-used actions first:

- **Dictionary** – Look up a word on the current page *(only shown once a dictionary is installed — see [5.5 Dictionary Lookup](#55-dictionary-lookup))*.
- **Select Chapter** – Open the table of contents to jump to a specific chapter (see [Chapter Selection](#53-chapter-selection) below).
- **Toggle Bookmark** – Add or remove a bookmark at the current page.
- **Bookmarks** – Open your saved bookmarks for this book *(only shown once you have at least one)*.
- **Font Size** – Adjust the reading font size for this book.
- **Font Name** – Switch reading font for this book *(shown for Arabic books, or Latin books once SD-card fonts are installed)*.
- **Line Spacing** – Adjust line spacing for this book.
- **Text Alignment** – Adjust paragraph alignment for this book.
- **Reading Orientation** – Cycle through screen orientations without leaving the reader.

### 5.2 Settings Tab

- **Footnotes** – Navigate to the footnotes for the current section *(only shown in books that contain footnotes)*.
- **Lookup History** – Your past dictionary lookups *(only shown once a dictionary is installed)*.
- **Go to %** – Jump to a specific position in the book by percentage.
- **Auto Turn (Pages Per Minute)** – Cycle through automatic page turn speed options for hands-free reading.
- **Reset Book Settings** – Clear any per-book font/spacing/alignment overrides, reverting to your global Reader settings.
- **Take Screenshot** – Save a screenshot of the current page to the `screenshots/` folder.
- **Show page as QR** – Display a QR code encoding the current reading position.
- **Sync Progress** – Push or pull reading progress with a KOReader sync server (see [KOReader Sync Quick Setup](#398-koreader-sync-quick-setup)).
- **Delete Book Cache** – Clear the cached layout data for the current book, forcing a re-index on next open.
- **Go Home** – Close the book and return to the Home screen.

### 5.3 Chapter Selection

Accessible by selecting **Select Chapter** from the Reading tab.

1. Use **Left** (or **Up**), or **Right** (or **Down**) to highlight the desired chapter.
2. Press **Confirm** to jump to that chapter.
3. *Alternatively, press **Back** to cancel and return to your current page.*

---

### 5.4 Bookmarks

Bookmarks can be created to quickly save and restore your place in a book, either via the drawer's **Toggle Bookmark**
row, or with the **hold-Confirm** shortcut described below.

To create a bookmark with the shortcut, hold **Confirm** for about half a second while inside a book. A popup will appear letting you know a bookmark was created. The popup message will automatically disappear in a couple of seconds.

To open bookmarks, open the **Reader Drawer** and select **Bookmarks** from the Reading tab. Bookmarks can be opened by navigating to them and pressing **Confirm**, which will redirect you to that place in the book. You can delete bookmarks by holding **Confirm** for about 0.7 seconds, and then pressing **Confirm** again to confirm deletion, or **Back** to cancel.

Bookmarks are stored in the `.crosspoint/bookmarks` folder in JSON format.

### 5.5 Dictionary Lookup

Once at least one dictionary is installed (see [3.8 Dictionary](#38-dictionary)), a **Dictionary** row appears at the
top of the Reader Drawer's Reading tab. Selecting it overlays the current page with one word highlighted, ready to
look up:

* **Left / Right** – move the highlight to the previous/next word (wrapping across lines).
* **Up / Down** – jump the highlight up/down a full line, keeping horizontal position.
* **Confirm** – look up the highlighted word (automatically rejoining a word split across a line break by hyphenation), showing its definition, a list of close suggestions if it isn't found exactly, or a "not found" message.
* **Back** – cancel and return to the page.

---

## 6. Current Limitations & Roadmap

Please note that this firmware is currently in active development. The following are known limitations:

* **Cover Images:** Large cover images embedded into EPUB require several seconds (~10s for ~2000 pixel tall image) to convert for sleep screen and home screen thumbnail. Consider optimizing the EPUB with e.g. https://github.com/bigbag/epub-to-xtc-converter to speed this up.
* **Unsupported Image Formats:** Most JPG and PNG images in EPUBs render correctly. GIFs and progressive JPEGs are not supported and will fall back to an `[Image]` placeholder.

---

## 7. Troubleshooting Issues & Escaping Bootloop

If an issue or crash is encountered while using Foulad eInk, feel free to raise an issue ticket and attach the logs.

**Crash reports on SD card:** After a crash, the firmware automatically saves a crash report to the SD card (no USB connection needed). Check the root of the SD card for a crash log file and include it with any bug report.

**Serial monitor logs:** For more detailed debugging, connect the device to a computer and run the custom debugging monitor script (requires Python 3 with `pyserial`, `colorama`, and `matplotlib`; install via `pip3 install pyserial colorama matplotlib`):

```
python3 scripts/debugging_monitor.py
```

The script auto-detects the serial port. You can also specify one explicitly:

```
python3 scripts/debugging_monitor.py /dev/ttyACM0        # Linux
python3 scripts/debugging_monitor.py /dev/tty.usbmodem1  # macOS
python3 scripts/debugging_monitor.py COM7                # Windows
```

**Features:**

- Color-coded log output by category (errors, memory, display, EPUB parsing, etc.)
- Live memory usage graph (free RAM, total RAM, max contiguous allocation) updated every second
- Interactive command prompt — type a command and press Enter to send it to the device
- Screenshot capture — saves the current display to `screenshot.bmp` when triggered by the device

**Options:**

| Option               | Description                                               |
| -------------------- | --------------------------------------------------------- |
| `--baud RATE`        | Baud rate (default: 115200)                               |
| `--filter KEYWORD`   | Show only lines containing the keyword (case-insensitive) |
| `--suppress KEYWORD` | Hide lines containing the keyword (case-insensitive)      |

**Examples:**

```
# Show only memory-related log lines
python3 scripts/debugging_monitor.py --filter MEM

# Hide noisy SD card log lines
python3 scripts/debugging_monitor.py --suppress "[SD]"
```

Press **Ctrl-C** or close the graph window to exit.

If the device is stuck in a bootloop, press and release the Reset button. Then, press and hold on to the configured Back button and the Power Button to boot to the Home Screen.

There can be issues with broken cache or config. In this case, delete the `.crosspoint` directory on your SD card (or consider deleting only `settings.json`, `state.json`, or `epub_*` cache directories in the `.crosspoint/` folder).
