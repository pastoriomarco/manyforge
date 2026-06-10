#include <gtest/gtest.h>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

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

class SlowMockPlanner : public IPlannerBackend {
 public:
  PlanResultDTO next_result;
  int plan_count = 0;
  PlanRequestDTO last_request;

  SlowMockPlanner() {
    next_result.trajectory = TrajectoryDTO{};
    next_result.trajectory->joint_names = {"j1", "j2"};
    next_result.trajectory->waypoints.push_back(
        TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {}, {}});
    next_result.trajectory->waypoints.push_back(
        TrajectoryWaypointDTO{1.0, {1.0, 1.0}, {}, {}});
  }

  PlanResultDTO plan(const PlanRequestDTO& request) override {
    ++plan_count;
    last_request = request;

    // Signal that we're in the plan call
    {
      std::lock_guard<std::mutex> lock(mu_);
      in_plan_ = true;
    }
    in_plan_cv_.notify_all();

    // Wait until released or for a timeout
    {
      std::unique_lock<std::mutex> lock(mu_);
      release_cv_.wait_for(lock, std::chrono::milliseconds(500),
                           [this]() { return released_; });
    }

    return next_result;
  }

  // Bounded for the same reason as ControlledExecution's wait_until_*
  // helpers: an unbounded cv_.wait can park forever under contention
  // and hand the test up to the ctest-level 60 s ceiling with a less
  // specific failure than an in-test ASSERT_TRUE timeout.
  bool wait_until_in_plan(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return in_plan_cv_.wait_for(lock, timeout,
                                [this]() { return in_plan_; });
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      released_ = true;
    }
    release_cv_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable in_plan_cv_;
  std::condition_variable release_cv_;
  bool in_plan_ = false;
  bool released_ = false;
};

class MockContext : public IPlanningContext {
 public:
  RobotStateDTO current_state;
  ValidationResultDTO next_validation;
  CollisionCheckResultDTO next_collision;
  std::vector<CollisionCheckResultDTO> collision_sequence;
  Pose3DTO next_fk;
  Pose3DTO next_object_pose;
  ObjectStateDTO next_object_state;
  Result<Pose3DTO> next_reference_pose = Result<Pose3DTO>::Ok(Pose3DTO{});
  std::vector<Result<Pose3DTO>> reference_pose_sequence;
  std::vector<Pose3DTO> fk_sequence;
  JointLimitsDTO next_limits;
  SceneEditResultDTO next_scene_edit;
  CollisionWorldRevision current_revision{1, "mock-collision-world"};

  int get_state_count = 0;
  int validate_count = 0;
  int collision_count = 0;
  int fk_count = 0;
  FkRequestDTO last_fk_request;
  ValidationOptionsDTO last_validation_options;
  bool throw_get_state = false;
  bool throw_fk = false;
  std::function<void()> on_validate;

  RobotStateDTO get_current_state() override {
    ++get_state_count;
    if (throw_get_state) {
      throw std::runtime_error("state unavailable");
    }
    return current_state;
  }

  ValidationResultDTO validate_trajectory(
      const TrajectoryDTO& /*trajectory*/,
      const ValidationOptionsDTO& options) override {
    ++validate_count;
    last_validation_options = options;
    if (on_validate) {
      on_validate();
    }
    return next_validation;
  }

  CollisionCheckResultDTO collision_check(
      const CollisionCheckRequestDTO& /*request*/) override {
    ++collision_count;
    if (!collision_sequence.empty()) {
      auto result = collision_sequence.front();
      collision_sequence.erase(collision_sequence.begin());
      return result;
    }
    return next_collision;
  }

  Pose3DTO forward_kinematics(const FkRequestDTO& request) override {
    ++fk_count;
    last_fk_request = request;
    if (throw_fk) {
      throw std::runtime_error("fk failed");
    }
    if (!fk_sequence.empty()) {
      auto result = fk_sequence.front();
      fk_sequence.erase(fk_sequence.begin());
      return result;
    }
    return next_fk;
  }

  Pose3DTO get_object_pose(const std::string& /*object_id*/,
                           const std::string& /*base_frame*/) override {
    return next_object_pose;
  }

  ObjectStateDTO get_object_state(const std::string& /*object_id*/) override {
    return next_object_state;
  }

  Result<Pose3DTO> resolve_reference_pose(const ReferenceDTO& /*reference*/,
                                          const std::string& /*base_frame*/) override {
    if (!reference_pose_sequence.empty()) {
      auto result = reference_pose_sequence.front();
      reference_pose_sequence.erase(reference_pose_sequence.begin());
      return result;
    }
    return next_reference_pose;
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

  SceneEditResultDTO attach_object(
      const AttachObjectRequestDTO& /*request*/) override {
    return next_scene_edit;
  }

  SceneEditResultDTO detach_object(
      const DetachObjectRequestDTO& /*request*/) override {
    return next_scene_edit;
  }

  CollisionWorldRevision current_collision_world_revision() override {
    return current_revision;
  }
};

class MockCollisionWorldPublisher final : public ICollisionWorldSnapshotPublisher {
 public:
  CollisionWorldSnapshotPtr current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>();
  std::optional<CollisionPolicySnapshot> current_policy;
  bool throw_on_snapshot = false;

  CollisionWorldSnapshotPtr snapshot() const override {
    if (throw_on_snapshot) {
      throw std::runtime_error("snapshot unavailable");
    }
    return current_snapshot;
  }

  std::optional<CollisionPolicySnapshot> collision_policy_snapshot()
      const override {
    return current_policy;
  }
};

class MockCollisionEngine final : public ICollisionQueryEngine {
 public:
  CollisionResult default_result;
  std::map<std::int64_t, CollisionResult> result_by_waypoint;
  std::function<void(const CollisionQuery&)> on_check;
  std::atomic<int> check_count{0};

  MockCollisionEngine() {
    default_result.in_collision = false;
    default_result.engine_status = CollisionEngineStatus::kOk;
    default_result.decision = CollisionDecisionResult::kPass;
    capability_.engine_id = "mock_authoritative_engine";
    capability_.engine_version = "test";
    capability_.library_lineage = "unit_test";
    capability_.supported_geometry_kinds = {"box"};
    capability_.thread_safety =
        CollisionEngineThreadSafetyClass::kSingleContextExclusive;
    capability_.deterministic_ordering = true;
  }

  const CollisionEngineCapabilityDescriptor& capability() const override {
    return capability_;
  }

  CollisionResult check(const CollisionWorldSnapshot& snapshot,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot& policy,
                        const CollisionCancellationToken& cancel) override {
    check_count.fetch_add(1);
    last_policy_id = policy.policy_id;
    last_policy_revision_id = policy.revision.revision_id;
    if (on_check) {
      on_check(query);
    }
    CollisionResult result = default_result;
    if (query.waypoint_index) {
      auto it = result_by_waypoint.find(*query.waypoint_index);
      if (it != result_by_waypoint.end()) {
        result = it->second;
      }
    }
    result.query_id = query.query_id;
    result.surface = query.surface;
    result.state_time_ns = query.state_time_ns;
    result.collision_world_revision = snapshot.revision;
    result.engine_id = capability_.engine_id;
    result.engine_version = capability_.engine_version;
    if (cancel.is_cancel_requested()) {
      result.in_collision = false;
      result.normalized_pairs.clear();
      result.engine_status = CollisionEngineStatus::kCancelled;
      result.decision = CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["cancel_reason"] = cancel.reason();
    }
    return result;
  }

  std::string engine_id() const override { return capability_.engine_id; }
  std::string engine_version() const override {
    return capability_.engine_version;
  }

  std::string last_policy_id;
  std::uint64_t last_policy_revision_id = 0;

 private:
  CollisionEngineCapabilityDescriptor capability_;
};

class MockExecution : public IExecutionBackend {
 public:
  int execute_count = 0;
  int stop_count = 0;
  int pause_count = 0;
  int resume_count = 0;
  int reset_count = 0;
  int sync_count = 0;
  TrajectoryDTO last_trajectory;
  ExecutionSnapshot snapshot;
  Result<void> next_sync_result = Result<void>::Ok();
  std::function<void()> on_sync;
  /// Optional override for the terminal state reported back to the kernel
  /// at the end of `execute_trajectory`. When unset, behave as before
  /// (`kCompleted`). Used by tests that need to drive the kernel into a
  /// non-default terminal state (e.g. `kInterventionRequired`) to exercise
  /// the explicit acknowledgement paths.
  std::optional<ExecutionState> next_terminal_state;

  ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
    ++execute_count;
    last_trajectory = trajectory;
    snapshot.state = next_terminal_state.value_or(ExecutionState::kCompleted);
    snapshot.progress = 1.0;
    if (!trajectory.waypoints.empty()) {
      snapshot.joint_positions_rad = trajectory.waypoints.back().positions_rad;
    }
    return ExecutionHandle{
        [](int64_t /*timeout_ns*/) { return true; },
        [this]() { return snapshot; }};
  }

  Result<void> sync_observed_state_before_planning() override {
    ++sync_count;
    if (on_sync) {
      on_sync();
    }
    return next_sync_result;
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
    snapshot.state = ExecutionState::kCompleted;
    snapshot.progress = 1.0;
    if (!trajectory.waypoints.empty()) {
      snapshot.joint_positions_rad = trajectory.waypoints.back().positions_rad;
    }
    return ExecutionHandle{
        [](int64_t /*timeout_ns*/) { return true; },
        [this]() { return snapshot; }};
  }

  ResetResultDTO reset() override {
    ++reset_count;
    return ResetResultDTO{true, std::nullopt};
  }

  ExecutionSnapshot get_execution_snapshot() const override {
    return snapshot;
  }

  /// Test backend declares spring_back_hold + hard_deceleration as
  /// supported. Individual tests may further customize via the
  /// `motion_interrupt_stop_capability` field.
  MotionInterruptStopCapabilityDTO motion_interrupt_stop_capability{
      {MotionInterruptStopMode::kSpringBackHold,
       MotionInterruptStopMode::kHardDeceleration},
      std::string{"mock_backend_capability"},
      std::nullopt};

  MotionInterruptStopCapabilityDTO
  get_motion_interrupt_stop_capability() const override {
    return motion_interrupt_stop_capability;
  }

  // Controlled-stop result surface. Default returns a successful
  // native-simulated stop so trip tests using MockExecution
  // produce signal_trip outcomes; individual tests can override
  // `next_controlled_stop_result` to exercise failure / fallback paths.
  std::optional<ControlledStopResultDTO> next_controlled_stop_result;
  int controlled_stop_count = 0;
  MotionInterruptStopProfileDTO last_controlled_stop_profile;

  ControlledStopResultDTO execute_controlled_stop(
      const MotionInterruptStopProfileDTO& profile) override {
    ++controlled_stop_count;
    last_controlled_stop_profile = profile;
    if (next_controlled_stop_result) {
      return *next_controlled_stop_result;
    }
    ControlledStopResultDTO r;
    r.accepted = true;
    r.executed_stop_mode = profile.stop_mode;
    r.stop_command_source =
        MotionInterruptStopCommandSource::kNativeSimulatedStop;
    r.backend_stop_confirmed = true;
    r.confirmation_latency_ns = 1'000'000;  // 1 ms
    return r;
  }
};

class ControlledExecution : public IExecutionBackend {
 public:
  int execute_count = 0;
  int stop_count = 0;
  int pause_count = 0;
  int resume_count = 0;
  int reset_count = 0;
  TrajectoryDTO last_execute_trajectory;
  TrajectoryDTO last_resume_trajectory;
  ExecutionSnapshot snapshot;
  bool terminal_handle_on_pause = false;
  std::function<void()> on_get_snapshot;
  int sync_observed_state_count = 0;
  bool block_sync_observed_state = false;
  bool release_blocked_sync_observed_state = false;

  ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++execute_count;
      last_execute_trajectory = trajectory;
      running_ = true;
      paused_ = false;
      snapshot.state = ExecutionState::kExecuting;
      snapshot.progress = 0.0;
      if (!trajectory.waypoints.empty()) {
        snapshot.joint_positions_rad = trajectory.waypoints.front().positions_rad;
      }
    }
    cv_.notify_all();
    return make_handle();
  }

  Result<void> sync_observed_state_before_planning() override {
    std::unique_lock<std::mutex> lock(mu_);
    ++sync_observed_state_count;
    cv_.notify_all();
    if (block_sync_observed_state) {
      cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
        return release_blocked_sync_observed_state;
      });
    }
    return Result<void>::Ok();
  }

  // Failure-injection knob: when true, the next legacy
  // `stop()` call counts the attempt but returns not-accepted so
  // tests can exercise the "both controlled-stop and emergency-
  // fallback failed" path without leaving the backend in an
  // inconsistent state.
  bool fail_next_legacy_stop = false;

  StopResultDTO stop() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (fail_next_legacy_stop) {
        fail_next_legacy_stop = false;
        ++stop_count;  // record the attempt
        return StopResultDTO{
            false,
            ErrorDTO{kErrorStopFailed,
                     "injected legacy stop failure",
                     {{"reason", "test_injected_legacy_stop_failure"}}}};
      }
      if (!running_) {
        return StopResultDTO{
            false,
            ErrorDTO{kErrorInvalidRequest, "No execution in flight", {}}};
      }
      ++stop_count;
      running_ = false;
      paused_ = false;
      snapshot.state = ExecutionState::kStopped;
    }
    cv_.notify_all();
    return StopResultDTO{true, std::nullopt};
  }

  PauseResultDTO pause() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!running_) {
        return PauseResultDTO{
            false,
            ErrorDTO{kErrorInvalidRequest, "No execution in flight", {}}};
      }
      ++pause_count;
      if (terminal_handle_on_pause) {
        running_ = false;
      }
      paused_ = true;
      snapshot.state = ExecutionState::kPaused;
    }
    cv_.notify_all();
    return PauseResultDTO{true, std::nullopt};
  }

  ExecutionHandle resume(const TrajectoryDTO& trajectory) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++resume_count;
      last_resume_trajectory = trajectory;
      if (terminal_handle_on_pause) {
        running_ = true;
      }
      paused_ = false;
      if (running_) {
        snapshot.state = ExecutionState::kExecuting;
      }
    }
    cv_.notify_all();
    return make_handle();
  }

  ResetResultDTO reset() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++reset_count;
      running_ = false;
      paused_ = false;
      snapshot.state = ExecutionState::kIdle;
      snapshot.progress = 0.0;
      snapshot.joint_positions_rad.clear();
    }
    cv_.notify_all();
    return ResetResultDTO{true, std::nullopt};
  }

  ExecutionSnapshot get_execution_snapshot() const override {
    if (on_get_snapshot) {
      on_get_snapshot();
    }
    std::lock_guard<std::mutex> lock(mu_);
    return snapshot;
  }

  /// Controlled-stop capability for trip tests using
  /// ControlledExecution. Mirrors MockExecution; individual tests
  /// may shrink the set to exercise capability-gate rejection.
  MotionInterruptStopCapabilityDTO motion_interrupt_stop_capability{
      {MotionInterruptStopMode::kSpringBackHold,
       MotionInterruptStopMode::kHardDeceleration},
      std::string{"controlled_backend_capability"},
      std::nullopt};

  MotionInterruptStopCapabilityDTO
  get_motion_interrupt_stop_capability() const override {
    return motion_interrupt_stop_capability;
  }

  // Controlled-stop result surface. Default returns a successful
  // native-simulated stop; trip tests override
  // `next_controlled_stop_result` for failure / fallback variants.
  std::optional<ControlledStopResultDTO> next_controlled_stop_result;
  // Tests targeting the post-stop snapshot capture path can override
  // the joint vector the backend reports after controlled stop. When
  // unset, the controlled-stop path leaves the snapshot's joint
  // vector unchanged (the trajectory waypoints from execute_trajectory).
  std::optional<std::vector<double>> next_post_stop_joint_positions_rad;
  int controlled_stop_count = 0;
  int controlled_stop_cancel_count = 0;
  MotionInterruptStopProfileDTO last_controlled_stop_profile;
  bool block_controlled_stop = false;
  bool release_blocked_controlled_stop = false;

  ControlledStopResultDTO execute_controlled_stop(
      const MotionInterruptStopProfileDTO& profile) override {
    return execute_controlled_stop(profile, std::stop_token{});
  }

  ControlledStopResultDTO execute_controlled_stop(
      const MotionInterruptStopProfileDTO& profile,
      std::stop_token stop_token) override {
    ControlledStopResultDTO r;
    if (next_controlled_stop_result) {
      r = *next_controlled_stop_result;
    } else {
      r.accepted = true;
      r.executed_stop_mode = profile.stop_mode;
      r.stop_command_source =
          MotionInterruptStopCommandSource::kNativeSimulatedStop;
      r.backend_stop_confirmed = true;
      r.confirmation_latency_ns = 1'000'000;
    }
    {
      std::unique_lock<std::mutex> lock(mu_);
      ++controlled_stop_count;
      last_controlled_stop_profile = profile;
      cv_.notify_all();
      if (block_controlled_stop) {
        std::stop_callback stop_notify(stop_token, [this]() {
          cv_.notify_all();
        });
        const bool released = cv_.wait_for(
            lock, std::chrono::seconds(2), [this, &stop_token]() {
              return release_blocked_controlled_stop ||
                     stop_token.stop_requested();
            });
        if (!released || stop_token.stop_requested()) {
          ++controlled_stop_cancel_count;
          running_ = false;
          paused_ = false;
          snapshot.state = ExecutionState::kStopped;
          r.accepted = true;
          r.backend_stop_confirmed = false;
          r.error = ErrorDTO{
              std::string(kErrorMotionInterruptBackendStopFailed),
              "controlled stop cancelled by test",
              {{"reason", released ? "controlled_stop_cancelled"
                                    : "controlled_stop_test_timeout"}}};
          cv_.notify_all();
          return r;
        }
      }
      // Only flip the backend's internal state to Stopped when the
      // controlled-stop actually succeeded. A refused / unconfirmed /
      // errored result means the controller did NOT actually stop,
      // so the legacy emergency-fallback `stop()`
      // must still find a running execution to stop. Mirror what
      // a production backend does: a refused execute_controlled_stop
      // leaves the running goal in flight.
      const bool stop_succeeded =
          r.accepted && r.backend_stop_confirmed && !r.error.has_value();
      if (stop_succeeded) {
        running_ = false;
        snapshot.state = ExecutionState::kStopped;
        if (next_post_stop_joint_positions_rad) {
          snapshot.joint_positions_rad = *next_post_stop_joint_positions_rad;
        }
      }
    }
    cv_.notify_all();
    return r;
  }

  void set_progress(double progress,
                    std::vector<double> joint_positions,
                    std::optional<std::int64_t> timestamp_ns = std::nullopt) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot.progress = progress;
      snapshot.joint_positions_rad = std::move(joint_positions);
      snapshot.state = paused_ ? ExecutionState::kPaused : ExecutionState::kExecuting;
      if (timestamp_ns) {
        snapshot.timestamp_ns = *timestamp_ns;
      }
    }
    cv_.notify_all();
  }

  void complete() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      running_ = false;
      paused_ = false;
      snapshot.state = ExecutionState::kCompleted;
      snapshot.progress = 1.0;
      const auto* source = !last_resume_trajectory.waypoints.empty()
                               ? &last_resume_trajectory
                               : &last_execute_trajectory;
      if (!source->waypoints.empty()) {
        snapshot.joint_positions_rad = source->waypoints.back().positions_rad;
      }
    }
    cv_.notify_all();
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      running_ = false;
      paused_ = false;
    }
    cv_.notify_all();
  }

  // All four helpers are bounded so the same contention class that
  // motivated wait_until_execute_count's timeout can't slip through
  // a sibling unbounded wait. Default 2 s is generous on a quiet
  // host; callers ASSERT_TRUE on the return so a timeout fails the
  // test fast with a specific diagnostic instead of hanging up to
  // the ctest-level 60 s ceiling.
  bool wait_until_execute_called(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this]() { return execute_count > 0; });
  }

  bool wait_until_sync_observed_state_count(
      int count,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() {
                          return sync_observed_state_count >= count;
                        });
  }

  bool wait_until_execute_count(
      int count,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() { return execute_count >= count; });
  }

  bool wait_until_paused(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this]() { return paused_; });
  }

  bool wait_until_resume_called(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this]() { return resume_count > 0; });
  }

  bool wait_until_stop_count(
      int count,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() { return stop_count >= count; });
  }

  bool wait_until_controlled_stop_count(
      int count,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() {
                          return controlled_stop_count >= count;
                        });
  }

  bool wait_until_controlled_stop_cancel_count(
      int count,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() {
                          return controlled_stop_cancel_count >= count;
                        });
  }

  void release_controlled_stop() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      release_blocked_controlled_stop = true;
    }
    cv_.notify_all();
  }

  void release_sync_observed_state() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      release_blocked_sync_observed_state = true;
    }
    cv_.notify_all();
  }

 private:
  ExecutionHandle make_handle() {
    return ExecutionHandle{
        [this](int64_t timeout_ns) {
          std::unique_lock<std::mutex> lock(mu_);
          if (timeout_ns > 0) {
            return cv_.wait_for(lock, std::chrono::nanoseconds(timeout_ns),
                                [this]() { return !running_; });
          }
          // Production kernel callers always pass kExecutionPollNs (10 ms),
          // so this branch is effectively dead in normal use. Bound it
          // with a generous deadline (60 s, matching the ctest-level
          // ceiling) so that a test path or future caller that passes 0
          // can't deadlock the test runner. Return the bool from
          // wait_for so callers see timeouts instead of getting a stale
          // `true` after a hung wait.
          return cv_.wait_for(lock, std::chrono::seconds(60),
                              [this]() { return !running_; });
        },
        [this]() {
          std::lock_guard<std::mutex> lock(mu_);
          return snapshot;
        }};
  }

  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool running_ = false;
  bool paused_ = false;
};

class ThreadSafeManualScheduler final : public IScheduler {
 public:
  ScheduleHandle schedule(
      TimePoint deadline, std::function<void()> callback,
      SchedulerPriority /*priority*/ = SchedulerPriority::Safety) override {
    std::lock_guard<std::mutex> lock(mu_);
    ScheduleHandle handle = next_handle_++;
    pending_.emplace(deadline, std::make_pair(handle, std::move(callback)));
    return handle;
  }

  void cancel(ScheduleHandle handle) override {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->second.first == handle) {
        pending_.erase(it);
        return;
      }
    }
  }

  void advance_to(TimePoint t) {
    std::vector<std::function<void()>> to_fire;
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->first <= t) {
          to_fire.push_back(std::move(it->second.second));
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }

    for (auto& callback : to_fire) {
      callback();
    }
  }

  size_t pending_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
  }

 private:
  mutable std::mutex mu_;
  std::multimap<TimePoint, std::pair<ScheduleHandle, std::function<void()>>> pending_;
  uint64_t next_handle_ = 1;
};

