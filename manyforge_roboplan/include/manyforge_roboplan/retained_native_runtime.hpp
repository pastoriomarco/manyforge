#pragma once

#include <cstddef>
#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/types/component_lifecycle_readiness.hpp>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/ports/iplanning_scene_synchronizer.hpp>
#include <manyforge_core/ports/ireference_pose_resolver.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/route_configuration_publication.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_focused_adapters.hpp>
#include <manyforge_roboplan/retained_gripper_position_limits.hpp>
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_stop_surveillance_owner.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <utility>
#include <span>
#include <vector>

namespace manyforge::core {
class GripperParticipantEngine;
}

namespace manyforge::roboplan_backend {
class RetainedNativeGripperRoutePublicationOwner;

class RetainedNativeCellRuntime;
class RetainedNativeGripperRuntime;

struct RetainedNativeRuntimeConfig final {
  core::RequestOwnerEpoch request_owner_epoch;
  core::CellId cell_id;
  std::filesystem::path operation_journal_path;
  std::size_t request_replay_capacity{1024U};
  core::CacheConfig cache;
  core::ExecutionKernelConfig kernel;
  SimpleSchedulerConfig scheduler;
  core::Duration gripper_update_period_ns{10'000'000};
  /// Exact motion-resource order from the retained runtime-unit product.
  /// Empty remains available only to lower-level fixtures that intentionally
  /// bootstrap from the scene's already-established state.
  std::vector<double> initial_motion_positions_rad;
  /// Process-owned monitoring policy. Public operation/caller payloads never
  /// select or override these cadences.
  std::optional<core::StopSurveillancePolicyDTO> stop_surveillance_policy;
  core::CellAdmissionInterventionValidator
      cell_admission_intervention_validator;
  core::CellAdmissionSettlementValidator cell_admission_settlement_validator;
  core::CellAdmissionReconcileValidator cell_admission_reconcile_validator;
  std::optional<core::ManagerLivenessRequiredSetDTO>
      manager_liveness_required_set;
  core::CellAdmissionProgramResetStatusSource cell_admission_program_reset_status_source;
};

struct RetainedNativeAddressedOwnerConfig final {
  core::RequestOwnerEpoch request_owner_epoch;
  core::CellId cell_id;
  std::filesystem::path operation_journal_path;
  std::size_t request_replay_capacity{1024U};
  core::IResourceOperationAdmissionGate *operation_admission_gate{};
  core::GraphRevision graph_revision;
  core::CellAdmissionInterventionValidator
      cell_admission_intervention_validator;
  core::CellAdmissionSettlementValidator cell_admission_settlement_validator;
  core::CellAdmissionReconcileValidator cell_admission_reconcile_validator;
  std::optional<core::ManagerLivenessRequiredSetDTO>
      manager_liveness_required_set;
  core::CellAdmissionProgramResetStatusSource cell_admission_program_reset_status_source;
};

using RetainedMotionBackendRouteRebinder = std::function<core::Result<void>(
    core::RuntimeParticipantBinding,
    std::vector<core::RuntimeParticipantBinding>)>;

using RetainedGripperBackendRouteRebinder = std::function<core::Result<void>(
    core::RuntimeParticipantBinding,
    std::shared_ptr<IRetainedObservedStateChannel>)>;

/// One exact execution adapter set selected by a retained runtime product.
/// The two interface pointers may alias the same dual-role adapter; shared
/// ownership keeps that case single-instance and gives it one shutdown path.
struct RetainedRuntimeBackendSet final {
  std::shared_ptr<core::IExecutionBackend> execution;
  std::shared_ptr<core::IGripperCommandBackend> gripper;
  core::IControlDomainExecutionPort *gripper_stop_execution_port{};
  RetainedMotionBackendRouteRebinder rebind_motion_route;
  RetainedGripperBackendRouteRebinder rebind_gripper_route;
};

using RetainedRuntimeBackendFactory = std::function<RetainedRuntimeBackendSet(
    const core::RuntimeParticipantBinding &,
    const std::optional<core::RuntimeParticipantBinding> &,
    const std::shared_ptr<RetainedObservedStateChannel> &,
    const std::shared_ptr<RetainedObservedStateChannel> &, core::IClock &,
    core::IScheduler &)>;

using RetainedReferenceResolverFactory =
    std::function<std::shared_ptr<core::IReferencePoseResolver>(
        const std::vector<std::shared_ptr<RetainedObservedStateChannel>> &)>;

/// A successful null payload is source absence, not a route-publication error.
/// Err is reserved for invalid/failed preparation and exact association errors;
/// callers must not hide unexpected transport failures as source absence.
using RetainedRobotMeasuredSourcePreparer = std::function<
    core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>>(
        const core::RuntimeParticipantBinding &candidate_motion,
        const std::optional<core::RuntimeParticipantBinding> &candidate_gripper,
        const std::vector<core::ActiveControlConfigurationDTO>
            &candidate_configurations)>;

struct RetainedResolvedParticipant final {
  core::RuntimeParticipantBinding binding;
  bool intrinsic_direct_control{};
};

using RetainedRuntimeParticipantResolver =
    std::function<core::Result<RetainedResolvedParticipant>(
        const core::OperationIntentDTO &, std::size_t)>;

using RetainedRuntimeParticipantResolverFactory =
    std::function<RetainedRuntimeParticipantResolver(
        std::vector<core::RuntimeParticipantBinding>)>;

/// Adapt a cell-wide Program-binding resolver to exact physical Kernel
/// registrations. This shares the native runtime's ambiguity and physical-owner
/// validation without requiring the motion runtime to own the cell resolver.
[[nodiscard]] core::RuntimeParticipantAdmissionResolver
make_retained_cell_kernel_participant_resolver(
    RetainedRuntimeParticipantResolver binding_resolver,
    std::vector<core::RuntimeParticipantExecutionRegistration>
        physical_registrations);

/// Build the exact cell Stop-surveillance source for one independently owned
/// gripper. The binding, readiness and state channel must come from the same
/// retained gripper owner; its participant engine supplies command-free
/// no-active-goal evidence.
[[nodiscard]] std::optional<RetainedStopSurveillanceSourceInput>
build_retained_gripper_stop_surveillance_source(
    const core::RuntimeParticipantBinding &binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness,
    const std::shared_ptr<IRetainedObservedStateChannel> &state_channel,
    core::IResolvedParticipantExecutionEngine *participant_engine);

/// Stable Kernel-facing owner for the current planning-scene synchronization
/// authority. A retained runtime keeps this object for its lifetime while the
/// owner may replace the publication-specific target under its Apply fence.
class RetainedPlanningSceneSynchronizer final
    : public core::IPlanningSceneSynchronizer {
public:
  explicit RetainedPlanningSceneSynchronizer(
      std::shared_ptr<core::IPlanningSceneSynchronizer> target);

  RetainedPlanningSceneSynchronizer(
      const RetainedPlanningSceneSynchronizer &) = delete;
  RetainedPlanningSceneSynchronizer &operator=(
      const RetainedPlanningSceneSynchronizer &) = delete;

  /// Atomically replace the authority used by subsequent calls. An in-flight
  /// call safely finishes against the authority it already captured.
  void ReplaceTarget(
      std::shared_ptr<core::IPlanningSceneSynchronizer> target);

  [[nodiscard]] core::Result<core::ProjectionRevisionDTO>
  SynchronizeBeforePlanning() override;

private:
  std::mutex mutex_;
  std::shared_ptr<core::IPlanningSceneSynchronizer> target_;
};

/// Stable Kernel-facing owner for the current retained-scene frame authority.
/// The underlying resolver is publication-specific; a retained runtime keeps
/// this proxy for its lifetime while the host replaces the target under the
/// Apply fence.
class RetainedReferencePoseResolverProxy final
    : public core::IReferencePoseResolver {
public:
  explicit RetainedReferencePoseResolverProxy(
      std::shared_ptr<core::IReferencePoseResolver> target);

  RetainedReferencePoseResolverProxy(
      const RetainedReferencePoseResolverProxy &) = delete;
  RetainedReferencePoseResolverProxy &operator=(
      const RetainedReferencePoseResolverProxy &) = delete;

  /// Replace the publication-specific authority used by subsequent calls.
  /// An in-flight call safely finishes against the target it already captured.
  void ReplaceTarget(std::shared_ptr<core::IReferencePoseResolver> target);

  [[nodiscard]] core::Result<core::ProgramFrameResolutionContextDTO>
  capture_program_frame_resolution_context(
      core::ProgramFrameResolutionCaptureDTO capture) const override;

  [[nodiscard]] core::Result<core::Pose3DTO> resolve_reference_pose(
      const core::ReferenceDTO &reference,
      const core::ProgramFrameResolutionContextDTO &context) const override;

  [[nodiscard]] core::Result<core::ResolvedSceneFramePoseDTO>
  resolve_scene_frame(
      const core::SceneFrameRefDTO &reference,
      const core::SceneFrameResolutionContextDTO &context) const override;

private:
  [[nodiscard]] std::shared_ptr<core::IReferencePoseResolver>
  CurrentTarget() const;

  mutable std::mutex mutex_;
  std::shared_ptr<core::IReferencePoseResolver> target_;
};

/// Already-verified inputs to one native runtime composition.
///
/// Deployment/product readers and the logical-scene owner remain outside this
/// boundary. They supply sealed bindings, one exact retained scene/projector,
/// a planner whose worker model has the same projection revision, and a
/// reference resolver backed by the logical-scene owner. No path, model name,
/// robot default, or group selector is accepted here.
struct RetainedNativeRuntimeInput final {
  core::RuntimeParticipantBinding motion_binding;
  std::optional<core::RuntimeParticipantBinding> gripper_binding;
  /// CCLC-issued, repository-read-back Stop readiness for the exact physical
  /// motion binding. Runtime composition never fabricates lifecycle or active
  /// configuration generations from retained desired state.
  std::optional<core::RuntimeParticipantStopReadiness> motion_stop_readiness;
  /// Equivalent readiness for the optional physical gripper binding.
  std::optional<core::RuntimeParticipantStopReadiness> gripper_stop_readiness;
  /// Additional Program contexts for the same exact physical motion target.
  /// This is used by build-owned direct-control Programs; aliases may differ
  /// only in Program/role identity and cannot widen the physical runtime.
  std::vector<core::RuntimeParticipantBinding> motion_binding_aliases;
  /// Additional Program contexts for the same exact physical gripper target.
  std::vector<core::RuntimeParticipantBinding> gripper_binding_aliases;
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<RetainedKinematicStateProjector> state_projector;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  collision::ManyForgeCollisionEngine::SharedModelPtr collision_shared_model;
  std::unique_ptr<RoboPlanPlannerBackend> planner;
  RetainedReferenceResolverFactory reference_resolver_factory;
  /// Optional exact Program/Program-Binding point resolver. The composition
  /// supplies only sealed bindings for this physical runtime; Core rechecks
  /// the request address, resource inventory, and pinned engine before use.
  RetainedRuntimeParticipantResolver participant_resolver;
  /// Rebuild the exact Program resolver after a route publication replaces
  /// physical participant bindings. Required by production compositions that
  /// admit route changes; omitted only by fixed/focused runtimes.
  RetainedRuntimeParticipantResolverFactory participant_resolver_factory;
  /// Optional configuration-fence interlock owned by the surrounding managed
  /// execution process. It must outlive this runtime composition.
  core::IResourceOperationAdmissionGate *operation_admission_gate{};
  /// Optional product-selected state authority. Empty preserves the native
  /// predicted-state channel. A controller-backed simulation supplies its
  /// exact commanded source set here before constructing the sole writer.
  std::optional<RetainedObservedStateChannelConfig> motion_state_channel_config;
  std::optional<RetainedObservedStateChannelConfig>
      gripper_state_channel_config;
  /// Optional retained-product execution adapter factory. Empty preserves the
  /// existing native execution and gripper implementations.
  RetainedRuntimeBackendFactory backend_factory;
  /// Optional owner-coordinated scene observation latch retained for every
  /// fresh planning admission.
  std::shared_ptr<core::IPlanningSceneSynchronizer> planning_scene_synchronizer;
  /// Complete build-owned physical-control inventory for CCLC and route
  /// readiness. It contains one exact representative for every selected
  /// graph control binding while motion/gripper bindings above remain the
  /// sole backend owners. Empty preserves the focused-runtime one-binding-per-
  /// owner shape.
  std::vector<core::RuntimeParticipantBinding>
      control_binding_representatives{};
  /// Sole Deployment-authored TCP-speed maximum for this exact motion target.
  std::optional<double> deployment_maximum_tcp_linear_speed_mps{};
  /// Retained manual-motion ceilings for the exact motion binding above.
  core::PerJointDynamicsLimitsDTO direct_control_joint_limits{};
  /// Saved settings, resolved once against the immutable model at activation.
  std::optional<
      deployment::execution_runtime_contract::OrdinaryMotionJerkSettings>
      ordinary_motion_jerk{};
  /// Cell-wide render wake shared by independently owned robot channels.
  std::shared_ptr<RetainedObservationWake> observation_wake{};
  /// Q37 (as `main`): when set, the Deployment-authored fault Stop profile
  /// below is the motion target's fault profile, read directly (absent means
  /// no fault profile); nothing is derived from the operator profile. Unset
  /// keeps the native lane's derivation from the retained Stop plan.
  bool fault_stop_profile_from_deployment{false};
  std::optional<core::MotionInterruptStopProfileDTO>
      deployment_fault_stop_profile{};
  /// Optional actual producer staging. Empty preserves legacy construction;
  /// measured readiness then reports an unavailable source association.
  RetainedRobotMeasuredSourcePreparer prepare_measured_sources{};
};

struct RetainedNativeRuntime;

/// Candidate product facts for a retained component. The host has already
/// compiled and verified the candidate bindings/readiness; this API performs
/// only the physical owner re-seal while the exact Apply fence is held.
/// Candidate collision resources are deliberately not accepted here: the
/// Core collision-resource replacement seam is not yet transactional, so the
/// host must update them through its separately verified cell-level path.
struct RetainedNativeRuntimeResealInput final {
  core::RuntimeParticipantBinding motion_binding;
  std::optional<core::RuntimeParticipantBinding> gripper_binding;
  std::vector<core::RuntimeParticipantBinding> motion_binding_aliases;
  std::vector<core::RuntimeParticipantBinding> gripper_binding_aliases;
  std::vector<core::RuntimeParticipantBinding> control_binding_representatives;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      participant_registrations;
  /// Independently owned grippers in the same managed cell. They are peers,
  /// not subordinate controllers of this motion component. Route publication
  /// must preserve their registrations and Stop sources in the shared Kernel.
  std::vector<std::shared_ptr<RetainedNativeGripperRuntime>>
      independent_gripper_peers;
  /// Non-owning stable cell authority; the Composition pins it through every
  /// publication and clears/rebinds this pointer before retiring an owner.
  RetainedNativeCellRuntime *cell_runtime_owner{};
  std::shared_ptr<core::IReferencePoseResolver> reference_resolver;
  core::ConfigurationFenceId apply_fence_id;
  core::ConfigurationFenceScopeDTO apply_scope;
  core::IResourceOperationAdmissionGate &admission_gate;
};

[[nodiscard]] core::Result<void> reseal_retained_native_runtime_for_apply(
    RetainedNativeRuntime &runtime, RetainedNativeRuntimeResealInput input);

/// Candidate whole-cell model computations for an already re-sealed retained
/// motion component. The generation was built before the fence from the
/// candidate planning model and the candidate (re-sealed) bindings.
struct RetainedNativeRuntimeModelRefreshInput final {
  std::shared_ptr<const RetainedKinematicModelGeneration> candidate;
  core::ConfigurationFenceId apply_fence_id;
  core::ConfigurationFenceScopeDTO apply_scope;
  core::IResourceOperationAdmissionGate &admission_gate;
};

/// Install one candidate model generation into a retained runtime under the
/// exact Apply fence: verify that the controller's own model facts are
/// unchanged, drain in-flight model users, select the generation in the
/// stable Kernel/host proxy and in every model-backed state channel, and
/// read the selection back. Controller, Kernel, planner-client and
/// observed-source identities are preserved. A failure is returned as is;
/// no predecessor generation is restored and admission stays closed.
[[nodiscard]] core::Result<void> refresh_retained_native_runtime_model_for_apply(
    RetainedNativeRuntime &runtime,
    RetainedNativeRuntimeModelRefreshInput input);

/// Owning native composition root. Destruction order is deliberate: the public
/// addressed owner unsubscribes first, then the subordinate Kernel quiesces,
/// then backends/scheduler/state/model resources retire.
struct RetainedNativeRuntime final
    : public deployment::IRouteConfigurationKernelPublicationOwner,
      public core::ICCLCComponentCommandReadinessPort {
  RetainedNativeRuntime(core::RuntimeParticipantBinding motion,
                        std::optional<core::RuntimeParticipantBinding> gripper)
      : motion_binding(std::move(motion)), gripper_binding(std::move(gripper)) {
  }

  ~RetainedNativeRuntime();

  RetainedNativeRuntime(const RetainedNativeRuntime &) = delete;
  RetainedNativeRuntime &operator=(const RetainedNativeRuntime &) = delete;

  core::RuntimeParticipantBinding motion_binding;
  std::optional<core::RuntimeParticipantBinding> gripper_binding;
  std::vector<core::RuntimeParticipantBinding> motion_binding_aliases;
  std::vector<core::RuntimeParticipantBinding> gripper_binding_aliases;
  std::vector<core::RuntimeParticipantBinding> control_binding_representatives;
  std::optional<core::StopSurveillancePolicyDTO> stop_surveillance_policy;
  std::optional<core::MotionInterruptStopProfileDTO>
      motion_operator_stop_profile;
  std::optional<core::MotionInterruptStopProfileDTO> motion_fault_stop_profile;
  /// The fault profile above is Deployment-authored (Q37), not derived.
  bool motion_fault_stop_profile_from_deployment{false};
  std::optional<core::RequestOwnerEpoch> addressed_request_owner_epoch;
  std::optional<core::CellId> addressed_cell_id;
  std::shared_ptr<RetainedObservedStateChannel> motion_state_channel;
  std::shared_ptr<RetainedObservedStateChannel> gripper_state_channel;
  /// Owner-local current controller proof. The enclosing composition pins
  /// the coordinator during reads and clears this binding before detachment.
  std::function<core::Result<core::ContentDigest>(
      const core::StopFaultRecoveryConfigurationExpectationDTO &)>
      stop_recovery_configuration_source;
  /// Advances existing CCLC replacement transactions on the host's ordinary
  /// reconciliation lane. Cleared before the owning CCLC is detached.
  std::function<core::Result<void>()> reconcile_controller_routes;

  std::shared_ptr<WallClock> clock;
  std::shared_ptr<SimpleScheduler> scheduler;
  std::unique_ptr<core::TrajectoryCache> cache;
  std::unique_ptr<RoboPlanPlannerBackend> planner;
  /// Publication-specific resolver retained for owner-side inspection and
  /// dynamic dispatch (for example, route/configuration rebinding).
  std::shared_ptr<core::IReferencePoseResolver> reference_resolver;
  /// Stable object installed into ExecutionKernelPorts. Replacing its target
  /// does not invalidate the Kernel's resolver reference.
  std::shared_ptr<RetainedReferencePoseResolverProxy>
      reference_resolver_proxy;
  std::unique_ptr<core::AddressedAngularObservedRobotStateAdapter>
      angular_observed_state;
  /// Stable kinematics, non-collision validator and collision-state
  /// projector installed into the Kernel and used by direct host queries.
  /// It selects one whole-cell model generation (scene, state projector and
  /// the target's model computations); a retained controller refreshes it
  /// under the Apply fence when the cell planning model changes.
  std::shared_ptr<RetainedKinematicModelProxy> kinematic_model;
  std::shared_ptr<core::IExecutionBackend> execution;
  std::shared_ptr<core::IGripperCommandBackend> gripper;
  std::shared_ptr<RetainedPlanningSceneSynchronizer>
      planning_scene_synchronizer;
  core::IControlDomainExecutionPort *gripper_stop_execution_port{};
  RetainedMotionBackendRouteRebinder rebind_motion_route;
  RetainedGripperBackendRouteRebinder rebind_gripper_route;
  std::optional<RetainedGripperPositionLimits> gripper_position_limits;
  /// Exact non-speed motion policy sealed against this motion target. It
  /// carries the Deployment-selected ordinary-motion jerk profile only.
  std::optional<core::RuntimeParticipantPolicySelection>
      motion_participant_policy;
  std::optional<double> deployment_maximum_tcp_linear_speed_mps;
  core::PerJointDynamicsLimitsDTO direct_control_joint_limits;

  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  std::shared_ptr<core::ICollisionQueryEngine> runtime_collision_engine;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      validation_collision_engines;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      lookahead_collision_engines;

  std::shared_ptr<core::ExecutionKernel> participant_engine;
  std::shared_ptr<core::GripperParticipantEngine> gripper_participant_engine;
  [[nodiscard]] core::IResolvedParticipantExecutionEngine *
  participant_engine_for(bool motion) const;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      participant_registrations;
  core::RuntimeParticipantAdmissionResolver participant_resolver;
  RetainedRuntimeParticipantResolverFactory participant_resolver_factory;
  /// Set once by composition; staging never changes lifecycle/controller state.
  RetainedRobotMeasuredSourcePreparer prepare_measured_sources;
  /// The stable Kernel stores a non-owning pointer to this source. A retained
  /// root may change during Apply, so the stable owner is shared with the
  /// candidate root until the Kernel itself retires.
  std::shared_ptr<RetainedStopSurveillanceOwner> stop_surveillance_owner;
  std::unique_ptr<core::SqliteOperationJournal> operation_journal;
  core::SqliteOperationJournal *cell_operation_journal{};
  [[nodiscard]] core::SqliteOperationJournal *
  active_operation_journal() const noexcept {
    return operation_journal != nullptr ? operation_journal.get()
                                        : cell_operation_journal;
  }
  std::unique_ptr<core::ResourceAddressedExecutionKernel> addressed_kernel;
  core::ResourceAddressedExecutionKernel *cell_addressed_kernel{};
  /// The standalone runtime owns its Kernel. Managed components borrow the
  /// process-stable cell Kernel, whose owner outlives all registered engines.
  [[nodiscard]] core::ResourceAddressedExecutionKernel *
  active_addressed_kernel() const noexcept {
    return addressed_kernel != nullptr ? addressed_kernel.get()
                                       : cell_addressed_kernel;
  }
  /// Non-owning root used only to assemble complete cell-level route updates.
  RetainedNativeRuntime *cell_runtime_root{};
  /// Additional independently built robot engines owned by this cell-level
  /// runtime. Only the primary owns the addressed Kernel and durable journal.
  std::vector<std::shared_ptr<RetainedNativeRuntime>> participant_peers;
  /// Independently managed grippers share the stable cell Kernel but are not
  /// subordinate to any motion component.
  std::vector<std::shared_ptr<RetainedNativeGripperRuntime>>
      independent_gripper_peers;
  /// Existing installed peer inventory, updated under the same route mutex.
  /// The weak self alias supplies lifetime only, never a second inventory.
  // The actual host installation borrows the process-stable cell Kernel and
  // journal here; component retirement never owns/destroys that borrowed Kernel.
  void BindCellRuntimeOwner(RetainedNativeCellRuntime& cell,
                            const std::string& runtime_epoch,
                            const std::string& cell_id);

  void BindIndependentGripperPeers(
      const std::shared_ptr<RetainedNativeRuntime>& root,
      std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> peers);
  /// Non-owning stable cell authority for managed component compositions.
  RetainedNativeCellRuntime *cell_runtime_owner{};

  [[nodiscard]] core::Result<std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &configuration) const override;

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &configuration,
      std::vector<deployment::VerifiedRouteReadinessParticipant> participants)
      override;

  /// Close one component's command eligibility under its exact held claim.
  [[nodiscard]] core::Result<core::ComponentCommandReadinessDTO>
  CloseComponentCommandEligibility(
      const core::LifecycleTransactionId &lifecycle_transaction_id,
      const core::ControllableComponentId &controllable_component_id) override;

  [[nodiscard]] core::Result<core::ComponentCommandReadinessDTO>
  CloseComponentCommandEligibility(
      const core::CellAdmissionClaimDTO &selected_claim,
      const core::SelectedComponentBringUpOfferDTO &offer) override;

  /// Publish one complete component result for the whole cell: re-seal the
  /// component's participants in every robot runtime at the published
  /// generation (same engines, routes and Stop plans), rebuild the Kernel
  /// participant resolver, and set eligibility atomically in the Kernel.
  [[nodiscard]] core::Result<core::ComponentCommandReadinessDTO>
  PublishComponentCommandReadiness(
      const core::ComponentCommandReadinessPublicationDTO &publication) override;

  [[nodiscard]] core::Result<core::ComponentCommandReadinessDTO>
  PublishComponentCommandReadiness(
      const core::CellAdmissionClaimDTO &selected_claim,
      const core::SelectedComponentBringUpOfferDTO &owner_offer,
      const core::ComponentLifecycleStatusDTO &selected_status,
      const core::SelectedControllerOccurrenceDTO &installed_occurrence,
      const std::vector<core::RuntimeParticipantExecutionRegistration> &installed_candidate_registrations,
      const std::vector<core::ActiveControlConfigurationDTO> &installed_candidate_configurations,
      bool command_eligible = false) override;

  [[nodiscard]] core::Result<core::ComponentCommandReadinessDTO>
  ReadComponentCommandReadiness(
      const core::ControllableComponentId &controllable_component_id)
      const override;

  /// Rebind the new in-process runtime to one exact durable predecessor while
  /// retaining the new provider epoch and keeping operation admission closed.
  /// CCLC uses this only during owner replacement before it explicitly
  /// observes and republishes a current-source successor configuration.
  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  AdoptAndReadBackRouteConfigurationPredecessor(
      const core::ActiveControlConfigurationDTO &configuration);

  /// Settle restart-orphaned resources after every retained route has been
  /// re-established from fresh, command-free physical evidence.
  [[nodiscard]] core::Result<void> ReconcileCommandFreeOrphans();

  [[nodiscard]] core::Result<std::optional<core::ActiveControlConfigurationDTO>>
  ReadCurrentPublishedRouteConfiguration(
      const core::ControlDomainId &control_domain_id) const;

  [[nodiscard]] core::Result<core::GripperCommandStatusDTO>
  ReadGripperStatus(const core::RuntimeParticipantBinding &source) const;

  [[nodiscard]] core::Result<core::RobotMeasuredStateSourceView>
  PrepareCurrentMeasuredSources();

  [[nodiscard]] core::Result<core::ComponentLifecycleReadinessSourceView>
  CaptureReadinessSources(
      const core::ComponentLifecycleCurrentSnapshotDTO &snapshot,
      const std::shared_ptr<RetainedNativeRuntime> &owning_runtime) const;

