#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_core/ports/icclc_kernel_route_configuration_fence_port.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/types/provider_candidate_facts.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/runtime_domain_configuration.hpp>
#include <memory>
#include <mutex>
#include <vector>

namespace manyforge::roboplan_backend {

struct ManagedKernelAdmissionObservation final {
  std::uint64_t operation_resource_generation;
  std::vector<manyforge::core::DomainActiveControlConfigurationDTO>
      active_control_configurations;
  std::vector<manyforge::core::DomainActiveControlConfigurationDTO>
      route_published_active_control_configurations;
  std::uint64_t affected_queued_operation_count;
  std::uint64_t affected_active_operation_count;

  friend bool operator==(const ManagedKernelAdmissionObservation &,
                         const ManagedKernelAdmissionObservation &) = default;
};

/// Process-local atomic admission gate shared by the managed execution owner
/// and its durable Apply-fence owner.
///
/// N7-a3 uses it while the managed command surface remains closed. N7-a4
/// joins operation admission to this same gate; it must not create a second
/// resource/fence truth.
using ManagedTerminalApplyFenceValidator =
    std::function<manyforge::core::Result<void>(
        const manyforge::core::ApplyConfigurationFenceIntentDTO &,
        const manyforge::core::ConfigurationFenceAcceptedDTO &)>;

class ManagedKernelAdmissionGate final
    : public manyforge::core::IResourceOperationAdmissionGate {
public:
  ManagedKernelAdmissionGate();

  [[nodiscard]] std::shared_ptr<std::recursive_mutex>
  shared_admission_mutex() const override;
  [[nodiscard]] manyforge::core::Result<void>
  reserve(manyforge::core::ResourceSlotTable &table,
          const manyforge::core::ResourceSlotOwnerDTO &owner,
          const std::vector<manyforge::core::ResourceId> &resources) override;
  [[nodiscard]] manyforge::core::Result<void> reserve_batch(
      manyforge::core::ResourceSlotTable &table,
      const std::vector<manyforge::core::ResourceSlotReservationOwnerDTO>
          &owners) override;
  [[nodiscard]] std::vector<manyforge::core::ResourceId>
  current_fenced_resources(
      const std::vector<manyforge::core::ResourceId> &resources) const override;
  [[nodiscard]] bool exact_fence_covers(
      const manyforge::core::ConfigurationFenceId &fence_id,
      const std::vector<manyforge::core::ResourceId> &resources) const override;
  [[nodiscard]] bool has_held_configuration_fences() const override;
  [[nodiscard]] bool exact_fence_is_held(
      const manyforge::core::ConfigurationFenceId &fence_id,
      const manyforge::core::ConfigurationFenceScopeDTO &scope) const override;

  /// Attach the exact table owned by the currently composed addressed Kernel.
  /// The gate keeps only a weak reference and owns no duplicate operation or
  /// configuration facts.
  void BindResourceTable(
      std::shared_ptr<const manyforge::core::ResourceSlotTable> table);

  [[nodiscard]] ManagedKernelAdmissionObservation
  Observe(const manyforge::core::ConfigurationFenceScopeDTO &scope) const;

  void AcquireFenceDurably(
      const manyforge::core::ConfigurationFenceId &fence_id,
      const manyforge::core::ConfigurationFenceScopeDTO &scope,
      std::uint64_t expected_operation_resource_generation,
      const std::vector<manyforge::core::DomainActiveControlConfigurationDTO>
          &expected_active_control_configurations,
      const std::function<void()> &commit_durable_fence,
      const std::function<void(const manyforge::core::ConfigurationFenceId &,
                               const manyforge::core::ConfigurationFenceScopeDTO
                                   &)> &validate_terminal_overlap = {});
  void AcquireRouteFenceDurably(
      const manyforge::core::ConfigurationFenceId &fence_id,
      const manyforge::core::ConfigurationFenceScopeDTO &scope,
      std::uint64_t expected_operation_resource_generation,
      const manyforge::core::ExpectedActiveControlConfigurationDTO
          &expected_active_control_configuration,
      const std::function<void()> &commit_durable_fence);
  void RestoreFence(const manyforge::core::ConfigurationFenceId &fence_id,
                    const manyforge::core::ConfigurationFenceScopeDTO &scope);
  void ReleaseFence(const manyforge::core::ConfigurationFenceId &fence_id,
                    const manyforge::core::ConfigurationFenceScopeDTO &scope);

  /// Selected release checks the effect pin before committing its ledger row.
  void
  ReleaseFenceDurably(const manyforge::core::ConfigurationFenceId &fence_id,
                      const manyforge::core::ConfigurationFenceScopeDTO &scope,
                      const std::function<void()> &commit_durable_release);

  /// Execute one bounded in-process installation step only while the exact
  /// durable fence remains held. This is an Apply interlock, not caller
  /// authorization; the fence owner separately validates its generation.
  void RunWithExactHeldFence(
      const manyforge::core::ConfigurationFenceId &fence_id,
      const manyforge::core::ConfigurationFenceScopeDTO &scope,
      const std::function<void()> &effect);

  ManagedKernelAdmissionGate(const ManagedKernelAdmissionGate &) = delete;
  ManagedKernelAdmissionGate &
  operator=(const ManagedKernelAdmissionGate &) = delete;

private:
  struct HeldFence final {
    manyforge::core::ConfigurationFenceId fence_id;
    manyforge::core::ConfigurationFenceScopeDTO scope;
    std::size_t active_effect_count{};
  };

  std::shared_ptr<std::recursive_mutex> admission_mutex_;
  std::weak_ptr<const manyforge::core::ResourceSlotTable> resource_table_;
  bool resource_table_was_bound_{false};
  std::vector<HeldFence> held_fences_;
};

struct ManagedKernelApplyFenceOwnerConfig final {
  std::filesystem::path ledger_path;
  std::filesystem::path manager_state_root;
  std::filesystem::path retained_package_root;
  manyforge::core::CellId cell_id;
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RequestOwnerEpoch request_owner_epoch;
  std::function<std::int64_t()> now_ns;
  // During a managed Apply the child is launched from the activation root,
  // while the public pointer still names the predecessor.  Keep the
  // activation root as a bounded fallback so the Kernel can verify either
  // exact retained bundle during that transition.  Standalone owners leave
  // this empty.
  std::filesystem::path candidate_retained_package_root{};
  // The current composed Kernel owns this lookup and its lifetime. Absence
  // refuses selected effects; a generation alone is never held-claim proof.
  std::function<
      manyforge::core::Result<manyforge::core::CellAdmissionRecoveryReceiptDTO>(
          const manyforge::core::CellAdmissionRecoveryReceiptQueryDTO &)>
      selected_claim_recovery_receipt{};
  // Stable Kernel inventory; callback takes only its state mutex. Closed
  // installed metadata proves identity, never operation eligibility.
  std::function<manyforge::core::Result<std::optional<
      manyforge::core::RuntimeProviderDomainConfigurationReadbackDTO>>(
      const manyforge::core::ControlDomainId &)>
      provider_configuration_source{};
};

/// Durable Kernel-owned Apply, route and selected bring-up fence lifecycle.
/// These typed owners share one generation, request ledger and admission gate.
class ManagedKernelApplyFenceOwner final
    : public manyforge::core::IManagerKernelApplyFencePort,
      public manyforge::core::ICCLCKernelRouteConfigurationFencePort {
public:
  ManagedKernelApplyFenceOwner(ManagedKernelApplyFenceOwnerConfig config,
                               ManagedKernelAdmissionGate &admission_gate);
  ~ManagedKernelApplyFenceOwner() override;

  [[nodiscard]] manyforge::core::ConfigurationFenceAdmissionSnapshotDTO
  ObserveAdmission(const manyforge::core::ConfigurationFenceAdmissionQueryDTO
                       &query) const override;
  [[nodiscard]] manyforge::core::ConfigurationFenceAcceptedDTO Acquire(
      const manyforge::core::ApplyConfigurationFenceIntentDTO &intent) override;
  [[nodiscard]] manyforge::core::ConfigurationFenceAcceptedDTO
  Acquire(const manyforge::core::ApplyConfigurationFenceIntentDTO &intent,
          const ManagedTerminalApplyFenceValidator &validate_terminal_overlap);
  [[nodiscard]]
  manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const manyforge::core::ApplyFenceAcquireAttemptQueryDTO &query)
      const override;
  [[nodiscard]] manyforge::core::ApplyFenceReconcileResultDTO
  Rehydrate(const manyforge::core::ApplyFenceRehydrateDTO &request) override;
  [[nodiscard]] manyforge::core::ConfigurationFenceStatusDTO
  QueryStatus(const manyforge::core::ConfigurationFenceStatusQueryDTO &query)
      const override;

