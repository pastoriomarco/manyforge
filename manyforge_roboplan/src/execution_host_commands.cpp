#include "execution_host_commands.hpp"
#include "execution_host_posix.hpp"
#include <manyforge_deployment/artifact_codec.hpp>
#include <algorithm>
#include <cmath>

namespace manyforge::roboplan_backend::host_detail {

void RequireExactObjectSize(const CanonicalValue &value, std::size_t size,
                            std::string_view label) {
  Require(value.is_object() && value.as_object().size() == size,
          std::string(label) + " has an unknown or missing field");
}

const CanonicalValue &Member(const CanonicalValue &root,
                             std::string_view name) {
  const auto *value = root.find_object_member(name);
  Require(value != nullptr, "command is missing field " + std::string(name));
  return *value;
}

std::string Text(const CanonicalValue &root, std::string_view name,
                 bool allow_empty) {
  const auto *value = std::get_if<std::string>(&Member(root, name).storage());
  Require(value != nullptr && (allow_empty || !value->empty()) &&
              value->size() <= kMaximumIdentityBytes,
          "command field " + std::string(name) + " must be one bounded string");
  return *value;
}

double Number(const CanonicalValue &root, std::string_view name) {
  const auto *value = std::get_if<double>(&Member(root, name).storage());
  Require(value != nullptr && std::isfinite(*value),
          "command field " + std::string(name) + " must be one finite number");
  return *value;
}

bool Boolean(const CanonicalValue &root, std::string_view name) {
  const auto *value = std::get_if<bool>(&Member(root, name).storage());
  Require(value != nullptr,
          "command field " + std::string(name) + " must be boolean");
  return *value;
}

std::uint64_t Counter(const CanonicalValue &root, std::string_view name,
                      std::uint64_t minimum) {
  const double value = Number(root, name);
  Require(value >= static_cast<double>(minimum) && std::floor(value) == value &&
              value <= kMaximumSafeInteger,
          "command field " + std::string(name) +
              " must be one safe unsigned integer");
  return static_cast<std::uint64_t>(value);
}

std::optional<double> OptionalNumber(const CanonicalValue &root,
                                     std::string_view name) {
  const auto &member = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto *value = std::get_if<double>(&member.storage());
  Require(value != nullptr && std::isfinite(*value),
          "command field " + std::string(name) + " must be null or finite");
  return *value;
}

std::optional<std::uint64_t> OptionalCounter(const CanonicalValue &root,
                                             const std::string_view name,
                                             const std::uint64_t minimum) {
  const auto value = OptionalNumber(root, name);
  if (!value.has_value()) {
    return std::nullopt;
  }
  Require(*value >= static_cast<double>(minimum) &&
              std::floor(*value) == *value && *value <= kMaximumSafeInteger,
          "command field " + std::string(name) +
              " must be null or one safe unsigned integer");
  return static_cast<std::uint64_t>(*value);
}

std::optional<std::string> OptionalText(const CanonicalValue &root,
                                        std::string_view name) {
  const auto &member = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto *value = std::get_if<std::string>(&member.storage());
  Require(value != nullptr && !value->empty() &&
              value->size() <= kMaximumIdentityBytes,
          "command field " + std::string(name) +
              " must be null or one bounded string");
  return *value;
}

std::optional<std::string> OptionalBoundedText(const CanonicalValue &root,
                                               std::string_view name,
                                               std::size_t maximum_bytes) {
  const auto &member = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto *value = std::get_if<std::string>(&member.storage());
  Require(value != nullptr && !value->empty() && value->size() <= maximum_bytes,
          "command field " + std::string(name) +
              " must be null or one bounded string");
  return *value;
}

const CanonicalValue::Array &Array(const CanonicalValue &root,
                                   std::string_view name) {
  const auto &member = Member(root, name);
  const auto *values = std::get_if<CanonicalValue::Array>(&member.storage());
  Require(values != nullptr,
          "command field " + std::string(name) + " must be an array");
  return *values;
}

std::vector<std::string> TextArray(const CanonicalValue &root,
                                   std::string_view name, std::size_t maximum) {
  const auto &values = Array(root, name);
  Require(values.size() <= maximum,
          "command text array exceeds its fixed bound");
  std::vector<std::string> result;
  result.reserve(values.size());
  for (const auto &value : values) {
    const auto *text = std::get_if<std::string>(&value.storage());
    Require(text != nullptr && !text->empty() &&
                text->size() <= kMaximumIdentityBytes,
            "command text array contains an invalid value");
    result.push_back(*text);
  }
  return result;
}

std::vector<double> NumberArray(const CanonicalValue &root,
                                std::string_view name, std::size_t maximum) {
  const auto &values = Array(root, name);
  Require(values.size() <= maximum,
          "command number array exceeds its fixed bound");
  std::vector<double> result;
  result.reserve(values.size());
  for (const auto &value : values) {
    const auto *number = std::get_if<double>(&value.storage());
    Require(number != nullptr && std::isfinite(*number),
            "command number array contains a non-finite value");
    result.push_back(*number);
  }
  return result;
}

[[nodiscard]] core::Pose3DTO DecodePose(const CanonicalValue &value) {
  RequireExactObjectSize(value, 2U, "pose");
  const auto &position = Member(value, "position_m");
  const auto &orientation = Member(value, "orientation");
  RequireExactObjectSize(position, 3U, "pose position");
  RequireExactObjectSize(orientation, 4U, "pose orientation");
  return core::Pose3DTO{
      core::Vector3DTO{Number(position, "x_m"), Number(position, "y_m"),
                       Number(position, "z_m")},
      core::QuaternionDTO{Number(orientation, "x"), Number(orientation, "y"),
                          Number(orientation, "z"), Number(orientation, "w")}};
}

/// Decode one portable reference. `frame_role` may carry an exact robot
/// qualifier `target` ({target_role_id, controllable_component_id,
/// command_target_id}); its Program-Binding agreement is checked at admission
/// by `VerifyReferenceTargets`, never inferred here.
[[nodiscard]] core::ReferenceDTO
DecodeReference(const CanonicalValue &value, const bool allow_robot_target) {
  Require(value.is_object(), "motion reference must be an object");
  const auto type = Text(value, "type");
  if (type == "cell_frame") {
    RequireExactObjectSize(value, 1U, "cell-frame reference");
    return core::ReferenceDTO(core::ReferenceDTO::Type::kCellFrame,
                              std::string{});
  }
  if (type == "frame_role") {
    const auto *target = value.find_object_member("target");
    if (target == nullptr) {
      RequireExactObjectSize(value, 2U, "motion reference");
      return core::ReferenceDTO(core::ReferenceDTO::Type::kFrameRole,
                                Text(value, "frame_role_id"));
    }
    Require(allow_robot_target,
            "robot-qualified frame reference is unsupported here");
    RequireExactObjectSize(value, 3U, "robot frame reference");
    RequireExactObjectSize(*target, 3U, "robot frame reference target");
    core::ReferenceDTO reference(
        core::FrameRoleId(Text(value, "frame_role_id")),
        core::ReferenceTargetDTO{
            core::TargetRoleId(Text(*target, "target_role_id")),
            core::ControllableComponentId(
                Text(*target, "controllable_component_id")),
            core::CommandTargetId(Text(*target, "command_target_id"))});
    Require(core::reference_identity_matches_type(reference),
            "robot frame reference is incomplete");
    return reference;
  }
  RequireExactObjectSize(value, 2U, "motion reference");
  if (type == "component_body_role") {
    return core::ReferenceDTO(core::ReferenceDTO::Type::kComponentBodyRole,
                              Text(value, "collision_body_role_id"));
  }
  if (type == "scene_resource") {
    return core::ReferenceDTO(core::ReferenceDTO::Type::kSceneResource,
                              Text(value, "scene_resource_id"));
  }
  if (type == "runtime_object") {
    return core::ReferenceDTO(core::ReferenceDTO::Type::kRuntimeObject,
                              Text(value, "runtime_object_id"));
  }
  if (type == "pose") {
    return core::ReferenceDTO(DecodePose(Member(value, "pose")));
  }
  Fail("motion reference type is unsupported");
}

[[nodiscard]] std::optional<core::ReferenceDTO>
DecodeOptionalReference(const CanonicalValue &root, const std::string_view name,
                        const bool allow_robot_target) {
  const auto &value = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(value.storage())) {
    return std::nullopt;
  }
  return DecodeReference(value, allow_robot_target);
}

