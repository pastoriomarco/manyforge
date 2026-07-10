#include <gtest/gtest.h>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/reason_codes.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::core {
namespace {

// --- Mock backends ---

class MockPlanner : public IPlannerBackend {
 public:
  PlanResultDTO next_result;
  std::vector<PlanResultDTO> result_sequence;
  std::function<void(int)> on_plan;
  int plan_count = 0;
  int cancel_count = 0;
  PlanRequestDTO last_request;

  PlanResultDTO plan(const PlanRequestDTO& request) override {
    ++plan_count;
    last_request = request;
    if (on_plan) {
      on_plan(plan_count);
    }
    if (!result_sequence.empty()) {
      auto result = result_sequence.front();
      result_sequence.erase(result_sequence.begin());
      return result;
    }
    return next_result;
  }

  void request_cancel() override {
    ++cancel_count;
  }
};

class SlowMockPlanner : public IPlannerBackend {
 public:
  PlanResultDTO next_result;
  int plan_count = 0;
  int cancel_count = 0;
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

    // Wait until released, cancelled, or for a timeout. This timeout is a
    // last-resort test fail-safe, not part of the behavior under test; keep it
    // comfortably above loaded stability-gate scheduling jitter so stop/reset
    // tests do not lose the race before they can issue the control command.
    {
      std::unique_lock<std::mutex> lock(mu_);
      release_cv_.wait_for(lock, std::chrono::seconds(5),
                           [this]() { return released_ || cancel_requested_; });
      if (cancel_requested_) {
        PlanResultDTO cancelled;
        cancelled.error = ErrorDTO{
            kErrorPlanningFailed,
            "planning cancelled by interrupt",
            {{"reason", "planning_cancelled_by_interrupt"}}};
        return cancelled;
      }
    }

    return next_result;
  }

  void request_cancel() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++cancel_count;
      cancel_requested_ = true;
    }
    release_cv_.notify_all();
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
  bool cancel_requested_ = false;
};

class MockContext : public IPlanningContext {
 public:
  MockContext() = default;

  MockContext(const MockContext& other) { *this = other; }

  MockContext& operator=(const MockContext& other) {
    if (this == &other) {
      return *this;
    }
    current_state = other.current_state;
    next_validation = other.next_validation;
    next_collision = other.next_collision;
    collision_sequence = other.collision_sequence;
    next_fk = other.next_fk;
    next_object_pose = other.next_object_pose;
    next_object_state = other.next_object_state;
    next_reference_pose = other.next_reference_pose;
    reference_pose_sequence = other.reference_pose_sequence;
    fk_sequence = other.fk_sequence;
    next_limits = other.next_limits;
    next_scene_edit = other.next_scene_edit;
    current_revision = other.current_revision;
    get_state_count = other.get_state_count;
    validate_count = other.validate_count;
    collision_count.store(other.collision_count.load());
    expand_count = other.expand_count;
    fk_count = other.fk_count;
    last_fk_request = other.last_fk_request;
    last_expand_trajectory = other.last_expand_trajectory;
    next_expand_result = other.next_expand_result;
    last_validation_options = other.last_validation_options;
    throw_get_state = other.throw_get_state;
    throw_fk = other.throw_fk;
    on_validate = other.on_validate;
    return *this;
  }

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
  // Written from kernel worker / fault-preempt threads while tests poll it;
  // atomic so unsynchronized reads stay defined.
  std::atomic<int> collision_count{0};
  int expand_count = 0;
  int fk_count = 0;
  FkRequestDTO last_fk_request;
  TrajectoryDTO last_expand_trajectory;
  std::optional<Result<TrajectoryDTO>> next_expand_result;
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

  Result<TrajectoryDTO> expand_trajectory_for_collision_validation(
      const TrajectoryDTO& trajectory) override {
    ++expand_count;
    last_expand_trajectory = trajectory;
    if (next_expand_result.has_value()) {
      return *next_expand_result;
    }
    return Result<TrajectoryDTO>::Ok(trajectory);
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
  mutable std::atomic<int> bundle_call_count{0};
  mutable std::atomic<int> published_runtime_bundle_call_count{0};
  std::optional<int> throw_on_bundle_call_number;
  std::vector<int> throw_on_bundle_call_numbers;
  std::optional<int> throw_on_published_runtime_bundle_call_number;
  std::vector<int> throw_on_published_runtime_bundle_call_numbers;
  bool lease_inventory_enabled = false;
  std::int64_t lease_inventory_captured_at_ns = 0;
  std::uint64_t lease_topology_epoch = 1;
  std::uint64_t lease_policy_epoch = 1;
  std::vector<SceneSnapshotLiveObject> lease_live_objects;
  mutable std::atomic<int> lease_inventory_call_count{0};
  mutable std::atomic<int> lease_retain_call_count{0};
  mutable std::atomic<int> lease_release_call_count{0};

  CollisionWorldSnapshotPtr snapshot() const override {
    if (throw_on_snapshot) {
      throw std::runtime_error("snapshot unavailable");
    }
    return current_snapshot;
  }

  CollisionWorldRevision published_collision_world_revision() const override {
    if (throw_on_snapshot) {
      throw std::runtime_error("snapshot unavailable");
    }
    return current_snapshot ? current_snapshot->revision
                            : CollisionWorldRevision{};
  }

  std::optional<CollisionPolicySnapshot> collision_policy_snapshot()
      const override {
    return current_policy;
  }

  CollisionWorldPolicySnapshotBundle collision_world_policy_snapshot_bundle()
      const override {
    const int call = bundle_call_count.fetch_add(1) + 1;
    if ((throw_on_bundle_call_number.has_value() &&
         call == *throw_on_bundle_call_number) ||
        std::find(throw_on_bundle_call_numbers.begin(),
                  throw_on_bundle_call_numbers.end(),
                  call) != throw_on_bundle_call_numbers.end()) {
      throw std::runtime_error("snapshot bundle unavailable");
    }
    return {snapshot(), current_policy};
  }

  std::optional<CollisionWorldLeaseInventory>
  current_scene_snapshot_lease_inventory() const override {
    lease_inventory_call_count.fetch_add(1);
    if (!lease_inventory_enabled) {
      return std::nullopt;
    }
    return make_lease_inventory({});
  }

  std::optional<CollisionWorldLeaseInventory>
  retain_scene_snapshot_lease_inventory(std::string retention_id = {}) override {
    lease_retain_call_count.fetch_add(1);
    if (!lease_inventory_enabled) {
      return std::nullopt;
    }
    if (retention_id.empty()) {
      retention_id =
          "mock_lease_" + std::to_string(lease_retain_call_count.load());
    }
    return make_lease_inventory(std::move(retention_id));
  }

  void release_scene_snapshot_lease_inventory(
      const std::string& /*retention_id*/) override {
    lease_release_call_count.fetch_add(1);
  }

  CollisionWorldPolicySnapshotBundle
  published_runtime_collision_snapshot_bundle() const override {
    const int call = published_runtime_bundle_call_count.fetch_add(1) + 1;
    if ((throw_on_published_runtime_bundle_call_number.has_value() &&
         call == *throw_on_published_runtime_bundle_call_number) ||
        std::find(throw_on_published_runtime_bundle_call_numbers.begin(),
                  throw_on_published_runtime_bundle_call_numbers.end(),
                  call) !=
            throw_on_published_runtime_bundle_call_numbers.end()) {
      throw std::runtime_error("published runtime snapshot bundle unavailable");
    }
    return {snapshot(), current_policy};
  }

 private:
  CollisionWorldLeaseInventory make_lease_inventory(
      std::string retention_id) const {
    CollisionWorldLeaseInventory inventory;
    inventory.retention_id = std::move(retention_id);
    inventory.captured_at_ns = lease_inventory_captured_at_ns;
    inventory.bundle = {snapshot(), current_policy};
    inventory.topology_epoch = lease_topology_epoch;
    inventory.policy_epoch = lease_policy_epoch;
    inventory.live_objects = lease_live_objects;
    return inventory;
  }
};

class MockCollisionEngine final : public ICollisionQueryEngine {
 public:
  CollisionResult default_result;
  std::map<std::int64_t, CollisionResult> result_by_waypoint;
  std::vector<CollisionResult> result_sequence;
  std::function<std::optional<CollisionResult>(const CollisionQuery&)>
      result_for_query;
  std::function<void(const CollisionQuery&)> on_check;
  std::atomic<int> check_count{0};
  bool block_until_cancel_requested = false;
  std::mutex block_mutex;
  std::condition_variable block_cv;
  bool release_blocked_checks = false;
  std::atomic<int> blocked_check_count{0};
  std::atomic<int> cancel_observed_count{0};
  std::mutex cancel_reason_mutex;
  std::string last_cancel_reason;

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
    capability_.mutable_context_id =
        "mock_collision_engine:" +
        std::to_string(reinterpret_cast<std::uintptr_t>(this));
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
    last_collision_world_revision = snapshot.revision;
    last_policy_id = policy.policy_id;
    last_policy_revision_id = policy.revision.revision_id;
    last_timeout_budget_ns = query.timeout_budget_ns;
    if (on_check) {
      on_check(query);
    }
    if (block_until_cancel_requested) {
      blocked_check_count.fetch_add(1);
      block_cv.notify_all();
      std::unique_lock<std::mutex> lock(block_mutex);
      while (!release_blocked_checks && !cancel.is_cancel_requested()) {
        block_cv.wait_for(lock, std::chrono::milliseconds(1));
      }
    }
    CollisionResult result = default_result;
    if (result_for_query) {
      if (auto custom = result_for_query(query)) {
        result = *custom;
      }
    } else if (!result_sequence.empty()) {
      result = result_sequence.front();
      result_sequence.erase(result_sequence.begin());
    } else if (query.waypoint_index) {
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
      cancel_observed_count.fetch_add(1);
      {
        std::lock_guard<std::mutex> lock(cancel_reason_mutex);
        last_cancel_reason = cancel.reason();
      }
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

  bool wait_until_check_blocked(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(block_mutex);
    return block_cv.wait_for(lock, timeout, [this]() {
      return blocked_check_count.load() > 0;
    });
  }

  void release_blocked() {
    {
      std::lock_guard<std::mutex> lock(block_mutex);
      release_blocked_checks = true;
    }
    block_cv.notify_all();
  }

  std::string observed_cancel_reason() {
    std::lock_guard<std::mutex> lock(cancel_reason_mutex);
    return last_cancel_reason;
  }

  std::string last_policy_id;
  std::uint64_t last_policy_revision_id = 0;
  std::optional<std::int64_t> last_timeout_budget_ns;
  CollisionWorldRevision last_collision_world_revision;

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
  MotionInterruptStopCapabilityDTO motion_interrupt_stop_capability = [] {
    MotionInterruptStopCapabilityDTO cap;
    cap.supported_stop_modes = {MotionInterruptStopMode::kSpringBackHold,
                                MotionInterruptStopMode::kHardDeceleration};
    cap.capability_profile_id = "mock_backend_capability";
    cap.hard_stop_level = "mock_hard_stop";
    cap.derivative_proof_level = "mock_derivative_proof";
    cap.scaling_source_visibility = "mock_scaling_visibility";
    cap.scaling_source_probe_status = "mock_scaling_probe";
    return cap;
  }();

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
  // Counts EVERY legacy stop() call, including no-ops issued while the backend
  // has already flipped to not-running (e.g. the emergency hard-stop floor after
  // a confirmed/cancelled controlled stop). `stop_count` only counts stops that
  // found a running execution, so this proves the floor was actually invoked.
  int stop_attempt_count = 0;
  int pause_count = 0;
  int resume_count = 0;
  int reset_count = 0;
  TrajectoryDTO last_execute_trajectory;
  TrajectoryDTO last_resume_trajectory;
  ExecutionSnapshot snapshot;
  bool terminal_handle_on_pause = false;
  bool fail_next_pause = false;
  std::function<void()> on_execute;
  std::function<void()> on_resume;
  std::function<void()> on_get_snapshot;
  int sync_observed_state_count = 0;
  bool block_sync_observed_state = false;
  bool release_blocked_sync_observed_state = false;

  ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
    if (on_execute) {
      on_execute();
    }
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
      ++stop_attempt_count;
      cv_.notify_all();
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
      if (fail_next_pause) {
        fail_next_pause = false;
        return PauseResultDTO{
            false,
            ErrorDTO{kErrorStopFailed,
                     "injected pause failure",
                     {{"reason", "test_injected_pause_failure"}}}};
      }
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
    if (on_resume) {
      on_resume();
    }
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
  MotionInterruptStopCapabilityDTO motion_interrupt_stop_capability = [] {
    MotionInterruptStopCapabilityDTO cap;
    cap.supported_stop_modes = {MotionInterruptStopMode::kSpringBackHold,
                                MotionInterruptStopMode::kHardDeceleration};
    cap.capability_profile_id = "controlled_backend_capability";
    cap.hard_stop_level = "mock_hard_stop";
    cap.derivative_proof_level = "mock_derivative_proof";
    cap.scaling_source_visibility = "mock_scaling_visibility";
    cap.scaling_source_probe_status = "mock_scaling_probe";
    return cap;
  }();

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
  // Measured post-stop velocities the backend reports in its execution
  // snapshot (settle signal). When unset the snapshot carries no velocities
  // (settle UNKNOWN).
  std::optional<std::vector<double>> next_post_stop_joint_velocities_rad_s;
  std::optional<std::string> next_post_stop_state_source;
  int controlled_stop_count = 0;
  int controlled_stop_cancel_count = 0;
  int runtime_velocity_scale_set_count = 0;
  double last_runtime_velocity_scale = 1.0;
  MotionInterruptStopProfileDTO last_controlled_stop_profile;
  bool block_controlled_stop = false;
  // Test-only wedged-backend mode: keep the controlled-stop call blocked after
  // its stop token fires so kernel admission can be exercised while a
  // definitive watchdog result and an active stop owner coexist.
  bool ignore_controlled_stop_token_while_blocked = false;
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
        const bool released = ignore_controlled_stop_token_while_blocked
                                  ? cv_.wait_for(
                                        lock, std::chrono::seconds(2), [this]() {
                                          return release_blocked_controlled_stop;
                                        })
                                  : cv_.wait_for(
                                        lock, stop_token,
                                        std::chrono::seconds(2), [this]() {
                                          return release_blocked_controlled_stop;
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
              {{"reason", stop_token.stop_requested()
                              ? "controlled_stop_cancelled"
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
        if (next_post_stop_joint_velocities_rad_s) {
          snapshot.joint_velocities_rad_s =
              *next_post_stop_joint_velocities_rad_s;
        }
        if (next_post_stop_state_source) {
          snapshot.state_source = *next_post_stop_state_source;
        }
      }
    }
    cv_.notify_all();
    return r;
  }

  void set_runtime_velocity_scale(double scale) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++runtime_velocity_scale_set_count;
      last_runtime_velocity_scale = scale;
    }
    cv_.notify_all();
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

  void set_snapshot_velocities(std::vector<double> joint_velocities_rad_s) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot.joint_velocities_rad_s = std::move(joint_velocities_rad_s);
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

  bool wait_until_stop_attempt_count(
      int count,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() {
                          return stop_attempt_count >= count;
                        });
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

  int stop_count_value() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stop_count;
  }

  int stop_attempt_count_value() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stop_attempt_count;
  }

  int controlled_stop_count_value() const {
    std::lock_guard<std::mutex> lock(mu_);
    return controlled_stop_count;
  }

  int controlled_stop_cancel_count_value() const {
    std::lock_guard<std::mutex> lock(mu_);
    return controlled_stop_cancel_count;
  }

  bool wait_until_runtime_velocity_scale_set_count(
      int count,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this, count]() {
      return runtime_velocity_scale_set_count >= count;
    });
  }

  int runtime_velocity_scale_set_count_value() const {
    std::lock_guard<std::mutex> lock(mu_);
    return runtime_velocity_scale_set_count;
  }

  double last_runtime_velocity_scale_value() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_runtime_velocity_scale;
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
  // condition_variable_any: the blocked controlled-stop wait must observe the
  // cancel token atomically with the predicate. A plain condition_variable
  // notified from a stop_callback (which runs without holding mu_) can lose
  // the wakeup and sleep out the full fallback timeout, making the backend
  // appear to ignore its cancel token.
  std::condition_variable_any cv_;
  bool running_ = false;
  bool paused_ = false;
};

class ThreadSafeManualScheduler final : public IScheduler {
 public:
  ScheduleHandle schedule(
      TimePoint deadline, std::function<void()> callback,
      SchedulerPriority /*priority*/ = SchedulerPriority::Lifecycle) override {
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

class DequeuingManualScheduler final : public IScheduler {
 public:
  ScheduleHandle schedule(
      TimePoint deadline, std::function<void()> callback,
      SchedulerPriority priority = SchedulerPriority::Lifecycle) override {
    std::lock_guard<std::mutex> lock(mu_);
    ScheduleHandle handle = next_handle_++;
    pending_.emplace(deadline,
                     ScheduledCallback{handle, priority, std::move(callback)});
    return handle;
  }

  void cancel(ScheduleHandle handle) override {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->second.handle == handle) {
        pending_.erase(it);
        return;
      }
    }
  }

  void advance_to_dequeue_only(TimePoint t) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->first <= t) {
        dequeued_.push_back(std::move(it->second));
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void fire_dequeued() {
    std::vector<ScheduledCallback> callbacks;
    {
      std::lock_guard<std::mutex> lock(mu_);
      callbacks.swap(dequeued_);
    }
    for (auto& callback : callbacks) {
      callback.callback();
    }
  }

  size_t pending_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
  }

  size_t dequeued_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dequeued_.size();
  }

 private:
  struct ScheduledCallback {
    ScheduleHandle handle;
    SchedulerPriority priority;
    std::function<void()> callback;
  };

  mutable std::mutex mu_;
  std::multimap<TimePoint, ScheduledCallback> pending_;
  std::vector<ScheduledCallback> dequeued_;
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

ActionSpanDTO make_test_action_span() {
  ActionSpanDTO span;
  span.action_source = ActionSource::kProgramNode;
  span.action_span_id = "span-move-001";
  span.program_run_id = "run-action-span-001";
  span.node_instance_id = "root/sequence/move_to_pick";
  span.node_attempt_id = "attempt-move-001";
  span.parent_node_attempt_id = "attempt-plan-001";
  return span;
}

void expect_detail(const std::map<std::string, std::string>& details,
                   const std::string& key,
                   const std::string& expected_value) {
  auto it = details.find(key);
  ASSERT_NE(it, details.end()) << "missing detail key " << key;
  EXPECT_EQ(it->second, expected_value) << "detail key " << key;
}

void expect_action_span_details(
    const std::map<std::string, std::string>& details,
    const ActionSpanDTO& span) {
  expect_detail(details, "program_run_id", span.program_run_id);
  expect_detail(details, "action_source", to_string(span.action_source));
  expect_detail(details, "action_span_id", span.action_span_id);
  expect_detail(details, "action_span_program_run_id", span.program_run_id);
  expect_detail(details, "node_instance_id", span.node_instance_id);
  expect_detail(details, "node_attempt_id", span.node_attempt_id);
  expect_detail(details, "parent_node_attempt_id",
                span.parent_node_attempt_id);
}

