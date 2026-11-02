#pragma once

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

/// Joint goal for motion planning (spec §4.1)
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

/// Pose goal for motion planning (spec §4.2)
struct PoseGoalDTO {
  Pose3DTO goal;  // expressed in base_frame
  friend bool operator==(const PoseGoalDTO&, const PoseGoalDTO&) = default;
};

/// Named target reference (spec §4.3)
struct NamedTargetDTO {
  std::string name;
  friend bool operator==(const NamedTargetDTO&, const NamedTargetDTO&) = default;
};

using ReferenceIdentityDTO = std::variant<std::monostate, FrameRoleId, CollisionBodyRoleId,
                                          SceneResourceId, RuntimeObjectId>;

/// Exact robot qualifier for a frame-role reference. The portable target role
/// is the Program authoring identity; the controllable component and command
/// target are its exact Program-Binding resolution, verified by the owner that
/// admits the reference. Absence means the moving participant's own target, so
/// an own-robot frame never aliases an identically named frame on another robot.
struct ReferenceTargetDTO {
  TargetRoleId target_role_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;

  [[nodiscard]] bool complete() const noexcept {
    return !target_role_id.empty() && !controllable_component_id.empty() &&
           !command_target_id.empty();
  }

  friend bool operator==(const ReferenceTargetDTO&, const ReferenceTargetDTO&) = default;
};

/// Portable, target-scoped reference or inline base-frame pose (spec §4.4).
/// The enum and strong identity are deliberately redundant so malformed wire
/// values cannot reinterpret a frame role as a scene resource.
struct ReferenceDTO {
  enum class Type {
    kFrameRole,
    kComponentBodyRole,
    kSceneResource,
    kRuntimeObject,
    kPose,
    /// The retained cell frame. Carries no identity and no pose.
    kCellFrame,
  };
  Type type{Type::kPose};
  ReferenceIdentityDTO id;
  std::optional<Pose3DTO> pose;
  /// Only valid for `kFrameRole`: names the robot that owns the frame role.
  std::optional<ReferenceTargetDTO> target;

  ReferenceDTO() = default;
  ReferenceDTO(Type selected_type, std::string selected_id) : type(selected_type) {
    switch (type) {
      case Type::kFrameRole:
        id = FrameRoleId(std::move(selected_id));
        break;
      case Type::kComponentBodyRole:
        id = CollisionBodyRoleId(std::move(selected_id));
        break;
      case Type::kSceneResource:
        id = SceneResourceId(std::move(selected_id));
        break;
      case Type::kRuntimeObject:
        id = RuntimeObjectId(std::move(selected_id));
        break;
      case Type::kPose:
      case Type::kCellFrame:
        id = std::monostate{};
        break;
    }
  }
  ReferenceDTO(FrameRoleId frame_role_id, ReferenceTargetDTO robot)
      : type(Type::kFrameRole), id(std::move(frame_role_id)), target(std::move(robot)) {}
  explicit ReferenceDTO(Pose3DTO inline_pose)
      : type(Type::kPose), id(std::monostate{}), pose(std::move(inline_pose)) {}
  friend bool operator==(const ReferenceDTO&, const ReferenceDTO&) = default;
};

[[nodiscard]] inline bool reference_identity_matches_type(const ReferenceDTO& reference) noexcept {
  if (reference.target.has_value() &&
      (reference.type != ReferenceDTO::Type::kFrameRole || !reference.target->complete())) {
    return false;
  }
  switch (reference.type) {
    case ReferenceDTO::Type::kFrameRole:
      return std::holds_alternative<FrameRoleId>(reference.id) &&
             !std::get<FrameRoleId>(reference.id).empty();
    case ReferenceDTO::Type::kComponentBodyRole:
      return std::holds_alternative<CollisionBodyRoleId>(reference.id) &&
             !std::get<CollisionBodyRoleId>(reference.id).empty();
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
        if constexpr (std::is_same_v<Value, std::monostate>) {
          return {};
        } else {
          return value.value();
        }
      },
      reference.id);
}

/// Cartesian path goal (spec §4.5)
/// Cartesian timing (Program `cartesian_timing`, Deployment default).
enum class CartesianSpeedModeDTO {
  /// RoboPlan Bounded: accelerate, hold the TCP speed limit, decelerate per
  /// smooth run; stops at corners sharper than its corner tolerance.
  kConstantSpeed,
  /// RoboPlan TimeOptimal: TOPP-RA within the joint limits, the TCP speed
  /// limit held on its grid (RP-3).
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
/// per robot, N12 / RP-3b). Cartesian paths follow `cartesian_timing`.
enum class MotionTimingDTO {
  /// TOPP-RA within the joint limits, then one uniform stretch until the
  /// active TCP stays at 0.98 of its speed limit at the samples (M8).
  kSmooth,
  /// RP-3: the TCP speed limit held at 0.98 inside TOPP-RA on its grid.
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

/// Motion specification as a tagged union (spec §4)
using MotionSpecDTO = std::variant<JointGoalDTO, PoseGoalDTO, NamedTargetDTO, CartesianPathDTO>;

/// Internal absolute/resolution-bound motion consumed by planning. Named
/// targets remain in this transitional union until the I6 program-target map
/// is connected; the addressed boundary never copies an unresolved name.
using ResolvedMotionSpecDTO =
    std::variant<ResolvedJointGoalDTO, PoseGoalDTO, NamedTargetDTO, CartesianPathDTO>;

}  // namespace manyforge::core
