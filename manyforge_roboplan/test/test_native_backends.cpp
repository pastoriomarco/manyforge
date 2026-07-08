#include <gtest/gtest.h>

#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>

#include <Eigen/Dense>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
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

TEST_F(NativeKernelTest, CloseRejectsNewKernelSnapshotsButPreservesInFlightSnapshot) {
  auto kernel = bundle_->kernel_snapshot();
  ASSERT_NE(kernel, nullptr);

  bundle_->close();

  EXPECT_TRUE(bundle_->closed());
  EXPECT_EQ(bundle_->kernel, nullptr);
  EXPECT_THROW((void)bundle_->kernel_snapshot(), std::runtime_error);
  EXPECT_EQ(kernel->get_status().state, ExecutionState::kIdle);
}

TEST_F(NativeKernelTest, NativeExecutionBackendRejectsInvalidLifecycleTimeouts) {
  RoboPlanExecutionBackendConfig config;
  config.reset_completion_timeout_s = 0.0;
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

TEST(NativeSchedulerTest, MonitoringLaneFiresWhileRuntimeSafetyLaneIsBlocked) {
  // Audit K3 regression: with the previous single-worker scheduler a
  // long-blocking runtime-safety callback (e.g. collision_check_tick stuck
  // waiting on the scene mutex) would prevent the Monitoring watchdog
  // from firing — including the watchdog whose job is to abort that
  // stall. With the split scheduler the lanes are independent.
  SimpleScheduler scheduler;
  std::atomic<bool> runtime_safety_can_exit{false};
  std::atomic<bool> runtime_safety_running{false};
  std::atomic<bool> monitoring_fired{false};

  const auto schedule_at = [&](int64_t offset_ns) {
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return now_ns + offset_ns;
  };

  // Runtime-safety callback parks itself until the test releases it; mirrors
  // a collision_check_tick stalled on `scene_mutex_` during a long RRT.
  scheduler.schedule(
      schedule_at(10'000'000LL),
      [&]() {
        runtime_safety_running.store(true);
        while (!runtime_safety_can_exit.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      },
      SchedulerPriority::RuntimeSafety);

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

  EXPECT_TRUE(runtime_safety_running.load())
      << "Runtime-safety callback did not reach its parking loop";
  EXPECT_TRUE(monitoring_fired.load())
      << "Monitoring watchdog did not fire while runtime-safety lane was blocked";

  // Release the parked runtime-safety callback so the scheduler destructor
  // does not race with a still-running task.
  runtime_safety_can_exit.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

TEST(NativeSchedulerTest, ControllerCommandLaneFiresWhileOtherRolesAreBlocked) {
  SimpleScheduler scheduler;
  std::atomic<bool> release_blocked_roles{false};
  std::atomic<int> blocked_roles_running{0};
  std::atomic<bool> controller_fired{false};

  const auto schedule_at = [&](int64_t offset_ns) {
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return now_ns + offset_ns;
  };

  const auto block_role = [&](SchedulerPriority priority) {
    scheduler.schedule(
        schedule_at(10'000'000LL),
        [&]() {
          blocked_roles_running.fetch_add(1);
          while (!release_blocked_roles.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        },
        priority);
  };

  block_role(SchedulerPriority::RuntimeSafety);
  block_role(SchedulerPriority::StopControl);
  block_role(SchedulerPriority::Lifecycle);
  block_role(SchedulerPriority::Monitoring);
  block_role(SchedulerPriority::Background);

  scheduler.schedule(
      schedule_at(20'000'000LL),
      [&]() { controller_fired.store(true); },
      SchedulerPriority::ControllerCommand);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!controller_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_EQ(blocked_roles_running.load(), 5);
  EXPECT_TRUE(controller_fired.load())
      << "Controller-command lane was starved by blocked lower-priority roles";

  release_blocked_roles.store(true);
  EXPECT_TRUE(scheduler.drain_all_for(std::chrono::seconds(1)));
}

TEST(NativeSchedulerTest, RuntimeSafetyLaneFiresWhileLifecycleLaneIsBlocked) {
  SimpleScheduler scheduler;
  std::atomic<bool> release_lifecycle{false};
  std::atomic<bool> lifecycle_running{false};
  std::atomic<bool> runtime_safety_fired{false};

  const auto schedule_at = [&](int64_t offset_ns) {
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return now_ns + offset_ns;
  };

  scheduler.schedule(
      schedule_at(10'000'000LL),
      [&]() {
        lifecycle_running.store(true);
        while (!release_lifecycle.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      },
      SchedulerPriority::Lifecycle);
  scheduler.schedule(
      schedule_at(20'000'000LL),
      [&]() { runtime_safety_fired.store(true); },
      SchedulerPriority::RuntimeSafety);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!runtime_safety_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(lifecycle_running.load());
  EXPECT_TRUE(runtime_safety_fired.load())
      << "Runtime-safety lane was starved by blocked lifecycle work";

  release_lifecycle.store(true);
  EXPECT_TRUE(scheduler.drain_all_for(std::chrono::seconds(1)));
}

TEST(NativeSchedulerTest, StopControlLaneFiresWhileLowerRolesAreBlocked) {
  SimpleScheduler scheduler;
  std::atomic<bool> release_blocked_roles{false};
  std::atomic<int> blocked_roles_running{0};
  std::atomic<bool> stop_control_fired{false};

  const auto schedule_at = [&](int64_t offset_ns) {
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return now_ns + offset_ns;
  };
  const auto block_role = [&](SchedulerPriority priority) {
    scheduler.schedule(
        schedule_at(10'000'000LL),
        [&]() {
          blocked_roles_running.fetch_add(1);
          while (!release_blocked_roles.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        },
        priority);
  };

  block_role(SchedulerPriority::Lifecycle);
  block_role(SchedulerPriority::Monitoring);
  block_role(SchedulerPriority::Background);

  scheduler.schedule(
      schedule_at(20'000'000LL),
      [&]() { stop_control_fired.store(true); },
      SchedulerPriority::StopControl);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!stop_control_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_EQ(blocked_roles_running.load(), 3);
  EXPECT_TRUE(stop_control_fired.load())
      << "Stop-control lane was starved by blocked lower-priority roles";

  release_blocked_roles.store(true);
  EXPECT_TRUE(scheduler.drain_all_for(std::chrono::seconds(1)));
}

TEST(NativeSchedulerTest, DestructionWakesParkedWorkersOnAllRoles) {
  auto scheduler = std::make_unique<SimpleScheduler>();
  const auto far_future =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          (std::chrono::steady_clock::now() + std::chrono::seconds(60))
              .time_since_epoch())
          .count();
  for (std::size_t i = 0; i < kSchedulerPriorityCount; ++i) {
    scheduler->schedule(
        far_future,
        []() { ADD_FAILURE() << "far-future callback fired during teardown"; },
        scheduler_priority_at(i));
  }

  auto destroy_future = std::async(std::launch::async, [&]() {
    scheduler.reset();
  });
  EXPECT_EQ(destroy_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
}

TEST(NativeSchedulerTest, BackgroundCallbackExceptionDoesNotStopWorker) {
  SimpleScheduler scheduler;
  std::atomic<bool> followup_fired{false};

  const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  scheduler.schedule(
      now_ns,
      []() { throw std::runtime_error("background diagnostic failed"); },
      SchedulerPriority::Background);
  scheduler.schedule(
      now_ns + 1'000'000LL,
      [&]() { followup_fired.store(true); },
      SchedulerPriority::Background);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!followup_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(followup_fired.load())
      << "Background worker stopped after a contained callback exception";
  EXPECT_TRUE(
      scheduler.drain_for(SchedulerPriority::Background, std::chrono::seconds(1)));
}

TEST(NativeSchedulerTest, DrainWaitsForDequeuedRuntimeSafetyCallback) {
  SimpleScheduler scheduler;
  std::promise<void> callback_entered;
  auto callback_entered_future = callback_entered.get_future();
  std::promise<void> release_callback;
  auto release_callback_future = release_callback.get_future();

  const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  scheduler.schedule(
      now_ns,
      [&]() {
        callback_entered.set_value();
        release_callback_future.wait();
      },
      SchedulerPriority::RuntimeSafety);

  ASSERT_EQ(callback_entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  auto drain_future = std::async(std::launch::async, [&]() {
    scheduler.drain(SchedulerPriority::RuntimeSafety);
  });
  EXPECT_EQ(drain_future.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  release_callback.set_value();
  EXPECT_EQ(drain_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
}

TEST(NativeSchedulerTest, DrainForTimesOutWhileRuntimeSafetyCallbackIsBlocked) {
  SimpleScheduler scheduler;
  std::promise<void> callback_entered;
  auto callback_entered_future = callback_entered.get_future();
  std::promise<void> release_callback;
  auto release_callback_future = release_callback.get_future();

  const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  scheduler.schedule(
      now_ns,
      [&]() {
        callback_entered.set_value();
        release_callback_future.wait();
      },
      SchedulerPriority::RuntimeSafety);

  ASSERT_EQ(callback_entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(
      scheduler.drain_for(SchedulerPriority::RuntimeSafety,
                          std::chrono::milliseconds(20)));
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count(),
            250);

  release_callback.set_value();
  EXPECT_TRUE(
      scheduler.drain_for(SchedulerPriority::RuntimeSafety,
                          std::chrono::seconds(1)));
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

TEST_F(NativeKernelTest,
       NativeExecutionSnapshotsStayFreshWhenSceneUpdateLockIsBusy) {
  auto scene_mutex = std::make_shared<std::mutex>();
  RoboPlanExecutionBackend backend(
      bundle_->scene, "arm", *bundle_->clock, scene_mutex);

  TrajectoryDTO trajectory;
  trajectory.joint_names = {"shoulder_pan_joint", "shoulder_lift_joint",
                            "elbow_joint", "wrist_1_joint",
                            "wrist_2_joint", "wrist_3_joint"};
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, {0.0, -0.5, 0.5, -0.5, -0.5, 0.0}, {}, {}});
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{2.0, {0.2, -0.7, 0.7, -0.7, -0.4, 0.1}, {}, {}});

  std::unique_lock<std::mutex> scene_lock(*scene_mutex);
  std::optional<ExecutionHandle> handle;
  std::atomic<bool> dispatch_returned{false};
  std::thread dispatch_thread([&]() {
    handle.emplace(backend.execute_trajectory(trajectory));
    dispatch_returned.store(true);
  });

  const auto dispatch_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (!dispatch_returned.load() &&
         std::chrono::steady_clock::now() < dispatch_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!dispatch_returned.load()) {
    scene_lock.unlock();
    dispatch_thread.join();
    if (handle) {
      auto stop = backend.stop();
      EXPECT_TRUE(stop.accepted) << (stop.error ? stop.error->message : "");
      EXPECT_TRUE(handle->wait_for_completion(1'000'000'000));
    }
    FAIL() << "execute_trajectory must not block behind scene projection";
  }
  dispatch_thread.join();
  ASSERT_TRUE(handle.has_value());

  const auto start_snapshot = backend.get_execution_snapshot();
  auto observed = start_snapshot;
  bool advanced_while_scene_locked = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    observed = backend.get_execution_snapshot();
    if (observed.sequence_number > start_snapshot.sequence_number) {
      advanced_while_scene_locked = true;
      break;
    }
  }

  EXPECT_TRUE(advanced_while_scene_locked)
      << "execution snapshots must not block behind best-effort scene updates";
  const auto age_ns = bundle_->clock->now() - observed.timestamp_ns;
  EXPECT_LT(age_ns, 100'000'000)
      << "runtime collision monitor freshness budget is 100 ms";
  const auto metrics_while_locked = backend.scene_projection_metrics();
  EXPECT_GT(metrics_while_locked.misses_total, 0u)
      << "best-effort scene projection misses must be observable";

  scene_lock.unlock();
  auto metrics_after_unlock = backend.scene_projection_metrics();
  const auto projection_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (metrics_after_unlock.successful_updates_total == 0u &&
         std::chrono::steady_clock::now() < projection_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    metrics_after_unlock = backend.scene_projection_metrics();
  }
  EXPECT_GT(metrics_after_unlock.successful_updates_total, 0u);
  EXPECT_GT(metrics_after_unlock.last_success_timestamp_ns, 0);
  EXPECT_GE(metrics_after_unlock.last_success_age_ns, 0u);

  auto stop = backend.stop();
  EXPECT_TRUE(stop.accepted) << (stop.error ? stop.error->message : "");
  ASSERT_TRUE(handle->wait_for_completion);
  EXPECT_TRUE(handle->wait_for_completion(1'000'000'000));
}

TEST_F(NativeKernelTest, NativeExecutionBackendResumeFreshDispatchesSuppliedTrajectory) {
  RoboPlanExecutionBackend backend(
      bundle_->scene, "arm", *bundle_->clock, nullptr);

  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.joint_names.empty());
  ASSERT_EQ(start_state.joint_names.size(), start_state.positions_rad.size());

  TrajectoryDTO original;
  original.joint_names = start_state.joint_names;
  original.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  auto original_goal = start_state.positions_rad;
  original_goal.front() += 0.2;
  original.waypoints.push_back(
      TrajectoryWaypointDTO{3.0, original_goal, {}, {}});

  auto original_handle = backend.execute_trajectory(original);
  const auto executing_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (backend.get_execution_snapshot().state != ExecutionState::kExecuting &&
         std::chrono::steady_clock::now() < executing_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(backend.get_execution_snapshot().state, ExecutionState::kExecuting);

  auto pause = backend.pause();
  ASSERT_TRUE(pause.accepted) << (pause.error ? pause.error->message : "");
  const auto paused_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (backend.get_execution_snapshot().state != ExecutionState::kPaused &&
         std::chrono::steady_clock::now() < paused_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(backend.get_execution_snapshot().state, ExecutionState::kPaused);

  TrajectoryDTO resumed;
  resumed.joint_names = start_state.joint_names;
  auto resume_start = start_state.positions_rad;
  resume_start.front() += 0.45;
  auto resume_goal = resume_start;
  resume_goal.front() += 0.05;
  resumed.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, resume_start, {}, {}});
  resumed.waypoints.push_back(
      TrajectoryWaypointDTO{3.0, resume_goal, {}, {}});

  auto resume_handle = backend.resume(resumed);
  const auto resume_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  ExecutionSnapshot snapshot;
  while (std::chrono::steady_clock::now() < resume_deadline) {
    snapshot = backend.get_execution_snapshot();
    if (snapshot.state == ExecutionState::kExecuting &&
        snapshot.joint_positions_rad.size() == resume_start.size() &&
        std::abs(snapshot.joint_positions_rad.front() - resume_start.front()) <
            1e-9) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_EQ(snapshot.state, ExecutionState::kExecuting);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), resume_start.size());
  EXPECT_NEAR(snapshot.joint_positions_rad.front(), resume_start.front(), 1e-9);
  EXPECT_NEAR(snapshot.joint_positions_rad.front(), start_state.positions_rad.front() + 0.45, 1e-9);

  auto stop = backend.stop();
  EXPECT_TRUE(stop.accepted) << (stop.error ? stop.error->message : "");
  ASSERT_TRUE(resume_handle.wait_for_completion);
  EXPECT_TRUE(resume_handle.wait_for_completion(1'000'000'000));
  (void)original_handle;
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
  EXPECT_EQ(result.error->code, kErrorPlanningTimeout);
  EXPECT_EQ(result.error->details.at("reason"), "planning_timeout");
  EXPECT_NE(result.error->message.find("timed out"), std::string::npos);
}

TEST_F(NativeKernelTest, PlannerDoesNotHoldSceneMutexDuringLongRrt) {
  auto scene_mutex = std::make_shared<std::mutex>();
  RoboPlanPlanningContext context(bundle_->scene, "arm", scene_mutex);

  roboplan::RRTOptions rrt_opts;
  rrt_opts.group_name = "arm";
  rrt_opts.max_planning_time = 0.5;
  rrt_opts.max_nodes = 5'000'000;
  rrt_opts.max_connection_distance = 1e-9;
  rrt_opts.collision_check_step_size = 1e-9;
  rrt_opts.goal_biasing_probability = 0.0;

  roboplan::SimpleIkOptions ik_opts;
  ik_opts.group_name = "arm";
  ik_opts.step_size = 0.25;
  ik_opts.max_restarts = 50;
  ik_opts.max_time = 1.0;
  auto single_shot_ik_opts = ik_opts;
  single_shot_ik_opts.max_time = 0.05;

  RoboPlanPlannerBackend planner(bundle_->scene, rrt_opts, bundle_->shortcutter,
                                 bundle_->toppra, bundle_->ik, scene_mutex,
                                 bundle_->single_shot_ik, ik_opts,
                                 single_shot_ik_opts);

  PlanRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "native_long_rrt_lock_scope";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.start_state = context.get_current_state();
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.planning_timeout_s = 0.5;

  JointGoalDTO goal;
  goal.joint_names = req.start_state.joint_names;
  goal.positions_rad = {2.6, -2.4, 2.2, -2.0, -1.8, 1.6};
  req.motion = goal;

  auto plan_future = std::async(std::launch::async,
                                [&]() { return planner.plan(req); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_EQ(plan_future.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout)
      << "test setup expected RRT to still be active";

  const auto read_started = std::chrono::steady_clock::now();
  auto state = context.get_current_state();
  const auto read_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - read_started);
  EXPECT_FALSE(state.joint_names.empty());
  EXPECT_LT(read_elapsed.count(), 150)
      << "planner held scene_mutex across long RRT work";

  auto result = plan_future.get();
  ASSERT_FALSE(result.trajectory.has_value());
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorPlanningTimeout);
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
  // This test exercises the TCP limit diagnostics, not the planner deadline.
  req.planning_timeout_s = std::numeric_limits<double>::infinity();

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

namespace {
// Peak |value| for one joint across a trajectory's per-waypoint vectors.
// `field` selects positions_rad / velocities_rad_s / accelerations_rad_s2.
double peak_abs_for_joint(const TrajectoryDTO& traj, const std::string& joint,
                          std::vector<double> TrajectoryWaypointDTO::*field) {
  auto it = std::find(traj.joint_names.begin(), traj.joint_names.end(), joint);
  if (it == traj.joint_names.end()) {
    return -1.0;
  }
  const size_t j =
      static_cast<size_t>(std::distance(traj.joint_names.begin(), it));
  double peak = 0.0;
  for (const auto& wp : traj.waypoints) {
    const std::vector<double>& vec = wp.*field;
    if (j < vec.size()) {
      peak = std::max(peak, std::abs(vec[j]));
    }
  }
  return peak;
}
}  // namespace

// Phase 4-P5.1K: a tight per-joint velocity cap must reduce the sampled peak
// joint velocity to at/below the cap and lengthen the trajectory, proving the
// cap is enforced by the TOPP-RA retiming layer (not just carried).
TEST_F(NativeKernelTest, PerJointVelocityCapSlowsTrajectoryAndBoundsPeak) {
  auto make_req = [&](const char* move_id) {
    PlanRequestDTO req;
    req.robot_id = "robot1";
    req.group_id = "arm";
    req.move_id = move_id;
    req.base_frame = "base_link";
    req.tcp_frame = "tool0";
    req.start_state = bundle_->context->get_current_state();
    req.tolerances = {0.05, 0.005, 0.05};
    req.speed_profile = {1.0, 1.0, std::nullopt};
    req.planning_timeout_s = 10.0;
    JointGoalDTO goal;
    goal.joint_names = req.start_state.joint_names;
    goal.positions_rad = {0.3, -0.8, 0.8, -0.3, -0.8, 0.1};
    req.motion = goal;
    return req;
  };

  auto base = bundle_->planner->plan(make_req("dyn_vel_baseline"));
  ASSERT_TRUE(base.trajectory.has_value())
      << (base.error ? base.error->message : "no error");
  const double base_peak = peak_abs_for_joint(
      *base.trajectory, "elbow_joint", &TrajectoryWaypointDTO::velocities_rad_s);
  ASSERT_GT(base_peak, 0.0);
  const double base_duration = base.trajectory->waypoints.back().time_from_start_s;

  const double cap = base_peak * 0.5;  // force the limiter to engage
  auto capped_req = make_req("dyn_vel_capped");
  JointDynamicsLimitDTO lim;
  lim.max_velocity_rad_s = cap;
  capped_req.per_joint_dynamics_limits["elbow_joint"] = lim;

  auto capped = bundle_->planner->plan(capped_req);
  ASSERT_TRUE(capped.trajectory.has_value())
      << (capped.error ? capped.error->message : "no error");
  const double capped_peak = peak_abs_for_joint(
      *capped.trajectory, "elbow_joint", &TrajectoryWaypointDTO::velocities_rad_s);
  const double capped_duration =
      capped.trajectory->waypoints.back().time_from_start_s;

  EXPECT_LE(capped_peak, cap * 1.05);
  EXPECT_GT(capped_duration, base_duration);
}

// Phase 4-P5.1K: a tight per-joint acceleration cap must reduce the sampled
// peak joint acceleration to at/below the cap and lengthen the trajectory.
TEST_F(NativeKernelTest, PerJointAccelerationCapBoundsPeak) {
  auto make_req = [&](const char* move_id) {
    PlanRequestDTO req;
    req.robot_id = "robot1";
    req.group_id = "arm";
    req.move_id = move_id;
    req.base_frame = "base_link";
    req.tcp_frame = "tool0";
    req.start_state = bundle_->context->get_current_state();
    req.tolerances = {0.05, 0.005, 0.05};
    req.speed_profile = {1.0, 1.0, std::nullopt};
    req.planning_timeout_s = 10.0;
    JointGoalDTO goal;
    goal.joint_names = req.start_state.joint_names;
    goal.positions_rad = {0.3, -0.8, 0.8, -0.3, -0.8, 0.1};
    req.motion = goal;
    return req;
  };

  auto base = bundle_->planner->plan(make_req("dyn_acc_baseline"));
  ASSERT_TRUE(base.trajectory.has_value())
      << (base.error ? base.error->message : "no error");
  const double base_peak =
      peak_abs_for_joint(*base.trajectory, "elbow_joint",
                         &TrajectoryWaypointDTO::accelerations_rad_s2);
  ASSERT_GT(base_peak, 0.0);
  const double base_duration = base.trajectory->waypoints.back().time_from_start_s;

  const double cap = base_peak * 0.7;  // achievable reduction; engages the limiter
  auto capped_req = make_req("dyn_acc_capped");
  JointDynamicsLimitDTO lim;
  lim.max_acceleration_rad_s2 = cap;
  capped_req.per_joint_dynamics_limits["elbow_joint"] = lim;

  auto capped = bundle_->planner->plan(capped_req);
  ASSERT_TRUE(capped.trajectory.has_value())
      << (capped.error ? capped.error->message : "no error");
  const double capped_peak =
      peak_abs_for_joint(*capped.trajectory, "elbow_joint",
                         &TrajectoryWaypointDTO::accelerations_rad_s2);
  const double capped_duration =
      capped.trajectory->waypoints.back().time_from_start_s;

  EXPECT_LE(capped_peak, cap * 1.10);  // accel sampling carries more margin
  EXPECT_GT(capped_duration, base_duration);
}

// Phase 4-P5.1K fail-closed: the discrete TOPP-RA path cannot enforce jerk, so
// any supplied jerk cap is rejected rather than silently accepted.
TEST_F(NativeKernelTest, PerJointJerkCapRejectedAsUnsupported) {
  PlanRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "dyn_jerk_rejected";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.start_state = bundle_->context->get_current_state();
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.planning_timeout_s = 10.0;
  JointGoalDTO goal;
  goal.joint_names = req.start_state.joint_names;
  goal.positions_rad = {0.3, -0.8, 0.8, -0.3, -0.8, 0.1};
  req.motion = goal;
  JointDynamicsLimitDTO lim;
  lim.max_jerk_rad_s3 = 5.0;
  req.per_joint_dynamics_limits["elbow_joint"] = lim;

  auto result = bundle_->planner->plan(req);
  ASSERT_FALSE(result.trajectory.has_value());
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorUnsupportedCapability);
  EXPECT_EQ(result.error->details.at("reason"), "unsupported_joint_dynamics_limit");
  EXPECT_EQ(result.error->details.at("dimension"), "jerk");
}

// Phase 4-P5.1K: a MALFORMED jerk cap (NaN/inf/<=0) is an invalid request, not
// merely "unsupported" — value validation precedes the enforceability verdict.
TEST_F(NativeKernelTest, PerJointMalformedJerkCapRejectedAsInvalid) {
  for (const double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
    PlanRequestDTO req;
    req.robot_id = "robot1";
    req.group_id = "arm";
    req.move_id = "dyn_jerk_invalid";
    req.base_frame = "base_link";
    req.tcp_frame = "tool0";
    req.start_state = bundle_->context->get_current_state();
    req.tolerances = {0.05, 0.005, 0.05};
    req.speed_profile = {1.0, 1.0, std::nullopt};
    req.planning_timeout_s = 10.0;
    JointGoalDTO goal;
    goal.joint_names = req.start_state.joint_names;
    goal.positions_rad = {0.3, -0.8, 0.8, -0.3, -0.8, 0.1};
    req.motion = goal;
    JointDynamicsLimitDTO lim;
    lim.max_jerk_rad_s3 = bad;
    req.per_joint_dynamics_limits["elbow_joint"] = lim;

    auto result = bundle_->planner->plan(req);
    ASSERT_FALSE(result.trajectory.has_value());
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, kErrorInvalidRequest) << "bad jerk = " << bad;
    EXPECT_EQ(result.error->details.at("reason"), "invalid_joint_dynamics_limit");
    EXPECT_EQ(result.error->details.at("dimension"), "jerk");
  }
}

// Phase 4-P5.1K fail-closed: a non-positive cap is rejected at the kernel
// boundary even if upstream validation were bypassed.
TEST_F(NativeKernelTest, PerJointNonPositiveVelocityCapRejected) {
  PlanRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "dyn_invalid_velocity";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.start_state = bundle_->context->get_current_state();
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.planning_timeout_s = 10.0;
  JointGoalDTO goal;
  goal.joint_names = req.start_state.joint_names;
  goal.positions_rad = {0.3, -0.8, 0.8, -0.3, -0.8, 0.1};
  req.motion = goal;
  JointDynamicsLimitDTO lim;
  lim.max_velocity_rad_s = -1.0;
  req.per_joint_dynamics_limits["elbow_joint"] = lim;

  auto result = bundle_->planner->plan(req);
  ASSERT_FALSE(result.trajectory.has_value());
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorInvalidRequest);
  EXPECT_EQ(result.error->details.at("reason"), "invalid_joint_dynamics_limit");
}

// Phase 4-P5.1K fail-closed: a cap keyed by a joint that is not in the active
// planning group (e.g. a typo) must be rejected, never silently ignored.
TEST_F(NativeKernelTest, PerJointCapUnknownJointRejected) {
  PlanRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "dyn_unknown_joint";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.start_state = bundle_->context->get_current_state();
  req.tolerances = {0.05, 0.005, 0.05};
  req.speed_profile = {1.0, 1.0, std::nullopt};
  req.planning_timeout_s = 10.0;
  JointGoalDTO goal;
  goal.joint_names = req.start_state.joint_names;
  goal.positions_rad = {0.3, -0.8, 0.8, -0.3, -0.8, 0.1};
  req.motion = goal;
  JointDynamicsLimitDTO lim;
  lim.max_velocity_rad_s = 0.5;
  req.per_joint_dynamics_limits["elbow_joint_typo"] = lim;  // not a group joint

  auto result = bundle_->planner->plan(req);
  ASSERT_FALSE(result.trajectory.has_value());
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorUnsupportedCapability);
  EXPECT_EQ(result.error->details.at("reason"), "unsupported_joint_dynamics_limit");
  EXPECT_EQ(result.error->details.at("joint"), "elbow_joint_typo");
  EXPECT_EQ(result.error->details.at("failure_stage"), "request_validation");
}

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
  ASSERT_TRUE(cap.hard_stop_level.has_value());
  EXPECT_EQ(*cap.hard_stop_level, "native_immediate_hold");
  ASSERT_TRUE(cap.derivative_proof_level.has_value());
  EXPECT_EQ(*cap.derivative_proof_level, "manyforge_core_native_path_ramp");
  ASSERT_TRUE(cap.scaling_source_visibility.has_value());
  EXPECT_EQ(*cap.scaling_source_visibility, "native_runtime_scale");
  ASSERT_TRUE(cap.scaling_source_probe_status.has_value());
  EXPECT_EQ(*cap.scaling_source_probe_status, "not_applicable");
  EXPECT_EQ(cap.unsupported_reasons.at("hard_deceleration"),
            "ros2_control_jtc_only_in_current_native_backend");
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
  // spring_back_hold is now a two-phase on-path replacement: forward decel,
  // then return along the accepted path to the trip cursor. It must not silently
  // complete the original trajectory, and it must not use the old native
  // stop-and-hold shortcut.
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
  profile.deceleration_time_s = 0.5;
  profile.max_stop_time_s = 1.5;
  for (const auto& joint_name : traj.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  const auto call_start = bundle_->clock->now();
  auto result = bundle_->execution->execute_controlled_stop(profile);
  const auto call_end = bundle_->clock->now();
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  EXPECT_EQ(result.executed_stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  EXPECT_EQ(result.stop_command_source,
            MotionInterruptStopCommandSource::kNativeSimulatedPathRamp);
  EXPECT_EQ(result.diagnostics["spring_back_mode"], "on_path_two_phase");
  EXPECT_GE(result.confirmation_latency_ns, 0);
  EXPECT_LE(result.confirmation_latency_ns,
            std::int64_t{call_end - call_start});

  // The wait_for_completion handle is now satisfied (the worker
  // exited under stop_token).
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));

  // Post-stop snapshot reflects the returned trip point, not the planned
  // endpoint. It should also have advanced from the trajectory start because
  // the worker had time to move before the stop was requested.
  auto stopped = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(stopped.state, ExecutionState::kStopped);
  ASSERT_EQ(stopped.joint_positions_rad.size(),
            start_state.positions_rad.size());
  EXPECT_NE(stopped.joint_positions_rad[0], goal[0])
      << "spring_back_hold MUST NOT silently complete the trajectory";
  EXPECT_NE(stopped.joint_positions_rad[0], start_state.positions_rad[0])
      << "spring_back_hold MUST return to the trip cursor, not reset to start";
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_SpringBackHoldPublishesReplaySnapshots) {
  // The native viewer consumes the backend/kernel execution snapshot stream.
  // spring_back_hold must therefore publish the authored two-phase replacement
  // while it runs, not only the terminal returned trip point.
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.7;
  if (goal.size() > 1) {
    goal[1] -= 0.3;
  }

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{3.0, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "test_spring_back_snapshot_stream";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.deceleration_time_s = 1.0;
  profile.max_stop_time_s = 2.0;
  for (const auto& joint_name : traj.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto stop_future = std::async(std::launch::async, [&]() {
    return bundle_->execution->execute_controlled_stop(profile);
  });

  bool observed_stopping = false;
  double min_j0 = std::numeric_limits<double>::infinity();
  double max_j0 = -std::numeric_limits<double>::infinity();
  std::uint64_t min_sequence = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_sequence = 0;
  const auto sample_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
  while (std::chrono::steady_clock::now() < sample_deadline) {
    auto snapshot = bundle_->execution->get_execution_snapshot();
    if (snapshot.state == ExecutionState::kStopping &&
        !snapshot.joint_positions_rad.empty()) {
      observed_stopping = true;
      min_j0 = std::min(min_j0, snapshot.joint_positions_rad[0]);
      max_j0 = std::max(max_j0, snapshot.joint_positions_rad[0]);
      min_sequence = std::min(min_sequence, snapshot.sequence_number);
      max_sequence = std::max(max_sequence, snapshot.sequence_number);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  auto result = stop_future.get();
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  EXPECT_EQ(result.executed_stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  EXPECT_TRUE(observed_stopping)
      << "spring_back_hold must expose an in-progress Stopping snapshot";
  EXPECT_GT(max_sequence, min_sequence)
      << "spring_back_hold must publish multiple replay snapshots";
  EXPECT_GT(max_j0 - min_j0, 1e-4)
      << "spring_back_hold replay snapshots must move through the path ramp";

  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_SpringBackHoldZeroVelocityTermination) {
  // spring_back_hold parity: the native backend MUST end the two-phase
  // spring-back replay in a stable stopped state. The native simulator is
  // kinematic, so the safety property we pin here is no residual motion after
  // the returned trip cursor is published.
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.5;
  if (goal.size() > 1) {
    goal[1] -= 0.3;
  }

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{2.0, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_spring_back_zero_velocity";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.deceleration_time_s = 0.5;
  profile.max_stop_time_s = 1.5;
  for (const auto& joint_name : traj.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto result = bundle_->execution->execute_controlled_stop(profile);
  ASSERT_TRUE(result.accepted);
  ASSERT_TRUE(result.backend_stop_confirmed);
  EXPECT_EQ(result.executed_stop_mode,
            MotionInterruptStopMode::kSpringBackHold);

  // Worker yielded under the stop — it is no longer driving the arm.
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));

  auto held = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(held.state, ExecutionState::kStopped);
  ASSERT_EQ(held.joint_positions_rad.size(), start_state.positions_rad.size());
  // Returned to the trip area: did not silently complete the trajectory.
  EXPECT_NE(held.joint_positions_rad[0], goal[0])
      << "spring_back_hold must not complete the trajectory";

  // Zero-velocity terminus + stable hold: sample again after a hold window;
  // the anchor must be unchanged. A nonzero residual velocity (coasting /
  // overshoot) or a still-running worker would move the joint here.
  const auto held_positions = held.joint_positions_rad;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto still = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(still.state, ExecutionState::kStopped)
      << "spring_back_hold must hold kStopped (no re-entry to motion)";
  ASSERT_EQ(still.joint_positions_rad.size(), held_positions.size());
  for (size_t i = 0; i < held_positions.size(); ++i) {
    EXPECT_DOUBLE_EQ(still.joint_positions_rad[i], held_positions[i])
        << "spring_back_hold must hold a stable zero-velocity anchor (joint "
        << i << ")";
  }
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
       NativeBackend_ControlledStop_TerminalSceneProjectionMissRetries) {
  auto* backend =
      dynamic_cast<RoboPlanExecutionBackend*>(bundle_->execution.get());
  ASSERT_NE(backend, nullptr);

  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.6;

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{2.0, goal, {}, {}});

  std::unique_lock<std::mutex> scene_lock(*bundle_->scene_mutex);
  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_soft_on_path_scene_retry";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.15;
  profile.max_stop_time_s = 1.0;
  profile.allowed_fallback_modes = {MotionInterruptStopMode::kSpringBackHold};
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto result = bundle_->execution->execute_controlled_stop(profile);
  ASSERT_TRUE(result.accepted) << (result.error ? result.error->message : "");
  ASSERT_TRUE(result.backend_stop_confirmed)
      << (result.error ? result.error->message : "");

  const auto metrics_while_locked = backend->scene_projection_metrics();
  EXPECT_GT(metrics_while_locked.misses_total, 0u);
  EXPECT_GT(metrics_while_locked.terminal_retries_scheduled_total, 0u);
  EXPECT_EQ(metrics_while_locked.terminal_retry_successes_total, 0u);

  scene_lock.unlock();
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));

  auto metrics_after_unlock = backend->scene_projection_metrics();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (metrics_after_unlock.terminal_retry_successes_total == 0u &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    metrics_after_unlock = backend->scene_projection_metrics();
  }

  EXPECT_GT(metrics_after_unlock.terminal_retry_successes_total, 0u);
  EXPECT_EQ(metrics_after_unlock.terminal_retry_exhausted_total, 0u);
  EXPECT_GT(metrics_after_unlock.successful_updates_total, 0u);
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_SoftOnPathReplayIsCancellable) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.8;
  if (goal.size() > 1) {
    goal[1] += 0.3;
  }

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{3.0, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(180));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_soft_on_path_cancellable";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.45;
  profile.max_stop_time_s = 1.0;
  profile.allowed_fallback_modes = {MotionInterruptStopMode::kSpringBackHold};
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  std::stop_source stop_source;
  std::optional<ControlledStopResultDTO> result;
  std::thread stop_thread([&]() {
    result = bundle_->execution->execute_controlled_stop(
        profile, stop_source.get_token());
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  const auto cancel_start = std::chrono::steady_clock::now();
  stop_source.request_stop();
  stop_thread.join();
  const auto cancel_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - cancel_start);

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->accepted);
  EXPECT_FALSE(result->backend_stop_confirmed);
  ASSERT_TRUE(result->error.has_value());
  EXPECT_EQ(result->error->details.at("reason"), "controlled_stop_cancelled");
  EXPECT_LT(cancel_elapsed, std::chrono::milliseconds(150));
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
  auto stopped = bundle_->execution->get_execution_snapshot();
  EXPECT_EQ(stopped.state, ExecutionState::kStopped);
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_SoftOnPathReplayStopsOnBackendStop) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.8;
  if (goal.size() > 1) {
    goal[1] += 0.3;
  }

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{3.0, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(180));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_soft_on_path_backend_stop";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.7;
  profile.max_stop_time_s = 1.5;
  profile.allowed_fallback_modes = {MotionInterruptStopMode::kSpringBackHold};
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto stop_future = std::async(std::launch::async, [&]() {
    return bundle_->execution->execute_controlled_stop(profile);
  });

  bool observed_replay = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (bundle_->execution->get_execution_snapshot().state ==
        ExecutionState::kStopping) {
      observed_replay = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(observed_replay);

  const auto cancel_start = std::chrono::steady_clock::now();
  auto stop = bundle_->execution->stop();
  ASSERT_TRUE(stop.accepted);
  if (stop_future.wait_for(std::chrono::milliseconds(200)) !=
      std::future_status::ready) {
    auto late_result = stop_future.get();
    (void)late_result;
    FAIL() << "backend stop did not cancel native soft_on_path replay";
  }
  const auto cancel_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - cancel_start);
  auto result = stop_future.get();
  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.backend_stop_confirmed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "controlled_stop_cancelled");
  EXPECT_LT(cancel_elapsed, std::chrono::milliseconds(200));
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
}

TEST_F(NativeKernelTest,
       NativeBackend_ControlledStop_SpringBackReplayStopsOnBackendStop) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.8;
  if (goal.size() > 1) {
    goal[1] -= 0.3;
  }

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{3.0, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(180));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_spring_back_backend_stop";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.deceleration_time_s = 1.0;
  profile.max_stop_time_s = 2.0;
  for (const auto& joint_name : traj.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto stop_future = std::async(std::launch::async, [&]() {
    return bundle_->execution->execute_controlled_stop(profile);
  });

  bool observed_replay = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (bundle_->execution->get_execution_snapshot().state ==
        ExecutionState::kStopping) {
      observed_replay = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(observed_replay);

  const auto cancel_start = std::chrono::steady_clock::now();
  auto stop = bundle_->execution->stop();
  ASSERT_TRUE(stop.accepted);
  if (stop_future.wait_for(std::chrono::milliseconds(200)) !=
      std::future_status::ready) {
    auto late_result = stop_future.get();
    (void)late_result;
    FAIL() << "backend stop did not cancel native spring_back replay";
  }
  const auto cancel_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - cancel_start);
  auto result = stop_future.get();
  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.backend_stop_confirmed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "controlled_stop_cancelled");
  EXPECT_LT(cancel_elapsed, std::chrono::milliseconds(200));
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
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
  EXPECT_EQ(result.diagnostics.at("runtime_scale_isolation"),
            "owned_scale_frozen_during_stop");
  EXPECT_EQ(result.diagnostics.at("timeline_authority_tier"),
            "owned_scale_asserted");
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
       NativeBackend_ControlledStop_DefersDirectScaleWriteUntilStopExits) {
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

  bundle_->execution->set_runtime_velocity_scale(1.0);
  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(180));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_soft_on_path_direct_scale_defer";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.45;
  profile.max_stop_time_s = 1.0;
  profile.allowed_fallback_modes = {MotionInterruptStopMode::kSpringBackHold};
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto* execution = bundle_->execution.get();
  std::thread slider_thread([execution]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    execution->set_runtime_velocity_scale(0.0);
  });

  auto result = bundle_->execution->execute_controlled_stop(profile);
  slider_thread.join();

  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  EXPECT_EQ(result.executed_stop_mode, MotionInterruptStopMode::kSoftOnPath);
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));

  auto restart_goal = start_state.positions_rad;
  restart_goal[0] += 0.4;
  TrajectoryDTO restart;
  restart.joint_names = start_state.joint_names;
  restart.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  restart.waypoints.push_back(TrajectoryWaypointDTO{1.0, restart_goal, {}, {}});

  auto restart_handle = bundle_->execution->execute_trajectory(restart);
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  const auto held = restart_handle.get_snapshot();
  ASSERT_EQ(held.joint_positions_rad.size(), start_state.positions_rad.size());
  EXPECT_NEAR(held.joint_positions_rad[0], start_state.positions_rad[0], 0.02)
      << "direct backend scale write made during a controlled stop must be "
         "applied after the stop exits, freezing the next trajectory at scale=0";

  bundle_->execution->set_runtime_velocity_scale(1.0);
  ASSERT_TRUE(restart_handle.wait_for_completion(3'000'000'000LL));
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
       NativeBackend_NaturalCompletionPublishesStoppingWhileOriginalTrajectoryFinishes) {
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.positions_rad.empty());

  std::vector<double> goal = start_state.positions_rad;
  goal[0] += 0.2;

  TrajectoryDTO traj;
  traj.joint_names = start_state.joint_names;
  traj.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, start_state.positions_rad, {}, {}});
  traj.waypoints.push_back(TrajectoryWaypointDTO{0.8, goal, {}, {}});

  auto handle = bundle_->execution->execute_trajectory(traj);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_soft_on_path_natural_completion_visible";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 1.0;
  profile.max_stop_time_s = 1.0;
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  auto future = std::async(std::launch::async, [this, profile]() {
    return bundle_->execution->execute_controlled_stop(profile);
  });

  bool saw_stopping = false;
  for (int i = 0; i < 50; ++i) {
    if (future.wait_for(std::chrono::milliseconds(0)) ==
        std::future_status::ready) {
      break;
    }
    if (bundle_->execution->get_execution_snapshot().state ==
        ExecutionState::kStopping) {
      saw_stopping = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  auto result = future.get();
  EXPECT_TRUE(saw_stopping);
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  EXPECT_TRUE(result.completed_by_natural_completion);
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
  EXPECT_EQ(bundle_->execution->get_execution_snapshot().state,
            ExecutionState::kCompleted);
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
  goal[0] += 1.0;
  if (goal.size() > 1) {
    goal[1] -= 0.6;
  }

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.activation_mode = MotionInterruptActivationMode::kTripIfAlreadyActive;
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "spring_back_v1";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  // Keep this scoped-trip fixture out of the short-path refusal branch: the
  // trajectory is deliberately long enough for the two-phase spring-back ramp
  // to fit before the accepted endpoint, so this test exercises the successful
  // signal-trip outcome rather than planner-limit rejection.
  profile.deceleration_time_s = 0.5;
  profile.max_stop_time_s = 1.5;
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }
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

  // Let the worker advance so the spring-back trip point is not at the
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
            MotionInterruptStopCommandSource::kNativeSimulatedPathRamp);
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
  // spring_back_hold is now a timed two-phase path ramp. The backend should
  // spend approximately the requested stop time replaying it, and still remain
  // bounded by the profile stop deadline.
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
  profile.deceleration_time_s = 0.5;
  profile.max_stop_time_s = 1.5;
  for (const auto& joint_name : traj.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }

  const auto call_start = bundle_->clock->now();
  auto result = bundle_->execution->execute_controlled_stop(profile);
  const auto call_end = bundle_->clock->now();
  const auto elapsed_ns = call_end - call_start;
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  // The worker yields promptly, then the backend replays the two-phase
  // replacement over the requested stop time. Latency recorded by the backend
  // must not exceed the elapsed time.
  EXPECT_LE(result.confirmation_latency_ns, elapsed_ns);
  EXPECT_GE(elapsed_ns, std::int64_t{400'000'000})
      << "spring_back_hold should visibly execute the requested path ramp";
  EXPECT_LT(elapsed_ns, std::int64_t{1'500'000'000})
      << "spring_back_hold MUST remain inside max_stop_time_s under nominal "
      << "conditions";

  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000LL));
}

