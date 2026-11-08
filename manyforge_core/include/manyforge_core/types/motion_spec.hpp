#pragma once

#include <manyforge_core/types/program_reference.hpp>

#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/planner_settings.hpp>
#include <manyforge_core/types/robot_defaults.generated.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {

/// How the typed motion payload is interpreted at kernel admission.
enum class MotionTargetModeDTO {
  kAbsolute,
  kRelative,
};

/// Joint goal for motion planning
struct JointGoalDTO {
  /// Exact source-local URDF joint names, scoped by the enclosing target role.
  /// These are authoring semantics, not generated runtime names.
  std::vector<std::string> joint_names;
  std::vector<double> positions_rad;
  friend bool operator==(const JointGoalDTO&, const JointGoalDTO&) = default;
};

/// Kernel-produced joint goal for a resolved planner participant. Generated
/// names are adapter-private and can only appear after exact Program-Binding
/// resolution.
struct ResolvedJointGoalDTO {
  std::vector<ResourceId> resources;
  std::vector<std::string> joint_names;
  std::vector<double> positions_rad;
  friend bool operator==(const ResolvedJointGoalDTO&, const ResolvedJointGoalDTO&) = default;
};

/// Pose goal for motion planning
struct PoseGoalDTO {
  Pose3DTO goal;  // expressed in base_frame
  friend bool operator==(const PoseGoalDTO&, const PoseGoalDTO&) = default;
};

/// Named target reference
struct NamedTargetDTO {
  std::string name;
  friend bool operator==(const NamedTargetDTO&, const NamedTargetDTO&) = default;
};

using ReferenceIdentityDTO = std::variant<std::monostate, ProgramFrameReferenceDTO,
                                          ProgramCollisionBodyReferenceDTO,
                                          SceneResourceId, RuntimeObjectId>;

/// A direct component reference, Scene identity or inline pose.
struct ReferenceDTO {
  enum class Type {
    kComponentFrame,
    kComponentBody,
    kSceneResource,
    kRuntimeObject,
    kPose,
    kCellFrame,
  };
  Type type{Type::kPose};
  ReferenceIdentityDTO id;
  std::optional<Pose3DTO> pose;

  ReferenceDTO() = default;
  ReferenceDTO(Type selected_type, std::string selected_id) : type(selected_type) {
    switch (type) {
      case Type::kSceneResource: id = SceneResourceId(std::move(selected_id)); break;
      case Type::kRuntimeObject: id = RuntimeObjectId(std::move(selected_id)); break;
      case Type::kComponentFrame:
      case Type::kComponentBody:
      case Type::kPose:
      case Type::kCellFrame: id = std::monostate{}; break;
    }
  }
  explicit ReferenceDTO(ProgramFrameReferenceDTO frame)
      : type(Type::kComponentFrame), id(std::move(frame)) {}
  explicit ReferenceDTO(ProgramCollisionBodyReferenceDTO body)
      : type(Type::kComponentBody), id(std::move(body)) {}
  explicit ReferenceDTO(Pose3DTO inline_pose)
      : type(Type::kPose), id(std::monostate{}), pose(std::move(inline_pose)) {}
  friend bool operator==(const ReferenceDTO&, const ReferenceDTO&) = default;
};

[[nodiscard]] inline bool reference_identity_matches_type(const ReferenceDTO& reference) noexcept {
  switch (reference.type) {
    case ReferenceDTO::Type::kComponentFrame: {
      const auto* frame = std::get_if<ProgramFrameReferenceDTO>(&reference.id);
      return frame && !frame->component_id.empty() && !frame->frame_id.empty();
    }
    case ReferenceDTO::Type::kComponentBody: {
      const auto* body = std::get_if<ProgramCollisionBodyReferenceDTO>(&reference.id);
      return body && !body->component_id.empty() && !body->collision_body_id.empty();
    }
    case ReferenceDTO::Type::kSceneResource:
      return std::holds_alternative<SceneResourceId>(reference.id) &&
             !std::get<SceneResourceId>(reference.id).empty();
    case ReferenceDTO::Type::kRuntimeObject:
      return std::holds_alternative<RuntimeObjectId>(reference.id) &&
             !std::get<RuntimeObjectId>(reference.id).empty();
    case ReferenceDTO::Type::kPose:
    case ReferenceDTO::Type::kCellFrame:
      return std::holds_alternative<std::monostate>(reference.id);
  }
  return false;
}

