#pragma once

#include <string>

#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

class IPlanningContext;

struct CacheCompatibilityResult {
  bool compatible = false;
  std::string reason;
};

CacheCompatibilityResult incompatible_cache(std::string reason);

CacheCompatibilityResult cached_trajectory_matches_request_goal(
    IPlanningContext& context,
    const TrajectoryDTO& trajectory,
    const ExecuteRequestDTO& request);

CacheCompatibilityResult cached_trajectory_matches_current_start(
    const TrajectoryDTO& trajectory,
    const RobotStateDTO& current_state,
    double tolerance_rad);

}  // namespace manyforge::core
