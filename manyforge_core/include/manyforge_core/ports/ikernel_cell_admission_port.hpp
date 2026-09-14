#pragma once

#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/manager_liveness.hpp>

namespace manyforge::core {

struct ActiveControlConfigurationDTO;

class IKernelCellAdmissionPort {
 public:
  virtual ~IKernelCellAdmissionPort() = default;

  [[nodiscard]] virtual AuthoringAdmissionDTO GetAuthoringAdmission() const = 0;
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO& snapshot) = 0;
  [[nodiscard]] virtual Result<void> Publish(
      const ManagerLivenessObservationDTO& observation) {
    static_cast<void>(observation);
    return Result<void>::Err(
        ErrorDTO{"cell_admission_liveness_publication_unsupported",
                 "Kernel cell-admission liveness publication is unsupported",
                 {}});
  }
  [[nodiscard]] virtual Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) = 0;
  /// Process-local replacement recovery; ordinary transport does not expose
  /// this authority. Kernel must verify its exact adopted predecessor.
  [[nodiscard]] virtual Result<CellAdmissionClaimDTO> AcquireRouteRecoveryCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO&, const ActiveControlConfigurationDTO&) {
    return Result<CellAdmissionClaimDTO>::Err(
        ErrorDTO{"invalid_request",
                 "Kernel route recovery admission is unavailable",
                 {{"reason", "route_recovery_admission_unsupported"}}});
  }
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) = 0;
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const CellAdmissionReconcileDTO& reconcile) = 0;
  [[nodiscard]] virtual Result<CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const CellAdmissionRecoveryReceiptQueryDTO& query) const = 0;
  [[nodiscard]] virtual Result<void> ValidateProgramResetSceneAuthorization(
      const ProgramResetSceneAuthorizationDTO& authorization) const {
    static_cast<void>(authorization);
    return Result<void>::Err(
        ErrorDTO{"authoring_not_ready",
                 "Program Reset scene authorization is unsupported", {}});
  }
};

}  // namespace manyforge::core
