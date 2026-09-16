#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/ports/ireference_pose_resolver.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/route_configuration_publication.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_focused_adapters.hpp>
#include <manyforge_roboplan/retained_gripper_command_backend.hpp>
#include <manyforge_roboplan/retained_stop_surveillance_owner.hpp>
#include <manyforge_roboplan/roboplan_execution_backend.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <utility>
#include <vector>

namespace manyforge::core {
class GripperParticipantEngine;
}

namespace manyforge::roboplan_backend {

// One execution cell may contain multiple independently controlled robots.
// This is a hard construction bound, not a configured group size.
inline constexpr std::size_t kNativeMaximumLiveStopParticipants = 64U;

struct RetainedNativeRuntimeConfig final {
  core::RequestOwnerEpoch request_owner_epoch;
  core::CellId cell_id;
  std::filesystem::path operation_journal_path;
  std::size_t request_replay_capacity{1024U};
  core::CacheConfig cache;
  core::ExecutionKernelConfig kernel;
  RoboPlanExecutionBackendConfig execution;
  SimpleSchedulerConfig scheduler;
  core::Duration gripper_update_period_ns{10'000'000};
  /// Exact motion-resource order from the retained runtime-unit product.
  /// Empty remains available only to lower-level fixtures that intentionally
  /// bootstrap from the scene's already-established state.
  std::vector<double> initial_motion_positions_rad;
  /// Cell-wide Stop envelope supplied by the current configuration owner when
  /// no verified participant readiness is installed yet. It is never
  /// inferred from a not-stoppable retained policy or a test constant.
  std::optional<core::StopDeadlineBudgetDTO> cell_stop_deadline_budget;
  /// Process-owned monitoring policy. Public operation/caller payloads never
  /// select or override these cadences.
  std::optional<core::StopSurveillancePolicyDTO> stop_surveillance_policy;
  core::CellAdmissionInterventionValidator
      cell_admission_intervention_validator;
  core::CellAdmissionSettlementValidator cell_admission_settlement_validator;
  core::CellAdmissionReconcileValidator cell_admission_reconcile_validator;
  std::optional<core::ManagerLivenessRequiredSetDTO>
      manager_liveness_required_set;
};

inline constexpr core::StopSurveillancePolicyDTO
    kMaintainedNativeStopSurveillancePolicy{10'000'000, 100'000'000, 64U};

struct RetainedNativeAddressedOwnerConfig final {
  core::RequestOwnerEpoch request_owner_epoch;
  core::CellId cell_id;
  std::filesystem::path operation_journal_path;
  std::size_t request_replay_capacity{1024U};
  core::IResourceOperationAdmissionGate *operation_admission_gate{};
  core::GraphRevision graph_revision;
  std::optional<core::StopDeadlineBudgetDTO> cell_stop_deadline_budget;
  core::CellAdmissionInterventionValidator
      cell_admission_intervention_validator;
  core::CellAdmissionSettlementValidator cell_admission_settlement_validator;
  core::CellAdmissionReconcileValidator cell_admission_reconcile_validator;
  std::optional<core::ManagerLivenessRequiredSetDTO>
      manager_liveness_required_set;
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
  /// Exact Deployment-qualified retained runtime policy. Absence is
  /// observable and keeps motion admission closed.
  std::optional<core::RuntimePolicyDTO> runtime_policy{};
  /// Exact retained catalog artifact that supplied `runtime_policy`.
  std::optional<core::RuntimePolicyCatalogRecordRefDTO>
      runtime_policy_source_artifact{};
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
  /// Independent Deployment-authored cap for this exact motion target.  It
  /// tightens, but never relabels or weakens, the immutable runtime policy.
  std::optional<double> deployment_maximum_tcp_linear_speed_mps{};
  /// Retained manual-motion ceilings for the exact motion binding above.
  core::PerJointDynamicsLimitsDTO direct_control_joint_limits{};
  /// Saved settings, resolved once against the immutable model at activation.
  std::optional<
      deployment::native_execution_runtime_contract::OrdinaryMotionJerkSettings>
      ordinary_motion_jerk{};
  /// Cell-wide render wake shared by independently owned robot channels.
  std::shared_ptr<RetainedObservationWake> observation_wake{};
};

/// Owning native composition root. Destruction order is deliberate: the public
/// addressed owner unsubscribes first, then the subordinate Kernel quiesces,
/// then backends/scheduler/state/model resources retire.
struct RetainedNativeRuntime final
    : public deployment::IRouteConfigurationKernelPublicationOwner {
  RetainedNativeRuntime(core::RuntimeParticipantBinding motion,
                        std::optional<core::RuntimeParticipantBinding> gripper)
      : motion_binding(std::move(motion)), gripper_binding(std::move(gripper)) {
  }

  RetainedNativeRuntime(const RetainedNativeRuntime &) = delete;
  RetainedNativeRuntime &operator=(const RetainedNativeRuntime &) = delete;

  core::RuntimeParticipantBinding motion_binding;
  std::optional<core::RuntimeParticipantBinding> gripper_binding;
  std::vector<core::RuntimeParticipantBinding> motion_binding_aliases;
  std::vector<core::RuntimeParticipantBinding> gripper_binding_aliases;
  std::vector<core::RuntimeParticipantBinding> control_binding_representatives;
  std::optional<core::StopDeadlineBudgetDTO> cell_stop_deadline_budget;
  std::optional<core::StopSurveillancePolicyDTO> stop_surveillance_policy;
  std::optional<core::MotionInterruptStopProfileDTO>
      motion_operator_stop_profile;
  std::optional<core::MotionInterruptStopProfileDTO> motion_fault_stop_profile;
  std::optional<core::RequestOwnerEpoch> addressed_request_owner_epoch;
  std::optional<core::CellId> addressed_cell_id;
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<RetainedKinematicStateProjector> state_projector;
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
  std::shared_ptr<core::IReferencePoseResolver> reference_resolver;
  std::unique_ptr<core::AddressedAngularObservedRobotStateAdapter>
      angular_observed_state;
  std::unique_ptr<RetainedRoboPlanKinematicsAdapter> kinematics;
  std::unique_ptr<RoboPlanTrajectoryValidator> trajectory_validator;
  std::unique_ptr<RoboPlanCollisionTrajectoryProjector>
      collision_trajectory_projector;
  std::shared_ptr<core::IExecutionBackend> execution;
  std::shared_ptr<core::IGripperCommandBackend> gripper;
  std::shared_ptr<core::IPlanningSceneSynchronizer> planning_scene_synchronizer;
  core::IControlDomainExecutionPort *gripper_stop_execution_port{};
  RetainedMotionBackendRouteRebinder rebind_motion_route;
  RetainedGripperBackendRouteRebinder rebind_gripper_route;
  std::optional<RetainedGripperPositionLimits> gripper_position_limits;
  /// Exact Deployment-selected runtime policy sealed against this motion
  /// target. Retained for focused observability; it is immutable and does not
  /// grant a second policy owner.
  std::optional<core::RuntimeParticipantPolicySelection> motion_runtime_policy;
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
  std::unique_ptr<RetainedStopSurveillanceOwner> stop_surveillance_owner;
  std::unique_ptr<core::SqliteOperationJournal> operation_journal;
  std::unique_ptr<core::ResourceAddressedExecutionKernel> addressed_kernel;
  core::ResourceAddressedExecutionKernel *cell_addressed_kernel{};
  /// Non-owning root used only to assemble complete cell-level route updates.
  RetainedNativeRuntime *cell_runtime_root{};
  /// Additional independently built robot engines owned by this cell-level
  /// runtime. Only the primary owns the addressed Kernel and durable journal.
  std::vector<std::shared_ptr<RetainedNativeRuntime>> participant_peers;

  [[nodiscard]] core::Result<std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &configuration) const override;

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &configuration,
      std::vector<deployment::VerifiedRouteReadinessParticipant> participants)
      override;

  /// Rebind the new in-process runtime to one exact durable predecessor while
  /// retaining the new provider epoch and keeping operation admission closed.
  /// CCLC uses this only during owner replacement before it explicitly
  /// observes and republishes a current-source successor configuration.
  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  AdoptAndReadBackRouteConfigurationPredecessor(
      const core::ActiveControlConfigurationDTO &configuration);

  [[nodiscard]] core::Result<std::optional<core::ActiveControlConfigurationDTO>>
  ReadCurrentPublishedRouteConfiguration(
      const core::ControlDomainId &control_domain_id) const;

  /// Final source check and evidence write share route-publication ordering.
  /// The host supplies the verified confirmation representative and its
  /// explicit policy; this is not an unqualified public mutation endpoint.
  [[nodiscard]] core::Result<core::GripperGraspEvidenceDTO>
  RecordGraspObservation(const core::RuntimeParticipantBinding &source,
                         const core::GripperGraspObservationDTO &observation,
                         core::Duration freshness_budget_ns,
                         core::TimePoint admission_deadline_ns);

  [[nodiscard]] core::Result<core::GripperCommandStatusDTO>
  ReadGripperStatus(const core::RuntimeParticipantBinding &source) const;

private:
  // Caller holds route_publication_mutex_. No owner read or write escapes it.
  [[nodiscard]] bool
  GripperSourceIsCurrent(const core::RuntimeParticipantBinding &source) const;
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

/// Resolve current Stop-recovery facts for either a directly attached owner
/// or the process-stable managed owner. This is one product-facts adapter;
/// ownership of the Stop latch and its clear remains in Core.
[[nodiscard]] core::Result<core::StopFaultRecoveryFactsDTO>
build_retained_native_stop_recovery_facts(
    RetainedNativeRuntime &runtime,
    core::ResourceAddressedExecutionKernel &addressed_kernel,
    core::IResourceOperationAdmissionGate *operation_admission_gate,
    const core::StopStatusDTO &status, std::int64_t assessed_at_ns);

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
                               RetainedNativeRuntimeConfig config);

} // namespace manyforge::roboplan_backend
