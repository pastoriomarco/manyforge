#pragma once

#include <gtest/gtest.h>

#include <utility>

namespace manyforge::deployment::test {

template <typename Root, typename Projection>
void expect_source_preserving_rvalue_transfers(
    const Root& seed, Projection projection) {
  Root construction_source(seed);
  const auto expected = projection(construction_source);
  Root constructed(std::move(construction_source));

  EXPECT_EQ(projection(construction_source), expected);
  EXPECT_EQ(projection(constructed), expected);

  Root assignment_source(seed);
  Root assigned(seed);
  assigned = std::move(assignment_source);

  EXPECT_EQ(projection(assignment_source), expected);
  EXPECT_EQ(projection(assigned), expected);

  // Reusing both sides through a second rvalue expression proves that neither
  // the construction nor assignment form consumes the qualified source.
  Root reused_source(std::move(construction_source));
  Root reused_destination(seed);
  reused_destination = std::move(constructed);

  EXPECT_EQ(projection(construction_source), expected);
  EXPECT_EQ(projection(reused_source), expected);
  EXPECT_EQ(projection(constructed), expected);
  EXPECT_EQ(projection(reused_destination), expected);
}

}  // namespace manyforge::deployment::test
