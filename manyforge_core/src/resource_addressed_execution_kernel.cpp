#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ErrorDTO error(std::string code, std::string message, std::string reason) {
  return ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

template <typename T>
[[nodiscard]] Result<T> reject(std::string message, std::string reason,
                               ErrorCode code = ErrorCode::kExecutionRejected) {
  return Result<T>::Err(error(std::string(to_string(code)), std::move(message), std::move(reason)));
}

[[nodiscard]] std::vector<RuntimeParticipantBinding> extract_bindings(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  std::vector<RuntimeParticipantBinding> bindings;
  bindings.reserve(registrations.size());
  for (const auto& registration : registrations) {
    if (registration.engine == nullptr) {
      throw std::invalid_argument("runtime participant registration has no execution engine");
    }
    bindings.push_back(registration.binding);
  }
  return bindings;
}

[[nodiscard]] const ProgramFrameRoleBindingDTO* find_frame(
    const RuntimeParticipantBindingDTO& binding, const FrameRoleId& role) noexcept {
  const auto found = std::find_if(
      binding.frame_bindings.begin(), binding.frame_bindings.end(),
      [&](const ProgramFrameRoleBindingDTO& candidate) { return candidate.frame_role_id == role; });
  return found == binding.frame_bindings.end() ? nullptr : &*found;
}

[[nodiscard]] const RuntimeResourceBindingDTO* find_source_joint(
    const RuntimeParticipantBindingDTO& binding, const std::string& source_joint_name) noexcept {
  const auto found = std::find_if(binding.resources.begin(), binding.resources.end(),
                                  [&](const RuntimeResourceBindingDTO& candidate) {
                                    return candidate.source_joint_name.has_value() &&
                                           *candidate.source_joint_name == source_joint_name;
                                  });
  return found == binding.resources.end() ? nullptr : &*found;
}

[[nodiscard]] const ProgramModelRoleBindingDTO* find_model(
    const RuntimeParticipantBindingDTO& binding,
    const ModelInstanceId& model_instance_id) noexcept {
  const auto found = std::find_if(binding.model_bindings.begin(), binding.model_bindings.end(),
                                  [&](const ProgramModelRoleBindingDTO& candidate) {
                                    return candidate.model_instance_id == model_instance_id;
                                  });
  return found == binding.model_bindings.end() ? nullptr : &*found;
}

[[nodiscard]] CapabilityId required_planner_capability(const MotionSpecDTO& motion) {
  return std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, JointGoalDTO>) {
          return CapabilityId("planning.motion.joint_goal");
        } else if constexpr (std::is_same_v<Value, PoseGoalDTO>) {
          return CapabilityId("planning.motion.pose_goal");
        } else if constexpr (std::is_same_v<Value, NamedTargetDTO>) {
          return CapabilityId("planning.motion.named_target");
        } else {
          return CapabilityId("planning.motion.cartesian_path");
        }
      },
      motion);
}

[[nodiscard]] Result<std::optional<ResolvedContactGraspBindingDTO>>
resolve_contact_grasp_binding(const MotionParticipantIntentDTO& motion,
                              const RuntimeParticipantBindingDTO& binding) {
  if (!motion.contact_policy_id.has_value()) {
    return Result<std::optional<ResolvedContactGraspBindingDTO>>::Ok(
        std::nullopt);
  }
  const auto policy = std::lower_bound(
      binding.contact_grasp_bindings.begin(),
      binding.contact_grasp_bindings.end(), *motion.contact_policy_id,
      [](const auto& candidate, const ContactPolicyId& id) {
        return candidate.contact_policy_id < id;
      });
  if (policy == binding.contact_grasp_bindings.end() ||
      policy->contact_policy_id != *motion.contact_policy_id) {
    // Other contact-policy modes remain selected from the retained scene
    // policy. Only Program-owned contact_grasp requires this late binding.
    return Result<std::optional<ResolvedContactGraspBindingDTO>>::Ok(
        std::nullopt);
  }
  const auto object = std::lower_bound(
      motion.runtime_object_bindings.begin(),
      motion.runtime_object_bindings.end(), policy->runtime_object_id,
      [](const auto& candidate, const RuntimeObjectId& id) {
        return candidate.runtime_object_id < id;
      });
  if (object == motion.runtime_object_bindings.end() ||
      object->runtime_object_id != policy->runtime_object_id) {
    return reject<std::optional<ResolvedContactGraspBindingDTO>>(
        "contact-grasp policy has no selected runtime object",
        "contact_grasp_runtime_object_unresolved",
        ErrorCode::kMissingReference);
  }
  std::vector<CollisionBodyId> body_ids;
  for (const auto& body : policy->allowed_body_bindings) {
    body_ids.insert(body_ids.end(), body.collision_body_ids.begin(),
                    body.collision_body_ids.end());
  }
  std::sort(body_ids.begin(), body_ids.end());
  body_ids.erase(std::unique(body_ids.begin(), body_ids.end()),
                 body_ids.end());
  if (body_ids.empty()) {
    return reject<std::optional<ResolvedContactGraspBindingDTO>>(
        "contact-grasp policy resolved no robot collision bodies",
        "contact_grasp_body_binding_empty", ErrorCode::kInvariantViolation);
  }
  return Result<std::optional<ResolvedContactGraspBindingDTO>>::Ok(
      std::optional<ResolvedContactGraspBindingDTO>{
          ResolvedContactGraspBindingDTO{
          policy->contact_policy_id,
          policy->contact_policy_revision,
          policy->runtime_object_id,
          object->scene_resource_id,
          std::move(body_ids),
          policy->max_penetration_m,
          policy->terminal_window_start_fraction}});
}

[[nodiscard]] ResolvedFrameDTO resolved_component_frame(const RuntimeParticipantBindingDTO& binding,
                                                        const ProgramFrameRoleBindingDTO& frame) {
  return ResolvedComponentModelFrameDTO{binding.controllable_component_id, frame.model_instance_id,
                                        frame.frame_role_id, frame.compiled_frame_name,
                                        frame.model_revision};
}

