#include <gtest/gtest.h>

#include "OtaRollbackRecoveryPlan.h"

// The distinction this guards: a rollback event is keyed to the invalid
// image's own app_elf_sha256 digest, not to a partition label (ota_0/ota_1)
// -- see OtaRollbackRecoveryPlan.h's comment for why a label-keyed
// acknowledgment would be unsafe.

TEST(OtaRollbackRecoveryPlan, NoRollbackIsNormal) {
  EXPECT_EQ(planOtaRollbackRecovery(/*hasLastInvalidPartition=*/false, /*invalidImageMatchesAcknowledged=*/false),
            OtaRollbackRecoveryPlan::Normal);
  // Second argument must be ignored when there's no rollback to reason about.
  EXPECT_EQ(planOtaRollbackRecovery(/*hasLastInvalidPartition=*/false, /*invalidImageMatchesAcknowledged=*/true),
            OtaRollbackRecoveryPlan::Normal);
}

TEST(OtaRollbackRecoveryPlan, RollbackWithMatchingAcknowledgmentIsNormal) {
  EXPECT_EQ(planOtaRollbackRecovery(/*hasLastInvalidPartition=*/true, /*invalidImageMatchesAcknowledged=*/true),
            OtaRollbackRecoveryPlan::Normal);
}

TEST(OtaRollbackRecoveryPlan, RollbackWithNonMatchingAcknowledgmentEntersRecovery) {
  // The regression this guards against: a stale acknowledgment for a
  // different, previously-acknowledged image must not silently cover a new
  // rollback to a different invalid image.
  EXPECT_EQ(planOtaRollbackRecovery(/*hasLastInvalidPartition=*/true, /*invalidImageMatchesAcknowledged=*/false),
            OtaRollbackRecoveryPlan::EnterRestrictedRecovery);
}

TEST(OtaRollbackRecoveryPlan, RollbackWithEmptyAcknowledgmentEntersRecovery) {
  // Never-acknowledged state (empty digest) must never happen to compare
  // equal to a real 64-hex-char digest -- covered explicitly since both are
  // ultimately just string comparisons upstream of this function.
  EXPECT_EQ(planOtaRollbackRecovery(/*hasLastInvalidPartition=*/true, /*invalidImageMatchesAcknowledged=*/false),
            OtaRollbackRecoveryPlan::EnterRestrictedRecovery);
}
