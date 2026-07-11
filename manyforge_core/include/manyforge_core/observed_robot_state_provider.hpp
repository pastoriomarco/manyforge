#pragma once

#include <manyforge_core/ports/iobserved_robot_state_provider.hpp>

namespace manyforge::core {

class PublishedObservedRobotStateProvider final
    : public IObservedRobotStateProvider {
 public:
  explicit PublishedObservedRobotStateProvider(
      const ObservedRobotStatePublication& publication)
      : publication_(&publication) {}

  Result<RobotStateDTO> current_state(std::int64_t now_ns) const override;
  const ObservedRobotStatePublication* publication() const noexcept override {
    return publication_;
  }

 private:
  const ObservedRobotStatePublication* publication_;
};

}  // namespace manyforge::core
