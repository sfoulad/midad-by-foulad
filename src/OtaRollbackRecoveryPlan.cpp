#include "OtaRollbackRecoveryPlan.h"

OtaRollbackRecoveryPlan planOtaRollbackRecovery(const bool hasLastInvalidPartition,
                                                const bool invalidImageMatchesAcknowledged) {
  if (!hasLastInvalidPartition) {
    return OtaRollbackRecoveryPlan::Normal;
  }
  return invalidImageMatchesAcknowledged ? OtaRollbackRecoveryPlan::Normal
                                         : OtaRollbackRecoveryPlan::EnterRestrictedRecovery;
}
