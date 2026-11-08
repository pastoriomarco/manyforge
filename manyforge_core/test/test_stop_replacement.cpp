#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <manyforge_core/motion_interrupt/stop_replacement.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

constexpr double kTolerance = 1e-9;

TrajectoryWaypointDTO Point(double t, std::vector<double> p, std::vector<double> v,
                            std::vector<double> a = {}) {
  if (a.empty()) a.assign(p.size(), 0.0);
  return TrajectoryWaypointDTO{t, std::move(p), std::move(v), std::move(a)};
}

/// One-joint straight path at 0.5 rad per path second, sampled every 0.5 s.
TrajectoryDTO Linear(double start_s = 0.0, double end_s = 2.0, double step_s = 0.5) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"j1"};
  for (double t = start_s; t <= end_s + 1e-12; t += step_s) {
    trajectory.waypoints.push_back(Point(t, {0.5 * t}, {0.5}));
  }
  return trajectory;
}

/// Two joints on a curved path with varying speed, knots at uneven times,
/// exact analytic P/V/A at every knot.
TrajectoryDTO Curved() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"j1", "j2"};
  for (const double t : {0.0, 0.3, 0.55, 0.9, 1.05, 1.4, 1.8, 2.2}) {
    trajectory.waypoints.push_back(
        Point(t, {std::sin(1.3 * t) + 0.2 * t * t, t * std::cos(0.7 * t)},
              {1.3 * std::cos(1.3 * t) + 0.4 * t, std::cos(0.7 * t) - 0.7 * t * std::sin(0.7 * t)},
              {-1.69 * std::sin(1.3 * t) + 0.4,
               -1.4 * std::sin(0.7 * t) - 0.49 * t * std::cos(0.7 * t)}));
  }
  return trajectory;
}

/// The controller's interpolation between two P/V/A knots (JTC's quintic
/// spline in its monomial form, independent of Core's Hermite basis).
struct PVA {
  double p, v, a;
};
PVA Quintic(const TrajectoryWaypointDTO& from, const TrajectoryWaypointDTO& to,
            std::size_t joint, double time) {
  const double T = to.time_from_start_s - from.time_from_start_s;
  const double p0 = from.positions_rad[joint], p1 = to.positions_rad[joint];
  const double v0 = from.velocities_rad_s[joint], v1 = to.velocities_rad_s[joint];
  const double a0 = from.accelerations_rad_s2[joint], a1 = to.accelerations_rad_s2[joint];
  const double c3 = (20.0 * (p1 - p0) - (8.0 * v1 + 12.0 * v0) * T - (3.0 * a0 - a1) * T * T) /
                    (2.0 * T * T * T);
  const double c4 = (-30.0 * (p1 - p0) + (14.0 * v1 + 16.0 * v0) * T +
                     (3.0 * a0 - 2.0 * a1) * T * T) /
                    (2.0 * T * T * T * T);
  const double c5 = (12.0 * (p1 - p0) - 6.0 * (v1 + v0) * T - (a0 - a1) * T * T) /
                    (2.0 * T * T * T * T * T);
  const double x = time - from.time_from_start_s;
  return {p0 + v0 * x + 0.5 * a0 * x * x + c3 * x * x * x + c4 * x * x * x * x +
              c5 * x * x * x * x * x,
          v0 + a0 * x + 3.0 * c3 * x * x + 4.0 * c4 * x * x * x + 5.0 * c5 * x * x * x * x,
          a0 + 6.0 * c3 * x + 12.0 * c4 * x * x + 20.0 * c5 * x * x * x};
}

/// The controller's evaluation of `trajectory` at `time` (inside its range).
PVA Evaluate(const TrajectoryDTO& trajectory, std::size_t joint, double time) {
  const auto& points = trajectory.waypoints;
  std::size_t index = 0;
  while (index + 2U < points.size() && points[index + 1U].time_from_start_s <= time) ++index;
  return Quintic(points[index], points[index + 1U], joint, time);
}

/// A Stop continuation as the helper hands it to the join (path time at the
/// factor, time 0 at `origin_s` on `active`): a lead-in resampling the active
/// path on a uniform grid that misses its knots, then a ramp whose first
/// sample is the trip point at `factor * lead_in_s`, slowing to rest.
TrajectoryDTO Continuation(const TrajectoryDTO& active, double origin_s, double factor,
                           double lead_in_s) {
  TrajectoryDTO continuation;
  continuation.joint_names = {"j2", "j1"};  // reordered by the join
  const auto add = [&](double time, double path_time, double rate) {
    const auto state = sample_trajectory(active, path_time).value();
    continuation.waypoints.push_back(
        Point(time, {state.positions_rad[1], state.positions_rad[0]},
              {rate * state.velocities_rad_s[1], rate * state.velocities_rad_s[0]},
              {rate * rate * state.accelerations_rad_s2[1],
               rate * rate * state.accelerations_rad_s2[0]}));
  };
  const double trip_time = factor * lead_in_s;
  for (double time = 0.0; time < trip_time - 1e-12; time += 0.04) add(time, origin_s + time, 1.0);
  for (int k = 0; k <= 5; ++k) {
    const double r = 0.2 * k;  // rate 1 -> 0 over 0.2 path seconds
    add(trip_time + 0.2 * r, origin_s + trip_time + 0.2 * (r - 0.5 * r * r), 1.0 - r);
  }
  return continuation;
}

// --- Closed-form brake -------------------------------------------------------

ControlledHardStopBrakeInputDTO BrakeInput(double factor) {
  return ControlledHardStopBrakeInputDTO{
      {"j1", "j2", "j3"},
      {0.1, -0.2, 0.3},
      {1.0, -2.0, 0.5},
      factor,
      {{"j1", 4.0}, {"j2", 10.0}, {"j3", 1.0}},
      0.004};
}

TEST(ControlledHardStopBrakeTest, StartsAtTheActualSpeedAndStaysWithinEveryJointLimit) {
  for (const double factor : {0.25, 0.5, 0.75, 1.0}) {
    SCOPED_TRACE(factor);
    const auto input = BrakeInput(factor);
    const auto brake = plan_controlled_hard_stop_brake(input);
    ASSERT_TRUE(brake.ok()) << brake.error().message;
    const auto& points = brake.value().trajectory.waypoints;
    ASSERT_GE(points.size(), 2U);
    // Common stop time from the slowest-braking joint: f |v| / d per joint is
    // 0.25 f, 0.2 f and 0.5 f; j3 limits.
    EXPECT_NEAR(brake.value().stop_time_s, 0.5 * factor, kTolerance);
    EXPECT_NEAR(brake.value().stop_path_time_s, factor * 0.5 * factor, kTolerance);
    EXPECT_EQ(brake.value().limiting_joint, "j3");
    // Applying the factor once: physical start speed f * dq/ds equals the
    // actual speed f * v (no drop).
    EXPECT_DOUBLE_EQ(points.front().time_from_start_s, 0.0);
    for (std::size_t joint = 0; joint < 3U; ++joint) {
      EXPECT_NEAR(factor * points.front().velocities_rad_s[joint],
                  factor * input.reference_path_velocities[joint], kTolerance);
      EXPECT_NEAR(points.front().positions_rad[joint], input.reference_positions_rad[joint],
                  kTolerance);
    }
    const std::vector<double> limits{4.0, 10.0, 1.0};
    for (std::size_t index = 0; index < points.size(); ++index) {
      const auto& point = points[index];
      if (index > 0U) {
        EXPECT_GT(point.time_from_start_s, points[index - 1U].time_from_start_s);
        EXPECT_LE(point.time_from_start_s - points[index - 1U].time_from_start_s,
                  factor * input.controller_period_s + kTolerance);
      }
      for (std::size_t joint = 0; joint < 3U; ++joint) {
        // Physical deceleration f^2 * d2q/ds2 within the limit; the
        // limiting joint brakes at exactly its limit.
        const double physical = std::abs(factor * factor * point.accelerations_rad_s2[joint]);
        EXPECT_LE(physical, limits[joint] + kTolerance);
        if (joint == 2U) {
          EXPECT_NEAR(physical, limits[joint], kTolerance);
        }
        // Direction preserved: never reverses.
        EXPECT_GE(point.velocities_rad_s[joint] * input.reference_path_velocities[joint], 0.0);
      }
    }
    const auto& stop = points.back();
    EXPECT_NEAR(stop.time_from_start_s, brake.value().stop_path_time_s, kTolerance);
    for (std::size_t joint = 0; joint < 3U; ++joint) {
      EXPECT_EQ(stop.velocities_rad_s[joint], 0.0);
      const double u = factor * input.reference_path_velocities[joint];
      EXPECT_NEAR(stop.positions_rad[joint],
                  input.reference_positions_rad[joint] + 0.5 * u * brake.value().stop_time_s,
                  kTolerance);
    }
  }
}