[[nodiscard]] Result<ExecuteRequestDTO> build_motion_request(
    const OperationIntentDTO& operation, const OperationParticipantIntentDTO& participant,
    const MotionParticipantIntentDTO& motion, const RuntimeParticipantBindingDTO& binding,
    const OperationId& operation_id, OperationRevision operation_revision,
    const OperationParticipantId& participant_id, const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation) {
  if (binding.model_groups.size() != 1U) {
    return reject<ExecuteRequestDTO>("I6 motion requires exactly one retained model group",
                                     "i6_motion_model_group_cardinality_invalid");
  }
  const auto* base = find_frame(binding, motion.base_frame_role_id);
  const auto* tcp = find_frame(binding, motion.tcp_frame_role_id);
  if (base == nullptr || tcp == nullptr) {
    return reject<ExecuteRequestDTO>("motion frame roles do not resolve through the exact binding",
                                     "motion_frame_role_unresolved");
  }

  const auto& model_group = binding.model_groups.front();
  const auto* group_model = find_model(binding, model_group.model_instance_id);
  if (group_model == nullptr || !binding.control_path.has_value()) {
    return reject<ExecuteRequestDTO>(
        "motion model group or control path is absent from the exact binding",
        "motion_runtime_binding_incomplete", ErrorCode::kInvariantViolation);
  }
  std::vector<std::string> compiled_joint_names;
  compiled_joint_names.reserve(binding.resources.size());
  for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
    const auto& resource = binding.resources[index];
    if (resource.coordinate_kind != CoordinateKind::kAngular ||
        std::count(model_group.expanded_compiled_joint_names.begin(),
                   model_group.expanded_compiled_joint_names.end(),
                   resource.compiled_resource_name) != 1) {
      return reject<ExecuteRequestDTO>(
          "motion requires all-angular resources belonging to the exact retained model group",
          "motion_resource_projection_not_all_angular", ErrorCode::kInvariantViolation);
    }
    compiled_joint_names.push_back(resource.compiled_resource_name);
  }
  const auto& control = *binding.control_path;
  const auto planner_capability = required_planner_capability(motion.motion);
  const auto* planner_definition = find_builtin_capability_definition(planner_capability);
  if (planner_definition == nullptr) {
    return reject<ExecuteRequestDTO>("motion kind has no registered planning capability",
                                     "motion_planner_capability_unregistered",
                                     ErrorCode::kInvariantViolation);
  }
  const CapabilityDefinitionRevision planner_revision(
      std::string(planner_definition->capability_definition_revision));
  const auto planner_path =
      std::find_if(binding.supporting_capability_provider_paths.begin(),
                   binding.supporting_capability_provider_paths.end(),
                   [&](const RuntimeCapabilityProviderPathDTO& candidate) {
                     return candidate.capability_id == planner_capability &&
                            candidate.capability_definition_revision == planner_revision;
                   });
  if (planner_path == binding.supporting_capability_provider_paths.end()) {
    return reject<ExecuteRequestDTO>(
        "motion planner capability does not resolve through the exact binding",
        "motion_planner_capability_unresolved", ErrorCode::kMissingReference);
  }
  std::vector<PlannerModelBindingDTO> models;
  models.reserve(binding.model_bindings.size());
  for (const auto& model : binding.model_bindings) {
    models.push_back(PlannerModelBindingDTO{model.model_instance_id, model.model_revision});
  }
  std::vector<PlannerRuntimeUnitBindingDTO> runtime_units;
  runtime_units.reserve(binding.runtime_unit_epochs.size());
  for (const auto& runtime : binding.runtime_unit_epochs) {
    runtime_units.push_back(
        PlannerRuntimeUnitBindingDTO{runtime.runtime_unit_id, runtime.runtime_epoch});
  }
  std::vector<ResourceId> bound_resources;
  bound_resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    bound_resources.push_back(resource.resource_id);
  }

  ResolvedExecutionBindingDTO exact_binding{
      operation_id,
      participant_id,
      controller_goal_id,
      controller_goal_generation,
      binding.program_id,
      binding.program_revision,
      binding.program_binding_id,
      operation.program_run_id,
      binding.controllable_component_id,
      binding.component_type_id,
      binding.component_type_revision,
      binding.command_target_id,
      binding.capability_id,
      binding.capability_definition_revision,
      PlannerCapabilityBindingDTO{
          planner_path->capability_id, planner_path->capability_definition_revision,
          planner_path->capability_provider_binding_id,
          planner_path->capability_provider_binding_revision, planner_path->provider_instance_id,
          planner_path->provider_manifest_id, planner_path->provider_manifest_revision,
          planner_path->provider_runtime_epoch},
      std::move(models),
      PlannerModelGroupBindingDTO{model_group.model_group_role_id, model_group.model_instance_id,
                                  group_model->model_revision, model_group.compiled_group_name,
                                  std::move(compiled_joint_names)},
      control.control_domain_id,
      control.control_domain_revision,
      control.control_binding_id,
      control.control_binding_revision,
      control.control_interface_contract_id,
      control.control_interface_revision,
      control.controller_topology_id,
      control.topology_revision,
      control.control_route_id,
      control.activation_set_id,
      control.active_control_configuration_id,
      control.active_control_configuration_generation,
      control.provider_instance_id,
      control.provider_manifest_id,
      control.provider_manifest_revision,
      control.provider_runtime_epoch,
      std::move(runtime_units),
      std::move(bound_resources),
      resolved_component_frame(binding, *base),
      resolved_component_frame(binding, *tcp),
      {},
      {},
      control.runtime_scale_qualification,
      binding.retained_bundle_id,
      binding.graph_revision,
      binding.target_role_id,
      binding.frame_bindings,
      binding.collision_body_bindings};

  ExecuteRequestDTO request(std::move(exact_binding));
  request.move_id = motion.move_id.value();
  request.tolerances = motion.tolerances;
  request.speed_profile = motion.speed_profile;
  request.timeouts = motion.timeouts;
  request.cache_mode = motion.cache_mode;
  request.planning_profile = motion.planning_profile;
  request.target_mode = motion.target_mode;
  request.target_reference = motion.target_reference;
  request.offset_reference = motion.offset_reference;
  request.motion_interrupt_scopes = motion.motion_interrupt_scopes;
  request.action_span = participant.action_span;
  request.contact_policy_id =
      motion.contact_policy_id.has_value() ? motion.contact_policy_id->value() : std::string{};
  request.runtime_object_bindings = motion.runtime_object_bindings;
  auto contact_grasp = resolve_contact_grasp_binding(motion, binding);
  if (!contact_grasp.ok()) {
    return Result<ExecuteRequestDTO>::Err(contact_grasp.error());
  }
  request.resolved_contact_grasp = std::move(contact_grasp.value());
  if (control.runtime_scale_qualification.has_value()) {
    const auto& qualification = *control.runtime_scale_qualification;
    request.initial_runtime_scale =
        ResolvedRuntimeScaleCommandDTO{operation_id,
                                       operation_revision,
                                       participant_id,
                                       binding.controllable_component_id,
                                       binding.command_target_id,
                                       binding.capability_id,
                                       control.control_domain_id,
                                       control.control_domain_revision,
                                       control.control_binding_id,
                                       control.control_binding_revision,
                                       controller_goal_id,
                                       controller_goal_generation,
                                       control.controller_topology_id,
                                       control.topology_revision,
                                       control.control_route_id,
                                       qualification.scaling_domain_id,
                                       qualification.coordinated_barrier_id,
                                       request.binding.resources,
                                       control.active_control_configuration_id,
                                       control.active_control_configuration_generation,
                                       RuntimeScaleGeneration(0U),
                                       RuntimeScaleGeneration(0U),
                                       1.0};
  }

  if (const auto* goal = std::get_if<JointGoalDTO>(&motion.motion)) {
    std::set<std::string, std::less<>> seen_joint_names;
    std::vector<ResourceId> resolved_resources;
    std::vector<std::string> compiled_names;
    resolved_resources.reserve(goal->joint_names.size());
    compiled_names.reserve(goal->joint_names.size());
    for (const auto& joint_name : goal->joint_names) {
      const auto* resource = find_source_joint(binding, joint_name);
      if (resource == nullptr || resource->coordinate_kind != CoordinateKind::kAngular ||
          !seen_joint_names.insert(joint_name).second) {
        return reject<ExecuteRequestDTO>(
            "joint goal source joints are missing, duplicated, or non-angular",
            "joint_goal_source_joint_unresolved");
      }
      resolved_resources.push_back(resource->resource_id);
      compiled_names.push_back(resource->compiled_resource_name);
    }
    request.motion = ResolvedJointGoalDTO{std::move(resolved_resources), std::move(compiled_names),
                                          goal->positions_rad};
  } else if (const auto* pose = std::get_if<PoseGoalDTO>(&motion.motion)) {
    request.motion = *pose;
  } else if (const auto* path = std::get_if<CartesianPathDTO>(&motion.motion)) {
    request.motion = *path;
  } else {
    return reject<ExecuteRequestDTO>(
        "named targets require the retained Program target map before planning",
        "named_target_resolution_unavailable", ErrorCode::kMissingReference);
  }

  for (const auto& [joint_name, limit] : motion.per_joint_dynamics_limits) {
    const auto* resource = find_source_joint(binding, joint_name);
    if (resource == nullptr || resource->coordinate_kind != CoordinateKind::kAngular) {
      return reject<ExecuteRequestDTO>(
          "motion dynamics source joint does not resolve to an angular resource",
          "motion_dynamics_source_joint_unresolved");
    }
    request.per_resource_dynamics_limits.push_back(ResolvedResourceDynamicsLimitDTO{
        resource->resource_id, resource->compiled_resource_name, limit});
  }
  std::sort(request.per_resource_dynamics_limits.begin(),
            request.per_resource_dynamics_limits.end(),
            [&](const ResolvedResourceDynamicsLimitDTO& lhs,
                const ResolvedResourceDynamicsLimitDTO& rhs) {
              const auto left = std::find(request.binding.resources.begin(),
                                          request.binding.resources.end(), lhs.resource_id);
              const auto right = std::find(request.binding.resources.begin(),
                                           request.binding.resources.end(), rhs.resource_id);
              return left < right;
            });
  return Result<ExecuteRequestDTO>::Ok(std::move(request));
}

[[nodiscard]] Result<GripperCommandRequestDTO> build_command_request(
    const CapabilityCommandIntentDTO& command, const RuntimeParticipantBindingDTO& binding) {
  if (binding.model_groups.size() != 1U || binding.resources.size() != 1U) {
    return reject<GripperCommandRequestDTO>(
        "the retained I6 gripper mechanism requires one model group and one command resource",
        "i6_gripper_command_scope_unsupported");
  }
  if (command.command_schema_id.value() != kGripperPositionCommandSchemaId ||
      command.command_schema_revision.value() != kGripperPositionCommandSchemaRevision) {
    return reject<GripperCommandRequestDTO>("the command is not the qualified I6 gripper schema",
                                            "i6_command_schema_unsupported",
                                            ErrorCode::kUnsupportedCapability);
  }

  double command_duration_s = 0.0;
  for (const auto& option : command.caller_options) {
    if (option.option_id.value() != "command_duration_s") {
      return reject<GripperCommandRequestDTO>(
          "the command carries an option not declared by its schema", "command_option_unsupported",
          ErrorCode::kInvalidRequest);
    }
    command_duration_s = option.value_s;
  }

  const auto& value = std::get<GripperPositionCommandValueDTO>(command.command_value);
  const auto kind = coordinate_kind(value.position);
  if (kind != binding.resources.front().coordinate_kind) {
    return reject<GripperCommandRequestDTO>(
        "gripper command unit differs from its retained resource",
        "gripper_command_coordinate_kind_mismatch", ErrorCode::kInvalidRequest);
  }
  const double target_position = kind == CoordinateKind::kAngular
                                     ? std::get<AngularPositionDTO>(value.position).value_rad
                                     : std::get<LinearPositionDTO>(value.position).value_m;
  std::optional<double> max_effort;
  if (value.max_effort.has_value()) {
    max_effort = kind == CoordinateKind::kAngular
                     ? std::get<AngularEffortDTO>(*value.max_effort).value_nm
                     : std::get<LinearEffortDTO>(*value.max_effort).value_n;
  }

  GripperCommandRequestDTO request;
  request.gripper_id = binding.command_target_id.value();
  request.group_id = binding.model_groups.front().compiled_group_name;
  request.joint_name = binding.resources.front().compiled_resource_name;
  request.target_position = target_position;
  request.duration_s = command_duration_s;
  request.max_effort = max_effort;
  request.commanded_state = "position";
  request.actuation_kind = kind == CoordinateKind::kAngular ? "angular" : "linear";
  request.state_authority = binding.control_path->state_authority_id.value();
  request.completion_policy = "goal_fenced_target_reached";
  request.grasp_confirmation_policy = "binding_feedback_required";
  request.transport_kind = "resolved_binding";
  request.evidence = {"command_terminal"};
  request.evidence_state = "required_pending";
  request.completion_timeout_s = command.completion_timeout_s;
  return Result<GripperCommandRequestDTO>::Ok(std::move(request));
}

[[nodiscard]] OperationStateKind operation_state(ExecutionState state) noexcept {
  switch (state) {
    case ExecutionState::kIdle:
      return OperationStateKind::kPrepared;
    case ExecutionState::kPlanning:
      return OperationStateKind::kPlanning;
    case ExecutionState::kValidating:
      return OperationStateKind::kValidating;
    case ExecutionState::kExecuting:
      return OperationStateKind::kExecuting;
    case ExecutionState::kInterrupted:
      return OperationStateKind::kInterrupted;
    case ExecutionState::kPaused:
      return OperationStateKind::kPaused;
    case ExecutionState::kStopping:
      return OperationStateKind::kStopping;
    case ExecutionState::kStopped:
      return OperationStateKind::kStopped;
    case ExecutionState::kCompleted:
      return OperationStateKind::kCompleted;
    case ExecutionState::kFailed:
      return OperationStateKind::kFailed;
    case ExecutionState::kInterventionRequired:
      return OperationStateKind::kInterventionRequired;
  }
  return OperationStateKind::kInterventionRequired;
}

[[nodiscard]] bool terminal(OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kCompleted:
    case OperationStateKind::kStopped:
    case OperationStateKind::kFailed:
    case OperationStateKind::kInterrupted:
    case OperationStateKind::kInterventionRequired:
    case OperationStateKind::kCancelledPreDispatch:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool control_admissible(OperationControlKind control,
                                      OperationStateKind state) noexcept {
  switch (control) {
    case OperationControlKind::kPause:
      return state == OperationStateKind::kExecuting;
    case OperationControlKind::kResume:
      return state == OperationStateKind::kPaused;
    case OperationControlKind::kReset:
      return state != OperationStateKind::kCompleted;
    case OperationControlKind::kAcknowledgeCompleted:
      return state == OperationStateKind::kCompleted;
  }
  return false;
}

[[nodiscard]] const char* control_name(OperationControlKind control) noexcept {
  switch (control) {
    case OperationControlKind::kPause:
      return "pause";
    case OperationControlKind::kResume:
      return "resume";
    case OperationControlKind::kReset:
      return "reset";
    case OperationControlKind::kAcknowledgeCompleted:
      return "acknowledge_completed";
  }
  return "unknown";
}

[[nodiscard]] std::optional<ResourceSlotState> slot_state_for(OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kDispatching:
    case OperationStateKind::kExecuting:
    case OperationStateKind::kPaused:
      return ResourceSlotState::kDispatched;
    case OperationStateKind::kStopping:
      return ResourceSlotState::kStopping;
    case OperationStateKind::kCompleted:
    case OperationStateKind::kStopped:
    case OperationStateKind::kFailed:
    case OperationStateKind::kInterrupted:
    case OperationStateKind::kInterventionRequired:
    case OperationStateKind::kCancelledPreDispatch:
      return ResourceSlotState::kTerminal;
    case OperationStateKind::kQueued:
    case OperationStateKind::kResolving:
    case OperationStateKind::kPlanning:
    case OperationStateKind::kValidating:
    case OperationStateKind::kPrepared:
      return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] ErrorDTO status_error(ErrorCode code) {
  return ErrorDTO{std::string(to_string(code)),
                  "subordinate participant reached a terminal error state",
                  {{"reason", "subordinate_terminal_error"}}};
}

}  // namespace

