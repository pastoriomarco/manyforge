// Tests for the streaming-session execution mode (ADR-0016).
//
// These tests target the kernel-side contract (begin/push/end lifecycle,
// state transitions, event emission, session-id validation) using a small
// streaming-capable mock backend. Backend-specific behavior
// (RoboPlanExecutionBackend rate limiting, Ros2ControlExecutionBackend
// anchored FJT goal construction) is tested in the respective backend
// test suites where the ROS / scene infrastructure can be exercised.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <vector>

#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/cache/trajectory_cache.hpp>

namespace manyforge::core {
namespace {

// ---- Minimal mocks shared with the existing test file's patterns ----

class FakeClock : public IClock {
 public:
  TimePoint now_value_ns = 0;
  TimePoint now() const override { return now_value_ns; }
};

class ManualScheduler : public IScheduler {
 public:
  ScheduleHandle schedule(TimePoint /*deadline*/,
                          std::function<void()> /*callback*/) override {
    return ScheduleHandle{0};
  }
  void cancel(ScheduleHandle /*handle*/) override {}
};

class StubPlanner : public IPlannerBackend {
 public:
  PlanResultDTO plan(const PlanRequestDTO& /*request*/) override {
    PlanResultDTO out;
    return out;
  }

  // ADR-0016: streaming IK pre-resolution. Records the request for
  // assertions; returns a deterministic joint vector for testing.
  int solve_ik_calls = 0;
  IkRequestDTO last_ik_request;
  std::vector<double> next_ik_positions = {0.1, 0.2};
  std::vector<std::string> next_ik_joint_names = {"j1", "j2"};
  bool reject_ik = false;

  IkResultDTO solve_ik(const IkRequestDTO& request) override {
    ++solve_ik_calls;
    last_ik_request = request;
    IkResultDTO result;
    if (reject_ik) {
      result.ok = false;
      result.error = ErrorDTO{kErrorIkFailed, "test-ik-reject", {}};
      return result;
    }
    result.ok = true;
    result.joint_names = next_ik_joint_names;
    result.positions_rad = next_ik_positions;
    return result;
  }
};

// Stub IPlanningContext — streaming-session paths don't actually call into
// planning context, so every method returns a benign default. The joint-
// state knobs (current_joint_names / current_positions_rad) let tests drive
// the kernel's partial-joint seeding path (spec/200 §2): the kernel calls
// get_current_state() on the first push of a session to seed
// last_commanded_joints_, and rejects with `stale_state` if the snapshot
// doesn't cover every session joint.
class StubContext : public IPlanningContext {
 public:
  std::vector<std::string> current_joint_names = {"j1", "j2"};
  std::vector<double> current_positions_rad = {0.0, 0.0};

  RobotStateDTO get_current_state() override {
    RobotStateDTO state;
    state.joint_names = current_joint_names;
    state.positions_rad = current_positions_rad;
    return state;
  }
  ValidationResultDTO validate_trajectory(
      const TrajectoryDTO& /*trajectory*/,
      const ValidationOptionsDTO& /*options*/) override {
    return ValidationResultDTO{true, std::nullopt};
  }
  CollisionCheckResultDTO collision_check(
      const CollisionCheckRequestDTO& /*request*/) override {
    return CollisionCheckResultDTO{false, std::nullopt, std::nullopt};
  }
  Pose3DTO forward_kinematics(const FkRequestDTO& /*request*/) override {
    return Pose3DTO{};
  }
  Pose3DTO get_object_pose(const std::string& /*object_id*/,
                           const std::string& /*base_frame*/) override {
    return Pose3DTO{};
  }
  ObjectStateDTO get_object_state(const std::string& /*object_id*/) override {
    return ObjectStateDTO{};
  }
  Result<Pose3DTO> resolve_reference_pose(
      const ReferenceDTO& /*reference*/,
      const std::string& /*base_frame*/) override {
    return Result<Pose3DTO>::Err(ErrorCode::kMissingReference);
  }
  JointLimitsDTO get_joint_limits(const std::string& /*robot_id*/,
                                   const std::string& /*group_id*/) override {
    return JointLimitsDTO{};
  }
  SceneEditResultDTO add_object(
      const AddObjectRequestDTO& /*request*/) override {
    return SceneEditResultDTO{};
  }
  SceneEditResultDTO remove_object(
      const RemoveObjectRequestDTO& /*request*/) override {
    return SceneEditResultDTO{};
  }
  SceneEditResultDTO update_object_pose(
      const UpdateObjectPoseRequestDTO& /*request*/) override {
    return SceneEditResultDTO{};
  }
  SceneEditResultDTO attach_object(
      const AttachObjectRequestDTO& /*request*/) override {
    return SceneEditResultDTO{};
  }
  SceneEditResultDTO detach_object(
      const DetachObjectRequestDTO& /*request*/) override {
    return SceneEditResultDTO{};
  }
};

// Streaming-capable mock backend. Records begin/push/end calls and exposes
// the recorded targets for assertion. Discrete-trajectory ops are stubbed
// as no-ops (this fixture is streaming-focused).
class StreamingMock : public IExecutionBackend {
 public:
  std::atomic<int> begin_calls{0};
  std::atomic<int> push_calls{0};
  std::atomic<int> end_calls{0};