[[nodiscard]] inline std::string_view reference_identity_value(
    const ReferenceDTO& reference) noexcept {
  return std::visit(
      [](const auto& value) -> std::string_view {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::monostate>) return {};
        else if constexpr (std::is_same_v<Value, ProgramFrameReferenceDTO>) return value.frame_id.value();
        else if constexpr (std::is_same_v<Value, ProgramCollisionBodyReferenceDTO>) return value.collision_body_id.value();
        else return value.value();
      }, reference.id);
}

/// Cartesian path goal
/// Cartesian timing (Program `cartesian_timing`, Deployment default).
enum class CartesianSpeedModeDTO {
  /// RoboPlan Bounded: accelerate, hold the TCP speed limit, decelerate per
  /// smooth run; stops at corners sharper than its corner tolerance.
  kConstantSpeed,
  /// RoboPlan TimeOptimal: TOPP-RA within the joint limits, the TCP speed
  /// limit held on its grid.
  kTimeOptimal,
};

[[nodiscard]] inline std::string_view cartesian_speed_mode_name(
    CartesianSpeedModeDTO mode) noexcept {
  return mode == CartesianSpeedModeDTO::kTimeOptimal ? "time_optimal" : "constant_speed";
}

[[nodiscard]] inline constexpr std::optional<CartesianSpeedModeDTO> parse_cartesian_speed_mode(
    std::string_view name) noexcept {
  if (name == "constant_speed") return CartesianSpeedModeDTO::kConstantSpeed;
  if (name == "time_optimal") return CartesianSpeedModeDTO::kTimeOptimal;
  return std::nullopt;
}

/// The Deployment `cartesian_timing` default (manyforge_core/resources/robot_defaults.tsv).
inline constexpr CartesianSpeedModeDTO kDefaultCartesianTiming =
    parse_cartesian_speed_mode(kDefaultCartesianTimingName).value();

/// Joint-space timing of joint and pose goals (Deployment `motion_timing`
/// per robot). Cartesian paths follow `cartesian_timing`.
enum class MotionTimingDTO {
  /// TOPP-RA within the joint limits, then one uniform stretch until the
  /// active TCP stays at 0.98 of its speed limit at the samples.
  kSmooth,
  /// The TCP speed limit held at 0.98 inside TOPP-RA on its grid.
  kFast,
};

[[nodiscard]] inline constexpr std::optional<MotionTimingDTO> parse_motion_timing(
    std::string_view name) noexcept {
  if (name == "smooth") return MotionTimingDTO::kSmooth;
  if (name == "fast") return MotionTimingDTO::kFast;
  return std::nullopt;
}

/// The Deployment `motion_timing` default (manyforge_core/resources/robot_defaults.tsv).
inline constexpr MotionTimingDTO kDefaultMotionTiming =
    parse_motion_timing(kDefaultMotionTimingName).value();

enum class CartesianFallbackPolicyDTO {
  /// The selected backend must implement the full Cartesian-path contract.
  kFailClosed,
  /// Capability negotiation may explicitly compile the final waypoint to a
  /// joint-space pose-goal motion. This changes path semantics and must never
  /// be selected implicitly by an adapter.
  kAllowJointSpaceFallback,
};

struct CartesianPathDTO {
  std::vector<Pose3DTO> waypoints;  // each expressed in base_frame
  double max_translation_step_m = 0.005;
  double max_rotation_step_rad = 0.05;
  /// Absent: the Deployment's `cartesian_timing`.
  std::optional<CartesianSpeedModeDTO> speed_mode{};
  double max_angular_speed_rad_s = 0.5;
  double max_linear_acceleration_mps2 = 0.5;
  double max_angular_acceleration_rad_s2 = 2.5;
  CartesianFallbackPolicyDTO fallback_policy = CartesianFallbackPolicyDTO::kFailClosed;
  /// Corner blend radius (m) per waypoint, empty (every corner exact) or one
  /// entry per waypoint: the corner
  /// at an interior waypoint is rounded by an arc starting and ending this far
  /// from it (clamped to half of each adjacent segment). 0 keeps the exact
  /// corner. The first and last entries are unused.
  std::vector<double> blend_radii_m{};
  friend bool operator==(const CartesianPathDTO&, const CartesianPathDTO&) = default;
};

/// Motion specification as a tagged union
using MotionSpecDTO = std::variant<JointGoalDTO, PoseGoalDTO, NamedTargetDTO, CartesianPathDTO>;

/// Internal absolute/resolution-bound motion consumed by planning. Named
/// targets remain in this transitional union until the program-target map
/// is connected; the addressed boundary never copies an unresolved name.
using ResolvedMotionSpecDTO =
    std::variant<ResolvedJointGoalDTO, PoseGoalDTO, NamedTargetDTO, CartesianPathDTO>;

}  // namespace manyforge::core