struct ResourceAddressedExecutionKernel::ReplayEntry final {
  OperationIntentDTO intent;
  OperationAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::ControlReplayEntry final {
  OperationControlRequestDTO request;
  OperationControlAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::StopReplayEntry final {
  StopIntentDTO intent;
  StopAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::RuntimeScaleReplayEntry final {
  RuntimeScaleRequestDTO request;
  RuntimeScaleAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::ActiveOperation final {
  OperationIntentDTO intent;
  OperationAcceptedDTO accepted;
  RuntimeParticipantBinding binding;
  IResolvedParticipantExecutionEngine* engine{};
  ResourceSlotOwnerDTO slot_owner;
  std::vector<ResourceId> resources;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  OperationStateDTO state;
  ResourceSlotState slot_state{ResourceSlotState::kReserved};
  std::uint64_t subordinate_id{};
  std::uint64_t subordinate_generation{};
  std::optional<OperationControlKind> pending_control;
  std::optional<ErrorDTO> last_subordinate_error;
  std::optional<RuntimeScaleStatusDTO> runtime_scale_status;
};

struct ResourceAddressedExecutionKernel::EngineSubscription final {
  IResolvedParticipantExecutionEngine* engine{};
  SubscriptionHandle handle{};
};

ResourceAddressedExecutionKernel::ResourceAddressedExecutionKernel(
    std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
    IOperationJournal& journal, ResourceAddressedExecutionKernelConfig config)
    : registry_(extract_bindings(registrations)),
      resource_slots_(registry_.resource_inventory()),
      registrations_(std::move(registrations)),
      clock_(clock),
      journal_(journal),
      config_(std::move(config)) {
  if (config_.request_owner_epoch.empty() || config_.request_replay_capacity == 0U ||
      config_.request_replay_capacity > kMaxI6OperationReplayEntries) {
    throw std::invalid_argument("addressed Kernel request-owner epoch or replay bound is invalid");
  }
  replay_ledger_.reserve(config_.request_replay_capacity);
  control_replay_ledger_.reserve(config_.request_replay_capacity);
  stop_replay_ledger_.reserve(config_.request_replay_capacity);
  runtime_scale_replay_ledger_.reserve(config_.request_replay_capacity);

  auto recovery =
      journal_.BeginOwnerEpoch(config_.request_owner_epoch, clock_.now(),
                               config_.request_replay_capacity, registry_.resource_inventory());
  if (!recovery.ok()) {
    throw std::runtime_error("addressed Kernel durable owner admission failed: " +
                             recovery.error().code + ": " + recovery.error().message);
  }
  historical_operations_ = std::move(recovery.value().historical_operations);
  if (!recovery.value().unreconciled_resources.empty()) {
    auto marked = resource_slots_.mark_unreconciled_external_or_orphan_goal(
        recovery.value().unreconciled_resources);
    if (!marked.ok()) {
      throw std::runtime_error("addressed Kernel durable orphan recovery failed: " +
                               marked.error().code + ": " + marked.error().message);
    }
  }

  for (const auto& registration : registrations_) {
    if (std::any_of(
            engine_subscriptions_.begin(), engine_subscriptions_.end(),
            [&](const EngineSubscription& item) { return item.engine == registration.engine; })) {
      continue;
    }
    auto* engine = registration.engine;
    const auto handle = engine->subscribe_events(
        [this, engine](const KernelEvent& event) { on_engine_event(engine, event); });
    engine_subscriptions_.push_back(EngineSubscription{engine, handle});
  }
}

ResourceAddressedExecutionKernel::~ResourceAddressedExecutionKernel() {
  for (const auto& subscription : engine_subscriptions_) {
    subscription.engine->unsubscribe_events(subscription.handle);
  }
}

IResolvedParticipantExecutionEngine* ResourceAddressedExecutionKernel::engine_for(
    const RuntimeParticipantBinding& binding) const noexcept {
  const auto found = std::find_if(registrations_.begin(), registrations_.end(),
                                  [&](const RuntimeParticipantExecutionRegistration& registration) {
                                    return registration.binding.value() == binding.value();
                                  });
  return found == registrations_.end() ? nullptr : found->engine;
}

Result<OperationAcceptedDTO> ResourceAddressedExecutionKernel::execute(
    const OperationIntentDTO& intent) {
  std::lock_guard submit_lock(submission_mutex_);

  auto validation = validate_operation_intent(intent);
  if (!validation.ok()) {
    return Result<OperationAcceptedDTO>::Err(validation.error());
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<OperationAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "operation request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }
  if (intent.participants.size() != 1U ||
      intent.coordination_policy != OperationCoordinationPolicy::kIndependent) {
    return reject<OperationAcceptedDTO>("I6 admits one independent participant per operation",
                                        "i6_single_participant_only");
  }

  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay =
        std::find_if(replay_ledger_.begin(), replay_ledger_.end(), [&](const ReplayEntry& entry) {
          return entry.intent.request_context.request_id == intent.request_context.request_id;
        });
    if (replay != replay_ledger_.end()) {
      if (replay->intent != intent) {
        return Result<OperationAcceptedDTO>::Err(
            error("request_id_conflict", "operation request ID was reused with different intent",
                  "request_id_conflict"));
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<OperationAcceptedDTO>::Ok(std::move(accepted));
    }
    const auto control_request = std::find_if(
        control_replay_ledger_.begin(), control_replay_ledger_.end(),
        [&](const ControlReplayEntry& entry) {
          return entry.request.request_context.request_id == intent.request_context.request_id;
        });
    if (control_request != control_replay_ledger_.end()) {
      return Result<OperationAcceptedDTO>::Err(error(
          "request_id_conflict", "operation request ID was already used by an operation control",
          "request_id_conflict"));
    }
    if (std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.intent.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    })) {
      return Result<OperationAcceptedDTO>::Err(error(
          "request_id_conflict", "operation request ID was already used by another Kernel command",
          "request_id_conflict"));
    }
    if (replay_ledger_.size() + control_replay_ledger_.size() + stop_replay_ledger_.size() +
            runtime_scale_replay_ledger_.size() >=
        config_.request_replay_capacity) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_ledger_full", "operation replay ledger reached its fixed bound",
                "request_ledger_full"));
    }
    if (active_operation_ != nullptr) {
      return reject<OperationAcceptedDTO>("I6 admits only one unsettled operation",
                                          "i6_single_operation_busy");
    }
  }

  auto resolved = registry_.resolve(intent, 0U);
  if (!resolved.ok()) {
    return Result<OperationAcceptedDTO>::Err(resolved.error());
  }
  auto* engine = engine_for(resolved.value());
  if (engine == nullptr) {
    return reject<OperationAcceptedDTO>("resolved participant has no registered subordinate engine",
                                        "runtime_participant_engine_missing",
                                        ErrorCode::kInvariantViolation);
  }
  if (next_operation_id_ == 0U || next_operation_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return reject<OperationAcceptedDTO>("operation identity space is exhausted",
                                        "operation_id_exhausted", ErrorCode::kInternalError);
  }

