#pragma once

#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_core/types/manager_liveness.hpp>
#include <manyforge_core/types/operator_alarm_attention.hpp>

namespace manyforge::core {

struct ActiveControlConfigurationDTO;
struct SelectedComponentBringUpOfferDTO;

class IKernelCellAdmissionPort {
 public:
  virtual ~IKernelCellAdmissionPort() = default;

  [[nodiscard]] virtual AuthoringAdmissionDTO GetAuthoringAdmission() const = 0;
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO& snapshot) = 0;
  [[nodiscard]] virtual Result<void> Publish(const ManagerLivenessObservationDTO& observation) {
    static_cast<void>(observation);
    return Result<void>::Err(ErrorDTO{"cell_admission_liveness_publication_unsupported",
                                      "Kernel cell-admission liveness publication is unsupported",
                                      {}});
  }
  /// Headless Alarm Lifecycle Owner publication of its generation-fenced
  /// motion-attention projection. Only the Kernel evaluates it at admission.
  [[nodiscard]] virtual Result<OperatorAlarmAttentionAcceptedDTO> PublishOperatorAlarmAttention(
      const OperatorAlarmAttentionSnapshotDTO& snapshot) {
    static_cast<void>(snapshot);
    return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
        ErrorDTO{"cell_admission_operator_alarm_attention_unsupported",
                 "Kernel operator-alarm attention publication is unsupported",
                 {}});
  }
  /// CCLC component-incident containment (addendum 2, 2026-10-08): one
  /// system-attributed cell Full Stop through the Kernel's ordinary Stop
  /// ingress that always latches a cell Stop fault, even on an idle cell.
  /// Returns at acceptance and never waits for the physical outcome; a
  /// repeated request for the same incident is an exact replay.
  [[nodiscard]] virtual Result<ComponentIncidentContainmentReceiptDTO>
  RequestComponentIncidentContainment(const ComponentIncidentContainmentIntentDTO& intent) {
    static_cast<void>(intent);
    return Result<ComponentIncidentContainmentReceiptDTO>::Err(
        ErrorDTO{"cell_admission_incident_containment_unsupported",
                 "Kernel component-incident containment is unsupported",
                 {{"reason", "incident_containment_unsupported"}}});
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
  /// Private effectful selected-component admission. Ordinary wire acquisition
  /// never exposes this entry or treats an observation-only route as a permit.
  [[nodiscard]] virtual Result<CellAdmissionClaimDTO> AcquireSelectedComponentBringUpClaim(
      const CellAdmissionClaimIntentDTO&, const SelectedComponentBringUpOfferDTO&) {
    return Result<CellAdmissionClaimDTO>::Err(
        ErrorDTO{"invalid_request",
                 "Kernel selected bring-up admission is unavailable",
                 {{"reason", "selected_bring_up_admission_unsupported"}}});
  }
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) = 0;
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const CellAdmissionReconcileDTO& reconcile) = 0;
  [[nodiscard]] virtual Result<CellAdmissionRecoveryReceiptDTO> GetCellAdmissionRecoveryReceipt(
      const CellAdmissionRecoveryReceiptQueryDTO& query) const = 0;
  [[nodiscard]] virtual Result<void> ValidateProgramResetSceneAuthorization(
      const ProgramResetSceneAuthorizationDTO& authorization) const {
    static_cast<void>(authorization);
    return Result<void>::Err(
        ErrorDTO{"authoring_not_ready", "Program Reset scene authorization is unsupported", {}});
  }
};

}  // namespace manyforge::core