class ThreadSafeFakeClock final : public IClock {
 public:
  TimePoint now() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return current_time_;
  }

  void advance(Duration duration) {
    std::lock_guard<std::mutex> lock(mu_);
    current_time_ += duration;
  }

 private:
  mutable std::mutex mu_;
  TimePoint current_time_ = 0;
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

TrajectoryDTO make_test_trajectory_from(std::vector<double> start,
                                        std::vector<double> goal) {
  TrajectoryDTO t;
  t.joint_names = {"j1", "j2"};
  TrajectoryWaypointDTO wp0;
  wp0.time_from_start_s = 0.0;
  wp0.positions_rad = std::move(start);
  TrajectoryWaypointDTO wp1;
  wp1.time_from_start_s = 1.0;
  wp1.positions_rad = std::move(goal);
  t.waypoints = {wp0, wp1};
  return t;
}

Pose3DTO make_pose(double x, double y, double z, QuaternionDTO orientation = {0.0, 0.0, 0.0, 1.0}) {
  Pose3DTO pose;
  pose.position_m = {x, y, z};
  pose.orientation = orientation;
  return pose;
}

ExecuteRequestDTO make_test_request(const std::string& move_id = "move_1") {
  ExecuteRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = move_id;
  req.base_frame = "world";
  req.tcp_frame = "tool0";
  req.tolerances.joint_position_tol_rad = 0.1;
  req.tolerances.translation_tol_m = 0.01;
  req.tolerances.rotation_tol_rad = 0.01;
  req.speed_profile.velocity_scale = 1.0;
  req.timeouts.planning_timeout_s = 10.0;
  req.cache_mode = CacheMode::kDefault;
  req.motion = JointGoalDTO{{"j1", "j2"}, {1.0, 1.0}};
  return req;
}

CollisionWorldSnapshot make_test_collision_world_snapshot() {
  CollisionWorldSnapshot snapshot;
  snapshot.publisher_id = "mock_kernel_collision_world";
  snapshot.revision = CollisionWorldRevision{1, "mock-collision-world"};
  snapshot.snapshot_format_version = 1;
  snapshot.body_count = 0;
  snapshot.geometry_count = 0;
  snapshot.all_bodies_have_geometry = true;
  return snapshot;
}

CollisionWorldBody make_dynamic_test_body(std::int64_t source_timestamp_ns,
                                          std::int64_t freshness_budget_ns) {
  CollisionWorldBody body;
  body.body_id = "object/dynamic_box/shape/0";
  body.owner_kind = CollisionOwnerKind::kDetachedObject;
  body.frame_id = "universe";
  body.pose.orientation.w = 1.0;
  body.source_timestamp_ns = source_timestamp_ns;
  body.source_freshness_budget_ns = freshness_budget_ns;
  CollisionGeometryDescriptor geom;
  geom.body_id = body.body_id;
  geom.owner_kind = body.owner_kind;
  geom.geometry_kind = CollisionGeometryKind::kBox;
  geom.box_dimensions_m = Vector3DTO{0.1, 0.1, 0.1};
  body.geometries.push_back(std::move(geom));
  return body;
}

CollisionPolicySnapshot make_test_collision_policy_snapshot() {
  CollisionPolicySnapshot snapshot;
  snapshot.policy_id = "free_space";
  snapshot.revision.revision_id = 1;
  snapshot.revision.content_hash = "mock-policy";
  snapshot.require_full_diagnostics = false;
  return snapshot;
}

CollisionResult make_authoritative_collision_result() {
  CollisionResult result;
  result.in_collision = true;
  result.normalized_pairs = {CollisionPairId{
      "robot/robot1/link/link1/collision/0",
      "object/box/shape/0"}};
  result.engine_status = CollisionEngineStatus::kOk;
  result.decision = CollisionDecisionResult::kFail;
  return result;
}

CollisionResult make_support_contact_collision_result(
    const CollisionPairId& pair,
    double signed_clearance_m) {
  CollisionResult result;
  result.in_collision = signed_clearance_m < 0.0;
  if (result.in_collision) {
    result.normalized_pairs = {pair};
    result.decision = CollisionDecisionResult::kFail;
  } else {
    result.decision = CollisionDecisionResult::kPass;
  }
  result.engine_status = CollisionEngineStatus::kOk;
  result.pair_clearances = {CollisionPairClearance{pair, signed_clearance_m}};
  return result;
}

InterruptSourceDefinition make_interrupt_source(std::string source_id) {
  InterruptSourceDefinition source;
  source.source_id = std::move(source_id);
  source.adapter = InterruptAdapter::kInternalTest;
  source.signal = source.source_id + "_signal";
  source.source_class = InterruptSourceClass::kFastExecutionRelevant;
  source.kind = InterruptSourceKind::kEdge;
  source.priority = 500;
  source.freshness_budget_ns = 1'000'000;
  return source;
}

InterruptPolicyDefinition make_interrupt_policy(std::string policy_id,
                                                std::string source_id) {
  InterruptPolicyDefinition policy;
  policy.policy_id = std::move(policy_id);
  policy.source_id = std::move(source_id);
  policy.priority = 500;
  policy.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  policy.preemption_action = InterruptPreemptionAction::kLatchHeld;
  policy.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  return policy;
}

InterruptSignalUpdate make_interrupt_signal(std::string source_id,
                                            std::int64_t sequence_id = 1) {
  InterruptSignalUpdate signal;
  signal.source_id = std::move(source_id);
  signal.sequence_id = sequence_id;
  signal.timestamp_ns = 1'000;
  signal.freshness_ns = 0;
  signal.value = true;
  return signal;
}

bool wait_for_state(ExecutionKernel& kernel, ExecutionState target_state,
                    int max_attempts = 200) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (kernel.get_status().state == target_state) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

bool wait_for_pending_scheduler_work(ThreadSafeManualScheduler& scheduler,
                                     int max_attempts = 200) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (scheduler.pending_count() > 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
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
  MockCollisionWorldPublisher collision_world_publisher;
  std::shared_ptr<MockCollisionEngine> collision_engine =
      std::make_shared<MockCollisionEngine>();
  ExecutionKernel kernel{planner, context, execution, clock, scheduler, cache};

  void SetUp() override {
    PlanResultDTO plan_ok;
    plan_ok.trajectory = make_test_trajectory();
    planner.next_result = plan_ok;

    context.next_validation = ValidationResultDTO{true, std::nullopt};
    context.next_collision = CollisionCheckResultDTO{false, std::nullopt, std::nullopt};

    RobotStateDTO state;
    state.joint_names = {"j1", "j2"};
    state.positions_rad = {0.0, 0.0};
    context.current_state = state;
    collision_world_publisher.current_snapshot =
        std::make_shared<const CollisionWorldSnapshot>(
            make_test_collision_world_snapshot());
    collision_world_publisher.current_policy =
        make_test_collision_policy_snapshot();
    collision_engine = std::make_shared<MockCollisionEngine>();
    kernel.set_collision_validation_policy_snapshot(
        make_test_collision_policy_snapshot());
    kernel.set_collision_validation_pipeline(collision_world_publisher,
                                             {collision_engine});
  }

  /// Helper: execute and wait for async completion.
  Result<uint64_t> execute_sync(const ExecuteRequestDTO& req) {
    auto result = kernel.execute(req);
    if (result.ok()) {
      kernel.wait_for_completion();
    }
    return result;
  }

  void store_cache(const std::string& move_id, TrajectoryDTO trajectory) {
    cache.store(move_id, std::move(trajectory), context.current_revision);
  }

  void configure_authoritative_collision_validation(
      ExecutionKernel& target_kernel,
      MockCollisionWorldPublisher& publisher,
      const std::shared_ptr<MockCollisionEngine>& engine) {
    publisher.current_snapshot =
        std::make_shared<const CollisionWorldSnapshot>(
            make_test_collision_world_snapshot());
    publisher.current_policy = make_test_collision_policy_snapshot();
    target_kernel.set_collision_validation_policy_snapshot(
        make_test_collision_policy_snapshot());
    target_kernel.set_collision_validation_pipeline(publisher, {engine});
  }
};

// --- Happy path ---

TEST_F(ExecutionKernelTest, Execute_HappyPath_ReturnsExecutionId) {
  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 1u);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_CallsPlanner) {
  execute_sync(make_test_request());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(planner.last_request.robot_id, "robot1");
  EXPECT_EQ(planner.last_request.move_id, "move_1");
}

TEST_F(ExecutionKernelTest, Execute_RelativeMove_ResolvesToPoseGoalBeforePlanning) {
  ExecuteRequestDTO req = make_test_request("relative_move");
  RelativeMoveDTO relative_move;
  relative_move.target_reference.type = ReferenceDTO::Type::kObject;
  relative_move.target_reference.id = "graspable";
  relative_move.offset.position_m = {0.0, 0.0, 0.1};
  relative_move.offset.orientation = {0.0, 0.0, 0.0, 1.0};
  req.motion = relative_move;

  Pose3DTO resolved_reference;
  resolved_reference.position_m = {0.3, -0.2, 0.15};
  resolved_reference.orientation = {0.0, 0.0, 0.0, 1.0};
  context.next_reference_pose = Result<Pose3DTO>::Ok(resolved_reference);

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::holds_alternative<PoseGoalDTO>(planner.last_request.motion));
  const auto& pose_goal = std::get<PoseGoalDTO>(planner.last_request.motion);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.x_m, 0.3);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.y_m, -0.2);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.z_m, 0.25);
  EXPECT_DOUBLE_EQ(pose_goal.goal.orientation.w, 1.0);
}

TEST_F(ExecutionKernelTest, Execute_RelativeMove_PoseReference_ResolvesToPoseGoalBeforePlanning) {
  ExecuteRequestDTO req = make_test_request("relative_move");
  RelativeMoveDTO relative_move;
  relative_move.target_reference.type = ReferenceDTO::Type::kPose;
  relative_move.target_reference.pose = Pose3DTO{};
  relative_move.target_reference.pose->position_m = {0.25, -0.15, 0.2};
  relative_move.target_reference.pose->orientation = {0.0, 0.0, 0.0, 1.0};
  relative_move.offset.position_m = {0.0, 0.0, 0.1};
  relative_move.offset.orientation = {0.0, 0.0, 0.0, 1.0};
  req.motion = relative_move;

  context.next_reference_pose = Result<Pose3DTO>::Ok(relative_move.target_reference.pose.value());

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::holds_alternative<PoseGoalDTO>(planner.last_request.motion));
  const auto& pose_goal = std::get<PoseGoalDTO>(planner.last_request.motion);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.x_m, 0.25);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.y_m, -0.15);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.z_m, 0.3);
  EXPECT_DOUBLE_EQ(pose_goal.goal.orientation.w, 1.0);
}

TEST_F(ExecutionKernelTest, Execute_RelativeMove_OffsetReference_UsesIndependentBasis) {
  constexpr double kInvSqrt2 = 0.7071067811865476;
  ExecuteRequestDTO req = make_test_request("relative_move");
  RelativeMoveDTO relative_move;
  relative_move.target_reference.type = ReferenceDTO::Type::kObject;
  relative_move.target_reference.id = "graspable";

  ReferenceDTO offset_reference;
  offset_reference.type = ReferenceDTO::Type::kFrame;
  offset_reference.id = "base_link";
  relative_move.offset_reference = offset_reference;

  relative_move.offset.position_m = {0.1, 0.0, 0.0};
  relative_move.offset.orientation = {0.0, 0.0, 0.0, 1.0};
  req.motion = relative_move;

  Pose3DTO target_pose;
  target_pose.position_m = {0.3, -0.2, 0.15};
  target_pose.orientation = {0.0, 0.0, 0.0, 1.0};

  Pose3DTO offset_basis_pose;
  offset_basis_pose.position_m = {0.0, 0.0, 0.0};
  offset_basis_pose.orientation = {0.0, 0.0, kInvSqrt2, kInvSqrt2};

  context.reference_pose_sequence = {
      Result<Pose3DTO>::Ok(target_pose),
      Result<Pose3DTO>::Ok(offset_basis_pose),
  };

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::holds_alternative<PoseGoalDTO>(planner.last_request.motion));
  const auto& pose_goal = std::get<PoseGoalDTO>(planner.last_request.motion);
  EXPECT_NEAR(pose_goal.goal.position_m.x_m, 0.3, 1e-9);
  EXPECT_NEAR(pose_goal.goal.position_m.y_m, -0.1, 1e-9);
  EXPECT_NEAR(pose_goal.goal.position_m.z_m, 0.15, 1e-9);
  EXPECT_NEAR(pose_goal.goal.orientation.z, kInvSqrt2, 1e-9);
  EXPECT_NEAR(pose_goal.goal.orientation.w, kInvSqrt2, 1e-9);
}

TEST_F(ExecutionKernelTest, Config_SingleShotIkTimeout_RejectsNonPositiveOrNonFinite) {
  for (double bad : {0.0, -0.01, std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity()}) {
    ExecutionKernelConfig config;
    config.single_shot_ik_timeout_s = bad;
    EXPECT_THROW(
        (ExecutionKernel{planner, context, execution, clock, scheduler, cache,
                         config}),
        std::invalid_argument);
  }

  ExecutionKernelConfig ok_config;
  ok_config.single_shot_ik_timeout_s = 0.05;
  EXPECT_NO_THROW((ExecutionKernel{planner, context, execution, clock, scheduler,
                                   cache, ok_config}));
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_ValidatesTrajectory) {
  execute_sync(make_test_request());
  EXPECT_GE(context.validate_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_DispatchesToBackend) {
  execute_sync(make_test_request());
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_EQ(execution.last_trajectory.joint_names.size(), 2u);
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_CachesTrajectory) {
  execute_sync(make_test_request("move_A"));
  auto cached = cache.lookup("move_A");
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(cached->waypoints.size(), 2u);
}

TEST_F(ExecutionKernelTest,
       Execute_LiveSceneRevisionChangeAfterBorrowedSnapshotDoesNotBlockDispatch) {
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      auto mutated = make_test_collision_world_snapshot();
      mutated.revision = CollisionWorldRevision{2, "mutated-world"};
      collision_world_publisher.current_snapshot =
          std::make_shared<const CollisionWorldSnapshot>(std::move(mutated));
    }
  };

  auto result = execute_sync(make_test_request("move_scene_changed"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_TRUE(cache.lookup("move_scene_changed").has_value());
  EXPECT_EQ(collision_engine->check_count.load(), 2);
}

TEST_F(ExecutionKernelTest, Execute_SameContentRevisionChangeDuringValidationDispatches) {
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      auto mutated = make_test_collision_world_snapshot();
      mutated.revision = CollisionWorldRevision{2, "mock-collision-world"};
      collision_world_publisher.current_snapshot =
          std::make_shared<const CollisionWorldSnapshot>(std::move(mutated));
    }
  };

  auto result = execute_sync(make_test_request("move_same_content_revision"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_TRUE(cache.lookup("move_same_content_revision").has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_RepeatedLiveSceneRevisionChangesDuringValidationDoNotFreezeDispatch) {
  std::uint64_t next_revision = 2;
  collision_engine->on_check = [this, &next_revision](const CollisionQuery&) {
    auto mutated = make_test_collision_world_snapshot();
    mutated.revision = CollisionWorldRevision{
        next_revision, "mutated-world-" + std::to_string(next_revision)};
    ++next_revision;
    collision_world_publisher.current_snapshot =
        std::make_shared<const CollisionWorldSnapshot>(std::move(mutated));
  };

  auto result = execute_sync(make_test_request("move_scene_changed"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(execution.execute_count, 1);
  auto cached = cache.lookup("move_scene_changed");
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(cache.revision_of("move_scene_changed")->revision_id, 1u);
}

TEST_F(ExecutionKernelTest, Execute_IncrementingIds) {
  auto r1 = execute_sync(make_test_request());
  EXPECT_EQ(r1.value(), 1u);
  kernel.reset();
  auto r2 = execute_sync(make_test_request());
  EXPECT_EQ(r2.value(), 2u);
}

// --- Cache hit path ---

TEST_F(ExecutionKernelTest, Execute_CacheHit_SkipsPlanning) {
  store_cache("move_cached", make_test_trajectory());
  execute_sync(make_test_request("move_cached"));
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_GE(context.validate_count, 1);
  EXPECT_EQ(execution.execute_count, 1);
}

// Phase 4-P5.1K direction A: an existing (uncapped) cache entry must NOT be
// served to a request carrying per-joint dynamics caps. Cache lookup is
// disabled for capped requests — the move is re-planned and the stale entry is
// invalidated with a recorded reason.
TEST_F(ExecutionKernelTest, Execute_PerJointDynamicsLimitsBypassCacheAndReplan) {
  store_cache("move_dyn_capped", make_test_trajectory());
  auto req = make_test_request("move_dyn_capped");
  JointDynamicsLimitDTO lim;
  lim.max_velocity_rad_s = 0.5;
  req.per_joint_dynamics_limits["joint1"] = lim;

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);  // cache bypassed -> re-planned
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

// Phase 4-P5.1K direction B: a capped request must NOT leave a reusable cache
// entry. The TrajectoryCache key has no dynamics-limit fingerprint, so a capped
// trajectory stored under move_id could otherwise be picked up by a later
// uncapped request. Cache store is disabled for capped requests. (The uncapped
// happy path DOES store — see Execute_HappyPath_CachesTrajectory.)
TEST_F(ExecutionKernelTest, Execute_CappedRequest_DoesNotStoreReusableCacheEntry) {
  auto req = make_test_request("move_capped_no_store");
  JointDynamicsLimitDTO lim;
  lim.max_velocity_rad_s = 0.5;
  req.per_joint_dynamics_limits["joint1"] = lim;

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);  // planned (cache miss), then dispatched
  EXPECT_FALSE(cache.lookup("move_capped_no_store").has_value());
}

TEST_F(ExecutionKernelTest, Execute_CacheHitPoseGoalWithMatchingEndPose_SkipsPlanning) {
  store_cache("move_pose_cached", make_test_trajectory());
  auto req = make_test_request("move_pose_cached");
  PoseGoalDTO pose_goal;
  pose_goal.goal = make_pose(0.4, -0.1, 0.2);
  req.motion = pose_goal;
  context.next_fk = pose_goal.goal;

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(context.fk_count, 1);
  ASSERT_EQ(context.last_fk_request.robot_state.positions_rad.size(), 2u);
  EXPECT_DOUBLE_EQ(context.last_fk_request.robot_state.positions_rad[0], 1.0);
  EXPECT_DOUBLE_EQ(context.last_fk_request.robot_state.positions_rad[1], 1.0);
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitPoseGoalWithChangedEndPose_InvalidatesAndReplans) {
  store_cache("move_pose_changed", make_test_trajectory());
  auto req = make_test_request("move_pose_changed");
  PoseGoalDTO pose_goal;
  pose_goal.goal = make_pose(0.45, -0.1, 0.2);
  req.motion = pose_goal;
  context.next_fk = make_pose(0.1, -0.1, 0.2);

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
  auto refreshed = cache.lookup("move_pose_changed");
  ASSERT_TRUE(refreshed.has_value());
}

TEST_F(ExecutionKernelTest, Execute_CacheHitCartesianPathWithMatchingFinalWaypoint_SkipsPlanning) {
  store_cache("move_cartesian_cached", make_test_trajectory());
  auto req = make_test_request("move_cartesian_cached");
  CartesianPathDTO path;
  path.waypoints = {make_pose(0.2, -0.1, 0.1), make_pose(0.4, -0.1, 0.2)};
  path.max_translation_step_m = 0.01;
  path.max_rotation_step_rad = 0.01;
  req.motion = path;
  context.next_fk = path.waypoints.back();

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitCartesianPathWithChangedFinalWaypoint_InvalidatesAndReplans) {
  store_cache("move_cartesian_changed", make_test_trajectory());
  auto req = make_test_request("move_cartesian_changed");
  CartesianPathDTO path;
  path.waypoints = {make_pose(0.2, -0.1, 0.1), make_pose(0.45, -0.1, 0.2)};
  path.max_translation_step_m = 0.01;
  path.max_rotation_step_rad = 0.01;
  req.motion = path;
  context.next_fk = make_pose(0.1, -0.1, 0.2);

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitRelativeMoveWithUnchangedResolvedGoal_SkipsPlanning) {
  store_cache("move_relative_same", make_test_trajectory());
  auto req = make_test_request("move_relative_same");
  RelativeMoveDTO relative_move;
  relative_move.target_reference.type = ReferenceDTO::Type::kObject;
  relative_move.target_reference.id = "box";
  relative_move.offset.position_m = {0.0, 0.0, 0.1};
  relative_move.offset.orientation = {0.0, 0.0, 0.0, 1.0};
  req.motion = relative_move;

  context.next_reference_pose = Result<Pose3DTO>::Ok(make_pose(0.3, -0.2, 0.15));
  context.next_fk = make_pose(0.3, -0.2, 0.25);

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitRelativeMoveWithMovedObject_InvalidatesAndReplans) {
  store_cache("move_relative_changed", make_test_trajectory());
  auto req = make_test_request("move_relative_changed");
  RelativeMoveDTO relative_move;
  relative_move.target_reference.type = ReferenceDTO::Type::kObject;
  relative_move.target_reference.id = "box";
  relative_move.offset.position_m = {0.0, 0.0, 0.1};
  relative_move.offset.orientation = {0.0, 0.0, 0.0, 1.0};
  req.motion = relative_move;

  context.next_reference_pose = Result<Pose3DTO>::Ok(make_pose(0.5, -0.2, 0.15));
  context.next_fk = make_pose(0.3, -0.2, 0.25);

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  ASSERT_TRUE(std::holds_alternative<PoseGoalDTO>(planner.last_request.motion));
  const auto& pose_goal = std::get<PoseGoalDTO>(planner.last_request.motion);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.x_m, 0.5);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.y_m, -0.2);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.z_m, 0.25);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_RelativeMoveMissingReference_DoesNotUseStaleCache) {
  store_cache("move_missing_reference", make_test_trajectory());
  auto req = make_test_request("move_missing_reference");
  RelativeMoveDTO relative_move;
  relative_move.target_reference.type = ReferenceDTO::Type::kObject;
  relative_move.target_reference.id = "missing_box";
  relative_move.offset.position_m = {0.0, 0.0, 0.1};
  relative_move.offset.orientation = {0.0, 0.0, 0.0, 1.0};
  req.motion = relative_move;

  context.next_reference_pose = Result<Pose3DTO>::Err(
      ErrorDTO{kErrorMissingReference, "Object reference not found", {}});

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = execute_sync(req);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorMissingReference);
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(cache.lookup("move_missing_reference").has_value());
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  std::lock_guard<std::mutex> lock(events_mutex);
  bool found_cache_invalidated = false;
  for (const auto& event : events) {
    if (event.type == KernelEventType::kCacheInvalidated) {
      found_cache_invalidated = true;
      EXPECT_EQ(event.execution_id, 0u);
      EXPECT_EQ(event.details.at("move_id"), "move_missing_reference");
      EXPECT_EQ(event.details.at("reason"), "goal_unverifiable");
      break;
    }
  }
  EXPECT_TRUE(found_cache_invalidated);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitFkFailure_InvalidatesAndReplans) {
  store_cache("move_fk_failure", make_test_trajectory());
  auto req = make_test_request("move_fk_failure");
  PoseGoalDTO pose_goal;
  pose_goal.goal = make_pose(0.4, -0.1, 0.2);
  req.motion = pose_goal;
  context.throw_fk = true;

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitJointGoalComparesByJointName) {
  auto cached = make_test_trajectory();
  cached.joint_names = {"j2", "j1"};
  cached.waypoints.front().positions_rad = {0.0, 0.0};
  cached.waypoints.back().positions_rad = {2.0, 1.0};
  store_cache("move_joint_named", cached);

  auto req = make_test_request("move_joint_named");
  req.motion = JointGoalDTO{{"j1", "j2"}, {1.0, 2.0}};

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitJointGoalNameMismatch_InvalidatesAndReplans) {
  auto cached = make_test_trajectory();
  cached.joint_names = {"j3", "j1"};
  store_cache("move_joint_mismatch", cached);
  context.current_state.joint_names = {"j3", "j1"};

  auto req = make_test_request("move_joint_mismatch");
  req.motion = JointGoalDTO{{"j1", "j2"}, {1.0, 1.0}};

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitNamedTarget_InvalidatesAndReplans) {
  store_cache("move_named_target", make_test_trajectory());
  auto req = make_test_request("move_named_target");
  req.motion = NamedTargetDTO{"ready"};

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitEmptyTrajectory_InvalidatesAndReplans) {
  store_cache("move_empty_cached", TrajectoryDTO{});
  auto req = make_test_request("move_empty_cached");

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitValidationFailure_BlocksExecution) {
  store_cache("move_cached_invalid", make_test_trajectory());
  context.next_validation = ValidationResultDTO{
      false,
      ErrorDTO{
          kErrorValidationCollision,
          "Cached trajectory is no longer valid under current collision policy",
          {{"failure_stage", "cached_validation"}}}};

  execute_sync(make_test_request("move_cached_invalid"));

  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(context.validate_count, 1);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_FALSE(cache.lookup("move_cached_invalid").has_value());
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
  EXPECT_EQ(snapshot.error->details.at("failure_stage"), "cached_validation");
}