  const auto& binding = resolved.value().value();
  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  const auto operation_id = OperationId("operation:" + config_.request_owner_epoch.value() + ":" +
                                        std::to_string(next_operation_id_));
  const auto participant_id = OperationParticipantId(operation_id.value() + ":participant:1");
  const auto controller_goal_id = ControllerGoalId(operation_id.value() + ":controller-goal:1");
  const ControllerGoalGeneration controller_goal_generation(1U);
  const OperationRevision operation_revision(1U);
  const auto& control = *binding.control_path;
  const auto payload_kind =
      std::holds_alternative<MotionParticipantIntentDTO>(intent.participants.front().payload)
          ? OperationParticipantPayloadKind::kMotion
          : OperationParticipantPayloadKind::kCapabilityCommand;
  ResourceSlotOwnerDTO slot_owner{operation_id,
                                  operation_revision,
                                  participant_id,
                                  control.control_domain_id,
                                  control.control_domain_revision,
                                  control.control_binding_id,
                                  control.control_binding_revision,
                                  control.controller_topology_id,
                                  control.topology_revision,
                                  control.control_route_id,
                                  control.active_control_configuration_id,
                                  control.active_control_configuration_generation,
                                  control.containment_granularity};
  std::optional<ExecuteRequestDTO> motion_request;
  std::optional<GripperCommandRequestDTO> command_request;
  if (payload_kind == OperationParticipantPayloadKind::kMotion) {
    const auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
    auto built = build_motion_request(intent, intent.participants.front(), motion, binding,
                                      operation_id, operation_revision, participant_id,
                                      controller_goal_id, controller_goal_generation);
    if (!built.ok()) {
      return Result<OperationAcceptedDTO>::Err(built.error());
    }
    motion_request = std::move(built.value());
  } else {
    const auto& command = std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload);
    auto built = build_command_request(command, binding);
    if (!built.ok()) {
      return Result<OperationAcceptedDTO>::Err(built.error());
    }
    command_request = std::move(built.value());
    auto private_validation = engine->validate_gripper_command(*command_request);
    if (!private_validation.ok()) {
      return Result<OperationAcceptedDTO>::Err(private_validation.error());
    }
  }
  auto reservation = resource_slots_.reserve(slot_owner, resources);
  if (!reservation.ok()) {
    return Result<OperationAcceptedDTO>::Err(reservation.error());
  }

  const auto accepted_at_ns = clock_.now();
  const OperationAcceptedDTO accepted{intent.request_context.request_id,
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      operation_id,
                                      operation_revision,
                                      {OperationParticipantAssignmentDTO{0U, participant_id}},
                                      accepted_at_ns};
  std::optional<RuntimeScaleStatusDTO> initial_runtime_scale_status;
  if (payload_kind == OperationParticipantPayloadKind::kMotion &&
      control.runtime_scale_qualification.has_value()) {
    const auto& qualification = *control.runtime_scale_qualification;
    initial_runtime_scale_status = RuntimeScaleStatusDTO{
        operation_id,
        operation_revision,
        RuntimeScaleGeneration(0U),
        1.0,
        1.0,
        {RuntimeScaleParticipantStatusDTO{participant_id,
                                          binding.controllable_component_id,
                                          binding.command_target_id,
                                          binding.capability_id,
                                          control.control_domain_id,
                                          control.control_domain_revision,
                                          control.control_binding_id,
                                          control.control_binding_revision,
                                          controller_goal_id,
                                          controller_goal_generation,
                                          control.controller_topology_id,
                                          control.topology_revision,
                                          control.control_route_id,
                                          qualification.scaling_domain_id,
                                          qualification.coordinated_barrier_id,
                                          resources,
                                          control.active_control_configuration_id,
                                          control.active_control_configuration_generation,
                                          RuntimeScaleGeneration(0U),
                                          1.0,
                                          1.0,
                                          0.0,
                                          control.provider_runtime_epoch,
                                          0U,
                                          0,
                                          true,
                                          RuntimeScaleApplicationState::kPending,
                                          std::nullopt}},
        RuntimeScaleApplicationState::kPending,
        accepted_at_ns};
  }
  OperationParticipantStateDTO participant_state{
      participant_id,
      controller_goal_id,
      controller_goal_generation,
      binding.controllable_component_id,
      binding.command_target_id,
      binding.capability_id,
      control.control_domain_id,
      control.control_domain_revision,
      control.control_binding_id,
      control.control_binding_revision,
      binding.capability_provider_path.has_value()
          ? std::optional(binding.capability_provider_path->capability_provider_binding_id)
          : std::nullopt,
      binding.capability_provider_path.has_value()
          ? std::optional(binding.capability_provider_path->capability_provider_binding_revision)
          : std::nullopt,
      payload_kind,
      OperationStateKind::kResolving,
      0.0,
      resources,
      std::nullopt};
  OperationStateDTO operation_state_dto{operation_id,
                                        operation_revision,
                                        intent.program_binding_id,
                                        intent.program_run_id,
                                        intent.coordination_policy,
                                        OperationStateKind::kResolving,
                                        {std::move(participant_state)},
                                        accepted_at_ns,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt};
  if (initial_runtime_scale_status.has_value()) {
    operation_state_dto.runtime_scale_generation = RuntimeScaleGeneration(0U);
  }

  auto durable_acceptance = journal_.RecordOperationAccepted(config_.request_owner_epoch, intent,
                                                             accepted, operation_state_dto);
  if (!durable_acceptance.ok()) {
    const auto release = resource_slots_.release(slot_owner, resources);
    if (!release.ok()) {
      return Result<OperationAcceptedDTO>::Err(release.error());
    }
    return Result<OperationAcceptedDTO>::Err(durable_acceptance.error());
  }

  {
    std::lock_guard state_lock(state_mutex_);
    active_operation_ = std::make_unique<ActiveOperation>(ActiveOperation{
        intent, accepted, resolved.value(), engine, slot_owner, resources, payload_kind,
        std::move(operation_state_dto), ResourceSlotState::kReserved, 0U, 0U, std::nullopt,
        std::nullopt, std::move(initial_runtime_scale_status)});
    replay_ledger_.push_back(ReplayEntry{intent, accepted});
  }
  ++next_operation_id_;

  Result<void> dispatch_result = Result<void>::Ok();
  if (payload_kind == OperationParticipantPayloadKind::kMotion) {
    auto dispatch = engine->execute(*motion_request);
    if (!dispatch.ok()) {
      dispatch_result = Result<void>::Err(dispatch.error());
    } else {
      std::lock_guard state_lock(state_mutex_);
      if (active_operation_ == nullptr ||
          active_operation_->accepted.operation_id != operation_id) {
        dispatch_result = Result<void>::Err(error(
            std::string(to_string(ErrorCode::kInvariantViolation)),
            "active operation disappeared during subordinate admission", "active_operation_lost"));
      } else if (active_operation_->subordinate_id != 0U &&
                 active_operation_->subordinate_id != dispatch.value()) {
        dispatch_result =
            Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                    "subordinate event and execute result identities disagree",
                                    "subordinate_execution_id_mismatch"));
      } else {
        active_operation_->subordinate_id = dispatch.value();
        if (active_operation_->state.state == OperationStateKind::kResolving) {
          auto planning_state = active_operation_->state;
          planning_state.state = OperationStateKind::kPlanning;
          planning_state.participants.front().state = OperationStateKind::kPlanning;
          planning_state.updated_at_ns = clock_.now();
          auto durable_state =
              journal_.RecordOperationState(config_.request_owner_epoch, planning_state,
                                            active_operation_->runtime_scale_status.has_value()
                                                ? &*active_operation_->runtime_scale_status
                                                : nullptr,
                                            "subordinate_motion_admitted");
          if (durable_state.ok()) {
            active_operation_->state = std::move(planning_state);
          } else {
            dispatch_result = Result<void>::Err(durable_state.error());
          }
        }
      }
    }
  } else {
    const auto dispatch = engine->command_gripper(*command_request);
    if (!dispatch.accepted || dispatch.error.has_value()) {
      dispatch_result = Result<void>::Err(dispatch.error.value_or(
          error(std::string(to_string(ErrorCode::kExecutionRejected)),
                "subordinate gripper command was rejected", "subordinate_command_rejected")));
    } else {
      std::lock_guard state_lock(state_mutex_);
      if (active_operation_ == nullptr ||
          active_operation_->accepted.operation_id != operation_id) {
        dispatch_result = Result<void>::Err(error(
            std::string(to_string(ErrorCode::kInvariantViolation)),
            "active command disappeared during subordinate admission", "active_operation_lost"));
      } else {
        active_operation_->subordinate_id = dispatch.command_id;
        active_operation_->subordinate_generation = dispatch.generation;
        auto executing_state = active_operation_->state;
        executing_state.state = OperationStateKind::kExecuting;
        executing_state.participants.front().state = OperationStateKind::kExecuting;
        executing_state.updated_at_ns = clock_.now();
        auto durable_state =
            journal_.RecordOperationState(config_.request_owner_epoch, executing_state,
                                          active_operation_->runtime_scale_status.has_value()
                                              ? &*active_operation_->runtime_scale_status
                                              : nullptr,
                                          "subordinate_command_admitted");
        if (durable_state.ok()) {
          auto transition =
              resource_slots_.transition(slot_owner, resources, ResourceSlotState::kDispatched);
          if (!transition.ok()) {
            dispatch_result = transition;
          } else {
            active_operation_->slot_state = ResourceSlotState::kDispatched;
            active_operation_->state = std::move(executing_state);
          }
        } else {
          dispatch_result = Result<void>::Err(durable_state.error());
        }
      }
    }
  }

  if (!dispatch_result.ok()) {
    std::lock_guard state_lock(state_mutex_);
    if (active_operation_ != nullptr && active_operation_->accepted.operation_id == operation_id) {
      auto failed_state = active_operation_->state;
      failed_state.state = OperationStateKind::kInterventionRequired;
      failed_state.participants.front().state = OperationStateKind::kInterventionRequired;
      failed_state.participants.front().terminal_error = dispatch_result.error();
      failed_state.updated_at_ns = clock_.now();
      auto durable_failure =
          journal_.RecordOperationState(config_.request_owner_epoch, failed_state,
                                        active_operation_->runtime_scale_status.has_value()
                                            ? &*active_operation_->runtime_scale_status
                                            : nullptr,
                                        "subordinate_dispatch_failed_after_acceptance");
      if (durable_failure.ok()) {
        if (active_operation_->slot_state != ResourceSlotState::kTerminal) {
          const auto transition = resource_slots_.transition(active_operation_->slot_owner,
                                                             active_operation_->resources,
                                                             ResourceSlotState::kTerminal);
          if (transition.ok()) {
            active_operation_->slot_state = ResourceSlotState::kTerminal;
          }
        }
        active_operation_->state = std::move(failed_state);
      }
    }
    return Result<OperationAcceptedDTO>::Ok(accepted);
  }
  return Result<OperationAcceptedDTO>::Ok(accepted);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::pause(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kPause);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::resume(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kResume);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::reset(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kReset);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::acknowledge_completed(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kAcknowledgeCompleted);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::apply_control(
    const OperationControlRequestDTO& request, OperationControlKind control) {
  std::lock_guard control_lock(control_mutex_);
  if (request.request_context.request_id.empty() ||
      request.request_context.request_owner_epoch.empty() ||
      request.request_context.source_label.empty() || request.operation_id.empty() ||
      !request.expected_operation_revision.nonzero() ||
      (request.reason.has_value() && request.reason->empty()) ||
      !request.attribution.locally_valid()) {
    return reject<OperationControlAcceptedDTO>("operation control request is malformed",
                                               "operation_control_request_invalid",
                                               ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<OperationControlAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "operation control names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  IResolvedParticipantExecutionEngine* engine = nullptr;
  std::uint64_t subordinate_id = 0U;
  std::uint64_t subordinate_generation = 0U;
  std::string command_target;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  OperationControlAcceptedDTO accepted{RequestId("pending"),
                                       RequestReplayStatusDTO::kAcceptedNew,
                                       OperationId("pending"),
                                       OperationRevision(0U),
                                       control,
                                       OperationRevision(0U),
                                       0};
  const bool settles_operation = control == OperationControlKind::kReset ||
                                 control == OperationControlKind::kAcknowledgeCompleted;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                                     [&](const ControlReplayEntry& entry) {
                                       return entry.request.request_context.request_id ==
                                              request.request_context.request_id;
                                     });
    if (replay != control_replay_ledger_.end()) {
      if (replay->request != request || replay->accepted.control != control) {
        return Result<OperationControlAcceptedDTO>::Err(error(
            "request_id_conflict", "operation control request ID was reused with different intent",
            "request_id_conflict"));
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<OperationControlAcceptedDTO>::Ok(std::move(accepted));
    }
    if (std::any_of(replay_ledger_.begin(), replay_ledger_.end(), [&](const ReplayEntry& entry) {
          return entry.intent.request_context.request_id == request.request_context.request_id;
        })) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict", "operation control request ID was already used by Execute",
                "request_id_conflict"));
    }
    if (std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.intent.request_context.request_id ==
                             request.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             request.request_context.request_id;
                    })) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict",
                "operation control request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    if (replay_ledger_.size() + control_replay_ledger_.size() + stop_replay_ledger_.size() +
            runtime_scale_replay_ledger_.size() >=
        config_.request_replay_capacity) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_ledger_full", "operation replay ledger reached its fixed bound",
                "request_ledger_full"));
    }
    if (active_operation_ == nullptr ||
        active_operation_->accepted.operation_id != request.operation_id) {
      return reject<OperationControlAcceptedDTO>("operation control names a non-resident operation",
                                                 "operation_not_resident",
                                                 ErrorCode::kMissingReference);
    }
    const auto current_revision = active_operation_->state.operation_revision;
    if (current_revision != request.expected_operation_revision) {
      return reject<OperationControlAcceptedDTO>("operation control expected revision is stale",
                                                 "operation_revision_stale");
    }
    if (!control_admissible(control, active_operation_->state.state)) {
      return reject<OperationControlAcceptedDTO>(
          std::string(control_name(control)) + " is invalid for the current operation state",
          "operation_control_state_invalid", ErrorCode::kInvalidStateTransition);
    }
    if (active_operation_->pending_control.has_value()) {
      return reject<OperationControlAcceptedDTO>(
          "another operation control is already being admitted", "operation_control_busy");
    }
    engine = active_operation_->engine;
    subordinate_id = active_operation_->subordinate_id;
    subordinate_generation = active_operation_->subordinate_generation;
    command_target = active_operation_->binding.value().command_target_id.value();
    payload_kind = active_operation_->payload_kind;
    if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
        (control == OperationControlKind::kPause || control == OperationControlKind::kResume)) {
      return reject<OperationControlAcceptedDTO>(
          "the qualified I6 command participant has no pause or resume realization",
          control == OperationControlKind::kPause ? "command_pause_unsupported"
                                                  : "command_resume_unsupported",
          ErrorCode::kUnsupportedCapability);
    }
    if (current_revision.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<OperationControlAcceptedDTO>("operation revision space is exhausted",
                                                 "operation_revision_exhausted",
                                                 ErrorCode::kInternalError);
    }
    const OperationRevision next_revision(current_revision.value() + 1U);
    const auto accepted_at_ns = clock_.now();
    accepted = OperationControlAcceptedDTO{request.request_context.request_id,
                                           RequestReplayStatusDTO::kAcceptedNew,
                                           request.operation_id,
                                           current_revision,
                                           control,
                                           next_revision,
                                           accepted_at_ns};
    auto accepted_state = active_operation_->state;
    accepted_state.operation_revision = next_revision;
    accepted_state.updated_at_ns = accepted_at_ns;
    auto durable_acceptance = journal_.RecordControlAccepted(config_.request_owner_epoch, request,
                                                             accepted, accepted_state);
    if (!durable_acceptance.ok()) {
      return Result<OperationControlAcceptedDTO>::Err(durable_acceptance.error());
    }

    auto revised =
        settles_operation && active_operation_->slot_state != ResourceSlotState::kTerminal
            ? resource_slots_.transition_and_revise_owner(
                  active_operation_->slot_owner, active_operation_->resources,
                  ResourceSlotState::kTerminal, next_revision)
            : resource_slots_.revise_owner(active_operation_->slot_owner,
                                           active_operation_->resources, next_revision);
    control_replay_ledger_.push_back(ControlReplayEntry{request, accepted});
    if (!revised.ok()) {
      auto intervention = accepted_state;
      intervention.state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().terminal_error = revised.error();
      intervention.updated_at_ns = clock_.now();
      auto durable_failure =
          journal_.RecordOperationState(config_.request_owner_epoch, intervention, nullptr,
                                        "resource_revision_failed_after_control_acceptance");
      if (durable_failure.ok()) {
        active_operation_->state = std::move(intervention);
      }
      return Result<OperationControlAcceptedDTO>::Ok(accepted);
    }
    active_operation_->slot_owner = revised.value();
    if (settles_operation) {
      active_operation_->slot_state = ResourceSlotState::kTerminal;
    }
    active_operation_->state = std::move(accepted_state);
    if (active_operation_->runtime_scale_status.has_value()) {
      active_operation_->runtime_scale_status->operation_revision = next_revision;
      active_operation_->runtime_scale_status->updated_at_ns = accepted_at_ns;
    }
    active_operation_->pending_control = control;
  }

  bool accepted_by_subordinate = false;
  std::optional<ErrorDTO> subordinate_error;
  switch (control) {
    case OperationControlKind::kPause: {
      const auto result = engine->pause();
      accepted_by_subordinate = result.accepted;
      subordinate_error = result.error;
      break;
    }
    case OperationControlKind::kResume: {
      const auto result = engine->resume(std::nullopt);
      accepted_by_subordinate = result.accepted;
      subordinate_error = result.error;
      break;
    }
    case OperationControlKind::kReset: {
      if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
        const auto result =
            engine->reset_gripper(command_target, subordinate_id, subordinate_generation);
        accepted_by_subordinate = result.ok();
        if (!result.ok()) {
          subordinate_error = result.error();
        }
      } else {
        const auto result = engine->reset(std::nullopt);
        accepted_by_subordinate = result.accepted;
        subordinate_error = result.error;
      }
      break;
    }
    case OperationControlKind::kAcknowledgeCompleted:
      if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
        accepted_by_subordinate = true;
      } else {
        const auto result = engine->acknowledge_completed(subordinate_id, std::nullopt);
        accepted_by_subordinate = result.accepted;
        subordinate_error = result.error;
      }
      break;
  }
  if (!accepted_by_subordinate) {
    const auto failure = subordinate_error.value_or(
        error(std::string(to_string(ErrorCode::kExecutionRejected)),
              std::string("subordinate rejected accepted ") + control_name(control),
              "subordinate_control_rejected"));
    (void)apply_subordinate_state(engine, subordinate_id, OperationStateKind::kInterventionRequired,
                                  0.0, failure);
    std::lock_guard state_lock(state_mutex_);
    if (active_operation_ != nullptr &&
        active_operation_->accepted.operation_id == request.operation_id &&
        active_operation_->pending_control == control) {
      active_operation_->pending_control.reset();
    }
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }

  std::lock_guard state_lock(state_mutex_);
  if (active_operation_ == nullptr ||
      active_operation_->accepted.operation_id != request.operation_id) {
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }
  active_operation_->pending_control.reset();
  if (settles_operation) {
    auto release =
        resource_slots_.release(active_operation_->slot_owner, active_operation_->resources);
    if (!release.ok()) {
      auto intervention = active_operation_->state;
      intervention.state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().terminal_error = release.error();
      intervention.updated_at_ns = clock_.now();
      auto durable_failure =
          journal_.RecordOperationState(config_.request_owner_epoch, intervention,
                                        active_operation_->runtime_scale_status.has_value()
                                            ? &*active_operation_->runtime_scale_status
                                            : nullptr,
                                        "resource_release_failed_after_control_effect");
      if (durable_failure.ok()) {
        active_operation_->state = std::move(intervention);
      }
      return Result<OperationControlAcceptedDTO>::Ok(accepted);
    }
    historical_operations_.push_back(active_operation_->state);
    active_operation_.reset();
  }
  return Result<OperationControlAcceptedDTO>::Ok(accepted);
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::stop(const StopIntentDTO& intent) {
  std::lock_guard control_lock(control_mutex_);
  if (intent.request_context.request_id.empty() ||
      intent.request_context.request_owner_epoch.empty() ||
      intent.request_context.source_label.empty() || intent.operations.size() != 1U ||
      intent.operations.front().operation_id.empty() ||
      !intent.operations.front().expected_operation_revision.nonzero() ||
      (intent.reason.has_value() && intent.reason->empty()) ||
      !intent.attribution.locally_valid()) {
    return reject<StopAcceptedDTO>("ordinary Stop intent is malformed", "stop_intent_invalid",
                                   ErrorCode::kInvalidRequest);
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<StopAcceptedDTO>::Err(error("stale_request_owner_epoch",
                                              "Stop request names a stale Kernel owner epoch",
                                              "stale_request_owner_epoch"));
  }

  IResolvedParticipantExecutionEngine* engine = nullptr;
  std::uint64_t subordinate_id = 0U;
  std::uint64_t subordinate_generation = 0U;
  std::string command_target;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  StopAcceptedDTO accepted{intent.request_context.request_id,
                           RequestReplayStatusDTO::kAcceptedNew,
                           StopId("pending"),
                           StopGeneration(0U),
                           {},
                           OperationRevision(0U),
                           0};
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(
        stop_replay_ledger_.begin(), stop_replay_ledger_.end(), [&](const StopReplayEntry& entry) {
          return entry.intent.request_context.request_id == intent.request_context.request_id;
        });
    if (replay != stop_replay_ledger_.end()) {
      if (replay->intent != intent) {
        return Result<StopAcceptedDTO>::Err(
            error("request_id_conflict", "Stop request ID was reused with different intent",
                  "request_id_conflict"));
      }
      auto replayed = replay->accepted;
      replayed.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<StopAcceptedDTO>::Ok(std::move(replayed));
    }
    const auto request_id = intent.request_context.request_id;
    const bool used_elsewhere =
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& entry) {
                      return entry.intent.request_context.request_id == request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& entry) {
                      return entry.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id == request_id;
                    });
    if (used_elsewhere) {
      return Result<StopAcceptedDTO>::Err(
          error("request_id_conflict", "Stop request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    if (replay_ledger_.size() + control_replay_ledger_.size() + stop_replay_ledger_.size() +
            runtime_scale_replay_ledger_.size() >=
        config_.request_replay_capacity) {
      return Result<StopAcceptedDTO>::Err(error("request_ledger_full",
                                                "operation replay ledger reached its fixed bound",
                                                "request_ledger_full"));
    }

    const auto& target = intent.operations.front();
    if (active_operation_ == nullptr ||
        active_operation_->accepted.operation_id != target.operation_id) {
      return reject<StopAcceptedDTO>("Stop names a non-resident operation",
                                     "operation_not_resident", ErrorCode::kMissingReference);
    }
    if (active_operation_->state.operation_revision != target.expected_operation_revision) {
      return reject<StopAcceptedDTO>("Stop expected operation revision is stale",
                                     "operation_revision_stale");
    }
    const StopGeneration current_generation =
        active_operation_->state.active_stop_generation.value_or(StopGeneration(0U));
    if (current_generation != target.expected_stop_generation) {
      return reject<StopAcceptedDTO>("Stop generation is stale", "stop_generation_stale");
    }

    if (active_operation_->state.state == OperationStateKind::kStopping &&
        active_operation_->state.active_stop_id.has_value() &&
        active_operation_->state.active_stop_generation.has_value()) {
      accepted = StopAcceptedDTO{intent.request_context.request_id,
                                 RequestReplayStatusDTO::kAcceptedNew,
                                 *active_operation_->state.active_stop_id,
                                 *active_operation_->state.active_stop_generation,
                                 {target.operation_id},
                                 active_operation_->state.operation_revision,
                                 clock_.now()};
      auto durable_join = journal_.RecordStopAccepted(config_.request_owner_epoch, intent, accepted,
                                                      active_operation_->state);
      if (!durable_join.ok()) {
        return Result<StopAcceptedDTO>::Err(durable_join.error());
      }
      stop_replay_ledger_.push_back(StopReplayEntry{intent, accepted});
      return Result<StopAcceptedDTO>::Ok(accepted);
    }
    if (terminal(active_operation_->state.state) ||
        active_operation_->state.state == OperationStateKind::kStopping) {
      return reject<StopAcceptedDTO>("Stop is invalid for the current operation state",
                                     "stop_state_invalid", ErrorCode::kInvalidStateTransition);
    }
    if (active_operation_->pending_control.has_value()) {
      return reject<StopAcceptedDTO>("another operation control is already being admitted",
                                     "operation_control_busy");
    }
    if (next_stop_id_ == 0U || next_stop_id_ == std::numeric_limits<std::uint64_t>::max() ||
        active_operation_->state.operation_revision.value() ==
            std::numeric_limits<std::uint64_t>::max() ||
        current_generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<StopAcceptedDTO>("Stop identity space is exhausted", "stop_identity_exhausted",
                                     ErrorCode::kInternalError);
    }

    const StopId stop_id("stop:" + config_.request_owner_epoch.value() + ":" +
                         std::to_string(next_stop_id_));
    const StopGeneration next_generation(current_generation.value() + 1U);
    const OperationRevision next_revision(active_operation_->state.operation_revision.value() + 1U);
    const auto accepted_at_ns = clock_.now();
    auto stopping_state = active_operation_->state;
    stopping_state.operation_revision = next_revision;
    stopping_state.state = OperationStateKind::kStopping;
    stopping_state.participants.front().state = OperationStateKind::kStopping;
    stopping_state.active_stop_id = stop_id;
    stopping_state.active_stop_generation = next_generation;
    stopping_state.updated_at_ns = accepted_at_ns;
    accepted = StopAcceptedDTO{intent.request_context.request_id,
                               RequestReplayStatusDTO::kAcceptedNew,
                               stop_id,
                               next_generation,
                               {target.operation_id},
                               next_revision,
                               accepted_at_ns};
    auto durable_acceptance =
        journal_.RecordStopAccepted(config_.request_owner_epoch, intent, accepted, stopping_state);
    if (!durable_acceptance.ok()) {
      return Result<StopAcceptedDTO>::Err(durable_acceptance.error());
    }
    auto revised = resource_slots_.transition_and_revise_owner(
        active_operation_->slot_owner, active_operation_->resources, ResourceSlotState::kStopping,
        next_revision);
    if (!revised.ok()) {
      stop_replay_ledger_.push_back(StopReplayEntry{intent, accepted});
      auto intervention = stopping_state;
      intervention.state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().terminal_error = revised.error();
      intervention.updated_at_ns = clock_.now();
      auto durable_failure =
          journal_.RecordOperationState(config_.request_owner_epoch, intervention, nullptr,
                                        "resource_revision_failed_after_stop_acceptance");
      if (durable_failure.ok()) {
        active_operation_->state = std::move(intervention);
      }
      return Result<StopAcceptedDTO>::Ok(accepted);
    }
    active_operation_->slot_owner = revised.value();
    active_operation_->slot_state = ResourceSlotState::kStopping;
    active_operation_->state = std::move(stopping_state);
    if (active_operation_->runtime_scale_status.has_value()) {
      active_operation_->runtime_scale_status->operation_revision = next_revision;
      active_operation_->runtime_scale_status->updated_at_ns = accepted_at_ns;
    }
    stop_replay_ledger_.push_back(StopReplayEntry{intent, accepted});
    ++next_stop_id_;
    engine = active_operation_->engine;
    subordinate_id = active_operation_->subordinate_id;
    subordinate_generation = active_operation_->subordinate_generation;
    command_target = active_operation_->binding.value().command_target_id.value();
    payload_kind = active_operation_->payload_kind;
  }

  if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
    const auto subordinate =
        engine->stop_gripper(command_target, subordinate_id, subordinate_generation);
    if (!subordinate.ok()) {
      (void)apply_subordinate_state(engine, subordinate_id,
                                    OperationStateKind::kInterventionRequired, 0.0,
                                    subordinate.error());
      return Result<StopAcceptedDTO>::Ok(std::move(accepted));
    }
    {
      std::lock_guard state_lock(state_mutex_);
      if (active_operation_ != nullptr &&
          active_operation_->accepted.operation_id == intent.operations.front().operation_id) {
        active_operation_->subordinate_generation = subordinate.value();
      }
    }
    (void)apply_subordinate_state(engine, subordinate_id, OperationStateKind::kStopped, 0.0,
                                  std::nullopt);
    return Result<StopAcceptedDTO>::Ok(std::move(accepted));
  }

  const auto subordinate = engine->stop();
  if (!subordinate.accepted || subordinate.error.has_value()) {
    const auto failure = subordinate.error.value_or(
        error(std::string(to_string(ErrorCode::kExecutionRejected)),
              "subordinate rejected an accepted scoped Stop", "subordinate_stop_rejected"));
    (void)apply_subordinate_state(engine, subordinate_id, OperationStateKind::kInterventionRequired,
                                  0.0, failure);
  }
  return Result<StopAcceptedDTO>::Ok(std::move(accepted));
}