TEST_F(NativeKernelTest,
       NativeKernel_OperatorSpringBackStopRemainsStoppingForPathRamp) {
  ExecutionKernelConfig kernel_config;
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "operator_spring_back_hold";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.deceleration_time_s = 0.5;
  profile.max_stop_time_s = 1.5;
  auto start_state = bundle_->context->get_current_state();
  ASSERT_FALSE(start_state.joint_names.empty());
  for (const auto& joint_name : start_state.joint_names) {
    profile.per_joint_max_deceleration_rad_s2[joint_name] = 100.0;
  }
  kernel_config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(profile);

  auto local_bundle = create_native_kernel("ur5_gripper", "arm", kernel_config);
  ASSERT_NE(local_bundle, nullptr);
  auto local_start = local_bundle->context->get_current_state();
  ASSERT_FALSE(local_start.positions_rad.empty());

  std::vector<double> goal = local_start.positions_rad;
  goal[0] += 0.8;
  if (goal.size() > 1) {
    goal[1] -= 0.4;
  }

  ExecuteRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "operator_spring_back_e2e";
  req.base_frame = "base_link";
  req.tcp_frame = "tool0";
  req.tolerances = {0.05, 0.005, 0.05};
  // Keep the source trajectory comfortably longer than the fixed stop
  // observation window. This test is about native spring-back replay staying
  // observable in kStopping; if the nominal move completes before stop captures
  // the active trajectory, the kernel correctly reports kCompleted instead.
  req.speed_profile = {0.2, 0.2, std::nullopt};
  req.timeouts = {10.0, 30.0};
  req.cache_mode = CacheMode::kDefault;
  JointGoalDTO jg;
  jg.joint_names = local_start.joint_names;
  jg.positions_rad = goal;
  req.motion = jg;

  auto exec_result = local_bundle->kernel->execute(req);
  ASSERT_TRUE(exec_result.ok())
      << "execute failed: " << exec_result.error().message;
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (local_bundle->kernel->get_status().state == ExecutionState::kExecuting) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(local_bundle->kernel->get_status().state, ExecutionState::kExecuting);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto stop_start = std::chrono::steady_clock::now();
  auto stop = local_bundle->kernel->stop();
  ASSERT_TRUE(stop.accepted);

  // stop() returns after accepting operator intent, but the native backend
  // should still be replaying the spring-back replacement rather than
  // immediately collapsing to Stopped.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(local_bundle->kernel->get_status().state, ExecutionState::kStopping);

  bool observed_stopping_snapshot = false;
  double min_j0 = std::numeric_limits<double>::infinity();
  double max_j0 = -std::numeric_limits<double>::infinity();
  std::uint64_t min_sequence = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_sequence = 0;
  const auto sample_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
  while (std::chrono::steady_clock::now() < sample_deadline) {
    auto snapshot = local_bundle->kernel->get_execution_snapshot();
    if (snapshot.state == ExecutionState::kStopping &&
        !snapshot.joint_positions_rad.empty()) {
      observed_stopping_snapshot = true;
      min_j0 = std::min(min_j0, snapshot.joint_positions_rad[0]);
      max_j0 = std::max(max_j0, snapshot.joint_positions_rad[0]);
      min_sequence = std::min(min_sequence, snapshot.sequence_number);
      max_sequence = std::max(max_sequence, snapshot.sequence_number);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_TRUE(observed_stopping_snapshot)
      << "operator spring_back_hold must expose live Stopping snapshots";
  EXPECT_GT(max_sequence, min_sequence)
      << "operator spring_back_hold must publish multiple replay snapshots";
  EXPECT_GT(max_j0 - min_j0, 1e-4)
      << "operator spring_back_hold replay snapshots must move through the path ramp";

  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto state = local_bundle->kernel->get_status().state;
    if (state == ExecutionState::kStopped ||
        state == ExecutionState::kCompleted ||
        state == ExecutionState::kInterventionRequired ||
        state == ExecutionState::kFailed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
  EXPECT_EQ(local_bundle->kernel->get_status().state, ExecutionState::kStopped);
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(
                stop_elapsed).count(),
            400)
      << "operator spring_back_hold should visibly execute the requested path ramp";

  auto final_snapshot = local_bundle->kernel->get_execution_snapshot();
  ASSERT_FALSE(final_snapshot.joint_positions_rad.empty());
  EXPECT_NE(final_snapshot.joint_positions_rad[0], goal[0])
      << "spring_back_hold must not complete the original trajectory";
}