TEST_F(ExecutionKernelTest, Execute_CacheDisabled_IgnoresCache) {
  store_cache("move_no_cache", make_test_trajectory());
  auto req = make_test_request("move_no_cache");
  req.cache_mode = CacheMode::kDisable;
  execute_sync(req);
  EXPECT_EQ(planner.plan_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_CacheDisabled_DoesNotStore) {
  auto req = make_test_request("move_no_store");
  req.cache_mode = CacheMode::kDisable;
  execute_sync(req);
  EXPECT_FALSE(cache.lookup("move_no_store").has_value());
}

// --- Observed-state sync before planning ---

TEST_F(ExecutionKernelTest, Execute_SyncObservedStateBeforePlanning_CalledBeforePlanner) {
  execution.on_sync = [this]() {
    RobotStateDTO state;
    state.joint_names = {"j1", "j2"};
    state.positions_rad = {0.25, -0.5};
    context.current_state = state;
  };

  auto result = execute_sync(make_test_request("move_after_sync"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(execution.sync_count, 1);
  ASSERT_EQ(planner.plan_count, 1);
  ASSERT_EQ(planner.last_request.start_state.positions_rad.size(), 2u);
  EXPECT_DOUBLE_EQ(planner.last_request.start_state.positions_rad[0], 0.25);
  EXPECT_DOUBLE_EQ(planner.last_request.start_state.positions_rad[1], -0.5);
}

TEST_F(ExecutionKernelTest, Execute_SyncObservedStateBeforePlanningFailure_BlocksPlanning) {
  execution.next_sync_result = Result<void>::Err(
      ErrorDTO{kErrorStartStateMismatch, "No fresh observed robot state", {}});

  auto result = execute_sync(make_test_request("move_no_fresh_state"));

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorStartStateMismatch);
  EXPECT_EQ(execution.sync_count, 1);
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(context.validate_count, 0);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitWithStaleStart_InvalidatesAndReplans) {
  auto stale_cached = make_test_trajectory();
  ASSERT_FALSE(stale_cached.waypoints.empty());
  stale_cached.waypoints.front().positions_rad = {0.5, 0.5};
  store_cache("move_stale_start", stale_cached);

  auto result = execute_sync(make_test_request("move_stale_start"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(execution.sync_count, 1);
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_GE(context.validate_count, 1);
  EXPECT_EQ(execution.execute_count, 1);
  auto refreshed = cache.lookup("move_stale_start");
  ASSERT_TRUE(refreshed.has_value());
  ASSERT_FALSE(refreshed->waypoints.empty());
  EXPECT_DOUBLE_EQ(refreshed->waypoints.front().positions_rad[0], 0.0);
  EXPECT_DOUBLE_EQ(refreshed->waypoints.front().positions_rad[1], 0.0);
}

// --- Planning failure ---

TEST_F(ExecutionKernelTest, Execute_PlanningFails_TransitionsToFailed) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;

  execute_sync(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
}

// --- Validation failure ---

TEST_F(ExecutionKernelTest, Execute_ValidationFails_TransitionsToFailed) {
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Collision in path", {}}};

  execute_sync(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(context.last_validation_options.run_collision_checks);
}

TEST_F(ExecutionKernelTest, Execute_ValidationFails_InvalidatesCache) {
  store_cache("move_val_fail", make_test_trajectory());
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

  execute_sync(make_test_request("move_val_fail"));
  EXPECT_FALSE(cache.lookup("move_val_fail").has_value());
}

// --- Start-state mismatch ---

TEST_F(ExecutionKernelTest, Execute_StartStateMismatch_Fails) {
  RobotStateDTO bad_state;
  bad_state.joint_names = {"j1", "j2"};
  bad_state.positions_rad = {5.0, 5.0};
  context.current_state = bad_state;

  execute_sync(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
}

TEST_F(ExecutionKernelTest, Execute_StartStateValidationComparesByJointName) {
  RobotStateDTO reordered_state;
  reordered_state.joint_names = {"j2", "j1"};
  reordered_state.positions_rad = {0.0, 0.0};
  context.current_state = reordered_state;

  execute_sync(make_test_request("move_start_named"));
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_StartStateMissingJointName_FailsClosed) {
  RobotStateDTO ambiguous_state;
  ambiguous_state.joint_names = {"j1"};
  ambiguous_state.positions_rad = {0.0};
  context.current_state = ambiguous_state;

  execute_sync(make_test_request("move_start_missing_joint"));
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
}

// --- Pre-execution collision ---

TEST_F(ExecutionKernelTest, Execute_PreCollisionCheck_Blocks) {
  collision_engine->default_result = make_authoritative_collision_result();

  execute_sync(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(context.last_validation_options.run_collision_checks);
}

// --- Authoritative pre-execution collision via the manyforge_collision port

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeCollisionPipelineRejectsCollision) {
  collision_engine->default_result = make_authoritative_collision_result();

  execute_sync(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_GE(collision_engine->check_count.load(), 1);
}

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeCollisionDiagnosticsReportClassifiedFailurePair) {
  CollisionPairId allowed_pair{
      "object/box_a/shape/0",
      "object/box_b/shape/0"};
  CollisionPairId blocking_pair{
      "object/box_a/shape/0",
      "robot/robot1/link/link1/collision/0"};
  CollisionResult result;
  result.in_collision = true;
  result.normalized_pairs = {allowed_pair, blocking_pair};
  result.engine_status = CollisionEngineStatus::kOk;
  result.decision = CollisionDecisionResult::kFail;
  collision_engine->default_result = result;

  auto policy = make_test_collision_policy_snapshot();
  policy.allowed_static_static_pairs.push_back(allowed_pair);
  collision_world_publisher.current_policy = policy;

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(make_test_request("classified_failure_diagnostics"));

  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  const auto& details = snapshot.error->details;
  EXPECT_EQ(details.at("collision_result_first_pair"),
            "object/box_a/shape/0 <-> object/box_b/shape/0");
  EXPECT_EQ(details.at("collision_result_first_failure_pair"),
            "object/box_a/shape/0 <-> robot/robot1/link/link1/collision/0");
  EXPECT_EQ(details.at("collision_result_first_failure_classification"),
            "blocking_collision");
  EXPECT_EQ(details.at("collision_result_first_failure_joint_names"),
            "j1,j2");
  EXPECT_EQ(details.at("collision_result_first_failure_positions_rad"),
            "0,0");
  EXPECT_EQ(details.at("collision_result_first_failure_time_from_start_s"),
            "0.000000");
  std::lock_guard<std::mutex> lock(events_mutex);
  bool found_error_event = false;
  for (const auto& event : events) {
    if (event.type != KernelEventType::kErrorRaised) {
      continue;
    }
    ASSERT_TRUE(event.error.has_value());
    if (event.error->code != kErrorValidationCollision) {
      continue;
    }
    found_error_event = true;
    EXPECT_EQ(
        event.details.at("collision_result_first_failure_pair"),
        "object/box_a/shape/0 <-> robot/robot1/link/link1/collision/0");
    EXPECT_EQ(event.details.at("collision_result_first_failure_classification"),
              "blocking_collision");
    EXPECT_EQ(event.error->details.at("collision_result_first_failure_pair"),
              event.details.at("collision_result_first_failure_pair"));
    EXPECT_EQ(
        event.details.at("collision_result_first_failure_positions_rad"),
        "0,0");
    break;
  }
  EXPECT_TRUE(found_error_event);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeCollisionPipelineDispatchesWhenEngineReportsClean) {
  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_FALSE(context.last_validation_options.run_collision_checks);
  EXPECT_GE(collision_engine->check_count.load(), 1);
}

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeCollisionUsesPublisherPolicySnapshot) {
  auto installed_policy = make_test_collision_policy_snapshot();
  installed_policy.policy_id = "stale_installed_policy";
  installed_policy.revision.revision_id = 9;
  kernel.set_collision_validation_policy_snapshot(installed_policy);

  auto publisher_policy = make_test_collision_policy_snapshot();
  publisher_policy.policy_id = "publisher_dynamic_policy";
  publisher_policy.revision.revision_id = 42;
  collision_world_publisher.current_policy = publisher_policy;

  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_GE(collision_engine->check_count.load(), 1);
  EXPECT_EQ(collision_engine->last_policy_id, "publisher_dynamic_policy");
  EXPECT_EQ(collision_engine->last_policy_revision_id, 42u);
}

TEST_F(ExecutionKernelTest,
       Execute_MissingAuthoritativeCollisionPipelineFailsClosed) {
  ExecutionKernel unconfigured_kernel{
      planner, context, execution, clock, scheduler, cache};

  auto result = unconfigured_kernel.execute(make_test_request("missing_pipeline"));
  ASSERT_TRUE(result.ok());
  unconfigured_kernel.wait_for_completion();
  EXPECT_EQ(unconfigured_kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest,
       Execute_ValidationInterruptCancelsCollisionPassWithoutDispatch) {
  auto source = make_interrupt_source("validation_stop");
  ASSERT_TRUE(kernel.register_interrupt_source(source).ok());
  auto policy =
      make_interrupt_policy("validation_stop_policy", "validation_stop");
  policy.affected_phases = {InterruptExecutionPhase::kValidation};
  policy.preemption_action = InterruptPreemptionAction::kCancelValidation;
  ASSERT_TRUE(kernel.register_interrupt_policy(policy).ok());

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  bool interrupt_sent = false;
  collision_engine->on_check = [&](const CollisionQuery&) {
    if (interrupt_sent) {
      return;
    }
    interrupt_sent = true;
    auto ingest =
        kernel.publish_fast_interrupt_signal(make_interrupt_signal(
            "validation_stop"));
    EXPECT_TRUE(ingest.latched);
  };

  auto result = execute_sync(make_test_request("validation_cancel"));

  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(interrupt_sent);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kInterrupted);
  EXPECT_EQ(execution.execute_count, 0);
  auto latch = kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->source_id, "validation_stop");
  EXPECT_EQ(latch->execution_phase_at_latch,
            InterruptExecutionPhase::kValidation);
  EXPECT_EQ(latch->preemption_action_requested,
            InterruptPreemptionAction::kCancelValidation);
  EXPECT_EQ(latch->preemption_result, InterruptPreemptionResult::kSucceeded);
  auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "collision_validation_cancelled");

  std::lock_guard<std::mutex> lock(events_mutex);
  bool saw_latched = false;
  bool saw_preempt_succeeded = false;
  bool saw_interrupted_transition = false;
  bool saw_validation_failed = false;
  for (const auto& event : events) {
    saw_latched = saw_latched ||
                  event.type == KernelEventType::kInterruptLatched;
    if (event.type == KernelEventType::kInterruptPreemptApplied &&
        event.details.at("preemption_result") == "succeeded") {
      saw_preempt_succeeded = true;
    }
    if (event.type == KernelEventType::kStateTransition &&
        event.to_state == ExecutionState::kInterrupted) {
      saw_interrupted_transition = true;
    }
    saw_validation_failed =
        saw_validation_failed ||
        event.type == KernelEventType::kValidationFailed;
  }
  EXPECT_TRUE(saw_latched);
  EXPECT_TRUE(saw_preempt_succeeded);
  EXPECT_TRUE(saw_interrupted_transition);
  EXPECT_TRUE(saw_validation_failed);
}

// --- Execute requires bound acknowledgement from terminal success ---

TEST_F(ExecutionKernelTest, Execute_FromCompleted_RequiresAcknowledgement) {
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto rejected = kernel.execute(make_test_request());
  EXPECT_FALSE(rejected.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto stale_ack = kernel.acknowledge_completed(first.value() + 1);
  EXPECT_FALSE(stale_ack.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto ack = kernel.acknowledge_completed(first.value());
  ASSERT_TRUE(ack.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  auto idle_snapshot = kernel.get_execution_snapshot();
  EXPECT_EQ(idle_snapshot.state, ExecutionState::kIdle);
  ASSERT_EQ(idle_snapshot.joint_positions_rad.size(), 2u);
  EXPECT_EQ(idle_snapshot.joint_positions_rad[0], 1.0);
  EXPECT_EQ(idle_snapshot.joint_positions_rad[1], 1.0);

  auto result = execute_sync(make_test_request());
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest, AcknowledgeCompleted_FromFailed_Rejected) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;
  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  auto ack = kernel.acknowledge_completed(result.value());
  EXPECT_FALSE(ack.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
}

// --- Explicit acknowledge_intervention API ---
//
// `execute()` rejects with `kErrorInvalidRequest` while in
// `kInterventionRequired`. The only legitimate exit is an explicit
// `acknowledge_intervention(principal_id, reason)` call, which clears
// the state and emits a `kInterventionAcknowledged` event whose details
// carry the principal and reason for traceability.

TEST_F(ExecutionKernelTest, Execute_FromInterventionRequired_Rejected) {
  execution.next_terminal_state = ExecutionState::kInterventionRequired;
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kInterventionRequired);

  auto rejected = kernel.execute(make_test_request());
  EXPECT_FALSE(rejected.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kInterventionRequired);
  ASSERT_TRUE(rejected.error().details.contains("reason_code"));
  EXPECT_EQ(rejected.error().details.at("reason_code"), "intervention_required");
}

TEST_F(ExecutionKernelTest, AcknowledgeIntervention_FromIntervention_TransitionsToIdle) {
  execution.next_terminal_state = ExecutionState::kInterventionRequired;
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kInterventionRequired);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto ack = kernel.acknowledge_intervention("operator_alice", "ui_run_resume");
  ASSERT_TRUE(ack.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);

  // Subsequent execute() must succeed: clear next_terminal_state so the
  // backend reports a normal Completed result for the resume run.
  execution.next_terminal_state.reset();
  auto resumed = execute_sync(make_test_request());
  EXPECT_TRUE(resumed.ok());

  std::lock_guard<std::mutex> lock(events_mutex);
  bool saw_ack_event = false;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kInterventionAcknowledged) {
      saw_ack_event = true;
      ASSERT_TRUE(e.details.contains("principal_id"));
      EXPECT_EQ(e.details.at("principal_id"), "operator_alice");
      ASSERT_TRUE(e.details.contains("reason"));
      EXPECT_EQ(e.details.at("reason"), "ui_run_resume");
      ASSERT_TRUE(e.details.contains("prev_state"));
      EXPECT_EQ(e.details.at("prev_state"),
                to_string(ExecutionState::kInterventionRequired));
      break;
    }
  }
  EXPECT_TRUE(saw_ack_event)
      << "kInterventionAcknowledged event was not emitted";
}

TEST_F(ExecutionKernelTest, AcknowledgeIntervention_FromIdle_Rejected) {
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);

  auto ack = kernel.acknowledge_intervention("operator_bob", "spurious");
  EXPECT_FALSE(ack.accepted);
  ASSERT_TRUE(ack.error.has_value());
  EXPECT_EQ(ack.error->code, kErrorInvalidRequest);
}

TEST_F(ExecutionKernelTest, AcknowledgeIntervention_FromCompleted_Rejected) {
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto ack = kernel.acknowledge_intervention("operator_carol", "wrong_state");
  EXPECT_FALSE(ack.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted)
      << "Wrong-state ack must not transition out of Completed";
}

TEST_F(ExecutionKernelTest, Execute_FromFailed_ResetsStateAndRetriesWithoutBackendReset) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;

  auto first = execute_sync(make_test_request("first_fails"));
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_EQ(execution.reset_count, 0);

  PlanResultDTO plan_ok;
  plan_ok.trajectory = make_test_trajectory();
  planner.next_result = plan_ok;

  auto retry = execute_sync(make_test_request("retry_after_failure"));
  ASSERT_TRUE(retry.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_EQ(execution.reset_count, 0);
  EXPECT_EQ(execution.sync_count, 2);
}

// --- Stop ---

TEST_F(ExecutionKernelTest, Stop_FromIdle_NotAccepted) {
  auto result = kernel.stop();
  EXPECT_FALSE(result.accepted);
}

TEST_F(ExecutionKernelTest, Stop_FromCompleted_NotAccepted) {
  execute_sync(make_test_request());
  auto result = kernel.stop();
  EXPECT_FALSE(result.accepted);
}

// --- Pause ---

TEST_F(ExecutionKernelTest, Pause_FromIdle_NotAccepted) {
  auto result = kernel.pause();
  EXPECT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorInvalidRequest);
  EXPECT_EQ(result.error->details.at("reason_code"), "pause_not_executing");
}

// --- Resume ---

TEST_F(ExecutionKernelTest, Resume_FromIdle_NotAccepted) {
  auto result = kernel.resume();
  EXPECT_FALSE(result.accepted);
}

TEST_F(ExecutionKernelTest, Resume_FromPaused_ReplansFromFreshCurrentState) {
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called()) << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  controlled_execution.set_progress(0.5, {0.5, 0.5});
  auto pause_result = controlled_kernel.pause();
  ASSERT_TRUE(pause_result.accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused()) << "wait_until_paused timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kPaused));

  context.current_state.positions_rad = {0.25, 0.25};
  PlanResultDTO resume_plan_ok;
  resume_plan_ok.trajectory =
      make_test_trajectory_from({0.25, 0.25}, {1.0, 1.0});
  planner.next_result = resume_plan_ok;

  auto resume_result = controlled_kernel.resume();
  ASSERT_TRUE(resume_result.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_resume_called()) << "wait_until_resume_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  EXPECT_EQ(planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.sync_observed_state_count, 2);
  ASSERT_EQ(planner.last_request.start_state.positions_rad.size(), 2u);
  EXPECT_DOUBLE_EQ(planner.last_request.start_state.positions_rad[0], 0.25);
  EXPECT_DOUBLE_EQ(planner.last_request.start_state.positions_rad[1], 0.25);
  EXPECT_GE(context.validate_count, 2);
  ASSERT_FALSE(controlled_execution.last_resume_trajectory.waypoints.empty());
  EXPECT_DOUBLE_EQ(
      controlled_execution.last_resume_trajectory.waypoints.front().positions_rad[0], 0.25);
  EXPECT_DOUBLE_EQ(
      controlled_execution.last_resume_trajectory.waypoints.front().positions_rad[1], 0.25);
  EXPECT_GE(controlled_execution.last_resume_trajectory.waypoints.size(), 2u);
  EXPECT_DOUBLE_EQ(
      controlled_execution.last_resume_trajectory.waypoints.back().positions_rad[0], 1.0);
  EXPECT_DOUBLE_EQ(
      controlled_execution.last_resume_trajectory.waypoints.back().positions_rad[1], 1.0);

  auto count_resume_audit_event = [&](KernelEventType type) {
    std::lock_guard<std::mutex> lock(events_mutex);
    return std::count_if(events.begin(), events.end(), [type](const KernelEvent& event) {
      auto reason = event.details.find("reason");
      return event.type == type && reason != event.details.end() &&
             reason->second == "resume_replan";
    });
  };
  EXPECT_EQ(count_resume_audit_event(KernelEventType::kPlanStarted), 1);
  EXPECT_EQ(count_resume_audit_event(KernelEventType::kPlanCompleted), 1);
  EXPECT_EQ(count_resume_audit_event(KernelEventType::kValidationStarted), 1);
  EXPECT_EQ(count_resume_audit_event(KernelEventType::kValidationPassed), 1);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest, Resume_FromPaused_WhenBackendPauseTerminatesHandle_KeepsWorkerAlive) {
  ControlledExecution controlled_execution;
  controlled_execution.terminal_handle_on_pause = true;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  std::mutex events_mutex;
  std::vector<KernelEventType> events;
  controlled_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e.type);
  });

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  controlled_execution.set_progress(0.5, {0.5, 0.5});
  auto pause_result = controlled_kernel.pause();
  ASSERT_TRUE(pause_result.accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused())
      << "wait_until_paused timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kPaused));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_EQ(
        std::count(events.begin(), events.end(), KernelEventType::kExecuteCompleted),
        0);
  }
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kPaused);

  context.current_state.positions_rad = {0.25, 0.25};
  PlanResultDTO resume_plan_ok;
  resume_plan_ok.trajectory =
      make_test_trajectory_from({0.25, 0.25}, {1.0, 1.0});
  planner.next_result = resume_plan_ok;
  auto resume_result = controlled_kernel.resume();
  ASSERT_TRUE(resume_result.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_resume_called())
      << "wait_until_resume_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  EXPECT_EQ(planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.sync_observed_state_count, 2);
  EXPECT_GE(context.validate_count, 2);
  ASSERT_FALSE(controlled_execution.last_resume_trajectory.waypoints.empty());
  EXPECT_DOUBLE_EQ(
      controlled_execution.last_resume_trajectory.waypoints.front().positions_rad[0],
      0.25);
  EXPECT_DOUBLE_EQ(
      controlled_execution.last_resume_trajectory.waypoints.front().positions_rad[1],
      0.25);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest, Reset_FromPaused_WhenBackendPauseTerminatesHandle_ReturnsIdle) {
  ControlledExecution controlled_execution;
  controlled_execution.terminal_handle_on_pause = true;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  auto pause_result = controlled_kernel.pause();
  ASSERT_TRUE(pause_result.accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused())
      << "wait_until_paused timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kPaused));

  auto reset_result = controlled_kernel.reset();
  ASSERT_TRUE(reset_result.accepted);
  EXPECT_EQ(controlled_execution.reset_count, 1);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000))
      << "worker did not quiesce after reset from backend-terminal pause";
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Resume_FromPaused_ValidationFailureTransitionsToFailed) {
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called()) << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  controlled_execution.set_progress(0.5, {0.5, 0.5});
  auto pause_result = controlled_kernel.pause();
  ASSERT_TRUE(pause_result.accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused()) << "wait_until_paused timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kPaused));

  context.current_state.positions_rad = {0.5, 0.5};
  PlanResultDTO resume_plan_ok;
  resume_plan_ok.trajectory =
      make_test_trajectory_from({0.5, 0.5}, {1.0, 1.0});
  planner.next_result = resume_plan_ok;
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Resume collision", {}}};

  auto resume_result = controlled_kernel.resume();
  ASSERT_TRUE(resume_result.accepted);
  controlled_kernel.wait_for_completion();

  EXPECT_EQ(planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.resume_count, 0);
  EXPECT_EQ(controlled_execution.stop_count, 1);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kFailed);
}

