#include <gtest/gtest.h>

#include <manyforge_roboplan/native_kernel_factory.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

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

TEST_F(NativeKernelTest, NativeExecutionBackendRejectsInvalidLifecycleTimeouts) {
  RoboPlanExecutionBackendConfig config;
  config.reset_completion_timeout_s = 0.0;
  EXPECT_THROW(
      RoboPlanExecutionBackend(
          bundle_->scene, "arm", *bundle_->clock, nullptr, config),
      std::invalid_argument);

  config.reset_completion_timeout_s = 5.0;
  config.streaming_stop_completion_timeout_s = 0.0;
  EXPECT_THROW(
      RoboPlanExecutionBackend(
          bundle_->scene, "arm", *bundle_->clock, nullptr, config),
      std::invalid_argument);
}

TEST(NativeSchedulerTest, SimpleSchedulerHonorsFutureDeadline) {
  SimpleScheduler scheduler;
  std::atomic<bool> fired{false};

  const auto now = std::chrono::steady_clock::now();
  const auto deadline =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          now.time_since_epoch())
          .count() +
      50'000'000LL;

  scheduler.schedule(deadline, [&fired]() { fired.store(true); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(fired.load());

  const auto timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (!fired.load() && std::chrono::steady_clock::now() < timeout) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(fired.load());
}

TEST(NativeSchedulerTest, MonitoringLaneFiresWhileSafetyLaneIsBlocked) {
  // Audit K3 regression: with the previous single-worker scheduler a
  // long-blocking Safety callback (e.g. collision_check_tick stuck
  // waiting on the scene mutex) would prevent the Monitoring watchdog
  // from firing — including the watchdog whose job is to abort that
  // stall. With the split scheduler the lanes are independent.
  SimpleScheduler scheduler;
  std::atomic<bool> safety_can_exit{false};
  std::atomic<bool> safety_running{false};
  std::atomic<bool> monitoring_fired{false};

  const auto schedule_at = [&](int64_t offset_ns) {
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return now_ns + offset_ns;
  };

  // Safety callback parks itself until the test releases it; mirrors
  // a collision_check_tick stalled on `scene_mutex_` during a long RRT.
  scheduler.schedule(
      schedule_at(10'000'000LL),
      [&]() {
        safety_running.store(true);
        while (!safety_can_exit.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      },
      SchedulerPriority::Safety);

  // Monitoring callback must fire even while Safety is parked.
  scheduler.schedule(
      schedule_at(20'000'000LL),
      [&]() { monitoring_fired.store(true); },
      SchedulerPriority::Monitoring);

  // Wait up to 500 ms for the watchdog to fire under contention.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!monitoring_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(safety_running.load())
      << "Safety callback did not reach its parking loop";
  EXPECT_TRUE(monitoring_fired.load())
      << "Monitoring watchdog did not fire while Safety lane was blocked";

  // Release the parked Safety callback so the scheduler destructor
  // does not race with a still-running task.
  safety_can_exit.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

TEST_F(NativeKernelTest, NormalizedCollisionCheckAtHomePreservesWorldRevision) {
  CollisionQuery query;
  query.query_id = "normalized-home";
  query.robot_state = bundle_->context->get_current_state();
  query.collision_world_revision = CollisionWorldRevision{42, "world-rev-42"};

  auto result = bundle_->context->collision_check_normalized(query);
  EXPECT_FALSE(result.in_collision);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kOk);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kPass);
  EXPECT_EQ(result.engine_id, "roboplan_scene");
  EXPECT_EQ(result.engine_version, "unknown");
  ASSERT_TRUE(result.collision_world_revision.has_value());
  EXPECT_EQ(result.collision_world_revision->revision_id, 42u);
  EXPECT_EQ(result.collision_world_revision->content_hash, "world-rev-42");
}

TEST_F(NativeKernelTest, NormalizedCollisionCheckMapsRobotObjectPair) {
  bundle_->context->set_collision_mapping_robot_id("ur5_gripper");

  AddObjectRequestDTO robot_blocker;
  robot_blocker.object_id = "robot_blocker";
  robot_blocker.frame_id = "universe";
  robot_blocker.shape = BoxShapeDTO{{0.3, 0.3, 0.3}};
  robot_blocker.pose.position_m = {0.0, 0.0, 0.05};
  robot_blocker.pose.orientation = {0, 0, 0, 1};
  auto blocker_result = bundle_->context->add_object(robot_blocker);
  ASSERT_TRUE(blocker_result.success)
      << (blocker_result.error.has_value() ? blocker_result.error->message : "");

  CollisionQuery query;
  query.query_id = "normalized-robot-blocker";
  query.robot_state = bundle_->context->get_current_state();
  auto result = bundle_->context->collision_check_normalized(query);

  ASSERT_TRUE(result.in_collision);
  ASSERT_TRUE(result.contact_body_a.has_value());
  ASSERT_TRUE(result.contact_body_b.has_value());
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kOk)
      << "raw pair: " << *result.contact_body_a << " <-> "
      << *result.contact_body_b;
  EXPECT_EQ(result.decision, CollisionDecisionResult::kFail);
  ASSERT_FALSE(result.normalized_pairs.empty());

  const auto has_robot_object_pair =
      std::any_of(result.normalized_pairs.begin(), result.normalized_pairs.end(),
                  [](const CollisionPairId& pair) {
                    const bool a_object =
                        pair.body_a == "object/robot_blocker/shape/0";
                    const bool b_object =
                        pair.body_b == "object/robot_blocker/shape/0";
                    const bool a_robot =
                        pair.body_a.rfind("robot/ur5_gripper/link/", 0) == 0;
                    const bool b_robot =
                        pair.body_b.rfind("robot/ur5_gripper/link/", 0) == 0;
                    return (a_object && b_robot) || (b_object && a_robot);
                  });
  EXPECT_TRUE(has_robot_object_pair);
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
  auto state = bundle_->context->get_object_state("test_box");
  EXPECT_TRUE(state.exists);
  ASSERT_TRUE(state.frame_id.has_value());
  EXPECT_EQ(*state.frame_id, "universe");

  auto pose = bundle_->context->get_object_pose("test_box", "universe");
  EXPECT_NEAR(pose.position_m.x_m, 0.5, 1e-9);
  EXPECT_NEAR(pose.position_m.y_m, 0.0, 1e-9);
  EXPECT_NEAR(pose.position_m.z_m, 0.5, 1e-9);

  RemoveObjectRequestDTO rem_req;
  rem_req.object_id = "test_box";
  auto rem_result = bundle_->context->remove_object(rem_req);
  EXPECT_TRUE(rem_result.success);
  auto removed_state = bundle_->context->get_object_state("test_box");
  EXPECT_FALSE(removed_state.exists);
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

TEST_F(NativeKernelTest, DetachedStaticObjectOverlapDoesNotBlockRobotState) {
  auto add_overlapping_box = [this](const std::string& object_id) {
    AddObjectRequestDTO req;
    req.object_id = object_id;
    req.frame_id = "universe";
    req.shape = BoxShapeDTO{{0.1, 0.1, 0.1}};
    req.pose.position_m = {0.7, 0.45, 0.35};
    req.pose.orientation = {0, 0, 0, 1};

    auto result = bundle_->context->add_object(req);
    EXPECT_TRUE(result.success)
        << (result.error.has_value() ? result.error->message : "");
  };

  add_overlapping_box("static_overlap_a");
  add_overlapping_box("static_overlap_b");

  CollisionCheckRequestDTO check;
  check.robot_state = bundle_->context->get_current_state();
  EXPECT_FALSE(bundle_->context->collision_check(check).in_collision);

  AddObjectRequestDTO robot_blocker;
  robot_blocker.object_id = "robot_blocker";
  robot_blocker.frame_id = "universe";
  robot_blocker.shape = BoxShapeDTO{{0.3, 0.3, 0.3}};
  robot_blocker.pose.position_m = {0.0, 0.0, 0.05};
  robot_blocker.pose.orientation = {0, 0, 0, 1};
  auto blocker_result = bundle_->context->add_object(robot_blocker);
  EXPECT_TRUE(blocker_result.success)
      << (blocker_result.error.has_value() ? blocker_result.error->message : "");
  EXPECT_TRUE(bundle_->context->collision_check(check).in_collision);
}

TEST_F(NativeKernelTest, AddMeshObject) {
  const auto mesh_path =
      std::filesystem::temp_directory_path() / "manyforge_test_mesh.obj";
  {
    std::ofstream out(mesh_path);
    out << "v 0 0 0\n"
        << "v 0.05 0 0\n"
        << "v 0 0.05 0\n"
        << "f 1 2 3\n";
  }

  AddObjectRequestDTO req;
  req.object_id = "test_mesh";
  req.frame_id = "universe";
  req.shape = MeshShapeDTO{mesh_path.string(), {1.0, 1.0, 1.0}};
  req.pose.position_m = {0.4, 0.1, 0.5};
  req.pose.orientation = {0, 0, 0, 1};

  auto result = bundle_->context->add_object(req);
  EXPECT_TRUE(result.success) << (result.error.has_value() ? result.error->message : "");

  auto state = bundle_->context->get_object_state("test_mesh");
  EXPECT_TRUE(state.exists);
  auto pose = bundle_->context->get_object_pose("test_mesh", "universe");
  EXPECT_NEAR(pose.position_m.x_m, 0.4, 1e-9);
  EXPECT_NEAR(pose.position_m.y_m, 0.1, 1e-9);
  EXPECT_NEAR(pose.position_m.z_m, 0.5, 1e-9);
}

TEST_F(NativeKernelTest, AttachedFrameObjectPoseResolvesFromBackendState) {
  auto link_state = bundle_->context->get_current_state();
  FkRequestDTO fk_req;
  fk_req.robot_state = link_state;
  fk_req.target_frame = "tool0";
  fk_req.base_frame = "universe";
  auto tool_pose = bundle_->context->forward_kinematics(fk_req);

  AddObjectRequestDTO add_req;
  add_req.object_id = "carried_box";
  add_req.frame_id = "tool0";
  add_req.shape = BoxShapeDTO{{0.05, 0.05, 0.05}};
  add_req.pose.position_m = {0.0, 0.0, 0.0};
  add_req.pose.orientation = {0.0, 0.0, 0.0, 1.0};

  auto add_result = bundle_->context->add_object(add_req);
  ASSERT_TRUE(add_result.success);

  auto state = bundle_->context->get_object_state("carried_box");
  EXPECT_TRUE(state.exists);
  EXPECT_TRUE(state.attached);
  ASSERT_TRUE(state.frame_id.has_value());
  EXPECT_EQ(*state.frame_id, "tool0");

  auto pose = bundle_->context->get_object_pose("carried_box", "universe");
  EXPECT_NEAR(pose.position_m.x_m, tool_pose.position_m.x_m, 1e-6);
  EXPECT_NEAR(pose.position_m.y_m, tool_pose.position_m.y_m, 1e-6);
  EXPECT_NEAR(pose.position_m.z_m, tool_pose.position_m.z_m, 1e-6);
}

TEST_F(NativeKernelTest, AttachObjectReparentsWhilePreservingWorldPose) {
  AddObjectRequestDTO add_req;
  add_req.object_id = "carried_box";
  add_req.frame_id = "universe";
  add_req.shape = BoxShapeDTO{{0.05, 0.05, 0.05}};
  add_req.pose.position_m = {0.45, 0.0, 0.55};
  add_req.pose.orientation = {0.0, 0.0, 0.0, 1.0};
  ASSERT_TRUE(bundle_->context->add_object(add_req).success);

  auto pose_before = bundle_->context->get_object_pose("carried_box", "universe");

  AttachObjectRequestDTO attach_req;
  attach_req.object_id = "carried_box";
  attach_req.frame_id = "tool0";
  auto attach_result = bundle_->context->attach_object(attach_req);
  ASSERT_TRUE(attach_result.success);

  auto state = bundle_->context->get_object_state("carried_box");
  EXPECT_TRUE(state.exists);
  EXPECT_TRUE(state.attached);
  ASSERT_TRUE(state.frame_id.has_value());
  EXPECT_EQ(*state.frame_id, "tool0");

  auto pose_after = bundle_->context->get_object_pose("carried_box", "universe");
  EXPECT_NEAR(pose_after.position_m.x_m, pose_before.position_m.x_m, 1e-6);
  EXPECT_NEAR(pose_after.position_m.y_m, pose_before.position_m.y_m, 1e-6);
  EXPECT_NEAR(pose_after.position_m.z_m, pose_before.position_m.z_m, 1e-6);
}

TEST_F(NativeKernelTest, DetachObjectReparentsBackToUniverseWhilePreservingWorldPose) {
  AddObjectRequestDTO add_req;
  add_req.object_id = "carried_box";
  add_req.frame_id = "tool0";
  add_req.shape = BoxShapeDTO{{0.05, 0.05, 0.05}};
  add_req.pose.position_m = {0.01, 0.0, 0.0};
  add_req.pose.orientation = {0.0, 0.0, 0.0, 1.0};
  ASSERT_TRUE(bundle_->context->add_object(add_req).success);

  auto pose_before = bundle_->context->get_object_pose("carried_box", "universe");

  DetachObjectRequestDTO detach_req;
  detach_req.object_id = "carried_box";
  detach_req.frame_id = "universe";
  auto detach_result = bundle_->context->detach_object(detach_req);
  ASSERT_TRUE(detach_result.success);

  auto state = bundle_->context->get_object_state("carried_box");
  EXPECT_TRUE(state.exists);
  EXPECT_FALSE(state.attached);
  ASSERT_TRUE(state.frame_id.has_value());
  EXPECT_EQ(*state.frame_id, "universe");

  auto pose_after = bundle_->context->get_object_pose("carried_box", "universe");
  EXPECT_NEAR(pose_after.position_m.x_m, pose_before.position_m.x_m, 1e-6);
  EXPECT_NEAR(pose_after.position_m.y_m, pose_before.position_m.y_m, 1e-6);
  EXPECT_NEAR(pose_after.position_m.z_m, pose_before.position_m.z_m, 1e-6);
}

// Regression for RoboPlan's historical index-cache-staleness bug: a sequence
// of remove+add upserts used to corrupt RoboPlan Scene's name→index map. The
// next mutation (here, an attach) then targeted the wrong geometry and parked
// an unrelated object at the wrong pose, producing phantom collisions between
// the robot base/shoulder/ground and the misplaced object. RoboPlan main now
// owns that bookkeeping, so ManyForge uses direct remove/reparent operations.
TEST_F(NativeKernelTest, UpsertCascadeFollowedByAttachDoesNotCorruptScene) {
  // Build a 3-object scene that mirrors the production failure: a ground
  // plane, a graspable, and a wall-shaped third object placed where the
  // wrapper-rendered scene shows it sitting clear of the robot.
  auto add_box = [&](const std::string& id, std::array<double, 3> dims,
                     std::array<double, 3> pos) {
    AddObjectRequestDTO r;
    r.object_id = id;
    r.frame_id = "universe";
    r.shape = BoxShapeDTO{{dims[0], dims[1], dims[2]}};
    r.pose.position_m = {pos[0], pos[1], pos[2]};
    r.pose.orientation = {0.0, 0.0, 0.0, 1.0};
    return bundle_->context->add_object(r);
  };
  auto remove_box = [&](const std::string& id) {
    RemoveObjectRequestDTO r;
    r.object_id = id;
    return bundle_->context->remove_object(r);
  };

  ASSERT_TRUE(add_box("ground", {1.0, 1.0, 0.2}, {0.0, 0.0, -0.11}).success);
  ASSERT_TRUE(add_box("graspable", {0.1, 0.01, 0.01}, {0.3, -0.3, 0.15}).success);
  ASSERT_TRUE(add_box("box_1", {1.0, 0.02, 0.2}, {0.0, -0.2, 0.1}).success);

  // Mimic the upsert pattern emitted by the composer at session start.
  ASSERT_TRUE(remove_box("ground").success);
  ASSERT_TRUE(add_box("ground", {1.0, 1.0, 0.2}, {0.0, 0.0, -0.11}).success);
  ASSERT_TRUE(remove_box("graspable").success);
  ASSERT_TRUE(add_box("graspable", {0.1, 0.01, 0.01}, {0.3, -0.3, 0.15}).success);
  ASSERT_TRUE(remove_box("box_1").success);
  ASSERT_TRUE(add_box("box_1", {1.0, 0.02, 0.2}, {0.0, -0.2, 0.1}).success);

  // Attach graspable to tool0. With the bug, this used to write into box_1's
  // slot in the geometry vector, parking box_1 near the robot base and
  // producing phantom collisions on the next planning attempt.
  AttachObjectRequestDTO ar;
  ar.object_id = "graspable";
  ar.frame_id = "tool0";
  ASSERT_TRUE(bundle_->context->attach_object(ar).success);

  // Box_1 must still report at universe (0, -0.2, 0.1) — not on the robot.
  auto box_pose = bundle_->context->get_object_pose("box_1", "universe");
  EXPECT_NEAR(box_pose.position_m.x_m,  0.0, 1e-6);
  EXPECT_NEAR(box_pose.position_m.y_m, -0.2, 1e-6);
  EXPECT_NEAR(box_pose.position_m.z_m,  0.1, 1e-6);

  // And the start-state collision check must come back clean for the
  // post-attach robot configuration.
  auto state = bundle_->context->get_current_state();
  CollisionCheckRequestDTO cc;
  cc.robot_state = state;
  auto cc_result = bundle_->context->collision_check(cc);
  EXPECT_FALSE(cc_result.in_collision)
      << "Phantom collision after upsert+attach indicates the index-cache "
         "staleness regression has returned.";
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

  bundle_->kernel->wait_for_completion();
  auto status = bundle_->kernel->get_status();
  EXPECT_EQ(status.state, ExecutionState::kCompleted);
}

TEST_F(NativeKernelTest, PlannerUsesPerRequestPlanningTimeout) {
  roboplan::RRTOptions rrt_opts;
  rrt_opts.group_name = "arm";
  rrt_opts.max_planning_time = 5.0;
  rrt_opts.max_nodes = 5000;
  rrt_opts.max_connection_distance = 0.1;  // force the planner into the RRT loop

  RoboPlanPlannerBackend planner(bundle_->scene, rrt_opts, bundle_->shortcutter,
                                 bundle_->toppra, bundle_->ik);

  PlanRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "native_planning_timeout";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.start_state = bundle_->context->get_current_state();
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.planning_timeout_s = 1e-6;

  JointGoalDTO goal;
  goal.joint_names = req.start_state.joint_names;
  goal.positions_rad = {0.3, -0.8, 0.8, -0.3, -0.8, 0.1};
  req.motion = goal;

  auto result = planner.plan(req);
  ASSERT_FALSE(result.trajectory.has_value());
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorPlanningFailed);
  EXPECT_NE(result.error->message.find("timed out"), std::string::npos);
}

