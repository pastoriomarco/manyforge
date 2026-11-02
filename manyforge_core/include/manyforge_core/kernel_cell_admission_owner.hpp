#pragma once

#include <cstdint>
#include <functional>
#include <manyforge_core/ports/i_manager_liveness_observation_port.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

class IIoSignalService;
class ResourceAddressedExecutionKernel;

using CellAdmissionInterventionValidator = std::function<Result<void>(
    const CellAdmissionClaimIntentDTO&, const std::optional<CellAdmissionClaimDTO>&)>;
// Trusted process-local callbacks. No caller or wire supplies terminal proof.
using CellAdmissionConfigurationApplyValidator = std::function<Result<void>(
    const CellAdmissionClaimIntentDTO&, std::span<const CellAdmissionClaimDTO>,
    const CellAdmissionReadinessSnapshotDTO&, const RuntimeEpoch&)>;
using CellAdmissionConfigurationApplySuccessValidator =
    std::function<Result<void>(const CellAdmissionClaimDTO&, const CellAdmissionClaimReleaseDTO&,
                               std::span<const CellAdmissionClaimDTO>)>;
using CellAdmissionProgramNodeValidator =
    std::function<Result<void>(const ProgramNodeAdmissionEvidenceDTO&)>;
using CellAdmissionSettlementValidator =
    std::function<Result<void>(const CellAdmissionClaimDTO&, const CellAdmissionClaimReleaseDTO&)>;
