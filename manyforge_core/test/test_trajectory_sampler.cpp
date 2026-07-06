#include <manyforge_core/trajectory/trajectory_sampler.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using manyforge::core::sample_trajectory;
using manyforge::core::to_string;
using manyforge::core::TrajectoryDTO;
using manyforge::core::TrajectorySampleError;
using manyforge::core::TrajectorySampleFallback;
using manyforge::core::TrajectorySampleResult;
using manyforge::core::TrajectoryWaypointDTO;

// Tolerance pinned by the API contract.
constexpr double kPosAbsTol = 1e-9;
constexpr double kPosRelTol = 1e-12;
constexpr double kVelAbsTol = 1e-7;
constexpr double kAccAbsTol = 1e-5;

::testing::AssertionResult ApproxEq(double expected, double actual,
                                    double abs_tol, double rel_tol = 0.0) {
  const double diff = std::abs(expected - actual);
  if (diff <= abs_tol) return ::testing::AssertionSuccess();
  if (rel_tol > 0.0) {
    const double scale = std::max(std::abs(expected), std::abs(actual));
    if (diff <= rel_tol * scale) return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "expected=" << expected << " actual=" << actual
         << " diff=" << diff << " abs_tol=" << abs_tol;
}

TrajectoryWaypointDTO Waypoint(double t, std::vector<double> p) {
  TrajectoryWaypointDTO wp;
  wp.time_from_start_s = t;
  wp.positions_rad = std::move(p);
  return wp;
}

TrajectoryWaypointDTO WaypointPV(double t, std::vector<double> p,
                                 std::vector<double> v) {
  TrajectoryWaypointDTO wp;
  wp.time_from_start_s = t;
  wp.positions_rad = std::move(p);
  wp.velocities_rad_s = std::move(v);
  return wp;
}

TrajectoryWaypointDTO WaypointPVA(double t, std::vector<double> p,
                                  std::vector<double> v,
                                  std::vector<double> a) {
  TrajectoryWaypointDTO wp;
  wp.time_from_start_s = t;
  wp.positions_rad = std::move(p);
  wp.velocities_rad_s = std::move(v);
  wp.accelerations_rad_s2 = std::move(a);
  return wp;
}

// ============================================================================
// Empty / single-waypoint / clamp behaviour
// ============================================================================

TEST(TrajectorySampler, EmptyTrajectoryReturnsClampedEmptyResult) {
  TrajectoryDTO traj;
  auto r = sample_trajectory(traj, 0.0);
  ASSERT_TRUE(r.ok());
  const auto& s = r.value();
  EXPECT_TRUE(s.positions_rad.empty());
  EXPECT_TRUE(s.velocities_rad_s.empty());
  EXPECT_TRUE(s.accelerations_rad_s2.empty());
  EXPECT_FALSE(s.segment_index.has_value());
  EXPECT_EQ(s.degree, 0u);
  EXPECT_TRUE(s.clamped);
  EXPECT_EQ(s.fallback_reason, TrajectorySampleFallback::kNone);
}

TEST(TrajectorySampler, SingleWaypointHoldsPositionWithZeroDerivatives) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2"};
  traj.waypoints.push_back(WaypointPV(0.0, {1.0, 2.0}, {5.0, 6.0}));

  for (double t : {-1.0, 0.0, 1.0, 1000.0}) {
    auto r = sample_trajectory(traj, t);
    ASSERT_TRUE(r.ok()) << "t=" << t;
    const auto& s = r.value();
    EXPECT_TRUE(ApproxEq(1.0, s.positions_rad[0], kPosAbsTol));
    EXPECT_TRUE(ApproxEq(2.0, s.positions_rad[1], kPosAbsTol));
    EXPECT_DOUBLE_EQ(0.0, s.velocities_rad_s[0]);
    EXPECT_DOUBLE_EQ(0.0, s.velocities_rad_s[1]);
    EXPECT_DOUBLE_EQ(0.0, s.accelerations_rad_s2[0]);
    EXPECT_DOUBLE_EQ(0.0, s.accelerations_rad_s2[1]);
    EXPECT_FALSE(s.segment_index.has_value());
    EXPECT_EQ(s.degree, 0u);
    EXPECT_TRUE(s.clamped);
  }
}