TEST_F(NativeKernelTest, PlannerRejectsUnsatisfiedTcpSpeedLimit) {
  PlanRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "native_tcp_limit_rejected_in_planning";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.start_state = bundle_->context->get_current_state();
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, 1e-9};
  req.planning_timeout_s = 10.0;

  JointGoalDTO goal;
  goal.joint_names = req.start_state.joint_names;
  goal.positions_rad = {0.1, -0.5, 0.5, -0.5, -0.5, 0.0};
  req.motion = goal;

  auto result = bundle_->planner->plan(req);
  ASSERT_FALSE(result.trajectory.has_value());
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorValidationLimitsViolation);
  EXPECT_EQ(result.error->details.at("failure_stage"), "time_parameterization");
  EXPECT_EQ(result.error->details.at("limit_type"), "tcp_linear_speed");
  EXPECT_EQ(result.error->details.at("toppra_iterations"), "32");
}

TEST_F(NativeKernelTest, ExecutionBackendPublishesProgressAndInterpolatesSceneState) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.25;
  if (goal.size() > 1) {
    goal[1] -= 0.2;
  }

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{0.3, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  auto initial = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(initial.state, ExecutionState::kExecuting);
  // The dispatch worker may publish the first interpolation tick before this
  // immediate observer read returns. Assert the stable contract instead of
  // requiring an exact zero-progress race against the worker thread.
  EXPECT_GE(initial.progress, 0.0);
  EXPECT_LT(initial.progress, 0.05);

  std::this_thread::sleep_for(std::chrono::milliseconds(120));

  auto mid = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(mid.state, ExecutionState::kExecuting);
  EXPECT_GT(mid.sequence_number, initial.sequence_number);
  EXPECT_GT(mid.progress, initial.progress);
  EXPECT_LT(mid.progress, 1.0);
  ASSERT_EQ(mid.joint_positions_rad.size(), goal.size());
  EXPECT_NEAR(mid.joint_positions_rad[0], start_state.positions_rad[0], 0.2);
  EXPECT_NE(mid.joint_positions_rad[0], goal[0]);

  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));

  auto final = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(final.state, ExecutionState::kCompleted);
  EXPECT_DOUBLE_EQ(final.progress, 1.0);
  ASSERT_EQ(final.joint_positions_rad.size(), goal.size());

  auto final_state = bundle_->context->get_current_state();
  ASSERT_EQ(final_state.positions_rad.size(), goal.size());
  EXPECT_NEAR(final_state.positions_rad[0], goal[0], 1e-3);
  if (goal.size() > 1) {
    EXPECT_NEAR(final_state.positions_rad[1], goal[1], 1e-3);
  }
}

