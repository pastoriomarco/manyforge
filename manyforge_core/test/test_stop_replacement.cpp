#include <gtest/gtest.h>

#include <cmath>
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

// --- Q42 closed-form brake ---------------------------------------------------

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

// --- Q20 join ------------------------------------------------------------------

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

// --- Q26 elapsed and Q22 lead-in rule -----------------------------------------

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
    if (factor == 0.0) continue;  // Stop itself retains Q45's cancel-only path.
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

// --- Q17/Q38 path-time conversion ----------------------------------------------

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

}  // namespace
}  // namespace manyforge::core
