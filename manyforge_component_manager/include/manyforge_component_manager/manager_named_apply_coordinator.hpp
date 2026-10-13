#pragma once

#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_fence_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_fence_release_coordinator.hpp>
#include <manyforge_component_manager/manager_planned_maintenance_coordinator.hpp>
#include <manyforge_component_manager/manager_runtime_convergence_coordinator.hpp>
#include <manyforge_component_manager/manager_scene_initialization_coordinator.hpp>
#include <manyforge_component_manager/manager_selected_bundle_transition_coordinator.hpp>
#include <manyforge_component_manager/manager_selected_runtime_composition_coordinator.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/types/scene_transaction.hpp>

#include <functional>
#include <memory>
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
/// bounded prepared activation; the caller never authors that internal identity.
struct ManagerNamedApplyRequestDTO final {
  manyforge::core::PublicRequestContextDTO request_context;
  manyforge::core::DeploymentArtifactId deployment_artifact_id;
  manyforge::core::DeploymentRevision deployment_revision;
  manyforge::core::RetainedBundleId expected_current_retained_bundle_id;
  manyforge::core::GraphRevision expected_current_graph_revision;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;
  ManagerApplyPreparedActivationDTO prepared_activation;
  std::optional<manyforge::core::AdmissionGeneration>
      expected_admission_generation;
  std::optional<manyforge::core::CellAdmissionClaimDTO>
      parent_authoring_claim;
  /// Manager-checked startup input captured before acceptance. Exact replay
  /// retains the stored snapshot and base rather than recapturing either.
  std::optional<ManagerIsaacPoseObserverLaunchFactsDTO>
      isaac_pose_observer_launch_facts{};

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerNamedApplyRequestDTO &,
                         const ManagerNamedApplyRequestDTO &) = default;
};

enum class ManagerNamedApplyResultState {
  kPending,
  kSucceeded,
  kFailed,
  kUnknownEffect,
};

struct ManagerNamedApplyResult final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  ManagerNamedApplyResultState state;
  bool request_replayed;
  std::optional<ManagerApplyTerminalOutcomeDTO> terminal_outcome;
};

/// Single-progression-owner composition of the I4 named-Apply phase owners.
///
/// This class adds no replacement state machine: it routes each durable phase
/// to the already qualified I4 coordinator that owns it. A new request first
/// resolves the exact current publication and sealed bundles. It samples the
/// Kernel admission state once before the first Acquire; Acquire then performs
/// the atomic reject-if-busy compare-and-swap. Same-process progression reuses
/// one captured pair of verified bundles and never repeats a completed external
/// effect. A restarted Manager cannot reconstruct that capture and records the
/// interrupted operation as unknown rather than resuming it automatically.
class ManagerNamedApplyCoordinator final {
public:
  /// Exact, verified Program/Binding projection captured before any Apply
  /// effect. The source may read retained products; it must not select a
  /// mutable public or authoring workspace.
  using InitialSceneSource = std::function<std::vector<manyforge::core::AddObjectRequestDTO>(
      const ManagerApplyPreparedActivationDTO &,
      const manyforge::deployment::RetainedBundle &)>;
  /// Supplied only by the trusted, fresh named-Apply call after exact preview
  /// acknowledgment and current configured-process inspection. It derives
  /// immutable input from this owner's captured verified products; it neither
  /// acquires authority nor effects a component. Replay/restart never invokes it.
  using OriginalBringUpPlanSource = std::function<ManagerApplyBringUpPlanDTO(
      const ManagerApplyIntentDTO &,
      const manyforge::deployment::RetainedBundle &,
      const manyforge::deployment::RetainedBundle &,
      const ManagerActiveRuntimePlanDTO &)>;
  /// Fresh caller continuation after the exact host Prepare acknowledgement.
  /// It runs before Scene installation/Finalize, without waiting for observer
  /// Open. Retained or recovered acceptance cannot reconstruct this callback.
  using OriginalPrepareAcknowledgement = std::function<void(
      const ManagerApplyIntentDTO &,
      const manyforge::deployment::RetainedBundle &,
      const ManagerSelectedRuntimeCompositionResultDTO &)>;
  using ApplyIdFactory = std::function<manyforge::core::ApplyId()>;
  using ApplyProgressObserver =
      std::function<void(const ManagerApplyIntentDTO &)>;

  ManagerNamedApplyCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      IManagerActiveRuntimePlanSource &active_plan_source,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      manyforge::core::IActiveBundlePublicationStore &publication_store,
      manyforge::core::IManagerSceneProjectionMaintenancePort &scene_owner,
      manyforge::core::IManagerSceneProjectionBootstrapPort
          *bootstrap_scene_owner,
      manyforge::core::IManagerKernelApplyFencePort &kernel_fence,
      manyforge::core::IKernelCellAdmissionPort &cell_admission,
      IManagerSelectedRuntimeCompositionPort &runtime_composition,
      IRuntimeUnitProcessPort &process_port,
      IRuntimeUnitProgressPort &progress_port,
      ManagerPlannedMaintenanceCoordinator &planned_maintenance,
      manyforge::core::RuntimeEpoch manager_runtime_epoch,
      std::string operating_system,
      std::vector<std::string> launch_environment_contracts,
      RuntimeObservationReducer::Clock clock_ns,
      ApplyIdFactory apply_id_factory,
      InitialSceneSource initial_scene_source,
      RuntimePreStartGuard pre_start_guard = {},
      ApplyProgressObserver apply_progress_observer = {});

  /// Validate and durably accept one exact request without advancing any
  /// runtime effect. Exact replay returns terminal evidence when it already
  /// exists; otherwise the sole progression owner resumes it through
  /// RecoverIncompleteApply(). Only the original process capture can progress
  /// a linked bring-up parent; persisted facts alone remain observations.
  [[nodiscard]] ManagerNamedApplyResult
  AcceptOrReplay(const ManagerNamedApplyRequestDTO &request,
                 OriginalBringUpPlanSource original_bring_up_plan_source = {},
                 OriginalPrepareAcknowledgement original_prepare_acknowledgement = {});

  /// Resume the sole nonterminal Apply, if any. More than one nonterminal
  /// transaction is an integrity failure because this coordinator owns one
  /// serialized progression lane. A linked parent progresses only with this
  /// process's captured original claim and inputs. Recovery never reconstructs
  /// live delegation from retained observations.
  [[nodiscard]] std::optional<ManagerNamedApplyResult> RecoverIncompleteApply();

  ManagerNamedApplyCoordinator(const ManagerNamedApplyCoordinator &) = delete;
  ManagerNamedApplyCoordinator &
  operator=(const ManagerNamedApplyCoordinator &) = delete;

