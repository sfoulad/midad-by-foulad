// The hostname a pinned TLS request is verified against comes from exactly one
// place: parseHttpUrl (src/network/HttpUrlParts.h). These tests are the negative
// half of "hostname verification is on" -- a verifier told the wrong name checks
// the wrong thing, however correct the rest of the handshake is.

#include <gtest/gtest.h>

#include "HttpUrlParts.h"

namespace {

HttpUrlParts parse(const char* url) {
  HttpUrlParts parts;
  EXPECT_TRUE(parseHttpUrl(url, parts)) << url;
  return parts;
}

TEST(HttpUrlParts, SplitsTheProductionPairingEndpoints) {
  const HttpUrlParts start = parse("https://api.midad.one/device-login/start");
  EXPECT_EQ(start.host, "api.midad.one");
  EXPECT_EQ(start.path, "/device-login/start");
  EXPECT_EQ(start.port, 443);
  EXPECT_TRUE(start.https);

  const HttpUrlParts poll = parse("https://api.midad.one/device-login/poll");
  EXPECT_EQ(poll.host, "api.midad.one");
  EXPECT_EQ(poll.path, "/device-login/poll");
}

TEST(HttpUrlParts, KeepsQueryAndSuppliesAPathWhenThereIsNone) {
  EXPECT_EQ(parse("https://api.midad.one").path, "/");
  EXPECT_EQ(parse("https://api.midad.one?a=b").path, "/?a=b");
  EXPECT_EQ(parse("https://api.midad.one?a=b").host, "api.midad.one");
  EXPECT_EQ(parse("https://api.midad.one/x?a=b#c").path, "/x?a=b#c");
}

TEST(HttpUrlParts, HonoursExplicitPortsAndSchemeDefaults) {
  EXPECT_EQ(parse("https://api.midad.one:8443/x").port, 8443);
  EXPECT_EQ(parse("https://api.midad.one:8443/x").host, "api.midad.one");
  EXPECT_EQ(parse("http://example.test/x").port, 80);
  EXPECT_FALSE(parse("http://example.test/x").https);
  EXPECT_TRUE(parse("HTTPS://api.midad.one/x").https);  // schemes are case-insensitive
}

// The attack this parser exists to defeat: a URL that READS as the pinned host
// but is a request to somewhere else. The name handed to the certificate check
// must be the authority's real host, so the handshake fails instead of
// succeeding against an attacker's certificate.
TEST(HttpUrlParts, UserinfoNeverBecomesTheVerifiedHost) {
  EXPECT_EQ(parse("https://api.midad.one@evil.example/device-login/start").host, "evil.example");
  EXPECT_EQ(parse("https://api.midad.one:token@evil.example/x").host, "evil.example");
  EXPECT_EQ(parse("https://a@b@evil.example/x").host, "evil.example");
  EXPECT_EQ(parse("https://api.midad.one@evil.example:8443/x").port, 8443);
}

TEST(HttpUrlParts, LookalikeHostsAreNotTheProductionHost) {
  EXPECT_EQ(parse("https://api.midad.one.evil.example/x").host, "api.midad.one.evil.example");
  EXPECT_EQ(parse("https://api-midad.one/x").host, "api-midad.one");
  EXPECT_EQ(parse("https://evil.example/api.midad.one/start").host, "evil.example");
}

TEST(HttpUrlParts, RejectsWhatItCannotSplitSafely) {
  HttpUrlParts parts;
  EXPECT_FALSE(parseHttpUrl("api.midad.one/device-login/start", parts));  // no scheme
  EXPECT_FALSE(parseHttpUrl("ftp://api.midad.one/x", parts));
  EXPECT_FALSE(parseHttpUrl("https:///device-login/start", parts));  // empty authority
  EXPECT_FALSE(parseHttpUrl("https://api.midad.one:notaport/x", parts));
  EXPECT_FALSE(parseHttpUrl("https://api.midad.one:99999/x", parts));
  EXPECT_FALSE(parseHttpUrl("https://api.midad.one:0/x", parts));
  EXPECT_FALSE(parseHttpUrl("https://api midad.one/x", parts));
  EXPECT_FALSE(parseHttpUrl("", parts));
}

TEST(HttpUrlParts, StripsIpv6Brackets) {
  const HttpUrlParts local = parse("https://[2001:db8::1]:8443/x");
  EXPECT_EQ(local.host, "2001:db8::1");
  EXPECT_EQ(local.port, 8443);
  EXPECT_EQ(parse("https://[2001:db8::1]/x").port, 443);
}

}  // namespace