  /// Private observation of the SAME installed ordinary NativeApply while
  /// its verified predecessor is still public. Facts come from the original
  /// facade's fresh full-plan reader, never a caller-created candidate receipt.
  /// The bounded callback rechecks only actual installed request/facade/group
  /// identity under the existing ledger->composition order, before admission.
  [[nodiscard]] manyforge::core::ConfigurationFenceStatusDTO
  QueryInstalledNativeApplyCandidateStatus(
      const manyforge::core::ConfigurationFenceStatusQueryDTO &query,
      const manyforge::core::ActiveBundlePublicationReceiptDTO &predecessor,
      const manyforge::core::CellAdmissionClaimDTO &parent,
      const manyforge::core::NativeApplyPlanDTO &expected_plan,
      const std::vector<manyforge::core::InstalledProviderCandidateFactsDTO> &facts,
      const std::function<void()> &revalidate_installed_attempt) const;

  /// Run one selected-runtime installation effect only while the exact
  /// durable Apply fence is held and the active publication is still the
  /// predecessor named by Component Manager. Candidate active-control
  /// configurations are deliberately not required until after the effect:
  /// they are facts produced by installing the candidate participants.
  /// This is an internal Apply interlock, not command authorization.
  void RunWithExactHeldPredecessor(
      const manyforge::core::ConfigurationFenceStatusQueryDTO &query,
      const manyforge::core::ActiveBundlePublicationGeneration
          &expected_predecessor_publication_generation,
      const manyforge::core::ContentDigest
          &expected_predecessor_public_pointer_digest,
      const std::function<void()> &effect) const;

