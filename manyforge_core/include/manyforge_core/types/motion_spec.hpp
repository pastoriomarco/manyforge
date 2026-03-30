#pragma once

#include <string>
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

/// Reference to a frame, object, or robot link (spec §4.4)
struct ReferenceDTO {
  enum class Type {
    kFrame,
    kObject,
    kRobotLink,
  };
  Type type;
  std::string id;
};

/// Relative move goal (spec §4.4)
struct RelativeMoveDTO {
  ReferenceDTO reference;
  Pose3DTO offset;  // T_ref_to_goal, expressed in the reference frame
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