TEST(TrajectorySampler, PreStartClampUsesStrictInequality) {
  // A trajectory whose first waypoint has non-zero velocity. Sampling at
  // t < t_first must hold position and zero out velocity; sampling at
  // exactly t_first must return the first waypoint's stored velocity.
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(WaypointPV(1.0, {0.5}, {2.0}));
  traj.waypoints.push_back(WaypointPV(2.0, {1.5}, {2.0}));

  // Strictly before t_first: pre-start clamp, zero velocity.
  {
    auto r = sample_trajectory(traj, 0.5);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().clamped);
    EXPECT_DOUBLE_EQ(0.5, r.value().positions_rad[0]);
    EXPECT_DOUBLE_EQ(0.0, r.value().velocities_rad_s[0]);
    EXPECT_DOUBLE_EQ(0.0, r.value().accelerations_rad_s2[0]);
    EXPECT_FALSE(r.value().segment_index.has_value());
    EXPECT_EQ(r.value().degree, 0u);
  }

  // Exactly at t_first: NOT clamped. Samples segment 0 at s=0; cubic
  // polynomial returns the stored start velocity.
  {
    auto r = sample_trajectory(traj, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().clamped);
    EXPECT_TRUE(ApproxEq(0.5, r.value().positions_rad[0], kPosAbsTol));
    EXPECT_TRUE(ApproxEq(2.0, r.value().velocities_rad_s[0], kVelAbsTol));
    ASSERT_TRUE(r.value().segment_index.has_value());
    EXPECT_EQ(*r.value().segment_index, 0u);
    EXPECT_EQ(r.value().degree, 3u);
  }
}

TEST(TrajectorySampler, PostEndClampUsesStrictInequality) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(WaypointPV(0.0, {0.0}, {1.0}));
  traj.waypoints.push_back(WaypointPV(1.0, {1.0}, {3.0}));

  // Exactly at t_last: NOT clamped. Samples final segment at s=1; cubic
  // polynomial returns the stored terminal velocity.
  {
    auto r = sample_trajectory(traj, 1.0);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().clamped);
    EXPECT_TRUE(ApproxEq(1.0, r.value().positions_rad[0], kPosAbsTol));
    EXPECT_TRUE(ApproxEq(3.0, r.value().velocities_rad_s[0], kVelAbsTol));
    ASSERT_TRUE(r.value().segment_index.has_value());
    EXPECT_EQ(*r.value().segment_index, 0u);
    EXPECT_EQ(r.value().degree, 3u);
  }

  // Strictly after t_last: post-end clamp, zero velocity.
  {
    auto r = sample_trajectory(traj, 1.5);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().clamped);
    EXPECT_DOUBLE_EQ(1.0, r.value().positions_rad[0]);
    EXPECT_DOUBLE_EQ(0.0, r.value().velocities_rad_s[0]);
    EXPECT_DOUBLE_EQ(0.0, r.value().accelerations_rad_s2[0]);
    EXPECT_FALSE(r.value().segment_index.has_value());
    EXPECT_EQ(r.value().degree, 0u);
  }
}

// ============================================================================
// Linear segments (positions only)
// ============================================================================

TEST(TrajectorySampler, LinearMidpointMatchesClosedForm) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0, 10.0}));
  traj.waypoints.push_back(Waypoint(2.0, {4.0, 14.0}));

  auto r = sample_trajectory(traj, 1.0);
  ASSERT_TRUE(r.ok());
  const auto& s = r.value();
  EXPECT_EQ(s.degree, 1u);
  EXPECT_FALSE(s.clamped);
  ASSERT_TRUE(s.segment_index.has_value());
  EXPECT_EQ(*s.segment_index, 0u);
  EXPECT_TRUE(ApproxEq(2.0, s.positions_rad[0], kPosAbsTol));
  EXPECT_TRUE(ApproxEq(12.0, s.positions_rad[1], kPosAbsTol));
  // Constant velocity within a linear segment.
  EXPECT_TRUE(ApproxEq(2.0, s.velocities_rad_s[0], kVelAbsTol));
  EXPECT_TRUE(ApproxEq(2.0, s.velocities_rad_s[1], kVelAbsTol));
  // Zero acceleration on a linear segment.
  EXPECT_DOUBLE_EQ(0.0, s.accelerations_rad_s2[0]);
  EXPECT_DOUBLE_EQ(0.0, s.accelerations_rad_s2[1]);
}