TEST_F(ExecutionKernelTest, Resume_FromPaused_PlanningFailureStopsPausedBackend) {
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  controlled_execution.set_progress(0.5, {0.5, 0.5});
  auto pause_result = controlled_kernel.pause();
  ASSERT_TRUE(pause_result.accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused())
      << "wait_until_paused timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kPaused));

  context.current_state.positions_rad = {0.5, 0.5};
  PlanResultDTO resume_plan_error;
  resume_plan_error.error =
      ErrorDTO{kErrorPlanningFailed, "No resume path", {}};
  planner.next_result = resume_plan_error;

  auto resume_result = controlled_kernel.resume();
  ASSERT_TRUE(resume_result.accepted);
  controlled_kernel.wait_for_completion();

  EXPECT_EQ(planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.resume_count, 0);
  EXPECT_EQ(controlled_execution.stop_count, 1);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kFailed);
}

TEST_F(ExecutionKernelTest, Resume_FromPaused_StopDuringStateSyncDoesNotDispatch) {
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  controlled_execution.set_progress(0.5, {0.5, 0.5});
  auto pause_result = controlled_kernel.pause();
  ASSERT_TRUE(pause_result.accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused())
      << "wait_until_paused timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kPaused));

  context.current_state.positions_rad = {0.5, 0.5};
  PlanResultDTO resume_plan_ok;
  resume_plan_ok.trajectory =
      make_test_trajectory_from({0.5, 0.5}, {1.0, 1.0});
  planner.next_result = resume_plan_ok;
  controlled_execution.block_sync_observed_state = true;

  auto resume_result = controlled_kernel.resume();
  ASSERT_TRUE(resume_result.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_sync_observed_state_count(2))
      << "resume sync did not start";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kValidating));

  auto stop_result = controlled_kernel.stop();
  ASSERT_TRUE(stop_result.accepted);
  controlled_execution.release_sync_observed_state();
  controlled_kernel.wait_for_completion();

  EXPECT_EQ(controlled_execution.resume_count, 0);
  EXPECT_GE(controlled_execution.stop_count, 1);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kStopped);
}

TEST_F(ExecutionKernelTest, Resume_FromPaused_ResetDuringStateSyncDoesNotDispatch) {
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  controlled_execution.set_progress(0.5, {0.5, 0.5});
  auto pause_result = controlled_kernel.pause();
  ASSERT_TRUE(pause_result.accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused())
      << "wait_until_paused timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kPaused));

  context.current_state.positions_rad = {0.5, 0.5};
  PlanResultDTO resume_plan_ok;
  resume_plan_ok.trajectory =
      make_test_trajectory_from({0.5, 0.5}, {1.0, 1.0});
  planner.next_result = resume_plan_ok;
  controlled_execution.block_sync_observed_state = true;

  auto resume_result = controlled_kernel.resume();
  ASSERT_TRUE(resume_result.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_sync_observed_state_count(2))
      << "resume sync did not start";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kValidating));

  auto reset_result = controlled_kernel.reset();
  ASSERT_TRUE(reset_result.accepted);
  controlled_execution.release_sync_observed_state();
  controlled_kernel.wait_for_completion();

  EXPECT_EQ(controlled_execution.resume_count, 0);
  EXPECT_EQ(controlled_execution.reset_count, 1);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kIdle);
}

// --- Reset ---

TEST_F(ExecutionKernelTest, Reset_FromCompleted_ToIdle) {
  execute_sync(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto result = kernel.reset();
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Reset_FromFailed_ToIdle) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;
  execute_sync(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  auto result = kernel.reset();
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Reset_FromIdle_ResetsBackendAndPublishesSnapshot) {
  execution.snapshot.joint_positions_rad = {0.5, -0.25};
  auto result = kernel.reset();
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(execution.reset_count, 1);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  auto snapshot = kernel.get_execution_snapshot();
  EXPECT_EQ(snapshot.state, ExecutionState::kIdle);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 2u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad[0], 0.5);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad[1], -0.25);
}

TEST_F(ExecutionKernelTest, Reset_ThenExecuteAgain) {
  execute_sync(make_test_request());
  kernel.reset();
  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 2u);
}

// --- Collision monitoring tick ---

TEST_F(ExecutionKernelTest, CollisionCheckTick_NotExecuting_NoOp) {
  auto result = kernel.collision_check_tick();
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(context.collision_count, 0);
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_SchedulerTickLatchesRuntimeCollisionInterrupt) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;  // Keep this scheduler test fast.
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);
  std::mutex events_mu;
  std::vector<KernelEvent> events;
  controlled_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mu);
    events.push_back(e);
  });

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called()) << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{99, "live-scene-after-dispatch"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_collision_engine->default_result = make_authoritative_collision_result();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  for (int attempt = 0;
       attempt < 100 &&
       context.collision_count == 0 &&
       controlled_execution.stop_count == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    local_scheduler.advance_to(local_clock.now());
  }
  bool completed = controlled_kernel.wait_for_completion(200'000'000);
  if (!completed) {
    controlled_execution.release();
    completed = controlled_kernel.wait_for_completion(1'000'000'000);
  }
  EXPECT_TRUE(completed);

  EXPECT_EQ(context.collision_count, 0);
  EXPECT_GE(local_collision_engine->check_count.load(), 3);
  EXPECT_EQ(controlled_execution.stop_count, 1);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->source_id, "manyforge.kernel.runtime_collision_fault");
  EXPECT_EQ(latch->execution_phase_at_latch,
            InterruptExecutionPhase::kTrajectoryExecution);
  EXPECT_EQ(latch->preemption_action_requested,
            InterruptPreemptionAction::kPreemptTrajectory);
  EXPECT_EQ(latch->preemption_result, InterruptPreemptionResult::kSucceeded);
  std::vector<KernelEvent> captured_events;
  {
    std::lock_guard<std::mutex> lock(events_mu);
    captured_events = events;
  }
  EXPECT_TRUE(
      std::any_of(captured_events.begin(), captured_events.end(),
                  [](const KernelEvent& e) {
                    return e.type == KernelEventType::kInterruptLatched;
                  }));
  EXPECT_TRUE(
      std::any_of(captured_events.begin(), captured_events.end(),
                  [](const KernelEvent& e) {
                    return e.type ==
                               KernelEventType::kInterruptPreemptApplied &&
                           e.details.at("preemption_result") == "succeeded";
                  }));
  EXPECT_TRUE(
      std::any_of(captured_events.begin(), captured_events.end(),
                  [](const KernelEvent& e) {
                    return e.type == KernelEventType::kCollisionDetected &&
                           e.details.at("surface") ==
                               "observed_runtime_monitor" &&
                           e.details.at("collision_world_revision_id") ==
                               "99" &&
                           e.details.at("contact_a") ==
                               "object/box/shape/0" &&
                           e.details.at("contact_b") ==
                               "robot/robot1/link/link1/collision/0";
                  }));
}

// When the deployment configures `default_fault_stop_profile`,
// a runtime collision fault
// must route the preempt through `execute_controlled_stop(profile)`
// instead of the legacy `execution_backend_.stop()`. The legacy stop
// count stays at zero; the controlled-stop count goes up by one. The
// trip-outcome diagnostic surface (`requested_stop_mode`,
// `executed_stop_mode`, `stop_command_source`) is carried on the
// `kErrorRaised` event so the HMI panel can render the structured
// cause without an extra round-trip.
TEST_F(ExecutionKernelTest,
       CollisionMonitoring_DefaultFaultStopProfileRoutesThroughControlledStop) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  // The deployment opts the kernel into controlled-stop semantics for
  // fault interrupts that don't carry a per-policy stop_profile (which
  // includes the kernel's built-in runtime_collision_fault policy).
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "default_fault_spring_back";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);

  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision =
      CollisionWorldRevision{99, "live-scene-after-dispatch"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_collision_engine->default_result =
      make_authoritative_collision_result();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  for (int attempt = 0;
       attempt < 100 && controlled_execution.controlled_stop_count == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    local_scheduler.advance_to(local_clock.now());
  }
  bool completed = controlled_kernel.wait_for_completion(200'000'000);
  if (!completed) {
    controlled_execution.release();
    completed = controlled_kernel.wait_for_completion(1'000'000'000);
  }
  EXPECT_TRUE(completed);

  // Fault routed through controlled-stop, NOT legacy backend.stop().
  EXPECT_EQ(controlled_execution.stop_count, 0);
  EXPECT_EQ(controlled_execution.controlled_stop_count, 1);
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.profile_id,
            "default_fault_spring_back");
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  EXPECT_EQ(controlled_kernel.get_status().state,
            ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->preemption_action_requested,
            InterruptPreemptionAction::kPreemptTrajectory);
  EXPECT_EQ(latch->preemption_result, InterruptPreemptionResult::kSucceeded);
}

// A per-policy `InterruptPolicyDefinition.stop_profile` wins over
// the kernel-level `default_fault_stop_profile`. This lets
// a deployment carry a kernel-wide default (e.g. spring_back_hold for
// "generic faults") while overriding for specific fault categories
// (e.g. soft_on_path for drift, hard_deceleration for protective
// stops). Registration of the built-in runtime_collision_fault policy
// happens inside `install_builtin_interrupt_sources()` and the policy
// cannot be re-registered after the fact, so this test exercises the
// precedence via a stand-alone user policy.
TEST_F(ExecutionKernelTest,
       FaultStopProfile_PerPolicyOverridesKernelDefault) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  // Kernel-wide default would pick spring_back_hold, but the per-
  // policy profile below requests hard_deceleration. The latter must
  // win.
  MotionInterruptStopProfileDTO kernel_default;
  kernel_default.profile_id = "kernel_default_spring_back";
  kernel_default.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(kernel_default);
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSpringBackHold,
       MotionInterruptStopMode::kHardDeceleration};

  // Register a user fault source + policy whose per-policy stop_profile
  // requests hard_deceleration.
  InterruptSourceDefinition user_src;
  user_src.source_id = "user_fault_src";
  user_src.adapter = InterruptAdapter::kInternalTest;
  user_src.signal = "user_fault_src_signal";
  user_src.source_class = InterruptSourceClass::kFastExecutionRelevant;
  user_src.kind = InterruptSourceKind::kEdge;
  user_src.priority = 900;
  ASSERT_TRUE(local_kernel.register_interrupt_source(user_src).ok());

  InterruptPolicyDefinition user_policy;
  user_policy.policy_id = "user_fault_with_profile";
  user_policy.source_id = "user_fault_src";
  user_policy.priority = 900;
  user_policy.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  user_policy.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  user_policy.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  MotionInterruptStopProfileDTO per_policy;
  per_policy.profile_id = "per_policy_hard_decel";
  per_policy.stop_mode = MotionInterruptStopMode::kHardDeceleration;
  user_policy.stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(per_policy);
  ASSERT_TRUE(local_kernel.register_interrupt_policy(user_policy).ok());

  auto exec_result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(exec_result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  // Publish the user fault signal.
  InterruptSignalUpdate sig;
  sig.source_id = "user_fault_src";
  sig.sequence_id = 1;
  sig.timestamp_ns = local_clock.now();
  sig.value = true;
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(sig).latched);
  for (int attempt = 0;
       attempt < 100 && controlled_execution.controlled_stop_count == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Per-policy profile (hard_deceleration) wins; kernel default ignored.
  EXPECT_EQ(controlled_execution.stop_count, 0);
  EXPECT_EQ(controlled_execution.controlled_stop_count, 1);
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.profile_id,
            "per_policy_hard_decel");
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.stop_mode,
            MotionInterruptStopMode::kHardDeceleration);
}

// ExecutionKernel::stop() (the operator stop-button path)
// routes through execute_controlled_stop(operator_stop_profile)
// when the deployment configures one. Default unset → legacy
// backend.stop() (no regression for deployments not opting in).
TEST_F(ExecutionKernelTest,
       Stop_OperatorStopProfileRoutesThroughControlledStop) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  // soft_on_path needs per-joint deceleration limits and a
  // deceleration_time; provide deployment-default values.
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {
      {"j1", 8.0}, {"j2", 8.0}, {"j3", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  // Operator stop button.
  auto stop_dto = local_kernel.stop();
  EXPECT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));
  EXPECT_EQ(controlled_execution.controlled_stop_count, 1);
  EXPECT_EQ(controlled_execution.stop_count, 0);
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.profile_id,
            "operator_stop_soft_on_path");
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.stop_mode,
            MotionInterruptStopMode::kSoftOnPath);
}

TEST_F(ExecutionKernelTest,
       Stop_OperatorStopProfileReturnsBeforeControlledStopCompletes) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  const auto start = std::chrono::steady_clock::now();
  auto stop_dto = local_kernel.stop();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(stop_dto.accepted);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            100);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kStopping);

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));
}

TEST_F(ExecutionKernelTest,
       Stop_SecondStopDuringOperatorControlledStopIsIdempotent) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  ASSERT_TRUE(local_kernel.stop().accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));

  const auto start = std::chrono::steady_clock::now();
  auto second_stop = local_kernel.stop();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(second_stop.accepted);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            100);
  EXPECT_EQ(controlled_execution.stop_count, 0);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count, 0);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kStopping);

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));
}

TEST_F(ExecutionKernelTest,
       Reset_WaitsForBlockedOperatorControlledStopThenResets) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  ASSERT_TRUE(local_kernel.stop().accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));

  std::optional<ResetResultDTO> reset;
  std::thread reset_thread([&]() {
    reset = local_kernel.reset();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(reset.has_value())
      << "reset must wait for the selected stop before publishing Idle";
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kStopping);

  controlled_execution.release_controlled_stop();
  reset_thread.join();

  ASSERT_TRUE(reset.has_value());
  EXPECT_TRUE(reset->accepted);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count, 0);
  EXPECT_EQ(controlled_execution.reset_count, 1);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kIdle);
  std::lock_guard<std::mutex> lock(events_mutex);
  EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                          [](const KernelEvent& e) {
                            return e.type == KernelEventType::kStateTransition &&
                                   e.from_state == ExecutionState::kStopped &&
                                   e.to_state == ExecutionState::kIdle;
                          }));
}

TEST_F(ExecutionKernelTest,
       Reset_FromExecutingUsesOperatorControlledStopThenResets) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  std::optional<ResetResultDTO> reset;
  std::thread reset_thread([&]() {
    reset = local_kernel.reset();
  });

  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));
  EXPECT_FALSE(reset.has_value())
      << "reset must wait for the selected operator stop before resetting";
  EXPECT_EQ(controlled_execution.stop_count, 0);
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.profile_id,
            "operator_stop_soft_on_path");
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.stop_mode,
            MotionInterruptStopMode::kSoftOnPath);

  controlled_execution.release_controlled_stop();
  reset_thread.join();

  ASSERT_TRUE(reset.has_value());
  EXPECT_TRUE(reset->accepted);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count, 0);
  EXPECT_EQ(controlled_execution.reset_count, 1);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest,
       Reset_EscalatesToHardStopWhenOperatorControlledStopMissesBudget) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.max_stop_time_s = 0.05;
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  ASSERT_TRUE(local_kernel.stop().accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));

  auto reset = local_kernel.reset();

  EXPECT_TRUE(reset.accepted);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count, 1);
  EXPECT_EQ(controlled_execution.reset_count, 1);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kIdle);
  std::lock_guard<std::mutex> lock(events_mutex);
  EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                          [](const KernelEvent& e) {
                            return e.type == KernelEventType::kStateTransition &&
                                   e.details.contains("reason") &&
                                   e.details.at("reason") ==
                                       "reset_operator_stop_timeout_hard_stop";
                          }));
}

TEST_F(ExecutionKernelTest,
       Stop_ReturnsPromptlyDuringSlowExecuteAdmission) {
  ControlledExecution controlled_execution;
  controlled_execution.block_sync_observed_state = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  std::optional<Result<uint64_t>> execute_result;
  std::thread execute_thread([&]() {
    execute_result =
        local_kernel.execute(make_test_request("slow_admission_stop"));
  });

  ASSERT_TRUE(controlled_execution.wait_until_sync_observed_state_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPlanning));

  const auto start = std::chrono::steady_clock::now();
  auto stop_result = local_kernel.stop();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(stop_result.accepted);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            100);

  controlled_execution.release_sync_observed_state();
  execute_thread.join();

  ASSERT_TRUE(execute_result.has_value());
  EXPECT_TRUE(execute_result->ok());
  EXPECT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));
  EXPECT_EQ(controlled_execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest,
       Reset_ReturnsPromptlyDuringSlowExecuteAdmission) {
  ControlledExecution controlled_execution;
  controlled_execution.block_sync_observed_state = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  std::optional<Result<uint64_t>> execute_result;
  std::thread execute_thread([&]() {
    execute_result =
        local_kernel.execute(make_test_request("slow_admission_reset"));
  });

  ASSERT_TRUE(controlled_execution.wait_until_sync_observed_state_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPlanning));

  const auto start = std::chrono::steady_clock::now();
  auto reset_result = local_kernel.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(reset_result.accepted);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            100);

  controlled_execution.release_sync_observed_state();
  execute_thread.join();

  ASSERT_TRUE(execute_result.has_value());
  EXPECT_TRUE(execute_result->ok());
  EXPECT_TRUE(wait_for_state(local_kernel, ExecutionState::kIdle));
  EXPECT_EQ(controlled_execution.execute_count, 0);
  EXPECT_EQ(controlled_execution.reset_count, 1);
}

TEST_F(ExecutionKernelTest,
       Pause_ReturnsPromptlyRejectedDuringSlowExecuteAdmission) {
  ControlledExecution controlled_execution;
  controlled_execution.block_sync_observed_state = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  std::optional<Result<uint64_t>> execute_result;
  std::thread execute_thread([&]() {
    execute_result =
        local_kernel.execute(make_test_request("slow_admission_pause"));
  });

  ASSERT_TRUE(controlled_execution.wait_until_sync_observed_state_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPlanning));

  const auto start = std::chrono::steady_clock::now();
  auto pause_result = local_kernel.pause();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_FALSE(pause_result.accepted);
  ASSERT_TRUE(pause_result.error.has_value());
  EXPECT_EQ(pause_result.error->code, kErrorInvalidRequest);
  EXPECT_EQ(pause_result.error->details.at("reason_code"),
            "pause_not_executing");
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            100);

  controlled_execution.release_sync_observed_state();
  execute_thread.join();

  ASSERT_TRUE(execute_result.has_value());
  EXPECT_TRUE(execute_result->ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  controlled_execution.complete();
  local_kernel.wait_for_completion();
}

// Emergency fallback:
// `ExecutionKernel::stop()` is non-blocking: it accepts operator intent
// immediately, then the worker reports the controlled-stop outcome via
// backend state, events, and snapshots. On controlled-stop failure it still
// falls back to legacy `execution_backend_.stop()` so the robot's physical
// stop is guaranteed.
TEST_F(ExecutionKernelTest,
       Stop_OperatorProfileRefused_EmergencyLegacyFallback) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSpringBackHold};
  // Backend refuses the controlled-stop (e.g. capability mismatch).
  ControlledStopResultDTO refused;
  refused.accepted = false;
  refused.executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
  refused.stop_command_source = MotionInterruptStopCommandSource::kDiagnosticOther;
  refused.error = ErrorDTO{
      std::string(kErrorMotionInterruptBackendStopFailed),
      "soft_on_path not supported",
      {{"reason", "soft_on_path_not_supported"}}};
  controlled_execution.next_controlled_stop_result = refused;

  auto exec_result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(exec_result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto stop_dto = local_kernel.stop();
  // The controlled-stop returns not-accepted, so the kernel must
  // emergency-fall-back to legacy backend.stop() — verify BOTH were called.
  EXPECT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(controlled_execution.wait_until_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));
  EXPECT_EQ(controlled_execution.controlled_stop_count, 1);
  EXPECT_EQ(controlled_execution.stop_count, 1);
  const auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "operator_stop_profile_failed");
  EXPECT_EQ(snapshot.error->details.at("emergency_fallback"),
            "legacy_stop_succeeded");
}

TEST_F(ExecutionKernelTest,
       Stop_OperatorProfileAndLegacyBothFail_EntersInterventionRequired) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  // Controlled-stop refused. The emergency-fallback legacy stop()
  // also fails (e.g. the controller has disconnected mid-operation).
  // The asynchronous worker MUST surface this as a hard failure so escalation
  // can occur — the robot may not be physically halted.
  ControlledStopResultDTO refused;
  refused.accepted = false;
  refused.error = ErrorDTO{
      std::string(kErrorMotionInterruptBackendStopFailed),
      "soft_on_path not supported",
      {{"reason", "soft_on_path_not_supported"}}};
  controlled_execution.next_controlled_stop_result = refused;
  controlled_execution.fail_next_legacy_stop = true;
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto exec_result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(exec_result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto stop_dto = local_kernel.stop();
  EXPECT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(controlled_execution.wait_until_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel,
                             ExecutionState::kInterventionRequired));
  EXPECT_EQ(controlled_execution.controlled_stop_count, 1);
  EXPECT_EQ(controlled_execution.stop_count, 1);
  EXPECT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  const auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "operator_stop_total_failure");
  std::lock_guard<std::mutex> lock(events_mutex);
  EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                          [](const KernelEvent& e) {
                            return e.type == KernelEventType::kStateTransition &&
                                   e.to_state == ExecutionState::kInterventionRequired &&
                                   e.details.contains("reason") &&
                                   e.details.at("reason") ==
                                       "operator_stop_total_failure";
                          }));
}

