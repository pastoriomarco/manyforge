#pragma once

#include "execution_host_config.hpp"
#include "execution_host_scene_bridge.hpp"
#include "product_backed_runtime_participant_resolver.hpp"
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <manyforge_core/types/validation.hpp>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_native_component_owner.hpp>
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_native_cclc_runtime.hpp>
#include <manyforge_roboplan/retained_native_cell_command_readiness_port.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <manyforge_roboplan/controller_launch_process.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {

struct Composition final {
  core::ControllableComponentId controllable_component_id;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> program_binding;
  std::shared_ptr<const deployment::ProgramBinding> direct_control_binding;
  std::shared_ptr<const deployment::ProgramBinding>
      direct_control_gripper_program_binding;
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
  std::shared_ptr<RetainedNativeRuntime> runtime;
  // Managed native cells keep this owner independently of any motion engine.
  // A component view may instead hold an independently managed gripper.
  RetainedNativeCellRuntime *cell_runtime{};
  std::shared_ptr<RetainedNativeGripperRuntime> gripper_runtime;
  std::shared_ptr<const ProductBackedRuntimeParticipantResolver>
      target_resolver;
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
  std::shared_ptr<core::ICollisionQueryEngine>
      candidate_runtime_collision_engine;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      candidate_validation_collision_engines;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      candidate_lookahead_collision_engines;
  core::ProjectionRevisionDTO projection_revision;
  core::ActiveBundlePublicationGeneration publication_generation;
  std::optional<core::TargetRoleId> status_motion_target_role_id;
  std::optional<core::TargetRoleId> status_gripper_target_role_id;
  std::shared_ptr<ManagerSceneAuthorityProxy> scene_authority_proxy;
  // Declared before dependent projection/frame owners, including participant
  // compositions, so their destruction completes before these pins are freed.
  std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> required_projection_pins;
  std::shared_ptr<RetainedPlannerSceneProjection> planner_scene_projection;
  std::shared_ptr<RetainedSceneFrameResolver> scene_frame_resolver;
#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
  std::shared_ptr<void> pick_place_planning_fixture_lifetime;
#endif
  std::unique_ptr<core::AddressedPickPlacePlanningOwner>
      pick_place_planning_owner;
  std::unique_ptr<component_manager::SqliteActiveBundlePublicationStore>
      cclc_active_publication_reader;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      cclc_artifact_source;
  std::shared_ptr<deployment::FilesystemVerifiedRetainedProviderManifestSource>
      cclc_provider_manifest_source;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> ros2_control_runtime_handle;
  // Build-owned selected lane metadata; current route authority remains in
  // the existing runtime publisher and is rechecked for every source view.
  std::optional<RetainedMeasuredJointStateLane> measured_joint_state_lane;
  std::shared_ptr<ControllerLaunchProcess> controller_process;
  std::string controller_request_context;
  std::optional<ControllerLaunchSupervisor::Ensured> prepared_controller_launch;
  bool controller_launch_adopted{};
  std::shared_ptr<RetainedRos2ControlLifecycleProduct> cclc_lifecycle_product;
  // These external CCLC dependencies are declared before the composition so
  // reverse member destruction retires CCLC first, then readiness/route owner,
  // while the runtime and stable cell Kernel remain alive.
  std::unique_ptr<deployment::IRouteConfigurationKernelPublicationOwner>
      cclc_cell_route_publication_owner;
  std::unique_ptr<RetainedNativeCellCommandReadinessPort>
      cclc_cell_command_readiness_port;
  std::unique_ptr<deployment::CCLCRuntimeComposition> cclc_runtime;
  std::function<core::Result<void>()> cclc_cell_route_reconciler;
  std::atomic_bool cclc_attached{false};
  std::atomic_bool route_recovery_pending{false};
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
      std::shared_ptr<RetainedNativeRuntime> selected_runtime,
      core::ProjectionRevisionDTO selected_projection,
      core::ActiveBundlePublicationGeneration selected_generation,
      std::optional<core::TargetRoleId> selected_status_motion_target_role_id,
      std::optional<core::TargetRoleId> selected_status_gripper_target_role_id);
};

struct ExecutionCompositionAccess final {
  mutable std::shared_mutex mutex;
  Composition *current{};
  // The stable cell Kernel. It is installed once and outlives every
  // composition; the Stop lane and cell admission read it without the
  // composition lock.
  std::atomic<core::ResourceAddressedExecutionKernel *> addressed_kernel{};

  void replace_current(Composition *replacement);
};
} // namespace manyforge::roboplan_backend::host_detail
