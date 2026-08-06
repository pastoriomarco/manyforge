#include <manyforge_roboplan/retained_native_runtime.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] RetainedObservedStateChannelConfig native_state_config(
    const core::RuntimeParticipantBinding& binding) {
  const auto& value = binding.value();
  if (!value.control_path.has_value() ||
      value.control_path->state_authority_class !=
          core::StateAuthorityClass::kPredicted) {
    throw std::invalid_argument(
        "native simulated runtime requires a predicted state authority");
  }
  return RetainedObservedStateChannelConfig{
      core::ObservedStateSourceKind::kPredictedState,
      "roboplan_native/" + value.controllable_component_id.value() + "/" +
          value.command_target_id.value() + "/" +
          value.control_path->control_binding_revision.value(),
      0,
      1U};
}

void require_planner_binding(
    const core::RuntimeParticipantBinding& runtime,
    const RoboPlanPlannerBackend& planner,
    const core::ProjectionRevisionDTO& projection_revision) {
  const auto& value = runtime.value();
  const auto& candidate = planner.candidate_binding();
  if (value.model_groups.size() != 1U) {
    throw std::invalid_argument(
        "native planner requires one retained participant model group");
  }
  const auto& model_group = value.model_groups.front();
  const auto model = std::find_if(
      value.model_bindings.begin(), value.model_bindings.end(),
      [&](const auto& binding) {
        return binding.model_instance_id == model_group.model_instance_id;
      });
  if (model == value.model_bindings.end() ||
      std::count_if(value.model_bindings.begin(), value.model_bindings.end(),
                    [&](const auto& binding) {
                      return binding.model_instance_id ==
                             model_group.model_instance_id;
                    }) != 1) {
    throw std::invalid_argument(
        "native planner model group has no exact retained model binding");
  }
  std::vector<core::ResolvedFrameDTO> permitted_frames;
  permitted_frames.reserve(value.frame_bindings.size());
  for (const auto& frame : value.frame_bindings) {
    permitted_frames.emplace_back(core::ResolvedComponentModelFrameDTO{
        value.controllable_component_id, frame.model_instance_id,
        frame.frame_role_id, frame.compiled_frame_name,
        frame.model_revision});
  }
  std::vector<std::string> resource_joint_names;
  resource_joint_names.reserve(value.resources.size());
  for (const auto& resource : value.resources) {
    resource_joint_names.push_back(resource.compiled_resource_name);
  }
  if (planner.projection_binding().projection_revision !=
          projection_revision ||
      candidate.controllable_component_id !=
          value.controllable_component_id ||
      candidate.command_target_id != value.command_target_id ||
      candidate.model_instance_id != model_group.model_instance_id ||
      candidate.model_revision != model->model_revision ||
      candidate.compiled_group_name !=
          model_group.compiled_group_name ||
      candidate.compiled_joint_names !=
          resource_joint_names ||
      candidate.permitted_frames != permitted_frames) {
    throw std::invalid_argument(
        "native planner crosses its retained participant or projection");
  }
  std::vector<core::ResourceId> resources;
  resources.reserve(value.resources.size());
  for (const auto& resource : value.resources) {
    resources.push_back(resource.resource_id);
  }
  if (candidate.resources != resources) {
    throw std::invalid_argument(
        "native planner resource order differs from its retained target");
  }
}

[[nodiscard]] std::shared_ptr<core::ICollisionQueryEngine> collision_engine(
    const collision::ManyForgeCollisionEngine::SharedModelPtr& model,
    const collision::ManyForgeCollisionEngine::SharedSceneCachePtr& cache) {
  return std::make_shared<collision::ManyForgeCollisionEngine>(model, cache);
}

}  // namespace