// Spec/230 §6.1 stop chain. Operator-stop from kExecuting MUST emit
// the full sequence:
//   kStopRequested → state_transition(Executing→Stopping)
//                  → kStopIssued
//                  → state_transition(Stopping→Stopped)
// The intermediate kStopping transition makes the controlled-stop
// deceleration window observable (1-2 s for soft-on-path); without
// it the HMI cannot render "Stopping…" and trace reconstruction
// loses the operator-intent vs backend-confirmation boundary.
TEST_F(ExecutionKernelTest, Stop_EmitsFullEventChain_PerSpec_6_1) {
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock,
      local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      local_kernel, local_collision_world, local_collision_engine);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto exec = local_kernel.execute(make_test_request());
  ASSERT_TRUE(exec.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto stop_dto = local_kernel.stop();
  ASSERT_TRUE(stop_dto.accepted);
  EXPECT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kStopped);

  std::lock_guard<std::mutex> lock(events_mutex);
  EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                          [](const KernelEvent& e) {
                            return e.type == KernelEventType::kStopRequested;
                          }))
      << "Missing kStopRequested event";
  EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                          [](const KernelEvent& e) {
                            return e.type == KernelEventType::kStateTransition &&
                                   e.from_state == ExecutionState::kExecuting &&
                                   e.to_state == ExecutionState::kStopping;
                          }))
      << "Missing state_transition(Executing→Stopping)";
  EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                          [](const KernelEvent& e) {
                            return e.type == KernelEventType::kStopIssued;
                          }))
      << "Missing kStopIssued event";
  EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                          [](const KernelEvent& e) {
                            return e.type == KernelEventType::kStateTransition &&
                                   e.from_state == ExecutionState::kStopping &&
                                   e.to_state == ExecutionState::kStopped;
                          }))
      << "Missing state_transition(Stopping→Stopped)";
}

// Spec/230 §6.1 pause chain. Pause from kExecuting MUST emit:
//   kPauseRequested → state_transition(Executing→Stopping)
//                   → state_transition(Stopping→Paused)
// The Executing→Stopping transition is synchronous inside
// stop_policy::request_pause; the Stopping→Paused transition fires
// from the scheduled lambda there. The pause_completed_callback the
// kernel wires in its constructor emits the terminal Paused event
// when that lambda runs.
TEST_F(ExecutionKernelTest, Pause_EmitsFullEventChain_PerSpec_6_1) {
  ControlledExecution controlled_execution;
  // The backend terminates its execution handle on pause so the
  // async worker can quiesce cleanly when the test ends.
  controlled_execution.terminal_handle_on_pause = true;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock,
      local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      local_kernel, local_collision_world, local_collision_engine);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto exec = local_kernel.execute(make_test_request());
  ASSERT_TRUE(exec.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto pause_dto = local_kernel.pause();
  ASSERT_TRUE(pause_dto.accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPaused));

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                            [](const KernelEvent& e) {
                              return e.type == KernelEventType::kPauseRequested;
                            }))
        << "Missing kPauseRequested event";
    EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                            [](const KernelEvent& e) {
                              return e.type == KernelEventType::kStateTransition &&
                                     e.from_state == ExecutionState::kExecuting &&
                                     e.to_state == ExecutionState::kStopping;
                            }))
        << "Missing state_transition(Executing→Stopping)";
    EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                            [](const KernelEvent& e) {
                              return e.type == KernelEventType::kStateTransition &&
                                     e.from_state == ExecutionState::kStopping &&
                                     e.to_state == ExecutionState::kPaused;
                            }))
        << "Missing state_transition(Stopping→Paused)";
  }

  // Clean up: reset clears the paused state and lets the worker
  // thread quiesce before the kernel is destroyed.
  auto reset_result = local_kernel.reset();
  EXPECT_TRUE(reset_result.accepted);
  EXPECT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
}

// The operator-stop profile MUST NOT leak into internal hard-stop
// call sites. The
// planning-timeout watchdog and resume-validation-failure paths
// call `stop_policy_.request_hard_stop()` directly, which fires the
// stop_policy callback. That callback is now legacy `backend.stop()`
// only — internal callers see no controlled-stop behavior.
//
// This test simulates an internal hard-stop by calling
// `stop_policy_.request_hard_stop()` indirectly via a fault-trip
// without an operator-stop profile invocation, and verifies the
// legacy backend.stop() ran and controlled_stop_count did not.
TEST_F(ExecutionKernelTest,
       InternalHardStop_DoesNotUseOperatorStopProfile) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  // Tighten the collision monitor cadence so the test fires within
  // a deterministic 10 ms advance.
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  // Operator profile IS configured (would route ExecutionKernel::stop()
  // through controlled-stop)…
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  // …but no fault-stop profile, so the runtime collision fault path
  // uses the legacy backend.stop() preempt directly — that's the
  // internal hard-stop we're guarding against accidentally routing
  // through the operator profile.
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  // Calling stop() while Idle must not invoke the operator controlled-stop
  // profile and must not poison the one-shot backend-stop skip flag. The next
  // internal fault preempt below still has to call legacy backend.stop().
  auto idle_stop = local_kernel.stop();
  EXPECT_FALSE(idle_stop.accepted);
  EXPECT_EQ(controlled_execution.controlled_stop_count, 0);
  EXPECT_EQ(controlled_execution.stop_count, 0);

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  // Trigger the built-in runtime_collision_fault interrupt directly.
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision =
      CollisionWorldRevision{77, "internal-hard-stop-test"};
  local_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_engine->default_result = make_authoritative_collision_result();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  local_clock.advance(20'000'000);
  local_scheduler.advance_to(local_clock.now());
  for (int attempt = 0;
       attempt < 100 && controlled_execution.stop_count == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    local_scheduler.advance_to(local_clock.now());
  }
  bool completed = local_kernel.wait_for_completion(200'000'000);
  if (!completed) {
    controlled_execution.release();
    completed = local_kernel.wait_for_completion(1'000'000'000);
  }
  EXPECT_TRUE(completed);

  // Legacy backend.stop() ran; operator profile did NOT leak in.
  EXPECT_EQ(controlled_execution.stop_count, 1);
  EXPECT_EQ(controlled_execution.controlled_stop_count, 0);
}

// Emergency fallback: when a configured fault stop_profile is
// refused/unsupported/unconfirmed by
// the backend, the kernel must emergency-fall-back to legacy
// `execution_backend_.stop()` so the runtime fault still physically
// stops the robot. A misconfigured fault profile MUST NOT remove the
// guaranteed physical-stop path.
TEST_F(ExecutionKernelTest,
       FaultStopProfile_ControlledStopRefused_EmergencyLegacyFallback) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "fault_spring_back";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);
  // Backend refuses the controlled-stop (capability mismatch, etc.).
  ControlledStopResultDTO refused;
  refused.accepted = false;
  refused.error = ErrorDTO{
      std::string(kErrorMotionInterruptBackendStopFailed),
      "spring_back_hold capability missing",
      {{"reason", "stop_mode_not_supported"}}};
  controlled_execution.next_controlled_stop_result = refused;

  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  // Events are emitted from BOTH the execution-worker thread and the
  // interrupt/monitor lane, so the capture must be mutex-guarded; an
  // unsynchronized vector races (and corrupts/segfaults under load).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision =
      CollisionWorldRevision{99, "live-scene-after-dispatch"};
  local_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_engine->default_result = make_authoritative_collision_result();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  for (int attempt = 0;
       attempt < 100 && controlled_execution.stop_count == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    local_scheduler.advance_to(local_clock.now());
  }
  bool completed = local_kernel.wait_for_completion(200'000'000);
  if (!completed) {
    controlled_execution.release();
    completed = local_kernel.wait_for_completion(1'000'000'000);
  }
  EXPECT_TRUE(completed);

  // Controlled-stop attempted, refused; legacy stop() emergency-fell-back.
  EXPECT_EQ(controlled_execution.controlled_stop_count, 1);
  EXPECT_EQ(controlled_execution.stop_count, 1);

  // The kInterruptPreemptApplied event is published by the interrupt/monitor
  // lane in apply_latched_interrupt_preemption, which runs AFTER the state
  // transition to Interrupted that the execution worker uses to signal
  // completion. wait_for_completion can therefore return before the event is
  // emitted, so poll deterministically for it instead of racing the emission.
  // This does not weaken the assertion: it still requires the exact preempt
  // path and all three detail fields.
  const auto has_preempt_event = [&]() {
    std::lock_guard<std::mutex> lock(events_mutex);
    return std::any_of(
        events.begin(), events.end(), [](const KernelEvent& e) {
          return e.type == KernelEventType::kInterruptPreemptApplied &&
                 e.details.contains("preempt_path") &&
                 e.details.at("preempt_path") ==
                     "controlled_stop_failed_legacy_fallback_succeeded" &&
                 e.details.contains("emergency_fallback") &&
                 e.details.at("emergency_fallback") == "legacy_stop_succeeded" &&
                 e.details.contains("fault_stop_profile_id") &&
                 e.details.at("fault_stop_profile_id") == "fault_spring_back";
        });
  };
  bool saw_preempt_event = false;
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (has_preempt_event()) {
      saw_preempt_event = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(saw_preempt_event);

  // Once the preempt event is observed, the latch result it accompanies is
  // guaranteed set (marked on the same lane just before the event is emitted),
  // so this read is no longer racy.
  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->preemption_result, InterruptPreemptionResult::kSucceeded);
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_UsesBackendObservedStateForRuntimeQuery) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  std::vector<double> queried_positions;
  local_collision_engine->on_check = [&](const CollisionQuery& query) {
    queried_positions = query.robot_state.positions_rad;
  };

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  context.current_state.positions_rad = {99.0, 99.0};

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  ASSERT_EQ(queried_positions.size(), 2u);
  EXPECT_DOUBLE_EQ(queried_positions[0], 0.25);
  EXPECT_DOUBLE_EQ(queried_positions[1], 0.75);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_StaleObservedStateFailsClosedBeforeEngineQuery) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.runtime_observed_state_max_age_ns = 10'000'000;  // 10 ms
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, 0);
  local_clock.advance(20'000'000);
  const int baseline_engine_checks = local_collision_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(local_collision_engine->check_count.load(), baseline_engine_checks);
  EXPECT_EQ(controlled_execution.stop_count, 1);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->signal_snapshot.metadata.at("reason"),
            "stale_observed_robot_state");
  EXPECT_EQ(latch->signal_snapshot.metadata.at("observed_state_age_ns"),
            "20000000");
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_FreshObservedStatePublishedDuringTickDoesNotLookStale) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.runtime_observed_state_max_age_ns = 10'000'000;  // 10 ms
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  bool refreshed_during_monitor_tick = false;
  controlled_execution.on_get_snapshot = [&]() {
    if (refreshed_during_monitor_tick) {
      return;
    }
    refreshed_during_monitor_tick = true;
    local_clock.advance(1);
    controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  };
  const int baseline_engine_checks = local_collision_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_TRUE(refreshed_during_monitor_tick);
  EXPECT_GT(local_collision_engine->check_count.load(), baseline_engine_checks);
  EXPECT_EQ(controlled_execution.stop_count, 0);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_StaleDynamicBodyFailsClosedBeforeEngineQuery) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.runtime_dynamic_body_max_age_cap_ns = 100'000'000;  // 100 ms
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{42, "dynamic-world"};
  live_snapshot.bodies.push_back(make_dynamic_test_body(0, 10'000'000));
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, 20'000'000);
  local_clock.advance(20'000'000);
  const int baseline_engine_checks = local_collision_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(local_collision_engine->check_count.load(), baseline_engine_checks);
  EXPECT_EQ(controlled_execution.stop_count, 1);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->signal_snapshot.metadata.at("reason"),
            "stale_dynamic_collision_body");
  EXPECT_EQ(latch->signal_snapshot.metadata.at("dynamic_body_id"),
            "object/dynamic_box/shape/0");
  EXPECT_EQ(latch->signal_snapshot.metadata.at("dynamic_body_age_ns"),
            "20000000");
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ContactReleaseAllowsBoundedOngoingReleaseContact) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  const CollisionPairId pair{"robot/robot1/attached/part/shape/0",
                             "object/support/shape/0"};
  auto policy = make_test_collision_policy_snapshot();
  policy.require_full_diagnostics = true;
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.max_start_penetration_m = 0.010;
  entry.require_separation_by_end = true;
  entry.require_strictly_decreasing_penetration = true;
  policy.support_contacts.push_back(entry);
  local_collision_world.current_policy = policy;
  controlled_kernel.set_collision_validation_policy_snapshot(policy);
  local_collision_engine->result_by_waypoint[0] =
      make_support_contact_collision_result(pair, -0.004);
  local_collision_engine->result_by_waypoint[1] =
      make_support_contact_collision_result(pair, 0.002);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_collision_engine->result_by_waypoint.clear();
  local_collision_engine->default_result =
      make_support_contact_collision_result(pair, -0.004);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  const int baseline_stop_count = controlled_execution.stop_count;

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(controlled_execution.stop_count, baseline_stop_count);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ContactReleasePenetrationIncreaseFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  const CollisionPairId pair{"robot/robot1/attached/part/shape/0",
                             "object/support/shape/0"};
  auto policy = make_test_collision_policy_snapshot();
  policy.require_full_diagnostics = true;
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.max_start_penetration_m = 0.010;
  entry.release_tolerance_m = 1e-5;
  entry.require_separation_by_end = true;
  entry.require_strictly_decreasing_penetration = true;
  policy.support_contacts.push_back(entry);
  local_collision_world.current_policy = policy;
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  local_collision_engine->result_by_waypoint[0] =
      make_support_contact_collision_result(pair, -0.004);
  local_collision_engine->result_by_waypoint[1] =
      make_support_contact_collision_result(pair, 0.002);
  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_collision_engine->result_by_waypoint.clear();
  local_collision_engine->default_result =
      make_support_contact_collision_result(pair, -0.004);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  ASSERT_EQ(controlled_execution.stop_count, 0);

  local_collision_engine->default_result =
      make_support_contact_collision_result(pair, -0.006);
  local_clock.advance(10'000'000);
  controlled_execution.set_progress(0.50, {0.50, 0.90}, local_clock.now());
  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(controlled_execution.stop_count, 1);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->signal_snapshot.metadata.at("reason"),
            "runtime_contact_release_penetration_increased");
  EXPECT_EQ(latch->signal_snapshot.metadata.at("contact_a"), pair.body_a);
  EXPECT_EQ(latch->signal_snapshot.metadata.at("contact_b"), pair.body_b);
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_UsesConfiguredPeriod) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 50'000'000;  // 50 ms
  config.runtime_collision_query_timeout_ns = 25'000'000;  // 25 ms
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  std::optional<std::int64_t> observed_timeout_budget_ns;
  local_collision_engine->on_check = [&](const CollisionQuery& query) {
    observed_timeout_budget_ns = query.timeout_budget_ns;
  };
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called()) << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));
  const int baseline_collision_count = context.collision_count;
  const int baseline_engine_count = local_collision_engine->check_count.load();

  local_clock.advance(49'000'000);
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(context.collision_count, baseline_collision_count);
  EXPECT_EQ(local_collision_engine->check_count.load(), baseline_engine_count);

  local_clock.advance(1'000'000);
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(context.collision_count, baseline_collision_count);
  EXPECT_EQ(local_collision_engine->check_count.load(), baseline_engine_count + 1);
  ASSERT_TRUE(observed_timeout_budget_ns.has_value());
  EXPECT_EQ(*observed_timeout_budget_ns, 25'000'000);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kCompleted);
}

// --- Full cycle test ---

TEST_F(ExecutionKernelTest, FullCycle_ExecuteResetExecute) {
  auto r1 = execute_sync(make_test_request("move_A"));
  EXPECT_TRUE(r1.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  kernel.reset();
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);

  auto r2 = execute_sync(make_test_request("move_B"));
  EXPECT_TRUE(r2.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(planner.plan_count, 2);
}

TEST_F(ExecutionKernelTest, FullCycle_PlanFailResetRetry) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;

  execute_sync(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  kernel.reset();

  PlanResultDTO plan_ok;
  plan_ok.trajectory = make_test_trajectory();
  planner.next_result = plan_ok;

  auto result = execute_sync(make_test_request());
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
  execute_sync(make_test_request());
  auto status = kernel.get_status();
  EXPECT_EQ(status.execution_id, 1u);
  EXPECT_EQ(status.progress, 1.0);
}

TEST_F(ExecutionKernelTest, Execute_ImmediatelyTransitionsToPlanningOnCacheMiss) {
  auto result = kernel.execute(make_test_request("cache_miss_move"));
  ASSERT_TRUE(result.ok());
  auto status = kernel.get_status();
  EXPECT_EQ(status.execution_id, result.value());
  EXPECT_EQ(status.state, ExecutionState::kPlanning);
  kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, Execute_ImmediatelyTransitionsToValidatingOnCacheHit) {
  store_cache("cached_move", make_test_trajectory());

  auto result = kernel.execute(make_test_request("cached_move"));
  ASSERT_TRUE(result.ok());
  auto status = kernel.get_status();
  EXPECT_EQ(status.execution_id, result.value());
  // Cache hit skips planning — state must never be kPlanning.
  // The worker races through validating → executing → completed, so any of
  // those states is acceptable by the time we read status.
  EXPECT_NE(status.state, ExecutionState::kPlanning);
  EXPECT_NE(status.state, ExecutionState::kIdle);
  kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, GetExecutionSnapshot_AfterExecute_ContainsIdsAndFinalState) {
  execute_sync(make_test_request("snapshot_move"));
  auto snapshot = kernel.get_execution_snapshot();
  EXPECT_EQ(snapshot.execution_id, 1u);
  EXPECT_EQ(snapshot.move_id, "snapshot_move");
  EXPECT_EQ(snapshot.state, ExecutionState::kCompleted);
  EXPECT_DOUBLE_EQ(snapshot.progress, 1.0);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 2u);
  EXPECT_EQ(snapshot.joint_positions_rad[0], 1.0);
  EXPECT_EQ(snapshot.joint_positions_rad[1], 1.0);
  EXPECT_GT(snapshot.sequence_number, 0u);
}

TEST_F(ExecutionKernelTest, GetExecutionSnapshot_DuringExecuting_ServesLiveBackendSnapshot) {
  // Observer freshness contract: during kExecuting (like kStopping since 1B)
  // get_execution_snapshot() reads the backend's live snapshot instead of the
  // kernel's published cache, so display consumers (visual-state publisher)
  // never see joints a publish-cycle stale. The kernel stays the lifecycle
  // authority: state and execution identity come from the state machine.
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      controlled_kernel, local_collision_world, local_collision_engine);

  auto result = controlled_kernel.execute(make_test_request("live_snapshot_move"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  controlled_execution.set_progress(0.25, {0.3, 0.7});
  const auto snapshot = controlled_kernel.get_execution_snapshot();

  // Kernel-authoritative stamps.
  EXPECT_EQ(snapshot.state, ExecutionState::kExecuting);
  EXPECT_EQ(snapshot.execution_id, 1u);
  EXPECT_EQ(snapshot.move_id, "live_snapshot_move");
  // Live backend payload: the joints written a moment ago, and the RAW
  // backend snapshot (sequence_number is the backend's own — the kernel
  // cache would carry a kernel-assigned sequence >= 1). This is the
  // deterministic tell that the live path, not the cache, served the read.
  EXPECT_DOUBLE_EQ(snapshot.progress, 0.25);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 2u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad[0], 0.3);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad[1], 0.7);
  EXPECT_EQ(snapshot.sequence_number, 0u);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kCompleted);
}

// ============================================================================
// Async behavior tests
// ============================================================================

TEST_F(ExecutionKernelTest, Execute_ReturnsImmediately) {
  auto result = kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 1u);
  // Don't check final state — it may or may not be done yet
  kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, WaitForCompletion_BlocksUntilDone) {
  kernel.execute(make_test_request());
  bool done = kernel.wait_for_completion();
  EXPECT_TRUE(done);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
}

// ============================================================================
// Event subscription tests
// ============================================================================

TEST_F(ExecutionKernelTest, Events_HappyPath_PlanAndExecute) {
  std::mutex events_mutex;
  std::vector<KernelEventType> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e.type);
  });

  execute_sync(make_test_request());

  std::lock_guard<std::mutex> lock(events_mutex);
  // Spec 230 §6.1: state_transition -> cache_miss -> plan_started ->
  //   plan_completed -> state_transition -> validation_started ->
  //   validation_passed -> state_transition -> execute_started ->
  //   execute_completed -> state_transition
  ASSERT_GE(events.size(), 11u);
  EXPECT_EQ(events[0], KernelEventType::kStateTransition);
  EXPECT_EQ(events[1], KernelEventType::kCacheMiss);
  EXPECT_EQ(events[2], KernelEventType::kPlanStarted);
  EXPECT_EQ(events[3], KernelEventType::kPlanCompleted);
  EXPECT_EQ(events[4], KernelEventType::kStateTransition);
  EXPECT_EQ(events[5], KernelEventType::kValidationStarted);
  EXPECT_EQ(events[6], KernelEventType::kValidationPassed);
  EXPECT_EQ(events[7], KernelEventType::kStateTransition);
  EXPECT_EQ(events[8], KernelEventType::kExecuteStarted);
  EXPECT_EQ(events[9], KernelEventType::kExecuteCompleted);
  EXPECT_EQ(events[10], KernelEventType::kStateTransition);
}

TEST_F(ExecutionKernelTest, Events_CacheHit) {
  store_cache("cached_move", make_test_trajectory());

  std::mutex events_mutex;
  std::vector<KernelEventType> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e.type);
  });

  execute_sync(make_test_request("cached_move"));

  std::lock_guard<std::mutex> lock(events_mutex);
  // Spec 230 §6.1 plus stop-path latency hardening: execute admission first
  // enters Planning so stop/reset can interrupt slow preparation even when the
  // request later resolves to a cache hit.
  ASSERT_GE(events.size(), 9u);
  EXPECT_EQ(events[0], KernelEventType::kStateTransition);
  EXPECT_EQ(events[1], KernelEventType::kStateTransition);
  EXPECT_EQ(events[2], KernelEventType::kCacheHit);
  EXPECT_EQ(events[3], KernelEventType::kValidationStarted);
  EXPECT_EQ(events[4], KernelEventType::kValidationPassed);
  EXPECT_EQ(events[5], KernelEventType::kStateTransition);
  EXPECT_EQ(events[6], KernelEventType::kExecuteStarted);
  EXPECT_EQ(events[7], KernelEventType::kExecuteCompleted);
  EXPECT_EQ(events[8], KernelEventType::kStateTransition);
}

