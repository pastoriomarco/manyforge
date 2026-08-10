#include <algorithm>
#include <cmath>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] RetainedObservedStateChannelConfig
native_state_config(const core::RuntimeParticipantBinding &binding) {
  const auto &value = binding.value();
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
      0, 1U};
}

void require_planner_binding(
    const core::RuntimeParticipantBinding &runtime,
    const RoboPlanPlannerBackend &planner,
    const core::ProjectionRevisionDTO &projection_revision) {
  const auto &value = runtime.value();
  const auto &candidate = planner.candidate_binding();
  if (value.model_groups.size() != 1U) {
    throw std::invalid_argument(
        "native planner requires one retained participant model group");
  }
  const auto &model_group = value.model_groups.front();
  const auto model = std::find_if(
      value.model_bindings.begin(), value.model_bindings.end(),
      [&](const auto &binding) {
        return binding.model_instance_id == model_group.model_instance_id;
      });
  if (model == value.model_bindings.end() ||
      std::count_if(value.model_bindings.begin(), value.model_bindings.end(),
                    [&](const auto &binding) {
                      return binding.model_instance_id ==
                             model_group.model_instance_id;
                    }) != 1) {
    throw std::invalid_argument(
        "native planner model group has no exact retained model binding");
  }
  std::vector<core::ResolvedFrameDTO> permitted_frames;
  permitted_frames.reserve(value.frame_bindings.size());
  for (const auto &frame : value.frame_bindings) {
    permitted_frames.emplace_back(core::ResolvedComponentModelFrameDTO{
        value.controllable_component_id, frame.model_instance_id,
        frame.frame_role_id, frame.compiled_frame_name, frame.model_revision});
  }
  std::vector<std::string> resource_joint_names;
  resource_joint_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resource_joint_names.push_back(resource.compiled_resource_name);
  }
  if (planner.projection_binding().projection_revision != projection_revision ||
      candidate.controllable_component_id != value.controllable_component_id ||
      candidate.command_target_id != value.command_target_id ||
      candidate.model_instance_id != model_group.model_instance_id ||
      candidate.model_revision != model->model_revision ||
      candidate.compiled_group_name != model_group.compiled_group_name ||
      candidate.compiled_joint_names != resource_joint_names ||
      candidate.permitted_frames != permitted_frames) {
    throw std::invalid_argument(
        "native planner crosses its retained participant or projection");
  }
  std::vector<core::ResourceId> resources;
  resources.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
  }
  if (candidate.resources != resources) {
    throw std::invalid_argument(
        "native planner resource order differs from its retained target");
  }
}

[[nodiscard]] RetainedGripperPositionLimits
retained_gripper_limits(const roboplan::Scene &scene,
                        const core::RuntimeParticipantBinding &binding) {
  const auto &value = binding.value();
  if (value.resources.size() != 1U) {
    throw std::invalid_argument(
        "native gripper limits require one retained resource");
  }
  const auto &resource = value.resources.front();
  const auto joint = scene.getJointInfo(resource.compiled_resource_name);
  if (!joint.has_value() || joint->num_position_dofs != 1U ||
      joint->limits.min_position.size() != 1 ||
      joint->limits.max_position.size() != 1 ||
      !std::isfinite(joint->limits.min_position[0]) ||
      !std::isfinite(joint->limits.max_position[0]) ||
      joint->limits.min_position[0] > joint->limits.max_position[0]) {
    throw std::invalid_argument(
        "native gripper resource has no exact finite retained position limits");
  }
  const bool matching_joint_kind =
      (resource.coordinate_kind == core::CoordinateKind::kAngular &&
       joint->type == roboplan::JointType::REVOLUTE) ||
      (resource.coordinate_kind == core::CoordinateKind::kLinear &&
       joint->type == roboplan::JointType::PRISMATIC);
  if (!matching_joint_kind) {
    throw std::invalid_argument(
        "native gripper resource kind differs from its retained model joint");
  }
  return RetainedGripperPositionLimits{resource.coordinate_kind,
                                       joint->limits.min_position[0],
                                       joint->limits.max_position[0]};
}

[[nodiscard]] std::shared_ptr<core::ICollisionQueryEngine> collision_engine(
    const collision::ManyForgeCollisionEngine::SharedModelPtr &model,
    const collision::ManyForgeCollisionEngine::SharedSceneCachePtr &cache) {
  return std::make_shared<collision::ManyForgeCollisionEngine>(model, cache);
}

[[nodiscard]] bool same_provider_runtime(
    const core::RuntimeCapabilityProviderPathDTO &left,
    const core::RuntimeCapabilityProviderPathDTO &right) noexcept {
  return left.provider_instance_id == right.provider_instance_id &&
         left.provider_manifest_id == right.provider_manifest_id &&
         left.provider_manifest_revision == right.provider_manifest_revision &&
         left.provider_runtime_epoch == right.provider_runtime_epoch;
}

