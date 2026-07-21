#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>

#include <cstring>

#include "ArabicFontSystem.h"
#include "CrossPointSettings.h"
#include "QuranBook.h"
#include "CrossPointState.h"
#include "GameHighScoresStore.h"
#include "TasbihStore.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/OtaUpdateActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "util/BatteryDiagLog.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"
#include "util/SleepDiagLog.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
ArabicFontSystem arabicFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Fonts
EpdFont bitter14RegularFont(&bitter_14_regular);
EpdFont bitter14BoldFont(&bitter_14_bold);
EpdFont bitter14ItalicFont(&bitter_14_italic);
EpdFont bitter14BoldItalicFont(&bitter_14_bolditalic);
EpdFontFamily bitter14FontFamily(&bitter14RegularFont, &bitter14BoldFont, &bitter14ItalicFont,
                                 &bitter14BoldItalicFont);
EpdFont lexenddeca14RegularFont(&lexenddeca_14_regular);
EpdFont lexenddeca14BoldFont(&lexenddeca_14_bold);
EpdFontFamily lexenddeca14FontFamily(&lexenddeca14RegularFont, &lexenddeca14BoldFont);
#ifndef OMIT_FONTS
EpdFont bitter12RegularFont(&bitter_12_regular);
EpdFont bitter12BoldFont(&bitter_12_bold);
EpdFont bitter12ItalicFont(&bitter_12_italic);
EpdFont bitter12BoldItalicFont(&bitter_12_bolditalic);
EpdFontFamily bitter12FontFamily(&bitter12RegularFont, &bitter12BoldFont, &bitter12ItalicFont,
                                 &bitter12BoldItalicFont);
EpdFont bitter16RegularFont(&bitter_16_regular);
EpdFont bitter16BoldFont(&bitter_16_bold);
EpdFont bitter16ItalicFont(&bitter_16_italic);
EpdFont bitter16BoldItalicFont(&bitter_16_bolditalic);
EpdFontFamily bitter16FontFamily(&bitter16RegularFont, &bitter16BoldFont, &bitter16ItalicFont,
                                 &bitter16BoldItalicFont);
EpdFont bitter18RegularFont(&bitter_18_regular);
EpdFont bitter18BoldFont(&bitter_18_bold);
EpdFont bitter18ItalicFont(&bitter_18_italic);
EpdFont bitter18BoldItalicFont(&bitter_18_bolditalic);
EpdFontFamily bitter18FontFamily(&bitter18RegularFont, &bitter18BoldFont, &bitter18ItalicFont,
                                 &bitter18BoldItalicFont);

// Digit-only 32pt font for the Tasbih app's counter -- none of the reader
// sizes above go this large. Registered as a single "regular" style; the
// font file itself is bold weight (see convert-builtin-fonts.sh).
EpdFont tasbih32BoldFont(&tasbih_32_bold);
EpdFontFamily tasbih32FontFamily(&tasbih32BoldFont);