TEST(TrajectorySampler, LinearC0ContinuityAtInternalJoin) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {2.0}));
  traj.waypoints.push_back(Waypoint(2.0, {1.0}));

  // Right-continuous rule: at exact internal time, sample the next
  // segment.
  auto r = sample_trajectory(traj, 1.0);
  ASSERT_TRUE(r.ok());
  const auto& s = r.value();
  ASSERT_TRUE(s.segment_index.has_value());
  EXPECT_EQ(*s.segment_index, 1u);
  EXPECT_TRUE(ApproxEq(2.0, s.positions_rad[0], kPosAbsTol));
  // Segment 1's constant velocity is (1 - 2) / (2 - 1) = -1.
  EXPECT_TRUE(ApproxEq(-1.0, s.velocities_rad_s[0], kVelAbsTol));
}

// ============================================================================
// Cubic Hermite segments (positions + velocities)
// ============================================================================

TEST(TrajectorySampler, CubicEndpointsExactlyMatchStoredPositions) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(WaypointPV(0.0, {0.0}, {1.0}));
  traj.waypoints.push_back(WaypointPV(2.0, {5.0}, {3.0}));

  auto r0 = sample_trajectory(traj, 0.0);
  ASSERT_TRUE(r0.ok());
  EXPECT_EQ(r0.value().degree, 3u);
  EXPECT_TRUE(ApproxEq(0.0, r0.value().positions_rad[0], kPosAbsTol));
  EXPECT_TRUE(ApproxEq(1.0, r0.value().velocities_rad_s[0], kVelAbsTol));

  auto r1 = sample_trajectory(traj, 2.0);
  ASSERT_TRUE(r1.ok());
  EXPECT_EQ(r1.value().degree, 3u);
  EXPECT_TRUE(ApproxEq(5.0, r1.value().positions_rad[0], kPosAbsTol));
  EXPECT_TRUE(ApproxEq(3.0, r1.value().velocities_rad_s[0], kVelAbsTol));
}

TEST(TrajectorySampler, CubicMidpointMatchesClosedForm) {
  // Hand-derived expected midpoint for cubic Hermite with:
  //   p_a = 0, v_a = 0, p_b = 1, v_b = 0, T = 1.
  // At s = 0.5:
  //   H00 = 1 - 3(0.25) + 2(0.125) = 0.5
  //   H10 = 0.5 - 2(0.25) + 0.125 = 0.125
  //   H01 = 3(0.25) - 2(0.125) = 0.5
  //   H11 = -0.25 + 0.125 = -0.125
  //   p = 0*0.5 + 1*0*0.125 + 1*0.5 + 1*0*(-0.125) = 0.5
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(WaypointPV(0.0, {0.0}, {0.0}));
  traj.waypoints.push_back(WaypointPV(1.0, {1.0}, {0.0}));

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().degree, 3u);
  EXPECT_TRUE(ApproxEq(0.5, r.value().positions_rad[0], kPosAbsTol));
  // Symmetric cubic with zero endpoint velocities reaches peak velocity
  // 1.5 / T = 1.5 at midpoint.
  EXPECT_TRUE(ApproxEq(1.5, r.value().velocities_rad_s[0], kVelAbsTol));
  // Acceleration crosses zero at midpoint.
  EXPECT_TRUE(ApproxEq(0.0, r.value().accelerations_rad_s2[0], kAccAbsTol));
}

TEST(TrajectorySampler, CubicC1AcrossInternalJoin) {
  // Two-segment cubic where the shared waypoint has a defined velocity.
  // Sampling the same waypoint from either side must agree on position
  // (C0) and velocity (C1). The right-continuous rule means the exact
  // internal time samples the second segment; check that the limit from
  // the previous segment also approaches the same velocity.
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(WaypointPV(0.0, {0.0}, {0.0}));
  traj.waypoints.push_back(WaypointPV(1.0, {1.0}, {2.0}));
  traj.waypoints.push_back(WaypointPV(2.0, {3.0}, {1.0}));

  auto right = sample_trajectory(traj, 1.0);
  ASSERT_TRUE(right.ok());
  ASSERT_TRUE(right.value().segment_index.has_value());
  EXPECT_EQ(*right.value().segment_index, 1u);
  EXPECT_TRUE(ApproxEq(1.0, right.value().positions_rad[0], kPosAbsTol));
  EXPECT_TRUE(ApproxEq(2.0, right.value().velocities_rad_s[0], kVelAbsTol));

  auto left_limit = sample_trajectory(traj, 0.9999999);
  ASSERT_TRUE(left_limit.ok());
  ASSERT_TRUE(left_limit.value().segment_index.has_value());
  EXPECT_EQ(*left_limit.value().segment_index, 0u);
  EXPECT_NEAR(1.0, left_limit.value().positions_rad[0], 1e-5);
  EXPECT_NEAR(2.0, left_limit.value().velocities_rad_s[0], 1e-4);
}

