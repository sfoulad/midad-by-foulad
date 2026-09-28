#include <gtest/gtest.h>

#include "network/FirmwareUpdatePolicy.h"
#include "network/OtaUpdater.h"

namespace fup = firmware_update_policy;

// The device that reproduced the field defect: an X4 Pro running an RC build
// tagged with a 7-character short SHA.
static constexpr char RUNNING_RC[] = "1.8.57-rc+254abcd";
static constexpr char RUNNING_STABLE[] = "1.8.57";

// ---------------------------------------------------------------------------
// isVersionNewer: normalization
// ---------------------------------------------------------------------------

TEST(IsVersionNewer, IdenticalVersionIsNotNewer) {
  EXPECT_FALSE(fup::isVersionNewer("1.8.57", RUNNING_STABLE));
  EXPECT_FALSE(fup::isVersionNewer(RUNNING_RC, RUNNING_RC));
}

TEST(IsVersionNewer, IdenticalVersionWithLeadingVIsNotNewer) {
  EXPECT_FALSE(fup::isVersionNewer("v1.8.57", RUNNING_STABLE));
  EXPECT_FALSE(fup::isVersionNewer("V1.8.57", RUNNING_STABLE));
  EXPECT_FALSE(fup::isVersionNewer("1.8.57", "v1.8.57"));
}

TEST(IsVersionNewer, IdenticalVersionWithMatchingCommitMetadataIsNotNewer) {
  EXPECT_FALSE(fup::isVersionNewer("v1.8.57-rc+254abcd", RUNNING_RC));
  EXPECT_FALSE(fup::isVersionNewer("1.8.57-rc+254abcd", RUNNING_RC));
}

// The re-offer loop from the field: the tag names the same version and channel,
// only the short-SHA truncation differs (7 vs 8 characters of the same commit).
TEST(IsVersionNewer, SameVersionDifferentShaTruncationIsNotNewer) {
  EXPECT_FALSE(fup::isVersionNewer("v1.8.57-rc+254abcd6", RUNNING_RC));
  EXPECT_FALSE(fup::isVersionNewer("1.8.57-rc+254abcd6", RUNNING_RC));
  EXPECT_FALSE(fup::isVersionNewer("v1.8.57-rc+254abcd", "1.8.57-rc+254abcd6"));
}

// Equal triple, both RC, genuinely different commits: build metadata cannot
// order two builds, and a newer build always gets a bumped patch from the
// release workflows -- never re-offer.
TEST(IsVersionNewer, SameVersionDifferentRcCommitIsNotNewer) {
  EXPECT_FALSE(fup::isVersionNewer("v1.8.57-rc+4280234", RUNNING_RC));
}

// ---------------------------------------------------------------------------
// isVersionNewer: ordering
// ---------------------------------------------------------------------------

TEST(IsVersionNewer, OlderRcIsNotNewer) { EXPECT_FALSE(fup::isVersionNewer("v1.8.50-rc+aaaaaaa", RUNNING_RC)); }

TEST(IsVersionNewer, NewerRcIsNewer) { EXPECT_TRUE(fup::isVersionNewer("v1.8.98-rc+4280234", RUNNING_RC)); }

TEST(IsVersionNewer, NewerMinorAndMajorAreNewer) {
  EXPECT_TRUE(fup::isVersionNewer("v1.9.0", RUNNING_STABLE));
  EXPECT_TRUE(fup::isVersionNewer("v2.0.0", "1.9.99"));
}

TEST(IsVersionNewer, OlderMinorAndMajorAreNotNewer) {
  EXPECT_FALSE(fup::isVersionNewer("v1.7.99", RUNNING_STABLE));
  EXPECT_FALSE(fup::isVersionNewer("v0.9.9", RUNNING_STABLE));
}

// The one legitimate same-number upgrade: the stable release that finalizes
// the RC the device is running.
TEST(IsVersionNewer, StableFinalizingTheRunningRcIsNewer) {
  EXPECT_TRUE(fup::isVersionNewer("v1.8.57", RUNNING_RC));
  EXPECT_TRUE(fup::isVersionNewer("1.8.57", RUNNING_RC));
}