std::shared_ptr<RetainedNativeRuntime> create_retained_native_runtime(
    RetainedNativeRuntimeInput input, RetainedNativeRuntimeConfig config) {
  if (!input.scene || !input.scene_mutex || !input.state_projector ||
      !input.collision_publisher || !input.planner ||
      !input.reference_resolver || config.request_owner_epoch.empty() ||
      config.request_replay_capacity == 0U ||
      config.request_replay_capacity > core::kMaxI6OperationReplayEntries ||
      config.kernel.validation_collision_workers < 1 ||
      config.kernel.validation_collision_workers > 64 ||
      config.kernel.runtime_collision_lookahead_workers < 1 ||
      config.kernel.runtime_collision_lookahead_workers > 64 ||
      config.gripper_update_period_ns <= 0) {
    throw std::invalid_argument(
        "native runtime composition input is incomplete or unbounded");
  }
  const auto current_inventory =
      input.collision_publisher->current_scene_snapshot_lease_inventory();
  if (!current_inventory.has_value() ||
      !current_inventory->projection_binding.valid() ||
      !current_inventory->bundle.world ||
      !current_inventory->bundle.policy.has_value()) {
    throw std::invalid_argument(
        "native runtime requires one initialized retained collision world");
  }
  const auto& projection_revision =
      current_inventory->projection_binding.projection_revision;
  const auto& motion_value = input.motion_binding.value();
  if (projection_revision.retained_bundle_id !=
          motion_value.retained_bundle_id ||
      projection_revision.graph_revision != motion_value.graph_revision) {
    throw std::invalid_argument(
        "native runtime motion binding crosses its collision projection");
  }
  require_planner_binding(input.motion_binding, *input.planner,
                          projection_revision);

  auto runtime = std::make_shared<RetainedNativeRuntime>(
      input.motion_binding, input.gripper_binding);
  runtime->scene = std::move(input.scene);
  runtime->scene_mutex = std::move(input.scene_mutex);
  runtime->state_projector = std::move(input.state_projector);
  runtime->collision_publisher = std::move(input.collision_publisher);

  runtime->clock = std::make_unique<WallClock>();
  runtime->scheduler =
      std::make_unique<SimpleScheduler>(std::move(config.scheduler));
  runtime->cache = std::make_unique<core::TrajectoryCache>(config.cache);
  runtime->planner = std::move(input.planner);
  runtime->reference_resolver = std::move(input.reference_resolver);

  runtime->motion_state_channel =
      std::make_shared<RetainedObservedStateChannel>(
          runtime->motion_binding, runtime->state_projector,
          native_state_config(runtime->motion_binding));
  const auto motion_initialized =
      runtime->motion_state_channel->initialize_from_projection(
          runtime->clock->now());
  if (!motion_initialized.applied()) {
    throw std::invalid_argument(
        "native motion state could not be seeded from the retained model");
  }
  runtime->angular_observed_state =
      std::make_unique<core::AddressedAngularObservedRobotStateAdapter>(
          runtime->motion_state_channel->publication());
  runtime->kinematics =
      std::make_unique<RetainedRoboPlanKinematicsAdapter>(
          runtime->motion_binding, runtime->scene, runtime->scene_mutex);
  runtime->trajectory_validator = RoboPlanTrajectoryValidator::create(
      runtime->motion_binding, runtime->scene, runtime->scene_mutex);
  runtime->collision_trajectory_projector =
      RoboPlanCollisionTrajectoryProjector::create(
          runtime->motion_binding, runtime->scene, runtime->scene_mutex);
  runtime->execution = std::make_unique<RoboPlanExecutionBackend>(
      runtime->motion_binding, runtime->motion_state_channel,
      *runtime->clock, std::move(config.execution));

  if (runtime->gripper_binding.has_value()) {
    const auto& gripper_value = runtime->gripper_binding->value();
    if (gripper_value.retained_bundle_id !=
            motion_value.retained_bundle_id ||
        gripper_value.graph_revision != motion_value.graph_revision ||
        gripper_value.controllable_component_id !=
            motion_value.controllable_component_id) {
      throw std::invalid_argument(
          "native aggregate gripper crosses its motion component generation");
    }
    runtime->gripper_state_channel =
        std::make_shared<RetainedObservedStateChannel>(
            *runtime->gripper_binding, runtime->state_projector,
            native_state_config(*runtime->gripper_binding));
    const auto gripper_initialized =
        runtime->gripper_state_channel->initialize_from_projection(
            runtime->clock->now());
    if (!gripper_initialized.applied()) {
      throw std::invalid_argument(
          "native gripper state could not be seeded from the retained model");
    }
    runtime->gripper = std::make_unique<RetainedGripperCommandBackend>(
        *runtime->gripper_binding, runtime->gripper_state_channel,
        *runtime->clock, *runtime->scheduler,
        config.gripper_update_period_ns);
  }

  const auto& shared_model =
      runtime->collision_publisher->collision_shared_model();
  if (!shared_model) {
    throw std::invalid_argument(
        "native collision publisher has no retained shared model");
  }
  runtime->runtime_collision_engine = collision_engine(
      shared_model,
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          shared_model));
  const auto validation_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          shared_model);
  runtime->validation_collision_engines.reserve(
      static_cast<std::size_t>(config.kernel.validation_collision_workers));
  for (std::size_t index = 0U;
       index < static_cast<std::size_t>(
                   config.kernel.validation_collision_workers);
       ++index) {
    runtime->validation_collision_engines.push_back(
        collision_engine(shared_model, validation_cache));
  }
  const auto lookahead_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          shared_model);
  runtime->lookahead_collision_engines.reserve(
      static_cast<std::size_t>(
          config.kernel.runtime_collision_lookahead_workers));
  for (std::size_t index = 0U;
       index < static_cast<std::size_t>(
                   config.kernel.runtime_collision_lookahead_workers);
       ++index) {
    runtime->lookahead_collision_engines.push_back(
        collision_engine(shared_model, lookahead_cache));
  }

  core::ExecutionKernelPorts ports{
      *runtime->angular_observed_state,
      *runtime->kinematics,
      *runtime->reference_resolver,
      *runtime->trajectory_validator,
      *runtime->collision_trajectory_projector,
      nullptr,
      runtime->gripper.get()};
  runtime->participant_engine = std::make_shared<core::ExecutionKernel>(
      *runtime->planner, ports, *runtime->execution, *runtime->clock,
      *runtime->scheduler, *runtime->cache, std::move(config.kernel));
  runtime->participant_engine->set_collision_resources(
      *runtime->collision_publisher, runtime->runtime_collision_engine,
      runtime->validation_collision_engines,
      runtime->lookahead_collision_engines);
  runtime->participant_engine->set_collision_validation_policy_snapshot(
      runtime->collision_publisher->collision_policy_snapshot());

  std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
  registrations.push_back(
      {runtime->motion_binding, runtime->participant_engine.get()});
  if (runtime->gripper_binding.has_value()) {
    registrations.push_back(
        {*runtime->gripper_binding, runtime->participant_engine.get()});
  }
  runtime->addressed_kernel =
      std::make_unique<core::ResourceAddressedExecutionKernel>(
          std::move(registrations), *runtime->clock,
          core::ResourceAddressedExecutionKernelConfig{
              std::move(config.request_owner_epoch),
              config.request_replay_capacity});
  return runtime;
}

}  // namespace manyforge::roboplan_backend
