#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include "retained_native_runtime_internal.hpp"
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

using manyforge::content_identity::CanonicalValue;

inline constexpr std::string_view kNoActiveGoalEvidenceDomain =
    "native-no-active-goal-evidence-digest";
inline constexpr std::string_view kNoActiveGoalEvidenceApiVersion =
    "manyforge.native-no-active-goal-evidence.v1";

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

[[nodiscard]] const core::JsonValueDTO *
json_member(const core::JsonObjectDTO &object,
            const std::string_view name) noexcept {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] const core::JsonValueDTO::Object &
json_object(const core::JsonValueDTO &value, const std::string_view field) {
  const auto *object = std::get_if<core::JsonValueDTO::Object>(&value.value);
  if (object == nullptr) {
    throw std::invalid_argument(std::string(field) + " is not an object");
  }
  return *object;
}

[[nodiscard]] const core::JsonValueDTO::Array &
json_array(const core::JsonValueDTO &value, const std::string_view field) {
  const auto *array = std::get_if<core::JsonValueDTO::Array>(&value.value);
  if (array == nullptr) {
    throw std::invalid_argument(std::string(field) + " is not an array");
  }
  return *array;
}

[[nodiscard]] const std::string &json_string(const core::JsonValueDTO &value,
                                             const std::string_view field) {
  const auto *text = std::get_if<std::string>(&value.value);
  if (text == nullptr || text->empty()) {
    throw std::invalid_argument(std::string(field) +
                                " is not a non-empty string");
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
    const auto *threshold_value = json_member(row, "settle_velocity_threshold");
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
      result.push_back(
          {resource_id, core::AngularVelocityDTO{json_positive_number(
                            *number, "Stop default value_rad_s")}});
    } else {
      const auto *number = json_member(threshold, "value_m_s");
      if (kind != "linear_m_s" || number == nullptr) {
        throw std::invalid_argument(
            "Stop default settle velocity differs from a linear resource");
      }
      result.push_back(
          {resource_id, core::LinearVelocityDTO{json_positive_number(
                            *number, "Stop default value_m_s")}});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const auto &left, const auto &right) {
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
      std::move(binding),
      readiness,
      std::move(defaults),
      core::MotionInterruptNodeCategory::kGeneric,
      engine,
      stop_execution_port,
      runtime_policy,
      dynamic_cast<core::IResolvedPreparedParticipantExecutionEngine *>(
          engine)};
}