private:
  friend class RetainedNativeGripperRoutePublicationOwner;
  [[nodiscard]] core::Result<std::shared_ptr<RetainedNativeGripperRoutePublicationOwner>>
  CurrentIndependentGripperRouteOwner(const core::ControlDomainId& domain) const;
  std::weak_ptr<RetainedNativeRuntime> installed_self_;
  friend core::Result<void> reseal_retained_native_runtime_for_apply(
      RetainedNativeRuntime &runtime,
      RetainedNativeRuntimeResealInput input);
  friend core::Result<void> refresh_retained_native_runtime_model_for_apply(
      RetainedNativeRuntime &runtime,
      RetainedNativeRuntimeModelRefreshInput input);
  friend core::Result<void> rejoin_retained_native_runtime_participants(
      const std::shared_ptr<RetainedNativeRuntime> &primary,
      std::vector<std::shared_ptr<RetainedNativeRuntime>> peers,
      RetainedRuntimeParticipantResolverFactory participant_resolver_factory,
      std::shared_ptr<RetainedStopSurveillanceOwner> stable_stop_observer,
      std::vector<RetainedStopSurveillanceSourceInput>
          &candidate_surveillance_sources,
      core::IResourceOperationAdmissionGate &admission_gate,
      const core::ConfigurationFenceId &fence_id,
      const core::ConfigurationFenceScopeDTO &scope);

  // Caller holds the cell root's route_publication_mutex_ (this runtime's
  // own when it is the root). No owner read or write escapes it.
  [[nodiscard]] bool
  GripperSourceIsCurrent(const core::RuntimeParticipantBinding &source) const;
  std::shared_ptr<const RetainedPreparedRobotMeasuredSources>
      prepared_measured_sources_;
  mutable std::mutex route_publication_mutex_;
};

