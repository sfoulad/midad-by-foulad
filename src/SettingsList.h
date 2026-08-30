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
#include "SettingsEnumLabels.h"
#include "activities/settings/SettingsActivity.h"
#include "util/DictionaryRegistry.h"

inline bool boardHasTouch() {
#ifdef SIMULATOR
  return false;
#else
  return BoardConfig::hasTouch();
#endif
}

// Capacitive Home key (X4 Pro). Same simulator caveat as boardHasTouch().
inline bool boardHasHomeKey() {
#ifdef SIMULATOR
  return false;
#else
  return BoardConfig::hasHomeKey();
#endif
}

// Copy a compile-time label table into the std::vector<StrId> SettingInfo::Enum
// takes. `count` trims trailing board-conditional entries (see
// settings_labels::kLongPressMenuNoHomeKeyCount / kShortPwrBtnNoTouchCount);
// the trimmed list stays a prefix of the full one, so no stored value shifts.
inline std::vector<StrId> enumLabels(const StrId* labels, const std::size_t count) { return {labels, labels + count}; }

template <std::size_t N>
inline std::vector<StrId> enumLabels(const StrId (&labels)[N]) {
  return {labels, labels + N};
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
// above -- same two-axis structure (built-in families + optional SD override) as the
// main reading font. Built as an ENUM (not a plain SettingInfo::Action) specifically so
// the settings list shows the currently selected font's name inline, the same way Font
// Family does, instead of requiring the user to open the picker just to see what's
// selected. Confirm opens SettingsActivity's standard option popup (three or more
// values), listing the same families this table defines.
//
// Contributed to the Settings screen by Midad's SettingsExtension provider (see
// src/MidadSettingsExtension.cpp), not from getSettingsList() below: the Arabic
// reading font is a Midad feature, and the extension point is how a downstream
// integration adds rows without CrossPoint's own files knowing about it.
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

// Build the dictionary selection setting dynamically from the folders discovered
// under /dictionaries. "None" plus one option per dictionary; the selected folder
// name persists in SETTINGS.dictionaryName (saved/loaded manually in
// CrossPointSettings::toJson/fromJson — the generic loop skips dynamic entries).
inline SettingInfo buildDictionarySetting(const std::vector<DictionaryEntry>& dictionaries) {
  std::vector<std::string> folderNames;
  folderNames.reserve(dictionaries.size());
  std::transform(dictionaries.begin(), dictionaries.end(), std::back_inserter(folderNames),
                 [](const DictionaryEntry& d) { return d.name; });

  SettingInfo s;
  s.nameId = StrId::STR_DICTIONARY;
  s.type = SettingType::ENUM;
  s.enumStringValues.reserve(folderNames.size() + 1);
  s.enumStringValues.push_back(I18N.get(StrId::STR_NONE_OPT));
  s.enumStringValues.insert(s.enumStringValues.end(), folderNames.begin(), folderNames.end());
  s.category = StrId::STR_CAT_READER;

  s.valueGetter = [folderNames]() -> uint8_t {
    for (size_t i = 0; i < folderNames.size(); i++) {
      // Compare within the settings field capacity: an over-long folder name is
      // stored truncated, and must still match its list entry.
      if (strncmp(folderNames[i].c_str(), SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName) - 1) == 0) {
        return static_cast<uint8_t>(i + 1);
      }
    }
    return 0;  // "None", also when the stored folder no longer exists
  };

  s.valueSetter = [folderNames](uint8_t v) {
    if (v == 0 || v > folderNames.size()) {
      SETTINGS.dictionaryName[0] = '\0';
      return;
    }
    strncpy(SETTINGS.dictionaryName, folderNames[v - 1].c_str(), sizeof(SETTINGS.dictionaryName) - 1);
    SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
  };

  return s;
}

