#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_core/ports/icclc_kernel_disable_handoff_port.hpp>
#include <manyforge_core/ports/icclc_kernel_route_configuration_fence_port.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <memory>
#include <mutex>
#include <vector>

namespace manyforge::core {
class ResourceAddressedExecutionKernel;
}

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
      const std::function<void()> &commit_durable_fence);
  void AcquireRouteFenceDurably(
      const manyforge::core::ConfigurationFenceId &fence_id,
      const manyforge::core::ConfigurationFenceScopeDTO &scope,
      std::uint64_t expected_operation_resource_generation,
      const manyforge::core::ExpectedActiveControlConfigurationDTO
          &expected_active_control_configuration,
      const std::function<void()> &commit_durable_fence);
  [[nodiscard]] ManagedKernelAdmissionObservation AcquireDisableFenceDurably(
      const manyforge::core::ConfigurationFenceId &fence_id,
      const manyforge::core::ConfigurationFenceScopeDTO &scope,
      const std::function<void(const ManagedKernelAdmissionObservation &)>
          &commit_durable_fence);
  void WidenDisableFenceDurably(
      const manyforge::core::ConfigurationFenceId &fence_id,
      const manyforge::core::ConfigurationFenceScopeDTO &prior_scope,
      const manyforge::core::ConfigurationFenceScopeDTO &widened_scope,
      const std::function<void(const ManagedKernelAdmissionObservation &)>
          &commit_durable_widening);
  void ReleaseDisableFenceDurably(
      const manyforge::core::ConfigurationFenceId &fence_id,
      const manyforge::core::ConfigurationFenceScopeDTO &scope,
      const std::function<void(const ManagedKernelAdmissionObservation &)>
          &commit_durable_release);
  void RestoreFence(const manyforge::core::ConfigurationFenceId &fence_id,
                    const manyforge::core::ConfigurationFenceScopeDTO &scope);
  void ReleaseFence(const manyforge::core::ConfigurationFenceId &fence_id,
                    const manyforge::core::ConfigurationFenceScopeDTO &scope);

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
};

/// Durable Kernel-owned Apply and route-configuration fence lifecycle for the
/// selected managed native execution host. Both surfaces share one generation,
/// request ledger, and admission gate.
class ManagedKernelApplyFenceOwner final
    : public manyforge::core::IManagerKernelApplyFencePort,
      public manyforge::core::ICCLCKernelRouteConfigurationFencePort,
      public manyforge::core::ICCLCKernelDisableHandoffPort {
public:
  ManagedKernelApplyFenceOwner(ManagedKernelApplyFenceOwnerConfig config,
                               ManagedKernelAdmissionGate &admission_gate);
  ~ManagedKernelApplyFenceOwner() override;

  [[nodiscard]] manyforge::core::ConfigurationFenceAdmissionSnapshotDTO
  ObserveAdmission(const manyforge::core::ConfigurationFenceAdmissionQueryDTO
                       &query) const override;
  [[nodiscard]] manyforge::core::ConfigurationFenceAcceptedDTO Acquire(
      const manyforge::core::ApplyConfigurationFenceIntentDTO &intent) override;
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

  /// The post-publication half of the same parent Apply interlock. The
  /// successor receipt must name that Apply and still be the active pointer;
  /// the parent fence remains held throughout the delegated operation.
  void RunWithExactHeldSuccessor(
      const manyforge::core::ConfigurationFenceStatusQueryDTO &query,
      const manyforge::core::ActiveBundlePublicationReceiptDTO &successor,
      const std::function<void()> &effect) const;

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

  /// Bind the one stable addressed Kernel object after physical composition
  /// creates it. Reconfiguration moves ownership but preserves this object;
  /// rebinding to another address is rejected.
  void BindDisableHandoffKernel(
      manyforge::core::ResourceAddressedExecutionKernel &kernel);

  [[nodiscard]] manyforge::core::Result<
      manyforge::core::DisableHandoffAdmissionSnapshotDTO>
  ObserveDisableHandoffAdmission(
      const manyforge::core::DisableHandoffAdmissionQueryDTO &query)
      const override;
  [[nodiscard]]
  manyforge::core::Result<manyforge::core::DisableHandoffAcceptedDTO>
  BeginDisableHandoff(
      const manyforge::core::DisableHandoffRequestDTO &request) override;
  [[nodiscard]]
  manyforge::core::Result<manyforge::core::DisableHandoffStatusDTO>
  DisableHandoffStatus(
      const manyforge::core::LifecycleTransactionId &lifecycle_transaction_id,
      manyforge::core::LifecycleTransactionGeneration
          lifecycle_transaction_generation) override;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::DisableHandoffAttemptReconciliationDTO>
  ReconcileDisableHandoffAttempt(
      const manyforge::core::DisableHandoffRequestDTO &request) override;
  [[nodiscard]]
  manyforge::core::Result<manyforge::core::DisableFenceReleasedDTO>
  ReleaseDisableFence(
      const manyforge::core::DisableFenceReleaseRequestDTO &request) override;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::DisableFenceReleaseAttemptReconciliationDTO>
  ReconcileDisableFenceReleaseAttempt(
      const manyforge::core::DisableFenceReleaseRequestDTO &request) override;

  ManagedKernelApplyFenceOwner(const ManagedKernelApplyFenceOwner &) = delete;
  ManagedKernelApplyFenceOwner &
  operator=(const ManagedKernelApplyFenceOwner &) = delete;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace manyforge::roboplan_backend
