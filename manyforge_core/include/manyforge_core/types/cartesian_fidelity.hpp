#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace manyforge::core {

/// Producer-measured continuous Cartesian-path fidelity for one exact
/// candidate trajectory.  The certified maxima include the unsampled interval
/// bound and are therefore the values compared with the requested tolerances.
struct CartesianFidelityMetricsDTO final {
  double requested_translation_tolerance_m{};
  double requested_rotation_tolerance_rad{};
  double measured_max_translation_error_m{};
  double measured_max_rotation_error_rad{};
  double unsampled_translation_bound_m{};
  double unsampled_rotation_bound_rad{};
  double certified_max_translation_error_m{};
  double certified_max_rotation_error_rad{};

  friend bool operator==(const CartesianFidelityMetricsDTO&,
                         const CartesianFidelityMetricsDTO&) = default;
};

[[nodiscard]] inline bool valid_cartesian_fidelity_metrics(
    const CartesianFidelityMetricsDTO& value) noexcept {
  constexpr double kComparisonTolerance = 1e-12;
  const std::array<double, 8U> numbers{
      value.requested_translation_tolerance_m, value.requested_rotation_tolerance_rad,
      value.measured_max_translation_error_m,  value.measured_max_rotation_error_rad,
      value.unsampled_translation_bound_m,     value.unsampled_rotation_bound_rad,
      value.certified_max_translation_error_m, value.certified_max_rotation_error_rad};
  return std::all_of(numbers.begin(), numbers.end(),
                     [](double item) { return std::isfinite(item); }) &&
         value.requested_translation_tolerance_m > 0.0 &&
         value.requested_rotation_tolerance_rad > 0.0 &&
         value.measured_max_translation_error_m >= 0.0 &&
         value.measured_max_rotation_error_rad >= 0.0 &&
         value.unsampled_translation_bound_m >= 0.0 && value.unsampled_rotation_bound_rad >= 0.0 &&
         value.certified_max_translation_error_m + kComparisonTolerance >=
             value.measured_max_translation_error_m + value.unsampled_translation_bound_m &&
         value.certified_max_rotation_error_rad + kComparisonTolerance >=
             value.measured_max_rotation_error_rad + value.unsampled_rotation_bound_rad &&
         value.certified_max_translation_error_m <=
             value.requested_translation_tolerance_m + kComparisonTolerance &&
         value.certified_max_rotation_error_rad <=
             value.requested_rotation_tolerance_rad + kComparisonTolerance;
}

}  // namespace manyforge::core
