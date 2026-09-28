#include <gtest/gtest.h>

#include <string_view>

#include "FouladEbooksConfig.h"

// isFouladEbooksUrl() decides whether a request may carry this device's serial
// header, and it is the only thing standing between "remote removal works" and a
// failure with no symptom: a host it does not recognise still fetches fine, it
// just silently stops identifying the device. Midad Core lives on its own
// hostname (api.midad.one) rather than a path under the site, so the check has to
// know about it explicitly -- and it must keep recognising the two older hosts,
// because a stored catalog URL is only rewritten once OpdsServerStore::fromJson
// has recognised it as ours.
//
// Header-only and constexpr, so this needs nothing but <string_view>.

namespace {

// Absolute hrefs are the norm on this surface, not the exception: Core's root
// feed, its pagination links and its cover/acquisition links are all absolute, so
// every one of these strings is a URL the device really follows.
constexpr std::string_view CORE_URLS[] = {
    "https://api.midad.one/opds",
    "https://api.midad.one/opds/library",
    "https://api.midad.one/opds/library?q=dune",
    "https://api.midad.one/opds/library?page=2",
    "https://api.midad.one/opds/library/collections",
    "https://api.midad.one/opds/library/search.xml",
    "https://api.midad.one/opds/store",
    "https://api.midad.one/opds/books/abc123/download",
    "https://api.midad.one/library/items/abc123/cover",
    "https://api.midad.one/device-login/start",
    "https://api.midad.one/device-login/poll",
    "https://api.midad.one/device-login/self-revoke",
};

}  // namespace

TEST(FouladCoreEndpoints, RecognisesEveryCoreSurface) {
  for (const auto url : CORE_URLS) {
    EXPECT_TRUE(isFouladEbooksUrl(url)) << url;
  }
}

TEST(FouladCoreEndpoints, StillRecognisesTheOlderHosts) {
  // A device paired before Core has the old URL on its SD card and keeps using it
  // until the store migration rewrites it -- which only happens for a URL this
  // function already accepts.
  EXPECT_TRUE(isFouladEbooksUrl("http://midad.one/opds"));
  EXPECT_TRUE(isFouladEbooksUrl("https://midad.one/opds/books/1/download"));
  EXPECT_TRUE(isFouladEbooksUrl("http://foulad.one/opds"));
}

TEST(FouladCoreEndpoints, HostMatchIsWholeHostNotSubstring) {
  // The failure this guards against is a lookalike host collecting the device
  // serial. Matching must be on the whole hostname, both directions.
  EXPECT_FALSE(isFouladEbooksUrl("https://api.midad.one.attacker.example/opds"));
  EXPECT_FALSE(isFouladEbooksUrl("https://notapi.midad.one/opds"));
  EXPECT_FALSE(isFouladEbooksUrl("https://midad.one.attacker.example/opds"));
  EXPECT_FALSE(isFouladEbooksUrl("https://evil.example/api.midad.one/opds"));
  // Userinfo must not be able to smuggle our host past the check.
  EXPECT_FALSE(isFouladEbooksUrl("https://api.midad.one@evil.example/opds"));
}

TEST(FouladCoreEndpoints, ThirdPartyOpdsServersNeverGetTheSerial) {
  EXPECT_FALSE(isFouladEbooksUrl("https://opds.example.org/catalog"));
  EXPECT_FALSE(isFouladEbooksUrl("http://192.168.1.10:8080/opds"));
  EXPECT_FALSE(isFouladEbooksUrl("not-a-url"));
  EXPECT_FALSE(isFouladEbooksUrl(""));
}

TEST(FouladCoreEndpoints, HostCompareIsCaseInsensitive) {
  EXPECT_TRUE(isFouladEbooksUrl("https://API.MIDAD.ONE/opds"));
  EXPECT_TRUE(isFouladEbooksUrl("https://Api.Midad.One:443/opds/library"));
}

TEST(FouladCoreEndpoints, EndpointsPointAtCoreOverHttps) {
  // Core is HTTPS-only and answers no plain-http port; a POST would not follow
  // the redirect that answered it anyway.
  for (const auto* url : {FOULAD_EBOOKS_URL, FOULAD_EBOOKS_DEVICE_LOGIN_START_URL, FOULAD_EBOOKS_DEVICE_LOGIN_POLL_URL,
                          FOULAD_EBOOKS_DEVICE_SIGNOUT_URL}) {
    const std::string_view view{url};
    EXPECT_EQ(view.rfind("https://api.midad.one/", 0), 0u) << url;
    EXPECT_TRUE(isFouladEbooksUrl(view)) << url;
  }
}

TEST(FouladCoreEndpoints, DeviceLoginPathsCarryNoApiPrefix) {
  // Core mounts the pairing router at the root. The legacy server's "/api"
  // prefix here answered 404, which the poll loop could not tell from a dead
  // session.
  EXPECT_STREQ(FOULAD_EBOOKS_DEVICE_LOGIN_START_URL, "https://api.midad.one/device-login/start");
  EXPECT_STREQ(FOULAD_EBOOKS_DEVICE_LOGIN_POLL_URL, "https://api.midad.one/device-login/poll");
  // "Remove me", naming no target: Core revokes whichever token authenticated it.
  EXPECT_STREQ(FOULAD_EBOOKS_DEVICE_SIGNOUT_URL, "https://api.midad.one/device-login/self-revoke");
}

TEST(FouladCoreEndpoints, CatalogRootIsTheOpdsRoot) {
  // Both feeds hang off this one root: anonymous it is the public Store, and with
  // a device credential it is the navigation feed offering My Library, the Store
  // and (when the reader has any) Shelves.
  EXPECT_STREQ(FOULAD_EBOOKS_URL, "https://api.midad.one/opds");
}