[[nodiscard]] std::vector<core::ResourceId>
stop_observation_resources(const core::RuntimeParticipantBinding &binding,
                           const core::ResolvedStopEscalationStageDTO &stage) {
  if (const auto *resource_scope =
          std::get_if<core::ResolvedStopResourceContainmentScopeDTO>(
              &stage.containment_scope)) {
    return resource_scope->resource_ids;
  }
  std::vector<core::ResourceId> resources;
  resources.reserve(binding.value().resources.size());
  for (const auto &resource : binding.value().resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  return resources;
}

[[nodiscard]] RetainedNoActiveGoalEvidenceSource
motion_goal_observer(core::IResolvedParticipantExecutionEngine *engine,
                     core::RuntimeParticipantBinding binding);

[[nodiscard]] RetainedNoActiveGoalEvidenceSource
gripper_goal_observer(core::IResolvedParticipantExecutionEngine *engine,
                      core::RuntimeParticipantBinding binding);

[[nodiscard]] std::optional<RetainedStopSurveillanceSourceInput>
stop_surveillance_source(
    const core::RuntimeParticipantBinding &binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness,
    const std::shared_ptr<RetainedObservedStateChannel> &channel,
    RetainedNoActiveGoalEvidenceSource no_active_goal_evidence_source) {
  if (channel == nullptr || !no_active_goal_evidence_source) {
    return std::nullopt;
  }
  RetainedStopSurveillanceSourceInput result{
      binding, channel, {}, std::move(no_active_goal_evidence_source)};
  if (readiness.has_value()) {
    const auto &plan = readiness->value().resolved_stop_base_plan;
    for (const auto &stage : plan.escalation_stages) {
      const auto resources = stop_observation_resources(binding, stage);
      for (const auto &requirement : stage.evidence_requirements) {
        if (requirement.requirement.evidence_kind !=
                core::StopEvidenceRequirementKind::kMotionRest ||
            requirement.resolved_source.source_kind !=
                core::StopEvidenceSourceKind::kBindingStateAuthority) {
          continue;
        }
        result.observation_sources.push_back(
            RetainedStopSurveillanceObservationSource{
                requirement.requirement, requirement.resolved_source,
                resources});
      }
    }
  }
  return result;
}

[[nodiscard]] std::vector<RetainedStopSurveillanceSourceInput>
stop_surveillance_sources(
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantStopReadiness>
        &motion_readiness,
    const std::shared_ptr<RetainedObservedStateChannel> &motion_channel,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
    const std::optional<core::RuntimeParticipantStopReadiness>
        &gripper_readiness,
    const std::shared_ptr<RetainedObservedStateChannel> &gripper_channel,
    core::IResolvedParticipantExecutionEngine *engine) {
  std::vector<RetainedStopSurveillanceSourceInput> result;
  if (auto motion = stop_surveillance_source(
          motion_binding, motion_readiness, motion_channel,
          motion_goal_observer(engine, motion_binding));
      motion.has_value()) {
    result.push_back(std::move(*motion));
  }
  if (gripper_binding.has_value()) {
    if (auto gripper = stop_surveillance_source(
            *gripper_binding, gripper_readiness, gripper_channel,
            gripper_goal_observer(engine, *gripper_binding));
        gripper.has_value()) {
      result.push_back(std::move(*gripper));
    }
  }
  return result;
}

[[nodiscard]] std::optional<core::RuntimeParticipantStopReadiness>
readiness_for(const std::vector<core::RuntimeParticipantExecutionRegistration>
                  &registrations,
              const core::RuntimeParticipantBinding &binding) {
  const auto found = std::find_if(
      registrations.begin(), registrations.end(),
      [&](const auto &row) { return row.binding.value() == binding.value(); });
  return found == registrations.end() ? std::nullopt : found->stop_readiness;
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
  return core::Result<Value>::Err(
      core::ErrorDTO{std::string(core::to_string(code)),
                     std::move(message),
                     {{"reason", std::move(reason)}}});
}

[[nodiscard]] core::ContentDigest no_active_goal_evidence_digest(
    const core::RuntimeParticipantBinding &binding,
    const std::string_view target_kind, const std::string_view state,
    const std::uint64_t goal_id, const std::uint64_t goal_generation,
    const std::string_view correlation, const std::int64_t observed_at_ns) {
  const auto &value = binding.value();
  const auto &path = *value.control_path;
  const CanonicalValue evidence(CanonicalValue::Object{
      {"command_target_id", CanonicalValue(value.command_target_id.value())},
      {"controllable_component_id",
       CanonicalValue(value.controllable_component_id.value())},
      {"correlation", CanonicalValue(std::string(correlation))},
      {"goal_generation", CanonicalValue(std::to_string(goal_generation))},
      {"goal_id", CanonicalValue(std::to_string(goal_id))},
      {"observed_at_ns", CanonicalValue(std::to_string(observed_at_ns))},
      {"provider_runtime_epoch",
       CanonicalValue(path.provider_runtime_epoch.value())},
      {"provider_runtime_unit_id",
       CanonicalValue(path.provider_runtime_unit_id.value())},
      {"state", CanonicalValue(std::string(state))},
      {"target_kind", CanonicalValue(std::string(target_kind))},
  });
  return core::ContentDigest(content_identity::framed_sha256(
      kNoActiveGoalEvidenceDomain, kNoActiveGoalEvidenceApiVersion, evidence));
}

[[nodiscard]] RetainedNoActiveGoalEvidenceSource
motion_goal_observer(core::IResolvedParticipantExecutionEngine *engine,
                     core::RuntimeParticipantBinding binding) {
  if (engine == nullptr || !binding.value().control_path.has_value())
    return {};
  return [engine,
          binding = std::move(binding)](const std::int64_t observed_at_ns)
             -> core::Result<core::ContentDigest> {
    const auto status = engine->get_status();
    const bool no_active_goal =
        status.state == core::ExecutionState::kIdle ||
        status.state == core::ExecutionState::kStopped ||
        status.state == core::ExecutionState::kCompleted ||
        status.state == core::ExecutionState::kFailed ||
        status.state == core::ExecutionState::kInterventionRequired;
    if (!no_active_goal) {
      return route_publication_error<core::ContentDigest>(
          "native_motion_goal_still_active",
          "native motion target still has a nonterminal goal",
          core::ErrorCode::kInvalidStateTransition);
    }
    return core::Result<core::ContentDigest>::Ok(no_active_goal_evidence_digest(
        binding, "motion", core::to_string(status.state), status.execution_id,
        0U, status.active_move_id, observed_at_ns));
  };
}

[[nodiscard]] RetainedNoActiveGoalEvidenceSource
gripper_goal_observer(core::IResolvedParticipantExecutionEngine *engine,
                      core::RuntimeParticipantBinding binding) {
  if (engine == nullptr || !binding.value().control_path.has_value())
    return {};
  return [engine,
          binding = std::move(binding)](const std::int64_t observed_at_ns)
             -> core::Result<core::ContentDigest> {
    const auto target = binding.value().command_target_id.value();
    const auto status = engine->get_gripper_status(target);
    const bool no_active_goal =
        status.gripper_id == target &&
        (status.state == core::GripperCommandState::kIdle ||
         status.state == core::GripperCommandState::kCompleted ||
         status.state == core::GripperCommandState::kFailed);
    if (!no_active_goal) {
      return route_publication_error<core::ContentDigest>(
          "native_gripper_goal_still_active",
          "native gripper target still has a nonterminal goal",
          core::ErrorCode::kInvalidStateTransition);
    }
    return core::Result<core::ContentDigest>::Ok(no_active_goal_evidence_digest(
        binding, "gripper", core::gripper_command_state_name(status.state),
        status.command_id, status.generation, status.gripper_id,
        observed_at_ns));
  };
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

[[nodiscard]] core::Result<core::RuntimeParticipantBinding>
rebase_runtime_binding_to_predecessor(
    const core::RuntimeParticipantBinding &binding,
    const core::ActiveControlConfigurationDTO &configuration) {
  const auto &value = binding.value();
  if (!configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      !value.control_path.has_value()) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_predecessor_rebind_input_invalid",
        "durable route predecessor rebind has no exact control path",
        core::ErrorCode::kInvalidRequest);
  }
  const auto &path = *value.control_path;
  const auto selected =
      std::find_if(configuration.binding_routes.begin(),
                   configuration.binding_routes.end(), [&](const auto &row) {
                     return row.control_binding_id == path.control_binding_id;
                   });
  if (path.control_domain_id != configuration.control_domain_id ||
      path.control_domain_revision != configuration.control_domain_revision ||
      path.controller_topology_id != configuration.controller_topology_id ||
      path.topology_revision != configuration.topology_revision ||
      path.provider_runtime_unit_id != configuration.source_runtime_unit_id ||
      selected == configuration.binding_routes.end() ||
      selected->control_binding_revision != path.control_binding_revision) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_predecessor_rebind_mismatch",
        "durable route predecessor crosses its retained runtime binding",
        core::ErrorCode::kInvalidRequest);
  }
  auto candidate = value;
  candidate.control_path->control_route_id = selected->control_route_id;
  candidate.control_path->activation_set_id = configuration.activation_set_id;
  candidate.control_path->active_control_configuration_id =
      configuration.active_control_configuration_id;
  candidate.control_path->active_control_configuration_generation =
      configuration.active_control_configuration_generation;
  candidate.control_path->runtime_scale_qualification.reset();
  auto sealed = core::verify_runtime_participant_binding(std::move(candidate));
  if (!sealed.ok()) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_predecessor_rebind_invalid", sealed.error().message,
        core::ErrorCode::kInvalidRequest);
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

