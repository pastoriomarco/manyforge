#pragma once

#include <algorithm>
#include <cstdint>

namespace manyforge::core {

/// Age of a sample at a caller's time boundary, both on the receiver's own
/// clock. A sample received after the boundary (the boundary was taken a few
/// milliseconds before the read) is newer than it: its age is 0, never
/// negative. Compares before subtracting, so an extreme receipt cannot
/// overflow.
[[nodiscard]] constexpr std::int64_t received_sample_age_ns(
    const std::int64_t received_ns, const std::int64_t boundary_ns) {
  return received_ns >= boundary_ns ? 0 : boundary_ns - received_ns;
}

/// The one freshness rule of locally received samples: current unless older
/// than `budget_ns` at the boundary; a sample received after the boundary is
/// current.
[[nodiscard]] constexpr bool received_sample_current(
    const std::int64_t received_ns, const std::int64_t boundary_ns,
    const std::int64_t budget_ns) {
  return received_sample_age_ns(received_ns, boundary_ns) <= budget_ns;
}

}  // namespace manyforge::core
