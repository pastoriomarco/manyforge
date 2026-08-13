#include <algorithm>
#include <cmath>
#include <map>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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
         left.provider_runtime_unit_id == right.provider_runtime_unit_id &&
         left.provider_runtime_epoch == right.provider_runtime_epoch;
}

[[nodiscard]] const core::JsonValueDTO *json_member(
    const core::JsonObjectDTO &object, const std::string_view name) noexcept {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] const core::JsonValueDTO::Object &json_object(
    const core::JsonValueDTO &value, const std::string_view field) {
  const auto *object = std::get_if<core::JsonValueDTO::Object>(&value.value);
  if (object == nullptr) {
    throw std::invalid_argument(std::string(field) + " is not an object");
  }
  return *object;
}

[[nodiscard]] const core::JsonValueDTO::Array &json_array(
    const core::JsonValueDTO &value, const std::string_view field) {
  const auto *array = std::get_if<core::JsonValueDTO::Array>(&value.value);
  if (array == nullptr) {
    throw std::invalid_argument(std::string(field) + " is not an array");
  }
  return *array;
}

[[nodiscard]] const std::string &json_string(
    const core::JsonValueDTO &value, const std::string_view field) {
  const auto *text = std::get_if<std::string>(&value.value);
  if (text == nullptr || text->empty()) {
    throw std::invalid_argument(std::string(field) + " is not a non-empty string");
  }
  return *text;
}

[[nodiscard]] double json_positive_number(const core::JsonValueDTO &value,
                                          const std::string_view field) {
  const auto *number = std::get_if<double>(&value.value);
  if (number == nullptr || !std::isfinite(*number) || *number <= 0.0) {
    throw std::invalid_argument(std::string(field) +
                                " is not a finite positive number");
  }
  return *number;
}

[[nodiscard]] std::vector<core::HardStopResourceLimitDTO>
retained_default_stop_limits(
    const core::RuntimeParticipantBinding &binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness) {
  if (!readiness.has_value()) {
    return {};
  }
  const auto &stages =
      readiness->value().resolved_stop_base_plan.escalation_stages;
  if (stages.empty() ||
      !stages.front().default_contract_parameters.has_value()) {
    return {};
  }
  const auto *limits_value = json_member(
      *stages.front().default_contract_parameters, "per_resource_limits");
  if (limits_value == nullptr) {
    return {};
  }
  const auto &limits =
      json_array(*limits_value, "Stop default per_resource_limits");
  if (limits.size() != binding.value().resources.size()) {
    throw std::invalid_argument(
        "Stop default resource limits do not cover the exact binding");
  }

  std::map<core::ResourceId, core::CoordinateKind> expected;
  for (const auto &resource : binding.value().resources) {
    if (!expected.emplace(resource.resource_id, resource.coordinate_kind)
             .second) {
      throw std::invalid_argument(
          "runtime binding repeats a Stop resource identity");
    }
  }
  std::vector<core::HardStopResourceLimitDTO> result;
  result.reserve(limits.size());
  std::set<core::ResourceId> actual;
  for (const auto &value : limits) {
    const auto &row = json_object(value, "Stop default resource limit");
    const auto *resource_value = json_member(row, "resource_id");
    const auto *threshold_value =
        json_member(row, "settle_velocity_threshold");
    if (resource_value == nullptr || threshold_value == nullptr) {
      throw std::invalid_argument(
          "Stop default resource limit is missing an exact field");
    }
    const core::ResourceId resource_id(
        json_string(*resource_value, "Stop default resource_id"));
    const auto expected_kind = expected.find(resource_id);
    if (expected_kind == expected.end() || !actual.insert(resource_id).second) {
      throw std::invalid_argument(
          "Stop default resource limits are duplicated or out of scope");
    }
    const auto &threshold =
        json_object(*threshold_value, "Stop default settle velocity");
    const auto *kind_value = json_member(threshold, "coordinate_kind");
    if (kind_value == nullptr) {
      throw std::invalid_argument(
          "Stop default settle velocity has no coordinate kind");
    }
    const auto &kind =
        json_string(*kind_value, "Stop default settle coordinate kind");
    if (expected_kind->second == core::CoordinateKind::kAngular) {
      const auto *number = json_member(threshold, "value_rad_s");
      if (kind != "angular_rad_s" || number == nullptr) {
        throw std::invalid_argument(
            "Stop default settle velocity differs from an angular resource");
      }
      result.push_back({resource_id, core::AngularVelocityDTO{
                                         json_positive_number(
                                             *number,
                                             "Stop default value_rad_s")}});
    } else {
      const auto *number = json_member(threshold, "value_m_s");
      if (kind != "linear_m_s" || number == nullptr) {
        throw std::invalid_argument(
            "Stop default settle velocity differs from a linear resource");
      }
      result.push_back({resource_id, core::LinearVelocityDTO{
                                         json_positive_number(
                                             *number,
                                             "Stop default value_m_s")}});
    }
  }
  std::sort(result.begin(), result.end(), [](const auto &left,
                                             const auto &right) {
    return left.resource_id < right.resource_id;
  });
  return result;
}