TEST(ControlledHardStopBrakeTest, AllJointsReachRestTogetherAlongTheCurrentDirection) {
  const auto brake = plan_controlled_hard_stop_brake(BrakeInput(0.5));
  ASSERT_TRUE(brake.ok());
  const auto& points = brake.value().trajectory.waypoints;
  // Velocities stay proportional to the initial ones: one common stop.
  for (const auto& point : points) {
    const double ratio = point.velocities_rad_s[0] / 1.0;
    EXPECT_NEAR(point.velocities_rad_s[1], -2.0 * ratio, kTolerance);
    EXPECT_NEAR(point.velocities_rad_s[2], 0.5 * ratio, kTolerance);
  }
}

TEST(ControlledHardStopBrakeTest, RestIsASingleHoldPointAndAStillJointStaysPut) {
  auto input = BrakeInput(0.5);
  input.reference_path_velocities = {0.0, 0.0, 0.0};
  const auto hold = plan_controlled_hard_stop_brake(input);
  ASSERT_TRUE(hold.ok());
  EXPECT_EQ(hold.value().stop_time_s, 0.0);
  EXPECT_TRUE(hold.value().limiting_joint.empty());
  ASSERT_EQ(hold.value().trajectory.waypoints.size(), 1U);
  EXPECT_EQ(hold.value().trajectory.waypoints.front().positions_rad, input.reference_positions_rad);
  EXPECT_EQ(hold.value().trajectory.waypoints.front().velocities_rad_s,
            std::vector<double>(3U, 0.0));

  input.reference_path_velocities = {1.0, 0.0, 0.0};
  const auto partial = plan_controlled_hard_stop_brake(input);
  ASSERT_TRUE(partial.ok());
  EXPECT_EQ(partial.value().limiting_joint, "j1");
  for (const auto& point : partial.value().trajectory.waypoints) {
    EXPECT_EQ(point.positions_rad[1], -0.2);
    EXPECT_EQ(point.velocities_rad_s[1], 0.0);
  }
}

TEST(ControlledHardStopBrakeTest, ShortBrakeStillEndsWithItsExactStopPoint) {
  auto input = BrakeInput(1.0);
  input.reference_path_velocities = {0.001, 0.0, 0.0};
  const auto brake = plan_controlled_hard_stop_brake(input);
  ASSERT_TRUE(brake.ok());
  ASSERT_EQ(brake.value().trajectory.waypoints.size(), 2U);
  EXPECT_NEAR(brake.value().trajectory.waypoints.back().time_from_start_s, 0.001 / 4.0,
              kTolerance);
}

TEST(ControlledHardStopBrakeTest, RejectsUnusableInputs) {
  auto zero_factor = BrakeInput(0.0);
  EXPECT_FALSE(plan_controlled_hard_stop_brake(zero_factor).ok());
  auto over = BrakeInput(1.5);
  EXPECT_FALSE(plan_controlled_hard_stop_brake(over).ok());
  auto missing = BrakeInput(0.5);
  missing.max_deceleration_rad_s2.erase("j2");
  const auto missing_result = plan_controlled_hard_stop_brake(missing);
  ASSERT_FALSE(missing_result.ok());
  EXPECT_EQ(missing_result.error().details.at("reason"),
            "controlled_hard_stop_brake_input_invalid");
  EXPECT_EQ(missing_result.error().details.at("joint"), "j2");
  auto period = BrakeInput(0.5);
  period.controller_period_s = 0.0;
  EXPECT_FALSE(plan_controlled_hard_stop_brake(period).ok());
  auto nan = BrakeInput(0.5);
  nan.reference_path_velocities[0] = std::nan("");
  EXPECT_FALSE(plan_controlled_hard_stop_brake(nan).ok());
  auto size = BrakeInput(0.5);
  size.reference_positions_rad.pop_back();
  EXPECT_FALSE(plan_controlled_hard_stop_brake(size).ok());
}

// --- Join ----------------------------------------------------------------------

TEST(StopReplacementJoinTest, KeepsPastPointsWithNegativeTimesThenTheContinuation) {
  const auto active = Linear();  // t = 0, 0.5, 1, 1.5, 2
  TrajectoryDTO continuation;
  continuation.joint_names = {"j1"};
  // Time 0 at active path time 1.0.
  for (const double t : {0.0, 0.1, 0.3, 0.6}) {
    continuation.waypoints.push_back(Point(t, {0.5 + 0.5 * t}, {0.5}));
  }
  const auto joined = build_stop_replacement_join(active, 1.2, continuation, 1.0, 1.0);
  ASSERT_TRUE(joined.ok()) << joined.error().message;
  const auto& points = joined.value().waypoints;
  ASSERT_EQ(points.size(), 5U);
  // Past points: never clamped, shifted by the elapsed time.
  EXPECT_NEAR(points[0].time_from_start_s, -1.2, kTolerance);
  EXPECT_NEAR(points[1].time_from_start_s, -0.7, kTolerance);
  EXPECT_NEAR(points[2].time_from_start_s, -0.2, kTolerance);
  EXPECT_EQ(points[0].positions_rad, active.waypoints[0].positions_rad);
  EXPECT_EQ(points[2].positions_rad, active.waypoints[2].positions_rad);
  // Continuation points already passed are dropped; the rest shift by -0.2.
  EXPECT_NEAR(points[3].time_from_start_s, 0.1, kTolerance);
  EXPECT_NEAR(points[4].time_from_start_s, 0.4, kTolerance);
  EXPECT_EQ(points[4].positions_rad, continuation.waypoints[3].positions_rad);
  for (std::size_t index = 1; index < points.size(); ++index) {
    EXPECT_GT(points[index].time_from_start_s, points[index - 1U].time_from_start_s);
  }
}

TEST(StopReplacementJoinTest, OriginalPathIsPreservedThroughTheLeadIn) {
  const auto active = Linear(0.0, 2.0, 0.25);
  // A lead-in resampling the active path from its time 1.0 every 0.05 s.
  const auto lead_in = Linear(0.0, 0.3, 0.05);
  TrajectoryDTO continuation = lead_in;
  for (auto& point : continuation.waypoints) point.positions_rad[0] += 0.5;  // origin q(1.0)
  const double elapsed = 1.07;
  const auto joined = build_stop_replacement_join(active, elapsed, continuation, 1.0, 1.0);
  ASSERT_TRUE(joined.ok());
  for (const auto& point : joined.value().waypoints) {
    // Every point lies on the active path at its own time on the active clock.
    EXPECT_NEAR(point.positions_rad[0], 0.5 * (elapsed + point.time_from_start_s), kTolerance);
  }
}

