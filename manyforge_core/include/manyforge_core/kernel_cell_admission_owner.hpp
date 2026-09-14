#pragma once

#include <functional>
#include <manyforge_core/ports/i_manager_liveness_observation_port.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <mutex>
#include <vector>

namespace manyforge::core {

using CellAdmissionInterventionValidator = std::function<Result<void>(
    const CellAdmissionClaimIntentDTO&, const std::optional<CellAdmissionClaimDTO>&)>;
using CellAdmissionProgramNodeValidator =
    std::function<Result<void>(const ProgramNodeAdmissionEvidenceDTO&)>;
using CellAdmissionSettlementValidator =
    std::function<Result<void>(const CellAdmissionClaimDTO&, const CellAdmissionClaimReleaseDTO&)>;
using CellAdmissionReconcileValidator = std::function<Result<CellAdmissionReconcileValidationDTO>(
    const std::vector<CellAdmissionClaimDTO>&,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>&, const CellAdmissionReconcileDTO&)>;

/// The sole focused cell-admission ledger composed inside Kernel. Kernel takes
/// its shared admission mutex before this state mutex; Stop recognition and
/// forensic projection take neither in that order and retain their reserve.
class KernelCellAdmissionOwner final : public IKernelCellAdmissionPort,
                                       public IManagerLivenessObservationPort {
 public:
  KernelCellAdmissionOwner(
      RequestOwnerEpoch kernel_owner_epoch, std::size_t capacity, IClock& clock,
      IOperationJournal& journal,
      std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionClaimDTO> recovery,
      AdmissionGeneration recovered_generation_high_water = AdmissionGeneration(0U),
      CellAdmissionInterventionValidator intervention_validator = {},
      CellAdmissionSettlementValidator settlement_validator = {},
      CellAdmissionReconcileValidator reconcile_validator = {},
      std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionMembershipDTO>
          recovered_membership = {},
      std::vector<CellAdmissionCurrentOwnerEpochDTO> recovered_current_owners = {},
      std::optional<ManagerLivenessRequiredSetDTO> manager_liveness_required_set = std::nullopt,
      CellAdmissionClaimGeneration recovered_claim_generation_high_water =
          CellAdmissionClaimGeneration(0U),
      CellAdmissionProgramNodeValidator program_node_validator = {},
      std::function<bool()> stop_fault_active = {});

  /// Noncritical publication of the live Kernel fault restriction. Closing is
  /// immediate; reopening waits for a durable admission generation advance.
  [[nodiscard]] Result<void> RefreshStopFaultAdmission();

  [[nodiscard]] AuthoringAdmissionDTO GetAuthoringAdmission() const override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO& snapshot) override;
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) override;
  /// Process-local Kernel composition: validate its freshly resolved manual
  /// offer without storing a second pending-offer registry.
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireManualCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const CellAdmissionInterventionValidator& validate_offer);
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireRouteRecoveryClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const CellAdmissionInterventionValidator& validate_offer);
  /// Read the same readiness/claim preconditions used by manual acquisition.
  /// This reserves nothing; the composing Kernel still checks the target hold.
  [[nodiscard]] Result<void> CheckManualCellAdmissionReadiness(
      std::optional<CellAdmissionClaimGeneration> parent) const;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const CellAdmissionReconcileDTO& reconcile) override;
  [[nodiscard]] Result<CellAdmissionRecoveryReceiptDTO> GetCellAdmissionRecoveryReceipt(
      const CellAdmissionRecoveryReceiptQueryDTO& query) const override;
  [[nodiscard]] Result<void> ValidateProgramResetSceneAuthorization(
      const ProgramResetSceneAuthorizationDTO& authorization) const override;
  [[nodiscard]] Result<void> Publish(const ManagerLivenessObservationDTO& observation) override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReplaceManagerLivenessRequiredSet(
      const ManagerLivenessRequiredSetDTO& required_set);
  [[nodiscard]] bool LatchManagerLivenessLoss(const ManagerLivenessLossIntentDTO& intent) noexcept;

  [[nodiscard]] bool has_unresolved_claims() const;
  [[nodiscard]] bool has_unresolved_claims_except(CellAdmissionOwnerRoleDTO owner_role) const;
  /// Kernel-owned attachment barrier; Manager readiness cannot clear it.
  [[nodiscard]] Result<void> SetRouteRecoveryPending(bool pending);
  /// Current held authority, excluding released and reconciled predecessors.
  [[nodiscard]] std::optional<CellAdmissionClaimDTO> held_claim(
      CellAdmissionClaimKindDTO kind) const;
  /// Bounded ordinary-release snapshot, never read by physical Stop. An empty
  /// node identity denotes an unscoped child (e.g. intervention/recovery).
  [[nodiscard]] std::vector<std::string> unsettled_program_child_nodes(
      CellAdmissionClaimGeneration parent) const;
  /// Internal successor-owner request using the exact existing ledger inventory.
  /// It grants no release: Reconcile still requires Kernel settlement validation.
  [[nodiscard]] Result<std::optional<CellAdmissionReconcileDTO>>
  manual_owner_reconciliation_request() const;

 private:
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const CellAdmissionInterventionValidator& validate_offer);
  struct Entry final {
    CellAdmissionClaimDTO claim;
    std::optional<CellAdmissionClaimReleaseDTO> release;
    std::optional<CellAdmissionReconcileSettlementDTO> reconcile_settlement;
  };

  [[nodiscard]] AuthoringAdmissionDTO AdmissionLocked() const noexcept;
  [[nodiscard]] bool NonCycleReadinessLocked() const noexcept;
  [[nodiscard]] bool ManualParentReadyLocked(const Entry* parent) const noexcept;
  [[nodiscard]] bool BaseReadinessLocked() const noexcept;
  [[nodiscard]] bool MatchingStartingProgramCycleLocked(
      const CellAdmissionClaimIntentDTO& intent) const noexcept;
  [[nodiscard]] const Entry* FindReplayLocked(const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] Entry* FindHeldLocked(CellAdmissionClaimGeneration generation);
  [[nodiscard]] const Entry* ParentCycleLocked(const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] bool HasUnsettledChildrenLocked(
      CellAdmissionClaimGeneration parent,
      const std::vector<CellAdmissionReconcileSettlementDTO>& prospective = {}) const;
  [[nodiscard]] bool IsStaleOwnerLocked(const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] Result<void> AdvanceGenerationLocked();

  struct ManagerLivenessRow final {
    RequiredManagerLivenessReceiverDTO receiver;
    std::uint64_t status_sequence{};
    bool current_unfenced{};
    bool direct_loss_inhibited{};
    bool generation_advance_pending{};
    std::uint64_t generation_pending_sequence{};
    bool generation_pending_current{};
  };

  RequestOwnerEpoch kernel_owner_epoch_;
  std::size_t capacity_;
  IClock& clock_;
  IOperationJournal& journal_;
  CellAdmissionInterventionValidator intervention_validator_;
  CellAdmissionSettlementValidator settlement_validator_;
  CellAdmissionReconcileValidator reconcile_validator_;
  CellAdmissionProgramNodeValidator program_node_validator_;
  std::function<bool()> stop_fault_active_;
  bool published_stop_fault_active_{};
  mutable std::mutex mutex_;
  mutable std::mutex liveness_transition_mutex_;
  CellAdmissionReadinessSnapshotDTO readiness_;
  bool route_recovery_pending_{};
  std::optional<ManagerLivenessRequiredSetDTO> manager_liveness_required_set_;
  std::vector<ManagerLivenessRow> manager_liveness_rows_;
  std::size_t manager_liveness_current_unfenced_count_{};
  AdmissionGeneration generation_{1U};
  CellAdmissionClaimGeneration next_claim_generation_{1U};
  std::size_t held_claim_count_{};
  std::vector<Entry> entries_;
  std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionMembershipDTO> membership_;
  std::vector<CellAdmissionCurrentOwnerEpochDTO> current_owners_;
  std::vector<std::pair<CellAdmissionReconcileDTO, CellAdmissionReconcileValidationDTO>>
      reconciliations_;
};

}  // namespace manyforge::core