[[nodiscard]] core::RuntimeParticipantExecutionRegistration
runtime_registration(
    core::RuntimeParticipantBinding binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness,
    core::IResolvedParticipantExecutionEngine *engine,
    core::IControlDomainExecutionPort *stop_execution_port,
    const std::optional<core::RuntimeParticipantPolicySelection>
        &runtime_policy) {
  if (runtime_policy.has_value() &&
      (runtime_policy->value().controllable_component_id !=
           binding.value().controllable_component_id ||
       runtime_policy->value().command_target_id !=
           binding.value().command_target_id)) {
    throw std::invalid_argument(
        "runtime policy selection crosses its participant binding");
  }
  auto defaults = retained_default_stop_limits(binding, readiness);
  return core::RuntimeParticipantExecutionRegistration{
      std::move(binding), readiness, std::move(defaults),
      core::MotionInterruptNodeCategory::kGeneric, engine,
      stop_execution_port, runtime_policy,
      dynamic_cast<core::IResolvedPreparedParticipantExecutionEngine *>(
          engine)};
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

template <typename Value>
[[nodiscard]] core::Result<Value> route_publication_error(
    std::string reason, std::string message,
    core::ErrorCode code = core::ErrorCode::kInvariantViolation) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(code)), std::move(message),
      {{"reason", std::move(reason)}}});
}

[[nodiscard]] core::Result<core::RuntimeParticipantBinding>
rebase_program_alias(const core::RuntimeParticipantBinding &alias,
                     const core::RuntimeParticipantBinding &old_physical,
                     const core::RuntimeParticipantBinding &new_physical) {
  if (!same_physical_binding(old_physical, alias, true)) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_publication_alias_crosses_physical_owner",
        "runtime Program alias differs from its previous physical owner");
  }
  auto candidate = alias.value();
  candidate.control_path = new_physical.value().control_path;
  candidate.runtime_unit_epochs = new_physical.value().runtime_unit_epochs;
  auto sealed = core::verify_runtime_participant_binding(std::move(candidate));
  if (!sealed.ok()) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_publication_alias_rebind_invalid", sealed.error().message);
  }
  if (!same_physical_binding(new_physical, sealed.value(), true)) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_publication_alias_rebind_mismatch",
        "rebound Program alias differs from its new physical owner");
  }
  return sealed;
}