  uint64_t next_session_id = 1;
  uint64_t active_session_id = 0;
  StreamingSessionConfigDTO last_config;
  std::vector<StreamingTargetDTO> received_targets;
  SoftStopStrategy last_end_strategy = SoftStopStrategy::kSpringBack;

  // Test hooks
  bool reject_begin = false;
  bool reject_push = false;
  bool reject_end = false;

  ExecutionHandle execute_trajectory(
      const TrajectoryDTO& /*trajectory*/) override {
    return ExecutionHandle{[](int64_t) { return true; },
                           [this]() { return snapshot_; }};
  }
  Result<void> sync_observed_state_before_planning() override {
    return Result<void>::Ok();
  }
  StopResultDTO stop() override { return StopResultDTO{true, std::nullopt}; }
  PauseResultDTO pause() override {
    return PauseResultDTO{true, std::nullopt};
  }
  ExecutionHandle resume(const TrajectoryDTO& /*trajectory*/) override {
    return ExecutionHandle{[](int64_t) { return true; },
                           [this]() { return snapshot_; }};
  }
  ResetResultDTO reset() override {
    return ResetResultDTO{true, std::nullopt};
  }
  ExecutionSnapshot get_execution_snapshot() const override {
    return snapshot_;
  }

  StreamingSessionBeginResultDTO begin_streaming_session(
      const StreamingSessionConfigDTO& config) override {
    ++begin_calls;
    if (reject_begin) {
      return {false, 0,
              ErrorDTO{kErrorInvalidRequest, "test-reject", {}}};
    }
    last_config = config;
    active_session_id = next_session_id++;
    return {true, active_session_id, std::nullopt};
  }
  StreamingPushResultDTO push_streaming_target(
      uint64_t session_id,
      const StreamingTargetDTO& target) override {
    ++push_calls;
    if (reject_push) {
      return {false, 0,
              ErrorDTO{kErrorInvalidRequest, "test-push-reject", {}}};
    }
    if (session_id != active_session_id) {
      return {false, 0,
              ErrorDTO{kErrorInvalidRequest, "wrong session_id", {}}};
    }
    received_targets.push_back(target);
    return {true, static_cast<uint64_t>(received_targets.size()),
            std::nullopt};
  }
  StreamingSessionEndResultDTO end_streaming_session(
      uint64_t session_id,
      SoftStopStrategy strategy) override {
    ++end_calls;
    if (reject_end) {
      return {false, ErrorDTO{kErrorStopFailed, "test-end-reject", {}}};
    }
    if (session_id != active_session_id) {
      return {false, ErrorDTO{kErrorInvalidRequest, "wrong session_id", {}}};
    }
    last_end_strategy = strategy;
    active_session_id = 0;
    return {true, std::nullopt};
  }

 private:
  ExecutionSnapshot snapshot_{};
};

// ---- Test fixture ----

class StreamingSessionTest : public ::testing::Test {
 protected:
  FakeClock clock;
  ManualScheduler scheduler;
  StubPlanner planner;
  StubContext context;
  StreamingMock backend;
  TrajectoryCache cache{CacheConfig{4}};
  ExecutionKernel kernel{planner, context, backend, clock, scheduler, cache};

  std::vector<KernelEvent> events;
  std::mutex events_mutex;
  SubscriptionHandle subscription{0};

