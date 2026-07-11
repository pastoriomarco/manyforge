#pragma once

#include <map>
#include <string>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/validation.hpp>

namespace manyforge::core {

class IRobotKinematics {
 public:
  virtual ~IRobotKinematics() = default;

  virtual Result<Pose3DTO> forward_kinematics(
      const FkRequestDTO& request) const = 0;
  virtual Result<std::map<std::string, Pose3DTO>> forward_kinematics_batch(
      const RobotStateDTO& robot_state,
      const std::vector<std::string>& target_frames,
      const std::string& base_frame) const = 0;
  virtual Result<JointLimitsDTO> joint_limits(
      const std::string& robot_id, const std::string& group_id) const = 0;
  virtual Result<std::vector<std::string>> group_joint_names(
      const std::string& group_id) const = 0;
};

}  // namespace manyforge::core
