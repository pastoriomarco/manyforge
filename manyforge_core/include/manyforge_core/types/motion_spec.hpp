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
struct CartesianPathDTO {
  std::vector<Pose3DTO> waypoints;  // each expressed in base_frame
  double max_translation_step_m;
  double max_rotation_step_rad;
};

/// Motion specification as a tagged union (spec §4)
using MotionSpecDTO = std::variant<JointGoalDTO, PoseGoalDTO, NamedTargetDTO, RelativeMoveDTO, CartesianPathDTO>;

}  // namespace manyforge::core