  void SetUp() override {
    subscription = kernel.subscribe_events([this](const KernelEvent& ev) {
      std::lock_guard<std::mutex> lock(events_mutex);
      events.push_back(ev);
    });
  }
  void TearDown() override {
    kernel.unsubscribe_events(subscription);
  }

  std::vector<KernelEvent> snapshot_events() {
    std::lock_guard<std::mutex> lock(events_mutex);
    return events;
  }

  size_t count_events_of(KernelEventType type) {
    size_t count = 0;
    for (const auto& ev : snapshot_events()) {
      if (ev.type == type) ++count;
    }
    return count;
  }

  StreamingSessionConfigDTO default_config() {
    StreamingSessionConfigDTO cfg;
    cfg.group_id = "arm";
    cfg.joint_names = {"j1", "j2"};
    cfg.principal_id = "test_operator";
    cfg.default_input_mode = StreamingInputMode::kJoint;
    return cfg;
  }

  StreamingTargetDTO joint_target(std::initializer_list<double> q) {
    StreamingTargetDTO t;
    t.kind = StreamingInputMode::kJoint;
    t.joint_names = {"j1", "j2"};
    t.joint_positions_rad.assign(q.begin(), q.end());
    return t;
  }

  StreamingTargetDTO cartesian_target(
      std::array<double, 3> pos,
      std::array<double, 4> quat = {0.0, 0.0, 0.0, 1.0}) {
    StreamingTargetDTO t;
    t.kind = StreamingInputMode::kCartesian;
    t.position_m = pos;
    t.orientation_xyzw = quat;
    return t;
  }

  StreamingSessionConfigDTO cartesian_config() {
    auto cfg = default_config();
    cfg.default_input_mode = StreamingInputMode::kCartesian;
    cfg.tcp_frame = "tcp";
    cfg.cartesian_reference_frame = "base";
    return cfg;
  }
};

// ---- begin_streaming_session ----

TEST_F(StreamingSessionTest, Begin_FromIdle_AcceptedAndTransitionsToStreaming) {
  ASSERT_EQ(kernel.get_status().state, ExecutionState::kIdle);

  auto r = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(r.accepted) << "begin should accept from kIdle";
  EXPECT_NE(r.session_id, 0u);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kStreaming);
  EXPECT_EQ(backend.begin_calls, 1);
}

TEST_F(StreamingSessionTest, Begin_EmitsSessionStartedAndStateTransitionEvents) {
  auto r = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(r.accepted);
  EXPECT_GE(count_events_of(KernelEventType::kStreamingSessionStarted), 1u);
  EXPECT_GE(count_events_of(KernelEventType::kStateTransition), 1u);
}

TEST_F(StreamingSessionTest, Begin_SecondCallFromStreaming_Rejected) {
  ASSERT_TRUE(kernel.begin_streaming_session(default_config()).accepted);
  auto r2 = kernel.begin_streaming_session(default_config());
  EXPECT_FALSE(r2.accepted) << "second begin should reject";
  EXPECT_TRUE(r2.error.has_value());
}

TEST_F(StreamingSessionTest, Begin_BackendRejection_LeavesKernelInIdle) {
  backend.reject_begin = true;
  auto r = kernel.begin_streaming_session(default_config());
  EXPECT_FALSE(r.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

// ---- push_streaming_target ----

TEST_F(StreamingSessionTest, Push_HappyPath_AcceptsTargetAndEmitsEvent) {
  auto begin_result = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin_result.accepted);

  auto push_result = kernel.push_streaming_target(
      begin_result.session_id, joint_target({0.1, 0.2}));
  EXPECT_TRUE(push_result.accepted);
  EXPECT_EQ(backend.push_calls, 1);
  EXPECT_GE(count_events_of(KernelEventType::kStreamingTargetReceived), 1u);
  ASSERT_EQ(backend.received_targets.size(), 1u);
  EXPECT_EQ(backend.received_targets[0].joint_positions_rad,
            (std::vector<double>{0.1, 0.2}));
}

TEST_F(StreamingSessionTest, Push_WithMismatchedSessionId_Rejected) {
  auto begin_result = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin_result.accepted);

  const uint64_t wrong_id = begin_result.session_id + 42;
  auto push_result =
      kernel.push_streaming_target(wrong_id, joint_target({0.1, 0.2}));
  EXPECT_FALSE(push_result.accepted);
  EXPECT_EQ(backend.push_calls, 0)
      << "kernel must not forward to backend on session_id mismatch";
}

TEST_F(StreamingSessionTest, Push_FromIdle_Rejected) {
  auto push_result = kernel.push_streaming_target(1, joint_target({0.1, 0.2}));
  EXPECT_FALSE(push_result.accepted);
  EXPECT_EQ(backend.push_calls, 0);
}

TEST_F(StreamingSessionTest, Push_BackendRejection_EmitsRejectionEvent) {
  auto begin_result = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin_result.accepted);
  backend.reject_push = true;