Result<RuntimeScaleAcceptedDTO> ResourceAddressedExecutionKernel::set_runtime_scale(
    const RuntimeScaleRequestDTO& request) {
  std::lock_guard control_lock(control_mutex_);
  if (request.request_context.request_id.empty() ||
      request.request_context.request_owner_epoch.empty() ||
      request.request_context.source_label.empty() || request.operation_id.empty() ||
      !request.expected_operation_revision.nonzero() || !std::isfinite(request.scale) ||
      request.scale < 0.0 || request.scale > 1.0 || !request.attribution.locally_valid()) {
    return reject<RuntimeScaleAcceptedDTO>("runtime-scale request is malformed",
                                           "runtime_scale_request_invalid",
                                           ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<RuntimeScaleAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "runtime-scale request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  IResolvedParticipantExecutionEngine* engine = nullptr;
  std::optional<ResolvedRuntimeScaleCommandDTO> resolved_command;
  RuntimeScaleAcceptedDTO accepted{request.request_context.request_id,
                                   RequestReplayStatusDTO::kAcceptedNew,
                                   request.operation_id,
                                   request.expected_operation_revision,
                                   request.scale,
                                   RuntimeScaleGeneration(0U),
                                   0};
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(
        runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
        [&](const RuntimeScaleReplayEntry& entry) {
          return entry.request.request_context.request_id == request.request_context.request_id;
        });
    if (replay != runtime_scale_replay_ledger_.end()) {
      if (replay->request != request) {
        return Result<RuntimeScaleAcceptedDTO>::Err(error(
            "request_id_conflict", "runtime-scale request ID was reused with different intent",
            "request_id_conflict"));
      }
      auto replayed = replay->accepted;
      replayed.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<RuntimeScaleAcceptedDTO>::Ok(std::move(replayed));
    }
    const auto request_id = request.request_context.request_id;
    const bool used_elsewhere =
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& entry) {
                      return entry.intent.request_context.request_id == request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& entry) {
                      return entry.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.intent.request_context.request_id == request_id;
                    });
    if (used_elsewhere) {
      return Result<RuntimeScaleAcceptedDTO>::Err(
          error("request_id_conflict",
                "runtime-scale request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    if (replay_ledger_.size() + control_replay_ledger_.size() + stop_replay_ledger_.size() +
            runtime_scale_replay_ledger_.size() >=
        config_.request_replay_capacity) {
      return Result<RuntimeScaleAcceptedDTO>::Err(
          error("request_ledger_full", "operation replay ledger reached its fixed bound",
                "request_ledger_full"));
    }
    if (active_operation_ == nullptr ||
        active_operation_->accepted.operation_id != request.operation_id) {
      return reject<RuntimeScaleAcceptedDTO>("runtime scale names a non-resident operation",
                                             "operation_not_resident",
                                             ErrorCode::kMissingReference);
    }
    if (active_operation_->state.operation_revision != request.expected_operation_revision) {
      return reject<RuntimeScaleAcceptedDTO>("runtime scale expected operation revision is stale",
                                             "operation_revision_stale");
    }
    if (active_operation_->payload_kind != OperationParticipantPayloadKind::kMotion ||
        !active_operation_->binding.value().control_path.has_value() ||
        !active_operation_->binding.value().control_path->runtime_scale_qualification.has_value() ||
        !active_operation_->runtime_scale_status.has_value()) {
      return reject<RuntimeScaleAcceptedDTO>(
          "the exact operation binding has no qualified runtime scaling",
          "runtime_scale_unsupported", ErrorCode::kUnsupportedCapability);
    }
    const auto current_generation =
        active_operation_->runtime_scale_status->runtime_scale_generation;
    if (current_generation != request.expected_runtime_scale_generation) {
      return reject<RuntimeScaleAcceptedDTO>("runtime-scale generation is stale",
                                             "runtime_scale_generation_stale");
    }
    const auto state = active_operation_->state.state;
    if (state != OperationStateKind::kPlanning && state != OperationStateKind::kValidating &&
        state != OperationStateKind::kPrepared && state != OperationStateKind::kDispatching &&
        state != OperationStateKind::kExecuting && state != OperationStateKind::kPaused) {
      return reject<RuntimeScaleAcceptedDTO>(
          "runtime scale is invalid for the current operation state", "runtime_scale_state_invalid",
          ErrorCode::kInvalidStateTransition);
    }
    if (current_generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<RuntimeScaleAcceptedDTO>("runtime-scale generation space is exhausted",
                                             "runtime_scale_generation_exhausted",
                                             ErrorCode::kInternalError);
    }

    const RuntimeScaleGeneration next_generation(current_generation.value() + 1U);
    accepted = RuntimeScaleAcceptedDTO{request.request_context.request_id,
                                       RequestReplayStatusDTO::kAcceptedNew,
                                       request.operation_id,
                                       request.expected_operation_revision,
                                       request.scale,
                                       next_generation,
                                       clock_.now()};
    auto accepted_status = *active_operation_->runtime_scale_status;
    accepted_status.runtime_scale_generation = next_generation;
    accepted_status.requested_scale = request.scale;
    accepted_status.state = RuntimeScaleApplicationState::kPending;
    accepted_status.updated_at_ns = accepted.accepted_at_ns;
    auto& participant = accepted_status.participants.front();
    participant.accepted_generation = next_generation;
    participant.commanded_scale = request.scale;
    participant.state = RuntimeScaleApplicationState::kPending;
    participant.error.reset();
    auto accepted_state = active_operation_->state;
    accepted_state.runtime_scale_generation = next_generation;
    accepted_state.updated_at_ns = accepted.accepted_at_ns;
    auto durable_acceptance = journal_.RecordRuntimeScaleAccepted(
        config_.request_owner_epoch, request, accepted, accepted_state, accepted_status);
    if (!durable_acceptance.ok()) {
      return Result<RuntimeScaleAcceptedDTO>::Err(durable_acceptance.error());
    }
    active_operation_->state = std::move(accepted_state);
    active_operation_->runtime_scale_status = std::move(accepted_status);
    runtime_scale_replay_ledger_.push_back(RuntimeScaleReplayEntry{request, accepted});
    engine = active_operation_->engine;
    const auto& binding = active_operation_->binding.value();
    const auto& control = *binding.control_path;
    const auto& qualification = *control.runtime_scale_qualification;
    const auto& operation_participant = active_operation_->state.participants.front();
    resolved_command =
        ResolvedRuntimeScaleCommandDTO{request.operation_id,
                                       request.expected_operation_revision,
                                       operation_participant.participant_id,
                                       binding.controllable_component_id,
                                       binding.command_target_id,
                                       binding.capability_id,
                                       control.control_domain_id,
                                       control.control_domain_revision,
                                       control.control_binding_id,
                                       control.control_binding_revision,
                                       operation_participant.controller_goal_id,
                                       operation_participant.controller_goal_generation,
                                       control.controller_topology_id,
                                       control.topology_revision,
                                       control.control_route_id,
                                       qualification.scaling_domain_id,
                                       qualification.coordinated_barrier_id,
                                       active_operation_->resources,
                                       control.active_control_configuration_id,
                                       control.active_control_configuration_generation,
                                       current_generation,
                                       next_generation,
                                       request.scale};
  }

  const auto commanded = engine->set_runtime_scale(*resolved_command);
  if (!commanded.ok()) {
    std::lock_guard state_lock(state_mutex_);
    if (active_operation_ != nullptr &&
        active_operation_->accepted.operation_id == request.operation_id &&
        active_operation_->runtime_scale_status.has_value() &&
        active_operation_->runtime_scale_status->runtime_scale_generation ==
            accepted.runtime_scale_generation) {
      auto status = *active_operation_->runtime_scale_status;
      status.state = RuntimeScaleApplicationState::kFailed;
      status.updated_at_ns = clock_.now();
      status.participants.front().state = RuntimeScaleApplicationState::kFailed;
      status.participants.front().error = commanded.error();
      auto state = active_operation_->state;
      auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                   "runtime_scale_effect_rejected");
      if (durable.ok()) {
        active_operation_->state = std::move(state);
        active_operation_->runtime_scale_status = std::move(status);
      }
    }
  } else {
    const auto& exact = commanded.value();
    const bool acceptance_matches =
        exact.operation_id == resolved_command->operation_id &&
        exact.operation_revision == resolved_command->operation_revision &&
        exact.participant_id == resolved_command->participant_id &&
        exact.controllable_component_id == resolved_command->controllable_component_id &&
        exact.command_target_id == resolved_command->command_target_id &&
        exact.capability_id == resolved_command->capability_id &&
        exact.control_domain_id == resolved_command->control_domain_id &&
        exact.control_domain_revision == resolved_command->control_domain_revision &&
        exact.control_binding_id == resolved_command->control_binding_id &&
        exact.control_binding_revision == resolved_command->control_binding_revision &&
        exact.controller_goal_id == resolved_command->controller_goal_id &&
        exact.controller_goal_generation == resolved_command->controller_goal_generation &&
        exact.controller_topology_id == resolved_command->controller_topology_id &&
        exact.topology_revision == resolved_command->topology_revision &&
        exact.control_route_id == resolved_command->control_route_id &&
        exact.scaling_domain_id == resolved_command->scaling_domain_id &&
        exact.coordinated_barrier_id == resolved_command->coordinated_barrier_id &&
        exact.resources == resolved_command->resources &&
        exact.active_control_configuration_id ==
            resolved_command->active_control_configuration_id &&
        exact.active_control_configuration_generation ==
            resolved_command->active_control_configuration_generation &&
        exact.accepted_generation == resolved_command->requested_runtime_scale_generation &&
        std::abs(exact.commanded_scale - resolved_command->requested_scale) <= 1e-12;
    if (!acceptance_matches) {
      std::lock_guard state_lock(state_mutex_);
      if (active_operation_ != nullptr &&
          active_operation_->accepted.operation_id == request.operation_id &&
          active_operation_->runtime_scale_status.has_value()) {
        auto status = *active_operation_->runtime_scale_status;
        status.state = RuntimeScaleApplicationState::kFailed;
        status.updated_at_ns = clock_.now();
        status.participants.front().state = RuntimeScaleApplicationState::kFailed;
        status.participants.front().error =
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  "participant engine returned mismatched runtime-scale acceptance",
                  "runtime_scale_acceptance_mismatch");
        auto state = active_operation_->state;
        auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                     "runtime_scale_acceptance_mismatch");
        if (durable.ok()) {
          active_operation_->state = std::move(state);
          active_operation_->runtime_scale_status = std::move(status);
        }
      }
    } else {
      const auto observed = engine->runtime_scale_status(resolved_command->operation_id,
                                                         resolved_command->control_binding_id);
      if (!observed.ok()) {
        std::lock_guard state_lock(state_mutex_);
        if (active_operation_ != nullptr &&
            active_operation_->accepted.operation_id == request.operation_id &&
            active_operation_->runtime_scale_status.has_value()) {
          auto status = *active_operation_->runtime_scale_status;
          status.state = RuntimeScaleApplicationState::kFailed;
          status.updated_at_ns = clock_.now();
          status.participants.front().state = RuntimeScaleApplicationState::kFailed;
          status.participants.front().error = observed.error();
          auto state = active_operation_->state;
          auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                       "runtime_scale_status_unavailable");
          if (durable.ok()) {
            active_operation_->state = std::move(state);
            active_operation_->runtime_scale_status = std::move(status);
          }
        }
      } else {
        (void)apply_runtime_scale_status(engine, observed.value());
      }
    }
  }
  return Result<RuntimeScaleAcceptedDTO>::Ok(std::move(accepted));
}