[[nodiscard]] bool same_supporting_provider_runtime(
    const std::vector<core::RuntimeCapabilityProviderPathDTO> &primary,
    const std::vector<core::RuntimeCapabilityProviderPathDTO> &alias) noexcept {
  return !primary.empty() &&
         std::all_of(alias.begin(), alias.end(),
                     [&](const auto &candidate) {
                       return std::any_of(primary.begin(), primary.end(),
                                          [&](const auto &selected) {
                                            return same_provider_runtime(
                                                selected, candidate);
                                          });
                     }) &&
         std::all_of(primary.begin(), primary.end(), [&](const auto &selected) {
           return std::any_of(
               alias.begin(), alias.end(), [&](const auto &candidate) {
                 return same_provider_runtime(selected, candidate);
               });
         });
}

[[nodiscard]] bool same_physical_binding(
    const core::RuntimeParticipantBinding &primary,
    const core::RuntimeParticipantBinding &alias,
    const bool permit_supporting_capability_expansion) noexcept {
  const auto &left = primary.value();
  const auto &right = alias.value();
  return right.contact_grasp_bindings.empty() &&
         left.retained_bundle_id == right.retained_bundle_id &&
         left.graph_revision == right.graph_revision &&
         left.controllable_component_id == right.controllable_component_id &&
         left.component_type_id == right.component_type_id &&
         left.component_type_revision == right.component_type_revision &&
         left.command_target_id == right.command_target_id &&
         left.capability_id == right.capability_id &&
         left.capability_definition_revision ==
             right.capability_definition_revision &&
         left.command_schema == right.command_schema &&
         left.model_bindings == right.model_bindings &&
         left.model_groups == right.model_groups &&
         left.frame_bindings == right.frame_bindings &&
         left.resources == right.resources &&
         left.collision_body_bindings == right.collision_body_bindings &&
         left.control_path == right.control_path &&
         left.capability_provider_path == right.capability_provider_path &&
         (left.supporting_capability_provider_paths ==
              right.supporting_capability_provider_paths ||
          (permit_supporting_capability_expansion &&
           same_supporting_provider_runtime(
               left.supporting_capability_provider_paths,
               right.supporting_capability_provider_paths))) &&
         left.runtime_unit_epochs == right.runtime_unit_epochs;
}

} // namespace

