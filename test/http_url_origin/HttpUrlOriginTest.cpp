#include <gtest/gtest.h>

#include "UrlOrigin.h"

// Regression coverage for the origin-comparison logic HttpDownloader's
// redirect loop uses to decide whether to strip Authorization/X-Device-Serial
// headers. Exercised on real hardware as the OTA GitHub -> release-CDN hop
// (see docs/ota-signing-key-management.md and the redirect loop in
// src/network/HttpDownloader.cpp) -- a real device hit a TLS handshake
// failure immediately after this code ran, traced to heap fragmentation from
// an extra allocation this function's std::string-returning predecessor
// caused on every redirect hop. urlOrigin() was moved out of
// HttpDownloader.cpp into this header specifically so its behavior can be
// pinned down independent of ESP-IDF/Arduino, matching a std::string-based
// reference implementation kept here to guarantee refactor-safety.

namespace {
// The exact behavior urlOrigin() replaced: a std::string-allocating version.
// Kept here, not in production code, purely so every case below can assert
// both implementations agree -- if this ever drifts from urlOrigin(), the
// heap-free rewrite silently changed origin-comparison semantics.
std::string referenceUrlOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return url;
  const size_t pathStart = url.find_first_of("/?#", schemeEnd + 3);
  return pathStart == std::string::npos ? url : url.substr(0, pathStart);
}
}  // namespace

TEST(UrlOrigin, ExtractsSchemeHostFromPathUrl) {
  EXPECT_EQ(urlOrigin("https://github.com/sfoulad/midad-by-foulad/releases/download/v1/firmware.bin"),
            "https://github.com");
}

TEST(UrlOrigin, ExtractsSchemeHostFromCdnRedirectTarget) {
  // The actual redirect target class that failed on real hardware: GitHub's
  // release asset CDN, a different host than github.com.
  EXPECT_EQ(urlOrigin("https://objects.githubusercontent.com/github-production-release-asset/1/firmware.bin?X-Amz="
                      "abc"),
            "https://objects.githubusercontent.com");
}

TEST(UrlOrigin, KeepsPortInOrigin) {
  EXPECT_EQ(urlOrigin("http://192.168.1.50:8080/opds/root.xml"), "http://192.168.1.50:8080");
}

TEST(UrlOrigin, NoPathStopsAtQueryString) {
  // A URL with no '/' at all after the scheme -- stopping at '/' alone would
  // fold the query string into the "origin" and misclassify a same-origin
  // redirect as cross-origin. This is the exact edge case the function's own
  // header comment calls out.
  EXPECT_EQ(urlOrigin("https://host?key=value"), "https://host");
}

TEST(UrlOrigin, NoPathStopsAtFragment) { EXPECT_EQ(urlOrigin("https://host#section"), "https://host"); }

TEST(UrlOrigin, BareOriginUnchanged) { EXPECT_EQ(urlOrigin("https://midad.one"), "https://midad.one"); }

TEST(UrlOrigin, MissingSchemeReturnsWholeInput) { EXPECT_EQ(urlOrigin("not-a-url"), "not-a-url"); }

TEST(UrlOrigin, SameOriginDifferentPathsCompareEqual) {
  EXPECT_EQ(urlOrigin("https://midad.one/opds/a.xml"), urlOrigin("https://midad.one/opds/b.xml"));
}

TEST(UrlOrigin, CrossOriginComparesUnequal) {
  EXPECT_NE(urlOrigin("https://github.com/owner/repo/releases/download/v1/firmware.bin"),
            urlOrigin("https://objects.githubusercontent.com/github-production-release-asset/1/firmware.bin"));
}

// Refactor-safety: the heap-free std::string_view implementation must never
// disagree with the std::string-returning implementation it replaced, across
// every case above plus a few more edge shapes.
TEST(UrlOrigin, MatchesReferenceImplementationAcrossShapes) {
  const char* const cases[] = {
      "https://github.com/sfoulad/midad-by-foulad/releases/download/v1/firmware.bin",
      "https://objects.githubusercontent.com/github-production-release-asset/1/firmware.bin?X-Amz=abc",
      "http://192.168.1.50:8080/opds/root.xml",
      "https://host?key=value",
      "https://host#section",
      "https://midad.one",
      "not-a-url",
      "",
      "https://",
  };
  for (const char* const c : cases) {
    EXPECT_EQ(std::string(urlOrigin(c)), referenceUrlOrigin(c)) << "mismatch for input: " << c;
  }
}

// urlIsHttps() is the plaintext gate on HttpDownloader::fetchUrlVerified: a
// URL it rejects never reaches a socket on the verified path. The negative
// cases below are the "HTTPS -> HTTP downgrade fails at the front door" half
// of the OTA transport's negative security tests (the in-flight redirect
// downgrade is refused separately by runGetWolf/esp_http_client).
TEST(UrlIsHttps, AcceptsHttpsAnyCase) {
  EXPECT_TRUE(urlIsHttps("https://api.github.com/repos/x/y/releases/latest"));
  EXPECT_TRUE(urlIsHttps("HTTPS://api.github.com/"));
  EXPECT_TRUE(urlIsHttps("HttpS://release-assets.githubusercontent.com/f.bin"));
}

TEST(UrlIsHttps, RejectsEveryNonHttpsShape) {
  EXPECT_FALSE(urlIsHttps("http://api.github.com/"));    // the downgrade itself
  EXPECT_FALSE(urlIsHttps("HTTP://api.github.com/"));    // case-insensitively
  EXPECT_FALSE(urlIsHttps("ftp://api.github.com/"));     // other scheme
  EXPECT_FALSE(urlIsHttps("xhttps://api.github.com/"));  // prefix impostor
  EXPECT_FALSE(urlIsHttps("https:/api.github.com/"));    // malformed separator
  EXPECT_FALSE(urlIsHttps("https//api.github.com/"));    // missing colon
  EXPECT_FALSE(urlIsHttps(" https://api.github.com/"));  // leading whitespace
  EXPECT_FALSE(urlIsHttps("api.github.com/https://"));   // scheme not at start
  EXPECT_FALSE(urlIsHttps(""));                          // empty
  EXPECT_FALSE(urlIsHttps("https:"));                    // truncated
}
