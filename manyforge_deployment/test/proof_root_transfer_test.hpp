#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
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

template <typename Root, typename SharedProjection>
void expect_immutable_shared_copy_storage(
    const Root& seed, SharedProjection shared_projection) {
  static_assert(std::is_nothrow_copy_constructible_v<Root>);
  static_assert(std::is_nothrow_copy_assignable_v<Root>);
  static_assert(sizeof(Root) == sizeof(std::shared_ptr<const void>));
  static_assert(std::is_lvalue_reference_v<
                decltype(shared_projection(seed))>);

  const auto* const expected =
      std::addressof(shared_projection(seed));

  Root copied(seed);
  EXPECT_EQ(std::addressof(shared_projection(copied)), expected);

  Root construction_source(seed);
  Root constructed(std::move(construction_source));
  EXPECT_EQ(std::addressof(shared_projection(construction_source)),
            expected);
  EXPECT_EQ(std::addressof(shared_projection(constructed)), expected);

  Root assignment_source(seed);
  Root assigned(seed);
  assigned = std::move(assignment_source);
  EXPECT_EQ(std::addressof(shared_projection(assignment_source)), expected);
  EXPECT_EQ(std::addressof(shared_projection(assigned)), expected);
}

}  // namespace manyforge::deployment::test
