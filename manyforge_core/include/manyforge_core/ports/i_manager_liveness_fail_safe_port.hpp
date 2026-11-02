#pragma once

#include <manyforge_core/types/manager_liveness.hpp>

namespace manyforge::core {

class IManagerLivenessFailSafePort {
 public:
  virtual ~IManagerLivenessFailSafePort() = default;
  virtual void LatchAndContain(
      const ManagerLivenessLossIntentDTO& intent) noexcept = 0;
};

}  // namespace manyforge::core
