// Host-side proof that the OTA trust anchors (src/network/OtaCaCerts.h) do
// what their header claims, including the NEGATIVE cases: the recorded live
// chains of api.github.com and release-assets.githubusercontent.com verify
// against the embedded anchors, an untrusted/wrong CA fails, and a hostname
// mismatch fails. The chains live as committed fixtures (captured 2026-08-30
// with `openssl s_client -showcerts`; verification time is pinned to the
// capture epoch so certificate expiry can never turn these tests flaky).
//
// X.509 path building and hostname matching are exercised through the openssl
// CLI: what these tests pin down is the CONTENT of the anchor set and of the
// recorded chains, not wolfSSL's verifier. The wolfSSL side of the same
// guarantees is code, not data -- SecureClient loads exactly this PEM
// (fail-closed on parse), keeps the default WOLFSSL_VERIFY_PEER, and arms
// wolfSSL_check_domain_name() for every non-insecure connection -- and its
// end-to-end behavior is the on-device half of the test plan. Skips (never
// passes) when no OpenSSL with the required flags is available; CI's ubuntu
// runner always has one.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "network/OtaCaCerts.h"

namespace {

// The fixture capture time: verification is evaluated as of this instant.
constexpr const char* kVerifyAtEpoch = "1788044180";  // 2026-08-30

std::string fixture(const std::string& name) { return std::string(OTA_CA_FIXTURES_DIR "/") + name; }

std::string shellQuote(const std::string& s) { return "'" + s + "'"; }

int runCommand(const std::string& cmd) {
  // Redirect both streams: the openssl error text is part of the expected
  // behavior in the negative cases, not test noise worth printing.
  const int rc = std::system((cmd + " >/dev/null 2>&1").c_str());
  return rc;
}

// An openssl whose `verify` knows -no-CApath/-no-CAstore (so the system trust
// store can't silently rescue a chain our anchors reject -- LibreSSL's verify,
// macOS's /usr/bin/openssl, lacks these and is deliberately not usable here),
// -attime, and -verify_hostname. Empty string when none qualifies.
std::string findOpenssl() {
  static std::string cached = [] {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("OTA_TEST_OPENSSL")) candidates.push_back(env);
    candidates.push_back("openssl");
    candidates.push_back("/opt/homebrew/opt/openssl@3/bin/openssl");
    candidates.push_back("/usr/local/opt/openssl@3/bin/openssl");
    for (const auto& c : candidates) {
      // `verify -help` exits nonzero on some builds; capability is judged from
      // the help text instead.
      const std::string probeCmd = shellQuote(c) + " verify -help 2>&1";
      FILE* pipe = popen(probeCmd.c_str(), "r");
      if (!pipe) continue;
      std::string help;
      char buf[512];
      while (fgets(buf, sizeof(buf), pipe)) help += buf;
      pclose(pipe);
      if (help.find("-no-CApath") != std::string::npos && help.find("-attime") != std::string::npos &&
          help.find("-verify_hostname") != std::string::npos && help.find("-no-CAstore") != std::string::npos) {
        return c;
      }
    }
    return std::string();
  }();
  return cached;
}

// Writes `pem` to a file under the test's temp dir and returns its path.
std::string writePem(const std::string& name, const std::string& pem) {
  const std::string path = testing::TempDir() + name;
  std::ofstream out(path, std::ios::trunc);
  out << pem;
  EXPECT_TRUE(out.good());
  return path;
}

// Splits the two concatenated anchors so the wrong-CA cases can verify each
// chain against only the OTHER chain's root.
std::string nthCert(const std::string& bundle, const size_t index) {
  constexpr char kBegin[] = "-----BEGIN CERTIFICATE-----";
  constexpr char kEnd[] = "-----END CERTIFICATE-----";
  size_t pos = 0;
  for (size_t i = 0; i <= index; i++) {
    pos = bundle.find(kBegin, pos);
    if (pos == std::string::npos) return "";
    if (i == index) {
      const size_t end = bundle.find(kEnd, pos);
      if (end == std::string::npos) return "";
      return bundle.substr(pos, end - pos + sizeof(kEnd) - 1) + "\n";
    }
    pos += sizeof(kBegin) - 1;
  }
  return "";
}