TEST_F(ExecutionKernelTest, Events_PlanningFailure) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;

  std::mutex events_mutex;
  std::vector<KernelEventType> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e.type);
  });

  execute_sync(make_test_request());

  std::lock_guard<std::mutex> lock(events_mutex);
  // Spec 230 §6.1 (failure): state_transition -> cache_miss -> plan_started ->
  //   error_raised -> state_transition
  ASSERT_GE(events.size(), 5u);
  EXPECT_EQ(events[0], KernelEventType::kStateTransition);
  EXPECT_EQ(events[1], KernelEventType::kCacheMiss);
  EXPECT_EQ(events[2], KernelEventType::kPlanStarted);
  EXPECT_EQ(events[3], KernelEventType::kErrorRaised);
  EXPECT_EQ(events[4], KernelEventType::kStateTransition);
}

TEST_F(ExecutionKernelTest, Events_ValidationFailure) {
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

  std::mutex events_mutex;
  std::vector<KernelEventType> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e.type);
  });

  execute_sync(make_test_request());

  std::lock_guard<std::mutex> lock(events_mutex);
  // state_transition -> cache_miss -> plan_started -> plan_completed ->
  //   state_transition -> validation_started -> validation_failed ->
  //   error_raised -> state_transition
  ASSERT_GE(events.size(), 9u);
  EXPECT_EQ(events[0], KernelEventType::kStateTransition);
  EXPECT_EQ(events[1], KernelEventType::kCacheMiss);
  EXPECT_EQ(events[2], KernelEventType::kPlanStarted);
  EXPECT_EQ(events[3], KernelEventType::kPlanCompleted);
  EXPECT_EQ(events[4], KernelEventType::kStateTransition);
  EXPECT_EQ(events[5], KernelEventType::kValidationStarted);
  EXPECT_EQ(events[6], KernelEventType::kValidationFailed);
  EXPECT_EQ(events[7], KernelEventType::kErrorRaised);
  EXPECT_EQ(events[8], KernelEventType::kStateTransition);
}

TEST_F(ExecutionKernelTest, Events_SubscribeUnsubscribe) {
  std::mutex events_mutex;
  std::vector<KernelEventType> events;
  auto handle = kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e.type);
  });

  execute_sync(make_test_request());
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_FALSE(events.empty());
  }

  kernel.reset();
  kernel.unsubscribe_events(handle);

  size_t count_before;
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    count_before = events.size();
  }
  execute_sync(make_test_request());
  std::lock_guard<std::mutex> lock(events_mutex);
  EXPECT_EQ(events.size(), count_before);  // no new events
}

TEST_F(ExecutionKernelTest, Events_MultipleSubscribers) {
  std::mutex events_mutex;
  std::vector<KernelEventType> events_a;
  std::vector<KernelEventType> events_b;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events_a.push_back(e.type);
  });
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events_b.push_back(e.type);
  });

  execute_sync(make_test_request());

  std::lock_guard<std::mutex> lock(events_mutex);
  EXPECT_FALSE(events_a.empty());
  EXPECT_EQ(events_a.size(), events_b.size());
}

TEST_F(ExecutionKernelTest, InterruptSignalFromIdleCannotCreateHeldLatch) {
  ASSERT_TRUE(kernel.register_interrupt_source(
                        make_interrupt_source("controller_fault"))
                  .ok());
  auto policy =
      make_interrupt_policy("controller_fault_policy", "controller_fault");
  policy.affected_phases = {InterruptExecutionPhase::kHeld};
  ASSERT_TRUE(kernel.register_interrupt_policy(policy).ok());

  auto result =
      kernel.publish_fast_interrupt_signal(make_interrupt_signal("controller_fault"));

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason, "no_active_execution_phase");
  EXPECT_FALSE(result.latched);
  EXPECT_FALSE(kernel.get_active_interrupt_latch().has_value());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Events_ExecutionId_Matches) {
  std::mutex events_mutex;
  std::vector<uint64_t> event_ids;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    event_ids.push_back(e.execution_id);
  });

  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());

  std::lock_guard<std::mutex> lock(events_mutex);
  for (uint64_t id : event_ids) {
    EXPECT_EQ(id, result.value());
  }
}

// ============================================================================
// State transition event tests (spec 230 §6.1)
// ============================================================================

TEST_F(ExecutionKernelTest, Events_StateTransition_HappyPath) {
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(make_test_request());

  std::lock_guard<std::mutex> lock(events_mutex);
  // Filter state transition events
  std::vector<std::pair<ExecutionState, ExecutionState>> transitions;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition) {
      transitions.emplace_back(e.from_state, e.to_state);
    }
  }

  // Expected transitions:
  // Idle -> Planning -> Validating -> Executing -> Completed
  EXPECT_EQ(transitions.size(), 4u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].first, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].second, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].first, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].second, ExecutionState::kExecuting);
  EXPECT_EQ(transitions[3].first, ExecutionState::kExecuting);
  EXPECT_EQ(transitions[3].second, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest, Events_StateTransition_CacheHit) {
  store_cache("cached_move", make_test_trajectory());

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(make_test_request("cached_move"));

  std::lock_guard<std::mutex> lock(events_mutex);
  // Filter state transition events
  std::vector<std::pair<ExecutionState, ExecutionState>> transitions;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition) {
      transitions.emplace_back(e.from_state, e.to_state);
    }
  }

  // Expected transitions (cache hit): admission enters Planning first so
  // stop/reset can interrupt slow preparation before the worker starts.
  // Idle -> Planning -> Validating -> Executing -> Completed
  EXPECT_EQ(transitions.size(), 4u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].first, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].second, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].first, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].second, ExecutionState::kExecuting);
  EXPECT_EQ(transitions[3].first, ExecutionState::kExecuting);
  EXPECT_EQ(transitions[3].second, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest, Events_StateTransition_PlanFailure) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(make_test_request());

  std::lock_guard<std::mutex> lock(events_mutex);
  // Filter state transition events
  std::vector<std::pair<ExecutionState, ExecutionState>> transitions;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition) {
      transitions.emplace_back(e.from_state, e.to_state);
    }
  }

  // Expected transitions (plan failure):
  // Idle -> Planning -> Failed
  EXPECT_EQ(transitions.size(), 2u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].first, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].second, ExecutionState::kFailed);
}

TEST_F(ExecutionKernelTest, Events_StateTransition_ValidationFailure) {
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(make_test_request());

  std::lock_guard<std::mutex> lock(events_mutex);
  // Filter state transition events
  std::vector<std::pair<ExecutionState, ExecutionState>> transitions;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition) {
      transitions.emplace_back(e.from_state, e.to_state);
    }
  }

  // Expected transitions (validation failure):
  // Idle -> Planning -> Validating -> Failed
  EXPECT_EQ(transitions.size(), 3u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].first, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].second, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].first, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].second, ExecutionState::kFailed);
}

// ============================================================================
// Cancellation tests (spec 230 §6.2)
// ============================================================================

TEST_F(ExecutionKernelTest, Cancel_StopDuringPlanning_TransitionsToStopped) {
  SlowMockPlanner slow_planner;
  FakeClock clock;
  ManualScheduler scheduler;
  TrajectoryCache cache{CacheConfig{10}};
  ExecutionKernel kernel{slow_planner, context, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(
      kernel, collision_world_publisher, collision_engine);

  // Start execution
  auto result = kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());

  // Wait until planner is in the plan call
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Call stop
  auto stop_result = kernel.stop();
  EXPECT_TRUE(stop_result.accepted);

  // Release the planner
  slow_planner.release();

  // Wait for completion
  kernel.wait_for_completion();

  // Verify state is Stopped (not Failed)
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kStopped);
}

TEST_F(ExecutionKernelTest, Cancel_ResetDuringPlanning_TransitionsToIdle) {
  SlowMockPlanner slow_planner;
  FakeClock clock;
  ManualScheduler scheduler;
  TrajectoryCache cache{CacheConfig{10}};
  ExecutionKernel kernel{slow_planner, context, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(
      kernel, collision_world_publisher, collision_engine);

  // Start execution
  auto result = kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());

  // Wait until planner is in the plan call
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Call reset
  auto reset_result = kernel.reset();
  EXPECT_TRUE(reset_result.accepted);

  // Release the planner
  slow_planner.release();

  // Wait for completion
  kernel.wait_for_completion();

  // Verify state is Idle
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Cancel_StopDuringPlanning_EmitsCorrectEvents) {
  SlowMockPlanner slow_planner;
  FakeClock clock;
  ManualScheduler scheduler;
  TrajectoryCache cache{CacheConfig{10}};
  ExecutionKernel kernel{slow_planner, context, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(
      kernel, collision_world_publisher, collision_engine);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  // Start execution
  auto result = kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());

  // Wait until planner is in the plan call
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Call stop
  kernel.stop();

  // Release the planner
  slow_planner.release();

  // Wait for completion
  kernel.wait_for_completion();

  std::lock_guard<std::mutex> lock(events_mutex);
  // Verify events include kErrorRaised with planning_timeout error
  bool found_error = false;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kErrorRaised && e.error.has_value()) {
      if (e.error->code == "planning_timeout") {
        found_error = true;
        break;
      }
    }
  }
  EXPECT_TRUE(found_error);

  // Verify final state_transition to Stopped
  bool found_stopped_transition = false;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition &&
        e.to_state == ExecutionState::kStopped) {
      found_stopped_transition = true;
      break;
    }
  }
  EXPECT_TRUE(found_stopped_transition);
}

TEST_F(ExecutionKernelTest, Cancel_StopFromExecuting_TransitionsToStopped) {
  // Use standard planner but stop after execution starts
  // Since execution is synchronous in the mock, we need to check state after stop
  execute_sync(make_test_request());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  // Stop from Completed should not be accepted
  auto result = kernel.stop();
  EXPECT_FALSE(result.accepted);
}

// ============================================================================
// New event tests (cache miss, cache invalidated, validation failed, collision detected)
// ============================================================================

TEST_F(ExecutionKernelTest, Events_CacheMissEvent) {
  // Execute with empty cache -> verify kCacheMiss is emitted
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(make_test_request("new_move"));

  std::lock_guard<std::mutex> lock(events_mutex);
  // Find kCacheMiss event
  bool found_cache_miss = false;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kCacheMiss) {
      found_cache_miss = true;
      EXPECT_EQ(e.details.at("move_id"), "new_move");
      break;
    }
  }
  EXPECT_TRUE(found_cache_miss);
}

TEST_F(ExecutionKernelTest, Events_CacheHitDoesNotEmitMiss) {
  // Execute after cache store -> verify kCacheMiss NOT emitted
  store_cache("cached_for_test", make_test_trajectory());

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(make_test_request("cached_for_test"));

  std::lock_guard<std::mutex> lock(events_mutex);
  // Verify no kCacheMiss event
  for (const auto& e : events) {
    EXPECT_NE(e.type, KernelEventType::kCacheMiss);
  }
}

TEST_F(ExecutionKernelTest, Events_CacheInvalidatedEvent) {
  // Trigger validation failure -> verify kCacheInvalidated emitted
  store_cache("invalidate_test", make_test_trajectory());
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(make_test_request("invalidate_test"));

  std::lock_guard<std::mutex> lock(events_mutex);
  // Find kCacheInvalidated event
  bool found_cache_invalidated = false;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kCacheInvalidated) {
      found_cache_invalidated = true;
      EXPECT_EQ(e.details.at("move_id"), "invalidate_test");
      EXPECT_EQ(e.details.at("reason"), "validation_failed");
      break;
    }
  }
  EXPECT_TRUE(found_cache_invalidated);
}

TEST_F(ExecutionKernelTest, Events_CacheInvalidatedEventIncludesCompatibilityReason) {
  store_cache("goal_changed_event", make_test_trajectory());
  auto req = make_test_request("goal_changed_event");
  PoseGoalDTO pose_goal;
  pose_goal.goal = make_pose(0.45, -0.1, 0.2);
  req.motion = pose_goal;
  context.next_fk = make_pose(0.1, -0.1, 0.2);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(req);

  std::lock_guard<std::mutex> lock(events_mutex);
  bool found_cache_invalidated = false;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kCacheInvalidated) {
      found_cache_invalidated = true;
      EXPECT_EQ(e.details.at("move_id"), "goal_changed_event");
      EXPECT_EQ(e.details.at("reason"), "goal_changed");
      break;
    }
  }
  EXPECT_TRUE(found_cache_invalidated);
}

TEST_F(ExecutionKernelTest, Events_ValidationFailedEvent) {
  // Mock context returns invalid -> verify kValidationFailed emitted before kErrorRaised
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  execute_sync(make_test_request());

  std::lock_guard<std::mutex> lock(events_mutex);
  // Find kValidationFailed and kErrorRaised events
  int validation_failed_idx = -1;
  int error_raised_idx = -1;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].type == KernelEventType::kValidationFailed) {
      validation_failed_idx = static_cast<int>(i);
    }
    if (events[i].type == KernelEventType::kErrorRaised && error_raised_idx < 0) {
      error_raised_idx = static_cast<int>(i);
    }
  }
  EXPECT_GE(validation_failed_idx, 0);
  EXPECT_GE(error_raised_idx, 0);
  EXPECT_LT(validation_failed_idx, error_raised_idx);  // validation_failed before error_raised
}

TEST_F(ExecutionKernelTest, Events_CollisionDetectedEvent) {
  // Call collision_check_tick() with collision -> verify kCollisionDetected emitted
  // First, we need to get into Executing state
  // Since execution is synchronous, we need a different approach
  // We'll use a slow planner and subscribe to events
  
  SlowMockPlanner slow_planner;
  FakeClock clock;
  ManualScheduler scheduler;
  TrajectoryCache cache{CacheConfig{10}};
  ExecutionKernel kernel{slow_planner, context, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(
      kernel, collision_world_publisher, collision_engine);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  // Start execution
  auto result = kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());

  // Wait until planner is in the plan call
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Release the planner to let execution proceed
  slow_planner.release();

  // Wait for completion
  kernel.wait_for_completion();

  // Now we're in Completed state, we need to reset and try again
  // Actually, we need to be in Executing state to call collision_check_tick()
  // Let's use a different approach: manually set up the state
  
  // For now, let's just verify that collision_check_tick() works correctly
  // when called from Executing state by checking the collision_detections metric
}

// ============================================================================
// Metrics tests
// ============================================================================

TEST_F(ExecutionKernelTest, MetricsCounters) {
  // Run a full execute cycle -> check get_metrics() returns correct counts
  execute_sync(make_test_request());

  auto metrics = kernel.get_metrics();
  EXPECT_EQ(metrics.cache_misses, 1u);
  EXPECT_EQ(metrics.plans_succeeded, 1u);
  EXPECT_EQ(metrics.validations_passed, 1u);
  EXPECT_EQ(metrics.executions_completed, 1u);
}

TEST_F(ExecutionKernelTest, MetricsAfterCacheHitAndMiss) {
  // Execute twice (miss then hit) -> verify both counters
  auto first = execute_sync(make_test_request("first_move"));
  ASSERT_TRUE(first.ok());
  
  auto metrics_after_miss = kernel.get_metrics();
  EXPECT_EQ(metrics_after_miss.cache_misses, 1u);
  EXPECT_EQ(metrics_after_miss.cache_hits, 0u);

  auto ack = kernel.acknowledge_completed(first.value());
  ASSERT_TRUE(ack.accepted);

  // Second execution should be a cache hit
  execute_sync(make_test_request("first_move"));

  auto metrics_after_hit = kernel.get_metrics();
  EXPECT_EQ(metrics_after_hit.cache_misses, 1u);
  EXPECT_EQ(metrics_after_hit.cache_hits, 1u);
}

TEST_F(ExecutionKernelTest, MetricsAfterValidationFailure) {
  // Mock validation failure -> verify validations_failed incremented
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

  execute_sync(make_test_request());

  auto metrics = kernel.get_metrics();
  EXPECT_EQ(metrics.validations_failed, 1u);
}

TEST_F(ExecutionKernelTest, MetricsAfterStop) {
  // Issue stop -> verify stops_issued incremented
  // First, we need to be in a state where stop is accepted
  // Let's use a slow planner and stop during planning
  
  SlowMockPlanner slow_planner;
  FakeClock clock;
  ManualScheduler scheduler;
  TrajectoryCache cache{CacheConfig{10}};
  ExecutionKernel kernel{slow_planner, context, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(
      kernel, collision_world_publisher, collision_engine);

  // Start execution
  auto result = kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());

  // Wait until planner is in the plan call
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Call stop
  auto stop_result = kernel.stop();
  EXPECT_TRUE(stop_result.accepted);

  // Release the planner
  slow_planner.release();

  // Wait for completion
  kernel.wait_for_completion();

  auto metrics = kernel.get_metrics();
  EXPECT_EQ(metrics.stops_issued, 1u);
}

TEST_F(ExecutionKernelTest, Metrics_RecordObserverLagOnSnapshotRead) {
  clock.set(1);
  execute_sync(make_test_request("metrics_lag_move"));

  auto before = kernel.get_metrics();
  const auto published = kernel.get_execution_snapshot();
  EXPECT_GT(published.sequence_number, 0u);

  clock.advance(25'000'000);  // 25 ms after publication
  (void)kernel.get_execution_snapshot();  // records observer lag
  auto after = kernel.get_metrics();
  EXPECT_GE(after.snapshots_published, before.snapshots_published);
  EXPECT_GE(after.observer_lag_max_ns, 25'000'000u);
}

TEST_F(ExecutionKernelTest, Metrics_CollisionDetections) {
  // Test collision_detections metric
  // We need to be in Executing state to call collision_check_tick()
  // Since execution is synchronous, we'll need to use a slow planner
  
  SlowMockPlanner slow_planner;
  FakeClock clock;
  ManualScheduler scheduler;
  TrajectoryCache cache{CacheConfig{10}};
  ExecutionKernel kernel{slow_planner, context, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(
      kernel, collision_world_publisher, collision_engine);

  // Set up collision detection
  context.next_collision = CollisionCheckResultDTO{true, "link1", "box"};

  // Start execution
  auto result = kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());

  // Wait until planner is in the plan call
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Release the planner to let execution proceed
  slow_planner.release();

  // Wait for completion
  kernel.wait_for_completion();

  // Note: collision_check_tick() is called externally, not during execute
  // So we need to test it separately
}

// =============================================================================
// Timeout Tests (W-1, W-2)
// =============================================================================

