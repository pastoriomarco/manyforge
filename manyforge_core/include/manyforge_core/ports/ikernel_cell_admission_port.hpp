#pragma once

#include <manyforge_core/types/cell_admission.hpp>
#include <memory>
#include <manyforge_core/types/manager_liveness.hpp>
#include <manyforge_core/types/operator_alarm_attention.hpp>

namespace manyforge::core {

class IIoSignalService;

class IKernelCellAdmissionPort {
 public:
  virtual ~IKernelCellAdmissionPort() = default;

  [[nodiscard]] virtual AuthoringAdmissionDTO GetAuthoringAdmission() const = 0;
  /// Process-local notification only: the affected service is pinned when
  /// genuine BB teardown mutates it, even if a later update fails. Callers
  /// queue processing only after this call unwinds; the pin is not completion
  /// proof and is never serialized over the admission protocol.
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO& snapshot,
      std::shared_ptr<IIoSignalService>* actual_retired_service = nullptr) = 0;
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
  [[nodiscard]] virtual Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) = 0;
  /// A Program Reset claim, proven from the Program runner's current cycle
  /// status that the request carries beside the intent (OD-139).
  [[nodiscard]] virtual Result<CellAdmissionClaimDTO> AcquireProgramResetClaim(
      const CellAdmissionClaimIntentDTO& intent, const ProgramCycleStatusDTO& program_cycle_status) {
    static_cast<void>(intent);
    static_cast<void>(program_cycle_status);
    return Result<CellAdmissionClaimDTO>::Err(
        ErrorDTO{"authoring_not_ready", "Program Reset claim acquisition is unsupported", {}});
  }
  /// A Manager-Apply claim with the candidate bundle its Apply installs, carried
  /// beside the intent for the execution host. The Kernel needs the intent only.
  [[nodiscard]] virtual Result<CellAdmissionClaimDTO> AcquireManagerApplyClaim(
      const CellAdmissionClaimIntentDTO& intent, const RetainedBundleId& candidate_retained_bundle_id) {
    static_cast<void>(candidate_retained_bundle_id);
    return AcquireCellAdmissionClaim(intent);
  }
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) = 0;
  /// The Manager's release of an Apply's claim with the Apply's outcome, carried
  /// beside the release for the execution host. The Kernel needs the release only.
  [[nodiscard]] virtual Result<AuthoringAdmissionDTO> ReleaseManagerApplyClaim(
      const CellAdmissionClaimReleaseDTO& release, const ManagerApplyReleaseOutcomeDTO& outcome) {
    static_cast<void>(outcome);
    return ReleaseCellAdmissionClaim(release);
  }
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