  auto push_result = kernel.push_streaming_target(
      begin_result.session_id, joint_target({0.1, 0.2}));
  EXPECT_FALSE(push_result.accepted);
  EXPECT_GE(count_events_of(KernelEventType::kStreamingTargetRejected), 1u);
}

// ---- end_streaming_session ----

TEST_F(StreamingSessionTest, End_HappyPath_AutoAckReturnsToIdle) {
  auto begin_result = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin_result.accepted);

  auto end_result = kernel.end_streaming_session(
      begin_result.session_id, SoftStopStrategy::kSpringBack);
  EXPECT_TRUE(end_result.accepted);
  // ADR-0016: streaming end auto-returns to kIdle (operator mouseUp IS
  // the acknowledgement; no separate reset step). Distinct from
  // discrete trajectories which require acknowledge_completed.
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(backend.end_calls, 1);
  EXPECT_EQ(backend.last_end_strategy, SoftStopStrategy::kSpringBack);
}

TEST_F(StreamingSessionTest, End_EmitsSessionEndedEvent) {
  auto begin_result = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin_result.accepted);
  ASSERT_TRUE(kernel.end_streaming_session(begin_result.session_id,
                                           SoftStopStrategy::kSpringBack)
                  .accepted);
  EXPECT_GE(count_events_of(KernelEventType::kStreamingSessionEnded), 1u);
}

TEST_F(StreamingSessionTest, End_WithMismatchedSessionId_Rejected) {
  auto begin_result = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin_result.accepted);

  const uint64_t wrong_id = begin_result.session_id + 99;
  auto end_result =
      kernel.end_streaming_session(wrong_id, SoftStopStrategy::kSpringBack);
  EXPECT_FALSE(end_result.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kStreaming)
      << "rejected end must NOT mutate kernel state";
  EXPECT_EQ(backend.end_calls, 0);
}

TEST_F(StreamingSessionTest, End_FromIdle_Rejected) {
  auto end_result =
      kernel.end_streaming_session(1, SoftStopStrategy::kSpringBack);
  EXPECT_FALSE(end_result.accepted);
}

TEST_F(StreamingSessionTest, End_BackendFailure_TransitionsToInterventionRequired) {
  auto begin_result = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin_result.accepted);
  backend.reject_end = true;

  auto end_result = kernel.end_streaming_session(
      begin_result.session_id, SoftStopStrategy::kSpringBack);
  EXPECT_FALSE(end_result.accepted);
  EXPECT_EQ(kernel.get_status().state,
            ExecutionState::kInterventionRequired);
}

// ---- Lifecycle integration ----

TEST_F(StreamingSessionTest, BeginPushManyTargetsThenEnd_TerminatesCleanly) {
  auto begin_result = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin_result.accepted);

  // Push a burst of targets — should be accepted while in kStreaming.
  for (int i = 0; i < 30; ++i) {
    const double q1 = 0.01 * i;
    auto r = kernel.push_streaming_target(begin_result.session_id,
                                          joint_target({q1, q1 * 2}));
    ASSERT_TRUE(r.accepted) << "push #" << i << " unexpectedly rejected";
  }
  EXPECT_EQ(backend.push_calls, 30);

  auto end_result = kernel.end_streaming_session(
      begin_result.session_id, SoftStopStrategy::kSpringBack);
  EXPECT_TRUE(end_result.accepted);
  // Auto-ack to kIdle (ADR-0016).
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

// ---- ADR-0016 Cartesian IK pre-resolution ----