[[nodiscard]] core::MotionSpecDTO
DecodeMotionSpec(const CanonicalValue &value) {
  const auto kind = Text(value, "kind");
  if (kind == "joint_goal") {
    RequireExactObjectSize(value, 3U, "joint goal");
    return core::JointGoalDTO{
        TextArray(value, "joint_names", core::kMaxOperationParticipants),
        NumberArray(value, "positions_rad", core::kMaxOperationParticipants)};
  }
  if (kind == "pose_goal") {
    RequireExactObjectSize(value, 2U, "pose goal");
    return core::PoseGoalDTO{DecodePose(Member(value, "goal"))};
  }
  if (kind == "cartesian_path") {
    const auto *blend = value.find_object_member("max_blend_deviation_rad");
    // A Cartesian path has nine required fields; the blend-deviation field is
    // the one optional tenth field.  Keep the decoder aligned with the
    // frontend/headless CartesianPath contract.
    RequireExactObjectSize(value, blend == nullptr ? 9U : 10U,
                           "Cartesian path");
    const auto &encoded_waypoints = Array(value, "waypoints");
    Require(!encoded_waypoints.empty() &&
                encoded_waypoints.size() <= core::kMaxOperationParticipants,
            "Cartesian path waypoint count is outside its fixed bound");
    std::vector<core::Pose3DTO> waypoints;
    waypoints.reserve(encoded_waypoints.size());
    for (const auto &waypoint : encoded_waypoints) {
      waypoints.push_back(DecodePose(waypoint));
    }
    Require(Text(value, "speed_mode") == "bounded",
            "Cartesian speed mode is unsupported");
    core::CartesianFallbackPolicyDTO fallback;
    const auto fallback_name = Text(value, "fallback_policy");
    if (fallback_name == "fail_closed") {
      fallback = core::CartesianFallbackPolicyDTO::kFailClosed;
    } else if (fallback_name == "allow_joint_space_fallback") {
      fallback = core::CartesianFallbackPolicyDTO::kAllowJointSpaceFallback;
    } else {
      Fail("Cartesian fallback policy is unsupported");
    }
    core::CartesianPathDTO result{
        std::move(waypoints),
        Number(value, "max_translation_step_m"),
        Number(value, "max_rotation_step_rad"),
        core::CartesianSpeedModeDTO::kBounded,
        Number(value, "max_angular_speed_rad_s"),
        Number(value, "max_linear_acceleration_mps2"),
        Number(value, "max_angular_acceleration_rad_s2"),
        fallback};
    if (blend != nullptr) {
      result.max_blend_deviation_rad = Number(value, "max_blend_deviation_rad");
    }
    return result;
  }
  Fail("motion specification kind is unsupported");
}

[[nodiscard]] core::SceneRevision
DecodeSceneRevision(const CanonicalValue &value) {
  RequireExactObjectSize(value, 3U, "scene revision");
  const double revision = Number(value, "revision_id");
  Require(revision >= 1.0 && std::floor(revision) == revision &&
              revision <= 9'007'199'254'740'991.0,
          "scene revision id is invalid");
  return core::SceneRevision{core::SceneId(Text(value, "scene_id")),
                             static_cast<std::uint64_t>(revision),
                             core::ContentDigest(Text(value, "content_hash"))};
}

[[nodiscard]] core::ActionSpanDTO
DecodeActionSpan(const CanonicalValue &value) {
  RequireExactObjectSize(value, 6U, "action span");
  const auto source =
      core::action_source_from_string(Text(value, "action_source"));
  Require(source.has_value(), "action span source is unsupported");
  const auto optional_or_empty = [&](std::string_view name) {
    return OptionalText(value, name).value_or(std::string{});
  };
  return core::ActionSpanDTO{*source,
                             Text(value, "action_span_id"),
                             optional_or_empty("program_run_id"),
                             optional_or_empty("node_instance_id"),
                             optional_or_empty("node_attempt_id"),
                             optional_or_empty("parent_node_attempt_id")};
}

[[nodiscard]] core::PublicRequestContextDTO
DecodeRequestContext(const CanonicalValue &value) {
  RequireExactObjectSize(value, 4U, "public request context");
  return core::PublicRequestContextDTO{
      core::RequestId(Text(value, "request_id")),
      core::RequestOwnerEpoch(Text(value, "request_owner_epoch")),
      Text(value, "source_label"), OptionalText(value, "correlation_id")};
}

[[nodiscard]] core::RequestAttributionDTO
DecodeRequestAttribution(const CanonicalValue &value) {
  RequireExactObjectSize(value, 3U, "request attribution");
  core::RequestTransportDTO transport;
  const auto transport_name = Text(value, "transport");
  if (transport_name == "ros") {
    transport = core::RequestTransportDTO::kRos;
  } else if (transport_name == "http") {
    transport = core::RequestTransportDTO::kHttp;
  } else if (transport_name == "internal") {
    transport = core::RequestTransportDTO::kInternal;
  } else {
    Fail("request attribution transport is unsupported");
  }
  core::RequestAssuranceDTO assurance;
  const auto assurance_name = Text(value, "assurance");
  if (assurance_name == "unverified") {
    assurance = core::RequestAssuranceDTO::kUnverified;
  } else if (assurance_name == "externally_authenticated") {
    assurance = core::RequestAssuranceDTO::kExternallyAuthenticated;
  } else {
    Fail("request attribution assurance is unsupported");
  }
  core::RequestAttributionDTO attribution{transport, assurance,
                                          OptionalText(value, "principal_id")};
  Require(attribution.locally_valid(),
          "request attribution is internally inconsistent");
  return attribution;
}

[[nodiscard]] core::JointDynamicsLimitDTO
DecodeDynamicsLimit(const CanonicalValue &value) {
  RequireExactObjectSize(value, 3U, "joint dynamics limit");
  return {OptionalNumber(value, "max_velocity_rad_s"),
          OptionalNumber(value, "max_acceleration_rad_s2"),
          OptionalNumber(value, "max_jerk_rad_s3")};
}