TEST_F(NativeKernelTest, ExecutionBackendResetPublishesAuthoritativeIdleSnapshot) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.25;
  if (goal.size() > 1) {
    goal[1] -= 0.2;
  }

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{0.3, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  (void)handle;
  auto reset_result = bundle_->execution->reset();
  EXPECT_TRUE(reset_result.accepted);

  auto reset_snapshot = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(reset_snapshot.state, ExecutionState::kIdle);
  EXPECT_DOUBLE_EQ(reset_snapshot.progress, 0.0);
  EXPECT_TRUE(reset_snapshot.joint_positions_rad.empty());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto later_snapshot = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(later_snapshot.state, ExecutionState::kIdle);
  EXPECT_DOUBLE_EQ(later_snapshot.progress, 0.0);
  EXPECT_TRUE(later_snapshot.joint_positions_rad.empty());
}

TEST_F(NativeKernelTest, KernelStreamingSnapshotFollowsBackendLiveState) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.joint_names.empty());
  ASSERT_EQ(start_state.joint_names.size(), start_state.positions_rad.size());

  StreamingSessionConfigDTO config;
  config.group_id = "arm";
  config.joint_names = start_state.joint_names;
  config.default_input_mode = StreamingInputMode::kJoint;
  config.max_dispatch_rate_hz = 30.0;

  auto begin = bundle_->kernel->begin_streaming_session(config);
  ASSERT_TRUE(begin.accepted) << (begin.error ? begin.error->message : "");

  const double target = start_state.positions_rad[0] + 0.25;
  StreamingTargetDTO command;
  command.kind = StreamingInputMode::kJoint;
  command.joint_names = {start_state.joint_names[0]};
  command.joint_positions_rad = {target};
  command.source_sequence = 1;

  auto push = bundle_->kernel->push_streaming_target(begin.session_id, command);
  ASSERT_TRUE(push.accepted) << (push.error ? push.error->message : "");

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  ExecutionSnapshot live_snapshot;
  bool saw_live_progress = false;
  bool reached_target = false;
  do {
    live_snapshot = bundle_->kernel->get_execution_snapshot();
    if (live_snapshot.joint_positions_rad.size() ==
        start_state.positions_rad.size()) {
      if (std::abs(live_snapshot.joint_positions_rad[0] -
                   start_state.positions_rad[0]) > 0.01) {
        saw_live_progress = true;
      }
      if (std::abs(live_snapshot.joint_positions_rad[0] - target) < 1e-3) {
        reached_target = true;
      }
    }
    if (saw_live_progress && reached_target) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (std::chrono::steady_clock::now() < deadline);

  EXPECT_EQ(live_snapshot.state, ExecutionState::kStreaming);
  ASSERT_EQ(live_snapshot.joint_positions_rad.size(),
            start_state.positions_rad.size());
  EXPECT_TRUE(saw_live_progress)
      << "kernel observers must see backend streaming progress; otherwise "
         "Composer joint controls cannot close deterministically";
  ASSERT_TRUE(reached_target)
      << "test must keep the streaming session open until arrival; "
         "end_streaming_session is a soft-stop primitive, not a finish-to-target "
         "primitive";

  auto end =
      bundle_->kernel->end_streaming_session(begin.session_id,
                                             SoftStopStrategy::kSpringBack);
  EXPECT_TRUE(end.accepted) << (end.error ? end.error->message : "");
  EXPECT_EQ(bundle_->kernel->get_status().state, ExecutionState::kIdle);

  const auto final_snapshot = bundle_->kernel->get_execution_snapshot();
  EXPECT_EQ(final_snapshot.state, ExecutionState::kIdle);
  ASSERT_EQ(final_snapshot.joint_positions_rad.size(),
            start_state.positions_rad.size());
  EXPECT_NEAR(final_snapshot.joint_positions_rad[0], target, 1e-3)
      << "clean streaming end must retain the final backend joint state in "
         "the observer snapshot; otherwise Composer controls jump back to "
         "a stale pre-streaming value";
}

