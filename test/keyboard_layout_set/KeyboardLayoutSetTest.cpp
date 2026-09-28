#include <gtest/gtest.h>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "KeyboardLayoutSet.h"

namespace {

using freeink::ui::KeyboardLayoutId;

// The table is expected to gain non-Latin layouts; the guard tests below are
// about what happens when it does, so they arm themselves rather than being
// pinned to one particular layout.
int firstNonLatinRow() {
  for (uint8_t i = 0; i < keyboard_layouts::COUNT; ++i) {
    if (!keyboard_layouts::ALL[i].latin) return i;
  }
  return -1;
}

uint16_t bitFor(const KeyboardLayoutId id) {
  for (uint8_t i = 0; i < keyboard_layouts::COUNT; ++i) {
    if (keyboard_layouts::ALL[i].id == id) return keyboard_layouts::bitAt(i);
  }
  return 0;
}

class KeyboardLayoutSetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SETTINGS.keyboardLayouts = 0;
    I18N.setLanguage(Language::EN);
  }
};

// The guard that keeps Latin input reachable is only as good as the bit set it
// tests against. A hand-written LATIN_BITS span claimed bits the table had not
// assigned yet, so the first non-Latin layout appended satisfied the guard by
// itself. Derive it, and this cannot drift.
TEST_F(KeyboardLayoutSetTest, LatinBitsNamesExactlyTheLatinRows) {
  uint16_t expected = 0;
  for (uint8_t i = 0; i < keyboard_layouts::COUNT; ++i) {
    if (keyboard_layouts::ALL[i].latin) expected |= keyboard_layouts::bitAt(i);
  }
  EXPECT_EQ(keyboard_layouts::LATIN_BITS, expected);
}

TEST_F(KeyboardLayoutSetTest, NoNonLatinRowIsCountedAsLatin) {
  for (uint8_t i = 0; i < keyboard_layouts::COUNT; ++i) {
    if (keyboard_layouts::ALL[i].latin) continue;
    EXPECT_EQ(keyboard_layouts::LATIN_BITS & keyboard_layouts::bitAt(i), 0)
        << "row " << static_cast<int>(i) << " is not Latin but is claimed by LATIN_BITS";
  }
}

TEST_F(KeyboardLayoutSetTest, ANonLatinRowIsNeverInsideLatinBits) {
  const int row = firstNonLatinRow();
  if (row < 0) GTEST_SKIP() << "no non-Latin layout ships yet";
  EXPECT_EQ(keyboard_layouts::bitAt(static_cast<uint8_t>(row)) & keyboard_layouts::LATIN_BITS, 0);
}

// The regression: a saved mask naming only a non-Latin layout must still come
// back with English, or a Wi-Fi password becomes untypable.
TEST_F(KeyboardLayoutSetTest, ANonLatinOnlyMaskGetsEnglishBack) {
  const int row = firstNonLatinRow();
  if (row < 0) GTEST_SKIP() << "no non-Latin layout ships yet";
  const uint16_t nonLatin = keyboard_layouts::bitAt(static_cast<uint8_t>(row));
  SETTINGS.keyboardLayouts = nonLatin;
  const uint16_t result = keyboard_layouts::enabled();
  EXPECT_TRUE(result & bitFor(KeyboardLayoutId::QwertyEn));
  EXPECT_TRUE(result & nonLatin) << "the user's own choice must survive";
  EXPECT_TRUE(result & keyboard_layouts::LATIN_BITS);
}

TEST_F(KeyboardLayoutSetTest, EveryReachableMaskKeepsALatinLayout) {
  const uint16_t all = static_cast<uint16_t>((1u << keyboard_layouts::COUNT) - 1);
  for (uint16_t mask = 0; mask <= all; ++mask) {
    SETTINGS.keyboardLayouts = mask;
    EXPECT_TRUE(keyboard_layouts::enabled() & keyboard_layouts::LATIN_BITS) << "mask " << mask;
  }
}