[[nodiscard]] core::MotionParticipantIntentDTO
DecodeMotionPayload(const CanonicalValue &value) {
  const bool has_runtime_object_bindings =
      value.is_object() &&
      value.find_object_member("runtime_object_bindings") != nullptr;
  // Optional nullable member; omitted by callers that predate framed goals.
  const bool has_goal_frame_member =
      value.is_object() &&
      value.find_object_member("goal_frame_reference") != nullptr;
  Require(value.is_object() &&
              value.as_object().size() ==
                  16U + (has_runtime_object_bindings ? 1U : 0U) +
                      (has_goal_frame_member ? 1U : 0U),
          "motion payload has an unknown or missing field");
  Require(Text(value, "kind") == "motion",
          "operation payload kind is unsupported");

  const auto &tolerances = Member(value, "tolerances");
  RequireExactObjectSize(tolerances, 3U, "motion tolerances");
  const auto &speed = Member(value, "speed_profile");
  RequireExactObjectSize(speed, 3U, "motion speed profile");
  const auto &timeouts = Member(value, "timeouts");
  RequireExactObjectSize(timeouts, 2U, "motion timeouts");

  core::CacheMode cache_mode;
  const auto cache = Text(value, "cache_mode");
  if (cache == "default") {
    cache_mode = core::CacheMode::kDefault;
  } else if (cache == "disable") {
    cache_mode = core::CacheMode::kDisable;
  } else {
    Fail("motion cache mode is unsupported");
  }
  core::PlanningProfileDTO planning_profile;
  const auto profile = Text(value, "planning_profile");
  if (profile == "first_feasible") {
    planning_profile = core::PlanningProfileDTO::kFirstFeasible;
  } else if (profile == "balanced") {
    planning_profile = core::PlanningProfileDTO::kBalanced;
  } else if (profile == "quality_bounded") {
    planning_profile = core::PlanningProfileDTO::kQualityBounded;
  } else {
    Fail("motion planning profile is unsupported");
  }
  core::MotionTargetModeDTO target_mode;
  const auto target_mode_name = Text(value, "target_mode");
  if (target_mode_name == "absolute") {
    target_mode = core::MotionTargetModeDTO::kAbsolute;
  } else if (target_mode_name == "relative") {
    target_mode = core::MotionTargetModeDTO::kRelative;
  } else {
    Fail("motion target mode is unsupported");
  }
  auto target_reference =
      DecodeOptionalReference(value, "target_reference", true);
  auto offset_reference =
      DecodeOptionalReference(value, "offset_reference", true);
  auto goal_frame_reference =
      has_goal_frame_member
          ? DecodeOptionalReference(value, "goal_frame_reference", true)
          : std::nullopt;
  Require(Array(value, "motion_interrupt_scopes").empty(),
          "native motion cannot carry I8 interrupt scopes");

  const auto &motion = Member(value, "motion");
  auto decoded_motion = DecodeMotionSpec(motion);
  const bool joint_goal =
      std::holds_alternative<core::JointGoalDTO>(decoded_motion);
  if (goal_frame_reference.has_value()) {
    Require(target_mode == core::MotionTargetModeDTO::kAbsolute &&
                (std::holds_alternative<core::PoseGoalDTO>(decoded_motion) ||
                 std::holds_alternative<core::CartesianPathDTO>(
                     decoded_motion)) &&
                goal_frame_reference->type != core::ReferenceDTO::Type::kPose,
            "goal frame requires an absolute pose or Cartesian goal and a "
            "named frame");
  }
  if (target_mode == core::MotionTargetModeDTO::kAbsolute) {
    Require(!target_reference.has_value() && !offset_reference.has_value(),
            "absolute motion cannot carry references");
  } else if (joint_goal) {
    Require(!target_reference.has_value() && !offset_reference.has_value(),
            "relative joint motion cannot carry spatial references");
  } else {
    Require(target_reference.has_value(),
            "relative spatial motion requires target_reference");
  }

  std::optional<core::ContactPolicyId> contact_policy;
  if (const auto contact_name = OptionalText(value, "contact_policy_id");
      contact_name.has_value()) {
    contact_policy = core::ContactPolicyId(*contact_name);
  }
  std::vector<core::RuntimeObjectReferenceBindingDTO> runtime_objects;
  if (has_runtime_object_bindings) {
    const auto &encoded_runtime_objects =
        Array(value, "runtime_object_bindings");
    Require(encoded_runtime_objects.size() <=
                core::kMaxOperationRuntimeObjectBindings,
            "runtime-object binding count exceeds its fixed bound");
    runtime_objects.reserve(encoded_runtime_objects.size());
    for (const auto &encoded : encoded_runtime_objects) {
      RequireExactObjectSize(encoded, 2U, "runtime-object binding");
      runtime_objects.push_back(core::RuntimeObjectReferenceBindingDTO{
          core::RuntimeObjectId(Text(encoded, "runtime_object_id")),
          core::SceneResourceId(Text(encoded, "scene_resource_id"))});
    }
  }
  if (joint_goal) {
    Require(
        !contact_policy.has_value() && runtime_objects.empty(),
        "joint motion cannot carry contact-policy or runtime-object bindings");
  }

  const auto &dynamics = Member(value, "per_joint_dynamics_limits");
  Require(dynamics.is_object() &&
              dynamics.as_object().size() <= core::kMaxOperationParticipants,
          "joint dynamics map exceeds its fixed bound");
  std::map<std::string, core::JointDynamicsLimitDTO> limits;
  for (const auto &[joint_name, limit] : dynamics.as_object()) {
    Require(!joint_name.empty() && joint_name.size() <= kMaximumIdentityBytes,
            "joint dynamics map contains an invalid name");
    const auto [position, inserted] =
        limits.emplace(joint_name, DecodeDynamicsLimit(limit));
    static_cast<void>(position);
    Require(inserted, "joint dynamics map repeats one name");
  }

  return core::MotionParticipantIntentDTO{
      core::MoveId(Text(value, "move_id")),
      core::FrameRoleId(Text(value, "base_frame_role_id")),
      core::FrameRoleId(Text(value, "tcp_frame_role_id")),
      core::TolerancesDTO{Number(tolerances, "joint_position_tol_rad"),
                          Number(tolerances, "translation_tol_m"),
                          Number(tolerances, "rotation_tol_rad")},
      core::SpeedProfileDTO{Number(speed, "velocity_scale"),
                            Number(speed, "acceleration_scale"),
                            OptionalNumber(speed, "max_tcp_linear_speed_mps")},
      core::TimeoutsDTO{Number(timeouts, "planning_timeout_s"),
                        Number(timeouts, "execution_timeout_s")},
      cache_mode,
      planning_profile,
      target_mode,
      std::move(target_reference),
      std::move(offset_reference),
      std::move(decoded_motion),
      std::move(contact_policy),
      std::move(limits),
      {},
      std::move(runtime_objects),
      std::move(goal_frame_reference)};
}

[[nodiscard]] core::CoordinatePositionDTO
DecodeCoordinatePosition(const CanonicalValue &value) {
  RequireExactObjectSize(value, 2U, "coordinate position");
  const auto kind = Text(value, "kind");
  if (kind == "angular_rad") {
    return core::AngularPositionDTO{Number(value, "value_rad")};
  }
  if (kind == "linear_m") {
    return core::LinearPositionDTO{Number(value, "value_m")};
  }
  Fail("coordinate position kind is unsupported");
}

[[nodiscard]] core::CoordinateEffortDTO
DecodeCoordinateEffort(const CanonicalValue &value) {
  RequireExactObjectSize(value, 2U, "coordinate effort");
  const auto kind = Text(value, "kind");
  if (kind == "angular_newton_meter") {
    return core::AngularEffortDTO{Number(value, "value_nm")};
  }
  if (kind == "linear_newton") {
    return core::LinearEffortDTO{Number(value, "value_n")};
  }
  Fail("coordinate effort kind is unsupported");
}

[[nodiscard]] core::CapabilityCommandIntentDTO
DecodeCapabilityCommandPayload(const CanonicalValue &value) {
  RequireExactObjectSize(value, 6U, "capability-command payload");
  Require(Text(value, "kind") == "capability_command",
          "operation payload kind is unsupported");
  const auto &command_value = Member(value, "command_value");
  RequireExactObjectSize(command_value, 3U, "gripper-position command value");
  Require(Text(command_value, "kind") == "gripper_position",
          "capability-command value kind is unsupported");

  std::optional<core::CoordinateEffortDTO> max_effort;
  const auto &effort = Member(command_value, "max_effort");
  if (!std::holds_alternative<std::nullptr_t>(effort.storage())) {
    max_effort = DecodeCoordinateEffort(effort);
  }

  const auto &option_values = Array(value, "caller_options");
  Require(option_values.size() <= core::kMaxOperationCommandOptions,
          "capability-command options exceed their fixed bound");
  std::vector<core::CommandDurationOptionDTO> caller_options;
  caller_options.reserve(option_values.size());
  for (const auto &option : option_values) {
    RequireExactObjectSize(option, 2U, "capability-command option");
    caller_options.push_back(core::CommandDurationOptionDTO{
        core::CommandOptionId(Text(option, "option_id")),
        Number(option, "value_s")});
  }

  return core::CapabilityCommandIntentDTO{
      core::CommandSchemaId(Text(value, "command_schema_id")),
      core::CommandSchemaRevision(Text(value, "command_schema_revision")),
      core::GripperPositionCommandValueDTO{
          DecodeCoordinatePosition(Member(command_value, "position")),
          std::move(max_effort)},
      Number(value, "completion_timeout_s"), std::move(caller_options)};
}

[[nodiscard]] core::OperationParticipantPayloadDTO
DecodeParticipantPayload(const CanonicalValue &value) {
  const auto kind = Text(value, "kind");
  if (kind == "motion") {
    return DecodeMotionPayload(value);
  }
  if (kind == "capability_command") {
    return DecodeCapabilityCommandPayload(value);
  }
  Fail("operation payload kind is unsupported");
}

[[nodiscard]] std::optional<core::ProgramNodeExecutionScopeDTO>
DecodeProgramExecutionScope(const CanonicalValue &value,
                            const core::RequestAttributionDTO &attribution) {
  const auto *scope = value.find_object_member("execution_scope");
  if (scope == nullptr)
    return std::nullopt;
  Require(attribution.transport == core::RequestTransportDTO::kInternal &&
              attribution.assurance == core::RequestAssuranceDTO::kUnverified &&
              !attribution.principal_id.has_value(),
          "Program origin requires the private owner-client path");
  RequireExactObjectSize(*scope, 2U, "Program node execution scope");
  core::ProgramNodeExecutionScopeDTO decoded{
      core::ProgramNodeInstanceId(Text(*scope, "node_instance_id")),
      Text(*scope, "node_attempt_id")};
  Require(core::valid_program_node_execution_scope(decoded),
          "Program node execution scope is malformed or oversized");
  return decoded;
}