[[nodiscard]] core::RuntimeParticipantAdmissionResolver
make_kernel_participant_resolver(
    RetainedRuntimeParticipantResolver binding_resolver,
    std::vector<core::RuntimeParticipantExecutionRegistration>
        physical_registrations) {
  if (!binding_resolver) {
    return {};
  }
  return [binding_resolver = std::move(binding_resolver),
          physical_registrations = std::move(physical_registrations)](
             const core::OperationIntentDTO &operation,
             const std::size_t participant_index)
             -> core::Result<core::RuntimeParticipantExecutionRegistration> {
    auto binding = binding_resolver(operation, participant_index);
    if (!binding.ok()) {
      return core::Result<core::RuntimeParticipantExecutionRegistration>::Err(
          binding.error());
    }
    const core::RuntimeParticipantExecutionRegistration *physical = nullptr;
    for (const auto &candidate : physical_registrations) {
      if (!same_physical_binding(candidate.binding, binding.value(), true)) {
        continue;
      }
      if (physical != nullptr) {
        return route_publication_error<
            core::RuntimeParticipantExecutionRegistration>(
            "runtime_participant_physical_owner_ambiguous",
            "resolved Program participant matches multiple physical owners");
      }
      physical = &candidate;
    }
    if (physical == nullptr) {
      return route_publication_error<
          core::RuntimeParticipantExecutionRegistration>(
          "runtime_participant_stop_port_missing",
          "resolved Program participant has no exact composed physical owner",
          core::ErrorCode::kMissingReference);
    }
    try {
      return core::Result<core::RuntimeParticipantExecutionRegistration>::Ok(
          runtime_registration(std::move(binding.value()),
                               physical->stop_readiness, physical->engine,
                               physical->stop_execution_port,
                               physical->runtime_policy));
    } catch (const std::exception &exception) {
      return route_publication_error<
          core::RuntimeParticipantExecutionRegistration>(
          "runtime_participant_stop_readiness_invalid", exception.what());
    }
  };
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

  std::optional<core::RuntimeParticipantPolicySelection> motion_runtime_policy;
  if (input.runtime_policy.has_value()) {
    auto sealed = core::verify_runtime_participant_policy_selection(
        *input.runtime_policy, input.motion_binding);
    if (!sealed.ok()) {
      throw std::invalid_argument(
          "native runtime policy does not seal against the exact motion "
          "binding: " +
          sealed.error().message);
    }
    motion_runtime_policy = std::move(sealed.value());
  }

  auto runtime = std::make_shared<RetainedNativeRuntime>(input.motion_binding,
                                                         input.gripper_binding);
  runtime->motion_binding_aliases = std::move(input.motion_binding_aliases);
  runtime->gripper_binding_aliases = std::move(input.gripper_binding_aliases);
  runtime->participant_resolver_factory =
      std::move(input.participant_resolver_factory);
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
  registrations.push_back(runtime_registration(
      runtime->motion_binding, input.motion_stop_readiness,
      runtime->participant_engine.get(), runtime->execution.get(),
      motion_runtime_policy));
  for (const auto &alias : runtime->motion_binding_aliases) {
    registrations.push_back(runtime_registration(
        alias, input.motion_stop_readiness,
        runtime->participant_engine.get(), runtime->execution.get(),
        motion_runtime_policy));
  }
  if (runtime->gripper_binding.has_value()) {
    registrations.push_back(runtime_registration(
        *runtime->gripper_binding, input.gripper_stop_readiness,
        runtime->participant_engine.get(), nullptr, std::nullopt));
  }
  for (const auto &alias : runtime->gripper_binding_aliases) {
    registrations.push_back(runtime_registration(
        alias, input.gripper_stop_readiness,
        runtime->participant_engine.get(), nullptr, std::nullopt));
  }
  std::vector<core::RuntimeParticipantExecutionRegistration>
      physical_registrations;
  physical_registrations.push_back(runtime_registration(
      runtime->motion_binding, input.motion_stop_readiness,
      runtime->participant_engine.get(), runtime->execution.get(),
      motion_runtime_policy));
  if (runtime->gripper_binding.has_value()) {
    physical_registrations.push_back(runtime_registration(
        *runtime->gripper_binding, input.gripper_stop_readiness,
        runtime->participant_engine.get(), nullptr, std::nullopt));
  }
  auto binding_resolver = std::move(input.participant_resolver);
  if (runtime->participant_resolver_factory) {
    std::vector<core::RuntimeParticipantBinding> physical_bindings{
        runtime->motion_binding};
    if (runtime->gripper_binding.has_value()) {
      physical_bindings.push_back(*runtime->gripper_binding);
    }
    binding_resolver =
        runtime->participant_resolver_factory(std::move(physical_bindings));
  }
  auto participant_resolver = make_kernel_participant_resolver(
      std::move(binding_resolver), std::move(physical_registrations));
  runtime->cell_stop_deadline_budget = config.cell_stop_deadline_budget;
  runtime->participant_registrations = std::move(registrations);
  runtime->participant_resolver = std::move(participant_resolver);
  return runtime;
}

