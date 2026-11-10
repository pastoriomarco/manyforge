#pragma once

#include <manyforge_deployment/cell_applicability.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace manyforge::deployment::detail {

struct ApplicabilityOutputCardinality final {
  std::size_t command_claims = 0U;
  std::size_t state_claims = 0U;
  std::size_t logical_rows = 0U;

  friend bool operator==(const ApplicabilityOutputCardinality&,
                         const ApplicabilityOutputCardinality&) = default;
};

/// Allocation-free production budget used before applicability output materialization.
///
/// Every rejected charge leaves the accumulator unchanged. Keeping this seam
/// private lets the exact million-row boundaries be qualified without
/// allocating the very output those boundaries exist to prevent.
class ApplicabilityOutputBudget final {
 public:
  void charge_binding_directions(std::size_t command_claims,
                                 std::size_t state_claims) {
    if (command_claims >
            kMaxExpandedControlInterfaceClaimsPerDirection ||
        state_claims > kMaxExpandedControlInterfaceClaimsPerDirection) {
      throw CellApplicabilityError(
          "per-binding control-interface expansion exceeds 65536 rows");
    }
    if (command_claims >
            kMaxCellExpandedControlInterfaceClaimsPerDirection -
                cardinality_.command_claims ||
        state_claims >
            kMaxCellExpandedControlInterfaceClaimsPerDirection -
                cardinality_.state_claims) {
      throw CellApplicabilityError(
          "cell control-interface expansion exceeds 1000000 rows in one "
          "direction");
    }
    cardinality_.command_claims += command_claims;
    cardinality_.state_claims += state_claims;
  }

  void charge_logical_rows(std::size_t rows,
                           std::string_view context =
                               "cell logical applicability rows") {
    if (rows >
        kMaxCellLogicalApplicabilityRows - cardinality_.logical_rows) {
      throw CellApplicabilityError(std::string(context) +
                                   " exceeds the maximum");
    }
    cardinality_.logical_rows += rows;
  }

  [[nodiscard]] const ApplicabilityOutputCardinality& cardinality()
      const noexcept {
    return cardinality_;
  }

 private:
  ApplicabilityOutputCardinality cardinality_;
};

}  // namespace manyforge::deployment::detail
