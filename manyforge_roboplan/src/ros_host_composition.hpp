#pragma once

#include "component_runtime_pins.hpp"
#include "mock_io_input_result.hpp"

#include "execution_host_config.hpp"
#include "host_component_runtime.hpp"
#include <manyforge_roboplan/program_reset_position.hpp>
#include <stop_token>
#include "execution_host_scene_bridge.hpp"
#include "product_backed_runtime_participant_resolver.hpp"
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <manyforge_core/types/validation.hpp>
#include <manyforge_core/ports/iio_signal_service.hpp>
#include <manyforge_core/types/io_activation_rule.hpp>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_deployment/component_spec.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_execution_host.hpp>
#include <manyforge_roboplan/retained_cell_runtime.hpp>
#include <manyforge_roboplan/retained_component_owner.hpp>
#include <manyforge_roboplan/retained_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_gripper_route_publication_owner.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <manyforge_roboplan/controller_launch_process.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace manyforge::ros2_control_execution { class VolatileMotionTrace; }

namespace manyforge::roboplan_backend { struct ComponentAdapterFamily; }

namespace manyforge::roboplan_backend::host_detail {

// Metadata classification only, on the caller's verified retained graph. A
// genuine I/O provider-only component has no physical representative to plan.
// This grants no driver installation, lifecycle/readiness or command authority.
[[nodiscard]] bool IsProviderOnlyNativeComponent(
    const core::CellGraphDTO &qualified_graph,
    const core::ControllableComponentId &component_id);

class HostIoSignalRuntime;
struct HostIoOutputChannelEntry;

struct CompositionIoResourcePreparation final {
  core::ActiveBundlePublicationGeneration publication_generation;
  bool complete{};
};

struct CellFrameStateSource final {
  RetainedSceneFrameStateSourceInput frame_source;
  std::shared_ptr<const core::ObservedRobotStatePublication> numeric_publication;
};

// Common actual cell product data. Physical components borrow this SAME base;
// a zero-component cell owns this value directly, without a dummy component.
struct ExecutionProduct {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> program_binding;
  std::optional<deployment::PreparedActivationProgramOutcomeDTO> selected_program_outcome{};
  std::shared_ptr<core::IIoSignalService> io_signal_service{};
  std::shared_ptr<const core::IoActivationRuleCatalogDTO> compiled_global_io_rules{};
  // Mutually exclusive with the actual service catalog. Same prepared product
  // ownership; no service/catalog generation exists for this waiting product.
  std::shared_ptr<const core::IoRuleWaitingForProgramCatalogDTO>
      compiled_waiting_global_io_rules{};
  std::shared_ptr<HostIoSignalRuntime> io_signal_runtime{};
  std::shared_ptr<const ProductBackedRuntimeParticipantResolver> target_resolver;
  core::ProjectionRevisionDTO projection_revision;
  core::ActiveBundlePublicationGeneration publication_generation;
  std::optional<core::NamedApplyPublicationTransactionDTO> origin_named_apply{};

