#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/trajectory/trajectory_splice.hpp>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

TrajectoryDTO Active() {
  TrajectoryDTO result;
  result.resources = {ResourceId("joint.1")};
  result.joint_names = {"j1"};
  // q(t) = 1 - t + 2t^2, so the cut at 1 has P/V/A = (2,3,4).
  for (const double time : {0.0, 0.3, 0.8, 1.4, 2.0}) {
    result.waypoints.push_back(
        {time, {1.0 - time + 2.0 * time * time}, {-1.0 + 4.0 * time}, {4.0}});
  }
  result.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0};
  return result;
}

TrajectoryDTO Successor() {
  TrajectoryDTO result;
  result.resources = {ResourceId("joint.1")};
  result.joint_names = {"j1"};
  // At entry .4, P/V/A = (4.625,8.875,21).
  for (const double time : {0.0, 0.4, 0.9, 1.7}) {
    const double offset = time - 0.4;
    result.waypoints.push_back(
        {time, {4.625 + 8.875 * offset + 10.5 * offset * offset}, {8.875 + 21.0 * offset}, {21.0}});
  }
  result.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{3.0, 12.0};
  return result;
}

TrajectoryDTO ConstantCurve(const std::vector<double>& times) {
  TrajectoryDTO result;
  result.resources = {ResourceId("joint.1")};
  result.joint_names = {"j1"};
  for (const auto time : times) result.waypoints.push_back({time, {0.0}, {0.0}, {0.0}});
  return result;
}

Result<TrajectoryDTO> Splice(const TrajectoryDTO& active = Active(),
                             const TrajectoryDTO& successor = Successor()) {
  return build_quintic_trajectory_splice(active, successor, 0.5, 1.0, 0.4, 0.5);
}

TEST(TrajectorySplice, ConnectsNonzeroPvaWithTheExactQuintic) {
  const auto result = Splice();
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto start = sample_trajectory(result.value(), 0.5);
  const auto end = sample_trajectory(result.value(), 1.0);
  const auto middle = sample_trajectory(result.value(), 0.75);
  ASSERT_TRUE(start.ok() && end.ok() && middle.ok());
  EXPECT_DOUBLE_EQ(start.value().positions_rad[0], 2.0);
  // The inlet is sampled inside an old segment; analytic PVA allows the
  // existing sampler's roundoff, while copied-knot equality stays exact below.
  EXPECT_NEAR(start.value().velocities_rad_s[0], 3.0, 1e-12);
  EXPECT_NEAR(start.value().accelerations_rad_s2[0], 4.0, 1e-12);
  EXPECT_DOUBLE_EQ(end.value().positions_rad[0], 4.625);
  EXPECT_DOUBLE_EQ(end.value().velocities_rad_s[0], 8.875);
  EXPECT_DOUBLE_EQ(end.value().accelerations_rad_s2[0], 21.0);
  // Independent polynomial q(x)=2+3x+2x^2+5x^3-x^4+2x^5, x=.25.
  EXPECT_NEAR(middle.value().positions_rad[0], 2.951171875, 1e-12);
  EXPECT_NEAR(middle.value().velocities_rad_s[0], 4.9140625, 1e-12);
  EXPECT_NEAR(middle.value().accelerations_rad_s2[0], 11.375, 1e-12);
  EXPECT_EQ(middle.value().degree, 5U);
  EXPECT_FALSE(result.value().runtime_scale_bounds.has_value());
  EXPECT_FALSE(result.value().cartesian_fidelity_metrics.has_value());
}

