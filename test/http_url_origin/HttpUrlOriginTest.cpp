#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "HttpVerifiedFetch.h"
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

// --- RedirectOriginGuard -----------------------------------------------------
//
// The rule that keeps a device token off a third party's server when a cover or
// download link redirects. runGetWolf() asks credentialsAllowed() before EVERY
// request in a chain and sets or clears Basic auth from the answer, so these
// cases are the security boundary itself, not a helper's convenience.

namespace {

// Walks a chain the way runGetWolf() does -- ask before each request, advance on
// each redirect -- and reports whether credentials went out on the final request.
bool credentialsSurviveChain(const std::string& start, const std::vector<std::string>& hops) {
  RedirectOriginGuard guard(start);
  bool allowedOnLastRequest = guard.credentialsAllowed();
  for (const std::string& hop : hops) {
    guard.onHop(hop);
    allowedOnLastRequest = guard.credentialsAllowed();
  }
  return allowedOnLastRequest;
}

}  // namespace

TEST(RedirectOriginGuard, AllowsCredentialsOnTheFirstRequest) {
  const RedirectOriginGuard guard("https://api.midad.one/opds/cover/1.jpg");
  EXPECT_TRUE(guard.credentialsAllowed());
  EXPECT_FALSE(guard.leftStartOrigin());
}

TEST(RedirectOriginGuard, SameOriginRedirectKeepsCredentials) {
  // A signed cover URL that redirects within the catalogue's own origin is the
  // case keep-alive exists for; stripping here would break authenticated covers.
  EXPECT_TRUE(credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg",
                                      {"https://api.midad.one/media/signed/1.jpg?sig=abc"}));
}

TEST(RedirectOriginGuard, DifferentHostDropsCredentials) {
  // The R2/CDN shape: the token must not reach object storage.
  EXPECT_FALSE(credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg",
                                       {"https://pub-example.r2.dev/covers/1.jpg?X-Amz-Signature=abc"}));
}

TEST(RedirectOriginGuard, SubdomainChangeDropsCredentials) {
  EXPECT_FALSE(
      credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg", {"https://cdn.midad.one/covers/1.jpg"}));
}

TEST(RedirectOriginGuard, LegacyHostRedirectDropsCredentials) {
  // foulad.one redirects to midad.one server-side for devices paired before the
  // rename (FouladEbooksConfig.h) -- a real cross-origin hop this firmware takes.
  EXPECT_FALSE(credentialsSurviveChain("http://foulad.one/opds/cover/1.jpg", {"http://midad.one/opds/cover/1.jpg"}));
}

TEST(RedirectOriginGuard, SchemeDowngradeDropsCredentialsOnTheSameHost) {
  // Same host, different scheme: still a different origin. Sending Basic auth
  // over the http hop would put the token on the wire in clear.
  EXPECT_FALSE(credentialsSurviveChain("https://midad.one/opds/cover/1.jpg", {"http://midad.one/opds/cover/1.jpg"}));
}

TEST(RedirectOriginGuard, PortChangeDropsCredentialsOnTheSameHost) {
  EXPECT_FALSE(credentialsSurviveChain("http://192.168.1.50:8080/opds/cover/1.jpg",
                                       {"http://192.168.1.50:9090/opds/cover/1.jpg"}));
}

TEST(RedirectOriginGuard, ReturningToTheStartOriginDoesNotRestoreCredentials) {
  // The latch. A server that can redirect can redirect A -> B -> A; if the guard
  // reset on the way back, the token would ride the final hop after an attacker
  // had already had the chain in hand.
  EXPECT_FALSE(credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg",
                                       {"https://evil.example/x", "https://api.midad.one/opds/cover/1.jpg"}));
}

TEST(RedirectOriginGuard, LatchSurvivesManyLaterSameOriginHops) {
  EXPECT_FALSE(credentialsSurviveChain(
      "https://api.midad.one/a",
      {"https://evil.example/x", "https://api.midad.one/b", "https://api.midad.one/c", "https://api.midad.one/d"}));
}

TEST(RedirectOriginGuard, PathOnlyRedirectKeepsCredentials) {
  EXPECT_TRUE(credentialsSurviveChain("https://api.midad.one/opds/root.xml",
                                      {"https://api.midad.one/opds/v2/root.xml", "https://api.midad.one/opds/v2/a"}));
}

TEST(RedirectOriginGuard, QueryOnlyRedirectKeepsCredentials) {
  // urlOrigin() stops at '?', so a signature appended to the same origin is not
  // mistaken for a cross-origin hop.
  EXPECT_TRUE(credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg",
                                      {"https://api.midad.one/opds/cover/1.jpg?sig=abc"}));
}

TEST(RedirectOriginGuard, DefaultPortSpellingFailsSafe) {
  // "https://h" and "https://h:443" are the same origin in principle but differ
  // textually. Documented behaviour: strip. The failure direction must always be
  // an unnecessary strip, never an unintended send.
  EXPECT_FALSE(credentialsSurviveChain("https://api.midad.one/a", {"https://api.midad.one:443/a"}));
}

