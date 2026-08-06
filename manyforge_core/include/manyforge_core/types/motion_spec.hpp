#pragma once

#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
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

/// Reference to a frame, object, robot link, or inline pose (spec §4.4)
struct ReferenceDTO {
  enum class Type {
    kFrame,
    kObject,
    kRobotLink,
    kPose,
  };
  Type type;
  std::string id;
  std::optional<Pose3DTO> pose;
  friend bool operator==(const ReferenceDTO&, const ReferenceDTO&) = default;
};

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
  double max_linear_speed_mps = 0.1;
  double max_angular_speed_rad_s = 0.5;
  double max_linear_acceleration_mps2 = 0.5;
  double max_angular_acceleration_rad_s2 = 2.5;
  CartesianFallbackPolicyDTO fallback_policy = CartesianFallbackPolicyDTO::kFailClosed;
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