TEST(TrajectorySplice, PreservesPastPointsAndFuturePrefixPolynomials) {
  const auto active = Active();
  const auto result = Splice(active);
  ASSERT_TRUE(result.ok());
  const auto& joined = result.value();
  ASSERT_GE(joined.waypoints.size(), 4U);
  for (std::size_t index = 0; index < 3U; ++index) {
    EXPECT_DOUBLE_EQ(joined.waypoints[index].time_from_start_s,
                     active.waypoints[index].time_from_start_s - 0.5);
    EXPECT_EQ(joined.waypoints[index].positions_rad, active.waypoints[index].positions_rad);
    EXPECT_EQ(joined.waypoints[index].velocities_rad_s, active.waypoints[index].velocities_rad_s);
    EXPECT_EQ(joined.waypoints[index].accelerations_rad_s2,
              active.waypoints[index].accelerations_rad_s2);
  }
  EXPECT_LT(joined.waypoints.front().time_from_start_s, 0.0);
  // The interval after "now" and before the cut is still the old polynomial.
  for (const double old_time : {0.1, 0.4, 0.6, 0.9, 0.999}) {
    const auto before = sample_trajectory(active, old_time);
    const auto after = sample_trajectory(joined, old_time - 0.5);
    ASSERT_TRUE(before.ok() && after.ok());
    EXPECT_NEAR(after.value().positions_rad[0], before.value().positions_rad[0], 1e-12);
    EXPECT_NEAR(after.value().velocities_rad_s[0], before.value().velocities_rad_s[0], 1e-11);
    EXPECT_NEAR(after.value().accelerations_rad_s2[0], before.value().accelerations_rad_s2[0],
                1e-10);
  }
}

TEST(TrajectorySplice, PreservesSuffixTimingAndMilestonesWithoutRetime) {
  auto successor = Successor();
  successor.cartesian_waypoint_times_s = {0.4, 0.9, 1.7};
  const auto result = Splice(Active(), successor);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto& joined = result.value();
  const double shift = 1.0 - 0.4;
  for (const auto& point : successor.waypoints) {
    if (point.time_from_start_s < 0.4) continue;
    const auto found =
        std::find_if(joined.waypoints.begin(), joined.waypoints.end(), [&](const auto& candidate) {
          return candidate.time_from_start_s == point.time_from_start_s + shift;
        });
    ASSERT_NE(found, joined.waypoints.end());
    EXPECT_EQ(found->positions_rad, point.positions_rad);
    EXPECT_EQ(found->velocities_rad_s, point.velocities_rad_s);
    EXPECT_EQ(found->accelerations_rad_s2, point.accelerations_rad_s2);
  }
  ASSERT_EQ(joined.cartesian_waypoint_times_s.size(), 3U);
  for (std::size_t index = 0; index < 3U; ++index)
    EXPECT_DOUBLE_EQ(joined.cartesian_waypoint_times_s[index],
                     successor.cartesian_waypoint_times_s[index] + shift);
  const auto before = sample_trajectory(successor, 1.1);
  const auto after = sample_trajectory(joined, 1.1 + shift);
  ASSERT_TRUE(before.ok() && after.ok());
  EXPECT_NEAR(after.value().positions_rad[0], before.value().positions_rad[0], 1e-12);
  EXPECT_NEAR(after.value().velocities_rad_s[0], before.value().velocities_rad_s[0], 1e-12);
  EXPECT_NEAR(after.value().accelerations_rad_s2[0], before.value().accelerations_rad_s2[0], 1e-11);
}

TEST(TrajectorySplice, ReordersColumnsOnlyWithTheirExactResources) {
  auto active = Active();
  auto successor = Successor();
  active.joint_names.push_back("j2");
  active.resources.push_back(ResourceId("joint.2"));
  for (auto& point : active.waypoints) {
    point.positions_rad.push_back(10.0);
    point.velocities_rad_s.push_back(0.0);
    point.accelerations_rad_s2.push_back(0.0);
  }
  successor.joint_names = {"j2", "j1"};
  successor.resources = {ResourceId("joint.2"), ResourceId("joint.1")};
  for (auto& point : successor.waypoints) {
    point.positions_rad.insert(point.positions_rad.begin(), 20.0);
    point.velocities_rad_s.insert(point.velocities_rad_s.begin(), 0.0);
    point.accelerations_rad_s2.insert(point.accelerations_rad_s2.begin(), 0.0);
  }
  const auto result = Splice(active, successor);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().joint_names, active.joint_names);
  EXPECT_EQ(result.value().resources, active.resources);
  EXPECT_EQ(result.value().waypoints.back().positions_rad,
            (std::vector<double>{successor.waypoints.back().positions_rad[1], 20.0}));
  const auto valid_successor = successor;
  successor.resources[1] = ResourceId("other.resource");
  EXPECT_FALSE(Splice(active, successor).ok());
  successor = valid_successor;
  successor.resources[1] = successor.resources[0];
  EXPECT_FALSE(Splice(active, successor).ok());
  successor = valid_successor;
  successor.joint_names[1] = successor.joint_names[0];
  EXPECT_FALSE(Splice(active, successor).ok());
  successor = valid_successor;
  successor.joint_names[1] = "different.joint";
  EXPECT_FALSE(Splice(active, successor).ok());
}

