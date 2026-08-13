#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <stop_token>

namespace manyforge::core {

/// ROS-independent boundary for one exact lifecycle provider occurrence.
///
/// CCLC owns transaction sequencing, retries, deadlines, reservations, and
/// terminal results. A brand adapter exposes normalized observations and
/// executes at most the one semantic primitive selected by CCLC.
class IBrandLifecyclePort {
 public:
  virtual ~IBrandLifecyclePort() = default;

  [[nodiscard]] virtual Result<BrandLifecycleObservationDTO> Observe(
      const BrandLifecycleObservationQueryDTO& query) = 0;

  [[nodiscard]] virtual BrandLifecyclePrimitiveResultDTO Dispatch(
      const BrandLifecyclePrimitiveIntentDTO& intent, std::stop_token cancellation) = 0;
};

}  // namespace manyforge::core