TEST_F(StreamingSessionTest,
       CartesianTarget_ResolvesViaPlannerIk_ForwardedAsJointToBackend) {
  ASSERT_TRUE(kernel.begin_streaming_session(cartesian_config()).accepted);

  // Set the IK to a known joint vector and push a Cartesian target.
  planner.next_ik_positions = {0.5, -0.3};
  planner.next_ik_joint_names = {"j1", "j2"};

  auto push_result = kernel.push_streaming_target(
      kernel.get_status().execution_id != 0 ? 0 : 1,  // session_id is 1 here
      cartesian_target({0.4, 0.1, 0.6}));
  // The first session opens with id=1 (kernel counter starts at 1).
  // We re-issue with the correct id if the first call rejected on id mismatch.
  if (!push_result.accepted) {
    push_result = kernel.push_streaming_target(
        1, cartesian_target({0.4, 0.1, 0.6}));
  }
  EXPECT_TRUE(push_result.accepted);
  EXPECT_EQ(planner.solve_ik_calls, 1)
      << "kernel must call planner.solve_ik for cartesian targets";

  // Verify the backend received a JOINT target (resolved from IK), not the
  // raw Cartesian target.
  ASSERT_EQ(backend.received_targets.size(), 1u);
  EXPECT_EQ(backend.received_targets[0].kind, StreamingInputMode::kJoint);
  EXPECT_EQ(backend.received_targets[0].joint_positions_rad,
            (std::vector<double>{0.5, -0.3}));
}

TEST_F(StreamingSessionTest, CartesianTarget_IkRejection_EmitsRejectionEvent) {
  ASSERT_TRUE(kernel.begin_streaming_session(cartesian_config()).accepted);
  planner.reject_ik = true;

  auto push_result =
      kernel.push_streaming_target(1, cartesian_target({0.4, 0.1, 0.6}));
  EXPECT_FALSE(push_result.accepted);
  EXPECT_EQ(planner.solve_ik_calls, 1);
  EXPECT_EQ(backend.push_calls, 0) << "IK rejection must not reach backend";
  EXPECT_GE(count_events_of(KernelEventType::kStreamingTargetRejected), 1u);
}

TEST_F(StreamingSessionTest, CartesianTarget_MissingTcpFrame_Rejected) {
  auto cfg = default_config();
  cfg.default_input_mode = StreamingInputMode::kCartesian;
  cfg.tcp_frame.clear();  // explicit miss
  ASSERT_TRUE(kernel.begin_streaming_session(cfg).accepted);

  auto push_result =
      kernel.push_streaming_target(1, cartesian_target({0.4, 0.1, 0.6}));
  EXPECT_FALSE(push_result.accepted);
  EXPECT_EQ(planner.solve_ik_calls, 0)
      << "missing tcp_frame must reject before planner call";
}

TEST_F(StreamingSessionTest, JointTarget_DoesNotInvokeIk) {
  ASSERT_TRUE(kernel.begin_streaming_session(default_config()).accepted);

  auto push_result =
      kernel.push_streaming_target(1, joint_target({0.1, 0.2}));
  EXPECT_TRUE(push_result.accepted);
  EXPECT_EQ(planner.solve_ik_calls, 0)
      << "joint targets must bypass the IK path";
}

TEST_F(StreamingSessionTest, SecondSessionAfterEnd_AllowedWithoutReset) {
  auto first = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(first.accepted);
  ASSERT_TRUE(kernel.end_streaming_session(
                       first.session_id, SoftStopStrategy::kSpringBack)
                  .accepted);
  // ADR-0016: streaming end auto-returns kernel to kIdle. Operator
  // (or upstream caller) can begin a new session immediately without
  // needing an explicit reset click — this is the symptom that
  // surfaced as "gizmo stopped working after first drag".
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);

  auto second = kernel.begin_streaming_session(default_config());
  EXPECT_TRUE(second.accepted)
      << "successive streaming sessions must be admitted without an "
         "intervening reset (regression against post-end kStopped lockout)";
  EXPECT_NE(second.session_id, first.session_id)
      << "kernel must assign distinct session ids";
}

// ---- Stop/Reset lifecycle from kStreaming (review-fix regression) ----