[[nodiscard]] core::OperationIntentDTO
DecodeOperation(const CanonicalValue &value,
                const core::RequestAttributionDTO &attribution) {
  const auto *manual_context = value.find_object_member("manual_handover");
  RequireExactObjectSize(value, manual_context == nullptr ? 10U : 11U,
                         "operation intent");
  std::optional<core::ManualProgramHandoverContextDTO> manual_handover;
  if (manual_context != nullptr &&
      !std::holds_alternative<std::nullptr_t>(manual_context->storage())) {
    RequireExactObjectSize(*manual_context, 4U, "manual handover context");
    manual_handover = core::ManualProgramHandoverContextDTO{
        core::RequestOwnerEpoch(
            Text(*manual_context, "program_runner_owner_epoch")),
        core::ProgramRunId(Text(*manual_context, "program_run_id")),
        core::CellAdmissionClaimGeneration(
            Counter(*manual_context, "claim_generation", 1U)),
        Counter(*manual_context, "selection_revision", 0U)};
  }
  const auto &request = Member(value, "request_context");
  const auto &participant_values = Array(value, "participants");
  Require(!participant_values.empty() &&
              participant_values.size() <= core::kMaxOperationParticipants,
          "operation participant count exceeds its bounds");
  const auto policy_name = Text(value, "coordination_policy");
  Require(policy_name == "independent" || policy_name == "coordinated_all",
          "operation coordination policy is unsupported");
  const auto policy = policy_name == "independent"
                          ? core::OperationCoordinationPolicy::kIndependent
                          : core::OperationCoordinationPolicy::kCoordinatedAll;
  Require((participant_values.size() == 1U) ==
              (policy == core::OperationCoordinationPolicy::kIndependent),
          "operation participant count differs from its coordination policy");
  std::vector<core::OperationParticipantIntentDTO> participants;
  participants.reserve(participant_values.size());
  for (const auto &participant : participant_values) {
    auto execution_scope =
        DecodeProgramExecutionScope(participant, attribution);
    RequireExactObjectSize(participant, execution_scope.has_value() ? 6U : 5U,
                           "operation participant");
    std::optional<core::ActionSpanDTO> action_span;
    const auto &span = Member(participant, "action_span");
    if (!std::holds_alternative<std::nullptr_t>(span.storage())) {
      action_span = DecodeActionSpan(span);
    }
    participants.push_back(core::OperationParticipantIntentDTO{
        core::ComponentRoleId(Text(participant, "component_role_id")),
        core::TargetRoleId(Text(participant, "target_role_id")),
        core::CapabilityId(Text(participant, "required_capability_id")),
        std::move(action_span),
        DecodeParticipantPayload(Member(participant, "payload")),
        std::move(execution_scope)});
  }
  std::optional<core::SceneRevision> expected_scene;
  const auto &scene = Member(value, "expected_scene_revision");
  if (!std::holds_alternative<std::nullptr_t>(scene.storage())) {
    expected_scene = DecodeSceneRevision(scene);
  }
  core::OperationIntentDTO result{
      DecodeRequestContext(request),
      core::ProgramId(Text(value, "program_id")),
      core::ProgramRevision(Text(value, "program_revision")),
      core::ProgramBindingId(Text(value, "program_binding_id")),
      core::ProgramRunId(Text(value, "program_run_id")),
      core::GraphRevision(Text(value, "expected_graph_revision")),
      core::RetainedBundleId(Text(value, "expected_retained_bundle_id")),
      std::move(expected_scene),
      std::move(participants),
      policy,
      attribution,
      std::move(manual_handover)};
  if (result.manual_handover) {
    const auto valid = core::validate_operation_intent(result);
    Require(valid.ok(), valid.ok() ? "" : valid.error().message);
  }
  return result;
}

[[nodiscard]] std::array<double, 3U> DecodeTriple(const CanonicalValue &root,
                                                  const std::string_view name) {
  const auto values = NumberArray(root, name, 3U);
  Require(values.size() == 3U, "command field " + std::string(name) +
                                   " must contain exactly three numbers");
  return {values[0], values[1], values[2]};
}

[[nodiscard]] core::PickOffsetEnvelopeDTO
DecodePickEnvelope(const CanonicalValue &value) {
  RequireExactObjectSize(value, 4U, "pick offset envelope");
  const auto position_min = DecodeTriple(value, "position_min_m");
  const auto position_max = DecodeTriple(value, "position_max_m");
  const auto orientation_min = DecodeTriple(value, "orientation_rpy_min_deg");
  const auto orientation_max = DecodeTriple(value, "orientation_rpy_max_deg");
  for (std::size_t index = 0U; index < 3U; ++index) {
    Require(position_min[index] <= position_max[index] &&
                orientation_min[index] <= orientation_max[index],
            "pick offset envelope minimum exceeds its maximum");
  }
  return {position_min, position_max, orientation_min, orientation_max};
}

[[nodiscard]] core::PickCandidateSynthesisIntentDTO
DecodePickSynthesis(const CanonicalValue &value) {
  RequireExactObjectSize(value, 11U, "pick synthesis intent");
  core::PickCandidateSearchStrategyDTO strategy;
  const auto strategy_name = Text(value, "search_strategy");
  if (strategy_name == "first_feasible") {
    strategy = core::PickCandidateSearchStrategyDTO::kFirstFeasible;
  } else if (strategy_name == "best_cost") {
    strategy = core::PickCandidateSearchStrategyDTO::kBestCost;
  } else {
    Fail("pick search strategy is unsupported");
  }
  const auto maximum = Counter(value, "maximum_candidate_count", 1U);
  Require(maximum <= 4096U,
          "pick maximum candidate count exceeds its fixed bound");
  const auto translation_step = Number(value, "translation_step_m");
  const auto rotation_step = Number(value, "rotation_step_deg");
  const auto min_tcp_z = OptionalNumber(value, "min_tcp_z_m");
  Require(translation_step > 0.0 && rotation_step > 0.0 &&
              (!min_tcp_z.has_value() || *min_tcp_z >= 0.0),
          "pick synthesis step or minimum TCP height is invalid");
  return core::PickCandidateSynthesisIntentDTO{
      DecodePickEnvelope(Member(value, "pick_offset_envelope")),
      DecodePickEnvelope(Member(value, "approach_offset_envelope")),
      DecodeOptionalReference(value, "pick_offset_reference", true),
      DecodeOptionalReference(value, "approach_offset_reference", true),
      DecodeTriple(value, "base_tool_orientation_rpy_deg"),
      translation_step,
      rotation_step,
      Boolean(value, "force_vertical_pick"),
      min_tcp_z,
      static_cast<std::size_t>(maximum),
      strategy};
}

[[nodiscard]] core::PickSequencePlanningIntentDTO
DecodePickPlanningIntent(const CanonicalValue &value,
                         const core::RequestAttributionDTO &attribution) {
  RequireExactObjectSize(value, 9U, "pick planning intent");
  return core::PickSequencePlanningIntentDTO{
      Text(value, "sequence_id"),
      core::SceneResourceId(Text(value, "selected_object_id")),
      core::RuntimeObjectId(Text(value, "selected_runtime_object_id")),
      core::FrameRoleId(Text(value, "carrier_frame_role_id")),
      DecodePickSynthesis(Member(value, "synthesis")),
      DecodeOperation(Member(value, "approach_operation"), attribution),
      DecodeOperation(Member(value, "descend_operation"), attribution),
      DecodeOperation(Member(value, "tool_operation"), attribution),
      DecodeOperation(Member(value, "retreat_operation"), attribution)};
}

[[nodiscard]] core::PlaceSequencePlanningIntentDTO
DecodePlacePlanningIntent(const CanonicalValue &value,
                          const core::RequestAttributionDTO &attribution) {
  RequireExactObjectSize(value, 7U, "place planning intent");
  return core::PlaceSequencePlanningIntentDTO{
      Text(value, "sequence_id"),
      core::SceneResourceId(Text(value, "selected_object_id")),
      core::FrameRoleId(Text(value, "carrier_frame_role_id")),
      DecodeOperation(Member(value, "approach_operation"), attribution),
      DecodeOperation(Member(value, "descend_operation"), attribution),
      DecodeOperation(Member(value, "tool_operation"), attribution),
      DecodeOperation(Member(value, "retreat_operation"), attribution)};
}

[[nodiscard]] core::OperationControlRequestDTO
DecodeOperationControlRequest(const CanonicalValue &value,
                              const core::RequestAttributionDTO &attribution) {
  auto execution_scope = DecodeProgramExecutionScope(value, attribution);
  RequireExactObjectSize(value, execution_scope.has_value() ? 5U : 4U,
                         "operation control request");
  return core::OperationControlRequestDTO{
      DecodeRequestContext(Member(value, "request_context")),
      core::OperationId(Text(value, "operation_id")),
      core::OperationRevision(
          Counter(value, "expected_operation_revision", 1U)),
      OptionalBoundedText(value, "reason", kMaximumReasonBytes),
      attribution,
      std::move(execution_scope)};
}