  /// Private post-install handoff before publication: the same parent and
  /// public predecessor remain exact, but current provider rows/absences must
  /// now describe the installed candidate rather than the removed predecessor.
  void RunWithExactHeldInstalledCandidate(
      const manyforge::core::ConfigurationFenceStatusQueryDTO &query,
      manyforge::core::ActiveBundlePublicationGeneration predecessor_generation,
      const manyforge::core::ContentDigest &predecessor_pointer_digest,
      const manyforge::core::CellAdmissionClaimDTO &parent,
      const manyforge::core::ProjectionRevisionDTO &candidate,
      const std::vector<
          manyforge::core::RuntimeProviderDomainConfigurationReadbackDTO>
          &actual_candidate_configurations,
      const std::function<void()> &effect) const;

  /// The post-publication half of the same parent Apply interlock. The
  /// successor receipt must name that Apply and still be the active pointer;
  /// the parent fence remains held throughout the delegated operation.
  /// Original live Apply child only: the host independently reads the actual
  /// original child receipts and retains the complete installed driver group
  /// through the action. Observations alone cannot reconstruct that authority.
  void RunWithExactHeldOriginalApplyCandidate(
      const manyforge::core::ConfigurationFenceStatusQueryDTO &query,
      manyforge::core::ActiveBundlePublicationGeneration predecessor_generation,
      const manyforge::core::ContentDigest &predecessor_pointer_digest,
      const manyforge::core::ApplyComponentBringUpAuthorityDTO &parent,
      const manyforge::core::ProjectionRevisionDTO &candidate,
      const std::vector<
          manyforge::core::RuntimeProviderDomainConfigurationReadbackDTO>
          &actual_candidate_configurations,
      const std::function<void()> &effect) const;

