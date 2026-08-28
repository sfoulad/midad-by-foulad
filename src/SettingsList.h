#pragma once

// BoardConfig is device-only SDK surface (no simulator equivalent); neither
// [env:simulator] (X4) nor [env:simulator_x3] models a touch-capable board,
// so hasTouch() is correctly always-false there.
#ifndef SIMULATOR
#include <BoardConfig.h>
#endif
#include <HalClock.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "ReaderFontSizes.h"
#include "activities/settings/SettingsActivity.h"

inline bool boardHasTouch() {
#ifdef SIMULATOR
  return false;
#else
  return BoardConfig::hasTouch();
#endif
}

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Selectable built-in labels and their stored FONT_FAMILY value, in display order.
  // Bitter's glyph data was removed from flash (~778KB) and is now a Manage Fonts
  // download, leaving Lexend Deca as the only built-in Latin reading family. BITTER=0
  // stays a reserved enum value that aliases Lexend in CrossPointSettings, so a device
  // with it saved still renders -- it is simply no longer offered here.
  //
  // Display index is therefore NOT the stored value. That was already true for SD
  // fonts, which persist by name rather than index (see valueSetter), so shortening
  // this list cannot move anyone's saved choice.
  static constexpr std::pair<StrId, uint8_t> kSelectableFonts[] = {
      {StrId::STR_LEXEND_DECA, CrossPointSettings::LEXENDDECA},
  };
  static constexpr int kBuiltinCount = static_cast<int>(sizeof(kSelectableFonts) / sizeof(kSelectableFonts[0]));

  std::vector<StrId> enumValues;
  enumValues.reserve(kBuiltinCount);
  for (const auto& [strId, familyValue] : kSelectableFonts) enumValues.push_back(strId);
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    std::transform(enumValues.begin(), enumValues.end(), std::back_inserter(allStringValues),
                   [](StrId strId) { return I18N.get(strId); });
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(kBuiltinCount + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    // Every built-in value resolves to the one remaining family, including the
    // reserved BITTER=0, so there is only ever one built-in row to land on.
    return 0;
  };

  s.valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < kBuiltinCount) {
      SETTINGS.fontFamily = kSelectableFonts[v].second;
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - kBuiltinCount;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Build the LATIN reading-font size setting dynamically. Selectable point sizes
// depend on the active family: the fixed built-in set (BUILTIN_READER_POINT_SIZES)
// when no SD family is selected, or the SD family's actually-installed sizes
// otherwise (see ReaderFontSizes.h). Mirrors buildFontFamilySetting() above's
// dynamic-enum-with-lambda-getter/setter shape. Arabic Font Size is untouched --
// it stays the static SMALL/MEDIUM/LARGE/EXTRA_LARGE enum entry below.
inline SettingInfo buildFontSizeSetting(const SdCardFontRegistry* registry) {
  const std::vector<uint8_t> sizes = readerFontPointSizes(registry, SETTINGS.sdFontFamilyName);
  std::vector<std::string> labels;
  labels.reserve(sizes.size());
  for (const uint8_t pt : sizes) labels.push_back(std::to_string(pt) + " pt");  // "pt" deliberately untranslated

  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.enumStringValues = std::move(labels);
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;

  s.valueGetter = [sizes]() -> uint8_t {
    const uint8_t pt = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
    for (int i = 0; i < static_cast<int>(sizes.size()); i++) {
      if (sizes[i] == pt) return static_cast<uint8_t>(i);
    }
    return 0;
  };
  s.valueSetter = [sizes](uint8_t v) {
    if (v < sizes.size()) SETTINGS.fontPointSize = sizes[v];
  };
  return s;
}

// Build the Arabic font family setting dynamically, mirroring buildFontFamilySetting()
// above -- same two-axis structure (5 built-in families + optional SD override) as the
// main reading font. Built as an ENUM (not a plain SettingInfo::Action) specifically so
// the settings list shows the currently selected font's name inline, the same way Font
// Family does, instead of requiring the user to open the picker just to see what's
// selected. SettingsActivity::toggleCurrentSetting() special-cases STR_ARABIC_FONT (like
// it already does for STR_FONT_FAMILY) to launch the picker on Confirm instead of cycling
// through a plain popup -- the picker's live glyph preview is worth keeping for Arabic
// specifically, where the built-in styles look meaningfully different from each other.
inline SettingInfo buildArabicFontFamilySetting(const SdCardFontRegistry* registry) {
  // Selectable built-in font labels (StrId) and their stored ARABIC_FONT_FAMILY value,
  // in display order -- mirrors ArabicFontSelectionActivity.cpp's own
  // kSelectableArabicFonts. Amiri (value 1) was removed to save flash space and is no
  // longer offered; its enum value stays reserved (see CrossPointSettings::
  // ARABIC_FONT_FAMILY) so a stale setting/sidecar still holding it resolves through
  // ArabicFontSystem's Naskh alias instead of colliding with another entry here.
  // Display index is therefore NOT the same as the stored value -- valueGetter/
  // valueSetter below translate between the two explicitly.
  static constexpr std::pair<StrId, uint8_t> kSelectableFonts[] = {
      {StrId::STR_NOTO_NASKH_ARABIC, CrossPointSettings::NOTONASKHARABIC},
      {StrId::STR_UTHMANI_HAFS, CrossPointSettings::UTHMANICHAFS},
      {StrId::STR_TAJAWAL, CrossPointSettings::TAJAWAL},
  };
  static constexpr int kSelectableCount = static_cast<int>(sizeof(kSelectableFonts) / sizeof(kSelectableFonts[0]));

  std::vector<StrId> enumValues;
  enumValues.reserve(kSelectableCount);
  for (const auto& [strId, familyValue] : kSelectableFonts) enumValues.push_back(strId);
  std::vector<std::string> enumStringValues;

  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  const int sdFontCount = static_cast<int>(enumStringValues.size());

  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    std::transform(enumValues.begin(), enumValues.end(), std::back_inserter(allStringValues),
                   [](StrId strId) { return I18N.get(strId); });
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_ARABIC_FONT;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "arabicFontFamily";
  s.category = StrId::STR_CAT_READER;

  std::vector<std::string> sdFamilyNames;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    if (SETTINGS.sdArabicFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdArabicFontFamilyName) {
          return static_cast<uint8_t>(kSelectableCount + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    for (int i = 0; i < kSelectableCount; i++) {
      if (kSelectableFonts[i].second == SETTINGS.arabicFontFamily) return static_cast<uint8_t>(i);
    }
    return 0;  // stale/legacy value (e.g. removed Amiri=1) or out of range
  };

  s.valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < kSelectableCount) {
      SETTINGS.arabicFontFamily = kSelectableFonts[v].second;
      SETTINGS.sdArabicFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - kSelectableCount;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(SETTINGS.sdArabicFontFamilyName, sdFamilyNames[sdIdx].c_str(),
                sizeof(SETTINGS.sdArabicFontFamilyName) - 1);
        SETTINGS.sdArabicFontFamilyName[sizeof(SETTINGS.sdArabicFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

inline std::vector<StrId> buildLongPressMenuValues() {
  static constexpr StrId VALUES[] = {StrId::STR_KOSYNC, StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION,
                                     StrId::STR_DICTIONARY, StrId::STR_READER_MENU};
  const size_t count = BoardConfig::hasHomeKey() ? std::size(VALUES) : std::size(VALUES) - 1;
  return {VALUES, VALUES + count};
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The static list is constructed exactly once (master's optimization, #1086 +
// #1636) so the per-entry SettingInfo cost is paid once; every call then copies
// it. When an SdCardFontRegistry is supplied AND has SD card fonts installed,
// the font-family entry is replaced in that copy with a registry-aware version.
// The font-size entry is always rebuilt, since its options are point sizes read
// from the active family rather than a fixed enum.
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr) {
  static const std::vector<SettingInfo> baseList = [] {
    // Built via sequential push_back (not a single brace-init aggregate) --
    // ~50 SettingInfo entries in one initializer_list forced the compiler to
    // materialize ALL of them at once (each with its own inline std::vector<StrId>
    // for Enum options), and that combined temporary array lives in this
    // lambda's own stack frame. On real hardware that blew loopTask's ~8KB
    // stack (Guru Meditation: Stack protection fault, inside a heap_caps_malloc
    // call triggered by constructing this list) the moment two more entries
    // (Gym's toggle + weight-unit enum) were added -- v1.7.64 hung on every
    // boot. push_back keeps only one entry's temporary alive at a time.
    std::vector<SettingInfo> v;
    v.reserve(64);
    // --- Display ---
    // Enum settings are persisted as numeric values. Assign these labels by enum
    // value (not positional order) so a reordered menu or enum cannot silently
    // swap their behavior -- a prior "fix" here swapped None and Cover+Custom in
    // the WRONG direction under the old positional-initializer-list scheme, so
    // picking "Cover + Custom" stored index 5 = BLANK -- a deliberately blank
    // sleep screen displayed as "Cover + Custom" (live user report: blank screen
    // when sleeping from inside a book with Cover+Custom selected --
    // SleepActivity.cpp's COVER_CUSTOM case correctly shows the book cover in
    // that situation, but the setting never actually stored COVER_CUSTOM=4).
    std::vector<StrId> sleepScreenValues(CrossPointSettings::SLEEP_SCREEN_MODE_COUNT);
    sleepScreenValues[CrossPointSettings::DARK] = StrId::STR_DARK;
    sleepScreenValues[CrossPointSettings::LIGHT] = StrId::STR_LIGHT;
    sleepScreenValues[CrossPointSettings::CUSTOM] = StrId::STR_CUSTOM;
    sleepScreenValues[CrossPointSettings::COVER] = StrId::STR_COVER;
    sleepScreenValues[CrossPointSettings::COVER_CUSTOM] = StrId::STR_COVER_CUSTOM;
    sleepScreenValues[CrossPointSettings::BLANK] = StrId::STR_NONE_OPT;
    sleepScreenValues[CrossPointSettings::QUICK_RESUME] = StrId::STR_QUICK_RESUME;
    sleepScreenValues[CrossPointSettings::DASHBOARD] = StrId::STR_SLEEP_DASHBOARD;
    sleepScreenValues[CrossPointSettings::TRANSPARENT_CUSTOM] = StrId::STR_TRANSPARENT;
    v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                                  std::move(sleepScreenValues), "sleepScreen", StrId::STR_CAT_DISPLAY));
    // Fit and filter act on whatever image the sleep screen draws, and a book cover is
    // only one of them: CUSTOM draws the picture from /sleep through the very same
    // renderBitmapSleepScreen (SleepActivity.cpp), and TRANSPARENT_CUSTOM places its
    // overlay through the same calculateBitmapPlacement. Hiding the two rows outside
    // COVER left both silently in force on those modes, at whatever a previous Cover
    // session happened to leave them. Dark, Light, Blank, Quick Resume and Dashboard
    // draw no image at all and ignore both.
    const auto sleepScreenDrawsImage = [] {
      const auto mode = SETTINGS.sleepScreen;
      return mode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
             mode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM ||
             mode == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM ||
             mode == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM;
    };
    // The filter is the one exception: the overlay renders with preserveBackground,
    // which skips both the inversion and the greyscale gate, so the setting really
    // would do nothing on TRANSPARENT_CUSTOM.
    const auto sleepScreenFiltersImage = [] {
      const auto mode = SETTINGS.sleepScreen;
      return mode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
             mode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM ||
             mode == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    };
    v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                                  {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY)
                    .shownWhen(sleepScreenDrawsImage));
    v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                                  {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                                  "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY)
                    .shownWhen(sleepScreenFiltersImage));
    // "Resume the last screen when sleeping on a timeout" only adds anything while
    // some OTHER sleep screen is selected: with Sleep Screen already set to Quick
    // Resume, every sleep resumes the last screen anyway, and
    // SettingsActivity::syncQuickResumeTimeoutForSleepScreen() force-holds this at
    // On for as long as that mode is picked. Showing it there offered a toggle that
    // silently sprang back.
    v.push_back(
        SettingInfo::Enum(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                          {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "quickResumeSleepScreen", StrId::STR_CAT_DISPLAY)
            .shownWhen([] { return SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME; }));
    v.push_back(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                                  {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                                  StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(
        StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
        {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
        "refreshFrequency", StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                                    StrId::STR_CAT_DISPLAY));
    // Whole-UI inversion (reader + Arabic + games + theme) applied at the
    // renderer's panel-push points -- see GfxRenderer::setDarkMode.
    v.push_back(SettingInfo::Toggle(StrId::STR_DARK_MODE, &CrossPointSettings::darkModeEnabled, "darkModeEnabled",
                                    StrId::STR_CAT_DISPLAY));
    // NOTE: no STR_UI_THEME entry -- Midad is single-theme firmware
    // (CrossPointSettings::uiTheme is fixed at FOULAD, see its declaration).
    // CrossPoint's multi-theme picker (Classic/Lyra/Lyra Extended/RoundedRaff)
    // does not apply; RoundedRaffTheme was removed entirely (Group I).

    // --- Reader ---
    // Built-in font-family entry. Replaced per-call with a registry-aware
    // version when SD fonts are installed.
    v.push_back(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily, {StrId::STR_LEXEND_DECA},
                                  "fontFamily", StrId::STR_CAT_READER));
    // Placeholder -- replaced unconditionally below with buildFontSizeSetting(),
    // since the selectable point sizes depend on the active family (built-in
    // fixed set, or an SD family's actually-installed sizes), not a fixed enum.
    v.push_back(SettingInfo::Enum(StrId::STR_FONT_SIZE, nullptr, {}, "fontSize", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Enum(StrId::STR_ARABIC_FONT_SIZE, &CrossPointSettings::arabicFontSize,
                                  {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE},
                                  "arabicFontSize", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                                  {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE},
                                  "lineSpacing", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin,
                                   {CrossPointSettings::SCREEN_MARGIN_MIN, CrossPointSettings::SCREEN_MARGIN_MAX,
                                    CrossPointSettings::SCREEN_MARGIN_STEP},
                                   "screenMargin", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Enum(
        StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
        {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE},
        "paragraphAlignment", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                                    StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled,
                                    "focusReadingEnabled", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled,
                                    "hyphenationEnabled", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_TRACK_READING_STATS, &CrossPointSettings::trackReadingStats,
                                    "trackReadingStats", StrId::STR_CAT_READER));
    // A daily goal is a target for a number nothing records while Track Reading
    // Stats is off -- EpubReaderActivity gates all three of its accumulation
    // points on that flag, so the goal has nothing to measure against.
    v.push_back(SettingInfo::Enum(StrId::STR_DAILY_READING_GOAL, &CrossPointSettings::dailyReadingGoal,
                                  {StrId::STR_GOAL_15M, StrId::STR_GOAL_30M, StrId::STR_GOAL_45M, StrId::STR_GOAL_1H,
                                   StrId::STR_GOAL_90M, StrId::STR_GOAL_2H},
                                  "dailyReadingGoal", StrId::STR_CAT_READER)
                    .shownWhen([] { return SETTINGS.trackReadingStats != 0; }));
    v.push_back(SettingInfo::Enum(
        StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
        {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW},
        "orientation", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                                    "extraParagraphSpacing", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                                    StrId::STR_CAT_READER));
    v.push_back(
        SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER));
    // Night mode = inverted output polarity on the reading surfaces only
    // (EPUB/TXT/XTC; ActivityManager resolves the polarity per render).
    // Reader category, since it does not affect the rest of the UI.
    v.push_back(SettingInfo::Toggle(StrId::STR_NIGHT_MODE, &CrossPointSettings::screenInverted, "screenInverted",
                                    StrId::STR_CAT_READER));
    // --- Controls ---
    v.push_back(SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                                  {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED}, "sideButtonLayout",
                                  StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Enum(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                                  {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "touchReaderControls",
                                  StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION,
                                    &CrossPointSettings::frontButtonFollowOrientation, "frontButtonFollowOrientation",
                                    StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                                  {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                                   StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                                  "longPressButtonBehavior", StrId::STR_CAT_CONTROLS));
    v.push_back(
        SettingInfo::Enum(StrId::STR_LONG_PRESS_MENU, &CrossPointSettings::longPressMenuFunction,
                          {StrId::STR_KOSYNC, StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION, StrId::STR_DICTIONARY},
                          "longPressMenuFunction", StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Enum(
        StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
        {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_FOOTNOTES},
        "shortPwrBtn", StrId::STR_CAT_CONTROLS));
    // Only reachable when the short power press is bound to footnotes; this rule
    // used to be an explicit valuePtr test inside SettingsActivity's category loop.
    v.push_back(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                                    "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS)
                    .shownWhen([] { return SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES; }));
    v.push_back(SettingInfo::Toggle(StrId::STR_BACK_SHORT_TO_FILE_BROWSER, &CrossPointSettings::backShortToFileBrowser,
                                    "backShortToFileBrowser", StrId::STR_CAT_CONTROLS));

    // --- Apps ---
    // Quran/Games/Tasbih/Stop Watch/Pomodoro/Gym/Midad BLE/Debug Logging live
    // on MidadAppSettings, not CrossPointSettings, and their SettingInfo rows
    // are defined in src/MidadSettingsList.h/.cpp instead of here -- see
    // SettingsActivity::rebuildSettingsLists()'s appendMidadAppSettings() call
    // and docs/upstream-sync-architecture.md's Phase B for why keeping them
    // out of this upstream-owned file matters.
    //
    // KOReader Sync itself is a device-only ACTION appended in
    // SettingsActivity::rebuildSettingsLists() (web-only, uses
    // KOReaderCredentialStore) -- see the comment further below.

    // --- System ---
    v.push_back(SettingInfo::Value(
        StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
        {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
        "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));
    v.push_back(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles,
                                    "showHiddenFiles", StrId::STR_CAT_SYSTEM));
    v.push_back(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS,
                                    &CrossPointSettings::removeReadBooksFromRecents, "removeReadBooksFromRecents",
                                    StrId::STR_CAT_SYSTEM));
    v.push_back(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                                    "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM));
    v.push_back(SettingInfo::Toggle(StrId::STR_OTA_PRERELEASE, &CrossPointSettings::otaPrereleaseEnabled,
                                    "otaPrereleaseEnabled", StrId::STR_CAT_SYSTEM));

    // OPDS download folder: persisted + web-exposed, but category-less so it
    // is hidden from the on-device Settings screen (edited via OPDS UI).
    v.push_back(SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, &SETTINGS.opdsDownloadFolder[0],
                                    sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"));
    // OPDS download filename format: persisted + web-exposed, category-less so it
    // is hidden from the on-device Settings screen (cycled from the OPDS UI).
    v.push_back(SettingInfo::Enum(StrId::STR_OPDS_FILENAME_FORMAT, &CrossPointSettings::opdsFilenameFormat,
                                  {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR, StrId::STR_FMT_TITLE},
                                  "opdsFilenameFormat"));

    // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
    v.push_back(SettingInfo::DynamicString(
        StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
        [](const std::string& val) {
          KOREADER_STORE.setCredentials(val, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
        },
        "koUsername", StrId::STR_KOREADER_SYNC));
    v.push_back(SettingInfo::DynamicString(
        StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
        [](const std::string& val) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), val);
          KOREADER_STORE.saveToFile();
        },
        "koPassword", StrId::STR_KOREADER_SYNC));
    v.push_back(SettingInfo::DynamicString(
        StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
        [](const std::string& val) {
          KOREADER_STORE.setServerUrl(val);
          KOREADER_STORE.saveToFile();
        },
        "koServerUrl", StrId::STR_KOREADER_SYNC));
    v.push_back(SettingInfo::DynamicEnum(
        StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
        [](uint8_t val) {
          KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(val));
          KOREADER_STORE.saveToFile();
        },
        "koMatchMethod", StrId::STR_KOREADER_SYNC));
    v.push_back(SettingInfo::DynamicEnum(
        StrId::STR_SEND_METADATA, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata()); },
        [](uint8_t val) {
          KOREADER_STORE.setSendMetadata(val != 0);
          KOREADER_STORE.saveToFile();
        },
        "koSendMetadata", StrId::STR_KOREADER_SYNC));
    v.push_back(SettingInfo::DynamicEnum(
        StrId::STR_SYNC_BEHAVIOR, {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
        [](uint8_t val) {
          KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(val));
          KOREADER_STORE.saveToFile();
        },
        "koSyncBehavior", StrId::STR_KOREADER_SYNC));
    // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
    v.push_back(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                                    "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE,
                                    &CrossPointSettings::statusBarBookProgressPercentage,
                                    "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                                  {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                                  {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                                    StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                                  {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    // Clock entries (web settings only; device UI uses ClockOffsetActivity for the offset).
    // Range 0..104 = quarter-hour steps from UTC-12:00 to UTC+14:00, biased by 48.
    // Assign by enum value (not positional order) for the same reason as sleepScreenValues above.
    std::vector<StrId> statusBarClockValues(CrossPointSettings::STATUS_BAR_CLOCK_MODE_COUNT);
    statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_HIDE] = StrId::STR_HIDE;
    statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_RIGHT] = StrId::STR_DIR_RIGHT;
    statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_LEFT] = StrId::STR_DIR_LEFT;
    v.push_back(SettingInfo::Enum(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock,
                                  std::move(statusBarClockValues), "statusBarClock", StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                                   "clockUtcOffsetQ", StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                                  {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
    // on next WiFi connect, which is useful when crossing time zones.
    v.push_back(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced,
                                    "clockHasBeenSynced", StrId::STR_CUSTOMISE_STATUS_BAR));
    // Only show tilt page turn setting when the QMI8658 IMU is present (X3)
    if (halTiltSensor.isAvailable()) {
      // Insert after the short power button setting (end of Controls section)
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->nameId == StrId::STR_SHORT_PWR_BTN) {
          v.insert(it + 1, SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                             {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED},
                                             "tiltPageTurn", StrId::STR_CAT_CONTROLS));
          break;
        }
      }
    }
    return v;
  }();

  std::vector<SettingInfo> v = baseList;
  if (!boardHasTouch()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) { return s.nameId == StrId::STR_TOUCH_READER_CONTROLS; }),
            v.end());
  }
  if (boardHasTouch()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) {
                             return s.nameId == StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION ||
                                    s.nameId == StrId::STR_SUNLIGHT_FADING_FIX ||
                                    s.nameId == StrId::STR_BACK_SHORT_TO_FILE_BROWSER;
                           }),
            v.end());
  }
  if (registry && registry->getFamilyCount() > 0) {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_FAMILY; });
    if (it != v.end()) {
      *it = buildFontFamilySetting(registry);
    }
  }
  // Unconditional (unlike font-family's SD-fonts-installed guard above): the
  // built-in point-size set is dynamic too (BUILTIN_READER_POINT_SIZES), not a
  // fixed enum, so this always needs the registry-aware build.
  {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_SIZE; });
    if (it != v.end()) {
      *it = buildFontSizeSetting(registry);
    }
  }
  return v;
}
