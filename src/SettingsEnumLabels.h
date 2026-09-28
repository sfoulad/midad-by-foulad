#pragma once

#include <I18n.h>

#include <cstddef>
#include <iterator>

#include "CrossPointSettings.h"
#include "util/OpdsFilename.h"

// Label tables for every fixed-order enum-backed Settings row.
//
// A row's stored value indexes straight into its label list: SettingsActivity
// renders `enumValues[value]` (blank when the value is out of range) and one
// Confirm writes `(value + 1) % enumValues.size()`. A list shorter than its
// enum therefore does not merely hide options -- it renders an out-of-range
// value as an empty row and silently rewrites it to a low value the user
// never picked. CrossPointSettings::fromJson() clamps the same way, so a
// value pushed over the web API is discarded on the next load.
//
// Keeping the tables here, next to a static_assert against the enum's own
// COUNT sentinel, makes that class of bug a compile error rather than a
// device-only symptom. Adding an enum value without its label no longer
// builds. `inline constexpr` (not `static constexpr`) so the tables are one
// flash-resident copy shared by every translation unit that includes this.
//
// Deliberately NOT here: the sleep-screen and status-bar-clock tables, which
// are built as vectors sized by their COUNT sentinel and then assigned BY
// ENUM VALUE (see SettingsList.h) -- that shape already cannot go short, and
// its explicit value-indexed assignment guards a reordering hazard a flat
// array would reintroduce.
namespace settings_labels {

// --- Display ---
inline constexpr StrId kSleepCoverMode[] = {StrId::STR_FIT, StrId::STR_CROP};
static_assert(std::size(kSleepCoverMode) == CrossPointSettings::SLEEP_SCREEN_COVER_MODE_COUNT,
              "sleepScreenCoverMode labels must cover every SLEEP_SCREEN_COVER_MODE value");

inline constexpr StrId kSleepCoverFilter[] = {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED};
static_assert(std::size(kSleepCoverFilter) == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER_COUNT,
              "sleepScreenCoverFilter labels must cover every SLEEP_SCREEN_COVER_FILTER value");

inline constexpr StrId kQuickResumeTimeout[] = {StrId::STR_STATE_OFF, StrId::STR_STATE_ON};
static_assert(std::size(kQuickResumeTimeout) == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN_COUNT,
              "quickResumeSleepScreen labels must cover every QUICK_RESUME_SLEEP_SCREEN value");

inline constexpr StrId kHideBattery[] = {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS};
static_assert(std::size(kHideBattery) == CrossPointSettings::HIDE_BATTERY_PERCENTAGE_COUNT,
              "hideBatteryPercentage labels must cover every HIDE_BATTERY_PERCENTAGE value");

// REFRESH_NEVER (5) is what getRefreshFrequency() returns INT_MAX for.
inline constexpr StrId kRefreshFrequency[] = {StrId::STR_PAGES_1,  StrId::STR_PAGES_5,  StrId::STR_PAGES_10,
                                              StrId::STR_PAGES_15, StrId::STR_PAGES_30, StrId::STR_NEVER};
static_assert(std::size(kRefreshFrequency) == CrossPointSettings::REFRESH_FREQUENCY_COUNT,
              "refreshFrequency labels must cover every REFRESH_FREQUENCY value");

// --- Reader ---
// Arabic reading-font size only; the Latin size is a point size (ReaderFontSizes.h).
inline constexpr StrId kArabicFontSize[] = {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE};
static_assert(std::size(kArabicFontSize) == CrossPointSettings::FONT_SIZE_COUNT,
              "arabicFontSize labels must cover every FONT_SIZE value");

inline constexpr StrId kLineSpacing[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
static_assert(std::size(kLineSpacing) == CrossPointSettings::LINE_COMPRESSION_COUNT,
              "lineSpacing labels must cover every LINE_COMPRESSION value");

inline constexpr StrId kParagraphAlignment[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER,
                                                StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE};
static_assert(std::size(kParagraphAlignment) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT,
              "paragraphAlignment labels must cover every PARAGRAPH_ALIGNMENT value");

inline constexpr StrId kDailyReadingGoal[] = {StrId::STR_GOAL_15M, StrId::STR_GOAL_30M, StrId::STR_GOAL_45M,
                                              StrId::STR_GOAL_1H,  StrId::STR_GOAL_90M, StrId::STR_GOAL_2H};
static_assert(std::size(kDailyReadingGoal) == std::size(CrossPointSettings::DAILY_GOAL_MINUTES),
              "dailyReadingGoal labels must cover every DAILY_GOAL_MINUTES slot");

inline constexpr StrId kOrientation[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                         StrId::STR_LANDSCAPE_CCW};
static_assert(std::size(kOrientation) == CrossPointSettings::ORIENTATION_COUNT,
              "orientation labels must cover every ORIENTATION value");

inline constexpr StrId kImageRendering[] = {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER,
                                            StrId::STR_IMAGES_SUPPRESS};
static_assert(std::size(kImageRendering) == CrossPointSettings::IMAGE_RENDERING_COUNT,
              "imageRendering labels must cover every IMAGE_RENDERING value");

inline constexpr StrId kReaderMenuStyle[] = {StrId::STR_MENU_STYLE_LIST, StrId::STR_MENU_STYLE_TOOLBAR};
static_assert(std::size(kReaderMenuStyle) == CrossPointSettings::READER_MENU_STYLE_COUNT,
              "readerMenuStyle labels must cover every READER_MENU_STYLE value");

// --- Controls ---
inline constexpr StrId kSideButtonLayout[] = {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED};
static_assert(std::size(kSideButtonLayout) == CrossPointSettings::SIDE_BUTTON_LAYOUT_COUNT,
              "sideButtonLayout labels must cover every SIDE_BUTTON_LAYOUT value");

// Swipe (2) is the default on touch boards; ReaderUtils.h consumes all four.
inline constexpr StrId kTouchReaderControls[] = {StrId::STR_STATE_OFF, StrId::STR_STATE_TAP, StrId::STR_STATE_SWIPE,
                                                 StrId::STR_STATE_INVERTED_TAP};
static_assert(std::size(kTouchReaderControls) == CrossPointSettings::TOUCH_READER_CONTROLS_COUNT,
              "touchReaderControls labels must cover every TOUCH_READER_CONTROLS value");

inline constexpr StrId kShowReaderMenu[] = {StrId::STR_STATE_OFF, StrId::STR_STATE_TAP, StrId::STR_STATE_SWIPE_UP};
static_assert(std::size(kShowReaderMenu) == CrossPointSettings::SHOW_READER_MENU_COUNT,
              "showReaderMenu labels must cover every SHOW_READER_MENU value");

inline constexpr StrId kLongPressBehavior[] = {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                                               StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION};
static_assert(std::size(kLongPressBehavior) == CrossPointSettings::LONG_PRESS_BUTTON_BEHAVIOR_COUNT,
              "longPressButtonBehavior labels must cover every LONG_PRESS_BUTTON_BEHAVIOR value");

// LP_MENU_READER_MENU (4) is last because the reader menu is only offered on
// home-key boards, where the key's long press keeps the menu reachable --
// buildLongPressMenuValues() in SettingsList.h trims exactly that final entry
// elsewhere, so the shorter list is still a prefix of the full one and no
// stored value changes meaning.
inline constexpr StrId kLongPressMenu[] = {StrId::STR_KOSYNC, StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION,
                                           StrId::STR_DICTIONARY, StrId::STR_READER_MENU};
static_assert(std::size(kLongPressMenu) == CrossPointSettings::LONG_PRESS_MENU_FUNCTION_COUNT,
              "longPressMenuFunction labels must cover every LONG_PRESS_MENU_FUNCTION value");
inline constexpr std::size_t kLongPressMenuNoHomeKeyCount = std::size(kLongPressMenu) - 1;

// PWR_CONFIRM (5) is last for the same prefix reason as kLongPressMenu: it is
// only offered on touch boards (#if FREEINK_CAP_TOUCH in SettingsList.h),
// where the power button is the only Confirm available.
inline constexpr StrId kShortPwrBtn[] = {StrId::STR_IGNORE,        StrId::STR_SLEEP,     StrId::STR_PAGE_TURN,
                                         StrId::STR_FORCE_REFRESH, StrId::STR_FOOTNOTES, StrId::STR_CONFIRM};
static_assert(std::size(kShortPwrBtn) == CrossPointSettings::SHORT_PWRBTN_COUNT,
              "shortPwrBtn labels must cover every SHORT_PWRBTN value");
inline constexpr std::size_t kShortPwrBtnNoTouchCount = std::size(kShortPwrBtn) - 1;

inline constexpr StrId kTiltPageTurn[] = {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED};
static_assert(std::size(kTiltPageTurn) == CrossPointSettings::TILT_PAGE_TURN_COUNT,
              "tiltPageTurn labels must cover every TILT_PAGE_TURN value");

// --- Status bar ---
inline constexpr StrId kStatusBarProgressBar[] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};
static_assert(std::size(kStatusBarProgressBar) == CrossPointSettings::STATUS_BAR_PROGRESS_BAR_COUNT,
              "statusBarProgressBar labels must cover every STATUS_BAR_PROGRESS_BAR value");

inline constexpr StrId kProgressBarThickness[] = {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM,
                                                  StrId::STR_PROGRESS_BAR_THICK};
static_assert(std::size(kProgressBarThickness) == CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT,
              "statusBarProgressBarThickness labels must cover every STATUS_BAR_PROGRESS_BAR_THICKNESS value");

inline constexpr StrId kStatusBarTitle[] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};
static_assert(std::size(kStatusBarTitle) == CrossPointSettings::STATUS_BAR_TITLE_COUNT,
              "statusBarTitle labels must cover every STATUS_BAR_TITLE value");

inline constexpr StrId kXtcStatusBar[] = {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP};
static_assert(std::size(kXtcStatusBar) == CrossPointSettings::XTC_STATUS_BAR_MODE_COUNT,
              "xtcStatusBarMode labels must cover every XTC_STATUS_BAR_MODE value");

// clockFormat is a plain 0 = 24h / 1 = 12h byte with no COUNT sentinel of its own.
inline constexpr StrId kClockFormat[] = {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H};
static_assert(std::size(kClockFormat) == 2, "clockFormat labels must cover 24h and 12h");

// --- Hidden (category-less: persisted + web-exposed, edited elsewhere) ---
inline constexpr StrId kOpdsFilenameFormat[] = {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR,
                                                StrId::STR_FMT_TITLE};
static_assert(std::size(kOpdsFilenameFormat) == static_cast<std::size_t>(OpdsFilenameFormat::Count),
              "opdsFilenameFormat labels must cover every OpdsFilenameFormat value");

}  // namespace settings_labels
