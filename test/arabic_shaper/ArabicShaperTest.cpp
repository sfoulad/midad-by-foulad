#include <gtest/gtest.h>

#include "ArabicShaper.h"

// shapeText returns VISUAL order (reversed for RTL runs), so expectations list
// the shaped presentation forms in visual left-to-right order: for a pure-RTL
// word that is the LAST letter first.
namespace {
std::vector<uint32_t> shape(const char* utf8) { return ArabicShaper::shapeText(utf8); }
}  // namespace

// Standard Arabic sanity: "كتب" (Kaf-Teh-Beh) -> initial, medial, final.
TEST(ArabicShaper, StandardArabicJoins) {
  const auto out = shape("كتب");
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2], 0xFEDBu);  // Kaf initial (visual rightmost)
  EXPECT_EQ(out[1], 0xFE98u);  // Teh medial
  EXPECT_EQ(out[0], 0xFE90u);  // Beh final (visual leftmost)
}

// Persian "پچ" -- Peh joins forward into Tcheh.
TEST(ArabicShaper, PersianPehTchehJoin) {
  const auto out = shape("پچ");
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1], 0xFB58u);  // Peh initial
  EXPECT_EQ(out[0], 0xFB7Bu);  // Tcheh final
}

// Persian "گژ" -- Gaf initial, then Jeh (right-joining) takes its final form
// and does NOT join forward.
TEST(ArabicShaper, PersianGafJehRightJoining) {
  const auto out = shape("گژک");
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2], 0xFB94u);  // Gaf initial
  EXPECT_EQ(out[1], 0xFB8Bu);  // Jeh final (joined from the right only)
  EXPECT_EQ(out[0], 0xFB8Eu);  // Keheh isolated -- Jeh broke the chain
}

// Persian word-final Farsi Yeh renders the dotless final form (FBFD).
TEST(ArabicShaper, FarsiYehDotlessFinal) {
  const auto out = shape("بی");  // Beh + Farsi Yeh
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1], 0xFE91u);  // Beh initial
  EXPECT_EQ(out[0], 0xFBFDu);  // Farsi Yeh final (dotless)
}

// Ottoman/Kurdish Ae: joins to the PREVIOUS letter (final form) but never to
// the following one -- the next letter starts a fresh joining group.
TEST(ArabicShaper, AeNeverJoinsForward) {
  const auto out = shape("بەب");  // Beh + Ae + Beh
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2], 0xFE91u);  // Beh initial (joined into Ae)
  EXPECT_EQ(out[1], 0xFEEAu);  // Ae final (borrowed Heh final glyph)
  EXPECT_EQ(out[0], 0xFE8Fu);  // second Beh ISOLATED -- Ae did not join forward
}

// Ottoman Ng (dual-joining) takes a medial form between joining letters.
TEST(ArabicShaper, OttomanNgMedial) {
  const auto out = shape("بڭب");  // Beh + Ng + Beh
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2], 0xFE91u);  // Beh initial
  EXPECT_EQ(out[1], 0xFBD6u);  // Ng medial
  EXPECT_EQ(out[0], 0xFE90u);  // Beh final
}