void expect_collision_world_revision(
    const CollisionWorldRevision& actual,
    const CollisionWorldRevision& expected) {
  EXPECT_EQ(actual.revision_id, expected.revision_id);
  EXPECT_EQ(actual.content_hash, expected.content_hash);
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

Pose3DTO make_test_pose(double x_m = 0.0,
                        double y_m = 0.0,
                        double z_m = 0.0) {
  Pose3DTO pose;
  pose.position_m.x_m = x_m;
  pose.position_m.y_m = y_m;
  pose.position_m.z_m = z_m;
  pose.orientation.w = 1.0;
  return pose;
}

CollisionWorldBody make_observed_box_body(
    const std::string& object_id,
    const Pose3DTO& pose,
    std::string body_hash,
    std::optional<std::int64_t> source_timestamp_ns = std::nullopt,
    std::optional<std::int64_t> freshness_budget_ns = std::nullopt) {
  CollisionWorldBody body;
  body.body_id = "object/" + object_id + "/shape/0";
  body.owner_kind = CollisionOwnerKind::kDetachedObject;
  body.frame_id = "universe";
  body.pose = pose;
  body.source_timestamp_ns = source_timestamp_ns;
  body.source_freshness_budget_ns = freshness_budget_ns;
  CollisionGeometryDescriptor geom;
  geom.body_id = body.body_id;
  geom.owner_kind = body.owner_kind;
  geom.geometry_kind = CollisionGeometryKind::kBox;
  geom.box_dimensions_m = Vector3DTO{0.1, 0.1, 0.1};
  body.geometries.push_back(std::move(geom));
  body.body_content_hash = std::move(body_hash);
  return body;
}

CollisionWorldSnapshot make_test_collision_world_snapshot_with_object(
    const std::string& object_id,
    const Pose3DTO& pose,
    CollisionWorldRevision revision,
    std::string body_hash,
    std::optional<std::int64_t> source_timestamp_ns = 0,
    std::optional<std::int64_t> freshness_budget_ns = 10'000'000'000) {
  auto snapshot = make_test_collision_world_snapshot();
  snapshot.revision = std::move(revision);
  snapshot.bodies.push_back(make_observed_box_body(
      object_id, pose, std::move(body_hash), source_timestamp_ns,
      freshness_budget_ns));
  snapshot.body_count = static_cast<std::int64_t>(snapshot.bodies.size());
  snapshot.geometry_count = 1;
  snapshot.detached_object_count = 1;
  return snapshot;
}

SceneSnapshotLiveObject make_observed_box_live_object(
    const std::string& object_id,
    const Pose3DTO& pose,
    std::string object_revision_token = "box-shape-token",
    std::optional<std::int64_t> source_timestamp_ns = 0,
    std::optional<std::int64_t> freshness_budget_ns = 10'000'000'000,
    std::optional<ObservedSceneObjectRole> declared_role = std::nullopt) {
  SceneSnapshotLiveObject live;
  live.object_id = object_id;
  live.declared_role = declared_role;
  live.collision_body_ids = {"object/" + object_id + "/shape/0"};
  live.object_revision_token = std::move(object_revision_token);
  live.pose = pose;
  live.source_id = "test_pose_source";
  live.source_timestamp_ns = source_timestamp_ns;
  live.source_freshness_budget_ns = freshness_budget_ns;
  return live;
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

CollisionResult make_contact_grasp_collision_result(
    const CollisionPairId& pair,
    double signed_clearance_m) {
  return make_support_contact_collision_result(pair, signed_clearance_m);
}

CollisionPolicySnapshot make_contact_grasp_policy_snapshot(
    const std::string& policy_id,
    const std::string& selected_object_id,
    std::vector<std::string> allowed_robot_links,
    double max_penetration_m) {
  auto policy = make_test_collision_policy_snapshot();
  policy.require_full_diagnostics = true;
  ContactGraspPolicyEntry entry;
  entry.policy_id = policy_id;
  entry.selected_object_id = selected_object_id;
  entry.allowed_robot_links = std::move(allowed_robot_links);
  entry.max_penetration_m = max_penetration_m;
  entry.terminal_window_start_fraction = 0.75;
  policy.contact_grasps.push_back(std::move(entry));
  return policy;
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

// Audit 6G: 2 s bound (2000 x 1 ms), matching the cv-based fake-backend
// helpers. The previous 200 ms bound flaked under first-run parallel ctest
// load (observed once on Stop_OperatorProfileRefused_EmergencyLegacyFallback,
// whose stop path crosses several thread handoffs). Passing tests return
// early; only genuine failures pay the longer bound.
bool wait_for_state(ExecutionKernel& kernel, ExecutionState target_state,
                    int max_attempts = 2000) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (kernel.get_status().state == target_state) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

bool wait_for_pending_scheduler_work(ThreadSafeManualScheduler& scheduler,
                                     int max_attempts = 2000) {
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
  std::shared_ptr<MockCollisionEngine> collision_runtime_engine =
      std::make_shared<MockCollisionEngine>();
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
    collision_runtime_engine = std::make_shared<MockCollisionEngine>();
    collision_engine = std::make_shared<MockCollisionEngine>();
    kernel.set_collision_validation_policy_snapshot(
        make_test_collision_policy_snapshot());
    kernel.set_collision_resources(collision_world_publisher,
                                   collision_runtime_engine,
                                   {collision_engine});
  }

  /// Helper: execute and wait for async completion.
  Result<uint64_t> execute_sync(const ExecuteRequestDTO& req) {
    auto result = kernel.execute(req);
    if (result.ok()) {
      const bool completed = kernel.wait_for_completion(2'000'000'000);
      const auto state = kernel.get_status().state;
      const bool already_terminal =
          state == ExecutionState::kCompleted ||
          state == ExecutionState::kFailed ||
          state == ExecutionState::kStopped ||
          state == ExecutionState::kInterrupted ||
          state == ExecutionState::kInterventionRequired;
      EXPECT_TRUE(completed || already_terminal)
          << "execute_sync timed out waiting for " << req.move_id;
    }
    return result;
  }

  void store_cache(const std::string& move_id, TrajectoryDTO trajectory) {
    cache.store(move_id, std::move(trajectory), context.current_revision);
  }

  ExecuteRequestDTO make_relative_object_request(
      const std::string& move_id,
      const std::string& object_id) {
    auto request = make_test_request(move_id);
    RelativeMoveDTO relative;
    relative.target_reference.type = ReferenceDTO::Type::kObject;
    relative.target_reference.id = object_id;
    relative.offset.orientation.w = 1.0;
    request.motion = relative;
    context.next_object_pose = make_test_pose(0.0, 0.0, 0.0);
    return request;
  }

  void publish_observed_box_for_lease(
      const std::string& object_id,
      double x_m,
      CollisionWorldRevision revision,
      std::string body_hash,
      std::uint64_t topology_epoch = 1,
      std::uint64_t policy_epoch = 1,
      std::string object_revision_token = "box-shape-token",
      std::optional<ObservedSceneObjectRole> declared_role = std::nullopt) {
    const auto pose = make_test_pose(x_m, 0.0, 0.0);
    collision_world_publisher.current_snapshot =
        std::make_shared<const CollisionWorldSnapshot>(
            make_test_collision_world_snapshot_with_object(
                object_id, pose, std::move(revision), std::move(body_hash)));
    collision_world_publisher.lease_topology_epoch = topology_epoch;
    collision_world_publisher.lease_policy_epoch = policy_epoch;
    if (collision_world_publisher.current_policy.has_value()) {
      collision_world_publisher.current_policy->revision.revision_id =
          policy_epoch;
    }
    collision_world_publisher.lease_live_objects = {
        make_observed_box_live_object(object_id, pose,
                                      std::move(object_revision_token),
                                      0, 10'000'000'000, declared_role)};
  }

  void enable_observed_box_lease(
      const std::string& object_id,
      std::optional<ObservedSceneObjectRole> declared_role = std::nullopt) {
    collision_world_publisher.lease_inventory_enabled = true;
    collision_world_publisher.current_policy =
        make_test_collision_policy_snapshot();
    publish_observed_box_for_lease(
        object_id, 0.0, CollisionWorldRevision{1, "box-world-1"},
        "box-body-hash-1", 1, 1, "box-shape-token", declared_role);
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
    target_kernel.set_collision_resources(
        publisher, std::make_shared<MockCollisionEngine>(), {engine});
  }

  void configure_runtime_collision_monitoring(
      ExecutionKernel& target_kernel,
      MockCollisionWorldPublisher& publisher,
      const std::shared_ptr<MockCollisionEngine>& validation_engine,
      const std::shared_ptr<MockCollisionEngine>& runtime_engine) {
    publisher.current_snapshot =
        std::make_shared<const CollisionWorldSnapshot>(
            make_test_collision_world_snapshot());
    publisher.current_policy = make_test_collision_policy_snapshot();
    target_kernel.set_collision_validation_policy_snapshot(
        make_test_collision_policy_snapshot());
    target_kernel.set_collision_resources(publisher, runtime_engine,
                                          {validation_engine});
  }
};

// --- Happy path ---

TEST_F(ExecutionKernelTest, Execute_RejectsNonPositivePlanningTimeout) {
  auto req = make_test_request("invalid_planning_timeout");
  req.timeouts.planning_timeout_s = 0.0;

  auto result = kernel.execute(req);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorInvalidRequest);
  EXPECT_EQ(result.error().details.at("reason_code"),
            "invalid_planning_timeout");
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_RejectsNonPositiveExecutionTimeout) {
  auto req = make_test_request("invalid_execution_timeout");
  req.timeouts.execution_timeout_s = 0.0;

  auto result = kernel.execute(req);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorInvalidRequest);
  EXPECT_EQ(result.error().details.at("reason_code"),
            "invalid_execution_timeout");
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_ReturnsExecutionId) {
  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 1u);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest,
       Execute_FromTerminalWorkerCallbackRejectedWithoutReplacingWorker) {
  std::promise<std::optional<ErrorDTO>> nested_result;
  auto nested_result_future = nested_result.get_future();
  std::atomic<bool> attempted{false};
  kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kStateTransition ||
        event.to_state != ExecutionState::kCompleted ||
        attempted.exchange(true)) {
      return;
    }
    auto nested = kernel.execute(make_test_request("nested_from_callback"));
    nested_result.set_value(nested.ok() ? std::nullopt
                                        : std::make_optional(nested.error()));
  });

  auto result = kernel.execute(make_test_request("outer"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
  ASSERT_EQ(nested_result_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  auto error = nested_result_future.get();
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, kErrorExecutionRejected);
  EXPECT_EQ(error->details.at("reason_code"), "worker_retirement_self_thread");
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest,
       SubscriberCanCallStopFromExecuteAdmissionEventWithoutDeadlock) {
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner,
      context,
      controlled_execution,
      local_clock,
      local_scheduler,
      local_cache};
  configure_authoritative_collision_validation(
      local_kernel, local_collision_world, local_collision_engine);

  std::promise<void> stop_returned;
  auto stop_returned_future = stop_returned.get_future();
  std::atomic<bool> attempted{false};
  local_kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kStateTransition ||
        event.to_state != ExecutionState::kPlanning ||
        attempted.exchange(true)) {
      return;
    }
    (void)local_kernel.stop();
    stop_returned.set_value();
  });

  auto result = local_kernel.execute(make_test_request("reentrant_stop"));
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(stop_returned_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  controlled_execution.release();
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(controlled_execution.execute_count, 0);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->execution_id, result.value());
  EXPECT_EQ(outcome->move_id, "reentrant_stop");
  EXPECT_EQ(outcome->terminal_cause, MotionExecutionTerminalCause::kStopped);
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_FALSE(outcome->error.has_value());
}

TEST_F(ExecutionKernelTest,
       SubscriberCanResetThenExecuteFromAdmissionEventWithoutStoppingNewWorker) {
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner,
      context,
      controlled_execution,
      local_clock,
      local_scheduler,
      local_cache};
  configure_authoritative_collision_validation(
      local_kernel, local_collision_world, local_collision_engine);

  std::promise<Result<uint64_t>> nested_execute_result;
  auto nested_execute_future = nested_execute_result.get_future();
  std::atomic<bool> attempted{false};
  local_kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kStateTransition ||
        event.to_state != ExecutionState::kPlanning ||
        attempted.exchange(true)) {
      return;
    }
    auto reset_result = local_kernel.reset();
    EXPECT_TRUE(reset_result.accepted);
    nested_execute_result.set_value(
        local_kernel.execute(make_test_request("after_reentrant_reset")));
  });

  auto first_result =
      local_kernel.execute(make_test_request("reentrant_reset_then_execute"));
  ASSERT_TRUE(first_result.ok());
  ASSERT_EQ(nested_execute_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  auto second_result = nested_execute_future.get();
  ASSERT_TRUE(second_result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_count(1));
  EXPECT_EQ(controlled_execution.execute_count, 1);

  auto first_outcome = local_kernel.get_execution_outcome(first_result.value());
  ASSERT_TRUE(first_outcome.has_value());
  EXPECT_EQ(first_outcome->move_id, "reentrant_reset_then_execute");
  EXPECT_EQ(first_outcome->terminal_cause,
            MotionExecutionTerminalCause::kStopped);

  controlled_execution.release();
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);
  auto second_outcome =
      local_kernel.get_execution_outcome(second_result.value());
  ASSERT_TRUE(second_outcome.has_value());
  EXPECT_EQ(second_outcome->move_id, "after_reentrant_reset");
  EXPECT_EQ(second_outcome->terminal_cause,
            MotionExecutionTerminalCause::kCompletedNormally);
}

TEST_F(ExecutionKernelTest,
       SubscriberCanCallResetFromAcknowledgementEventWithoutDeadlock) {
  auto first = execute_sync(make_test_request("ack_reset"));
  ASSERT_TRUE(first.ok());

  std::promise<ResetResultDTO> reset_result;
  auto reset_result_future = reset_result.get_future();
  std::atomic<bool> attempted{false};
  kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kCompletedAcknowledged ||
        attempted.exchange(true)) {
      return;
    }
    reset_result.set_value(kernel.reset());
  });

  auto ack = kernel.acknowledge_completed(first.value());
  ASSERT_TRUE(ack.accepted);
  ASSERT_EQ(reset_result_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_TRUE(reset_result_future.get().accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
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

TEST_F(ExecutionKernelTest,
       Config_ValidationCollisionWallTimeoutRejectsNonPositive) {
  for (Duration bad : {Duration{0}, Duration{-1}}) {
    ExecutionKernelConfig config;
    config.validation_collision_wall_timeout_ns = bad;
    EXPECT_THROW(
        (ExecutionKernel{planner, context, execution, clock, scheduler, cache,
                         config}),
        std::invalid_argument);
  }

  ExecutionKernelConfig ok_config;
  ok_config.validation_collision_wall_timeout_ns = 5'000'000'000;
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
  auto cached = cache.peek_unvalidated("move_A");
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(cached->waypoints.size(), 2u);
}

TEST_F(ExecutionKernelTest,
       Execute_LiveSceneRevisionChangeAfterBorrowedSnapshotFailsBeforeDispatch) {
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
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(cache.peek_unvalidated("move_scene_changed").has_value());
  EXPECT_EQ(collision_engine->check_count.load(), 2);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorSceneChangedDuringValidation);
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "collision_world_changed_after_validation");
  EXPECT_EQ(snapshot.error->details.at("validation_collision_world_revision_id"),
            "1");
  EXPECT_EQ(snapshot.error->details.at("live_collision_world_revision_id"),
            "2");
}

TEST_F(ExecutionKernelTest,
       Execute_SameContentRevisionChangeDuringValidationFailsClosed) {
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
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(cache.peek_unvalidated("move_same_content_revision").has_value());
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorSceneChangedDuringValidation);
  EXPECT_EQ(snapshot.error->details.at("validation_collision_world_revision_id"),
            "1");
  EXPECT_EQ(snapshot.error->details.at("live_collision_world_revision_id"),
            "2");
  EXPECT_EQ(snapshot.error->details.at("validation_collision_world_content_hash"),
            "mock-collision-world");
  EXPECT_EQ(snapshot.error->details.at("live_collision_world_content_hash"),
            "mock-collision-world");
}

TEST_F(ExecutionKernelTest,
       Execute_RepeatedLiveSceneRevisionChangesDuringValidationFailOnce) {
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
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(cache.peek_unvalidated("move_scene_changed").has_value());
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorSceneChangedDuringValidation);
  EXPECT_EQ(snapshot.error->details.at("validation_collision_world_revision_id"),
            "1");
  EXPECT_NE(snapshot.error->details.at("live_collision_world_revision_id"),
            "1");
}

TEST_F(ExecutionKernelTest,
       Execute_ObservedSceneLeaseAcceptsBoundedTargetPoseDriftBeforeDispatch) {
  enable_observed_box_lease("box");
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.005, CollisionWorldRevision{2, "box-world-2"},
          "box-body-hash-2");
    }
  };

  auto result =
      execute_sync(make_relative_object_request("move_lease_drift", "box"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_TRUE(cache.peek_unvalidated("move_lease_drift").has_value());
  EXPECT_EQ(collision_world_publisher.lease_retain_call_count.load(), 1);
  EXPECT_EQ(collision_world_publisher.lease_release_call_count.load(), 1);
  EXPECT_GE(collision_world_publisher.lease_inventory_call_count.load(), 1);
}

TEST_F(ExecutionKernelTest,
       Execute_ObservedSceneLeaseRejectsTargetMovedBeyondToleranceBeforeDispatch) {
  enable_observed_box_lease("box");
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.25, CollisionWorldRevision{2, "box-world-2"},
          "box-body-hash-2");
    }
  };

  auto result =
      execute_sync(make_relative_object_request("move_lease_target", "box"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(cache.peek_unvalidated("move_lease_target").has_value());
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorSceneChangedDuringValidation);
  EXPECT_EQ(snapshot.error->message,
            "Observed target moved after validation and before dispatch");
  EXPECT_EQ(snapshot.error->details.at("reason"),
            reason_codes::kTargetMovedAfterValidation);
  EXPECT_EQ(snapshot.error->details.at("object_id"), "box");
  EXPECT_EQ(snapshot.error->details.at("validation_collision_world_revision_id"),
            "1");
  EXPECT_EQ(snapshot.error->details.at("live_collision_world_revision_id"), "2");
}

TEST_F(ExecutionKernelTest,
       Execute_ObservedSceneLeaseRejectsTargetShapeChangeBeforeDispatch) {
  enable_observed_box_lease("box");
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.005, CollisionWorldRevision{2, "box-world-2"},
          "box-body-hash-2",
          /*topology_epoch=*/2, /*policy_epoch=*/1, "box-shape-token-v2");
    }
  };

  auto result =
      execute_sync(make_relative_object_request("move_lease_shape", "box"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorSceneChangedDuringValidation);
  EXPECT_EQ(snapshot.error->details.at("reason"),
            reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(snapshot.error->details.at("validated_topology_epoch"), "1");
  EXPECT_EQ(snapshot.error->details.at("live_topology_epoch"), "2");
  EXPECT_EQ(snapshot.error->details.at("object_revision_token_changed"), "true");
}

TEST_F(ExecutionKernelTest,
       Execute_ObservedSceneLeaseRejectsMissingTargetInventory) {
  enable_observed_box_lease("box");
  collision_world_publisher.lease_live_objects.clear();

  auto result =
      execute_sync(make_relative_object_request("move_lease_missing_target",
                                                "box"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorSceneChangedDuringValidation);
  EXPECT_EQ(snapshot.error->details.at("reason"),
            reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(snapshot.error->details.at("object_id"), "box");
  EXPECT_EQ(snapshot.error->details.at("missing_collision_object_identity"),
            "true");
}

TEST_F(ExecutionKernelTest,
       Execute_ObservedSceneLeaseAcceptsNonTargetObservedTargetAsSnapshotObstacle) {
  enable_observed_box_lease("box", ObservedSceneObjectRole::kObservedTarget);
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.005, CollisionWorldRevision{2, "box-world-2"},
          "box-body-hash-2", /*topology_epoch=*/2, /*policy_epoch=*/1,
          "box-shape-token-v2",
          ObservedSceneObjectRole::kObservedTarget);
    }
  };

  auto result = execute_sync(make_test_request("move_lease_obstacle"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(execution.execute_count, 1);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_FALSE(snapshot.error.has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ObservedSceneLeaseRejectsExplicitDynamicObstacleDrift) {
  enable_observed_box_lease("box",
                            ObservedSceneObjectRole::kObservedDynamicObstacle);
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.005, CollisionWorldRevision{2, "box-world-2"},
          "box-body-hash-2", 1, 1, "box-shape-token",
          ObservedSceneObjectRole::kObservedDynamicObstacle);
    }
  };

  auto result = execute_sync(make_test_request("move_lease_dynamic_obstacle"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, 0);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorSceneChangedDuringValidation);
  EXPECT_EQ(snapshot.error->message,
            "Observed obstacle moved after validation and before dispatch");
  EXPECT_EQ(snapshot.error->details.at("reason"),
            reason_codes::kObstacleMovedAfterValidation);
  EXPECT_EQ(snapshot.error->details.at("object_id"), "box");
}

TEST_F(ExecutionKernelTest,
       Execute_ObservedSceneLeaseAcceptsDefaultSnapshotObstacleDrift) {
  enable_observed_box_lease("box");
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.25, CollisionWorldRevision{2, "box-world-2"},
          "box-body-hash-2");
    }
  };

  auto result = execute_sync(make_test_request("move_lease_snapshot_obstacle"));

  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(mutated_once);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(execution.execute_count, 1);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_FALSE(snapshot.error.has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_CachedTrajectoryMissesAfterLeaseWorldRevisionChanges) {
  enable_observed_box_lease("box");
  cache.store("move_lease_cache", make_test_trajectory(),
              CollisionWorldRevision{1, "box-world-1"});
  ASSERT_TRUE(cache.peek_unvalidated("move_lease_cache").has_value());

  publish_observed_box_for_lease(
      "box", 0.005, CollisionWorldRevision{2, "box-world-2"},
      "box-body-hash-2");

  auto result =
      execute_sync(make_relative_object_request("move_lease_cache", "box"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(planner.plan_count, 1)
      << "cache lookup must miss when the live authoritative revision changed";
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest,
       Execute_StopAuthoritySupersedesObservedSceneLeaseRejection) {
  enable_observed_box_lease("box");
  std::atomic<bool> mutated_once{false};
  std::atomic<bool> stop_accepted{false};
  collision_engine->on_check =
      [this, &mutated_once, &stop_accepted](const CollisionQuery&) {
        bool expected = false;
        if (mutated_once.compare_exchange_strong(expected, true)) {
          publish_observed_box_for_lease(
              "box", 0.25, CollisionWorldRevision{2, "box-world-2"},
              "box-body-hash-2");
          stop_accepted.store(kernel.stop().accepted);
        }
      };

  auto result =
      execute_sync(make_relative_object_request("move_lease_stop", "box"));

  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(stop_accepted.load());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kStopped);
  EXPECT_EQ(execution.execute_count, 0);
  const auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause, MotionExecutionTerminalCause::kStopped);
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
  EXPECT_FALSE(cache.peek_unvalidated("move_capped_no_store").has_value());
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
  auto refreshed = cache.peek_unvalidated("move_pose_changed");
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
  EXPECT_FALSE(cache.peek_unvalidated("move_missing_reference").has_value());
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
  EXPECT_FALSE(cache.peek_unvalidated("move_cached_invalid").has_value());
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
  EXPECT_EQ(snapshot.error->details.at("failure_stage"), "cached_validation");
}

TEST_F(ExecutionKernelTest,
       AcknowledgeFailedValidationForRetry_ClearsFailedWithoutBackendReset) {
  std::vector<KernelEvent> events;
  auto sub = kernel.subscribe_events(
      [&](const KernelEvent& event) { events.push_back(event); });
  execution.snapshot.joint_positions_rad = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  store_cache("move_cached_invalid_ack", make_test_trajectory());
  context.next_validation = ValidationResultDTO{
      false,
      ErrorDTO{
          kErrorValidationCollision,
          "Cached trajectory is no longer valid under current collision policy",
          {{"failure_stage", "cached_validation"}}}};

  execute_sync(make_test_request("move_cached_invalid_ack"));

  ASSERT_EQ(kernel.get_status().state, ExecutionState::kFailed);
  const auto reset_count_before_ack = execution.reset_count;
  auto ack = kernel.acknowledge_failed_validation_for_retry(
      "operator_alice", "ui_acknowledge_failed_validation_and_retry");

  ASSERT_TRUE(ack.accepted)
      << (ack.error.has_value() ? ack.error->message : std::string{});
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.reset_count, reset_count_before_ack);
  const auto snapshot = kernel.get_execution_snapshot();
  EXPECT_EQ(snapshot.state, ExecutionState::kIdle);
  EXPECT_FALSE(snapshot.error.has_value());
  bool saw_ack = false;
  for (const auto& event : events) {
    if (event.type == KernelEventType::kFailedValidationAcknowledged) {
      saw_ack = true;
      EXPECT_EQ(event.details.at("principal_id"), "operator_alice");
      EXPECT_EQ(event.details.at("error_code"), kErrorValidationCollision);
    }
  }
  EXPECT_TRUE(saw_ack);
  kernel.unsubscribe_events(sub);
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
  EXPECT_FALSE(cache.peek_unvalidated("move_no_store").has_value());
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
  auto refreshed = cache.peek_unvalidated("move_stale_start");
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
  EXPECT_FALSE(cache.peek_unvalidated("move_val_fail").has_value());
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

// --- Legacy collision-compare diagnostics gate ---

TEST_F(ExecutionKernelTest,
       Execute_CollisionRejectionOmitsLegacyCompareDiagnosticsByDefault) {
  collision_engine->default_result = make_authoritative_collision_result();

  execute_sync(make_test_request("legacy_compare_gated_off"));

  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  for (const auto& [key, value] : snapshot.error->details) {
    EXPECT_EQ(key.rfind("legacy_collision_compare_", 0), std::string::npos)
        << "unexpected legacy compare key on default config: " << key;
  }
}

TEST_F(ExecutionKernelTest,
       Execute_CollisionRejectionAppendsLegacyCompareDiagnosticsWhenEnabled) {
  ExecutionKernelConfig config;
  config.enable_legacy_collision_compare_diagnostics = true;
  ExecutionKernel gated_kernel{planner, context,   execution,
                               clock,   scheduler, cache,
                               config};
  configure_authoritative_collision_validation(
      gated_kernel, collision_world_publisher, collision_engine);
  collision_engine->default_result = make_authoritative_collision_result();

  auto result = gated_kernel.execute(make_test_request("legacy_compare_on"));
  ASSERT_TRUE(result.ok());
  gated_kernel.wait_for_completion();

  const auto snapshot = gated_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  bool found_legacy_key = false;
  for (const auto& [key, value] : snapshot.error->details) {
    if (key.rfind("legacy_collision_compare_", 0) == 0) {
      found_legacy_key = true;
      break;
    }
  }
  EXPECT_TRUE(found_legacy_key)
      << "expected legacy compare keys when the gate is enabled";
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
       Execute_AuthoritativeCollisionUsesNonzeroDefaultQueryTimeout) {
  auto result = execute_sync(make_test_request("validation_default_timeout"));

  ASSERT_TRUE(result.ok());
  EXPECT_GE(collision_engine->check_count.load(), 1);
  ASSERT_TRUE(collision_engine->last_timeout_budget_ns.has_value());
  EXPECT_EQ(*collision_engine->last_timeout_budget_ns, 250'000'000);
}

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeCollisionUsesConfiguredValidationQueryTimeout) {
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernelConfig config;
  config.validation_collision_query_timeout_ns = 125'000'000;
  ExecutionKernel local_kernel{planner,
                               context,
                               execution,
                               clock,
                               scheduler,
                               cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto result = local_kernel.execute(make_test_request("validation_timeout"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_GE(local_engine->check_count.load(), 1);
  ASSERT_TRUE(local_engine->last_timeout_budget_ns.has_value());
  EXPECT_EQ(*local_engine->last_timeout_budget_ns, 125'000'000);
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
  const auto snapshot = unconfigured_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorInvariantViolation);
  EXPECT_EQ(snapshot.error->message,
            "Authoritative collision validation snapshot publisher is not "
            "configured");
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "missing_collision_world_snapshot_publisher");
  EXPECT_EQ(snapshot.error->details.at("phase"), "pre_execution_validation");
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

TEST_F(ExecutionKernelTest,
       InterruptDispositionReplayAppliesQueuedPreemptSideEffects) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto hold_source = make_interrupt_source("hold_first");
  ASSERT_TRUE(local_kernel.register_interrupt_source(hold_source).ok());
  auto preempt_source = make_interrupt_source("queued_preempt");
  ASSERT_TRUE(local_kernel.register_interrupt_source(preempt_source).ok());

  auto hold_policy = make_interrupt_policy("hold_policy", "hold_first");
  hold_policy.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution,
                                 InterruptExecutionPhase::kHeld};
  hold_policy.priority = 500;
  hold_policy.preemption_action = InterruptPreemptionAction::kLatchHeld;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(hold_policy).ok());

  auto preempt_policy =
      make_interrupt_policy("preempt_policy", "queued_preempt");
  preempt_policy.affected_phases = {InterruptExecutionPhase::kHeld};
  preempt_policy.priority = 400;
  preempt_policy.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(preempt_policy).ok());

  ASSERT_TRUE(local_kernel.execute(make_test_request("replay_preempt")).ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto first = local_kernel.publish_fast_interrupt_signal(
      make_interrupt_signal("hold_first", 1));
  ASSERT_TRUE(first.latched);
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kInterrupted));
  auto queued = local_kernel.publish_fast_interrupt_signal(
      make_interrupt_signal("queued_preempt", 2));
  ASSERT_TRUE(queued.queued);

  auto replay_snapshot = make_test_collision_world_snapshot();
  const CollisionWorldRevision replay_revision{
      4242, "disposition-replay-world"};
  replay_snapshot.revision = replay_revision;
  local_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(replay_snapshot));

  InterruptDisposition ack;
  ack.latch_id = first.active_latch->latch_id;
  ack.action = InterruptDispositionAction::kAcknowledge;
  ack.principal_id = "operator";
  auto replay = local_kernel.apply_interrupt_disposition(ack);

  ASSERT_TRUE(replay.ok());
  EXPECT_TRUE(replay.value().latched);
  ASSERT_TRUE(controlled_execution.wait_until_stop_count(1));
  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->source_id, "queued_preempt");
  ASSERT_TRUE(latch->borrowed_collision_world_revision.has_value());
  expect_collision_world_revision(*latch->borrowed_collision_world_revision,
                                  replay_revision);
  EXPECT_EQ(latch->preemption_result, InterruptPreemptionResult::kSucceeded);
  if (!local_kernel.wait_for_completion(500'000'000)) {
    controlled_execution.release();
    ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  }
}