// ============================================================================
// Quintic segments (positions + velocities + accelerations)
// ============================================================================

TEST(TrajectorySampler, QuinticEndpointsExactlyMatchStoredStates) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(WaypointPVA(0.0, {0.0}, {1.0}, {2.0}));
  traj.waypoints.push_back(WaypointPVA(2.0, {5.0}, {3.0}, {-1.0}));

  auto r0 = sample_trajectory(traj, 0.0);
  ASSERT_TRUE(r0.ok());
  EXPECT_EQ(r0.value().degree, 5u);
  EXPECT_TRUE(ApproxEq(0.0, r0.value().positions_rad[0], kPosAbsTol));
  EXPECT_TRUE(ApproxEq(1.0, r0.value().velocities_rad_s[0], kVelAbsTol));
  EXPECT_TRUE(ApproxEq(2.0, r0.value().accelerations_rad_s2[0], kAccAbsTol));

  auto r1 = sample_trajectory(traj, 2.0);
  ASSERT_TRUE(r1.ok());
  EXPECT_EQ(r1.value().degree, 5u);
  EXPECT_TRUE(ApproxEq(5.0, r1.value().positions_rad[0], kPosAbsTol));
  EXPECT_TRUE(ApproxEq(3.0, r1.value().velocities_rad_s[0], kVelAbsTol));
  EXPECT_TRUE(ApproxEq(-1.0, r1.value().accelerations_rad_s2[0], kAccAbsTol));
}

TEST(TrajectorySampler, QuinticC2AcrossInternalJoin) {
  // Three waypoints with full velocity + acceleration data. The
  // right-continuous sampling rule still applies; the limit from the
  // previous segment must approach the same acceleration.
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(WaypointPVA(0.0, {0.0}, {0.0}, {0.0}));
  traj.waypoints.push_back(WaypointPVA(1.0, {1.0}, {2.0}, {3.0}));
  traj.waypoints.push_back(WaypointPVA(2.0, {3.0}, {1.0}, {-1.0}));

  auto right = sample_trajectory(traj, 1.0);
  ASSERT_TRUE(right.ok());
  ASSERT_TRUE(right.value().segment_index.has_value());
  EXPECT_EQ(*right.value().segment_index, 1u);
  EXPECT_TRUE(ApproxEq(1.0, right.value().positions_rad[0], kPosAbsTol));
  EXPECT_TRUE(ApproxEq(2.0, right.value().velocities_rad_s[0], kVelAbsTol));
  EXPECT_TRUE(ApproxEq(3.0, right.value().accelerations_rad_s2[0], kAccAbsTol));

  auto left_limit = sample_trajectory(traj, 0.9999999);
  ASSERT_TRUE(left_limit.ok());
  ASSERT_TRUE(left_limit.value().segment_index.has_value());
  EXPECT_EQ(*left_limit.value().segment_index, 0u);
  EXPECT_NEAR(1.0, left_limit.value().positions_rad[0], 1e-5);
  EXPECT_NEAR(2.0, left_limit.value().velocities_rad_s[0], 1e-4);
  EXPECT_NEAR(3.0, left_limit.value().accelerations_rad_s2[0], 1e-3);
}

// ============================================================================
// Internal-waypoint boundary rule (right-continuous; final endpoint uses
// the final segment at alpha=1).
// ============================================================================

