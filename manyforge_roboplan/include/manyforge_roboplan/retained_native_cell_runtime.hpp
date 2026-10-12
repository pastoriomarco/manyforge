#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#include <manyforge_roboplan/retained_stop_surveillance_owner.hpp>

namespace manyforge::roboplan_backend {

class RetainedObservationWake;

/// Inputs for the stable cell-level native owner. Product-specific participant
/// engines are constructed elsewhere; this owner needs only their verified
/// registrations and the exact Kernel resolver.
struct RetainedNativeCellRuntimeInput final {
  std::shared_ptr<WallClock> clock;
  std::shared_ptr<SimpleScheduler> scheduler;
  std::filesystem::path operation_journal_path;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      participant_registrations;
  core::ResourceAddressedExecutionKernelConfig kernel_config;
  std::vector<RetainedStopSurveillanceSourceInput> stop_sources;
  RetainedStopSurveillanceEmptySourceMode stop_source_mode{
      RetainedStopSurveillanceEmptySourceMode::kManagedCellBootstrapEmpty};
  core::StopSurveillancePolicyDTO stop_surveillance_policy{
      kMaintainedNativeStopSurveillancePolicy};
};

/// Process-stable native cell authority, independent of any motion planner or
/// robot runtime. It owns the shared time services, cell Stop observer,
/// observation wake, operation journal, and addressed Kernel. Participant engines are non-owning
/// registration targets and must outlive this owner until its Kernel drains.
///
/// The Kernel is destroyed first so it can unsubscribe from every registered
/// engine while the journal, observer, scheduler, and engine owners remain
/// alive.
/// A caller-supplied `kernel_config.stop_fault_recovery_facts_resolver` is
/// preserved verbatim; any external objects captured by that resolver must
/// outlive this cell owner and its Kernel. When omitted, recovery facts are
/// supplied by the owned Stop surveillance observer. Runtime reconfiguration
/// requires the full candidate Stop-source table and installs it at the
/// Kernel's publication callback boundary before returning success.
class RetainedNativeCellRuntime final {
 public:
  explicit RetainedNativeCellRuntime(RetainedNativeCellRuntimeInput input);
  ~RetainedNativeCellRuntime();

  RetainedNativeCellRuntime(const RetainedNativeCellRuntime &) = delete;
  RetainedNativeCellRuntime &operator=(const RetainedNativeCellRuntime &) = delete;
  RetainedNativeCellRuntime(RetainedNativeCellRuntime &&) = delete;
  RetainedNativeCellRuntime &operator=(RetainedNativeCellRuntime &&) = delete;

  /// Check one physical Stop source per backend against the complete capability
  /// inventory before constructor, host preparation, or Kernel publication.
  [[nodiscard]] static core::Result<void> ValidateStopSourceCoverage(
      const std::vector<RetainedStopSurveillanceSourceInput> &sources,
      RetainedStopSurveillanceEmptySourceMode empty_source_mode,
      const std::vector<core::RuntimeParticipantExecutionRegistration>
          &registrations);

  [[nodiscard]] std::shared_ptr<WallClock> clock() const noexcept;
  [[nodiscard]] std::shared_ptr<SimpleScheduler> scheduler() const noexcept;
  /// Every retained, added, or replaced component shares this cell stream.
  /// Component publication changes must not create a second wake incarnation.
  [[nodiscard]] std::shared_ptr<RetainedObservationWake>
  observation_wake() const noexcept;
  [[nodiscard]] std::shared_ptr<RetainedStopSurveillanceOwner>
  stop_surveillance_owner() const noexcept;
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
  /// Compose current recovery facts from this cell's exact Stop-source table,
  /// current Kernel route readbacks, orphan evidence and fenced resources. It
  /// is safe for gripper-only cells and never requires a motion runtime.
  [[nodiscard]] core::Result<core::StopFaultRecoveryFactsDTO>
  BuildStopRecoveryFacts(const core::StopStatusDTO &status,
                         std::int64_t assessed_at_ns);

  /// Resolve only command-free orphan facts from the stable cell observer.
  /// Restart calls this before route recovery can release admission; no
  /// motion owner is required.
  [[nodiscard]] core::Result<void> ReconcileCommandFreeOrphans();

  /// Adopt an already-built component-readiness snapshot after the Kernel has
  /// published and exactly read back the matching generation-only re-seal.
  /// This is deliberately a no-fail swap: callers must hold the CCLC lifecycle
  /// claim, exact parent Apply fence, or route-recovery exclusion through the
  /// Kernel publication/readback and this call. In particular, it must not be
  /// used to repair an unverified Kernel result.
  void AdoptVerifiedComponentReadinessSnapshot(
      std::vector<core::RuntimeParticipantExecutionRegistration> &&
          registrations,
      core::RuntimeParticipantAdmissionResolver &&participant_resolver)
      noexcept;

  /// Route product changes through this owner so its retained registration,
  /// resolver, and Stop-source snapshots advance only at Kernel publication.
  [[nodiscard]] core::Result<void> reconfigure_runtime_product(
      std::vector<core::RuntimeParticipantExecutionRegistration>
          registrations,
      core::RuntimeParticipantAdmissionResolver participant_resolver,
      std::vector<RetainedStopSurveillanceSourceInput> candidate_stop_sources,
      std::function<core::Result<void>()> commit_publication = {});

  /// Install a Manager-selected component delta beneath its exact held fence.
  /// The optional commit callback executes at the Kernel publication boundary.
  [[nodiscard]] core::Result<void> reconfigure_runtime_component_set(
      std::vector<core::RuntimeParticipantExecutionRegistration>
          registrations,
      core::RuntimeParticipantAdmissionResolver participant_resolver,
      std::vector<RetainedStopSurveillanceSourceInput> candidate_stop_sources,
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
  RetainedStopSurveillanceEmptySourceMode stop_source_mode_{
      RetainedStopSurveillanceEmptySourceMode::kManagedCellBootstrapEmpty};
  std::shared_ptr<RetainedStopSurveillanceOwner> stop_surveillance_owner_;
  core::RequestOwnerEpoch request_owner_epoch_;
  core::CellId cell_id_;
  core::IResourceOperationAdmissionGate *operation_admission_gate_{};
  std::unique_ptr<core::SqliteOperationJournal> operation_journal_;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      participant_registrations_;
  core::RuntimeParticipantAdmissionResolver participant_resolver_;
  std::unique_ptr<core::ResourceAddressedExecutionKernel> addressed_kernel_;
};

}  // namespace manyforge::roboplan_backend
