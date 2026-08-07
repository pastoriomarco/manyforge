#include <algorithm>
#include <cmath>
#include <manyforge_core/types/operation_intent.hpp>
#include <set>
#include <string>
#include <type_traits>

namespace manyforge::core {
namespace {

[[nodiscard]] Result<void> invalid(std::string message, std::string reason) {
  return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                    std::move(message),
                                    {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool finite_pose(const Pose3DTO& pose) noexcept {
  return std::isfinite(pose.position_m.x_m) && std::isfinite(pose.position_m.y_m) &&
         std::isfinite(pose.position_m.z_m) && std::isfinite(pose.orientation.x) &&
         std::isfinite(pose.orientation.y) && std::isfinite(pose.orientation.z) &&
         std::isfinite(pose.orientation.w);
}

[[nodiscard]] bool valid_reference(const ReferenceDTO& reference) noexcept {
  if (reference.type == ReferenceDTO::Type::kPose) {
    return reference.id.empty() && reference.pose.has_value() && finite_pose(*reference.pose);
  }
  return !reference.id.empty() && !reference.pose.has_value();
}

[[nodiscard]] bool valid_action_span(const ActionSpanDTO& span) noexcept {
  const auto bounded = [](const std::string& value) {
    return value.size() <= kMaxActionSpanFieldBytes;
  };
  if (span.action_span_id.empty() || !bounded(span.action_span_id) ||
      !bounded(span.program_run_id) || !bounded(span.node_instance_id) ||
      !bounded(span.node_attempt_id) || !bounded(span.parent_node_attempt_id)) {
    return false;
  }
  if (span.action_source == ActionSource::kProgramNode) {
    return !span.program_run_id.empty() && !span.node_instance_id.empty() &&
           !span.node_attempt_id.empty() && span.action_span_id == span.node_attempt_id;
  }
  return span.node_instance_id.empty() && span.node_attempt_id.empty();
}

[[nodiscard]] bool valid_motion(const MotionSpecDTO& motion) noexcept {
  return std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, JointGoalDTO>) {
          std::set<std::string, std::less<>> joint_names;
          return !value.joint_names.empty() &&
                 value.joint_names.size() == value.positions_rad.size() &&
                 std::all_of(value.joint_names.begin(), value.joint_names.end(),
                             [&](const std::string& joint_name) {
                               return !joint_name.empty() && joint_names.insert(joint_name).second;
                             }) &&
                 std::all_of(value.positions_rad.begin(), value.positions_rad.end(),
                             [](double item) { return std::isfinite(item); });
        } else if constexpr (std::is_same_v<Value, PoseGoalDTO>) {
          return finite_pose(value.goal);
        } else if constexpr (std::is_same_v<Value, NamedTargetDTO>) {
          return !value.name.empty();
        } else {
          return !value.waypoints.empty() &&
                 std::all_of(value.waypoints.begin(), value.waypoints.end(), finite_pose) &&
                 std::isfinite(value.max_translation_step_m) &&
                 value.max_translation_step_m > 0.0 && std::isfinite(value.max_rotation_step_rad) &&
                 value.max_rotation_step_rad > 0.0 && std::isfinite(value.max_linear_speed_mps) &&
                 value.max_linear_speed_mps > 0.0 && std::isfinite(value.max_angular_speed_rad_s) &&
                 value.max_angular_speed_rad_s > 0.0 &&
                 std::isfinite(value.max_linear_acceleration_mps2) &&
                 value.max_linear_acceleration_mps2 > 0.0 &&
                 std::isfinite(value.max_angular_acceleration_rad_s2) &&
                 value.max_angular_acceleration_rad_s2 > 0.0;
        }
      },
      motion);
}

[[nodiscard]] bool valid_limit(const JointDynamicsLimitDTO& limit) noexcept {
  const auto valid_optional = [](const std::optional<double>& value) {
    return !value.has_value() || (std::isfinite(*value) && *value > 0.0);
  };
  return valid_optional(limit.max_velocity_rad_s) &&
         valid_optional(limit.max_acceleration_rad_s2) && valid_optional(limit.max_jerk_rad_s3);
}

[[nodiscard]] Result<void> validate_motion_intent(const MotionParticipantIntentDTO& motion) {
  if (motion.move_id.empty() || motion.base_frame_role_id.empty() ||
      motion.tcp_frame_role_id.empty()) {
    return invalid("motion intent has an empty portable identity", "motion_identity_empty");
  }
  if (!std::isfinite(motion.tolerances.joint_position_tol_rad) ||
      motion.tolerances.joint_position_tol_rad < 0.0 ||
      !std::isfinite(motion.tolerances.translation_tol_m) ||
      motion.tolerances.translation_tol_m < 0.0 ||
      !std::isfinite(motion.tolerances.rotation_tol_rad) ||
      motion.tolerances.rotation_tol_rad < 0.0) {
    return invalid("motion tolerances must be finite and nonnegative", "motion_tolerance_invalid");
  }
  if (!std::isfinite(motion.speed_profile.velocity_scale) ||
      motion.speed_profile.velocity_scale <= 0.0 || motion.speed_profile.velocity_scale > 1.0 ||
      !std::isfinite(motion.speed_profile.acceleration_scale) ||
      motion.speed_profile.acceleration_scale <= 0.0 ||
      motion.speed_profile.acceleration_scale > 1.0 ||
      (motion.speed_profile.max_tcp_linear_speed_mps.has_value() &&
       (!std::isfinite(*motion.speed_profile.max_tcp_linear_speed_mps) ||
        *motion.speed_profile.max_tcp_linear_speed_mps <= 0.0))) {
    return invalid("motion speed profile is invalid", "motion_speed_profile_invalid");
  }
  if (!std::isfinite(motion.timeouts.planning_timeout_s) ||
      motion.timeouts.planning_timeout_s <= 0.0 ||
      !std::isfinite(motion.timeouts.execution_timeout_s) ||
      motion.timeouts.execution_timeout_s <= 0.0) {
    return invalid("motion timeouts must be finite and positive", "motion_timeout_invalid");
  }
  if (!valid_motion(motion.motion)) {
    return invalid("motion payload is malformed", "motion_payload_invalid");
  }
  if ((motion.target_reference.has_value() && !valid_reference(*motion.target_reference)) ||
      (motion.offset_reference.has_value() && !valid_reference(*motion.offset_reference))) {
    return invalid("motion reference is malformed", "motion_reference_invalid");
  }
  if (motion.target_mode == MotionTargetModeDTO::kAbsolute &&
      (motion.target_reference.has_value() || motion.offset_reference.has_value())) {
    return invalid("absolute motion cannot carry relative references",
                   "absolute_motion_has_reference");
  }
  for (const auto& [joint_name, limit] : motion.per_joint_dynamics_limits) {
    if (joint_name.empty() || !valid_limit(limit)) {
      return invalid("motion dynamics limit is malformed", "motion_dynamics_limit_invalid");
    }
  }
  return Result<void>::Ok();
}

[[nodiscard]] Result<void> validate_command_intent(const CapabilityCommandIntentDTO& command) {
  if (command.command_schema_id.empty() || command.command_schema_revision.empty() ||
      !std::isfinite(command.completion_timeout_s) || command.completion_timeout_s <= 0.0 ||
      command.caller_options.size() > kMaxOperationCommandOptions) {
    return invalid("capability command envelope is malformed", "capability_command_invalid");
  }
  std::set<CommandOptionId> option_ids;
  for (const auto& option : command.caller_options) {
    if (option.option_id.empty() || !std::isfinite(option.value_s) || option.value_s < 0.0 ||
        !option_ids.insert(option.option_id).second) {
      return invalid("capability command option is malformed or duplicated",
                     "capability_command_option_invalid");
    }
  }
  const auto& value = std::get<GripperPositionCommandValueDTO>(command.command_value);
  const auto position_kind = coordinate_kind(value.position);
  const double position = position_kind == CoordinateKind::kAngular
                              ? std::get<AngularPositionDTO>(value.position).value_rad
                              : std::get<LinearPositionDTO>(value.position).value_m;
  if (!std::isfinite(position)) {
    return invalid("gripper command position must be finite", "gripper_command_position_invalid");
  }
  if (value.max_effort.has_value()) {
    if (coordinate_kind(*value.max_effort) != position_kind) {
      return invalid("gripper command effort unit differs from position unit",
                     "gripper_command_effort_unit_mismatch");
    }
    const double effort = position_kind == CoordinateKind::kAngular
                              ? std::get<AngularEffortDTO>(*value.max_effort).value_nm
                              : std::get<LinearEffortDTO>(*value.max_effort).value_n;
    if (!std::isfinite(effort) || effort < 0.0) {
      return invalid("gripper command effort must be finite and nonnegative",
                     "gripper_command_effort_invalid");
    }
  }
  return Result<void>::Ok();
}

}  // namespace