// openssl verify against ONLY the given CA file (system store excluded), at
// the fixture capture time. Returns true when the chain verifies.
bool verifyChain(const std::string& caFile, const std::string& intFile, const std::string& leafFile,
                 const std::string& hostname = "") {
  const std::string& ossl = findOpenssl();
  std::string cmd = shellQuote(ossl) + " verify -no-CApath -no-CAstore -attime " + kVerifyAtEpoch + " -CAfile " +
                    shellQuote(caFile) + " -untrusted " + shellQuote(intFile);
  if (!hostname.empty()) cmd += " -verify_hostname " + shellQuote(hostname);
  cmd += " " + shellQuote(leafFile);
  return runCommand(cmd) == 0;
}

class OtaCaAnchorsOpensslTest : public testing::Test {
 protected:
  void SetUp() override {
    if (findOpenssl().empty()) {
      GTEST_SKIP() << "no OpenSSL with -no-CApath/-no-CAstore/-attime/-verify_hostname available "
                      "(set OTA_TEST_OPENSSL to point at one)";
    }
    bundle = writePem("ota-anchors.pem", ota_ca::kGithubOtaCaAnchors);
    // Anchor [0] is USERTrust ECC (api.github.com's root), anchor [1] is
    // ISRG Root X1 (the release CDN's root) -- see OtaCaCerts.h.
    usertrustOnly = writePem("ota-anchor-usertrust.pem", nthCert(ota_ca::kGithubOtaCaAnchors, 0));
    isrgOnly = writePem("ota-anchor-isrg.pem", nthCert(ota_ca::kGithubOtaCaAnchors, 1));
  }

  std::string bundle;
  std::string usertrustOnly;
  std::string isrgOnly;
};

// --- pure content checks (no openssl needed) ----------------------------

TEST(OtaCaAnchorsContent, ExactlyTwoAnchorsNoKeyMaterial) {
  const std::string pem = ota_ca::kGithubOtaCaAnchors;
  size_t begins = 0;
  size_t ends = 0;
  for (size_t p = pem.find("-----BEGIN CERTIFICATE-----"); p != std::string::npos;
       p = pem.find("-----BEGIN CERTIFICATE-----", p + 1)) {
    begins++;
  }
  for (size_t p = pem.find("-----END CERTIFICATE-----"); p != std::string::npos;
       p = pem.find("-----END CERTIFICATE-----", p + 1)) {
    ends++;
  }
  EXPECT_EQ(begins, 2u);
  EXPECT_EQ(ends, 2u);
  EXPECT_EQ(pem.find("PRIVATE KEY"), std::string::npos);
}

// --- chain verification against the anchors -----------------------------

TEST_F(OtaCaAnchorsOpensslTest, ApiGithubChainVerifies) {
  EXPECT_TRUE(verifyChain(bundle, fixture("api-int.pem"), fixture("api-leaf.pem")));
}

TEST_F(OtaCaAnchorsOpensslTest, ReleaseAssetsChainVerifies) {
  EXPECT_TRUE(verifyChain(bundle, fixture("cdn-int.pem"), fixture("cdn-leaf.pem")));
}

// Wrong/untrusted CA must fail: each chain against only the OTHER chain's
// anchor. This also proves both anchors are load-bearing -- neither chain is
// being rescued by the other root or by any hidden default store.
TEST_F(OtaCaAnchorsOpensslTest, ApiChainRejectedByWrongCa) {
  EXPECT_FALSE(verifyChain(isrgOnly, fixture("api-int.pem"), fixture("api-leaf.pem")));
}

TEST_F(OtaCaAnchorsOpensslTest, ReleaseAssetsChainRejectedByWrongCa) {
  EXPECT_FALSE(verifyChain(usertrustOnly, fixture("cdn-int.pem"), fixture("cdn-leaf.pem")));
}

// --- hostname binding ----------------------------------------------------

TEST_F(OtaCaAnchorsOpensslTest, HostnamesTheOtaFlowConnectsToMatch) {
  EXPECT_TRUE(verifyChain(bundle, fixture("api-int.pem"), fixture("api-leaf.pem"), "api.github.com"));
  EXPECT_TRUE(
      verifyChain(bundle, fixture("cdn-int.pem"), fixture("cdn-leaf.pem"), "release-assets.githubusercontent.com"));
}

TEST_F(OtaCaAnchorsOpensslTest, HostnameMismatchRejected) {
  EXPECT_FALSE(verifyChain(bundle, fixture("api-int.pem"), fixture("api-leaf.pem"), "evil.example.com"));
  // A CA-signed cert for one GitHub property must not satisfy a connection to
  // the other host: the leafs' SAN sets differ and the check must notice.
  EXPECT_FALSE(
      verifyChain(bundle, fixture("api-int.pem"), fixture("api-leaf.pem"), "release-assets.githubusercontent.com"));
}

}  // namespace
