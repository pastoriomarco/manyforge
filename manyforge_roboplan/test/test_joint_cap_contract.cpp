#include <gtest/gtest.h>

#include <manyforge_roboplan/joint_cap_contract.hpp>

#include <map>
#include <string>

#include <Eigen/Dense>

using namespace manyforge::roboplan_backend;

// Phase 4-P5.1K: the cap-measurement helper must fail closed when a capped
// dimension lacks samples (rather than treating the missing peak as zero, which
// would falsely pass the cap), and must measure peaks/ratios when complete.
TEST(JointCapMeasurementTest, IncompleteVelocitySamplesFailClosed) {
  roboplan::JointTrajectory traj;
  traj.joint_names = {"elbow_joint"};
  traj.times = {0.0, 0.1};
  traj.positions = {Eigen::VectorXd::Zero(1), Eigen::VectorXd::Ones(1)};
  // velocities intentionally left EMPTY -> cannot verify a velocity cap.
  std::map<std::string, double> vel_caps{{"elbow_joint", 0.5}};
  std::map<std::string, double> acc_caps;

  auto m = measure_joint_cap_peaks(traj, vel_caps, acc_caps);
  EXPECT_FALSE(m.samples_complete);
  EXPECT_EQ(m.incomplete_dimension, "velocity");
  EXPECT_EQ(m.incomplete_joint, "elbow_joint");
}

TEST(JointCapMeasurementTest, CompleteSamplesMeasurePeakAndRatio) {
  roboplan::JointTrajectory traj;
  traj.joint_names = {"elbow_joint"};
  traj.times = {0.0, 0.1, 0.2};
  traj.positions = {Eigen::VectorXd::Zero(1), Eigen::VectorXd::Zero(1),
                    Eigen::VectorXd::Zero(1)};
  auto v0 = (Eigen::VectorXd(1) << 0.2).finished();
  auto v1 = (Eigen::VectorXd(1) << -1.0).finished();  // peak |v| = 1.0
  auto v2 = (Eigen::VectorXd(1) << 0.4).finished();
  traj.velocities = {v0, v1, v2};

  std::map<std::string, double> vel_caps{{"elbow_joint", 0.5}};  // peak 1.0 > 0.5
  std::map<std::string, double> acc_caps;

  auto m = measure_joint_cap_peaks(traj, vel_caps, acc_caps);
  EXPECT_TRUE(m.samples_complete);
  EXPECT_DOUBLE_EQ(m.worst_vel_peak, 1.0);
  EXPECT_EQ(m.worst_vel_joint, "elbow_joint");
  EXPECT_DOUBLE_EQ(m.worst_vel_ratio, 2.0);  // 1.0 / 0.5
  EXPECT_DOUBLE_EQ(m.worst_acc_ratio, 1.0);  // no accel cap -> unviolated
}

TEST(JointCapMeasurementTest, UncappedJointIgnoredEvenWithoutSamples) {
  // A joint present in the trajectory but NOT capped must not trigger the
  // incomplete-samples path (we only require samples for capped dimensions).
  roboplan::JointTrajectory traj;
  traj.joint_names = {"shoulder_pan_joint", "elbow_joint"};
  traj.times = {0.0, 0.1};
  traj.positions = {Eigen::VectorXd::Zero(2), Eigen::VectorXd::Zero(2)};
  // Only provide velocities for the (single) capped joint scenario: leave empty.
  std::map<std::string, double> vel_caps;  // no caps at all
  std::map<std::string, double> acc_caps;

  auto m = measure_joint_cap_peaks(traj, vel_caps, acc_caps);
  EXPECT_TRUE(m.samples_complete);
  EXPECT_DOUBLE_EQ(m.worst_vel_ratio, 1.0);
  EXPECT_DOUBLE_EQ(m.worst_acc_ratio, 1.0);
}

TEST(JointCapMeasurementTest, CappedJointAbsentFromTrajectoryFailsClosed) {
  // A capped joint that is not among the trajectory's joint names is never
  // sampled; treating it as satisfied would be a silent enforcement gap. The
  // measurement must fail closed and name the offending dimension/joint.
  roboplan::JointTrajectory traj;
  traj.joint_names = {"shoulder_pan_joint"};
  traj.times = {0.0, 0.1};
  traj.positions = {Eigen::VectorXd::Zero(1), Eigen::VectorXd::Zero(1)};
  auto v0 = (Eigen::VectorXd(1) << 0.1).finished();
  auto v1 = (Eigen::VectorXd(1) << 0.1).finished();
  traj.velocities = {v0, v1};

  std::map<std::string, double> vel_caps{{"elbow_joint", 0.5}};  // not in traj
  std::map<std::string, double> acc_caps;

  auto m = measure_joint_cap_peaks(traj, vel_caps, acc_caps);
  EXPECT_FALSE(m.samples_complete);
  EXPECT_EQ(m.incomplete_dimension, "velocity");
  EXPECT_EQ(m.incomplete_joint, "elbow_joint");
}

TEST(JointCapMeasurementTest, AccelCappedJointAbsentFromTrajectoryFailsClosed) {
  roboplan::JointTrajectory traj;
  traj.joint_names = {"shoulder_pan_joint"};
  traj.times = {0.0, 0.1};
  traj.positions = {Eigen::VectorXd::Zero(1), Eigen::VectorXd::Zero(1)};
  auto a0 = (Eigen::VectorXd(1) << 0.1).finished();
  auto a1 = (Eigen::VectorXd(1) << 0.1).finished();
  traj.accelerations = {a0, a1};

  std::map<std::string, double> vel_caps;
  std::map<std::string, double> acc_caps{{"wrist_2_joint", 1.0}};  // not in traj

  auto m = measure_joint_cap_peaks(traj, vel_caps, acc_caps);
  EXPECT_FALSE(m.samples_complete);
  EXPECT_EQ(m.incomplete_dimension, "acceleration");
  EXPECT_EQ(m.incomplete_joint, "wrist_2_joint");
}
