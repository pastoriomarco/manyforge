#pragma once

namespace manyforge::roboplan_backend {

// Advertised upper bound; the exact trajectory preparation may be slower.
[[nodiscard]] inline constexpr double
qualified_runtime_scale_slew_per_second() noexcept {
  return 4.0;
}

} // namespace manyforge::roboplan_backend
