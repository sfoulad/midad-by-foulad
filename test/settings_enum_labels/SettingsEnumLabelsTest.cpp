#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>

#include "CrossPointSettings.h"
#include "SettingsEnumLabels.h"
#include "util/OpdsFilename.h"

// Every enum-backed Settings row indexes its label list with the stored value:
// SettingsActivity renders enumValues[value] (blank when out of range) and one
// Confirm writes (value + 1) % enumValues.size(), while
// CrossPointSettings::fromJson() clamps a loaded value the same way. A label
// list shorter than its enum therefore hides options AND corrupts a value the
// UI cannot even display -- exactly how touchReaderControls defaulted to Swipe
// while offering only Off/On, and how refreshFrequency's Never went missing.
//
// src/SettingsEnumLabels.h carries a static_assert per table, so a mismatch is
// already a compile error. These cases restate the same contract per row as an
// executable audit: the failure names the offending setting instead of a
// static_assert message, and the expected counts are read from each enum's own
// COUNT sentinel rather than restated here.

namespace {

// Board-conditional rows deliberately publish a PREFIX of their full table
// (see buildLongPressMenuValues() and the FREEINK_CAP_TOUCH split in
// SettingsList.h). A prefix keeps every stored value's meaning; only a
// different ORDER would move it. These check that the trim really is a prefix
// -- i.e. exactly one entry short, from the end.
constexpr std::size_t kOneTrimmed = 1;

}  // namespace

// --- Display ---

TEST(SettingsEnumLabels, SleepScreenCoverModeCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kSleepCoverMode),
            static_cast<std::size_t>(CrossPointSettings::SLEEP_SCREEN_COVER_MODE_COUNT));
}

TEST(SettingsEnumLabels, SleepScreenCoverFilterCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kSleepCoverFilter),
            static_cast<std::size_t>(CrossPointSettings::SLEEP_SCREEN_COVER_FILTER_COUNT));
}

TEST(SettingsEnumLabels, QuickResumeTimeoutCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kQuickResumeTimeout),
            static_cast<std::size_t>(CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN_COUNT));
}

TEST(SettingsEnumLabels, HideBatteryCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kHideBattery),
            static_cast<std::size_t>(CrossPointSettings::HIDE_BATTERY_PERCENTAGE_COUNT));
}

// Regression: the list stopped at "30 pages" while REFRESH_NEVER existed and
// getRefreshFrequency() already returned INT_MAX for it, so Never was
// unreachable on-device and a 5 pushed over the web API rendered blank.
TEST(SettingsEnumLabels, RefreshFrequencyCoversEveryValueIncludingNever) {
  EXPECT_EQ(std::size(settings_labels::kRefreshFrequency),
            static_cast<std::size_t>(CrossPointSettings::REFRESH_FREQUENCY_COUNT));
  EXPECT_EQ(settings_labels::kRefreshFrequency[CrossPointSettings::REFRESH_NEVER], StrId::STR_NEVER);
}

// --- Reader ---

TEST(SettingsEnumLabels, ArabicFontSizeCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kArabicFontSize), static_cast<std::size_t>(CrossPointSettings::FONT_SIZE_COUNT));
}

TEST(SettingsEnumLabels, LineSpacingCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kLineSpacing),
            static_cast<std::size_t>(CrossPointSettings::LINE_COMPRESSION_COUNT));
}

TEST(SettingsEnumLabels, ParagraphAlignmentCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kParagraphAlignment),
            static_cast<std::size_t>(CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT));
}

TEST(SettingsEnumLabels, DailyReadingGoalCoversEveryGoalSlot) {
  EXPECT_EQ(std::size(settings_labels::kDailyReadingGoal), std::size(CrossPointSettings::DAILY_GOAL_MINUTES));
}

TEST(SettingsEnumLabels, OrientationCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kOrientation), static_cast<std::size_t>(CrossPointSettings::ORIENTATION_COUNT));
}

TEST(SettingsEnumLabels, ImageRenderingCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kImageRendering),
            static_cast<std::size_t>(CrossPointSettings::IMAGE_RENDERING_COUNT));
}

// Regression: readerMenuStyle had no row at all, so main.cpp's touch-board
// Toolbar seed won every boot with no way to choose the list style.
TEST(SettingsEnumLabels, ReaderMenuStyleCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kReaderMenuStyle),
            static_cast<std::size_t>(CrossPointSettings::READER_MENU_STYLE_COUNT));
  EXPECT_EQ(settings_labels::kReaderMenuStyle[CrossPointSettings::READER_MENU_TOOLBAR], StrId::STR_MENU_STYLE_TOOLBAR);
}

// --- Controls ---

TEST(SettingsEnumLabels, SideButtonLayoutCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kSideButtonLayout),
            static_cast<std::size_t>(CrossPointSettings::SIDE_BUTTON_LAYOUT_COUNT));
}

// Regression: two labels (Off/On) against a four-value enum whose DEFAULT is
// Swipe (2). The row rendered blank on a fresh X4 Pro and one Confirm wrote
// (2 + 1) % 2 = 1, silently downgrading Swipe to Tap with no way back.
TEST(SettingsEnumLabels, TouchReaderControlsCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kTouchReaderControls),
            static_cast<std::size_t>(CrossPointSettings::TOUCH_READER_CONTROLS_COUNT));
}