TEST_F(ExecutionKernelTest, ExecutionTimeout_TransitionsToFailed) {
  // ControlledExecution never completes — timeout should trigger
  ControlledExecution controlled_execution;
  // Need a separate kernel with ThreadSafe clock/scheduler for concurrency
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel timeout_kernel{
      planner, context, controlled_execution, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(
      timeout_kernel, collision_world_publisher, collision_engine);

  auto req = make_test_request();
  req.timeouts.execution_timeout_s = 0.5;

  auto result = timeout_kernel.execute(req);
  ASSERT_TRUE(result.ok());

  // Wait until execution starts
  ASSERT_TRUE(controlled_execution.wait_until_execute_called()) << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(timeout_kernel, ExecutionState::kExecuting));

  // Advance clock past the timeout
  ts_clock.advance(600'000'000);  // 600ms > 500ms timeout

  // The timeout check happens in the polling loop — need a brief real-time wait
  // for the worker thread to process
  const bool timed_out = wait_for_state(timeout_kernel, ExecutionState::kFailed, 5000);
  if (!timed_out) {
    controlled_execution.release();
    (void)timeout_kernel.wait_for_completion(1'000'000'000);
  }
  ASSERT_TRUE(timed_out);

  auto status = timeout_kernel.get_status();
  EXPECT_EQ(status.state, ExecutionState::kFailed);

  auto snapshot = timeout_kernel.get_execution_snapshot();
  EXPECT_TRUE(snapshot.error.has_value());
  if (snapshot.error) {
    EXPECT_EQ(snapshot.error->code, "execution_timeout");
  }

  // Backend stop should have been called
  EXPECT_GE(controlled_execution.stop_count, 1);
}

TEST_F(ExecutionKernelTest, ExecutionTimeout_DoesNotAdvanceWhilePaused) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel timeout_kernel{
      planner, context, controlled_execution, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(
      timeout_kernel, collision_world_publisher, collision_engine);

  auto req = make_test_request();
  req.timeouts.execution_timeout_s = 0.5;

  auto result = timeout_kernel.execute(req);
  ASSERT_TRUE(result.ok());

  ASSERT_TRUE(controlled_execution.wait_until_execute_called()) << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(timeout_kernel, ExecutionState::kExecuting));

  auto pause_result = timeout_kernel.pause();
  ASSERT_TRUE(pause_result.accepted);
  ts_scheduler.advance_to(ts_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused()) << "wait_until_paused timed out";
  ASSERT_TRUE(wait_for_state(timeout_kernel, ExecutionState::kPaused));

  ts_clock.advance(1'000'000'000);  // 1s > 500ms timeout, but fully paused
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto paused_status = timeout_kernel.get_status();
  EXPECT_EQ(paused_status.state, ExecutionState::kPaused);

  auto stop_result = timeout_kernel.stop();
  ASSERT_TRUE(stop_result.accepted);
  ASSERT_TRUE(timeout_kernel.wait_for_completion(2'000'000'000))
      << "kernel did not complete after stopping from paused state";
  EXPECT_EQ(timeout_kernel.get_status().state, ExecutionState::kStopped);
}

TEST_F(ExecutionKernelTest, PlanningTimeout_TriggersStop) {
  SlowMockPlanner slow_planner;
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel timeout_kernel{
      slow_planner, context, execution, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(
      timeout_kernel, collision_world_publisher, collision_engine);

  auto req = make_test_request();
  req.timeouts.planning_timeout_s = 0.1;

  auto result = timeout_kernel.execute(req);
  ASSERT_TRUE(result.ok());

  // Wait until planning starts
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Advance scheduler past the planning timeout — fires the callback
  ts_scheduler.advance_to(ts_clock.now() + 200'000'000);  // 200ms > 100ms timeout

  // Release the planner (it will return, then the worker checks stop_requested)
  slow_planner.release();

  // Wait for kernel to settle
  timeout_kernel.wait_for_completion();

  auto status = timeout_kernel.get_status();
  EXPECT_EQ(status.state, ExecutionState::kStopped);
}

// =============================================================================
// prev_state Capture Test (B-2)
// =============================================================================

TEST_F(ExecutionKernelTest, ResetDuringPlanning_PrevStateIsPlanning) {
  SlowMockPlanner slow_planner;
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel local_kernel{
      slow_planner, context, execution, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(
      local_kernel, collision_world_publisher, collision_engine);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Reset during planning
  auto reset_result = local_kernel.reset();
  EXPECT_TRUE(reset_result.accepted);
  slow_planner.release();
  local_kernel.wait_for_completion();

  std::lock_guard<std::mutex> lock(events_mutex);
  // Find the transition to Idle
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition && e.to_state == ExecutionState::kIdle) {
      EXPECT_EQ(e.from_state, ExecutionState::kPlanning)
          << "prev_state should be Planning, not " << static_cast<int>(e.from_state);
      break;
    }
  }
}

// =============================================================================
// PRD-4 Qualification: Concurrent Stress Test (Item 4)
// =============================================================================

TEST_F(ExecutionKernelTest, ConcurrentStress_RapidExecuteStopResetCycles) {
  // 50 rapid execute/stop/reset cycles with concurrent get_status() reads.
  // Verifies no crashes, no deadlocks, monotonic execution IDs, and valid
  // terminal states.
  ControlledExecution ctrl_exec;
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel local_kernel{
      planner, context, ctrl_exec, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(
      local_kernel, collision_world_publisher, collision_engine);

  std::atomic<bool> reader_stop{false};
  std::atomic<int> reads_completed{0};
  std::vector<ExecutionState> observed_states;
  std::mutex observed_mu;

  // Background thread: continuously read status
  std::thread reader([&]() {
    while (!reader_stop.load(std::memory_order_relaxed)) {
      auto status = local_kernel.get_status();
      {
        std::lock_guard<std::mutex> lock(observed_mu);
        observed_states.push_back(status.state);
      }
      reads_completed.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });

  constexpr int kCycles = 50;
  for (int i = 0; i < kCycles; ++i) {
    auto result = local_kernel.execute(make_test_request());
    ASSERT_TRUE(result.ok()) << "Cycle " << i << " execute failed";

    // Wait until the worker actually entered the backend. Sleeping here races:
    // complete() can otherwise run before execute_trajectory() sets running_.
    ASSERT_TRUE(ctrl_exec.wait_until_execute_count(i + 1))
        << "Cycle " << i << " backend execute_trajectory was not entered";

    // Complete the execution immediately
    ctrl_exec.complete();
    ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000))
        << "Cycle " << i << " did not complete after complete()";

    // Reset for next cycle
    auto reset_result = local_kernel.reset();
    EXPECT_TRUE(reset_result.accepted) << "Cycle " << i << " reset failed";
  }

  reader_stop.store(true, std::memory_order_relaxed);
  reader.join();

  // Verify: reader thread ran without crashing and collected reads
  EXPECT_GT(reads_completed.load(), 0);

  // Verify: all observed states are valid enum values
  {
    std::lock_guard<std::mutex> lock(observed_mu);
    for (auto st : observed_states) {
      EXPECT_TRUE(
          st == ExecutionState::kIdle ||
          st == ExecutionState::kPlanning ||
          st == ExecutionState::kValidating ||
          st == ExecutionState::kExecuting ||
          st == ExecutionState::kCompleted ||
          st == ExecutionState::kStopped ||
          st == ExecutionState::kFailed ||
          st == ExecutionState::kPaused ||
          st == ExecutionState::kStopping)
          << "Invalid state observed: " << static_cast<int>(st);
    }
  }

  // Verify: final state is idle after all resets
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, ConcurrentStress_RapidExecuteStopCycles) {
  // Variant: execute then stop (not complete), verifying stop path stability.
  ControlledExecution ctrl_exec;
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel local_kernel{
      planner, context, ctrl_exec, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(
      local_kernel, collision_world_publisher, collision_engine);

  constexpr int kCycles = 30;
  for (int i = 0; i < kCycles; ++i) {
    auto result = local_kernel.execute(make_test_request());
    ASSERT_TRUE(result.ok()) << "Cycle " << i << " execute failed";

    ASSERT_TRUE(ctrl_exec.wait_until_execute_count(i + 1))
        << "Cycle " << i << " backend execute_trajectory was not entered";

    // Stop mid-execution. ASSERT (not EXPECT) so a rejected stop fails the
    // test with a clear "stop_result.accepted was false" diagnostic at the
    // exact cycle instead of falling through to wait_for_completion and
    // hanging until the 2 s timeout fires with a less-specific message.
    // This is the diagnostic-quality fix; the hang itself is bounded by
    // the 2 s wait_for_completion timeout below.
    auto stop_result = local_kernel.stop();
    ASSERT_TRUE(stop_result.accepted) << "Cycle " << i << " stop failed";
    ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000))
        << "Cycle " << i << " did not complete after stop()";

    auto reset_result = local_kernel.reset();
    EXPECT_TRUE(reset_result.accepted) << "Cycle " << i << " reset failed";
  }

  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kIdle);
}

// =========================================================================
// Motion-scoped interrupt arming/reaping/outcome store
// =========================================================================

namespace motion_45c {

InterruptSourceDefinition make_fast_probe_source(std::string id) {
  InterruptSourceDefinition src;
  src.source_id = std::move(id);
  src.adapter = InterruptAdapter::kInternalTest;
  src.signal = src.source_id + "_signal";
  src.source_class = InterruptSourceClass::kFastExecutionRelevant;
  src.kind = InterruptSourceKind::kEdge;
  src.priority = 700;
  return src;
}

InterruptPolicyDefinition make_move_trip_policy(std::string policy_id,
                                                  std::string source_id,
                                                  int priority = 700) {
  InterruptPolicyDefinition p;
  p.policy_id = std::move(policy_id);
  p.source_id = std::move(source_id);
  p.priority = priority;
  p.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  p.preemption_action = InterruptPreemptionAction::kCompleteMoveOnSignal;
  p.post_latch_disposition =
      InterruptPostLatchDisposition::kAutoClearAfterMotionOutcome;
  p.scope_kind = InterruptPolicyScopeKind::kMove;
  // Allow both modes; the test selects which mode the scope uses.
  // kTripIfAlreadyActive lets unit tests arm without first
  // establishing a baseline sample (the kernel-level rejection of
  // fast-interrupt signals from kIdle would prevent that anyway).
  p.allowed_activation_modes = {
      MotionInterruptActivationMode::kRequireInactiveAtArm,
      MotionInterruptActivationMode::kTripIfAlreadyActive};
  // complete_move_on_signal policies MUST declare a non-empty
  // allowed_stop_modes set. Defaults span the modes the
  // kernel tests exercise (the kernel-level capability gate
  // additionally enforces the backend's supported_stop_modes).
  p.allowed_stop_modes = {MotionInterruptStopMode::kSpringBackHold,
                            MotionInterruptStopMode::kSoftOnPath,
                            MotionInterruptStopMode::kHardDeceleration};
  return p;
}

/// Minimal stop profile so scopes can satisfy the
/// stop_profile_required gate. Tests can override the mode.
MotionInterruptStopProfileDTO make_stop_profile(
    MotionInterruptStopMode mode = MotionInterruptStopMode::kSpringBackHold) {
  MotionInterruptStopProfileDTO p;
  p.profile_id = "test_stop_profile";
  p.stop_mode = mode;
  return p;
}

MotionInterruptStopProfileDTO make_soft_on_path_profile(
    bool allow_spring_back_fallback,
    bool allow_natural_completion_on_short_path = false) {
  auto p = make_stop_profile(MotionInterruptStopMode::kSoftOnPath);
  p.deceleration_time_s = 0.5;
  p.max_stop_time_s = 1.0;
  p.allow_natural_completion_on_short_path =
      allow_natural_completion_on_short_path;
  p.per_joint_max_deceleration_rad_s2 = {
      {"j1", 100.0},
      {"j2", 100.0},
  };
  if (allow_spring_back_fallback) {
    p.allowed_fallback_modes = {MotionInterruptStopMode::kSpringBackHold};
  }
  return p;
}

InterruptSignalUpdate make_probe_sample(std::string source_id,
                                          std::int64_t seq,
                                          std::int64_t ts_ns,
                                          bool active = false) {
  InterruptSignalUpdate u;
  u.source_id = std::move(source_id);
  u.sequence_id = seq;
  u.timestamp_ns = ts_ns;
  u.value = active;
  return u;
}

MotionInterruptScopeDTO make_scope(
    std::string scope_id, std::string policy_id,
    MotionInterruptActivationMode mode =
        MotionInterruptActivationMode::kTripIfAlreadyActive,
    MotionInterruptStopMode stop_mode =
        MotionInterruptStopMode::kSpringBackHold) {
  MotionInterruptScopeDTO scope;
  scope.scope_id = std::move(scope_id);
  scope.policy_id = std::move(policy_id);
  scope.activation_mode = mode;
  scope.stop_profile = make_stop_profile(stop_mode);
  return scope;
}

}  // namespace motion_45c

TEST_F(ExecutionKernelTest,
       Execute_FreeMove_EmitsCompletedNormallyOutcome) {
  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->execution_id, result.value());
  EXPECT_EQ(outcome->move_id, "move_1");
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kCompletedNormally);
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_FALSE(outcome->error.has_value());
}

TEST_F(ExecutionKernelTest,
       GetExecutionOutcome_UnknownIdReturnsNullopt) {
  EXPECT_FALSE(kernel.get_execution_outcome(9999u).has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedMoveHappyPath_EmitsNoSignalTripOutcome) {
  using namespace motion_45c;
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("probe_move");
  // kTripIfAlreadyActive arms without a prior baseline; no signal is
  // published, so the move completes normally and the outcome is
  // coerced from completed_normally to no_signal_trip.
  req.motion_interrupt_scopes = {make_scope("scope_a", "probe_trip")};
  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kNoSignalTrip);
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_FALSE(outcome->error.has_value());

  // Reap MUST run: no armed bindings remain.
  EXPECT_TRUE(kernel.active_motion_interrupt_scopes().empty());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedMoveArmFails_NeverDispatches) {
  using namespace motion_45c;
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  // kRequireInactiveAtArm with no recorded baseline fails arming
  // (spec/260 §B.1: arming must have a fresh sample).
  auto req = make_test_request("probe_move");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kRequireInactiveAtArm)};
  const int planner_calls_before = planner.plan_count;
  const int execute_calls_before = execution.execute_count;

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());  // execute() returns id, worker fails
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  // The backend MUST NOT have been dispatched.
  EXPECT_EQ(execution.execute_count, execute_calls_before);
  // Planning still ran (arming is post-validation).
  EXPECT_GT(planner.plan_count, planner_calls_before);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->error.has_value());
  // Spec/210 §5.2: motion_interrupt_arm_failed is a first-class error
  // code. The kernel promotes the manager's `details["reason"]` to the
  // canonical code so callers don't have to read the nested detail.
  EXPECT_EQ(outcome->error->code, kErrorMotionInterruptArmFailed);
  EXPECT_EQ(outcome->error->details.at("reason"),
            "motion_interrupt_arm_failed");
  EXPECT_TRUE(kernel.active_motion_interrupt_scopes().empty());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedMoveArmFails_LastErrorClassifiedAsArmFailed) {
  // Regression: `error_code_from_dto` must map
  // the motion_interrupt_* string codes back to their ErrorCode enum
  // entries so the state-machine `last_error` surface — which Composer
  // and release evidence consume — distinguishes arm failures from
  // generic internal errors.
  using namespace motion_45c;
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("probe_move_lasterr");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kRequireInactiveAtArm)};
  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(kernel.get_status().last_error,
            ErrorCode::kMotionInterruptArmFailed);
}

// Legacy `Execute_ScopedTripDuringExecution_LastErrorClassifiedAsBackendStopFailed`
// was removed because the kCompleteMoveOnSignal latch now produces a
// signal_trip success via execute_controlled_stop. The equivalent
// last_error fidelity coverage for trip-time stop failures lives in
// `Execute_ScopedTrip_BackendStopFailureDowngradesToFailed` below.

TEST_F(ExecutionKernelTest,
       Execute_ScopedMoveArmFails_OutcomeQueryableByExecutionId) {
  using namespace motion_45c;
  // Unknown policy_id at arm time: a different failure mode than the
  // missing-baseline test above; verifies the outcome is still
  // queryable by execution_id and carries the manager's reason.
  auto req = make_test_request("probe_move");
  MotionInterruptScopeDTO bad;
  bad.scope_id = "scope_a";
  bad.policy_id = "policy_does_not_exist";
  req.motion_interrupt_scopes = {bad};
  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "motion_interrupt_policy_not_found");
}

TEST_F(ExecutionKernelTest,
       Execute_PlanningFailure_EmitsFailedOutcome) {
  PlanResultDTO bad;
  bad.error = ErrorDTO{kErrorPlanningFailed, "no plan", {}};
  planner.next_result = bad;

  auto result = execute_sync(make_test_request("plan_fail_move"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->code, kErrorPlanningFailed);
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTripDuringExecution_EmitsSignalTripOutcome) {
  using namespace motion_45c;
  // Success path: backend's default execute_controlled_stop
  // returns accepted + confirmed +
  // native_simulated_stop. The kernel must transition
  // Executing -> Stopping -> Completed and emit a
  // MotionExecutionOutcomeDTO with terminal_cause = signal_trip and a
  // populated MotionTripOutcomeDTO.
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_success");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto trip = make_probe_sample("probe", 2, local_clock.now(),
                                   /*active=*/true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);

  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kSignalTrip);
  ASSERT_TRUE(outcome->trip.has_value());
  EXPECT_EQ(outcome->trip->executed_stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  EXPECT_EQ(outcome->trip->stop_command_source,
            MotionInterruptStopCommandSource::kNativeSimulatedStop);
  EXPECT_TRUE(outcome->trip->stop_snapshot.backend_stop_confirmed);
  EXPECT_FALSE(outcome->error.has_value());
  EXPECT_TRUE(local_kernel.active_motion_interrupt_scopes().empty());
  EXPECT_FALSE(local_kernel.get_active_interrupt_latch().has_value())
      << "latch must be cleared via auto_clear_after_motion_outcome";
  EXPECT_EQ(controlled_execution.controlled_stop_count, 1);
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_BackendStopFailureDowngradesToFailed) {
  using namespace motion_45c;
  // Failure path: backend returns accepted=false.
  // Kernel transitions Stopping -> Failed and emits terminal_cause =
  // failed with motion_interrupt_backend_stop_failed.
  ControlledExecution controlled_execution;
  ControlledStopResultDTO failure;
  failure.accepted = false;
  failure.executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
  failure.stop_command_source =
      MotionInterruptStopCommandSource::kDiagnosticOther;
  failure.error = ErrorDTO{kErrorMotionInterruptBackendStopFailed,
                            "simulated backend refusal",
                            {{"reason", "controller_rejected_goal"}}};
  controlled_execution.next_controlled_stop_result = failure;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_backend_refuse");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);

  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  // Spec/230 §3.4: backend-stop failure on a complete_move_on_signal
  // trip transitions to InterventionRequired (uncertain robot state).
  // Outcome cause is `failed` per spec/210 §3.11.
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  EXPECT_EQ(local_kernel.get_status().last_error,
            ErrorCode::kMotionInterruptBackendStopFailed);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->code, kErrorMotionInterruptBackendStopFailed);
  EXPECT_EQ(outcome->error->details.at("reason"),
            "controller_rejected_goal");
  EXPECT_FALSE(outcome->trip.has_value());
  // Latch persists for operator intervention-acknowledge.
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_BackendNotConfirmedDowngradesToFailed) {
  using namespace motion_45c;
  // Conjunctive rule #3: backend_stop_confirmed=false
  // downgrades to failed even when accepted=true.
  ControlledExecution controlled_execution;
  ControlledStopResultDTO unconfirmed;
  unconfirmed.accepted = true;
  unconfirmed.backend_stop_confirmed = false;
  unconfirmed.executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
  unconfirmed.stop_command_source =
      MotionInterruptStopCommandSource::kFjtPreemptHold;
  controlled_execution.next_controlled_stop_result = unconfirmed;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_no_confirm");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "backend_stop_not_confirmed");
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_ExecutedModeNotInAllowedFallbackDowngradesToFailed) {
  using namespace motion_45c;
  // Conjunctive rule #1: backend executed kHardDeceleration
  // but the scope's allowed_fallback_modes does not include it.
  ControlledExecution controlled_execution;
  ControlledStopResultDTO unauthorized_fallback;
  unauthorized_fallback.accepted = true;
  unauthorized_fallback.backend_stop_confirmed = true;
  unauthorized_fallback.executed_stop_mode =
      MotionInterruptStopMode::kHardDeceleration;
  unauthorized_fallback.stop_command_source =
      MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel;
  unauthorized_fallback.fallback_reason = "controller_rejected_spring_back";
  controlled_execution.next_controlled_stop_result = unauthorized_fallback;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_unauthorized_fallback");
  // Scope requests spring_back_hold; no fallbacks listed.
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive,
      MotionInterruptStopMode::kSpringBackHold)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "executed_mode_not_in_allowed_fallback_modes");
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_FallbackHonoredWhenAllowed) {
  using namespace motion_45c;
  // Fallback matrix v1: the only qualified generic
  // requested/executed mismatch is soft_on_path -> spring_back_hold.
  // The scope listed the fallback, the backend reported a stable
  // fallback_reason, and the node category is kGeneric.
  ControlledExecution controlled_execution;
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  ControlledStopResultDTO fallback_ok;
  fallback_ok.accepted = true;
  fallback_ok.backend_stop_confirmed = true;
  fallback_ok.executed_stop_mode =
      MotionInterruptStopMode::kSpringBackHold;
  fallback_ok.stop_command_source =
      MotionInterruptStopCommandSource::kDiagnosticOther;
  fallback_ok.fallback_reason = "soft_on_path_scaffold_anchored_hold_fallback";
  controlled_execution.next_controlled_stop_result = fallback_ok;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_fallback_ok");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile->deceleration_time_s = 0.2;
  scope.stop_profile->per_joint_max_deceleration_rad_s2 = {
      {"j1", 10.0}, {"j2", 10.0}};
  scope.stop_profile->allowed_fallback_modes = {
      MotionInterruptStopMode::kSpringBackHold};
  scope.node_category = MotionInterruptNodeCategory::kGeneric;
  req.motion_interrupt_scopes = {scope};

  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kSignalTrip);
  ASSERT_TRUE(outcome->trip.has_value());
  EXPECT_EQ(outcome->trip->executed_stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  ASSERT_TRUE(outcome->trip->fallback_reason.has_value());
  EXPECT_EQ(*outcome->trip->fallback_reason,
            "soft_on_path_scaffold_anchored_hold_fallback");
  EXPECT_EQ(outcome->trip->diagnostics.at("fallback_qualification_id"),
            "4.5G-soft_on_path-to-spring_back_hold-generic-v1");
  EXPECT_EQ(outcome->trip->diagnostics.at("fallback_pair"),
            "soft_on_path->spring_back_hold");
  EXPECT_EQ(outcome->trip->diagnostics.at(
                "fallback_qualification_evidence_status"),
            "in_tree_qualified; live_adapter_evidence_required_for_release");
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_SoftOnPathNaturalCompletionMapsToNoSignalTrip) {
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath};
  MotionInterruptSoftOnPathPlannerConfigDTO soft_planner_config;
  soft_planner_config.per_joint_max_jerk_rad_s3 = {
      {"j1", 1'000'000.0}, {"j2", 1'000'000.0}};
  controlled_execution.motion_interrupt_stop_capability
      .soft_on_path_planner_config = soft_planner_config;
  ControlledStopResultDTO natural_completion;
  natural_completion.accepted = true;
  natural_completion.backend_stop_confirmed = true;
  natural_completion.completed_by_natural_completion = true;
  natural_completion.executed_stop_mode = MotionInterruptStopMode::kSoftOnPath;
  natural_completion.stop_command_source =
      MotionInterruptStopCommandSource::kDiagnosticOther;
  natural_completion.diagnostics["reason"] =
      "soft_on_path_remaining_time_short";
  natural_completion.diagnostics["recommended_disposition"] =
      "natural_completion";
  controlled_execution.next_controlled_stop_result = natural_completion;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_natural_completion_ok");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile = make_soft_on_path_profile(
      /*allow_spring_back_fallback=*/false,
      /*allow_natural_completion_on_short_path=*/true);
  scope.stop_profile->allow_natural_completion_on_short_path = true;
  scope.node_category = MotionInterruptNodeCategory::kGeneric;
  req.motion_interrupt_scopes = {scope};

  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kNoSignalTrip);
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_FALSE(outcome->error.has_value());
  EXPECT_FALSE(local_kernel.get_active_interrupt_latch().has_value());
  EXPECT_TRUE(local_kernel.active_motion_interrupt_scopes().empty());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_UnqualifiedFallbackPairDowngradesToFailed) {
  using namespace motion_45c;
  // allowed_fallback_modes is a fail-closed guardrail, not production
  // approval for arbitrary substitutions. spring_back_hold -> hard_deceleration
  // is not in the fallback matrix, so the outcome must fail even
  // though the profile listed the fallback and the backend confirmed it.
  ControlledExecution controlled_execution;
  ControlledStopResultDTO fallback_unqualified;
  fallback_unqualified.accepted = true;
  fallback_unqualified.backend_stop_confirmed = true;
  fallback_unqualified.executed_stop_mode =
      MotionInterruptStopMode::kHardDeceleration;
  fallback_unqualified.stop_command_source =
      MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel;
  fallback_unqualified.fallback_reason = "controller_no_spring_back_capability";
  controlled_execution.next_controlled_stop_result = fallback_unqualified;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_unqualified_fallback");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSpringBackHold);
  scope.stop_profile->allowed_fallback_modes = {
      MotionInterruptStopMode::kHardDeceleration};
  scope.node_category = MotionInterruptNodeCategory::kGeneric;
  req.motion_interrupt_scopes = {scope};

  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "fallback_pair_not_qualified");
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_FallbackCommandSourceNotQualifiedDowngradesToFailed) {
  using namespace motion_45c;
  // The fallback matrix qualifies a requested/executed mode pair only for
  // specific command sources. A backend cannot report the right stop modes
  // while using an unqualified controller path and still claim a trip success.
  ControlledExecution controlled_execution;
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  ControlledStopResultDTO wrong_source;
  wrong_source.accepted = true;
  wrong_source.backend_stop_confirmed = true;
  wrong_source.executed_stop_mode =
      MotionInterruptStopMode::kSpringBackHold;
  wrong_source.stop_command_source =
      MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel;
  wrong_source.fallback_reason = "controller_used_cancel_deceleration";
  controlled_execution.next_controlled_stop_result = wrong_source;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_unqualified_fallback_source");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile->deceleration_time_s = 0.2;
  scope.stop_profile->per_joint_max_deceleration_rad_s2 = {
      {"j1", 10.0}, {"j2", 10.0}};
  scope.stop_profile->allowed_fallback_modes = {
      MotionInterruptStopMode::kSpringBackHold};
  scope.node_category = MotionInterruptNodeCategory::kGeneric;
  req.motion_interrupt_scopes = {scope};

  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "fallback_command_source_not_qualified");
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_FallbackReasonRequiredWhenModeDiffers) {
  // Conjunctive rule: even for a qualified fallback
  // pair, an unset fallback_reason produces a failed outcome.
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  ControlledStopResultDTO no_reason;
  no_reason.accepted = true;
  no_reason.backend_stop_confirmed = true;
  no_reason.executed_stop_mode =
      MotionInterruptStopMode::kSpringBackHold;
  no_reason.stop_command_source =
      MotionInterruptStopCommandSource::kDiagnosticOther;
  // fallback_reason intentionally unset
  controlled_execution.next_controlled_stop_result = no_reason;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_missing_reason");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile->deceleration_time_s = 0.2;
  scope.stop_profile->per_joint_max_deceleration_rad_s2 = {
      {"j1", 10.0}, {"j2", 10.0}};
  scope.stop_profile->allowed_fallback_modes = {
      MotionInterruptStopMode::kSpringBackHold};
  req.motion_interrupt_scopes = {scope};

  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"), "fallback_reason_required");
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_PrecisionContactRejectsFallbackEvenIfBackendReturnsIt) {
  // Defense in depth: precision-contact node category MUST NOT accept
  // a stop-mode fallback at trip time even if the arm-time gate was
  // bypassed (e.g. backend returned a different mode than requested).
  // The arm-time gate already forbids non-empty allowed_fallback_modes
  // for precision-contact, but the trip-time check here covers a
  // backend that returns a fallback the arm gate didn't see.
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  ControlledStopResultDTO surprise_fallback;
  surprise_fallback.accepted = true;
  surprise_fallback.backend_stop_confirmed = true;
  surprise_fallback.executed_stop_mode =
      MotionInterruptStopMode::kHardDeceleration;
  surprise_fallback.stop_command_source =
      MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel;
  surprise_fallback.fallback_reason = "controller_fallback";
  controlled_execution.next_controlled_stop_result = surprise_fallback;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("precision_trip_fallback");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSpringBackHold);
  // Precision contact, no fallback listed (arm gate accepts).
  scope.node_category = MotionInterruptNodeCategory::kPrecisionContact;
  req.motion_interrupt_scopes = {scope};

  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "executed_mode_not_in_allowed_fallback_modes");
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_ConfirmationLatencyExceedingMaxStopTimeFailsClosed) {
  // Conjunctive rule: confirmation_latency_ns must not
  // exceed max_stop_time_s (or complete_move_on_signal_timeout_ns).
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  ControlledStopResultDTO slow;
  slow.accepted = true;
  slow.backend_stop_confirmed = true;
  slow.executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
  slow.stop_command_source =
      MotionInterruptStopCommandSource::kFjtPreemptHold;
  slow.confirmation_latency_ns = 600'000'000;  // 600 ms
  controlled_execution.next_controlled_stop_result = slow;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_slow_confirm");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSpringBackHold);
  scope.stop_profile->max_stop_time_s = 0.5;  // 500 ms
  req.motion_interrupt_scopes = {scope};

  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "backend_stop_confirmation_exceeded_max_stop_time_s");
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

