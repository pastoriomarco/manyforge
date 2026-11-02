#pragma once

#include <manyforge_core/types/cell_admission.hpp>
#include <memory>
#include <manyforge_core/types/manager_liveness.hpp>
#include <manyforge_core/types/operator_alarm_attention.hpp>

namespace manyforge::core {

class IIoSignalService;

/// Stack-scoped exclusion on the existing Kernel admission mutex. It
/// creates no claim, generation or retained permission. Validate is a current
/// fact check, not a promise that future Stop faults cannot arise.
class ICellCloseAdmissionLease {
 public:
  virtual ~ICellCloseAdmissionLease() = default;
  [[nodiscard]] virtual Result<void> ValidatePrerequisites() const = 0;
};

class IKernelCellAdmissionPort {
 public:
  virtual ~IKernelCellAdmissionPort() = default;

  [[nodiscard]] virtual Result<std::unique_ptr<ICellCloseAdmissionLease>>
  AcquireCellCloseAdmissionLease() {
    return Result<std::unique_ptr<ICellCloseAdmissionLease>>::Err(
        {"cell_close_prerequisites_unavailable", "the Kernel Close admission join is unavailable", {}});
  }
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
  /// CP9 stream health: while `hold` is set, new commands of the component
  /// are refused with it (a `fault`/`blocking` stream is not live). With
  /// `cell_wide` (a component without motion of its own, e.g. a pose source)
  /// new commands of every component are refused with it. `nullopt` lifts
  /// it. No incident, Stop or claim; running work is not touched.
  [[nodiscard]] virtual Result<void> PublishComponentStreamHold(
      const ControllableComponentId& component, const std::optional<ErrorDTO>& hold,
      bool cell_wide = false) {
    static_cast<void>(component);
    static_cast<void>(hold);
    static_cast<void>(cell_wide);
    return Result<void>::Err(ErrorDTO{"cell_admission_stream_hold_unsupported",
                                      "Kernel component stream holds are unsupported",
                                      {}});
  }
  [[nodiscard]] virtual Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) = 0;
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
