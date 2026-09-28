#include <gtest/gtest.h>

#include <set>
#include <string>

#include "OpdsCoverCachePath.h"

// OpdsCoverCachePath::build() formats into a fixed stack buffer instead of chaining
// operator+ across seven temporaries, because it runs three times per cover on a heap
// an OPDS catalogue walk has already fragmented (see the header). The hazard a fixed
// buffer introduces is silent truncation: two different covers colliding on one cache
// file, or a path that no longer ends in ".bmp" and so never validates. Everything
// below is about that.
//
// std::hash<std::string> is not portable across builds (64-bit here, 32-bit on the
// ESP32-C3), so nothing here asserts a literal hash value -- only the shape of the
// path and the properties the cache depends on.

namespace {

constexpr int W = 160;
constexpr int H = 240;

TEST(OpdsCoverCachePath, HasDirectoryPrefixAndBmpSuffix) {
  const std::string path = OpdsCoverCachePath::build("urn:uuid:1234", W, H);
  EXPECT_EQ(path.rfind(std::string(OpdsCoverCachePath::DIR) + "/", 0), 0u);
  ASSERT_GE(path.size(), 4u);
  EXPECT_EQ(path.substr(path.size() - 4), ".bmp");
}

TEST(OpdsCoverCachePath, EncodesTheRenderedSize) {
  const std::string path = OpdsCoverCachePath::build("urn:uuid:1234", W, H);
  EXPECT_NE(path.find("_160x240.bmp"), std::string::npos);
}

TEST(OpdsCoverCachePath, NeverTruncates) {
  // The longest realistic inputs: a long entry id (the hash is fixed-width, so id
  // length cannot grow the path) and dimensions far beyond any real panel.
  const std::string longId(4096, 'x');
  const std::string path = OpdsCoverCachePath::build(longId, 2147483647, -2147483647);
  EXPECT_LT(path.size(), OpdsCoverCachePath::MAX_LEN);
  EXPECT_EQ(path.substr(path.size() - 4), ".bmp");
  EXPECT_NE(path.find("_2147483647x-2147483647.bmp"), std::string::npos);
}

TEST(OpdsCoverCachePath, DistinctSizesGetDistinctFiles) {
  // A cover is cached per rendered size; an orientation change must not read back the
  // previous orientation's bitmap.
  std::set<std::string> paths;
  paths.insert(OpdsCoverCachePath::build("book-1", 160, 240));
  paths.insert(OpdsCoverCachePath::build("book-1", 240, 160));
  paths.insert(OpdsCoverCachePath::build("book-1", 160, 241));
  EXPECT_EQ(paths.size(), 3u);
}

TEST(OpdsCoverCachePath, DistinctEntriesGetDistinctFiles) {
  EXPECT_NE(OpdsCoverCachePath::build("book-1", W, H), OpdsCoverCachePath::build("book-2", W, H));
}

TEST(OpdsCoverCachePath, IsStableForTheSameEntry) {
  // The three call sites per cover (pre-scan, fetch loop, ensureOpdsCoverCached) must
  // agree, or a cached cover is re-downloaded forever.
  EXPECT_EQ(OpdsCoverCachePath::build("urn:uuid:abc", W, H), OpdsCoverCachePath::build("urn:uuid:abc", W, H));
}

TEST(OpdsCoverCachePath, EmptyEntryIdStillYieldsAValidPath) {
  const std::string path = OpdsCoverCachePath::build("", W, H);
  EXPECT_EQ(path.rfind(std::string(OpdsCoverCachePath::DIR) + "/", 0), 0u);
  EXPECT_EQ(path.substr(path.size() - 4), ".bmp");
}

}  // namespace