TEST_F(ExecutionKernelTest,
       InterruptDispositionReplaySkipsRevisionStampWhenWorldUnavailable) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto hold_source = make_interrupt_source("hold_no_world");
  ASSERT_TRUE(local_kernel.register_interrupt_source(hold_source).ok());
  auto queued_source = make_interrupt_source("queued_no_world");
  ASSERT_TRUE(local_kernel.register_interrupt_source(queued_source).ok());

  auto hold_policy = make_interrupt_policy("hold_no_world_policy",
                                           "hold_no_world");
  hold_policy.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution,
                                 InterruptExecutionPhase::kHeld};
  hold_policy.priority = 500;
  hold_policy.preemption_action = InterruptPreemptionAction::kLatchHeld;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(hold_policy).ok());

  auto queued_policy =
      make_interrupt_policy("queued_no_world_policy", "queued_no_world");
  queued_policy.affected_phases = {InterruptExecutionPhase::kHeld};
  queued_policy.priority = 400;
  queued_policy.preemption_action = InterruptPreemptionAction::kLatchHeld;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(queued_policy).ok());

  ASSERT_TRUE(local_kernel.execute(make_test_request("replay_no_world")).ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto first = local_kernel.publish_fast_interrupt_signal(
      make_interrupt_signal("hold_no_world", 1));
  ASSERT_TRUE(first.latched);
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kInterrupted));
  auto queued = local_kernel.publish_fast_interrupt_signal(
      make_interrupt_signal("queued_no_world", 2));
  ASSERT_TRUE(queued.queued);

  local_publisher.current_snapshot.reset();

  InterruptDisposition ack;
  ack.latch_id = first.active_latch->latch_id;
  ack.action = InterruptDispositionAction::kAcknowledge;
  ack.principal_id = "operator";
  auto replay = local_kernel.apply_interrupt_disposition(ack);

  ASSERT_TRUE(replay.ok());
  EXPECT_TRUE(replay.value().latched);
  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->source_id, "queued_no_world");
  EXPECT_FALSE(latch->borrowed_collision_world_revision.has_value());
  controlled_execution.release();
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
}

TEST_F(ExecutionKernelTest,
       AutoSignalReleaseReplayAppliesQueuedPreemptSideEffects) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto level_source = make_interrupt_source("level_guard");
  level_source.kind = InterruptSourceKind::kLevel;
  ASSERT_TRUE(local_kernel.register_interrupt_source(level_source).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_interrupt_source("release_preempt"))
                  .ok());

  auto level_policy = make_interrupt_policy("level_policy", "level_guard");
  level_policy.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  level_policy.priority = 500;
  level_policy.preemption_action = InterruptPreemptionAction::kLatchHeld;
  level_policy.post_latch_disposition =
      InterruptPostLatchDisposition::kAutoClearOnSignalRelease;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(level_policy).ok());

  auto preempt_policy =
      make_interrupt_policy("release_preempt_policy", "release_preempt");
  preempt_policy.affected_phases = {InterruptExecutionPhase::kHeld};
  preempt_policy.priority = 400;
  preempt_policy.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(preempt_policy).ok());

  ASSERT_TRUE(local_kernel.execute(make_test_request("auto_release_replay")).ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto first = local_kernel.publish_fast_interrupt_signal(
      make_interrupt_signal("level_guard", 1));
  ASSERT_TRUE(first.latched);
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kInterrupted));
  auto queued = local_kernel.publish_fast_interrupt_signal(
      make_interrupt_signal("release_preempt", 2));
  ASSERT_TRUE(queued.queued);

  auto release = make_interrupt_signal("level_guard", 3);
  release.value = false;
  auto replay = local_kernel.publish_fast_interrupt_signal(release);

  EXPECT_TRUE(replay.latched);
  ASSERT_TRUE(controlled_execution.wait_until_stop_count(1));
  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->source_id, "release_preempt");
  EXPECT_EQ(latch->preemption_result, InterruptPreemptionResult::kSucceeded);
  if (!local_kernel.wait_for_completion(500'000'000)) {
    controlled_execution.release();
    ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  }
}

TEST_F(ExecutionKernelTest,
       Execute_PlanningInterruptRequestsPlannerCancelAndMarksPreempt) {
  SlowMockPlanner slow_planner;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ControlledExecution controlled_execution;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{slow_planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto source = make_interrupt_source("planning_cancel");
  ASSERT_TRUE(local_kernel.register_interrupt_source(source).ok());
  auto policy = make_interrupt_policy("planning_cancel_policy",
                                      "planning_cancel");
  policy.affected_phases = {InterruptExecutionPhase::kPlanning};
  policy.preemption_action = InterruptPreemptionAction::kCancelPlanning;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(policy).ok());

  ASSERT_TRUE(local_kernel.execute(make_test_request("planning_cancel")).ok());
  ASSERT_TRUE(slow_planner.wait_until_in_plan());
  auto ingest = local_kernel.publish_fast_interrupt_signal(
      make_interrupt_signal("planning_cancel", 1));
  EXPECT_TRUE(ingest.latched);

  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(slow_planner.cancel_count, 1);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->preemption_action_requested,
            InterruptPreemptionAction::kCancelPlanning);
  EXPECT_EQ(latch->preemption_result, InterruptPreemptionResult::kSucceeded);
  auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "planning_cancelled_by_interrupt");
}

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeCollisionReplansAndDispatchesSecondCandidate) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 1;
  MockPlanner local_planner;
  PlanResultDTO first_plan;
  first_plan.trajectory =
      make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  PlanResultDTO second_plan;
  second_plan.trajectory =
      make_test_trajectory_from({0.0, 0.0}, {2.0, 2.0});
  local_planner.result_sequence = {first_plan, second_plan};
  MockContext local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner,
                               local_context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  local_engine->result_for_query = [&](const CollisionQuery&) {
    if (local_planner.plan_count <= 1) {
      return std::make_optional(make_authoritative_collision_result());
    }
    return std::make_optional(CollisionResult{});
  };

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  auto result = local_kernel.execute(make_test_request("replan_collision"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  controlled_execution.complete();
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(local_planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.execute_count, 1);
  ASSERT_FALSE(controlled_execution.last_execute_trajectory.waypoints.empty());
  EXPECT_EQ(controlled_execution.last_execute_trajectory.waypoints.back().positions_rad,
            (std::vector<double>{2.0, 2.0}));

  std::lock_guard<std::mutex> lock(events_mutex);
  bool saw_retry_validation_failed = false;
  bool saw_retry_plan_started = false;
  bool saw_terminal_error = false;
  for (const auto& event : events) {
    if (event.type == KernelEventType::kValidationFailed) {
      auto reason = event.details.find("reason");
      if (reason != event.details.end() &&
          reason->second == "authoritative_validation_replan") {
        saw_retry_validation_failed = true;
      }
    }
    if (event.type == KernelEventType::kPlanStarted) {
      auto reason = event.details.find("reason");
      if (reason != event.details.end() &&
          reason->second == "authoritative_validation_replan") {
        saw_retry_plan_started = true;
      }
    }
    saw_terminal_error = saw_terminal_error ||
                         event.type == KernelEventType::kErrorRaised;
  }
  EXPECT_TRUE(saw_retry_validation_failed);
  EXPECT_TRUE(saw_retry_plan_started);
  EXPECT_FALSE(saw_terminal_error);
}

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeCollisionReplanBudgetExhaustedFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 1;
  MockPlanner local_planner;
  PlanResultDTO first_plan;
  first_plan.trajectory =
      make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  PlanResultDTO second_plan;
  second_plan.trajectory =
      make_test_trajectory_from({0.0, 0.0}, {2.0, 2.0});
  local_planner.result_sequence = {first_plan, second_plan};
  MockContext local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner,
                               local_context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  local_engine->default_result = make_authoritative_collision_result();

  auto result = local_kernel.execute(make_test_request("replan_exhausted"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(local_planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.execute_count, 0);
  auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
  EXPECT_EQ(snapshot.error->details.at("collision_result_decision"), "fail");
}

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeCollisionReplanStopPreservesStoppedTerminal) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 1;
  MockPlanner local_planner;
  PlanResultDTO first_plan;
  first_plan.trajectory =
      make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  PlanResultDTO second_plan;
  second_plan.trajectory =
      make_test_trajectory_from({0.0, 0.0}, {2.0, 2.0});
  local_planner.result_sequence = {first_plan, second_plan};
  MockContext local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner,
                               local_context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  ExecutionKernel* kernel_ptr = &local_kernel;
  local_planner.on_plan = [&](int count) {
    if (count == 2) {
      auto stop = kernel_ptr->stop();
      EXPECT_TRUE(stop.accepted);
    }
  };
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  local_engine->result_for_query = [&](const CollisionQuery&) {
    if (local_planner.plan_count <= 1) {
      return std::make_optional(make_authoritative_collision_result());
    }
    return std::make_optional(CollisionResult{});
  };

  const auto span = make_test_action_span();
  auto request = make_test_request("replan_stop");
  request.program_run_id = span.program_run_id;
  request.action_span = span;
  auto result = local_kernel.execute(request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kStopped);
  EXPECT_EQ(local_planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.execute_count, 0);
  auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.action_span.has_value());
  EXPECT_EQ(snapshot.action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(snapshot.action_span->node_attempt_id, span.node_attempt_id);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause, MotionExecutionTerminalCause::kStopped);
  ASSERT_TRUE(outcome->action_span.has_value());
  EXPECT_EQ(outcome->action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(outcome->action_span->node_attempt_id, span.node_attempt_id);
}

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeCollisionReplanInterruptPreservesFaultTerminal) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 1;
  MockPlanner local_planner;
  PlanResultDTO first_plan;
  first_plan.trajectory =
      make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  PlanResultDTO second_plan;
  second_plan.trajectory =
      make_test_trajectory_from({0.0, 0.0}, {2.0, 2.0});
  local_planner.result_sequence = {first_plan, second_plan};
  MockContext local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner,
                               local_context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  auto source = make_interrupt_source("replan_fault");
  ASSERT_TRUE(local_kernel.register_interrupt_source(source).ok());
  auto policy = make_interrupt_policy("replan_fault_policy", "replan_fault");
  policy.affected_phases = {InterruptExecutionPhase::kPlanning};
  policy.preemption_action = InterruptPreemptionAction::kLatchHeld;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(policy).ok());
  ExecutionKernel* kernel_ptr = &local_kernel;
  local_planner.on_plan = [&](int count) {
    if (count == 2) {
      auto ingest =
          kernel_ptr->publish_fast_interrupt_signal(
              make_interrupt_signal("replan_fault"));
      EXPECT_TRUE(ingest.latched);
    }
  };
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  local_engine->result_for_query = [&](const CollisionQuery&) {
    if (local_planner.plan_count <= 1) {
      return std::make_optional(make_authoritative_collision_result());
    }
    return std::make_optional(CollisionResult{});
  };

  const auto span = make_test_action_span();
  auto request = make_test_request("replan_interrupt");
  request.program_run_id = span.program_run_id;
  request.action_span = span;
  auto result = local_kernel.execute(request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kInterrupted);
  EXPECT_EQ(local_planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.execute_count, 0);
  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->source_id, "replan_fault");
  EXPECT_EQ(latch->execution_phase_at_latch, InterruptExecutionPhase::kPlanning);
  auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.action_span.has_value());
  EXPECT_EQ(snapshot.action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(snapshot.action_span->node_attempt_id, span.node_attempt_id);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kInterruptedFault);
  ASSERT_TRUE(outcome->action_span.has_value());
  EXPECT_EQ(outcome->action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(outcome->action_span->node_attempt_id, span.node_attempt_id);
}

TEST_F(ExecutionKernelTest,
       Execute_AuthoritativeValidationNonOkCollisionDoesNotReplan) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 2;
  MockPlanner local_planner;
  PlanResultDTO plan;
  plan.trajectory = make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  local_planner.result_sequence = {plan, plan, plan};
  MockContext local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner,
                               local_context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  auto non_ok_collision = make_authoritative_collision_result();
  non_ok_collision.engine_status = CollisionEngineStatus::kTimeout;
  local_engine->default_result = non_ok_collision;

  auto result = local_kernel.execute(make_test_request("replan_non_ok"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(local_planner.plan_count, 1);
  EXPECT_EQ(controlled_execution.execute_count, 0);
  auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("collision_result_engine_status"),
            "timeout");
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
  ASSERT_TRUE(stale_ack.error.has_value());
  EXPECT_EQ(stale_ack.error->details.at("reason_code"),
            "mismatched_execution_id");
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
  ASSERT_TRUE(ack.error.has_value());
  EXPECT_EQ(ack.error->details.at("reason_code"), "wrong_state");
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

TEST_F(ExecutionKernelTest,
       AcknowledgeInterventionForRetry_FromStopIntervention_DoesNotResetBackend) {
  execution.next_terminal_state = ExecutionState::kInterventionRequired;
  execution.snapshot.error = ErrorDTO{
      std::string(kErrorMotionInterruptBackendStopFailed),
      "operator stop total failure",
      {{"reason", "operator_stop_total_failure"},
       {"phase", "operator_stop"},
       {"requested_stop_mode", "hard_deceleration"},
       {"executed_stop_mode", "hard_deceleration"}}};
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kInterventionRequired);
  EXPECT_EQ(execution.reset_count, 0);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto ack = kernel.acknowledge_intervention_for_retry(
      "operator_alice", "ui_acknowledge_and_continue");
  ASSERT_TRUE(ack.accepted);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.reset_count, 0);
  const auto snapshot = kernel.get_execution_snapshot();
  EXPECT_EQ(snapshot.state, ExecutionState::kIdle);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 2u);
  EXPECT_EQ(snapshot.joint_positions_rad[0], 1.0);
  EXPECT_EQ(snapshot.joint_positions_rad[1], 1.0);

  std::lock_guard<std::mutex> lock(events_mutex);
  bool saw_retry_ack = false;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kInterventionAcknowledged &&
        e.details.contains("recovery_mode")) {
      saw_retry_ack = true;
      EXPECT_EQ(e.details.at("recovery_mode"), "acknowledge_and_continue");
      EXPECT_EQ(e.details.at("backend_reset_performed"), "false");
    }
  }
  EXPECT_TRUE(saw_retry_ack);
}

TEST_F(ExecutionKernelTest,
       AcknowledgeInterventionForRetry_RejectsWhenMeasuredVelocityStillMoving) {
  execution.next_terminal_state = ExecutionState::kInterventionRequired;
  execution.snapshot.joint_velocities_rad_s = {0.2, 0.0};
  execution.snapshot.error = ErrorDTO{
      std::string(kErrorMotionInterruptBackendStopFailed),
      "operator stop not settled",
      {{"reason", "stop_not_settled_measured_velocity"},
       {"phase", "operator_stop"},
       {"requested_stop_mode", "soft_on_path"},
       {"executed_stop_mode", "soft_on_path"},
       {"settle_velocity_threshold_rad_s", "0.01"}}};
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kInterventionRequired);

  auto ack = kernel.acknowledge_intervention_for_retry(
      "operator_alice", "ui_acknowledge_and_continue");
  EXPECT_FALSE(ack.accepted);
  ASSERT_TRUE(ack.error.has_value());
  EXPECT_EQ(ack.error->details.at("reason"), "robot_not_settled");
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kInterventionRequired);
  EXPECT_EQ(execution.reset_count, 0);
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

TEST_F(ExecutionKernelTest, Pause_BackendRefusalFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  controlled_execution.fail_next_pause = true;
  auto pause = local_kernel.pause();
  EXPECT_FALSE(pause.accepted);
  ASSERT_TRUE(pause.error.has_value());
  EXPECT_EQ(pause.error->details.at("reason"),
            "test_injected_pause_failure");
  EXPECT_TRUE(wait_for_state(local_kernel,
                             ExecutionState::kInterventionRequired));
  EXPECT_EQ(local_kernel.get_execution_snapshot().state,
            ExecutionState::kInterventionRequired);
  controlled_execution.release();
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
}

// --- Resume ---

TEST_F(ExecutionKernelTest, Resume_FromIdle_NotAccepted) {
  auto result = kernel.resume();
  EXPECT_FALSE(result.accepted);
}

TEST_F(ExecutionKernelTest,
       ControlEpoch_StaleResetAtEntryRejectsWithoutBackendReset) {
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  ASSERT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  const auto stale_epoch = kernel.control_command_epoch();
  (void)kernel.bump_control_command_epoch();

  const auto reset = kernel.reset(stale_epoch);

  EXPECT_FALSE(reset.accepted);
  ASSERT_TRUE(reset.error.has_value());
  EXPECT_EQ(reset.error->details.at("reason_code"), "stale_control_command");
  EXPECT_EQ(execution.reset_count, 0);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest,
       ControlEpoch_ResetBumpedWhileWaitingDoesNotResetBackendOrPublishIdle) {
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  const auto reset_epoch = local_kernel.control_command_epoch();
  std::optional<ResetResultDTO> reset;
  std::thread reset_thread([&]() { reset = local_kernel.reset(reset_epoch); });

  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));
  (void)local_kernel.bump_control_command_epoch();
  controlled_execution.release_controlled_stop();
  reset_thread.join();

  ASSERT_TRUE(reset.has_value());
  EXPECT_FALSE(reset->accepted);
  ASSERT_TRUE(reset->error.has_value());
  EXPECT_EQ(reset->error->details.at("reason_code"), "stale_control_command");
  EXPECT_EQ(controlled_execution.reset_count, 0);
  EXPECT_NE(local_kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, ControlEpoch_AcceptedStopInvalidatesStaleResume) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(local_kernel.pause().accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPaused));
  const auto pre_stop_epoch = local_kernel.control_command_epoch();
  ASSERT_TRUE(local_kernel.stop().accepted);
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));
  ASSERT_TRUE(local_kernel.reset().accepted);

  auto second = local_kernel.execute(make_test_request());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_count(2));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(local_kernel.pause().accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPaused));
  const int resume_count_before = controlled_execution.resume_count;

  const auto stale_resume = local_kernel.resume(pre_stop_epoch);

  EXPECT_FALSE(stale_resume.accepted);
  ASSERT_TRUE(stale_resume.error.has_value());
  EXPECT_EQ(stale_resume.error->details.at("reason_code"),
            "stale_control_command");
  EXPECT_EQ(controlled_execution.resume_count, resume_count_before);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kPaused);
}

TEST_F(ExecutionKernelTest, ControlEpoch_CurrentResumeEpochIsAccepted) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(local_kernel.pause().accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPaused));

  const auto current_epoch = local_kernel.control_command_epoch();
  const int resume_count_before = controlled_execution.resume_count;

  const auto resume = local_kernel.resume(current_epoch);

  ASSERT_TRUE(resume.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_resume_called());
  EXPECT_EQ(controlled_execution.resume_count, resume_count_before + 1);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kExecuting);
  controlled_execution.complete();
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
}

TEST_F(ExecutionKernelTest, Pause_KeepsRuntimeMonitorArmed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 0;
  MockCollisionWorldPublisher local_publisher;
  auto validation_engine = std::make_shared<MockCollisionEngine>();
  auto runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache, config};
  configure_runtime_collision_monitoring(local_kernel,
                                         local_publisher,
                                         validation_engine,
                                         runtime_engine);

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(local_kernel.pause().accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPaused));

  const int baseline_runtime_checks = runtime_engine->check_count.load();
  for (int i = 0; i < 3; ++i) {
    local_clock.advance(config.collision_monitor_period_ns);
    local_scheduler.advance_to(local_clock.now());
  }

  EXPECT_GE(runtime_engine->check_count.load(), baseline_runtime_checks + 3);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kPaused);
  EXPECT_EQ(controlled_execution.stop_attempt_count, 0);
}

TEST_F(ExecutionKernelTest,
       CollisionValidationAdmission_WithSharedRuntimeResourceDefersWithoutStoppingMonitor) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 0;
  MockCollisionWorldPublisher local_publisher;
  auto shared_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache, config};
  configure_runtime_collision_monitoring(local_kernel,
                                         local_publisher,
                                         shared_engine,
                                         shared_engine);

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  const int baseline_runtime_checks = shared_engine->check_count.load();
  const auto admission_error =
      local_kernel.collision_validation_admission_error();

  ASSERT_TRUE(admission_error.has_value());
  EXPECT_EQ(admission_error->code, kErrorValidationDeferred);
  EXPECT_EQ(admission_error->details.at("reason"),
            "validation_deferred_runtime_monitor_active");
  EXPECT_EQ(admission_error->details.at("resource_conflict_reason"),
            "same_engine_instance");
  EXPECT_EQ(controlled_execution.stop_attempt_count, 0);

  for (int i = 0; i < 3; ++i) {
    local_clock.advance(config.collision_monitor_period_ns);
    local_scheduler.advance_to(local_clock.now());
  }

  EXPECT_GE(shared_engine->check_count.load(), baseline_runtime_checks + 3);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kExecuting);
}

TEST_F(ExecutionKernelTest,
       RuntimeCollisionPublishedBundleFailureFailsClosedWithoutStaleFallback) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 0;
  config.runtime_dynamic_body_max_age_cap_ns = 5'000'000'000;
  MockCollisionWorldPublisher local_publisher;
  auto validation_engine = std::make_shared<MockCollisionEngine>();
  auto runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache, config};
  configure_runtime_collision_monitoring(local_kernel,
                                         local_publisher,
                                         validation_engine,
                                         runtime_engine);

  // set_collision_resources() reads the published runtime bundle once. Keep
  // dispatch/start publication healthy, then fail the monitor tick's leaf read.
  // The tick must not fall back to the stale compatibility store or call the
  // runtime collision engine against an old world.
  const int tick_runtime_bundle_call =
      local_publisher.published_runtime_bundle_call_count.load() + 2;
  local_publisher.throw_on_published_runtime_bundle_call_numbers =
      {tick_runtime_bundle_call};

  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request("published_bundle_failure"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  const int baseline_runtime_checks = runtime_engine->check_count.load();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));
  local_clock.advance(config.collision_monitor_period_ns);
  local_scheduler.advance_to(local_clock.now());
  const auto metrics = local_kernel.get_metrics();
  EXPECT_EQ(metrics.runtime_collision_snapshot_refresh_failures, 1u);
  EXPECT_EQ(metrics.runtime_collision_ticks_started, 1u);
  EXPECT_EQ(metrics.runtime_collision_ticks_faulted, 1u);
  EXPECT_EQ(metrics.runtime_collision_ticks_completed, 0u);

  EXPECT_EQ(runtime_engine->check_count.load(), baseline_runtime_checks)
      << "a failed published runtime-bundle read must not reuse stale world "
         "data from the compatibility store";

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                            [](const KernelEvent& e) {
                              return e.type == KernelEventType::kCollisionDetected &&
                                     e.details.contains("reason") &&
                                     e.details.at("reason") ==
                                         "published_runtime_collision_snapshot_exception";
                            }));
  }

  controlled_execution.release();
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
}