TEST_F(NativeKernelTest, StreamingRuntimeVelocityScaleAppliesPerTick) {
  // ADR-0018: changing the velocity scale on an in-flight streaming session
  // MUST take effect on worker ticks without tearing the session down.
  //
  // Strategy: open a session at scale=1.0, push a far-away joint
  // target, sample worker progress for a fixed window. Then call
  // set_runtime_velocity_scale(0.05), sample for the same window
  // size. The second window's commanded-joint delta must be much
  // smaller — the worker honored the runtime read. Tolerances are
  // sized for the simulated backend (tick = 10 ms, v_cap = 0.5 rad/s
  // default in the engine).
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.joint_names.empty());

  StreamingSessionConfigDTO config;
  config.group_id = "arm";
  config.joint_names = start_state.joint_names;
  config.default_input_mode = StreamingInputMode::kJoint;
  config.max_dispatch_rate_hz = 100.0;
  config.velocity_scale = 1.0;

  auto begin = bundle_->kernel->begin_streaming_session(config);
  ASSERT_TRUE(begin.accepted) << (begin.error ? begin.error->message : "");

  // Push a target far enough that the joint will travel for the
  // entire ~600 ms sampling window even at full scale.
  const double target = start_state.positions_rad[0] + 1.5;
  StreamingTargetDTO command;
  command.kind = StreamingInputMode::kJoint;
  command.joint_names = {start_state.joint_names[0]};
  command.joint_positions_rad = {target};
  command.source_sequence = 1;

  auto push = bundle_->kernel->push_streaming_target(begin.session_id, command);
  ASSERT_TRUE(push.accepted) << (push.error ? push.error->message : "");

  // Let the worker accelerate up to steady-state at scale=1.0
  // (engine default a_cap = 2.0 rad/s², tick = 10 ms, so ramp from 0
  // to 0.5 rad/s takes ~250 ms).
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Window 1: scale=1.0 (steady-state). Sample for 200 ms.
  constexpr auto kSampleWindow = std::chrono::milliseconds(200);
  const auto p_before_full = bundle_->kernel->get_execution_snapshot()
                                 .joint_positions_rad;
  ASSERT_EQ(p_before_full.size(), start_state.positions_rad.size());
  std::this_thread::sleep_for(kSampleWindow);
  const auto p_after_full = bundle_->kernel->get_execution_snapshot()
                                .joint_positions_rad;
  ASSERT_EQ(p_after_full.size(), start_state.positions_rad.size());
  const double delta_full = p_after_full[0] - p_before_full[0];

  // Drop the runtime scale. Phase 1 expects this to take effect on
  // the next tick — no preempt, no replan, just a per-tick atomic
  // read by the worker.
  bundle_->kernel->set_runtime_velocity_scale(0.05);

  // The accel limiter (engine default 2 rad/s², 10 ms tick → 0.02
  // rad/s velocity change per tick) takes ~250 ms to decelerate
  // from 0.5 rad/s steady-state down to 0.025 rad/s (the new cap at
  // scale=0.05 × 0.5 rad/s v_cap). Wait it out before sampling so
  // the test characterizes the *new steady-state* velocity, not the
  // mid-ramp transient.
  std::this_thread::sleep_for(std::chrono::milliseconds(350));

  // Window 2: scale=0.05 (steady-state). Sample for the same 200 ms.
  const auto p_before_slow = bundle_->kernel->get_execution_snapshot()
                                 .joint_positions_rad;
  std::this_thread::sleep_for(kSampleWindow);
  const auto p_after_slow = bundle_->kernel->get_execution_snapshot()
                                .joint_positions_rad;
  const double delta_slow = p_after_slow[0] - p_before_slow[0];

  // At scale=0.05 with v_cap=0.5, steady-state velocity is
  // 0.025 rad/s → 5 mrad over 200 ms. At scale=1.0, ~100 mrad over
  // 200 ms. Asserting slow < full / 4 leaves headroom for any
  // residual ramp time without papering over the runtime read.
  EXPECT_GT(delta_full, 0.05)
      << "session must make material progress at scale=1.0";
  EXPECT_LT(delta_slow, delta_full / 4.0)
      << "runtime scale change to 0.05 must throttle commanded "
         "velocity; delta_full=" << delta_full
      << " delta_slow=" << delta_slow;

  auto end = bundle_->kernel->end_streaming_session(
      begin.session_id, SoftStopStrategy::kSpringBack);
  EXPECT_TRUE(end.accepted) << (end.error ? end.error->message : "");
}