TEST(TrajectorySampler, ExactInternalWaypointSamplesNextSegment) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));
  traj.waypoints.push_back(Waypoint(2.0, {-3.0}));

  // Segment 0: slope +1. Segment 1: slope -4. At t=1 the right-continuous
  // rule picks segment 1 → velocity -4.
  auto r = sample_trajectory(traj, 1.0);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(r.value().segment_index.has_value());
  EXPECT_EQ(*r.value().segment_index, 1u);
  EXPECT_TRUE(ApproxEq(1.0, r.value().positions_rad[0], kPosAbsTol));
  EXPECT_TRUE(ApproxEq(-4.0, r.value().velocities_rad_s[0], kVelAbsTol));
}

TEST(TrajectorySampler, ExactFinalEndpointSamplesFinalSegment) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));
  traj.waypoints.push_back(Waypoint(2.0, {-3.0}));

  // At t=2 (the final endpoint), sample segment 1 at alpha=1.
  auto r = sample_trajectory(traj, 2.0);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(r.value().segment_index.has_value());
  EXPECT_EQ(*r.value().segment_index, 1u);
  EXPECT_FALSE(r.value().clamped);
  EXPECT_TRUE(ApproxEq(-3.0, r.value().positions_rad[0], kPosAbsTol));
}

// ============================================================================
// Degree fallback paths
// ============================================================================

TEST(TrajectorySampler, IncompleteVelocityFallsBackToLinear) {
  // Only one endpoint carries velocity.
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(WaypointPV(0.0, {0.0}, {2.0}));
  traj.waypoints.push_back(Waypoint(1.0, {4.0}));

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().degree, 1u);
  EXPECT_EQ(r.value().fallback_reason,
            TrajectorySampleFallback::kIncompleteVelocityEndpoint);
}

TEST(TrajectorySampler, JointCountMismatchVelocityFallsBackToLinear) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2"};
  // Endpoint a has velocities of length 1; mismatched to joint_count = 2.
  TrajectoryWaypointDTO a;
  a.time_from_start_s = 0.0;
  a.positions_rad = {0.0, 0.0};
  a.velocities_rad_s = {1.0};  // wrong size
  TrajectoryWaypointDTO b;
  b.time_from_start_s = 1.0;
  b.positions_rad = {1.0, 1.0};
  b.velocities_rad_s = {1.0, 1.0};
  traj.waypoints = {a, b};

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().degree, 1u);
  EXPECT_EQ(r.value().fallback_reason,
            TrajectorySampleFallback::kJointCountMismatchVelocity);
}

TEST(TrajectorySampler, IncompleteAccelerationFallsBackToCubic) {
  // Both endpoints have velocity; only endpoint a has acceleration.
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(WaypointPVA(0.0, {0.0}, {0.0}, {1.0}));
  traj.waypoints.push_back(WaypointPV(1.0, {1.0}, {0.0}));

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().degree, 3u);
  EXPECT_EQ(r.value().fallback_reason,
            TrajectorySampleFallback::kIncompleteAccelerationEndpoint);
}

TEST(TrajectorySampler, JointCountMismatchAccelerationFallsBackToCubic) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2"};
  TrajectoryWaypointDTO a;
  a.time_from_start_s = 0.0;
  a.positions_rad = {0.0, 0.0};
  a.velocities_rad_s = {0.0, 0.0};
  a.accelerations_rad_s2 = {1.0};  // wrong size
  TrajectoryWaypointDTO b;
  b.time_from_start_s = 1.0;
  b.positions_rad = {1.0, 1.0};
  b.velocities_rad_s = {0.0, 0.0};
  b.accelerations_rad_s2 = {1.0, 1.0};
  traj.waypoints = {a, b};

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().degree, 3u);
  EXPECT_EQ(r.value().fallback_reason,
            TrajectorySampleFallback::kJointCountMismatchAcceleration);
}

// ============================================================================
// Malformed-input errors (returned via Result::Err, NOT fallback)
// ============================================================================

TEST(TrajectorySampler, EmptyJointNamesIsError) {
  TrajectoryDTO traj;
  // joint_names empty; one positionless waypoint.
  traj.waypoints.push_back(Waypoint(0.0, {}));
  traj.waypoints.push_back(Waypoint(1.0, {}));

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code,
            std::string(to_string(TrajectorySampleError::kEmptyJointNames)));
}

TEST(TrajectorySampler, PositionJointCountMismatchIsError) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0, 0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));  // wrong size

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code,
            std::string(to_string(
                TrajectorySampleError::kPositionJointCountMismatch)));
}