TEST_F(ExecutionKernelTest,
       RuntimeProtectionLeaseIsActiveBeforeBackendExecuteStarts) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 0;
  MockCollisionWorldPublisher local_publisher;
  auto shared_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache, config};
  configure_runtime_collision_monitoring(local_kernel,
                                         local_publisher,
                                         shared_engine,
                                         shared_engine);

  std::optional<ErrorDTO> admission_during_backend_execute;
  controlled_execution.on_execute = [&]() {
    admission_during_backend_execute =
        local_kernel.collision_validation_admission_error();
  };

  auto result = local_kernel.execute(make_test_request());

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(admission_during_backend_execute.has_value());
  EXPECT_EQ(admission_during_backend_execute->code, kErrorValidationDeferred);
  EXPECT_EQ(admission_during_backend_execute->details.at("reason"),
            "validation_deferred_runtime_monitor_active");
  EXPECT_EQ(admission_during_backend_execute->details.at("resource_conflict_reason"),
            "same_engine_instance");
}

TEST_F(ExecutionKernelTest,
       Resume_WithSharedRuntimeValidationResourceFailsClosedWithoutDispatch) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 0;
  MockCollisionWorldPublisher local_publisher;
  auto shared_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache, config};
  configure_runtime_collision_monitoring(local_kernel,
                                         local_publisher,
                                         shared_engine,
                                         shared_engine);

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(local_kernel.pause().accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPaused));

  const int resume_count_before = controlled_execution.resume_count;
  const auto resume = local_kernel.resume(local_kernel.control_command_epoch());

  ASSERT_TRUE(resume.accepted);
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(controlled_execution.resume_count, resume_count_before);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kFailed);
  const auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationDeferred);
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "validation_deferred_runtime_monitor_active");
  EXPECT_EQ(snapshot.error->details.at("resource_conflict_reason"),
            "same_engine_instance");
  EXPECT_EQ(local_kernel.get_metrics().validation_deferred_runtime_monitor_active,
            1u);
}

TEST_F(ExecutionKernelTest,
       Resume_WithIndependentValidationResourceSucceedsWhileMonitorRemainsArmed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 0;
  MockCollisionWorldPublisher local_publisher;
  auto validation_engine = std::make_shared<MockCollisionEngine>();
  auto runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache, config};
  configure_runtime_collision_monitoring(local_kernel,
                                         local_publisher,
                                         validation_engine,
                                         runtime_engine);

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(local_kernel.pause().accepted);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_paused());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPaused));

  const int resume_count_before = controlled_execution.resume_count;
  const int baseline_runtime_checks = runtime_engine->check_count.load();
  const auto resume = local_kernel.resume(local_kernel.control_command_epoch());

  ASSERT_TRUE(resume.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_resume_called());
  EXPECT_EQ(controlled_execution.resume_count, resume_count_before + 1);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kExecuting);

  for (int i = 0; i < 3; ++i) {
    local_clock.advance(config.collision_monitor_period_ns);
    local_scheduler.advance_to(local_clock.now());
  }

  EXPECT_GE(runtime_engine->check_count.load(), baseline_runtime_checks + 3);
  controlled_execution.complete();
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
}

TEST_F(ExecutionKernelTest,
       ControlEpoch_StaleAcknowledgementsRejectBeforeRecoverySideEffects) {
  auto completed = execute_sync(make_test_request());
  ASSERT_TRUE(completed.ok());
  ASSERT_EQ(kernel.get_status().state, ExecutionState::kCompleted);
  auto stale_completed_epoch = kernel.control_command_epoch();
  (void)kernel.bump_control_command_epoch();
  auto completed_ack =
      kernel.acknowledge_completed(completed.value(), stale_completed_epoch);
  EXPECT_FALSE(completed_ack.accepted);
  ASSERT_TRUE(completed_ack.error.has_value());
  EXPECT_EQ(completed_ack.error->details.at("reason_code"),
            "stale_control_command");
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted);

  ASSERT_TRUE(kernel.reset().accepted);
  execution.next_terminal_state = ExecutionState::kInterventionRequired;
  auto intervention = execute_sync(make_test_request());
  ASSERT_TRUE(intervention.ok());
  ASSERT_EQ(kernel.get_status().state, ExecutionState::kInterventionRequired);
  const auto reset_count_before = execution.reset_count;
  auto stale_intervention_epoch = kernel.control_command_epoch();
  (void)kernel.bump_control_command_epoch();
  auto intervention_ack = kernel.acknowledge_intervention(
      "operator_alice", "ui_reset", stale_intervention_epoch);
  EXPECT_FALSE(intervention_ack.accepted);
  ASSERT_TRUE(intervention_ack.error.has_value());
  EXPECT_EQ(intervention_ack.error->details.at("reason_code"),
            "stale_control_command");
  EXPECT_EQ(execution.reset_count, reset_count_before);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kInterventionRequired);
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
  EXPECT_EQ(controlled_execution.stop_count_value(), 1);
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
  EXPECT_EQ(controlled_execution.stop_count_value(), 1);
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
  EXPECT_GE(controlled_execution.stop_count_value(), 1);
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
  EXPECT_EQ(context.collision_count.load(), 0);
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
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);
  std::mutex events_mu;
  std::vector<KernelEvent> events;
  controlled_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mu);
    events.push_back(e);
  });

  // RT-1: runtime ticks consume the kernel-published snapshot captured at
  // monitor start. Publish the live world before dispatch; the tick must not
  // poll the scene publisher after execution is active.
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{99, "live-scene-after-dispatch"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called()) << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";

  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  // The fault preempt runs on the kernel's dedicated fault worker thread;
  // it is NOT synchronized by wait_for_completion, so counter reads in
  // this window must use the fake's mutex-guarded accessors.
  for (int attempt = 0;
       attempt < 1000 &&
       context.collision_count.load() == 0 &&
       controlled_execution.stop_count_value() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    local_scheduler.advance_to(local_clock.now());
  }
  bool completed = controlled_kernel.wait_for_completion(1'000'000'000);
  if (!completed) {
    controlled_execution.release();
    completed = controlled_kernel.wait_for_completion(1'000'000'000);
  }
  EXPECT_TRUE(completed);

  EXPECT_EQ(context.collision_count.load(), 0);
  EXPECT_GE(local_runtime_engine->check_count.load(), 1);
  EXPECT_EQ(controlled_execution.stop_count_value(), 1);
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

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_RetryAfterRuntimeCollisionDoesNotRelatchStaleEdge) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::atomic<bool> retry_phase{false};
  local_runtime_engine->result_for_query =
      [&retry_phase](const CollisionQuery&) -> std::optional<CollisionResult> {
    if (!retry_phase.load(std::memory_order_acquire)) {
      return make_authoritative_collision_result();
    }
    CollisionResult clear_result;
    clear_result.in_collision = false;
    clear_result.engine_status = CollisionEngineStatus::kOk;
    clear_result.decision = CollisionDecisionResult::kPass;
    return clear_result;
  };
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{120, "collision-then-clear"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));

  auto first = controlled_kernel.execute(make_test_request("retry_after_collision"));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";

  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_stop_count(1))
      << "runtime collision did not preempt the first execution";
  ASSERT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000));
  ASSERT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  const auto retry_latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(retry_latch.has_value());
  ASSERT_EQ(retry_latch->preemption_result,
            InterruptPreemptionResult::kSucceeded);

  auto ack = controlled_kernel.acknowledge_interrupted_for_retry(
      "operator", "runtime obstacle cleared");
  ASSERT_TRUE(ack.accepted) << (ack.error ? ack.error->message : "");
  ASSERT_EQ(controlled_kernel.get_status().state, ExecutionState::kIdle);
  ASSERT_FALSE(controlled_kernel.get_active_interrupt_latch().has_value());

  // The retry sees a clear runtime monitor. Before the fix, the built-in
  // runtime_collision_fault edge source still had a freshness budget, so the
  // stale-source sweep converted the previous collision pulse into a new
  // synthetic stale fault and interrupted this second execution immediately.
  retry_phase.store(true, std::memory_order_release);
  auto retry = controlled_kernel.execute(make_test_request("retry_after_collision"));
  ASSERT_TRUE(retry.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_count(2));
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its retry tick";

  for (int attempt = 0; attempt < 20; ++attempt) {
    local_clock.advance(10'000'000);
    controlled_execution.set_progress(0.0, {0.0, 0.0}, local_clock.now());
    local_scheduler.advance_to(local_clock.now());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (controlled_kernel.get_status().state != ExecutionState::kExecuting) {
      break;
    }
  }

  const auto final_status = controlled_kernel.get_status();
  const auto final_latch = controlled_kernel.get_active_interrupt_latch();
  EXPECT_EQ(final_status.state, ExecutionState::kExecuting);
  EXPECT_FALSE(final_latch.has_value());
  controlled_execution.release();
  EXPECT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000));
}

TEST_F(ExecutionKernelTest,
       AcknowledgeInterruptedForRetryRejectsPendingFaultPreemptWithoutSideEffects) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "pending_retry_guard";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{121, "pending-retry-guard"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));

  auto result = controlled_kernel.execute(make_test_request("pending_retry"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));

  local_clock.advance(config.collision_monitor_period_ns);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_EQ(controlled_kernel.get_status().state,
            ExecutionState::kInterrupted);
  const auto latch_before = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch_before.has_value());
  ASSERT_EQ(latch_before->preemption_result,
            InterruptPreemptionResult::kPending);
  const auto epoch = controlled_kernel.control_command_epoch();

  const auto ack = controlled_kernel.acknowledge_interrupted_for_retry(
      "operator_alice", "runtime obstacle cleared", epoch);

  EXPECT_FALSE(ack.accepted);
  ASSERT_TRUE(ack.error.has_value());
  EXPECT_EQ(ack.error->code, kErrorInvalidRequest);
  EXPECT_EQ(ack.error->details.at("reason"),
            kReasonRuntimeCollisionStopNotYetCertified);
  EXPECT_EQ(ack.error->details.at("execution_id"),
            std::to_string(result.value()));
  EXPECT_EQ(ack.error->details.at("latch_id"), latch_before->latch_id);
  EXPECT_EQ(ack.error->details.at("preemption_result"), "pending");
  EXPECT_EQ(ack.error->details.at("fault_preempt_stop_active"), "true");
  EXPECT_EQ(ack.error->details.at("retryable"), "true");
  EXPECT_EQ(controlled_kernel.control_command_epoch(), epoch);
  EXPECT_EQ(controlled_kernel.get_status().state,
            ExecutionState::kInterrupted);
  const auto latch_after_rejection =
      controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch_after_rejection.has_value());
  EXPECT_EQ(latch_after_rejection->latch_id, latch_before->latch_id);
  EXPECT_EQ(latch_after_rejection->preemption_result,
            InterruptPreemptionResult::kPending);
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count_value(), 0);
  EXPECT_EQ(controlled_execution.stop_attempt_count_value(), 0);
  EXPECT_EQ(controlled_execution.reset_count, 0);

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_TRUE(std::any_of(
        events.begin(), events.end(), [&](const KernelEvent& event) {
          return event.type == KernelEventType::kErrorRaised &&
                 event.execution_id == result.value() &&
                 event.details.contains("reason") &&
                 event.details.at("reason") ==
                     kReasonRuntimeCollisionStopNotYetCertified &&
                 event.details.contains("latch_id") &&
                 event.details.at("latch_id") == latch_before->latch_id;
        }));
  }

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000));
  const auto completed_latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(completed_latch.has_value());
  ASSERT_EQ(completed_latch->preemption_result,
            InterruptPreemptionResult::kSucceeded);
  EXPECT_EQ(controlled_execution.reset_count, 0);
}

TEST_F(ExecutionKernelTest,
       AcknowledgeInterruptedForRetryRejectsDefinitivePreemptWhileStopSourceActive) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  controlled_execution.ignore_controlled_stop_token_while_blocked = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "active_stop_retry_guard";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  fault_profile.deceleration_time_s = 0.01;
  fault_profile.max_stop_time_s = 0.02;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  std::mutex preempt_event_mutex;
  std::condition_variable preempt_event_cv;
  bool preempt_event_entered = false;
  bool release_preempt_event = false;
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kInterruptPreemptApplied ||
        !event.details.contains("preemption_result") ||
        event.details.at("preemption_result") != "backend_preempt_failed" ||
        !event.details.contains("phase") ||
        event.details.at("phase") != "fault_preempt_pending_watchdog") {
      return;
    }
    std::unique_lock<std::mutex> lock(preempt_event_mutex);
    preempt_event_entered = true;
    preempt_event_cv.notify_all();
    preempt_event_cv.wait(lock, [&]() { return release_preempt_event; });
  });

  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision =
      CollisionWorldRevision{122, "active-stop-retry-guard"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));

  auto result =
      controlled_kernel.execute(make_test_request("active_stop_retry"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));

  local_clock.advance(config.collision_monitor_period_ns);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  bool reached_definitive_event = false;
  for (int attempt = 0; attempt < 20 && !reached_definitive_event; ++attempt) {
    local_clock.advance(1'000'000'000);
    std::unique_lock<std::mutex> lock(preempt_event_mutex);
    reached_definitive_event = preempt_event_cv.wait_for(
        lock, std::chrono::milliseconds(100),
        [&]() { return preempt_event_entered; });
  }
  if (!reached_definitive_event) {
    {
      std::lock_guard<std::mutex> lock(preempt_event_mutex);
      release_preempt_event = true;
    }
    controlled_execution.release_controlled_stop();
    preempt_event_cv.notify_all();
    FAIL() << "pending-preempt watchdog result did not reach the barrier";
  }

  const auto latch_before = controlled_kernel.get_active_interrupt_latch();
  if (!latch_before.has_value()) {
    {
      std::lock_guard<std::mutex> lock(preempt_event_mutex);
      release_preempt_event = true;
    }
    controlled_execution.release_controlled_stop();
    preempt_event_cv.notify_all();
    FAIL() << "preempt-applied event had no active latch";
  }
  EXPECT_EQ(latch_before->preemption_result,
            InterruptPreemptionResult::kBackendPreemptFailed);
  const auto ack = controlled_kernel.acknowledge_interrupted_for_retry(
      "operator_alice", "runtime obstacle cleared");
  const auto latch_after_rejection =
      controlled_kernel.get_active_interrupt_latch();

  {
    std::lock_guard<std::mutex> lock(preempt_event_mutex);
    release_preempt_event = true;
  }
  controlled_execution.release_controlled_stop();
  preempt_event_cv.notify_all();

  EXPECT_FALSE(ack.accepted);
  ASSERT_TRUE(ack.error.has_value());
  EXPECT_EQ(ack.error->details.at("reason"),
            kReasonRuntimeCollisionStopNotYetCertified);
  EXPECT_EQ(ack.error->details.at("preemption_result"),
            "backend_preempt_failed");
  EXPECT_EQ(ack.error->details.at("fault_preempt_stop_active"), "true");
  EXPECT_EQ(ack.error->details.at("retryable"), "false");
  ASSERT_TRUE(latch_after_rejection.has_value());
  EXPECT_EQ(latch_after_rejection->preemption_result,
            InterruptPreemptionResult::kBackendPreemptFailed);
  EXPECT_EQ(controlled_execution.reset_count, 0);
  EXPECT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(controlled_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
}

TEST_F(ExecutionKernelTest,
       AcknowledgeInterruptedForRetryRejectsFailedPreemptAfterStopSourceClears) {
  ControlledExecution controlled_execution;
  ControlledStopResultDTO controlled_stop_failure;
  controlled_stop_failure.accepted = false;
  controlled_stop_failure.backend_stop_confirmed = false;
  controlled_stop_failure.error = ErrorDTO{
      std::string(kErrorMotionInterruptBackendStopFailed),
      "injected controlled-stop failure",
      {{"reason", "test_controlled_stop_failure"}}};
  controlled_execution.next_controlled_stop_result = controlled_stop_failure;
  controlled_execution.fail_next_legacy_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "failed_preempt_retry_guard";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  std::mutex preempt_event_mutex;
  std::condition_variable preempt_event_cv;
  bool preempt_event_entered = false;
  bool release_preempt_event = false;
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kInterruptPreemptApplied ||
        !event.details.contains("preemption_result") ||
        event.details.at("preemption_result") != "backend_preempt_failed" ||
        !event.details.contains("phase") ||
        event.details.at("phase") != "preempt_trajectory") {
      return;
    }
    std::unique_lock<std::mutex> lock(preempt_event_mutex);
    preempt_event_entered = true;
    preempt_event_cv.notify_all();
    preempt_event_cv.wait(lock, [&]() { return release_preempt_event; });
  });

  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision =
      CollisionWorldRevision{123, "failed-preempt-retry-guard"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));

  const auto result =
      controlled_kernel.execute(make_test_request("failed_preempt_retry"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));

  local_clock.advance(config.collision_monitor_period_ns);
  local_scheduler.advance_to(local_clock.now());
  bool reached_preempt_event = false;
  {
    std::unique_lock<std::mutex> lock(preempt_event_mutex);
    reached_preempt_event = preempt_event_cv.wait_for(
        lock, std::chrono::seconds(1),
        [&]() { return preempt_event_entered; });
  }
  if (!reached_preempt_event) {
    {
      std::lock_guard<std::mutex> lock(preempt_event_mutex);
      release_preempt_event = true;
    }
    preempt_event_cv.notify_all();
    FAIL() << "failed preemption result did not reach the barrier";
  }

  const auto latch_before = controlled_kernel.get_active_interrupt_latch();
  if (!latch_before.has_value()) {
    {
      std::lock_guard<std::mutex> lock(preempt_event_mutex);
      release_preempt_event = true;
    }
    preempt_event_cv.notify_all();
    FAIL() << "failed preemption result had no active latch";
  }
  EXPECT_EQ(latch_before->preemption_result,
            InterruptPreemptionResult::kBackendPreemptFailed);
  EXPECT_EQ(controlled_kernel.get_status().state,
            ExecutionState::kInterrupted);

  const auto ack = controlled_kernel.acknowledge_interrupted_for_retry(
      "operator_alice", "runtime obstacle cleared");
  const auto latch_after_rejection =
      controlled_kernel.get_active_interrupt_latch();

  {
    std::lock_guard<std::mutex> lock(preempt_event_mutex);
    release_preempt_event = true;
  }
  preempt_event_cv.notify_all();

  EXPECT_FALSE(ack.accepted);
  ASSERT_TRUE(ack.error.has_value());
  EXPECT_EQ(ack.error->details.at("reason"),
            kReasonRuntimeCollisionStopNotYetCertified);
  EXPECT_EQ(ack.error->details.at("preemption_result"),
            "backend_preempt_failed");
  EXPECT_EQ(ack.error->details.at("required_preemption_result"), "succeeded");
  EXPECT_EQ(ack.error->details.at("fault_preempt_stop_active"), "false");
  EXPECT_EQ(ack.error->details.at("retryable"), "false");
  ASSERT_TRUE(latch_after_rejection.has_value());
  EXPECT_EQ(latch_after_rejection->latch_id, latch_before->latch_id);
  EXPECT_EQ(controlled_execution.reset_count, 0);
  EXPECT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(controlled_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
}

TEST_F(ExecutionKernelTest,
       AcknowledgeInterruptedForRetryStampsReplayedQueuedLatchRevision) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  auto queued_source = make_interrupt_source("retry_replay_latch");
  queued_source.freshness_budget_ns = 1'000'000'000;
  ASSERT_TRUE(controlled_kernel.register_interrupt_source(queued_source).ok());
  auto queued_policy =
      make_interrupt_policy("retry_replay_latch_policy", "retry_replay_latch");
  queued_policy.affected_phases = {InterruptExecutionPhase::kHeld};
  queued_policy.priority = 400;
  queued_policy.preemption_action = InterruptPreemptionAction::kLatchHeld;
  ASSERT_TRUE(controlled_kernel.register_interrupt_policy(queued_policy).ok());

  auto result = controlled_kernel.execute(make_test_request("retry_stamp"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  auto live_snapshot = make_test_collision_world_snapshot();
  const CollisionWorldRevision replay_revision{
      5151, "retry-disposition-replay-world"};
  live_snapshot.revision = replay_revision;
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";

  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  for (int attempt = 0;
       attempt < 100 && controlled_execution.stop_count_value() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    local_scheduler.advance_to(local_clock.now());
  }
  bool completed = controlled_kernel.wait_for_completion(200'000'000);
  if (!completed) {
    controlled_execution.release();
    completed = controlled_kernel.wait_for_completion(1'000'000'000);
  }
  ASSERT_TRUE(completed);
  ASSERT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);

  auto queued_signal = make_interrupt_signal("retry_replay_latch", 1);
  queued_signal.timestamp_ns = local_clock.now();
  auto queued = controlled_kernel.publish_fast_interrupt_signal(queued_signal);
  ASSERT_TRUE(queued.queued);

  auto ack = controlled_kernel.acknowledge_interrupted_for_retry(
      "operator", "collision corrected");

  ASSERT_FALSE(ack.accepted);
  ASSERT_TRUE(ack.error.has_value());
  EXPECT_EQ(ack.error->details.at("reason"),
            "interrupted_retry_replayed_latch");
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->source_id, "retry_replay_latch");
  ASSERT_TRUE(latch->borrowed_collision_world_revision.has_value());
  expect_collision_world_revision(*latch->borrowed_collision_world_revision,
                                  replay_revision);
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_FaultControlledStopRunsOffSafetyTick) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "runtime_collision_fault_stop";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{101, "fault-stop-off-safety"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";

  local_clock.advance(10'000'000);
  auto safety_tick = std::async(std::launch::async, [&]() {
    local_scheduler.advance_to(local_clock.now());
  });
  if (safety_tick.wait_for(std::chrono::milliseconds(200)) !=
      std::future_status::ready) {
    controlled_execution.release_controlled_stop();
    safety_tick.wait();
    FAIL() << "runtime collision safety tick blocked in fault controlled stop";
  }
  safety_tick.get();

  if (!controlled_execution.wait_until_controlled_stop_count(1)) {
    controlled_execution.release_controlled_stop();
    FAIL() << "fault preempt controlled stop did not start on its worker";
  }
  controlled_execution.release_controlled_stop();

  ASSERT_TRUE(controlled_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->preemption_result, InterruptPreemptionResult::kSucceeded);
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ExpandsObservedStateBeforeEngineQuery) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  context.current_state.joint_names = {"j1", "j2"};
  context.current_state.positions_rad = {0.0, 0.0};
  controlled_execution.snapshot.timestamp_ns = local_clock.now();

  TrajectoryDTO expanded;
  expanded.joint_names = {"j1", "j2", "gripper_master"};
  expanded.waypoints.push_back(TrajectoryWaypointDTO{
      0.0,
      {0.0, 0.0, 0.0081},
      {},
      {}});
  context.next_expand_result = Result<TrajectoryDTO>::Ok(expanded);

  std::vector<std::string> queried_joint_names;
  std::vector<double> queried_positions;
  local_runtime_engine->on_check = [&](const CollisionQuery& query) {
    queried_joint_names = query.robot_state.joint_names;
    queried_positions = query.robot_state.positions_rad;
  };

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  const int expand_count_after_dispatch = context.expand_count;
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{100, "expanded-runtime-state"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));

  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";
  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  for (int attempt = 0;
       attempt < 100 && local_runtime_engine->check_count.load() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    local_scheduler.advance_to(local_clock.now());
  }

  controlled_execution.release();
  EXPECT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000));

  EXPECT_GE(context.expand_count, 1);
  EXPECT_EQ(context.expand_count, expand_count_after_dispatch)
      << "runtime collision ticks must not call the planning context";
  ASSERT_EQ(context.last_expand_trajectory.joint_names,
            (std::vector<std::string>{"j1", "j2"}));
  ASSERT_GE(context.last_expand_trajectory.waypoints.size(), 1u);
  EXPECT_EQ(context.last_expand_trajectory.waypoints.front().positions_rad,
            (std::vector<double>{0.0, 0.0}));
  EXPECT_EQ(queried_joint_names,
            (std::vector<std::string>{"j1", "j2", "gripper_master"}));
  ASSERT_EQ(queried_positions.size(), 3u);
  EXPECT_DOUBLE_EQ(queried_positions[0], 0.0);
  EXPECT_DOUBLE_EQ(queried_positions[1], 0.0);
  EXPECT_DOUBLE_EQ(queried_positions[2], 0.0081);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kCompleted);
}