core::Result<std::vector<core::RuntimeParticipantBinding>>
RetainedNativeRuntime::ReadCurrentPhysicalParticipants(
    const core::ActiveControlConfigurationDTO &configuration) const {
  if (!configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled) {
    return route_publication_error<
        std::vector<core::RuntimeParticipantBinding>>(
        "route_publication_configuration_invalid",
        "native route publication requested an invalid configuration",
        core::ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(route_publication_mutex_);
  std::vector<core::RuntimeParticipantBinding> result;
  result.reserve(configuration.binding_routes.size());
  for (const auto &selected : configuration.binding_routes) {
    const core::RuntimeParticipantBinding *physical = nullptr;
    for (const auto *candidate : {&motion_binding,
                                  gripper_binding.has_value()
                                      ? &*gripper_binding
                                      : nullptr}) {
      if (candidate == nullptr ||
          !candidate->value().control_path.has_value()) {
        continue;
      }
      const auto &path = *candidate->value().control_path;
      if (path.control_domain_id != configuration.control_domain_id ||
          path.control_binding_id != selected.control_binding_id ||
          path.control_binding_revision !=
              selected.control_binding_revision) {
        continue;
      }
      if (physical != nullptr) {
        return route_publication_error<
            std::vector<core::RuntimeParticipantBinding>>(
            "route_publication_physical_binding_ambiguous",
            "native runtime has multiple physical owners for one binding");
      }
      physical = candidate;
    }
    if (physical == nullptr) {
      return route_publication_error<
          std::vector<core::RuntimeParticipantBinding>>(
          "route_publication_physical_binding_missing",
          "native runtime omits a selected physical binding",
          core::ErrorCode::kMissingReference);
    }
    result.push_back(*physical);
  }
  return core::Result<std::vector<core::RuntimeParticipantBinding>>::Ok(
      std::move(result));
}

core::Result<core::RuntimeRouteConfigurationReadbackDTO>
RetainedNativeRuntime::PublishAndReadBackRouteConfiguration(
    const core::ActiveControlConfigurationDTO &configuration,
    std::vector<deployment::VerifiedRouteReadinessParticipant> participants) {
  if (!configuration.locally_valid()) {
    return route_publication_error<
        core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_configuration_invalid",
        "native route publication has an invalid configuration",
        core::ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(route_publication_mutex_);
  if (addressed_kernel == nullptr || participant_engine == nullptr) {
    return route_publication_error<
        core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_kernel_missing",
        "native route publication has no stable addressed Kernel owner",
        core::ErrorCode::kMissingReference);
  }

  if (configuration.state == core::ActiveControlConfigurationStateDTO::
                                 kExternalOrUnknownUnreconciled) {
    if (!participants.empty()) {
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_unreconciled_participants_present",
          "unreconciled route publication contains readiness participants",
          core::ErrorCode::kInvalidRequest);
    }
    auto published =
        addressed_kernel->publish_runtime_route_configuration(configuration);
    if (!published.ok()) {
      return published;
    }
    return addressed_kernel->runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
  }
  if (configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      participants.size() != configuration.binding_routes.size()) {
    return route_publication_error<
        core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_participant_set_invalid",
        "reconciled route publication has an incomplete participant set",
        core::ErrorCode::kInvalidRequest);
  }

  try {
    auto next_motion = motion_binding;
    auto next_gripper = gripper_binding;
    std::optional<core::RuntimeParticipantStopReadiness> motion_readiness;
    std::optional<core::RuntimeParticipantStopReadiness> gripper_readiness;
    std::optional<core::RuntimeParticipantPolicySelection> motion_runtime_policy;
    const auto current_registration = [&](
                                          const core::RuntimeParticipantBinding
                                              &binding)
        -> const core::RuntimeParticipantExecutionRegistration * {
      const auto found = std::find_if(
          participant_registrations.begin(), participant_registrations.end(),
          [&](const auto &candidate) {
            return candidate.binding.value() == binding.value();
          });
      return found == participant_registrations.end() ? nullptr : &*found;
    };
    if (const auto *current = current_registration(motion_binding);
        current != nullptr) {
      motion_readiness = current->stop_readiness;
      motion_runtime_policy = current->runtime_policy;
    }
    if (gripper_binding.has_value()) {
      if (const auto *current = current_registration(*gripper_binding);
          current != nullptr) {
        gripper_readiness = current->stop_readiness;
      }
    }

    std::set<core::ControlBindingId> supplied_bindings;
    for (auto &participant : participants) {
      const auto &value = participant.participant_binding.value();
      if (!value.control_path.has_value() ||
          value.control_path->control_domain_id !=
              configuration.control_domain_id ||
          participant.readiness_row.control_binding_id !=
              value.control_path->control_binding_id ||
          participant.readiness_row.control_binding_revision !=
              value.control_path->control_binding_revision ||
          participant.readiness_row.controllable_component_id !=
              value.controllable_component_id ||
          participant.readiness_row.resolved_stop_base_plan_id !=
              participant.stop_readiness.value()
                  .resolved_stop_base_plan.resolved_stop_base_plan_id ||
          !supplied_bindings
               .insert(value.control_path->control_binding_id)
               .second) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_participant_referent_mismatch",
            "route readiness participant differs from its publication row");
      }
      const auto matches_motion =
          motion_binding.value().control_path.has_value() &&
          motion_binding.value().control_path->control_binding_id ==
              value.control_path->control_binding_id;
      const auto matches_gripper =
          gripper_binding.has_value() &&
          gripper_binding->value().control_path.has_value() &&
          gripper_binding->value().control_path->control_binding_id ==
              value.control_path->control_binding_id;
      if (matches_motion == matches_gripper) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_physical_binding_mismatch",
            "route readiness participant has no sole physical owner");
      }
      if (matches_motion) {
        next_motion = std::move(participant.participant_binding);
        motion_readiness = std::move(participant.stop_readiness);
      } else {
        next_gripper = std::move(participant.participant_binding);
        gripper_readiness = std::move(participant.stop_readiness);
      }
    }
    if (supplied_bindings.size() != configuration.binding_routes.size() ||
        std::any_of(configuration.binding_routes.begin(),
                    configuration.binding_routes.end(), [&](const auto &row) {
                      return !supplied_bindings.contains(
                          row.control_binding_id);
                    })) {
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_selected_binding_missing",
          "route readiness omits one selected physical binding",
          core::ErrorCode::kMissingReference);
    }

    std::vector<core::RuntimeParticipantBinding> next_motion_aliases;
    next_motion_aliases.reserve(motion_binding_aliases.size());
    for (const auto &alias : motion_binding_aliases) {
      auto rebound = rebase_program_alias(alias, motion_binding, next_motion);
      if (!rebound.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
      next_motion_aliases.push_back(std::move(rebound.value()));
    }
    std::vector<core::RuntimeParticipantBinding> next_gripper_aliases;
    next_gripper_aliases.reserve(gripper_binding_aliases.size());
    if (gripper_binding.has_value() && next_gripper.has_value()) {
      for (const auto &alias : gripper_binding_aliases) {
        auto rebound =
            rebase_program_alias(alias, *gripper_binding, *next_gripper);
        if (!rebound.ok()) {
          return core::Result<
              core::RuntimeRouteConfigurationReadbackDTO>::Err(
              rebound.error());
        }
        next_gripper_aliases.push_back(std::move(rebound.value()));
      }
    }

    std::vector<core::RuntimeParticipantExecutionRegistration>
        physical_registrations;
    physical_registrations.push_back(runtime_registration(
        next_motion, motion_readiness, participant_engine.get(),
        execution.get(), motion_runtime_policy));
    if (next_gripper.has_value()) {
      physical_registrations.push_back(runtime_registration(
          *next_gripper, gripper_readiness, participant_engine.get(), nullptr,
          std::nullopt));
    }
    std::vector<core::RuntimeParticipantExecutionRegistration> registrations =
        physical_registrations;
    for (const auto &alias : next_motion_aliases) {
      registrations.push_back(runtime_registration(
          alias, motion_readiness, participant_engine.get(), execution.get(),
          motion_runtime_policy));
    }
    for (const auto &alias : next_gripper_aliases) {
      registrations.push_back(runtime_registration(
          alias, gripper_readiness, participant_engine.get(), nullptr,
          std::nullopt));
    }

    RetainedRuntimeParticipantResolver binding_resolver;
    if (participant_resolver_factory) {
      std::vector<core::RuntimeParticipantBinding> physical_bindings{
          next_motion};
      if (next_gripper.has_value()) {
        physical_bindings.push_back(*next_gripper);
      }
      binding_resolver =
          participant_resolver_factory(std::move(physical_bindings));
      if (!binding_resolver) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_resolver_factory_failed",
            "native participant resolver factory returned no resolver");
      }
    } else if (participant_resolver) {
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_resolver_factory_missing",
          "dynamic native participant resolution cannot retain stale physical facts");
    }
    auto kernel_resolver = make_kernel_participant_resolver(
        std::move(binding_resolver), physical_registrations);
    auto published = addressed_kernel->publish_runtime_route_configuration(
        configuration, registrations, kernel_resolver);
    if (!published.ok()) {
      return published;
    }
    auto readback = addressed_kernel->runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
    if (!readback.ok() || readback.value() != published.value()) {
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_independent_readback_mismatch",
          "native Kernel readback differs from its route publication");
    }

    motion_binding = std::move(next_motion);
    gripper_binding = std::move(next_gripper);
    motion_binding_aliases = std::move(next_motion_aliases);
    gripper_binding_aliases = std::move(next_gripper_aliases);
    participant_registrations = std::move(registrations);
    participant_resolver = std::move(kernel_resolver);
    if (motion_readiness.has_value()) {
      cell_stop_deadline_budget =
          motion_readiness->value().resolved_stop_base_plan.deadline_budget;
    } else if (gripper_readiness.has_value()) {
      cell_stop_deadline_budget =
          gripper_readiness->value().resolved_stop_base_plan.deadline_budget;
    }
    return readback;
  } catch (const std::exception &exception) {
    return route_publication_error<
        core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_native_composition_failed", exception.what());
  } catch (...) {
    return route_publication_error<
        core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_native_composition_failed",
        "native route publication failed");
  }
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
      config.graph_revision.empty() ||
      config.request_replay_capacity == 0U ||
      config.request_replay_capacity > core::kMaxI6OperationReplayEntries) {
    throw std::invalid_argument(
        "native addressed-owner attachment is incomplete or duplicated");
  }
  const core::CellId kernel_cell_id = config.cell_id;
  runtime->operation_journal = std::make_unique<core::SqliteOperationJournal>(
      std::move(config.operation_journal_path), std::move(config.cell_id));
  try {
    runtime->addressed_kernel =
        std::make_unique<core::ResourceAddressedExecutionKernel>(
            runtime->participant_registrations, *runtime->clock,
            *runtime->operation_journal,
            core::ResourceAddressedExecutionKernelConfig{
                std::move(config.request_owner_epoch),
                kernel_cell_id,
                config.request_replay_capacity,
                std::move(runtime->participant_resolver),
                config.operation_admission_gate,
                std::move(config.graph_revision),
                std::move(config.cell_stop_deadline_budget)});
  } catch (...) {
    runtime->operation_journal.reset();
    throw;
  }
}

std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime(RetainedNativeRuntimeInput input,
                               RetainedNativeRuntimeConfig config) {
  auto stop_budget = config.cell_stop_deadline_budget;
  if (!stop_budget.has_value() && input.motion_stop_readiness.has_value()) {
    stop_budget = input.motion_stop_readiness->value()
                      .resolved_stop_base_plan.deadline_budget;
  }
  config.cell_stop_deadline_budget = stop_budget;
  RetainedNativeAddressedOwnerConfig owner_config{
      std::move(config.request_owner_epoch), std::move(config.cell_id),
      std::move(config.operation_journal_path), config.request_replay_capacity,
      input.operation_admission_gate,
      input.motion_binding.value().graph_revision, std::move(stop_budget)};
  auto runtime = create_retained_native_runtime_participants(std::move(input),
                                                             std::move(config));
  attach_retained_native_addressed_owner(runtime, std::move(owner_config));
  return runtime;
}

} // namespace manyforge::roboplan_backend
