#include <gtest/gtest.h>

#include <manyforge_roboplan/native_kernel_factory.hpp>

using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

class NativeKernelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bundle_ = create_native_kernel("ur5_gripper", "arm");
    ASSERT_NE(bundle_, nullptr);
    ASSERT_NE(bundle_->kernel, nullptr);
  }

  std::shared_ptr<NativeKernelBundle> bundle_;
};

// --- Factory ---

TEST_F(NativeKernelTest, FactoryCreatesAllComponents) {
  EXPECT_NE(bundle_->scene, nullptr);
  EXPECT_NE(bundle_->rrt, nullptr);
  EXPECT_NE(bundle_->shortcutter, nullptr);
  EXPECT_NE(bundle_->toppra, nullptr);
  EXPECT_NE(bundle_->ik, nullptr);
  EXPECT_NE(bundle_->planner, nullptr);
  EXPECT_NE(bundle_->context, nullptr);
  EXPECT_NE(bundle_->execution, nullptr);
  EXPECT_NE(bundle_->clock, nullptr);
  EXPECT_NE(bundle_->scheduler, nullptr);
  EXPECT_NE(bundle_->cache, nullptr);
  EXPECT_NE(bundle_->kernel, nullptr);
}

TEST_F(NativeKernelTest, InitialStateIsIdle) {
  auto status = bundle_->kernel->get_status();
  EXPECT_EQ(status.state, ExecutionState::kIdle);
}

// --- Planning Context ---

TEST_F(NativeKernelTest, GetCurrentStateReturnsJoints) {
  auto state = bundle_->context->get_current_state();
  EXPECT_FALSE(state.joint_names.empty());
  EXPECT_EQ(state.joint_names.size(), state.positions_rad.size());
}

TEST_F(NativeKernelTest, ForwardKinematicsReturnsPose) {
  auto state = bundle_->context->get_current_state();
  FkRequestDTO req;
  req.robot_state = state;
  req.target_frame = "tool0";
  req.base_frame = "base_link";

  auto pose = bundle_->context->forward_kinematics(req);
  // Should return a valid position (not all zeros for a real URDF)
  double dist = std::sqrt(
      pose.position_m.x_m * pose.position_m.x_m +
      pose.position_m.y_m * pose.position_m.y_m +
      pose.position_m.z_m * pose.position_m.z_m);
  EXPECT_GT(dist, 0.01);  // TCP should be > 1cm from base
}

TEST_F(NativeKernelTest, CollisionCheckAtHomeNotInCollision) {
  auto state = bundle_->context->get_current_state();
  CollisionCheckRequestDTO req;
  req.robot_state = state;
  auto result = bundle_->context->collision_check(req);
  EXPECT_FALSE(result.in_collision);
}

TEST_F(NativeKernelTest, GetJointLimitsReturnsValidData) {
  auto limits = bundle_->context->get_joint_limits("robot1", "arm");
  EXPECT_FALSE(limits.joint_names.empty());
  EXPECT_EQ(limits.joint_names.size(), limits.position_lower_rad.size());
  EXPECT_EQ(limits.joint_names.size(), limits.position_upper_rad.size());
  EXPECT_EQ(limits.joint_names.size(), limits.velocity_max_rad_s.size());
}

// --- Scene Edits ---

TEST_F(NativeKernelTest, AddAndRemoveBoxObject) {
  AddObjectRequestDTO add_req;
  add_req.object_id = "test_box";
  add_req.frame_id = "universe";
  add_req.shape = BoxShapeDTO{{0.1, 0.1, 0.1}};
  add_req.pose.position_m = {0.5, 0.0, 0.5};
  add_req.pose.orientation = {0, 0, 0, 1};

  auto add_result = bundle_->context->add_object(add_req);
  EXPECT_TRUE(add_result.success);

  RemoveObjectRequestDTO rem_req;
  rem_req.object_id = "test_box";
  auto rem_result = bundle_->context->remove_object(rem_req);
  EXPECT_TRUE(rem_result.success);
}

