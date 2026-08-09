#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <memory>
#include <mutex>
#include <vector>

namespace manyforge::roboplan_backend {

struct ManagedKernelAdmissionObservation final {
  std::uint64_t operation_resource_generation;
  std::vector<manyforge::core::DomainActiveControlConfigurationDTO>
      active_control_configurations;
  std::uint64_t affected_queued_operation_count;
  std::uint64_t affected_active_operation_count;
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
  [[nodiscard]] manyforge::core::Result<void> reserve(
      manyforge::core::ResourceSlotTable &table,
      const manyforge::core::ResourceSlotOwnerDTO &owner,
      const std::vector<manyforge::core::ResourceId> &resources) override;

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
      const std::vector<
          manyforge::core::DomainActiveControlConfigurationDTO>
          &expected_active_control_configurations,
      const std::function<void()> &commit_durable_fence);
  void RestoreFence(const manyforge::core::ConfigurationFenceId &fence_id,
                    const manyforge::core::ConfigurationFenceScopeDTO &scope);
  void ReleaseFence(const manyforge::core::ConfigurationFenceId &fence_id,
                    const manyforge::core::ConfigurationFenceScopeDTO &scope);

  ManagedKernelAdmissionGate(const ManagedKernelAdmissionGate &) = delete;
  ManagedKernelAdmissionGate &
  operator=(const ManagedKernelAdmissionGate &) = delete;

private:
  struct HeldFence final {
    manyforge::core::ConfigurationFenceId fence_id;
    manyforge::core::ConfigurationFenceScopeDTO scope;
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
};

/// Durable Kernel-owned Apply-fence lifecycle for the selected managed native
/// execution host.
class ManagedKernelApplyFenceOwner final
    : public manyforge::core::IManagerKernelApplyFencePort {
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
  [[nodiscard]] manyforge::core::ConfigurationFenceReleasedDTO Release(
      const manyforge::core::ConfigurationFenceReleaseDTO &request) override;

  ManagedKernelApplyFenceOwner(const ManagedKernelApplyFenceOwner &) = delete;
  ManagedKernelApplyFenceOwner &
  operator=(const ManagedKernelApplyFenceOwner &) = delete;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace manyforge::roboplan_backend