TEST(IsVersionNewer, RcDoesNotUpgradeTheEqualNumberedStable) {
  EXPECT_FALSE(fup::isVersionNewer("v1.8.57-rc+4280234", RUNNING_STABLE));
}

// ---------------------------------------------------------------------------
// isVersionNewer: malformed input never justifies flashing
// ---------------------------------------------------------------------------

TEST(IsVersionNewer, UnparsableInputIsNotNewer) {
  EXPECT_FALSE(fup::isVersionNewer(nullptr, RUNNING_STABLE));
  EXPECT_FALSE(fup::isVersionNewer("", RUNNING_STABLE));
  EXPECT_FALSE(fup::isVersionNewer("not-a-version", RUNNING_STABLE));
  EXPECT_FALSE(fup::isVersionNewer("v1.8", RUNNING_STABLE));
  EXPECT_FALSE(fup::isVersionNewer("v9.9.9", "dev-simulator"));
  EXPECT_FALSE(fup::isVersionNewer("v9.9.9", nullptr));
  EXPECT_FALSE(fup::isVersionNewer("v9.9.9", ""));
}

// OtaUpdater's single-argument form must give the same answers against the
// compiled-in CROSSPOINT_VERSION (pinned to RUNNING_RC by this test's build).
TEST(IsVersionNewer, OtaUpdaterDelegatesToTheSharedComparator) {
  EXPECT_FALSE(OtaUpdater::isVersionNewer("v1.8.57-rc+254abcd"));
  EXPECT_FALSE(OtaUpdater::isVersionNewer("v1.8.57-rc+254abcd6"));
  EXPECT_TRUE(OtaUpdater::isVersionNewer("v1.8.98-rc+4280234"));
  EXPECT_TRUE(OtaUpdater::isVersionNewer("v1.8.57"));
}

// ---------------------------------------------------------------------------
// boardAssetFileName
// ---------------------------------------------------------------------------

TEST(BoardAssetFileName, X4UsesTheDefaultAsset) {
  char out[48];
  fup::boardAssetFileName("x4", 2, out, sizeof(out));
  EXPECT_STREQ("firmware.bin", out);
}

TEST(BoardAssetFileName, OtherBoardsGetPerBoardAssets) {
  char out[48];
  fup::boardAssetFileName("x4pro", 5, out, sizeof(out));
  EXPECT_STREQ("firmware-x4pro.bin", out);
  fup::boardAssetFileName("sticky", 6, out, sizeof(out));
  EXPECT_STREQ("firmware-sticky.bin", out);
  // Board name is (pointer, length) into a longer tag string -- length wins.
  fup::boardAssetFileName("x4pro;trailing", 5, out, sizeof(out));
  EXPECT_STREQ("firmware-x4pro.bin", out);
}

// ---------------------------------------------------------------------------
// chooseOffer
// ---------------------------------------------------------------------------

namespace {
constexpr char X4_ASSET[] = "firmware.bin";
constexpr char X4PRO_ASSET[] = "firmware-x4pro.bin";

fup::ChannelInfo channel(const char* tag) { return {tag, nullptr, 0, false}; }

fup::ChannelInfo channelWithAssets(const char* tag, const char* const* assets, const size_t count) {
  return {tag, assets, count, true};
}
}  // namespace

TEST(ChooseOffer, NewerRcWithBoardAssetListedIsOffered) {
  const char* assets[] = {"firmware.bin", "firmware-x4pro.bin"};
  const auto decision = fup::chooseOffer(channelWithAssets("v1.8.98-rc+4280234", assets, 2), channel(nullptr),
                                         /*wantsPrerelease=*/true, RUNNING_RC, X4PRO_ASSET);
  EXPECT_EQ(fup::OfferOutcome::OFFER, decision.outcome);
  EXPECT_STREQ("v1.8.98-rc+4280234", decision.tag);
}