/// Assemble only the product-specific subordinate participant runtime. This
/// is the preparation boundary for a fenced Apply: it opens no operation
/// journal and creates no addressed Kernel owner. Supplied time capabilities
/// keep a later candidate attached to the same stable owner lifetime and role-
/// isolated process scheduler.
[[nodiscard]] std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime_participants(
    RetainedNativeRuntimeInput input, RetainedNativeRuntimeConfig config,
    std::shared_ptr<WallClock> shared_clock = {},
    std::shared_ptr<SimpleScheduler> shared_scheduler = {});

/// Join independently planned robot engines beneath one cell-level addressed
/// Kernel. The participants must share the same clock, scheduler and retained
/// product; this function adds no timing synchronization policy.
void join_retained_native_runtime_participants(
    const std::shared_ptr<RetainedNativeRuntime> &primary,
    std::vector<std::shared_ptr<RetainedNativeRuntime>> peers,
    RetainedRuntimeParticipantResolverFactory participant_resolver_factory);

/// Rebuild cell-local registration/resolver aggregates after candidate
/// per-component re-seals and return a validated candidate Stop-source table
/// for the host's atomic Kernel commit hook. This never mutates the stable
/// observer or Kernel and creates no engines; it only publishes new runtime
/// membership after the complete candidate set validates under the exact
/// Apply fence.
[[nodiscard]] core::Result<void>
rejoin_retained_native_runtime_participants(
    const std::shared_ptr<RetainedNativeRuntime> &primary,
    std::vector<std::shared_ptr<RetainedNativeRuntime>> peers,
    RetainedRuntimeParticipantResolverFactory participant_resolver_factory,
    std::shared_ptr<RetainedStopSurveillanceOwner> stable_stop_observer,
    std::vector<RetainedStopSurveillanceSourceInput>
        &candidate_surveillance_sources,
    core::IResourceOperationAdmissionGate &admission_gate,
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope);

