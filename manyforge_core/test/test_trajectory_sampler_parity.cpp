// Parity tests proving that the shared trajectory sampler reproduces the
// observable output of the existing per-call-site interpolation helpers
// on fixtures where backwards compatibility is required, and matches the
// existing soft-on-path planner-local Hermite/quintic math on fixtures
// where both implementations are polynomial-based.
//
// These tests do NOT compare new cubic/quintic output against old
// linear-only helpers. Those call sites' linear behavior is the
// obsoleted reference; the type-aware split below ensures the parity
// scope is restricted to what backwards compatibility actually means.

#include <manyforge_core/trajectory/trajectory_sampler.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using manyforge::core::sample_trajectory;
using manyforge::core::to_string;
using manyforge::core::TrajectoryDTO;
using manyforge::core::TrajectorySampleError;
using manyforge::core::TrajectorySampleResult;
using manyforge::core::TrajectoryWaypointDTO;

// Tolerances pinned by the sampler API contract.
constexpr double kPosAbsTol = 1e-9;
constexpr double kVelAbsTol = 1e-7;
constexpr double kAccAbsTol = 1e-5;

// ----------------------------------------------------------------------------
// Reference implementations (kept inline as the documented "behavior before
// migration"). When call sites are swapped over to the shared sampler, the
// observable behavior on the fixtures below must remain stable. The
// reference formulas here are the canonical record of what the old
// per-call-site implementations did.
// ----------------------------------------------------------------------------

// Reference: linear position interpolation as previously implemented by
// the native RoboPlan execution backend's per-tick position projector.
// Inclusive boundary semantics: time at or before t_first holds the first
// waypoint position; time at or after t_last holds the last waypoint
// position. Linear inside; positions only.
std::vector<double> reference_linear_position(const TrajectoryDTO& traj,
                                              double time_s) {
  if (traj.waypoints.empty()) return {};
  if (traj.waypoints.size() == 1 ||
      time_s <= traj.waypoints.front().time_from_start_s) {
    return traj.waypoints.front().positions_rad;
  }
  if (time_s >= traj.waypoints.back().time_from_start_s) {
    return traj.waypoints.back().positions_rad;
  }
  for (std::size_t i = 1; i < traj.waypoints.size(); ++i) {
    const auto& prev = traj.waypoints[i - 1];
    const auto& next = traj.waypoints[i];
    if (time_s > next.time_from_start_s) continue;
    const double dt = next.time_from_start_s - prev.time_from_start_s;
    if (dt <= 1e-9) return next.positions_rad;
    const double alpha = std::clamp(
        (time_s - prev.time_from_start_s) / dt, 0.0, 1.0);
    std::vector<double> out(prev.positions_rad.size(), 0.0);
    for (std::size_t j = 0; j < prev.positions_rad.size(); ++j) {
      out[j] = prev.positions_rad[j] +
               alpha * (next.positions_rad[j] - prev.positions_rad[j]);
    }
    return out;
  }
  return traj.waypoints.back().positions_rad;
}

struct ReferenceSample {
  std::vector<double> position;
  std::vector<double> velocity;
  std::vector<double> acceleration;
};