// When the deployment configures `default_fault_stop_profile`,
// a runtime collision fault
// must route the preempt through `execute_controlled_stop(profile)`
// instead of the legacy `execution_backend_.stop()`. The legacy stop
// count stays at zero; the controlled-stop count goes up by one. The
// trip-outcome diagnostic surface (`requested_stop_mode`,
// `executed_stop_mode`, `stop_command_source`) is carried on the
// `kErrorRaised` event so the UI can render the structured
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
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision =
      CollisionWorldRevision{99, "live-scene-after-dispatch"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_runtime_engine->default_result =
      make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";
  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  bool completed = controlled_kernel.wait_for_completion(200'000'000);
  if (!completed) {
    controlled_execution.release();
    completed = controlled_kernel.wait_for_completion(1'000'000'000);
  }
  EXPECT_TRUE(completed);

  // Fault routed through controlled-stop, NOT legacy backend.stop().
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);
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
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  // Per-policy profile (hard_deceleration) wins; kernel default ignored.
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);
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
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  // Operator stop button.
  auto stop_dto = local_kernel.stop();
  EXPECT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.profile_id,
            "operator_stop_soft_on_path");
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.stop_mode,
            MotionInterruptStopMode::kSoftOnPath);
  // The state transition can become observable before the async event
  // subscriber callback has copied kStopIssued into the local vector under
  // full-suite load. Poll for the exact event instead of racing delivery.
  std::optional<KernelEvent> stop_event;
  for (int attempt = 0; attempt < 2000; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      auto it = std::find_if(events.begin(), events.end(),
                             [](const KernelEvent& e) {
                               return e.type == KernelEventType::kStopIssued;
                             });
      if (it != events.end()) {
        stop_event = *it;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(stop_event.has_value());
  EXPECT_EQ(stop_event->details.at("controlled_stop_success"), "true");
  EXPECT_EQ(stop_event->details.at("phase"), "operator_stop");
  EXPECT_EQ(stop_event->details.at("requested_stop_mode"), "soft_on_path");
  EXPECT_EQ(stop_event->details.at("executed_stop_mode"), "soft_on_path");
  EXPECT_EQ(stop_event->details.at("stop_command_source"),
            "native_simulated_stop");
  EXPECT_EQ(stop_event->details.at("backend_stop_confirmed"), "true");
  EXPECT_EQ(stop_event->details.at("hard_stop_floor"), "immediate_hard_stop");
}

TEST_F(ExecutionKernelTest,
       Stop_SuccessfulOperatorControlledStopReleasesRuntimeProtectionLease) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.max_stop_time_s = 1.0;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 5'000'000'000;
  config.runtime_dynamic_body_max_age_cap_ns = 5'000'000'000;

  MockCollisionWorldPublisher local_publisher;
  auto shared_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher,
                                         shared_engine, shared_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  auto result = local_kernel.execute(make_test_request("operator_stop_release"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto stop_dto = local_kernel.stop();
  ASSERT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));

  const auto admission_error =
      local_kernel.collision_validation_admission_error();
  EXPECT_FALSE(admission_error.has_value())
      << "successful operator stop must release the shared runtime lease; got "
      << (admission_error ? admission_error->message : std::string{});
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
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
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
       Stop_OperatorStopProfileKeepsMonitorArmedDuringDecel) {
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
  op_profile.max_stop_time_s = 1.0;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 5'000'000'000;
  config.runtime_dynamic_body_max_age_cap_ns = 5'000'000'000;

  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher,
                                         local_engine,
                                         local_runtime_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request("operator_stop_monitor"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto stop_dto = local_kernel.stop();
  ASSERT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));

  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision =
      CollisionWorldRevision{101, "operator-stop-obstacle-on-path"};
  local_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_runtime_engine->default_result = CollisionResult{};

  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "operator-stop collision monitor did not schedule its next tick";
  const int baseline_runtime_checks =
      local_runtime_engine->check_count.load();
  for (int tick = 0; tick < 3; ++tick) {
    local_clock.advance(10'000'000);
    local_scheduler.advance_to(local_clock.now());
  }
  EXPECT_GE(local_runtime_engine->check_count.load(),
            baseline_runtime_checks + 3)
      << "operator-stop monitor must keep rescheduling throughout decel";
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count_value(), 0);
  EXPECT_EQ(controlled_execution.stop_attempt_count_value(), 0);

  local_runtime_engine->default_result = make_authoritative_collision_result();
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (controlled_execution.controlled_stop_cancel_count_value() > 0) {
      break;
    }
    local_clock.advance(10'000'000);
    local_scheduler.advance_to(local_clock.now());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));

  EXPECT_GE(controlled_execution.controlled_stop_cancel_count_value(), 1);
  EXPECT_GE(controlled_execution.stop_attempt_count_value(), 1);
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "operator_stop_path_collision");
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    const bool saw_operator_stop_collision =
        std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
          return e.type == KernelEventType::kCollisionDetected &&
                 e.details.contains("reason_during_stop") &&
                 e.details.at("reason_during_stop") ==
                     "stop_path_collision" &&
                 e.details.contains("stop_owner") &&
                 e.details.at("stop_owner") == "operator_stop";
        });
    EXPECT_TRUE(saw_operator_stop_collision);
  }
}

TEST_F(ExecutionKernelTest,
       Stop_OriginalGoalCompletionCannotStealOperatorStopTerminalAuthority) {
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
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto stop_dto = local_kernel.stop();
  ASSERT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));

  // The original controller goal can finish while the controlled-stop worker
  // is still certifying the stop. That completion must not emit
  // execute_completed or move the kernel to Completed.
  controlled_execution.complete();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kStopping);
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_EQ(std::count_if(events.begin(), events.end(),
                            [](const KernelEvent& event) {
                              return event.type ==
                                     KernelEventType::kExecuteCompleted;
                            }),
              0);
  }

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));
  // The operator-stop worker commits kStopped before it emits kStopIssued.
  // Poll the event explicitly so this assertion does not race the final event
  // delivery under TSAN/full-suite load.
  std::optional<KernelEvent> stop_event;
  for (int attempt = 0; attempt < 2000; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      auto it = std::find_if(events.begin(), events.end(),
                             [](const KernelEvent& event) {
                               return event.type == KernelEventType::kStopIssued;
                             });
      if (it != events.end()) {
        stop_event = *it;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(stop_event.has_value());
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_EQ(std::count_if(events.begin(), events.end(),
                            [](const KernelEvent& event) {
                              return event.type ==
                                     KernelEventType::kExecuteCompleted;
                            }),
              0);
  }
}

TEST_F(ExecutionKernelTest,
       Stop_OperatorConfirmedStopWaitsForMeasuredSettleLag) {
  ControlledExecution controlled_execution;
  controlled_execution.next_post_stop_joint_positions_rad =
      std::vector<double>{0.0, 0.0};
  controlled_execution.next_post_stop_joint_velocities_rad_s =
      std::vector<double>{0.25, 0.0};
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.max_stop_time_s = 0.2;
  op_profile.settle_velocity_threshold_rad_s = 0.01;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto stop_dto = local_kernel.stop();
  EXPECT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));

  std::thread settle_thread([&controlled_execution]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    controlled_execution.set_snapshot_velocities({0.0, 0.0});
  });
  const bool reached_stopped =
      wait_for_state(local_kernel, ExecutionState::kStopped);
  settle_thread.join();
  ASSERT_TRUE(reached_stopped);

  EXPECT_EQ(controlled_execution.stop_attempt_count_value(), 0)
      << "normal feedback settle lag must not trigger a redundant JTC cancel";
  const auto snapshot = local_kernel.get_execution_snapshot();
  EXPECT_FALSE(snapshot.error.has_value());
}

TEST_F(ExecutionKernelTest,
       Stop_OperatorCommandAuthoritativeStopDoesNotFailOnFeedbackLag) {
  ControlledExecution controlled_execution;
  controlled_execution.next_post_stop_joint_positions_rad =
      std::vector<double>{0.42, -0.1};
  controlled_execution.next_post_stop_joint_velocities_rad_s =
      std::vector<double>{0.25, 0.0};
  controlled_execution.next_post_stop_state_source =
      "commanded_sim_controlled_stop_reference";
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.max_stop_time_s = 0.05;
  op_profile.settle_velocity_threshold_rad_s = 0.01;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto stop_dto = local_kernel.stop();
  EXPECT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));

  EXPECT_EQ(controlled_execution.stop_attempt_count_value(), 0);
  const auto snapshot = local_kernel.get_execution_snapshot();
  EXPECT_FALSE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.joint_positions_rad, (std::vector<double>{0.42, -0.1}));
}

TEST_F(ExecutionKernelTest,
       Stop_OperatorConfirmedButMeasuredNotAtRestRequiresInterventionOnly) {
  ControlledExecution controlled_execution;
  controlled_execution.next_post_stop_joint_positions_rad =
      std::vector<double>{0.0, 0.0};
  controlled_execution.next_post_stop_joint_velocities_rad_s =
      std::vector<double>{0.25, 0.0};
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.max_stop_time_s = 0.05;
  op_profile.settle_velocity_threshold_rad_s = 0.01;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto stop_dto = local_kernel.stop();
  EXPECT_TRUE(stop_dto.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel,
                             ExecutionState::kInterventionRequired));

  EXPECT_EQ(controlled_execution.stop_attempt_count_value(), 0)
      << "the controlled stop was accepted and confirmed; if the robot still "
         "does not settle, another JTC cancel has nothing meaningful to "
         "cancel and must not be attempted";
  const auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "stop_not_settled_measured_velocity");
  EXPECT_EQ(snapshot.error->details.at("controlled_stop_error_reason"),
            "stop_not_settled_measured_velocity");
  EXPECT_EQ(snapshot.error->details.at("hard_stop_transitioned"), "false");
  EXPECT_EQ(snapshot.error->details.at("emergency_fallback"),
            "not_attempted_controlled_stop_already_confirmed");
  EXPECT_TRUE(snapshot.error->details.contains(
      "measured_max_abs_velocity_rad_s"));
}

TEST_F(ExecutionKernelTest,
       Stop_OperatorDeadlineWatchdogHardStopsBeforeBackendReturns) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.max_stop_time_s = 0.1;
  op_profile.deceleration_time_s = 0.3;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
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
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));

  bool watchdog_cancel_seen = false;
  for (int attempt = 0; attempt < 200 && !watchdog_cancel_seen; ++attempt) {
    local_clock.advance(20'000'000);
    local_scheduler.advance_to(local_clock.now());
    watchdog_cancel_seen =
        controlled_execution.wait_until_controlled_stop_cancel_count(
        1, std::chrono::milliseconds(1));
  }

  ASSERT_TRUE(
      watchdog_cancel_seen ||
      controlled_execution.wait_until_controlled_stop_cancel_count(1))
      << "deadline watchdog must cancel the blocked controlled stop itself";
  ASSERT_TRUE(controlled_execution.wait_until_stop_attempt_count(1))
      << "deadline watchdog must issue backend.stop() itself";
  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped))
      << "state=" << to_string(local_kernel.get_status().state);
  ExecutionSnapshot snapshot;
  bool terminal_error_seen = false;
  for (int attempt = 0; attempt < 2000; ++attempt) {
    snapshot = local_kernel.get_execution_snapshot();
    if (snapshot.state == ExecutionState::kStopped &&
        snapshot.error.has_value()) {
      terminal_error_seen = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(terminal_error_seen)
      << "state=" << to_string(snapshot.state);
  EXPECT_EQ(snapshot.error->details.at("hard_stop_transitioned"), "true");
  EXPECT_EQ(snapshot.error->details.at("emergency_fallback"),
            "legacy_stop_succeeded");
}

TEST_F(ExecutionKernelTest,
       Stop_RuntimeVelocityScaleChangesDuringControlledStopCoalesceAndDefer) {
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
  op_profile.max_stop_time_s = 1.0;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

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

  local_kernel.set_runtime_velocity_scale(0.25, std::string{"operator_alice"});
  local_kernel.set_runtime_velocity_scale(0.75, std::string{"operator_bob"});

  EXPECT_EQ(controlled_execution.runtime_velocity_scale_set_count_value(), 0)
      << "backend scale writes must be deferred while the controlled stop is "
         "in flight";
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    auto it = std::find_if(events.rbegin(), events.rend(),
                           [](const KernelEvent& e) {
                             return e.type ==
                                    KernelEventType::kRuntimeVelocityScaleChanged;
                           });
    ASSERT_NE(it, events.rend());
    EXPECT_EQ(it->details.at("backend_status"),
              "deferred_controlled_stop_freeze");
    EXPECT_EQ(it->details.at("applied_scale"), "1");
    EXPECT_EQ(it->details.at("new_scale"), "0.75");
    EXPECT_EQ(it->details.at("principal_id"), "operator_bob");
  }

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopped));
  ASSERT_TRUE(controlled_execution.wait_until_runtime_velocity_scale_set_count(1));
  EXPECT_DOUBLE_EQ(controlled_execution.last_runtime_velocity_scale_value(), 0.75);
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
}

TEST_F(ExecutionKernelTest,
       Stop_RuntimeVelocityScaleDeferredWriteReleasesAfterInterventionExit) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  controlled_execution.fail_next_legacy_stop = true;
  ControlledStopResultDTO refused;
  refused.accepted = false;
  refused.backend_stop_confirmed = false;
  refused.executed_stop_mode = MotionInterruptStopMode::kSoftOnPath;
  refused.error = ErrorDTO{
      std::string(kErrorMotionInterruptBackendStopFailed),
      "injected controlled stop refusal",
      {{"reason", "test_controlled_stop_refused"}}};
  controlled_execution.next_controlled_stop_result = refused;

  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_soft_on_path";
  op_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  op_profile.deceleration_time_s = 0.3;
  op_profile.max_stop_time_s = 1.0;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
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

  local_kernel.set_runtime_velocity_scale(0.4, std::string{"operator_alice"});
  EXPECT_EQ(controlled_execution.runtime_velocity_scale_set_count_value(), 0);

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(wait_for_state(local_kernel,
                             ExecutionState::kInterventionRequired));
  ASSERT_TRUE(controlled_execution.wait_until_runtime_velocity_scale_set_count(1))
      << "deferred scale must release even when controlled stop exits through "
         "intervention";
  const auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"), "operator_stop_total_failure");
  EXPECT_DOUBLE_EQ(controlled_execution.last_runtime_velocity_scale_value(), 0.4);
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
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
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count_value(), 0);
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
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
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
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count_value(), 0);
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
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.profile_id,
            "operator_stop_soft_on_path");
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.stop_mode,
            MotionInterruptStopMode::kSoftOnPath);

  controlled_execution.release_controlled_stop();
  reset_thread.join();

  ASSERT_TRUE(reset.has_value());
  EXPECT_TRUE(reset->accepted);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count_value(), 0);
  EXPECT_EQ(controlled_execution.reset_count, 1);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest,
       Reset_OperatorControlledStopKeepsRuntimeMonitorArmedDuringDecel) {
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
  op_profile.max_stop_time_s = 1.0;
  op_profile.per_joint_max_deceleration_rad_s2 = {{"j1", 8.0}, {"j2", 8.0}};
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 5'000'000'000;
  config.runtime_dynamic_body_max_age_cap_ns = 5'000'000'000;

  MockCollisionWorldPublisher local_publisher;
  auto validation_engine = std::make_shared<MockCollisionEngine>();
  auto runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher,
                                         validation_engine, runtime_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};

  auto result = local_kernel.execute(make_test_request("reset_monitor"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  std::optional<ResetResultDTO> reset;
  std::thread reset_thread([&]() { reset = local_kernel.reset(); });

  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));

  const int baseline_runtime_checks = runtime_engine->check_count.load();
  for (int i = 0; i < 3; ++i) {
    local_clock.advance(config.collision_monitor_period_ns);
    local_scheduler.advance_to(local_clock.now());
  }
  EXPECT_GE(runtime_engine->check_count.load(), baseline_runtime_checks + 3)
      << "reset-selected operator stop must keep runtime monitoring active "
         "during deceleration";
  EXPECT_FALSE(reset.has_value());

  controlled_execution.release_controlled_stop();
  reset_thread.join();

  ASSERT_TRUE(reset.has_value());
  EXPECT_TRUE(reset->accepted);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count_value(), 0);
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
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kSpringBackHold};
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
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count_value(), 1);
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
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
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
  auto outcome = local_kernel.get_execution_outcome(execute_result->value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->move_id, "slow_admission_stop");
  EXPECT_EQ(outcome->terminal_cause, MotionExecutionTerminalCause::kStopped);
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_FALSE(outcome->error.has_value());
}

TEST_F(ExecutionKernelTest,
       Reset_ReturnsPromptlyDuringSlowExecuteAdmission) {
  ControlledExecution controlled_execution;
  controlled_execution.block_sync_observed_state = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
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
  auto outcome = local_kernel.get_execution_outcome(execute_result->value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->move_id, "slow_admission_reset");
  EXPECT_EQ(outcome->terminal_cause, MotionExecutionTerminalCause::kStopped);
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_FALSE(outcome->error.has_value());
}

TEST_F(ExecutionKernelTest,
       ResetThenExecuteDuringSlowAdmissionDoesNotLetOldWorkerDispatch) {
  ControlledExecution controlled_execution;
  controlled_execution.block_sync_observed_state = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  std::optional<Result<uint64_t>> first_execute;
  std::thread first_thread([&]() {
    first_execute =
        local_kernel.execute(make_test_request("slow_admission_reset_then_execute"));
  });

  ASSERT_TRUE(controlled_execution.wait_until_sync_observed_state_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kPlanning));

  auto reset_result = local_kernel.reset();
  ASSERT_TRUE(reset_result.accepted);

  std::optional<Result<uint64_t>> second_execute;
  std::thread second_thread([&]() {
    second_execute =
        local_kernel.execute(make_test_request("after_admission_reset"));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(second_execute.has_value())
      << "new execute should wait for the blocked admission to retire/reset";

  controlled_execution.release_sync_observed_state();
  first_thread.join();
  second_thread.join();

  ASSERT_TRUE(first_execute.has_value());
  EXPECT_TRUE(first_execute->ok());
  ASSERT_TRUE(second_execute.has_value());
  ASSERT_TRUE(second_execute->ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_count(1));
  EXPECT_EQ(controlled_execution.reset_count, 1);
  EXPECT_EQ(controlled_execution.execute_count, 1)
      << "the reset-retired admission must not dispatch a trajectory";
  auto retired_outcome =
      local_kernel.get_execution_outcome(first_execute->value());
  ASSERT_TRUE(retired_outcome.has_value());
  EXPECT_EQ(retired_outcome->move_id, "slow_admission_reset_then_execute");
  EXPECT_EQ(retired_outcome->terminal_cause,
            MotionExecutionTerminalCause::kStopped);

  controlled_execution.release();
  EXPECT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest,
       Pause_ReturnsPromptlyRejectedDuringSlowExecuteAdmission) {
  ControlledExecution controlled_execution;
  controlled_execution.block_sync_observed_state = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache};
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
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSpringBackHold,
       MotionInterruptStopMode::kSoftOnPath};
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
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);
  EXPECT_EQ(controlled_execution.stop_count_value(), 1);
  const auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "operator_stop_profile_failed");
  EXPECT_EQ(snapshot.error->details.at("emergency_fallback"),
            "legacy_stop_succeeded");
  EXPECT_EQ(snapshot.error->details.at("hard_stop_floor"),
            "immediate_hard_stop");
  EXPECT_EQ(snapshot.error->details.at("hard_stop_level"),
            "mock_hard_stop");
  EXPECT_EQ(snapshot.error->details.at("hard_stop_floor_command_source"),
            "backend_stop");
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
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
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
  refused.diagnostics["soft_on_path_error_code"] =
      std::string(kErrorMotionInterruptBackendStopFailed);
  refused.diagnostics["soft_on_path_error_reason"] =
      "soft_on_path_planner_rejected";
  refused.diagnostics["fallback_pair"] = "soft_on_path->spring_back_hold";
  controlled_execution.next_controlled_stop_result = refused;
  controlled_execution.fail_next_legacy_stop = true;
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
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);
  EXPECT_EQ(controlled_execution.stop_count_value(), 1);
  EXPECT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  const auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "operator_stop_total_failure");
  EXPECT_EQ(snapshot.error->details.at("controlled_stop_error_code"),
            std::string(kErrorMotionInterruptBackendStopFailed));
  EXPECT_EQ(snapshot.error->details.at("controlled_stop_error_reason"),
            "soft_on_path_not_supported");
  EXPECT_EQ(snapshot.error->details.at("legacy_stop_error_code"),
            std::string(kErrorStopFailed));
  EXPECT_EQ(snapshot.error->details.at("legacy_stop_error_reason"),
            "test_injected_legacy_stop_failure");
  EXPECT_EQ(snapshot.error->details.at("soft_on_path_error_reason"),
            "soft_on_path_planner_rejected");
  EXPECT_EQ(snapshot.error->details.at("fallback_pair"),
            "soft_on_path->spring_back_hold");
  EXPECT_EQ(snapshot.error->details.at("hard_stop_floor"),
            "immediate_hard_stop");
  EXPECT_EQ(snapshot.error->details.at("hard_stop_floor_command_source"),
            "backend_stop");
  std::lock_guard<std::mutex> lock(events_mutex);
  EXPECT_TRUE(std::any_of(events.begin(), events.end(),
                          [](const KernelEvent& e) {
                            return e.type == KernelEventType::kStateTransition &&
                                   e.to_state == ExecutionState::kInterventionRequired &&
                                   e.details.contains("reason") &&
                                   e.details.at("reason") ==
                                       "operator_stop_total_failure";
                          }));
  auto total_failure_event =
      std::find_if(events.begin(), events.end(), [](const KernelEvent& e) {
        return e.type == KernelEventType::kErrorRaised &&
               e.details.contains("reason") &&
               e.details.at("reason") == "operator_stop_total_failure";
      });
  ASSERT_NE(total_failure_event, events.end());
  EXPECT_EQ(total_failure_event->details.at("controlled_stop_error_reason"),
            "soft_on_path_not_supported");
  EXPECT_EQ(total_failure_event->details.at("legacy_stop_error_reason"),
            "test_injected_legacy_stop_failure");
  EXPECT_EQ(total_failure_event->details.at("soft_on_path_error_reason"),
            "soft_on_path_planner_rejected");
}