TEST_F(NativeKernelTest, StreamingRuntimeVelocityScaleRejectsNonFinite) {
  // ADR-0018 / K2-aligned: non-finite inputs must NOT
  // propagate into the worker's atomic read. The kernel itself
  // filters before forwarding to the backend; the backend's setter
  // re-checks. Test confirms the in-flight session is unperturbed.
  auto start_state = bundle_->context->get_current_state();
  StreamingSessionConfigDTO config;
  config.group_id = "arm";
  config.joint_names = start_state.joint_names;
  config.default_input_mode = StreamingInputMode::kJoint;
  config.max_dispatch_rate_hz = 100.0;
  config.velocity_scale = 0.5;

  auto begin = bundle_->kernel->begin_streaming_session(config);
  ASSERT_TRUE(begin.accepted) << (begin.error ? begin.error->message : "");

  // NaN, +Inf, -Inf: each must be ignored. The kernel returns void,
  // so we verify by sampling the session is still alive afterward.
  bundle_->kernel->set_runtime_velocity_scale(
      std::numeric_limits<double>::quiet_NaN());
  bundle_->kernel->set_runtime_velocity_scale(
      std::numeric_limits<double>::infinity());
  bundle_->kernel->set_runtime_velocity_scale(
      -std::numeric_limits<double>::infinity());

  // Session is still in kStreaming and accepting pushes — the
  // failing setter calls did not corrupt state.
  EXPECT_EQ(bundle_->kernel->get_status().state, ExecutionState::kStreaming);
  StreamingTargetDTO command;
  command.kind = StreamingInputMode::kJoint;
  command.joint_names = {start_state.joint_names[0]};
  command.joint_positions_rad = {start_state.positions_rad[0] + 0.1};
  command.source_sequence = 1;
  auto push = bundle_->kernel->push_streaming_target(begin.session_id, command);
  EXPECT_TRUE(push.accepted)
      << "non-finite scale write must not poison subsequent pushes";

  auto end = bundle_->kernel->end_streaming_session(
      begin.session_id, SoftStopStrategy::kSpringBack);
  EXPECT_TRUE(end.accepted) << (end.error ? end.error->message : "");
}