/// Resolve current Stop-recovery facts for either a directly attached owner
/// or the process-stable managed owner. This is one product-facts adapter;
/// ownership of the Stop latch and its clear remains in Core.
[[nodiscard]] core::Result<core::StopFaultRecoveryFactsDTO>
build_retained_native_stop_recovery_facts(
    RetainedNativeRuntime &runtime,
    core::ResourceAddressedExecutionKernel &addressed_kernel,
    core::IResourceOperationAdmissionGate *operation_admission_gate,
    const core::StopStatusDTO &status, std::int64_t assessed_at_ns);

/// Shared Stop-fault recovery join for the process-stable cell owner and
/// motion-owned compatibility path. The observer supplies exact current
/// source channels and orphan evidence; this function verifies those sources
/// against the addressed Kernel, reconciles command-free captured orphans,
/// then joins Kernel and admission-fence blockers. A motion owner may provide
/// its stronger owner-local configuration verifier; when absent, the exact
/// observer source table is used for both motion and gripper-only cells.
[[nodiscard]] core::Result<core::StopFaultRecoveryFactsDTO>
build_retained_cell_stop_recovery_facts(
    RetainedStopSurveillanceOwner &stop_surveillance_owner,
    core::ResourceAddressedExecutionKernel &addressed_kernel,
    core::IClock &clock, const core::RequestOwnerEpoch &request_owner_epoch,
    const core::CellId &cell_id,
    core::IResourceOperationAdmissionGate *operation_admission_gate,
    const core::StopStatusDTO &status, std::int64_t assessed_at_ns,
    std::function<core::Result<core::ContentDigest>(
        const core::StopFaultRecoveryConfigurationExpectationDTO &)>
        configuration_source_override = {});

/// Attach the sole stable addressed owner to one prepared participant set.
/// The owner config is process-selected, not supplied by a Program or caller.
void attach_retained_native_addressed_owner(
    const std::shared_ptr<RetainedNativeRuntime> &runtime,
    RetainedNativeAddressedOwnerConfig config);

/// Assemble and seed one production native participant runtime from exact
/// retained values. Inconsistent projection, planner, state, component, target,
/// group, unit, or worker identities throw before any operation is admitted.
[[nodiscard]] std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime(RetainedNativeRuntimeInput input,
                               RetainedNativeRuntimeConfig config,
                               std::shared_ptr<WallClock> shared_clock = {},
                               std::shared_ptr<SimpleScheduler> shared_scheduler = {});

} // namespace manyforge::roboplan_backend