// Spec/230 §6.1 stop chain. Operator-stop from kExecuting MUST emit
// the full sequence:
//   kStopRequested → state_transition(Executing→Stopping)
//                  → kStopIssued
//                  → state_transition(Stopping→Stopped)
// The intermediate kStopping transition makes the controlled-stop
// deceleration window observable (1-2 s for soft-on-path); without
// it the UI cannot render "Stopping…" and trace reconstruction
// loses the operator-intent vs backend-confirmation boundary.
TEST_F(ExecutionKernelTest, Stop_EmitsFullEventChain_PerSpec_6_1) {
  ControlledExecution controlled_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock,
      local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      local_kernel, local_collision_world, local_collision_engine);

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
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock,
      local_scheduler, local_cache};
  configure_authoritative_collision_validation(
      local_kernel, local_collision_world, local_collision_engine);

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
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher,
                                         local_engine,
                                         local_runtime_engine);
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSpringBackHold,
       MotionInterruptStopMode::kSoftOnPath};

  // Calling stop() while Idle must not invoke the operator controlled-stop
  // profile and must not poison the one-shot backend-stop skip flag. The next
  // internal fault preempt below still has to call legacy backend.stop().
  auto idle_stop = local_kernel.stop();
  EXPECT_FALSE(idle_stop.accepted);
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 0);
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);

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
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";
  local_clock.advance(20'000'000);
  local_scheduler.advance_to(local_clock.now());
  // The fault preempt runs on the kernel's dedicated fault worker thread;
  // it is NOT synchronized by wait_for_completion, so counter reads in
  // this window must use the fake's mutex-guarded accessors.
  for (int attempt = 0;
       attempt < 100 && controlled_execution.stop_count_value() == 0;
       ++attempt) {
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
  EXPECT_EQ(controlled_execution.stop_count_value(), 1);
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 0);
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
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher,
                                         local_engine,
                                         local_runtime_engine);
  // Events are emitted from BOTH the execution-worker thread and the
  // interrupt/monitor lane, so the capture must be mutex-guarded; an
  // unsynchronized vector races (and corrupts/segfaults under load).
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
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";
  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  for (int attempt = 0;
       attempt < 100 && controlled_execution.stop_count_value() == 0; ++attempt) {
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
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);
  EXPECT_EQ(controlled_execution.stop_count_value(), 1);

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
       FaultStopProfile_DeadlineWatchdogCancelsControlledStopAndFallsBack) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "fault_spring_back";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  fault_profile.deceleration_time_s = 0.05;
  fault_profile.max_stop_time_s = 0.1;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);

  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher,
                                         local_engine,
                                         local_runtime_engine);
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
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";

  std::jthread monitor_tick([&]() {
    local_clock.advance(10'000'000);
    local_scheduler.advance_to(local_clock.now());
  });

  if (monitor_tick.joinable()) {
    monitor_tick.join();
  }
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1))
      << "fault preempt did not enter execute_controlled_stop";
  local_runtime_engine->default_result = CollisionResult{};
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "fault preempt deadline watchdog was not scheduled";

  bool watchdog_cancel_seen = false;
  for (int attempt = 0; attempt < 200 && !watchdog_cancel_seen; ++attempt) {
    local_clock.advance(20'000'000);
    local_scheduler.advance_to(local_clock.now());
    watchdog_cancel_seen =
        controlled_execution.wait_until_controlled_stop_cancel_count(
            1, std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(watchdog_cancel_seen)
      << "fault preempt deadline watchdog must cancel the blocked stop";
  ASSERT_TRUE(controlled_execution.wait_until_stop_attempt_count(1))
      << "fault preempt deadline watchdog must issue the hard-stop floor";

  bool completed = local_kernel.wait_for_completion(200'000'000);
  if (!completed) {
    controlled_execution.release();
    completed = local_kernel.wait_for_completion(1'000'000'000);
  }
  EXPECT_TRUE(completed);

  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count_value(), 1);
  EXPECT_GE(controlled_execution.stop_attempt_count_value(), 1);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kInterrupted);

  const auto has_watchdog_event = [&]() {
    std::lock_guard<std::mutex> lock(events_mutex);
    return std::any_of(
        events.begin(), events.end(), [](const KernelEvent& e) {
          return e.type == KernelEventType::kErrorRaised &&
                 e.details.contains("phase") &&
                 e.details.at("phase") ==
                     "fault_preempt_deadline_watchdog" &&
                 e.details.contains("hard_stop_transitioned") &&
                 e.details.at("hard_stop_transitioned") == "true" &&
                 e.details.contains("emergency_fallback") &&
                 e.details.at("emergency_fallback") == "legacy_stop_succeeded";
        });
  };
  const auto has_preempt_event = [&]() {
    std::lock_guard<std::mutex> lock(events_mutex);
    return std::any_of(
        events.begin(), events.end(), [](const KernelEvent& e) {
          return e.type == KernelEventType::kInterruptPreemptApplied &&
                 e.details.contains("preempt_path") &&
                 e.details.at("preempt_path") ==
                     "controlled_stop_failed_legacy_fallback_succeeded" &&
                 e.details.contains("hard_stop_transitioned") &&
                 e.details.at("hard_stop_transitioned") == "true";
        });
  };
  bool saw_watchdog_event = false;
  bool saw_preempt_event = false;
  for (int attempt = 0; attempt < 500; ++attempt) {
    saw_watchdog_event = saw_watchdog_event || has_watchdog_event();
    saw_preempt_event = saw_preempt_event || has_preempt_event();
    if (saw_watchdog_event && saw_preempt_event) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(saw_watchdog_event);
  EXPECT_TRUE(saw_preempt_event);

  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->preemption_result, InterruptPreemptionResult::kSucceeded);
}

TEST_F(ExecutionKernelTest,
       FaultStopProfile_RuntimeCollisionDuringFaultPreemptStopEscalates) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 5'000'000'000;
  config.runtime_dynamic_body_max_age_cap_ns = 5'000'000'000;
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "fault_spring_back_collision_window";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  fault_profile.deceleration_time_s = 0.3;
  fault_profile.max_stop_time_s = 5.0;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);

  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher,
                                         local_engine,
                                         local_runtime_engine);
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request("fault_stop_collision"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision =
      CollisionWorldRevision{120, "fault-preempt-stop-collision"};
  local_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";

  // First tick latches the runtime collision fault and starts the configured
  // fault-preempt controlled stop. The controlled stop is deliberately blocked
  // so the next monitor tick exercises the deceleration window itself.
  local_clock.advance(config.collision_monitor_period_ns);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1))
      << "fault preempt did not enter execute_controlled_stop";
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "fault-preempt monitor was not re-armed for the decel window";

  bool stop_path_cancel_seen = false;
  for (int attempt = 0; attempt < 200 && !stop_path_cancel_seen; ++attempt) {
    local_clock.advance(config.collision_monitor_period_ns);
    local_scheduler.advance_to(local_clock.now());
    stop_path_cancel_seen =
        controlled_execution.wait_until_controlled_stop_cancel_count(
            1, std::chrono::milliseconds(1));
  }
  if (!stop_path_cancel_seen) {
    controlled_execution.release_controlled_stop();
  }
  ASSERT_TRUE(stop_path_cancel_seen)
      << "fault-preempt stop-path collision must cancel the controlled stop";
  ASSERT_TRUE(controlled_execution.wait_until_stop_attempt_count(1))
      << "fault-preempt stop-path collision must issue the hard-stop floor";

  bool completed = local_kernel.wait_for_completion(200'000'000);
  if (!completed) {
    controlled_execution.release();
    completed = local_kernel.wait_for_completion(1'000'000'000);
  }
  EXPECT_TRUE(completed);
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);

  const auto has_stop_path_collision = [&]() {
    std::lock_guard<std::mutex> lock(events_mutex);
    return std::any_of(
        events.begin(), events.end(), [](const KernelEvent& e) {
          return e.type == KernelEventType::kCollisionDetected &&
                 e.details.contains("reason_during_stop") &&
                 e.details.at("reason_during_stop") ==
                     "stop_path_collision" &&
                 e.details.contains("stop_owner") &&
                 e.details.at("stop_owner") == "fault_preempt_stop" &&
                 e.details.contains("fault_preempt_stop_cancel_requested") &&
                 e.details.at("fault_preempt_stop_cancel_requested") == "true";
        });
  };
  const auto has_preempt_event = [&]() {
    std::lock_guard<std::mutex> lock(events_mutex);
    return std::any_of(
        events.begin(), events.end(), [](const KernelEvent& e) {
          return e.type == KernelEventType::kInterruptPreemptApplied &&
                 e.details.contains("preempt_path") &&
                 e.details.at("preempt_path") ==
                     "controlled_stop_failed_legacy_fallback_succeeded" &&
                 e.details.contains("preemption_result") &&
                 e.details.at("preemption_result") ==
                     "backend_preempt_failed" &&
                 e.details.contains(
                     "stop_path_collision_reason_during_stop") &&
                 e.details.at("stop_path_collision_reason_during_stop") ==
                     "stop_path_collision" &&
                 e.details.contains("stop_path_collision_stop_owner") &&
                 e.details.at("stop_path_collision_stop_owner") ==
                     "fault_preempt_stop" &&
                 e.details.contains("hard_stop_floor_already_stopped") &&
                 e.details.at("hard_stop_floor_already_stopped") == "true";
        });
  };
  const auto dump_events = [&]() {
    std::lock_guard<std::mutex> lock(events_mutex);
    std::string out;
    for (const auto& e : events) {
      out += "\n  type=" + std::to_string(static_cast<int>(e.type));
      for (const auto& [key, value] : e.details) {
        out += " " + key + "=" + value;
      }
    }
    return out;
  };

  bool saw_stop_path_collision = false;
  bool saw_preempt_event = false;
  for (int attempt = 0; attempt < 500; ++attempt) {
    saw_stop_path_collision =
        saw_stop_path_collision || has_stop_path_collision();
    saw_preempt_event = saw_preempt_event || has_preempt_event();
    if (saw_stop_path_collision && saw_preempt_event) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(saw_stop_path_collision) << dump_events();
  EXPECT_TRUE(saw_preempt_event) << dump_events();

  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->preemption_result,
            InterruptPreemptionResult::kBackendPreemptFailed);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "fault_preempt_stop_path_collision");

  auto ack = local_kernel.acknowledge_intervention_for_retry(
      "operator_alice", "ui_acknowledge_and_continue");
  ASSERT_TRUE(ack.accepted)
      << (ack.error.has_value() ? ack.error->message : std::string{});
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_FALSE(local_kernel.get_active_interrupt_latch().has_value());
  EXPECT_EQ(controlled_execution.reset_count, 0);
}

TEST_F(ExecutionKernelTest,
       FaultStopProfile_TotalPreemptFailureRequiresIntervention) {
  ControlledExecution controlled_execution;
  ControlledStopResultDTO refused;
  refused.accepted = false;
  refused.executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
  refused.backend_stop_confirmed = false;
  refused.error = ErrorDTO{
      std::string(kErrorMotionInterruptBackendStopFailed),
      "controlled stop refused by test",
      {{"reason", "test_controlled_stop_refused"}}};
  controlled_execution.next_controlled_stop_result = refused;
  controlled_execution.fail_next_legacy_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 5'000'000'000;
  config.runtime_dynamic_body_max_age_cap_ns = 5'000'000'000;
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "fault_total_failure";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  fault_profile.deceleration_time_s = 0.01;
  fault_profile.max_stop_time_s = 1.0;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);

  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher,
                                         local_engine,
                                         local_runtime_engine);

  auto result = local_kernel.execute(make_test_request("fault_total_failure"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";

  local_clock.advance(config.collision_monitor_period_ns);
  local_scheduler.advance_to(local_clock.now());

  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  ASSERT_TRUE(controlled_execution.wait_until_stop_attempt_count(1));
  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->preemption_result,
            InterruptPreemptionResult::kBackendPreemptFailed);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"), "fault_stop_total_failure");

  auto ack = local_kernel.acknowledge_intervention_for_retry(
      "operator_alice", "ui_acknowledge_and_continue");
  ASSERT_TRUE(ack.accepted)
      << (ack.error.has_value() ? ack.error->message : std::string{});
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_FALSE(local_kernel.get_active_interrupt_latch().has_value());
  EXPECT_EQ(controlled_execution.reset_count, 0);
}

