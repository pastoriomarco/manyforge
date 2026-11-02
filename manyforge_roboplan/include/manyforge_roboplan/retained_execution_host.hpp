#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/ports/iplanning_scene_synchronizer.hpp>
#include <manyforge_core/ports/ireference_pose_resolver.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/execution_host_support.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_focused_adapters.hpp>
#include <manyforge_roboplan/retained_gripper_position_limits.hpp>
#include <manyforge_roboplan/retained_gripper_runtime.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
class GripperParticipantEngine;
}

namespace manyforge::roboplan_backend {
class RetainedGripperRoutePublicationOwner;

class RetainedCellRuntime;
class RetainedGripperRuntime;

struct RetainedExecutionHostConfig final {
  core::CacheConfig cache;
  core::ExecutionKernelConfig kernel;
  SimpleSchedulerConfig scheduler;
  /// Exact motion-resource order from the retained runtime-unit product.
  /// Empty remains available only to lower-level fixtures that intentionally
  /// bootstrap from the scene's already-established state.
  std::vector<double> initial_motion_positions_rad;
};

using RetainedMotionBackendRouteRebinder = std::function<core::Result<void>(
    core::RuntimeParticipantBinding,
    std::vector<core::RuntimeParticipantBinding>)>;

/// One exact execution adapter set selected by a retained runtime product.
struct RetainedRuntimeBackendSet final {
  std::shared_ptr<core::IExecutionBackend> execution;
  RetainedMotionBackendRouteRebinder rebind_motion_route;
  /// Active TCP (M7): the robot's Set TCP step; absent: Set TCP is refused
  /// (`set_tcp_unavailable`).
  std::optional<core::SetTcpStep> set_tcp{};
};

using RetainedRuntimeBackendFactory = std::function<RetainedRuntimeBackendSet(
    const core::RuntimeParticipantBinding &,
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
        const std::vector<core::ActiveControlConfigurationDTO>
            &candidate_configurations)>;

struct RetainedResolvedParticipant final {
  core::RuntimeParticipantBinding binding;
  bool intrinsic_direct_control{};
  /// B7: the owner of the motion endpoint's frame (the index's owner of its
  /// model); the Kernel stamps the request's TCP frame with it.
  std::optional<core::ControllableComponentId> motion_endpoint_owner{};
};

using RetainedRuntimeParticipantResolver =
    std::function<core::Result<RetainedResolvedParticipant>(
        const core::OperationIntentDTO &, std::size_t)>;

using RetainedRuntimeParticipantResolverFactory =
    std::function<RetainedRuntimeParticipantResolver(
        std::vector<core::RuntimeParticipantBinding>)>;

/// Adapt a cell-wide Program-binding resolver to exact composed Kernel owners.
/// Physical representatives retain the native Stop-owner validation. Logical
/// outputs retain their actual channel engine and sealed provider binding;
/// neither requires the motion runtime to own the stable cell resolver.
[[nodiscard]] core::RuntimeParticipantAdmissionResolver
make_retained_cell_kernel_participant_resolver(
    RetainedRuntimeParticipantResolver binding_resolver,
    std::vector<core::RuntimeParticipantExecutionRegistration>
        physical_registrations);

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

/// Existing retained model-map exposures, restricted to the exact motion
/// component/model or the models of components mounted on it (a tool's
/// TCPs), form the planner's frame inventory: each frame once, stamped with
/// its owner (B7). Without a map the focused-runtime legacy inventory remains
/// exactly the participant's frames.
[[nodiscard]] std::vector<core::ResolvedFrameDTO>
retained_motion_planner_frames(
    const core::RuntimeParticipantBinding &runtime,
    const deployment::VerifiedRetainedModelMap *model_map = nullptr);

/// LOADFIX9: the compiled names of `retained_motion_planner_frames`, in the
/// same order. The motion model generation computes FK for each of them.
[[nodiscard]] std::vector<std::string> retained_motion_planner_frame_names(
    const core::RuntimeParticipantBinding &runtime,
    const deployment::VerifiedRetainedModelMap *model_map);

/// Already-verified inputs to one execution host composition.
///
/// Deployment/product readers and the logical-scene owner remain outside this
/// boundary. They supply sealed bindings, one exact retained scene/projector,
/// a planner whose worker model has the same projection revision, and a
/// reference resolver backed by the logical-scene owner. No path, model name,
/// robot default, or group selector is accepted here.
struct RetainedExecutionHostInput final {
  core::RuntimeParticipantBinding motion_binding;
  /// Repository-read-back Stop readiness for the exact physical motion
  /// binding. Runtime composition never fabricates lifecycle or active
  /// configuration generations from retained desired state.
  std::optional<core::RuntimeParticipantStopReadiness> motion_stop_readiness;
  /// Additional Program contexts for the same exact physical motion target.
  /// This is used by build-owned direct-control Programs; aliases may differ
  /// only in Program/role identity and cannot widen the physical runtime.
  std::vector<core::RuntimeParticipantBinding> motion_binding_aliases;
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
  /// Optional product-selected state authority. Empty preserves the native
  /// predicted-state channel. A controller-backed simulation supplies its
  /// exact commanded source set here before constructing the sole writer.
  std::optional<RetainedObservedStateChannelConfig> motion_state_channel_config;
  /// Optional retained-product execution adapter factory. Empty preserves the
  /// existing native execution implementation.
  RetainedRuntimeBackendFactory backend_factory;
  /// Optional owner-coordinated scene observation latch retained for every
  /// fresh planning admission.
  std::shared_ptr<core::IPlanningSceneSynchronizer> planning_scene_synchronizer;
  /// Complete build-owned physical-control inventory for route readiness. It contains one exact representative for every selected
  /// graph control binding while the motion binding above remains the
  /// sole backend owner. Empty preserves the focused-runtime one-binding-per-
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
  /// Used only to verify the construction-time planner inventory; not a
  /// separate endpoint registry or an operation frame-selection authority.
  std::optional<deployment::VerifiedRetainedModelMap> planner_model_map{};
};

struct RetainedExecutionHost;

/// Candidate product facts for a retained component. The host has already
/// compiled and verified the candidate bindings/readiness; this API performs
/// only the physical owner re-seal while the exact Apply fence is held.
/// Candidate collision resources are deliberately not accepted here: the
/// Core collision-resource replacement seam is not yet transactional, so the
/// host must update them through its separately verified cell-level path.
struct RetainedExecutionHostResealInput final {
  core::RuntimeParticipantBinding motion_binding;
  std::vector<core::RuntimeParticipantBinding> motion_binding_aliases;
  std::vector<core::RuntimeParticipantBinding> control_binding_representatives;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      participant_registrations;
  /// Independently owned grippers in the same managed cell. They are peers,
  /// not subordinate controllers of this motion component. Route publication
  /// must preserve their registrations and Stop sources in the shared Kernel.
  std::vector<std::shared_ptr<RetainedGripperRuntime>>
      independent_gripper_peers;
  /// Non-owning stable cell authority; the Composition pins it through every
  /// publication and clears/rebinds this pointer before retiring an owner.
  RetainedCellRuntime *cell_runtime_owner{};
  std::shared_ptr<core::IReferencePoseResolver> reference_resolver;
  core::ConfigurationFenceId apply_fence_id;
  core::ConfigurationFenceScopeDTO apply_scope;
  core::IResourceOperationAdmissionGate &admission_gate;
};

[[nodiscard]] core::Result<void> reseal_retained_execution_host_for_apply(
    RetainedExecutionHost &runtime, RetainedExecutionHostResealInput input);

/// Candidate whole-cell model computations for an already re-sealed retained
/// motion component. The generation was built before the fence from the
/// candidate planning model and the candidate (re-sealed) bindings.
struct RetainedExecutionHostModelRefreshInput final {
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
[[nodiscard]] core::Result<void> refresh_retained_execution_host_model_for_apply(
    RetainedExecutionHost &runtime,
    RetainedExecutionHostModelRefreshInput input);

/// One selected binding's route publication input: the binding resealed
/// against the active configuration, its verified Stop readiness, and the
/// matching route-readiness row.
struct RouteReadinessParticipant final {
  core::RuntimeParticipantBinding participant_binding;
  core::RuntimeParticipantStopReadiness stop_readiness;
  core::RouteReadinessPublicationRowDTO readiness_row;
};

/// Native component composition root. It borrows the process-stable cell
/// Kernel, whose owner retires first and unsubscribes from its engines; then
/// the subordinate Kernel quiesces and backends/scheduler/state/model
/// resources retire.
struct RetainedExecutionHost final {
  explicit RetainedExecutionHost(core::RuntimeParticipantBinding motion)
      : motion_binding(std::move(motion)) {}

