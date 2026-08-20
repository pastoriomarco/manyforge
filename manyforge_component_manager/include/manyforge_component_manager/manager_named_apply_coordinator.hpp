#pragma once

#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_fence_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_fence_release_coordinator.hpp>
#include <manyforge_component_manager/manager_runtime_convergence_coordinator.hpp>
#include <manyforge_component_manager/manager_scene_activation_coordinator.hpp>
#include <manyforge_component_manager/manager_scene_stage_coordinator.hpp>
#include <manyforge_component_manager/manager_selected_bundle_transition_coordinator.hpp>
#include <manyforge_component_manager/manager_selected_runtime_composition_coordinator.hpp>

#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::component_manager {

class ManagerNamedApplyCoordinatorError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Certain, pre-effect rejection of a new Apply while another Apply or an
/// affected operation is active. It is deliberately not caller authorization
/// and never invokes Stop.
class ManagerNamedApplyBusyError final
    : public ManagerNamedApplyCoordinatorError {
public:
  using ManagerNamedApplyCoordinatorError::ManagerNamedApplyCoordinatorError;
};

/// Exact public selection normalized by the Manager after ingress.
/// `candidate_retained_bundle_id` comes from one independently verified,
/// bounded catalog lookup; the caller never authors that internal identity.
struct ManagerNamedApplyRequestDTO final {
  manyforge::core::PublicRequestContextDTO request_context;
  manyforge::core::DeploymentArtifactId deployment_artifact_id;
  manyforge::core::DeploymentRevision deployment_revision;
  manyforge::core::RetainedBundleId expected_current_retained_bundle_id;
  manyforge::core::GraphRevision expected_current_graph_revision;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerNamedApplyRequestDTO &,
                         const ManagerNamedApplyRequestDTO &) = default;
};

enum class ManagerNamedApplyResultState {
  kPending,
  kSucceeded,
};

struct ManagerNamedApplyResult final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  ManagerNamedApplyResultState state;
  bool request_replayed;
  std::optional<ManagerApplyTerminalOutcomeDTO> terminal_outcome;
};

/// Sole-strand production composition of the I4 named-Apply phase owners.
///
/// This class adds no replacement state machine: it routes each durable phase
/// to the already qualified I4 coordinator that owns it. A new request first
/// resolves the exact current publication and sealed bundles. It samples the
/// Kernel admission state once before the first Acquire; Acquire then performs
/// the atomic reject-if-busy compare-and-swap. Recovery resumes from durable
/// phase evidence and never repeats an already completed external effect.
class ManagerNamedApplyCoordinator final {
public:
  using ApplyIdFactory = std::function<manyforge::core::ApplyId()>;

  ManagerNamedApplyCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      IManagerActiveRuntimePlanSource &active_plan_source,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      manyforge::core::IActiveBundlePublicationStore &publication_store,
      manyforge::core::IManagerSceneProjectionHandoffPort &scene_owner,
      manyforge::core::IManagerSceneProjectionBootstrapPort
          *bootstrap_scene_owner,
      manyforge::core::IManagerKernelApplyFencePort &kernel_fence,
      IManagerSelectedRuntimeCompositionPort &runtime_composition,
      IRuntimeUnitProcessPort &process_port,
      IRuntimeUnitProgressPort &progress_port,
      manyforge::core::RuntimeEpoch manager_runtime_epoch,
      std::string operating_system,
      std::vector<std::string> launch_environment_contracts,
      RuntimeObservationReducer::Clock clock_ns,
      ApplyIdFactory apply_id_factory,
      RuntimePreStartGuard pre_start_guard = {});

  [[nodiscard]] ManagerNamedApplyResult
  ApplyOrRecover(const ManagerNamedApplyRequestDTO &request);

  /// Resume the sole nonterminal Apply, if any. More than one nonterminal
  /// transaction is an integrity failure because this coordinator owns one
  /// serialized Manager command strand.
  [[nodiscard]] std::optional<ManagerNamedApplyResult> RecoverIncompleteApply();

  ManagerNamedApplyCoordinator(const ManagerNamedApplyCoordinator &) = delete;
  ManagerNamedApplyCoordinator &
  operator=(const ManagerNamedApplyCoordinator &) = delete;

private:
  [[nodiscard]] ManagerNamedApplyResult
  Continue(const ManagerApplyIntentDTO &intent, bool request_replayed);
  [[nodiscard]] manyforge::core::ApplyId MintApplyId();

  SqliteManagerApplyJournal &apply_journal_;
  IManagerActiveRuntimePlanSource &active_plan_source_;
  const manyforge::deployment::IVerifiedRetainedBundleSource
      &retained_bundle_source_;
  manyforge::core::IManagerKernelApplyFencePort &kernel_fence_;
  manyforge::core::RuntimeEpoch manager_runtime_epoch_;
  ApplyIdFactory apply_id_factory_;
  ManagerApplyFenceCoordinator fence_coordinator_;
  ManagerApplyRuntimeConvergenceCoordinator runtime_coordinator_;
  ManagerSceneStageCoordinator scene_stage_coordinator_;
  ManagerSceneActivationCoordinator scene_activation_coordinator_;
  ManagerSelectedRuntimeCompositionCoordinator runtime_composition_coordinator_;
  ManagerSelectedBundleTransitionCoordinator selected_bundle_coordinator_;
  ManagerApplyFenceReleaseCoordinator fence_release_coordinator_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