TEST(StopReplacementJoinTest, ExcludesAnActivePointAtTheReferenceTimeAndReordersJoints) {
  TrajectoryDTO active;
  active.joint_names = {"a", "b"};
  active.waypoints = {Point(0.0, {0.0, 1.0}, {0.0, 0.0}), Point(1.0, {1.0, 2.0}, {1.0, 1.0})};
  TrajectoryDTO continuation;
  continuation.joint_names = {"b", "a"};
  continuation.waypoints = {Point(0.0, {2.0, 1.0}, {1.0, 1.0}), Point(0.5, {2.2, 1.2}, {0.0, 0.0})};
  const auto joined = build_stop_replacement_join(active, 1.0, continuation, 1.0, 1.0);
  ASSERT_TRUE(joined.ok());
  ASSERT_EQ(joined.value().waypoints.size(), 3U);
  EXPECT_NEAR(joined.value().waypoints[0].time_from_start_s, -1.0, kTolerance);
  EXPECT_DOUBLE_EQ(joined.value().waypoints[1].time_from_start_s, 0.0);
  EXPECT_EQ(joined.value().joint_names, active.joint_names);
  EXPECT_EQ(joined.value().waypoints[2].positions_rad, (std::vector<double>{1.2, 2.2}));
}

TEST(StopReplacementJoinTest, RefusesAnElapsedContinuationAndIncompleteInputs) {
  const auto active = Linear();
  TrajectoryDTO continuation;
  continuation.joint_names = {"j1"};
  continuation.waypoints = {Point(0.0, {0.5}, {0.5}), Point(0.1, {0.55}, {0.0})};
  const auto elapsed = build_stop_replacement_join(active, 1.5, continuation, 1.0, 1.0);
  ASSERT_FALSE(elapsed.ok());
  EXPECT_EQ(elapsed.error().details.at("reason"), "stop_join_continuation_elapsed");

  auto incomplete = active;
  incomplete.waypoints[1].accelerations_rad_s2.clear();
  EXPECT_EQ(build_stop_replacement_join(incomplete, 1.0, continuation, 1.0, 1.0)
                .error()
                .details.at("reason"),
            "stop_join_input_invalid");
  auto other_joint = continuation;
  other_joint.joint_names = {"j9"};
  EXPECT_FALSE(build_stop_replacement_join(active, 1.0, other_joint, 1.0, 1.0).ok());
}

TEST(StopReplacementJoinTest, LeadInIsTheOriginalPathsOwnKnotsThenTheExactTripState) {
  const auto active = Curved();
  const double origin = 0.85, factor = 0.5, lead_in = 0.5;
  const double trip = stop_trip_path_s(origin, factor, lead_in);  // 1.1
  const double elapsed = 0.95;
  const auto continuation = Continuation(active, origin, factor, lead_in);
  const auto joined = build_stop_replacement_join(active, elapsed, continuation, origin, trip);
  ASSERT_TRUE(joined.ok()) << joined.error().message;
  const auto& points = joined.value().waypoints;
  EXPECT_EQ(joined.value().joint_names, active.joint_names);
  // Knots 0, 0.3, 0.55, 0.9 (past, negative times) and 1.05 (lead-in), verbatim.
  ASSERT_GE(points.size(), 7U);
  for (std::size_t index = 0; index < 5U; ++index) {
    const auto& knot = active.waypoints[index];
    EXPECT_NEAR(points[index].time_from_start_s, knot.time_from_start_s - elapsed, 1e-12);
    EXPECT_EQ(points[index].positions_rad, knot.positions_rad);
    EXPECT_EQ(points[index].velocities_rad_s, knot.velocities_rad_s);
    EXPECT_EQ(points[index].accelerations_rad_s2, knot.accelerations_rad_s2);
  }
  EXPECT_LT(points[3].time_from_start_s, 0.0);
  EXPECT_GT(points[4].time_from_start_s, 0.0);
  // The trip point carries the original curve's own state there.
  const auto& trip_point = points[5];
  EXPECT_NEAR(trip_point.time_from_start_s, trip - elapsed, 1e-12);
  for (std::size_t joint = 0; joint < 2U; ++joint) {
    const auto original = Evaluate(active, joint, trip);
    EXPECT_NEAR(trip_point.positions_rad[joint], original.p, 1e-11);
    EXPECT_NEAR(trip_point.velocities_rad_s[joint], original.v, 1e-10);
    EXPECT_NEAR(trip_point.accelerations_rad_s2[joint], original.a, 1e-9);
  }
  // Last knot -> trip point reproduces the original polynomial.
  for (const double fraction : {0.1, 0.25, 0.5, 0.75, 0.9}) {
    const double time = points[4].time_from_start_s +
                        fraction * (points[5].time_from_start_s - points[4].time_from_start_s);
    for (std::size_t joint = 0; joint < 2U; ++joint) {
      const auto executed = Quintic(points[4], points[5], joint, time);
      const auto original = Evaluate(active, joint, time + elapsed);
      EXPECT_NEAR(executed.p, original.p, 1e-10);
      EXPECT_NEAR(executed.v, original.v, 1e-8);
      EXPECT_NEAR(executed.a, original.a, 1e-6);
    }
  }
  // No lead-in grid sample; the ramp after its trip sample, joints reordered.
  ASSERT_EQ(points.size(), 5U + 1U + 5U);
  const std::size_t ramp_begin = continuation.waypoints.size() - 5U;
  for (std::size_t k = 0; k < 5U; ++k) {
    const auto& expected = continuation.waypoints[ramp_begin + k];
    const auto& actual = points[6U + k];
    EXPECT_NEAR(actual.time_from_start_s, expected.time_from_start_s + origin - elapsed, 1e-12);
    EXPECT_EQ(actual.positions_rad,
              (std::vector<double>{expected.positions_rad[1], expected.positions_rad[0]}));
  }
  EXPECT_EQ(points.back().velocities_rad_s, (std::vector<double>{0.0, 0.0}));
  for (std::size_t index = 1; index < points.size(); ++index) {
    EXPECT_GT(points[index].time_from_start_s, points[index - 1U].time_from_start_s);
  }
}

TEST(StopReplacementJoinTest, NoOriginalKnotInsideTheLeadInStillEndsOnTheExactTripState) {
  const auto active = Curved();
  // Lead-in from 1.45 to 1.45 + 0.75 * 0.4 = 1.75, between knots 1.4 and 1.8.
  const double origin = 1.45, factor = 0.75, lead_in = 0.4;
  const double trip = stop_trip_path_s(origin, factor, lead_in);
  const double elapsed = 1.5;
  const auto joined = build_stop_replacement_join(
      active, elapsed, Continuation(active, origin, factor, lead_in), origin, trip);
  ASSERT_TRUE(joined.ok()) << joined.error().message;
  const auto& points = joined.value().waypoints;
  ASSERT_GT(points.size(), 7U);
  EXPECT_EQ(points[5].positions_rad, active.waypoints[5].positions_rad);  // knot 1.4, past
  EXPECT_NEAR(points[5].time_from_start_s, 1.4 - elapsed, 1e-12);
  EXPECT_NEAR(points[6].time_from_start_s, trip - elapsed, 1e-12);
  for (const double fraction : {0.2, 0.5, 0.8}) {
    const double time = points[5].time_from_start_s +
                        fraction * (points[6].time_from_start_s - points[5].time_from_start_s);
    for (std::size_t joint = 0; joint < 2U; ++joint) {
      EXPECT_NEAR(Quintic(points[5], points[6], joint, time).p,
                  Evaluate(active, joint, time + elapsed).p, 1e-10);
    }
  }
  EXPECT_GT(points[7].time_from_start_s, points[6].time_from_start_s + 1e-3);
}