EpdFont lexenddeca12RegularFont(&lexenddeca_12_regular);
EpdFont lexenddeca12BoldFont(&lexenddeca_12_bold);
EpdFontFamily lexenddeca12FontFamily(&lexenddeca12RegularFont, &lexenddeca12BoldFont);
EpdFont lexenddeca16RegularFont(&lexenddeca_16_regular);
EpdFont lexenddeca16BoldFont(&lexenddeca_16_bold);
EpdFontFamily lexenddeca16FontFamily(&lexenddeca16RegularFont, &lexenddeca16BoldFont);
EpdFont lexenddeca18RegularFont(&lexenddeca_18_regular);
EpdFont lexenddeca18BoldFont(&lexenddeca_18_bold);
EpdFontFamily lexenddeca18FontFamily(&lexenddeca18RegularFont, &lexenddeca18BoldFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

// UI_10_FONT_ID/UI_12_FONT_ID are backed by Inter (was Ubuntu) -- see
// lib/EpdFont/scripts/convert-builtin-fonts.sh for the swap rationale. Macro
// names kept unchanged so no other call site needs touching.
EpdFont ui10RegularFont(&inter_10_regular);
EpdFont ui10BoldFont(&inter_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&inter_12_regular);
EpdFont ui12BoldFont(&inter_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// Built-in default for ArabicFontSystem -- always present in flash so Arabic titles
// render out of the box with no SD card setup. Bundled at the three sizes actually
// used for Arabic-eligible UI text (SMALL_FONT_ID=8pt, UI_10_FONT_ID=10pt,
// UI_12_FONT_ID=12pt) so Arabic renders at the same size/baseline as the Latin text
// around it instead of one fixed size that overflows small rows/grid cells. Users can
// still override with a custom SD font family via Settings -> Reader -> Arabic Font.
EpdFont notosansarabic8RegularFont(&notosansarabic_8_regular);
EpdFontFamily notosansarabic8FontFamily(&notosansarabic8RegularFont);
EpdFont notosansarabic10RegularFont(&notosansarabic_10_regular);
EpdFontFamily notosansarabic10FontFamily(&notosansarabic10RegularFont);
EpdFont notosansarabic12RegularFont(&notosansarabic_12_regular);
EpdFontFamily notosansarabic12FontFamily(&notosansarabic12RegularFont);

// Arabic reading-text font family (Noto Naskh Arabic, OFL-licensed), bundled at the
// same four reading sizes as the Latin fonts above (12/14/16/18pt = Small/Medium/
// Large/X-Large) so Arabic body text can match the user's chosen size independently
// of the Latin reading font. Other families were trimmed from flash to keep OTA
// images small; custom families can still be loaded from the SD card (Settings ->
// Reader -> Arabic Font). See ArabicFontSystem.cpp for family/size -> font ID
// resolution.

EpdFont notonaskharabic12RegularFont(&notonaskharabic_12_regular);
EpdFontFamily notonaskharabic12FontFamily(&notonaskharabic12RegularFont);
EpdFont notonaskharabic14RegularFont(&notonaskharabic_14_regular);
EpdFontFamily notonaskharabic14FontFamily(&notonaskharabic14RegularFont);
EpdFont notonaskharabic16RegularFont(&notonaskharabic_16_regular);
EpdFontFamily notonaskharabic16FontFamily(&notonaskharabic16RegularFont);
EpdFont notonaskharabic18RegularFont(&notonaskharabic_18_regular);
EpdFontFamily notonaskharabic18FontFamily(&notonaskharabic18RegularFont);

// KFGQPC Uthmanic Hafs (King Fahd Complex; use/copy/distribute permitted,
// modification not permitted -- see conversion note in
// lib/EpdFont/builtinFonts/source/UthmanicHafs/): the Madinah Mushaf's own
// typeface, Hafs 'an 'Asim riwayah -- the built-in Quranic reading family (see
// QuranBook.h). Same four reading sizes as Naskh.
EpdFont uthmanichafs12RegularFont(&uthmanichafs_12_regular);
EpdFontFamily uthmanichafs12FontFamily(&uthmanichafs12RegularFont);
EpdFont uthmanichafs14RegularFont(&uthmanichafs_14_regular);
EpdFontFamily uthmanichafs14FontFamily(&uthmanichafs14RegularFont);
EpdFont uthmanichafs16RegularFont(&uthmanichafs_16_regular);
EpdFontFamily uthmanichafs16FontFamily(&uthmanichafs16RegularFont);
EpdFont uthmanichafs18RegularFont(&uthmanichafs_18_regular);
EpdFontFamily uthmanichafs18FontFamily(&uthmanichafs18RegularFont);

// Tajawal (Boutros International, OFL-licensed, from Google Fonts): a modern
// geometric-sans reading option, alongside the two traditional book-printing
// styles above. Same four reading sizes as Naskh/UthmanicHafs, plus 8/10pt so
// it can also serve as the Arabic UI-chrome font (see applyArabicMappings) --
// 12pt is shared with the reading tier, so only 8/10 need their own objects.
EpdFont tajawal8RegularFont(&tajawal_8_regular);
EpdFontFamily tajawal8FontFamily(&tajawal8RegularFont);
EpdFont tajawal10RegularFont(&tajawal_10_regular);
EpdFontFamily tajawal10FontFamily(&tajawal10RegularFont);
EpdFont tajawal12RegularFont(&tajawal_12_regular);
EpdFontFamily tajawal12FontFamily(&tajawal12RegularFont);
EpdFont tajawal14RegularFont(&tajawal_14_regular);
EpdFontFamily tajawal14FontFamily(&tajawal14RegularFont);
EpdFont tajawal16RegularFont(&tajawal_16_regular);
EpdFontFamily tajawal16FontFamily(&tajawal16RegularFont);
EpdFont tajawal18RegularFont(&tajawal_18_regular);
EpdFontFamily tajawal18FontFamily(&tajawal18RegularFont);

// Quran Common (same King Fahd Complex-adjacent provenance as UthmanicHafs above --
// see lib/EpdFont/builtinFonts/source/QuranCommon/): a tiny 1-glyph dedicated font
// carrying the real Bismillah ligature (U+FDFD), which UthmanicHafs itself lacks.
// Routed to via GfxRenderer::setBismillahFontId, not the general Arabic font map --
// see the Bismillah marker branch in GfxRenderer.cpp. Single fixed 18pt size, not
// one per reading tier: the glyph is a whole vocalized phrase baked into one wide
// glyph (241px at 18pt), and EpdGlyph::width is a uint8_t (255px cap) -- any size
// above ~18pt pushes the glyph past that ceiling. 18pt is the largest safe size, so
// it's used for every Arabic Font Size setting.
EpdFont quranCommon18RegularFont(&quran_common_18_regular);
EpdFontFamily quranCommon18FontFamily(&quranCommon18RegularFont);

// Surah banner (same King Fahd Complex-adjacent provenance -- see
// lib/EpdFont/builtinFonts/source/SurahNameV4/): 114 calligraphic surah-name
// glyphs baked from surah-name-v4.ttf via fontconvert.py's --glyph-map (this font
// has no cmap entries at all -- see the NOTICE.md there and
// tools/quran/gen_surah_banner_glyphmap.py). Routed to via
// GfxRenderer::parseSurahBannerMarker, not the general Arabic font map. Single
// fixed 24pt size, not one per reading tier -- this is a once-per-surah chrome
// element, not line-by-line reading text.
EpdFont surahBanner24RegularFont(&surah_banner_24_regular);
EpdFontFamily surahBanner24FontFamily(&surahBanner24RegularFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_TARGET_OTA_INSTALL = 2;
constexpr uint32_t SILENT_REBOOT_TARGET_OTA_CHECK = 3;
constexpr uint32_t SILENT_REBOOT_TARGET_FILE_TRANSFER = 4;
constexpr uint32_t SILENT_REBOOT_TARGET_FOULAD_EBOOKS = 5;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// Set once in setup() from the RTC silent-reboot magic; see SilentRestart.h.
static bool gBootWasSilentRestart = false;
bool bootWasSilentRestart() { return gBootWasSilentRestart; }

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToOtaInstall() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_OTA_INSTALL;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=ota_install)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToOtaCheck() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_OTA_CHECK;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=ota_check)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToFileTransfer() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_FILE_TRANSFER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=file_transfer)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToFouladEbooks() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_FOULAD_EBOOKS;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=foulad_ebooks)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() < calibratedPressDuration);
    abort = gpio.getPowerButtonHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    powerManager.startDeepSleep(gpio);
  }
}
void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    // This backstop firing means some activity's onExit() didn't disconnect WiFi
    // itself -- worth a permanent record since that's exactly the kind of leak a
    // "battery drains fast" report can't otherwise be traced to off-device.
    char wifiBuf[96];
    snprintf(wifiBuf, sizeof(wifiBuf), "%lu WiFi still on at sleep entry (mode=%d) -- backstop disconnect",
             millis(), static_cast<int>(WiFi.getMode()));
    BatteryDiagLog::append(wifiBuf);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  {
    char buf[96];
    snprintf(buf, sizeof(buf), "%lu entering deep sleep battery=%u%% fromTimeout=%d", millis(),
              powerManager.getBatteryPercentage(), fromTimeout);
    BatteryDiagLog::append(buf);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(BITTER_14_FONT_ID, bitter14FontFamily);
  renderer.insertFont(LEXENDDECA_14_FONT_ID, lexenddeca14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(BITTER_12_FONT_ID, bitter12FontFamily);
  renderer.insertFont(BITTER_16_FONT_ID, bitter16FontFamily);
  renderer.insertFont(BITTER_18_FONT_ID, bitter18FontFamily);
  renderer.insertFont(TASBIH_32_FONT_ID, tasbih32FontFamily);
  renderer.insertFont(LEXENDDECA_12_FONT_ID, lexenddeca12FontFamily);
  renderer.insertFont(LEXENDDECA_16_FONT_ID, lexenddeca16FontFamily);
  renderer.insertFont(LEXENDDECA_18_FONT_ID, lexenddeca18FontFamily);

#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  renderer.insertFont(NOTOSANSARABIC_8_FONT_ID, notosansarabic8FontFamily);
  renderer.insertFont(NOTOSANSARABIC_10_FONT_ID, notosansarabic10FontFamily);
  renderer.insertFont(NOTOSANSARABIC_12_FONT_ID, notosansarabic12FontFamily);
  renderer.insertFont(NOTONASKHARABIC_12_FONT_ID, notonaskharabic12FontFamily);
  renderer.insertFont(NOTONASKHARABIC_14_FONT_ID, notonaskharabic14FontFamily);
  renderer.insertFont(NOTONASKHARABIC_16_FONT_ID, notonaskharabic16FontFamily);
  renderer.insertFont(NOTONASKHARABIC_18_FONT_ID, notonaskharabic18FontFamily);
  renderer.insertFont(UTHMANICHAFS_12_FONT_ID, uthmanichafs12FontFamily);
  renderer.insertFont(UTHMANICHAFS_14_FONT_ID, uthmanichafs14FontFamily);
  renderer.insertFont(UTHMANICHAFS_16_FONT_ID, uthmanichafs16FontFamily);
  renderer.insertFont(UTHMANICHAFS_18_FONT_ID, uthmanichafs18FontFamily);
  renderer.insertFont(TAJAWAL_8_FONT_ID, tajawal8FontFamily);
  renderer.insertFont(TAJAWAL_10_FONT_ID, tajawal10FontFamily);
  renderer.insertFont(TAJAWAL_12_FONT_ID, tajawal12FontFamily);
  renderer.insertFont(TAJAWAL_14_FONT_ID, tajawal14FontFamily);
  renderer.insertFont(TAJAWAL_16_FONT_ID, tajawal16FontFamily);
  renderer.insertFont(TAJAWAL_18_FONT_ID, tajawal18FontFamily);
  renderer.insertFont(QURANCOMMON_18_FONT_ID, quranCommon18FontFamily);
  renderer.insertFont(SURAHBANNER_24_FONT_ID, surahBanner24FontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);
  arabicFontSystem.begin(renderer);

  // Self-heal the extracted Quran (and its default-font sidecar) at boot for
  // devices where the toggle is already on -- covers SD swaps, torn writes,
  // and firmware upgrades that changed the embedded copy or sidecar format.
  if (SETTINGS.quranEnabled) {
    QuranBook::ensureExtracted();
  }

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif

  HalSystem::begin();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_FOULAD_EBOOKS) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  gBootWasSilentRestart = isSilentReboot;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  GAME_SCORES.loadFromFile();
  TASBIH.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);
  {
    // Record what the loaded auto-sleep timeout actually resolves to -- separates "the
    // saved setting is wrong" from "the setting is right but something keeps resetting
    // the inactivity timer" for a user reporting auto-sleep never triggers.
    char buf[96];
    snprintf(buf, sizeof(buf), "%lu boot sleepTimeoutMinutes=%u sleepTimeoutMs=%lu", millis(),
             (unsigned)SETTINGS.sleepTimeoutMinutes, SETTINGS.getSleepTimeoutMs());
    SleepDiagLog::append(buf);
  }

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon.
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent &&
             (snapshotTarget == SILENT_REBOOT_TARGET_OTA_INSTALL || snapshotTarget == SILENT_REBOOT_TARGET_OTA_CHECK)) {
    // Land straight back in the OTA screen on this freshly-booted (unfragmented)
    // heap: OTA_CHECK is the flow's entry point (Home/Settings reboot before even
    // checking), OTA_INSTALL means the user already confirmed the update before
    // the reboot, so auto-install is armed -- see silentRestartToOtaCheck() /
    // silentRestartToOtaInstall().
    activityManager.replaceActivity(
        std::make_unique<OtaUpdateActivity>(renderer, mappedInputManager,
                                            /*autoInstall=*/snapshotTarget == SILENT_REBOOT_TARGET_OTA_INSTALL));
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_FILE_TRANSFER) {
    // Same fresh-heap treatment as OTA: the web server needs large contiguous
    // allocations for the WiFi driver and TCP buffers, and a fragmented heap
    // makes page loads crawl. Settings reboots here before starting it.
    activityManager.goToFileTransfer();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_FOULAD_EBOOKS) {
    // Same fresh-heap treatment: OPDS browsing stacks WiFi + feed parsing +
    // cover download/decode allocations; started from a fragmented session
    // heap it aborted with OOM twice on-device (crash reports 7 and 8).
    activityManager.goToFouladEbooks();
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

#ifdef SIMULATOR
  // The simulator's S key requests a simulated deep sleep (see the simulator
  // library's HalGPIO); the firmware has to consume it -- exercises the real
  // SleepActivity render path on the desktop.
  if (gpio.consumeSimulatorSleepRequest()) {
    enterDeepSleep();
  }
#endif

  renderer.setFadingFix(SETTINGS.fadingFix);
  // Applied per tick like fadingFix, so flipping the toggle inverts the very
  // next refresh -- including the settings screen itself.
  renderer.setDarkMode(SETTINGS.darkModeEnabled != 0);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  const bool anyPressed = gpio.wasAnyPressed();
  const bool anyReleased = gpio.wasAnyReleased();
  const bool tiltActivity = halTiltSensor.hadActivity();
  const bool blockedByActivity = activityManager.preventAutoSleep();
  if (anyPressed || anyReleased || tiltActivity || blockedByActivity) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity

    // A user report of "auto-sleep never triggers" is otherwise unreproducible off-device
    // (no serial cable in hand) -- log which condition(s) actually kept resetting the
    // timer, throttled so a stuck/noisy button doesn't spam the bounded log every loop.
    static unsigned long lastSleepDiagLogMs = 0;
    const unsigned long nowMs = millis();
    if (nowMs - lastSleepDiagLogMs >= 3000) {
      lastSleepDiagLogMs = nowMs;
      char buf[160];
      snprintf(buf, sizeof(buf), "%lu pressed=%d released=%d tilt=%d activityBlock=%d activity=%s", nowMs, anyPressed,
                anyReleased, tiltActivity, blockedByActivity,
                blockedByActivity ? activityManager.currentActivityDebugName() : "-");
      SleepDiagLog::append(buf);
    }
  }

  // Periodic battery/WiFi/CPU-frequency breadcrumb, independent of the sleep-diag
  // block above (which only fires on activity) -- a "battery drains fast" report has
  // no serial cable to show us whether the radio was left on or the CPU stuck at full
  // frequency; sampled every 5 minutes so a long session doesn't blow through
  // BatteryDiagLog::MAX_LINES in minutes.
  {
    static unsigned long lastBatteryDiagLogMs = 0;
    const unsigned long nowMs = millis();
    if (nowMs - lastBatteryDiagLogMs >= 300000) {
      lastBatteryDiagLogMs = nowMs;
      char buf[160];
      snprintf(buf, sizeof(buf), "%lu battery=%u%% wifiMode=%d wifiStatus=%d powerSaving=%d activity=%s", nowMs,
                powerManager.getBatteryPercentage(), static_cast<int>(WiFi.getMode()),
                static_cast<int>(WiFi.status()), millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS,
                activityManager.currentActivityDebugName());
      BatteryDiagLog::append(buf);
    }
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
