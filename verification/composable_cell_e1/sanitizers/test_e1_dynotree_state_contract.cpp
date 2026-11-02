#include <gtest/gtest.h>

#include <type_traits>

#include "dynotree/StateSpace.h"

namespace {

using SO2State = dynotree::SO2Squared<double>;
using RnState = dynotree::RnSquared<double, 3>;

static_assert(std::is_aggregate_v<SO2State>);
static_assert(std::is_aggregate_v<RnState>);

TEST(E1DynotreeStateContract, DefaultStateIsDeterministic) {
  const SO2State so2{};
  const RnState rn{};
  const dynotree::RnSquared<double, Eigen::Dynamic> dynamic_rn{};

  EXPECT_TRUE(so2.lb.isZero());
  EXPECT_TRUE(so2.ub.isZero());
  EXPECT_TRUE(rn.lb.isZero());
  EXPECT_TRUE(rn.ub.isZero());
  EXPECT_TRUE(rn.weights.isOnes());
  EXPECT_FALSE(rn.use_weights);
  EXPECT_EQ(dynamic_rn.lb.size(), 0);
  EXPECT_EQ(dynamic_rn.ub.size(), 0);
  EXPECT_EQ(dynamic_rn.weights.size(), 0);
}

TEST(E1DynotreeStateContract, AggregateInitializationRemainsSourceCompatible) {
  const SO2State so2{Eigen::Matrix<double, 1, 1>::Constant(-1.0),
                     Eigen::Matrix<double, 1, 1>::Constant(1.0), {}};
  EXPECT_DOUBLE_EQ(so2.lb[0], -1.0);
  EXPECT_DOUBLE_EQ(so2.ub[0], 1.0);
}

}  // namespace