[[nodiscard]] core::StopIntentDTO
DecodeStopIntent(const CanonicalValue &value,
                 const core::RequestAttributionDTO &attribution) {
  Require(value.is_object() && (value.as_object().size() == 2U ||
                                value.as_object().size() == 3U),
          "Stop intent has an unknown or missing field");
  const auto selector_count =
      static_cast<std::size_t>(value.find_object_member("operations") !=
                               nullptr) +
      static_cast<std::size_t>(value.find_object_member("components") !=
                               nullptr) +
      static_cast<std::size_t>(value.find_object_member("resources") !=
                               nullptr);
  Require(selector_count == 1U,
          "Stop intent must contain exactly one selector kind");
  core::StopSelectorDTO selector;
  if (value.find_object_member("operations") != nullptr) {
    std::vector<core::StopOperationTargetDTO> targets;
    const auto &rows = Array(value, "operations");
    Require(!rows.empty() && rows.size() <= core::kMaximumStopSelectorTargets,
            "Stop operation selector exceeds its row bound");
    targets.reserve(rows.size());
    for (const auto &target : rows) {
      RequireExactObjectSize(target, 3U, "Stop operation target");
      targets.push_back(core::StopOperationTargetDTO{
          core::OperationId(Text(target, "operation_id")),
          core::OperationRevision(
              Counter(target, "expected_operation_revision", 1U)),
          core::StopGeneration(
              Counter(target, "expected_stop_generation", 0U))});
    }
    selector = std::move(targets);
  } else if (value.find_object_member("components") != nullptr) {
    std::vector<core::StopComponentTargetDTO> targets;
    const auto &rows = Array(value, "components");
    Require(!rows.empty() && rows.size() <= core::kMaximumStopSelectorTargets,
            "Stop component selector exceeds its row bound");
    targets.reserve(rows.size());
    for (const auto &target : rows) {
      RequireExactObjectSize(target, 3U, "Stop component target");
      targets.push_back(core::StopComponentTargetDTO{
          core::ControllableComponentId(
              Text(target, "controllable_component_id")),
          core::GraphRevision(Text(target, "expected_graph_revision")),
          core::ComponentLifecycleGeneration(
              Counter(target, "expected_component_lifecycle_generation", 1U))});
    }
    selector = std::move(targets);
  } else {
    std::vector<core::StopResourceTargetDTO> targets;
    const auto &rows = Array(value, "resources");
    Require(!rows.empty() && rows.size() <= core::kMaximumStopSelectorTargets,
            "Stop resource selector exceeds its row bound");
    targets.reserve(rows.size());
    for (const auto &target : rows) {
      RequireExactObjectSize(target, 4U, "Stop resource target");
      targets.push_back(core::StopResourceTargetDTO{
          core::ResourceId(Text(target, "resource_id")),
          core::GraphRevision(Text(target, "expected_graph_revision")),
          Counter(target, "expected_operation_resource_generation", 0U),
          core::StopGeneration(
              Counter(target, "expected_stop_generation", 0U))});
    }
    selector = std::move(targets);
  }
  std::optional<std::string> reason;
  if (value.find_object_member("reason") != nullptr) {
    reason = OptionalBoundedText(value, "reason", kMaximumReasonBytes);
  }
  return core::StopIntentDTO{
      DecodeRequestContext(Member(value, "request_context")),
      std::move(selector), std::move(reason), attribution};
}

[[nodiscard]] core::StopIntentDTO
DecodeProgramRunStopIntent(const CanonicalValue &value,
                           const core::RequestAttributionDTO &attribution) {
  Require(value.is_object() && (value.as_object().size() == 2U ||
                                value.as_object().size() == 3U),
          "Program-run Stop intent has an unknown or missing field");
  std::optional<std::string> reason;
  if (value.find_object_member("reason") != nullptr) {
    reason = OptionalBoundedText(value, "reason", kMaximumReasonBytes);
  }
  return core::StopIntentDTO{
      DecodeRequestContext(Member(value, "request_context")),
      std::vector<core::StopProgramRunTargetDTO>{core::StopProgramRunTargetDTO{
          core::ProgramRunId(Text(value, "program_run_id"))}},
      std::move(reason), attribution};
}

[[nodiscard]] core::FullStopIntentDTO
DecodeFullStopIntent(const CanonicalValue &value,
                     const core::RequestAttributionDTO &attribution) {
  RequireExactObjectSize(value, 2U, "Full Stop intent");
  return core::FullStopIntentDTO{
      DecodeRequestContext(Member(value, "request_context")),
      core::StopGeneration(Counter(value, "expected_stop_generation", 0U)),
      attribution};
}

[[nodiscard]] core::StopFaultRecoveryIntentDTO
DecodeStopFaultRecoveryIntent(const CanonicalValue &value) {
  Require(value.is_object() && (value.as_object().size() == 10U ||
                                value.as_object().size() == 11U),
          "Stop fault-recovery intent has an unknown or missing field");
  std::vector<core::StopFaultRecoveryConfigurationExpectationDTO> expectations;
  const auto &rows = Array(value, "configuration_expectations");
  Require(!rows.empty() &&
              rows.size() <= core::kMaximumStopFaultRecoveryEvidenceRefs,
          "Stop fault-recovery configuration table exceeds its row bound");
  expectations.reserve(rows.size());
  for (const auto &row : rows) {
    RequireExactObjectSize(row, 3U,
                           "Stop fault-recovery configuration expectation");
    expectations.push_back(core::StopFaultRecoveryConfigurationExpectationDTO{
        core::ControlDomainId(Text(row, "control_domain_id")),
        core::ActiveControlConfigurationId(
            Text(row, "active_control_configuration_id")),
        core::ActiveControlConfigurationGeneration(
            Counter(row, "active_control_configuration_generation", 1U))});
  }
  std::optional<std::string> reason;
  if (value.find_object_member("reason") != nullptr) {
    reason = OptionalBoundedText(value, "reason", kMaximumReasonBytes);
  }
  return core::StopFaultRecoveryIntentDTO{
      DecodeRequestContext(Member(value, "request_context")),
      core::StopFaultLatchId(Text(value, "expected_latch_id")),
      core::StopFaultLatchGeneration(
          Counter(value, "expected_latch_generation", 1U)),
      Counter(value, "expected_recovery_assessment_revision", 1U),
      core::StopCertificateId(Text(value, "expected_stop_certificate_id")),
      core::StopCertificateRevision(
          Counter(value, "expected_stop_certificate_revision", 1U)),
      core::CellId(Text(value, "expected_cell_id")),
      core::RetainedBundleId(Text(value, "expected_retained_bundle_id")),
      core::GraphRevision(Text(value, "expected_graph_revision")),
      std::move(expectations),
      std::move(reason)};
}

[[nodiscard]] core::RuntimeScaleControlSelectorDTO
DecodeRuntimeScaleControlSelector(const CanonicalValue &value) {
  if (value.find_object_member("group_targets") == nullptr) {
    RequireExactObjectSize(value, 3U, "runtime-scale control selector");
    return core::RuntimeScaleControlSelectorDTO{
        core::ProgramBindingId(Text(value, "program_binding_id")),
        core::ComponentRoleId(Text(value, "component_role_id")),
        core::TargetRoleId(Text(value, "target_role_id"))};
  }
  RequireExactObjectSize(value, 2U, "runtime-scale group selector");
  const auto &targets = Array(value, "group_targets");
  Require(targets.size() >= 2U &&
              targets.size() <= core::kMaxOperationParticipants,
          "runtime-scale group selector target count is out of bounds");
  std::vector<core::RuntimeScaleControlTargetDTO> decoded;
  decoded.reserve(targets.size());
  for (const auto &target : targets) {
    RequireExactObjectSize(target, 2U, "runtime-scale group target");
    core::RuntimeScaleControlTargetDTO item{
        core::ComponentRoleId(Text(target, "component_role_id")),
        core::TargetRoleId(Text(target, "target_role_id"))};
    Require(std::none_of(decoded.begin(), decoded.end(),
                         [&](const auto &prior) { return prior == item; }),
            "runtime-scale group selector contains duplicate targets");
    decoded.push_back(std::move(item));
  }
  return core::RuntimeScaleControlSelectorDTO{
      core::ProgramBindingId(Text(value, "program_binding_id")),
      core::ComponentRoleId(""), core::TargetRoleId(""), std::move(decoded)};
}

