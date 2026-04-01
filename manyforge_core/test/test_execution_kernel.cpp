#include <gtest/gtest.h>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

namespace manyforge::core {
namespace {

// --- Mock backends ---

class MockPlanner : public IPlannerBackend {
 public:
  PlanResultDTO next_result;
  int plan_count = 0;
  PlanRequestDTO last_request;

  PlanResultDTO plan(const PlanRequestDTO& request) override {
    ++plan_count;
    last_request = request;
    return next_result;
  }
};

class MockContext : public IPlanningContext {
 public:
  RobotStateDTO current_state;
  ValidationResultDTO next_validation;
  CollisionCheckResultDTO next_collision;
  Pose3DTO next_fk;
  JointLimitsDTO next_limits;
  SceneEditResultDTO next_scene_edit;

  int get_state_count = 0;
  int validate_count = 0;
  int collision_count = 0;

  RobotStateDTO get_current_state() override {
    ++get_state_count;
    return current_state;
  }

  ValidationResultDTO validate_trajectory(
      const TrajectoryDTO& /*trajectory*/,
      const ValidationOptionsDTO& /*options*/) override {
    ++validate_count;
    return next_validation;
  }

  CollisionCheckResultDTO collision_check(
      const CollisionCheckRequestDTO& /*request*/) override {
    ++collision_count;
    return next_collision;
  }

  Pose3DTO forward_kinematics(const FkRequestDTO& /*request*/) override {
    return next_fk;
  }

  JointLimitsDTO get_joint_limits(const std::string& /*robot_id*/,
                                   const std::string& /*group_id*/) override {
    return next_limits;
  }

  SceneEditResultDTO add_object(const AddObjectRequestDTO& /*request*/) override {
    return next_scene_edit;
  }

  SceneEditResultDTO remove_object(const RemoveObjectRequestDTO& /*request*/) override {
    return next_scene_edit;
  }

  SceneEditResultDTO update_object_pose(
      const UpdateObjectPoseRequestDTO& /*request*/) override {
    return next_scene_edit;
  }
};

class MockExecution : public IExecutionBackend {
 public:
  int execute_count = 0;
  int stop_count = 0;
  int pause_count = 0;
  int resume_count = 0;
  int reset_count = 0;
  TrajectoryDTO last_trajectory;

  ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
    ++execute_count;
    last_trajectory = trajectory;
    return ExecutionHandle{};
  }

  StopResultDTO stop() override {
    ++stop_count;
    return StopResultDTO{true, std::nullopt};
  }

  PauseResultDTO pause() override {
    ++pause_count;
    return PauseResultDTO{true, std::nullopt};
  }

  ExecutionHandle resume(const TrajectoryDTO& trajectory) override {
    ++resume_count;
    last_trajectory = trajectory;
    return ExecutionHandle{};
  }

  ResetResultDTO reset() override {
    ++reset_count;
    return ResetResultDTO{true, std::nullopt};
  }
};

// --- Helper ---

TrajectoryDTO make_test_trajectory() {
  TrajectoryDTO t;
  t.joint_names = {"j1", "j2"};
  TrajectoryWaypointDTO wp0;
  wp0.time_from_start_s = 0.0;
  wp0.positions_rad = {0.0, 0.0};
  TrajectoryWaypointDTO wp1;
  wp1.time_from_start_s = 1.0;
  wp1.positions_rad = {1.0, 1.0};
  t.waypoints = {wp0, wp1};
  return t;
}

ExecuteRequestDTO make_test_request(const std::string& move_id = "move_1") {
  ExecuteRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = move_id;
  req.base_frame = "world";
  req.tcp_frame = "tool0";
  req.tolerances.joint_position_tol_rad = 0.1;
  req.speed_profile.velocity_scale = 1.0;
  req.timeouts.planning_timeout_s = 10.0;
  req.cache_mode = CacheMode::kDefault;
  req.motion = JointGoalDTO{{"j1", "j2"}, {1.0, 1.0}};
  return req;
}

// --- Test fixture ---

class ExecutionKernelTest : public ::testing::Test {
 protected:
  FakeClock clock;
  ManualScheduler scheduler;
  MockPlanner planner;
  MockContext context;
  MockExecution execution;
  TrajectoryCache cache{CacheConfig{10}};
  ExecutionKernel kernel{planner, context, execution, clock, scheduler, cache};