std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime_participants(
    RetainedNativeRuntimeInput input, RetainedNativeRuntimeConfig config,
    std::shared_ptr<WallClock> shared_clock) {
  if (!input.scene || !input.scene_mutex || !input.state_projector ||
      !input.collision_publisher || !input.planner ||
      !input.collision_shared_model || !input.reference_resolver_factory ||
      config.kernel.validation_collision_workers < 1 ||
      config.kernel.validation_collision_workers > 64 ||
      config.kernel.runtime_collision_lookahead_workers < 1 ||
      config.kernel.runtime_collision_lookahead_workers > 64 ||
      config.gripper_update_period_ns <= 0 ||
      (!config.initial_motion_positions_rad.empty() &&
       (config.initial_motion_positions_rad.size() !=
            input.motion_binding.value().resources.size() ||
        std::any_of(
            config.initial_motion_positions_rad.begin(),
            config.initial_motion_positions_rad.end(),
            [](const double value) { return !std::isfinite(value); })))) {
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
  const auto &projection_revision =
      current_inventory->projection_binding.projection_revision;
  const auto &motion_value = input.motion_binding.value();
  const std::size_t registration_count =
      1U + input.motion_binding_aliases.size() +
      (input.gripper_binding.has_value() ? 1U : 0U) +
      input.gripper_binding_aliases.size();
  if (registration_count > core::kMaxOperationParticipants ||
      std::any_of(input.motion_binding_aliases.begin(),
                  input.motion_binding_aliases.end(), [&](const auto &alias) {
                    return !same_physical_binding(input.motion_binding, alias,
                                                  true);
                  })) {
    throw std::invalid_argument(
        "native motion alias crosses or widens its physical runtime binding");
  }
  if ((!input.gripper_binding.has_value() &&
       !input.gripper_binding_aliases.empty()) ||
      (input.gripper_binding.has_value() &&
       std::any_of(input.gripper_binding_aliases.begin(),
                   input.gripper_binding_aliases.end(), [&](const auto &alias) {
                     return !same_physical_binding(*input.gripper_binding,
                                                   alias, false);
                   }))) {
    throw std::invalid_argument(
        "native gripper alias crosses or widens its physical runtime binding");
  }
  if (motion_value.control_path.has_value() &&
      motion_value.control_path->runtime_scale_qualification.has_value()) {
    const auto &runtime_scale =
        *motion_value.control_path->runtime_scale_qualification;
    if (runtime_scale.acknowledgement_kind !=
            core::RuntimeScaleAcknowledgementKind::kAppliedState ||
        runtime_scale.coordinated_barrier_id.has_value() ||
        runtime_scale.maximum_slew_per_second !=
            RoboPlanExecutionBackend::
                qualified_runtime_scale_slew_per_second()) {
      throw std::invalid_argument("native motion runtime-scale qualification "
                                  "differs from the selected mechanism");
    }
  }
  if (projection_revision.retained_bundle_id !=
          motion_value.retained_bundle_id ||
      projection_revision.graph_revision != motion_value.graph_revision) {
    throw std::invalid_argument(
        "native runtime motion binding crosses its collision projection");
  }
  require_planner_binding(input.motion_binding, *input.planner,
                          projection_revision);

  auto runtime = std::make_shared<RetainedNativeRuntime>(input.motion_binding,
                                                         input.gripper_binding);
  runtime->scene = std::move(input.scene);
  runtime->scene_mutex = std::move(input.scene_mutex);
  runtime->state_projector = std::move(input.state_projector);
  runtime->collision_publisher = std::move(input.collision_publisher);

  runtime->clock = shared_clock != nullptr ? std::move(shared_clock)
                                           : std::make_shared<WallClock>();
  runtime->scheduler =
      std::make_unique<SimpleScheduler>(std::move(config.scheduler));
  runtime->cache = std::make_unique<core::TrajectoryCache>(config.cache);
  runtime->planner = std::move(input.planner);

  runtime->motion_state_channel =
      std::make_shared<RetainedObservedStateChannel>(
          runtime->motion_binding, runtime->state_projector,
          native_state_config(runtime->motion_binding));
  const auto initial_timestamp_ns = runtime->clock->now();
  const auto motion_initialized =
      config.initial_motion_positions_rad.empty()
          ? runtime->motion_state_channel->initialize_from_projection(
                initial_timestamp_ns)
          : runtime->motion_state_channel->publish_and_project(
                config.initial_motion_positions_rad,
                std::vector<double>(config.initial_motion_positions_rad.size(),
                                    0.0),
                initial_timestamp_ns, initial_timestamp_ns);
  if (!motion_initialized.applied()) {
    throw std::invalid_argument(
        "native motion state could not be seeded from the retained model");
  }
  runtime->angular_observed_state =
      std::make_unique<core::AddressedAngularObservedRobotStateAdapter>(
          runtime->motion_state_channel->publication());
  runtime->kinematics = std::make_unique<RetainedRoboPlanKinematicsAdapter>(
      runtime->motion_binding, runtime->scene, runtime->scene_mutex);
  runtime->trajectory_validator = RoboPlanTrajectoryValidator::create(
      runtime->motion_binding, runtime->scene, runtime->scene_mutex);
  runtime->collision_trajectory_projector =
      RoboPlanCollisionTrajectoryProjector::create(
          runtime->motion_binding, runtime->scene, runtime->scene_mutex);
  runtime->execution = std::make_unique<RoboPlanExecutionBackend>(
      runtime->motion_binding, runtime->motion_state_channel, *runtime->clock,
      std::move(config.execution));

  if (runtime->gripper_binding.has_value()) {
    const auto &gripper_value = runtime->gripper_binding->value();
    if (gripper_value.retained_bundle_id != motion_value.retained_bundle_id ||
        gripper_value.graph_revision != motion_value.graph_revision ||
        gripper_value.controllable_component_id !=
            motion_value.controllable_component_id) {
      throw std::invalid_argument(
          "native aggregate gripper crosses its motion component generation");
    }
    if (gripper_value.resources.size() != 1U) {
      throw std::invalid_argument(
          "native gripper binding has no exact scalar resource");
    }
    const bool model_backed =
        gripper_value.resources.front().source_joint_name.has_value();
    runtime->gripper_state_channel =
        std::make_shared<RetainedObservedStateChannel>(
            *runtime->gripper_binding,
            model_backed ? runtime->state_projector : nullptr,
            native_state_config(*runtime->gripper_binding));
    if (model_backed) {
      const auto gripper_initialized =
          runtime->gripper_state_channel->initialize_from_projection(
              runtime->clock->now());
      if (!gripper_initialized.applied()) {
        throw std::invalid_argument(
            "native gripper state could not be seeded from the retained model");
      }
      runtime->gripper_position_limits =
          retained_gripper_limits(*runtime->scene, *runtime->gripper_binding);
    }
    runtime->gripper = std::make_unique<RetainedGripperCommandBackend>(
        *runtime->gripper_binding, runtime->gripper_state_channel,
        runtime->gripper_position_limits, *runtime->clock, *runtime->scheduler,
        config.gripper_update_period_ns);
  }

  std::vector<std::shared_ptr<RetainedObservedStateChannel>> state_channels{
      runtime->motion_state_channel};
  if (runtime->gripper_state_channel) {
    state_channels.push_back(runtime->gripper_state_channel);
  }
  runtime->reference_resolver =
      input.reference_resolver_factory(state_channels);
  if (!runtime->reference_resolver) {
    throw std::invalid_argument(
        "native runtime reference resolver factory returned no resolver");
  }

  const auto &shared_model = input.collision_shared_model;
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
       index <
       static_cast<std::size_t>(config.kernel.validation_collision_workers);
       ++index) {
    runtime->validation_collision_engines.push_back(
        collision_engine(shared_model, validation_cache));
  }
  const auto lookahead_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          shared_model);
  runtime->lookahead_collision_engines.reserve(static_cast<std::size_t>(
      config.kernel.runtime_collision_lookahead_workers));
  for (std::size_t index = 0U;
       index < static_cast<std::size_t>(
                   config.kernel.runtime_collision_lookahead_workers);
       ++index) {
    runtime->lookahead_collision_engines.push_back(
        collision_engine(shared_model, lookahead_cache));
  }

  core::ExecutionKernelPorts ports{*runtime->angular_observed_state,
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
  for (const auto &alias : input.motion_binding_aliases) {
    registrations.push_back({alias, runtime->participant_engine.get()});
  }
  if (runtime->gripper_binding.has_value()) {
    registrations.push_back(
        {*runtime->gripper_binding, runtime->participant_engine.get()});
  }
  for (const auto &alias : input.gripper_binding_aliases) {
    registrations.push_back({alias, runtime->participant_engine.get()});
  }
  core::RuntimeParticipantAdmissionResolver participant_resolver;
  if (input.participant_resolver) {
    auto binding_resolver = std::move(input.participant_resolver);
    auto *const participant_engine = runtime->participant_engine.get();
    participant_resolver =
        [binding_resolver = std::move(binding_resolver),
         participant_engine](const core::OperationIntentDTO &operation,
                             const std::size_t participant_index)
        -> core::Result<core::RuntimeParticipantExecutionRegistration> {
      auto binding = binding_resolver(operation, participant_index);
      if (!binding.ok()) {
        return core::Result<core::RuntimeParticipantExecutionRegistration>::Err(
            binding.error());
      }
      return core::Result<core::RuntimeParticipantExecutionRegistration>::Ok(
          core::RuntimeParticipantExecutionRegistration{
              std::move(binding.value()), participant_engine});
    };
  }
  runtime->participant_registrations = std::move(registrations);
  runtime->participant_resolver = std::move(participant_resolver);
  return runtime;
}