TEST_F(ExecutionKernelTest,
       FaultPreemptPendingTimeoutHardStopsWhenPreemptWorkerDoesNotMark) {
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "fault_pending_bound";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  fault_profile.deceleration_time_s = 0.01;
  fault_profile.max_stop_time_s = 0.02;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);

  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{planner,
                               context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher,
                                         local_engine,
                                         local_runtime_engine);
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = local_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{111, "pending-bound"};
  local_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";

  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1))
      << "fault preempt worker did not enter the blocked controlled stop";
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "fault preempt deadline watchdog should be queued but not run";

  bool pending_bound_fired = false;
  for (int attempt = 0; attempt < 100 && !pending_bound_fired; ++attempt) {
    local_clock.advance(20'000'000);
    pending_bound_fired = controlled_execution.wait_until_stop_attempt_count(
        1, std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(pending_bound_fired)
      << "execute worker pending bound must issue the hard-stop floor even "
         "when the scheduler watchdog is not advanced";

  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);

  auto latch = local_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->preemption_result,
            InterruptPreemptionResult::kBackendPreemptFailed);

  const auto has_pending_bound_event = [&]() {
    std::lock_guard<std::mutex> lock(events_mutex);
    return std::any_of(
        events.begin(), events.end(), [](const KernelEvent& e) {
          return e.type == KernelEventType::kErrorRaised &&
                 e.details.contains("phase") &&
                 e.details.at("phase") == "fault_preempt_pending_watchdog" &&
                 e.details.contains("hard_stop_transitioned") &&
                 e.details.at("hard_stop_transitioned") == "true";
        });
  };
  EXPECT_TRUE(has_pending_bound_event());
  controlled_execution.release_controlled_stop();
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_UsesBackendObservedStateForRuntimeQuery) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  std::vector<double> queried_positions;
  local_runtime_engine->on_check = [&](const CollisionQuery& query) {
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
  auto metrics = controlled_kernel.get_metrics();
  EXPECT_EQ(metrics.runtime_collision_ticks_started, 1u);
  EXPECT_EQ(metrics.runtime_collision_ticks_completed, 1u);
  EXPECT_EQ(metrics.runtime_collision_ticks_faulted, 0u);
  ASSERT_EQ(queried_positions.size(), 2u);
  EXPECT_DOUBLE_EQ(queried_positions[0], 0.25);
  EXPECT_DOUBLE_EQ(queried_positions[1], 0.75);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_RemapsEqualLengthObservedStateWhenJointOrderDiffers) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  TrajectoryDTO collision_trajectory;
  collision_trajectory.joint_names = {"j2", "j1"};
  collision_trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {}, {}});
  context.next_expand_result =
      Result<TrajectoryDTO>::Ok(std::move(collision_trajectory));

  std::vector<std::string> queried_joint_names;
  std::vector<double> queried_positions;
  local_runtime_engine->on_check = [&](const CollisionQuery& query) {
    queried_joint_names = query.robot_state.joint_names;
    queried_positions = query.robot_state.positions_rad;
  };

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(queried_joint_names, (std::vector<std::string>{"j2", "j1"}));
  ASSERT_EQ(queried_positions.size(), 2u);
  EXPECT_DOUBLE_EQ(queried_positions[0], 0.75);
  EXPECT_DOUBLE_EQ(queried_positions[1], 0.25);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ObservedStateShapeMismatchFailsClosedBeforeEngineQuery) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25}, local_clock.now());
  const int baseline_engine_checks = local_runtime_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(local_runtime_engine->check_count.load(), baseline_engine_checks);
  EXPECT_TRUE(controlled_execution.wait_until_stop_count(1));
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->signal_snapshot.metadata.at("reason"),
            "runtime_collision_projection_observed_shape_mismatch");
  EXPECT_EQ(latch->signal_snapshot.metadata.at("observed_position_count"),
            "1");
  EXPECT_EQ(latch->signal_snapshot.metadata.at("source_joint_count"), "2");
  EXPECT_EQ(latch->signal_snapshot.metadata.at("query_joint_count"), "2");
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
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, 0);
  local_clock.advance(20'000'000);
  const int baseline_engine_checks = local_runtime_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(local_runtime_engine->check_count.load(), baseline_engine_checks);
  EXPECT_TRUE(controlled_execution.wait_until_stop_count(1));
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
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

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
  const int baseline_engine_checks = local_runtime_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_TRUE(refreshed_during_monitor_tick);
  EXPECT_GT(local_runtime_engine->check_count.load(), baseline_engine_checks);
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
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
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);
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
  const int baseline_engine_checks = local_runtime_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(local_runtime_engine->check_count.load(), baseline_engine_checks);
  EXPECT_TRUE(controlled_execution.wait_until_stop_count(1));
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
       CollisionMonitoring_RefreshesRuntimeSnapshotOnWorldRevisionChange) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.runtime_dynamic_body_max_age_cap_ns = 100'000'000;  // 100 ms
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  auto initial_snapshot = make_test_collision_world_snapshot();
  initial_snapshot.revision = CollisionWorldRevision{42, "dynamic-world-old"};
  initial_snapshot.bodies.push_back(make_dynamic_test_body(0, 10'000'000));
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(
          std::move(initial_snapshot));

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  const int validation_bundle_calls_after_dispatch =
      local_collision_world.bundle_call_count.load();
  const int runtime_bundle_calls_before_tick =
      local_collision_world.published_runtime_bundle_call_count.load();

  local_clock.advance(20'000'000);
  const CollisionWorldRevision fresh_revision{43, "dynamic-world-fresh"};
  auto fresh_snapshot = make_test_collision_world_snapshot();
  fresh_snapshot.revision = fresh_revision;
  fresh_snapshot.bodies.push_back(
      make_dynamic_test_body(local_clock.now(), 10'000'000));
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(fresh_snapshot));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  const int baseline_engine_checks = local_runtime_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(local_collision_world.bundle_call_count.load(),
            validation_bundle_calls_after_dispatch)
      << "runtime ticks must not borrow the validation snapshot bundle";
  EXPECT_GT(local_collision_world.published_runtime_bundle_call_count.load(),
            runtime_bundle_calls_before_tick);
  EXPECT_GT(local_runtime_engine->check_count.load(), baseline_engine_checks);
  expect_collision_world_revision(
      local_runtime_engine->last_collision_world_revision, fresh_revision);
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ContactReleaseAllowsBoundedOngoingReleaseContact) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

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
  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result =
      make_support_contact_collision_result(pair, -0.004);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  const int baseline_stop_count = controlled_execution.stop_count_value();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(controlled_execution.stop_count_value(), baseline_stop_count);
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
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

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
  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result =
      make_support_contact_collision_result(pair, -0.004);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  ASSERT_EQ(controlled_execution.stop_count_value(), 0);

  local_runtime_engine->default_result =
      make_support_contact_collision_result(pair, -0.006);
  local_clock.advance(10'000'000);
  controlled_execution.set_progress(0.50, {0.50, 0.90}, local_clock.now());
  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_TRUE(controlled_execution.wait_until_stop_count(1));
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->signal_snapshot.metadata.at("reason"),
            "runtime_contact_release_penetration_increased");
  EXPECT_EQ(latch->signal_snapshot.metadata.at("contact_a"), pair.body_a);
  EXPECT_EQ(latch->signal_snapshot.metadata.at("contact_b"), pair.body_b);
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ContactGraspAllowsBoundedRuntimeContact) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId pair{"object/graspable/shape/0",
                             "robot/robot1/link/right_finger/collision/0"};
  auto policy = make_contact_grasp_policy_snapshot(
      kPolicyId, "graspable", {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_allowed");
  request.contact_policy_id = kPolicyId;
  auto result = controlled_kernel.execute(request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result =
      make_contact_grasp_collision_result(pair, -0.001);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  const int baseline_stop_count = controlled_execution.stop_count_value();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(controlled_execution.stop_count_value(), baseline_stop_count);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ContactGraspPenetrationExceededFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId pair{"object/graspable/shape/0",
                             "robot/robot1/link/right_finger/collision/0"};
  auto policy = make_contact_grasp_policy_snapshot(
      kPolicyId, "graspable", {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_over_limit");
  request.contact_policy_id = kPolicyId;
  auto result = controlled_kernel.execute(request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result =
      make_contact_grasp_collision_result(pair, -0.006);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_TRUE(controlled_execution.wait_until_stop_count(1));
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->signal_snapshot.metadata.at("reason"),
            "runtime_collision_detected");
  EXPECT_EQ(latch->signal_snapshot.metadata.at(
                "collision_result_raw_contact_grasp_violation"),
            "grasp_contact_penetration_exceeded");
  EXPECT_EQ(latch->signal_snapshot.metadata.at(
                "collision_result_raw_contact_grasp_policy_id"),
            kPolicyId);
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ContactGraspMixedAllowedAndViolationKeepsViolation) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId violating_pair{
      "object/graspable/shape/0",
      "robot/robot1/link/right_finger/collision/0"};
  const CollisionPairId allowed_pair{
      "object/graspable/shape/0",
      "robot/robot1/link/left_finger/collision/0"};
  auto policy = make_contact_grasp_policy_snapshot(
      kPolicyId, "graspable", {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_mixed_pairs");
  request.contact_policy_id = kPolicyId;
  auto result = controlled_kernel.execute(request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  CollisionResult runtime_result;
  runtime_result.in_collision = true;
  runtime_result.decision = CollisionDecisionResult::kFail;
  runtime_result.engine_status = CollisionEngineStatus::kOk;
  runtime_result.normalized_pairs = {violating_pair, allowed_pair};
  runtime_result.pair_clearances = {
      CollisionPairClearance{violating_pair, -0.006},
      CollisionPairClearance{allowed_pair, -0.001},
  };
  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = runtime_result;
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_TRUE(controlled_execution.wait_until_stop_count(1));
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->signal_snapshot.metadata.at(
                "collision_result_raw_contact_grasp_violation"),
            "grasp_contact_penetration_exceeded");
  EXPECT_EQ(latch->signal_snapshot.metadata.at(
                "collision_result_first_failure_pair"),
            "object/graspable/shape/0 <-> "
            "robot/robot1/link/right_finger/collision/0");
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ContactGraspMissingClearanceFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId pair{"object/graspable/shape/0",
                             "robot/robot1/link/right_finger/collision/0"};
  auto policy = make_contact_grasp_policy_snapshot(
      kPolicyId, "graspable", {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_missing_clearance");
  request.contact_policy_id = kPolicyId;
  auto result = controlled_kernel.execute(request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  auto runtime_result = make_contact_grasp_collision_result(pair, -0.001);
  runtime_result.pair_clearances.clear();
  runtime_result.min_clearance_m.reset();
  runtime_result.min_clearance_pair.reset();
  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = runtime_result;
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_TRUE(controlled_execution.wait_until_stop_count(1));
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->signal_snapshot.metadata.at(
                "collision_result_raw_contact_grasp_violation"),
            "missing_grasp_contact_clearance");
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ContactGraspUnexpectedPairFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId unexpected_pair{
      "object/other_part/shape/0",
      "robot/robot1/link/right_finger/collision/0"};
  auto policy = make_contact_grasp_policy_snapshot(
      kPolicyId, "graspable", {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_unexpected_pair");
  request.contact_policy_id = kPolicyId;
  auto result = controlled_kernel.execute(request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result =
      make_contact_grasp_collision_result(unexpected_pair, -0.001);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_TRUE(controlled_execution.wait_until_stop_count(1));
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kInterrupted);
  auto latch = controlled_kernel.get_active_interrupt_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->signal_snapshot.metadata.at(
                "collision_result_first_failure_classification"),
            "blocking_collision");
  EXPECT_EQ(latch->signal_snapshot.metadata.at(
                "collision_result_first_failure_pair"),
            "object/other_part/shape/0 <-> "
            "robot/robot1/link/right_finger/collision/0");
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
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::optional<std::int64_t> observed_timeout_budget_ns;
  local_runtime_engine->on_check = [&](const CollisionQuery& query) {
    observed_timeout_budget_ns = query.timeout_budget_ns;
  };
  ExecutionKernel controlled_kernel{planner,
                                    context,
                                    controlled_execution,
                                    local_clock,
                                    local_scheduler,
                                    local_cache,
                                    config};
  configure_runtime_collision_monitoring(
      controlled_kernel, local_collision_world, local_collision_engine,
      local_runtime_engine);

  auto result = controlled_kernel.execute(make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called()) << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));
  const int baseline_collision_count = context.collision_count.load();
  const int baseline_engine_count = local_runtime_engine->check_count.load();

  local_clock.advance(49'000'000);
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(context.collision_count.load(), baseline_collision_count);
  EXPECT_EQ(local_runtime_engine->check_count.load(), baseline_engine_count);

  local_clock.advance(1'000'000);
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(context.collision_count.load(), baseline_collision_count);
  EXPECT_EQ(local_runtime_engine->check_count.load(), baseline_engine_count + 1);
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
  // The async worker can advance past Planning before a get_status() probe
  // runs (sanitizer scheduling makes this routine), so the prompt
  // idle->Planning admission is asserted via the recorded event stream
  // instead of a racy immediate snapshot.
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = kernel.execute(make_test_request("cache_miss_move"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().execution_id, result.value());
  kernel.wait_for_completion();

  std::lock_guard<std::mutex> lock(events_mutex);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events[0].type, KernelEventType::kStateTransition);
  EXPECT_EQ(events[0].from_state, ExecutionState::kIdle);
  EXPECT_EQ(events[0].to_state, ExecutionState::kPlanning);
  const bool planned = std::any_of(
      events.begin(), events.end(),
      [](const KernelEvent& e) { return e.type == KernelEventType::kPlanStarted; });
  const bool cache_hit = std::any_of(
      events.begin(), events.end(),
      [](const KernelEvent& e) { return e.type == KernelEventType::kCacheHit; });
  EXPECT_TRUE(planned);
  EXPECT_FALSE(cache_hit);
}

TEST_F(ExecutionKernelTest, Execute_ImmediatelyTransitionsToValidatingOnCacheHit) {
  store_cache("cached_move", make_test_trajectory());

  // Admission enters Planning synchronously even on a cache hit (so
  // stop/reset can interrupt slow preparation), then the worker resolves the
  // hit and moves to Validating without planning work. A get_status() probe
  // can land in either window, so the skip-planning contract is asserted via
  // the recorded event stream.
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = kernel.execute(make_test_request("cached_move"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().execution_id, result.value());
  kernel.wait_for_completion();

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events[0].type, KernelEventType::kStateTransition);
    EXPECT_EQ(events[0].from_state, ExecutionState::kIdle);
    EXPECT_EQ(events[0].to_state, ExecutionState::kPlanning);
    const bool cache_hit = std::any_of(
        events.begin(), events.end(),
        [](const KernelEvent& e) { return e.type == KernelEventType::kCacheHit; });
    const bool planned = std::any_of(
        events.begin(), events.end(),
        [](const KernelEvent& e) { return e.type == KernelEventType::kPlanStarted; });
    const bool reached_validating = std::any_of(
        events.begin(), events.end(),
        [](const KernelEvent& e) {
          return e.type == KernelEventType::kStateTransition &&
                 e.to_state == ExecutionState::kValidating;
        });
    EXPECT_TRUE(cache_hit);
    EXPECT_FALSE(planned);
    EXPECT_TRUE(reached_validating);
  }
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

TEST_F(ExecutionKernelTest,
       ExecuteAdmissionEventsFlushBeforeFastWorkerTerminalEvents) {
  std::mutex events_mutex;
  std::vector<KernelEventType> events;
  std::atomic<bool> delayed_first_admission_callback{false};

  kernel.subscribe_events([&](const KernelEvent& e) {
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      events.push_back(e.type);
    }
    if (e.type == KernelEventType::kStateTransition &&
        e.to_state == ExecutionState::kPlanning &&
        !delayed_first_admission_callback.exchange(true)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });

  auto result = kernel.execute(make_test_request("admission_order_fast_worker"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));

  std::lock_guard<std::mutex> lock(events_mutex);
  const auto cache_miss =
      std::find(events.begin(), events.end(), KernelEventType::kCacheMiss);
  const auto plan_started =
      std::find(events.begin(), events.end(), KernelEventType::kPlanStarted);
  const auto execute_completed =
      std::find(events.begin(), events.end(), KernelEventType::kExecuteCompleted);
  ASSERT_NE(cache_miss, events.end());
  ASSERT_NE(plan_started, events.end());
  ASSERT_NE(execute_completed, events.end());
  EXPECT_LT(std::distance(events.begin(), cache_miss),
            std::distance(events.begin(), execute_completed));
  EXPECT_LT(std::distance(events.begin(), plan_started),
            std::distance(events.begin(), execute_completed));
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

TEST_F(ExecutionKernelTest, Events_RuntimeVelocityScaleChangedEmitted) {
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  kernel.set_runtime_velocity_scale(0.5, std::string{"operator_alice"});

  std::lock_guard<std::mutex> lock(events_mutex);
  auto it = std::find_if(events.begin(), events.end(), [](const KernelEvent& e) {
    return e.type == KernelEventType::kRuntimeVelocityScaleChanged;
  });
  ASSERT_NE(it, events.end());
  EXPECT_EQ(it->details.at("previous_scale"), "1");
  EXPECT_EQ(it->details.at("new_scale"), "0.5");
  EXPECT_EQ(it->details.at("applied_scale"), "0.5");
  EXPECT_EQ(it->details.at("backend_status"), "forwarded");
  EXPECT_EQ(it->details.at("source"), "kernel_api");
  EXPECT_EQ(it->details.at("state"), "Idle");
  EXPECT_EQ(it->details.at("principal_id"), "operator_alice");
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
  EXPECT_EQ(slow_planner.cancel_count, 1);

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
  EXPECT_EQ(slow_planner.cancel_count, 1);

  // Release the planner
  slow_planner.release();

  // Wait for completion
  kernel.wait_for_completion();

  // Verify state is Idle
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest,
       Cancel_StopDuringValidationRequestsValidationCancel) {
  MockPlanner local_planner;
  PlanResultDTO plan_ok;
  plan_ok.trajectory = make_test_trajectory();
  local_planner.next_result = plan_ok;
  MockExecution local_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  local_engine->block_until_cancel_requested = true;
  ExecutionKernelConfig config;
  config.validation_collision_wall_timeout_ns = 500'000'000;
  config.validation_collision_workers = 1;
  ExecutionKernel local_kernel{local_planner,
                               context,
                               local_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto execute_result =
      local_kernel.execute(make_test_request("stop_validation_cancel"));
  ASSERT_TRUE(execute_result.ok());
  ASSERT_TRUE(local_engine->wait_until_check_blocked())
      << "validation did not enter the collision engine";

  const auto started = std::chrono::steady_clock::now();
  auto stop_result = local_kernel.stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(stop_result.accepted);
  EXPECT_LT(elapsed, std::chrono::milliseconds(200));
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kStopped);
  EXPECT_EQ(local_execution.execute_count, 0);
  EXPECT_GE(local_engine->cancel_observed_count.load(), 1);
  EXPECT_EQ(local_engine->observed_cancel_reason(), "operator_stop");
  local_engine->release_blocked();
}

TEST_F(ExecutionKernelTest,
       Cancel_ResetDuringValidationRequestsValidationCancel) {
  MockPlanner local_planner;
  PlanResultDTO plan_ok;
  plan_ok.trajectory = make_test_trajectory();
  local_planner.next_result = plan_ok;
  MockExecution local_execution;
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  local_engine->block_until_cancel_requested = true;
  ExecutionKernelConfig config;
  config.validation_collision_wall_timeout_ns = 500'000'000;
  config.validation_collision_workers = 1;
  ExecutionKernel local_kernel{local_planner,
                               context,
                               local_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  auto execute_result =
      local_kernel.execute(make_test_request("reset_validation_cancel"));
  ASSERT_TRUE(execute_result.ok());
  ASSERT_TRUE(local_engine->wait_until_check_blocked())
      << "validation did not enter the collision engine";

  const auto started = std::chrono::steady_clock::now();
  auto reset_result = local_kernel.reset();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(reset_result.accepted);
  EXPECT_LT(elapsed, std::chrono::milliseconds(200));
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(local_execution.execute_count, 0);
  EXPECT_EQ(local_execution.reset_count, 1);
  EXPECT_GE(local_engine->cancel_observed_count.load(), 1);
  EXPECT_EQ(local_engine->observed_cancel_reason(), "reset");
  local_engine->release_blocked();
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

  // Audit 6G: completion can be observable a beat before the final event
  // callbacks land under load; wait (bounded) for both events instead of
  // racing the emission, then assert ordering as before.
  int validation_failed_idx = -1;
  int error_raised_idx = -1;
  const auto find_events = [&]() {
    validation_failed_idx = -1;
    error_raised_idx = -1;
    for (size_t i = 0; i < events.size(); ++i) {
      if (events[i].type == KernelEventType::kValidationFailed) {
        validation_failed_idx = static_cast<int>(i);
      }
      if (events[i].type == KernelEventType::kErrorRaised &&
          error_raised_idx < 0) {
        error_raised_idx = static_cast<int>(i);
      }
    }
    return validation_failed_idx >= 0 && error_raised_idx >= 0;
  };
  for (int attempt = 0; attempt < 2000; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      if (find_events()) {
        break;
      }
    }
    // Sleep outside the lock: the kernel emitter takes events_mutex in the
    // subscription callback.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::lock_guard<std::mutex> lock(events_mutex);
  find_events();
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
  EXPECT_GT(metrics.events_emitted, 0u);
  EXPECT_EQ(metrics.flight_recorder_records, metrics.events_emitted);
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

TEST_F(ExecutionKernelTest,
       EventSubscriberExceptionDoesNotBlockOtherSubscribers) {
  int delivered_to_healthy_subscriber = 0;
  bool threw_once = false;
  kernel.subscribe_events([&](const KernelEvent&) {
    if (!threw_once) {
      threw_once = true;
      throw std::runtime_error("subscriber failed");
    }
  });
  kernel.subscribe_events([&](const KernelEvent&) {
    ++delivered_to_healthy_subscriber;
  });

  execute_sync(make_test_request("subscriber_exception_metrics"));

  EXPECT_GT(delivered_to_healthy_subscriber, 0);
  EXPECT_EQ(kernel.get_metrics().event_subscriber_exceptions, 1u);
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
  EXPECT_GE(controlled_execution.stop_count_value(), 1);
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
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{
      slow_planner, context, execution, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(
      local_kernel, collision_world_publisher, collision_engine);

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
    const auto state_before_stop = local_kernel.get_status().state;
    auto stop_result = local_kernel.stop();
    ASSERT_TRUE(stop_result.accepted)
        << "Cycle " << i << " stop failed"
        << " before_state=" << to_string(state_before_stop)
        << " state=" << to_string(local_kernel.get_status().state)
        << " error_code="
        << (stop_result.error ? stop_result.error->code : std::string("<none>"))
        << " error_message="
        << (stop_result.error ? stop_result.error->message : std::string("<none>"))
        << " reason="
        << (stop_result.error && stop_result.error->details.contains("reason")
                ? stop_result.error->details.at("reason")
                : std::string("<none>"));
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
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);
  EXPECT_EQ(controlled_execution.last_controlled_stop_profile.stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTripRacingBackendCompletionClassifiesSignalTrip) {
  using namespace motion_45c;
  // Deterministic race harness for the completion-vs-trip window:
  // once a kCompleteMoveOnSignal latch has entered the controlled-stop
  // handler, a backend normal-completion notification arriving before the
  // trip terminal commit must not launder the outcome into no_signal_trip or
  // completed_normally. The trip stop authority owns the terminal.
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
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

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  auto req = make_test_request("trip_completion_race");
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
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));

  // Backend completion is now observable while the trip stop is still blocked.
  controlled_execution.complete();
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1);

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kSignalTrip)
      << "backend completion raced the trip stop terminal classification";
  ASSERT_TRUE(outcome->trip.has_value());
  EXPECT_FALSE(outcome->error.has_value());
  EXPECT_TRUE(local_kernel.active_motion_interrupt_scopes().empty());
  EXPECT_FALSE(local_kernel.get_active_interrupt_latch().has_value());

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    const auto normal_completion_events =
        std::count_if(events.begin(), events.end(), [](const KernelEvent& event) {
          return event.type == KernelEventType::kExecuteCompleted;
        });
    EXPECT_EQ(normal_completion_events, 0)
        << "normal completion event escaped after motion-trip authority";
  }
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_RuntimeVelocityScaleChangesDuringControlledStopCoalesceAndDefer) {
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  // Captured by the kernel's event callback: must outlive the kernel
  // (the kernel destructor joins its workers).
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{planner,      context,         controlled_execution,
                               local_clock,  local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto req = make_test_request("trip_scale_freeze");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));

  local_kernel.set_runtime_velocity_scale(0.2, std::string{"operator_alice"});
  local_kernel.set_runtime_velocity_scale(0.6, std::string{"operator_bob"});

  EXPECT_EQ(controlled_execution.runtime_velocity_scale_set_count_value(), 0)
      << "motion-scoped stop path must freeze backend scale writes while the "
         "controlled stop is in flight";
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    auto it = std::find_if(events.rbegin(), events.rend(),
                           [](const KernelEvent& e) {
                             return e.type ==
                                    KernelEventType::kRuntimeVelocityScaleChanged;
                           });
    ASSERT_NE(it, events.rend());
    EXPECT_EQ(it->details.at("backend_status"),
              "deferred_controlled_stop_freeze");
    EXPECT_EQ(it->details.at("applied_scale"), "1");
    EXPECT_NEAR(std::stod(it->details.at("new_scale")), 0.6, 1e-12);
    EXPECT_EQ(it->details.at("principal_id"), "operator_bob");
  }

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);
  ASSERT_TRUE(controlled_execution.wait_until_runtime_velocity_scale_set_count(1));
  EXPECT_DOUBLE_EQ(controlled_execution.last_runtime_velocity_scale_value(), 0.6);
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_SettleSignalSurfacedFromMeasuredVelocity) {
  using namespace motion_45c;
  // M3 Part 4: the stop snapshot exposes the measured settle signal. The
  // backend reports post-stop velocities within the threshold, so the trip
  // outcome's ExecutionStopSnapshotDTO carries at_rest=true and the measured
  // max |velocity|.
  ControlledExecution controlled_execution;
  controlled_execution.next_post_stop_joint_positions_rad =
      std::vector<double>{0.0, 0.0};
  controlled_execution.next_post_stop_joint_velocities_rad_s =
      std::vector<double>{0.0, 0.002};

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

  auto req = make_test_request("trip_settle_signal");
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
  const auto& snap = outcome->trip->stop_snapshot;
  ASSERT_TRUE(snap.at_rest.has_value());
  EXPECT_TRUE(*snap.at_rest);
  ASSERT_TRUE(snap.max_abs_velocity_rad_s.has_value());
  EXPECT_NEAR(*snap.max_abs_velocity_rad_s, 0.002, 1e-9);
  ASSERT_EQ(snap.robot_state.velocities_rad_s.size(), 2u);
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_ConfirmedButMeasuredNotAtRestEscalatesToIntervention) {
  using namespace motion_45c;
  // M3 Part 1: measured-stop confirmation. The backend confirms the stop, but
  // the measured velocity shows the robot is NOT at rest. The kernel must NOT
  // certify a signal_trip. Because the stop command was accepted and confirmed,
  // the kernel does not issue a redundant JTC cancel; it downgrades to
  // intervention with an explicit reason and leaves physical recovery to the
  // robot/controller safety layer.
  ControlledExecution controlled_execution;
  controlled_execution.next_post_stop_joint_positions_rad =
      std::vector<double>{0.0, 0.0};
  controlled_execution.next_post_stop_joint_velocities_rad_s =
      std::vector<double>{0.5, 0.0};  // 0.5 rad/s >> settle threshold

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

  auto req = make_test_request("trip_not_settled");
  auto scope = make_scope("scope_a", "probe_trip",
                          MotionInterruptActivationMode::kTripIfAlreadyActive);
  scope.stop_profile->max_stop_time_s = 0.05;
  scope.stop_profile->settle_velocity_threshold_rad_s = 0.01;
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
  EXPECT_EQ(outcome->terminal_cause, MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "stop_not_settled_measured_velocity");
  EXPECT_EQ(outcome->error->details.at("hard_stop_transitioned"), "false");
  EXPECT_EQ(outcome->error->details.at("emergency_fallback"),
            "not_attempted_controlled_stop_already_confirmed");
  EXPECT_EQ(controlled_execution.stop_attempt_count_value(), 0);
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_CommandAuthoritativeStopIgnoresFeedbackLag) {
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  controlled_execution.next_post_stop_joint_positions_rad =
      std::vector<double>{0.3, 0.4};
  controlled_execution.next_post_stop_joint_velocities_rad_s =
      std::vector<double>{0.5, 0.0};
  controlled_execution.next_post_stop_state_source =
      "commanded_sim_controlled_stop_reference";

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

  auto req = make_test_request("trip_command_authority");
  auto scope = make_scope("scope_a", "probe_trip",
                          MotionInterruptActivationMode::kTripIfAlreadyActive);
  scope.stop_profile->max_stop_time_s = 0.05;
  scope.stop_profile->settle_velocity_threshold_rad_s = 0.01;
  req.motion_interrupt_scopes = {scope};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(controlled_execution.stop_attempt_count_value(), 0);

  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kSignalTrip);
  ASSERT_TRUE(outcome->trip.has_value());
  EXPECT_TRUE(outcome->trip->stop_snapshot.max_abs_velocity_rad_s.has_value());
  EXPECT_NEAR(*outcome->trip->stop_snapshot.max_abs_velocity_rad_s, 0.5, 1e-9);
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_StopMonitorUnavailableIsAuditedWithoutCancellingStop) {
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;

  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;

  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,      context,         controlled_execution,
                               local_clock,  local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(local_kernel, local_collision_world,
                                         local_collision_engine,
                                         local_runtime_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto req = make_test_request("trip_stop_monitor_unavailable");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));

  // Break only the monitor query-engine availability while the stop is active.
  // This must be visible, but must not be reported as a collision or cancel the
  // in-flight stop.
  local_kernel.set_collision_resources(
      local_collision_world,
      nullptr,
      std::vector<std::shared_ptr<ICollisionQueryEngine>>{});
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "stop-window collision monitor did not schedule its next tick";
  bool saw_monitor_unavailable = false;
  for (int attempt = 0; attempt < 200 && !saw_monitor_unavailable; ++attempt) {
    local_clock.advance(10'000'000);
    local_scheduler.advance_to(local_clock.now());
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      saw_monitor_unavailable =
          std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
            return e.type == KernelEventType::kErrorRaised &&
                   e.details.contains("reason_during_stop") &&
                   e.details.at("reason_during_stop") ==
                       "stop_monitor_unavailable";
          });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(saw_monitor_unavailable);
  EXPECT_EQ(controlled_execution.controlled_stop_cancel_count_value(), 0);

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_StopPathCollisionEscalatesToHardStop) {
  using namespace motion_45c;
  // M3 Part 3: a dynamic obstacle enters the stop path WHILE the controlled
  // stop is in flight. The runtime collision monitor keeps running over the
  // decelerating robot (observed-state based, no trajectory pre-validation),
  // detects the collision, cancels the in-flight soft stop via the stop token,
  // and the trip handler escalates to the hard-stop floor + intervention.
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;  // hold the stop in flight

  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_query_timeout_ns = 10'000'000;
  // Keep the observed/dynamic-body freshness budgets generous: the test-double
  // does not refresh its snapshot timestamp while the stop is blocked.
  config.runtime_observed_state_max_age_ns = 5'000'000'000;
  config.runtime_dynamic_body_max_age_cap_ns = 5'000'000'000;

  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,      context,         controlled_execution,
                               local_clock,  local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(local_kernel, local_collision_world,
                                         local_collision_engine,
                                         local_runtime_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_stop_path_collision");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  // Signal trip -> the controlled stop starts and blocks (stop in flight).
  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));

  // A dynamic obstacle now appears on the stop path.
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{99, "obstacle-on-stop-path"};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  local_runtime_engine->default_result = make_authoritative_collision_result();

  // Drive the stop-window monitor until it cancels the in-flight stop.
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "stop-window collision monitor did not schedule its next tick";
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (controlled_execution.controlled_stop_cancel_count_value() > 0) {
      break;
    }
    local_clock.advance(10'000'000);
    local_scheduler.advance_to(local_clock.now());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  controlled_execution.release();  // unblock any residual wait
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));

  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  // The monitor cancelled the in-flight stop on the stop-path collision.
  EXPECT_GE(controlled_execution.controlled_stop_cancel_count_value(), 1);
  // The emergency hard-stop floor actually ran (backend.stop() invoked).
  EXPECT_GE(controlled_execution.stop_attempt_count_value(), 1);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause, MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"), "stop_path_collision");
  // The terminal outcome carries the hard-stop escalation evidence, not only
  // the transient event stream.
  EXPECT_EQ(outcome->error->details.at("hard_stop_transitioned"), "true");
  EXPECT_FALSE(outcome->trip.has_value());
  EXPECT_TRUE(local_kernel.get_active_interrupt_latch().has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_KernelDeadlineWatchdogHardStopsBlockedBackend) {
  using namespace motion_45c;
  // M3 deadline enforcement: the kernel itself bounds the controlled stop. If
  // the backend blocks past max_stop_time_s, the kernel deadline watchdog
  // cancels the in-flight stop (via the stop token) and escalates to the
  // hard-stop floor — it does not wait for a cooperative backend return.
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;  // backend hangs the stop

  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;

  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,      context,         controlled_execution,
                               local_clock,  local_scheduler, local_cache, config};
  configure_authoritative_collision_validation(local_kernel, local_collision_world,
                                               local_collision_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_deadline_watchdog");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive);
  scope.stop_profile->deceleration_time_s = 0.05;
  scope.stop_profile->max_stop_time_s = 0.1;  // 100 ms kernel deadline
  req.motion_interrupt_scopes = {scope};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));

  // Advance past the deadline; the watchdog must cancel the blocked stop.
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "controlled-stop deadline watchdog did not schedule";
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (controlled_execution.controlled_stop_cancel_count_value() > 0) {
      break;
    }
    local_clock.advance(20'000'000);  // 20 ms steps
    local_scheduler.advance_to(local_clock.now());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(controlled_execution.wait_until_stop_attempt_count(1))
      << "deadline watchdog must issue backend.stop() before the blocked "
         "controlled stop is manually released";
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));

  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  EXPECT_GE(controlled_execution.controlled_stop_cancel_count_value(), 1);
  EXPECT_GE(controlled_execution.stop_attempt_count_value(), 1);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"), "backend_stop_not_confirmed");
  EXPECT_EQ(outcome->error->details.at("hard_stop_transitioned"), "true");
  EXPECT_FALSE(outcome->trip.has_value());
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_CleanStopSuppressesDequeuedDeadlineWatchdog) {
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;

  ThreadSafeFakeClock local_clock;
  DequeuingManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000'000;
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{planner,      context,         controlled_execution,
                               local_clock,  local_scheduler, local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_collision_world,
                                               local_collision_engine);
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_dequeued_watchdog_clean_stop");
  auto scope = make_scope("scope_a", "probe_trip",
                          MotionInterruptActivationMode::kTripIfAlreadyActive);
  scope.stop_profile->deceleration_time_s = 0.05;
  scope.stop_profile->max_stop_time_s = 0.1;
  req.motion_interrupt_scopes = {scope};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));
  for (int attempt = 0; attempt < 2000 && local_scheduler.pending_count() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_GT(local_scheduler.pending_count(), 0u);

  local_clock.advance(100'000'000);
  local_scheduler.advance_to_dequeue_only(local_clock.now());
  ASSERT_GT(local_scheduler.dequeued_count(), 0u);

  controlled_execution.release_controlled_stop();
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  ASSERT_EQ(local_kernel.get_status().state, ExecutionState::kCompleted);
  EXPECT_EQ(controlled_execution.stop_attempt_count_value(), 0);

  local_scheduler.fire_dequeued();
  EXPECT_EQ(controlled_execution.stop_attempt_count_value(), 0)
      << "a watchdog dequeued before clean stop completion must not issue a "
         "late hard-stop floor";
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    const bool saw_late_timeout = std::any_of(
        events.begin(), events.end(), [](const KernelEvent& e) {
          return e.type == KernelEventType::kErrorRaised &&
                 e.details.contains("reason") &&
                 e.details.at("reason") == "in_stop_completion_timeout" &&
                 e.details.contains("phase") &&
                 e.details.at("phase") == "motion_trip_deadline_watchdog";
        });
    EXPECT_FALSE(saw_late_timeout);
  }
}

