#pragma once

#include <manyforge_core/types/cell_admission.hpp>

namespace manyforge::core {

class IKernelCellAdmissionPort {
 public:
  virtual ~IKernelCellAdmissionPort() = default;

  [[nodiscard]] virtual AuthoringAdmissionDTO GetAuthoringAdmission() const = 0;
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO& snapshot) = 0;
  [[nodiscard]] virtual Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) = 0;
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) = 0;
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const CellAdmissionReconcileDTO& reconcile) = 0;
};

}  // namespace manyforge::core
