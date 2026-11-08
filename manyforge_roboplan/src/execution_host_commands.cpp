#include "execution_host_commands.hpp"
#include "execution_host_posix.hpp"
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_core/tcp_selector_codec.hpp>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <limits>

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

/// Decode a direct component reference or one of the existing scene forms.
[[nodiscard]] core::ReferenceDTO
DecodeReference(const CanonicalValue &value) {
  Require(value.is_object(), "motion reference must be an object");
  const auto type = Text(value, "type");
  if (type == "cell_frame") {
    RequireExactObjectSize(value, 1U, "cell-frame reference");
    return core::ReferenceDTO(core::ReferenceDTO::Type::kCellFrame, std::string{});
  }
  RequireExactObjectSize(value, 2U, "motion reference");
  if (type == "component_frame") {
    const auto &frame = Member(value, "frame");
    RequireExactObjectSize(frame, 2U, "component frame reference");
    return core::ReferenceDTO(core::ProgramFrameReferenceDTO{
        core::ControllableComponentId(Text(frame, "component_id")),
        core::FrameRoleId(Text(frame, "frame_id"))});
  }
  if (type == "component_body") {
    const auto &body = Member(value, "body");
    RequireExactObjectSize(body, 2U, "component body reference");
    return core::ReferenceDTO(core::ProgramCollisionBodyReferenceDTO{
        core::ControllableComponentId(Text(body, "component_id")),
        core::CollisionBodyRoleId(Text(body, "collision_body_id"))});
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
DecodeOptionalReference(const CanonicalValue &root, const std::string_view name) {
  const auto &value = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(value.storage())) {
    return std::nullopt;
  }
  return DecodeReference(value);
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
    const auto *speed_mode = value.find_object_member("speed_mode");
    // A Cartesian path has nine required fields; `speed_mode` is the one
    // optional tenth field (absent: the Deployment's `cartesian_timing`).
    // Keep the decoder aligned with the frontend/headless CartesianPath
    // contract.
    RequireExactObjectSize(value, speed_mode == nullptr ? 9U : 10U,
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
    std::optional<core::CartesianSpeedModeDTO> mode;
    if (speed_mode != nullptr) {
      mode = core::parse_cartesian_speed_mode(Text(value, "speed_mode"));
      Require(mode.has_value(), "Cartesian speed mode is unsupported");
    }
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
        mode,
        Number(value, "max_angular_speed_rad_s"),
        Number(value, "max_linear_acceleration_mps2"),
        Number(value, "max_angular_acceleration_rad_s2"),
        fallback,
        NumberArray(value, "blend_radii_m", core::kMaxOperationParticipants)};
    Require(result.blend_radii_m.size() == result.waypoints.size(),
            "Cartesian blend radii must give one radius per waypoint");
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

/// The motion's `planner` block: any subset of its members; absent or null
/// members take the Deployment planner default.
[[nodiscard]] core::PlannerOverrideDTO
DecodePlannerOverride(const CanonicalValue &value) {
  Require(value.is_object(), "motion planner must be an object");
  for (const auto &[name, member] : value.as_object()) {
    static_cast<void>(member);
    Require(name == "search" || name == "optimize" || name == "n" ||
                name == "collision_check_step_rad" || name == "max_nodes",
            "motion planner has an unknown field");
  }
  const auto set = [&](std::string_view name) {
    const auto *member = value.find_object_member(name);
    return member != nullptr && !member->is_null();
  };
  const auto u32 = [&](std::string_view name) {
    const auto counter = Counter(value, name, 0U);
    Require(counter <= std::numeric_limits<std::uint32_t>::max(),
            "motion planner " + std::string(name) + " is out of range");
    return static_cast<std::uint32_t>(counter);
  };
  core::PlannerOverrideDTO planner;
  if (set("search")) {
    planner.search = core::planner_search_from_string(Text(value, "search"));
    Require(planner.search.has_value(), "motion planner search is unsupported");
  }
  if (set("optimize")) {
    planner.optimize =
        core::planner_optimize_from_string(Text(value, "optimize"));
    Require(planner.optimize.has_value(),
            "motion planner optimize is unsupported");
  }
  if (set("n")) planner.n = u32("n");
  if (set("collision_check_step_rad"))
    planner.collision_check_step_rad = Number(value, "collision_check_step_rad");
  if (set("max_nodes")) planner.max_nodes = u32("max_nodes");
  Require(core::valid_planner_override(planner),
          "motion planner value is out of range");
  return planner;
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
  // Optional nullable member; absent or null means the Deployment clearance.
  const bool has_clearance_member =
      value.is_object() && value.find_object_member("clearance_m") != nullptr;
  // Optional nullable member; absent or null means the Deployment planner.
  const bool has_planner_member =
      value.is_object() && value.find_object_member("planner") != nullptr;
  Require(value.is_object() &&
              value.as_object().size() ==
                  15U + (has_runtime_object_bindings ? 1U : 0U) +
                      (has_goal_frame_member ? 1U : 0U) +
                      (has_clearance_member ? 1U : 0U) +
                      (has_planner_member ? 1U : 0U),
          "motion payload has an unknown or missing field");
  Require(Text(value, "kind") == "motion",
          "operation payload kind is unsupported");

  // Active TCP: the three-form selector; the older
  // `{target_role_id, frame_role_id}` endpoint is refused typed.
  auto endpoint = core::DecodeTcpSelector(Member(value, "tcp_endpoint"));
  Require(endpoint.ok(), endpoint.ok() ? std::string{} : endpoint.error().message);

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
  const auto planner =
      has_planner_member && !Member(value, "planner").is_null()
          ? DecodePlannerOverride(Member(value, "planner"))
          : core::PlannerOverrideDTO{};
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
      DecodeOptionalReference(value, "target_reference");
  auto offset_reference =
      DecodeOptionalReference(value, "offset_reference");
  auto goal_frame_reference =
      has_goal_frame_member
          ? DecodeOptionalReference(value, "goal_frame_reference")
          : std::nullopt;
  Require(Array(value, "motion_interrupt_scopes").empty(),
          "native motion cannot carry interrupt scopes");

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

  if (joint_goal && !contact_policy.has_value()) {
    Require(runtime_objects.empty(),
            "joint motion runtime-object bindings require an explicit contact policy");
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

  const auto &base_frame = Member(value, "base_frame");
  RequireExactObjectSize(base_frame, 2U, "motion base frame");
  return core::MotionParticipantIntentDTO{
      core::MoveId(Text(value, "move_id")),
      {core::ControllableComponentId(Text(base_frame, "component_id")),
       core::FrameRoleId(Text(base_frame, "frame_id"))},
      std::move(endpoint.value()),
      core::TolerancesDTO{Number(tolerances, "joint_position_tol_rad"),
                          Number(tolerances, "translation_tol_m"),
                          Number(tolerances, "rotation_tol_rad")},
      core::SpeedProfileDTO{Number(speed, "velocity_scale"),
                            Number(speed, "acceleration_scale"),
                            OptionalNumber(speed, "max_tcp_linear_speed_mps")},
      core::TimeoutsDTO{Number(timeouts, "planning_timeout_s"),
                        Number(timeouts, "execution_timeout_s")},
      cache_mode,
      planner,
      target_mode,
      std::move(target_reference),
      std::move(offset_reference),
      std::move(decoded_motion),
      std::move(contact_policy),
      std::move(limits),
      {},
      std::move(runtime_objects),
      std::move(goal_frame_reference),
      has_clearance_member ? OptionalNumber(value, "clearance_m") : std::nullopt};
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
  Require(Text(value, "kind") == "capability_command",
          "operation payload kind is unsupported");
  const auto &command_value = Member(value, "command_value");
  RequireExactObjectSize(command_value, 3U, "capability-command value");
  Require(Text(command_value, "kind") == "gripper_position",
          "capability-command value kind is unsupported");
  std::optional<core::CoordinateEffortDTO> max_effort;
  const auto &effort = Member(command_value, "max_effort");
  if (!std::holds_alternative<std::nullptr_t>(effort.storage())) {
    max_effort = DecodeCoordinateEffort(effort);
  }
  core::GripperPositionCommandValueDTO decoded_value{
      DecodeCoordinatePosition(Member(command_value, "position")),
      std::move(max_effort)};

  RequireExactObjectSize(value, 6U, "capability-command payload");
  const std::optional<double> completion_timeout_s =
      Number(value, "completion_timeout_s");

  const auto &option_values = Array(value, "caller_options");
  Require(option_values.size() <= core::kMaxOperationCommandOptions,
          "capability-command options exceed their fixed bound");
  std::vector<core::CommandOptionDTO> caller_options;
  caller_options.reserve(option_values.size());
  for (const auto &option : option_values) {
    RequireExactObjectSize(option, 2U, "capability-command option");
    core::CommandOptionId option_id(Text(option, "option_id"));
    if (option.find_object_member("value_bool") != nullptr) {
      caller_options.push_back(
          core::CommandOptionDTO{std::move(option_id), Boolean(option, "value_bool")});
    } else {
      caller_options.push_back(
          core::CommandOptionDTO{std::move(option_id), Number(option, "value_s")});
    }
  }

  return core::CapabilityCommandIntentDTO{
      core::CommandSchemaId(Text(value, "command_schema_id")),
      core::CommandSchemaRevision(Text(value, "command_schema_revision")),
      std::move(decoded_value), completion_timeout_s, std::move(caller_options)};
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
  if (kind == "set_tcp") {
    // Active TCP: `{"kind": "set_tcp", "tcp": <selector>}`.
    RequireExactObjectSize(value, 2U, "Set TCP payload");
    auto tcp = core::DecodeTcpSelector(Member(value, "tcp"));
    Require(tcp.ok(), tcp.ok() ? std::string{} : tcp.error().message);
    return core::SetTcpParticipantIntentDTO{std::move(tcp.value())};
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
    RequireExactObjectSize(participant,
                           4U + (execution_scope.has_value() ? 1U : 0U),
                           "operation participant");
    std::optional<core::ActionSpanDTO> action_span;
    const auto &span = Member(participant, "action_span");
    if (!std::holds_alternative<std::nullptr_t>(span.storage())) {
      action_span = DecodeActionSpan(span);
    }
    auto payload = DecodeParticipantPayload(Member(participant, "payload"));
    core::CapabilityId capability(Text(participant, "required_capability_id"));
    const auto &target = Member(participant, "target");
    RequireExactObjectSize(target, 2U, "operation target");
    participants.push_back(core::OperationParticipantIntentDTO{
        {core::ControllableComponentId(Text(target, "component_id")),
         core::CommandTargetId(Text(target, "command_target_id"))},
        std::move(capability), std::move(action_span), std::move(payload),
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
      DecodeOptionalReference(value, "pick_offset_reference"),
      DecodeOptionalReference(value, "approach_offset_reference"),
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
  const auto &carrier = Member(value, "carrier_frame");
  RequireExactObjectSize(carrier, 2U, "pick carrier frame");
  return core::PickSequencePlanningIntentDTO{
      Text(value, "sequence_id"),
      core::SceneResourceId(Text(value, "selected_object_id")),
      core::RuntimeObjectId(Text(value, "selected_runtime_object_id")),
      {core::ControllableComponentId(Text(carrier, "component_id")),
       core::FrameRoleId(Text(carrier, "frame_id"))},
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
  const auto &carrier = Member(value, "carrier_frame");
  RequireExactObjectSize(carrier, 2U, "place carrier frame");
  return core::PlaceSequencePlanningIntentDTO{
      Text(value, "sequence_id"),
      core::SceneResourceId(Text(value, "selected_object_id")),
      {core::ControllableComponentId(Text(carrier, "component_id")),
       core::FrameRoleId(Text(carrier, "frame_id"))},
      DecodeOperation(Member(value, "approach_operation"), attribution),
      DecodeOperation(Member(value, "descend_operation"), attribution),
      DecodeOperation(Member(value, "tool_operation"), attribution),
      DecodeOperation(Member(value, "retreat_operation"), attribution)};
}

[[nodiscard]] core::OperationControlRequestDTO
DecodeOperationControlRequest(const CanonicalValue &value,
                              const core::RequestAttributionDTO &attribution) {
  auto execution_scope = DecodeProgramExecutionScope(value, attribution);
  RequireExactObjectSize(value, 4U + (execution_scope ? 1U : 0U),
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

[[nodiscard]] core::RuntimeScaleControlSelectorDTO
DecodeRuntimeScaleControlSelector(const CanonicalValue &value) {
  if (value.find_object_member("group_targets") == nullptr) {
    RequireExactObjectSize(value, 3U, "runtime-scale control selector");
    return core::RuntimeScaleControlSelectorDTO{
        core::ProgramBindingId(Text(value, "program_binding_id")),
        core::ControllableComponentId(Text(value, "component_id")),
        core::CommandTargetId(Text(value, "command_target_id"))};
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
        core::ControllableComponentId(Text(target, "component_id")),
        core::CommandTargetId(Text(target, "command_target_id"))};
    Require(std::none_of(decoded.begin(), decoded.end(),
                         [&](const auto &prior) { return prior == item; }),
            "runtime-scale group selector contains duplicate targets");
    decoded.push_back(std::move(item));
  }
  return core::RuntimeScaleControlSelectorDTO{
      core::ProgramBindingId(Text(value, "program_binding_id")),
      core::ControllableComponentId(""), core::CommandTargetId(""), std::move(decoded)};
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
  if (value == "close_operation") {
    return core::RecoveryAction::kCloseOperation;
  }
  if (value == "reset_cycle") {
    return core::RecoveryAction::kResetCycle;
  }
  Fail("operation recovery action is unsupported");
}

[[nodiscard]] core::KernelRecoveryActionRequestDTO
DecodeOperationRecoveryRequest(const CanonicalValue &value) {
  const bool has_fresh_motion_attempt =
      value.find_object_member("fresh_motion_attempt") != nullptr;
  RequireExactObjectSize(value, has_fresh_motion_attempt ? 10U : 9U,
                         "operation recovery request");
  return core::KernelRecoveryActionRequestDTO{
      Text(value, "request_id"),
      DecodeRecoveryAction(Text(value, "action")),
      Text(value, "principal_id", true),
      Counter(value, "expected_assessment_revision", 1U),
      Counter(value, "expected_control_command_epoch", 0U),
      Counter(value, "expected_execution_id", 1U),
      Text(value, "expected_stop_certificate_id", true),
      Counter(value, "expected_stop_owner_generation", 0U),
      Text(value, "reason", true),
      has_fresh_motion_attempt ? Boolean(value, "fresh_motion_attempt")
                               : false};
}

/// Commands whose addressed-Kernel entry takes its submission ordering. A
/// motion-preview session (and pick/place planning built on one) retains that
/// ordering for its whole planning transaction, so these commands run on the
/// submission-ordered lane, FIFO in host enqueue order, and never occupy the
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
  case CommandKind::kResetRobotPosition:
  case CommandKind::kResetActiveTcp:
  case CommandKind::kReadActiveTcpOrdered:
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
         kind == CommandKind::kResetRobotPositionCapability ||
         kind == CommandKind::kTargets || kind == CommandKind::kControlState;
}

/// In-memory Stop-progression reads of the stable Kernel. They are served
/// beside Stop on the independent control socket, never on the ordinary one.
[[nodiscard]] bool IsStopLaneRead(const CommandKind kind) noexcept {
  return kind == CommandKind::kReadStop || kind == CommandKind::kReadComponentModel ||
         kind == CommandKind::kComponentRecoveryPreflight ||
         kind == CommandKind::kComponentAction;
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
  case CommandKind::kResetRobotPosition:
    return true;
  default:
    return false;
  }
}

/// A Program reset claim is a whole-cell mutation: the Kernel serializes it
/// with the submission lock a preview session retains for its whole planning
/// time. Like Stop, it preempts advisory planning before it is dispatched
/// instead of waiting behind it. Other claim kinds never take that lock.
[[nodiscard]] bool PreemptsPlanning(
    const component_manager::detail::KernelCellAdmissionRpcPacket
        &packet) noexcept {
  const auto *intent =
      std::get_if<core::CellAdmissionClaimIntentDTO>(&packet.request);
  return intent != nullptr &&
         intent->claim_kind == core::CellAdmissionClaimKindDTO::kProgramReset;
}

namespace {

std::uint64_t IoCounter(const CanonicalValue& object, std::string_view name,
                        std::uint64_t minimum) {
  const auto& value = Member(object, name);
  Require(value.is_string(), "I/O counter must be a canonical decimal string");
  const auto& text = value.as_string();
  Require(!text.empty() && text.size() <= 20U &&
              (text.size() == 1U || text.front() != '0'),
          "I/O counter is not canonical unsigned decimal");
  std::uint64_t result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
              result >= minimum,
          "I/O counter exceeds its unsigned range");
  return result;
}

core::IoWatchRegistrationDTO DecodeIoWatchRegistration(const CanonicalValue& value) {
  RequireExactObjectSize(value, 4U, "I/O watch registration");
  const auto& scope = Member(value, "scope");
  RequireExactObjectSize(scope, 3U, "I/O watch scope");
  const auto& node = Member(scope, "node");
  RequireExactObjectSize(node, 2U, "I/O watch node scope");
  core::ProgramNodeExecutionScopeDTO node_scope{
      core::ProgramNodeInstanceId(Text(node, "node_instance_id")),
      Text(node, "node_attempt_id")};
  Require(core::valid_program_node_execution_scope(node_scope),
          "I/O watch node scope is invalid");
  return {core::RequestOwnerEpoch(Text(value, "owner_epoch")),
          IoCounter(value, "catalog_generation", 1U),
          IoCounter(value, "registration_id", 1U),
          {core::RequestOwnerEpoch(Text(scope, "program_runner_owner_epoch")),
           core::ProgramRunId(Text(scope, "program_run_id")),
           std::move(node_scope)}};
}

core::ProgramIoWatchScopeReleaseRequestDTO
DecodeIoWatchScopeRelease(const CanonicalValue &request) {
  RequireExactObjectSize(request, 8U, "I/O watch scope release");
  Require(Text(request, "schema_version") ==
              "manyforge.io_watch_scope_release.v1",
          "I/O watch scope release schema is unsupported");
  core::ProgramNodeExecutionScopeDTO node{
      core::ProgramNodeInstanceId(Text(request, "node_instance_id")),
      Text(request, "node_attempt_id")};
  Require(core::valid_program_node_execution_scope(node),
          "I/O watch node scope is invalid");
  return {core::RequestOwnerEpoch(Text(request, "owner_epoch")),
          core::ProgramBindingId(Text(request, "program_binding_id")),
          core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
          core::CellAdmissionClaimGeneration(
              Counter(request, "claim_generation", 1U)),
          core::ProgramRunId(Text(request, "program_run_id")),
          std::move(node)};
}

core::IoProgramBlackboardPublicationDTO
DecodeBlackboardPublication(const CanonicalValue &request) {
  RequireExactObjectSize(request, 12U, "Program BB publication");
  Require(Text(request, "schema_version") ==
              "manyforge.program_bbkey_publication.v1",
          "Program BB publication schema is unsupported");
  const auto &claim = Member(request, "claim_generation");
  const auto &run = Member(request, "program_run_id");
  Require(claim.is_null() == run.is_null(),
          "Program BB Run and claim must both be present or both null");
  std::optional<core::CellAdmissionClaimGeneration> claim_generation;
  std::optional<core::ProgramRunId> program_run_id;
  if (!claim.is_null()) {
    claim_generation = core::CellAdmissionClaimGeneration(
        Counter(request, "claim_generation", 1U));
    program_run_id = core::ProgramRunId(Text(request, "program_run_id"));
  }
  const auto kind = Text(request, "kind");
  Require(kind == "baseline" || kind == "update",
          "Program BB publication kind is unsupported");
  std::vector<core::IoBlackboardValueDTO> values;
  std::string previous;
  for (const auto &row : Array(request, "values")) {
    RequireExactObjectSize(row, 3U, "Program BB value");
    auto name = Text(row, "name");
    Require(previous < name,
            "Program BB value names must be unique and canonical-sorted");
    previous = name;
    const auto type = Text(row, "value_type");
    core::IoConditionValueKind value_kind =
        core::IoConditionValueKind::kBoolean;
    if (type == "bool")
      value_kind = core::IoConditionValueKind::kBoolean;
    else if (type == "float64")
      value_kind = core::IoConditionValueKind::kFloat64;
    else if (type == "string")
      value_kind = core::IoConditionValueKind::kString;
    else if (type == "int64")
      value_kind = core::IoConditionValueKind::kInt64;
    else
      Fail("Program BB value type is unsupported");
    std::optional<core::IoConditionValueDTO> value;
    if (Member(row, "value").is_null()) {
      // Truthful unset key: its declared type is retained independently.
    } else if (type == "bool")
      value = core::IoConditionValueDTO{Boolean(row, "value")};
    else if (type == "float64")
      value = core::IoConditionValueDTO{Number(row, "value")};
    else if (type == "string") {
      const auto &text = Member(row, "value");
      Require(text.is_string(), "Program BB string value has the wrong type");
      value = core::IoConditionValueDTO{
          text.as_string()}; // Complete packet bounds the payload.
    } else if (type == "int64") {
      const auto &text = Member(row, "value");
      Require(text.is_string() && !text.as_string().empty() &&
                  text.as_string().size() <= 20U,
              "Program BB int64 must be canonical signed decimal");
      const auto &bytes = text.as_string();
      std::int64_t integer{};
      const auto parsed =
          std::from_chars(bytes.data(), bytes.data() + bytes.size(), integer);
      Require(parsed.ec == std::errc{} &&
                  parsed.ptr == bytes.data() + bytes.size() &&
                  std::to_string(integer) == bytes,
              "Program BB int64 is noncanonical or outside its signed range");
      value = core::IoConditionValueDTO{integer};
    }
    values.push_back({std::move(name), value_kind, std::move(value)});
  }
  return {
      {core::RequestOwnerEpoch(Text(request, "owner_epoch")),
       core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
       Counter(request, "cycle_generation", 1U), std::move(claim_generation),
       std::move(program_run_id),
       core::ProgramBindingId(Text(request, "program_binding_id")),
       core::ActiveBundlePublicationGeneration(
           Counter(request, "active_publication_generation", 1U))},
      kind == "baseline" ? core::IoBlackboardPublicationKind::kBaseline
                         : core::IoBlackboardPublicationKind::kUpdate,
      IoCounter(request, "message_sequence", 1U),
      IoCounter(request, "previous_sequence", 0U),
      std::move(values),
      0}; // Kernel replaces this with the trusted receiver clock at admission.
}

} // namespace

std::vector<core::ProgramMockSimulationSignalDTO>
DecodeProgramMockSimulationSignals(const CanonicalValue& rows) {
  Require(rows.is_array() && !rows.as_array().empty(), "Mock simulation signals must be a nonempty array");
  std::vector<core::ProgramMockSimulationSignalDTO> result;
  result.reserve(rows.as_array().size());
  for (const auto& row : rows.as_array()) {
    RequireExactObjectSize(row, 3U, "Mock simulation row");
    const auto& selector = Member(row, "signal");
    RequireExactObjectSize(selector, 2U, "Mock simulation selector");
    std::optional<core::ProgramMockSimulationChangeDTO> change;
    if (const auto& value = Member(row, "change"); !value.is_null()) {
      RequireExactObjectSize(value, 2U, "Mock simulation change");
      change = core::ProgramMockSimulationChangeDTO{Counter(value, "after_ms", 0U), Boolean(value, "value")};
    }
    core::ProgramMockSimulationSignalDTO next{
        {core::ControllableComponentId(Text(selector, "component_id")),
         Text(selector, "signal_name")}, Boolean(row, "start_value"), change};
    if (!result.empty()) {
      const auto& previous = result.back();
      Require(previous.signal < next.signal,
              "Mock simulation selectors must be sorted and unique");
    }
    result.push_back(std::move(next));
  }
  return result;
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
  if (command == "publish_program_bbkeys") {
    RequireExactObjectSize(root, 4U, "Program BB publication command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Program BB publication requires internal owner attribution");
    Command decoded{CommandKind::kPublishProgramBlackboard};
    decoded.blackboard_publication =
        DecodeBlackboardPublication(Member(root, "request"));
    return decoded;
  }
  if (command == "set_mock_io_run_paused") {
    RequireExactObjectSize(root, 4U, "Run Mock simulation clock command");
    const auto attribution = DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance == core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(), "Run Mock simulation requires internal owner attribution");
    const auto& request = Member(root, "request");
    RequireExactObjectSize(request, 9U, "Run Mock simulation clock request");
    Require(Text(request, "schema_version") == "manyforge.mock_io_run_pause.v1",
            "Run Mock simulation clock schema is unsupported");
    std::vector<core::ProgramNodeInstanceId> held_nodes;
    for (const auto& value : Array(request, "held_node_instance_ids")) {
      const auto* id = std::get_if<std::string>(&value.storage());
      Require(id && !id->empty() && id->size() <= 128U && id->find('\0') == std::string::npos &&
                  (held_nodes.empty() || held_nodes.back().value() < *id),
              "Run Mock simulation held mask must contain canonical unique captured node IDs");
      held_nodes.emplace_back(*id);
    }
    Command decoded{CommandKind::kSetMockIoRunPaused};
    decoded.mock_run_simulation_pause = core::ProgramRunMockSimulationPauseRequestDTO{
        {core::RequestOwnerEpoch(Text(request, "owner_epoch")), core::ProgramBindingId(Text(request, "program_binding_id")),
         core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
         core::CellAdmissionClaimGeneration(Counter(request, "claim_generation", 1U)),
         core::ProgramRunId(Text(request, "program_run_id"))},
        IoCounter(request, "run_revision", 1U), Boolean(request, "paused"), std::move(held_nodes)};
    return decoded;
  }
  if (command == "start_mock_io_node_simulation" || command == "end_mock_io_node_simulation") {
    RequireExactObjectSize(root, 4U, "Node Mock simulation command");
    const auto attribution = DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance == core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(), "Node Mock simulation requires internal owner attribution");
    const auto& request = Member(root, "request");
    const bool ending = command == "end_mock_io_node_simulation";
    RequireExactObjectSize(request, ending ? 9U : 10U, "Node Mock simulation request");
    Require(Text(request, "schema_version") == (ending ? "manyforge.mock_io_node_end.v1" : "manyforge.mock_io_node_start.v2"),
            "Node Mock simulation schema is unsupported");
    core::ProgramNodeExecutionScopeDTO node{core::ProgramNodeInstanceId(Text(request, "node_instance_id")),
                                           Text(request, "node_attempt_id")};
    Require(core::valid_program_node_execution_scope(node), "Node Mock simulation scope is invalid");
    core::ProgramNodeMockSimulationScopeDTO scope{
        core::RequestOwnerEpoch(Text(request, "owner_epoch")), core::ProgramBindingId(Text(request, "program_binding_id")),
        core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
        core::CellAdmissionClaimGeneration(Counter(request, "claim_generation", 1U)),
        core::ProgramRunId(Text(request, "program_run_id")), std::move(node),
        IoCounter(request, "node_start_index", 1U)};
    Command decoded{ending ? CommandKind::kEndMockIoNodeSimulation : CommandKind::kStartMockIoNodeSimulation};
    if (ending) decoded.mock_node_simulation_end = std::move(scope);
    else decoded.mock_node_simulation = core::ProgramNodeMockSimulationRequestDTO{
        std::move(scope), DecodeProgramMockSimulationSignals(Member(request, "signals"))};
    return decoded;
  }
  if (command == "set_mock_io_input") {
    RequireExactObjectSize(root, 4U, "Mock input command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Mock input commands require internal owner attribution");
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 6U, "Mock input request");
    Require(Text(request, "schema_version") == "manyforge.mock_io_input_request.v2",
            "Mock input request schema is unsupported");
    // Validate before nested aggregate construction so refusal unwinds only
    // independent owning locals, preserving the original field order.
    auto owner = core::RequestOwnerEpoch(Text(request, "owner_epoch"));
    auto binding = core::ProgramBindingId(Text(request, "program_binding_id"));
    const auto &signal = Member(request, "signal");
    RequireExactObjectSize(signal, 2U, "Mock input signal");
    core::ProgramSignalReferenceDTO reference{
        core::ControllableComponentId(Text(signal, "component_id")),
        Text(signal, "signal_name")};
    auto source_epoch =
        core::IoSourceIncarnation(Text(request, "expected_source_epoch"));
    const auto value = Boolean(request, "value");
    Command decoded{CommandKind::kSetMockIoInput};
    decoded.mock_io_input_set = MockIoInputSetRequest{
        {std::move(owner), std::move(binding), std::nullopt, {}, {}, {},
         std::move(reference)},
        std::move(source_epoch), value};
    return decoded;
  }
  if (command == "set_carried_object_lost") {
    RequireExactObjectSize(root, 4U, "carried-object-lost signal command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "carried-object-lost signal requires internal owner attribution");
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 7U, "carried-object-lost signal request");
    CarriedObjectLostSetRequest decoded_request{
        core::ControllableComponentId(Text(request, "controllable_component_id")),
        core::CommandTargetId(Text(request, "command_target_id")),
        Boolean(request, "level"),
        OptionalText(request, "carried_object_attachment_id"),
        OptionalText(request, "scene_resource_id"),
        OptionalText(request, "loss_event_id"),
        OptionalText(request, "set_key")};
    const bool identified = decoded_request.carried_object_attachment_id &&
                            decoded_request.scene_resource_id &&
                            decoded_request.loss_event_id;
    const bool anonymous = !decoded_request.carried_object_attachment_id &&
                           !decoded_request.scene_resource_id &&
                           !decoded_request.loss_event_id &&
                           !decoded_request.set_key;
    Require(decoded_request.level ? identified : anonymous,
            "carried-object-lost signal names its attachment, object and loss "
            "event iff the level is true (and a key only then)");
    Command decoded{CommandKind::kSetCarriedObjectLost};
    decoded.carried_object_lost_set = std::move(decoded_request);
    return decoded;
  }
  if (command == "register_io_watch" || command == "read_io_watch" ||
      command == "release_io_watch") {
    RequireExactObjectSize(root, 4U, "I/O watch command");
    const auto attribution = DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance == core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "I/O watch commands require internal owner attribution");
    if (command != "register_io_watch") {
      Command decoded{command == "read_io_watch"
                          ? CommandKind::kReadIoWatch
                          : CommandKind::kReleaseIoWatch};
      if (command == "release_io_watch" &&
          root.find_object_member("scope_request"))
        decoded.io_watch_scope_release =
            DecodeIoWatchScopeRelease(Member(root, "scope_request"));
      else
        decoded.io_watch_registration =
            DecodeIoWatchRegistration(Member(root, "registration"));
      return decoded;
    }
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 10U, "I/O watch request");
    Require(Text(request, "schema_version") == "manyforge.io_watch_request.v2",
            "I/O watch request schema is unsupported");
    const auto& condition = Member(request, "condition");
    RequireExactObjectSize(condition, 2U, "I/O watch condition");
    RequireExactObjectSize(Member(request, "signal"), 2U, "I/O watch signal");
    core::ProgramNodeExecutionScopeDTO node{
        core::ProgramNodeInstanceId(Text(request, "node_instance_id")),
        Text(request, "node_attempt_id")};
    Require(core::valid_program_node_execution_scope(node), "I/O watch node scope is invalid");
    Command decoded{CommandKind::kRegisterIoWatch};
    decoded.io_watch_condition_unsupported = Text(condition, "kind") != "level";
    decoded.io_watch_request = core::ProgramIoWatchRequestDTO{
        core::RequestOwnerEpoch(Text(request, "owner_epoch")),
        core::ProgramBindingId(Text(request, "program_binding_id")),
        core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
        core::CellAdmissionClaimGeneration(Counter(request, "claim_generation", 1U)),
        core::ProgramRunId(Text(request, "program_run_id")), std::move(node),
        {core::ControllableComponentId(Text(Member(request, "signal"), "component_id")),
         Text(Member(request, "signal"), "signal_name")},
        Boolean(condition, "expected_value")};
    return decoded;
  }
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
    const bool node_release = request.find_object_member("node_instance_id") != nullptr;
    Require(!node_release || release,
            "exact node selection is exclusive to individual hold release");
    RequireExactObjectSize(request, node_release ? 7U : 6U, "Program selection request");
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
    if (node_release) {
      Require(decoded.program_selection->group_ids.empty(),
              "individual hold release cannot mix node and group selectors");
      decoded.program_selection->node_instance_id =
          core::ProgramNodeInstanceId(Text(request, "node_instance_id"));
    }
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
  if (command == "read_volatile_motion_trace") {
    RequireExactObjectSize(root, 4U, "Volatile motion trace read");
    (void)DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kReadVolatileMotionTrace};
    decoded.trace_allowance_bytes = Counter(root, "allowance_bytes", 1U);
    Require(decoded.trace_allowance_bytes <= 3U * 1024U * 1024U,
            "volatile motion trace allowance exceeds its bounded range");
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
  if (command == "reset_robot_position_capability") {
    RequireExactObjectSize(root, 2U, "Reset position capability command");
    return Command{CommandKind::kResetRobotPositionCapability};
  }
  if (command == "reset_robot_position") {
    RequireExactObjectSize(root, 4U, "Reset position command");
    const auto &authorization = Member(root, "authorization");
    RequireExactObjectSize(authorization, 9U, "Reset position authorization");
    const auto run = OptionalText(authorization, "program_run_id");
    const auto cycle_claim = OptionalCounter(authorization, "cycle_claim_generation", 1U);
    const auto reference = OptionalText(authorization, "terminal_or_settlement_reference");
    const auto reset_claim = OptionalCounter(authorization, "reset_claim_generation", 1U);
    core::ProgramResetPositionAuthorizationDTO grant{
        core::RequestOwnerEpoch(Text(authorization, "program_runner_owner_epoch")),
        Counter(authorization, "cycle_generation", 1U),
        core::RequestOwnerEpoch(Text(authorization, "request_owner_epoch")),
        core::RequestId(Text(authorization, "request_id")),
        core::ContentDigest(Text(authorization, "request_payload_digest")),
        run ? std::optional{core::ProgramRunId(*run)} : std::nullopt,
        cycle_claim ? std::optional{core::CellAdmissionClaimGeneration(*cycle_claim)} : std::nullopt,
        reference ? std::optional{core::ContentDigest(*reference)} : std::nullopt,
        reset_claim ? std::optional{core::CellAdmissionClaimGeneration(*reset_claim)} : std::nullopt};
    Require(core::ValidateProgramResetPositionAuthorization(grant).ok(),
            "Reset position authorization has an invalid shape");
    const auto &expected = Member(root, "expected_deployment_identity");
    RequireExactObjectSize(expected, 4U, "Reset position expected Deployment");
    Command decoded{CommandKind::kResetRobotPosition};
    decoded.reset_robot_position = ProgramResetPositionRequest{
        std::move(grant), core::RequestOwnerEpoch(Text(expected, "owner_epoch")),
        core::ActiveBundlePublicationGeneration(Counter(expected, "active_publication_generation", 1U)),
        core::RetainedBundleId(Text(expected, "retained_bundle_id")),
        core::GraphRevision(Text(expected, "graph_revision"))};
    return decoded;
  }
  if (command == "targets") {
    RequireExactObjectSize(root, 2U, "targets command");
    return Command{CommandKind::kTargets};
  }
  if (command == "resolve_attach_active_tcp") {
    RequireExactObjectSize(root, 4U, "attach Active TCP query");
    Command decoded{CommandKind::kResolveAttachActiveTcp};
    decoded.attach_binding_id = core::ProgramBindingId(Text(root, "program_binding_id"));
    const auto &target = Member(root, "target");
    RequireExactObjectSize(target, 2U, "attach holder target");
    decoded.attach_holder = core::ProgramTargetReferenceDTO{
        core::ControllableComponentId(Text(target, "component_id")),
        core::CommandTargetId(Text(target, "command_target_id"))};
    return decoded;
  }
  if (command == "read_active_tcp") {
    const bool has_ordered = root.find_object_member("ordered") != nullptr;
    RequireExactObjectSize(root, has_ordered ? 3U : 2U, "Active TCP read command");
    const bool ordered = has_ordered && Boolean(root, "ordered");
    return Command{ordered ? CommandKind::kReadActiveTcpOrdered
                           : CommandKind::kReadActiveTcp};
  }
  if (command == "reset_active_tcp") {
    RequireExactObjectSize(root, 3U, "Active TCP reset command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance == core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Active TCP reset requires internal unverified attribution");
    return Command{CommandKind::kResetActiveTcp};
  }
  if (command == "query_target") {
    RequireExactObjectSize(root, 3U, "target observation command");
    const auto &query = Member(root, "query");
    const auto kind = Text(query, "kind");
    if (kind == "frame_transform") {
      RequireExactObjectSize(query, 7U, "frame transform query");
      Require(Text(query, "schema_version") ==
                  "manyforge.target_observation_query.v2",
              "target observation query schema is unsupported");
      const auto &pose = Member(query, "pose");
      const auto &source = Member(query, "source_frame");
      const auto &target = Member(query, "target_frame");
      Command decoded{CommandKind::kQueryTarget};
      decoded.target_query = TargetObservationQuery{
          core::RequestOwnerEpoch(Text(query, "owner_epoch")),
          core::ProgramBindingId(Text(query, "program_binding_id")),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          FrameTransformQuery{DecodePose(pose), DecodeReference(source),
                              DecodeReference(target), pose, source,
                              target}};
      return decoded;
    }
    if (kind == "io_signal") {
      RequireExactObjectSize(query, 5U, "I/O signal query");
      Require(Text(query, "schema_version") == "manyforge.target_observation_query.v2",
              "target observation query schema is unsupported");
      const auto &signal = Member(query, "signal");
      RequireExactObjectSize(signal, 2U, "I/O signal reference");
      Command decoded{CommandKind::kQueryTarget};
      decoded.target_query = TargetObservationQuery{
          core::RequestOwnerEpoch(Text(query, "owner_epoch")),
          core::ProgramBindingId(Text(query, "program_binding_id")),
          std::nullopt, {}, {}, {},
          core::ProgramSignalReferenceDTO{
              core::ControllableComponentId(Text(signal, "component_id")),
              Text(signal, "signal_name")}};
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
                "manyforge.target_observation_query.v2",
            "target observation query schema is unsupported");
    const auto relative_frame =
        std::find_if(query.as_object().begin(), query.as_object().end(),
                     [](const auto &member) {
                       return member.first == "relative_to_frame";
                     });
    Command decoded{CommandKind::kQueryTarget};
    const auto &target = Member(query, "target");
    RequireExactObjectSize(target, 2U, "observation target");
    const auto decode_frame = [&](const CanonicalValue &frame) {
      RequireExactObjectSize(frame, 2U, "observation frame");
      return core::ProgramFrameReferenceDTO{
          core::ControllableComponentId(Text(frame, "component_id")),
          core::FrameRoleId(Text(frame, "frame_id"))};
    };
    decoded.target_query = TargetObservationQuery{
        core::RequestOwnerEpoch(Text(query, "owner_epoch")),
        core::ProgramBindingId(Text(query, "program_binding_id")),
        core::ProgramTargetReferenceDTO{
            core::ControllableComponentId(Text(target, "component_id")),
            core::CommandTargetId(Text(target, "command_target_id"))},
        kind == "frame_pose" ? std::optional(decode_frame(Member(query, "frame"))) : std::nullopt,
        kind == "frame_pose" && relative_frame != query.as_object().end()
            ? std::optional(decode_frame(Member(query, "relative_to_frame"))) : std::nullopt};
    return decoded;
  }
  if (command == "query_operation") {
    RequireExactObjectSize(root, 3U, "Program operation query command");
    const auto &query = Member(root, "query");
    RequireExactObjectSize(query, 5U, "Program operation query");
    Require(Text(query, "schema_version") ==
                "manyforge.program_operation_query.v2",
            "Program operation query schema is unsupported");
    Command decoded{CommandKind::kQueryOperation};
    const auto &target = Member(query, "target");
    RequireExactObjectSize(target, 2U, "operation query target");
    decoded.operation_query = ProgramOperationQuery{
        core::RequestOwnerEpoch(Text(query, "owner_epoch")),
        core::ProgramBindingId(Text(query, "program_binding_id")),
        core::ProgramRunId(Text(query, "program_run_id")),
        {core::ControllableComponentId(Text(target, "component_id")),
         core::CommandTargetId(Text(target, "command_target_id"))}};
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
  if (command == "read_component_model") {
    RequireExactObjectSize(root, 2U, "component model query command");
    return Command{CommandKind::kReadComponentModel};
  }
  if (command == "component_recovery_preflight") {
    RequireExactObjectSize(root, 3U, "component recovery preflight command");
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 1U, "component recovery preflight request");
    Command decoded{CommandKind::kComponentRecoveryPreflight};
    decoded.component_id = Text(request, "component_id");
    return decoded;
  }
  if (command == "component_action") {
    RequireExactObjectSize(root, 3U, "component action command");
    const auto &request = Member(root, "request");
    const auto *confirmation = request.find_object_member("recovery_preflight");
    RequireExactObjectSize(request, confirmation ? 3U : 2U, "component action request");
    Command decoded{CommandKind::kComponentAction};
    decoded.component_id = Text(request, "component_id");
    const auto action = core::parse_component_action(Text(request, "action"));
    Require(action.has_value(), "component action is not a known action");
    decoded.component_action = *action;
    if (confirmation) {
      Require(*action == core::ComponentAction::kEnable,
              "recovery preflight is only valid for Enable");
      RequireExactObjectSize(*confirmation, 2U, "component recovery confirmation");
      ComponentRecoveryConfirmation selected;
      selected.snapshot_id = Text(*confirmation, "snapshot_id");
      const auto &rows = Member(*confirmation, "confirmed");
      Require(rows.is_array() && !rows.as_array().empty() && rows.as_array().size() <= 2U,
              "recovery confirmation must name the member and optional owner");
      for (const auto &row : rows.as_array()) {
        RequireExactObjectSize(row, 6U, "component recovery fact");
        core::ComponentRecoveryFact fact{Text(row, "component_id"), OptionalText(row, "cause_code"),
                                         OptionalText(row, "vendor_code"), Text(row, "eligibility")};
        fact.stop_code = OptionalText(row, "stop_code");
        fact.stop_vendor_code = OptionalText(row, "stop_vendor_code");
        Require(fact.eligibility == "ready" || fact.eligibility == "recover" ||
                    fact.eligibility == "blocked" || fact.eligibility == "unknown" ||
                    fact.eligibility == "excluded", "unknown component recovery eligibility");
        selected.confirmed.push_back(std::move(fact));
      }
      decoded.recovery_preflight = std::move(selected);
    }
    return decoded;
  }
  if (command == "set_rule_active") {
    RequireExactObjectSize(root, 4U, "set rule active command");
    Command decoded{CommandKind::kSetRuleActive};
    decoded.rule_id = core::InterruptPolicyId(Text(root, "rule_id"));
    decoded.rule_active = Boolean(root, "active");
    return decoded;
  }
  if (command == "reset_rule_activation") {
    RequireExactObjectSize(root, 2U, "reset rule activation command");
    return Command{CommandKind::kResetRuleActivation};
  }
  if (command == "clear_outside_change") {
    RequireExactObjectSize(root, 4U, "clear outside change command");
    Command decoded{CommandKind::kClearOutsideChange};
    decoded.outside_change_output = core::IoSignalId{
        core::ControllableComponentId(Text(root, "controllable_component_id")),
        Text(root, "signal_name")};
    return decoded;
  }
  if (command == "set_io_output") {
    // `signal_id` is `<component_id>/<signal_name>`.
    RequireExactObjectSize(root, 3U, "set I/O output command");
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 3U, "set I/O output request");
    const auto signal_id = Text(request, "signal_id");
    const auto origin = Text(request, "origin");
    Require(origin == "manual" || origin == "program",
            "set I/O output origin is not manual or program");
    const auto slash = signal_id.find('/');
    Require(slash != std::string::npos && slash != 0U && slash + 1U < signal_id.size(),
            "set I/O output signal_id is not <component_id>/<signal_name>");
    const auto &value = Member(request, "value");
    Require(value.is_bool() || value.is_number(),
            "set I/O output value is not a boolean or a number");
    Command decoded{CommandKind::kSetIoOutput};
    decoded.io_output_signal = core::IoSignalId{
        core::ControllableComponentId(signal_id.substr(0U, slash)),
        signal_id.substr(slash + 1U)};
    decoded.io_output_value = value.is_bool() ? std::variant<bool, double>(value.as_bool())
                                              : std::variant<bool, double>(value.as_number());
    decoded.io_output_manual = origin == "manual";
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
  // `{code, message}` plus optional `details` and `producer`; the producer
  // is presentation provenance and is not carried further.
  const auto *producer = value.is_object() ? value.find_object_member("producer") : nullptr;
  const auto *encoded_details = value.is_object() ? value.find_object_member("details") : nullptr;
  Require(value.is_object() &&
              value.as_object().size() ==
                  2U + (producer != nullptr ? 1U : 0U) + (encoded_details != nullptr ? 1U : 0U) &&
              (producer == nullptr || producer->is_string()),
          "Manager error has an unknown or missing field");
  const auto code = Text(value, "code");
  const auto message = Text(value, "message");
  std::map<std::string, std::string> details;
  if (encoded_details != nullptr) {
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
