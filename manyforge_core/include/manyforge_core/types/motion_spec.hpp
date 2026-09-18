#pragma once

#include <manyforge_core/types/geometry.hpp>
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

/// Planner-neutral candidate-selection policy. Adapters map these semantics to
/// native algorithms; requests never expose backend-specific flags.
enum class PlanningProfileDTO {
  kFirstFeasible,
  kBalanced,
  kQualityBounded,
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
  };
  Type type{Type::kPose};
  ReferenceIdentityDTO id;
  std::optional<Pose3DTO> pose;

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
        id = std::monostate{};
        break;
    }
  }
  explicit ReferenceDTO(Pose3DTO inline_pose)
      : type(Type::kPose), id(std::monostate{}), pose(std::move(inline_pose)) {}
  friend bool operator==(const ReferenceDTO&, const ReferenceDTO&) = default;
};

[[nodiscard]] inline bool reference_identity_matches_type(const ReferenceDTO& reference) noexcept {
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
enum class CartesianSpeedModeDTO {
  /// Bound Cartesian linear/angular speed and acceleration while tracing.
  kBounded,
};

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
  CartesianSpeedModeDTO speed_mode = CartesianSpeedModeDTO::kBounded;
  double max_angular_speed_rad_s = 0.5;
  double max_linear_acceleration_mps2 = 0.5;
  double max_angular_acceleration_rad_s2 = 2.5;
  CartesianFallbackPolicyDTO fallback_policy = CartesianFallbackPolicyDTO::kFailClosed;
  /// Maximum joint-path corner deviation admitted while smoothing the
  /// Cartesian tracker output. Zero preserves the unsmoothed polyline.
  double max_blend_deviation_rad = 0.01;
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
