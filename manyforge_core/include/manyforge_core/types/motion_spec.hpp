#pragma once

#include <string>
#include <optional>
#include <variant>
#include <vector>

#include <manyforge_core/types/geometry.hpp>

namespace manyforge::core {

/// Joint goal for motion planning (spec §4.1)
struct JointGoalDTO {
  std::vector<std::string> joint_names;
  std::vector<double> positions_rad;
};

/// Pose goal for motion planning (spec §4.2)
struct PoseGoalDTO {
  Pose3DTO goal;  // expressed in base_frame
};

/// Named target reference (spec §4.3)
struct NamedTargetDTO {
  std::string name;
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
};

/// Relative move goal (spec §4.4)
struct RelativeMoveDTO {
  ReferenceDTO target_reference;
  std::optional<ReferenceDTO> offset_reference;
  Pose3DTO offset;  // delta pose expressed in the offset reference frame
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
  CartesianFallbackPolicyDTO fallback_policy =
      CartesianFallbackPolicyDTO::kFailClosed;
};

/// Motion specification as a tagged union (spec §4)
using MotionSpecDTO = std::variant<JointGoalDTO, PoseGoalDTO, NamedTargetDTO, RelativeMoveDTO, CartesianPathDTO>;

}  // namespace manyforge::core