TEST_F(StreamingSessionTest, Stop_FromStreaming_DrivesSoftEndAndAutoAcksToIdle) {
  auto config = default_config();
  config.default_soft_stop_strategy = SoftStopStrategy::kSpringBack;
  ASSERT_TRUE(kernel.begin_streaming_session(config).accepted);

  auto stop_result = kernel.stop();
  EXPECT_TRUE(stop_result.accepted)
      << "stop() while kStreaming must succeed via soft-end path";
  // stop() routes to end_streaming_session which auto-acknowledges to
  // kIdle (ADR-0016): operator stop button on a streaming session ≡
  // mouseUp + ack.
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(backend.end_calls, 1)
      << "stop() must drive backend.end_streaming_session";
  EXPECT_EQ(backend.last_end_strategy, SoftStopStrategy::kSpringBack);
}

TEST_F(StreamingSessionTest,
       Reset_FromStreaming_ClearsSessionAndAllowsImmediateRebegin) {
  ASSERT_TRUE(kernel.begin_streaming_session(default_config()).accepted);

  auto reset_result = kernel.reset();
  EXPECT_TRUE(reset_result.accepted);

  // The fix under test: active_streaming_session_id_ must be cleared by
  // reset, so a subsequent begin is admitted from kIdle.
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  auto rebegin = kernel.begin_streaming_session(default_config());
  EXPECT_TRUE(rebegin.accepted)
      << "begin_streaming_session must succeed after reset cleared "
         "the previous session — review-fix regression check";
}

TEST_F(StreamingSessionTest,
       Push_AfterBeginUsesBackendSessionId_NotKernelId) {
  // Ensure the backend uses its OWN session_id counter so a mismatch
  // would be visible. The mock's next_session_id increments from 1,
  // matching the kernel's. To force divergence, do a fake "first begin
  // then reset" to bump the kernel counter past 1, then begin again.
  ASSERT_TRUE(kernel.begin_streaming_session(default_config()).accepted);
  ASSERT_TRUE(kernel.reset().accepted);
  // Kernel counter is now 2; backend counter advanced too via its own
  // increment. They still match in this stub, but the kernel now stores
  // backend_session_id explicitly and uses it for push/end, so any
  // future drift won't break the chain.
  auto second = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(second.accepted);

  auto push = kernel.push_streaming_target(
      second.session_id, joint_target({0.1, 0.2}));
  EXPECT_TRUE(push.accepted);
  EXPECT_EQ(backend.received_targets.size(), 1u);
}

// ---- ADR-0015 + ADR-0016: Cartesian speed cap ----
//
// The cap applies to the ROBOT's commanded motion, NOT to the operator's
// input rate. Fast gizmo drags are accepted; the kernel populates
// `target.min_time_from_start_s = distance / max_speed_mps` so the
// backend schedules the FJT goal long enough for the cap to hold.

TEST_F(StreamingSessionTest,
       CartesianTarget_FarApartWithCap_AcceptedWithSpeedFloor) {
  auto cfg = cartesian_config();
  cfg.max_tcp_linear_speed_mps = 0.1;  // 100 mm/s
  cfg.target_time_from_start_min_s = 0.030;
  ASSERT_TRUE(kernel.begin_streaming_session(cfg).accepted);

  ASSERT_TRUE(kernel.push_streaming_target(
                       1, cartesian_target({0.5, 0.0, 0.5}))
                  .accepted);

  // Far-apart second push (Δy = 1.0 m). Must be ACCEPTED, but with a
  // min_time_from_start_s ≥ 1.0 / 0.1 = 10 s so the backend schedules
  // the FJT goal long enough for the cap to hold.
  auto second = kernel.push_streaming_target(
      1, cartesian_target({0.5, 1.0, 0.5}));
  EXPECT_TRUE(second.accepted)
      << "the kernel must NEVER reject cartesian targets based on the "
         "operator's input rate; the cap applies to the robot's "
         "commanded motion via min_time_from_start_s, not to inputs";
  ASSERT_EQ(backend.received_targets.size(), 2u);
  // Second target reached the backend with the computed floor.
  EXPECT_GE(backend.received_targets[1].min_time_from_start_s, 10.0 - 1e-6);
}

