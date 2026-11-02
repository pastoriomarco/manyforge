#include <gtest/gtest.h>

#include "cell_applicability_budget_internal.hpp"

#include <cstddef>
#include <limits>

namespace manyforge::deployment::detail {
namespace {

TEST(CellApplicabilityBudget,
     IndependentlyAcceptsOneMillionCommandAndStateClaims) {
  ApplicabilityOutputBudget budget;
  for (std::size_t index = 0U; index < 15U; ++index) {
    budget.charge_binding_directions(
        kMaxExpandedControlInterfaceClaimsPerDirection,
        kMaxExpandedControlInterfaceClaimsPerDirection);
  }
  budget.charge_binding_directions(16960U, 16960U);

  EXPECT_EQ(
      budget.cardinality().command_claims,
      kMaxCellExpandedControlInterfaceClaimsPerDirection);
  EXPECT_EQ(
      budget.cardinality().state_claims,
      kMaxCellExpandedControlInterfaceClaimsPerDirection);
  EXPECT_EQ(budget.cardinality().logical_rows, 0U);
}

TEST(CellApplicabilityBudget,
     RejectsCellDirectionOneMillionAndOneWithoutMutatingTotals) {
  ApplicabilityOutputBudget command_budget;
  for (std::size_t index = 0U; index < 15U; ++index) {
    command_budget.charge_binding_directions(
        kMaxExpandedControlInterfaceClaimsPerDirection, 0U);
  }
  const auto command_before = command_budget.cardinality();
  EXPECT_THROW(command_budget.charge_binding_directions(16961U, 0U),
               CellApplicabilityError);
  EXPECT_EQ(command_budget.cardinality(), command_before);

  ApplicabilityOutputBudget state_budget;
  for (std::size_t index = 0U; index < 15U; ++index) {
    state_budget.charge_binding_directions(
        0U, kMaxExpandedControlInterfaceClaimsPerDirection);
  }
  const auto state_before = state_budget.cardinality();
  EXPECT_THROW(state_budget.charge_binding_directions(0U, 16961U),
               CellApplicabilityError);
  EXPECT_EQ(state_budget.cardinality(), state_before);
}

TEST(CellApplicabilityBudget,
     AcceptsExactLogicalBoundAndRejectsTheNextRowAtomically) {
  ApplicabilityOutputBudget budget;
  budget.charge_logical_rows(kMaxCellLogicalApplicabilityRows - 1U);
  budget.charge_logical_rows(1U);
  EXPECT_EQ(budget.cardinality().logical_rows,
            kMaxCellLogicalApplicabilityRows);

  const auto before = budget.cardinality();
  EXPECT_THROW(budget.charge_logical_rows(1U), CellApplicabilityError);
  EXPECT_EQ(budget.cardinality(), before);
}

TEST(CellApplicabilityBudget,
     RejectsPerBindingAndArithmeticOverflowWithoutPartialCharge) {
  ApplicabilityOutputBudget budget;
  const auto empty = budget.cardinality();

  EXPECT_THROW(
      budget.charge_binding_directions(
          kMaxExpandedControlInterfaceClaimsPerDirection + 1U, 0U),
      CellApplicabilityError);
  EXPECT_EQ(budget.cardinality(), empty);

  EXPECT_THROW(
      budget.charge_binding_directions(
          0U, std::numeric_limits<std::size_t>::max()),
      CellApplicabilityError);
  EXPECT_EQ(budget.cardinality(), empty);

  EXPECT_THROW(
      budget.charge_logical_rows(std::numeric_limits<std::size_t>::max()),
      CellApplicabilityError);
  EXPECT_EQ(budget.cardinality(), empty);
}

}  // namespace
}  // namespace manyforge::deployment::detail