  void SetUp() override {
    // Default: planner returns a valid trajectory.
    PlanResultDTO plan_ok;
    plan_ok.trajectory = make_test_trajectory();
    planner.next_result = plan_ok;

    // Default: validation passes.
    context.next_validation = ValidationResultDTO{true, std::nullopt};

    // Default: no collision.
    context.next_collision = CollisionCheckResultDTO{false, std::nullopt, std::nullopt};

    // Default: current state matches trajectory start.
    RobotStateDTO state;
    state.joint_names = {"j1", "j2"};
    state.positions_rad = {0.0, 0.0};
    context.current_state = state;
  }
};

// --- Happy path ---

TEST_F(ExecutionKernelTest, Execute_HappyPath_ReturnsExecutionId) {
  auto result = kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 1u);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_CallsPlanner) {
  kernel.execute(make_test_request());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(planner.last_request.robot_id, "robot1");
  EXPECT_EQ(planner.last_request.move_id, "move_1");
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_ValidatesTrajectory) {
  kernel.execute(make_test_request());
  EXPECT_GE(context.validate_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_DispatchesToBackend) {
  kernel.execute(make_test_request());
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_EQ(execution.last_trajectory.joint_names.size(), 2u);
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_CachesTrajectory) {
  kernel.execute(make_test_request("move_A"));
  auto cached = cache.lookup("move_A");
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(cached->waypoints.size(), 2u);
}

TEST_F(ExecutionKernelTest, Execute_IncrementingIds) {
  auto r1 = kernel.execute(make_test_request());
  EXPECT_EQ(r1.value(), 1u);
  kernel.reset();
  auto r2 = kernel.execute(make_test_request());
  EXPECT_EQ(r2.value(), 2u);
}

// --- Cache hit path ---

TEST_F(ExecutionKernelTest, Execute_CacheHit_SkipsPlanning) {
  cache.store("move_cached", make_test_trajectory());
  auto result = kernel.execute(make_test_request("move_cached"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 0);  // no planning needed
  EXPECT_GE(context.validate_count, 1);  // still validates
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_CacheDisabled_IgnoresCache) {
  cache.store("move_no_cache", make_test_trajectory());
  auto req = make_test_request("move_no_cache");
  req.cache_mode = CacheMode::kDisable;
  auto result = kernel.execute(req);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);  // plans even though cached
}

TEST_F(ExecutionKernelTest, Execute_CacheDisabled_DoesNotStore) {
  auto req = make_test_request("move_no_store");
  req.cache_mode = CacheMode::kDisable;
  kernel.execute(req);
  EXPECT_FALSE(cache.lookup("move_no_store").has_value());
}

// --- Planning failure ---

TEST_F(ExecutionKernelTest, Execute_PlanningFails_TransitionsToFailed) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;

  auto result = kernel.execute(make_test_request());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);  // never dispatched
}

// --- Validation failure ---

TEST_F(ExecutionKernelTest, Execute_ValidationFails_TransitionsToFailed) {
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Collision in path", {}}};

  auto result = kernel.execute(make_test_request());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_ValidationFails_InvalidatesCache) {
  cache.store("move_val_fail", make_test_trajectory());
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

  kernel.execute(make_test_request("move_val_fail"));
  EXPECT_FALSE(cache.lookup("move_val_fail").has_value());
}

// --- Start-state mismatch ---

