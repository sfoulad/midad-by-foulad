#include <gtest/gtest.h>

#include <ArabicShaper.h>

#include <algorithm>

// Regression coverage for two fixes:
//  1. ArabicShaper used to drop tashkeel/harakat diacritics entirely; they now pass
//     through so the renderer can draw them as stacked combining marks.
//  2. The visual-order (bidi) reversal used to reverse individual codepoints, which
//     would put a diacritic BEFORE its base letter once an RTL run flipped (diacritics
//     always follow their base in logical/Unicode storage order). It now reverses in
//     grapheme-cluster order, keeping each base + its trailing diacritics together.

TEST(ArabicShaper, DiacriticPassesThroughInsteadOfBeingDropped) {
  // Hamza (isolated form has no joining neighbors) + Fatha.
  const auto shaped = ArabicShaper::shapeText("ءَ");
  ASSERT_EQ(shaped.size(), 2u);
  EXPECT_EQ(shaped[0], 0xFE80u);  // Hamza isolated form
  EXPECT_EQ(shaped[1], 0x064Eu);  // Fatha, unchanged
}

TEST(ArabicShaper, MultiClusterVisualOrderKeepsDiacriticWithItsOwnBase) {
  // Hamza+Fatha then Hamza+Damma (Hamza is non-joining, so both isolate regardless of
  // context -- keeps this test's expectations independent of joining-context logic).
  // Logical/typed order is [Hamza1, Fatha, Hamza2, Damma]; Hamza2+Damma is the visually
  // LEFTMOST pair once reversed for LTR rendering, so it must come first in the output,
  // with each diacritic still immediately after (not before) its own Hamza.
  const auto shaped = ArabicShaper::shapeText("ءَءُ");
  ASSERT_EQ(shaped.size(), 4u);
  EXPECT_EQ(shaped[0], 0xFE80u);  // Hamza (2nd logical, leftmost visually)
  EXPECT_EQ(shaped[1], 0x064Fu);  // its Damma stays right after it
  EXPECT_EQ(shaped[2], 0xFE80u);  // Hamza (1st logical, rightmost visually)
  EXPECT_EQ(shaped[3], 0x064Eu);  // its Fatha stays right after it
}

TEST(ArabicShaper, LeadingDiacriticWithNoBaseDoesNotCrash) {
  const auto shaped = ArabicShaper::shapeText("َقال");  // Fatha before a real word
  EXPECT_FALSE(shaped.empty());
}

TEST(ArabicShaper, LamAlefLigatureStillApplies) {
  // Lam + Alef -> single ligature glyph (Presentation Forms-B), unaffected by the
  // diacritic-passthrough change since it runs in an earlier shaping step.
  const auto shaped = ArabicShaper::shapeText("لا");
  ASSERT_EQ(shaped.size(), 1u);
  EXPECT_EQ(shaped[0], 0xFEFBu);  // Lam-Alef isolated ligature
}

TEST(ArabicShaper, AllahLigatureStillDetected) {
  const auto shaped = ArabicShaper::shapeText("الله");
  EXPECT_NE(std::find(shaped.begin(), shaped.end(), 0xFDF2u), shaped.end());
}

TEST(ArabicCharacterClassification, BelowMarksAreClassifiedCorrectly) {
  EXPECT_TRUE(ArabicShaper::isArabicBelowMark(0x0650));   // Kasra
  EXPECT_TRUE(ArabicShaper::isArabicBelowMark(0x064D));   // Kasratan
  EXPECT_FALSE(ArabicShaper::isArabicBelowMark(0x064E));  // Fatha (above)
  EXPECT_FALSE(ArabicShaper::isArabicBelowMark(0x0651));  // Shadda (above)
  EXPECT_FALSE(ArabicShaper::isArabicBelowMark(0x0628));  // Beh (not a diacritic at all)
}