  ExecutionProduct(std::shared_ptr<const deployment::RetainedBundle> selected_bundle,
      std::shared_ptr<const deployment::ProgramBindingContract> selected_program,
      std::shared_ptr<const deployment::ProgramBinding> selected_binding,
      core::ProjectionRevisionDTO selected_projection,
      core::ActiveBundlePublicationGeneration selected_generation);
  ~ExecutionProduct();
  ExecutionProduct(const ExecutionProduct&) = delete;
  ExecutionProduct& operator=(const ExecutionProduct&) = delete;
};

struct Composition final : ExecutionProduct {
  core::ControllableComponentId controllable_component_id;
  // One actual generic transport holder shared by all generic devices and
  // retained across Apply; physical transport holders remain independent.
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> io_transport{};
  // CP9: the host's one component-stream sample bridge (Python sinks), pinned
  // by every composition hosting a pose source; on `io_transport`'s node.
  std::shared_ptr<void> component_stream_sample_bridge{};
  // The actual output leaves, shared across retained Apply. Metadata lives in
  // each leaf; this is only its existing composition lifetime pin.
  std::vector<std::shared_ptr<HostIoOutputChannelEntry>> io_output_channels{};
  // Completion of actual resource construction only, never lifecycle/readiness.
  // A partial failure remains pinned and cannot become an inert successful replay.
  std::optional<CompositionIoResourcePreparation> io_resource_preparation{};
  // Complete actual driver ownership, retained as this SAME immutable pin
  // only after exact component/domain/membership retention is established.
  std::shared_ptr<const IoProviderDriverGroup> io_provider_drivers{};
  std::shared_ptr<const ComponentRuntimePins> component_runtime_pins{};
  // Sealed logical input inventory; these bindings create no physical owner.
  std::vector<std::shared_ptr<const deployment::ProgramBinding>>
      direct_control_io_program_bindings{};
  std::shared_ptr<const deployment::ProgramBinding> direct_control_binding;
  std::shared_ptr<const deployment::ProgramBinding>
      direct_control_gripper_program_binding;
  // Construction-time physical target identity (component, target,
  // capability, resources). Route publication later rebinds the runtime's
  // control path (route, activation set, active configuration generation)
  // without updating these copies: read route-specific facts from the
  // runtime's current binding (RetainedExecutionHost::CurrentPhysicalBindings,
  // RetainedGripperRuntime::binding), never from here.
  std::optional<core::RuntimeParticipantBinding> direct_control_motion_binding;
  std::optional<core::RuntimeParticipantBinding>
      direct_control_gripper_binding;
  std::optional<core::JointLimitsDTO> direct_control_joint_limits;
  CanonicalValue direct_control_motion_descriptor;
  CanonicalValue direct_control_gripper_descriptor;
  // The staged cell planning-model files of this composition's product. The
  // runtime's RoboPlan worker reloads them by path whenever it restarts, so a
  // retained runtime shares this stage with every composition that retains it
  // while the cell planning model is unchanged.
  std::shared_ptr<const RetainedPlanningModelStage> stage;
  // Present only for a retained component whose candidate product carries
  // another cell planning model. Every model-dependent computation is
  // prepared from `stage` before the fence and installed under it: a motion
  // controller's worker base model and its complete kinematic generation
  // (scene, state projector, kinematics, validator, collision projector); an
  // independently owned model-backed gripper's state projector.
  std::optional<RoboPlanWorkerModelDTO> candidate_planner_model;
  std::shared_ptr<const RetainedKinematicModelGeneration>
      candidate_model_generation;
  std::shared_ptr<RetainedKinematicStateProjector>
      candidate_tool_state_projector;
  // A retained motion controller's candidate planner settings (cell
  // context), installed under the Apply fence with the model.
  std::optional<deployment::PlannerSettings> candidate_planner_settings;
  std::shared_ptr<RetainedExecutionHost> runtime;
  // Managed native cells keep this owner independently of any motion engine.
  // A component view may instead hold an independently managed gripper.
  RetainedCellRuntime *cell_runtime{};
  std::shared_ptr<RetainedGripperRuntime> gripper_runtime;
  // On the root composition of a cell without an arm runtime: the route
  // publication owner over the cell's grippers (`open_routes`, CONTRACTS §6).
  std::shared_ptr<RetainedGripperRoutePublicationOwner> gripper_route_owner;
  // Candidate metadata for an owner retained from the predecessor. The
  // runtime's registrations are re-sealed only at fenced installation.
  std::vector<core::RuntimeParticipantBinding>
      candidate_control_representatives;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      candidate_component_registrations;
  RetainedRuntimeParticipantResolverFactory
      candidate_participant_resolver_factory;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher>
      candidate_collision_publisher;
  collision::ManyForgeCollisionEngine::SharedModelPtr
      candidate_collision_model;
  collision::ManyForgeCollisionEngine::SharedSceneCachePtr
      candidate_geometric_contact_scene_cache;
  std::shared_ptr<core::ICollisionQueryEngine>
      candidate_runtime_collision_engine;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      candidate_validation_collision_engines;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      candidate_lookahead_collision_engines;
  std::optional<core::TargetRoleId> status_motion_target_role_id;
  std::optional<core::TargetRoleId> status_gripper_target_role_id;
  std::shared_ptr<ManagerSceneAuthorityProxy> scene_authority_proxy;
  // Declared before dependent projection/frame owners, including participant
  // compositions, so their destruction completes before these pins are freed.
  std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> required_projection_pins;
  std::shared_ptr<RetainedPlannerSceneProjection> planner_scene_projection;
  std::shared_ptr<RetainedSceneFrameResolver> scene_frame_resolver;
  // Same qualified physical sources as the cell frame resolver, including
  // numeric channel metadata for authorities that must remain unavailable.
  std::vector<CellFrameStateSource> cell_frame_state_sources;
#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
  std::shared_ptr<void> pick_place_planning_fixture_lifetime;
#endif
  std::unique_ptr<core::AddressedPickPlacePlanningOwner>
      pick_place_planning_owner;
  std::unique_ptr<component_manager::SqliteActiveBundlePublicationStore>
      active_publication_reader;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      artifact_source;
  std::shared_ptr<deployment::FilesystemVerifiedRetainedProviderManifestSource>
      provider_manifest_source;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> ros2_control_runtime_handle;
  // Prepared with the candidate; published only when this composition is selected.
  HostRobotTcpReader staged_robot_tcp_reader;
  void StageRobotTcpReader();
  // Build-owned selected lane metadata; current route authority remains in
  // the existing runtime publisher and is rechecked for every source view.
  std::optional<RetainedMeasuredJointStateLane> measured_joint_state_lane;
  std::shared_ptr<ControllerLaunchProcess> controller_process;
  std::string controller_request_context;
  std::optional<ControllerLaunchSupervisor::Ensured> prepared_controller_launch;
  bool controller_launch_adopted{};
  /// CONTRACTS §6: this component's adapter, coordinated by the host's one
  /// component runtime (made from the adapter registry, or the generic I/O
  /// driver for an I/O provider component).
  std::shared_ptr<core::IComponentAdapter> component_adapter;
  /// CONTRACTS §16: this component's owner (machine) and member role, when
  /// it is a member; absent otherwise.
  std::optional<std::string> owner_id;
  std::optional<std::string> member_role;
  /// CONTRACTS §16, BOUNDARY §4: on an arm member's composition (the
  /// machine's process runs here), its owner component, built before the arm;
  /// with a lane row, also the owner's family and the handle its members get.
  /// With the arm deactivated the owner is a view of its own and these name
  /// that view (it then runs the machine's process).
  std::optional<core::CoordinatedComponent> owner_component;
  std::shared_ptr<void> owner_handle;
  const ComponentAdapterFamily *owner_family{};
  std::vector<std::unique_ptr<Composition>> participant_compositions;