TEST_F(ExecutionKernelTest, Execute_StartStateMismatch_Fails) {
  RobotStateDTO bad_state;
  bad_state.joint_names = {"j1", "j2"};
  bad_state.positions_rad = {5.0, 5.0};  // far from trajectory start
  context.current_state = bad_state;

  auto result = kernel.execute(make_test_request());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorStartStateMismatch);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
}

// --- Pre-execution collision ---

TEST_F(ExecutionKernelTest, Execute_PreCollisionCheck_Blocks) {
  context.next_collision = CollisionCheckResultDTO{true, "link1", "box"};

  auto result = kernel.execute(make_test_request());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
}

// --- Execute from non-Idle ---

TEST_F(ExecutionKernelTest, Execute_FromNonIdle_Rejected) {
  kernel.execute(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto result = kernel.execute(make_test_request());
  EXPECT_FALSE(result.ok());
}

// --- Stop ---

TEST_F(ExecutionKernelTest, Stop_FromIdle_NotAccepted) {
  auto result = kernel.stop();
  EXPECT_FALSE(result.accepted);
}

TEST_F(ExecutionKernelTest, Stop_FromCompleted_NotAccepted) {
  kernel.execute(make_test_request());
  auto result = kernel.stop();
  EXPECT_FALSE(result.accepted);
}

// --- Pause ---

TEST_F(ExecutionKernelTest, Pause_FromIdle_NotAccepted) {
  auto result = kernel.pause();
  EXPECT_FALSE(result.accepted);
}

// --- Resume ---

TEST_F(ExecutionKernelTest, Resume_FromIdle_NotAccepted) {
  auto result = kernel.resume();
  EXPECT_FALSE(result.accepted);
}

// --- Reset ---

TEST_F(ExecutionKernelTest, Reset_FromCompleted_ToIdle) {
  kernel.execute(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto result = kernel.reset();
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Reset_FromFailed_ToIdle) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;
  kernel.execute(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  auto result = kernel.reset();
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Reset_FromIdle_NotAccepted) {
  auto result = kernel.reset();
  EXPECT_FALSE(result.accepted);
}

TEST_F(ExecutionKernelTest, Reset_ThenExecuteAgain) {
  kernel.execute(make_test_request());
  kernel.reset();
  auto result = kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 2u);
}

// --- Collision monitoring tick ---

TEST_F(ExecutionKernelTest, CollisionCheckTick_NotExecuting_NoOp) {
  auto result = kernel.collision_check_tick();
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(context.collision_count, 0);
}

// --- Full cycle test ---

TEST_F(ExecutionKernelTest, FullCycle_ExecuteResetExecute) {
  auto r1 = kernel.execute(make_test_request("move_A"));
  EXPECT_TRUE(r1.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  kernel.reset();
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);

  auto r2 = kernel.execute(make_test_request("move_B"));
  EXPECT_TRUE(r2.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(planner.plan_count, 2);
}

TEST_F(ExecutionKernelTest, FullCycle_PlanFailResetRetry) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;

  kernel.execute(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  kernel.reset();

  // Fix the planner.
  PlanResultDTO plan_ok;
  plan_ok.trajectory = make_test_trajectory();
  planner.next_result = plan_ok;

  auto result = kernel.execute(make_test_request());
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
}

// --- Status queries ---

TEST_F(ExecutionKernelTest, GetStatus_InitialState) {
  auto status = kernel.get_status();
  EXPECT_EQ(status.state, ExecutionState::kIdle);
  EXPECT_EQ(status.execution_id, 0u);
  EXPECT_EQ(status.progress, 0.0);
}

TEST_F(ExecutionKernelTest, GetStatus_AfterExecute_HasExecutionId) {
  kernel.execute(make_test_request());
  auto status = kernel.get_status();
  EXPECT_EQ(status.execution_id, 1u);
  EXPECT_EQ(status.progress, 1.0);
}

}  // namespace
}  // namespace manyforge::core