TEST(RedirectOriginGuard, LeftStartOriginTracksTheLatch) {
  RedirectOriginGuard guard("https://api.midad.one/a");
  EXPECT_FALSE(guard.leftStartOrigin());
  guard.onHop("https://api.midad.one/b");
  EXPECT_FALSE(guard.leftStartOrigin());
  guard.onHop("https://elsewhere.example/c");
  EXPECT_TRUE(guard.leftStartOrigin());
  guard.onHop("https://api.midad.one/d");
  EXPECT_TRUE(guard.leftStartOrigin());
}

// --- redirect admission control ----------------------------------------------
//
// Every case here is a shape runGetWolf() must REFUSE outright rather than follow
// with credentials stripped. Following a refused redirect would still fetch the
// body over whatever transport the server named, which for a downgrade means
// cleartext.

TEST(RedirectAdmission, PlainRelativeLocationIsFollowed) {
  EXPECT_EQ(checkRedirectLocation("/opds/v2/root.xml"), RedirectVerdict::Follow);
  EXPECT_EQ(checkRedirectLocation("cover-2.jpg"), RedirectVerdict::Follow);
  EXPECT_EQ(checkRedirectLocation("?sig=abc"), RedirectVerdict::Follow);
}

TEST(RedirectAdmission, AbsoluteHttpAndHttpsLocationsAreFollowed) {
  EXPECT_EQ(checkRedirectLocation("https://cdn.midad.one/covers/1.jpg"), RedirectVerdict::Follow);
  EXPECT_EQ(checkRedirectLocation("http://192.168.1.50:8080/opds/root.xml"), RedirectVerdict::Follow);
  // Scheme comparison is case-insensitive (RFC 3986 3.1).
  EXPECT_EQ(checkRedirectLocation("HTTPS://cdn.midad.one/covers/1.jpg"), RedirectVerdict::Follow);
}

TEST(RedirectAdmission, EmptyOrWhitespaceLocationIsRejected) {
  EXPECT_EQ(checkRedirectLocation(""), RedirectVerdict::RejectEmptyLocation);
  EXPECT_EQ(checkRedirectLocation("   "), RedirectVerdict::RejectEmptyLocation);
  EXPECT_EQ(checkRedirectLocation("\r\n\t "), RedirectVerdict::RejectEmptyLocation);
}

TEST(RedirectAdmission, ProtocolRelativeLocationIsRejected) {
  // RFC 3986 says "//evil.example/x" is a network-path reference inheriting the
  // scheme; SecureHttpClient::resolveUrl treats it as a path under the current
  // host. Rejecting removes the dependency on which reading applies.
  EXPECT_EQ(checkRedirectLocation("//evil.example/covers/1.jpg"), RedirectVerdict::RejectProtocolRelative);
  EXPECT_EQ(checkRedirectLocation("  //evil.example/x"), RedirectVerdict::RejectProtocolRelative);
}

TEST(RedirectAdmission, NonHttpSchemesAreRejected) {
  EXPECT_EQ(checkRedirectLocation("file:///etc/passwd"), RedirectVerdict::RejectUnsupportedScheme);
  EXPECT_EQ(checkRedirectLocation("data:text/html,<script>x</script>"), RedirectVerdict::RejectUnsupportedScheme);
  EXPECT_EQ(checkRedirectLocation("javascript:alert(1)"), RedirectVerdict::RejectUnsupportedScheme);
  EXPECT_EQ(checkRedirectLocation("mailto:someone@example.com"), RedirectVerdict::RejectUnsupportedScheme);
  EXPECT_EQ(checkRedirectLocation("ftp://files.example/x"), RedirectVerdict::RejectUnsupportedScheme);
}

TEST(RedirectAdmission, HttpSchemeWithoutAnAuthorityIsRejected) {
  // "http:/host/x" and "https:x" are not shapes a real server sends, and
  // resolveUrl() would mangle both into a path under the current origin.
  EXPECT_EQ(checkRedirectLocation("http:/evil.example/x"), RedirectVerdict::RejectUnsupportedScheme);
  EXPECT_EQ(checkRedirectLocation("https:cover.jpg"), RedirectVerdict::RejectUnsupportedScheme);
}

TEST(RedirectAdmission, UserinfoInAnAbsoluteLocationIsRejected) {
  // The shape this project has already been bitten by: reads as api.midad.one,
  // resolves to evil.example. See test/foulad_core_endpoints for the other half.
  EXPECT_EQ(checkRedirectLocation("https://api.midad.one@evil.example/opds"), RedirectVerdict::RejectUserinfo);
  EXPECT_EQ(checkRedirectLocation("https://user:pass@evil.example/covers/1.jpg"), RedirectVerdict::RejectUserinfo);
  EXPECT_EQ(checkRedirectLocation("https://api.midad.one@evil.example:443/opds"), RedirectVerdict::RejectUserinfo);
}

