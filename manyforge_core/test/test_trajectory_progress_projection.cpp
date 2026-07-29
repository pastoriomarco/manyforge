#include <gtest/gtest.h>

#include <array>
#include <manyforge_core/trajectory/trajectory_progress_projection.hpp>

namespace manyforge::core {
namespace {

TrajectoryDTO line_trajectory() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"j1", "j2"};
  trajectory.waypoints = {
      TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {}, {}},
      TrajectoryWaypointDTO{1.0, {1.0, 2.0}, {}, {}},
      TrajectoryWaypointDTO{2.0, {2.0, 4.0}, {}, {}},
  };
  return trajectory;
}

TEST(TrajectoryProgressProjectionTest, ProjectsMeasuredStateWithoutRegressingAcceptedProgress) {
  const auto result =
      project_trajectory_progress(line_trajectory(), std::array<double, 2>{1.5, 3.0}, 0.5);
  EXPECT_EQ(result.status, TrajectoryProgressProjectionStatus::kProjected);
  EXPECT_NEAR(result.trajectory_time_s, 1.5, 1e-12);
  EXPECT_NEAR(result.rms_distance_rad, 0.0, 1e-12);

  TrajectoryProgressProjectionConfig config;
  config.regression_tolerance_s = 0.0;
  const auto regressed =
      project_trajectory_progress(line_trajectory(), std::array<double, 2>{0.0, 0.0}, 1.5, config);
  EXPECT_EQ(regressed.status, TrajectoryProgressProjectionStatus::kProgressRegressed);
}

TEST(TrajectoryProgressProjectionTest, RejectsShapeMismatchAndRetracedPathAmbiguity) {
  const auto shape =
      project_trajectory_progress(line_trajectory(), std::array<double, 1>{0.5}, 0.0);
  EXPECT_EQ(shape.status, TrajectoryProgressProjectionStatus::kObservedStateShapeMismatch);

  TrajectoryDTO retraced;
  retraced.joint_names = {"j1"};
  retraced.waypoints = {
      TrajectoryWaypointDTO{0.0, {0.0}, {}, {}},
      TrajectoryWaypointDTO{1.0, {1.0}, {}, {}},
      TrajectoryWaypointDTO{2.0, {0.0}, {}, {}},
  };
  const auto ambiguous = project_trajectory_progress(retraced, std::array<double, 1>{0.5}, 0.0);
  EXPECT_EQ(ambiguous.status, TrajectoryProgressProjectionStatus::kProjectionAmbiguous);
}

TEST(TrajectoryProgressProjectionTest, AcceptsSlowForwardContinuousTrajectoryStart) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"j1"};
  trajectory.waypoints = {
      TrajectoryWaypointDTO{0.0, {0.0}, {}, {}},
      TrajectoryWaypointDTO{0.060677, {0.00005896}, {}, {}},
      TrajectoryWaypointDTO{0.121355, {0.0001776}, {}, {}},
  };

  const auto projected =
      project_trajectory_progress(trajectory, std::array<double, 1>{0.0}, 0.0);

  EXPECT_EQ(projected.status, TrajectoryProgressProjectionStatus::kProjected);
  EXPECT_NEAR(projected.trajectory_time_s, 0.0, 1e-12);
  EXPECT_EQ(projected.segment_index, 0U);
}

TEST(TrajectoryProgressProjectionTest, SlowStartDoesNotHideLaterDistinctBranch) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"j1"};
  trajectory.waypoints = {
      TrajectoryWaypointDTO{0.0, {0.0}, {}, {}},
      TrajectoryWaypointDTO{0.060677, {0.00005896}, {}, {}},
      TrajectoryWaypointDTO{0.121355, {0.0001776}, {}, {}},
      TrajectoryWaypointDTO{1.0, {1.0}, {}, {}},
      TrajectoryWaypointDTO{2.0, {0.00008}, {}, {}},
  };

  const auto ambiguous =
      project_trajectory_progress(trajectory, std::array<double, 1>{0.0}, 0.0);

  EXPECT_EQ(ambiguous.status, TrajectoryProgressProjectionStatus::kProjectionAmbiguous);
}

}  // namespace
}  // namespace manyforge::core
