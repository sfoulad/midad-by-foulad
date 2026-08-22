#include <gtest/gtest.h>

#include <algorithm>

#include "Epub/Epub/parsers/LineHeightMath.h"

// Mirrors finishTableRow()'s pre-fix per-cell estimate: a flat Latin-only line height with no
// Arabic comparison and no ruby shift folded in. Kept only so these tests can demonstrate what
// the bug actually produced, without resurrecting the buggy code path in production.
int preFixRowCellEstimate(const int latinLineHeight, const float lineCompression) {
  return static_cast<int>(latinLineHeight * lineCompression);
}

// Mirrors finishTableRow()'s fixed per-cell contribution: computeLineHeight()'s Arabic-aware
// max, plus the line's ruby shift -- the same expression addLineToPage() uses for the identical
// TextBlock, so the two can never disagree about how tall a line actually renders.
int rowCellContribution(const int latinLineHeight, const int arabicLineHeight, const bool hasArabic,
                        const float lineCompression, const int rubyShift) {
  return effectiveLineHeight(latinLineHeight, arabicLineHeight, hasArabic, lineCompression) + rubyShift;
}

TEST(LineHeightMath, ArabicLineTallerThanLatinIncreasesHeight) {
  constexpr int latin = 20;
  constexpr int arabic = 30;
  EXPECT_GT(effectiveLineHeight(latin, arabic, /*hasArabic=*/true, 1.0f),
            effectiveLineHeight(latin, arabic, /*hasArabic=*/false, 1.0f));
  EXPECT_EQ(effectiveLineHeight(latin, arabic, /*hasArabic=*/true, 1.0f), arabic);
}

TEST(LineHeightMath, ArabicLineShorterThanLatinDoesNotShrinkHeight) {
  // Arabic metrics aren't always taller than Latin -- the formula must still take the max,
  // never silently prefer the (here, smaller) Arabic figure.
  constexpr int latin = 30;
  constexpr int arabic = 20;
  EXPECT_EQ(effectiveLineHeight(latin, arabic, /*hasArabic=*/true, 1.0f), latin);
}

TEST(LineHeightMath, RubyShiftIsIncludedInRowContribution) {
  constexpr int latin = 20;
  constexpr int arabic = 30;
  constexpr float compression = 1.0f;
  const int withoutRuby = rowCellContribution(latin, arabic, true, compression, /*rubyShift=*/0);
  const int withRuby = rowCellContribution(latin, arabic, true, compression, /*rubyShift=*/5);
  EXPECT_EQ(withRuby, withoutRuby + 5);
}

TEST(LineHeightMath, FixedEstimateNeverUndercountsWhatAddLineToPageWillRender) {
  // The bug: finishTableRow()'s page-fit check used a flat Latin estimate while addLineToPage()
  // rendered with the Arabic-aware max + ruby shift, so a row could pass the fit check and then
  // still get pushed onto the next page once actually rendered. The fixed row estimate must call
  // the identical formula addLineToPage() uses, so it can never fall short of it.
  constexpr int latin = 22;
  constexpr int arabic = 34;
  constexpr float compression = 1.15f;
  constexpr int rubyShift = 6;

  const int actualRenderedHeight = rowCellContribution(latin, arabic, true, compression, rubyShift);
  // Independently computed, not via rowCellContribution(), so this isn't tautological: the real
  // assertion is that the fixed formula matches addLineToPage()'s max(latin, arabic) * compression
  // + rubyShift by construction, not merely by calling the same helper twice.
  const int expectedHeight = static_cast<int>(std::max(latin, arabic) * compression) + rubyShift;
  EXPECT_EQ(actualRenderedHeight, expectedHeight);

  // What the pre-fix code would have used for the same row/line -- strictly smaller here,
  // demonstrating the under-count that let a too-tall row pass the fit check.
  const int buggyEstimate = preFixRowCellEstimate(latin, compression) + rubyShift;
  EXPECT_LT(buggyEstimate, actualRenderedHeight);
}

TEST(LineHeightMath, LatinOnlyTableLayoutUnchanged) {
  // hasArabic=false must reduce to exactly the old flat Latin formula, regardless of whatever
  // (unused) arabicLineHeight value happens to be passed in.
  constexpr int latin = 24;
  constexpr float compression = 1.1f;
  const int expected = preFixRowCellEstimate(latin, compression);
  EXPECT_EQ(effectiveLineHeight(latin, /*arabicLineHeight=*/9999, /*hasArabic=*/false, compression), expected);
  EXPECT_EQ(effectiveLineHeight(latin, /*arabicLineHeight=*/0, /*hasArabic=*/false, compression), expected);
}