void attach_retained_native_addressed_owner(
    const std::shared_ptr<RetainedNativeRuntime> &runtime,
    RetainedNativeAddressedOwnerConfig config) {
  if (runtime == nullptr || runtime->clock == nullptr ||
      runtime->participant_registrations.empty() ||
      runtime->participant_engine == nullptr ||
      runtime->operation_journal != nullptr ||
      runtime->addressed_kernel != nullptr ||
      config.request_owner_epoch.empty() || config.cell_id.empty() ||
      config.operation_journal_path.empty() ||
      config.request_replay_capacity == 0U ||
      config.request_replay_capacity > core::kMaxI6OperationReplayEntries) {
    throw std::invalid_argument(
        "native addressed-owner attachment is incomplete or duplicated");
  }
  runtime->operation_journal = std::make_unique<core::SqliteOperationJournal>(
      std::move(config.operation_journal_path), std::move(config.cell_id));
  try {
    runtime->addressed_kernel =
        std::make_unique<core::ResourceAddressedExecutionKernel>(
            runtime->participant_registrations, *runtime->clock,
            *runtime->operation_journal,
            core::ResourceAddressedExecutionKernelConfig{
                std::move(config.request_owner_epoch),
                config.request_replay_capacity,
                std::move(runtime->participant_resolver),
                config.operation_admission_gate});
  } catch (...) {
    runtime->operation_journal.reset();
    throw;
  }
}

std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime(RetainedNativeRuntimeInput input,
                               RetainedNativeRuntimeConfig config) {
  RetainedNativeAddressedOwnerConfig owner_config{
      std::move(config.request_owner_epoch), std::move(config.cell_id),
      std::move(config.operation_journal_path), config.request_replay_capacity,
      input.operation_admission_gate};
  auto runtime = create_retained_native_runtime_participants(std::move(input),
                                                             std::move(config));
  attach_retained_native_addressed_owner(runtime, std::move(owner_config));
  return runtime;
}

} // namespace manyforge::roboplan_backend