// Reference: planner-local Hermite/quintic sampler as previously
// implemented by the soft-on-path stop planner. Endpoint clamp at
// time <= t_first and time >= t_last; otherwise per-segment polynomial
// chosen by which derivative vectors are present at both endpoints.
ReferenceSample reference_soft_on_path_sample(const TrajectoryDTO& traj,
                                              double time_s) {
  const std::size_t joint_count = traj.joint_names.size();
  ReferenceSample sample;

  if (time_s <= traj.waypoints.front().time_from_start_s) {
    const auto& wp = traj.waypoints.front();
    sample.position = wp.positions_rad;
    sample.velocity.assign(joint_count, 0.0);
    sample.acceleration.assign(joint_count, 0.0);
    if (wp.velocities_rad_s.size() == joint_count) {
      sample.velocity = wp.velocities_rad_s;
    }
    if (wp.accelerations_rad_s2.size() == joint_count) {
      sample.acceleration = wp.accelerations_rad_s2;
    }
    return sample;
  }
  if (time_s >= traj.waypoints.back().time_from_start_s) {
    const auto& wp = traj.waypoints.back();
    sample.position = wp.positions_rad;
    sample.velocity.assign(joint_count, 0.0);
    sample.acceleration.assign(joint_count, 0.0);
    if (wp.velocities_rad_s.size() == joint_count) {
      sample.velocity = wp.velocities_rad_s;
    }
    if (wp.accelerations_rad_s2.size() == joint_count) {
      sample.acceleration = wp.accelerations_rad_s2;
    }
    return sample;
  }

  std::size_t next_index = 1;
  while (next_index < traj.waypoints.size() &&
         time_s > traj.waypoints[next_index].time_from_start_s) {
    ++next_index;
  }
  const auto& p0 = traj.waypoints[next_index - 1];
  const auto& p1 = traj.waypoints[next_index];
  const double h = p1.time_from_start_s - p0.time_from_start_s;
  const double s = std::clamp(
      (time_s - p0.time_from_start_s) / h, 0.0, 1.0);
  const bool has_vel = p0.velocities_rad_s.size() == joint_count &&
                       p1.velocities_rad_s.size() == joint_count;
  const bool has_acc = has_vel &&
                       p0.accelerations_rad_s2.size() == joint_count &&
                       p1.accelerations_rad_s2.size() == joint_count;

  sample.position.assign(joint_count, 0.0);
  sample.velocity.assign(joint_count, 0.0);
  sample.acceleration.assign(joint_count, 0.0);

  for (std::size_t j = 0; j < joint_count; ++j) {
    const double q0 = p0.positions_rad[j];
    const double q1 = p1.positions_rad[j];
    if (has_acc) {
      const double v0 = p0.velocities_rad_s[j];
      const double v1 = p1.velocities_rad_s[j];
      const double a0 = p0.accelerations_rad_s2[j];
      const double a1 = p1.accelerations_rad_s2[j];
      const double c0 = q0;
      const double c1 = v0 * h;
      const double c2 = 0.5 * a0 * h * h;
      const double p = q1 - c0 - c1 - c2;
      const double v = v1 * h - c1 - 2.0 * c2;
      const double a = a1 * h * h - 2.0 * c2;
      const double c3 = 10.0 * p - 4.0 * v + 0.5 * a;
      const double c4 = -15.0 * p + 7.0 * v - a;
      const double c5 = 6.0 * p - 3.0 * v + 0.5 * a;
      sample.position[j] = c0 + c1 * s + c2 * s * s + c3 * s * s * s +
                           c4 * s * s * s * s + c5 * s * s * s * s * s;
      sample.velocity[j] =
          (c1 + 2.0 * c2 * s + 3.0 * c3 * s * s +
           4.0 * c4 * s * s * s + 5.0 * c5 * s * s * s * s) / h;
      sample.acceleration[j] =
          (2.0 * c2 + 6.0 * c3 * s + 12.0 * c4 * s * s +
           20.0 * c5 * s * s * s) / (h * h);
    } else if (has_vel) {
      const double v0 = p0.velocities_rad_s[j];
      const double v1 = p1.velocities_rad_s[j];
      const double h00 = 2.0 * s * s * s - 3.0 * s * s + 1.0;
      const double h10 = s * s * s - 2.0 * s * s + s;
      const double h01 = -2.0 * s * s * s + 3.0 * s * s;
      const double h11 = s * s * s - s * s;
      sample.position[j] = h00 * q0 + h10 * h * v0 + h01 * q1 + h11 * h * v1;
      sample.velocity[j] =
          ((6.0 * s * s - 6.0 * s) * q0 +
           (3.0 * s * s - 4.0 * s + 1.0) * h * v0 +
           (-6.0 * s * s + 6.0 * s) * q1 +
           (3.0 * s * s - 2.0 * s) * h * v1) / h;
      sample.acceleration[j] =
          ((12.0 * s - 6.0) * q0 +
           (6.0 * s - 4.0) * h * v0 +
           (-12.0 * s + 6.0) * q1 +
           (6.0 * s - 2.0) * h * v1) / (h * h);
    } else {
      const double delta = q1 - q0;
      sample.position[j] = q0 + s * delta;
      sample.velocity[j] = delta / h;
      sample.acceleration[j] = 0.0;
    }
  }
  return sample;
}

// ----------------------------------------------------------------------------
// Fixture builders
// ----------------------------------------------------------------------------

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

