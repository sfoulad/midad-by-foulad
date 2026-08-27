#include <gtest/gtest.h>

#include "components/ListHitTest.h"

// Single-page list: rect(0,0,100,300), rowHeight=30 -> 10 rows fit, no paging.
TEST(ListHitTest, HitsCorrectRowWithinSinglePage) {
  const auto r0 = listHitTest(0, 0, 100, 300, 5, 0, 30, 50, 5);
  EXPECT_TRUE(r0.hit);
  EXPECT_EQ(r0.index, 0);

  const auto r2 = listHitTest(0, 0, 100, 300, 5, 0, 30, 50, 65);
  EXPECT_TRUE(r2.hit);
  EXPECT_EQ(r2.index, 2);
}

TEST(ListHitTest, MissesOutsideHorizontalBounds) {
  EXPECT_FALSE(listHitTest(0, 0, 100, 300, 5, 0, 30, -1, 5).hit);
  EXPECT_FALSE(listHitTest(0, 0, 100, 300, 5, 0, 30, 100, 5).hit);
}

TEST(ListHitTest, MissesAboveRect) { EXPECT_FALSE(listHitTest(0, 10, 100, 300, 5, 0, 30, 50, 5).hit); }

TEST(ListHitTest, MissesBelowLastRealRowWhenFewerItemsThanPage) {
  // 3 items, page holds 10 rows: only rows 0-2 are real; a tap below row 2
  // (y=90, the 4th row slot) must miss rather than resolving to a phantom
  // index 3.
  EXPECT_FALSE(listHitTest(0, 0, 100, 300, 3, 0, 30, 50, 90).hit);
}

// Two-page list: 25 items, rect height 300 / rowHeight 30 -> 10 rows/page.
TEST(ListHitTest, SelectedIndexOnPageTwoOffsetsHitIndexByPageStart) {
  // selectedIndex=12 -> pageStartIndex = (12/10)*10 = 10, so a tap on the
  // first visible row (y=5) must resolve to absolute index 10, not 0.
  const auto r = listHitTest(0, 0, 100, 300, 25, 12, 30, 50, 5);
  EXPECT_TRUE(r.hit);
  EXPECT_EQ(r.index, 10);
}

TEST(ListHitTest, LastPageShowsOnlyRemainingItems) {
  // 25 items, page 2 (index 20-24) holds only 5 real rows even though the
  // page fits 10 -- a tap on row 6 of that page (y=185) must miss.
  const auto miss = listHitTest(0, 0, 100, 300, 25, 20, 30, 50, 185);
  EXPECT_FALSE(miss.hit);

  const auto hit = listHitTest(0, 0, 100, 300, 25, 20, 30, 50, 125);
  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.index, 24);
}

TEST(ListHitTest, ZeroItemCountNeverHits) { EXPECT_FALSE(listHitTest(0, 0, 100, 300, 0, 0, 30, 50, 5).hit); }

TEST(ListHitTest, ZeroOrNegativeRowHeightNeverHits) {
  EXPECT_FALSE(listHitTest(0, 0, 100, 300, 5, 0, 0, 50, 5).hit);
  EXPECT_FALSE(listHitTest(0, 0, 100, 300, 5, 0, -5, 50, 5).hit);
}

// Regression guard for the negative-selectedIndex sentinel some callers pass
// (e.g. FontDownloadActivity's tab-row index -1): integer division must
// still resolve to page 0, not a negative/garbage page start.
TEST(ListHitTest, NegativeSelectedIndexStillResolvesToFirstPage) {
  const auto r = listHitTest(0, 0, 100, 300, 5, -1, 30, 50, 5);
  EXPECT_TRUE(r.hit);
  EXPECT_EQ(r.index, 0);
}

TEST(ListHitTest, RectOffsetFromOriginIsHonored) {
  // rect starts at (40, 100), not the origin -- x/y bounds and the row math
  // must both be measured relative to rect.y, not absolute screen y.
  const auto miss = listHitTest(40, 100, 100, 300, 5, 0, 30, 50, 50);
  EXPECT_FALSE(miss.hit);

  const auto hit = listHitTest(40, 100, 100, 300, 5, 0, 30, 50, 165);
  EXPECT_TRUE(hit.hit);
  EXPECT_EQ(hit.index, 2);
}
