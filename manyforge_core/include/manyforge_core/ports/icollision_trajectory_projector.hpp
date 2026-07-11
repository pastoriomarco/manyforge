#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

class ICollisionTrajectoryProjector {
 public:
  virtual ~ICollisionTrajectoryProjector() = default;

  virtual Result<TrajectoryDTO> expand_for_collision(
      const TrajectoryDTO& trajectory,
      const RobotStateDTO& observed_start_state) const = 0;
};

}  // namespace manyforge::core