  ~RetainedExecutionHost();

  RetainedExecutionHost(const RetainedExecutionHost &) = delete;
  RetainedExecutionHost &operator=(const RetainedExecutionHost &) = delete;

  core::RuntimeParticipantBinding motion_binding;
  std::vector<core::RuntimeParticipantBinding> motion_binding_aliases;
  std::vector<core::RuntimeParticipantBinding> control_binding_representatives;
  std::optional<core::MotionInterruptStopProfileDTO>
      motion_operator_stop_profile;
  std::optional<core::MotionInterruptStopProfileDTO> motion_fault_stop_profile;
  /// The fault profile above is Deployment-authored (Q37), not derived.
  bool motion_fault_stop_profile_from_deployment{false};
  std::optional<core::RequestOwnerEpoch> addressed_request_owner_epoch;
  std::optional<core::CellId> addressed_cell_id;
  std::shared_ptr<RetainedObservedStateChannel> motion_state_channel;

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
  std::shared_ptr<RetainedPlanningSceneSynchronizer>
      planning_scene_synchronizer;
  RetainedMotionBackendRouteRebinder rebind_motion_route;
  /// Active TCP (M7): from the backend set; on every registration of the
  /// motion engine (physical, representatives, aliases).
  std::optional<core::SetTcpStep> set_tcp;
  /// Exact non-speed motion policy sealed against this motion target. It
  /// carries the Deployment-selected ordinary-motion jerk profile only.
  std::optional<core::RuntimeParticipantPolicySelection>
      motion_participant_policy;
  std::optional<double> deployment_maximum_tcp_linear_speed_mps;
  core::PerJointDynamicsLimitsDTO direct_control_joint_limits;

  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  collision::ManyForgeCollisionEngine::SharedModelPtr collision_shared_model;
  collision::ManyForgeCollisionEngine::SharedSceneCachePtr
      geometric_contact_scene_cache;
  std::shared_ptr<core::ICollisionQueryEngine> runtime_collision_engine;
  /// Retained endpoint-runtime query policy reused by independent geometric
  /// input adapters; it is construction policy, not a second timing owner.
  core::Duration collision_monitor_period_ns{};
  core::Duration runtime_collision_immediate_query_budget_ns{};
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      validation_collision_engines;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      lookahead_collision_engines;