TEST_F(StreamingSessionTest,
       CartesianTarget_CloseTogether_NoSpeedFloorRequired) {
  auto cfg = cartesian_config();
  cfg.max_tcp_linear_speed_mps = 0.1;  // 100 mm/s
  ASSERT_TRUE(kernel.begin_streaming_session(cfg).accepted);

  ASSERT_TRUE(kernel.push_streaming_target(
                       1, cartesian_target({0.5, 0.0, 0.5}))
                  .accepted);
  // Δ = 1 mm => min_time_from_start_s = 0.001/0.1 = 0.01 s, below
  // backend's own target_time_from_start_min_s floor.
  auto second = kernel.push_streaming_target(
      1, cartesian_target({0.501, 0.0, 0.5}));
  EXPECT_TRUE(second.accepted);
  ASSERT_EQ(backend.received_targets.size(), 2u);
  EXPECT_NEAR(backend.received_targets[1].min_time_from_start_s, 0.01, 1e-6);
}

TEST_F(StreamingSessionTest,
       CartesianTarget_NoCapConfigured_LeavesFloorUnset) {
  auto cfg = cartesian_config();
  cfg.max_tcp_linear_speed_mps = 0.0;  // "unlimited"
  ASSERT_TRUE(kernel.begin_streaming_session(cfg).accepted);

  ASSERT_TRUE(kernel.push_streaming_target(
                       1, cartesian_target({0.5, 0.0, 0.5}))
                  .accepted);
  auto second = kernel.push_streaming_target(
      1, cartesian_target({2.0, 2.0, 2.0}));
  EXPECT_TRUE(second.accepted);
  ASSERT_EQ(backend.received_targets.size(), 2u);
  EXPECT_EQ(backend.received_targets[1].min_time_from_start_s, 0.0)
      << "with no cap configured, the kernel must not populate the "
         "time-from-start floor";
}

TEST_F(StreamingSessionTest, Push_AcceptEmitsDispatchedEvent) {
  auto begin = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin.accepted);
  ASSERT_TRUE(
      kernel
          .push_streaming_target(begin.session_id, joint_target({0.1, 0.2}))
          .accepted);
  EXPECT_GE(count_events_of(KernelEventType::kStreamingTargetDispatched), 1u)
      << "kStreamingTargetDispatched must be emitted after a successful "
         "push so the failure archive / event stream can correlate "
         "operator frames with controller goal submissions";
}

// ---- Partial joint-map normalization (spec/200 §2) ----

TEST_F(StreamingSessionTest, PartialJoint_OverlaysOntoSeededFullVector) {
  // Seed StubContext with non-zero current positions so the seed step is
  // visibly carried into the backend frame.
  context.current_positions_rad = {1.5, -0.7};
  auto begin = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin.accepted);

  // Operator only touches j1; j2 should retain the seeded current value.
  StreamingTargetDTO partial;
  partial.kind = StreamingInputMode::kJoint;
  partial.joint_names = {"j1"};
  partial.joint_positions_rad = {0.5};
  auto r = kernel.push_streaming_target(begin.session_id, partial);

  EXPECT_TRUE(r.accepted);
  ASSERT_EQ(backend.received_targets.size(), 1u);
  const auto& got = backend.received_targets.back();
  ASSERT_EQ(got.kind, StreamingInputMode::kJoint);
  ASSERT_EQ(got.joint_names, (std::vector<std::string>{"j1", "j2"}));
  ASSERT_EQ(got.joint_positions_rad.size(), 2u);
  EXPECT_DOUBLE_EQ(got.joint_positions_rad[0], 0.5);
  EXPECT_DOUBLE_EQ(got.joint_positions_rad[1], -0.7);
}