// The hardware defect: v1.8.98-rc carries only the C3 firmware.bin. Whether
// the server lists the assets or says nothing, the X4 Pro must not be offered.
TEST(ChooseOffer, NewerReleaseWithoutX4ProAssetIsSuppressed) {
  const char* c3Only[] = {"firmware.bin"};
  const auto listed = fup::chooseOffer(channelWithAssets("v1.8.98-rc+4280234", c3Only, 1), channel(nullptr),
                                       /*wantsPrerelease=*/true, RUNNING_RC, X4PRO_ASSET);
  EXPECT_EQ(fup::OfferOutcome::MISSING_BOARD_ASSET, listed.outcome);
  EXPECT_EQ(nullptr, listed.tag);

  const auto unlisted = fup::chooseOffer(channel("v1.8.98-rc+4280234"), channel(nullptr),
                                         /*wantsPrerelease=*/true, RUNNING_RC, X4PRO_ASSET);
  EXPECT_EQ(fup::OfferOutcome::MISSING_BOARD_ASSET, unlisted.outcome);
  EXPECT_EQ(nullptr, unlisted.tag);
}

// Legacy C3 behavior is preserved: every release carries firmware.bin, so a
// server that lists no assets still produces offers for the default board.
TEST(ChooseOffer, DefaultAssetBoardIsOfferedWithoutAssetInfo) {
  const auto decision = fup::chooseOffer(channel("v1.8.98-rc+4280234"), channel(nullptr),
                                         /*wantsPrerelease=*/true, RUNNING_RC, X4_ASSET);
  EXPECT_EQ(fup::OfferOutcome::OFFER, decision.outcome);
  EXPECT_STREQ("v1.8.98-rc+4280234", decision.tag);
}

TEST(ChooseOffer, PrereleaseChannelIsIgnoredWhenSettingIsOff) {
  const auto decision = fup::chooseOffer(channel("v1.8.98-rc+4280234"), channel("v1.8.57"),
                                         /*wantsPrerelease=*/false, RUNNING_STABLE, X4_ASSET);
  EXPECT_EQ(fup::OfferOutcome::NONE, decision.outcome);
  EXPECT_EQ(nullptr, decision.tag);
}

TEST(ChooseOffer, StableChannelIsOfferedWhenSettingIsOff) {
  const auto decision = fup::chooseOffer(channel("v1.8.98-rc+4280234"), channel("v1.8.60"),
                                         /*wantsPrerelease=*/false, RUNNING_STABLE, X4_ASSET);
  EXPECT_EQ(fup::OfferOutcome::OFFER, decision.outcome);
  EXPECT_STREQ("v1.8.60", decision.tag);
}

TEST(ChooseOffer, RcMissingAssetFallsThroughToStableWithAsset) {
  const char* c3Only[] = {"firmware.bin"};
  const char* withX4Pro[] = {"firmware.bin", "firmware-x4pro.bin"};
  const auto decision =
      fup::chooseOffer(channelWithAssets("v1.8.98-rc+4280234", c3Only, 1), channelWithAssets("v1.8.60", withX4Pro, 2),
                       /*wantsPrerelease=*/true, RUNNING_RC, X4PRO_ASSET);
  EXPECT_EQ(fup::OfferOutcome::OFFER, decision.outcome);
  EXPECT_STREQ("v1.8.60", decision.tag);
}

TEST(ChooseOffer, InstalledVersionIsNeverReoffered) {
  const auto decision = fup::chooseOffer(channel("v1.8.57-rc+254abcd"), channel(nullptr),
                                         /*wantsPrerelease=*/true, RUNNING_RC, X4_ASSET);
  EXPECT_EQ(fup::OfferOutcome::NONE, decision.outcome);
  EXPECT_EQ(nullptr, decision.tag);
}

TEST(ChooseOffer, NothingReportedProducesNoOffer) {
  const auto decision =
      fup::chooseOffer(channel(nullptr), channel(nullptr), /*wantsPrerelease=*/true, RUNNING_RC, X4_ASSET);
  EXPECT_EQ(fup::OfferOutcome::NONE, decision.outcome);
  EXPECT_EQ(nullptr, decision.tag);
}