// Sweep evaluator: evaluate both reference and sampler at the same times
// and assert position equivalence.
void AssertPositionParity(const TrajectoryDTO& traj,
                          const std::vector<double>& times) {
  for (double t : times) {
    SCOPED_TRACE(::testing::Message() << "t=" << t);
    const auto ref = reference_linear_position(traj, t);
    auto r = sample_trajectory(traj, t);
    ASSERT_TRUE(r.ok());
    const auto& s = r.value();
    ASSERT_EQ(s.positions_rad.size(), ref.size());
    for (std::size_t j = 0; j < ref.size(); ++j) {
      EXPECT_NEAR(ref[j], s.positions_rad[j], kPosAbsTol)
          << "joint " << j;
    }
  }
}

void AssertFullSampleParity(const TrajectoryDTO& traj,
                            const std::vector<double>& times) {
  for (double t : times) {
    SCOPED_TRACE(::testing::Message() << "t=" << t);
    const auto ref = reference_soft_on_path_sample(traj, t);
    auto r = sample_trajectory(traj, t);
    ASSERT_TRUE(r.ok());
    const auto& s = r.value();
    ASSERT_EQ(s.positions_rad.size(), ref.position.size());
    ASSERT_EQ(s.velocities_rad_s.size(), ref.velocity.size());
    ASSERT_EQ(s.accelerations_rad_s2.size(), ref.acceleration.size());
    for (std::size_t j = 0; j < ref.position.size(); ++j) {
      EXPECT_NEAR(ref.position[j], s.positions_rad[j], kPosAbsTol)
          << "position joint " << j;
      EXPECT_NEAR(ref.velocity[j], s.velocities_rad_s[j], kVelAbsTol)
          << "velocity joint " << j;
      EXPECT_NEAR(ref.acceleration[j], s.accelerations_rad_s2[j], kAccAbsTol)
          << "acceleration joint " << j;
    }
  }
}

// ============================================================================
// Category 1: position-only parity vs the previous linear projector.
// Backwards compatibility — sampler reduces to linear on position-only
// trajectories and must produce identical position output.
// ============================================================================

TEST(TrajectorySamplerParity, LinearPositionOnly_SingleSegment) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0, 10.0}));
  traj.waypoints.push_back(Waypoint(2.0, {4.0, 14.0}));
  AssertPositionParity(
      traj, {-1.0, 0.0, 0.5, 1.0, 1.5, 2.0, 3.0});
}

TEST(TrajectorySamplerParity, LinearPositionOnly_ThreeSegment) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {2.0}));
  traj.waypoints.push_back(Waypoint(2.5, {-1.0}));
  traj.waypoints.push_back(Waypoint(4.0, {3.0}));
  AssertPositionParity(
      traj, {-0.5, 0.0, 0.25, 1.0, 1.75, 2.5, 3.0, 4.0, 4.5});
}

TEST(TrajectorySamplerParity, LinearPositionOnly_ExactInternalWaypoint) {
  // At an exact internal waypoint time the position must match the
  // waypoint position itself regardless of which side the sampler
  // selects, because both segments meet at that position.
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {5.0}));
  traj.waypoints.push_back(Waypoint(2.0, {1.0}));
  AssertPositionParity(traj, {1.0});
}

TEST(TrajectorySamplerParity, LinearPositionOnly_SingleWaypoint) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.5, {1.5}));
  AssertPositionParity(traj, {-1.0, 0.0, 0.5, 1.0, 5.0});
}

// ============================================================================
// Category 2: full-state parity vs the previous soft-on-path planner
// polynomial sampler. Both implementations are polynomial-based at the
// same degrees (linear / cubic Hermite / quintic); divergence here would
// be a bug.
//
// Note: the reference clamps inclusively at the trajectory's outer
// endpoints (time <= t_first, time >= t_last). The new sampler clamps
// using strict inequalities (time < t_first, time > t_last) and samples
// the segment at the exact endpoints. These tests therefore evaluate at
// times strictly inside (t_first, t_last) where both implementations
// agree on the segment formula.
// ============================================================================

TEST(TrajectorySamplerParity, CubicHermite_AgainstPreviousPlannerMath) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2"};
  traj.waypoints.push_back(WaypointPV(0.0, {0.0, 0.0}, {1.0, -0.5}));
  traj.waypoints.push_back(WaypointPV(2.0, {3.0, 1.5}, {0.0, 0.0}));
  traj.waypoints.push_back(WaypointPV(3.5, {-1.0, 2.0}, {0.5, -0.25}));
  AssertFullSampleParity(traj, {0.1, 0.5, 1.0, 1.5, 1.9, 2.1, 2.5, 3.0, 3.4});
}