TEST(RedirectAdmission, UserinfoIsRejectedOnTheInitialRequestUrlToo) {
  // Cover hrefs arrive absolute from the feed, so this shape can reach the
  // downloader with no redirect involved at all.
  EXPECT_EQ(checkRequestUrl("https://api.midad.one@evil.example/cover.jpg"), RedirectVerdict::RejectUserinfo);
  EXPECT_EQ(checkRequestUrl("https://api.midad.one/cover.jpg"), RedirectVerdict::Follow);
}

TEST(RedirectAdmission, AnAtSignInThePathIsNotUserinfo) {
  // '@' is legal in a path or query; only the authority matters.
  EXPECT_EQ(checkRequestUrl("https://api.midad.one/opds/user@example/1.jpg"), RedirectVerdict::Follow);
  EXPECT_EQ(checkRequestUrl("https://api.midad.one/opds?to=user@example"), RedirectVerdict::Follow);
}

TEST(RedirectAdmission, MissingAuthorityIsRejected) {
  EXPECT_EQ(checkRequestUrl("https:///opds/root.xml"), RedirectVerdict::RejectUnsupportedScheme);
  EXPECT_EQ(checkRequestUrl("not-a-url"), RedirectVerdict::RejectUnsupportedScheme);
}

TEST(RedirectAdmission, HttpsToHttpDowngradeIsRejected) {
  EXPECT_EQ(checkRedirectTarget("https://api.midad.one/cover.jpg", "http://api.midad.one/cover.jpg"),
            RedirectVerdict::RejectDowngrade);
  // Cross-origin downgrade is refused for the same reason, not merely stripped.
  EXPECT_EQ(checkRedirectTarget("https://api.midad.one/cover.jpg", "http://evil.example/cover.jpg"),
            RedirectVerdict::RejectDowngrade);
  // Case-insensitive on both sides.
  EXPECT_EQ(checkRedirectTarget("HTTPS://api.midad.one/a", "HTTP://api.midad.one/a"), RedirectVerdict::RejectDowngrade);
}

TEST(RedirectAdmission, HttpToHttpChainIsAllowed) {
  // A plain-HTTP server was never confidential, so there is nothing to strip --
  // refusing it would break local/self-hosted OPDS and FOULAD_EBOOKS_NEWS_URL.
  EXPECT_EQ(checkRedirectTarget("http://192.168.1.50:8080/opds", "http://192.168.1.50:8080/opds/v2"),
            RedirectVerdict::Follow);
  EXPECT_EQ(checkRedirectTarget("http://foulad.one/opds", "http://midad.one/opds"), RedirectVerdict::Follow);
}

TEST(RedirectAdmission, HttpToHttpsUpgradeIsAllowed) {
  EXPECT_EQ(checkRedirectTarget("http://midad.one/opds", "https://api.midad.one/opds"), RedirectVerdict::Follow);
}

TEST(RedirectAdmission, HttpsToHttpsIsAllowedAcrossOrigins) {
  // Allowed to FOLLOW; RedirectOriginGuard is what strips credentials for it.
  EXPECT_EQ(checkRedirectTarget("https://api.midad.one/cover.jpg", "https://pub-example.r2.dev/1.jpg"),
            RedirectVerdict::Follow);
}

TEST(RedirectAdmission, UserinfoIsCheckedBeforeTheDowngradeRule) {
  // A target that is both malformed and a downgrade reports the malformation;
  // either way it is refused, this just pins the reported reason.
  EXPECT_EQ(checkRedirectTarget("https://api.midad.one/a", "http://api.midad.one@evil.example/a"),
            RedirectVerdict::RejectUserinfo);
}

TEST(RedirectAdmission, EveryVerdictHasADistinctDescription) {
  // The rejection reason is all that gets logged (never the URL), so it has to
  // actually distinguish the cases.
  const std::vector<RedirectVerdict> all = {
      RedirectVerdict::Follow,
      RedirectVerdict::RejectEmptyLocation,
      RedirectVerdict::RejectProtocolRelative,
      RedirectVerdict::RejectUnsupportedScheme,
      RedirectVerdict::RejectUserinfo,
      RedirectVerdict::RejectDowngrade,
  };
  std::set<std::string> descriptions;
  for (const RedirectVerdict verdict : all) {
    const char* text = describeRedirectVerdict(verdict);
    ASSERT_NE(text, nullptr);
    EXPECT_STRNE(text, "unknown");
    descriptions.insert(text);
  }
  EXPECT_EQ(descriptions.size(), all.size());
}

// urlIsHttps() is the plaintext gate on HttpDownloader::fetchUrlVerified: a
// URL it rejects never reaches a socket on the verified path. The negative
// cases below are the "HTTPS -> HTTP downgrade fails at the front door" half
// of the OTA transport's negative security tests (the in-flight redirect
// downgrade is refused separately by runGetWolf/esp_http_client). The
// function itself is covered by test/http_verified_fetch; the malformed
// spellings below are extra shapes that suite does not enumerate.
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
