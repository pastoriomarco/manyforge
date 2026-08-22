#pragma once

#include <functional>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <mutex>
#include <vector>

namespace manyforge::core {

using CellAdmissionInterventionValidator =
    std::function<Result<void>(const CellAdmissionClaimIntentDTO&,
                               const std::optional<CellAdmissionClaimDTO>&)>;
using CellAdmissionSettlementValidator =
    std::function<Result<void>(const CellAdmissionClaimDTO&,
                               const CellAdmissionClaimReleaseDTO&)>;
using CellAdmissionReconcileValidator =
    std::function<Result<CellAdmissionReconcileValidationDTO>(
        const std::vector<CellAdmissionClaimDTO>&, const CellAdmissionReconcileDTO&)>;

/// The sole focused cell-admission ledger composed inside Kernel. Kernel takes
/// its shared admission mutex before this state mutex; Stop recognition and
/// forensic projection take neither in that order and retain their reserve.
class KernelCellAdmissionOwner final : public IKernelCellAdmissionPort {
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
      std::vector<CellAdmissionCurrentOwnerEpochDTO> recovered_current_owners = {});

  [[nodiscard]] AuthoringAdmissionDTO GetAuthoringAdmission() const override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO& snapshot) override;
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const CellAdmissionReconcileDTO& reconcile) override;

  [[nodiscard]] bool has_unresolved_claims() const;

 private:
  struct Entry final {
    CellAdmissionClaimDTO claim;
    std::optional<CellAdmissionClaimReleaseDTO> release;
  };

  [[nodiscard]] AuthoringAdmissionDTO AdmissionLocked() const noexcept;
  [[nodiscard]] bool BaseReadinessLocked() const noexcept;
  [[nodiscard]] const Entry* FindReplayLocked(const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] Entry* FindHeldLocked(CellAdmissionClaimGeneration generation);
  [[nodiscard]] const Entry* ParentCycleLocked(
      const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] bool IsStaleOwnerLocked(const CellAdmissionClaimIntentDTO& intent) const;

  RequestOwnerEpoch kernel_owner_epoch_;
  std::size_t capacity_;
  IClock& clock_;
  IOperationJournal& journal_;
  CellAdmissionInterventionValidator intervention_validator_;
  CellAdmissionSettlementValidator settlement_validator_;
  CellAdmissionReconcileValidator reconcile_validator_;
  mutable std::mutex mutex_;
  CellAdmissionReadinessSnapshotDTO readiness_;
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