TEST(StopReplacementJoinTest, TripPointOnAKnotIsThatKnotOnce) {
  const auto active = Curved();
  const double elapsed = 0.95;
  // Exactly on knot 1.05, and within the 1 ns tolerance either side.
  for (const double trip : {1.05, 1.05 - 1e-12, 1.05 + 1e-12}) {
    SCOPED_TRACE(trip);
    const double origin = 0.85;
    auto continuation = Continuation(active, origin, 1.0, trip - origin);
    const auto joined = build_stop_replacement_join(active, elapsed, continuation, origin, trip);
    ASSERT_TRUE(joined.ok()) << joined.error().message;
    const auto& points = joined.value().waypoints;
    std::size_t near_trip = 0;
    for (const auto& point : points) {
      if (std::abs(point.time_from_start_s - (1.05 - elapsed)) < 1e-6) {
        ++near_trip;
        for (std::size_t joint = 0; joint < 2U; ++joint) {
          EXPECT_NEAR(point.positions_rad[joint], active.waypoints[4].positions_rad[joint], 1e-11);
          EXPECT_NEAR(point.velocities_rad_s[joint], active.waypoints[4].velocities_rad_s[joint],
                      1e-9);
        }
      }
    }
    EXPECT_EQ(near_trip, 1U);
    EXPECT_EQ(points.size(), 5U + 5U);  // knots 0..1.05, then the ramp after the trip
    for (std::size_t index = 1; index < points.size(); ++index) {
      EXPECT_GT(points[index].time_from_start_s, points[index - 1U].time_from_start_s + 1e-9);
    }
  }
}

TEST(StopReplacementJoinTest, RefusesATripPointOutsideTheActivePathOrWithoutARampAfterIt) {
  const auto active = Curved();
  const auto continuation = Continuation(active, 2.0, 1.0, 0.1);
  const auto past_end = build_stop_replacement_join(active, 2.05, continuation, 2.0, 2.3);
  ASSERT_FALSE(past_end.ok());
  EXPECT_EQ(past_end.error().details.at("reason"), "stop_join_input_invalid");
  const auto before_origin = build_stop_replacement_join(active, 1.0, continuation, 2.0, 1.9);
  ASSERT_FALSE(before_origin.ok());
  EXPECT_EQ(before_origin.error().details.at("reason"), "stop_join_input_invalid");
  auto lead_in_only = Continuation(active, 1.0, 1.0, 0.2);
  lead_in_only.waypoints.resize(lead_in_only.waypoints.size() - 5U);  // ramp after the trip gone
  const auto no_ramp = build_stop_replacement_join(active, 1.05, lead_in_only, 1.0, 1.2);
  ASSERT_FALSE(no_ramp.ok());
  EXPECT_EQ(no_ramp.error().details.at("reason"), "stop_join_continuation_elapsed");
}

// --- Elapsed time and lead-in rule --------------------------------------------

TEST(StopJoinElapsedTest, AdvancesTheSampleCursorAtTheFrozenFactor) {
  const auto scaled = compute_stop_join_elapsed_s({1.0, 0.5, 0.01, 0.02, std::nullopt});
  ASSERT_TRUE(scaled.ok());
  EXPECT_NEAR(scaled.value(), 1.015, kTolerance);
  const auto unscaled = compute_stop_join_elapsed_s({1.0, 1.0, 0.01, 0.02, std::nullopt});
  ASSERT_TRUE(unscaled.ok());
  EXPECT_NEAR(unscaled.value(), 1.03, kTolerance);
  // Delayed delivery is the local time since the sample, not a wall clock
  // since dispatch: it advances the cursor only at the factor.
  const auto delayed = compute_stop_join_elapsed_s({1.0, 0.25, 0.2, 0.02, std::nullopt});
  ASSERT_TRUE(delayed.ok());
  EXPECT_NEAR(delayed.value(), 1.055, kTolerance);
  const auto frozen_at_zero = compute_stop_join_elapsed_s({1.0, 0.0, 0.2, 0.02, std::nullopt});
  ASSERT_TRUE(frozen_at_zero.ok());
  EXPECT_DOUBLE_EQ(frozen_at_zero.value(), 1.0);
}

TEST(StopJoinElapsedTest, RefusesAStartAtOrAfterTheLeadInEnd) {
  const auto inside = compute_stop_join_elapsed_s({1.0, 0.5, 0.01, 0.02, 1.025});
  ASSERT_TRUE(inside.ok());
  const auto at_end = compute_stop_join_elapsed_s({1.0, 0.5, 0.03, 0.02, 1.025});
  ASSERT_FALSE(at_end.ok());
  EXPECT_EQ(at_end.error().details.at("reason"), "stop_delta_budget_exceeded");
  EXPECT_EQ(at_end.error().details.at("recommended_disposition"), "hard_stop");
  EXPECT_FALSE(compute_stop_join_elapsed_s({1.0, 0.5, -0.01, 0.02, std::nullopt}).ok());
  EXPECT_FALSE(compute_stop_join_elapsed_s({1.0, 0.5, 0.01, -0.02, std::nullopt}).ok());
  EXPECT_FALSE(compute_stop_join_elapsed_s({1.0, 1.5, 0.01, 0.02, std::nullopt}).ok());
}

