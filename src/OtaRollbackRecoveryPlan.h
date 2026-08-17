#pragma once

// Pure decision logic for whether the device should enter OTA rollback
// restricted recovery -- split out of main.cpp so it's host-testable without
// any esp_ota_ops.h/Arduino dependency. See test/ota_rollback_recovery_plan/.

enum class OtaRollbackRecoveryPlan {
  Normal,                   // no rollback detected, or the rollback was already acknowledged
  EnterRestrictedRecovery,  // a new, unacknowledged rollback event was detected
};

// hasLastInvalidPartition: true if ESP-IDF's esp_ota_get_last_invalid_partition()
// returned non-null (the bootloader has marked some OTA slot ABORTED at least
// once -- this is a flash-persisted signal that survives across every future boot
// until that slot is freshly reflashed).
// invalidImageMatchesAcknowledged: true if the invalid partition's app_elf_sha256
// digest equals the digest the user already explicitly acknowledged via a prior
// "continue anyway." Deliberately keyed to the image digest, not the partition
// label (ota_0/ota_1) -- those labels get reused by every future OTA, so keying
// on them would let a brand new rollback silently inherit an old acknowledgment.
// Ignored when hasLastInvalidPartition is false.
OtaRollbackRecoveryPlan planOtaRollbackRecovery(bool hasLastInvalidPartition, bool invalidImageMatchesAcknowledged);