Result<void> validate_operation_intent(const OperationIntentDTO& intent) {
  if (intent.request_context.request_id.empty() ||
      intent.request_context.request_owner_epoch.empty() ||
      intent.request_context.source_label.empty() || intent.program_id.empty() ||
      intent.program_revision.empty() || intent.program_binding_id.empty() ||
      intent.program_run_id.empty() || intent.expected_graph_revision.empty() ||
      intent.expected_retained_bundle_id.empty()) {
    return invalid("operation intent has an empty required identity", "operation_identity_empty");
  }
  if (!intent.attribution.locally_valid()) {
    return invalid("operation intent has invalid adapter attribution",
                   "operation_attribution_invalid");
  }
  if (intent.expected_scene_revision.has_value() && !intent.expected_scene_revision->valid()) {
    return invalid("operation expected scene revision is invalid",
                   "operation_scene_revision_invalid");
  }
  if (intent.participants.empty() || intent.participants.size() > kMaxOperationParticipants) {
    return invalid("operation participant count is outside the fixed bound",
                   "operation_participant_count_invalid");
  }
  for (const auto& participant : intent.participants) {
    if (participant.component_role_id.empty() || participant.target_role_id.empty() ||
        participant.required_capability_id.empty()) {
      return invalid("operation participant has an empty role or capability",
                     "operation_participant_identity_empty");
    }
    if (participant.action_span.has_value() && !valid_action_span(*participant.action_span)) {
      return invalid("operation action span is malformed", "operation_action_span_invalid");
    }
    const auto payload_result = std::visit(
        [](const auto& payload) -> Result<void> {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, MotionParticipantIntentDTO>) {
            return validate_motion_intent(payload);
          } else {
            return validate_command_intent(payload);
          }
        },
        participant.payload);
    if (!payload_result.ok()) {
      return payload_result;
    }
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
