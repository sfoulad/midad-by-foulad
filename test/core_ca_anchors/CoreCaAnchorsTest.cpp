// Host-side proof that the api.midad.one trust anchors (src/network/CoreCaCerts.h)
// do what their header claims, including the NEGATIVE cases: the recorded live
// chain of api.midad.one verifies against the embedded anchors on both of its
// legitimate paths, and a wrong CA, a hostname mismatch, an altered signature,
// an expired/not-yet-valid moment and an incomplete chain all fail.
//
// The chain lives as committed fixtures (captured 2026-08-31 with
// `openssl s_client -servername api.midad.one -showcerts`); verification time is
// pinned to the capture epoch so certificate expiry can never turn the positive
// tests flaky, and the expiry tests move that instant deliberately.
//
// X.509 path building and hostname matching are exercised through the openssl
// CLI: what these tests pin down is the CONTENT of the anchor set and of the
// recorded chain, not any particular TLS stack's verifier. Skips (never passes)
// when no OpenSSL with the required flags is available; CI's ubuntu runner
// always has one.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "network/CoreCaCerts.h"

namespace {

// The fixture capture instant: the positive cases are evaluated as of here.
constexpr const char* kVerifyAtCapture = "1788206400";  // 2026-08-31T20:00:00Z
// After the leaf's notAfter (2026-11-25T08:25:04Z).
constexpr const char* kVerifyAfterLeafExpiry = "1800000000";  // 2027-01-15
// Before the leaf's notBefore (2026-08-27T07:25:06Z).
constexpr const char* kVerifyBeforeLeafValidity = "1780000000";  // 2026-05-29

std::string fixture(const std::string& name) { return std::string(CORE_CA_FIXTURES_DIR "/") + name; }

std::string shellQuote(const std::string& s) { return "'" + s + "'"; }

int runCommand(const std::string& cmd) {
  // Redirect both streams: the openssl error text is part of the expected
  // behavior in the negative cases, not test noise worth printing.
  return std::system((cmd + " >/dev/null 2>&1").c_str());
}

std::string readFile(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// An openssl whose `verify` knows -no-CApath/-no-CAstore (so the system trust
// store can't silently rescue a chain our anchors reject -- LibreSSL's verify,
// macOS's /usr/bin/openssl, lacks these and is deliberately not usable here),
// -attime, and -verify_hostname. Empty string when none qualifies.
std::string findOpenssl() {
  static std::string cached = [] {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("CORE_CA_TEST_OPENSSL")) candidates.push_back(env);
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

// Splits the concatenated anchors so each chain can be verified against one
// root at a time.
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

struct VerifyArgs {
  std::string caFile;
  std::string untrustedFile;  // empty: no intermediates supplied
  std::string leafFile;
  std::string hostname;  // empty: no hostname check
  const char* atTime = kVerifyAtCapture;
};

// openssl verify against ONLY the given CA file (system store excluded).
bool verifyChain(const VerifyArgs& a) {
  std::string cmd = shellQuote(findOpenssl()) + " verify -no-CApath -no-CAstore -attime " + a.atTime + " -CAfile " +
                    shellQuote(a.caFile);
  if (!a.untrustedFile.empty()) cmd += " -untrusted " + shellQuote(a.untrustedFile);
  if (!a.hostname.empty()) cmd += " -verify_hostname " + shellQuote(a.hostname);
  cmd += " " + shellQuote(a.leafFile);
  return runCommand(cmd) == 0;
}

class CoreCaAnchorsOpensslTest : public testing::Test {
 protected:
  void SetUp() override {
    if (findOpenssl().empty()) {
      GTEST_SKIP() << "no OpenSSL with -no-CApath/-no-CAstore/-attime/-verify_hostname available "
                      "(set CORE_CA_TEST_OPENSSL to point at one)";
    }
    anchors = writePem("core-anchors.pem", core_ca::kMidadCoreCaAnchors);
    // Anchor [0] is GTS Root R4 (self-signed, the alternate path's root),
    // anchor [1] is GlobalSign Root CA (the cross-signature's root).
    gtsRootR4Only = writePem("core-anchor-gtsr4.pem", nthCert(core_ca::kMidadCoreCaAnchors, 0));
    globalSignOnly = writePem("core-anchor-globalsign.pem", nthCert(core_ca::kMidadCoreCaAnchors, 1));
    // The constant the pinned wolfSSL pairing path actually loads.
    gtsRootR4Constant = writePem("core-anchor-gtsr4-constant.pem", core_ca::kMidadCoreCaGtsRootR4);

    leaf = fixture("api-midad-leaf.pem");
    tamperedLeaf = fixture("api-midad-leaf-tampered.pem");
    unrelatedRoot = fixture("unrelated-root-isrg-x1.pem");
    // What the server actually sends alongside the leaf.
    servedIntermediates = writePem("core-served-intermediates.pem", readFile(fixture("api-midad-int-we1.pem")) +
                                                                        readFile(fixture("api-midad-cross-gtsr4.pem")));
    // The same handshake with the redundant cross certificate omitted.
    alternateIntermediates = fixture("api-midad-int-we1.pem");
  }

  std::string anchors;
  std::string gtsRootR4Only;
  std::string globalSignOnly;
  std::string gtsRootR4Constant;
  std::string leaf;
  std::string tamperedLeaf;
  std::string unrelatedRoot;
  std::string servedIntermediates;
  std::string alternateIntermediates;
};

// --- pure content checks (no openssl needed) ----------------------------

TEST(CoreCaAnchorsContent, ExactlyTwoAnchorsNoKeyMaterial) {
  const std::string pem = core_ca::kMidadCoreCaAnchors;
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
  // A trust anchor set is public material only; a private key here would mean
  // someone pasted the wrong half of a keypair into the firmware.
  EXPECT_EQ(pem.find("PRIVATE KEY"), std::string::npos);
}

// kMidadCoreCaGtsRootR4 must be the audited anchor, not a second capture of it.
TEST(CoreCaAnchorsContent, GtsRootR4ConstantIsTheFirstAuditedAnchor) {
  const std::string standalone = core_ca::kMidadCoreCaGtsRootR4;
  EXPECT_EQ(standalone, nthCert(core_ca::kMidadCoreCaAnchors, 0));
  EXPECT_TRUE(nthCert(standalone, 1).empty());  // exactly one certificate
  EXPECT_EQ(standalone.find("PRIVATE KEY"), std::string::npos);
}

TEST(CoreCaAnchorsContent, BothAnchorsAreExtractable) {
  EXPECT_FALSE(nthCert(core_ca::kMidadCoreCaAnchors, 0).empty());
  EXPECT_FALSE(nthCert(core_ca::kMidadCoreCaAnchors, 1).empty());
  EXPECT_TRUE(nthCert(core_ca::kMidadCoreCaAnchors, 2).empty());
  EXPECT_NE(nthCert(core_ca::kMidadCoreCaAnchors, 0), nthCert(core_ca::kMidadCoreCaAnchors, 1));
}

// --- positive: both legitimate paths reach an anchor --------------------

TEST_F(CoreCaAnchorsOpensslTest, ServedChainVerifiesAgainstTheAnchorSet) {
  EXPECT_TRUE(verifyChain({anchors, servedIntermediates, leaf, "api.midad.one"}));
}

TEST_F(CoreCaAnchorsOpensslTest, ServedCrossSignedPathNeedsGlobalSignRootCa) {
  EXPECT_TRUE(verifyChain({globalSignOnly, servedIntermediates, leaf, "api.midad.one"}));
}

TEST_F(CoreCaAnchorsOpensslTest, AlternatePathNeedsGtsRootR4) {
  EXPECT_TRUE(verifyChain({gtsRootR4Only, alternateIntermediates, leaf, "api.midad.one"}));
}

// The property the pinned wolfSSL path depends on: with the redundant
// cross-certificate STILL on the wire, anchoring at GTS Root R4 alone verifies
// the served chain. A verifier that discards the trailing certificate whose
// subject it already trusts (openssl by default; wolfSSL with
// WOLFSSL_ALT_CERT_CHAINS, which scripts/patch_wolfssl_midad.py sets) therefore
// never has to check that certificate's RSA-2048 signature.
TEST_F(CoreCaAnchorsOpensslTest, GtsRootR4AloneVerifiesTheServedChain) {
  EXPECT_TRUE(verifyChain({gtsRootR4Constant, servedIntermediates, leaf, "api.midad.one"}));
  EXPECT_TRUE(verifyChain({gtsRootR4Constant, alternateIntermediates, leaf, "api.midad.one"}));
}

// Dropping the second anchor must not have dropped any other check with it.
TEST_F(CoreCaAnchorsOpensslTest, GtsRootR4AloneStillRejectsAWrongHostname) {
  EXPECT_FALSE(verifyChain({gtsRootR4Constant, servedIntermediates, leaf, "api.midad.one.evil.example"}));
  EXPECT_FALSE(verifyChain({gtsRootR4Constant, servedIntermediates, leaf, "evil.example"}));
  EXPECT_FALSE(verifyChain({gtsRootR4Constant, alternateIntermediates, leaf, "a.b.api.midad.one"}));
}

TEST_F(CoreCaAnchorsOpensslTest, GtsRootR4AloneStillRejectsWrongCaTamperedAndExpired) {
  EXPECT_FALSE(verifyChain({unrelatedRoot, servedIntermediates, leaf, "api.midad.one"}));
  EXPECT_FALSE(verifyChain({gtsRootR4Constant, servedIntermediates, tamperedLeaf, "api.midad.one"}));
  EXPECT_FALSE(verifyChain({gtsRootR4Constant, servedIntermediates, leaf, "api.midad.one", kVerifyAfterLeafExpiry}));
  EXPECT_FALSE(verifyChain({gtsRootR4Constant, "", leaf, "api.midad.one"}));
}

TEST_F(CoreCaAnchorsOpensslTest, EverySanTheLeafAdvertisesIsAccepted) {
  EXPECT_TRUE(verifyChain({anchors, servedIntermediates, leaf, "api.midad.one"}));
  EXPECT_TRUE(verifyChain({anchors, servedIntermediates, leaf, "midad.one"}));
  EXPECT_TRUE(verifyChain({anchors, servedIntermediates, leaf, "edge.api.midad.one"}));
}

// --- negative: why each anchor is not redundant -------------------------

// The reason GTS Root R4 is in the set: the day Google stops sending the
// cross certificate (it must by 2028-01-28, when GlobalSign Root CA expires),
// GlobalSign Root CA alone anchors nothing.
TEST_F(CoreCaAnchorsOpensslTest, GlobalSignAloneCannotVerifyTheAlternatePath) {
  EXPECT_FALSE(verifyChain({globalSignOnly, alternateIntermediates, leaf, "api.midad.one"}));
}

// --- negative: the four failure modes that must never verify ------------

TEST_F(CoreCaAnchorsOpensslTest, WrongCaIsRejected) {
  EXPECT_FALSE(verifyChain({unrelatedRoot, servedIntermediates, leaf, "api.midad.one"}));
  EXPECT_FALSE(verifyChain({unrelatedRoot, alternateIntermediates, leaf, "api.midad.one"}));
}

TEST_F(CoreCaAnchorsOpensslTest, HostnameMismatchIsRejected) {
  EXPECT_FALSE(verifyChain({anchors, servedIntermediates, leaf, "api.github.com"}));
  EXPECT_FALSE(verifyChain({anchors, servedIntermediates, leaf, "api.midad.one.evil.example"}));
  EXPECT_FALSE(verifyChain({anchors, servedIntermediates, leaf, "notmidad.one"}));
  // A single wildcard label matches one label, never two.
  EXPECT_FALSE(verifyChain({anchors, servedIntermediates, leaf, "a.b.api.midad.one"}));
}

TEST_F(CoreCaAnchorsOpensslTest, AlteredLeafSignatureIsRejected) {
  // Same TBS bytes as the live leaf, one bit flipped in the ECDSA signature.
  EXPECT_FALSE(verifyChain({anchors, servedIntermediates, tamperedLeaf, "api.midad.one"}));
  EXPECT_FALSE(verifyChain({anchors, alternateIntermediates, tamperedLeaf, "api.midad.one"}));
}

TEST_F(CoreCaAnchorsOpensslTest, ExpiredAndNotYetValidChainsAreRejected) {
  EXPECT_FALSE(verifyChain({anchors, servedIntermediates, leaf, "api.midad.one", kVerifyAfterLeafExpiry}));
  EXPECT_FALSE(verifyChain({anchors, servedIntermediates, leaf, "api.midad.one", kVerifyBeforeLeafValidity}));
}

TEST_F(CoreCaAnchorsOpensslTest, UntrustedOrIncompleteChainIsRejected) {
  // The leaf on its own: no intermediate, so nothing reaches an anchor.
  EXPECT_FALSE(verifyChain({anchors, "", leaf, "api.midad.one"}));
  // Trusting the intermediate instead of a root is not a substitute for an
  // anchor set: WE1 is not self-signed, so it cannot terminate a chain.
  EXPECT_FALSE(verifyChain({fixture("api-midad-int-we1.pem"), "", leaf, "api.midad.one"}));
}

}  // namespace