TEST(TrajectorySplice, SamplesInteriorCutsAndAllowsTheSuccessorEndpoint) {
  const auto active = Active();
  const auto successor = Successor();
  const auto result = build_quintic_trajectory_splice(active, successor, 0.5, 1.0, 0.6, 0.5);
  ASSERT_TRUE(result.ok());
  const auto entry = sample_trajectory(successor, 0.6);
  const auto joined = sample_trajectory(result.value(), 1.0);
  ASSERT_TRUE(entry.ok() && joined.ok());
  EXPECT_EQ(joined.value().positions_rad, entry.value().positions_rad);
  EXPECT_EQ(joined.value().velocities_rad_s, entry.value().velocities_rad_s);
  EXPECT_EQ(joined.value().accelerations_rad_s2, entry.value().accelerations_rad_s2);
  const auto endpoint = build_quintic_trajectory_splice(active, successor, 0.5, 1.0, 1.7, 0.5);
  ASSERT_TRUE(endpoint.ok());
  EXPECT_DOUBLE_EQ(endpoint.value().waypoints.back().time_from_start_s, 1.0);
}

TEST(TrajectorySplice, RejectsIncompleteMalformedAndNonfiniteCurves) {
  const auto check = [](auto mutate) {
    auto successor = Successor();
    mutate(successor);
    EXPECT_FALSE(Splice(Active(), successor).ok());
  };
  check([](auto& curve) { curve.waypoints.clear(); });
  check([](auto& curve) { curve.waypoints.resize(1); });
  check([](auto& curve) { curve.waypoints[1].velocities_rad_s.clear(); });
  check([](auto& curve) { curve.waypoints[1].accelerations_rad_s2.push_back(0.0); });
  check([](auto& curve) { curve.waypoints[1].positions_rad.clear(); });
  check([](auto& curve) { curve.waypoints[1].time_from_start_s = 0.0; });
  check([](auto& curve) {
    curve.waypoints[1].positions_rad[0] = std::numeric_limits<double>::infinity();
  });
  check([](auto& curve) {
    curve.waypoints[1].velocities_rad_s[0] = std::numeric_limits<double>::quiet_NaN();
  });
  check([](auto& curve) {
    curve.waypoints[1].accelerations_rad_s2[0] = std::numeric_limits<double>::infinity();
  });
  check([](auto& curve) { curve.joint_names[0].clear(); });
  check([](auto& curve) { curve.resources.clear(); });
  check([](auto& curve) { curve.resources[0] = ResourceId(""); });
  auto active = Active();
  active.waypoints[0].accelerations_rad_s2.clear();
  EXPECT_FALSE(Splice(active).ok());
}

TEST(TrajectorySplice, RejectsExpiredOutOfRangeAndInvalidTimeParameters) {
  const auto active = Active();
  const auto successor = Successor();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const double elapsed : {1.0, 1.2, nan, inf})
    EXPECT_FALSE(build_quintic_trajectory_splice(active, successor, elapsed, 1.0, 0.4, 0.5).ok());
  for (const double cut : {-0.1, 2.1, nan, inf})
    EXPECT_FALSE(build_quintic_trajectory_splice(active, successor, -0.2, cut, 0.4, 0.5).ok());
  for (const double entry : {-0.1, 1.8, nan, inf})
    EXPECT_FALSE(build_quintic_trajectory_splice(active, successor, 0.5, 1.0, entry, 0.5).ok());
  for (const double duration : {0.0, -0.1, nan, inf})
    EXPECT_FALSE(build_quintic_trajectory_splice(active, successor, 0.5, 1.0, 0.4, duration).ok());
}