// "Reader Menu" (the last entry) is only offered on home-key boards, where the
// key's long press keeps the menu reachable; elsewhere the list stops one short.
inline std::vector<StrId> buildLongPressMenuValues() {
  return enumLabels(settings_labels::kLongPressMenu, boardHasHomeKey() ? std::size(settings_labels::kLongPressMenu)
                                                                       : settings_labels::kLongPressMenuNoHomeKeyCount);
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
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr,
                                                const std::vector<DictionaryEntry>* dictionaries = nullptr) {
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
    v.reserve(72);
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
    v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                                  enumLabels(settings_labels::kSleepCoverMode), "sleepScreenCoverMode",
                                  StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                                  enumLabels(settings_labels::kSleepCoverFilter), "sleepScreenCoverFilter",
                                  StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                                  enumLabels(settings_labels::kQuickResumeTimeout), "quickResumeSleepScreen",
                                  StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                                  enumLabels(settings_labels::kHideBattery), "hideBatteryPercentage",
                                  StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
                                  enumLabels(settings_labels::kRefreshFrequency), "refreshFrequency",
                                  StrId::STR_CAT_DISPLAY));
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
    // Placeholder -- replaced unconditionally below with buildFontFamilySetting().
    // It only fixes the row's position in the Reader category; the real option
    // list depends on the SD font registry.
    v.push_back(SettingInfo::Enum(StrId::STR_FONT_FAMILY, nullptr, {}, "fontFamily", StrId::STR_CAT_READER));
    // Placeholder -- replaced unconditionally below with buildFontSizeSetting(),
    // since the selectable point sizes depend on the active family (built-in
    // fixed set, or an SD family's actually-installed sizes), not a fixed enum.
    v.push_back(SettingInfo::Enum(StrId::STR_FONT_SIZE, nullptr, {}, "fontSize", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Enum(StrId::STR_ARABIC_FONT_SIZE, &CrossPointSettings::arabicFontSize,
                                  enumLabels(settings_labels::kArabicFontSize), "arabicFontSize",
                                  StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                                  enumLabels(settings_labels::kLineSpacing), "lineSpacing", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin,
                                   {CrossPointSettings::SCREEN_MARGIN_MIN, CrossPointSettings::SCREEN_MARGIN_MAX,
                                    CrossPointSettings::SCREEN_MARGIN_STEP},
                                   "screenMargin", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                                  enumLabels(settings_labels::kParagraphAlignment), "paragraphAlignment",
                                  StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                                    StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled,
                                    "focusReadingEnabled", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled,
                                    "hyphenationEnabled", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_TRACK_READING_STATS, &CrossPointSettings::trackReadingStats,
                                    "trackReadingStats", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Enum(StrId::STR_DAILY_READING_GOAL, &CrossPointSettings::dailyReadingGoal,
                                  enumLabels(settings_labels::kDailyReadingGoal), "dailyReadingGoal",
                                  StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                                  enumLabels(settings_labels::kOrientation), "orientation", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                                    "extraParagraphSpacing", StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                                    StrId::STR_CAT_READER));
    v.push_back(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                                  enumLabels(settings_labels::kImageRendering), "imageRendering",
                                  StrId::STR_CAT_READER));
    // How Select opens the reader menu: the full-screen list or the toolbar
    // overlay. main.cpp seeds Toolbar on touch boards before the settings load,
    // so this row is what lets a user pick the other style and keep it.
    v.push_back(SettingInfo::Enum(StrId::STR_READER_MENU_STYLE, &CrossPointSettings::readerMenuStyle,
                                  enumLabels(settings_labels::kReaderMenuStyle), "readerMenuStyle",
                                  StrId::STR_CAT_READER));
    // Night mode = inverted output polarity on the reading surfaces only
    // (EPUB/TXT/XTC; ActivityManager resolves the polarity per render).
    // Reader category, since it does not affect the rest of the UI.
    v.push_back(SettingInfo::Toggle(StrId::STR_NIGHT_MODE, &CrossPointSettings::screenInverted, "screenInverted",
                                    StrId::STR_CAT_READER));
    // --- Controls ---
    v.push_back(SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                                  enumLabels(settings_labels::kSideButtonLayout), "sideButtonLayout",
                                  StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Enum(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                                  enumLabels(settings_labels::kTouchReaderControls), "touchReaderControls",
                                  StrId::STR_CAT_CONTROLS));
    // Persisted under the legacy "tapForReaderMenu" key: old saves map
    // 0 = Off, 1 = Tap.
    v.push_back(SettingInfo::Enum(StrId::STR_SHOW_READER_MENU, &CrossPointSettings::showReaderMenu,
                                  enumLabels(settings_labels::kShowReaderMenu), "tapForReaderMenu",
                                  StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION,
                                    &CrossPointSettings::frontButtonFollowOrientation, "frontButtonFollowOrientation",
                                    StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                                  enumLabels(settings_labels::kLongPressBehavior), "longPressButtonBehavior",
                                  StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Enum(StrId::STR_LONG_PRESS_MENU, &CrossPointSettings::longPressMenuFunction,
                                  buildLongPressMenuValues(), "longPressMenuFunction", StrId::STR_CAT_CONTROLS));
#if FREEINK_CAP_TOUCH
    v.push_back(SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                                  enumLabels(settings_labels::kShortPwrBtn), "shortPwrBtn", StrId::STR_CAT_CONTROLS));
#else
    // No Confirm option: the power button is only pressed into Confirm service
    // on touch boards, which have no front Confirm key.
    v.push_back(SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                                  enumLabels(settings_labels::kShortPwrBtn, settings_labels::kShortPwrBtnNoTouchCount),
                                  "shortPwrBtn", StrId::STR_CAT_CONTROLS));