TEST(TrajectorySampler, NonMonotonicTimesIsError) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));
  traj.waypoints.push_back(Waypoint(0.5, {0.5}));  // goes backwards

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code,
            std::string(to_string(
                TrajectorySampleError::kNonMonotonicWaypointTimes)));
}

TEST(TrajectorySampler, DuplicateTimesIsError) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.5}));  // duplicate time

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code,
            std::string(to_string(
                TrajectorySampleError::kDuplicateWaypointTimes)));
}

TEST(TrajectorySampler, NonFiniteSampleTimeIsError) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));

  for (double t : {std::numeric_limits<double>::quiet_NaN(),
                   std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity()}) {
    auto r = sample_trajectory(traj, t);
    ASSERT_FALSE(r.ok())
        << "expected Err for non-finite time " << t;
    EXPECT_EQ(r.error().code,
              std::string(to_string(
                  TrajectorySampleError::kNonFiniteSampleTime)));
  }
}

TEST(TrajectorySampler, NonFiniteWaypointTimeIsError) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(
      Waypoint(std::numeric_limits<double>::infinity(), {1.0}));

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code,
            std::string(to_string(
                TrajectorySampleError::kNonFiniteWaypointTime)));

  TrajectoryDTO traj_nan;
  traj_nan.joint_names = {"j1"};
  traj_nan.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj_nan.waypoints.push_back(
      Waypoint(std::numeric_limits<double>::quiet_NaN(), {1.0}));

  auto r2 = sample_trajectory(traj_nan, 0.5);
  ASSERT_FALSE(r2.ok());
  EXPECT_EQ(r2.error().code,
            std::string(to_string(
                TrajectorySampleError::kNonFiniteWaypointTime)));
}

TEST(TrajectorySampler, AccelerationWithoutVelocityIsReported) {
  // Acceleration vectors are present at both endpoints but velocity is
  // missing. The quintic basis is unreachable and the sampler falls
  // back to linear. The fallback reason must surface the velocity gap
  // so consumers see that derivative data was supplied but could not
  // be honored — silently reporting kNone would hide the diagnostic.
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  TrajectoryWaypointDTO a;
  a.time_from_start_s = 0.0;
  a.positions_rad = {0.0};
  a.accelerations_rad_s2 = {1.0};  // present, but no velocity
  TrajectoryWaypointDTO b;
  b.time_from_start_s = 1.0;
  b.positions_rad = {1.0};
  b.accelerations_rad_s2 = {1.0};
  traj.waypoints = {a, b};

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().degree, 1u);
  EXPECT_EQ(r.value().fallback_reason,
            TrajectorySampleFallback::kIncompleteVelocityEndpoint);
}

TEST(TrajectorySampler, AccelerationAtOneEndpointWithoutVelocityIsReported) {
  // One endpoint has acceleration only; velocity is missing on both
  // endpoints. Same expectation: surface the velocity gap.
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  TrajectoryWaypointDTO a;
  a.time_from_start_s = 0.0;
  a.positions_rad = {0.0};
  a.accelerations_rad_s2 = {1.0};
  TrajectoryWaypointDTO b;
  b.time_from_start_s = 1.0;
  b.positions_rad = {1.0};
  traj.waypoints = {a, b};

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().degree, 1u);
  EXPECT_EQ(r.value().fallback_reason,
            TrajectorySampleFallback::kIncompleteVelocityEndpoint);
}

// ============================================================================
// Result-shape sanity (vector sizes, optional usage)
// ============================================================================

TEST(TrajectorySampler, SampledVectorsMatchJointCount) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2", "j3"};
  traj.waypoints.push_back(WaypointPVA(0.0, {0.0, 0.0, 0.0},
                                       {0.0, 0.0, 0.0},
                                       {0.0, 0.0, 0.0}));
  traj.waypoints.push_back(WaypointPVA(1.0, {1.0, 2.0, 3.0},
                                       {0.5, 1.0, 1.5},
                                       {0.0, 0.0, 0.0}));

  auto r = sample_trajectory(traj, 0.5);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().positions_rad.size(), 3u);
  EXPECT_EQ(r.value().velocities_rad_s.size(), 3u);
  EXPECT_EQ(r.value().accelerations_rad_s2.size(), 3u);
  ASSERT_TRUE(r.value().segment_index.has_value());
  EXPECT_EQ(*r.value().segment_index, 0u);
  EXPECT_EQ(r.value().degree, 5u);
}

