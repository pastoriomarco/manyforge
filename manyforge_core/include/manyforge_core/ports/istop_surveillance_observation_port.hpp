#pragma once

#include <manyforge_core/stop_surveillance.hpp>

namespace manyforge::core {

/// Command-free post-terminal observation seam. Implementations read only the
/// exact immutable source named by the query and contain adapter exceptions in
/// a typed unavailable/invalid sample.
class IStopSurveillanceObservationPort {
 public:
  virtual ~IStopSurveillanceObservationPort() = default;

  [[nodiscard]] virtual StopSurveillanceObservationSampleDTO Observe(
      const StopSurveillanceObservationQueryDTO& query) noexcept = 0;
};

}  // namespace manyforge::core