[[nodiscard]] core::RuntimeScaleControlRequestDTO
DecodeRuntimeScaleControlRequest(
    const CanonicalValue &value,
    const core::RequestAttributionDTO &attribution) {
  RequireExactObjectSize(value, 4U, "runtime-scale control request");
  const double scale = Number(value, "scale");
  Require(scale >= 0.0 && scale <= 1.0,
          "runtime scale must be in the closed interval [0, 1]");
  return core::RuntimeScaleControlRequestDTO{
      DecodeRequestContext(Member(value, "request_context")),
      DecodeRuntimeScaleControlSelector(Member(value, "selector")),
      core::RuntimeScaleControlGeneration(
          Counter(value, "expected_control_generation", 0U)),
      scale, attribution};
}

[[nodiscard]] core::RecoveryAction
DecodeRecoveryAction(const std::string_view value) {
  if (value == "retry_motion_from_current_state") {
    return core::RecoveryAction::kRetryMotionFromCurrentState;
  }
  if (value == "acknowledge_and_continue") {
    return core::RecoveryAction::kAcknowledgeAndContinue;
  }
  if (value == "retry_after_scene_repair") {
    return core::RecoveryAction::kRetryAfterSceneRepair;
  }
  if (value == "replan_from_stopped_state") {
    return core::RecoveryAction::kReplanFromStoppedState;
  }
  if (value == "reset_cycle") {
    return core::RecoveryAction::kResetCycle;
  }
  Fail("operation recovery action is unsupported");
}

[[nodiscard]] core::KernelRecoveryActionRequestDTO
DecodeOperationRecoveryRequest(const CanonicalValue &value) {
  RequireExactObjectSize(value, 10U, "operation recovery request");
  return core::KernelRecoveryActionRequestDTO{
      Text(value, "request_id"),
      DecodeRecoveryAction(Text(value, "action")),
      Text(value, "principal_id", true),
      Counter(value, "expected_assessment_revision", 1U),
      Counter(value, "expected_control_command_epoch", 0U),
      Counter(value, "expected_execution_id", 1U),
      Text(value, "expected_latch_id", true),
      Text(value, "expected_stop_certificate_id", true),
      Counter(value, "expected_stop_owner_generation", 0U),
      Text(value, "reason", true)};
}

/// Commands whose addressed-Kernel entry takes its submission ordering. A
/// motion-preview session (and pick/place planning built on one) retains that
/// ordering for its whole planning transaction, so these commands run on the
/// submission-ordered lane, FIFO in accept order, and never occupy the
/// ordinary lane that serves Stop status, Reset, events, cell admission and
/// operator-alarm attention.
[[nodiscard]] bool IsSubmissionOrdered(const CommandKind kind) noexcept {
  switch (kind) {
  case CommandKind::kPreviewMotion:
  case CommandKind::kPlanPick:
  case CommandKind::kPlanPlace:
  case CommandKind::kPreviewCapabilityCommand:
  case CommandKind::kReadManualHandover:
  case CommandKind::kSubmit:
  case CommandKind::kSetRuntimeScaleControl:
    return true;
  default:
    return false;
  }
}

/// Ordinary projections that first read the live Scene revision from Manager.
/// That round trip waits on Manager (for example during an Apply), so it runs
/// on the Scene-read lane and never occupies the ordinary lane.
[[nodiscard]] bool ReadsManagerScene(const CommandKind kind) noexcept {
  return kind == CommandKind::kIdentity || kind == CommandKind::kStatus ||
         kind == CommandKind::kTargets || kind == CommandKind::kControlState;
}

/// In-memory Stop-progression reads of the stable Kernel. They are served
/// beside Stop on the independent control socket, never on the ordinary one.
[[nodiscard]] bool IsStopLaneRead(const CommandKind kind) noexcept {
  return kind == CommandKind::kReadStop || kind == CommandKind::kGetStopFault;
}

/// Every Stop form preempts advisory planning before it is dispatched: a
/// preview is effect-free and re-requestable, and containment never waits
/// for one. An accepted Reset preempts it after acceptance (a refused Reset
/// changed nothing a preview depends on).
[[nodiscard]] bool PreemptsPlanning(const CommandKind kind) noexcept {
  switch (kind) {
  case CommandKind::kStop:
  case CommandKind::kStopProgramRun:
  case CommandKind::kStopProgramSelection:
  case CommandKind::kFullStop:
  case CommandKind::kMalformedStop:
    return true;
  default:
    return false;
  }
}

/// A component-lifecycle claim is a whole-cell configuration mutation: the
/// Kernel serializes it with the submission lock a preview session retains
/// for its whole planning time. Like Stop, it preempts advisory planning
/// before it is dispatched instead of waiting behind it. Other claim kinds
/// never take that lock.
[[nodiscard]] bool PreemptsPlanning(
    const component_manager::detail::KernelCellAdmissionRpcPacket
        &packet) noexcept {
  const auto *intent =
      std::get_if<core::CellAdmissionClaimIntentDTO>(&packet.request);
  return intent != nullptr &&
         intent->claim_kind == core::CellAdmissionClaimKindDTO::kComponentLifecycle;
}

