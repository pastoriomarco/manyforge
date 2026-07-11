#pragma once

#include <cstdint>

#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/robot_state.hpp>

namespace manyforge::core {

class IObservedRobotStateProvider {
 public:
  virtual ~IObservedRobotStateProvider() = default;

  virtual Result<RobotStateDTO> current_state(std::int64_t now_ns) const = 0;
  virtual const ObservedRobotStatePublication* publication() const noexcept = 0;
};

}  // namespace manyforge::core
