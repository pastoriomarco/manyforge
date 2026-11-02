#pragma once

#include <manyforge_core/types/manager_liveness.hpp>

namespace manyforge::core {

class IManagerLivenessObservationPort {
 public:
  virtual ~IManagerLivenessObservationPort() = default;
  [[nodiscard]] virtual Result<void> Publish(
      const ManagerLivenessObservationDTO& observation) = 0;
};

}  // namespace manyforge::core