namespace detail {

RetainedObservedStateChannelConfig select_retained_state_channel_config(
    std::optional<RetainedObservedStateChannelConfig> selected,
    const core::RuntimeParticipantBinding &binding) {
  if (selected.has_value()) {
    return std::move(*selected);
  }
  return native_state_config(binding);
}

} // namespace detail

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

  auto motion_state_config = detail::select_retained_state_channel_config(
      std::move(input.motion_state_channel_config), runtime->motion_binding);
  runtime->motion_state_channel =
      std::make_shared<RetainedObservedStateChannel>(
          runtime->motion_binding, runtime->state_projector,
          std::move(motion_state_config));
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
            detail::select_retained_state_channel_config(
                std::move(input.gripper_state_channel_config),
                *runtime->gripper_binding));
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
  }

  if (input.backend_factory) {
    auto selected = input.backend_factory(
        runtime->motion_binding, runtime->gripper_binding,
        runtime->motion_state_channel, runtime->gripper_state_channel,
        *runtime->clock, *runtime->scheduler);
    runtime->execution = std::move(selected.execution);
    runtime->gripper = std::move(selected.gripper);
    runtime->gripper_stop_execution_port =
        selected.gripper_stop_execution_port;
    runtime->rebind_motion_route =
        std::move(selected.rebind_motion_route);
    runtime->rebind_gripper_route =
        std::move(selected.rebind_gripper_route);
  } else {
    auto native_execution = std::make_shared<RoboPlanExecutionBackend>(
        runtime->motion_binding, runtime->motion_state_channel, *runtime->clock,
        *runtime->scheduler, std::move(config.execution));
    runtime->execution = native_execution;
    runtime->rebind_motion_route =
        [native_execution](core::RuntimeParticipantBinding binding,
                           std::vector<core::RuntimeParticipantBinding>
                               program_aliases) {
          return native_execution->RebindRoute(std::move(binding),
                                               std::move(program_aliases));
        };
    if (runtime->gripper_binding.has_value()) {
      auto native_gripper =
          std::make_shared<RetainedGripperCommandBackend>(
              *runtime->gripper_binding, runtime->gripper_state_channel,
              runtime->gripper_position_limits, *runtime->clock,
              *runtime->scheduler, config.gripper_update_period_ns);
      runtime->gripper = native_gripper;
      runtime->gripper_stop_execution_port = native_gripper.get();
      runtime->rebind_gripper_route =
          [native_gripper](
              core::RuntimeParticipantBinding binding,
              std::shared_ptr<IRetainedObservedStateChannel> state_channel) {
            return native_gripper->RebindRoute(std::move(binding),
                                               std::move(state_channel));
          };
    }
  }
  if (!runtime->execution || !runtime->rebind_motion_route ||
      runtime->execution->observed_robot_state_publication() !=
          &runtime->motion_state_channel->publication().numeric_publication() ||
      runtime->gripper_binding.has_value() !=
          static_cast<bool>(runtime->gripper) ||
      (runtime->gripper_binding.has_value() &&
       !runtime->rebind_gripper_route)) {
    throw std::invalid_argument(
        "retained runtime execution adapter set is incomplete or owns a "
        "second state authority");
  }
  if (runtime->gripper_binding.has_value() &&
      runtime->gripper_stop_execution_port == nullptr) {
    throw std::invalid_argument(
        "retained runtime gripper adapter has no Stop execution port");
  }
  if (runtime->gripper_binding.has_value()) {
    const auto *execution_stop =
        static_cast<core::IControlDomainExecutionPort *>(
            runtime->execution.get());
    const auto *gripper_owned_stop =
        dynamic_cast<core::IControlDomainExecutionPort *>(
            runtime->gripper.get());
    if (runtime->gripper_stop_execution_port != execution_stop &&
        runtime->gripper_stop_execution_port != gripper_owned_stop) {
      throw std::invalid_argument(
          "retained runtime gripper Stop port is not owned by its adapter "
          "set");
    }
  }
  if (const auto rebound = runtime->rebind_motion_route(
          runtime->motion_binding, runtime->motion_binding_aliases);
      !rebound.ok()) {
    throw std::invalid_argument(rebound.error().message);
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
                                   runtime->gripper.get(),
                                   input.planning_scene_synchronizer.get()};
  runtime->planning_scene_synchronizer =
      std::move(input.planning_scene_synchronizer);
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
      runtime_registration(runtime->motion_binding, input.motion_stop_readiness,
                           runtime->participant_engine.get(),
                           runtime->execution.get(), motion_runtime_policy));
  for (const auto &alias : runtime->motion_binding_aliases) {
    registrations.push_back(runtime_registration(
        alias, input.motion_stop_readiness, runtime->participant_engine.get(),
        runtime->execution.get(), motion_runtime_policy));
  }
  if (runtime->gripper_binding.has_value()) {
    registrations.push_back(runtime_registration(
        *runtime->gripper_binding, input.gripper_stop_readiness,
        runtime->participant_engine.get(),
        runtime->gripper_stop_execution_port,
        std::nullopt));
  }
  for (const auto &alias : runtime->gripper_binding_aliases) {
    registrations.push_back(runtime_registration(
        alias, input.gripper_stop_readiness, runtime->participant_engine.get(),
        runtime->gripper_stop_execution_port, std::nullopt));
  }
  std::vector<core::RuntimeParticipantExecutionRegistration>
      physical_registrations;
  physical_registrations.push_back(
      runtime_registration(runtime->motion_binding, input.motion_stop_readiness,
                           runtime->participant_engine.get(),
                           runtime->execution.get(), motion_runtime_policy));
  if (runtime->gripper_binding.has_value()) {
    physical_registrations.push_back(runtime_registration(
        *runtime->gripper_binding, input.gripper_stop_readiness,
        runtime->participant_engine.get(),
        runtime->gripper_stop_execution_port,
        std::nullopt));
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
  runtime->stop_surveillance_policy = config.stop_surveillance_policy;
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
    for (const auto *candidate :
         {&motion_binding,
          gripper_binding.has_value() ? &*gripper_binding : nullptr}) {
      if (candidate == nullptr ||
          !candidate->value().control_path.has_value()) {
        continue;
      }
      const auto &path = *candidate->value().control_path;
      if (path.control_domain_id != configuration.control_domain_id ||
          path.control_binding_id != selected.control_binding_id ||
          path.control_binding_revision != selected.control_binding_revision) {
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

core::Result<std::optional<core::ActiveControlConfigurationDTO>>
RetainedNativeRuntime::ReadCurrentPublishedRouteConfiguration(
    const core::ControlDomainId &control_domain_id) const {
  if (addressed_kernel == nullptr) {
    return route_publication_error<
        std::optional<core::ActiveControlConfigurationDTO>>(
        "route_publication_kernel_missing",
        "native route current-fact read has no stable Kernel owner",
        core::ErrorCode::kMissingReference);
  }
  const auto readback =
      addressed_kernel->current_runtime_route_configuration(control_domain_id);
  if (!readback.ok()) {
    return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::
        Err(readback.error());
  }
  if (!readback.value().has_value()) {
    return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
        std::nullopt);
  }
  return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
      readback.value()->active_configuration);
}

core::Result<core::RuntimeRouteConfigurationReadbackDTO>
RetainedNativeRuntime::PublishAndReadBackRouteConfiguration(
    const core::ActiveControlConfigurationDTO &configuration,
    std::vector<deployment::VerifiedRouteReadinessParticipant> participants) {
  if (!configuration.locally_valid()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_configuration_invalid",
        "native route publication has an invalid configuration",
        core::ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(route_publication_mutex_);
  if (addressed_kernel == nullptr || participant_engine == nullptr) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
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
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_participant_set_invalid",
        "reconciled route publication has an incomplete participant set",
        core::ErrorCode::kInvalidRequest);
  }

  try {
    auto next_motion = motion_binding;
    auto next_gripper = gripper_binding;
    std::optional<core::RuntimeParticipantStopReadiness> motion_readiness;
    std::optional<core::RuntimeParticipantStopReadiness> gripper_readiness;
    std::optional<core::RuntimeParticipantPolicySelection>
        motion_runtime_policy;
    const auto current_registration =
        [&](const core::RuntimeParticipantBinding &binding)
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
          !supplied_bindings.insert(value.control_path->control_binding_id)
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
          return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
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
          *next_gripper, gripper_readiness, participant_engine.get(),
          gripper_stop_execution_port, std::nullopt));
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
          alias, gripper_readiness, participant_engine.get(),
          gripper_stop_execution_port, std::nullopt));
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
          "dynamic native participant resolution cannot retain stale physical "
          "facts");
    }
    auto kernel_resolver = make_kernel_participant_resolver(
        std::move(binding_resolver), physical_registrations);

    const auto transition_time_ns = clock->now();
    const auto old_motion = motion_binding;
    const auto old_gripper = gripper_binding;
    bool motion_rebound = false;
    bool gripper_rebound = false;
    const auto rollback_physical_route = [&]() noexcept {
      try {
        if (gripper_rebound && old_gripper.has_value() && gripper != nullptr) {
          const auto now_ns = clock->now();
          if (gripper_state_channel->RebindRoute(*old_gripper, now_ns).ok()) {
            static_cast<void>(rebind_gripper_route(*old_gripper,
                                                   gripper_state_channel));
          }
        }
        if (motion_rebound && execution != nullptr) {
          const auto now_ns = clock->now();
          if (motion_state_channel->RebindRoute(old_motion, now_ns).ok()) {
            static_cast<void>(rebind_motion_route(old_motion,
                                                  motion_binding_aliases));
          }
        }
      } catch (...) {
      }
    };
    if (next_motion.value() != motion_binding.value()) {
      auto rebound =
          motion_state_channel->RebindRoute(next_motion, transition_time_ns);
      if (!rebound.ok()) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_motion_state_rebind_failed",
            rebound.error().message);
      }
      motion_rebound = true;
      rebound = rebind_motion_route(next_motion, next_motion_aliases);
      if (!rebound.ok()) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_motion_backend_rebind_failed",
            rebound.error().message);
      }
    }
    const bool gripper_binding_changed =
        next_gripper.has_value() != gripper_binding.has_value() ||
        (next_gripper.has_value() && gripper_binding.has_value() &&
         next_gripper->value() != gripper_binding->value());
    if (gripper_binding_changed) {
      if (!next_gripper.has_value() || !gripper_binding.has_value() ||
          gripper_state_channel == nullptr || gripper == nullptr) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_gripper_owner_missing",
            "native route publication cannot rebind a missing gripper owner");
      }
      auto rebound =
          gripper_state_channel->RebindRoute(*next_gripper, transition_time_ns);
      if (!rebound.ok()) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_gripper_state_rebind_failed",
            rebound.error().message);
      }
      gripper_rebound = true;
      rebound =
          rebind_gripper_route(*next_gripper, gripper_state_channel);
      if (!rebound.ok()) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_gripper_backend_rebind_failed",
            rebound.error().message);
      }
    }
    auto next_surveillance_sources = stop_surveillance_sources(
        next_motion, motion_readiness, motion_state_channel, next_gripper,
        gripper_readiness, gripper_state_channel, participant_engine.get());
    if (stop_surveillance_owner != nullptr) {
      auto valid = RetainedStopSurveillanceOwner::ValidateSources(
          next_surveillance_sources);
      if (!valid.ok()) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_surveillance_source_invalid",
            valid.error().message);
      }
    }
    auto published = addressed_kernel->publish_runtime_route_configuration(
        configuration, registrations, kernel_resolver);
    if (!published.ok()) {
      rollback_physical_route();
      return published;
    }
    auto readback = addressed_kernel->runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
    if (!readback.ok() || readback.value() != published.value()) {
      rollback_physical_route();
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_independent_readback_mismatch",
          "native Kernel readback differs from its route publication");
    }

    if (stop_surveillance_owner != nullptr) {
      auto replaced = stop_surveillance_owner->ReplaceSources(
          std::move(next_surveillance_sources));
      if (!replaced.ok()) {
        throw std::logic_error(
            "prevalidated native surveillance source replacement failed");
      }
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
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_native_composition_failed", exception.what());
  } catch (...) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_native_composition_failed",
        "native route publication failed");
  }
}

