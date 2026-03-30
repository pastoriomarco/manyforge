#pragma once

#include <string>

#include <manyforge_core/types/tolerances.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/timeouts.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/motion_spec.hpp>

namespace manyforge::core {

/// Execute request - kernel input (spec §3.10)
struct ExecuteRequestDTO {
  std::string robot_id;
  std::string group_id;
  std::string move_id;
  std::string base_frame;
  std::string tcp_frame;
  TolerancesDTO tolerances;
  SpeedProfileDTO speed_profile;
  TimeoutsDTO timeouts;
  CacheMode cache_mode;
  MotionSpecDTO motion;
};

}  // namespace manyforge::core