TEST_F(KeyboardLayoutSetTest, AMaskNamingNoLayoutFallsBackRatherThanEnablingNothing) {
  // Bits above COUNT survive downgrades and hand-edited files.
  SETTINGS.keyboardLayouts = 0xFF00;
  const uint16_t result = keyboard_layouts::enabled();
  EXPECT_NE(result, 0);
  EXPECT_TRUE(result & bitFor(KeyboardLayoutId::QwertyEn));
}

TEST_F(KeyboardLayoutSetTest, UnconfiguredGivesTheUiLanguagePlusEnglish) {
  const int row = firstNonLatinRow();
  if (row < 0) GTEST_SKIP() << "no non-Latin layout ships yet";
  I18N.setLanguage(keyboard_layouts::ALL[row].language);
  SETTINGS.keyboardLayouts = 0;
  const uint16_t result = keyboard_layouts::enabled();
  EXPECT_TRUE(result & keyboard_layouts::bitAt(static_cast<uint8_t>(row)));
  EXPECT_TRUE(result & bitFor(KeyboardLayoutId::QwertyEn));
}

// BG and FA ship as UI languages with no keyboard of their own.
TEST_F(KeyboardLayoutSetTest, ALanguageWithNoLayoutStillLandsOnEnglish) {
  for (const Language language : {Language::BG, Language::FA}) {
    I18N.setLanguage(language);
    SETTINGS.keyboardLayouts = 0;
    EXPECT_TRUE(keyboard_layouts::enabled() & bitFor(KeyboardLayoutId::QwertyEn));
  }
}

TEST_F(KeyboardLayoutSetTest, StartingLayoutStaysWithinTheEnabledSet) {
  const int row = firstNonLatinRow();
  if (row < 0) GTEST_SKIP() << "no non-Latin layout ships yet";
  I18N.setLanguage(keyboard_layouts::ALL[row].language);
  SETTINGS.keyboardLayouts = bitFor(KeyboardLayoutId::QwertyEn);
  EXPECT_EQ(keyboard_layouts::startingLayout(), KeyboardLayoutId::QwertyEn);
}

TEST_F(KeyboardLayoutSetTest, StartingLayoutPrefersTheUiLanguageWhenItIsEnabled) {
  const int row = firstNonLatinRow();
  if (row < 0) GTEST_SKIP() << "no non-Latin layout ships yet";
  const auto& other = keyboard_layouts::ALL[row];
  I18N.setLanguage(other.language);
  SETTINGS.keyboardLayouts =
      static_cast<uint16_t>(bitFor(KeyboardLayoutId::QwertyEn) | keyboard_layouts::bitAt(static_cast<uint8_t>(row)));
  EXPECT_EQ(keyboard_layouts::startingLayout(), other.id);
}

TEST_F(KeyboardLayoutSetTest, NextCyclesOnlyThroughEnabledLayouts) {
  const int row = firstNonLatinRow();
  if (row < 0) GTEST_SKIP() << "no non-Latin layout ships yet";
  const auto& other = keyboard_layouts::ALL[row];
  SETTINGS.keyboardLayouts =
      static_cast<uint16_t>(bitFor(KeyboardLayoutId::QwertyEn) | keyboard_layouts::bitAt(static_cast<uint8_t>(row)));
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::QwertyEn), other.id);
  EXPECT_EQ(keyboard_layouts::next(other.id), KeyboardLayoutId::QwertyEn);
}

TEST_F(KeyboardLayoutSetTest, NextStaysPutWhenOnlyOneLayoutIsEnabled) {
  SETTINGS.keyboardLayouts = bitFor(KeyboardLayoutId::QwertyEn);
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::QwertyEn), KeyboardLayoutId::QwertyEn);
}

// Table position is the persisted bit assignment, so reordering rows silently
// reinterprets every saved mask.
TEST_F(KeyboardLayoutSetTest, EnglishStaysAtBitZero) {
  ASSERT_GT(keyboard_layouts::COUNT, 0);
  EXPECT_EQ(keyboard_layouts::ALL[0].id, KeyboardLayoutId::QwertyEn);
  EXPECT_TRUE(keyboard_layouts::ALL[0].latin);
}

}  // namespace