[[nodiscard]] Command DecodeCommand(std::string_view bytes) {
  Require(!bytes.empty() && bytes.size() <= kMaximumPacketBytes,
          "execution command packet is empty or oversized");
  CanonicalValue root;
  try {
    root = deployment::parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception &error) {
    Fail("execution command is not strict JSON: " + std::string(error.what()));
  }
  Require(content_identity::canonical_json(root) == bytes,
          "execution command must use canonical JSON encoding");
  Require(root.is_object(), "execution command root must be an object");
  Require(Text(root, "api_version") == kCommandApiVersion,
          "execution command API version is unsupported");
  const auto command = Text(root, "command");
  if (command == "stop_program_selection" ||
      command == "start_program_selection" ||
      command == "release_program_selection") {
    const bool start = command == "start_program_selection";
    const bool release = command == "release_program_selection";
    RequireExactObjectSize(root, (start || release) ? 5U : 4U,
                           "Program selection command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Program selection requires internal owner attribution");
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 6U, "Program selection request");
    Require(Text(request, "schema_version") ==
                "manyforge.program_execution_selection.v2",
            "Program selection request schema is unsupported");
    Command decoded{start   ? CommandKind::kStartProgramSelection
                    : release ? CommandKind::kReleaseProgramSelection
                              : CommandKind::kStopProgramSelection};
    decoded.program_selection = core::ProgramExecutionSelectionRequestDTO{
        DecodeRequestContext(Member(request, "request_context")),
        core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
        core::CellAdmissionClaimGeneration(
            Counter(request, "claim_generation", 1U)),
        core::ProgramRunId(Text(request, "program_run_id")),
        TextArray(request, "group_ids", core::kMaxProgramExecutionGroups),
        attribution};
    if (start || release)
      decoded.expected_selection_revision =
          Counter(root, "expected_selection_revision", 0U);
    return decoded;
  }
  if (command == "check_program_node_motion_attention") {
    RequireExactObjectSize(root, 4U, "Program node motion-attention check");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Program node check requires internal owner attribution");
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 6U, "Program node check request");
    Require(Text(request, "schema_version") ==
                "manyforge.program_node_motion_attention_check.v1",
            "Program node check request schema is unsupported");
    Command decoded{CommandKind::kCheckProgramNodeMotionAttention};
    decoded.program_node_attention = core::ProgramNodeMotionAttentionCheckDTO{
        DecodeRequestContext(Member(request, "request_context")),
        core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
        core::CellAdmissionClaimGeneration(
            Counter(request, "claim_generation", 1U)),
        core::ProgramRunId(Text(request, "program_run_id")),
        core::ProgramNodeInstanceId(Text(request, "node_instance_id")),
        attribution};
    return decoded;
  }
  if (command == "read_program_execution_scope") {
    RequireExactObjectSize(root, 3U, "Program execution scope read");
    Command decoded{CommandKind::kReadProgramExecutionScope};
    decoded.program_run_id = core::ProgramRunId(Text(root, "program_run_id"));
    return decoded;
  }
  if (command == "register_program_run_capture") {
    RequireExactObjectSize(root, 3U, "Program capture registration command");
    const auto &capture = Member(root, "capture");
    RequireExactObjectSize(capture, 10U, "Program capture registration");
    Require(Text(capture, "schema_version") ==
                "manyforge.program_run_execution_capture.v1",
            "Program capture registration schema is unsupported");
    Command decoded{CommandKind::kRegisterProgramCapture};
    decoded.program_capture = core::ProgramRunExecutionCaptureDTO{
        core::RequestOwnerEpoch(Text(capture, "request_owner_epoch")),
        core::RequestOwnerEpoch(Text(capture, "program_runner_owner_epoch")),
        core::CellAdmissionClaimGeneration(
            Counter(capture, "claim_generation", 1U)),
        core::ProgramRunId(Text(capture, "program_run_id")),
        core::ProgramId(Text(capture, "program_id")),
        core::ProgramRevision(Text(capture, "program_revision")),
        core::ProgramBindingId(Text(capture, "program_binding_id")),
        core::RetainedBundleId(Text(capture, "retained_bundle_id")),
        core::GraphRevision(Text(capture, "graph_revision")),
        {}};
    return decoded;
  }
  if (command == "status") {
    RequireExactObjectSize(root, 2U, "status command");
    return Command{CommandKind::kStatus};
  }
  if (command == "identity") {
    RequireExactObjectSize(root, 2U, "identity command");
    return Command{CommandKind::kIdentity};
  }
  if (command == "targets") {
    RequireExactObjectSize(root, 2U, "targets command");
    return Command{CommandKind::kTargets};
  }
  if (command == "query_target") {
    RequireExactObjectSize(root, 3U, "target observation command");
    const auto &query = Member(root, "query");
    const auto kind = Text(query, "kind");
    if (kind == "frame_transform") {
      RequireExactObjectSize(query, 7U, "frame transform query");
      Require(Text(query, "schema_version") ==
                  "manyforge.target_observation_query.v1",
              "target observation query schema is unsupported");
      const auto &pose = Member(query, "pose");
      const auto &source = Member(query, "source_frame");
      const auto &target = Member(query, "target_frame");
      Command decoded{CommandKind::kQueryTarget};
      decoded.target_query = TargetObservationQuery{
          core::RequestOwnerEpoch(Text(query, "owner_epoch")),
          core::ProgramBindingId(Text(query, "program_binding_id")),
          core::TargetRoleId(std::string{}),
          std::nullopt,
          std::nullopt,
          FrameTransformQuery{DecodePose(pose), DecodeReference(source, true),
                              DecodeReference(target, true), pose, source,
                              target}};
      return decoded;
    }
    Require(kind == "frame_pose" || kind == "gripper_status",
            "target observation kind is unsupported");
    if (kind == "frame_pose") {
      Require(query.as_object().size() == 6U || query.as_object().size() == 7U,
              "target observation query has an unexpected field count");
    } else {
      RequireExactObjectSize(query, 5U, "target observation query");
    }
    Require(Text(query, "schema_version") ==
                "manyforge.target_observation_query.v1",
            "target observation query schema is unsupported");
    const auto relative_frame =
        std::find_if(query.as_object().begin(), query.as_object().end(),
                     [](const auto &member) {
                       return member.first == "relative_to_frame_role_id";
                     });
    Command decoded{CommandKind::kQueryTarget};
    decoded.target_query = TargetObservationQuery{
        core::RequestOwnerEpoch(Text(query, "owner_epoch")),
        core::ProgramBindingId(Text(query, "program_binding_id")),
        core::TargetRoleId(Text(query, "target_role_id")),
        kind == "frame_pose"
            ? std::optional(core::FrameRoleId(Text(query, "frame_role_id")))
            : std::nullopt,
        kind == "frame_pose" && relative_frame != query.as_object().end()
            ? std::optional(
                  core::FrameRoleId(Text(query, "relative_to_frame_role_id")))
            : std::nullopt};
    return decoded;
  }
  if (command == "query_operation") {
    RequireExactObjectSize(root, 3U, "Program operation query command");
    const auto &query = Member(root, "query");
    RequireExactObjectSize(query, 5U, "Program operation query");
    Require(Text(query, "schema_version") ==
                "manyforge.program_operation_query.v1",
            "Program operation query schema is unsupported");
    Command decoded{CommandKind::kQueryOperation};
    decoded.operation_query = ProgramOperationQuery{
        core::RequestOwnerEpoch(Text(query, "owner_epoch")),
        core::ProgramBindingId(Text(query, "program_binding_id")),
        core::ProgramRunId(Text(query, "program_run_id")),
        core::TargetRoleId(Text(query, "target_role_id"))};
    return decoded;
  }
  if (command == "control_state") {
    RequireExactObjectSize(root, 2U, "control-state command");
    return Command{CommandKind::kControlState};
  }
  if (command == "observation") {
    Require(root.as_object().size() == 2U || root.as_object().size() == 5U,
            "observation command has an unexpected field count");
    Command decoded{CommandKind::kObservation};
    if (root.as_object().size() == 5U) {
      decoded.observation_cursor_supplied = true;
      const auto owner_epoch = OptionalText(root, "after_owner_epoch");
      const auto source_epoch = OptionalText(root, "after_source_epoch");
      const auto source_sequence =
          OptionalCounter(root, "after_source_sequence", 0U);
      const bool initial = !owner_epoch.has_value() &&
                           !source_epoch.has_value() &&
                           !source_sequence.has_value();
      const bool complete = owner_epoch.has_value() &&
                            source_epoch.has_value() &&
                            source_sequence.has_value();
      Require(initial || complete,
              "observation cursor must be wholly null or complete");
      if (complete) {
        decoded.observation_after_owner_epoch =
            core::RequestOwnerEpoch(*owner_epoch);
        decoded.observation_after_source_epoch = *source_epoch;
        decoded.observation_after_source_sequence = *source_sequence;
      }
    }
    return decoded;
  }
  if (command == "events") {
    RequireExactObjectSize(root, 5U, "events command");
    const auto limit = Counter(root, "limit", 1U);
    Require(limit <= kMaximumEventPage,
            "events command exceeds its page bound");
    Command decoded{CommandKind::kEvents};
    decoded.event_owner_epoch =
        core::RequestOwnerEpoch(Text(root, "owner_epoch"));
    decoded.after_sequence = Counter(root, "after_sequence", 0U);
    decoded.event_limit = static_cast<std::size_t>(limit);
    return decoded;
  }
  if (command == "read") {
    RequireExactObjectSize(root, 3U, "read command");
    Command decoded{CommandKind::kRead};
    decoded.operation_id = core::OperationId(Text(root, "operation_id"));
    return decoded;
  }
  if (command == "read_stop") {
    RequireExactObjectSize(root, 4U, "Stop-status read command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Stop-status read requires internal unverified attribution");
    Command decoded{CommandKind::kReadStop};
    decoded.stop_id = core::StopId(Text(root, "stop_id"));
    return decoded;
  }
  if (command == "get_operation_recovery") {
    RequireExactObjectSize(root, 3U, "operation recovery query command");
    Command decoded{CommandKind::kGetOperationRecovery};
    decoded.operation_id = core::OperationId(Text(root, "operation_id"));
    return decoded;
  }
  if (command == "apply_operation_recovery") {
    RequireExactObjectSize(root, 5U, "operation recovery action command");
    Command decoded{CommandKind::kApplyOperationRecovery};
    decoded.operation_id = core::OperationId(Text(root, "operation_id"));
    decoded.expected_operation_revision = core::OperationRevision(
        Counter(root, "expected_operation_revision", 1U));
    decoded.operation_recovery =
        DecodeOperationRecoveryRequest(Member(root, "request"));
    return decoded;
  }
  if (command == "get_runtime_scale_control") {
    RequireExactObjectSize(root, 3U, "runtime-scale control read command");
    Command decoded{CommandKind::kGetRuntimeScaleControl};
    decoded.runtime_scale_control_selector =
        DecodeRuntimeScaleControlSelector(Member(root, "selector"));
    return decoded;
  }
  if (command == "submit") {
    RequireExactObjectSize(root, 4U, "submit command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kSubmit};
    decoded.operation = DecodeOperation(Member(root, "operation"), attribution);
    return decoded;
  }
  if (command == "preview_motion" || command == "read_manual_handover") {
    RequireExactObjectSize(root, 4U, "motion preview command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    const auto &operation = Member(root, "operation");
    Command decoded{command == "preview_motion"
                        ? CommandKind::kPreviewMotion
                        : CommandKind::kReadManualHandover};
    decoded.operation = DecodeOperation(operation, attribution);
    decoded.operation_digest =
        core::ContentDigest(content_identity::sha256_digest(
            content_identity::canonical_json(operation)));
    return decoded;
  }
  if (command == "preview_capability_command") {
    RequireExactObjectSize(root, 4U, "capability-command preview command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    const auto &operation = Member(root, "operation");
    Command decoded{CommandKind::kPreviewCapabilityCommand};
    decoded.operation = DecodeOperation(operation, attribution);
    decoded.operation_digest =
        core::ContentDigest(content_identity::sha256_digest(
            content_identity::canonical_json(operation)));
    return decoded;
  }
  if (command == "plan_pick") {
    RequireExactObjectSize(root, 4U, "pick planning command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kPlanPick};
    decoded.pick_planning =
        DecodePickPlanningIntent(Member(root, "intent"), attribution);
    return decoded;
  }
  if (command == "plan_place") {
    RequireExactObjectSize(root, 4U, "place planning command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kPlanPlace};
    decoded.place_planning =
        DecodePlacePlanningIntent(Member(root, "intent"), attribution);
    return decoded;
  }
  if (command == "stop") {
    RequireExactObjectSize(root, 4U, "Stop command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kStop};
    decoded.stop = DecodeStopIntent(Member(root, "request"), attribution);
    return decoded;
  }
  if (command == "stop_program_run") {
    RequireExactObjectSize(root, 4U, "Program-run Stop command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Program-run Stop requires internal unverified attribution");
    Command decoded{CommandKind::kStopProgramRun};
    decoded.stop =
        DecodeProgramRunStopIntent(Member(root, "request"), attribution);
    return decoded;
  }
  if (command == "release_program_run_stop_fence") {
    RequireExactObjectSize(root, 6U, "Program-run Stop fence release command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Program-run Stop fence release requires internal attribution");
    Command decoded{CommandKind::kReleaseProgramRunStopFence};
    decoded.program_run_id = core::ProgramRunId(Text(root, "program_run_id"));
    decoded.stop_id = core::StopId(Text(root, "stop_id"));
    decoded.stop_generation =
        core::StopGeneration(Counter(root, "stop_generation", 1U));
    return decoded;
  }
  if (command == "full_stop") {
    RequireExactObjectSize(root, 4U, "Full Stop command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kFullStop};
    decoded.full_stop =
        DecodeFullStopIntent(Member(root, "request"), attribution);
    return decoded;
  }
  if (command == "malformed_stop") {
    RequireExactObjectSize(root, 6U, "recognized malformed Stop command");
    const auto route = Text(root, "recognized_route");
    Require(route == "/api/execution/stops" ||
                route == "/api/execution/full-stop",
            "recognized malformed Stop command names an unknown route");
    Command decoded{CommandKind::kMalformedStop};
    decoded.malformed_stop = MalformedStopCommand{
        core::RequestId(Text(root, "request_id")),
        DecodeRequestAttribution(Member(root, "attribution")), route,
        Text(root, "reason")};
    return decoded;
  }
  if (command == "get_stop_fault") {
    RequireExactObjectSize(root, 2U, "Stop-fault query command");
    return Command{CommandKind::kGetStopFault};
  }
  if (command == "recover_stop_fault") {
    RequireExactObjectSize(root, 3U, "Stop-fault recovery command");
    Command decoded{CommandKind::kRecoverStopFault};
    decoded.stop_fault_recovery =
        DecodeStopFaultRecoveryIntent(Member(root, "request"));
    return decoded;
  }
  if (command == "set_operation_runtime_scale") {
    RequireExactObjectSize(root, 4U, "operation runtime-scale command");
    const auto &value = Member(root, "request");
    RequireExactObjectSize(value, 5U, "operation runtime-scale request");
    const double scale = Number(value, "scale");
    Require(scale > 0.0 && scale <= 1.0,
            "node runtime scale must be in (0, 1]");
    Command decoded{CommandKind::kSetOperationRuntimeScale};
    decoded.operation_runtime_scale = core::RuntimeScaleRequestDTO{
        DecodeRequestContext(Member(value, "request_context")),
        core::OperationId(Text(value, "operation_id")),
        core::OperationRevision(
            Counter(value, "expected_operation_revision", 1U)),
        core::RuntimeScaleGeneration(
            Counter(value, "expected_runtime_scale_generation", 0U)),
        scale,
        DecodeRequestAttribution(Member(root, "attribution"))};
    return decoded;
  }
  if (command == "set_runtime_scale_control") {
    RequireExactObjectSize(root, 4U, "runtime-scale control command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kSetRuntimeScaleControl};
    decoded.runtime_scale_control =
        DecodeRuntimeScaleControlRequest(Member(root, "request"), attribution);
    return decoded;
  }
  CommandKind kind;
  if (command == "reset") {
    kind = CommandKind::kReset;
  } else if (command == "acknowledge_completed") {
    kind = CommandKind::kAcknowledgeCompleted;
  } else {
    Fail("execution command kind is unsupported");
  }
  RequireExactObjectSize(root, 4U, "operation control command");
  const auto attribution =
      DecodeRequestAttribution(Member(root, "attribution"));
  Command decoded{kind};
  decoded.control =
      DecodeOperationControlRequest(Member(root, "request"), attribution);
  return decoded;
}

[[nodiscard]] core::ProjectionRevisionDTO
DecodeProjectionRevision(const CanonicalValue &value) {
  RequireExactObjectSize(value, 3U, "projection revision");
  core::ProjectionRevisionDTO result{
      core::RetainedBundleId(Text(value, "retained_bundle_id")),
      core::GraphRevision(Text(value, "graph_revision")),
      DecodeSceneRevision(Member(value, "scene_revision"))};
  Require(result.valid(), "projection revision is invalid");
  return result;
}

[[nodiscard]] core::ErrorDTO DecodeManagerError(const CanonicalValue &value) {
  Require(value.is_object() && (value.as_object().size() == 2U ||
                                value.as_object().size() == 3U),
          "Manager error has an unknown or missing field");
  const auto code = Text(value, "code");
  const auto message = Text(value, "message");
  std::map<std::string, std::string> details;
  if (const auto *encoded_details = value.find_object_member("details")) {
    Require(encoded_details->is_object(),
            "Manager error details must be an object");
    for (const auto &[key, encoded_value] : encoded_details->as_object()) {
      const auto *text = std::get_if<std::string>(&encoded_value.storage());
      Require(!key.empty() && text != nullptr,
              "Manager error detail is invalid");
      details.emplace(key, *text);
    }
  }
  return {code, message, std::move(details)};
}

[[nodiscard]] CanonicalValue CanonicalCodecValue(const std::string_view bytes,
                                                 const std::string_view label) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  Require(content_identity::canonical_json(parsed.root()) == bytes,
          std::string(label) + " codec did not return canonical JSON");
  return parsed.root();
}

[[nodiscard]] core::RequestReplayStatusDTO
DecodeRequestReplayStatus(const CanonicalValue &root,
                          const std::string_view name) {
  const auto value = Text(root, name);
  if (value == "accepted_new") {
    return core::RequestReplayStatusDTO::kAcceptedNew;
  }
  if (value == "replayed_exact") {
    return core::RequestReplayStatusDTO::kReplayedExact;
  }
  if (value == "request_result_expired") {
    return core::RequestReplayStatusDTO::kRequestResultExpired;
  }
  if (value == "request_id_conflict") {
    return core::RequestReplayStatusDTO::kRequestIdConflict;
  }
  if (value == "request_ledger_full") {
    return core::RequestReplayStatusDTO::kRequestLedgerFull;
  }
  if (value == "stale_request_owner_epoch") {
    return core::RequestReplayStatusDTO::kStaleRequestOwnerEpoch;
  }
  Fail("Manager scene replay status is unsupported");
}

[[nodiscard]] core::SceneProjectionCommitStatusDTO
DecodeSceneProjectionCommitStatus(const CanonicalValue &root,
                                  const std::string_view name) {
  const auto value = Text(root, name);
  if (value == "not_attempted") {
    return core::SceneProjectionCommitStatusDTO::kNotAttempted;
  }
  if (value == "committed") {
    return core::SceneProjectionCommitStatusDTO::kCommitted;
  }
  if (value == "rejected_rollback_proven") {
    return core::SceneProjectionCommitStatusDTO::kRejectedRollbackProven;
  }
  if (value == "failed_projection_uncertain") {
    return core::SceneProjectionCommitStatusDTO::kFailedProjectionUncertain;
  }
  Fail("Manager scene projection status is unsupported");
}

[[nodiscard]] core::SceneUpdateStatusDTO
DecodeSceneUpdateStatus(const CanonicalValue &value) {
  const auto *text = std::get_if<std::string>(&value.storage());
  Require(text != nullptr, "Manager scene update status must be a string");
  if (*text == "geometry_changed") {
    return core::SceneUpdateStatusDTO::kGeometryChanged;
  }
  if (*text == "freshness_only") {
    return core::SceneUpdateStatusDTO::kFreshnessOnly;
  }
  if (*text == "no_op") {
    return core::SceneUpdateStatusDTO::kNoOp;
  }
  Fail("Manager scene update status is unsupported");
}
} // namespace manyforge::roboplan_backend::host_detail
