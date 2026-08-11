#include <gtest/gtest.h>

#include "FouladEbooksConfig.h"
#include "FouladOpdsHooksPure.h"

TEST(ExtractFouladBookId, ValidBookIdReturnsTrailingDigits) {
  EXPECT_EQ(FouladOpdsHooks::extractFouladBookId("urn:opds-library:book:12345"), "12345");
}

TEST(ExtractFouladBookId, NewsEntryIdIsRejected) {
  // Different namespace ("urn:midad:feed:<id>") -- must NOT be parsed as a
  // book id, or a news article silently overwrites a real book's reading
  // position (see the function's own comment).
  EXPECT_EQ(FouladOpdsHooks::extractFouladBookId("urn:midad:feed:3"), "");
}

TEST(ExtractFouladBookId, NonDigitTailIsRejected) {
  EXPECT_EQ(FouladOpdsHooks::extractFouladBookId("urn:opds-library:book:abc"), "");
}

TEST(ExtractFouladBookId, EmptyTailIsRejected) {
  EXPECT_EQ(FouladOpdsHooks::extractFouladBookId("urn:opds-library:book:"), "");
}

TEST(ExtractFouladBookId, UnrelatedPrefixIsRejected) {
  EXPECT_EQ(FouladOpdsHooks::extractFouladBookId("some-other-catalog-id"), "");
}

TEST(ExtractFouladBookId, EmptyStringIsRejected) { EXPECT_EQ(FouladOpdsHooks::extractFouladBookId(""), ""); }

TEST(AcquisitionExtension, XtchMapsToXtchExtension) {
  EXPECT_EQ(FouladOpdsHooks::acquisitionExtension("application/x-xtch"), ".xtch");
}

TEST(AcquisitionExtension, XtcMapsToXtcExtension) {
  EXPECT_EQ(FouladOpdsHooks::acquisitionExtension("application/x-xtc"), ".xtc");
}

TEST(AcquisitionExtension, AnythingElseDefaultsToEpub) {
  EXPECT_EQ(FouladOpdsHooks::acquisitionExtension("application/epub+zip"), ".epub");
  EXPECT_EQ(FouladOpdsHooks::acquisitionExtension(""), ".epub");
}

TEST(IsNewsFeed, MatchesTheNewsUrlExactly) { EXPECT_TRUE(FouladOpdsHooks::isNewsFeed(FOULAD_EBOOKS_NEWS_URL)); }

TEST(IsNewsFeed, DoesNotMatchTheCatalogUrl) { EXPECT_FALSE(FouladOpdsHooks::isNewsFeed(FOULAD_EBOOKS_URL)); }

TEST(IsNewsFeed, DoesNotMatchAnUnrelatedServer) {
  EXPECT_FALSE(FouladOpdsHooks::isNewsFeed("https://example.com/opds"));
}