#endif
    v.push_back(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                                    "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS));
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
                                  enumLabels(settings_labels::kOpdsFilenameFormat), "opdsFilenameFormat"));

    // Frontlight state: persisted and web-exposed, but category-less so the
    // Settings screen shows nothing here -- FrontlightPanelActivity owns
    // brightness/warmth/on, and Midad's extension provider contributes the
    // Restore Light on Wake row (see src/MidadSettingsExtension.cpp). Kept
    // unconditional: on a lightless board these are four inert bytes, and a
    // runtime Frontlight.present() guard would run before
    // HalFrontlight::begin(), which reads its starting brightness from here.
    v.push_back(SettingInfo::Value(StrId::STR_BRIGHTNESS, &CrossPointSettings::frontlightBrightness, {0, 100, 5},
                                   "frontlightBrightness"));
    v.push_back(
        SettingInfo::Value(StrId::STR_WARMTH, &CrossPointSettings::frontlightWarmth, {0, 100, 5}, "frontlightWarmth"));
    v.push_back(SettingInfo::Toggle(StrId::STR_FRONTLIGHT, &CrossPointSettings::frontlightOn, "frontlightOn"));
    v.push_back(SettingInfo::Toggle(StrId::STR_RESTORE_LIGHT_ON_WAKE, &CrossPointSettings::frontlightRestoreOnWake,
                                    "frontlightRestoreOnWake"));

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
                                  enumLabels(settings_labels::kStatusBarProgressBar), "statusBarProgressBar",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                                  enumLabels(settings_labels::kProgressBarThickness), "statusBarProgressBarThickness",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                                  enumLabels(settings_labels::kStatusBarTitle), "statusBarTitle",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                                    StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                                  enumLabels(settings_labels::kXtcStatusBar), "xtcStatusBarMode",
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
                                  enumLabels(settings_labels::kClockFormat), "clockFormat",
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
                                             enumLabels(settings_labels::kTiltPageTurn), "tiltPageTurn",
                                             StrId::STR_CAT_CONTROLS));
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
  // The reader-menu gesture choice only makes sense where the menu stays
  // reachable without the tap and the bottom edge is free (the capacitive Home
  // key); elsewhere the bottom-edge up-swipe is Home and the center tap is the
  // only path, so the setting stays at its Tap default.
  if (!boardHasHomeKey()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) { return s.nameId == StrId::STR_SHOW_READER_MENU; }),
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
  // Unconditional, like the font-size rebuild below. The placeholder entry
  // indexes its single label by the raw fontFamily byte, so a device holding
  // LEXENDDECA=1 (set from Text Settings) would render an out-of-range blank
  // row; buildFontFamilySetting()'s getter collapses every built-in value onto
  // the one remaining family instead.
  {
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
  if (dictionaries && !dictionaries->empty()) {
    // Insert at the end of the Reader category (just before the first Controls entry).
    auto it =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.category == StrId::STR_CAT_CONTROLS; });
    v.insert(it, buildDictionarySetting(*dictionaries));
  }
  return v;
}
