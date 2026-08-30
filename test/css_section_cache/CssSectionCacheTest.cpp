// Section-cache invalidation, in a Midad-owned test target.
//
// These cases belong to the upstream fix on fix/css-cache-section-invalidation,
// where they live in test/css_parser/CssParserTest.cpp. They are kept in a
// separate target here so this RC leaves that upstream-owned test file
// byte-identical and the thin-fork guard passes without an exemption.

#include <gtest/gtest.h>

#include "CssParser.h"

namespace {

using CacheStatus = CssParser::CacheStatus;
using CacheLoadResult = CssParser::CacheLoadResult;
using ParseResult = CssParser::ParseResult;

TEST(CssSectionCacheInvalidation, HydratedCompleteCacheKeepsSections) {
  for (const CacheStatus status :
       {CacheStatus::Missing, CacheStatus::Complete, CacheStatus::Partial, CacheStatus::Invalid}) {
    SCOPED_TRACE(static_cast<int>(status));
    EXPECT_FALSE(CssParser::sectionCacheIsStale(status, CacheLoadResult::Complete, ParseResult::Error));
    EXPECT_FALSE(CssParser::sectionCacheIsStale(status, CacheLoadResult::Complete, ParseResult::Complete));
  }
}

TEST(CssSectionCacheInvalidation, LowMemoryRetryKeepsSections) {
  // The cache file is left untouched for a later retry, so the sections built from it
  // are still consistent with what is on disk.
  for (const CacheStatus status :
       {CacheStatus::Missing, CacheStatus::Complete, CacheStatus::Partial, CacheStatus::Invalid}) {
    SCOPED_TRACE(static_cast<int>(status));
    EXPECT_FALSE(CssParser::sectionCacheIsStale(status, CacheLoadResult::LowMemory, ParseResult::Error));
  }
}

TEST(CssSectionCacheInvalidation, CompleteReparseDropsSections) {
  for (const CacheStatus status :
       {CacheStatus::Missing, CacheStatus::Complete, CacheStatus::Partial, CacheStatus::Invalid}) {
    SCOPED_TRACE(static_cast<int>(status));
    EXPECT_TRUE(CssParser::sectionCacheIsStale(status, CacheLoadResult::Invalid, ParseResult::Complete));
  }
}

TEST(CssSectionCacheInvalidation, PartialReparseDropsSectionsUnlessCacheWasAlreadyPartial) {
  EXPECT_TRUE(CssParser::sectionCacheIsStale(CacheStatus::Missing, CacheLoadResult::Invalid, ParseResult::Partial));
  EXPECT_TRUE(CssParser::sectionCacheIsStale(CacheStatus::Invalid, CacheLoadResult::Invalid, ParseResult::Partial));
  EXPECT_TRUE(CssParser::sectionCacheIsStale(CacheStatus::Complete, CacheLoadResult::Invalid, ParseResult::Partial));
  // parseCssFiles() preserves an existing partial cache rather than writing a shorter one.
  EXPECT_FALSE(CssParser::sectionCacheIsStale(CacheStatus::Partial, CacheLoadResult::Invalid, ParseResult::Partial));
}

TEST(CssSectionCacheInvalidation, FailedReparseDropsSectionsWhenTheOldRuleSetWasDeleted) {
  // Version bump path: inspectCache() reports Invalid, deleteCache() removes the old file,
  // then the reparse fails (an SD hiccup, or free heap under MIN_HEAP_FOR_CSS_PARSING).
  // Sections laid out against the deleted rule set must not survive.
  EXPECT_TRUE(CssParser::sectionCacheIsStale(CacheStatus::Invalid, CacheLoadResult::Invalid, ParseResult::Error));
  // A Complete header that failed to hydrate is deleted the same way.
  EXPECT_TRUE(CssParser::sectionCacheIsStale(CacheStatus::Complete, CacheLoadResult::Invalid, ParseResult::Error));
}

TEST(CssSectionCacheInvalidation, FailedReparseKeepsSectionsWhenNothingWasDeleted) {
  // Nothing was on disk and nothing was written: no rule set changed hands, so rebuilding
  // sections on every open of a book with no parseable CSS would be pure churn.
  EXPECT_FALSE(CssParser::sectionCacheIsStale(CacheStatus::Missing, CacheLoadResult::Invalid, ParseResult::Error));
  // A partial cache is preserved for a later retry.
  EXPECT_FALSE(CssParser::sectionCacheIsStale(CacheStatus::Partial, CacheLoadResult::Invalid, ParseResult::Error));
}

}  // namespace