Result<void> ResourceAddressedExecutionKernel::apply_runtime_scale_status(
    IResolvedParticipantExecutionEngine* engine,
    const ResolvedRuntimeScaleStatusDTO& resolved_status) {
  std::lock_guard state_lock(state_mutex_);
  if (active_operation_ == nullptr || active_operation_->engine != engine ||
      !active_operation_->runtime_scale_status.has_value()) {
    return Result<void>::Ok();
  }
  auto status = *active_operation_->runtime_scale_status;
  auto& participant = status.participants.front();
  const auto& binding = active_operation_->binding.value();
  const auto& control = *binding.control_path;
  const auto& qualification = *control.runtime_scale_qualification;
  const bool exact_target_matches =
      resolved_status.operation_id == status.operation_id &&
      resolved_status.operation_revision == status.operation_revision &&
      resolved_status.participant_id == participant.participant_id &&
      resolved_status.controllable_component_id == participant.controllable_component_id &&
      resolved_status.command_target_id == participant.command_target_id &&
      resolved_status.capability_id == participant.capability_id &&
      resolved_status.control_domain_id == participant.control_domain_id &&
      resolved_status.control_domain_revision == participant.control_domain_revision &&
      resolved_status.control_binding_id == participant.control_binding_id &&
      resolved_status.control_binding_revision == participant.control_binding_revision &&
      resolved_status.controller_goal_id == participant.controller_goal_id &&
      resolved_status.controller_goal_generation == participant.controller_goal_generation &&
      resolved_status.controller_topology_id == participant.controller_topology_id &&
      resolved_status.topology_revision == participant.topology_revision &&
      resolved_status.control_route_id == participant.control_route_id &&
      resolved_status.scaling_domain_id == participant.scaling_domain_id &&
      resolved_status.coordinated_barrier_id == participant.coordinated_barrier_id &&
      resolved_status.resources == participant.resources &&
      resolved_status.active_control_configuration_id ==
          participant.active_control_configuration_id &&
      resolved_status.active_control_configuration_generation ==
          participant.active_control_configuration_generation &&
      resolved_status.acknowledgement_source_epoch == control.provider_runtime_epoch;
  if (!exact_target_matches) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error =
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              "participant engine returned runtime-scale status for another binding",
              "runtime_scale_status_identity_mismatch");
    status.updated_at_ns = clock_.now();
    auto state = active_operation_->state;
    auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                 "runtime_scale_status_identity_mismatch");
    if (!durable.ok()) {
      return Result<void>::Err(durable.error());
    }
    active_operation_->state = std::move(state);
    active_operation_->runtime_scale_status = std::move(status);
    return Result<void>::Ok();
  }
  if (resolved_status.accepted_generation < status.runtime_scale_generation) {
    return Result<void>::Ok();
  }
  if (resolved_status.accepted_generation != status.runtime_scale_generation) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                              "participant engine advanced an unrequested runtime-scale generation",
                              "runtime_scale_status_generation_mismatch");
    status.updated_at_ns = clock_.now();
    auto state = active_operation_->state;
    auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                 "runtime_scale_status_generation_mismatch");
    if (!durable.ok()) {
      return Result<void>::Err(durable.error());
    }
    active_operation_->state = std::move(state);
    active_operation_->runtime_scale_status = std::move(status);
    return Result<void>::Ok();
  }
  participant.commanded_scale = resolved_status.commanded_scale;
  participant.applied_scale = resolved_status.applied_scale;
  participant.virtual_trajectory_time_s = resolved_status.virtual_trajectory_time_s;
  participant.acknowledgement_source_epoch = resolved_status.acknowledgement_source_epoch;
  participant.acknowledgement_sequence = resolved_status.acknowledgement_sequence;
  participant.acknowledgement_timestamp_ns = resolved_status.acknowledgement_timestamp_ns;
  participant.supported = resolved_status.state != RuntimeScaleApplicationState::kRejected;
  participant.error = resolved_status.error;
  status.effective_scale = resolved_status.applied_scale;
  status.updated_at_ns = clock_.now();

  const bool stale = resolved_status.acknowledgement_timestamp_ns > 0 &&
                     status.updated_at_ns > resolved_status.acknowledgement_timestamp_ns &&
                     status.updated_at_ns - resolved_status.acknowledgement_timestamp_ns >
                         qualification.freshness_budget_ns;
  if (resolved_status.error.has_value() || stale) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    if (stale && !participant.error.has_value()) {
      participant.error = error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                "runtime-scale applied-state acknowledgement is stale",
                                "runtime_scale_acknowledgement_stale");
    }
  } else {
    participant.state = resolved_status.state;
    status.state = participant.state;
  }
  auto state = active_operation_->state;
  auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                               "runtime_scale_status_observed");
  if (!durable.ok()) {
    return Result<void>::Err(durable.error());
  }
  active_operation_->state = std::move(state);
  active_operation_->runtime_scale_status = std::move(status);
  return Result<void>::Ok();
}

