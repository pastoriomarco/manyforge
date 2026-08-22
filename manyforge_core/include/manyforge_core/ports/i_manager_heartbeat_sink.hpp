#pragma once

#include <manyforge_core/types/manager_liveness.hpp>

namespace manyforge::core {

class IManagerHeartbeatSink {
 public:
  virtual ~IManagerHeartbeatSink() = default;
  [[nodiscard]] virtual Result<void> Renew(const ManagerHeartbeatDTO& heartbeat) = 0;
};

}  // namespace manyforge::core