TEST(StopReplacementStartTest, AlignsControllerHeaderAndCurveOriginTogetherAtTheAppliedFactor) {
  ExecutionLaneFactsDTO lane;
  lane.supports_scheduled_replacement = true;
  lane.supports_scale = true;
  lane.controller_update_rate_hz = 100.0;
  ExecutionControllerSampleDTO sample;
  sample.binding_epoch = ExecutionBindingEpoch(7U);
  sample.source_stamp_ns = 9'000'000'000;
  // Deliberately unrelated clock origins: receipt time is not a ROS stamp.
  sample.local_receipt_stamp_ns = 600'000'000;
  sample.cursor_s = 1.0;
  for (const double factor : {1.0, 0.5, 0.25, 0.0}) {
    sample.applied_factor = factor;
    const auto start = compute_stop_replacement_start(
        {sample.cursor_s, factor, 0.003, 0.02, 1.2}, sample, lane);
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(start.value().scheduled);
    EXPECT_EQ(start.value().scheduled->binding_epoch, sample.binding_epoch);
    EXPECT_EQ(start.value().scheduled->controller_start_stamp_ns, 9'030'000'000);
    EXPECT_NEAR(start.value().elapsed_path_s, 1.0 + factor * 0.03, 1e-12);
    if (factor == 0.0) continue;  // Stop itself retains the cancel-only path.
    const auto active = Curved();
    const auto continuation = Continuation(active, 1.0, factor, 0.2);
    const auto joined = build_stop_replacement_join(active, start.value().elapsed_path_s,
                                                    continuation, 1.0, 1.0 + factor * 0.2);
    ASSERT_TRUE(joined.ok());
    EXPECT_LT(joined.value().waypoints.front().time_from_start_s, 0.0);
    for (std::size_t joint = 0; joint < active.joint_names.size(); ++joint) {
      const auto before = Evaluate(active, joint, start.value().elapsed_path_s);
      const auto after = Evaluate(joined.value(), joint, 0.0);
      EXPECT_NEAR(before.p, after.p, 1e-12);
      EXPECT_NEAR(before.v, after.v, 1e-11);
      EXPECT_NEAR(before.a, after.a, 1e-10);
    }
  }
}

TEST(StopReplacementStartTest, KeepsExactTicksAndUsesTheControllersIntegerPeriod) {
  ExecutionLaneFactsDTO lane;
  lane.supports_scheduled_replacement = true;
  lane.controller_update_rate_hz = 100.0;
  ExecutionControllerSampleDTO sample;
  sample.binding_epoch = ExecutionBindingEpoch(1U);
  sample.source_stamp_ns = 1'000'000'000;
  const auto exact = compute_stop_replacement_start({1.0, 1.0, 0.0, 0.02, {}}, sample, lane);
  ASSERT_TRUE(exact.ok());
  ASSERT_TRUE(exact.value().scheduled);
  EXPECT_EQ(exact.value().scheduled->controller_start_stamp_ns, 1'020'000'000);
  lane.controller_update_rate_hz = 150.0;
  const auto truncated = compute_stop_replacement_start({1.0, 1.0, 0.0, 0.02, {}}, sample, lane);
  ASSERT_TRUE(truncated.ok());
  ASSERT_TRUE(truncated.value().scheduled);
  // Three integer periods are 19,999,998ns, strictly short of the allowance.
  EXPECT_EQ(truncated.value().scheduled->controller_start_stamp_ns, 1'026'666'664);
  EXPECT_NEAR(truncated.value().elapsed_path_s, 1.026666664, 1e-12);
}

TEST(StopReplacementStartTest, PreservesLegacyTimingAndRejectsInvalidOrExpiredScheduledStarts) {
  ExecutionLaneFactsDTO lane;
  ExecutionControllerSampleDTO sample;
  const auto legacy = compute_stop_replacement_start({1.0, 0.5, 0.003, 0.02, {}}, sample, lane);
  ASSERT_TRUE(legacy.ok());
  EXPECT_FALSE(legacy.value().scheduled);
  EXPECT_NEAR(legacy.value().elapsed_path_s, 1.0115, 1e-12);
  lane.supports_scheduled_replacement = true;
  lane.controller_update_rate_hz = 100.0;
  sample.binding_epoch = ExecutionBindingEpoch(1U);
  sample.source_stamp_ns = 1'000'000'000;
  // Legacy estimate is inside; advancing BOTH fields to the next tick is not.
  const auto expired = compute_stop_replacement_start({1.0, 0.5, 0.003, 0.02, 1.014}, sample, lane);
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().details.at("reason"), "stop_delta_budget_exceeded");
  for (int invalid = 0; invalid < 6; ++invalid) {
    auto bad_sample = sample;
    auto bad_lane = lane;
    auto input = StopJoinElapsedInputDTO{1.0, 0.5, 0.003, 0.02, {}};
    if (invalid == 0) bad_sample.binding_epoch = ExecutionBindingEpoch(0U);
    if (invalid == 1) bad_sample.source_stamp_ns = 0;
    if (invalid == 2) bad_sample.source_stamp_ns = std::numeric_limits<TimePoint>::max();
    if (invalid == 3) bad_lane.controller_update_rate_hz = 0.0;
    if (invalid == 4) input.frozen_factor = -0.1;
    if (invalid == 5) { bad_lane.supports_scale = true; bad_sample.applied_factor = 0.7; }
    EXPECT_FALSE(compute_stop_replacement_start(input, bad_sample, bad_lane).ok()) << invalid;
  }
}

// --- Experimental independent flags (stock JTC 6.10 recipe) --------------------

constexpr TimePoint kP150 = 6'666'666;  // the controller's integer period at 150 Hz

ExecutionLaneFactsDTO FlagLane(bool timer, bool midtick, bool v0 = false) {
  ExecutionLaneFactsDTO lane;
  lane.supports_scheduled_replacement = true;
  lane.supports_scale = true;
  lane.controller_update_rate_hz = 150.0;
  lane.experiment.release_timer = timer;
  lane.experiment.midtick_stamp = midtick;
  lane.experiment.v0_payload = v0;
  return lane;
}

ExecutionControllerSampleDTO FlagSample(TimePoint stamp, TimePoint receipt, double factor) {
  ExecutionControllerSampleDTO sample;
  sample.binding_epoch = ExecutionBindingEpoch(3U);
  sample.source_stamp_ns = stamp;
  sample.local_receipt_stamp_ns = receipt;  // unrelated local clock origin
  sample.cursor_s = 1.0;
  sample.applied_factor = factor;
  return sample;
}

TEST(ReplacementExperimentTest, TimerOnlyKeepsTheStampAndAimsArrivalHalfATickBeforeIt) {
  const auto sample = FlagSample(9'000'000'000, 600'000'000, 1.0);
  const StopJoinElapsedInputDTO input{1.0, 1.0, 0.002, 0.02, {}};
  const auto off = compute_stop_replacement_start(input, sample, FlagLane(false, false));
  const auto timer = compute_stop_replacement_start(input, sample, FlagLane(true, false));
  ASSERT_TRUE(off.ok() && timer.ok());
  EXPECT_FALSE(off.value().scheduled->experiment);
  // Same stamp policy (2 ms + 20 ms rounded up: 4 periods) and same D.
  EXPECT_EQ(timer.value().scheduled->controller_start_stamp_ns,
            off.value().scheduled->controller_start_stamp_ns);
  EXPECT_EQ(off.value().scheduled->controller_start_stamp_ns, 9'000'000'000 + 4 * kP150);
  EXPECT_EQ(timer.value().elapsed_path_s, off.value().elapsed_path_s);
  const auto& experiment = *timer.value().scheduled->experiment;
  EXPECT_EQ(experiment.source_stamp_ns, 9'000'000'000);
  EXPECT_EQ(experiment.target_tick_ns, 9'000'000'000 + 4 * kP150);
  ASSERT_TRUE(experiment.release_local_ns);
  EXPECT_EQ(*experiment.release_local_ns, 600'000'000 + 4 * kP150 - kP150 / 2 - 1'500'000);
  EXPECT_EQ(experiment.expected_latency_ns, 1'500'000);
  EXPECT_EQ(experiment.factor, 1.0);
  EXPECT_FALSE(experiment.midtick_stamp);
  EXPECT_FALSE(experiment.v0_payload);
}

TEST(ReplacementExperimentTest, MidtickOnlyShiftsTheStampAndDByHalfAPeriod) {
  const auto sample = FlagSample(9'000'000'000, 600'000'000, 0.5);
  const StopJoinElapsedInputDTO input{1.0, 0.5, 0.002, 0.02, {}};
  const auto midtick = compute_stop_replacement_start(input, sample, FlagLane(false, true));
  ASSERT_TRUE(midtick.ok());
  const auto& scheduled = *midtick.value().scheduled;
  EXPECT_EQ(scheduled.controller_start_stamp_ns, 9'000'000'000 + 4 * kP150 + kP150 / 2);
  EXPECT_NEAR(midtick.value().elapsed_path_s, 1.0 + 0.5 * (4.0 * kP150 + kP150 / 2) / 1e9, 1e-12);
  ASSERT_TRUE(scheduled.experiment);
  EXPECT_TRUE(scheduled.experiment->midtick_stamp);
  EXPECT_FALSE(scheduled.experiment->release_local_ns);
  EXPECT_EQ(scheduled.experiment->target_tick_ns, 9'000'000'000 + 4 * kP150);
}

TEST(ReplacementExperimentTest, V0OnlyRecordsTheFlagAndKeepsStampAndD) {
  const auto sample = FlagSample(9'000'000'000, 600'000'000, 1.0);
  const StopJoinElapsedInputDTO input{1.0, 1.0, 0.002, 0.02, {}};
  const auto off = compute_stop_replacement_start(input, sample, FlagLane(false, false));
  const auto v0 = compute_stop_replacement_start(input, sample, FlagLane(false, false, true));
  ASSERT_TRUE(off.ok() && v0.ok());
  EXPECT_EQ(v0.value().elapsed_path_s, off.value().elapsed_path_s);
  EXPECT_EQ(v0.value().scheduled->controller_start_stamp_ns,
            off.value().scheduled->controller_start_stamp_ns);
  ASSERT_TRUE(v0.value().scheduled->experiment);
  EXPECT_TRUE(v0.value().scheduled->experiment->v0_payload);
  EXPECT_FALSE(v0.value().scheduled->experiment->release_local_ns);
}

TEST(ReplacementExperimentTest, GridMathHoldsForJitteredTicksDelaysFactorsAndFlagCombinations) {
  // The sampled tick's own phase jitters (arbitrary stamps, up to +-6 % of P);
  // decision delays sweep two periods and more; factors down to 0.1.
  for (const bool timer : {false, true}) {
    for (const bool midtick : {false, true}) {
      const auto lane = FlagLane(timer, midtick);
      for (const double factor : {1.0, 0.5, 0.1}) {
        for (int jitter = -40; jitter <= 40; jitter += 13) {
          const TimePoint stamp = 5'000'000'000 + jitter * 9'973;
          for (int age_us = 0; age_us <= 25'000; age_us += 487) {
            const auto sample = FlagSample(stamp, 42'000'000, factor);
            const auto start = compute_stop_replacement_start(
                {1.0, factor, age_us * 1e-6, 0.02, {}}, sample, lane);
            ASSERT_TRUE(start.ok());
            const auto& scheduled = *start.value().scheduled;
            ASSERT_EQ(scheduled.experiment.has_value(), timer || midtick);
            const TimePoint now = 42'000'000 + static_cast<TimePoint>(age_us) * 1'000;
            const TimePoint tick = scheduled.controller_start_stamp_ns - (midtick ? kP150 / 2 : 0);
            EXPECT_EQ((tick - stamp) % kP150, 0);
            // Existing rule: the first grid tick at or after receipt age + allowance.
            EXPECT_GE(tick - stamp, (now - 42'000'000) + 20'000'000 - 1);
            EXPECT_LT(tick - stamp - kP150, (now - 42'000'000) + 20'000'000);
            EXPECT_NEAR(start.value().elapsed_path_s,
                        1.0 + factor * static_cast<double>(scheduled.controller_start_stamp_ns -
                                                           stamp) / 1e9,
                        1e-12);
            if (!scheduled.experiment) continue;
            EXPECT_EQ(scheduled.experiment->target_tick_ns, tick);
            EXPECT_EQ(scheduled.experiment->factor, factor);
            EXPECT_EQ(scheduled.experiment->release_local_ns.has_value(), timer);
            if (!timer) continue;
            // Release + latency maps to T - P/2 on the receipt-anchored clock.
            EXPECT_EQ(*scheduled.experiment->release_local_ns + 1'500'000 - 42'000'000,
                      tick - stamp - kP150 / 2);
            EXPECT_GE(*scheduled.experiment->release_local_ns - now,
                      20'000'000 - kP150 / 2 - 1'500'000 - 1);
          }
        }
      }
    }
  }
}

TEST(ReplacementExperimentTest, AReleaseAlreadyPassedIsVisibleAndChangesNothingElse) {
  // A zero allowance and an immediate decision put T on the sampled tick, so
  // the release lies P/2 + latency in the past: the port then sends at once
  // (missed), with the same stamp and D as without the timer.
  const auto sample = FlagSample(9'000'000'000, 600'000'000, 1.0);
  const StopJoinElapsedInputDTO input{1.0, 1.0, 0.0, 0.0, {}};
  const auto off = compute_stop_replacement_start(input, sample, FlagLane(false, false));
  const auto timer = compute_stop_replacement_start(input, sample, FlagLane(true, false));
  ASSERT_TRUE(off.ok() && timer.ok());
  EXPECT_LT(*timer.value().scheduled->experiment->release_local_ns, 600'000'000);
  EXPECT_EQ(timer.value().scheduled->controller_start_stamp_ns,
            off.value().scheduled->controller_start_stamp_ns);
  EXPECT_EQ(timer.value().elapsed_path_s, off.value().elapsed_path_s);
}

class CountingClock final : public IClock {
 public:
  TimePoint now() const override {
    ++reads;
    return value;
  }
  TimePoint value{0};
  mutable int reads{0};
};

TEST(ReplacementExperimentTest, ReleasePlanHoldsMissesOrRespectsTheLatestSendInstant) {
  const auto sample = FlagSample(9'000'000'000, 600'000'000, 1.0);
  const StopJoinElapsedInputDTO input{1.0, 1.0, 0.002, 0.02, {}};
  CountingClock clock;
  // No timer: no clock read, no hold, no outcome.
  for (const auto& lane : {FlagLane(false, false), FlagLane(false, true, true)}) {
    auto start = compute_stop_replacement_start(input, sample, lane);
    ASSERT_TRUE(start.ok());
    EXPECT_FALSE(plan_replacement_release(start.value(), clock, 0));
    EXPECT_EQ(clock.reads, 0);
  }
  const TimePoint release = 600'000'000 + 4 * kP150 - kP150 / 2 - 1'500'000;
  struct Case {
    TimePoint now, latest;
    ExecutionReleaseHold hold;
  };
  for (const auto& [now, latest, hold] :
       {Case{602'000'000, release, ExecutionReleaseHold::kHeld},
        Case{release, release + 1, ExecutionReleaseHold::kMissed},
        Case{release + 1, release + 1, ExecutionReleaseHold::kMissed},
        Case{602'000'000, release - 1, ExecutionReleaseHold::kDeadline}}) {
    auto start = compute_stop_replacement_start(input, sample, FlagLane(true, false));
    ASSERT_TRUE(start.ok());
    clock.value = now;
    const auto until = plan_replacement_release(start.value(), clock, latest);
    EXPECT_EQ(until.has_value(), hold == ExecutionReleaseHold::kHeld);
    if (until) {
      EXPECT_EQ(*until, release);
    }
    EXPECT_EQ(start.value().scheduled->experiment->release_hold, hold);
  }
}

TEST(ReplacementExperimentTest, AllowanceOverrideIsVisibleWithTimerOffAndAddsNoWaitOrClockRead) {
  const auto sample = FlagSample(9'000'000'000, 600'000'000, 1.0);
  CountingClock clock;
  for (const double allowance : {0.020, 0.030, 0.040}) {
    const StopJoinElapsedInputDTO input{1.0, 1.0, 0.002, allowance, {}};
    const auto plain = compute_stop_replacement_start(input, sample, FlagLane(false, false));
    auto lane = FlagLane(false, false);
    lane.experiment.start_allowance_override = true;
    auto observed = compute_stop_replacement_start(input, sample, lane);
    ASSERT_TRUE(plain.ok() && observed.ok());
    ASSERT_FALSE(plain.value().scheduled->experiment);
    ASSERT_TRUE(observed.value().scheduled->experiment);
    EXPECT_EQ(observed.value().scheduled->controller_start_stamp_ns,
              plain.value().scheduled->controller_start_stamp_ns);
    EXPECT_DOUBLE_EQ(observed.value().elapsed_path_s, plain.value().elapsed_path_s);
    const auto& experiment = *observed.value().scheduled->experiment;
    EXPECT_EQ(experiment.start_allowance_ns, std::llround(allowance * 1e9));
    EXPECT_TRUE(experiment.start_allowance_override);
    EXPECT_FALSE(experiment.release_local_ns);
    EXPECT_FALSE(plan_replacement_release(observed.value(), clock, 0));
    EXPECT_EQ(clock.reads, 0);
    EXPECT_EQ(experiment.release_hold, ExecutionReleaseHold::kNone);
  }
}

TEST(ReplacementExperimentTest, UnscheduledLanesIgnoreTheFlagsAndBadLatencyIsInvalid) {
  auto lane = FlagLane(true, true, true);
  lane.supports_scheduled_replacement = false;
  const auto start = compute_stop_replacement_start({1.0, 0.5, 0.003, 0.02, {}},
                                                    FlagSample(9'000'000'000, 1, 0.5), lane);
  ASSERT_TRUE(start.ok());
  EXPECT_FALSE(start.value().scheduled);
  EXPECT_NEAR(start.value().elapsed_path_s, 1.0115, 1e-12);
  for (const double latency : {-0.001, std::numeric_limits<double>::quiet_NaN(), 1.5}) {
    auto bad = FlagLane(true, false);
    bad.experiment.expected_latency_s = latency;
    EXPECT_FALSE(compute_stop_replacement_start({1.0, 0.5, 0.003, 0.02, {}},
                                                FlagSample(9'000'000'000, 1, 0.5), bad)
                     .ok());
  }
}

TEST(ReplacementExperimentTest, V0PayloadDropsThePrefixAndStartsWithThePlannedStateAtTheStamp) {
  const auto active = Curved();
  const double elapsed = 1.0125, factor = 0.5;
  const auto continuation = Continuation(active, 1.0, factor, 0.2);
  const auto joined =
      build_stop_replacement_join(active, elapsed, continuation, 1.0, 1.0 + factor * 0.2);
  ASSERT_TRUE(joined.ok());
  ASSERT_LT(joined.value().waypoints.front().time_from_start_s, 0.0);  // today's V6 prefix
  const auto v0 = stop_replacement_v0_payload(joined.value());
  ASSERT_TRUE(v0.ok());
  const auto& points = v0.value().waypoints;
  EXPECT_EQ(points.front().time_from_start_s, 0.0);
  for (std::size_t joint = 0; joint < active.joint_names.size(); ++joint) {
    const auto planned = Evaluate(active, joint, elapsed);
    EXPECT_NEAR(points.front().positions_rad[joint], planned.p, 1e-12);
    EXPECT_NEAR(points.front().velocities_rad_s[joint], planned.v, 1e-11);
    EXPECT_NEAR(points.front().accelerations_rad_s2[joint], planned.a, 1e-10);
  }
  std::size_t positive = 0;
  for (const auto& point : joined.value().waypoints)
    if (point.time_from_start_s > 1e-9) ++positive;
  ASSERT_EQ(points.size(), positive + 1U);
  for (std::size_t index = 1; index < points.size(); ++index) {
    EXPECT_GT(points[index].time_from_start_s, points[index - 1U].time_from_start_s);
    const auto& kept =
        joined.value().waypoints[joined.value().waypoints.size() - positive + index - 1U];
    EXPECT_EQ(points[index].time_from_start_s, kept.time_from_start_s);
    EXPECT_EQ(points[index].positions_rad, kept.positions_rad);
    EXPECT_EQ(points[index].velocities_rad_s, kept.velocities_rad_s);
  }
  // Commands after the stamp are unchanged: same polynomials from the first kept knot.
  for (double t = points[1].time_from_start_s; t < points.back().time_from_start_s; t += 0.01)
    for (std::size_t joint = 0; joint < 2U; ++joint)
      EXPECT_NEAR(Evaluate(v0.value(), joint, t).p, Evaluate(joined.value(), joint, t).p, 1e-12);
  EXPECT_FALSE(stop_replacement_v0_payload(TrajectoryDTO{}).ok());
}

// --- Experimental scheduled spring_back_fast (V0 shape) -------------------------

TEST(ScheduledFastSpringBackTest, StartsOnTheActiveCurveAtDAndKeepsTheAbsoluteStopTime) {
  const auto active = Curved();
  const double elapsed = 1.0, stop_path_s = 0.5, start = 1.0125;
  const std::vector<double> trip{0.4, -0.3};
  const auto stop = build_scheduled_spring_back_fast(active, start, trip, elapsed + stop_path_s);
  ASSERT_TRUE(stop.ok());
  const auto& points = stop.value().waypoints;
  ASSERT_EQ(points.size(), 2U);
  EXPECT_EQ(points.front().time_from_start_s, 0.0);
  const auto state = sample_trajectory(active, start).value();
  EXPECT_EQ(points.front().positions_rad, state.positions_rad);
  EXPECT_EQ(points.front().velocities_rad_s, state.velocities_rad_s);
  EXPECT_EQ(points.front().accelerations_rad_s2, state.accelerations_rad_s2);
  EXPECT_NEAR(points.back().time_from_start_s, elapsed + stop_path_s - start, 1e-15);
  EXPECT_EQ(points.back().positions_rad, trip);
  EXPECT_EQ(points.back().velocities_rad_s, std::vector<double>(2, 0.0));
  EXPECT_EQ(points.back().accelerations_rad_s2, std::vector<double>(2, 0.0));
  EXPECT_EQ(stop.value().joint_names, active.joint_names);
}

TEST(ScheduledFastSpringBackTest, RefusesAStartPastTheStopOrOutsideTheActivePath) {
  const auto active = Curved();
  const std::vector<double> trip{0.4, -0.3};
  for (const auto& [start, end] : {std::pair{1.5, 1.5}, std::pair{1.6, 1.5},
                                   std::pair{2.3, 2.8}, std::pair{1.0, 1.0 + 1e-10}}) {
    const auto stop = build_scheduled_spring_back_fast(active, start, trip, end);
    ASSERT_FALSE(stop.ok()) << start << " " << end;
    EXPECT_EQ(stop.error().details.at("reason"), "spring_back_fast_input_invalid");
  }
  EXPECT_FALSE(build_scheduled_spring_back_fast(active, 1.0, {0.4}, 1.5).ok());
}

TEST(ScheduledFastSpringBackTest, ProfileDifferenceAgainstTheZeroStampShape) {
  // Today: passed knots (shifted by -elapsed), then the stop point at the stop
  // time; on 6.10 the JTC splines from the LAST PASSED KNOT. Scheduled: the
  // spline starts at D on the active curve and ends at the same absolute time.
  const auto active = Curved();
  const double elapsed = 1.0, stop_path_s = 0.5, start = elapsed + 0.0125;  // H 12.5 ms ahead
  const std::vector<double> trip{std::sin(1.3 * 0.98) + 0.2 * 0.98 * 0.98,
                                 0.98 * std::cos(0.7 * 0.98)};
  TrajectoryDTO today;
  today.joint_names = active.joint_names;
  for (const auto& point : active.waypoints) {
    if (point.time_from_start_s >= elapsed) break;
    auto passed = point;
    passed.time_from_start_s -= elapsed;
    today.waypoints.push_back(passed);
  }
  today.waypoints.push_back(Point(stop_path_s, trip, {0.0, 0.0}, {0.0, 0.0}));
  const auto scheduled =
      build_scheduled_spring_back_fast(active, start, trip, elapsed + stop_path_s).value();
  const auto& from_today = today.waypoints[today.waypoints.size() - 2U];   // knot 0.9 -> -0.1
  const auto& to_today = today.waypoints.back();
  for (std::size_t joint = 0; joint < 2U; ++joint) {
    // At the handoff instant D the zero-stamp spline is off the active curve
    // (its segment began at the 0.9 s knot); the scheduled one is exactly on it.
    const auto on_curve = Evaluate(active, joint, start);
    const auto zero_stamp = Quintic(from_today, to_today, joint, start - elapsed);
    const auto v0 = Quintic(scheduled.waypoints[0], scheduled.waypoints[1], joint, 0.0);
    EXPECT_NEAR(v0.p, on_curve.p, 1e-12);
    EXPECT_NEAR(v0.v, on_curve.v, 1e-11);
    EXPECT_NEAR(v0.a, on_curve.a, 1e-10);
    // Peak |a| over each spline (dense scan) and the overshoot beyond the trip.
    double peak_today = 0.0, peak_scheduled = 0.0, over_today = 0.0, over_scheduled = 0.0;
    const double direction = on_curve.v >= 0.0 ? 1.0 : -1.0;
    for (int k = 0; k <= 20'000; ++k) {
      const double t_today = from_today.time_from_start_s +
                             (to_today.time_from_start_s - from_today.time_from_start_s) * k / 2e4;
      const double t_sched = scheduled.waypoints[1].time_from_start_s * k / 2e4;
      const auto a = Quintic(from_today, to_today, joint, t_today);
      const auto b = Quintic(scheduled.waypoints[0], scheduled.waypoints[1], joint, t_sched);
      if (t_today >= start - elapsed) peak_today = std::max(peak_today, std::abs(a.a));
      peak_scheduled = std::max(peak_scheduled, std::abs(b.a));
      over_today = std::max(over_today, direction * (a.p - trip[joint]));
      over_scheduled = std::max(over_scheduled, direction * (b.p - trip[joint]));
    }
    RecordProperty("joint" + std::to_string(joint) + "_handoff_jump_rad",
                   std::to_string(std::abs(zero_stamp.p - on_curve.p)));
    RecordProperty("joint" + std::to_string(joint) + "_peak_acc_today",
                   std::to_string(peak_today));
    RecordProperty("joint" + std::to_string(joint) + "_peak_acc_scheduled",
                   std::to_string(peak_scheduled));
    RecordProperty("joint" + std::to_string(joint) + "_overshoot_today",
                   std::to_string(over_today));
    RecordProperty("joint" + std::to_string(joint) + "_overshoot_scheduled",
                   std::to_string(over_scheduled));
    std::printf("joint%zu handoff_jump=%.3e rad peak_acc today=%.4f scheduled=%.4f "
                "overshoot today=%.5f scheduled=%.5f\n",
                joint, std::abs(zero_stamp.p - on_curve.p), peak_today, peak_scheduled,
                over_today, over_scheduled);
    EXPECT_GT(std::abs(zero_stamp.p - on_curve.p), 1e-6);
  }
}

// --- Path-time conversion ------------------------------------------------------

TEST(StopPathTimeConversionTest, ReproducesTheWallTimeMotionAtTheFrozenFactor) {
  TrajectoryDTO wall;
  wall.joint_names = {"j1"};
  wall.waypoints = {Point(0.0, {0.0}, {0.4}, {-0.8}), Point(0.5, {0.1}, {0.0}, {-0.8})};
  const auto path = convert_stop_trajectory_to_path_time(wall, 0.5);
  ASSERT_TRUE(path.ok());
  const auto& points = path.value().waypoints;
  EXPECT_DOUBLE_EQ(points[1].time_from_start_s, 0.25);
  EXPECT_EQ(points[1].positions_rad, wall.waypoints[1].positions_rad);
  // Applying the factor once gives back the wall-time derivatives.
  EXPECT_DOUBLE_EQ(0.5 * points[0].velocities_rad_s[0], 0.4);
  EXPECT_DOUBLE_EQ(0.25 * points[0].accelerations_rad_s2[0], -0.8);
  EXPECT_FALSE(convert_stop_trajectory_to_path_time(wall, 0.0).ok());
  wall.waypoints[0].velocities_rad_s.clear();
  EXPECT_FALSE(convert_stop_trajectory_to_path_time(wall, 1.0).ok());
}


TEST(StopSourceCursorTest, PlannedOffsetRefinesOnlyAgainstItsOwnReference) {
  for (const double rate : {100.0, 150.0}) for (const double factor : {1.0, 0.5, 0.1}) {
    ExecutionLaneFactsDTO lane;
    lane.joint_names = {"j1"};
    lane.controller_update_rate_hz = rate;
    const auto period_ns = static_cast<TimePoint>(1e9 / rate);
    const double planned = factor * static_cast<double>(period_ns / 2) / 1e9;
    for (const int kind : {0, 1, 2, 3}) {
      SCOPED_TRACE(::testing::Message() << rate << " factor=" << factor << " kind=" << kind);
      auto source = Linear();
      const double b = kind == 1 ? 0.0 : planned;
      const double actual = kind == 2 ? 0.0 : b;
      ExecutionControllerSampleDTO sample;
      sample.cursor_s = 0.5;
      sample.reference_positions_rad = {0.5 * (sample.cursor_s - actual)};
      sample.reference_velocities_rad_s = {0.5};
      if (kind == 3) {
        for (auto& point : source.waypoints) {
          point.positions_rad = {0.2 + 1e-12 * point.time_from_start_s};
          point.velocities_rad_s = {1e-12};
        }
        sample.reference_positions_rad = {0.2};
        sample.reference_velocities_rad_s = {1e-12};
      }
      EXPECT_NEAR(estimate_stop_source_cursor(source, sample, lane, b),
                  sample.cursor_s - (kind == 3 ? b : actual), 1e-12);
    }
  }
}

TEST(StopSourceCursorTest, UsesJointOrderAndKeepsTheEstimateInsideTheStockBridge) {
  auto source = Linear();
  source.joint_names.push_back("j2");
  for (auto& point : source.waypoints) {
    point.positions_rad.push_back(-0.25 * point.time_from_start_s);
    point.velocities_rad_s.push_back(-0.25);
    point.accelerations_rad_s2.push_back(0.0);
  }
  ExecutionLaneFactsDTO lane;
  lane.joint_names = {"j2", "j1"};
  lane.controller_update_rate_hz = 100.0;
  ExecutionControllerSampleDTO sample;
  sample.cursor_s = 0.5;
  sample.reference_positions_rad = {-0.125, 0.25};
  sample.reference_velocities_rad_s = {-0.25, 0.5};
  EXPECT_NEAR(estimate_stop_source_cursor(source, sample, lane, 0.005), 0.5, 1e-12);
  sample.cursor_s = 0.014;
  EXPECT_DOUBLE_EQ(estimate_stop_source_cursor(source, sample, lane, 0.005), 0.009);
  sample.cursor_s = 0.5;
  sample.reference_positions_rad[0] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_DOUBLE_EQ(estimate_stop_source_cursor(source, sample, lane, 0.005), 0.495);
  source.waypoints[0].velocities_rad_s.clear();
  EXPECT_DOUBLE_EQ(estimate_stop_source_cursor(source, sample, lane, 0.005), 0.495);
}

TEST(StopReplacementStartTest, PlannedOffsetAndActualCoreClipPreserveTheRemainingCurve) {
  for (const double rate : {100.0, 150.0}) for (const double factor : {1.0, 0.5, 0.1}) {
    auto lane = FlagLane(true, true);
    lane.controller_update_rate_hz = rate;
    auto sample = FlagSample(9'000'000'000, 600'000'000, factor);
    const auto start = compute_stop_replacement_start({1.0, factor, 0.0, 0.02, {}}, sample, lane);
    ASSERT_TRUE(start.ok());
    const auto period_ns = static_cast<TimePoint>(1e9 / rate);
    const double period = static_cast<double>(period_ns) / 1e9;
    EXPECT_NEAR(start.value().controller_offset_s,
                factor * static_cast<double>(period_ns / 2) / 1e9, 1e-15);
    const double first = period - start.value().controller_offset_s;
    const auto active = Curved();
    const auto continuation = Continuation(active, 1.0, factor, 0.2);
    const auto joined = build_stop_replacement_join(active, start.value().elapsed_path_s,
                                                    continuation, 1.0, 1.0 + factor * 0.2);
    ASSERT_TRUE(joined.ok());
    const auto clipped = stop_replacement_v0_payload(joined.value(), first);
    ASSERT_TRUE(clipped.ok());
    ASSERT_EQ(clipped.value().waypoints.front().time_from_start_s, first);
    for (const double time : {first, first + 0.001,
                             clipped.value().waypoints.back().time_from_start_s}) {
      const auto original = sample_trajectory(joined.value(), time).value();
      const auto actual = sample_trajectory(clipped.value(), time).value();
      for (std::size_t j = 0; j < 2U; ++j) {
        EXPECT_NEAR(original.positions_rad[j], actual.positions_rad[j], 1e-10);
        EXPECT_NEAR(original.velocities_rad_s[j], actual.velocities_rad_s[j], 1e-9);
        EXPECT_NEAR(original.accelerations_rad_s2[j], actual.accelerations_rad_s2[j], 1e-7);
      }
    }
    EXPECT_EQ(clipped.value().waypoints.back().positions_rad,
              joined.value().waypoints.back().positions_rad);
    EXPECT_EQ(clipped.value().waypoints.back().time_from_start_s,
              joined.value().waypoints.back().time_from_start_s);
  }
}

}  // namespace
}  // namespace manyforge::core