core::Result<core::RuntimeRouteConfigurationReadbackDTO>
RetainedNativeRuntime::AdoptAndReadBackRouteConfigurationPredecessor(
    const core::ActiveControlConfigurationDTO &configuration) {
  if (!configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_configuration_invalid",
        "native restart predecessor is not one reconciled configuration",
        core::ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(route_publication_mutex_);
  if (addressed_kernel == nullptr || participant_engine == nullptr ||
      execution == nullptr || motion_state_channel == nullptr ||
      clock == nullptr) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_runtime_incomplete",
        "native restart predecessor has no stable runtime owner",
        core::ErrorCode::kMissingReference);
  }

  try {
    auto next_motion = motion_binding;
    auto next_gripper = gripper_binding;
    const auto rebase_if_selected =
        [&](const core::RuntimeParticipantBinding &binding)
        -> core::Result<core::RuntimeParticipantBinding> {
      if (!binding.value().control_path.has_value() ||
          binding.value().control_path->control_domain_id !=
              configuration.control_domain_id) {
        return core::Result<core::RuntimeParticipantBinding>::Ok(binding);
      }
      return rebase_runtime_binding_to_predecessor(binding, configuration);
    };
    auto motion_rebase = rebase_if_selected(motion_binding);
    if (!motion_rebase.ok()) {
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          motion_rebase.error());
    }
    next_motion = std::move(motion_rebase.value());
    if (gripper_binding.has_value()) {
      auto gripper_rebase = rebase_if_selected(*gripper_binding);
      if (!gripper_rebase.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            gripper_rebase.error());
      }
      next_gripper = std::move(gripper_rebase.value());
    }

    const auto current_registration =
        [&](const core::RuntimeParticipantBinding &binding)
        -> const core::RuntimeParticipantExecutionRegistration * {
      const auto found = std::find_if(
          participant_registrations.begin(), participant_registrations.end(),
          [&](const auto &candidate) {
            return candidate.binding.value() == binding.value();
          });
      return found == participant_registrations.end() ? nullptr : &*found;
    };
    std::optional<core::RuntimeParticipantStopReadiness> motion_readiness;
    std::optional<core::RuntimeParticipantStopReadiness> gripper_readiness;
    std::optional<core::RuntimeParticipantPolicySelection>
        motion_runtime_policy;
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
    if (next_motion.value().control_path.has_value() &&
        next_motion.value().control_path->control_domain_id ==
            configuration.control_domain_id) {
      motion_readiness.reset();
    }
    if (next_gripper.has_value() &&
        next_gripper->value().control_path.has_value() &&
        next_gripper->value().control_path->control_domain_id ==
            configuration.control_domain_id) {
      gripper_readiness.reset();
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
          return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
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
          *next_gripper, gripper_readiness, participant_engine.get(),
          gripper_stop_execution_port, std::nullopt));
    }
    auto registrations = physical_registrations;
    for (const auto &alias : next_motion_aliases) {
      registrations.push_back(runtime_registration(
          alias, motion_readiness, participant_engine.get(), execution.get(),
          motion_runtime_policy));
    }
    for (const auto &alias : next_gripper_aliases) {
      registrations.push_back(runtime_registration(
          alias, gripper_readiness, participant_engine.get(),
          gripper_stop_execution_port, std::nullopt));
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
            "route_predecessor_resolver_factory_failed",
            "native predecessor resolver factory returned no resolver");
      }
    } else if (participant_resolver) {
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_predecessor_resolver_factory_missing",
          "native predecessor adoption cannot retain a stale resolver");
    }
    auto kernel_resolver = make_kernel_participant_resolver(
        std::move(binding_resolver), physical_registrations);

    const auto transition_time_ns = clock->now();
    const auto old_motion = motion_binding;
    const auto old_gripper = gripper_binding;
    bool motion_rebound = false;
    bool gripper_rebound = false;
    const auto rollback = [&]() noexcept {
      try {
        if (gripper_rebound && old_gripper.has_value() && gripper != nullptr &&
            gripper_state_channel != nullptr) {
          const auto now_ns = clock->now();
          if (gripper_state_channel->RebindRoute(*old_gripper, now_ns).ok()) {
            static_cast<void>(rebind_gripper_route(*old_gripper,
                                                   gripper_state_channel));
          }
        }
        if (motion_rebound) {
          const auto now_ns = clock->now();
          if (motion_state_channel->RebindRoute(old_motion, now_ns).ok()) {
            static_cast<void>(rebind_motion_route(old_motion,
                                                  motion_binding_aliases));
          }
        }
      } catch (...) {
      }
    };
    if (next_motion.value() != motion_binding.value()) {
      auto rebound =
          motion_state_channel->RebindRoute(next_motion, transition_time_ns);
      if (!rebound.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
      motion_rebound = true;
      rebound = rebind_motion_route(next_motion, next_motion_aliases);
      if (!rebound.ok()) {
        rollback();
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
    }
    if (next_gripper.has_value() != gripper_binding.has_value() ||
        (next_gripper.has_value() && gripper_binding.has_value() &&
         next_gripper->value() != gripper_binding->value())) {
      if (!next_gripper.has_value() || !gripper_binding.has_value() ||
          gripper_state_channel == nullptr || gripper == nullptr) {
        rollback();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_predecessor_gripper_owner_missing",
            "native predecessor rebind has no gripper owner");
      }
      auto rebound =
          gripper_state_channel->RebindRoute(*next_gripper, transition_time_ns);
      if (!rebound.ok()) {
        rollback();
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
      gripper_rebound = true;
      rebound =
          rebind_gripper_route(*next_gripper, gripper_state_channel);
      if (!rebound.ok()) {
        rollback();
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
    }

    auto published =
        addressed_kernel->adopt_runtime_route_configuration_predecessor(
            configuration, registrations, kernel_resolver);
    if (!published.ok()) {
      rollback();
      return published;
    }
    auto readback = addressed_kernel->runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
    if (!readback.ok() || readback.value() != published.value() ||
        readback.value().operation_admission_open ||
        !readback.value().readiness_rows.empty()) {
      rollback();
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_predecessor_readback_mismatch",
          "native predecessor adoption failed independent closed readback");
    }

    if (stop_surveillance_owner != nullptr) {
      auto next_sources = stop_surveillance_sources(
          next_motion, motion_readiness, motion_state_channel, next_gripper,
          gripper_readiness, gripper_state_channel, participant_engine.get());
      auto valid = RetainedStopSurveillanceOwner::ValidateSources(next_sources);
      if (!valid.ok()) {
        throw std::logic_error(
            "predecessor surveillance source validation failed");
      }
      auto replaced =
          stop_surveillance_owner->ReplaceSources(std::move(next_sources));
      if (!replaced.ok()) {
        throw std::logic_error(
            "predecessor surveillance source replacement failed");
      }
    }

    motion_binding = std::move(next_motion);
    gripper_binding = std::move(next_gripper);
    motion_binding_aliases = std::move(next_motion_aliases);
    gripper_binding_aliases = std::move(next_gripper_aliases);
    participant_registrations = std::move(registrations);
    participant_resolver = std::move(kernel_resolver);

    const auto slot_snapshot = addressed_kernel->resource_table_snapshot();
    std::vector<core::ResourceId> orphan_resources;
    const auto collect_domain_orphans =
        [&](const core::RuntimeParticipantBinding &binding) {
          const auto &value = binding.value();
          if (!value.control_path.has_value() ||
              value.control_path->control_domain_id !=
                  configuration.control_domain_id) {
            return;
          }
          for (const auto &resource : value.resources) {
            const auto slot = std::lower_bound(
                slot_snapshot.slots.begin(), slot_snapshot.slots.end(),
                resource.resource_id,
                [](const auto &row, const core::ResourceId &id) {
                  return row.resource_id < id;
                });
            if (slot != slot_snapshot.slots.end() &&
                slot->resource_id == resource.resource_id &&
                slot->state == core::ResourceSlotState::
                                   kUnreconciledExternalOrOrphanGoal) {
              orphan_resources.push_back(resource.resource_id);
            }
          }
        };
    collect_domain_orphans(motion_binding);
    if (gripper_binding.has_value()) {
      collect_domain_orphans(*gripper_binding);
    }
    std::sort(orphan_resources.begin(), orphan_resources.end());
    orphan_resources.erase(
        std::unique(orphan_resources.begin(), orphan_resources.end()),
        orphan_resources.end());
    if (!orphan_resources.empty()) {
      if (stop_surveillance_owner == nullptr ||
          !addressed_request_owner_epoch.has_value() ||
          !addressed_cell_id.has_value()) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_predecessor_orphan_reconciliation_owner_missing",
            "native predecessor has no command-free orphan reconciliation "
            "owner",
            core::ErrorCode::kMissingReference);
      }
      const auto assessed_at_ns = clock->now();
      auto facts =
          stop_surveillance_owner->BuildOrphanResourceReconciliationFacts(
              *addressed_request_owner_epoch, *addressed_cell_id,
              slot_snapshot.operation_resource_generation, orphan_resources,
              assessed_at_ns);
      if (!facts.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            facts.error());
      }
      auto reconciled =
          addressed_kernel->reconcile_orphaned_resources(facts.value());
      if (!reconciled.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            reconciled.error());
      }
    }
    return readback;
  } catch (const std::exception &exception) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_adoption_failed", exception.what());
  } catch (...) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_adoption_failed",
        "native predecessor adoption raised an unknown exception");
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
      config.operation_journal_path.empty() || config.graph_revision.empty() ||
      config.request_replay_capacity == 0U ||
      config.request_replay_capacity > core::kMaxI6OperationReplayEntries) {
    throw std::invalid_argument(
        "native addressed-owner attachment is incomplete or duplicated");
  }
  const core::RequestOwnerEpoch kernel_request_owner_epoch =
      config.request_owner_epoch;
  const core::CellId kernel_cell_id = config.cell_id;
  runtime->operation_journal = std::make_unique<core::SqliteOperationJournal>(
      std::move(config.operation_journal_path), std::move(config.cell_id));
  try {
    std::optional<core::StopSurveillanceExecutionConfig>
        stop_surveillance_config;
    runtime->stop_surveillance_owner =
        std::make_unique<RetainedStopSurveillanceOwner>(
            stop_surveillance_sources(
                runtime->motion_binding,
                readiness_for(runtime->participant_registrations,
                              runtime->motion_binding),
                runtime->motion_state_channel, runtime->gripper_binding,
                runtime->gripper_binding.has_value()
                    ? readiness_for(runtime->participant_registrations,
                                    *runtime->gripper_binding)
                    : std::nullopt,
                runtime->gripper_state_channel,
                runtime->participant_engine.get()));
    if (runtime->stop_surveillance_policy.has_value()) {
      stop_surveillance_config = core::StopSurveillanceExecutionConfig{
          runtime->scheduler.get(), runtime->stop_surveillance_owner.get(),
          *runtime->stop_surveillance_policy};
    }
    auto *const runtime_owner = runtime.get();
    auto *const admission_gate = config.operation_admission_gate;
    core::StopFaultRecoveryFactsResolver recovery_facts_resolver =
        [runtime_owner, admission_gate](const core::StopStatusDTO &status,
                                        const std::int64_t assessed_at_ns)
        -> core::Result<core::StopFaultRecoveryFactsDTO> {
      if (runtime_owner->stop_surveillance_owner == nullptr ||
          runtime_owner->addressed_kernel == nullptr ||
          !status.fault_latch.has_value()) {
        return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
            core::ErrorDTO{
                std::string(
                    core::to_string(core::ErrorCode::kMissingReference)),
                "native Stop recovery owner is unavailable",
                {{"reason", "native_stop_recovery_owner_unavailable"}}});
      }
      auto facts = runtime_owner->stop_surveillance_owner->BuildRecoveryFacts(
          status, assessed_at_ns);
      if (!facts.ok())
        return facts;
      const auto &latch = *status.fault_latch;
      for (const auto &slot :
           runtime_owner->addressed_kernel->resource_snapshot()) {
        if (!std::binary_search(latch.captured_resources.begin(),
                                latch.captured_resources.end(),
                                slot.resource_id)) {
          continue;
        }
        const bool unresolved =
            slot.state ==
                core::ResourceSlotState::kUnreconciledExternalOrOrphanGoal ||
            slot.state == core::ResourceSlotState::kReserved ||
            slot.state == core::ResourceSlotState::kDispatched ||
            slot.state == core::ResourceSlotState::kStopping ||
            (slot.state == core::ResourceSlotState::kTerminal &&
             slot.stop_generation > latch.stop_generation);
        if (unresolved) {
          facts.value().fenced_or_orphan_resources.push_back(slot.resource_id);
        }
      }
      if (admission_gate != nullptr) {
        auto fenced =
            admission_gate->current_fenced_resources(latch.captured_resources);
        facts.value().fenced_or_orphan_resources.insert(
            facts.value().fenced_or_orphan_resources.end(), fenced.begin(),
            fenced.end());
      }
      auto &blocked = facts.value().fenced_or_orphan_resources;
      std::sort(blocked.begin(), blocked.end());
      blocked.erase(std::unique(blocked.begin(), blocked.end()), blocked.end());
      if (core::stop_fault_recovery_facts_field_error(facts.value())
              .has_value()) {
        return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
            core::ErrorDTO{
                std::string(
                    core::to_string(core::ErrorCode::kInvariantViolation)),
                "native Stop recovery facts failed Kernel blocker join",
                {{"reason", "native_stop_recovery_blocker_join_invalid"}}});
      }
      return facts;
    };
    runtime->addressed_kernel =
        std::make_unique<core::ResourceAddressedExecutionKernel>(
            runtime->participant_registrations, *runtime->clock,
            *runtime->operation_journal,
            core::ResourceAddressedExecutionKernelConfig{
                std::move(config.request_owner_epoch), kernel_cell_id,
                config.request_replay_capacity,
                std::move(runtime->participant_resolver),
                config.operation_admission_gate,
                std::move(config.graph_revision),
                std::move(config.cell_stop_deadline_budget),
                core::kMaximumEmergencyStopCorrelationEntries, 0U, 0U,
                std::move(stop_surveillance_config),
                std::move(recovery_facts_resolver)});
    runtime->addressed_request_owner_epoch = kernel_request_owner_epoch;
    runtime->addressed_cell_id = kernel_cell_id;
  } catch (...) {
    runtime->stop_surveillance_owner.reset();
    runtime->operation_journal.reset();
    throw;
  }
}

std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime(RetainedNativeRuntimeInput input,
                               RetainedNativeRuntimeConfig config) {
  config.kernel.runtime_collision_stop_owned_by_addressed_kernel = true;
  auto stop_budget = config.cell_stop_deadline_budget;
  if (!stop_budget.has_value() && input.motion_stop_readiness.has_value()) {
    stop_budget = input.motion_stop_readiness->value()
                      .resolved_stop_base_plan.deadline_budget;
  }
  config.cell_stop_deadline_budget = stop_budget;
  RetainedNativeAddressedOwnerConfig owner_config{
      std::move(config.request_owner_epoch),
      std::move(config.cell_id),
      std::move(config.operation_journal_path),
      config.request_replay_capacity,
      input.operation_admission_gate,
      input.motion_binding.value().graph_revision,
      std::move(stop_budget)};
  auto runtime = create_retained_native_runtime_participants(std::move(input),
                                                             std::move(config));
  attach_retained_native_addressed_owner(runtime, std::move(owner_config));
  return runtime;
}

} // namespace manyforge::roboplan_backend