TEST(TrajectorySplice, RejectsSkippedOrMalformedSuccessorMilestones) {
  auto successor = Successor();
  for (const auto& milestones : std::vector<std::vector<double>>{
           {0.3}, {0.8, 0.7}, {-0.1}, {2.0}, {std::numeric_limits<double>::quiet_NaN()}}) {
    successor.cartesian_waypoint_times_s = milestones;
    EXPECT_FALSE(Splice(Active(), successor).ok());
  }
}

TEST(TrajectorySplice, RejectsOverflowAndUnrepresentableExactBoundaries) {
  const auto active = Active();
  const auto successor = Successor();
  const double maximum = std::numeric_limits<double>::max();
  EXPECT_FALSE(
      build_quintic_trajectory_splice(active, successor, -maximum, 1.0, 0.4, maximum).ok());
  EXPECT_FALSE(build_quintic_trajectory_splice(active, successor, 0.5, 1.0, 0.4, 1e200).ok());
  // Existing Stop join would coalesce these cuts; never change requested PVA silently.
  EXPECT_FALSE(build_quintic_trajectory_splice(active, successor, 0.5, 0.8 + 1e-10, 0.4, 0.5).ok());
  EXPECT_FALSE(build_quintic_trajectory_splice(active, successor, 0.5, 1.0, 0.4, 1e-10).ok());
  auto extreme = Successor();
  for (auto& point : extreme.waypoints) point.positions_rad[0] = maximum;
  auto opposite = Active();
  for (auto& point : opposite.waypoints) point.positions_rad[0] = -maximum;
  EXPECT_FALSE(Splice(opposite, extreme).ok());
}

TEST(TrajectorySplice, PreservesOrdinaryFractionalDurationsWithinRepresentationPrecision) {
  const auto result = build_quintic_trajectory_splice(Active(), Successor(), 0.123456789,
                                                      0.87654321, 0.23456789, 0.32109876);
  ASSERT_TRUE(result.ok()) << result.error().details.at("reason");
  const auto future = slice_future_trajectory(result.value(), 0.111222333);
  ASSERT_TRUE(future.ok()) << future.error().details.at("reason");
  EXPECT_DOUBLE_EQ(future.value().waypoints.front().time_from_start_s, 0.0);
}

TEST(TrajectorySplice, RejectsClockOffsetsThatDistortConnectorSuffixOrPrefixSpans) {
  const auto active = ConstantCurve({0.0, 1e16, 1e16 + 8.0});
  const auto successor = ConstantCurve({0.0, 1.0, 2.0, 5.0});
  // start + 3 rounds to start + 4; endpoint-expression equality misses this.
  const auto connector = build_quintic_trajectory_splice(active, successor, 0.0, 1e16, 1.0, 3.0);
  ASSERT_FALSE(connector.ok());
  EXPECT_EQ(connector.error().details.at("reason"), "trajectory_splice_time_precision_lost");
  // The connector now preserves D=4, but suffix spans 1,3 become 2,2.
  const auto suffix = build_quintic_trajectory_splice(active, successor, 0.0, 1e16, 1.0, 4.0);
  ASSERT_FALSE(suffix.ok());
  EXPECT_EQ(suffix.error().details.at("reason"), "trajectory_splice_time_precision_lost");
  const auto prefix = build_quintic_trajectory_splice(ConstantCurve({0.0, 3.0, 6.0, 10.0}),
                                                      successor, -1e16, 8.0, 5.0, 4.0);
  ASSERT_FALSE(prefix.ok());
  EXPECT_EQ(prefix.error().details.at("reason"), "trajectory_splice_time_precision_lost");
  const auto future = slice_future_trajectory(ConstantCurve({-1e16, 0.0, 3.0, 6.0, 10.0}), -1e16);
  ASSERT_FALSE(future.ok());
  EXPECT_EQ(future.error().details.at("reason"), "trajectory_splice_time_precision_lost");
}

