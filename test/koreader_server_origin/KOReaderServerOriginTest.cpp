#include <gtest/gtest.h>

#include "KOReaderServerOrigin.h"

using koreader_sync::isMidadSyncOrigin;

// Midad does not serve the KOSync protocol, and this client would otherwise send
// a username plus an unsalted MD5 password over an unverified connection. These
// tests pin the refusal predicate: a false negative reintroduces that exposure.

TEST(KOReaderServerOrigin, RecognisesEveryMidadHost) {
  EXPECT_TRUE(isMidadSyncOrigin("https://midad.one"));
  EXPECT_TRUE(isMidadSyncOrigin("https://api.midad.one"));
  EXPECT_TRUE(isMidadSyncOrigin("https://foulad.one"));
}

TEST(KOReaderServerOrigin, IgnoresSchemePathQueryAndFragment) {
  EXPECT_TRUE(isMidadSyncOrigin("http://midad.one/users/auth"));
  EXPECT_TRUE(isMidadSyncOrigin("https://midad.one/syncs/progress?x=1"));
  EXPECT_TRUE(isMidadSyncOrigin("https://midad.one#frag"));
}

TEST(KOReaderServerOrigin, StripsUserinfoAndPort) {
  // The host is not what a naive reader of the URL sees.
  EXPECT_TRUE(isMidadSyncOrigin("https://user:pass@midad.one:443/x"));
  EXPECT_TRUE(isMidadSyncOrigin("https://midad.one:8443"));
}

TEST(KOReaderServerOrigin, IsCaseInsensitive) {
  EXPECT_TRUE(isMidadSyncOrigin("https://MIDAD.ONE"));
  EXPECT_TRUE(isMidadSyncOrigin("https://Api.Midad.One/x"));
}

TEST(KOReaderServerOrigin, NeverMatchesOnSubstring) {
  // The whole point of a whole-host compare.
  EXPECT_FALSE(isMidadSyncOrigin("https://midad.one.attacker.tld"));
  EXPECT_FALSE(isMidadSyncOrigin("https://notmidad.one"));
  EXPECT_FALSE(isMidadSyncOrigin("https://api.midad.one.evil.example"));
  EXPECT_FALSE(isMidadSyncOrigin("https://evil.example/?next=https://midad.one"));
  // Userinfo that merely *contains* a Midad host must not match either.
  EXPECT_FALSE(isMidadSyncOrigin("https://midad.one@evil.example/x"));
}

TEST(KOReaderServerOrigin, LeavesThirdPartySyncServersAlone) {
  // Third-party/self-hosted sync stays a separate policy question: these must
  // keep working exactly as before.
  EXPECT_FALSE(isMidadSyncOrigin("https://sync.crosspointreader.com"));
  EXPECT_FALSE(isMidadSyncOrigin("https://sync.koreader.rocks:443"));
  EXPECT_FALSE(isMidadSyncOrigin("http://192.168.1.10:17200"));
}

TEST(KOReaderServerOrigin, RejectsMalformedInputRatherThanGuessing) {
  EXPECT_FALSE(isMidadSyncOrigin(""));
  EXPECT_FALSE(isMidadSyncOrigin("midad.one"));  // no scheme: getBaseUrl() always supplies one
}