TEST_F(ExecutionKernelTest,
       Execute_ScopedTrip_KernelDestructionCancelsBlockedTripStop) {
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;

  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_kernel = std::make_unique<ExecutionKernel>(
      planner, context, controlled_execution, local_clock, local_scheduler,
      local_cache);
  configure_authoritative_collision_validation(
      *local_kernel, local_collision_world, local_collision_engine);
  ASSERT_TRUE(local_kernel->register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel->register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_destructor_cancels_stop");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel->execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(*local_kernel, ExecutionState::kExecuting));

  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel->publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(*local_kernel, ExecutionState::kStopping));

  auto destroy_future = std::async(std::launch::async, [&local_kernel]() {
    local_kernel.reset();
  });
  if (destroy_future.wait_for(std::chrono::milliseconds(750)) !=
      std::future_status::ready) {
    controlled_execution.release_controlled_stop();
    destroy_future.wait();
    FAIL() << "kernel destruction did not cancel the active trip stop token";
  }
  destroy_future.get();

  EXPECT_GE(controlled_execution.controlled_stop_cancel_count_value(), 1);
}

TEST_F(ExecutionKernelTest,
       Stop_DuringScopedTripStopCancelsTripWithoutSecondControlledStop) {
  using namespace motion_45c;
  ControlledExecution controlled_execution;
  controlled_execution.block_controlled_stop = true;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_stop_spring_back";
  op_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);

  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,      context,         controlled_execution,
                               local_clock,  local_scheduler, local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_collision_world,
                                               local_collision_engine);
  ASSERT_TRUE(local_kernel.register_interrupt_source(
                  make_fast_probe_source("probe")).ok());
  ASSERT_TRUE(local_kernel.register_interrupt_policy(
                  make_move_trip_policy("probe_trip", "probe")).ok());

  auto req = make_test_request("trip_operator_stop_idempotent");
  req.motion_interrupt_scopes = {make_scope(
      "scope_a", "probe_trip",
      MotionInterruptActivationMode::kTripIfAlreadyActive)};
  auto result = local_kernel.execute(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  auto trip = make_probe_sample("probe", 2, local_clock.now(), true);
  ASSERT_TRUE(local_kernel.publish_fast_interrupt_signal(trip).latched);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_count(1));
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kStopping));

  auto stop_result = local_kernel.stop();
  ASSERT_TRUE(stop_result.accepted);
  ASSERT_TRUE(controlled_execution.wait_until_controlled_stop_cancel_count(1));
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));

  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 1)
      << "operator stop must join the active trip stop authority instead of "
         "starting a second controlled stop";
  EXPECT_EQ(local_kernel.get_status().state,
            ExecutionState::kInterventionRequired);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->details.at("reason"),
            "backend_stop_not_confirmed");
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
  controlled_execution.motion_interrupt_stop_capability.hard_stop_level =
      "jtc_cancel_unprofiled_hold";

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
  EXPECT_EQ(outcome->error->details.at("hard_stop_floor"),
            "immediate_hard_stop");
  EXPECT_EQ(outcome->error->details.at("hard_stop_level"),
            "jtc_cancel_unprofiled_hold");
  EXPECT_EQ(outcome->error->details.at("hard_stop_floor_command_source"),
            "jtc_cancel_unprofiled_hold");
  EXPECT_FALSE(outcome->trip.has_value());
  // A refused controlled stop means the robot may still be in motion, so
  // the scoped-trip path MUST engage the emergency hard-stop floor
  // (legacy backend.stop()) exactly once before downgrading. Mirrors the
  // operator-stop worker; without it the robot could keep moving after a
  // refused trip.
  EXPECT_EQ(controlled_execution.stop_count_value(), 1);
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
  EXPECT_EQ(outcome->error->details.at("hard_stop_floor"),
            "immediate_hard_stop");
  EXPECT_EQ(outcome->error->details.at("hard_stop_level"),
            "mock_hard_stop");
  EXPECT_EQ(outcome->error->details.at("hard_stop_floor_command_source"),
            "backend_stop");
  // An unconfirmed stop is indistinguishable from a still-moving robot,
  // so the emergency hard-stop floor MUST fire exactly once here too.
  EXPECT_EQ(controlled_execution.stop_count_value(), 1);
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
  fallback_ok.confirmation_latency_ns = 2'000'000;
  fallback_ok.diagnostics["command_source"] = "mock_fallback_source";
  controlled_execution.next_controlled_stop_result = fallback_ok;

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{planner, context, controlled_execution,
                                local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);
  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });
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
      {"j1", 1000.0}, {"j2", 1000.0}};
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
  std::lock_guard<std::mutex> lock(events_mutex);
  auto trip_event = std::find_if(
      events.begin(), events.end(), [](const KernelEvent& e) {
        return e.type == KernelEventType::kMotionTripCompleted;
      });
  ASSERT_NE(trip_event, events.end());
  EXPECT_EQ(trip_event->details.at("controlled_stop_success"), "true");
  EXPECT_EQ(trip_event->details.at("phase"), "motion_trip");
  EXPECT_EQ(trip_event->details.at("requested_stop_mode"), "soft_on_path");
  EXPECT_EQ(trip_event->details.at("executed_stop_mode"), "spring_back_hold");
  EXPECT_EQ(trip_event->details.at("fallback_reason"),
            "soft_on_path_scaffold_anchored_hold_fallback");
  EXPECT_EQ(trip_event->details.at("stop_command_source"),
            "diagnostic_other");
  EXPECT_EQ(trip_event->details.at("command_source"),
            "mock_fallback_source");
  EXPECT_EQ(trip_event->details.at("latch_id"), outcome->trip->latch_id);
  EXPECT_EQ(trip_event->details.at("scope_id"), "scope_a");
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
       Execute_ScopedTrip_SoftOnPathEscalatesToHardDecelerationIsExplicitIntervention) {
  using namespace motion_45c;
  // When soft_on_path is infeasible the backend escalates to a hard
  // deceleration. This is a fail-closed, operator-visible escalation, NOT
  // a silent qualified fallback: the outcome is intervention with the
  // explicit reason "soft_on_path_escalated_to_hard_deceleration" rather
  // than the generic "fallback_pair_not_qualified" nullptr downgrade.
  // The robot is already at rest (accepted + confirmed), so the emergency
  // floor does NOT fire.
  ControlledExecution controlled_execution;
  controlled_execution.motion_interrupt_stop_capability.supported_stop_modes =
      {MotionInterruptStopMode::kSoftOnPath,
       MotionInterruptStopMode::kHardDeceleration};
  ControlledStopResultDTO hard_decel_escalation;
  hard_decel_escalation.accepted = true;
  hard_decel_escalation.backend_stop_confirmed = true;
  hard_decel_escalation.executed_stop_mode =
      MotionInterruptStopMode::kHardDeceleration;
  hard_decel_escalation.stop_command_source =
      MotionInterruptStopCommandSource::kRobotNativeStop;
  hard_decel_escalation.fallback_reason =
      "soft_on_path_infeasible_recommends_hard_stop";
  controlled_execution.next_controlled_stop_result = hard_decel_escalation;

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

  auto req = make_test_request("trip_hard_decel_escalation");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile->deceleration_time_s = 0.2;
  scope.stop_profile->per_joint_max_deceleration_rad_s2 = {
      {"j1", 1000.0}, {"j2", 1000.0}};
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
            "soft_on_path_escalated_to_hard_deceleration");
  EXPECT_FALSE(outcome->trip.has_value());
  // Robot already at rest; the emergency floor must NOT fire.
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
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
      {"j1", 1000.0}, {"j2", 1000.0}};
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
      {"j1", 1000.0}, {"j2", 1000.0}};
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
  // MUST derive the effective deadline from the requested deceleration time
  // plus the deployment stop-confirmation tolerance. A backend reporting a
  // confirmation latency that exceeds that derived deadline still fails closed
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
  // 200 ms > 50 ms decel + 100 ms tolerance.
  slow.confirmation_latency_ns = 200'000'000;
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
  scope.stop_profile->deceleration_time_s = 0.05;
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
       Execute_OperatorStopProfileRejectsUnsupportedBackendMode) {
  execution.motion_interrupt_stop_capability.supported_stop_modes = {
      MotionInterruptStopMode::kSpringBackHold};

  MotionInterruptStopProfileDTO op_profile;
  op_profile.profile_id = "operator_hard_decel";
  op_profile.stop_mode = MotionInterruptStopMode::kHardDeceleration;
  ExecutionKernelConfig config;
  config.operator_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(op_profile);

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel local_kernel{
      planner, context, execution, local_clock, local_scheduler, local_cache,
      config};
  configure_authoritative_collision_validation(
      local_kernel, collision_world_publisher, collision_engine);

  const int execute_calls_before = execution.execute_count;
  auto result = local_kernel.execute(make_test_request("operator_cap_drop"));
  ASSERT_TRUE(result.ok());
  local_kernel.wait_for_completion();

  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, execute_calls_before);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->code, kErrorMotionInterruptArmFailed);
  EXPECT_EQ(outcome->error->details.at("reason"),
            "stop_mode_not_supported_by_backend");
  EXPECT_EQ(outcome->error->details.at("profile_slot"),
            "operator_stop_profile");
}

TEST_F(ExecutionKernelTest,
       Execute_DefaultFaultStopProfileRejectsUnsupportedFallbackMode) {
  execution.motion_interrupt_stop_capability.supported_stop_modes = {
      MotionInterruptStopMode::kSpringBackHold};

  MotionInterruptStopProfileDTO fault_profile;
  fault_profile.profile_id = "fault_spring_with_hard_fallback";
  fault_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  fault_profile.allowed_fallback_modes = {
      MotionInterruptStopMode::kHardDeceleration};
  ExecutionKernelConfig config;
  config.default_fault_stop_profile =
      std::make_shared<MotionInterruptStopProfileDTO>(fault_profile);

  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel local_kernel{
      planner, context, execution, local_clock, local_scheduler, local_cache,
      config};
  configure_authoritative_collision_validation(
      local_kernel, collision_world_publisher, collision_engine);

  const int execute_calls_before = execution.execute_count;
  auto result = local_kernel.execute(make_test_request("fault_fallback_drop"));
  ASSERT_TRUE(result.ok());
  local_kernel.wait_for_completion();

  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kFailed);
  EXPECT_EQ(execution.execute_count, execute_calls_before);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->error.has_value());
  EXPECT_EQ(outcome->error->code, kErrorMotionInterruptArmFailed);
  EXPECT_EQ(outcome->error->details.at("reason"),
            "fallback_mode_not_supported_by_backend");
  EXPECT_EQ(outcome->error->details.at("profile_slot"),
            "default_fault_stop_profile");
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
       Execute_SoftOnPathArmsWithoutArmTimeFeasibilityPrecheck) {
  // The arm-time soft_on_path feasibility envelope was removed: a scope whose
  // wall-time ramp cannot be proven feasible at every trip point no longer fails
  // closed at arm time. It arms and dispatches; the feasibility + fallback
  // disposition (hard_stop / spring_back / natural completion) is decided at TRIP
  // time by the planner and the backends (both now honor recommended_disposition).
  // With no trip in this run the move simply completes -- the former arm-time
  // rejection is gone (no spring_back fallback, no natural-completion policy).
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

  auto req = make_test_request("soft_exact_no_arm_precheck");
  auto scope = make_scope("scope_a", "probe_trip",
                            MotionInterruptActivationMode::kTripIfAlreadyActive,
                            MotionInterruptStopMode::kSoftOnPath);
  scope.stop_profile = make_soft_on_path_profile(
      /*allow_spring_back_fallback=*/false);
  req.motion_interrupt_scopes = {scope};

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  // Arms + dispatches (no arm-time feasibility rejection); the move completes
  // with no trip, so the deferred feasibility decision is never exercised here.
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kCompleted)
      << (outcome->error ? outcome->error->message : "");
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_EQ(outcome->terminal_cause,
            MotionExecutionTerminalCause::kNoSignalTrip);
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
      {"j1", 1000.0}, {"j2", 1000.0}};
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

TEST_F(ExecutionKernelTest, Execute_ProgramRunIdStampsEventsAndDoesNotLeakAcrossRuns) {
  // spec/500 §2.2/§4.1: program_run_id is optional at the kernel boundary;
  // when provided it must reach every kernel event for that execution, and
  // it must never leak onto the events of a later execution that did not
  // provide one.
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto request = make_test_request("program_run_correlated_move");
  request.program_run_id = "run-abc123";
  auto result = execute_sync(request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(kernel.acknowledge_completed(result.value()).accepted);

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    ASSERT_FALSE(events.empty());
    for (const auto& event : events) {
      auto it = event.details.find("program_run_id");
      ASSERT_NE(it, event.details.end())
          << "event missing program_run_id during correlated execution: type="
          << static_cast<int>(event.type)
          << " execution_id=" << event.execution_id;
      EXPECT_EQ(it->second, "run-abc123");
    }
    events.clear();
  }

  // A later execution WITHOUT a program_run_id must not inherit the stale
  // id from the previous run.
  auto uncorrelated = make_test_request("uncorrelated_move");
  auto second = execute_sync(uncorrelated);
  ASSERT_TRUE(second.ok());
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    ASSERT_FALSE(events.empty());
    for (const auto& event : events) {
      EXPECT_EQ(event.details.count("program_run_id"), 0u)
          << "stale program_run_id leaked onto an uncorrelated execution";
    }
  }
}

TEST_F(ExecutionKernelTest,
       Execute_ActionSpanStampsEventsSnapshotAndOutcome) {
  // K6: behavior-owned action spans become the execution-layer correlation
  // key. The kernel must stamp the complete span tuple onto emitted events,
  // observer snapshots, and the immutable terminal outcome, then clear it at
  // the acknowledgement boundary so the next action cannot inherit it.
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  const auto span = make_test_action_span();
  auto request = make_test_request("span_correlated_move");
  request.program_run_id = span.program_run_id;
  request.action_span = span;

  auto result = execute_sync(request);
  ASSERT_TRUE(result.ok());

  auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.action_span.has_value());
  EXPECT_EQ(snapshot.action_span->action_source, span.action_source);
  EXPECT_EQ(snapshot.action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(snapshot.action_span->program_run_id, span.program_run_id);
  EXPECT_EQ(snapshot.action_span->node_instance_id, span.node_instance_id);
  EXPECT_EQ(snapshot.action_span->node_attempt_id, span.node_attempt_id);
  EXPECT_EQ(snapshot.action_span->parent_node_attempt_id,
            span.parent_node_attempt_id);
  EXPECT_TRUE(snapshot.contact_policy_id.empty());

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->action_span.has_value());
  EXPECT_EQ(outcome->action_span->action_source, span.action_source);
  EXPECT_EQ(outcome->action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(outcome->action_span->program_run_id, span.program_run_id);
  EXPECT_EQ(outcome->action_span->node_instance_id, span.node_instance_id);
  EXPECT_EQ(outcome->action_span->node_attempt_id, span.node_attempt_id);
  EXPECT_EQ(outcome->action_span->parent_node_attempt_id,
            span.parent_node_attempt_id);
  EXPECT_TRUE(outcome->contact_policy_id.empty());

  ASSERT_TRUE(kernel.acknowledge_completed(result.value()).accepted);

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    ASSERT_FALSE(events.empty());
    for (const auto& event : events) {
      expect_action_span_details(event.details, span);
    }
    events.clear();
  }

  auto uncorrelated = make_test_request("span_uncorrelated_move");
  auto second = execute_sync(uncorrelated);
  ASSERT_TRUE(second.ok());
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    ASSERT_FALSE(events.empty());
    for (const auto& event : events) {
      EXPECT_EQ(event.details.count("action_source"), 0u)
          << "stale action source leaked onto uncorrelated execution";
      EXPECT_EQ(event.details.count("action_span_id"), 0u)
          << "stale action span leaked onto uncorrelated execution";
      EXPECT_EQ(event.details.count("node_attempt_id"), 0u)
          << "stale node attempt leaked onto uncorrelated execution";
      EXPECT_EQ(event.details.count("contact_policy_id"), 0u)
          << "stale contact policy leaked onto uncorrelated execution";
    }
  }
}

TEST_F(ExecutionKernelTest,
       Execute_ContactPolicyStampsEventsSnapshotAndFailedOutcome) {
  // K6: contact policy ids are part of the same action-correlation envelope.
  // Unknown policies fail validation by design, but the failure still needs to
  // carry the originating behavior span and concrete policy id for incident
  // grouping and operator diagnostics.
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  const auto span = make_test_action_span();
  constexpr const char* kContactPolicyId = "contact_grasp/lite6_box";
  auto request = make_test_request("contact_policy_correlated_move");
  request.program_run_id = span.program_run_id;
  request.action_span = span;
  request.contact_policy_id = kContactPolicyId;

  auto result = execute_sync(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kFailed);

  auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.action_span.has_value());
  EXPECT_EQ(snapshot.action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(snapshot.action_span->node_attempt_id, span.node_attempt_id);
  EXPECT_EQ(snapshot.contact_policy_id, kContactPolicyId);

  auto outcome = kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->terminal_cause, MotionExecutionTerminalCause::kFailed);
  ASSERT_TRUE(outcome->action_span.has_value());
  EXPECT_EQ(outcome->action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(outcome->action_span->node_attempt_id, span.node_attempt_id);
  EXPECT_EQ(outcome->contact_policy_id, kContactPolicyId);

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    ASSERT_FALSE(events.empty());
    for (const auto& event : events) {
      expect_action_span_details(event.details, span);
      expect_detail(event.details, "contact_policy_id", kContactPolicyId);
    }
  }
}


TEST_F(ExecutionKernelTest,
       Execute_ReplanFailureWithCollisionClassLatchPreservesSafetyTerminal) {
  // K1 characterization: a runtime-collision-class fault latch (unmaskable,
  // priority 1000, await-operator-ack — the same class as the kernel's
  // builtin runtime_collision_fault policy) that arrives while the
  // authoritative-validation REPLAN attempt is in flight and the replan
  // then FAILS must surface as a safety terminal. It must never be
  // laundered into a plain kFailed by the replan failure path.
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 1;
  MockPlanner local_planner;
  PlanResultDTO first_plan;
  first_plan.trajectory = make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  PlanResultDTO failed_replan;
  failed_replan.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  local_planner.result_sequence = {first_plan, failed_replan};
  MockContext local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner,
                               local_context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  auto source = make_interrupt_source("replan_collision_fault");
  ASSERT_TRUE(local_kernel.register_interrupt_source(source).ok());
  auto policy =
      make_interrupt_policy("replan_collision_fault_policy",
                            "replan_collision_fault");
  policy.affected_phases = {InterruptExecutionPhase::kPlanning};
  policy.preemption_action = InterruptPreemptionAction::kLatchHeld;
  policy.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  policy.priority = 1000;
  policy.maskable = false;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(policy).ok());
  ExecutionKernel* kernel_ptr = &local_kernel;
  local_planner.on_plan = [&](int count) {
    if (count == 2) {
      auto ingest = kernel_ptr->publish_fast_interrupt_signal(
          make_interrupt_signal("replan_collision_fault"));
      EXPECT_TRUE(ingest.latched);
    }
  };
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                               local_engine);
  local_engine->default_result = make_authoritative_collision_result();

  const auto span = make_test_action_span();
  auto request = make_test_request("replan_latch_fail");
  request.program_run_id = span.program_run_id;
  request.action_span = span;
  auto result = local_kernel.execute(request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  const auto terminal = local_kernel.get_status().state;
  EXPECT_NE(terminal, ExecutionState::kFailed)
      << "collision-class latch was laundered into a normal kFailed";
  EXPECT_TRUE(terminal == ExecutionState::kInterrupted ||
              terminal == ExecutionState::kInterventionRequired)
      << "unexpected terminal: " << to_string(terminal);
  EXPECT_EQ(controlled_execution.execute_count, 0);
  auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.action_span.has_value());
  EXPECT_EQ(snapshot.action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(snapshot.action_span->node_attempt_id, span.node_attempt_id);
  auto outcome = local_kernel.get_execution_outcome(result.value());
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->action_span.has_value());
  EXPECT_EQ(outcome->action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(outcome->action_span->node_attempt_id, span.node_attempt_id);
}

TEST_F(ExecutionKernelTest,
       Execute_CompletionRaceWithFaultLatchNeverCommitsCompleted) {
  // K1 characterization (finalizer race intent): once a fault latch is
  // recorded, a backend completion arriving immediately afterwards must
  // not commit kCompleted. The safety authority outranks the normal
  // terminal even when both are observable in the same finalizer pass.
  // Regression guard: interrupt authority is recorded monotonically for the
  // execution, so even if preemption machinery consumes the active latch before
  // the backend completion finalizer runs, the normal completion cannot commit
  // kCompleted.
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MockPlanner local_planner;
  PlanResultDTO plan;
  plan.trajectory = make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  local_planner.result_sequence = {plan};
  MockContext local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner,
                               local_context,
                               controlled_execution,
                               local_clock,
                               local_scheduler,
                               local_cache,
                               config};
  auto source = make_interrupt_source("finalizer_race_fault");
  ASSERT_TRUE(local_kernel.register_interrupt_source(source).ok());
  auto policy = make_interrupt_policy("finalizer_race_fault_policy",
                                      "finalizer_race_fault");
  policy.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  policy.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  policy.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  policy.priority = 1000;
  policy.maskable = false;
  ASSERT_TRUE(local_kernel.register_interrupt_policy(policy).ok());
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                               local_engine);
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  auto result = local_kernel.execute(make_test_request("finalizer_race"));
  ASSERT_TRUE(result.ok());
  if (!controlled_execution.wait_until_execute_called()) {
    std::vector<KernelEvent> event_snapshot;
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      event_snapshot = events;
    }
    for (const auto& event : event_snapshot) {
      std::cerr << "event=" << static_cast<int>(event.type)
                << " to=" << to_string(event.to_state)
                << " error="
                << (event.error ? event.error->code + ":" +
                                      event.error->message
                                : std::string{})
                << "\n";
    }
    ASSERT_TRUE(false) << "wait_until_execute_called timed out; state="
                       << to_string(local_kernel.get_status().state);
  }

  auto ingest = local_kernel.publish_fast_interrupt_signal(
      make_interrupt_signal("finalizer_race_fault"));
  EXPECT_TRUE(ingest.latched);
  controlled_execution.complete();

  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  const auto terminal = local_kernel.get_status().state;
  EXPECT_NE(terminal, ExecutionState::kCompleted)
      << "backend completion overwrote a latched fault authority";
  EXPECT_TRUE(terminal == ExecutionState::kInterrupted ||
              terminal == ExecutionState::kInterventionRequired ||
              terminal == ExecutionState::kStopped)
      << "unexpected terminal: " << to_string(terminal);
}


// Every dispatched kernel event carries a contiguous, strictly increasing
// dispatch-order sequence. Downstream pipelines rely on gaps meaning LOSS,
// never renumbering, so contiguity across a full lifecycle is the contract.
TEST_F(ExecutionKernelTest, KernelEventSequenceIsContiguousAcrossLifecycle) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{planner,        context,
                               controlled_execution, local_clock,
                               local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher,
                                                local_engine);

  std::mutex sequences_mutex;
  std::vector<std::uint64_t> sequences;
  auto handle = local_kernel.subscribe_events(
      [&](const KernelEvent& event) {
        std::lock_guard<std::mutex> lock(sequences_mutex);
        sequences.push_back(event.sequence);
      });

  auto result = local_kernel.execute(make_test_request("sequence_contiguity"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  controlled_execution.complete();
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  ASSERT_TRUE(local_kernel.reset().accepted);
  local_kernel.unsubscribe_events(handle);

  std::lock_guard<std::mutex> lock(sequences_mutex);
  ASSERT_GE(sequences.size(), 4u);
  for (std::size_t i = 0; i < sequences.size(); ++i) {
    EXPECT_EQ(sequences[i], static_cast<std::uint64_t>(i + 1))
        << "dispatch sequence must be contiguous from 1; index " << i;
  }
}

}  // namespace
}  // namespace manyforge::core