TEST_F(NativeKernelTest, RuntimeVelocityScaleZeroHoldsDiscreteTrajectoryTime) {
  // ADR-0018: runtime scale 0 is a hold/freeze of trajectory progress, not a
  // Stop. The native backend slews the applied scale smoothly, so this test
  // waits for the deceleration ramp before asserting the hold is stable.
  const auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.joint_names.empty());
  ASSERT_EQ(start_state.joint_names.size(), start_state.positions_rad.size());

  TrajectoryDTO trajectory;
  trajectory.joint_names = start_state.joint_names;
  TrajectoryWaypointDTO start_wp;
  start_wp.time_from_start_s = 0.0;
  start_wp.positions_rad = start_state.positions_rad;
  trajectory.waypoints.push_back(start_wp);

  auto target = start_state.positions_rad;
  target[0] += 0.8;
  TrajectoryWaypointDTO end_wp;
  end_wp.time_from_start_s = 1.0;
  end_wp.positions_rad = target;
  trajectory.waypoints.push_back(end_wp);

  bundle_->execution->set_runtime_velocity_scale(1.0);
  auto handle = bundle_->execution->execute_trajectory(trajectory);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const auto moving = handle.get_snapshot().joint_positions_rad;
  ASSERT_EQ(moving.size(), target.size());

  bundle_->execution->set_runtime_velocity_scale(0.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  const auto held_a = handle.get_snapshot().joint_positions_rad;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto held_b = handle.get_snapshot().joint_positions_rad;
  ASSERT_EQ(held_a.size(), target.size());
  ASSERT_EQ(held_b.size(), target.size());
  EXPECT_NEAR(held_b[0], held_a[0], 0.02)
      << "scale=0 must hold after the smooth deceleration ramp";

  bundle_->execution->set_runtime_velocity_scale(1.0);
  ASSERT_TRUE(handle.wait_for_completion(3'000'000'000LL));
  const auto final = handle.get_snapshot();
  EXPECT_EQ(final.state, ExecutionState::kCompleted);
  ASSERT_EQ(final.joint_positions_rad.size(), target.size());
  EXPECT_NEAR(final.joint_positions_rad[0], target[0], 1e-3);
}

TEST_F(NativeKernelTest, KernelExecuteWaitsForTimedBackendCompletion) {
  ExecuteRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "native_async_kernel";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.timeouts = {10.0, 30.0};
  req.cache_mode = CacheMode::kDisable;

  JointGoalDTO goal;
  goal.joint_names = {"shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
                      "wrist_1_joint", "wrist_2_joint", "wrist_3_joint"};
  goal.positions_rad = {0.3, -0.8, 0.7, -0.8, -0.4, 0.1};
  req.motion = goal;

  auto result = bundle_->kernel->execute(req);
  ASSERT_TRUE(result.ok());

  bool saw_executing = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    auto status = bundle_->kernel->get_status();
    if (status.state == ExecutionState::kExecuting) {
      saw_executing = true;
      EXPECT_LT(status.progress, 1.0);
      break;
    }
    if (status.state == ExecutionState::kCompleted ||
        status.state == ExecutionState::kFailed ||
        status.state == ExecutionState::kStopped) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  EXPECT_TRUE(saw_executing);
  ASSERT_TRUE(bundle_->kernel->wait_for_completion(15'000'000'000LL));
  EXPECT_EQ(bundle_->kernel->get_status().state, ExecutionState::kCompleted);

  auto snapshot = bundle_->kernel->get_execution_snapshot();
  EXPECT_EQ(snapshot.execution_id, result.value());
  EXPECT_EQ(snapshot.move_id, "native_async_kernel");
  EXPECT_EQ(snapshot.state, ExecutionState::kCompleted);
  EXPECT_DOUBLE_EQ(snapshot.progress, 1.0);
  EXPECT_FALSE(snapshot.joint_positions_rad.empty());
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
  bundle_->kernel->wait_for_completion();

  // Reset
  auto reset_result = bundle_->kernel->reset();
  EXPECT_TRUE(reset_result.accepted);
  EXPECT_EQ(bundle_->kernel->get_status().state, ExecutionState::kIdle);

  // Execute again
  req.move_id = "native_cycle_2";
  auto result2 = bundle_->kernel->execute(req);
  EXPECT_TRUE(result2.ok());
  bundle_->kernel->wait_for_completion();
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
  bundle_->kernel->wait_for_completion();
  EXPECT_EQ(bundle_->cache->size(), 1u);

  bundle_->kernel->reset();

  // Reset scene to original start so cached trajectory's start matches
  Eigen::VectorXd origin = Eigen::VectorXd::Zero(6);
  bundle_->scene->setJointPositions(origin);

  // Second execute — should hit cache
  auto r2 = bundle_->kernel->execute(req);
  EXPECT_TRUE(r2.ok());
  EXPECT_EQ(bundle_->kernel->get_status().state, ExecutionState::kValidating);
  bundle_->kernel->wait_for_completion();
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

TEST_F(NativeKernelTest, TrajectoryValidationCanSkipLegacyCollisionSweep) {
  auto state = bundle_->context->get_current_state();

  AddObjectRequestDTO add;
  add.object_id = "validation_overlap_box";
  BoxShapeDTO box;
  box.dimensions_m = {2.0, 2.0, 2.0};
  add.shape = box;
  add.frame_id = "base_link";
  add.pose.position_m = {0.0, 0.0, 0.0};
  add.pose.orientation = {0.0, 0.0, 0.0, 1.0};
  auto edit = bundle_->context->add_object(add);
  ASSERT_TRUE(edit.success) << (edit.error ? edit.error->message : "");

  TrajectoryDTO traj;
  traj.joint_names = state.joint_names;
  TrajectoryWaypointDTO wp;
  wp.time_from_start_s = 0.0;
  wp.positions_rad = state.positions_rad;
  traj.waypoints.push_back(wp);

  ValidationOptionsDTO opts;
  opts.run_collision_checks = false;
  auto result = bundle_->context->validate_trajectory(traj, opts);
  EXPECT_TRUE(result.valid);
}

TEST_F(NativeKernelTest,
       NormalizedCollisionCheckUnsupportedRequiredCapabilityFailsClosed) {
  auto state = bundle_->context->get_current_state();

  CollisionQuery query;
  query.robot_state = state;
  query.required_capabilities = {"penetration_depth"};

  auto result = bundle_->context->collision_check_normalized(query);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
}

TEST_F(NativeKernelTest, TrajectoryValidationReportsTcpSpeedBeforeCollisionSweep) {
  auto state = bundle_->context->get_current_state();
  ASSERT_GE(state.positions_rad.size(), 2u);

  TrajectoryDTO traj;
  traj.joint_names = state.joint_names;

  TrajectoryWaypointDTO start;
  start.time_from_start_s = 0.0;
  start.positions_rad = state.positions_rad;
  traj.waypoints.push_back(start);

  TrajectoryWaypointDTO fast;
  fast.time_from_start_s = 0.001;
  fast.positions_rad = state.positions_rad;
  fast.positions_rad[0] += 0.5;
  traj.waypoints.push_back(fast);

  ValidationOptionsDTO opts;
  opts.max_tcp_linear_speed_mps = 0.001;
  opts.tcp_frame = "tool0";
  opts.base_frame = "base_link";

  auto result = bundle_->context->validate_trajectory(traj, opts);
  EXPECT_FALSE(result.valid);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorValidationLimitsViolation);
  EXPECT_EQ(result.error->details.at("limit_type"), "tcp_linear_speed");
  EXPECT_EQ(result.error->details.at("collision_validation_duration_ns"), "0");
  EXPECT_EQ(result.error->details.at("max_segment_index"), "0");
  EXPECT_EQ(result.error->details.at("waypoint_count"), "2");
  EXPECT_NE(result.error->details.find("speed_validation_duration_ns"),
            result.error->details.end());
}

// =========================================================================
// Native backend `spring_back_hold` controlled-stop tests
// =========================================================================

TEST_F(NativeKernelTest,
       NativeBackend_StopCapability_DeclaresSpringBackHoldAndSoftOnPath) {
  // Spec/240 backend capability profile + spec/260 §5.6: the native
  // simulator now has concrete support for spring_back_hold and
  // soft_on_path. hard_deceleration remains ros2_control/JTC-only.
  auto cap = bundle_->execution->get_motion_interrupt_stop_capability();
  EXPECT_EQ(cap.supported_stop_modes.size(), 2u);
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSpringBackHold));
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSoftOnPath));
  EXPECT_FALSE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id, "roboplan_native_v2_soft_on_path");
  ASSERT_TRUE(cap.soft_on_path_planner_config.has_value());
  EXPECT_DOUBLE_EQ(cap.soft_on_path_planner_config->stop_traj_delta_s, 0.02);
  EXPECT_DOUBLE_EQ(cap.soft_on_path_planner_config->sample_period_s, 0.01);
  EXPECT_TRUE(cap.soft_on_path_planner_config->allow_model_velocity_fallback);
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_RejectsUnsupportedMode) {
  // Defense in depth: the kernel's arm-time gate already rejects
  // unsupported modes, but a backend that has been mis-wired must
  // also fail closed at trip time rather than silently producing a
  // hold for a different requested mode.
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "test_hard_deceleration";
  profile.stop_mode = MotionInterruptStopMode::kHardDeceleration;
  auto result = bundle_->execution->execute_controlled_stop(profile);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.backend_stop_confirmed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "stop_mode_not_supported_by_backend");
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_RejectsWithNoExecutionInFlight) {
  // No execution + no recorded observed state → fail closed.
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "test_no_inflight";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  auto result = bundle_->execution->execute_controlled_stop(profile);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.backend_stop_confirmed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "controlled_stop_no_observed_state");
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_SpringBackHoldPreemptsActiveExecution) {
  // Spec/260 §5.6 spring_back_hold semantics: preempt the active
  // trajectory, leave the worker at its observed joint position,
  // confirm the stop. The post-stop snapshot's joint vector MUST
  // anchor to a point along the trajectory (not at the endpoint,
  // since the worker was preempted mid-flight) and MUST be reported
  // with kStopped state and kNativeSimulatedStop command source.
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.5;
  if (goal.size() > 1) {
    goal[1] -= 0.4;
  }

  // Long trajectory so the worker is mid-flight when we preempt.
  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{2.0, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  // Wait a touch so the worker has time to advance past the anchor.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "test_spring_back";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.max_stop_time_s = 1.0;  // generous; well above the worker tick

  const auto call_start = bundle_->clock->now();
  auto result = bundle_->execution->execute_controlled_stop(profile);
  const auto call_end = bundle_->clock->now();
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  EXPECT_EQ(result.executed_stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  EXPECT_EQ(result.stop_command_source,
            MotionInterruptStopCommandSource::kNativeSimulatedStop);
  EXPECT_GE(result.confirmation_latency_ns, 0);
  EXPECT_LE(result.confirmation_latency_ns,
            std::int64_t{call_end - call_start});

  // The wait_for_completion handle is now satisfied (the worker
  // exited under stop_token).
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));

  // Post-stop snapshot reflects the anchored (preempted) position.
  auto stopped = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(stopped.state, ExecutionState::kStopped);
  ASSERT_EQ(stopped.joint_positions_rad.size(),
            start_state.positions_rad.size());
  // Anchor MUST be different from the planned endpoint (worker was
  // preempted) and SHOULD be advanced past the start position
  // (worker had ~150 ms of forward motion).
  EXPECT_NE(stopped.joint_positions_rad[0], goal[0])
      << "spring_back_hold MUST NOT silently complete the trajectory";
  EXPECT_NE(stopped.joint_positions_rad[0], start_state.positions_rad[0])
      << "spring_back_hold MUST anchor at observed (mid-flight) state";
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_SoftOnPathRunsWallTimePathRamp) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.6;
  if (goal.size() > 1) {
    goal[1] += 0.2;
  }

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{2.0, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_soft_on_path";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.15;
  profile.max_stop_time_s = 1.0;
  profile.allowed_fallback_modes = {MotionInterruptStopMode::kSpringBackHold};
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto result = bundle_->execution->execute_controlled_stop(profile);

  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  EXPECT_EQ(result.executed_stop_mode, MotionInterruptStopMode::kSoftOnPath);
  EXPECT_EQ(result.stop_command_source,
            MotionInterruptStopCommandSource::kNativeSimulatedPathRamp);
  ASSERT_TRUE(result.path_deviation_max_rad.has_value());
  EXPECT_GE(*result.path_deviation_max_rad, 0.0);
  ASSERT_TRUE(result.stop_distance_rad.has_value());
  EXPECT_GT(*result.stop_distance_rad, 0.0);
  EXPECT_EQ(result.diagnostics.at("runtime_scale_isolated"), "true");
  EXPECT_EQ(result.diagnostics.at("velocity_source"), "model_derived");

  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
  auto stopped = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(stopped.state, ExecutionState::kStopped);
  ASSERT_EQ(stopped.joint_positions_rad.size(), start_state.positions_rad.size());
  EXPECT_NE(stopped.joint_positions_rad[0], goal[0])
      << "soft_on_path stop MUST NOT silently finish the original trajectory";
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_SoftOnPathIgnoresSliderChangesDuringStop) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.7;
  if (goal.size() > 1) {
    goal[1] += 0.2;
  }

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{3.0, goal, {}, {}});

  bundle_->execution->set_runtime_velocity_scale(0.25);
  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(180));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_soft_on_path_scale_isolation";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.25;
  profile.max_stop_time_s = 1.0;
  profile.allowed_fallback_modes = {MotionInterruptStopMode::kSpringBackHold};
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto* execution = bundle_->execution.get();
  std::thread slider_thread([execution]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    execution->set_runtime_velocity_scale(0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    execution->set_runtime_velocity_scale(1.0);
  });

  const auto call_start = std::chrono::steady_clock::now();
  auto result = bundle_->execution->execute_controlled_stop(profile);
  const auto call_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - call_start);
  slider_thread.join();

  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  EXPECT_EQ(result.executed_stop_mode, MotionInterruptStopMode::kSoftOnPath);
  EXPECT_EQ(result.stop_command_source,
            MotionInterruptStopCommandSource::kNativeSimulatedPathRamp);
  EXPECT_EQ(result.diagnostics.at("runtime_scale_isolated"), "true");
  EXPECT_EQ(result.diagnostics.at("runtime_velocity_scale_at_trip"), "0.25");
  EXPECT_LT(call_elapsed, std::chrono::milliseconds(700))
      << "soft_on_path replacement trajectory must run in wall time; "
      << "operator slider changes during the controlled stop must not "
      << "stretch or freeze the stop";
  EXPECT_LT(result.confirmation_latency_ns, std::int64_t{700'000'000});

  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
  auto stopped = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(stopped.state, ExecutionState::kStopped);
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_SoftOnPathAllowsShortPathNaturalCompletion) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.1;

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{0.3, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(220));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_soft_on_path_natural_completion";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 1.0;
  profile.max_stop_time_s = 1.0;
  profile.allow_natural_completion_on_short_path = true;
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto result = bundle_->execution->execute_controlled_stop(profile);

  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  EXPECT_TRUE(result.completed_by_natural_completion);
  EXPECT_EQ(result.executed_stop_mode, MotionInterruptStopMode::kSoftOnPath);
  EXPECT_EQ(result.stop_command_source,
            MotionInterruptStopCommandSource::kDiagnosticOther);
  EXPECT_EQ(result.diagnostics.at("recommended_disposition"),
            "natural_completion");
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
  auto completed = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(completed.state, ExecutionState::kCompleted);
}