enum class ProgramResetJournalReadPurpose { kAcquire, kReloadInitialScene, kResetRobotPosition };
/// Current original Runner journal, read outside admission/owner locks.
/// The adapter never calls Manager/Kernel or mutates the journal.
using CellAdmissionProgramResetStatusSource = std::function<Result<ProgramCycleStatusDTO>(
    ProgramResetJournalReadPurpose, const CellAdmissionClaimIntentDTO&,
    const std::optional<CellAdmissionClaimDTO>&)>;
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
      CellAdmissionProgramResetStatusSource program_reset_status_source = {},
      std::function<Result<void>(std::shared_ptr<IIoSignalService>*)> required_health_loss_observer = {});

  /// Process-local health/current-cycle admission under this existing owner's
  /// mutex. Kernel acquires it before state_mutex_ and holds it only across
  /// bounded snapshot admission, never journal/host I/O or Stop dispatch.
  class ProgramBlackboardAdmissionGuard final {
   public:
    ProgramBlackboardAdmissionGuard(ProgramBlackboardAdmissionGuard&&) = default;
    ProgramBlackboardAdmissionGuard& operator=(ProgramBlackboardAdmissionGuard&&) = default;
    ProgramBlackboardAdmissionGuard(const ProgramBlackboardAdmissionGuard&) = delete;
    ProgramBlackboardAdmissionGuard& operator=(const ProgramBlackboardAdmissionGuard&) = delete;
    [[nodiscard]] const ProgramCycleStatusDTO& cycle() const noexcept { return cycle_; }
    [[nodiscard]] const RetainedBundleId& retained_bundle_id() const noexcept { return bundle_; }

   private:
    friend class KernelCellAdmissionOwner;
    ProgramBlackboardAdmissionGuard(std::unique_lock<std::mutex> lock, ProgramCycleStatusDTO cycle,
                                    RetainedBundleId bundle)
        : lock_(std::move(lock)), cycle_(std::move(cycle)), bundle_(std::move(bundle)) {}
    std::unique_lock<std::mutex> lock_;
    ProgramCycleStatusDTO cycle_;
    RetainedBundleId bundle_;
  };
  [[nodiscard]] Result<ProgramBlackboardAdmissionGuard> AcquireProgramBlackboardAdmission() const;

  /// Same owner-held managed-product currentness cut, without importing the
  /// Program/Runner/cycle permissions of blackboard publication. Global rules
  /// may exist without an Applied Program or Run. No I/O/effect is permitted
  /// while this guard is retained.
  class ManagedProductAdmissionGuard final {
   public:
    ManagedProductAdmissionGuard(ManagedProductAdmissionGuard&&) = default;
    ManagedProductAdmissionGuard& operator=(ManagedProductAdmissionGuard&&) = default;
    ManagedProductAdmissionGuard(const ManagedProductAdmissionGuard&) = delete;
    ManagedProductAdmissionGuard& operator=(const ManagedProductAdmissionGuard&) = delete;
    [[nodiscard]] const RetainedBundleId& retained_bundle_id() const noexcept { return bundle_; }

   private:
    friend class KernelCellAdmissionOwner;
    ManagedProductAdmissionGuard(std::unique_lock<std::mutex> lock, RetainedBundleId bundle)
        : lock_(std::move(lock)), bundle_(std::move(bundle)) {}
    std::unique_lock<std::mutex> lock_;
    RetainedBundleId bundle_;
  };
  [[nodiscard]] Result<ManagedProductAdmissionGuard> AcquireManagedProductAdmission() const;

  /// Process-local proof; only this owner can construct it. Journal I/O is
  /// completed before the composing Kernel takes submission/admission locks.
  class PreparedProgramResetStatus final {
   public:
    PreparedProgramResetStatus(const PreparedProgramResetStatus&) = default;
    PreparedProgramResetStatus(PreparedProgramResetStatus&&) = default;
    PreparedProgramResetStatus& operator=(const PreparedProgramResetStatus&) = default;
    PreparedProgramResetStatus& operator=(PreparedProgramResetStatus&&) = default;
   private:
    friend class KernelCellAdmissionOwner;
    explicit PreparedProgramResetStatus(const CellAdmissionClaimIntentDTO& accepted_intent)
        : intent(accepted_intent) {}
    const KernelCellAdmissionOwner* owner{};
    std::size_t manager_current_count{};
    AdmissionGeneration generation{0U};
    CellAdmissionReadinessSnapshotDTO readiness;
    std::optional<ManagerLivenessRequiredSetDTO> manager;
    ProgramResetJournalReadPurpose purpose{ProgramResetJournalReadPurpose::kAcquire};
    CellAdmissionClaimIntentDTO intent;
    std::optional<CellAdmissionClaimDTO> claim;
    std::optional<ProgramCycleStatusDTO> status;
  };
  [[nodiscard]] Result<PreparedProgramResetStatus> PrepareProgramResetStatus(
      ProgramResetJournalReadPurpose purpose, const CellAdmissionClaimIntentDTO& intent,
      const std::optional<CellAdmissionClaimDTO>& claim = std::nullopt) const;
  [[nodiscard]] Result<PreparedProgramResetStatus> PrepareProgramResetSceneAuthorization(
      const ProgramResetSceneAuthorizationDTO& authorization) const;
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquirePreparedProgramResetClaim(
      const CellAdmissionClaimIntentDTO& intent, const PreparedProgramResetStatus& proof);
  [[nodiscard]] Result<void> ValidatePreparedProgramResetSceneAuthorization(
      const ProgramResetSceneAuthorizationDTO& authorization,
      const PreparedProgramResetStatus& proof) const;

  [[nodiscard]] Result<PreparedProgramResetStatus> PrepareProgramResetPositionAuthorization(
      const ProgramResetPositionAuthorizationDTO& authorization) const;
  [[nodiscard]] Result<void> ValidatePreparedProgramResetPositionAuthorization(
      const ProgramResetPositionAuthorizationDTO& authorization,
      const PreparedProgramResetStatus& proof) const;

  [[nodiscard]] AuthoringAdmissionDTO GetAuthoringAdmission() const override;
  /// Sticky owner-cache storage uncertainty; status only, cleared by restart.
  [[nodiscard]] std::optional<ErrorDTO> persistence_outcome_unknown() const;
  [[nodiscard]] Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO& snapshot,
      std::shared_ptr<IIoSignalService>* actual_retired_service = nullptr) override;
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) override;
  /// Process-local Kernel composition: validate its freshly resolved manual
  /// offer without storing a second pending-offer registry.
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireManualCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const CellAdmissionInterventionValidator& validate_offer);
  /// Ordinary configuration convergence under exact current Manager/host and
  /// immutable-terminal proofs. All old held rows retain their exclusion.
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireConfigurationApplyClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const CellAdmissionConfigurationApplyValidator& validate);
  /// Called only after fresh success has released every exact old fence.
  /// Keeps the fresh claim held until its ordinary release.
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReleaseTerminalApplyClaimsAfterSuccess(
      const CellAdmissionClaimDTO& fresh_claim,
      const CellAdmissionClaimReleaseDTO& fresh_success_release,
      const CellAdmissionConfigurationApplySuccessValidator& validate);
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
  [[nodiscard]] bool LatchManagerLivenessLoss(
      const ManagerLivenessLossIntentDTO& intent,
      std::optional<ErrorDTO>* source_invalidation_error = nullptr) noexcept;

  [[nodiscard]] bool has_unresolved_claims() const;
  [[nodiscard]] bool has_unresolved_claims_except(CellAdmissionOwnerRoleDTO owner_role) const;
  /// Kernel-owned attachment barrier; Manager readiness cannot clear it.
  [[nodiscard]] Result<void> SetRouteRecoveryPending(bool pending);
  [[nodiscard]] bool route_recovery_pending() const;
  /// A required Manager was lost (`LatchManagerLivenessLoss`) and hasn't
  /// reported current since. Its next current status or a new required set
  /// (a restarted Manager) ends it.
  [[nodiscard]] bool manager_liveness_lost() const;
  /// No I/O: validate provenance before a composing owner retains a fault.
  [[nodiscard]] Result<void> ValidateProgramRunnerContactLossSource(
      const ProgramRunnerContactLossDTO& loss) const;
  /// Current held authority, excluding released and reconciled predecessors.
  [[nodiscard]] std::optional<CellAdmissionClaimDTO> held_claim(
      CellAdmissionClaimKindDTO kind) const;
  [[nodiscard]] std::optional<CellAdmissionClaimDTO> held_claim(
      CellAdmissionClaimKindDTO kind, const std::string& owner_transaction_id) const;
  [[nodiscard]] std::vector<CellAdmissionClaimDTO> held_claims(
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
  friend class ResourceAddressedExecutionKernel;
  // The existing owner invalidates under its mutex; only Kernel carries this
  // ephemeral affected-service pin to a post-lock scheduler notification.
  [[nodiscard]] Result<void> Publish(
      const ManagerLivenessObservationDTO& observation,
      std::shared_ptr<IIoSignalService>* actual_retired_service);
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReplaceManagerLivenessRequiredSet(
      const ManagerLivenessRequiredSetDTO& required_set,
      std::shared_ptr<IIoSignalService>* actual_retired_service);
  [[nodiscard]] bool LatchManagerLivenessLoss(
      const ManagerLivenessLossIntentDTO& intent,
      std::optional<ErrorDTO>* source_invalidation_error,
      std::shared_ptr<IIoSignalService>* actual_retired_service) noexcept;
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const CellAdmissionInterventionValidator& validate_offer,
      const CellAdmissionConfigurationApplyValidator& configuration_apply_validator = {},
      const PreparedProgramResetStatus* reset_proof = nullptr);
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
  [[nodiscard]] Result<ProgramCycleStatusDTO> CurrentResetLocked(
      ProgramResetJournalReadPurpose purpose, const CellAdmissionClaimIntentDTO& intent,
      const std::optional<CellAdmissionClaimDTO>& claim,
      const PreparedProgramResetStatus& proof) const;
  [[nodiscard]] bool MatchingProgramResetLocked(const CellAdmissionClaimIntentDTO& intent,
      const PreparedProgramResetStatus& proof) const;
  [[nodiscard]] const Entry* FindReplayLocked(const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] Entry* FindHeldLocked(CellAdmissionClaimGeneration generation);
  [[nodiscard]] const Entry* ParentCycleLocked(const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] bool HasUnsettledChildrenLocked(
      CellAdmissionClaimGeneration parent,
      const std::vector<CellAdmissionReconcileSettlementDTO>& prospective = {}) const;
  [[nodiscard]] bool IsStaleOwnerLocked(const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] Result<void> AdvanceGenerationLocked();
  void CloseForPersistenceUnknownLocked(ErrorDTO&& error, const CellAdmissionClaimDTO& claim);

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
  CellAdmissionProgramResetStatusSource program_reset_status_source_;
  CellAdmissionReconcileValidator reconcile_validator_;
  CellAdmissionProgramNodeValidator program_node_validator_;
  // Fixed at Kernel composition and destroyed with this owner. Runs while
  // mutex_ is held: bounded state -> signals invalidation only, with no owner
  // reentry, journal/host I/O or Stop dispatch.
  std::function<Result<void>(std::shared_ptr<IIoSignalService>*)> required_health_loss_observer_;
  mutable std::mutex mutex_;
  mutable std::mutex liveness_transition_mutex_;
  CellAdmissionReadinessSnapshotDTO readiness_;
  bool route_recovery_pending_{};
  std::optional<ErrorDTO> persistence_outcome_unknown_;
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