TEST(TrajectorySampler, ClampResultDropsSegmentIndex) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));

  for (double t : {-1.0, 2.0}) {
    auto r = sample_trajectory(traj, t);
    ASSERT_TRUE(r.ok()) << "t=" << t;
    EXPECT_FALSE(r.value().segment_index.has_value()) << "t=" << t;
    EXPECT_TRUE(r.value().clamped) << "t=" << t;
    EXPECT_EQ(r.value().degree, 0u) << "t=" << t;
  }
}

TEST(TrajectorySampler, PrevalidatedSamplingIsBitIdenticalWithReusedBuffer) {
  using manyforge::core::sample_trajectory_prevalidated;
  using manyforge::core::validate_trajectory_for_sampling;

  // Mixed-degree trajectory: quintic-capable middle segment, cubic tail
  // (missing accelerations), so the sweep crosses every emit path plus
  // both clamps and exact endpoints.
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2"};
  traj.waypoints.push_back(
      WaypointPVA(0.0, {0.0, 1.0}, {0.4, -0.2}, {0.1, 0.0}));
  traj.waypoints.push_back(
      WaypointPVA(0.7, {0.5, 0.6}, {0.9, 0.3}, {-0.2, 0.05}));
  traj.waypoints.push_back(TrajectoryWaypointDTO{
      1.4, {1.0, 0.2}, {0.0, 0.0}, {}});

  ASSERT_TRUE(validate_trajectory_for_sampling(traj).ok());

  // One reused output across the whole sweep, deliberately pre-poisoned so
  // stale state would be caught if any field were left unwritten.
  TrajectorySampleResult reused;
  reused.positions_rad = {99.0, 99.0, 99.0};
  reused.segment_index = 7;
  reused.degree = 9;
  reused.fallback_reason =
      TrajectorySampleFallback::kJointCountMismatchVelocity;

  for (double t = -0.2; t <= 1.6; t += 0.05) {
    auto classic = sample_trajectory(traj, t);
    ASSERT_TRUE(classic.ok()) << "t=" << t;
    ASSERT_TRUE(sample_trajectory_prevalidated(traj, t, reused).ok())
        << "t=" << t;
    EXPECT_EQ(classic.value().positions_rad, reused.positions_rad)
        << "t=" << t;
    EXPECT_EQ(classic.value().velocities_rad_s, reused.velocities_rad_s)
        << "t=" << t;
    EXPECT_EQ(classic.value().accelerations_rad_s2,
              reused.accelerations_rad_s2)
        << "t=" << t;
    EXPECT_EQ(classic.value().segment_index, reused.segment_index)
        << "t=" << t;
    EXPECT_EQ(classic.value().degree, reused.degree) << "t=" << t;
    EXPECT_EQ(classic.value().clamped, reused.clamped) << "t=" << t;
    EXPECT_EQ(classic.value().fallback_reason, reused.fallback_reason)
        << "t=" << t;
  }

  // Per-call rejections still apply on the prevalidated path.
  EXPECT_FALSE(sample_trajectory_prevalidated(
                   traj, std::numeric_limits<double>::quiet_NaN(), reused)
                   .ok());
}

TEST(TrajectorySampler, ValidateForSamplingMatchesClassicRejections) {
  using manyforge::core::validate_trajectory_for_sampling;

  // Empty trajectory validates Ok (sampling yields the empty clamp).
  EXPECT_TRUE(validate_trajectory_for_sampling(TrajectoryDTO{}).ok());

  TrajectoryDTO no_names;
  no_names.waypoints.push_back(Waypoint(0.0, {0.0}));
  EXPECT_FALSE(validate_trajectory_for_sampling(no_names).ok());

  TrajectoryDTO duplicate_times;
  duplicate_times.joint_names = {"j1"};
  duplicate_times.waypoints.push_back(Waypoint(0.0, {0.0}));
  duplicate_times.waypoints.push_back(Waypoint(0.0, {1.0}));
  EXPECT_FALSE(validate_trajectory_for_sampling(duplicate_times).ok());
}

}  // namespace
