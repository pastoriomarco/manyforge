#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_roboplan/execution_host_support.hpp>

namespace manyforge::roboplan_backend {

class RetainedObservationWake;

/// Inputs for the stable cell-level native owner. Product-specific participant
/// engines are constructed elsewhere; this owner needs only their verified
/// registrations and the exact Kernel resolver.
struct RetainedCellRuntimeInput final {
  std::shared_ptr<WallClock> clock;
  std::shared_ptr<SimpleScheduler> scheduler;
  std::filesystem::path operation_journal_path;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      participant_registrations;
  core::ResourceAddressedExecutionKernelConfig kernel_config;
};

/// Process-stable cell authority, independent of any motion planner or robot
/// runtime. It owns the shared time services, observation wake, operation
/// journal, and addressed Kernel. Participant engines are non-owning
/// registration targets and must outlive this owner until its Kernel drains.
///
/// The Kernel is destroyed first so it can unsubscribe from every registered
/// engine while the journal, scheduler, and engine owners remain alive.
class RetainedCellRuntime final {
 public:
  explicit RetainedCellRuntime(RetainedCellRuntimeInput input);
  ~RetainedCellRuntime();

  RetainedCellRuntime(const RetainedCellRuntime &) = delete;
  RetainedCellRuntime &operator=(const RetainedCellRuntime &) = delete;
  RetainedCellRuntime(RetainedCellRuntime &&) = delete;
  RetainedCellRuntime &operator=(RetainedCellRuntime &&) = delete;

  [[nodiscard]] std::shared_ptr<WallClock> clock() const noexcept;
  [[nodiscard]] std::shared_ptr<SimpleScheduler> scheduler() const noexcept;
  /// Every retained, added, or replaced component shares this cell stream.
  /// Component publication changes must not create a second wake incarnation.
  [[nodiscard]] std::shared_ptr<RetainedObservationWake>
  observation_wake() const noexcept;
  [[nodiscard]] core::SqliteOperationJournal &operation_journal() noexcept;
  [[nodiscard]] const core::SqliteOperationJournal &
  operation_journal() const noexcept;
  [[nodiscard]] core::ResourceAddressedExecutionKernel &addressed_kernel()
      noexcept;
  [[nodiscard]] const core::ResourceAddressedExecutionKernel &
  addressed_kernel() const noexcept;
  [[nodiscard]] const std::vector<
      core::RuntimeParticipantExecutionRegistration> &
  participant_registrations() const noexcept;
  [[nodiscard]] const core::RuntimeParticipantAdmissionResolver &
  participant_resolver() const noexcept;
  /// Adopt an already-built component-readiness snapshot after the Kernel has
  /// published and exactly read back the matching generation-only re-seal.
  /// This is deliberately a no-fail swap: callers must hold the exact parent
  /// Apply fence through the Kernel publication/readback and this call. In
  /// particular, it must not be used to repair an unverified Kernel result.
  void AdoptVerifiedComponentReadinessSnapshot(
      std::vector<core::RuntimeParticipantExecutionRegistration> &&
          registrations,
      core::RuntimeParticipantAdmissionResolver &&participant_resolver)
      noexcept;

  /// Route product changes through this owner so its retained registration
  /// and resolver snapshots advance only at Kernel publication.
  [[nodiscard]] core::Result<void> reconfigure_runtime_product(
      std::vector<core::RuntimeParticipantExecutionRegistration>
          registrations,
      core::RuntimeParticipantAdmissionResolver participant_resolver,
      std::function<core::Result<void>()> commit_publication = {});

  /// Install a Manager-selected component delta beneath its exact held fence.
  /// The optional commit callback executes at the Kernel publication boundary.
  [[nodiscard]] core::Result<void> reconfigure_runtime_component_set(
      std::vector<core::RuntimeParticipantExecutionRegistration> registrations,
      core::RuntimeParticipantAdmissionResolver participant_resolver,
      core::RetainedBundleId candidate_retained_bundle_id,
      core::GraphRevision candidate_graph_revision,
      const core::ConfigurationFenceId &apply_fence_id,
      const core::ConfigurationFenceScopeDTO &scope,
      const std::vector<core::ControllableComponentId> &retained_components,
      std::function<core::Result<void>()> commit_publication = {});

private:
  /// Declaration order is a lifetime contract: the Kernel is last and is
  /// therefore retired before these values during any unwinding path.
  std::shared_ptr<WallClock> clock_;
  std::shared_ptr<SimpleScheduler> scheduler_;
  std::shared_ptr<RetainedObservationWake> observation_wake_;
  std::unique_ptr<core::SqliteOperationJournal> operation_journal_;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      participant_registrations_;
  core::RuntimeParticipantAdmissionResolver participant_resolver_;
  std::unique_ptr<core::ResourceAddressedExecutionKernel> addressed_kernel_;
};

}  // namespace manyforge::roboplan_backend