  ~Composition();

  // A failed fence keeps engine/runtime pins, but its uninstalled process
  // has no active owner. Borrowed and already-adopted launches remain owned.
  void discard_unadopted_controller_launches();

  [[nodiscard]] core::ResourceAddressedExecutionKernel *
  active_addressed_kernel() const noexcept;

  [[nodiscard]] const void *component_runtime_identity() const noexcept;

  [[nodiscard]] std::vector<core::RuntimeUnitEpochBindingDTO>
  host_runtime_unit_epochs() const;

  [[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
  component_registrations() const;

  Composition(
      core::ControllableComponentId selected_component_id,
      std::shared_ptr<const deployment::RetainedBundle> selected_bundle,
      std::shared_ptr<const deployment::ProgramBindingContract>
          selected_program,
      std::shared_ptr<const deployment::ProgramBinding> selected_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_gripper_program_binding,
      std::optional<core::RuntimeParticipantBinding>
          selected_direct_control_motion_binding,
      std::optional<core::RuntimeParticipantBinding>
          selected_direct_control_gripper_binding,
      std::optional<core::JointLimitsDTO> selected_direct_control_joint_limits,
      CanonicalValue selected_direct_control_motion_descriptor,
      CanonicalValue selected_direct_control_gripper_descriptor,
      std::shared_ptr<const RetainedPlanningModelStage> selected_stage,
      std::shared_ptr<RetainedExecutionHost> selected_runtime,
      core::ProjectionRevisionDTO selected_projection,
      core::ActiveBundlePublicationGeneration selected_generation,
      std::optional<core::TargetRoleId> selected_status_motion_target_role_id,
      std::optional<core::TargetRoleId> selected_status_gripper_target_role_id);
};

/// CONTRACTS §16: the machine owners that ride on a member's view (an armed
/// machine's owner is built on its arm's composition, with no view of its
/// own), sorted. A retained member keeps its carried owner.
[[nodiscard]] std::vector<core::ControllableComponentId>
MemberCarriedOwnerIds(const Composition &cell);

[[nodiscard]] core::Result<MockIoInputSetResult> SetCompositionMockInput(
    Composition &composition, const core::IoSignalId &signal,
    const core::IoSourceIncarnation &expected_source_epoch, bool value);

struct ExecutionCompositionAccess final {
  // Stable host diagnostic owner; independent of component/rebind lifetime.
  std::shared_ptr<ros2_control_execution::VolatileMotionTrace> motion_trace{};
  mutable std::shared_mutex mutex;
  Composition *current{};
  // Mutually exclusive with current, changed under this SAME mutex. No
  // physical owner/engine/controllable component is invented for logic alone.
  std::shared_ptr<ExecutionProduct> logical_product{};
  // Caller holds mutex. Invalid parallel physical/logical selections refuse;
  // there is no precedence fallback to a stale copy.
  [[nodiscard]] ExecutionProduct *product() const;
  // The stable cell Kernel. It is installed once and outlives every
  // composition; the Stop lane and cell admission read it without the
  // composition lock.
  std::atomic<core::ResourceAddressedExecutionKernel *> addressed_kernel{};
  // Installed with the stable Kernel and retained independently of products.
  std::atomic<core::SqliteOperationJournal *> operation_journal{};
  // CONTRACTS §1-§6: the cell's component runtime, installed once with the
  // stable Kernel and kept across Apply. The Stop lane and the host tick read
  // it without the composition lock.
  std::shared_ptr<HostComponentRuntime> component_runtime{};

  std::function<core::Result<ProgramResetPositionCapability>(Composition&)>
      reset_robot_position_capability;
  std::function<core::Result<void>(Composition&, const ProgramResetPositionRequest&, std::stop_token)>
      reset_robot_position;

  // Runs slow verified source preparation before a frame's final shared pin.
  // Installed once with the stable host; no callback is invoked under Kernel.
  std::function<core::Result<void>(const core::IoProgramBlackboardScopeDTO &)>
      prepare_program_blackboard_context;

  // Retire borrowed Kernel/journal pointers and quiesce in-flight shared
  // call leases before their actual owners begin destruction.
  void detach_addressed_owner();

  // Both run under the exclusive `mutex` and return the removed components'
  // interrupted action threads (HostComponentRuntime::SetComponents): the
  // caller destroys them only after releasing `mutex`, which those actions
  // take (route opening inside `enable`, a driver restart).
  [[nodiscard]] std::vector<std::jthread> replace_current(Composition *replacement);
  [[nodiscard]] std::vector<std::jthread> replace_logical_product(
      std::shared_ptr<ExecutionProduct> replacement);
};
/// Every component of `cell` with its adapter, in cell order: each view's
/// owner (on an arm member's view), its component adapter, then the I/O
/// provider components of its driver group; members carry owner and role.
/// Owners come before their members (the arm builds the owner; I/O member
/// views follow the robot views). The owner of a machine with its arm
/// deactivated is a view of its own (`owner_component` names the view
/// itself): it is listed once, as the owner.
[[nodiscard]] std::vector<core::CoordinatedComponent> CoordinatedComponentsOf(
    const Composition *cell);

// Callback setup only. No source/readiness/command authority is created.
void BindHostIoRuleEvidenceProcessor(
    ExecutionProduct &product,
    const std::shared_ptr<ExecutionCompositionAccess> &actual_access);

} // namespace manyforge::roboplan_backend::host_detail