TEST(TrajectorySamplerParity, Quintic_AgainstPreviousPlannerMath) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(
      WaypointPVA(0.0, {0.0}, {0.0}, {0.0}));
  traj.waypoints.push_back(
      WaypointPVA(1.0, {2.0}, {1.5}, {1.0}));
  traj.waypoints.push_back(
      WaypointPVA(2.5, {-0.5}, {0.0}, {-0.5}));
  AssertFullSampleParity(traj, {0.1, 0.25, 0.5, 0.9, 1.1, 1.5, 2.0, 2.4});
}

// ============================================================================
// Category 3: malformed-input scope.
//
// Document the migration semantics for inputs that the previous
// per-callsite helpers tolerated permissively but that the shared
// sampler explicitly rejects. The previous helpers returned a best-
// effort positions vector or read past array ends; the new sampler
// returns Result::Err with a stable error code. Forwarding shims at
// each call site convert Err to an empty position vector AND log a
// structured warning so downstream diagnostics can detect the gap.
//
// These tests lock the new behavior; the previous behavior is
// preserved here in the inline references for migration audit.
// ============================================================================

TEST(TrajectorySamplerParity, MalformedInput_EmptyJointNames) {
  TrajectoryDTO traj;
  // joint_names left empty; waypoints carry no positions either.
  traj.waypoints.push_back(Waypoint(0.0, {}));
  traj.waypoints.push_back(Waypoint(1.0, {}));
  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(
      r.error().code,
      std::string(to_string(
          manyforge::core::TrajectorySampleError::kEmptyJointNames)));
}

TEST(TrajectorySamplerParity, MalformedInput_PositionJointCountMismatch) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1", "j2"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0, 0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));  // wrong width
  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(
      r.error().code,
      std::string(to_string(
          manyforge::core::TrajectorySampleError::kPositionJointCountMismatch)));
}

TEST(TrajectorySamplerParity, MalformedInput_NonMonotonicWaypointTimes) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));
  traj.waypoints.push_back(Waypoint(0.5, {0.5}));  // goes backwards
  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(
      r.error().code,
      std::string(to_string(
          manyforge::core::TrajectorySampleError::kNonMonotonicWaypointTimes)));
}

TEST(TrajectorySamplerParity, MalformedInput_DuplicateWaypointTimes) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.5}));  // duplicate time
  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(
      r.error().code,
      std::string(to_string(
          manyforge::core::TrajectorySampleError::kDuplicateWaypointTimes)));
}

TEST(TrajectorySamplerParity, MalformedInput_NonFiniteSampleTime) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(Waypoint(1.0, {1.0}));
  for (double t : {std::numeric_limits<double>::quiet_NaN(),
                   std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity()}) {
    auto r = sample_trajectory(traj, t);
    ASSERT_FALSE(r.ok()) << "non-finite t=" << t;
    EXPECT_EQ(
        r.error().code,
        std::string(to_string(
            manyforge::core::TrajectorySampleError::kNonFiniteSampleTime)));
  }
}

TEST(TrajectorySamplerParity, MalformedInput_NonFiniteWaypointTime) {
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  traj.waypoints.push_back(Waypoint(0.0, {0.0}));
  traj.waypoints.push_back(
      Waypoint(std::numeric_limits<double>::infinity(), {1.0}));
  auto r = sample_trajectory(traj, 0.5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(
      r.error().code,
      std::string(to_string(
          manyforge::core::TrajectorySampleError::kNonFiniteWaypointTime)));
}

TEST(TrajectorySamplerParity, MixedDegrees_PositionOnlySegmentInTheMiddle) {
  // First segment carries velocity; middle waypoint has no velocity;
  // last segment carries velocity again. Both reference and sampler
  // fall back to linear on the middle segment, cubic on the others.
  TrajectoryDTO traj;
  traj.joint_names = {"j1"};
  TrajectoryWaypointDTO w0;
  w0.time_from_start_s = 0.0;
  w0.positions_rad = {0.0};
  w0.velocities_rad_s = {1.0};
  TrajectoryWaypointDTO w1;
  w1.time_from_start_s = 1.0;
  w1.positions_rad = {1.0};
  // no velocity on w1
  TrajectoryWaypointDTO w2;
  w2.time_from_start_s = 2.0;
  w2.positions_rad = {2.0};
  w2.velocities_rad_s = {0.5};
  traj.waypoints = {w0, w1, w2};
  AssertFullSampleParity(traj, {0.25, 0.5, 0.99, 1.01, 1.5, 1.75});
}

}  // namespace