  std::shared_ptr<core::ExecutionKernel> participant_engine;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      participant_registrations;
  core::RuntimeParticipantAdmissionResolver participant_resolver;
  RetainedRuntimeParticipantResolverFactory participant_resolver_factory;
  /// Adopt the pre-staged factory only after exact stable-cell product commit.
  /// Caller retains the existing product-install exclusion across publication
  /// and adoption; no Core lock is held when taking the route mutex here.
  void AdoptVerifiedParticipantResolverFactory(
      RetainedRuntimeParticipantResolverFactory &&factory);
  /// Set once by composition; staging never changes lifecycle/controller state.
  RetainedRobotMeasuredSourcePreparer prepare_measured_sources;
  /// Borrowed process-stable cell Kernel and journal (BindCellRuntimeOwner);
  /// their owner outlives all registered engines.
  core::SqliteOperationJournal *cell_operation_journal{};
  core::ResourceAddressedExecutionKernel *cell_addressed_kernel{};
  /// Non-owning root used only to assemble complete cell-level route updates.
  RetainedExecutionHost *cell_runtime_root{};
  /// Additional independently built robot engines owned by this cell-level
  /// runtime. All of them borrow the same stable cell Kernel and journal.
  std::vector<std::shared_ptr<RetainedExecutionHost>> participant_peers;
  /// Independently managed grippers share the stable cell Kernel but are not
  /// subordinate to any motion component.
  std::vector<std::shared_ptr<RetainedGripperRuntime>>
      independent_gripper_peers;
  /// Existing installed peer inventory, updated under the same route mutex.
  /// The weak self alias supplies lifetime only, never a second inventory.
  // The actual host installation borrows the process-stable cell Kernel and
  // journal here; component retirement never owns/destroys that borrowed Kernel.
  void BindCellRuntimeOwner(RetainedCellRuntime& cell,
                            const std::string& runtime_epoch,
                            const std::string& cell_id);

  void BindIndependentGripperPeers(
      const std::shared_ptr<RetainedExecutionHost>& root,
      std::vector<std::shared_ptr<RetainedGripperRuntime>> peers);
  /// Non-owning stable cell authority for managed component compositions.
  RetainedCellRuntime *cell_runtime_owner{};

