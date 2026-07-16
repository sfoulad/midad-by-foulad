#include <gtest/gtest.h>

#include <algorithm>

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

// Some fonts (e.g. Tajawal) never define presentation-form glyphs at the legacy
// ISOLATED codepoint (0xFEE5 for Noon here) -- by Unicode convention it's
// identical to the plain base letter, and font authors expect GSUB to pick it.
// hasGlyph rejecting that one codepoint must fall back to the bare base
// codepoint (0x0646), not silently drop the letter -- a real bug found on
// Tajawal where whole words vanished/garbled because the old code never
// checked glyph availability before emitting a hardcoded presentation form.
TEST(ArabicShaper, MissingIsolatedFormFallsBackToBaseCodepoint) {
  const auto hasGlyph = [](uint32_t cp) { return cp != 0xFEE5u; };
  const auto out = ArabicShaper::shapeText("ن", hasGlyph);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 0x0646u);  // base Noon, not the missing isolated form
}

// Same fallback, but for a letter whose chosen FINAL form is unavailable and
// whose ISOLATED fallback (the next tier in the cascade) is unavailable too --
// exactly Tajawal's real situation, which defines final/initial/medial forms
// but never isolated ones. Must fall all the way to the base codepoint rather
// than getting stuck skipping tiers indefinitely or dropping the glyph.
TEST(ArabicShaper, MissingFinalAndIsolatedFormsFallBackToBaseCodepoint) {
  const auto hasGlyph = [](uint32_t cp) { return cp != 0xFE84u && cp != 0xFE83u; };
  const auto out = ArabicShaper::shapeText("بأ", hasGlyph);  // Beh + Alef Hamza Above
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1], 0xFE91u);  // Beh initial (unaffected)
  EXPECT_EQ(out[0], 0x0623u);  // base Alef Hamza Above, not the missing final/isolated forms
}

// --- Kashida (tatweel) justification ---

// "كتب" (Kaf-Teh-Beh): every letter is DUAL_JOINING, so both internal junctions
// (Kaf->Teh, Teh->Beh) are legal kashida points.
TEST(ArabicShaper, KashidaPointBetweenDualJoiningLetters) {
  EXPECT_TRUE(ArabicShaper::hasKashidaPoint("كتب"));
}

// "ودر" (Waw-Dal-Ra): all three are RIGHT_JOINING-only -- none of them extend a
// connection forward into the next letter, so no kashida point exists anywhere.
TEST(ArabicShaper, NoKashidaPointForRightJoiningChain) {
  EXPECT_FALSE(ArabicShaper::hasKashidaPoint("ودر"));
}

TEST(ArabicShaper, NoKashidaPointForSingleLetterWord) { EXPECT_FALSE(ArabicShaper::hasKashidaPoint("ب")); }

TEST(ArabicShaper, HasKashidaPointFalseForEmptyString) {
  EXPECT_FALSE(ArabicShaper::hasKashidaPoint(""));
  EXPECT_FALSE(ArabicShaper::hasKashidaPoint(nullptr));
}

// "كَتَب" (Kaf+Fatha+Teh+Fatha+Beh): a kashida run must be spliced in AFTER each
// letter's trailing harakat, never between the letter and its own mark -- or the
// mark visually detaches from its base letter (the bug this fixed on a real
// device: KFGQPC Uthmanic Hafs word joins broke this way before the fix).
TEST(ArabicShaper, KashidaSkipsTransparentDiacritics) {
  constexpr int tatweelAdvancePx = 10;
  const auto plain = shape("كَتَب");
  const auto withKashida = ArabicShaper::shapeTextWithKashida("كَتَب", 2 * tatweelAdvancePx, tatweelAdvancePx);

  const auto tatweelCount = std::count(withKashida.begin(), withKashida.end(), 0x0640u);
  EXPECT_EQ(tatweelCount, 2);
  EXPECT_EQ(withKashida.size(), plain.size() + 2);

  // Every diacritic (U+064B-065F, U+0670) must be immediately followed, in visual
  // (left-to-right draw) order, by a real letter -- never by an inserted tatweel run.
  for (size_t i = 0; i + 1 < withKashida.size(); i++) {
    const uint32_t cp = withKashida[i];
    const bool isDiacritic = (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670;
    if (isDiacritic) {
      EXPECT_NE(withKashida[i + 1], 0x0640u) << "diacritic at index " << i << " followed by tatweel";
    }
  }
}

// "كتبت" has 3 legal points (Kaf-Teh, Teh-Beh, Beh-Teh); 5 whole tatweel glyphs
// distributed across them should differ by at most one glyph per point.
TEST(ArabicShaper, KashidaDistributesEvenlyAcrossMultiplePoints) {
  constexpr int tatweelAdvancePx = 10;
  const auto out = ArabicShaper::shapeTextWithKashida("كتبت", 5 * tatweelAdvancePx, tatweelAdvancePx);

  std::vector<int> runLengths;
  int current = 0;
  for (const uint32_t cp : out) {
    if (cp == 0x0640u) {
      current++;
    } else if (current > 0) {
      runLengths.push_back(current);
      current = 0;
    }
  }
  if (current > 0) runLengths.push_back(current);

  ASSERT_EQ(runLengths.size(), 3u);
  const int total = runLengths[0] + runLengths[1] + runLengths[2];
  const int minLen = *std::min_element(runLengths.begin(), runLengths.end());
  const int maxLen = *std::max_element(runLengths.begin(), runLengths.end());
  EXPECT_EQ(total, 5);
  EXPECT_LE(maxLen - minLen, 1);
}

// A budget smaller than one tatweel glyph can't buy even a single insertion --
// output must be identical to plain shapeText().
TEST(ArabicShaper, KashidaDegradesGracefullyBelowOneGlyphWidth) {
  const auto plain = shape("كتب");
  const auto withKashida = ArabicShaper::shapeTextWithKashida("كتب", 5, 10);
  EXPECT_EQ(withKashida, plain);
}

// A font reporting no U+0640 glyph must produce plain (unstretched) output.
TEST(ArabicShaper, KashidaRespectsMissingGlyphCallback) {
  const auto hasGlyph = [](uint32_t cp) { return cp != 0x0640u; };
  const auto plain = ArabicShaper::shapeText("كتب", hasGlyph);
  const auto withKashida = ArabicShaper::shapeTextWithKashida("كتب", 20, 10, hasGlyph);
  EXPECT_EQ(withKashida, plain);
}