TEST_F(NativeKernelTest,
       NativeBackend_EndToEnd_ScopedTripProducesSignalTripOutcome) {
  // End-to-end integration: kernel + RoboPlanExecutionBackend +
  // InterruptManager produce a real signal_trip outcome with a populated
  // MotionTripOutcomeDTO. This exercises the controlled-stop port at the
  // kernel layer (handle_motion_trip_latch -> execute_controlled_stop ->
  // ExecutionStopSnapshotDTO capture), not via MockExecution.
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  // Register a fast-execution-relevant source and a move-scoped
  // complete_move_on_signal policy that permits spring_back_hold.
  InterruptSourceDefinition src;
  src.source_id = "probe";
  src.adapter = InterruptAdapter::kInternalTest;
  src.signal = "probe_signal";
  src.source_class = InterruptSourceClass::kFastExecutionRelevant;
  src.kind = InterruptSourceKind::kEdge;
  src.priority = 700;
  ASSERT_TRUE(bundle_->kernel->register_interrupt_source(src).ok());

  InterruptPolicyDefinition policy;
  policy.policy_id = "probe_trip";
  policy.source_id = "probe";
  policy.priority = 700;
  policy.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  policy.preemption_action = InterruptPreemptionAction::kCompleteMoveOnSignal;
  policy.post_latch_disposition =
      InterruptPostLatchDisposition::kAutoClearAfterMotionOutcome;
  policy.scope_kind = InterruptPolicyScopeKind::kMove;
  policy.allowed_activation_modes = {
      MotionInterruptActivationMode::kTripIfAlreadyActive};
  policy.allowed_stop_modes = {MotionInterruptStopMode::kSpringBackHold};
  ASSERT_TRUE(bundle_->kernel->register_interrupt_policy(policy).ok());

  // Build the request: a long(ish) joint goal so the worker is
  // mid-flight when we publish the trip.
  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.4;
  if (goal.size() > 1) {
    goal[1] -= 0.3;
  }

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.activation_mode = MotionInterruptActivationMode::kTripIfAlreadyActive;
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "spring_back_v1";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.max_stop_time_s = 1.0;
  scope.stop_profile = profile;

  ExecuteRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "scoped_trip_e2e";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.timeouts = {10.0, 30.0};
  req.cache_mode = CacheMode::kDefault;
  JointGoalDTO jg;
  jg.joint_names = start_state.joint_names;
  jg.positions_rad = goal;
  req.motion = jg;
  req.motion_interrupt_scopes = {scope};

  auto exec_result = bundle_->kernel->execute(req);
  ASSERT_TRUE(exec_result.ok())
      << "execute failed: " << exec_result.error().message;

  // Wait for the worker to reach kExecuting before publishing the trip.
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (bundle_->kernel->get_status().state == ExecutionState::kExecuting) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(bundle_->kernel->get_status().state, ExecutionState::kExecuting);

  // Let the worker advance so the anchored stop point is not at the
  // trajectory's start position.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Publish the trip.
  InterruptSignalUpdate trip;
  trip.source_id = "probe";
  trip.sequence_id = 2;
  trip.timestamp_ns = bundle_->clock->now();
  trip.value = true;
  auto ingest = bundle_->kernel->publish_fast_interrupt_signal(trip);
  ASSERT_TRUE(ingest.latched) << "expected move-scoped trip to latch; reason="
                                << ingest.reason;

  ASSERT_TRUE(bundle_->kernel->wait_for_completion(3'000'000'000LL));
  EXPECT_EQ(bundle_->kernel->get_status().state, ExecutionState::kCompleted);

  auto outcome = bundle_->kernel->get_execution_outcome(exec_result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kSignalTrip);
  ASSERT_TRUE(outcome->trip.has_value());
  EXPECT_EQ(outcome->trip->executed_stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  EXPECT_EQ(outcome->trip->stop_command_source,
            MotionInterruptStopCommandSource::kNativeSimulatedStop);
  EXPECT_TRUE(outcome->trip->stop_snapshot.backend_stop_confirmed);
  EXPECT_EQ(outcome->trip->stop_snapshot.source,
            ExecutionStopSnapshotSource::kExecutionBackendFeedback);
  // Stop snapshot satisfies spec/210 §3.7 size/ordering.
  EXPECT_FALSE(outcome->trip->stop_snapshot.robot_state.joint_names.empty());
  EXPECT_EQ(outcome->trip->stop_snapshot.robot_state.joint_names.size(),
            outcome->trip->stop_snapshot.robot_state.positions_rad.size());
  // Latch auto-cleared via auto_clear_after_motion_outcome on success.
  EXPECT_FALSE(bundle_->kernel->get_active_interrupt_latch().has_value());
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_RespectsMaxStopTimeBudget) {
  // The backend's own internal wait honors profile.max_stop_time_s.
  // A short profile budget MUST be observed by the controlled-stop
  // path. (The kernel still enforces its own deadline on top of
  // this; this test isolates the backend wait.)
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.3;

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{1.5, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "test_budget";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.max_stop_time_s = 0.5;  // 500 ms budget

  const auto call_start = bundle_->clock->now();
  auto result = bundle_->execution->execute_controlled_stop(profile);
  const auto call_end = bundle_->clock->now();
  const auto elapsed_ns = call_end - call_start;
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  // The worker should exit promptly (the tick is 10 ms). Latency
  // recorded by the backend must not exceed the elapsed time.
  EXPECT_LE(result.confirmation_latency_ns, elapsed_ns);
  EXPECT_LT(elapsed_ns, std::int64_t{500'000'000})
      << "spring_back_hold MUST exit well before max_stop_time_s under "
      << "nominal conditions";

  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
}