  [[nodiscard]] core::Result<std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &configuration) const;

  /// Value snapshot of this component's current physical bindings: the motion
  /// binding. Route rebinds replace it under the cell root's route_publication_mutex_; this read
  /// takes the same mutex, so it never observes a staged or rolled-back
  /// route. The caller must not hold that mutex.
  [[nodiscard]] std::vector<core::RuntimeParticipantBinding>
  CurrentPhysicalBindings() const;

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &configuration,
      std::vector<RouteReadinessParticipant> participants);

  [[nodiscard]] core::Result<std::optional<core::ActiveControlConfigurationDTO>>
  ReadCurrentPublishedRouteConfiguration(
      const core::ControlDomainId &control_domain_id) const;

  [[nodiscard]] core::Result<core::RobotMeasuredStateSourceView>
  PrepareCurrentMeasuredSources();

  /// Render-only viewer input: the enabled measured source currently prepared
  /// for this runtime's motion target, or empty. Never waits on the route
  /// publication fence (contention reads as absent); grants no state
  /// authority, readiness or freshness beyond the source's own budget.
  [[nodiscard]] std::optional<core::RequiredRobotMeasuredStateSource>
  TryReadMotionMeasuredSource() const;

private:
  friend class RetainedGripperRoutePublicationOwner;
  [[nodiscard]] core::Result<std::shared_ptr<RetainedGripperRoutePublicationOwner>>
  CurrentIndependentGripperRouteOwner(const core::ControlDomainId& domain) const;
  std::weak_ptr<RetainedExecutionHost> installed_self_;
  friend core::Result<void> reseal_retained_execution_host_for_apply(
      RetainedExecutionHost &runtime,
      RetainedExecutionHostResealInput input);
  friend core::Result<void> refresh_retained_execution_host_model_for_apply(
      RetainedExecutionHost &runtime,
      RetainedExecutionHostModelRefreshInput input);
  friend core::Result<void> rejoin_retained_execution_host_participants(
      const std::shared_ptr<RetainedExecutionHost> &primary,
      std::vector<std::shared_ptr<RetainedExecutionHost>> peers,
      RetainedRuntimeParticipantResolverFactory participant_resolver_factory,
      core::IResourceOperationAdmissionGate &admission_gate,
      const core::ConfigurationFenceId &fence_id,
      const core::ConfigurationFenceScopeDTO &scope);

  std::shared_ptr<const RetainedPreparedRobotMeasuredSources>
      prepared_measured_sources_;
  mutable std::mutex route_publication_mutex_;
};

/// Assemble only the product-specific subordinate participant runtime. This
/// is the preparation boundary for a fenced Apply: it opens no operation
/// journal and creates no addressed Kernel; the host binds the stable cell
/// Kernel afterwards (BindCellRuntimeOwner). Supplied time capabilities
/// keep a later candidate attached to the same stable owner lifetime and role-
/// isolated process scheduler.
[[nodiscard]] std::shared_ptr<RetainedExecutionHost>
create_retained_execution_host_participants(
    RetainedExecutionHostInput input, RetainedExecutionHostConfig config,
    std::shared_ptr<WallClock> shared_clock = {},
    std::shared_ptr<SimpleScheduler> shared_scheduler = {});

/// Join independently planned robot engines beneath one cell-level addressed
/// Kernel. The participants must share the same clock, scheduler and retained
/// product; this function adds no timing synchronization policy.
void join_retained_execution_host_participants(
    const std::shared_ptr<RetainedExecutionHost> &primary,
    std::vector<std::shared_ptr<RetainedExecutionHost>> peers,
    RetainedRuntimeParticipantResolverFactory participant_resolver_factory);

/// Rebuild cell-local registration/resolver aggregates after candidate
/// per-component re-seals. This never mutates the Kernel and creates no
/// engines; it only publishes new runtime membership after the complete
/// candidate set validates under the exact Apply fence.
[[nodiscard]] core::Result<void>
rejoin_retained_execution_host_participants(
    const std::shared_ptr<RetainedExecutionHost> &primary,
    std::vector<std::shared_ptr<RetainedExecutionHost>> peers,
    RetainedRuntimeParticipantResolverFactory participant_resolver_factory,
    core::IResourceOperationAdmissionGate &admission_gate,
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope);

} // namespace manyforge::roboplan_backend
