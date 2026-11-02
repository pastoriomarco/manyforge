#pragma once

#include <cstdint>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/observed_state.hpp>
#include <manyforge_core/types/robot_state.hpp>

namespace manyforge::core {

class IObservedRobotStateProvider {
 public:
  virtual ~IObservedRobotStateProvider() = default;

  virtual Result<RobotStateDTO> current_state(std::int64_t now_ns) const = 0;
  virtual const ObservedRobotStatePublication* publication() const noexcept = 0;

  /// Exact addressed envelope behind the compatibility RobotState projection.
  /// Legacy-only providers return an explicit unavailable error; production
  /// relative-motion admission requires this evidence and never reconstructs
  /// provider epoch/sequence from raw joint arrays.
  virtual Result<ObservedStatePublicationDTO> addressed_publication(
      const ControlBindingId&, const ControlBindingRevision&) const {
    return Result<ObservedStatePublicationDTO>::Err(
        ErrorDTO{kErrorStartStateMismatch,
                 "Addressed observed-state publication is unavailable",
                 {{"reason", "addressed_observed_state_unavailable"}}});
  }
};

}  // namespace manyforge::core
