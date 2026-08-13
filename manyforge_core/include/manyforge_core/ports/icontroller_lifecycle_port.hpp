#pragma once

#include <manyforge_core/types/controller_lifecycle.hpp>
#include <stop_token>

namespace manyforge::core {

/// Narrow adapter boundary for one exact retained controller product.
///
/// CCLC owns admission, fencing, reservations, plan choice, persistence,
/// retry/reconciliation, deadlines, and final configuration publication.
class IControllerLifecyclePort {
 public:
  virtual ~IControllerLifecyclePort() = default;

  [[nodiscard]] virtual Result<ControllerLifecycleObservationDTO> Observe(
      const ControllerLifecycleObservationQueryDTO& query) = 0;

  [[nodiscard]] virtual ControllerLifecycleTransitionResultDTO ApplyTransitionStep(
      const ControllerLifecycleTransitionIntentDTO& intent, std::stop_token cancellation) = 0;
};

}  // namespace manyforge::core
