#pragma once

#include <cstddef>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment::detail {

/// Shared construction-time resource accounting for normalized values.
///
/// This is an internal denial-of-service bound, not a schema or trust stage.
/// The complete reconstructed artifact must still pass its exact final output
/// budget.
struct CumulativeNormalizedValueBudget final {
  std::size_t nodes = 0U;
  std::size_t scalar_bytes = 0U;
};

/// Validates one complete value against a fresh fixed v1 output budget.
void validate_canonical_value_output_budget(const CanonicalValue& value);

/// Charges one normalized value to an existing cumulative output budget.
///
/// A failure leaves the budget consumed and the caller must abandon the
/// construction attempt.
void validate_canonical_value_output_budget_cumulatively(
    const CanonicalValue& value, CumulativeNormalizedValueBudget& budget);

}  // namespace manyforge::deployment::detail