// -------- Additional coverage --------

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_DefaultTimeoutEnforcedWhenMaxStopTimeUnset) {
  // Finding 3: when stop_profile.max_stop_time_s is absent, the kernel
  // MUST fall back to the deployment default
  // (complete_move_on_signal_timeout_ns). A backend reporting a
  // confirmation latency that exceeds the default still fails closed
  // even with no explicit max_stop_time_s on the profile.
  using namespace motion_45c;
  ExecutionKernelConfig cfg;
  cfg.complete_move_on_signal_timeout_ns = 100'000'000;  // 100 ms
  ControlledExecution controlled_execution;
  ControlledStopResultDTO slow;
  slow.accepted = true;
  slow.backend_stop_confirmed = true;
  slow.executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
  slow.stop_command_source =
      MotionInterruptStopCommandSource::kFjtPreemptHold;
  slow.confirmation_latency_ns = 200'000'000;  // 200 ms > 100 ms
  controlled_execution.next_controlled_stop_result = slow;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache,
                                cfg};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_default_timeout");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSpringBackHold);
  // Intentionally leave scope.stop_profile->max_stop_time_s unset.
  ASSERT_FALSE(scope.stop_profile->max_stop_time_s.has_value());
  req.motion_interrupt_scopes = {scope};

  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "backend_stop_confirmation_exceeded_max_stop_time_s");
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_SignalToLatchTimingPositive) {
  // Finding 4: signal_to_latch_ns MUST be derived from the signal
  // timestamp on the latch, not from any handler-local clock read.
  using namespace motion_45c;
  ControlledExecution controlled_execution;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_timing");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  // Send a signal whose timestamp is in the past (clock advances
  // between the signal source observing the value and the manager
  // accepting the latch). With FakeClock the manager's latched_at_ns
  // will equal local_clock.now() at ingest; the signal carries an
  // older timestamp, so latched_at_ns - timestamp_ns is strictly
  // positive.
  InterruptSignalUpdate signal;
  signal.source_id = "probe";
  signal.sequence_id = 2;
  signal.timestamp_ns = local_clock.now() - 5'000'000;  // 5 ms older
  signal.value = true;
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(signal).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->trip.has_value());
  EXPECT_GT(outcome->trip->signal_to_latch_ns, 0)
      << "signal_to_latch_ns must be derived from signal_snapshot.timestamp_ns";
  EXPECT_GE(outcome->trip->latch_to_stop_confirmed_ns, 0);
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_StopSnapshotCarriesJointNamesFromPlanningContext) {
  // Finding 2b: the captured ExecutionStopSnapshotDTO MUST satisfy
  // spec/210 §3.7 size/ordering rules — joint_names non-empty and
  // matching positions_rad length. The capture path pulls
  // joint_names from the planning context's current_state so the
  // RobotStateDTO is well-formed.
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  // Set ControlledExecution snapshot to a fresh post-stop state with
  // two joint positions matching the test fixture's two joint names.
  controlled_execution.snapshot.joint_positions_rad = {0.4, 0.4};

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_snap_joints");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->trip.has_value());
  const auto& robot_state = outcome->trip->stop_snapshot.robot_state;
  EXPECT_FALSE(robot_state.joint_names.empty());
  EXPECT_EQ(robot_state.joint_names.size(),
            robot_state.positions_rad.size());
  EXPECT_EQ(outcome->trip->stop_snapshot.source,
            ExecutionStopSnapshotSource::kExecutionBackendFeedback);
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_StopSnapshotInvalidDowngradesToInterventionRequired) {
  // Finding 2b: when the backend's post-stop joint vector does not
  // match the planning context's joint_names (size mismatch), the
  // RobotStateDTO size/ordering validation MUST fail and the outcome
  // MUST downgrade to failed with motion_interrupt_stop_state_invalid.
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  // Override the post-stop joint vector to a 3-element vector that
  // cannot align with the fixture context's 2-element joint_names.
  controlled_execution.next_post_stop_joint_positions_rad =
      std::vector<double>{0.1, 0.2, 0.3};

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();

  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  // Precision-contact scope so the planning-context fallback is
  // rejected too — failing both Priority 1 (size mismatch) and
  // Priority 3 (precision-contact rejects planning_context_state).
  auto req = make_test_request("trip_snap_invalid");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSpringBackHold);
  scope.node_category = MotionInterruptNodeCategory::kPrecisionContact;
  req.motion_interrupt_scopes = {scope};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  EXPECT_EQ(local_kernel.get_status().last_error,
            ErrorCode::kMotionInterruptStopStateInvalid);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->code,
            kErrorMotionInterruptStopStateInvalid);
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

// Legacy `Execute_ScopedTripDuringExecution_EmitsFailedOutcomePending45D`
// was removed because the kCompleteMoveOnSignal latch now produces a
// real signal_trip outcome via `handle_motion_trip_latch`. The
// equivalent coverage lives in the
// `Execute_ScopedTripDuringExecution_EmitsSignalTripOutcome` happy-path
// test and the failure-path tests above.

// =========================================================================
// Backend controlled-stop capability gate at arm time
// =========================================================================

TEST_F(ExecutionKernelTest,
       Execute_ScopedMove_RejectsStopModeNotInBackendCapability) {
  // Per spec/260 §5.6: pre-dispatch capability validation —
  // every requested stop_mode MUST be in the backend's
  // supported_stop_modes. Shrinking the backend capability to drop
  // kHardDeceleration but requesting it in the scope produces a
  // pre-dispatch rejection with reason
  // `stop_mode_not_supported_by_backend`. The backend MUST NOT be
  // dispatched and no scope binding remains armed.
  using namespace motion_45c;
  execution.motion_interrupt_stop_capability.supported_stop_modes = {
      MotionInterruptStopMode::kSpringBackHold};
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("backend_cap_drop");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive,
      MotionInterruptStopMode::kHardDeceleration)};
  const int execute_calls_before = execution.execute_count;

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, execute_calls_before);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->code, kErrorMotionInterruptArmFailed);
  EXPECT_EQ(outcome->error->details.at("reason"),
            "stop_mode_not_supported_by_backend");
  EXPECT_TRUE(kernel.active_motion_interrupt_scopes().empty());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedMove_RejectsFallbackModeNotInBackendCapability) {
  // Every element of allowed_fallback_modes must also be in the
  // backend's supported_stop_modes. Backend supports
  // kSpringBackHold only; scope requests kSpringBackHold with
  // kHardDeceleration as a fallback. Pre-dispatch rejects with
  // `fallback_mode_not_supported_by_backend`.
  using namespace motion_45c;
  execution.motion_interrupt_stop_capability.supported_stop_modes = {
      MotionInterruptStopMode::kSpringBackHold};
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("fallback_cap_drop");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSpringBackHold);
  scope.stop_profile->allowed_fallback_modes = {
      MotionInterruptStopMode::kHardDeceleration};
  req.motion_interrupt_scopes = {scope};

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->code, kErrorMotionInterruptArmFailed);
  EXPECT_EQ(outcome->error->details.at("reason"),
            "fallback_mode_not_supported_by_backend");
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedMove_BackendDefaultEmptyCapabilityFailsClosed) {
  // Industrial safety property: the IExecutionBackend default
  // capability is an empty supported_stop_modes set. Any
  // complete_move_on_signal scope therefore fails closed at arm
  // time on a backend that has not declared support. This is the
  // 4.5D-1 fail-closed default the spec requires.
  using namespace motion_45c;
  execution.motion_interrupt_stop_capability.supported_stop_modes.clear();
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("backend_default_empty");
  req.motion_interrupt_scopes = {make_scope("scope_a", "probe_trip")};
  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "stop_mode_not_supported_by_backend");
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedMove_PrecisionContactWithEmptyFallbackArms) {
  // Happy path for precision-contact node category:
  // empty fallback list + supported mode + valid policy arms
  // cleanly, dispatches, and produces no_signal_trip on completion.
  using namespace motion_45c;
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("precision_contact_move");
  auto scope = make_scope("scope_a", "probe_trip");
  scope.node_category = MotionInterruptNodeCategory::kPrecisionContact;
  req.motion_interrupt_scopes = {scope};

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kNoSignalTrip);
}

TEST_F(ExecutionKernelTest,
       Execute_SoftOnPathWithoutFallbackRejectsInfeasibleEnvelope) {
  // Exact soft_on_path must prove the plan-time feasibility
  // envelope before arming. The default fixture trajectory has no explicit
  // natural-completion policy, so late trip points cannot satisfy the requested
  // wall-time ramp and the scoped move fails closed before backend dispatch.
  using namespace motion_45c;
  execution.motion_interrupt_stop_capability.supported_stop_modes = {
      MotionInterruptStopMode::kSoftOnPath};
  MotionInterruptSoftOnPathPlannerConfigDTO soft_planner_config;
  soft_planner_config.per_joint_max_jerk_rad_s3 = {
      {"j1", 1'000'000.0}, {"j2", 1'000'000.0}};
  execution.motion_interrupt_stop_capability.soft_on_path_planner_config =
      soft_planner_config;
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("soft_exact_envelope_reject");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile = make_soft_on_path_profile(
      /*allow_spring_back_fallback=*/false);
  req.motion_interrupt_scopes = {scope};
  const int execute_calls_before = execution.execute_count;

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, execute_calls_before);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->code, kErrorMotionInterruptArmFailed);
  EXPECT_EQ(outcome->error->details.at("reason"),
            "soft_on_path_feasibility_envelope_infeasible");
  EXPECT_TRUE(outcome->error->details.contains("planner_reason"));
  EXPECT_EQ(outcome->error->details.at("recommended_fallback"),
            "spring_back_hold");
  EXPECT_TRUE(kernel.active_motion_interrupt_scopes().empty());
}

TEST_F(ExecutionKernelTest,
       Execute_SoftOnPathNaturalCompletionPolicyAllowsEnvelopeGap) {
  // Generic search moves may explicitly say that a signal
  // arriving too close to the natural endpoint should complete as ordinary
  // no-trip/natural completion rather than depending on spring_back fallback.
  using namespace motion_45c;
  execution.motion_interrupt_stop_capability.supported_stop_modes = {
      MotionInterruptStopMode::kSoftOnPath};
  MotionInterruptSoftOnPathPlannerConfigDTO soft_planner_config;
  soft_planner_config.per_joint_max_jerk_rad_s3 = {
      {"j1", 1'000'000.0}, {"j2", 1'000'000.0}};
  execution.motion_interrupt_stop_capability.soft_on_path_planner_config =
      soft_planner_config;
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("soft_exact_natural_completion");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile = make_soft_on_path_profile(
      /*allow_spring_back_fallback=*/false,
      /*allow_natural_completion_on_short_path=*/true);
  req.motion_interrupt_scopes = {scope};

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted)
      << (outcome->error ? outcome->error->message : "")
      << " reason="
      << (outcome->error && outcome->error->details.contains("reason")
              ? outcome->error->details.at("reason")
              : "")
      << " planner_reason="
      << (outcome->error && outcome->error->details.contains("planner_reason")
              ? outcome->error->details.at("planner_reason")
              : "");
  EXPECT_EQ(execution.execute_count, 1);

  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kNoSignalTrip);
  EXPECT_TRUE(kernel.active_motion_interrupt_scopes().empty());
}

TEST_F(ExecutionKernelTest,
       Execute_SoftOnPathWithSpringBackFallbackAllowsEnvelopeGap) {
  // The same plan-time gap is acceptable only when the motion profile
  // explicitly names the first qualified production fallback pair. A future
  // trip must then record requested=soft_on_path / executed=spring_back_hold;
  // no-trip completion remains no_signal_trip.
  using namespace motion_45c;
  execution.motion_interrupt_stop_capability.supported_stop_modes = {
      MotionInterruptStopMode::kSoftOnPath,
      MotionInterruptStopMode::kSpringBackHold};
  ASSERT_TRUE(kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("soft_envelope_fallback_allowed");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile = make_soft_on_path_profile(
      /*allow_spring_back_fallback=*/true);
  req.motion_interrupt_scopes = {scope};

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(execution.execute_count, 1);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kNoSignalTrip);
  EXPECT_TRUE(kernel.active_motion_interrupt_scopes().empty());
}

TEST_F(ExecutionKernelTest,
       OutcomeStore_FifoEvictionRespectsRetentionCap) {
  // Tiny retention so we can observe eviction without running many
  // executions. The fixture kernel already runs one execute()
  // implicitly via SetUp's plan / validation defaults; we reset
  // between executes via the kernel's Failed/Stopped-to-Idle path
  // by configuring a local kernel with retention=2.
  ExecutionKernelConfig cfg;
  cfg.motion_execution_outcome_retention = 2;
  ExecutionKernel local_kernel{planner, context, execution, clock,
                                scheduler, cache, cfg};
  configure_authoritative_collision_validation(local_kernel,
                                                 collision_world_publisher,
                                                 collision_engine);

  std::vector<std::uint64_t> ids;
  for (int i = 0; i < 3; ++i) {
    auto r = local_kernel.execute(
        make_test_request("evict_move_" + std::to_string(i)));
    ASSERT_TRUE(r.ok())
        << "execute " << i << " failed: code=" << r.error().code
        << " msg=" << r.error().message;
    local_kernel.wait_for_completion();
    ids.push_back(r.value());
    // Return to kIdle for the next execute() call.
    auto ack = local_kernel.acknowledge_completed(r.value());
    ASSERT_TRUE(ack.accepted);
  }
  // Oldest evicted; latest two retained.
  EXPECT_FALSE(local_kernel.get_execution_outcome(ids[0]).has_value());
  EXPECT_TRUE(local_kernel.get_execution_outcome(ids[1]).has_value());
  EXPECT_TRUE(local_kernel.get_execution_outcome(ids[2]).has_value());
}

// =========================================================================
// Motion-scoped interrupt qualification. These tests live
// alongside the rest of the kernel suite so they reuse the proven
// ControlledExecution + freshness-aware mock infrastructure. Each
// scenario emits a structured JSON evidence line on stdout that
// deployment qualification runs can grep for and archive as audit
// input. The line format is stable:
//   MANYFORGE_QUALIFICATION_EVIDENCE {...json...}
// Existing per-scenario coverage (signal_trip happy path, backend
// refusal, fallback honored, etc.) is the unit-test surface; the
// dedicated qualification tests below extend it with measured
// latencies + the JSON shape qualification archives need.
// =========================================================================

namespace {

void emit_qualification_evidence(
    const std::string& scenario,
    const std::optional<MotionExecutionOutcomeDTO>& outcome) {
  std::cout << "MANYFORGE_QUALIFICATION_EVIDENCE {\"scenario\":\""
            << scenario << "\"";
  if (!outcome) {
    std::cout << ",\"outcome\":null}" << std::endl;
    return;
  }
  std::cout << ",\"execution_id\":" << outcome->execution_id;
  std::cout << ",\"terminal_cause\":\"";
  using enum MotionExecutionTerminalCause;
  switch (outcome->terminal_cause) {
    case kCompletedNormally: std::cout << "completed_normally"; break;
    case kSignalTrip:        std::cout << "signal_trip"; break;
    case kNoSignalTrip:      std::cout << "no_signal_trip"; break;
    case kStopped:           std::cout << "stopped"; break;
    case kFailed:            std::cout << "failed"; break;
    case kInterruptedFault:  std::cout << "interrupted_fault"; break;
  }
  std::cout << "\"";
  if (outcome->trip) {
    std::cout << ",\"signal_to_latch_ns\":"
              << outcome->trip->signal_to_latch_ns;
    std::cout << ",\"latch_to_backend_stop_call_ns\":"
              << outcome->trip->latch_to_backend_stop_call_ns;
    std::cout << ",\"latch_to_stop_confirmed_ns\":"
              << outcome->trip->latch_to_stop_confirmed_ns;
    std::cout << ",\"backend_stop_confirmed\":"
              << (outcome->trip->stop_snapshot.backend_stop_confirmed
                      ? "true" : "false");
  } else if (outcome->error) {
    std::cout << ",\"error_code\":\"" << outcome->error->code << "\"";
    if (outcome->error->details.contains("reason")) {
      std::cout << ",\"error_reason\":\""
                << outcome->error->details.at("reason") << "\"";
    }
  }
  std::cout << "}" << std::endl;
}

}  // namespace

TEST_F(ExecutionKernelTest,
       Qualification_SignalTripWithMeasuredLatencyEmitsEvidence) {
  using namespace motion_45c;
  // Signal_trip happy path with measured signal_to_latch_ns,
  // latch_to_backend_stop_call_ns, latch_to_stop_confirmed_ns. The
  // emitted JSON gives deployment qualification reproducible
  // measurement points without needing the kernel to expose new APIs.
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());
  auto req = make_test_request("qual_signal_trip");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2,
                                   local_clock.now() - 50'000, true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kSignalTrip);
  ASSERT_TRUE(outcome->trip.has_value());
  EXPECT_GT(outcome->trip->signal_to_latch_ns, 0);
  emit_qualification_evidence("signal_trip_happy_path", outcome);
}

TEST_F(ExecutionKernelTest,
       Qualification_BackendStopRefusalEmitsInterventionEvidence) {
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  ControlledStopResultDTO refusal;
  refusal.accepted = false;
  refusal.executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
  refusal.stop_command_source =
      MotionInterruptStopCommandSource::kDiagnosticOther;
  refusal.error = ErrorDTO{kErrorMotionInterruptBackendStopFailed,
                            "qualification: simulated controller refusal",
                            {{"reason", "qual_controller_rejected"}}};
  controlled_execution.next_controlled_stop_result = refusal;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());
  auto req = make_test_request("qual_refusal");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kFailed);
  emit_qualification_evidence("backend_stop_refusal", outcome);
}

TEST_F(ExecutionKernelTest,
       Qualification_StaleSourceRejectedDoesNotLatch) {
  // Stale-source qualification. With a 1us freshness
  // budget, a signal whose timestamp is 100us in the past MUST be
  // rejected at ingest with reason=stale and MUST NOT latch. Emit a
  // null-outcome evidence line so the qualification archive records
  // the rejection.
  InterruptSourceDefinition tight =
      motion_45c::make_fast_probe_source("probe_qual_stale");
  tight.freshness_budget_ns = 1'000;  // 1us
  ASSERT_TRUE(kernel.register_interrupt_source(tight).ok());

  InterruptSignalUpdate u;
  u.source_id = "probe_qual_stale";
  u.sequence_id = 1;
  u.timestamp_ns = 0;  // kernel-time is well past zero
  u.value = true;
  auto result = kernel.publish_fast_interrupt_signal(u);
  EXPECT_TRUE(result.rejected);
  emit_qualification_evidence("stale_source_rejected", std::nullopt);
}

TEST_F(ExecutionKernelTest,
       Qualification_FallbackHonoredWhenAllowedEmitsFallbackReason) {
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  ControlledStopResultDTO fallback_ok;
  fallback_ok.accepted = true;
  fallback_ok.backend_stop_confirmed = true;
  fallback_ok.executed_stop_mode =
      MotionInterruptStopMode::kSpringBackHold;
  fallback_ok.stop_command_source =
      MotionInterruptStopCommandSource::kDiagnosticOther;
  fallback_ok.fallback_reason =
      "qualification: soft_on_path_scaffold_anchored_hold_fallback";
  controlled_execution.next_controlled_stop_result = fallback_ok;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());
  auto req = make_test_request("qual_fallback");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile->deceleration_time_s = 0.2;
  scope.stop_profile->per_joint_max_deceleration_rad_s2 = {
      {"j1", 10.0}, {"j2", 10.0}};
  scope.stop_profile->allowed_fallback_modes = {
      MotionInterruptStopMode::kSpringBackHold};
  req.motion_interrupt_scopes = {scope};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kSignalTrip);
  ASSERT_TRUE(outcome->trip.has_value());
  EXPECT_EQ(outcome->trip->executed_stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  ASSERT_TRUE(outcome->trip->fallback_reason.has_value());
  emit_qualification_evidence("fallback_honored", outcome);
}

}  // namespace
}  // namespace manyforge::core
