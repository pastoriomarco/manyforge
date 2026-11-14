#pragma once

#include <cstdint>
#include <functional>
#include <manyforge_core/journal_record.hpp>
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
    const CellAdmissionReadinessSnapshotDTO&)>;
using CellAdmissionConfigurationApplySuccessValidator =
    std::function<Result<void>(const CellAdmissionClaimDTO&, const CellAdmissionClaimReleaseDTO&,
                               std::span<const CellAdmissionClaimDTO>)>;
using CellAdmissionProgramNodeValidator =
    std::function<Result<void>(const ProgramNodeAdmissionEvidenceDTO&)>;
/// Settlement proof for the claim kinds whose owner keeps durable evidence
/// (Manager Apply, Authored Scene). Program-cycle and Program-Reset releases
/// never reach it: their exact held identity, kind and children are the
/// whole proof (OD-139).
using CellAdmissionSettlementValidator =
    std::function<Result<void>(const CellAdmissionClaimDTO&, const CellAdmissionClaimReleaseDTO&)>;
/// Hands claim records to the journal writer. Called only after the owner
/// released both of its mutexes.
using CellAdmissionJournalPoster = std::function<void(std::span<JournalRecord>)>;

/// The sole focused cell-admission ledger composed inside Kernel. Kernel takes
/// its shared admission mutex before this state mutex; Stop recognition and
/// forensic projection take neither in that order and retain their reserve.
class KernelCellAdmissionOwner final : public IKernelCellAdmissionPort,
                                       public IManagerLivenessObservationPort {
 public:
  KernelCellAdmissionOwner(
      std::size_t capacity, IClock& clock, CellAdmissionJournalPoster post_records,
      CellAdmissionInterventionValidator intervention_validator = {},
      CellAdmissionSettlementValidator settlement_validator = {},
      std::optional<ManagerLivenessRequiredSetDTO> manager_liveness_required_set = std::nullopt,
      CellAdmissionProgramNodeValidator program_node_validator = {},
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

  /// Before a Program Reset claim: an exact replay needs no current status
  /// (nullopt); otherwise the relayed cycle status must be current. Yields the
  /// exact run of a currently matching Reset, for motion-free cleanup. Memory
  /// only; the carried status is the Program runner's (OD-139).
  [[nodiscard]] Result<std::optional<ProgramRunNumber>> ProgramResetRunForCleanup(
      const CellAdmissionClaimIntentDTO& intent,
      const ProgramCycleStatusDTO& program_cycle_status) const;
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireProgramResetClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const ProgramCycleStatusDTO& program_cycle_status) override;
  [[nodiscard]] Result<void> ValidateProgramResetPositionAuthorization(
      const ProgramResetPositionAuthorizationDTO& authorization) const;

  [[nodiscard]] AuthoringAdmissionDTO GetAuthoringAdmission() const override;
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
  [[nodiscard]] Result<CellAdmissionRecoveryReceiptDTO> GetCellAdmissionRecoveryReceipt(
      const CellAdmissionRecoveryReceiptQueryDTO& query) const override;
  [[nodiscard]] Result<void> ValidateProgramResetSceneAuthorization(
      const ProgramResetSceneAuthorizationDTO& authorization) const override;
  [[nodiscard]] Result<void> Publish(const ManagerLivenessObservationDTO& observation) override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReplaceManagerLivenessRequiredSet(
      const ManagerLivenessRequiredSetDTO& required_set);

  [[nodiscard]] bool has_unresolved_claims() const;
  [[nodiscard]] bool has_unresolved_claims_except(ApplicationRole owner_role) const;
  /// Kernel-owned attachment barrier; Manager readiness cannot clear it.
  [[nodiscard]] Result<void> SetRouteRecoveryPending(bool pending);
  [[nodiscard]] bool route_recovery_pending() const;
  /// No I/O: validate provenance before a composing owner retains a fault.
  [[nodiscard]] Result<void> ValidateProgramRunnerContactLossSource(
      const ProgramRunnerContactLossDTO& loss) const;
  /// Current held authority, excluding released claims.
  [[nodiscard]] std::optional<CellAdmissionClaimDTO> held_claim(
      CellAdmissionClaimKindDTO kind) const;
  [[nodiscard]] std::optional<CellAdmissionClaimDTO> held_claim(
      CellAdmissionClaimKindDTO kind,
      const CellAdmissionOwnerTransactionId& owner_transaction_id) const;
  [[nodiscard]] std::vector<CellAdmissionClaimDTO> held_claims(
      CellAdmissionClaimKindDTO kind) const;
  /// Bounded ordinary-release snapshot, never read by physical Stop. An empty
  /// node identity denotes an unscoped child (e.g. intervention/recovery).
  [[nodiscard]] std::vector<std::string> unsettled_program_child_nodes(
      CellAdmissionClaimGeneration parent) const;

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
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const CellAdmissionInterventionValidator& validate_offer,
      const CellAdmissionConfigurationApplyValidator& configuration_apply_validator = {},
      const ProgramCycleStatusDTO* program_reset_status = nullptr);
  struct Entry final {
    CellAdmissionClaimDTO claim;
    std::optional<CellAdmissionClaimReleaseDTO> release;
  };
  /// A request's identity and intent, kept after its released claim is
  /// compacted so a resend is still recognised.
  struct Membership final {
    ApplicationRole owner_role{ApplicationRole::kAuthoringOwner};
    RequestKey request_key;
    ContentDigest semantic_intent_digest;
  };

  [[nodiscard]] AuthoringAdmissionDTO AdmissionLocked() const noexcept;
  [[nodiscard]] bool NonCycleReadinessLocked() const noexcept;
  [[nodiscard]] bool ManualParentReadyLocked(const Entry* parent) const noexcept;
  [[nodiscard]] bool BaseReadinessLocked() const noexcept;
  [[nodiscard]] bool MatchingStartingProgramCycleLocked(
      const CellAdmissionClaimIntentDTO& intent) const noexcept;
  [[nodiscard]] Result<ProgramCycleStatusDTO> CurrentResetLocked(
      const CellAdmissionClaimIntentDTO& intent, const ProgramCycleStatusDTO& carried) const;
  [[nodiscard]] bool MatchingProgramResetLocked(const CellAdmissionClaimIntentDTO& intent,
      const ProgramCycleStatusDTO& carried) const;
  [[nodiscard]] const Entry* FindReplayLocked(const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] Entry* FindHeldLocked(CellAdmissionClaimGeneration generation);
  [[nodiscard]] const Entry* ParentCycleLocked(const CellAdmissionClaimIntentDTO& intent) const;
  [[nodiscard]] bool HasUnsettledChildrenLocked(CellAdmissionClaimGeneration parent) const;
  [[nodiscard]] Result<void> AdvanceGenerationLocked();

  struct ManagerLivenessRow final {
    RequiredManagerLivenessReceiverDTO receiver;
    std::uint64_t status_sequence{};
    bool current_unfenced{};
    bool generation_advance_pending{};
    std::uint64_t generation_pending_sequence{};
    bool generation_pending_current{};
  };

  std::size_t capacity_;
  IClock& clock_;
  CellAdmissionJournalPoster post_records_;
  CellAdmissionInterventionValidator intervention_validator_;
  CellAdmissionSettlementValidator settlement_validator_;
  CellAdmissionProgramNodeValidator program_node_validator_;
  // Fixed at Kernel composition and destroyed with this owner. Runs while
  // mutex_ is held: bounded state -> signals invalidation only, with no owner
  // reentry, journal/host I/O or Stop dispatch.
  std::function<Result<void>(std::shared_ptr<IIoSignalService>*)> required_health_loss_observer_;
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
  std::vector<Membership> membership_;
};

}  // namespace manyforge::core
