#include <gtest/gtest.h>

#include "Utf8.h"

// Hebrew points/cantillation (U+0591-U+05BD, U+05BF, U+05C1-U+05C2, U+05C4-U+05C5, U+05C7)
// must classify as combining marks so GfxRenderer's contextual-anchoring path picks them up.
TEST(Utf8IsCombiningMark, ClassifiesHebrewPointsAsCombining) {
  EXPECT_TRUE(utf8IsCombiningMark(0x0591));  // first codepoint of the block (accent)
  EXPECT_TRUE(utf8IsCombiningMark(0x05B0));  // Sheva
  EXPECT_TRUE(utf8IsCombiningMark(0x05BD));  // Meteg, last codepoint before Maqaf
  EXPECT_TRUE(utf8IsCombiningMark(0x05BF));  // Rafe
  EXPECT_TRUE(utf8IsCombiningMark(0x05C1));  // Shin dot
  EXPECT_TRUE(utf8IsCombiningMark(0x05C2));  // Sin dot
  EXPECT_TRUE(utf8IsCombiningMark(0x05C4));  // upper dot
  EXPECT_TRUE(utf8IsCombiningMark(0x05C5));  // lower dot
  EXPECT_TRUE(utf8IsCombiningMark(0x05C7));  // Qamats Qatan, last codepoint of the block
}

// Four codepoints inside U+0591-U+05C7 are spacing punctuation (Unicode category Pd/Po),
// not combining marks -- they must keep normal advance width, not be treated as
// zero-advance overlays on the previous glyph.
TEST(Utf8IsCombiningMark, ExcludesHebrewPunctuationWithinTheBlock) {
  EXPECT_FALSE(utf8IsCombiningMark(0x05BE));  // Maqaf (hyphen)
  EXPECT_FALSE(utf8IsCombiningMark(0x05C0));  // Paseq
  EXPECT_FALSE(utf8IsCombiningMark(0x05C3));  // Sof Pasuq
  EXPECT_FALSE(utf8IsCombiningMark(0x05C6));  // Nun Hafukha
}

// Codepoints immediately outside the Hebrew points block must not be misclassified.
TEST(Utf8IsCombiningMark, DoesNotOverrunTheHebrewPointsBlock) {
  EXPECT_FALSE(utf8IsCombiningMark(0x0590));  // unassigned, just before the block
  EXPECT_FALSE(utf8IsCombiningMark(0x05C8));  // unassigned, just after the block
  EXPECT_FALSE(utf8IsCombiningMark(0x05D0));  // Alef -- a base letter, not a mark
}

// Pre-existing combining-mark ranges (Latin/Vietnamese diacritics) are unaffected by
// folding in the Hebrew check.
TEST(Utf8IsCombiningMark, StillClassifiesLatinCombiningMarks) {
  EXPECT_TRUE(utf8IsCombiningMark(0x0301));  // Combining Acute Accent
  EXPECT_FALSE(utf8IsCombiningMark('a'));
}