private:
  struct CapturedApplyInputs final {
    manyforge::core::ApplyId apply_id;
    manyforge::core::ApplyGeneration apply_generation;
    std::shared_ptr<const manyforge::deployment::RetainedBundle> predecessor;
    std::shared_ptr<const manyforge::deployment::RetainedBundle> candidate;
    std::vector<manyforge::core::AddObjectRequestDTO> initial_scene;
    std::optional<PersistedManagerApplyBringUpPlan> bring_up_plan{};
    OriginalPrepareAcknowledgement original_prepare_acknowledgement{};
    bool prepare_acknowledgement_consumed{};
    std::optional<manyforge::core::CellAdmissionClaimDTO> original_parent_claim{};
    std::vector<manyforge::core::ControllableComponentId> submitted_children{};
    bool original_preparation_completed{};
    std::optional<manyforge::core::ActiveBundlePublicationReceiptDTO> original_predecessor_publication{};
    std::optional<manyforge::core::QualifiedSceneProductAdoptionRequestDTO> original_scene_adoption{};
    std::optional<manyforge::core::QualifiedSceneConfigurationAdoptionRequestDTO> original_configuration_adoption{};
    bool original_scene_stage_consumed{};
    bool original_scene_stage_completed{};
    bool original_scene_begin_consumed{};
    bool original_scene_complete_consumed{};
    std::optional<manyforge::core::ProjectionMaintenanceInitializationStatusDTO> original_scene_completed{};
  };

  [[nodiscard]] ManagerNamedApplyResult
  Continue(const ManagerApplyIntentDTO &intent, bool request_replayed);
  [[nodiscard]] std::optional<ManagerNamedApplyResult>
  ObserveBringUpParent(const ManagerApplyIntentDTO &intent,
                       bool request_replayed) const;
  [[nodiscard]] manyforge::core::QualifiedSceneProductAdoptionRequestDTO
  CaptureOriginalSceneAdoption(
      const ManagerApplyIntentDTO &intent,
      const std::optional<manyforge::core::ApplyFenceAcquireAttemptQueryDTO> &staging_attempt = std::nullopt);
  [[nodiscard]] manyforge::core::ApplyFenceAcquireAttemptQueryDTO
  CaptureOriginalSceneStagingAttempt(const ManagerApplyIntentDTO &intent);
  [[nodiscard]] bool HasOriginalBringUpCapture(const ManagerApplyIntentDTO &intent) const;
  [[nodiscard]] bool AdvanceOriginalBringUpChildren(const ManagerApplyIntentDTO &intent,
                                                   bool after_publication);
  [[nodiscard]] ManagerNamedApplyResult FinishOriginalBringUpPublication(
      const PersistedManagerApplyIntent &intent, bool request_replayed);
  [[nodiscard]] bool HasOriginalConfigurationCapture(const ManagerApplyIntentDTO &intent) const;
  [[nodiscard]] manyforge::core::QualifiedSceneConfigurationAdoptionRequestDTO
  CaptureOriginalConfigurationAdoption(const ManagerApplyIntentDTO &intent,
      const std::optional<manyforge::core::ApplyFenceAcquireAttemptQueryDTO> &staging_attempt = std::nullopt);
  [[nodiscard]] ManagerNamedApplyResult ContinueOriginalConfiguration(
      const PersistedManagerApplyIntent &intent, bool request_replayed);
  [[nodiscard]] ManagerNamedApplyResult FinishOriginalConfigurationPublication(
      const PersistedManagerApplyIntent &intent, bool request_replayed);
  [[nodiscard]] manyforge::core::ApplyId MintApplyId();

  SqliteManagerApplyJournal &apply_journal_;
  IManagerActiveRuntimePlanSource &active_plan_source_;
  const manyforge::deployment::IVerifiedRetainedBundleSource
      &retained_bundle_source_;
  const manyforge::deployment::IVerifiedRetainedProductArtifactSource
      &artifact_source_;
  // Borrow the existing constructor-supplied publication store as a reader.
  // Its lifetime already covers this coordinator and the nested consumers.
  const manyforge::core::IActiveBundlePublicationReader &publication_reader_;
  manyforge::core::IManagerSceneProjectionMaintenancePort &scene_owner_;
  manyforge::core::IManagerKernelApplyFencePort &kernel_fence_;
  manyforge::core::IKernelCellAdmissionPort &cell_admission_;
  IManagerSelectedRuntimeCompositionPort &runtime_composition_port_;
  manyforge::core::RuntimeEpoch manager_runtime_epoch_;
  ApplyIdFactory apply_id_factory_;
  InitialSceneSource initial_scene_source_;
  ApplyProgressObserver apply_progress_observer_;
  ManagerApplyFenceCoordinator fence_coordinator_;
  ManagerPlannedMaintenanceCoordinator &planned_maintenance_;
  ManagerActiveRuntimeCoordinator active_runtime_coordinator_;
  ManagerApplyRuntimeConvergenceCoordinator runtime_coordinator_;
  ManagerSceneInitializationCoordinator scene_initialization_coordinator_;
  ManagerSelectedRuntimeCompositionCoordinator runtime_composition_coordinator_;
  ManagerSelectedBundleTransitionCoordinator selected_bundle_coordinator_;
  ManagerApplyFenceReleaseCoordinator fence_release_coordinator_;
  std::optional<CapturedApplyInputs> captured_apply_inputs_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