TEST(SettingsEnumLabels, TouchReaderControlsLabelsTheShippedDefault) {
  CrossPointSettings::TOUCH_READER_CONTROLS defaultValue = CrossPointSettings::TOUCH_READER_SWIPE;
  ASSERT_LT(static_cast<std::size_t>(defaultValue), std::size(settings_labels::kTouchReaderControls));
  EXPECT_EQ(settings_labels::kTouchReaderControls[defaultValue], StrId::STR_STATE_SWIPE);
}

TEST(SettingsEnumLabels, ShowReaderMenuCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kShowReaderMenu),
            static_cast<std::size_t>(CrossPointSettings::SHOW_READER_MENU_COUNT));
}

TEST(SettingsEnumLabels, LongPressBehaviorCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kLongPressBehavior),
            static_cast<std::size_t>(CrossPointSettings::LONG_PRESS_BUTTON_BEHAVIOR_COUNT));
}

// Regression: the row hardcoded four labels while LP_MENU_READER_MENU (4)
// existed, and buildLongPressMenuValues() -- which trims that entry off on
// non-home-key boards -- had no callers at all.
TEST(SettingsEnumLabels, LongPressMenuCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kLongPressMenu),
            static_cast<std::size_t>(CrossPointSettings::LONG_PRESS_MENU_FUNCTION_COUNT));
  EXPECT_EQ(settings_labels::kLongPressMenu[CrossPointSettings::LP_MENU_READER_MENU], StrId::STR_READER_MENU);
}

TEST(SettingsEnumLabels, LongPressMenuNoHomeKeyListIsAPrefix) {
  EXPECT_EQ(settings_labels::kLongPressMenuNoHomeKeyCount, std::size(settings_labels::kLongPressMenu) - kOneTrimmed);
  // Only the Reader Menu entry is dropped, so no other stored value moves.
  EXPECT_EQ(settings_labels::kLongPressMenuNoHomeKeyCount,
            static_cast<std::size_t>(CrossPointSettings::LP_MENU_READER_MENU));
}

TEST(SettingsEnumLabels, ShortPowerButtonCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kShortPwrBtn), static_cast<std::size_t>(CrossPointSettings::SHORT_PWRBTN_COUNT));
  EXPECT_EQ(settings_labels::kShortPwrBtn[CrossPointSettings::PWR_CONFIRM], StrId::STR_CONFIRM);
}

TEST(SettingsEnumLabels, ShortPowerButtonNoTouchListIsAPrefix) {
  EXPECT_EQ(settings_labels::kShortPwrBtnNoTouchCount, std::size(settings_labels::kShortPwrBtn) - kOneTrimmed);
  // Confirm is the only entry dropped on non-touch boards.
  EXPECT_EQ(settings_labels::kShortPwrBtnNoTouchCount, static_cast<std::size_t>(CrossPointSettings::PWR_CONFIRM));
}

TEST(SettingsEnumLabels, TiltPageTurnCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kTiltPageTurn),
            static_cast<std::size_t>(CrossPointSettings::TILT_PAGE_TURN_COUNT));
}

// --- Status bar ---

TEST(SettingsEnumLabels, StatusBarProgressBarCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kStatusBarProgressBar),
            static_cast<std::size_t>(CrossPointSettings::STATUS_BAR_PROGRESS_BAR_COUNT));
}

TEST(SettingsEnumLabels, ProgressBarThicknessCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kProgressBarThickness),
            static_cast<std::size_t>(CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT));
}

TEST(SettingsEnumLabels, StatusBarTitleCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kStatusBarTitle),
            static_cast<std::size_t>(CrossPointSettings::STATUS_BAR_TITLE_COUNT));
}

TEST(SettingsEnumLabels, XtcStatusBarCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kXtcStatusBar),
            static_cast<std::size_t>(CrossPointSettings::XTC_STATUS_BAR_MODE_COUNT));
}

TEST(SettingsEnumLabels, ClockFormatCoversBothFormats) {
  EXPECT_EQ(std::size(settings_labels::kClockFormat), static_cast<std::size_t>(2));
}

// --- Hidden (category-less) rows ---

TEST(SettingsEnumLabels, OpdsFilenameFormatCoversEveryValue) {
  EXPECT_EQ(std::size(settings_labels::kOpdsFilenameFormat), static_cast<std::size_t>(OpdsFilenameFormat::Count));
}

// The two tables SettingsList.h still builds by assigning BY ENUM VALUE into a
// COUNT-sized vector are deliberately not in SettingsEnumLabels.h. Their shape
// cannot go short, but it can only stay correct while the COUNT sentinel is the
// real cardinality, so pin the sentinels the vectors are sized from.
TEST(SettingsEnumLabels, ValueIndexedTablesHaveTheExpectedCardinality) {
  EXPECT_EQ(static_cast<std::size_t>(CrossPointSettings::SLEEP_SCREEN_MODE_COUNT),
            static_cast<std::size_t>(CrossPointSettings::TRANSPARENT_CUSTOM) + 1);
  EXPECT_EQ(static_cast<std::size_t>(CrossPointSettings::STATUS_BAR_CLOCK_MODE_COUNT),
            static_cast<std::size_t>(CrossPointSettings::STATUS_BAR_CLOCK_LEFT) + 1);
}