TEST_F(StreamingSessionTest,
       PartialJoint_SecondPushOverlaysOntoLastCommandedJoints) {
  context.current_positions_rad = {0.0, 0.0};
  auto begin = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin.accepted);

  // First push moves j1 to 0.5; backend sees {0.5, 0.0}.
  StreamingTargetDTO partial1;
  partial1.kind = StreamingInputMode::kJoint;
  partial1.joint_names = {"j1"};
  partial1.joint_positions_rad = {0.5};
  ASSERT_TRUE(kernel.push_streaming_target(begin.session_id, partial1).accepted);

  // Second push moves j2 to -1.2; backend MUST see {0.5, -1.2}, i.e.
  // the overlay onto the previously-commanded full vector (last_commanded_joints_),
  // not a fresh re-seed from context.get_current_state() which still
  // returns {0.0, 0.0}.
  StreamingTargetDTO partial2;
  partial2.kind = StreamingInputMode::kJoint;
  partial2.joint_names = {"j2"};
  partial2.joint_positions_rad = {-1.2};
  ASSERT_TRUE(kernel.push_streaming_target(begin.session_id, partial2).accepted);

  ASSERT_EQ(backend.received_targets.size(), 2u);
  const auto& got2 = backend.received_targets.back();
  ASSERT_EQ(got2.joint_positions_rad.size(), 2u);
  EXPECT_DOUBLE_EQ(got2.joint_positions_rad[0], 0.5);
  EXPECT_DOUBLE_EQ(got2.joint_positions_rad[1], -1.2);
}

TEST_F(StreamingSessionTest, UnknownJointName_RejectsWithUnknownJointNameCode) {
  auto begin = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin.accepted);

  StreamingTargetDTO bad;
  bad.kind = StreamingInputMode::kJoint;
  bad.joint_names = {"jX"};
  bad.joint_positions_rad = {0.5};
  auto r = kernel.push_streaming_target(begin.session_id, bad);

  EXPECT_FALSE(r.accepted);
  ASSERT_TRUE(r.error.has_value());
  EXPECT_EQ(r.error->code, std::string("unknown_joint_name"));
  EXPECT_EQ(backend.received_targets.size(), 0u)
      << "rejected frames must not reach the backend";
}

TEST_F(StreamingSessionTest, JointSizeMismatch_RejectsAsMalformed) {
  auto begin = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin.accepted);

  StreamingTargetDTO bad;
  bad.kind = StreamingInputMode::kJoint;
  bad.joint_names = {"j1", "j2"};
  bad.joint_positions_rad = {0.5};  // 1 != 2
  auto r = kernel.push_streaming_target(begin.session_id, bad);

  EXPECT_FALSE(r.accepted);
  ASSERT_TRUE(r.error.has_value());
  EXPECT_EQ(r.error->code, std::string("invalid_request"));
  // Message text is brittle but is the only structured hook we have
  // until details get plumbed across the binding; the WS layer matches
  // this prefix to surface `malformed_frame` to the operator.
  EXPECT_NE(r.error->message.find("joint_names.size()"), std::string::npos);
  EXPECT_EQ(backend.received_targets.size(), 0u);
}

TEST_F(StreamingSessionTest, DuplicateJointName_RejectsAsMalformed) {
  auto begin = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin.accepted);

  StreamingTargetDTO bad;
  bad.kind = StreamingInputMode::kJoint;
  bad.joint_names = {"j1", "j1"};
  bad.joint_positions_rad = {0.5, 0.6};
  auto r = kernel.push_streaming_target(begin.session_id, bad);

  EXPECT_FALSE(r.accepted);
  ASSERT_TRUE(r.error.has_value());
  EXPECT_EQ(r.error->code, std::string("invalid_request"));
  EXPECT_NE(r.error->message.find("duplicate joint name"), std::string::npos);
  EXPECT_EQ(backend.received_targets.size(), 0u);
}

TEST_F(StreamingSessionTest,
       FirstPushWithIncompleteCurrentState_RejectsAsStaleState) {
  // Snapshot is missing j2 — kernel must NOT silently zero-seed; spec
  // requires rejection with `stale_state` until the snapshot covers
  // every session joint.
  context.current_joint_names = {"j1"};
  context.current_positions_rad = {0.7};
  auto begin = kernel.begin_streaming_session(default_config());
  ASSERT_TRUE(begin.accepted);

  StreamingTargetDTO partial;
  partial.kind = StreamingInputMode::kJoint;
  partial.joint_names = {"j1"};
  partial.joint_positions_rad = {0.5};
  auto r = kernel.push_streaming_target(begin.session_id, partial);

  EXPECT_FALSE(r.accepted);
  ASSERT_TRUE(r.error.has_value());
  EXPECT_EQ(r.error->code, std::string("invalid_request"));
  EXPECT_NE(
      r.error->message.find("current robot state does not cover"),
      std::string::npos);
  EXPECT_EQ(backend.received_targets.size(), 0u);
}

}  // namespace
}  // namespace manyforge::core