  void RunWithExactHeldSuccessor(
      const manyforge::core::ConfigurationFenceStatusQueryDTO &query,
      const manyforge::core::ActiveBundlePublicationReceiptDTO &successor,
      const std::function<void()> &effect) const;

  /// Callback-local positive readback for private cross-owner operations.
  /// Keeps the same actual interlock as the existing operations and avoids
  /// recursively querying this owner's nonrecursive mutex.
  /// Candidate control configurations must already be installed. Use the
  /// no-readback predecessor overload for preparation/installation itself;
  /// this readback is for Scene adoption after child Holding, before publish.
  void RunWithExactHeldPredecessorReadback(
      const manyforge::core::ConfigurationFenceStatusQueryDTO &query,
      const manyforge::core::ActiveBundlePublicationGeneration &expected_predecessor_publication_generation,
      const manyforge::core::ContentDigest &expected_predecessor_public_pointer_digest,
      const std::function<void(const HeldStatusView&)> &effect) const;
  void RunWithExactHeldSuccessorReadback(
      const manyforge::core::ConfigurationFenceStatusQueryDTO &query,
      const manyforge::core::ActiveBundlePublicationReceiptDTO &successor,
      const std::function<void(const HeldStatusView&)> &effect) const;

  [[nodiscard]] manyforge::core::ConfigurationFenceReleasedDTO Release(
      const manyforge::core::ConfigurationFenceReleaseDTO &request) override;

  [[nodiscard]] manyforge::core::Result<
      manyforge::core::RouteConfigurationFenceAdmissionSnapshotDTO>
  ObserveRouteConfigurationFenceAdmission(
      const manyforge::core::RouteConfigurationFenceAdmissionQueryDTO &query)
      const override;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::RouteConfigurationFenceAcceptedDTO>
  AcquireRouteConfigurationFence(
      const manyforge::core::RouteConfigurationFenceIntentDTO &intent) override;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::RouteConfigurationFenceAcquireAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceAcquireAttempt(
      const manyforge::core::RouteConfigurationFenceIntentDTO &intent)
      const override;
  [[nodiscard]]
  manyforge::core::Result<manyforge::core::RouteConfigurationFenceStatusDTO>
  ObserveRouteConfigurationFenceStatus(
      const manyforge::core::RouteConfigurationFenceStatusQueryDTO &query)
      const override;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::RouteConfigurationFenceReleasedDTO>
  ReleaseRouteConfigurationFence(
      const manyforge::core::RouteConfigurationFenceReleaseRequestDTO &request)
      override;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::RouteConfigurationFenceReleaseAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceReleaseAttempt(
      const manyforge::core::RouteConfigurationFenceReleaseRequestDTO &request)
      const override;

  [[nodiscard]] manyforge::core::Result<
      manyforge::core::ConfigurationFenceAcceptedDTO>
  AcquireSelectedBringUpFence(
      const manyforge::core::SelectedBringUpConfigurationFenceIntentDTO
          &intent);
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO>
  ReconcileSelectedBringUpFenceAcquireAttempt(
      const manyforge::core::SelectedBringUpFenceAcquireAttemptQueryDTO &query)
      const;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::SelectedBringUpConfigurationFenceStatusDTO>
  ObserveSelectedBringUpFenceStatus(
      const manyforge::core::SelectedBringUpConfigurationFenceStatusQueryDTO
          &query) const;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::SelectedBringUpConfigurationFenceReleasedDTO>
  ReleaseSelectedBringUpFence(
      const manyforge::core::SelectedBringUpConfigurationFenceReleaseDTO
          &request);
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO>
  ReconcileSelectedBringUpFenceReleaseAttempt(
      const manyforge::core::SelectedBringUpFenceReleaseAttemptQueryDTO &query)
      const;
  void RunWithExactHeldSelectedPredecessor(
      const manyforge::core::SelectedBringUpConfigurationFenceStatusQueryDTO
          &query,
      const std::function<void()> &effect) const;

  ManagedKernelApplyFenceOwner(const ManagedKernelApplyFenceOwner &) = delete;
  ManagedKernelApplyFenceOwner &
  operator=(const ManagedKernelApplyFenceOwner &) = delete;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace manyforge::roboplan_backend