TEST_F(NativeKernelTest, AddSphereObject) {
  AddObjectRequestDTO req;
  req.object_id = "test_sphere";
  req.frame_id = "universe";
  req.shape = SphereShapeDTO{0.05};
  req.pose.position_m = {0.3, 0.1, 0.6};
  req.pose.orientation = {0, 0, 0, 1};

  auto result = bundle_->context->add_object(req);
  EXPECT_TRUE(result.success);
}

// --- Full Execute Cycle ---

TEST_F(NativeKernelTest, ExecuteJointGoalSucceeds) {
  ExecuteRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "native_test_1";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.timeouts = {10.0, 30.0};
  req.cache_mode = CacheMode::kDefault;

  // Small joint goal from home
  JointGoalDTO goal;
  goal.joint_names = {"shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
                      "wrist_1_joint", "wrist_2_joint", "wrist_3_joint"};
  goal.positions_rad = {0.1, -0.5, 0.5, -0.5, -0.5, 0.0};
  req.motion = goal;

  auto result = bundle_->kernel->execute(req);
  EXPECT_TRUE(result.ok()) << "Execute failed: "
                           << (result.ok() ? "" : result.error().message);

  auto status = bundle_->kernel->get_status();
  EXPECT_EQ(status.state, ExecutionState::kCompleted);
}

TEST_F(NativeKernelTest, ExecuteAndResetCycle) {
  ExecuteRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "native_cycle_1";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.timeouts = {10.0, 30.0};
  req.cache_mode = CacheMode::kDisable;

  JointGoalDTO goal;
  goal.joint_names = {"shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
                      "wrist_1_joint", "wrist_2_joint", "wrist_3_joint"};
  goal.positions_rad = {0.1, -0.5, 0.5, -0.5, -0.5, 0.0};
  req.motion = goal;

  // Execute
  auto result = bundle_->kernel->execute(req);
  EXPECT_TRUE(result.ok());

  // Reset
  auto reset_result = bundle_->kernel->reset();
  EXPECT_TRUE(reset_result.accepted);
  EXPECT_EQ(bundle_->kernel->get_status().state, ExecutionState::kIdle);

  // Execute again
  req.move_id = "native_cycle_2";
  auto result2 = bundle_->kernel->execute(req);
  EXPECT_TRUE(result2.ok());
}

TEST_F(NativeKernelTest, CacheHitSkipsPlanning) {
  ExecuteRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "native_cache_test";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.timeouts = {10.0, 30.0};
  req.cache_mode = CacheMode::kDefault;

  JointGoalDTO goal;
  goal.joint_names = {"shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
                      "wrist_1_joint", "wrist_2_joint", "wrist_3_joint"};
  goal.positions_rad = {0.1, -0.5, 0.5, -0.5, -0.5, 0.0};
  req.motion = goal;

  // First execute — plans and caches
  auto r1 = bundle_->kernel->execute(req);
  EXPECT_TRUE(r1.ok());
  EXPECT_EQ(bundle_->cache->size(), 1u);

  bundle_->kernel->reset();

  // Second execute — should hit cache
  auto r2 = bundle_->kernel->execute(req);
  EXPECT_TRUE(r2.ok());
  EXPECT_EQ(bundle_->cache->size(), 1u);  // Still 1 entry
}

TEST_F(NativeKernelTest, TrajectoryValidationWorks) {
  // Create a simple trajectory and validate
  auto state = bundle_->context->get_current_state();

  TrajectoryDTO traj;
  traj.joint_names = state.joint_names;
  TrajectoryWaypointDTO wp;
  wp.time_from_start_s = 0.0;
  wp.positions_rad = state.positions_rad;
  traj.waypoints.push_back(wp);

  ValidationOptionsDTO opts;
  auto result = bundle_->context->validate_trajectory(traj, opts);
  EXPECT_TRUE(result.valid);
}