TEST(TrajectorySplice, RejectsTinyIllConditionedSpansBelowTheAbsoluteErrorCeiling) {
  const auto tiny = ConstantCurve({0.0, 0.1, 0.100000000001, 0.2});
  // Translation error is far below 1 ns, but materially changes this tiny span.
  const auto splice = build_quintic_trajectory_splice(Active(), tiny, 0.95, 1.0, 0.1, 0.1);
  ASSERT_FALSE(splice.ok());
  EXPECT_EQ(splice.error().details.at("reason"), "trajectory_splice_time_precision_lost");
  const auto future =
      slice_future_trajectory(ConstantCurve({-0.1, 0.1, 0.100000000001, 0.2}), -0.05);
  ASSERT_FALSE(future.ok());
  EXPECT_EQ(future.error().details.at("reason"), "trajectory_splice_time_precision_lost");
}

TEST(TrajectorySplice, FutureViewPreservesTheExactInterpolantAtZeroAndInteriorCuts) {
  const auto delivery = Splice();
  ASSERT_TRUE(delivery.ok());
  for (const double cut : {0.0, 0.17, 0.5}) {
    const auto future = slice_future_trajectory(delivery.value(), cut);
    ASSERT_TRUE(future.ok());
    EXPECT_DOUBLE_EQ(future.value().waypoints.front().time_from_start_s, 0.0);
    EXPECT_FALSE(future.value().runtime_scale_bounds.has_value());
    EXPECT_FALSE(future.value().cartesian_fidelity_metrics.has_value());
    for (const double offset : {0.0, 0.1, 0.24, 0.5, 1.0}) {
      const auto expected = sample_trajectory(delivery.value(), cut + offset);
      const auto observed = sample_trajectory(future.value(), offset);
      ASSERT_TRUE(expected.ok() && observed.ok());
      EXPECT_NEAR(observed.value().positions_rad[0], expected.value().positions_rad[0], 1e-11);
      EXPECT_NEAR(observed.value().velocities_rad_s[0], expected.value().velocities_rad_s[0],
                  1e-10);
      EXPECT_NEAR(observed.value().accelerations_rad_s2[0],
                  expected.value().accelerations_rad_s2[0], 1e-9);
    }
    EXPECT_TRUE(std::all_of(future.value().waypoints.begin(), future.value().waypoints.end(),
                            [](const auto& point) { return point.time_from_start_s >= 0.0; }));
  }
  EXPECT_LT(delivery.value().waypoints.front().time_from_start_s, 0.0);
}

TEST(TrajectorySplice, FutureViewPreservesMilestonesWithoutInventingAnOrigin) {
  auto successor = Successor();
  successor.cartesian_waypoint_times_s = {0.4, 0.4, 0.9, 1.7};
  const auto delivery = Splice(Active(), successor);
  ASSERT_TRUE(delivery.ok());
  const auto future = slice_future_trajectory(delivery.value(), 0.2);
  ASSERT_TRUE(future.ok());
  ASSERT_EQ(future.value().cartesian_waypoint_times_s.size(), 4U);
  EXPECT_GT(future.value().cartesian_waypoint_times_s.front(), 0.0);
  for (std::size_t index = 0; index < 4U; ++index)
    EXPECT_DOUBLE_EQ(future.value().cartesian_waypoint_times_s[index],
                     delivery.value().cartesian_waypoint_times_s[index] - 0.2);
  EXPECT_FALSE(slice_future_trajectory(delivery.value(), 1.1).ok());
}

TEST(TrajectorySplice, FutureViewRejectsInvalidCutsAndUnrepresentableTranslations) {
  const auto curve = Active();
  for (const double cut : {-0.1, 2.0, 2.1, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()})
    EXPECT_FALSE(slice_future_trajectory(curve, cut).ok());
  auto malformed = curve;
  malformed.waypoints[1].velocities_rad_s.clear();
  EXPECT_FALSE(slice_future_trajectory(malformed, 0.5).ok());
  auto wide_clock = curve;
  const double maximum = std::numeric_limits<double>::max();
  wide_clock.waypoints = {
      {-maximum, {0.0}, {0.0}, {0.0}}, {0.0, {0.0}, {0.0}, {0.0}}, {maximum, {0.0}, {0.0}, {0.0}}};
  EXPECT_FALSE(slice_future_trajectory(wide_clock, -maximum).ok());
}

}  // namespace
}  // namespace manyforge::core