Result<RuntimeScaleStatusDTO> ResourceAddressedExecutionKernel::get_runtime_scale(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<RuntimeScaleStatusDTO>("operation identity is empty", "operation_id_empty",
                                         ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  if (active_operation_ == nullptr || active_operation_->accepted.operation_id != operation_id) {
    return reject<RuntimeScaleStatusDTO>("operation is not resident", "operation_not_resident",
                                         ErrorCode::kMissingReference);
  }
  if (!active_operation_->runtime_scale_status.has_value()) {
    return reject<RuntimeScaleStatusDTO>("operation binding has no qualified runtime scaling",
                                         "runtime_scale_unsupported",
                                         ErrorCode::kUnsupportedCapability);
  }
  return Result<RuntimeScaleStatusDTO>::Ok(*active_operation_->runtime_scale_status);
}

Result<OperationStateDTO> ResourceAddressedExecutionKernel::get_operation_state(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<OperationStateDTO>("operation identity is empty", "operation_id_empty",
                                     ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  if (active_operation_ != nullptr && active_operation_->accepted.operation_id == operation_id) {
    return Result<OperationStateDTO>::Ok(active_operation_->state);
  }
  const auto historical = std::find_if(
      historical_operations_.begin(), historical_operations_.end(),
      [&](const OperationStateDTO& state) { return state.operation_id == operation_id; });
  if (historical != historical_operations_.end()) {
    return Result<OperationStateDTO>::Ok(*historical);
  }
  return reject<OperationStateDTO>("operation is not resident or retained",
                                   "operation_not_resident", ErrorCode::kMissingReference);
}

Result<void> ResourceAddressedExecutionKernel::apply_subordinate_state(
    IResolvedParticipantExecutionEngine* engine, std::uint64_t subordinate_id,
    OperationStateKind state, double progress, std::optional<ErrorDTO> terminal_error) {
  std::lock_guard state_lock(state_mutex_);
  if (active_operation_ == nullptr || active_operation_->engine != engine) {
    return Result<void>::Ok();
  }
  if (state == OperationStateKind::kPrepared &&
      (active_operation_->pending_control == OperationControlKind::kReset ||
       active_operation_->pending_control == OperationControlKind::kAcknowledgeCompleted)) {
    // The subordinate Idle transition confirms the settling effect. The
    // durable owner retains the terminal state named by the accepted control;
    // it does not republish that private FSM reset as a new active operation.
    return Result<void>::Ok();
  }
  if (active_operation_->payload_kind == OperationParticipantPayloadKind::kMotion) {
    if (subordinate_id == 0U) {
      return reject<void>("subordinate execution identity is empty",
                          "subordinate_execution_id_empty", ErrorCode::kInvariantViolation);
    }
    if (active_operation_->subordinate_id == 0U) {
      active_operation_->subordinate_id = subordinate_id;
    } else if (active_operation_->subordinate_id != subordinate_id) {
      return Result<void>::Ok();
    }
  }

  auto desired_slot = slot_state_for(state);
  if (state == OperationStateKind::kStopping &&
      active_operation_->pending_control == OperationControlKind::kPause) {
    // Pause retains the reservation as dispatched while the subordinate
    // mechanism reaches its controlled Paused state. ResourceSlot::kStopping
    // is reserved for terminal containment/Reset paths.
    desired_slot.reset();
  }
  const auto desired_slot_value = desired_slot.value_or(active_operation_->slot_state);
  const bool slot_changed =
      desired_slot.has_value() && active_operation_->slot_state != desired_slot_value;
  const auto& participant = active_operation_->state.participants.front();
  const double bounded_progress = std::clamp(progress, 0.0, 1.0);
  const bool failure_terminal = state == OperationStateKind::kFailed ||
                                state == OperationStateKind::kStopped ||
                                state == OperationStateKind::kInterrupted ||
                                state == OperationStateKind::kInterventionRequired;
  if (failure_terminal) {
    const bool generic_subordinate_error =
        terminal_error.has_value() && terminal_error->details.contains("reason") &&
        terminal_error->details.at("reason") == "subordinate_terminal_error";
    if ((!terminal_error.has_value() || generic_subordinate_error) &&
        active_operation_->last_subordinate_error.has_value()) {
      terminal_error = active_operation_->last_subordinate_error;
    }
  }
  const bool same_terminal_error =
      participant.terminal_error.has_value() == terminal_error.has_value() &&
      (!terminal_error.has_value() ||
       (participant.terminal_error->code == terminal_error->code &&
        participant.terminal_error->message == terminal_error->message &&
        participant.terminal_error->details == terminal_error->details));
  if (!slot_changed && active_operation_->state.state == state && participant.state == state &&
      participant.progress == bounded_progress && (!terminal(state) || same_terminal_error)) {
    return Result<void>::Ok();
  }

  auto observed_state = active_operation_->state;
  auto& observed_participant = observed_state.participants.front();
  observed_state.state = state;
  observed_participant.state = state;
  observed_participant.progress = bounded_progress;
  if (terminal(state)) {
    observed_participant.terminal_error = std::move(terminal_error);
  }
  observed_state.updated_at_ns = clock_.now();
  auto durable_state =
      journal_.RecordOperationState(config_.request_owner_epoch, observed_state,
                                    active_operation_->runtime_scale_status.has_value()
                                        ? &*active_operation_->runtime_scale_status
                                        : nullptr,
                                    "subordinate_state_observed");
  if (!durable_state.ok()) {
    return Result<void>::Err(durable_state.error());
  }
  if (slot_changed) {
    auto transition = resource_slots_.transition(active_operation_->slot_owner,
                                                 active_operation_->resources, desired_slot_value);
    if (!transition.ok()) {
      auto intervention = observed_state;
      intervention.state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().terminal_error = transition.error();
      intervention.updated_at_ns = clock_.now();
      auto durable_failure =
          journal_.RecordOperationState(config_.request_owner_epoch, intervention,
                                        active_operation_->runtime_scale_status.has_value()
                                            ? &*active_operation_->runtime_scale_status
                                            : nullptr,
                                        "resource_transition_failed_after_state_observation");
      if (durable_failure.ok()) {
        active_operation_->state = std::move(intervention);
      }
      return transition;
    }
    active_operation_->slot_state = desired_slot_value;
  }
  active_operation_->state = std::move(observed_state);
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::on_engine_event(IResolvedParticipantExecutionEngine* engine,
                                                       const KernelEvent& event) {
  if (event.type == KernelEventType::kErrorRaised) {
    if (!event.error.has_value()) {
      return;
    }
    std::lock_guard state_lock(state_mutex_);
    if (active_operation_ != nullptr && active_operation_->engine == engine &&
        (active_operation_->subordinate_id == 0U ||
         active_operation_->subordinate_id == event.execution_id)) {
      active_operation_->last_subordinate_error = event.error;
    }
    return;
  }
  if (event.type != KernelEventType::kStateTransition &&
      event.type != KernelEventType::kExecuteStarted &&
      event.type != KernelEventType::kExecuteCompleted) {
    return;
  }
  auto state = event.type == KernelEventType::kExecuteStarted     ? OperationStateKind::kExecuting
               : event.type == KernelEventType::kExecuteCompleted ? OperationStateKind::kCompleted
                                                                  : operation_state(event.to_state);
  std::optional<ErrorDTO> terminal_error = event.error;
  if (!terminal_error.has_value() &&
      (state == OperationStateKind::kFailed || state == OperationStateKind::kStopped ||
       state == OperationStateKind::kInterventionRequired)) {
    const auto status = engine->get_status();
    terminal_error = status_error(status.last_error);
  }
  (void)apply_subordinate_state(engine, event.execution_id, state, engine->get_status().progress,
                                std::move(terminal_error));
}

Result<void> ResourceAddressedExecutionKernel::reconcile_subordinate_state() {
  std::lock_guard control_lock(control_mutex_);
  IResolvedParticipantExecutionEngine* engine = nullptr;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  std::uint64_t subordinate_id = 0U;
  std::uint64_t subordinate_generation = 0U;
  std::string command_target;
  OperationStateKind public_state{OperationStateKind::kPrepared};
  std::optional<OperationId> runtime_scale_operation_id;
  std::optional<ControlBindingId> runtime_scale_control_binding_id;
  {
    std::lock_guard state_lock(state_mutex_);
    if (active_operation_ == nullptr) {
      return Result<void>::Ok();
    }
    engine = active_operation_->engine;
    payload_kind = active_operation_->payload_kind;
    subordinate_id = active_operation_->subordinate_id;
    subordinate_generation = active_operation_->subordinate_generation;
    command_target = active_operation_->binding.value().command_target_id.value();
    public_state = active_operation_->state.state;
    if (active_operation_->runtime_scale_status.has_value()) {
      runtime_scale_operation_id = active_operation_->accepted.operation_id;
      runtime_scale_control_binding_id =
          active_operation_->binding.value().control_path->control_binding_id;
    }
  }

  if (payload_kind == OperationParticipantPayloadKind::kMotion) {
    const auto status = engine->get_status();
    std::optional<ErrorDTO> terminal_error;
    const auto state = operation_state(status.state);
    if (state == OperationStateKind::kFailed || state == OperationStateKind::kStopped ||
        state == OperationStateKind::kInterventionRequired) {
      terminal_error = status_error(status.last_error);
    }
    auto applied = apply_subordinate_state(engine, subordinate_id, state, status.progress,
                                           std::move(terminal_error));
    if (applied.ok() && runtime_scale_operation_id.has_value() &&
        runtime_scale_control_binding_id.has_value()) {
      const auto scale_status = engine->runtime_scale_status(*runtime_scale_operation_id,
                                                             *runtime_scale_control_binding_id);
      if (scale_status.ok()) {
        auto scale_applied = apply_runtime_scale_status(engine, scale_status.value());
        if (!scale_applied.ok()) {
          return scale_applied;
        }
      } else {
        std::lock_guard state_lock(state_mutex_);
        if (active_operation_ != nullptr &&
            active_operation_->accepted.operation_id == *runtime_scale_operation_id &&
            active_operation_->runtime_scale_status.has_value()) {
          auto public_status = *active_operation_->runtime_scale_status;
          public_status.state = RuntimeScaleApplicationState::kFailed;
          public_status.updated_at_ns = clock_.now();
          public_status.participants.front().state = RuntimeScaleApplicationState::kFailed;
          public_status.participants.front().error = scale_status.error();
          auto operation_state = active_operation_->state;
          auto durable =
              journal_.RecordOperationState(config_.request_owner_epoch, operation_state,
                                            &public_status, "runtime_scale_reconcile_unavailable");
          if (!durable.ok()) {
            return Result<void>::Err(durable.error());
          }
          active_operation_->state = std::move(operation_state);
          active_operation_->runtime_scale_status = std::move(public_status);
        }
      }
    }
    return applied;
  }

  const auto status = engine->get_gripper_status(command_target);
  switch (status.state) {
    case GripperCommandState::kIdle:
      if (public_state == OperationStateKind::kStopped && status.command_id == subordinate_id &&
          status.generation == subordinate_generation) {
        return Result<void>::Ok();
      }
      return apply_subordinate_state(
          engine, subordinate_id, OperationStateKind::kInterventionRequired, status.progress,
          error("invariant_violation",
                "accepted gripper command became idle without terminal evidence",
                "gripper_command_lost"));
    case GripperCommandState::kExecuting:
      return apply_subordinate_state(engine, subordinate_id, OperationStateKind::kExecuting,
                                     status.progress, std::nullopt);
    case GripperCommandState::kCompleted:
      return apply_subordinate_state(engine, subordinate_id, OperationStateKind::kCompleted,
                                     status.progress, std::nullopt);
    case GripperCommandState::kFailed:
      return apply_subordinate_state(
          engine, subordinate_id, OperationStateKind::kFailed, status.progress,
          status.error.value_or(error("execution_aborted", "gripper command failed",
                                      "gripper_command_failed_without_error")));
  }
  return reject<void>("gripper status is outside its closed enum", "gripper_status_unknown",
                      ErrorCode::kInvariantViolation);
}

std::vector<ResourceSlotSnapshotDTO> ResourceAddressedExecutionKernel::resource_snapshot() const {
  return resource_slots_.snapshot();
}

}  // namespace manyforge::core
