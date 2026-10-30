#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <manyforge_core/cache/trajectory_cache_intent.hpp>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>
#include <manyforge_core/testing/fake_execution_port.hpp>
#include <manyforge_core/trajectory/trajectory_splice.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/trajectory/validation_sampling.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/controller_lifecycle.hpp>
#include <manyforge_core/types/reason_codes.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if defined(__unix__)
#include <unistd.h>
#endif

namespace manyforge::core {
namespace {

constexpr auto kTestDigestA =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto kTestDigestB =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr auto kTestDigestC =
    "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr auto kTestDigestD =
    "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

std::string test_digest(std::string_view label) {
  // Deterministic digest-shaped content identity for fixture revisions; the
  // prepared motion product admits only digest-shaped world/policy hashes.
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx",
                static_cast<unsigned long long>(std::hash<std::string_view>{}(label)));
  return "sha256:" + std::string(hex) + hex + hex + hex;
}

ProjectionRevisionDTO test_projection_revision() {
  return ProjectionRevisionDTO{
      RetainedBundleId{kTestDigestA}, GraphRevision{kTestDigestB},
      SceneRevision{SceneId{kTestDigestC}, 1U, ContentDigest{kTestDigestD}}};
}

// --- Mock backends ---

class MockPlanner : public IPlannerBackend {
 public:
  PlanResultDTO next_result;
  std::vector<PlanResultDTO> result_sequence;
  std::function<void(int)> on_plan;
  int plan_count = 0;
  int cancel_count = 0;
  PlanRequestDTO last_request = testing::make_test_plan_request(
      "unplanned",
      ResolvedJointGoalDTO{
          {ResourceId("joint.1"), ResourceId("joint.2")}, {"j1", "j2"}, {0.0, 0.0}},
      {"j1", "j2"}, {0.0, 0.0});
  std::vector<std::uint32_t> planning_attempt_indices;

  PlanResultDTO plan(const PlanRequestDTO& request) override {
    ++plan_count;
    last_request = request;
    planning_attempt_indices.push_back(request.planning_attempt_index);
    if (on_plan) {
      on_plan(plan_count);
    }
    if (!result_sequence.empty()) {
      auto result = result_sequence.front();
      result_sequence.erase(result_sequence.begin());
      if (result.trajectory.has_value() && !result.projection_revision.valid()) {
        result.projection_revision = request.projection_binding.projection_revision;
      }
      if (result.trajectory.has_value() && result.trajectory->resources.empty()) {
        result.trajectory->resources = request.resources;
      }
      return result;
    }
    auto result = next_result;
    if (result.trajectory.has_value() && !result.projection_revision.valid()) {
      result.projection_revision = request.projection_binding.projection_revision;
    }
    if (result.trajectory.has_value() && result.trajectory->resources.empty()) {
      result.trajectory->resources = request.resources;
    }
    return result;
  }

  void request_cancel() override { ++cancel_count; }
};

class SlowMockPlanner : public IPlannerBackend {
 public:
  PlanResultDTO next_result;
  int plan_count = 0;
  int cancel_count = 0;
  PlanRequestDTO last_request = testing::make_test_plan_request(
      "unplanned",
      ResolvedJointGoalDTO{
          {ResourceId("joint.1"), ResourceId("joint.2")}, {"j1", "j2"}, {0.0, 0.0}},
      {"j1", "j2"}, {0.0, 0.0});

  SlowMockPlanner() {
    next_result.trajectory = TrajectoryDTO{};
    next_result.trajectory->resources = {ResourceId("joint.1"), ResourceId("joint.2")};
    next_result.trajectory->joint_names = {"j1", "j2"};
    next_result.trajectory->waypoints.push_back(TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {}, {}});
    next_result.trajectory->waypoints.push_back(TrajectoryWaypointDTO{1.0, {1.0, 1.0}, {}, {}});
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
        cancelled.error = ErrorDTO{kErrorPlanningFailed,
                                   "planning cancelled by interrupt",
                                   {{"reason", "planning_cancelled_by_interrupt"}}};
        return cancelled;
      }
    }

    auto result = next_result;
    if (result.trajectory.has_value() && !result.projection_revision.valid()) {
      result.projection_revision = request.projection_binding.projection_revision;
    }
    if (result.trajectory.has_value() && result.trajectory->resources.empty()) {
      result.trajectory->resources = request.resources;
    }
    return result;
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
  bool wait_until_in_plan(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return in_plan_cv_.wait_for(lock, timeout, [this]() { return in_plan_; });
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

class MockFocusedPorts final : public IObservedRobotStateProvider,
                               public IRobotKinematics,
                               public IReferencePoseResolver,
                               public ITrajectoryValidator,
                               public ICollisionTrajectoryProjector,
                               public ICollisionDiagnosticComparator,
                               public IGripperCommandBackend {
 public:
  MockFocusedPorts() = default;

  MockFocusedPorts(const MockFocusedPorts& other) { *this = other; }

  MockFocusedPorts& operator=(const MockFocusedPorts& other) {
    if (this == &other) {
      return *this;
    }
    current_state_value = other.current_state_value;
    addressed_publication_value = other.addressed_publication_value;
    next_validation = other.next_validation;
    next_collision = other.next_collision;
    collision_sequence = other.collision_sequence;
    next_fk = other.next_fk;
    next_reference_pose = other.next_reference_pose;
    reference_pose_sequence = other.reference_pose_sequence;
    fk_sequence = other.fk_sequence;
    next_limits = other.next_limits;
    current_revision = other.current_revision;
    get_state_count = other.get_state_count;
    validate_count = other.validate_count;
    collision_count.store(other.collision_count.load());
    expand_count = other.expand_count;
    fk_count = other.fk_count;
    last_fk_request = other.last_fk_request;
    last_expand_trajectory = other.last_expand_trajectory;
    next_expand_result = other.next_expand_result;
    expand_extra_joint = other.expand_extra_joint;
    last_validation_options = other.last_validation_options;
    throw_get_state = other.throw_get_state;
    throw_fk = other.throw_fk;
    on_validate = other.on_validate;
    return *this;
  }

  RobotStateDTO current_state_value;
  std::optional<ObservedStatePublicationDTO> addressed_publication_value;
  ValidationResultDTO next_validation;
  CollisionCheckResultDTO next_collision;
  mutable std::vector<CollisionCheckResultDTO> collision_sequence;
  Pose3DTO next_fk;
  Result<Pose3DTO> next_reference_pose = Result<Pose3DTO>::Ok(Pose3DTO{});
  mutable std::vector<Result<Pose3DTO>> reference_pose_sequence;
  mutable std::vector<Pose3DTO> fk_sequence;
  JointLimitsDTO next_limits;
  CollisionWorldRevision current_revision{1, test_digest("mock-collision-world")};

  mutable int get_state_count = 0;
  mutable int validate_count = 0;
  // Written from kernel worker threads while tests poll it;
  // atomic so unsynchronized reads stay defined.
  mutable std::atomic<int> collision_count{0};
  mutable int expand_count = 0;
  mutable int fk_count = 0;
  mutable FkRequestDTO last_fk_request;
  mutable TrajectoryDTO last_expand_trajectory;
  std::optional<Result<TrajectoryDTO>> next_expand_result;
  /// Like the RoboPlan projector: append a non-group joint held at 0 to the
  /// positions only, leaving the group's V/A vectors unexpanded.
  std::optional<std::string> expand_extra_joint;
  mutable ValidationOptionsDTO last_validation_options;
  bool throw_get_state = false;
  bool throw_fk = false;
  std::function<void()> on_validate;
  mutable std::mutex gripper_mutex;
  GripperCommandStatusDTO gripper_status;
  GripperCommandRequestDTO last_gripper_request;
  std::atomic<int> gripper_command_count{0};
  std::atomic<int> gripper_mark_lost_count{0};
  std::atomic<int> gripper_invalidation_count{0};
  std::atomic<bool> gripper_admission_blocked{false};
  std::atomic<GripperInvalidationReason> last_gripper_invalidation_reason{
      GripperInvalidationReason::kReset};

  Result<RobotStateDTO> current_state(std::int64_t) const override {
    ++get_state_count;
    if (throw_get_state) {
      return Result<RobotStateDTO>::Err(ErrorDTO{kErrorExecutionRejected,
                                                 "state unavailable",
                                                 {{"reason", "observed_state_unavailable"}}});
    }
    return Result<RobotStateDTO>::Ok(current_state_value);
  }

  const ObservedRobotStatePublication* publication() const noexcept override { return nullptr; }

  Result<ObservedStatePublicationDTO> addressed_publication(
      const ControlBindingId& control_binding_id,
      const ControlBindingRevision& control_binding_revision) const override {
    if (!addressed_publication_value.has_value() ||
        addressed_publication_value->control_binding_id != control_binding_id ||
        addressed_publication_value->control_binding_revision != control_binding_revision) {
      return Result<ObservedStatePublicationDTO>::Err(
          ErrorDTO{kErrorStartStateMismatch,
                   "test addressed publication is unavailable",
                   {{"reason", "addressed_observed_state_unavailable"}}});
    }
    return Result<ObservedStatePublicationDTO>::Ok(*addressed_publication_value);
  }

  Result<ProgramFrameResolutionContextDTO> capture_program_frame_resolution_context(
      ProgramFrameResolutionCaptureDTO capture) const override {
    std::sort(capture.runtime_object_bindings.begin(), capture.runtime_object_bindings.end(),
              [](const auto& left, const auto& right) {
                return left.runtime_object_id < right.runtime_object_id;
              });
    ProgramFrameResolutionContextDTO context{std::move(capture.binding),
                                             std::move(capture.start_state),
                                             std::move(capture.observed_start_publication),
                                             capture.projection_binding,
                                             std::move(capture.logical_scene),
                                             std::move(capture.runtime_object_bindings),
                                             {},
                                             {},
                                             capture.captured_at_ns};
    if (!context.valid()) {
      return Result<ProgramFrameResolutionContextDTO>::Err(
          ErrorDTO{kErrorMissingReference,
                   "test Program frame context is invalid",
                   {{"reason", "test_program_frame_context_invalid"}}});
    }
    return Result<ProgramFrameResolutionContextDTO>::Ok(std::move(context));
  }

  ValidationResultDTO validate_non_collision(const TrajectoryDTO& trajectory,
                                             const ValidationOptionsDTO& options,
                                             const CollisionCancellationToken&) const override {
    ++validate_count;
    last_validation_options = options;
    if (on_validate) {
      on_validate();
    }
    auto result = next_validation;
    if (result.valid && !result.error.has_value() && !result.trajectory_metrics.has_value() &&
        !trajectory.waypoints.empty()) {
      result.trajectory_metrics = TrajectoryValidationMetricsDTO{
          trajectory.waypoints.back().time_from_start_s,
          std::vector<double>(trajectory.joint_names.size(), 0.0),
          std::vector<double>(trajectory.joint_names.size(), 0.0),
          options.max_tcp_linear_speed_mps > 0.0 ? std::optional<double>{0.0} : std::nullopt};
    }
    return result;
  }

  Result<TrajectoryDTO> expand_for_collision(const TrajectoryDTO& trajectory,
                                             const RobotStateDTO&) const override {
    ++expand_count;
    last_expand_trajectory = trajectory;
    if (next_expand_result.has_value()) {
      return *next_expand_result;
    }
    if (expand_extra_joint) {
      auto expanded = trajectory;
      expanded.joint_names.push_back(*expand_extra_joint);
      for (auto& waypoint : expanded.waypoints) waypoint.positions_rad.push_back(0.0);
      return Result<TrajectoryDTO>::Ok(std::move(expanded));
    }
    return Result<TrajectoryDTO>::Ok(trajectory);
  }

  CollisionCheckResultDTO collision_check(const CollisionCheckRequestDTO& /*request*/) const {
    ++collision_count;
    if (!collision_sequence.empty()) {
      auto result = collision_sequence.front();
      collision_sequence.erase(collision_sequence.begin());
      return result;
    }
    return next_collision;
  }

  Result<Pose3DTO> forward_kinematics(const FkRequestDTO& request) const override {
    ++fk_count;
    last_fk_request = request;
    if (throw_fk) {
      return Result<Pose3DTO>::Err(
          ErrorDTO{kErrorInvalidRequest, "fk failed", {{"reason", "fk_failed"}}});
    }
    if (!fk_sequence.empty()) {
      auto result = fk_sequence.front();
      fk_sequence.erase(fk_sequence.begin());
      return Result<Pose3DTO>::Ok(result);
    }
    return Result<Pose3DTO>::Ok(next_fk);
  }

  Result<std::map<std::string, Pose3DTO>> forward_kinematics_batch(
      const RobotStateDTO& robot_state, const std::vector<std::string>& target_frames,
      const std::string& base_frame) const override {
    std::map<std::string, Pose3DTO> poses;
    for (const auto& frame : target_frames) {
      auto pose = forward_kinematics(FkRequestDTO{robot_state, frame, base_frame});
      if (!pose.ok()) {
        return Result<std::map<std::string, Pose3DTO>>::Err(pose.error());
      }
      poses.emplace(frame, std::move(pose.value()));
    }
    return Result<std::map<std::string, Pose3DTO>>::Ok(std::move(poses));
  }

  Result<Pose3DTO> resolve_reference_pose(const ReferenceDTO& /*reference*/,
                                          const ProgramFrameResolutionContextDTO&) const override {
    if (!reference_pose_sequence.empty()) {
      auto result = reference_pose_sequence.front();
      reference_pose_sequence.erase(reference_pose_sequence.begin());
      return result;
    }
    return next_reference_pose;
  }

  Result<JointLimitsDTO> joint_limits(const std::string& /*robot_id*/,
                                      const std::string& /*group_id*/) const override {
    return Result<JointLimitsDTO>::Ok(next_limits);
  }

  Result<std::vector<std::string>> group_joint_names(const std::string&) const override {
    return Result<std::vector<std::string>>::Ok(current_state_value.joint_names);
  }

  CollisionWorldRevision current_collision_world_revision() { return current_revision; }

  CollisionResult compare(const CollisionQuery& query) const override {
    CollisionCheckRequestDTO request;
    request.robot_state = query.robot_state;
    const auto legacy = collision_check(request);
    CollisionResult result;
    result.query_id = query.query_id;
    result.surface = query.surface;
    result.collision_world_revision = current_revision;
    result.in_collision = legacy.in_collision;
    result.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
    result.decision = legacy.in_collision ? CollisionDecisionResult::kInconclusiveFail
                                          : CollisionDecisionResult::kPass;
    result.engine_id = "mock_collision_diagnostic_comparator";
    result.engine_version = "test";
    return result;
  }

  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) override {
    std::lock_guard<std::mutex> lock(gripper_mutex);
    if (gripper_admission_blocked.load(std::memory_order_acquire)) {
      return {false, 0, 0,
              ErrorDTO{"gripper_command_not_authorized", "gripper command admission blocked", {}}};
    }
    ++gripper_command_count;
    last_gripper_request = request;
    gripper_status.state = GripperCommandState::kExecuting;
    gripper_status.gripper_id = request.gripper_id;
    gripper_status.command_id = 41;
    gripper_status.generation = 73;
    ++gripper_status.revision;
    gripper_status.target_position = request.target_position;
    return {true, 41, 73, std::nullopt};
  }

  GripperCommandStatusDTO get_gripper_status(const std::string& gripper_id) const override {
    std::lock_guard<std::mutex> lock(gripper_mutex);
    auto status = gripper_status;
    status.gripper_id = gripper_id;
    return status;
  }

  GripperCommandResultDTO mark_gripper_grasp_lost(const std::string& gripper_id,
                                                  std::uint64_t expected_command_id,
                                                  std::uint64_t expected_generation,
                                                  const std::string& evidence_label,
                                                  const std::string& message,
                                                  const std::string& reason_code) override {
    std::lock_guard<std::mutex> lock(gripper_mutex);
    if (gripper_status.command_id != expected_command_id ||
        gripper_status.generation != expected_generation) {
      return {false, gripper_status.command_id, gripper_status.generation,
              ErrorDTO{"stale_gripper_state", "gripper command identity changed", {}}};
    }
    ++gripper_mark_lost_count;
    gripper_status.state = GripperCommandState::kFailed;
    gripper_status.gripper_id = gripper_id;
    gripper_status.evidence.push_back(evidence_label);
    gripper_status.error = ErrorDTO{reason_code, message, {}};
    ++gripper_status.revision;
    return {true, gripper_status.command_id, gripper_status.generation, std::nullopt};
  }

  void invalidate_gripper_commands(GripperInvalidationReason reason) noexcept override {
    std::lock_guard<std::mutex> lock(gripper_mutex);
    gripper_admission_blocked.store(true, std::memory_order_release);
    last_gripper_invalidation_reason.store(reason, std::memory_order_release);
    ++gripper_invalidation_count;
    gripper_status.state = GripperCommandState::kIdle;
    ++gripper_status.generation;
    ++gripper_status.revision;
  }

  void authorize_gripper_commands() noexcept override {
    std::lock_guard<std::mutex> lock(gripper_mutex);
    gripper_admission_blocked.store(false, std::memory_order_release);
  }

  operator ExecutionKernelPorts() {
    return ExecutionKernelPorts{*this, *this, *this, *this, *this, this};
  }
};

class MockPlanningSceneSynchronizer final : public IPlanningSceneSynchronizer {
 public:
  Result<ProjectionRevisionDTO> SynchronizeBeforePlanning() override {
    ++call_count;
    return next_result;
  }

  int call_count{};
  Result<ProjectionRevisionDTO> next_result =
      Result<ProjectionRevisionDTO>::Ok(test_projection_revision());
};

class MockCollisionWorldPublisher final : public ICollisionWorldSnapshotPublisher {
 public:
  CollisionWorldSnapshotPtr current_snapshot = std::make_shared<const CollisionWorldSnapshot>();
  std::optional<CollisionPolicySnapshot> current_policy;
  bool throw_on_snapshot = false;
  mutable std::atomic<int> bundle_call_count{0};
  mutable std::atomic<int> published_runtime_bundle_call_count{0};
  std::optional<int> throw_on_bundle_call_number;
  std::vector<int> throw_on_bundle_call_numbers;
  std::optional<int> throw_on_published_runtime_bundle_call_number;
  std::vector<int> throw_on_published_runtime_bundle_call_numbers;
  bool lease_inventory_enabled = true;
  std::int64_t lease_inventory_captured_at_ns = 0;
  ProjectionRevisionDTO lease_projection_revision = test_projection_revision();
  std::shared_ptr<const LogicalSceneSnapshotDTO> current_logical_scene;
  mutable std::atomic<int> lease_inventory_call_count{0};
  mutable std::atomic<int> lease_retain_call_count{0};
  std::function<void()> on_lease_retain;
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
    return current_snapshot ? current_snapshot->revision : CollisionWorldRevision{};
  }

  std::optional<CollisionPolicySnapshot> collision_policy_snapshot() const override {
    return current_policy;
  }

  CollisionWorldPolicySnapshotBundle collision_world_policy_snapshot_bundle() const override {
    const int call = bundle_call_count.fetch_add(1) + 1;
    if ((throw_on_bundle_call_number.has_value() && call == *throw_on_bundle_call_number) ||
        std::find(throw_on_bundle_call_numbers.begin(), throw_on_bundle_call_numbers.end(), call) !=
            throw_on_bundle_call_numbers.end()) {
      throw std::runtime_error("snapshot bundle unavailable");
    }
    return {snapshot(), current_policy};
  }

  std::optional<CollisionWorldLeaseInventory> current_scene_snapshot_lease_inventory()
      const override {
    lease_inventory_call_count.fetch_add(1);
    if (!lease_inventory_enabled) {
      return std::nullopt;
    }
    return make_lease_inventory(ProjectionLeaseId{"mock_current_lease"});
  }

  std::optional<CollisionWorldLeaseInventory> retain_scene_snapshot_lease_inventory(
      ProjectionLeaseId projection_lease_id = ProjectionLeaseId{std::string{}}) override {
    lease_retain_call_count.fetch_add(1);
    if (on_lease_retain) on_lease_retain();
    if (!lease_inventory_enabled) {
      return std::nullopt;
    }
    if (projection_lease_id.empty()) {
      projection_lease_id =
          ProjectionLeaseId{"mock_lease_" + std::to_string(lease_retain_call_count.load())};
    }
    return make_lease_inventory(std::move(projection_lease_id));
  }

  void release_scene_snapshot_lease_inventory(
      const ProjectionLeaseId& /*projection_lease_id*/) override {
    lease_release_call_count.fetch_add(1);
  }

  RuntimeCollisionQueryBundlePtr published_runtime_collision_query_bundle() const override {
    const int call = published_runtime_bundle_call_count.fetch_add(1) + 1;
    if ((throw_on_published_runtime_bundle_call_number.has_value() &&
         call == *throw_on_published_runtime_bundle_call_number) ||
        std::find(throw_on_published_runtime_bundle_call_numbers.begin(),
                  throw_on_published_runtime_bundle_call_numbers.end(),
                  call) != throw_on_published_runtime_bundle_call_numbers.end()) {
      throw std::runtime_error("published runtime snapshot bundle unavailable");
    }
    return build_runtime_collision_query_bundle(
        CollisionWorldPolicySnapshotBundle{snapshot(), current_policy}, lease_projection_revision);
  }

 private:
  CollisionWorldLeaseInventory make_lease_inventory(ProjectionLeaseId projection_lease_id) const {
    CollisionWorldLeaseInventory inventory;
    inventory.projection_binding =
        ProjectionLeaseBindingDTO{lease_projection_revision, std::move(projection_lease_id)};
    inventory.logical_scene = current_logical_scene
                                  ? current_logical_scene
                                  : testing::make_test_logical_scene(inventory.projection_binding);
    inventory.captured_at_ns = lease_inventory_captured_at_ns;
    inventory.bundle = {snapshot(), current_policy};
    return inventory;
  }
};

class FakeCollisionApplyFenceAdmissionGate final : public IResourceOperationAdmissionGate {
 public:
  FakeCollisionApplyFenceAdmissionGate()
      : admission_mutex_(std::make_shared<std::recursive_mutex>()) {}

  [[nodiscard]] std::shared_ptr<std::recursive_mutex> shared_admission_mutex() const override {
    return admission_mutex_;
  }

  [[nodiscard]] Result<void> reserve(ResourceSlotTable&, const ResourceSlotOwnerDTO&,
                                     const std::vector<ResourceId>&) override {
    return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                      "test gate does not admit operations", {}});
  }

  [[nodiscard]] Result<void> reserve_batch(
      ResourceSlotTable&,
      const std::vector<ResourceSlotReservationOwnerDTO>&) override {
    return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                      "test gate does not admit operations", {}});
  }

  [[nodiscard]] std::vector<ResourceId> current_fenced_resources(
      const std::vector<ResourceId>& resources) const override {
    return held ? resources : std::vector<ResourceId>{};
  }

  [[nodiscard]] bool exact_fence_covers(
      const ConfigurationFenceId& fence_id,
      const std::vector<ResourceId>& resources) const override {
    return held && held_fence_id.has_value() && *held_fence_id == fence_id &&
           std::all_of(resources.begin(), resources.end(), [&](const auto& resource) {
             return std::find(held_scope.resources.begin(), held_scope.resources.end(), resource) !=
                    held_scope.resources.end();
           });
  }

  [[nodiscard]] bool exact_fence_is_held(
      const ConfigurationFenceId& fence_id,
      const ConfigurationFenceScopeDTO& scope) const override {
    return held && held_fence_id.has_value() && *held_fence_id == fence_id &&
           held_scope == scope;
  }

  bool held{true};
  std::optional<ConfigurationFenceId> held_fence_id;
  ConfigurationFenceScopeDTO held_scope;

 private:
  std::shared_ptr<std::recursive_mutex> admission_mutex_;
};

ConfigurationFenceScopeDTO make_collision_apply_fence_scope(
    std::string resource_id = "resource:collision-apply") {
  ConfigurationFenceScopeDTO scope;
  scope.controllable_component_ids = {ControllableComponentId{"component:collision-apply"}};
  scope.domains = {ConfigurationFenceDomainScopeDTO{
      ControlDomainId{"domain:collision-apply"}, std::nullopt,
      ControlDomainRevision{"revision:candidate"}}};
  scope.bindings = {ConfigurationFenceBindingScopeDTO{
      ControlBindingId{"binding:collision-apply"}, std::nullopt,
      ControlBindingRevision{"revision:candidate"}}};
  scope.resources = {ResourceId{std::move(resource_id)}};
  return scope;
}

class MockCollisionEngine final : public ICollisionQueryEngine {
 public:
  CollisionResult default_result;
  std::map<std::int64_t, CollisionResult> result_by_waypoint;
  std::vector<CollisionResult> result_sequence;
  std::function<std::optional<CollisionResult>(const CollisionQuery&)> result_for_query;
  std::function<void(const CollisionQuery&)> on_check;
  std::atomic<int> check_count{0};
  bool block_until_cancel_requested = false;
  std::mutex block_mutex;
  std::condition_variable block_cv;
  bool release_blocked_checks = false;
  std::atomic<int> blocked_check_count{0};
  std::atomic<int> cancel_observed_count{0};
  std::atomic<int> runtime_checks_in_flight{0};
  std::atomic<int> runtime_query_prepare_count{0};
  std::atomic<RuntimeCollisionQueryPrepareStatus> runtime_query_prepare_status{
      RuntimeCollisionQueryPrepareStatus::kReady};
  std::atomic<int> runtime_snapshot_prepare_count{0};
  std::atomic<bool> runtime_prepare_overlapped_check{false};
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
    capability_.supports_distance_query = true;
    capability_.supported_distance_request_modes = {
        CollisionDistanceRequestMode::kNone, CollisionDistanceRequestMode::kSelectedCollidingPairs,
        CollisionDistanceRequestMode::kAllCollidingPairs,
        CollisionDistanceRequestMode::kAllActivePairs};
    capability_.supports_penetration_depth = true;
    capability_.thread_safety = CollisionEngineThreadSafetyClass::kSingleContextExclusive;
    capability_.mutable_context_id =
        "mock_collision_engine:" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
    capability_.deterministic_ordering = true;
    capability_.runtime_query_memory_model = RuntimeCollisionQueryMemoryModel::kMeasuredBounded;
    capability_.runtime_query_max_joints = 256;
    capability_.runtime_query_max_world_bodies = 4096;
    capability_.runtime_query_max_collision_pairs = 4096;
    capability_.runtime_query_clean_allocation_upper_bound = 16;
    capability_.runtime_query_snapshot_change_may_allocate = true;
    capability_.runtime_query_snapshot_change_allocation_upper_bound = 256;
    capability_.runtime_query_qualified_wcet_ns = 10'000'000;
    capability_.runtime_query_snapshot_change_qualified_wcet_ns = 10'000'000;
  }

  const CollisionEngineCapabilityDescriptor& capability() const override { return capability_; }

  CollisionResult check(const CollisionWorldSnapshot& snapshot, const CollisionQuery& query,
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

  RuntimeCollisionQueryPrepareStatus prepare_runtime_query(
      std::span<const std::string> canonical_joint_names) noexcept override {
    runtime_query_prepare_count.fetch_add(1, std::memory_order_relaxed);
    if (runtime_checks_in_flight.load(std::memory_order_acquire) > 0) {
      runtime_prepare_overlapped_check.store(true, std::memory_order_release);
    }
    try {
      runtime_joint_names.assign(canonical_joint_names.begin(), canonical_joint_names.end());
      return runtime_query_prepare_status.load(std::memory_order_acquire);
    } catch (...) {
      return RuntimeCollisionQueryPrepareStatus::kEngineError;
    }
  }

  RuntimeCollisionQueryPrepareStatus prepare_runtime_snapshot(
      const CollisionWorldSnapshot& /*snapshot*/) noexcept override {
    runtime_snapshot_prepare_count.fetch_add(1, std::memory_order_relaxed);
    if (runtime_checks_in_flight.load(std::memory_order_acquire) > 0) {
      runtime_prepare_overlapped_check.store(true, std::memory_order_release);
    }
    return RuntimeCollisionQueryPrepareStatus::kReady;
  }

  RuntimeCollisionQueryResultView check_runtime(
      const CollisionWorldSnapshot& snapshot,
      const RuntimeCollisionQueryRequest& request) noexcept override {
    struct InFlightGuard {
      std::atomic<int>& count;
      explicit InFlightGuard(std::atomic<int>& in_flight) : count(in_flight) {
        count.fetch_add(1, std::memory_order_acq_rel);
      }
      ~InFlightGuard() { count.fetch_sub(1, std::memory_order_acq_rel); }
    } in_flight_guard{runtime_checks_in_flight};
    runtime_observations.clear();
    runtime_clearance_observations.clear();
    try {
      CollisionQuery query;
      query.query_id = "runtime_test_query";
      query.surface = CollisionSurface::kObservedRuntimeMonitor;
      query.collision_world_revision = snapshot.revision;
      query.robot_state.joint_names = runtime_joint_names;
      query.robot_state.positions_rad.assign(request.positions_rad.begin(),
                                             request.positions_rad.end());
      query.waypoint_index = 0;
      query.state_time_ns = request.state_time_ns;
      query.timeout_budget_ns = request.timeout_budget_ns;
      query.distance_request_mode = request.distance_request_mode;
      query.selected_distance_pairs.assign(request.selected_distance_pairs.begin(),
                                           request.selected_distance_pairs.end());
      if (request.distance_request_mode != CollisionDistanceRequestMode::kNone) {
        query.required_capabilities = {"distance", "penetration_depth"};
      }
      CollisionCancellationToken cancel;
      runtime_result = check(snapshot, query, CollisionPolicySnapshot{}, cancel);
      for (auto& pair : runtime_result.normalized_pairs) {
        if (pair.body_b < pair.body_a) {
          std::swap(pair.body_a, pair.body_b);
        }
      }
      for (auto& clearance : runtime_result.pair_clearances) {
        if (clearance.pair.body_b < clearance.pair.body_a) {
          std::swap(clearance.pair.body_a, clearance.pair.body_b);
        }
      }
      runtime_result.pair_clearances.erase(
          std::remove_if(
              runtime_result.pair_clearances.begin(), runtime_result.pair_clearances.end(),
              [&](const auto& clearance) {
                const bool colliding =
                    std::find(runtime_result.normalized_pairs.begin(),
                              runtime_result.normalized_pairs.end(),
                              clearance.pair) != runtime_result.normalized_pairs.end();
                switch (request.distance_request_mode) {
                  case CollisionDistanceRequestMode::kNone:
                    return true;
                  case CollisionDistanceRequestMode::kSelectedCollidingPairs:
                    return !colliding ||
                           std::find(request.selected_distance_pairs.begin(),
                                     request.selected_distance_pairs.end(),
                                     clearance.pair) == request.selected_distance_pairs.end();
                  case CollisionDistanceRequestMode::kAllCollidingPairs:
                    return !colliding;
                  case CollisionDistanceRequestMode::kAllActivePairs:
                    return false;
                }
                return true;
              }),
          runtime_result.pair_clearances.end());
      for (const auto& clearance : runtime_result.pair_clearances) {
        runtime_clearance_observations.push_back({&clearance.pair, clearance.signed_clearance_m});
      }
      for (const auto& pair : runtime_result.normalized_pairs) {
        std::optional<double> clearance;
        const auto it = std::find_if(runtime_result.pair_clearances.begin(),
                                     runtime_result.pair_clearances.end(),
                                     [&](const auto& candidate) { return candidate.pair == pair; });
        if (it != runtime_result.pair_clearances.end()) {
          clearance = it->signed_clearance_m;
        }
        runtime_observations.push_back({&pair, clearance});
      }
      RuntimeCollisionQueryResultView view;
      view.engine_status = runtime_result.engine_status;
      view.query_duration_ns = runtime_result.query_duration_ns.value_or(0);
      view.collision_pairs = runtime_observations;
      view.clearance_pairs = runtime_clearance_observations;
      if (runtime_result.engine_status != CollisionEngineStatus::kOk ||
          runtime_result.decision == CollisionDecisionResult::kInconclusiveFail) {
        view.status = RuntimeCollisionQueryStatus::kInconclusive;
        view.failure_reason = runtime_failure_reason;
      } else if (runtime_result.in_collision) {
        view.status = RuntimeCollisionQueryStatus::kCollision;
        view.failure_reason = RuntimeCollisionQueryFailureReason::kNone;
      } else {
        view.status = RuntimeCollisionQueryStatus::kClear;
        view.failure_reason = RuntimeCollisionQueryFailureReason::kNone;
      }
      return view;
    } catch (...) {
      RuntimeCollisionQueryResultView view;
      view.failure_reason = RuntimeCollisionQueryFailureReason::kEngineException;
      view.engine_status = CollisionEngineStatus::kEngineError;
      return view;
    }
  }

  std::string engine_id() const override { return capability_.engine_id; }
  std::string engine_version() const override { return capability_.engine_version; }

  std::vector<std::string> runtime_joint_names;
  CollisionResult runtime_result;
  std::vector<RuntimeCollisionPairObservation> runtime_observations;
  std::vector<RuntimeCollisionPairObservation> runtime_clearance_observations;
  RuntimeCollisionQueryFailureReason runtime_failure_reason =
      RuntimeCollisionQueryFailureReason::kEngineException;

  bool wait_until_check_blocked(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(block_mutex);
    return block_cv.wait_for(lock, timeout, [this]() { return blocked_check_count.load() > 0; });
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

ParticipantStopStageResultDTO unsupported_stop_stage(const ResolvedParticipantStopStageDTO& stage) {
  const ErrorDTO error{
      std::string(kErrorMotionInterruptBackendStopFailed),
      "historical ExecutionKernel test backend has no prepared Stop-stage membership",
      {{"reason", "prepared_stop_stage_not_available_in_legacy_test_backend"}}};
  ParticipantStopStageResultDTO result{stage,
                                       StopStageAttemptDispositionDTO::kNotDispatched,
                                       ParticipantStopStageOutcomeDTO::kFailed,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       0,
                                       {},
                                       error};
  if (stage.stage_index < stage.participant_stop_plan.stage_commands.size()) {
    const auto& requirements = stage.participant_stop_plan.stage_commands[stage.stage_index]
                                   .resolved_stage.evidence_requirements;
    result.evidence_items.reserve(requirements.size());
    for (const auto& requirement : requirements) {
      result.evidence_items.push_back(StopEvidenceItemDTO{
          requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
          StopEvidenceItemStateDTO::kMissing, requirement.resolved_source, std::nullopt,
          std::nullopt, std::nullopt, std::nullopt, error});
    }
  }
  return result;
}

class MockExecution : public IExecutionBackend {
 public:
  MockExecution()
      : observed_publication_(
            {"j1", "j2"}, {ObservedRobotStateSourceConfig{
                              ObservedRobotStateSourceKind::kJointState, "fixture_measured_state",
                              ObservedRobotStateAuthorityProfile::kMeasured, 1'000'000'000}}) {
    publish_observed_state(0, {0.0, 0.0}, {});
  }

  int execute_count = 0;
  int stop_count = 0;
  int reset_count = 0;
  int sync_count = 0;
  TrajectoryDTO last_trajectory;
  ExecutionSnapshot snapshot;
  Result<void> next_sync_result = Result<void>::Ok();
  std::function<void()> on_execute;
  std::function<void()> on_sync;
  std::function<void()> on_reset;
  /// Optional override for the terminal state reported back to the kernel
  /// at the end of `execute_trajectory`. When unset, behave as before
  /// (`kCompleted`). Used by tests that need to drive the kernel into a
  /// non-default terminal state (e.g. `kInterventionRequired`) to exercise
  /// the explicit acknowledgement paths.
  std::optional<ExecutionState> next_terminal_state;
  // Zero preserves the existing generation-less fixture. A test modelling an
  // effectful accepted dispatch supplies its positive backend generation.
  std::uint64_t execution_generation{0U};

  std::atomic<int> retire_count{0};
  Result<void> RetirePreparedExecution(std::uint64_t) override {
    ++retire_count;
    return Result<void>::Ok();
  }
  PrepareParticipantResultDTO Prepare(const PreparedOperationParticipantDTO& participant,
                                      std::uint64_t) override {
    const auto result = MakePrepareParticipantResult(
        participant,
        std::holds_alternative<PreparedMotionExecutionDTO>(participant.payload)
            ? PrepareParticipantResultStateDTO::kPrepared
            : PrepareParticipantResultStateDTO::kRejected,
        std::holds_alternative<PreparedMotionExecutionDTO>(participant.payload)
            ? std::nullopt
            : std::optional(ErrorDTO{std::string(kErrorInvalidRequest),
                                     "prepared payload tag mismatch",
                                     {{"reason", "prepared_payload_tag_mismatch"}}}));
    return result.value();
  }

  ExecutionHandle Dispatch(const PreparedOperationParticipantDTO& participant,
                           const ControllerGoalId& controller_goal_id,
                           ControllerGoalGeneration controller_goal_generation) override {
    auto handle =
        execute_trajectory(std::get<PreparedMotionExecutionDTO>(participant.payload).trajectory);
    handle.controller_goal_id = controller_goal_id;
    handle.controller_goal_generation = controller_goal_generation;
    handle.prepared_contract_digest = participant.prepared_contract_digest;
    return handle;
  }

  ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
    ++execute_count;
    last_trajectory = trajectory;
    snapshot.state = next_terminal_state.value_or(ExecutionState::kCompleted);
    snapshot.progress = 1.0;
    if (!trajectory.waypoints.empty()) {
      snapshot.joint_positions_rad = trajectory.waypoints.back().positions_rad;
    }
    if (on_execute) {
      on_execute();
    }
    return ExecutionHandle{[](int64_t /*timeout_ns*/) { return true; },
                           [this]() { return snapshot; }, execution_generation};
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

  ResetResultDTO reset() override {
    ++reset_count;
    if (on_reset) {
      on_reset();
    }
    return ResetResultDTO{true, std::nullopt};
  }

  ExecutionSnapshot get_execution_snapshot() const override { return snapshot; }

  const ObservedRobotStatePublication* observed_robot_state_publication() const noexcept override {
    return &observed_publication_;
  }

  void retire_execution_progress_generation(
      std::uint64_t /*execution_generation*/) noexcept override {}

  ExecutionProgressHazardClaimResult claim_execution_progress_hazard(
      std::uint64_t /*execution_generation*/) noexcept override {
    return ExecutionProgressHazardClaimResult::kGenerationMismatch;
  }

  void publish_observed_state(std::int64_t timestamp_ns, const std::vector<double>& positions,
                              const std::vector<double>& velocities) {
    static const std::vector<std::string> joints{"j1", "j2"};
    (void)observed_publication_.publish(ObservedRobotStateSourceKind::kJointState, joints,
                                        positions, velocities, timestamp_ns, timestamp_ns);
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

  MotionInterruptStopCapabilityDTO get_motion_interrupt_stop_capability() const override {
    return motion_interrupt_stop_capability;
  }

  ParticipantStopStageResultDTO TranslateStopStage(const ResolvedParticipantStopStageDTO& stage,
                                                   const RemainingStopBudgetDTO&,
                                                   std::stop_token) override {
    return unsupported_stop_stage(stage);
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
    r.stop_command_source = MotionInterruptStopCommandSource::kNativeSimulatedStop;
    r.backend_stop_confirmed = true;
    r.confirmation_latency_ns = 1'000'000;  // 1 ms
    return r;
  }

 private:
  ObservedRobotStatePublication observed_publication_;
};

class ControlledExecution : public IExecutionBackend {
 public:
  ControlledExecution()
      : observed_publication_({"j1", "j2"},
                              {ObservedRobotStateSourceConfig{
                                  ObservedRobotStateSourceKind::kJointState, "test_measured_state",
                                  ObservedRobotStateAuthorityProfile::kMeasured, 1'000'000'000}}) {
    publish_observed_state(0, {0.0, 0.0}, {});
  }

  int execute_count = 0;
  int stop_count = 0;
  // Counts EVERY legacy stop() call, including no-ops issued while the backend
  // has already flipped to not-running (e.g. the emergency hard-stop floor
  // after a confirmed/cancelled controlled stop). `stop_count` only counts
  // stops that found a running execution, so this proves the floor was actually
  // invoked.
  int stop_attempt_count = 0;
  int reset_count = 0;
  TrajectoryDTO last_execute_trajectory;
  ExecutionSnapshot snapshot;
  std::function<void()> on_execute;
  std::function<void()> on_get_snapshot;
  int sync_observed_state_count = 0;
  bool block_sync_observed_state = false;
  bool release_blocked_sync_observed_state = false;

  std::atomic<int> replacement_hold_begins{0};
  std::atomic<int> replacement_hold_ends{0};
  std::atomic<std::uint64_t> replacement_hold_candidate{0};
  std::atomic<std::uint64_t> replacement_hold_ended_candidate{0};
  std::atomic<std::int64_t> replacement_hold_deadline_ns{0};
  Result<void> BeginReplacementPreparationScaleHold(
      const PreparedMotionReplacementSourceDTO&, std::uint64_t candidate_id,
      std::int64_t preparation_deadline_ns, std::stop_token) override {
    replacement_hold_candidate.store(candidate_id);
    replacement_hold_deadline_ns.store(preparation_deadline_ns);
    ++replacement_hold_begins;
    return Result<void>::Ok();
  }
  Result<void> EndReplacementPreparationScaleHold(std::uint64_t candidate_id) override {
    replacement_hold_ended_candidate.store(candidate_id);
    ++replacement_hold_ends;
    return Result<void>::Ok();
  }

  std::atomic<int> retire_count{0};
  // Number of retirements to refuse as still winding down (item 12).
  std::atomic<int> retire_not_quiescent_refusals{0};
  Result<void> RetirePreparedExecution(std::uint64_t) override {
    ++retire_count;
    if (retire_not_quiescent_refusals.load() > 0) {
      --retire_not_quiescent_refusals;
      return Result<void>::Err(ErrorDTO{kErrorExecutionRejected,
                                        "Prepared execution still has active motion or Stop work",
                                        {{"reason", "prepared_execution_not_quiescent"}}});
    }
    return Result<void>::Ok();
  }
  PrepareParticipantResultDTO Prepare(const PreparedOperationParticipantDTO& participant,
                                      std::uint64_t) override {
    const auto result = MakePrepareParticipantResult(
        participant,
        std::holds_alternative<PreparedMotionExecutionDTO>(participant.payload)
            ? PrepareParticipantResultStateDTO::kPrepared
            : PrepareParticipantResultStateDTO::kRejected,
        std::holds_alternative<PreparedMotionExecutionDTO>(participant.payload)
            ? std::nullopt
            : std::optional(ErrorDTO{std::string(kErrorInvalidRequest),
                                     "prepared payload tag mismatch",
                                     {{"reason", "prepared_payload_tag_mismatch"}}}));
    return result.value();
  }

  ExecutionHandle Dispatch(const PreparedOperationParticipantDTO& participant,
                           const ControllerGoalId& controller_goal_id,
                           ControllerGoalGeneration controller_goal_generation) override {
    auto handle =
        execute_trajectory(std::get<PreparedMotionExecutionDTO>(participant.payload).trajectory);
    handle.controller_goal_id = controller_goal_id;
    handle.controller_goal_generation = controller_goal_generation;
    handle.prepared_contract_digest = participant.prepared_contract_digest;
    return handle;
  }

  ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
    if (on_execute) {
      on_execute();
    }
    std::uint64_t execution_generation = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++execute_count;
      last_execute_trajectory = trajectory;
      running_ = true;
      snapshot.state = ExecutionState::kExecuting;
      snapshot.progress = 0.0;
      if (!trajectory.waypoints.empty()) {
        snapshot.joint_positions_rad = trajectory.waypoints.front().positions_rad;
      }
      if (execution_progress_publication_) {
        execution_generation = ++execution_generation_;
      }
    }
    if (execution_progress_publication_) {
      (void)execution_progress_publication_->publish(execution_generation, 0, 0, 0);
    }
    cv_.notify_all();
    return make_handle(execution_generation);
  }

  Result<void> sync_observed_state_before_planning() override {
    std::unique_lock<std::mutex> lock(mu_);
    ++sync_observed_state_count;
    cv_.notify_all();
    if (block_sync_observed_state) {
      cv_.wait_for(lock, std::chrono::seconds(2),
                   [this]() { return release_blocked_sync_observed_state; });
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
        return StopResultDTO{false, ErrorDTO{kErrorStopFailed,
                                             "injected legacy stop failure",
                                             {{"reason", "test_injected_legacy_stop_failure"}}}};
      }
      if (!running_) {
        return StopResultDTO{false, ErrorDTO{kErrorInvalidRequest, "No execution in flight", {}}};
      }
      ++stop_count;
      running_ = false;
      snapshot.state = ExecutionState::kStopped;
    }
    cv_.notify_all();
    return StopResultDTO{true, std::nullopt};
  }

  ResetResultDTO reset() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++reset_count;
      running_ = false;
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

  const ObservedRobotStatePublication* observed_robot_state_publication() const noexcept override {
    return &observed_publication_;
  }

  void enable_execution_progress_publication() {
    execution_progress_publication_ = std::make_unique<ExecutionProgressPublication>(
        ExecutionProgressSourceKind::kBackendCommanded,
        ExecutionProgressAuthorityProfile::kPredictedNative, "test_predicted_native",
        1'000'000'000);
  }

  const ExecutionProgressPublication* execution_progress_publication() const noexcept override {
    return execution_progress_publication_.get();
  }

  void retire_execution_progress_generation(std::uint64_t execution_generation) noexcept override {
    if (execution_progress_publication_) {
      (void)execution_progress_publication_->invalidate_generation(execution_generation);
    }
  }

  ExecutionProgressHazardClaimResult claim_execution_progress_hazard(
      std::uint64_t execution_generation) noexcept override {
    return execution_progress_publication_
               ? execution_progress_publication_->claim_hazard_authority(execution_generation)
               : ExecutionProgressHazardClaimResult::kGenerationMismatch;
  }

  void publish_observed_state(std::int64_t timestamp_ns, const std::vector<double>& positions,
                              const std::vector<double>& velocities) {
    static const std::vector<std::string> joints{"j1", "j2"};
    (void)observed_publication_.publish(ObservedRobotStateSourceKind::kJointState, joints,
                                        positions, velocities, timestamp_ns, timestamp_ns);
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

  MotionInterruptStopCapabilityDTO get_motion_interrupt_stop_capability() const override {
    return motion_interrupt_stop_capability;
  }

  ParticipantStopStageResultDTO TranslateStopStage(const ResolvedParticipantStopStageDTO& stage,
                                                   const RemainingStopBudgetDTO&,
                                                   std::stop_token) override {
    return unsupported_stop_stage(stage);
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

  ControlledStopResultDTO execute_controlled_stop(const MotionInterruptStopProfileDTO& profile,
                                                  std::stop_token stop_token) override {
    ControlledStopResultDTO r;
    bool publish_stop_observation = false;
    std::vector<double> stop_positions;
    std::vector<double> stop_velocities;
    std::int64_t stop_timestamp_ns = 0;
    if (next_controlled_stop_result) {
      r = *next_controlled_stop_result;
    } else {
      r.accepted = true;
      r.executed_stop_mode = profile.stop_mode;
      r.stop_command_source = MotionInterruptStopCommandSource::kNativeSimulatedStop;
      r.backend_stop_confirmed = true;
      r.confirmation_latency_ns = 1'000'000;
    }
    {
      std::unique_lock<std::mutex> lock(mu_);
      ++controlled_stop_count;
      last_controlled_stop_profile = profile;
      cv_.notify_all();
      if (block_controlled_stop) {
        const bool released =
            ignore_controlled_stop_token_while_blocked
                ? cv_.wait_for(lock, std::chrono::seconds(2),
                               [this]() { return release_blocked_controlled_stop; })
                : cv_.wait_for(lock, stop_token, std::chrono::seconds(2),
                               [this]() { return release_blocked_controlled_stop; });
        if (!released || stop_token.stop_requested()) {
          ++controlled_stop_cancel_count;
          running_ = false;
          snapshot.state = ExecutionState::kStopped;
          r.accepted = true;
          r.backend_stop_confirmed = false;
          r.error =
              ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                       "controlled stop cancelled by test",
                       {{"reason", stop_token.stop_requested() ? "controlled_stop_cancelled"
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
      const bool stop_succeeded = r.accepted && r.backend_stop_confirmed && !r.error.has_value();
      if (stop_succeeded) {
        running_ = false;
        snapshot.state = ExecutionState::kStopped;
        if (next_post_stop_joint_positions_rad) {
          snapshot.joint_positions_rad = *next_post_stop_joint_positions_rad;
        }
        if (next_post_stop_joint_velocities_rad_s) {
          snapshot.joint_velocities_rad_s = *next_post_stop_joint_velocities_rad_s;
        }
        if (next_post_stop_state_source) {
          snapshot.state_source = *next_post_stop_state_source;
        }
        publish_stop_observation = true;
        stop_positions = snapshot.joint_positions_rad;
        stop_velocities = snapshot.joint_velocities_rad_s;
        stop_timestamp_ns = snapshot.timestamp_ns;
      }
    }
    if (publish_stop_observation && stop_positions.size() == 2u) {
      publish_observed_state(stop_timestamp_ns, stop_positions, stop_velocities);
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

  void set_progress(double progress, std::vector<double> joint_positions,
                    std::optional<std::int64_t> timestamp_ns = std::nullopt) {
    std::vector<double> observed_positions = joint_positions;
    std::int64_t observed_timestamp_ns = timestamp_ns.value_or(0);
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot.progress = progress;
      snapshot.joint_positions_rad = std::move(joint_positions);
      snapshot.state = ExecutionState::kExecuting;
      if (timestamp_ns) {
        snapshot.timestamp_ns = *timestamp_ns;
      } else {
        observed_timestamp_ns = snapshot.timestamp_ns;
      }
    }
    publish_observed_state(observed_timestamp_ns, observed_positions, {});
    cv_.notify_all();
  }

  void set_snapshot_velocities(std::vector<double> joint_velocities_rad_s) {
    std::vector<double> positions;
    std::vector<double> velocities = joint_velocities_rad_s;
    std::int64_t timestamp_ns = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot.joint_velocities_rad_s = std::move(joint_velocities_rad_s);
      positions = snapshot.joint_positions_rad;
      timestamp_ns = snapshot.timestamp_ns;
    }
    if (positions.size() == 2u) {
      publish_observed_state(timestamp_ns, positions, velocities);
    }
    cv_.notify_all();
  }

  void complete() {
    std::uint64_t execution_generation = 0;
    std::int64_t terminal_time_ns = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      running_ = false;
      snapshot.state = ExecutionState::kCompleted;
      snapshot.progress = 1.0;
      const auto* source = &last_execute_trajectory;
      if (!source->waypoints.empty()) {
        snapshot.joint_positions_rad = source->waypoints.back().positions_rad;
        terminal_time_ns =
            static_cast<std::int64_t>(source->waypoints.back().time_from_start_s * 1e9);
      }
      execution_generation = execution_generation_;
    }
    if (execution_progress_publication_ && execution_generation != 0) {
      (void)execution_progress_publication_->publish_terminal(
          execution_generation, terminal_time_ns, terminal_time_ns, terminal_time_ns);
    }
    cv_.notify_all();
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      running_ = false;
    }
    cv_.notify_all();
  }

  // The backend ends the execution on its own with `state` and no error.
  void end_without_completion(ExecutionState state) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      running_ = false;
      snapshot.state = state;
      snapshot.error.reset();
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
    return cv_.wait_for(lock, timeout, [this]() { return execute_count > 0; });
  }

  bool wait_until_sync_observed_state_count(
      int count, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() { return sync_observed_state_count >= count; });
  }

  bool wait_until_execute_count(
      int count, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this, count]() { return execute_count >= count; });
  }

  bool wait_until_stop_count(int count,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this, count]() { return stop_count >= count; });
  }

  bool wait_until_stop_attempt_count(
      int count, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this, count]() { return stop_attempt_count >= count; });
  }

  bool wait_until_controlled_stop_count(
      int count, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this, count]() { return controlled_stop_count >= count; });
  }

  bool wait_until_controlled_stop_cancel_count(
      int count, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() { return controlled_stop_cancel_count >= count; });
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
      int count, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() { return runtime_velocity_scale_set_count >= count; });
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
  ExecutionHandle make_handle(std::uint64_t execution_generation = 0) {
    return ExecutionHandle{[this](int64_t timeout_ns) {
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
                           },
                           execution_generation};
  }

  mutable std::mutex mu_;
  // condition_variable_any: the blocked controlled-stop wait must observe the
  // cancel token atomically with the predicate. A plain condition_variable
  // notified from a stop_callback (which runs without holding mu_) can lose
  // the wakeup and sleep out the full fallback timeout, making the backend
  // appear to ignore its cancel token.
  std::condition_variable_any cv_;
  bool running_ = false;
  ObservedRobotStatePublication observed_publication_;
  std::unique_ptr<ExecutionProgressPublication> execution_progress_publication_;
  std::uint64_t execution_generation_ = 0;
};

class ThreadSafeManualScheduler final : public IScheduler {
 public:
  ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback,
                          SchedulerPriority /*priority*/ = SchedulerPriority::Lifecycle) override {
    ScheduleHandle handle = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      handle = next_handle_++;
      pending_.emplace(deadline, std::make_pair(handle, std::move(callback)));
    }
    pending_changed_.notify_all();
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

  bool wait_for_pending_deadline(TimePoint deadline,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::unique_lock<std::mutex> lock(mu_);
    return pending_changed_.wait_for(lock, timeout, [&]() { return pending_.contains(deadline); });
  }

 private:
  mutable std::mutex mu_;
  std::condition_variable pending_changed_;
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
  t.resources = {ResourceId("joint.1"), ResourceId("joint.2")};
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

TrajectoryDTO make_test_trajectory_from(std::vector<double> start, std::vector<double> goal) {
  TrajectoryDTO t;
  t.resources = {ResourceId("joint.1"), ResourceId("joint.2")};
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
  auto req = testing::make_test_execute_request({"j1", "j2"}, "run-execution-kernel-test");
  req.binding.resources = {ResourceId("joint.1"), ResourceId("joint.2")};
  req.binding.retained_bundle_id = test_projection_revision().retained_bundle_id;
  req.binding.graph_revision = test_projection_revision().graph_revision;
  req.binding.controllable_component_id = ControllableComponentId("robot1");
  req.move_id = move_id;
  std::get<ResolvedComponentModelFrameDTO>(req.binding.base_frame).compiled_frame_name = "world";
  std::get<ResolvedComponentModelFrameDTO>(req.binding.tcp_frame).compiled_frame_name = "tool0";
  req.tolerances.joint_position_tol_rad = 0.1;
  req.tolerances.translation_tol_m = 0.01;
  req.tolerances.rotation_tol_rad = 0.01;
  req.speed_profile.velocity_scale = 1.0;
  req.timeouts.planning_timeout_s = 10.0;
  req.cache_mode = CacheMode::kDefault;
  req.motion = ResolvedJointGoalDTO{req.binding.resources, {"j1", "j2"}, {1.0, 1.0}};
  return req;
}

GripperCommandRequestDTO make_test_gripper_request() {
  GripperCommandRequestDTO request;
  request.gripper_id = "primary";
  request.group_id = "gripper";
  request.joint_name = "finger_joint";
  request.target_position = 0.7;
  request.duration_s = 0.1;
  return request;
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

void expect_detail(const std::map<std::string, std::string>& details, const std::string& key,
                   const std::string& expected_value) {
  auto it = details.find(key);
  ASSERT_NE(it, details.end()) << "missing detail key " << key;
  EXPECT_EQ(it->second, expected_value) << "detail key " << key;
}

void expect_action_span_details(const std::map<std::string, std::string>& details,
                                const ActionSpanDTO& span) {
  expect_detail(details, "program_run_id", span.program_run_id);
  expect_detail(details, "action_source", to_string(span.action_source));
  expect_detail(details, "action_span_id", span.action_span_id);
  expect_detail(details, "action_span_program_run_id", span.program_run_id);
  expect_detail(details, "node_instance_id", span.node_instance_id);
  expect_detail(details, "node_attempt_id", span.node_attempt_id);
  expect_detail(details, "parent_node_attempt_id", span.parent_node_attempt_id);
}

void expect_collision_world_revision(const CollisionWorldRevision& actual,
                                     const CollisionWorldRevision& expected) {
  EXPECT_EQ(actual.revision_id, expected.revision_id);
  EXPECT_EQ(actual.content_hash, expected.content_hash);
}

CollisionWorldSnapshot make_test_collision_world_snapshot() {
  CollisionWorldSnapshot snapshot;
  snapshot.publisher_id = "mock_kernel_collision_world";
  snapshot.revision = CollisionWorldRevision{1, test_digest("mock-collision-world")};
  snapshot.snapshot_format_version = 1;
  snapshot.body_count = 0;
  snapshot.geometry_count = 0;
  snapshot.all_bodies_have_geometry = true;
  return snapshot;
}

Pose3DTO make_test_pose(double x_m = 0.0, double y_m = 0.0, double z_m = 0.0) {
  Pose3DTO pose;
  pose.position_m.x_m = x_m;
  pose.position_m.y_m = y_m;
  pose.position_m.z_m = z_m;
  pose.orientation.w = 1.0;
  return pose;
}

CollisionWorldBody make_observed_box_body(const std::string& object_id, const Pose3DTO& pose,
                                          std::string body_hash) {
  CollisionWorldBody body;
  body.body_id = "scene/" + object_id + "/shape/0";
  body.owner_kind = CollisionOwnerKind::kSceneResource;
  body.frame_id = "universe";
  body.pose = pose;
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
    const std::string& object_id, const Pose3DTO& pose, CollisionWorldRevision revision,
    std::string body_hash) {
  auto snapshot = make_test_collision_world_snapshot();
  snapshot.revision = std::move(revision);
  snapshot.bodies.push_back(make_observed_box_body(object_id, pose, std::move(body_hash)));
  snapshot.body_count = static_cast<std::int64_t>(snapshot.bodies.size());
  snapshot.geometry_count = 1;
  snapshot.scene_resource_count = 1;
  return snapshot;
}

void install_mock_retained_object(MockCollisionWorldPublisher& publisher,
                                  const std::string& object_id) {
  const auto pose = make_test_pose();
  const auto revision = publisher.current_snapshot
                            ? publisher.current_snapshot->revision
                            : CollisionWorldRevision{1, test_digest("mock-collision-world")};
  publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(make_test_collision_world_snapshot_with_object(
          object_id, pose, revision, object_id + "-body-hash"));
}

CollisionWorldBody make_test_robot_link_body(const std::string& link_name) {
  CollisionWorldBody body;
  body.body_id = "component/arm/model/arm_model/body/" + link_name + "/collision/0";
  body.owner_kind = CollisionOwnerKind::kComponentModelBody;
  body.frame_id = "universe";
  body.pose.orientation.w = 1.0;
  CollisionGeometryDescriptor geom;
  geom.body_id = body.body_id;
  geom.owner_kind = body.owner_kind;
  geom.geometry_kind = CollisionGeometryKind::kBox;
  geom.box_dimensions_m = Vector3DTO{0.01, 0.01, 0.01};
  body.geometries.push_back(std::move(geom));
  body.body_content_hash = link_name + "-body-hash";
  return body;
}

void install_mock_contact_grasp_world(MockCollisionWorldPublisher& publisher,
                                      const std::string& object_id,
                                      const std::vector<std::string>& allowed_links) {
  install_mock_retained_object(publisher, object_id);
  auto snapshot = *publisher.current_snapshot;
  for (const auto& link_name : allowed_links) {
    snapshot.bodies.push_back(make_test_robot_link_body(link_name));
  }
  std::sort(snapshot.bodies.begin(), snapshot.bodies.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.body_id < rhs.body_id; });
  snapshot.body_count = static_cast<std::int64_t>(snapshot.bodies.size());
  snapshot.geometry_count = snapshot.body_count;
  snapshot.component_model_body_count = static_cast<std::int64_t>(allowed_links.size());
  publisher.current_snapshot = std::make_shared<const CollisionWorldSnapshot>(std::move(snapshot));
}

void install_mock_detached_logical_object(MockCollisionWorldPublisher& publisher,
                                          const std::string& object_id) {
  const ProjectionLeaseBindingDTO binding{publisher.lease_projection_revision,
                                          ProjectionLeaseId("mock_contact_grasp_lease")};
  auto scene = *testing::make_test_logical_scene(binding);
  scene.objects = {
      SceneObjectSnapshotDTO{SceneResourceId(object_id),
                             {BoxShapeDTO{Vector3DTO{0.1, 0.1, 0.1}}},
                             CellFixedSceneObjectPlacementDTO{scene.cell_id, make_test_pose()},
                             std::nullopt,
                             {}}};
  scene.revision.content_hash = logical_scene_identity::ComputeLogicalSceneContentHash(scene);
  publisher.lease_projection_revision.scene_revision = scene.revision;
  publisher.current_logical_scene =
      std::make_shared<const LogicalSceneSnapshotDTO>(std::move(scene));
}

// Binds a resolved, versioned contact grasp exactly as the addressed owner
// does; the prepared motion product admits no unversioned contact policy.
void bind_resolved_contact_grasp(ExecuteRequestDTO& request, MockCollisionWorldPublisher& publisher,
                                 const std::string& policy_id, const std::string& object_id,
                                 const std::vector<std::string>& allowed_links,
                                 double max_penetration_m) {
  install_mock_detached_logical_object(publisher, object_id);
  std::vector<CollisionBodyId> allowed_bodies;
  for (const auto& link : allowed_links) {
    allowed_bodies.emplace_back("component/arm/model/arm_model/body/" + link + "/collision/0");
  }
  std::sort(allowed_bodies.begin(), allowed_bodies.end());
  request.contact_policy_id = policy_id;
  request.runtime_object_bindings = {RuntimeObjectReferenceBindingDTO{
      RuntimeObjectId("selected_object"), SceneResourceId(object_id)}};
  request.resolved_contact_grasp =
      ResolvedContactGraspBindingDTO{ContactPolicyId(policy_id),
                                     ContactPolicyRevision(kTestDigestA),
                                     RuntimeObjectId("selected_object"),
                                     SceneResourceId(object_id),
                                     std::move(allowed_bodies),
                                     ContactGraspEnvelopeDTO{max_penetration_m, 0.75}};
}

[[nodiscard]] bool is_bound_contact_grasp_policy(const std::string& policy_id) {
  return policy_id.rfind("contact_grasp.bound.", 0) == 0U;
}

CollisionWorldBody make_dynamic_test_body() {
  CollisionWorldBody body;
  body.body_id = "scene/dynamic_box/shape/0";
  body.owner_kind = CollisionOwnerKind::kSceneResource;
  body.frame_id = "universe";
  body.pose.orientation.w = 1.0;
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
  snapshot.revision.content_hash = test_digest("mock-policy");
  snapshot.require_full_diagnostics = false;
  return snapshot;
}

CollisionResult make_authoritative_collision_result() {
  CollisionResult result;
  result.in_collision = true;
  result.normalized_pairs = {
      CollisionPairId{"component/arm/model/arm_model/body/link1/collision/0", "scene/box/shape/0"}};
  result.engine_status = CollisionEngineStatus::kOk;
  result.decision = CollisionDecisionResult::kFail;
  return result;
}

// M6 clearance fixtures: one robot link plus scene obstacles, and a mock
// engine answer for one pair at a signed distance that honours the query's
// clearance margin like ManyForgeCollisionEngine.
const CollisionBodyId kClearanceTestLink = "component/arm/model/arm_model/body/link1/collision/0";

CollisionWorldSnapshot make_clearance_test_world(CollisionWorldSnapshot world,
                                                 const std::vector<std::string>& obstacles) {
  world.bodies.push_back(make_test_robot_link_body("link1"));
  for (const auto& obstacle : obstacles) {
    world.bodies.push_back(make_observed_box_body(obstacle, make_test_pose(), obstacle + "-hash"));
  }
  std::sort(world.bodies.begin(), world.bodies.end(),
            [](const auto& left, const auto& right) { return left.body_id < right.body_id; });
  world.body_count = static_cast<std::int64_t>(world.bodies.size());
  world.geometry_count = world.body_count;
  world.component_model_body_count = 1;
  world.scene_resource_count = world.body_count - 1;
  return world;
}

CollisionPairId clearance_test_pair(const std::string& obstacle) {
  return CollisionPairId{kClearanceTestLink, "scene/" + obstacle + "/shape/0"};
}

std::optional<CollisionResult> clearance_test_result(const CollisionQuery& query,
                                                     const CollisionPairId& pair,
                                                     double signed_distance_m) {
  const double margin =
      query.clearance_margins ? query.clearance_margins->margin_for(pair) : 0.0;
  if (signed_distance_m > 0.0 && signed_distance_m >= margin) {
    return std::nullopt;
  }
  auto result = make_authoritative_collision_result();
  result.normalized_pairs = {pair};
  if (signed_distance_m > 0.0) {
    result.clearance_violations = {CollisionPairClearance{pair, signed_distance_m}};
  }
  return result;
}

double clearance_test_joint(const CollisionQuery& query, const std::string& joint) {
  const auto& names = query.robot_state.joint_names;
  const auto found = std::find(names.begin(), names.end(), joint);
  return found == names.end()
             ? 0.0
             : query.robot_state.positions_rad.at(
                   static_cast<std::size_t>(std::distance(names.begin(), found)));
}

CollisionResult make_support_contact_collision_result(const CollisionPairId& pair,
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

CollisionResult make_contact_grasp_collision_result(const CollisionPairId& pair,
                                                    double signed_clearance_m) {
  return make_support_contact_collision_result(pair, signed_clearance_m);
}

CollisionPolicySnapshot make_contact_grasp_policy_snapshot(
    const std::string& policy_id, const std::string& selected_object_id,
    std::vector<std::string> allowed_robot_links, double max_penetration_m) {
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

// Audit 6G: 2 s bound (2000 x 1 ms), matching the cv-based fake-backend
// helpers. The previous 200 ms bound flaked under first-run parallel ctest
// load (observed once on Stop_OperatorProfileRefused_EmergencyLegacyFallback,
// whose stop path crosses several thread handoffs). Passing tests return
// early; only genuine failures pay the longer bound.
bool wait_for_state(ExecutionKernel& kernel, ExecutionState target_state, int max_attempts = 2000) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (kernel.get_status().state == target_state && !kernel.get_status().fact.has_value()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

/// The participant's reported ending of its current execution, named for
/// assertions. The participant reports facts; it never holds an outcome.
std::string ended_as(const ExecutionStatus& status) {
  if (!status.fact.has_value()) return std::string("working:") + to_string(status.state);
  switch (status.fact->kind) {
    case ParticipantExecutionFactKind::kComputedCompletion:
      return "completed";
    case ParticipantExecutionFactKind::kLocalFailure:
      return status.fact->effect == ParticipantEffectKnowledge::kEffectUncertain
                 ? "failed_effect_uncertain"
                 : "failed";
    case ParticipantExecutionFactKind::kCancelledBeforeDispatch:
      return "cancelled_before_dispatch";
    case ParticipantExecutionFactKind::kPreemptedByAddressedStop:
      return "preempted_by_addressed_stop";
    case ParticipantExecutionFactKind::kPreemptedByReplacement:
      return "preempted_by_replacement";
  }
  return "unknown";
}

std::string ended_as(const ExecutionKernel& kernel) { return ended_as(kernel.get_status()); }

bool wait_for_ended(ExecutionKernel& kernel, const std::string& expected, int max_attempts = 2000) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (ended_as(kernel) == expected) {
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

struct LookaheadPermitDriveResult {
  bool admitted = false;
  MetricsSnapshot metrics;
};

LookaheadPermitDriveResult drive_until_lookahead_permit_admitted(
    ExecutionKernel& kernel, ThreadSafeFakeClock& clock, ThreadSafeManualScheduler& scheduler,
    std::int64_t monitor_period_ns, int max_attempts = 100) {
  LookaheadPermitDriveResult result;
  result.metrics = kernel.get_metrics();
  const auto jobs_started_before = result.metrics.runtime_collision_lookahead_jobs_started;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    result.metrics = kernel.get_metrics();
    if (result.metrics.runtime_collision_lookahead_permit_pending ||
        result.metrics.runtime_collision_lookahead_job_active ||
        result.metrics.runtime_collision_lookahead_jobs_started > jobs_started_before) {
      result.admitted = true;
      return result;
    }
    if (!result.metrics.runtime_collision_lookahead_armed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // RuntimeSafety deliberately never waits for the lower-priority lookahead
    // session mutex. A legal try-lock miss consumes no permit; production
    // retries on the next clear monitor tick. Stop advancing fake time as soon
    // as one request is accepted so its absolute query budget remains stable.
    clock.advance(monitor_period_ns);
    scheduler.advance_to(clock.now());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  result.metrics = kernel.get_metrics();
  result.admitted = result.metrics.runtime_collision_lookahead_permit_pending ||
                    result.metrics.runtime_collision_lookahead_job_active ||
                    result.metrics.runtime_collision_lookahead_jobs_started > jobs_started_before;
  return result;
}

KernelRecoveryActionRequestDTO bound_recovery_request(
    const KernelRecoveryAssessmentDTO& assessment, RecoveryAction action,
    std::string principal_id = "operator_test", std::string reason = "test recovery action",
    std::string request_id = "test-recovery-action") {
  KernelRecoveryActionRequestDTO request;
  request.request_id = std::move(request_id);
  request.action = action;
  request.principal_id = std::move(principal_id);
  request.reason = std::move(reason);
  request.expected_assessment_revision = assessment.assessment_revision;
  request.expected_control_command_epoch = assessment.control_command_epoch;
  request.expected_execution_id = assessment.execution_id;
  request.expected_latch_id = assessment.latch_id;
  request.expected_stop_certificate_id = assessment.stop_certificate_id;
  request.expected_stop_owner_generation = assessment.stop_owner_generation;
  return request;
}

KernelRecoveryActionResultDTO apply_recovery_action_after_projection(
    ExecutionKernel& kernel, RecoveryAction action, std::string principal_id = "operator_test",
    std::string reason = "test recovery action") {
  constexpr int kMaxAssessmentRebinds = 8;
  auto assessment = kernel.get_recovery_assessment();
  for (int attempt = 0; attempt <= kMaxAssessmentRebinds; ++attempt) {
    auto request =
        bound_recovery_request(assessment, action, principal_id, reason,
                               "test-recovery-action-after-projection-" + std::to_string(attempt));
    auto result = kernel.apply_recovery_action(request);
    if (result.accepted || !result.error.has_value() || !result.error->details.contains("reason") ||
        result.error->details.at("reason") != reason_codes::kRecoveryStaleAssessment ||
        attempt == kMaxAssessmentRebinds) {
      return result;
    }
    assessment = result.resulting_assessment;
  }
  std::terminate();
}

CollisionPipelineResourceOwners make_owned_collision_pipeline_resources() {
  auto publisher = std::make_shared<MockCollisionWorldPublisher>();
  publisher->current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(make_test_collision_world_snapshot());
  publisher->current_policy = make_test_collision_policy_snapshot();
  return CollisionPipelineResourceOwners{publisher, std::make_shared<MockCollisionEngine>(),
                                         {std::make_shared<MockCollisionEngine>()}, {}};
}

// --- Test fixture ---

// Records the details of the latest runtime collision fault; they are the
// metadata of the participant's runtime-collision latch.
class CollisionDetailsRecorder {
 public:
  explicit CollisionDetailsRecorder(ExecutionKernel& kernel)
      : kernel_(kernel), handle_(kernel.subscribe_events([this](const KernelEvent& event) {
          if (event.type != KernelEventType::kCollisionDetected) return;
          std::lock_guard<std::mutex> lock(mutex_);
          latest_ = event.details;
        })) {}
  CollisionDetailsRecorder(const CollisionDetailsRecorder&) = delete;
  CollisionDetailsRecorder& operator=(const CollisionDetailsRecorder&) = delete;
  ~CollisionDetailsRecorder() { kernel_.unsubscribe_events(handle_); }

  [[nodiscard]] std::optional<std::map<std::string, std::string>> latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
  }

 private:
  ExecutionKernel& kernel_;
  mutable std::mutex mutex_;
  std::optional<std::map<std::string, std::string>> latest_;
  SubscriptionHandle handle_;
};

// Drives the production prepared path (spec: PrepareMotion then the exact
// DispatchPreparedMotion) for one resolved motion. Defined after the prepared
// authority fixtures below.
[[nodiscard]] ResolvedPreparedMotionExecutionRequestDTO prepared_motion_request(
    ExecuteRequestDTO request);
[[nodiscard]] Result<std::uint64_t> prepare_and_dispatch(
    ExecutionKernel& kernel, ResolvedPreparedMotionExecutionRequestDTO request);
[[nodiscard]] Result<std::uint64_t> prepare_and_dispatch(ExecutionKernel& kernel,
                                                         ExecuteRequestDTO request);

class ExecutionKernelTest : public ::testing::Test {
 protected:
  FakeClock clock;
  ManualScheduler scheduler;
  MockPlanner planner;
  MockFocusedPorts context;
  MockExecution execution;
  TrajectoryCache cache{CacheConfig{10}};
  MockCollisionWorldPublisher collision_world_publisher;
  std::shared_ptr<MockCollisionEngine> collision_runtime_engine =
      std::make_shared<MockCollisionEngine>();
  std::shared_ptr<MockCollisionEngine> collision_engine = std::make_shared<MockCollisionEngine>();
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
    context.current_state_value = state;
    collision_world_publisher.current_snapshot =
        std::make_shared<const CollisionWorldSnapshot>(make_test_collision_world_snapshot());
    collision_world_publisher.current_policy = make_test_collision_policy_snapshot();
    collision_runtime_engine = std::make_shared<MockCollisionEngine>();
    collision_engine = std::make_shared<MockCollisionEngine>();
    kernel.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
    kernel.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                   {collision_engine});
  }

  void install_at_rest_recovery_observation() {
    execution.publish_observed_state(clock.now(), {1.0, 1.0}, {0.0, 0.0});
    clock.advance(20'000'000);
    scheduler.advance_to(clock.now());
    clock.advance(200'000'000);
    execution.publish_observed_state(clock.now(), {1.0, 1.0}, {0.0, 0.0});
    scheduler.advance_to(clock.now());
  }

  /// Helper: prepare, dispatch and wait for async completion.
  Result<uint64_t> execute_sync(const ExecuteRequestDTO& req) { return execute_sync(kernel, req); }

  Result<uint64_t> execute_sync(ExecutionKernel& target, const ExecuteRequestDTO& req) {
    auto prepared = prepared_motion_request(req);
    if (req.target_mode == MotionTargetModeDTO::kRelative || req.goal_frame_reference.has_value()) {
      auto controlled = testing::make_test_controlled_state(
          prepared.execution_request.binding, context.current_state_value.positions_rad,
          context.current_state_value.velocities_rad_s.empty()
              ? std::nullopt
              : std::optional<std::vector<double>>{context.current_state_value.velocities_rad_s});
      context.addressed_publication_value = testing::make_test_observed_state_publication(
          prepared.execution_request.binding, controlled, clock.now());
    }
    auto result = prepare_and_dispatch(target, std::move(prepared));
    if (result.ok()) {
      const bool completed = target.wait_for_completion(2'000'000'000);
      const bool already_terminal = target.get_status().fact.has_value();
      EXPECT_TRUE(completed || already_terminal)
          << "execute_sync timed out waiting for " << req.move_id;
    }
    return result;
  }

  void store_cache(const std::string& move_id, TrajectoryDTO trajectory) {
    store_cache(make_test_request(move_id), std::move(trajectory));
  }

  void store_cache(const ExecuteRequestDTO& request, TrajectoryDTO trajectory,
                   std::uint32_t planning_attempt_index = 0U) {
    const auto resolved_request = prepared_motion_request(request).execution_request;
    cache.store(trajectory_cache_key(resolved_request), std::move(trajectory),
                trajectory_cache_intent_identity(resolved_request), planning_attempt_index);
  }

  TrajectoryCacheKey cache_key(const std::string& move_id) const {
    return trajectory_cache_key(
        prepared_motion_request(make_test_request(move_id)).execution_request);
  }

  std::optional<TrajectoryDTO> peek_cache(const std::string& move_id) const {
    return cache.peek_unvalidated(cache_key(move_id));
  }

  ExecuteRequestDTO make_relative_object_request(const std::string& move_id,
                                                 const std::string& object_id) {
    auto request = make_test_request(move_id);
    request.target_mode = MotionTargetModeDTO::kRelative;
    request.target_reference = ReferenceDTO(ReferenceDTO::Type::kSceneResource, object_id);
    PoseGoalDTO offset;
    offset.goal.orientation.w = 1.0;
    request.motion = offset;
    return request;
  }

  void publish_observed_box_for_lease(
      const std::string& object_id, double x_m, CollisionWorldRevision revision,
      std::string body_hash) {
    const auto pose = make_test_pose(x_m, 0.0, 0.0);
    collision_world_publisher.current_snapshot = std::make_shared<const CollisionWorldSnapshot>(
        make_test_collision_world_snapshot_with_object(object_id, pose, std::move(revision),
                                                       std::move(body_hash)));
  }

  void enable_observed_box_lease(const std::string& object_id) {
    collision_world_publisher.lease_inventory_enabled = true;
    collision_world_publisher.current_policy = make_test_collision_policy_snapshot();
    publish_observed_box_for_lease(object_id, 0.0,
                                   CollisionWorldRevision{1, test_digest("box-world-1")},
                                   "box-body-hash-1");
  }

  void configure_authoritative_collision_validation(
      ExecutionKernel& target_kernel, MockCollisionWorldPublisher& publisher,
      const std::shared_ptr<MockCollisionEngine>& engine) {
    publisher.current_snapshot =
        std::make_shared<const CollisionWorldSnapshot>(make_test_collision_world_snapshot());
    publisher.current_policy = make_test_collision_policy_snapshot();
    target_kernel.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
    target_kernel.set_collision_resources(publisher, std::make_shared<MockCollisionEngine>(),
                                          {engine});
  }

  void configure_runtime_collision_monitoring(
      ExecutionKernel& target_kernel, MockCollisionWorldPublisher& publisher,
      const std::shared_ptr<MockCollisionEngine>& validation_engine,
      const std::shared_ptr<MockCollisionEngine>& runtime_engine,
      std::vector<std::shared_ptr<ICollisionQueryEngine>> lookahead_engines = {}) {
    publisher.current_snapshot =
        std::make_shared<const CollisionWorldSnapshot>(make_test_collision_world_snapshot());
    publisher.current_policy = make_test_collision_policy_snapshot();
    target_kernel.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
    target_kernel.set_collision_resources(publisher, runtime_engine, {validation_engine},
                                          std::move(lookahead_engines));
  }
};

// --- Happy path ---

TEST_F(ExecutionKernelTest, CollisionResourcesReplaceOnlyUnderExactApplyFenceAndReadBackOwners) {
  const ConfigurationFenceId apply_fence_id{"apply:collision-resource-replacement"};
  const auto apply_scope = make_collision_apply_fence_scope();
  ASSERT_TRUE(apply_scope.valid());
  FakeCollisionApplyFenceAdmissionGate admission_gate;
  admission_gate.held_fence_id = apply_fence_id;
  admission_gate.held_scope = apply_scope;

  const auto candidate = make_owned_collision_pipeline_resources();
  const auto installed = kernel.replace_collision_resources_under_apply_fence(
      apply_fence_id, apply_scope, admission_gate, candidate);
  ASSERT_TRUE(installed.ok()) << installed.error().message;
  EXPECT_EQ(installed.value().generation, 2U)
      << "the fixture installed one bootstrap resource set before this Apply";
  EXPECT_EQ(installed.value().resources, candidate);
  ASSERT_TRUE(installed.value().coordinator_policy_snapshot);
  const auto candidate_policy = candidate.snapshot_publisher->collision_policy_snapshot();
  ASSERT_TRUE(candidate_policy.has_value());
  EXPECT_EQ(*installed.value().coordinator_policy_snapshot, *candidate_policy);
  ASSERT_TRUE(installed.value().coordinator_policy_index);

  const auto readback = kernel.collision_resource_set_readback();
  ASSERT_TRUE(readback.ok()) << readback.error().message;
  EXPECT_EQ(readback.value(), installed.value());
  EXPECT_EQ(readback.value().coordinator_policy_snapshot,
            installed.value().coordinator_policy_snapshot);
  EXPECT_EQ(readback.value().coordinator_policy_index,
            installed.value().coordinator_policy_index);

  auto replacement = make_owned_collision_pipeline_resources();
  admission_gate.held_scope = make_collision_apply_fence_scope("resource:other");
  const auto wrong_scope = kernel.replace_collision_resources_under_apply_fence(
      apply_fence_id, apply_scope, admission_gate, replacement);
  ASSERT_FALSE(wrong_scope.ok());
  EXPECT_EQ(wrong_scope.error().details.at("reason"), "configuration_fence_not_held");
  EXPECT_EQ(kernel.collision_resource_set_readback().value(), installed.value());

  admission_gate.held_scope = apply_scope;
  replacement.validation_engines.push_back(nullptr);
  const auto sparse_candidate = kernel.replace_collision_resources_under_apply_fence(
      apply_fence_id, apply_scope, admission_gate, replacement);
  ASSERT_FALSE(sparse_candidate.ok());
  EXPECT_EQ(sparse_candidate.error().details.at("reason"),
            "collision_resource_candidate_engine_missing");
  EXPECT_EQ(kernel.collision_resource_set_readback().value(), installed.value());

  replacement.validation_engines.pop_back();
  replacement.runtime_monitor_engine = replacement.validation_engines.front();
  const auto invalid_candidate = kernel.replace_collision_resources_under_apply_fence(
      apply_fence_id, apply_scope, admission_gate, replacement);
  ASSERT_FALSE(invalid_candidate.ok());
  EXPECT_EQ(invalid_candidate.error().details.at("reason"),
            "collision_resource_candidate_invalid");
  EXPECT_EQ(kernel.collision_resource_set_readback().value(), installed.value());
}

TEST_F(ExecutionKernelTest, CollisionResourcesReadbackRequiresKernelOwnedPublisher) {
  const auto readback = kernel.collision_resource_set_readback();
  ASSERT_FALSE(readback.ok());
  EXPECT_EQ(readback.error().details.at("reason"),
            "collision_resource_publisher_owner_missing");
}

TEST_F(ExecutionKernelTest, IndependentGripperParticipantHasNoMotionLifecycleOrWorkers) {
  GripperParticipantEngine gripper(context, clock);
  const auto request = make_test_gripper_request();
  const auto admitted = gripper.command_gripper(request);
  ASSERT_TRUE(admitted.accepted);
  const auto observed = gripper.get_gripper_status(request.gripper_id);
  const auto invalidations = context.gripper_invalidation_count.load();
  {
    ExecutionKernel arm(planner, context, execution, clock, scheduler, cache);
    configure_authoritative_collision_validation(arm, collision_world_publisher, collision_engine);
    EXPECT_FALSE(arm.command_gripper(request).accepted);
    const auto motion = prepare_and_dispatch(arm, make_test_request("independent_arm"));
    ASSERT_TRUE(motion.ok());
    ASSERT_TRUE(arm.wait_for_completion(1'000'000'000));
    EXPECT_TRUE(arm.acknowledge_completed(motion.value()).accepted);
    EXPECT_EQ(gripper.get_gripper_status(request.gripper_id).generation, observed.generation);
    EXPECT_EQ(context.gripper_invalidation_count.load(), invalidations);
    const auto arm_state = arm.get_status();
    EXPECT_FALSE(
        gripper.PrepareMotion(prepared_motion_request(make_test_request("wrong_target"))).ok());
    EXPECT_EQ(arm.get_status().execution_id, arm_state.execution_id);
    EXPECT_EQ(arm.get_status().state, arm_state.state);
  }
  EXPECT_EQ(context.gripper_invalidation_count.load(), invalidations);
  EXPECT_EQ(context.gripper_command_count.load(), 1);
}

TEST_F(ExecutionKernelTest, MotionParticipantRejectsPreparedCommandLifecycleWithoutEffect) {
  const auto dispatch = kernel.DispatchPreparedCapabilityCommand(
      1U, ContentDigest("command.digest"), ControllerGoalId("command.goal"),
      ControllerGoalGeneration(1U));
  EXPECT_FALSE(dispatch.ok());
  EXPECT_FALSE(kernel.ObservePreparedCapabilityCommand(1U).ok());
  EXPECT_FALSE(kernel.CancelPreparedCapabilityCommand(1U).ok());
  EXPECT_EQ(context.gripper_command_count.load(), 0);
  EXPECT_EQ(context.gripper_invalidation_count.load(), 0);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Execute_RejectsNonPositivePlanningTimeout) {
  auto req = make_test_request("invalid_planning_timeout");
  req.timeouts.planning_timeout_s = 0.0;

  auto result = prepare_and_dispatch(kernel, req);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorInvalidRequest);
  EXPECT_EQ(result.error().details.at("reason_code"), "invalid_planning_timeout");
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, MotionPreviewUsesExplicitStartAndDoesNotDispatchOrMutateLifecycle) {
  auto inventory = collision_world_publisher.current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(inventory.has_value());
  RobotStateDTO explicit_start{{"j1", "j2"}, {0.0, 0.0}, {}};
  context.current_state_value.positions_rad = {0.4, 0.5};
  context.next_fk = make_pose(0.1, 0.2, 0.3);

  auto result = kernel.preview_motion(make_test_request("non_dispatch_preview"),
                                      std::move(*inventory), explicit_start, 7U);

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(planner.last_request.planning_attempt_index, 7U);
  EXPECT_EQ(planner.last_request.start_state.positions[0],
            CoordinatePositionDTO{AngularPositionDTO{0.0}});
  EXPECT_EQ(planner.last_request.start_state.positions[1],
            CoordinatePositionDTO{AngularPositionDTO{0.0}});
  EXPECT_EQ(context.last_expand_trajectory.resources, make_test_trajectory().resources);
  EXPECT_EQ(context.last_expand_trajectory.joint_names, make_test_trajectory().joint_names);
  ASSERT_EQ(context.last_expand_trajectory.waypoints.size(), 2U);
  EXPECT_EQ(context.last_expand_trajectory.waypoints.front().positions_rad,
            std::vector<double>({0.0, 0.0}));
  EXPECT_EQ(context.last_expand_trajectory.waypoints.back().positions_rad,
            std::vector<double>({1.0, 1.0}));
  EXPECT_EQ(result.value().start_state.joint_names, explicit_start.joint_names);
  EXPECT_EQ(result.value().start_state.positions_rad, explicit_start.positions_rad);
  EXPECT_EQ(result.value().start_state.velocities_rad_s, explicit_start.velocities_rad_s);
  EXPECT_EQ(result.value().final_state.joint_names, std::vector<std::string>({"j1", "j2"}));
  EXPECT_EQ(result.value().final_state.positions_rad, std::vector<double>({1.0, 1.0}));
  EXPECT_EQ(result.value().final_tcp_pose, context.next_fk);
  const ProjectionLeaseBindingDTO expected_projection_binding{
      test_projection_revision(), ProjectionLeaseId("mock_current_lease")};
  EXPECT_EQ(result.value().projection_binding, expected_projection_binding);
  EXPECT_EQ(cache.size(), 0U);
}

TEST_F(ExecutionKernelTest, MotionPreviewTimeoutRetainsUnderlyingFailureDiagnostics) {
  for (const bool already_wrapped : {false, true}) {
    auto inventory = collision_world_publisher.current_scene_snapshot_lease_inventory();
    ASSERT_TRUE(inventory.has_value());
    ErrorDTO underlying{kErrorValidationLimitsViolation, "Original validation failure",
                        {{"reason", "original_limit_failure"},
                         {"failure_stage", "cartesian_output_validation"},
                         {"joint", "j2"}}};
    if (already_wrapped) {
      underlying.details["underlying_error_code"] = "deepest_code";
      underlying.details["underlying_error_message"] = "Deepest failure";
      underlying.details["underlying_error_reason"] = "deepest_reason";
    }
    planner.next_result = PlanResultDTO{};
    planner.next_result.error = underlying;
    planner.on_plan = [this](int) { clock.advance(200'000'000); };
    auto request = make_test_request("preview_timeout_details");
    request.timeouts.planning_timeout_s = 0.1;

    const auto result = kernel.preview_motion(
        request, std::move(*inventory), RobotStateDTO{{"j1", "j2"}, {0.0, 0.0}, {}}, 0U);

    ASSERT_FALSE(result.ok());
    const auto &error = result.error();
    EXPECT_EQ(error.code, kErrorPlanningTimeout);
    EXPECT_EQ(error.message, "Motion preview planning timed out");
    EXPECT_EQ(error.details.at("reason"), "motion_preview_planning_timeout");
    EXPECT_EQ(error.details.at("failure_stage"), "cartesian_output_validation");
    EXPECT_EQ(error.details.at("joint"), "j2");
    EXPECT_EQ(error.details.at("underlying_error_code"),
              already_wrapped ? "deepest_code" : underlying.code);
    EXPECT_EQ(error.details.at("underlying_error_message"),
              already_wrapped ? "Deepest failure" : underlying.message);
    EXPECT_EQ(error.details.at("underlying_error_reason"),
              already_wrapped ? "deepest_reason" : "original_limit_failure");
    EXPECT_EQ(error.details.at("planning_elapsed_ns"), "200000000");
    EXPECT_EQ(error.details.at("planning_budget_ns"), "100000000");
    EXPECT_EQ(execution.execute_count, 0);
    EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  }
}

TEST_F(ExecutionKernelTest, MotionPreviewTimeoutAfterSuccessDoesNotInventFailureStage) {
  auto inventory = collision_world_publisher.current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(inventory.has_value());
  planner.on_plan = [this](int) { clock.advance(200'000'000); };
  auto request = make_test_request("preview_late_success");
  request.timeouts.planning_timeout_s = 0.1;

  const auto result = kernel.preview_motion(
      request, std::move(*inventory), RobotStateDTO{{"j1", "j2"}, {0.0, 0.0}, {}}, 0U);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorPlanningTimeout);
  EXPECT_EQ(result.error().details.at("reason"), "motion_preview_planning_timeout");
  EXPECT_EQ(result.error().details.at("planning_elapsed_ns"), "200000000");
  EXPECT_EQ(result.error().details.at("planning_budget_ns"), "100000000");
  EXPECT_EQ(result.error().details.count("failure_stage"), 0U);
  EXPECT_EQ(result.error().details.count("underlying_error_code"), 0U);
  EXPECT_EQ(result.error().details.count("underlying_error_message"), 0U);
  EXPECT_EQ(result.error().details.count("underlying_error_reason"), 0U);
  EXPECT_EQ(execution.execute_count, 0);
}

// --- Never-dispatched prepared failure fixtures ---
// Sealed prepared-motion authority for the fixture request. Only identity
// and the ordinary Stop plan are checked before planning; these failures stop
// at planning, before any prepared product exists.

[[nodiscard]] ResolvedStopBasePlanDTO prepared_motion_stop_base_plan() {
  const auto path =
      std::filesystem::path(__FILE__).parent_path() / "golden" / "resolved_stop_base_plan.v1.json";
  std::ifstream stream(path, std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (bytes.ends_with('\n')) bytes.pop_back();
  auto decoded = DecodeResolvedStopBasePlan(bytes);
  if (!decoded.ok()) throw std::logic_error(decoded.error().message);
  auto base = std::move(decoded.value());
  base.retained_bundle_id = test_projection_revision().retained_bundle_id;
  base.graph_revision = test_projection_revision().graph_revision;
  auto& nominal = base.escalation_stages.front();
  const ProviderContractRefDTO contract{
      ProviderContractRoleId("stop.command"),
      SchemaId(std::string(kMotionControlledStopContractSchemaId)),
      SchemaRevision(std::string(kMotionControlledStopContractSchemaRevision))};
  nominal.containment_command.provider_contract_ref = contract;
  nominal.command_qualification.controlled_substitution_contract->provider_contract_ref = contract;
  JsonValueDTO::Array limits;
  for (const auto& resource : {std::string("joint.1"), std::string("joint.2")}) {
    limits.emplace_back(JsonObjectDTO{
        {"resource_id", JsonValueDTO{resource}},
        {"max_deceleration", JsonValueDTO{JsonObjectDTO{
                                 {"coordinate_kind", JsonValueDTO{std::string("angular_rad_s2")}},
                                 {"value_rad_s2", JsonValueDTO{2.0}}}}},
        {"settle_velocity_threshold",
         JsonValueDTO{JsonObjectDTO{{"coordinate_kind", JsonValueDTO{std::string("angular_rad_s")}},
                                    {"value_rad_s", JsonValueDTO{0.02}}}}}});
  }
  nominal.default_contract_parameters =
      JsonObjectDTO{{"stop_mode", JsonValueDTO{std::string("soft_on_path")}},
                    {"jerk_limits_enabled", JsonValueDTO{false}},
                    {"deceleration_time_s", JsonValueDTO{1.0}},
                    {"allow_natural_completion_on_short_path", JsonValueDTO{true}},
                    {"per_resource_limits", JsonValueDTO{std::move(limits)}}};
  auto current = MakeResolvedStopBasePlan(std::move(base));
  if (!current.ok()) throw std::logic_error(current.error().message);
  return std::move(current.value());
}

[[nodiscard]] RuntimeParticipantBinding prepared_motion_binding(
    const ResolvedStopBasePlanDTO& base) {
  const auto& target = base.escalation_stages.front().command_target;
  std::vector<RuntimeCapabilityProviderPathDTO> supporting_providers{
      RuntimeCapabilityProviderPathDTO{
          CapabilityId("planning.motion.joint_goal"), CapabilityDefinitionRevision(kTestDigestC),
          CapabilityProviderBindingId(kTestDigestD), CapabilityProviderBindingRevision(kTestDigestA),
          ProviderInstanceId("planner.1"), ProviderManifestId(kTestDigestB),
          ProviderManifestRevision(kTestDigestC), RuntimeUnitId("planner.runtime"),
          RuntimeEpoch("planner.epoch")}};
  std::vector<RuntimeUnitEpochBindingDTO> runtime_unit_epochs{
      {RuntimeUnitId("planner.runtime"), RuntimeEpoch("planner.epoch")},
      {base.source_runtime_unit_id, base.source_runtime_epoch},
      {target.runtime_unit_id, target.runtime_unit_epoch}};
  std::sort(
      runtime_unit_epochs.begin(), runtime_unit_epochs.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.runtime_unit_id < rhs.runtime_unit_id; });
  RuntimeParticipantBindingDTO value{
      ProgramId("program.prepared"),
      ProgramRevision(kTestDigestA),
      ProgramBindingId(kTestDigestB),
      base.retained_bundle_id,
      base.graph_revision,
      ComponentRoleId("robot"),
      TargetRoleId("arm"),
      ControllableComponentId("robot1"),
      ComponentTypeId("robot.type"),
      ComponentTypeRevision("robot.type.revision"),
      CommandTargetId("arm"),
      CapabilityId("execution.trajectory.execute"),
      CapabilityDefinitionRevision(kTestDigestC),
      std::nullopt,
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision")}},
      {RuntimeModelGroupBindingDTO{ModelGroupRoleId("arm.group"),
                                   ModelInstanceId("robot.model"),
                                   "arm_group",
                                   {"joint_1", "joint_2"}}},
      {ProgramFrameRoleBindingDTO{FrameRoleId("base"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision"), "robot_base"},
       ProgramFrameRoleBindingDTO{FrameRoleId("tcp"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision"), "robot_tcp"}},
      {RuntimeResourceBindingDTO{ResourceRoleId("joint.1"), ResourceId("joint.1"),
                                 std::string("joint_1"), "joint_1", CoordinateKind::kAngular},
       RuntimeResourceBindingDTO{ResourceRoleId("joint.2"), ResourceId("joint.2"),
                                 std::string("joint_2"), "joint_2", CoordinateKind::kAngular}},
      {},
      RuntimeControlPathDTO{base.control_domain_id,
                            base.control_domain_revision,
                            base.control_binding_id,
                            base.control_binding_revision,
                            ControlInterfaceContractId("control.interface"),
                            ControlInterfaceRevision("control.interface.revision"),
                            base.controller_topology_id,
                            base.topology_revision,
                            base.control_route_id,
                            base.activation_set_id,
                            base.active_control_configuration_id,
                            base.active_control_configuration_generation,
                            target.provider_instance_id,
                            target.provider_manifest_id,
                            target.provider_manifest_revision,
                            target.runtime_unit_id,
                            target.provider_runtime_epoch,
                            StateAuthorityId("state.measured"),
                            StateAuthorityClass::kMeasured,
                            base.stop_evidence_policy_id,
                            base.stop_evidence_policy_revision,
                            ContainmentGranularity::kResource,
                            std::nullopt},
      std::nullopt,
      std::move(supporting_providers),
      std::move(runtime_unit_epochs),
      {}};
  auto verified = verify_runtime_participant_binding(std::move(value));
  if (!verified.ok()) throw std::logic_error(verified.error().message);
  return std::move(verified.value());
}

[[nodiscard]] ControllerLifecycleExpectedDomain prepared_motion_expected_domain(
    const ResolvedStopBasePlanDTO& base, const RuntimeParticipantBinding& binding) {
  const auto& path = *binding.value().control_path;
  const std::vector<ControllerInterfaceInstanceDTO> interfaces{{ResourceId("joint.1"), "position"},
                                                               {ResourceId("joint.2"), "position"}};
  const ControllerRoleId controller("hardware");
  ControllerLifecycleExpectedDomainDTO value{
      base.control_domain_id,
      base.control_domain_revision,
      base.controller_topology_id,
      base.topology_revision,
      ProductId("product.control.arm"),
      ProductContractId("manyforge.control-domain-control.v2"),
      ProductContractRevision(kTestDigestD),
      ContentDigest(kTestDigestA),
      ContentDigest(kTestDigestB),
      base.source_runtime_unit_id,
      base.source_runtime_epoch,
      {base.controller_topology_id,
       base.topology_revision,
       {{controller,
         {{path.control_interface_contract_id,
           path.control_interface_revision,
           {ControllerClaimDirection::kCommand, ControllerClaimDirection::kState}}},
         {},
         {}}},
       {},
       {{base.activation_set_id, {controller}, {}}},
       {{base.control_route_id,
         path.control_interface_contract_id,
         path.control_interface_revision,
         {controller},
         {base.activation_set_id}}}},
      {base.control_domain_id,
       base.control_domain_revision,
       base.controller_topology_id,
       base.topology_revision,
       base.activation_set_id,
       {{path.control_binding_id, path.control_binding_revision, path.control_route_id}}},
      {{path.control_binding_id, path.control_binding_revision, path.control_interface_contract_id,
        path.control_interface_revision, interfaces, interfaces}},
      {{controller,
        ControllerImplementationId("implementation.hardware"),
        ControllerImplementationRevision(kTestDigestC),
        "controller_interface.ControllerInterface",
        ContentDigest(kTestDigestD),
        interfaces,
        interfaces,
        {},
        {}}},
      {{path.control_binding_id, path.control_binding_revision, path.control_route_id,
        EndpointRoleId("endpoint.command"), "controller.command", true, false}}};
  auto verified = verify_controller_lifecycle_expected_domain(std::move(value));
  if (!verified.ok()) throw std::logic_error(verified.error().message);
  return std::move(verified.value());
}

[[nodiscard]] ResolvedPreparedMotionExecutionRequestDTO prepared_motion_request(
    ExecuteRequestDTO request) {
  const auto base = prepared_motion_stop_base_plan();
  auto binding = prepared_motion_binding(base);
  const auto domain = prepared_motion_expected_domain(base, binding);
  auto readiness = verify_runtime_participant_stop_readiness(
      {binding.value().controllable_component_id, ComponentLifecycleGeneration(1U),
       domain.value(), base},
      binding, domain);
  if (!readiness.ok()) throw std::logic_error(readiness.error().message);
  const auto& sealed = binding.value();
  auto& resolved = request.binding;
  resolved.operation_id = OperationId("operation.prepared");
  resolved.participant_id = OperationParticipantId("participant.prepared");
  if (resolved.program_run_id.empty()) resolved.program_run_id = ProgramRunId("run.prepared");
  resolved.program_id = sealed.program_id;
  resolved.program_revision = sealed.program_revision;
  resolved.program_binding_id = sealed.program_binding_id;
  resolved.controllable_component_id = sealed.controllable_component_id;
  resolved.command_target_id = sealed.command_target_id;
  resolved.capability_id = sealed.capability_id;
  // The motion product joins the resolved request to the sealed binding: bind
  // the same component, planner, control path, runtime units and resources.
  resolved.component_type_id = sealed.component_type_id;
  resolved.component_type_revision = sealed.component_type_revision;
  resolved.capability_definition_revision = sealed.capability_definition_revision;
  const auto& planner_path = sealed.supporting_capability_provider_paths.front();
  resolved.planner_capability_binding =
      PlannerCapabilityBindingDTO{planner_path.capability_id,
                                  planner_path.capability_definition_revision,
                                  planner_path.capability_provider_binding_id,
                                  planner_path.capability_provider_binding_revision,
                                  planner_path.provider_instance_id,
                                  planner_path.provider_manifest_id,
                                  planner_path.provider_manifest_revision,
                                  planner_path.provider_runtime_unit_id,
                                  planner_path.provider_runtime_epoch};
  const auto& path = *sealed.control_path;
  resolved.control_domain_id = path.control_domain_id;
  resolved.control_domain_revision = path.control_domain_revision;
  resolved.control_binding_id = path.control_binding_id;
  resolved.control_binding_revision = path.control_binding_revision;
  resolved.control_interface_contract_id = path.control_interface_contract_id;
  resolved.control_interface_revision = path.control_interface_revision;
  resolved.controller_topology_id = path.controller_topology_id;
  resolved.topology_revision = path.topology_revision;
  resolved.control_route_id = path.control_route_id;
  resolved.activation_set_id = path.activation_set_id;
  resolved.active_control_configuration_id = path.active_control_configuration_id;
  resolved.active_control_configuration_generation = path.active_control_configuration_generation;
  resolved.provider_instance_id = path.provider_instance_id;
  resolved.provider_manifest_id = path.provider_manifest_id;
  resolved.provider_manifest_revision = path.provider_manifest_revision;
  resolved.provider_runtime_epoch = path.provider_runtime_epoch;
  resolved.runtime_unit_bindings.clear();
  for (const auto& unit : sealed.runtime_unit_epochs) {
    resolved.runtime_unit_bindings.push_back({unit.runtime_unit_id, unit.runtime_epoch});
  }
  resolved.resources.clear();
  for (const auto& resource : sealed.resources) resolved.resources.push_back(resource.resource_id);
  constexpr double kDeploymentMaximumTcpLinearSpeedMps = 1.0;
  request.speed_profile.max_tcp_linear_speed_mps = kDeploymentMaximumTcpLinearSpeedMps;
  const OperationRevision revision(1U);
  const std::vector<HardStopResourceLimitDTO> limits{
      {ResourceId("joint.1"), AngularVelocityDTO{0.02}},
      {ResourceId("joint.2"), AngularVelocityDTO{0.02}}};
  auto stop = MakePreparedParticipantStopPlan(
      readiness.value().value().resolved_stop_base_plan,
      PreparedParticipantStopPlanContextDTO{
          resolved.operation_id, revision, resolved.participant_id, sealed.program_id,
          sealed.program_revision, sealed.program_binding_id, resolved.program_run_id,
          std::nullopt, ParticipantStopProfilePurposeDTO::kOrdinary, std::nullopt,
          MotionInterruptNodeCategory::kGeneric},
      sealed.resources, limits);
  if (!stop.ok()) throw std::logic_error(stop.error().details.at("reason"));
  return ResolvedPreparedMotionExecutionRequestDTO{
      std::move(request),
      revision,
      std::move(binding),
      std::move(readiness.value()),
      std::nullopt,
      kDeploymentMaximumTcpLinearSpeedMps,
      limits,
      MotionInterruptNodeCategory::kGeneric,
      std::make_shared<const PreparedParticipantStopPlanDTO>(std::move(stop.value())),
      nullptr};
}

Result<std::uint64_t> dispatch_when_prepared(ExecutionKernel& kernel, std::uint64_t execution_id) {
  // Dispatch the exact ready contract once preparation settles. A failed or
  // cancelled preparation is observed by the caller through the kernel.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto observed = kernel.ObservePreparedMotion(execution_id);
    if (!observed.ok()) break;
    const auto state = observed.value().state;
    if (state == ResolvedPreparedMotionExecutionStateDTO::kPrepared) {
      const auto dispatched = kernel.DispatchPreparedMotion(
          execution_id, observed.value().participant->prepared_contract_digest);
      if (!dispatched.ok()) return Result<std::uint64_t>::Err(dispatched.error());
      break;
    }
    if (state != ResolvedPreparedMotionExecutionStateDTO::kPreparing) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return Result<std::uint64_t>::Ok(execution_id);
}

Result<std::uint64_t> prepare_and_dispatch(ExecutionKernel& kernel,
                                           ResolvedPreparedMotionExecutionRequestDTO request) {
  auto prepared = kernel.PrepareMotion(std::move(request));
  if (!prepared.ok()) return prepared;
  return dispatch_when_prepared(kernel, prepared.value());
}

Result<std::uint64_t> prepare_and_dispatch(ExecutionKernel& kernel, ExecuteRequestDTO request) {
  return prepare_and_dispatch(kernel, prepared_motion_request(std::move(request)));
}

TEST_F(ExecutionKernelTest, NeverDispatchedPreparedFailureIsRetryableWithoutBackendReset) {
  // Live incident 2026-10-02: a Program motion whose IK had no solution
  // failed while preparing, before any dispatch. It never commanded the
  // robot, so the owner offers Retry instead of an unknown fault needing Reset.
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                            "RoboPlan could not resolve the pose goal",
                            {{"reason", "native_ik_no_solution"}}};
  planner.next_result = plan_err;
  const auto prepared = kernel.PrepareMotion(prepared_motion_request(make_test_request("ik")));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
  ASSERT_EQ(ended_as(kernel), "failed");
  const auto observed = kernel.ObservePreparedMotion(prepared.value());
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  EXPECT_EQ(observed.value().state, ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed);
  EXPECT_FALSE(observed.value().last_dispatched_participant);
  scheduler.advance_to(clock.now());

  const auto assessment = kernel.get_recovery_assessment();
  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kFailedValidation);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kRetryReady);
  EXPECT_TRUE(assessment.retry_admissible);
  EXPECT_EQ(assessment.allowed_actions,
            (std::vector<RecoveryAction>{RecoveryAction::kRetryAfterSceneRepair,
                                         RecoveryAction::kResetCycle}));
  const auto reset_count_before = execution.reset_count;
  const auto retried =
      apply_recovery_action_after_projection(kernel, RecoveryAction::kRetryAfterSceneRepair);
  ASSERT_TRUE(retried.accepted) << (retried.error ? retried.error->message : std::string{});
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.reset_count, reset_count_before);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, AutomaticPlanningRetryUsesExactPreparedFailureCause) {
  for (const auto* code :
       {kErrorPlanningFailed, kErrorPlanningTimeout, kErrorIkFailed, kErrorCartesianIncomplete,
        kErrorContactPolicyViolation, kErrorSceneChangedDuringValidation, kErrorValidationDeferred,
        kErrorValidationCollision, kErrorExecutionRejected}) {
    SCOPED_TRACE(code);
    PlanResultDTO failed_plan;
    failed_plan.error = ErrorDTO{code, "failed before dispatch", {{"reason", "validation_failed"}}};
    planner.next_result = failed_plan;
    const auto prepared = kernel.PrepareMotion(prepared_motion_request(make_test_request(code)));
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
    const auto observed = kernel.ObservePreparedMotion(prepared.value());
    ASSERT_TRUE(observed.ok());
    EXPECT_EQ(observed.value().state, ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed);
    EXPECT_FALSE(observed.value().last_dispatched_participant);
    scheduler.advance_to(clock.now());
    const auto assessment = kernel.get_recovery_assessment();
    EXPECT_TRUE(assessment.retry_admissible);
    EXPECT_EQ(assessment.retry_eligibility.eligible,
              std::string_view(code) == kErrorPlanningFailed ||
                  std::string_view(code) == kErrorPlanningTimeout ||
                  std::string_view(code) == kErrorIkFailed ||
                  std::string_view(code) == kErrorCartesianIncomplete);
    EXPECT_EQ(assessment.retry_eligibility.effect, MotionRetryEffect::kKnownNoEffect);
    EXPECT_EQ(execution.execute_count, 0);
    // The automatic subset never narrows existing operator repair.
    ASSERT_TRUE(
        apply_recovery_action_after_projection(kernel, RecoveryAction::kRetryAfterSceneRepair)
            .accepted);
  }
}

TEST_F(ExecutionKernelTest, AutomaticValidationRetryRequiresAuthoritativeBlockingCollision) {
  std::optional<MotionRetryEligibilityDTO> failure_candidate;
  const auto subscription = kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type == KernelEventType::kParticipantSettled)
      failure_candidate = event.retry_eligibility;
  });
  collision_engine->default_result = make_authoritative_collision_result();
  const auto prepared =
      kernel.PrepareMotion(prepared_motion_request(make_test_request("automatic_collision")));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
  ASSERT_EQ(ended_as(kernel), "failed");
  scheduler.advance_to(clock.now());
  const auto assessment = kernel.get_recovery_assessment();
  EXPECT_TRUE(assessment.retry_eligibility.eligible);
  EXPECT_EQ(assessment.retry_eligibility.cause, MotionRetryCause::kPlanningNoEffect);
  EXPECT_EQ(assessment.retry_eligibility.effect, MotionRetryEffect::kKnownNoEffect);
  ASSERT_TRUE(failure_candidate.has_value());
  EXPECT_EQ(*failure_candidate, assessment.retry_eligibility);
  EXPECT_EQ(execution.execute_count, 0);
  kernel.unsubscribe_events(subscription);
}

TEST_F(ExecutionKernelTest, AutomaticValidationRetryRejectsInconclusiveOrNonCollisionFailure) {
  for (const auto* reason : {"collision_validation_cancelled", "collision_validation_wall_timeout",
                             "stale_observed_robot_state", "contact_policy_violation"}) {
    SCOPED_TRACE(reason);
    // Even a backend validator using validation_collision and convincing
    // diagnostics cannot manufacture the authoritative collision decision.
    context.next_validation = ValidationResultDTO{
        false,
        ErrorDTO{kErrorValidationCollision, "validation is inconclusive", {{"reason", reason}}}};
    const auto prepared = kernel.PrepareMotion(prepared_motion_request(make_test_request(reason)));
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
    ASSERT_EQ(ended_as(kernel), "failed");
    scheduler.advance_to(clock.now());
    const auto assessment = kernel.get_recovery_assessment();
    EXPECT_TRUE(assessment.retry_admissible);
    EXPECT_FALSE(assessment.retry_eligibility.eligible);
    EXPECT_EQ(assessment.retry_eligibility.reason,
              MotionRetryIneligibilityReason::kValidationNotConclusiveCollision);
    EXPECT_EQ(execution.execute_count, 0);
    ASSERT_TRUE(
        apply_recovery_action_after_projection(kernel, RecoveryAction::kRetryAfterSceneRepair)
            .accepted);
  }
}

TEST_F(ExecutionKernelTest, AutomaticValidationRetryRejectsAuthoritativeInconclusiveResult) {
  std::optional<MotionRetryEligibilityDTO> failure_candidate;
  const auto subscription = kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type == KernelEventType::kParticipantSettled)
      failure_candidate = event.retry_eligibility;
  });
  auto inconclusive = make_authoritative_collision_result();
  inconclusive.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
  inconclusive.decision = CollisionDecisionResult::kInconclusiveFail;
  collision_engine->default_result = inconclusive;
  const auto prepared =
      kernel.PrepareMotion(prepared_motion_request(make_test_request("automatic_inconclusive")));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
  ASSERT_EQ(ended_as(kernel), "failed");
  scheduler.advance_to(clock.now());
  const auto assessment = kernel.get_recovery_assessment();
  EXPECT_TRUE(assessment.retry_admissible);
  EXPECT_FALSE(assessment.retry_eligibility.eligible);
  EXPECT_EQ(assessment.retry_eligibility.reason,
            MotionRetryIneligibilityReason::kValidationNotConclusiveCollision);
  ASSERT_TRUE(failure_candidate.has_value());
  EXPECT_EQ(*failure_candidate, assessment.retry_eligibility);
  EXPECT_EQ(execution.execute_count, 0);
  kernel.unsubscribe_events(subscription);
}

TEST_F(ExecutionKernelTest, AutomaticValidationRetryRejectsGenericEngineFailureWithoutCollision) {
  CollisionResult failed_query;
  // Engine failures use an inconclusive decision, preserved by policy merging;
  // kFail with no blocking pairs is reclassified as a clear policy result.
  failed_query.engine_status = CollisionEngineStatus::kEngineError;
  failed_query.decision = CollisionDecisionResult::kInconclusiveFail;
  collision_engine->default_result = failed_query;
  const auto prepared =
      kernel.PrepareMotion(prepared_motion_request(make_test_request("automatic_generic_failure")));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
  ASSERT_EQ(ended_as(kernel), "failed");
  scheduler.advance_to(clock.now());
  const auto assessment = kernel.get_recovery_assessment();
  EXPECT_TRUE(assessment.retry_admissible);
  EXPECT_FALSE(assessment.retry_eligibility.eligible);
  EXPECT_EQ(assessment.retry_eligibility.reason,
            MotionRetryIneligibilityReason::kValidationNotConclusiveCollision);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, AutomaticRetryRejectsPlanningCodeAfterDispatch) {
  std::optional<MotionRetryEligibilityDTO> failure_candidate;
  const auto subscription = kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type == KernelEventType::kParticipantSettled)
      failure_candidate = event.retry_eligibility;
  });
  execution.next_terminal_state = ExecutionState::kFailed;
  execution.execution_generation = 1U;
  execution.snapshot.error = ErrorDTO{kErrorPlanningFailed, "dispatched command failed", {}};
  const auto dispatched = execute_sync(make_test_request("automatic_dispatched_planning_code"));
  ASSERT_TRUE(dispatched.ok());
  const auto observed = kernel.ObservePreparedMotion(dispatched.value());
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().last_dispatched_participant);
  EXPECT_EQ(observed.value().state, ResolvedPreparedMotionExecutionStateDTO::kDispatched);
  const auto status = kernel.get_status();
  ASSERT_TRUE(status.fact);
  EXPECT_EQ(status.fact->kind, ParticipantExecutionFactKind::kLocalFailure);
  EXPECT_EQ(status.fact->effect, ParticipantEffectKnowledge::kEffectSettled);
  ASSERT_TRUE(status.fact->error);
  EXPECT_EQ(status.fact->error->code, kErrorPlanningFailed);
  scheduler.advance_to(clock.now());
  EXPECT_FALSE(kernel.get_recovery_assessment().retry_eligibility.eligible);
  ASSERT_TRUE(failure_candidate.has_value());
  EXPECT_FALSE(failure_candidate->eligible);
  EXPECT_EQ(execution.execute_count, 1);
  kernel.unsubscribe_events(subscription);
}

TEST_F(ExecutionKernelTest, FailureEventCarriesRetryProofBeforeWorkerAndAssessmentCompletion) {
  std::optional<KernelEvent> failure_event;
  std::optional<ResolvedPreparedMotionExecutionStateDTO> state_at_failure;
  std::optional<KernelRecoveryAssessmentDTO> assessment_at_failure;
  const auto subscription = kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kParticipantSettled || !event.participant_fact ||
        event.participant_fact->kind != ParticipantExecutionFactKind::kLocalFailure)
      return;
    failure_event = event;
    const auto prepared = kernel.ObservePreparedMotion(event.execution_id);
    if (prepared.ok()) state_at_failure = prepared.value().state;
    assessment_at_failure = kernel.get_recovery_assessment();
  });
  for (const auto* code : {kErrorPlanningFailed, kErrorIkFailed, kErrorCartesianIncomplete}) {
    SCOPED_TRACE(code);
    failure_event.reset();
    state_at_failure.reset();
    assessment_at_failure.reset();
    PlanResultDTO failed_plan;
    failed_plan.error = ErrorDTO{code, "no path found", {}};
    planner.next_result = failed_plan;
    const auto prepared =
        kernel.PrepareMotion(prepared_motion_request(make_test_request("failure_event_proof")));
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
    ASSERT_TRUE(failure_event.has_value());
    ASSERT_TRUE(failure_event->retry_eligibility.has_value());
    EXPECT_TRUE(failure_event->retry_eligibility->eligible);
    EXPECT_EQ(failure_event->retry_eligibility->cause, MotionRetryCause::kPlanningNoEffect);
    EXPECT_EQ(state_at_failure, ResolvedPreparedMotionExecutionStateDTO::kPreparing);
    ASSERT_TRUE(assessment_at_failure.has_value());
    EXPECT_NE(assessment_at_failure->execution_state, ExecutionState::kFailed);
    const auto completed = kernel.ObservePreparedMotion(prepared.value());
    ASSERT_TRUE(completed.ok());
    EXPECT_EQ(completed.value().state, ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed);
    scheduler.advance_to(clock.now());
    EXPECT_EQ(kernel.get_recovery_assessment().retry_eligibility,
              *failure_event->retry_eligibility);
    ASSERT_TRUE(
        apply_recovery_action_after_projection(kernel, RecoveryAction::kRetryAfterSceneRepair)
            .accepted);
  }
  kernel.unsubscribe_events(subscription);
}

TEST_F(ExecutionKernelTest, NeverDispatchedPreparedFailureRetryDeadlineFollowsMeasuredFreshness) {
  // Retry readiness of a no-effect failure keeps the same observation
  // freshness rule as the Stop-backed retry classes: newer measured samples
  // extend the published validity deadline without churning the revision,
  // and a stale source lets the deadline elapse (a press then refuses at
  // recovery admission; the Program offer itself carries no expiry).
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                            "RoboPlan could not resolve the pose goal",
                            {{"reason", "native_ik_no_solution"}}};
  planner.next_result = plan_err;
  const auto prepared =
      kernel.PrepareMotion(prepared_motion_request(make_test_request("ik_fresh")));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
  ASSERT_EQ(ended_as(kernel), "failed");

  constexpr std::int64_t kStepNs = 20'000'000;  // the default observation poll period
  const auto publish_and_poll = [&] {
    execution.publish_observed_state(clock.now(), {1.0, 1.0}, {0.0, 0.0});
    scheduler.advance_to(clock.now());
    clock.advance(kStepNs);
    scheduler.advance_to(clock.now());
  };
  // Qualify the measured source (default dwell 200 ms, two samples).
  for (int step = 0; step < 15; ++step) publish_and_poll();
  execution.publish_observed_state(clock.now(), {1.0, 1.0}, {0.0, 0.0});
  clock.advance(kStepNs);
  scheduler.advance_to(clock.now());
  const auto fresh = kernel.get_recovery_assessment();
  ASSERT_EQ(fresh.recovery_class, RecoveryClass::kFailedValidation);
  ASSERT_EQ(fresh.status, RecoveryAssessmentStatus::kRetryReady);
  ASSERT_TRUE(fresh.observed_state_fresh);
  ASSERT_GT(fresh.observed_state_valid_until_ns, clock.now());

  // Fresh samples keep the published deadline ahead of the clock across
  // several of their own freshness budgets, at one stable revision.
  const auto budget_ns = fresh.observed_state_valid_until_ns - (clock.now() - kStepNs);
  ASSERT_GT(budget_ns, 0);
  const auto steps = static_cast<int>(3 * budget_ns / kStepNs) + 3;
  for (int step = 0; step < steps; ++step) {
    publish_and_poll();
    const auto current = kernel.get_recovery_assessment();
    EXPECT_EQ(current.assessment_revision, fresh.assessment_revision);
    EXPECT_EQ(current.status, RecoveryAssessmentStatus::kRetryReady);
    EXPECT_GT(current.observed_state_valid_until_ns, clock.now() - kStepNs);
  }
  execution.publish_observed_state(clock.now(), {1.0, 1.0}, {0.0, 0.0});
  scheduler.advance_to(clock.now());
  const auto extended = kernel.get_recovery_assessment();
  EXPECT_GT(extended.observed_state_valid_until_ns, clock.now());
  EXPECT_GT(extended.observed_state_valid_until_ns,
            fresh.observed_state_valid_until_ns + 2 * budget_ns);
  EXPECT_EQ(extended.assessment_revision, fresh.assessment_revision);

  // The source goes stale: the Kernel-owned deadline elapses and the
  // published assessment says so; nothing extends it.
  clock.advance(extended.observed_state_valid_until_ns - clock.now());
  scheduler.advance_to(clock.now());
  clock.advance(kStepNs);
  scheduler.advance_to(clock.now());
  const auto stale = kernel.get_recovery_assessment();
  EXPECT_FALSE(stale.observed_state_fresh);
  EXPECT_GT(stale.assessment_revision, fresh.assessment_revision);
  EXPECT_LE(stale.observed_state_valid_until_ns, clock.now());
}

#if GTEST_HAS_DEATH_TEST && defined(__unix__)
TEST_F(ExecutionKernelTest, RecoveryObservationPollAllowsReentrantLifecycleAdmission) {
  // A fresh child bounds the old self-deadlock without leaving a hung thread
  // in the suite. Product time is entirely driven by the existing fake clock.
  const auto previous_style = ::testing::FLAGS_gtest_death_test_style;
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      ([&] {
        ::alarm(10U);
        const auto verify = [&] {
          PlanResultDTO failed;
          failed.error = ErrorDTO{kErrorIkFailed, "no path", {}};
          planner.next_result = failed;
          const auto prepared = kernel.PrepareMotion(
              prepared_motion_request(make_test_request("reentrant_recovery_poll")));
          ASSERT_TRUE(prepared.ok());
          ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
          ASSERT_EQ(ended_as(kernel), "failed");
          scheduler.advance_to(clock.now());
          install_at_rest_recovery_observation();
          const auto before = kernel.get_recovery_assessment();
          ASSERT_TRUE(before.observed_state_fresh);
          ASSERT_TRUE(before.observed_state_at_rest);
          // Use the existing pure terminal reducer's typed empty-Full-Stop
          // fixture. The subscriber probes lifecycle reentry with a foreign
          // candidate, never with missing strong certificate identities.
          const auto certificate = ReduceStopTerminal(
              {StopCertificateId("reentrant.poll.certificate"), StopCertificateRevision(1U),
               StopId("reentrant.poll.stop"), StopGeneration(1U),
               {StopRequestedSelectorKindDTO::kExplicitFull, ContentDigest(kTestDigestA),
                std::nullopt, std::nullopt},
               StopResolutionDispositionDTO::kExact,
               StopScopeResolutionReasonDTO::kExplicitFullStop, {},
               {{1U, StopResolutionDispositionDTO::kExact,
                 StopScopeResolutionReasonDTO::kExplicitFullStop, ContentDigest(kTestDigestB),
                 {}, {}, {}}},
               {}, {}, {}, std::nullopt, std::nullopt, std::nullopt, clock.now(), std::nullopt});
          ASSERT_TRUE(certificate.ok()) << certificate.error().message;
          unsigned reentered = 0U;
          std::optional<KernelEvent> published;
          const auto subscription = kernel.subscribe_events([&](const KernelEvent& event) {
            const auto trigger = event.details.find("trigger");
            if (event.type != KernelEventType::kRecoveryAssessmentChanged ||
                trigger == event.details.end() ||
                trigger->second != "observed_state_publication_poll")
              return;
            published = event;
            // This is the same lifecycle-taking boundary reached by the real
            // addressed fallback subscriber. A foreign ID refuses without an
            // effect after acquiring the existing lifecycle mutex.
            const auto nested = kernel.PrepareColdMotionAfterStop(
                std::numeric_limits<std::uint64_t>::max(), certificate.value());
            ASSERT_FALSE(nested.ok());
            EXPECT_EQ(nested.error().details.at("reason"), "spring_return_candidate_stale");
            ++reentered;
          });
          execution.publish_observed_state(clock.now(), {1.0, 1.0}, {1.0, 0.0});
          clock.advance(20'000'000);
          scheduler.advance_to(clock.now());
          EXPECT_EQ(reentered, 1U);
          ASSERT_TRUE(published);
          const auto after = kernel.get_recovery_assessment();
          EXPECT_EQ(published->execution_id, prepared.value());
          EXPECT_EQ(published->details.at("assessment_revision"),
                    std::to_string(after.assessment_revision));
          EXPECT_EQ(published->details.at("status"), recovery_assessment_status_name(after.status));
          EXPECT_GT(after.assessment_revision, before.assessment_revision);
          // Recovery freshness includes at-rest qualification; moving input
          // revokes it immediately even though its timestamp is recent.
          EXPECT_FALSE(after.observed_state_fresh);
          EXPECT_FALSE(after.observed_state_at_rest);
          EXPECT_EQ(after.recovery_class, before.recovery_class);
          EXPECT_EQ(execution.execute_count, 0);
          EXPECT_EQ(execution.reset_count, 0);
          EXPECT_EQ(planner.plan_count, 1);
          kernel.unsubscribe_events(subscription);
        };
        verify();
        if (::testing::Test::HasFailure()) {
          // GTest suppresses its normal child assertion printer. Preserve the
          // exact failed assertions on the death-test stderr channel.
          const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
          if (info != nullptr) {
            const auto* result = info->result();
            for (int index = 0; index < result->total_part_count(); ++index) {
              const auto& failure = result->GetTestPartResult(index);
              if (failure.failed()) {
                std::fprintf(stderr, "%s:%d: %s\n",
                             failure.file_name() != nullptr ? failure.file_name() : "test",
                             failure.line_number(), failure.message());
              }
            }
          }
          std::fflush(stderr);
        }
        ::alarm(0U);
        ::_exit(::testing::Test::HasFailure() ? 1 : 0);
      }()),
      ::testing::ExitedWithCode(0), "");
  ::testing::FLAGS_gtest_death_test_style = previous_style;
}
#endif

TEST_F(ExecutionKernelTest, DispatchedNonValidationFailureStaysResetRequired) {
  // Without exact never-dispatched preparation evidence a non-validation
  // failure keeps the conservative classification.
  execution.next_terminal_state = ExecutionState::kFailed;
  execution.snapshot.error = ErrorDTO{kErrorExecutionRejected, "Controller aborted", {}};
  execute_sync(make_test_request("failed_dispatched_assessment"));
  ASSERT_EQ(ended_as(kernel), "failed");
  scheduler.advance_to(clock.now());

  const auto assessment = kernel.get_recovery_assessment();
  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kUnknownFault);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kResetRequired);
  EXPECT_FALSE(
      apply_recovery_action_after_projection(kernel, RecoveryAction::kRetryAfterSceneRepair)
          .accepted);
}

TEST_F(ExecutionKernelTest, MotionPreviewAfterDispatchedFailureStillRequiresIdle) {
  // Only an exact prepared session that failed before dispatch relaxes the
  // idle requirement. A dispatched failed execution has no such evidence.
  execution.next_terminal_state = ExecutionState::kFailed;
  execution.snapshot.error = ErrorDTO{kErrorExecutionRejected, "Controller aborted", {}};
  execute_sync(make_test_request("failed_dispatched"));
  ASSERT_EQ(ended_as(kernel), "failed");
  auto inventory = collision_world_publisher.current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(inventory.has_value());
  RobotStateDTO explicit_start{{"j1", "j2"}, {0.0, 0.0}, {}};

  auto result = kernel.preview_motion(make_test_request("preview_after_failure"),
                                      std::move(*inventory), explicit_start, 0U);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "motion_preview_participant_not_idle");
  EXPECT_EQ(ended_as(kernel), "failed");
}

TEST_F(ExecutionKernelTest, Execute_RejectsNonPositiveExecutionTimeout) {
  auto req = make_test_request("invalid_execution_timeout");
  req.timeouts.execution_timeout_s = 0.0;

  auto result = prepare_and_dispatch(kernel, req);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorInvalidRequest);
  EXPECT_EQ(result.error().details.at("reason_code"), "invalid_execution_timeout");
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_ReturnsExecutionId) {
  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 1u);
  EXPECT_EQ(ended_as(kernel), "completed");
}

TEST_F(ExecutionKernelTest, Execute_BackendRejectedDispatchPreservesExactBackendCause) {
  execution.next_terminal_state = ExecutionState::kFailed;
  execution.snapshot.error = ErrorDTO{kErrorExecutionRejected,
                                      "Execution progress authority could not be armed",
                                      {{"reason", "execution_progress_publication_rejected"},
                                       {"publication_reason", "execution_generation_retired"}}};

  auto result = execute_sync(make_test_request("backend_dispatch_rejected"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(ended_as(kernel), "failed");
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorExecutionRejected);
  EXPECT_EQ(snapshot.error->details.at("reason"), "execution_progress_publication_rejected");
  EXPECT_EQ(snapshot.error->details.at("publication_reason"), "execution_generation_retired");
  EXPECT_EQ(snapshot.error->details.at("dispatch_boundary"), "execution_backend_handle");
  EXPECT_EQ(snapshot.error->details.at("execution_generation"), "0");
}

TEST_F(ExecutionKernelTest, Execute_FromTerminalWorkerCallbackRejectedWithoutReplacingWorker) {
  std::promise<std::optional<ErrorDTO>> nested_result;
  auto nested_result_future = nested_result.get_future();
  std::atomic<bool> attempted{false};
  kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kParticipantSettled || attempted.exchange(true)) {
      return;
    }
    auto nested = prepare_and_dispatch(kernel, make_test_request("nested_from_callback"));
    nested_result.set_value(nested.ok() ? std::nullopt : std::make_optional(nested.error()));
  });

  auto result = prepare_and_dispatch(kernel, make_test_request("outer"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
  ASSERT_EQ(nested_result_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  auto error = nested_result_future.get();
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, kErrorExecutionRejected);
  EXPECT_EQ(error->details.at("reason_code"), "worker_retirement_self_thread");
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_HappyPath_CallsPlanner) {
  execute_sync(make_test_request());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(planner.last_request.controllable_component_id.value(), "robot1");
  EXPECT_EQ(planner.last_request.move_id.value(), "move_1");
  EXPECT_EQ(planner.last_request.capability_id.value(), "execution.trajectory.execute");
  EXPECT_EQ(planner.last_request.planner_capability_binding.capability_id.value(),
            "planning.motion.joint_goal");
  EXPECT_EQ(planner.last_request.planner_capability_binding.provider_instance_id.value(),
            "planner.1");
  EXPECT_EQ(planner.last_request.resources,
            (std::vector<ResourceId>{ResourceId("joint.1"), ResourceId("joint.2")}));
  EXPECT_EQ(planner.last_request.start_state.resources, planner.last_request.resources);
  EXPECT_EQ(planner.last_request.start_state.control_binding_id,
            planner.last_request.control_binding_id);
}

TEST_F(ExecutionKernelTest, Execute_PlannerResultResourceMismatchFailsClosedBeforeDispatch) {
  planner.next_result.trajectory = make_test_trajectory();
  planner.next_result.trajectory->resources = {ResourceId("resource:other-1"),
                                               ResourceId("resource:other-2")};

  const auto result = execute_sync(make_test_request("wrong_resources"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_PlannerResultOrderingMismatchFailsClosedBeforeDispatch) {
  planner.next_result.trajectory = make_test_trajectory();
  planner.next_result.trajectory->resources = {ResourceId("joint.2"), ResourceId("joint.1")};
  planner.next_result.trajectory->joint_names = {"j2", "j1"};

  const auto result = execute_sync(make_test_request("wrong_order"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_RelativeMove_ResolvesToPoseGoalBeforePlanning) {
  ExecuteRequestDTO req = make_relative_object_request("relative_pose", "graspable");
  auto& relative_offset = std::get<PoseGoalDTO>(req.motion);
  relative_offset.goal.position_m = {0.0, 0.0, 0.1};

  Pose3DTO resolved_reference;
  resolved_reference.position_m = {0.3, -0.2, 0.15};
  resolved_reference.orientation = {0.0, 0.0, 0.0, 1.0};
  context.next_reference_pose = Result<Pose3DTO>::Ok(resolved_reference);
  install_mock_retained_object(collision_world_publisher, "graspable");

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
  ExecuteRequestDTO req = make_test_request("relative_pose");
  req.target_mode = MotionTargetModeDTO::kRelative;
  req.target_reference = ReferenceDTO(Pose3DTO{});
  req.target_reference->pose->position_m = {0.25, -0.15, 0.2};
  req.target_reference->pose->orientation = {0.0, 0.0, 0.0, 1.0};
  PoseGoalDTO relative_offset;
  relative_offset.goal.position_m = {0.0, 0.0, 0.1};
  relative_offset.goal.orientation = {0.0, 0.0, 0.0, 1.0};
  req.motion = relative_offset;

  context.next_reference_pose = Result<Pose3DTO>::Ok(req.target_reference->pose.value());

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::holds_alternative<PoseGoalDTO>(planner.last_request.motion));
  const auto& pose_goal = std::get<PoseGoalDTO>(planner.last_request.motion);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.x_m, 0.25);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.y_m, -0.15);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.z_m, 0.3);
  EXPECT_DOUBLE_EQ(pose_goal.goal.orientation.w, 1.0);
}

TEST_F(ExecutionKernelTest, Execute_AbsolutePoseInGoalFrame_ResolvesFromAdmissionContext) {
  ExecuteRequestDTO req = make_test_request("framed_absolute_pose");
  req.goal_frame_reference = ReferenceDTO(ReferenceDTO::Type::kFrameRole, "fixture");
  PoseGoalDTO framed_goal;
  framed_goal.goal.position_m = {0.0, 0.0, 0.1};
  framed_goal.goal.orientation = {0.0, 0.0, 0.0, 1.0};
  req.motion = framed_goal;

  Pose3DTO frame_in_base;
  frame_in_base.position_m = {0.25, -0.15, 0.2};
  frame_in_base.orientation = {0.0, 0.0, 0.0, 1.0};
  context.next_reference_pose = Result<Pose3DTO>::Ok(frame_in_base);

  auto result = execute_sync(req);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(std::holds_alternative<PoseGoalDTO>(planner.last_request.motion));
  const auto& pose_goal = std::get<PoseGoalDTO>(planner.last_request.motion);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.x_m, 0.25);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.y_m, -0.15);
  EXPECT_DOUBLE_EQ(pose_goal.goal.position_m.z_m, 0.3);
  EXPECT_DOUBLE_EQ(pose_goal.goal.orientation.w, 1.0);
}

TEST_F(ExecutionKernelTest, Execute_RelativeMove_OffsetReference_UsesIndependentBasis) {
  constexpr double kInvSqrt2 = 0.7071067811865476;
  ExecuteRequestDTO req = make_relative_object_request("relative_pose", "graspable");

  ReferenceDTO offset_reference;
  offset_reference = ReferenceDTO(ReferenceDTO::Type::kFrameRole, "frame-role:base");
  req.offset_reference = offset_reference;

  auto& relative_offset = std::get<PoseGoalDTO>(req.motion);
  relative_offset.goal.position_m = {0.1, 0.0, 0.0};

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
  install_mock_retained_object(collision_world_publisher, "graspable");

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
    EXPECT_THROW((ExecutionKernel{planner, context, execution, clock, scheduler, cache, config}),
                 std::invalid_argument);
  }

  ExecutionKernelConfig ok_config;
  ok_config.single_shot_ik_timeout_s = 0.05;
  EXPECT_NO_THROW(
      (ExecutionKernel{planner, context, execution, clock, scheduler, cache, ok_config}));
}

TEST_F(ExecutionKernelTest, Config_ValidationCollisionWallTimeoutRejectsNonPositive) {
  for (Duration bad : {Duration{0}, Duration{-1}}) {
    ExecutionKernelConfig config;
    config.validation_collision_wall_timeout_ns = bad;
    EXPECT_THROW((ExecutionKernel{planner, context, execution, clock, scheduler, cache, config}),
                 std::invalid_argument);
  }

  ExecutionKernelConfig ok_config;
  ok_config.validation_collision_wall_timeout_ns = 5'000'000'000;
  EXPECT_NO_THROW(
      (ExecutionKernel{planner, context, execution, clock, scheduler, cache, ok_config}));
}

TEST_F(ExecutionKernelTest, Config_ValidationCollisionWorkersRejectsOutOfRangeWithoutClamping) {
  for (const int bad : {0, 65}) {
    ExecutionKernelConfig config;
    config.validation_collision_workers = bad;
    EXPECT_THROW((ExecutionKernel{planner, context, execution, clock, scheduler, cache, config}),
                 std::invalid_argument);
  }

  for (const int admitted : {1, 64}) {
    ExecutionKernelConfig config;
    config.validation_collision_workers = admitted;
    EXPECT_NO_THROW(
        (ExecutionKernel{planner, context, execution, clock, scheduler, cache, config}));
  }
}

TEST_F(ExecutionKernelTest, DeclaredValidationWorkerCapacityRejectsInsufficientResources) {
  ExecutionKernelConfig config;
  config.validation_collision_workers = 2;
  config.validation_collision_workers_declared = true;
  ExecutionKernel declared_kernel{planner, context, execution, clock, scheduler, cache, config};

  EXPECT_THROW(declared_kernel.set_collision_resources(collision_world_publisher,
                                                       std::make_shared<MockCollisionEngine>(),
                                                       {std::make_shared<MockCollisionEngine>()}),
               std::invalid_argument);
  EXPECT_EQ(declared_kernel.get_metrics().validation_collision_workers_effective, 0u);
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
  auto cached = peek_cache("move_A");
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(cached->waypoints.size(), 2u);
}

TEST_F(ExecutionKernelTest,
       Execute_LiveSceneRevisionChangeAfterBorrowedSnapshotDispatchesOnLatestScene) {
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      auto mutated = make_test_collision_world_snapshot();
      mutated.revision = CollisionWorldRevision{2, test_digest("mutated-world")};
      collision_world_publisher.current_snapshot =
          std::make_shared<const CollisionWorldSnapshot>(std::move(mutated));
    }
  };

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  auto result = execute_sync(make_test_request("move_scene_changed"));

  // A newer world is an ordinary newer scene: dispatch compares nothing and
  // the runtime monitor checks it from the first tick.
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  // One validation pass over the two stored waypoints (both joints 0 -> 1
  // rad: sqrt(2) rad apart) and the interpolants at the planner's default
  // collision-check step.
  EXPECT_EQ(collision_engine->check_count.load(),
            static_cast<int>(std::ceil(std::sqrt(2.0) / kDefaultCollisionCheckStepRad)) + 1);
  std::lock_guard<std::mutex> lock(events_mutex);
  EXPECT_TRUE(std::none_of(events.begin(), events.end(), [](const KernelEvent& e) {
    return e.type == KernelEventType::kErrorRaised;
  }));
}

TEST_F(ExecutionKernelTest, Execute_SameContentRevisionChangeDuringValidationDispatchesOnLatestScene) {
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      auto mutated = make_test_collision_world_snapshot();
      mutated.revision = CollisionWorldRevision{2, test_digest("mock-collision-world")};
      collision_world_publisher.current_snapshot =
          std::make_shared<const CollisionWorldSnapshot>(std::move(mutated));
    }
  };

  auto result = execute_sync(make_test_request("move_same_content_revision"));

  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(mutated_once);
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_FALSE(kernel.get_execution_snapshot().error.has_value());
}

TEST_F(ExecutionKernelTest, Execute_RepeatedLiveSceneRevisionChangesDuringValidationDispatchOnce) {
  std::uint64_t next_revision = 2;
  collision_engine->on_check = [this, &next_revision](const CollisionQuery&) {
    auto mutated = make_test_collision_world_snapshot();
    mutated.revision = CollisionWorldRevision{
        next_revision, test_digest("mutated-world-" + std::to_string(next_revision))};
    ++next_revision;
    collision_world_publisher.current_snapshot =
        std::make_shared<const CollisionWorldSnapshot>(std::move(mutated));
  };

  auto result = execute_sync(make_test_request("move_scene_changed"));

  ASSERT_TRUE(result.ok());
  EXPECT_GT(next_revision, 2U);
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_FALSE(kernel.get_execution_snapshot().error.has_value());
}

TEST_F(ExecutionKernelTest, Execute_ObservedSceneLeaseAcceptsBoundedTargetPoseDriftBeforeDispatch) {
  enable_observed_box_lease("box");
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.005, CollisionWorldRevision{2, test_digest("box-world-2")}, "box-body-hash-2");
    }
  };

  auto result = execute_sync(make_relative_object_request("move_lease_drift", "box"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_TRUE(peek_cache("move_lease_drift").has_value());
  EXPECT_EQ(collision_world_publisher.lease_retain_call_count.load(), 1);
  EXPECT_EQ(collision_world_publisher.lease_release_call_count.load(), 1);
}

TEST_F(ExecutionKernelTest, Execute_RetainsOneExactProjectionThroughPlanValidationAndDispatch) {
  std::atomic<bool> lease_held_at_every_stage{true};
  std::atomic<bool> planning_observed{false};
  std::atomic<bool> validation_observed{false};
  std::atomic<bool> dispatch_observed{false};
  const auto observe_retained_lease = [&]() {
    if (collision_world_publisher.lease_retain_call_count.load() != 1 ||
        collision_world_publisher.lease_release_call_count.load() != 0) {
      lease_held_at_every_stage.store(false);
    }
  };
  planner.on_plan = [&](int) {
    observe_retained_lease();
    planning_observed.store(true);
  };
  context.on_validate = [&]() {
    observe_retained_lease();
    validation_observed.store(true);
  };
  execution.on_execute = [&]() {
    observe_retained_lease();
    dispatch_observed.store(true);
  };

  const auto result = execute_sync(make_test_request("move_exact_projection_lifetime"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_TRUE(planning_observed.load());
  EXPECT_TRUE(validation_observed.load());
  EXPECT_TRUE(dispatch_observed.load());
  EXPECT_TRUE(lease_held_at_every_stage.load());
  EXPECT_EQ(collision_world_publisher.lease_retain_call_count.load(), 1);
  EXPECT_EQ(collision_world_publisher.lease_release_call_count.load(), 1);
  EXPECT_EQ(planner.last_request.projection_binding.projection_revision,
            test_projection_revision());
  EXPECT_EQ(planner.last_request.retained_collision_world,
            collision_world_publisher.current_snapshot);
  ASSERT_NE(planner.last_request.retained_collision_policy, nullptr);
  ASSERT_TRUE(collision_world_publisher.current_policy.has_value());
  EXPECT_EQ(planner.last_request.retained_collision_policy->revision,
            collision_world_publisher.current_policy->revision);
}

TEST_F(ExecutionKernelTest, Execute_ObservedSceneLeaseAcceptsTargetMovedAfterValidation) {
  enable_observed_box_lease("box");
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.25, CollisionWorldRevision{2, test_digest("box-world-2")}, "box-body-hash-2");
    }
  };

  auto result = execute_sync(make_relative_object_request("move_lease_target", "box"));

  // The motion goes ahead on the latest scene; the next cycle plans against
  // the moved target.
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(mutated_once);
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_FALSE(kernel.get_execution_snapshot().error.has_value());
}

TEST_F(ExecutionKernelTest, Execute_ObservedSceneLeaseAcceptsTargetShapeChange) {
  enable_observed_box_lease("box");
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.005, CollisionWorldRevision{2, test_digest("box-world-2")}, "box-body-hash-2");
    }
  };

  auto result = execute_sync(make_relative_object_request("move_lease_shape", "box"));

  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(mutated_once);
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_ObservedSceneLeaseAcceptsNonTargetObjectChange) {
  enable_observed_box_lease("box");
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.005, CollisionWorldRevision{2, test_digest("box-world-2")}, "box-body-hash-2");
    }
  };

  auto result = execute_sync(make_test_request("move_lease_obstacle"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_FALSE(snapshot.error.has_value());
}

TEST_F(ExecutionKernelTest, Execute_ObservedSceneLeaseAcceptsDefaultSnapshotObstacleDrift) {
  enable_observed_box_lease("box");
  bool mutated_once = false;
  collision_engine->on_check = [this, &mutated_once](const CollisionQuery&) {
    if (!mutated_once) {
      mutated_once = true;
      publish_observed_box_for_lease(
          "box", 0.25, CollisionWorldRevision{2, test_digest("box-world-2")}, "box-body-hash-2");
    }
  };

  auto result = execute_sync(make_test_request("move_lease_snapshot_obstacle"));

  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(mutated_once);
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_FALSE(snapshot.error.has_value());
}

TEST_F(ExecutionKernelTest, Execute_IncrementingIds) {
  auto r1 = execute_sync(make_test_request());
  EXPECT_EQ(r1.value(), 1u);
  ASSERT_TRUE(kernel.acknowledge_completed(r1.value()).accepted);
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

TEST_F(ExecutionKernelTest, Execute_CachedTrajectoryRevalidatesAgainstChangedSceneWithoutPlanning) {
  std::atomic<int> hit_events{0};
  kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type == KernelEventType::kCacheHit) {
      hit_events.fetch_add(1, std::memory_order_relaxed);
    }
  });
  const auto request = make_test_request("move_changed_scene_valid");
  store_cache(request, make_test_trajectory());
  // The scene changed after the trajectory was stored. Reuse does not compare
  // revisions: the stored trajectory is validated against the latest scene.
  auto current_world = make_test_collision_world_snapshot();
  current_world.revision = {2, test_digest("current-collision-world")};
  collision_world_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(current_world));

  const auto result = execute_sync(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_GE(context.validate_count, 1);
  EXPECT_GE(collision_engine->check_count.load(), 1);
  EXPECT_EQ(execution.execute_count, 1);
  const auto metrics = kernel.get_metrics();
  EXPECT_EQ(metrics.cache_hits, 1u);
  EXPECT_EQ(metrics.cache_revalidations_failed, 0u);
  EXPECT_EQ(hit_events.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(peek_cache(request.move_id).has_value());
}

TEST_F(ExecutionKernelTest, Execute_FailedCachedTrajectoryInvalidatesAndReplansOnce) {
  std::atomic<int> hit_events{0};
  std::atomic<int> failure_events{0};
  kernel.subscribe_events([&](const KernelEvent& event) {
    if (event.type == KernelEventType::kCacheHit) {
      hit_events.fetch_add(1, std::memory_order_relaxed);
    } else if (event.type == KernelEventType::kCacheRevalidationFailed) {
      failure_events.fetch_add(1, std::memory_order_relaxed);
    }
  });
  const auto request = make_test_request("move_changed_scene_invalid");
  store_cache(request, make_test_trajectory());
  auto current_world = make_test_collision_world_snapshot();
  current_world.revision = {2, test_digest("current-collision-world")};
  collision_world_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(current_world));

  int collision_queries = 0;
  collision_engine->result_for_query =
      [&collision_queries](const CollisionQuery&) -> std::optional<CollisionResult> {
    CollisionResult result =
        collision_queries++ == 0 ? make_authoritative_collision_result() : CollisionResult{};
    if (!result.in_collision) {
      result.engine_status = CollisionEngineStatus::kOk;
      result.decision = CollisionDecisionResult::kPass;
    }
    return result;
  };

  const auto result = execute_sync(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(execution.execute_count, 1);
  const auto metrics = kernel.get_metrics();
  EXPECT_EQ(metrics.cache_hits, 1u);
  EXPECT_EQ(metrics.cache_revalidations_failed, 1u);
  EXPECT_EQ(hit_events.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(failure_events.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(metrics.cache_invalidations, 1u);
  // The re-planned, validated trajectory replaces the failed one.
  EXPECT_TRUE(peek_cache(request.move_id).has_value());
}

// Planner-intent cache admission: an existing uncapped entry must not be
// served to a request carrying per-joint dynamics caps.
TEST_F(ExecutionKernelTest, Execute_PerJointDynamicsLimitsBypassCacheAndReplan) {
  store_cache("move_dyn_capped", make_test_trajectory());
  auto req = make_test_request("move_dyn_capped");
  JointDynamicsLimitDTO lim;
  lim.max_velocity_rad_s = 0.5;
  req.per_resource_dynamics_limits.push_back(ResolvedResourceDynamicsLimitDTO{
      req.binding.resources.front(), req.binding.model_group_binding.compiled_joint_names.front(),
      lim});

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);  // cache bypassed -> re-planned
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest,
       Execute_CappedRequestStoresIntentScopedEntryNotReusableByUncappedRequest) {
  auto req = make_test_request("move_capped_no_store");
  JointDynamicsLimitDTO lim;
  lim.max_velocity_rad_s = 0.5;
  req.per_resource_dynamics_limits.push_back(ResolvedResourceDynamicsLimitDTO{
      req.binding.resources.front(), req.binding.model_group_binding.compiled_joint_names.front(),
      lim});

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_TRUE(peek_cache("move_capped_no_store").has_value());

  ASSERT_TRUE(kernel.acknowledge_completed(result.value()).accepted);
  auto uncapped = execute_sync(make_test_request("move_capped_no_store"));
  ASSERT_TRUE(uncapped.ok());
  EXPECT_EQ(planner.plan_count, 2);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitPoseGoalWithMatchingEndPose_SkipsPlanning) {
  auto req = make_test_request("move_pose_cached");
  PoseGoalDTO pose_goal;
  pose_goal.goal = make_pose(0.4, -0.1, 0.2);
  req.motion = pose_goal;
  store_cache(req, make_test_trajectory());
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
  auto req = make_test_request("move_pose_changed");
  PoseGoalDTO pose_goal;
  pose_goal.goal = make_pose(0.45, -0.1, 0.2);
  req.motion = pose_goal;
  store_cache(req, make_test_trajectory());
  context.next_fk = make_pose(0.1, -0.1, 0.2);

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
  auto refreshed = peek_cache("move_pose_changed");
  ASSERT_TRUE(refreshed.has_value());
}

TEST_F(ExecutionKernelTest, Execute_CacheHitCartesianPathWithMatchingFinalWaypoint_SkipsPlanning) {
  auto req = make_test_request("move_cartesian_cached");
  CartesianPathDTO path;
  path.waypoints = {make_pose(0.2, -0.1, 0.1), make_pose(0.4, -0.1, 0.2)};
  path.max_translation_step_m = 0.01;
  path.max_rotation_step_rad = 0.01;
  req.motion = path;
  store_cache(req, make_test_trajectory());
  context.next_fk = path.waypoints.back();

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest,
       Execute_CacheHitCartesianPathWithChangedFinalWaypoint_InvalidatesAndReplans) {
  auto req = make_test_request("move_cartesian_changed");
  CartesianPathDTO path;
  path.waypoints = {make_pose(0.2, -0.1, 0.1), make_pose(0.45, -0.1, 0.2)};
  path.max_translation_step_m = 0.01;
  path.max_rotation_step_rad = 0.01;
  req.motion = path;
  store_cache(req, make_test_trajectory());
  context.next_fk = make_pose(0.1, -0.1, 0.2);

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest,
       Execute_CartesianIntermediateWaypointChangeInvalidatesSameEndpointCache) {
  auto cached_request = make_test_request("move_cartesian_shape_changed");
  CartesianPathDTO cached_path;
  cached_path.waypoints = {make_pose(0.2, -0.1, 0.1), make_pose(0.3, -0.1, 0.15),
                           make_pose(0.4, -0.1, 0.2)};
  cached_request.motion = cached_path;
  store_cache(cached_request, make_test_trajectory());

  auto changed_request = cached_request;
  auto& changed_path = std::get<CartesianPathDTO>(changed_request.motion);
  changed_path.waypoints[1] = make_pose(0.3, 0.2, 0.15);
  context.next_fk = changed_path.waypoints.back();

  auto result = execute_sync(changed_request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(context.fk_count, 0);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitRelativeMoveWithUnchangedResolvedGoal_SkipsPlanning) {
  auto req = make_relative_object_request("move_relative_same", "box");
  std::get<PoseGoalDTO>(req.motion).goal.position_m = {0.0, 0.0, 0.1};

  context.next_reference_pose = Result<Pose3DTO>::Ok(make_pose(0.3, -0.2, 0.15));
  context.next_fk = make_pose(0.3, -0.2, 0.25);
  install_mock_retained_object(collision_world_publisher, "box");
  auto resolved_request = req;
  resolved_request.motion = PoseGoalDTO{make_pose(0.3, -0.2, 0.25)};
  store_cache(resolved_request, make_test_trajectory());

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitRelativeMoveWithMovedObject_InvalidatesAndReplans) {
  auto req = make_relative_object_request("move_relative_changed", "box");
  std::get<PoseGoalDTO>(req.motion).goal.position_m = {0.0, 0.0, 0.1};

  context.next_reference_pose = Result<Pose3DTO>::Ok(make_pose(0.5, -0.2, 0.15));
  context.next_fk = make_pose(0.3, -0.2, 0.25);
  install_mock_retained_object(collision_world_publisher, "box");
  auto previously_resolved_request = req;
  previously_resolved_request.motion = PoseGoalDTO{make_pose(0.3, -0.2, 0.25)};
  store_cache(previously_resolved_request, make_test_trajectory());

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
  auto req = make_relative_object_request("move_missing_reference", "missing_box");
  std::get<PoseGoalDTO>(req.motion).goal.position_m = {0.0, 0.0, 0.1};

  context.next_reference_pose =
      Result<Pose3DTO>::Err(ErrorDTO{kErrorMissingReference, "Object reference not found", {}});

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
  EXPECT_FALSE(peek_cache("move_missing_reference").has_value());
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
  auto req = make_test_request("move_fk_failure");
  PoseGoalDTO pose_goal;
  pose_goal.goal = make_pose(0.4, -0.1, 0.2);
  req.motion = pose_goal;
  store_cache(req, make_test_trajectory());
  context.throw_fk = true;

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(context.fk_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitResourceProjectionReorderInvalidatesAndReplans) {
  auto cached = make_test_trajectory();
  cached.resources = {ResourceId("joint.2"), ResourceId("joint.1")};
  cached.joint_names = {"j2", "j1"};
  cached.waypoints.front().positions_rad = {0.0, 0.0};
  cached.waypoints.back().positions_rad = {2.0, 1.0};
  auto req = make_test_request("move_joint_named");
  req.motion = ResolvedJointGoalDTO{req.binding.resources, {"j1", "j2"}, {1.0, 2.0}};
  store_cache(req, cached);

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitJointGoalNameMismatch_InvalidatesAndReplans) {
  auto cached = make_test_trajectory();
  cached.resources = {ResourceId("resource:j3"), ResourceId("joint.1")};
  cached.joint_names = {"j3", "j1"};
  store_cache("move_joint_mismatch", cached);

  auto req = make_test_request("move_joint_mismatch");
  req.motion = ResolvedJointGoalDTO{req.binding.resources, {"j1", "j2"}, {1.0, 1.0}};

  auto result = execute_sync(req);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(kernel.get_metrics().cache_invalidations, 1u);
}

TEST_F(ExecutionKernelTest, Execute_CacheHitNamedTarget_InvalidatesAndReplans) {
  auto req = make_test_request("move_named_target");
  req.motion = NamedTargetDTO{"ready"};
  store_cache(req, make_test_trajectory());

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
      false, ErrorDTO{kErrorValidationCollision,
                      "Cached trajectory is no longer valid under current collision policy",
                      {{"failure_stage", "cached_validation"}}}};

  execute_sync(make_test_request("move_cached_invalid"));

  // The failed cached trajectory is re-planned once; the re-planned one fails
  // the same validation, so nothing executes.
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(context.validate_count, 2);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_FALSE(peek_cache("move_cached_invalid").has_value());
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
  EXPECT_EQ(snapshot.error->details.at("failure_stage"), "cached_validation");
}

TEST_F(ExecutionKernelTest,
       ApplyRecoveryAction_GeneralCloseRetiresFailedAttemptWithoutResetOrMotion) {
  store_cache("close_failed", make_test_trajectory());
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision, "validation rejected the attempt", {}}};
  execute_sync(make_test_request("close_failed"));
  ASSERT_EQ(ended_as(kernel), "failed");
  const auto resets = execution.reset_count;
  const auto dispatches = execution.execute_count;
  const auto before_reset = kernel.get_status();
  const auto reset = apply_recovery_action_after_projection(kernel, RecoveryAction::kResetCycle);
  EXPECT_FALSE(reset.accepted);
  ASSERT_TRUE(reset.error);
  EXPECT_EQ(reset.error->details.at("reason"), reason_codes::kRecoveryActionNotAllowed);
  const auto after_reset = kernel.get_status();
  EXPECT_EQ(after_reset.state, before_reset.state);
  EXPECT_EQ(after_reset.execution_id, before_reset.execution_id);
  EXPECT_EQ(after_reset.active_move_id, before_reset.active_move_id);
  EXPECT_EQ(after_reset.fact, before_reset.fact);
  EXPECT_EQ(execution.reset_count, resets);
  EXPECT_EQ(execution.execute_count, dispatches);
  const auto result = apply_recovery_action_after_projection(
      kernel, RecoveryAction::kCloseOperation, "program_runner", "failure propagated");
  ASSERT_TRUE(result.accepted) << (result.error ? result.error->message : "");
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.reset_count, resets);
  EXPECT_EQ(execution.execute_count, dispatches);
  EXPECT_FALSE(result.resulting_assessment.eligible_close);
}

TEST_F(ExecutionKernelTest,
       ApplyRecoveryAction_RetryAfterSceneRepairClearsFailedWithoutBackendReset) {
  std::vector<KernelEvent> events;
  auto sub = kernel.subscribe_events([&](const KernelEvent& event) { events.push_back(event); });
  execution.snapshot.joint_positions_rad = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  store_cache("move_cached_invalid_ack", make_test_trajectory());
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorValidationCollision,
                      "Cached trajectory is no longer valid under current collision policy",
                      {{"failure_stage", "cached_validation"}}}};

  execute_sync(make_test_request("move_cached_invalid_ack"));

  ASSERT_EQ(ended_as(kernel), "failed");
  const auto reset_count_before_ack = execution.reset_count;
  auto ack =
      apply_recovery_action_after_projection(kernel, RecoveryAction::kRetryAfterSceneRepair,
                                             "operator_alice", "ui_retry_after_scene_repair");

  ASSERT_TRUE(ack.accepted) << (ack.error.has_value() ? ack.error->message : std::string{});
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

TEST_F(ExecutionKernelTest,
       ApplyRecoveryAction_RetryAfterSceneRepairAcceptsContactPolicyViolation) {
  std::vector<KernelEvent> events;
  const auto sub =
      kernel.subscribe_events([&](const KernelEvent& event) { events.push_back(event); });
  execution.snapshot.joint_positions_rad = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  store_cache("move_contact_policy_ack", make_test_trajectory());
  context.next_validation = ValidationResultDTO{
      false, ErrorDTO{kErrorContactPolicyViolation,
                      "Cached trajectory violates the active contact policy",
                      {{"policy_id", "support-contact-policy"},
                       {"contact_policy_violation", "contact_penetration_exceeds_policy"}}}};

  execute_sync(make_test_request("move_contact_policy_ack"));
  ASSERT_EQ(ended_as(kernel), "failed");

  const auto ack = apply_recovery_action_after_projection(
      kernel, RecoveryAction::kRetryAfterSceneRepair, "operator_alice",
      "ui_retry_after_contact_policy_repair");

  ASSERT_TRUE(ack.accepted) << (ack.error.has_value() ? ack.error->message : std::string{});
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  const auto snapshot = kernel.get_execution_snapshot();
  EXPECT_EQ(snapshot.state, ExecutionState::kIdle);
  EXPECT_FALSE(snapshot.error.has_value());
  const auto acknowledged =
      std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
        return event.type == KernelEventType::kFailedValidationAcknowledged;
      });
  ASSERT_NE(acknowledged, events.end());
  EXPECT_EQ(acknowledged->details.at("error_code"), kErrorContactPolicyViolation);
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
  EXPECT_FALSE(peek_cache("move_no_store").has_value());
}

// --- Observed-state sync before planning ---

TEST_F(ExecutionKernelTest, Execute_SyncObservedStateBeforePlanning_CalledBeforePlanner) {
  execution.on_sync = [this]() {
    RobotStateDTO state;
    state.joint_names = {"j1", "j2"};
    state.positions_rad = {0.25, -0.5};
    context.current_state_value = state;
  };

  auto result = execute_sync(make_test_request("move_after_sync"));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(execution.sync_count, 1);
  ASSERT_EQ(planner.plan_count, 1);
  const auto planned_start = angular_position_projection(planner.last_request.start_state);
  ASSERT_TRUE(planned_start.ok());
  ASSERT_EQ(planned_start.value().size(), 2u);
  EXPECT_DOUBLE_EQ(planned_start.value()[0], 0.25);
  EXPECT_DOUBLE_EQ(planned_start.value()[1], -0.5);
}

TEST_F(ExecutionKernelTest, Execute_SyncObservedStateBeforePlanningFailure_BlocksPlanning) {
  execution.next_sync_result =
      Result<void>::Err(ErrorDTO{kErrorStartStateMismatch, "No fresh observed robot state", {}});

  auto result = execute_sync(make_test_request("move_no_fresh_state"));

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorStartStateMismatch);
  EXPECT_EQ(execution.sync_count, 1);
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(context.validate_count, 0);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_EQ(kernel.get_status().state, ExecutionState::kIdle);
}

TEST_F(ExecutionKernelTest, Execute_SynchronizesExternalSceneBeforeRetainingPlanningProjection) {
  MockPlanningSceneSynchronizer scene_synchronizer;
  auto ports = static_cast<ExecutionKernelPorts>(context);
  ports.planning_scene_synchronizer = &scene_synchronizer;
  ExecutionKernel synchronized_kernel{planner, ports, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(synchronized_kernel, collision_world_publisher,
                                               collision_engine);

  const auto result =
      prepare_and_dispatch(synchronized_kernel, make_test_request("move_after_scene_sync"));
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(synchronized_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(scene_synchronizer.call_count, 1);
  EXPECT_EQ(planner.plan_count, 1);
  EXPECT_EQ(planner.last_request.projection_binding.projection_revision,
            test_projection_revision());
}

TEST_F(ExecutionKernelTest,
       Execute_AcceptsRetainedProjectionForwardOfSceneSynchronizationFloor) {
  MockPlanningSceneSynchronizer scene_synchronizer;
  auto retained_revision = test_projection_revision();
  retained_revision.scene_revision.revision_id = 2U;
  retained_revision.scene_revision.content_hash = ContentDigest{kTestDigestA};
  collision_world_publisher.lease_projection_revision = retained_revision;
  auto ports = static_cast<ExecutionKernelPorts>(context);
  ports.planning_scene_synchronizer = &scene_synchronizer;
  ExecutionKernel synchronized_kernel{planner, ports, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(synchronized_kernel, collision_world_publisher,
                                               collision_engine);

  const auto result = prepare_and_dispatch(
      synchronized_kernel, make_test_request("move_after_scene_sync_forward_revision"));

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(synchronized_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(scene_synchronizer.call_count, 1);
  ASSERT_EQ(planner.plan_count, 1);
  EXPECT_EQ(planner.last_request.projection_binding.projection_revision,
            retained_revision);
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_SceneSynchronizationFailureBlocksPlanningAndDispatch) {
  MockPlanningSceneSynchronizer scene_synchronizer;
  scene_synchronizer.next_result = Result<ProjectionRevisionDTO>::Err(ErrorDTO{
      "scene_runtime_pose_observation_stale", "before-planning pose observation is stale", {}});
  auto ports = static_cast<ExecutionKernelPorts>(context);
  ports.planning_scene_synchronizer = &scene_synchronizer;
  ExecutionKernel synchronized_kernel{planner, ports, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(synchronized_kernel, collision_world_publisher,
                                               collision_engine);

  const auto result =
      prepare_and_dispatch(synchronized_kernel, make_test_request("move_scene_sync_failed"));

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "scene_runtime_pose_observation_stale");
  EXPECT_EQ(scene_synchronizer.call_count, 1);
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_RejectsProjectionChangeAfterSceneSynchronization) {
  MockPlanningSceneSynchronizer scene_synchronizer;
  auto successor = test_projection_revision();
  successor.scene_revision.revision_id = 2U;
  scene_synchronizer.next_result = Result<ProjectionRevisionDTO>::Ok(successor);
  auto ports = static_cast<ExecutionKernelPorts>(context);
  ports.planning_scene_synchronizer = &scene_synchronizer;
  ExecutionKernel synchronized_kernel{planner, ports, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(synchronized_kernel, collision_world_publisher,
                                               collision_engine);

  const auto result =
      prepare_and_dispatch(synchronized_kernel, make_test_request("move_scene_sync_raced"));

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "planning_scene_synchronization_raced");
  EXPECT_EQ(scene_synchronizer.call_count, 1);
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest,
       Execute_RejectsSynchronizedFloorWithEqualRevisionAndDifferentHash) {
  MockPlanningSceneSynchronizer scene_synchronizer;
  auto floor = test_projection_revision();
  floor.scene_revision.content_hash = ContentDigest{kTestDigestA};
  scene_synchronizer.next_result = Result<ProjectionRevisionDTO>::Ok(floor);
  auto ports = static_cast<ExecutionKernelPorts>(context);
  ports.planning_scene_synchronizer = &scene_synchronizer;
  ExecutionKernel synchronized_kernel{planner, ports, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(synchronized_kernel, collision_world_publisher,
                                               collision_engine);

  const auto result = prepare_and_dispatch(
      synchronized_kernel, make_test_request("move_scene_sync_equal_revision_hash_mismatch"));

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "planning_scene_synchronization_raced");
  EXPECT_EQ(scene_synchronizer.call_count, 1);
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest,
       Execute_RejectsSynchronizedFloorFromDifferentSceneLineage) {
  MockPlanningSceneSynchronizer scene_synchronizer;
  auto floor = test_projection_revision();
  floor.scene_revision.scene_id = SceneId{kTestDigestD};
  scene_synchronizer.next_result = Result<ProjectionRevisionDTO>::Ok(floor);
  auto ports = static_cast<ExecutionKernelPorts>(context);
  ports.planning_scene_synchronizer = &scene_synchronizer;
  ExecutionKernel synchronized_kernel{planner, ports, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(synchronized_kernel, collision_world_publisher,
                                               collision_engine);

  const auto result = prepare_and_dispatch(
      synchronized_kernel, make_test_request("move_scene_sync_different_scene"));

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "planning_scene_synchronization_raced");
  EXPECT_EQ(scene_synchronizer.call_count, 1);
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(execution.execute_count, 0);
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
  auto refreshed = peek_cache("move_stale_start");
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
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
}

// --- Validation failure ---

TEST_F(ExecutionKernelTest, Execute_ValidationFails_TransitionsToFailed) {
  context.next_validation =
      ValidationResultDTO{false, ErrorDTO{kErrorValidationCollision, "Collision in path", {}}};

  execute_sync(make_test_request());
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(context.last_validation_options.run_collision_checks);
}

TEST_F(ExecutionKernelTest, Execute_ValidationFails_InvalidatesCache) {
  store_cache("move_val_fail", make_test_trajectory());
  context.next_validation =
      ValidationResultDTO{false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

  execute_sync(make_test_request("move_val_fail"));
  EXPECT_FALSE(peek_cache("move_val_fail").has_value());
}

// --- Start-state mismatch ---

TEST_F(ExecutionKernelTest, Execute_StartStateMismatch_Fails) {
  RobotStateDTO bad_state;
  bad_state.joint_names = {"j1", "j2"};
  bad_state.positions_rad = {5.0, 5.0};
  context.current_state_value = bad_state;

  execute_sync(make_test_request());
  EXPECT_EQ(ended_as(kernel), "failed");
}

TEST_F(ExecutionKernelTest, Execute_StartStateValidationComparesByJointName) {
  RobotStateDTO reordered_state;
  reordered_state.joint_names = {"j2", "j1"};
  reordered_state.positions_rad = {0.0, 0.0};
  context.current_state_value = reordered_state;

  execute_sync(make_test_request("move_start_named"));
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, Execute_StartStateMissingJointName_FailsClosed) {
  RobotStateDTO ambiguous_state;
  ambiguous_state.joint_names = {"j1"};
  ambiguous_state.positions_rad = {0.0};
  context.current_state_value = ambiguous_state;

  execute_sync(make_test_request("move_start_missing_joint"));
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
}

// --- Non-authoritative collision diagnostic comparator gate ---

TEST_F(ExecutionKernelTest, Execute_CollisionRejectionOmitsDiagnosticCompareByDefault) {
  collision_engine->default_result = make_authoritative_collision_result();

  execute_sync(make_test_request("diagnostic_compare_gated_off"));

  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  for (const auto& [key, value] : snapshot.error->details) {
    EXPECT_EQ(key.rfind("collision_diagnostic_compare_", 0), std::string::npos)
        << "unexpected diagnostic compare key on default config: " << key;
  }
}

TEST_F(ExecutionKernelTest, Execute_CollisionRejectionAppendsDiagnosticCompareWhenEnabled) {
  ExecutionKernelConfig config;
  config.enable_collision_diagnostic_compare = true;
  ExecutionKernel gated_kernel{planner, context, execution, clock, scheduler, cache, config};
  configure_authoritative_collision_validation(gated_kernel, collision_world_publisher,
                                               collision_engine);
  collision_engine->default_result = make_authoritative_collision_result();

  auto result = prepare_and_dispatch(gated_kernel, make_test_request("diagnostic_compare_on"));
  ASSERT_TRUE(result.ok());
  gated_kernel.wait_for_completion();

  const auto snapshot = gated_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  bool found_compare_key = false;
  for (const auto& [key, value] : snapshot.error->details) {
    if (key.rfind("collision_diagnostic_compare_", 0) == 0) {
      found_compare_key = true;
      break;
    }
  }
  EXPECT_TRUE(found_compare_key) << "expected diagnostic compare keys when the gate is enabled";
}

// --- Pre-execution collision ---

TEST_F(ExecutionKernelTest, Execute_PreCollisionCheck_Blocks) {
  collision_engine->default_result = make_authoritative_collision_result();

  execute_sync(make_test_request());
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(context.last_validation_options.run_collision_checks);
}

TEST_F(ExecutionKernelTest, Execute_ValidationRejectsCollisionStrictlyBetweenWaypoints) {
  // M55 F-1: the only colliding configuration lies strictly between the two
  // stored waypoints (0 and 1 rad). Checking the stored waypoints alone
  // accepted it; the runtime lookahead then stopped the motion. The band is
  // two planner steps wide, so a sample at the planner step lands in it.
  collision_engine->result_for_query =
      [](const CollisionQuery& query) -> std::optional<CollisionResult> {
    const auto& names = query.robot_state.joint_names;
    const auto j1 = std::find(names.begin(), names.end(), "j1");
    if (j1 == names.end()) return std::nullopt;
    const double position = query.robot_state.positions_rad.at(
        static_cast<std::size_t>(std::distance(names.begin(), j1)));
    if (position > 0.5 && position < 0.5 + 2.0 * kDefaultCollisionCheckStepRad) {
      return make_authoritative_collision_result();
    }
    return std::nullopt;
  };

  execute_sync(make_test_request("collision_between_waypoints"));
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
  const auto& positions =
      snapshot.error->details.at("collision_result_first_failure_positions_rad");
  const double failing_j1 = std::stod(positions.substr(0, positions.find(',')));
  EXPECT_GT(failing_j1, 0.0);
  EXPECT_LT(failing_j1, 1.0);
}

// --- M6 collision clearance (security margin)

TEST_F(ExecutionKernelTest, Config_CollisionClearanceAndCheckStepRejectOutOfRange) {
  for (const double bad : {-0.001, 0.0501}) {
    ExecutionKernelConfig config;
    config.collision_clearance_m = bad;
    EXPECT_THROW((ExecutionKernel{planner, context, execution, clock, scheduler, cache, config}),
                 std::invalid_argument);
  }
  for (const double bad : {0.0, -0.01, 0.2001}) {
    ExecutionKernelConfig config;
    config.collision_check_step_rad = bad;
    EXPECT_THROW((ExecutionKernel{planner, context, execution, clock, scheduler, cache, config}),
                 std::invalid_argument);
  }
  // 0 turns the clearance off; no lower floor otherwise.
  for (const double admitted : {0.0, 0.0002, 0.05}) {
    ExecutionKernelConfig config;
    config.collision_clearance_m = admitted;
    EXPECT_NO_THROW(
        (ExecutionKernel{planner, context, execution, clock, scheduler, cache, config}));
  }
  for (const double admitted : {0.001, 0.2}) {
    ExecutionKernelConfig config;
    config.collision_check_step_rad = admitted;
    EXPECT_NO_THROW(
        (ExecutionKernel{planner, context, execution, clock, scheduler, cache, config}));
  }
}

TEST_F(ExecutionKernelTest, Execute_ValidationChecksTheCurveAtTheConfiguredPlannerStep) {
  // M6 decision 18: validation samples the executed curve at the planner's
  // collision-check step, settable (Deployment collision_check_step_rad),
  // measured like RoboPlan's configuration distance. Both joints 0 -> 1 rad
  // (sqrt(2) rad) at 0.01: 142 steps, 143 checked states; not the default
  // step's 30 and not the former fixed 0.0005 rad joint step's 2001.
  ExecutionKernelConfig config;
  config.collision_check_step_rad = 0.01;
  ExecutionKernel stepped_kernel{planner, context, execution, clock, scheduler, cache, config};
  configure_authoritative_collision_validation(stepped_kernel, collision_world_publisher,
                                               collision_engine);
  std::mutex positions_mutex;
  std::map<std::int64_t, std::pair<double, double>> checked;
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    if (query.waypoint_index) {
      std::lock_guard lock(positions_mutex);
      checked[*query.waypoint_index] = {clearance_test_joint(query, "j1"),
                                        clearance_test_joint(query, "j2")};
    }
    return std::nullopt;
  };

  auto result = prepare_and_dispatch(stepped_kernel, make_test_request("planner_step"));
  ASSERT_TRUE(result.ok()) << result.error().message;
  stepped_kernel.wait_for_completion();
  EXPECT_EQ(ended_as(stepped_kernel), "completed");
  EXPECT_EQ(collision_engine->check_count.load(),
            static_cast<int>(std::ceil(std::sqrt(2.0) / 0.01)) + 1);
  std::lock_guard lock(positions_mutex);
  ASSERT_EQ(checked.size(), 143U);
  for (auto it = std::next(checked.begin()); it != checked.end(); ++it) {
    const auto& [j1, j2] = it->second;
    const auto& [p1, p2] = std::prev(it)->second;
    EXPECT_LE(std::hypot(j1 - p1, j2 - p2), 0.01 + 1e-12);
  }
}

TEST_F(ExecutionKernelTest, Execute_ValidationOfARetryUsesTheRetryPlannerStep) {
  // SX-608: validation uses the step the trajectory was planned with. Both
  // joints 0 -> 1 rad (sqrt(2) rad). The first attempt (0.05: 29 steps) has
  // a sample at j1 = 15/29 = 0.51724 inside a thin band and is refused; the
  // retry is planned at min(0.05, 0.01) and validated at 0.01 (142 steps,
  // samples at 73/142 = 0.51408 and 74/142 = 0.52113, both outside the
  // band), so it is accepted and dispatched.
  std::mutex indexes_mutex;
  std::int64_t max_index = -1;
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    if (query.waypoint_index) {
      std::lock_guard lock(indexes_mutex);
      max_index = std::max(max_index, *query.waypoint_index);
    }
    const double j1 = clearance_test_joint(query, "j1");
    if (j1 > 0.5170 && j1 < 0.5175) return make_authoritative_collision_result();
    return std::nullopt;
  };

  const auto result = execute_sync(make_test_request("retry_step"));
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_EQ(planner.last_request.planning_attempt_index, 1U);
  EXPECT_DOUBLE_EQ(collision_check_step_for_attempt(kDefaultCollisionCheckStepRad, 1U), 0.01);
  std::lock_guard lock(indexes_mutex);
  EXPECT_EQ(max_index, static_cast<std::int64_t>(std::ceil(std::sqrt(2.0) / 0.01)));
}

TEST_F(ExecutionKernelTest, Execute_CachedRetryTrajectoryIsRevalidatedAtTheRetryStep) {
  // SX-617: a trajectory planned on a retry (attempt 1, 0.01) is stored with
  // its attempt; a later cache hit is revalidated at 0.01 (143 states), not
  // at the first attempt's 0.05 (30 states), and the planner is not called.
  store_cache(make_test_request("cached_retry"), make_test_trajectory(), 1U);
  std::mutex indexes_mutex;
  std::int64_t max_index = -1;
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    if (query.waypoint_index) {
      std::lock_guard lock(indexes_mutex);
      max_index = std::max(max_index, *query.waypoint_index);
    }
    return std::nullopt;
  };
  const int plans_before = planner.plan_count;

  const auto result = execute_sync(make_test_request("cached_retry"));
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(planner.plan_count, plans_before);
  EXPECT_EQ(kernel.get_metrics().cache_hits, 1U);
  std::lock_guard lock(indexes_mutex);
  EXPECT_EQ(max_index, static_cast<std::int64_t>(std::ceil(std::sqrt(2.0) / 0.01)));
}

TEST_F(ExecutionKernelTest, Execute_RetryTrajectoryIsCachedWithItsAttempt) {
  // The retry of Execute_ValidationOfARetryUsesTheRetryPlannerStep is stored
  // with attempt 1, so its reuse keeps the 0.01 step.
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    const double j1 = clearance_test_joint(query, "j1");
    if (j1 > 0.5170 && j1 < 0.5175) return make_authoritative_collision_result();
    return std::nullopt;
  };
  const auto request = make_test_request("retry_cached");
  ASSERT_TRUE(execute_sync(request).ok());
  ASSERT_EQ(ended_as(kernel), "completed");
  const auto resolved = prepared_motion_request(request).execution_request;
  const auto stored =
      cache.lookup(trajectory_cache_key(resolved), trajectory_cache_intent_identity(resolved));
  ASSERT_EQ(stored.disposition, TrajectoryCacheLookupDisposition::kFound);
  EXPECT_EQ(stored.planning_attempt_index, 1U);
}

TEST_F(ExecutionKernelTest, Execute_DefaultClearanceIsOffAsInMerge55) {
  // M6 final decision: the default clearance is 0. With no Deployment or
  // move clearance, a pick whose fingers come within 1 mm of the part is
  // accepted, no margins reach the planner or validation, and no start
  // clearance query runs (merge 5.5 behaviour).
  ExecuteRequestDTO req = make_relative_object_request("clearance_default", "graspable");
  Pose3DTO reference;
  reference.position_m = {0.3, -0.2, 0.15};
  reference.orientation = {0.0, 0.0, 0.0, 1.0};
  context.next_reference_pose = Result<Pose3DTO>::Ok(reference);
  install_mock_retained_object(collision_world_publisher, "graspable");
  collision_world_publisher.current_snapshot = std::make_shared<const CollisionWorldSnapshot>(
      make_clearance_test_world(*collision_world_publisher.current_snapshot, {"table"}));
  const auto target = clearance_test_pair("graspable");
  std::atomic<int> queries_with_margins{0};
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    if (query.clearance_margins) ++queries_with_margins;
    return clearance_test_result(query, target,
                                 0.004 - 0.003 * clearance_test_joint(query, "j1"));
  };

  EXPECT_DOUBLE_EQ(kDefaultCollisionClearanceM, 0.0);
  const auto result = execute_sync(req);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_FALSE(planner.last_request.retained_clearance_margins);
  EXPECT_EQ(queries_with_margins.load(), 0);
}

TEST_F(ExecutionKernelTest, Execute_ClearanceAppliesToTheMoveTargetWithoutAContactPolicy) {
  // M6 decision 17: the move's target object is not exempt by itself. With a
  // 3 mm clearance, a pick approach whose fingers come within 1 mm of it,
  // with no contact-grasp policy, is refused as a clearance violation; the
  // table keeps the clearance as well.
  ExecuteRequestDTO req = make_relative_object_request("clearance_pick", "graspable");
  req.clearance_m = 0.003;
  Pose3DTO reference;
  reference.position_m = {0.3, -0.2, 0.15};
  reference.orientation = {0.0, 0.0, 0.0, 1.0};
  context.next_reference_pose = Result<Pose3DTO>::Ok(reference);
  install_mock_retained_object(collision_world_publisher, "graspable");
  collision_world_publisher.current_snapshot = std::make_shared<const CollisionWorldSnapshot>(
      make_clearance_test_world(*collision_world_publisher.current_snapshot, {"table"}));
  const auto target = clearance_test_pair("graspable");
  const auto table = clearance_test_pair("table");
  // 4 mm at the start (outside the clearance), 1 mm at the end.
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    if (auto near_target = clearance_test_result(
            query, target, 0.004 - 0.003 * clearance_test_joint(query, "j1"))) {
      return near_target;
    }
    return clearance_test_result(query, table, 0.05);
  };

  execute_sync(req);
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
  EXPECT_EQ(snapshot.error->details.at("classification"), "clearance");
  EXPECT_DOUBLE_EQ(std::stod(snapshot.error->details.at("collision_clearance_m")), 0.003);
  const auto& margins = planner.last_request.retained_clearance_margins;
  ASSERT_TRUE(margins);
  EXPECT_DOUBLE_EQ(margins->margin_for(target), 0.003);
  EXPECT_DOUBLE_EQ(margins->margin_for(table), 0.003);
}

TEST_F(ExecutionKernelTest, Execute_PerMoveClearanceReplacesTheDeploymentClearance) {
  // M6 decision 16: with a 3 mm Deployment clearance, the same pick with the
  // move's own clearance_m of 0.5 mm is accepted, and the planner and
  // validation both carry 0.5 mm; 0 turns the clearance off for the move.
  ExecutionKernelConfig config;
  config.collision_clearance_m = 0.003;
  ExecutionKernel cell_kernel{planner, context, execution, clock, scheduler, cache, config};
  configure_authoritative_collision_validation(cell_kernel, collision_world_publisher,
                                               collision_engine);
  Pose3DTO reference;
  reference.position_m = {0.3, -0.2, 0.15};
  reference.orientation = {0.0, 0.0, 0.0, 1.0};
  install_mock_retained_object(collision_world_publisher, "graspable");
  collision_world_publisher.current_snapshot = std::make_shared<const CollisionWorldSnapshot>(
      make_clearance_test_world(*collision_world_publisher.current_snapshot, {"table"}));
  const auto target = clearance_test_pair("graspable");
  const auto table = clearance_test_pair("table");
  std::mutex margins_mutex;
  std::set<double> validation_margins;
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    if (query.waypoint_index && *query.waypoint_index > 0) {
      std::lock_guard lock(margins_mutex);
      validation_margins.insert(query.clearance_margins
                                    ? query.clearance_margins->margin_for(target)
                                    : 0.0);
    }
    if (auto near_target = clearance_test_result(
            query, target, 0.004 - 0.003 * clearance_test_joint(query, "j1"))) {
      return near_target;
    }
    return clearance_test_result(query, table, 0.05);
  };

  ExecuteRequestDTO req = make_relative_object_request("clearance_pick_close", "graspable");
  req.clearance_m = 0.0005;
  context.next_reference_pose = Result<Pose3DTO>::Ok(reference);
  const auto result = execute_sync(cell_kernel, req);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(ended_as(cell_kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  const auto& margins = planner.last_request.retained_clearance_margins;
  ASSERT_TRUE(margins);
  EXPECT_DOUBLE_EQ(margins->margin_m, 0.0005);
  EXPECT_DOUBLE_EQ(margins->margin_for(target), 0.0005);
  EXPECT_DOUBLE_EQ(margins->margin_for(table), 0.0005);
  {
    std::lock_guard lock(margins_mutex);
    EXPECT_EQ(validation_margins, (std::set<double>{0.0005}));
  }

  // A computed completion waits for its acknowledgement before the next move.
  ASSERT_TRUE(cell_kernel.acknowledge_completed(result.value()).accepted);
  ExecuteRequestDTO off = make_relative_object_request("clearance_pick_off", "graspable");
  off.clearance_m = 0.0;
  context.next_reference_pose = Result<Pose3DTO>::Ok(reference);
  const auto off_result = execute_sync(cell_kernel, off);
  ASSERT_TRUE(off_result.ok()) << off_result.error().message;
  EXPECT_EQ(ended_as(cell_kernel), "completed");
  EXPECT_FALSE(planner.last_request.retained_clearance_margins);
}

TEST_F(ExecutionKernelTest, Execute_ClearanceReleasesAMoveThatStartsInsideIt) {
  // With a 3 mm clearance, after a Stop 2 mm from the wall, the +Z jog away (clearance grows with
  // j1) is accepted: the wall pair keeps a margin just below its start
  // clearance, for the planner and for validation.
  collision_world_publisher.current_snapshot = std::make_shared<const CollisionWorldSnapshot>(
      make_clearance_test_world(make_test_collision_world_snapshot(), {"wall"}));
  const auto wall = clearance_test_pair("wall");
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    return clearance_test_result(query, wall, 0.002 + 0.01 * clearance_test_joint(query, "j1"));
  };

  auto request = make_test_request("clearance_jog_away");
  request.clearance_m = 0.003;
  const auto result = execute_sync(request);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  ASSERT_TRUE(planner.last_request.retained_clearance_margins);
  EXPECT_NEAR(planner.last_request.retained_clearance_margins->margin_for(wall),
              0.002 - kClearanceStartReleaseToleranceM, 1e-12);
}

TEST_F(ExecutionKernelTest, Execute_ClearanceRefusesAMoveCloserToWhatItStartsNear) {
  // The same start, but the move approaches the wall: refused once it comes
  // closer than the released margin, as a clearance violation.
  collision_world_publisher.current_snapshot = std::make_shared<const CollisionWorldSnapshot>(
      make_clearance_test_world(make_test_collision_world_snapshot(), {"wall"}));
  const auto wall = clearance_test_pair("wall");
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    return clearance_test_result(query, wall, 0.002 - 0.01 * clearance_test_joint(query, "j1"));
  };

  auto request = make_test_request("clearance_jog_toward");
  request.clearance_m = 0.003;
  execute_sync(request);
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
  EXPECT_EQ(snapshot.error->details.at("classification"), "clearance");
}

// --- Collision validation samples the executed curve (M55 F-1 / FIX2)

namespace {
// A fine step, so the curve tests see many inserted samples.
constexpr double kTestStepRad = 0.0005;

TrajectoryWaypointDTO waypoint(double time_s, std::vector<double> positions,
                               std::vector<double> velocities = {},
                               std::vector<double> accelerations = {}) {
  TrajectoryWaypointDTO point;
  point.time_from_start_s = time_s;
  point.positions_rad = std::move(positions);
  point.velocities_rad_s = std::move(velocities);
  point.accelerations_rad_s2 = std::move(accelerations);
  return point;
}

TrajectoryDTO two_joint_trajectory(std::vector<TrajectoryWaypointDTO> points) {
  TrajectoryDTO trajectory;
  trajectory.resources = {ResourceId("joint.1"), ResourceId("joint.2")};
  trajectory.joint_names = {"j1", "j2"};
  trajectory.waypoints = std::move(points);
  return trajectory;
}

// Equal endpoints, start velocity +1 and end velocity -1 over 1 s: the
// executed cubic is p(s) = s - s^2 (peak 0.25 at s = 0.5); the chord is 0.
TrajectoryDTO curved_cubic_trajectory() {
  return two_joint_trajectory({waypoint(0.0, {0.0, 0.0}, {1.0, 0.0}),
                               waypoint(1.0, {0.0, 0.0}, {-1.0, 0.0})});
}

double max_consecutive_step(const TrajectoryDTO& trajectory) {
  double step = 0.0;
  for (std::size_t i = 1; i < trajectory.waypoints.size(); ++i) {
    for (std::size_t j = 0; j < trajectory.joint_names.size(); ++j) {
      step = std::max(step, std::abs(trajectory.waypoints[i].positions_rad[j] -
                                     trajectory.waypoints[i - 1].positions_rad[j]));
    }
  }
  return step;
}

std::optional<double> joint_position(const CollisionQuery& query, const std::string& name) {
  const auto& names = query.robot_state.joint_names;
  const auto found = std::find(names.begin(), names.end(), name);
  if (found == names.end()) return std::nullopt;
  return query.robot_state.positions_rad.at(
      static_cast<std::size_t>(std::distance(names.begin(), found)));
}
}  // namespace

TEST(CollisionValidationSamplingTest, SamplesTheExecutedSplineNotTheChord) {
  const auto sampled = sample_for_collision_validation(
      curved_cubic_trajectory(), kTestStepRad, kValidationMaxCheckedStates, {});
  ASSERT_TRUE(sampled.ok()) << sampled.error().message;
  const auto& samples = sampled.value().trajectory;
  double peak = 0.0;
  for (const auto& point : samples.waypoints) peak = std::max(peak, point.positions_rad[0]);
  EXPECT_NEAR(peak, 0.25, 1e-6);
  EXPECT_LE(max_consecutive_step(samples), kTestStepRad + 1e-12);
  EXPECT_GT(sampled.value().inserted, 0U);
}

TEST(CollisionValidationSamplingTest, QuinticOvershootStaysWithinTheStep) {
  // Equal endpoints with velocity and acceleration: the quintic leaves and
  // returns; every joint step on the executed curve stays within the bound.
  const auto trajectory = two_joint_trajectory(
      {waypoint(0.0, {0.3, -0.2}, {2.0, -1.0}, {-5.0, 4.0}),
       waypoint(0.5, {0.3, -0.2}, {-1.5, 0.5}, {3.0, -2.0})});
  const auto sampled = sample_for_collision_validation(
      trajectory, kTestStepRad, kValidationMaxCheckedStates, {});
  ASSERT_TRUE(sampled.ok()) << sampled.error().message;
  EXPECT_GT(sampled.value().inserted, 0U);
  EXPECT_LE(max_consecutive_step(sampled.value().trajectory), kTestStepRad + 1e-12);
  // The samples lie on the shared sampler's curve.
  TrajectorySampleResult reference;
  for (const auto& point : sampled.value().trajectory.waypoints) {
    ASSERT_TRUE(
        sample_trajectory_prevalidated(trajectory, point.time_from_start_s, reference).ok());
    EXPECT_EQ(point.positions_rad, reference.positions_rad);
  }
}

TEST(CollisionValidationSamplingTest, FractionsReferToTheStoredTrajectory) {
  // 1 rad in the first segment, 0.001 rad in the second: a sample at 75% of
  // the first segment is at stored fraction 0.375, not at 75% of the samples.
  const auto trajectory = two_joint_trajectory(
      {waypoint(0.0, {0.0, 0.0}), waypoint(1.0, {1.0, 0.0}), waypoint(2.0, {1.001, 0.0})});
  const auto sampled = sample_for_collision_validation(
      trajectory, kTestStepRad, kValidationMaxCheckedStates, {});
  ASSERT_TRUE(sampled.ok()) << sampled.error().message;
  const auto& points = sampled.value().trajectory.waypoints;
  const auto& fractions = sampled.value().original_fractions;
  ASSERT_EQ(points.size(), fractions.size());
  EXPECT_EQ(fractions.front(), 0.0);
  EXPECT_EQ(fractions.back(), 1.0);
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (points[i].time_from_start_s == 1.0) {
      EXPECT_EQ(fractions[i], 0.5);
    }
    if (std::abs(points[i].positions_rad[0] - 0.75) < 1e-9) {
      EXPECT_NEAR(fractions[i], 0.375, 1e-12);
    }
    if (i > 0) {
      EXPECT_GT(fractions[i], fractions[i - 1]);
    }
  }
}

TEST(CollisionValidationSamplingTest, RefusesAboveTheSampleBudgetAndOnCancellation) {
  // Both joints 0 -> 1 rad: sqrt(2) / 0.0005 -> 2829 steps, 2830 samples.
  const auto trajectory = make_test_trajectory();
  const auto over = sample_for_collision_validation(trajectory, kTestStepRad, 2829, {});
  ASSERT_FALSE(over.ok());
  EXPECT_EQ(over.error().code, kErrorValidationCollision);
  EXPECT_EQ(over.error().details.at("reason"), "collision_validation_sample_budget_exceeded");
  EXPECT_TRUE(sample_for_collision_validation(trajectory, kTestStepRad, 2830, {}).ok());
  const auto huge = sample_for_collision_validation(
      make_test_trajectory_from({0.0, 0.0}, {1.0e300, 0.0}), kTestStepRad,
      kValidationMaxCheckedStates, {});
  ASSERT_FALSE(huge.ok());
  EXPECT_EQ(huge.error().details.at("reason"), "collision_validation_sample_budget_exceeded");
  int polls = 0;
  const auto cancelled = sample_for_collision_validation(
      trajectory, kTestStepRad, kValidationMaxCheckedStates,
      [&polls]() -> const char* {
        ++polls;
        return "collision_validation_cancelled";
      });
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().details.at("reason"), "collision_validation_cancelled");
  EXPECT_EQ(polls, 1);
}

TEST(CollisionValidationSamplingTest, StopInsideOneLongSegmentIsHonouredWithinThePollInterval) {
  // One stored segment of 90 rad needs 180,000 samples. A stop that arrives
  // after the second poll inside that segment ends sampling within one more
  // poll interval, not at the end of the segment.
  const auto trajectory = make_test_trajectory_from({0.0, 0.0}, {90.0, 0.0});
  int polls = 0;
  const auto stopped = sample_for_collision_validation(
      trajectory, kTestStepRad, kValidationMaxCheckedStates,
      [&polls]() -> const char* {
        return ++polls >= 3 ? "collision_validation_wall_timeout" : nullptr;
      });
  ASSERT_FALSE(stopped.ok());
  EXPECT_EQ(stopped.error().details.at("reason"), "collision_validation_wall_timeout");
  EXPECT_EQ(stopped.error().details.at("stopped_at_segment"), "0");
  EXPECT_EQ(polls, 3);
  EXPECT_LE(std::stoul(stopped.error().details.at("inserted_samples")),
            2 * kValidationSamplingPollInterval);
}

TEST(CollisionValidationSamplingTest, StepIsTheJointSpaceDistanceLikeThePlanner) {
  // Both joints move 1 rad: the configuration distance is sqrt(2) rad, so
  // the default planner step (0.05) gives ceil(sqrt(2) / 0.05) = 29 steps,
  // and no two consecutive samples are farther apart than the step.
  const auto trajectory = two_joint_trajectory(
      {waypoint(0.0, {0.0, 0.0}), waypoint(1.0, {1.0, 1.0})});
  const auto sampled = sample_for_collision_validation(
      trajectory, kDefaultCollisionCheckStepRad, kValidationMaxCheckedStates, {});
  ASSERT_TRUE(sampled.ok()) << sampled.error().message;
  const auto& points = sampled.value().trajectory.waypoints;
  ASSERT_EQ(points.size(), 30U);
  for (std::size_t i = 1; i < points.size(); ++i) {
    EXPECT_LE(std::hypot(points[i].positions_rad[0] - points[i - 1].positions_rad[0],
                         points[i].positions_rad[1] - points[i - 1].positions_rad[1]),
              kDefaultCollisionCheckStepRad + 1e-12);
  }
}

TEST_F(ExecutionKernelTest, Execute_ValidationRejectsCollisionOnTheSplineNotTheChord) {
  // Both stored waypoints are at 0, and so is every point of their chord; only
  // the executed cubic reaches j1 > 0.2. Neither the stored waypoints nor the
  // chord densification (F1) saw it.
  PlanResultDTO plan;
  plan.trajectory = curved_cubic_trajectory();
  planner.next_result = plan;
  collision_engine->result_for_query = [](const CollisionQuery& query) -> std::optional<CollisionResult> {
    const auto j1 = joint_position(query, "j1");
    if (j1 && *j1 > 0.2) return make_authoritative_collision_result();
    return std::nullopt;
  };
  execute_sync(make_test_request("collision_on_spline"));
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
}

TEST_F(ExecutionKernelTest, Execute_ValidationProjectsSplineSamplesWhenTheModelAddsJoints) {
  // The projector adds a non-group joint to the positions only (its V/A stay
  // group-sized). The spline is still sampled from the stored P/V/A, and every
  // query carries the projected joint count.
  context.expand_extra_joint = "finger";
  PlanResultDTO plan;
  plan.trajectory = curved_cubic_trajectory();
  planner.next_result = plan;
  std::mutex shapes_mutex;
  std::set<std::size_t> shapes;
  collision_engine->result_for_query =
      [&](const CollisionQuery& query) -> std::optional<CollisionResult> {
    {
      std::lock_guard<std::mutex> lock(shapes_mutex);
      shapes.insert(query.robot_state.positions_rad.size());
    }
    const auto j1 = joint_position(query, "j1");
    if (j1 && *j1 > 0.2) return make_authoritative_collision_result();
    return std::nullopt;
  };
  execute_sync(make_test_request("collision_on_spline_projected"));
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
  std::lock_guard<std::mutex> lock(shapes_mutex);
  EXPECT_EQ(shapes, (std::set<std::size_t>{3U}));
}

// Same contact-window decisions as before densification: the terminal
// window refers to the stored trajectory's fractions, not to sample indexes.
class ContactWindowFractionTest : public ExecutionKernelTest {
 protected:
  void run_contact_at(bool early) {
    install_mock_contact_grasp_world(collision_world_publisher, "graspable",
                                     {"right_finger", "left_finger"});
    install_mock_detached_logical_object(collision_world_publisher, "graspable");
    const CollisionBodyId allowed_body(
        "component/arm/model/arm_model/body/right_finger/collision/0");
    const CollisionPairId pair{allowed_body, "scene/graspable/shape/0"};
    PlanResultDTO plan;
    // 1 rad in the first segment, 0.001 rad in the second.
    plan.trajectory = two_joint_trajectory(
        {waypoint(0.0, {0.0, 0.0}), waypoint(1.0, {1.0, 0.0}), waypoint(2.0, {1.001, 0.0})});
    planner.next_result = plan;
    collision_engine->result_for_query =
        [pair, early](const CollisionQuery& query) -> std::optional<CollisionResult> {
      const auto j1 = joint_position(query, "j1");
      const bool contact = j1 && (early ? (*j1 > 0.84 && *j1 < 0.86) : *j1 >= 1.001);
      if (contact) return make_contact_grasp_collision_result(pair, -0.001);
      return std::nullopt;
    };
    auto request = make_test_request(early ? "grasp_contact_early" : "grasp_contact_final");
    request.contact_policy_id = "pick_grasp";
    request.runtime_object_bindings = {RuntimeObjectReferenceBindingDTO{
        RuntimeObjectId("selected_object"), SceneResourceId("graspable")}};
    request.resolved_contact_grasp =
        ResolvedContactGraspBindingDTO{ContactPolicyId("pick_grasp"),
                                       ContactPolicyRevision(kTestDigestA),
                                       RuntimeObjectId("selected_object"),
                                       SceneResourceId("graspable"),
                                       {allowed_body},
                                       ContactGraspEnvelopeDTO{0.002, 0.75}};
    execute_sync(request);
  }
};

TEST_F(ContactWindowFractionTest, ContactAt85PercentOfALongFirstSegmentIsBeforeTheWindow) {
  // Stored fraction 0.425 < 0.75; renormalising by sample index (F1) gave ~0.85.
  run_contact_at(true);
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ContactWindowFractionTest, ContactAtTheStoredFinalWaypointIsInsideTheWindow) {
  run_contact_at(false);
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
}

// --- Authoritative pre-execution collision via the manyforge_collision port

TEST_F(ExecutionKernelTest, Execute_AuthoritativeCollisionPipelineRejectsCollision) {
  collision_engine->default_result = make_authoritative_collision_result();

  execute_sync(make_test_request());
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_GE(collision_engine->check_count.load(), 1);
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
}

TEST_F(ExecutionKernelTest, Execute_BoundedPenetrationLaterFirstContactEmitsNativePolicyViolation) {
  const CollisionPairId pair{"scene/insert/shape/0", "scene/fixture/shape/0"};
  auto policy = make_test_collision_policy_snapshot();
  policy.policy_id = "bounded-predispatch-contact";
  policy.require_full_diagnostics = true;
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
  entry.max_contact_penetration_m = 0.005;
  policy.support_contacts.push_back(entry);
  collision_world_publisher.current_policy = policy;
  kernel.set_collision_validation_policy_snapshot(policy);

  CollisionResult initially_clear;
  initially_clear.engine_status = CollisionEngineStatus::kOk;
  initially_clear.decision = CollisionDecisionResult::kPass;
  collision_engine->result_by_waypoint[0] = initially_clear;
  collision_engine->result_by_waypoint[1] = make_support_contact_collision_result(pair, -0.006);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  const auto subscription = kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  execute_sync(make_test_request("bounded-predispatch-later-contact"));
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);

  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorContactPolicyViolation);
  EXPECT_EQ(snapshot.error->details.at("policy_id"), "bounded-predispatch-contact");
  EXPECT_EQ(snapshot.error->details.at("contact_policy_id"), "bounded-predispatch-contact");
  EXPECT_EQ(snapshot.error->details.at("within_penetration_bound"), "false");

  std::optional<KernelEvent> violation;
  std::optional<KernelEvent> error_raised;
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    const auto found = std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
      return event.type == KernelEventType::kContactPolicyViolation;
    });
    if (found != events.end()) {
      violation = *found;
    }
    const auto error_found =
        std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
          return event.type == KernelEventType::kErrorRaised && event.error.has_value();
        });
    if (error_found != events.end()) {
      error_raised = *error_found;
    }
  }
  kernel.unsubscribe_events(subscription);

  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->details.at("surface"), "pre_dispatch_validation");
  EXPECT_EQ(violation->details.at("contact_policy_id"), "bounded-predispatch-contact");
  EXPECT_EQ(violation->details.at("policy_mode"), "bounded_penetration");
  EXPECT_EQ(violation->details.at("initial_contact_pair_count"), "0");
  EXPECT_EQ(violation->details.at("current_contact_pair_count"), "1");
  EXPECT_EQ(violation->details.at("contact_present"), "true");
  EXPECT_EQ(violation->details.at("distance_request_mode"), "selected_colliding_pairs");
  EXPECT_EQ(violation->details.at("evaluated_distance_pair_count"), "1");
  EXPECT_DOUBLE_EQ(std::stod(violation->details.at("penetration_depth_m")), 0.006);
  EXPECT_DOUBLE_EQ(std::stod(violation->details.at("max_contact_penetration_m")), 0.005);
  EXPECT_EQ(violation->details.at("within_penetration_bound"), "false");
  EXPECT_EQ(violation->details.at("response"), "block_dispatch");
  EXPECT_EQ(violation->details.find("release_evidence_mode"), violation->details.end());

  ASSERT_TRUE(error_raised.has_value());
  ASSERT_TRUE(error_raised->error.has_value());
  EXPECT_EQ(error_raised->error->code, kErrorContactPolicyViolation);
  EXPECT_EQ(error_raised->error->details.at("policy_id"), "bounded-predispatch-contact");
  EXPECT_EQ(error_raised->error->details.at("within_penetration_bound"), "false");
}

TEST_F(ExecutionKernelTest, Execute_AuthoritativeCollisionDiagnosticsReportClassifiedFailurePair) {
  CollisionPairId allowed_pair{"scene/box_a/shape/0", "scene/box_b/shape/0"};
  CollisionPairId blocking_pair{"scene/box_a/shape/0",
                                "component/arm/model/arm_model/body/link1/collision/0"};
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
            "scene/box_a/shape/0 <-> scene/box_b/shape/0");
  EXPECT_EQ(details.at("collision_result_first_failure_pair"),
            "component/arm/model/arm_model/body/link1/collision/0 <-> "
            "scene/box_a/shape/0");
  EXPECT_EQ(details.at("collision_result_first_failure_classification"), "blocking_collision");
  EXPECT_EQ(details.at("collision_result_first_failure_joint_names"), "j1,j2");
  EXPECT_EQ(details.at("collision_result_first_failure_positions_rad"), "0,0");
  EXPECT_EQ(details.at("collision_result_first_failure_time_from_start_s"), "0.000000");
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
    EXPECT_EQ(event.details.at("collision_result_first_failure_pair"),
              "component/arm/model/arm_model/body/link1/collision/0 <-> "
              "scene/box_a/shape/0");
    EXPECT_EQ(event.details.at("collision_result_first_failure_classification"),
              "blocking_collision");
    EXPECT_EQ(event.error->details.at("collision_result_first_failure_pair"),
              event.details.at("collision_result_first_failure_pair"));
    EXPECT_EQ(event.details.at("collision_result_first_failure_positions_rad"), "0,0");
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

TEST_F(ExecutionKernelTest, Execute_AuthoritativeCollisionUsesNonzeroDefaultQueryTimeout) {
  auto result = execute_sync(make_test_request("validation_default_timeout"));

  ASSERT_TRUE(result.ok());
  EXPECT_GE(collision_engine->check_count.load(), 1);
  ASSERT_TRUE(collision_engine->last_timeout_budget_ns.has_value());
  EXPECT_EQ(*collision_engine->last_timeout_budget_ns, 250'000'000);
}

TEST_F(ExecutionKernelTest, Execute_AuthoritativeCollisionUsesConfiguredValidationQueryTimeout) {
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernelConfig config;
  config.validation_collision_query_timeout_ns = 125'000'000;
  ExecutionKernel local_kernel{planner, context, execution, clock, scheduler, cache, config};
  configure_authoritative_collision_validation(local_kernel, local_publisher, local_engine);

  auto result = prepare_and_dispatch(local_kernel, make_test_request("validation_timeout"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_GE(local_engine->check_count.load(), 1);
  ASSERT_TRUE(local_engine->last_timeout_budget_ns.has_value());
  EXPECT_EQ(*local_engine->last_timeout_budget_ns, 125'000'000);
}

TEST_F(ExecutionKernelTest, Execute_AuthoritativeCollisionUsesPublisherPolicySnapshot) {
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

TEST_F(ExecutionKernelTest, Execute_MissingAuthoritativeCollisionPipelineFailsClosed) {
  ExecutionKernel unconfigured_kernel{planner, context, execution, clock, scheduler, cache};

  auto result = prepare_and_dispatch(unconfigured_kernel, make_test_request("missing_pipeline"));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorExecutionRejected);
  EXPECT_EQ(result.error().details.at("reason"), "scene_projection_authority_unavailable");
  EXPECT_EQ(unconfigured_kernel.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_AuthoritativeCollisionReplansAndDispatchesSecondCandidate) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 1;
  MockPlanner local_planner;
  PlanResultDTO first_plan;
  first_plan.trajectory = make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  PlanResultDTO second_plan;
  second_plan.trajectory = make_test_trajectory_from({0.0, 0.0}, {2.0, 2.0});
  local_planner.result_sequence = {first_plan, second_plan};
  MockFocusedPorts local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner, local_context,   controlled_execution,
                               local_clock,   local_scheduler, local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher, local_engine);
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

  auto result = prepare_and_dispatch(local_kernel, make_test_request("replan_collision"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  controlled_execution.complete();
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_EQ(ended_as(local_kernel), "completed");
  EXPECT_EQ(local_planner.plan_count, 2);
  EXPECT_EQ(local_planner.planning_attempt_indices, (std::vector<std::uint32_t>{0, 1}));
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
      if (reason != event.details.end() && reason->second == "authoritative_validation_replan") {
        saw_retry_validation_failed = true;
      }
    }
    if (event.type == KernelEventType::kPlanStarted) {
      auto reason = event.details.find("reason");
      if (reason != event.details.end() && reason->second == "authoritative_validation_replan") {
        saw_retry_plan_started = true;
      }
    }
    saw_terminal_error = saw_terminal_error || event.type == KernelEventType::kErrorRaised;
  }
  EXPECT_TRUE(saw_retry_validation_failed);
  EXPECT_TRUE(saw_retry_plan_started);
  EXPECT_FALSE(saw_terminal_error);
}

TEST_F(ExecutionKernelTest, Execute_AuthoritativeCollisionReplanBudgetExhaustedFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 1;
  MockPlanner local_planner;
  PlanResultDTO first_plan;
  first_plan.trajectory = make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  PlanResultDTO second_plan;
  second_plan.trajectory = make_test_trajectory_from({0.0, 0.0}, {2.0, 2.0});
  local_planner.result_sequence = {first_plan, second_plan};
  MockFocusedPorts local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner, local_context,   controlled_execution,
                               local_clock,   local_scheduler, local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher, local_engine);
  local_engine->default_result = make_authoritative_collision_result();

  auto result = prepare_and_dispatch(local_kernel, make_test_request("replan_exhausted"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_EQ(ended_as(local_kernel), "failed");
  EXPECT_EQ(local_planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.execute_count, 0);
  auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
  EXPECT_EQ(snapshot.error->details.at("collision_result_decision"), "fail");
}

TEST_F(ExecutionKernelTest, Execute_AuthoritativeContactPolicyViolationRetainsReplanBehavior) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 1;
  MockPlanner local_planner;
  PlanResultDTO first_plan;
  first_plan.trajectory = make_test_trajectory_from({0.0, 0.0}, {1.0, 1.0});
  PlanResultDTO second_plan;
  second_plan.trajectory = make_test_trajectory_from({0.0, 0.0}, {2.0, 2.0});
  local_planner.result_sequence = {first_plan, second_plan};
  MockFocusedPorts local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner, local_context,   controlled_execution,
                               local_clock,   local_scheduler, local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher, local_engine);

  const CollisionPairId pair{"scene/insert/shape/0", "scene/fixture/shape/0"};
  auto policy = make_test_collision_policy_snapshot();
  policy.policy_id = "bounded-replan-policy";
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
  entry.max_contact_penetration_m = 0.005;
  policy.support_contacts.push_back(entry);
  local_publisher.current_policy = policy;
  local_kernel.set_collision_validation_policy_snapshot(policy);
  local_engine->default_result = make_support_contact_collision_result(pair, -0.006);

  const auto result =
      prepare_and_dispatch(local_kernel, make_test_request("contact_policy_replan"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_EQ(ended_as(local_kernel), "failed");
  EXPECT_EQ(local_planner.plan_count, 2);
  EXPECT_EQ(controlled_execution.execute_count, 0);
  const auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorContactPolicyViolation);
  EXPECT_EQ(snapshot.error->details.at("policy_id"), "bounded-replan-policy");
}

TEST_F(ExecutionKernelTest,
       Execute_DiscoversOnlyActualCarriedObjectStartContactAndDispatchesAfterRelease) {
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockPlanner local_planner;
  PlanResultDTO plan;
  plan.trajectory = make_test_trajectory();
  local_planner.next_result = plan;
  MockFocusedPorts local_context = context;
  MockExecution local_execution;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 0;
  config.validation_collision_workers = 1;
  ExecutionKernel local_kernel{local_planner,   local_context, local_execution, local_clock,
                               local_scheduler, local_cache,   config};
  configure_authoritative_collision_validation(local_kernel, local_publisher, local_engine);

  auto world = make_test_collision_world_snapshot();
  auto carried = make_observed_box_body("carried", make_test_pose(), "carried-body-hash");
  carried.parent_body_id = "component/arm/model/arm_model/body/tool0/collision/0";
  carried.frame_id = *carried.parent_body_id;
  auto detached = make_observed_box_body("fixture", make_test_pose(), "fixture-body-hash");
  const CollisionPairId carried_contact{carried.body_id, detached.body_id};
  world.bodies = {make_test_robot_link_body("tool0"), std::move(carried), std::move(detached)};
  std::sort(world.bodies.begin(), world.bodies.end(),
            [](const auto& left, const auto& right) { return left.body_id < right.body_id; });
  world.body_count = static_cast<std::int64_t>(world.bodies.size());
  world.geometry_count = world.body_count;
  world.scene_resource_count = 2;
  world.component_model_body_count = 1;
  local_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(world));

  auto touching = make_support_contact_collision_result(carried_contact, 0.0);
  touching.in_collision = true;
  touching.normalized_pairs = {carried_contact};
  touching.decision = CollisionDecisionResult::kFail;
  const auto separated = make_support_contact_collision_result(carried_contact, 0.01);
  local_engine->result_for_query =
      [touching, separated](const CollisionQuery& query) -> std::optional<CollisionResult> {
    return query.waypoint_index == 0 ? touching : separated;
  };

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  local_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  const auto result =
      prepare_and_dispatch(local_kernel, make_test_request("dynamic-carried-start-contact"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  const auto terminal_snapshot = local_kernel.get_execution_snapshot();
  const auto terminal_diagnostic = [&]() {
    if (!terminal_snapshot.error.has_value()) {
      return std::string("no terminal error");
    }
    const auto reason = terminal_snapshot.error->details.find("reason");
    return terminal_snapshot.error->code + ": " + terminal_snapshot.error->message +
           (reason == terminal_snapshot.error->details.end() ? std::string{}
                                                             : " (" + reason->second + ")");
  }();
  EXPECT_EQ(ended_as(local_kernel), "completed") << terminal_diagnostic;
  EXPECT_EQ(local_execution.execute_count, 1);
  EXPECT_EQ(local_planner.plan_count, 1);
  EXPECT_GE(local_engine->check_count.load(), 3);

  std::lock_guard<std::mutex> lock(events_mutex);
  const auto validation = std::find_if(events.begin(), events.end(), [](const auto& event) {
    return event.type == KernelEventType::kValidationPassed &&
           event.details.contains("dynamically_discovered_start_contact_count");
  });
  ASSERT_NE(validation, events.end());
  EXPECT_EQ(validation->details.at("dynamically_discovered_start_contact_count"), "1");
}

TEST_F(ExecutionKernelTest, Execute_DoesNotPromoteDetachedCollisionOrNoncollidingCarriedPair) {
  FakeClock local_clock;
  ManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockPlanner local_planner;
  PlanResultDTO plan;
  plan.trajectory = make_test_trajectory();
  local_planner.next_result = plan;
  MockFocusedPorts local_context = context;
  MockExecution local_execution;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernelConfig config;
  config.authoritative_validation_collision_replan_attempts = 0;
  config.validation_collision_workers = 1;
  ExecutionKernel local_kernel{local_planner,   local_context, local_execution, local_clock,
                               local_scheduler, local_cache,   config};
  configure_authoritative_collision_validation(local_kernel, local_publisher, local_engine);

  auto world = make_test_collision_world_snapshot();
  auto carried = make_observed_box_body("carried", make_test_pose(), "carried-body-hash");
  carried.parent_body_id = "component/arm/model/arm_model/body/tool0/collision/0";
  carried.frame_id = *carried.parent_body_id;
  auto detached_a = make_observed_box_body("fixture-a", make_test_pose(), "fixture-a-body-hash");
  auto detached_b = make_observed_box_body("fixture-b", make_test_pose(), "fixture-b-body-hash");
  const CollisionPairId blocking_detached_pair{detached_a.body_id, detached_b.body_id};
  const CollisionPairId noncolliding_carried_pair{carried.body_id, detached_a.body_id};
  world.bodies = {make_test_robot_link_body("tool0"), std::move(carried), std::move(detached_a),
                  std::move(detached_b)};
  std::sort(world.bodies.begin(), world.bodies.end(),
            [](const auto& left, const auto& right) { return left.body_id < right.body_id; });
  world.body_count = static_cast<std::int64_t>(world.bodies.size());
  world.geometry_count = world.body_count;
  world.scene_resource_count = 3;
  world.component_model_body_count = 1;
  local_publisher.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(world));

  auto blocking = make_support_contact_collision_result(blocking_detached_pair, 0.0);
  blocking.in_collision = true;
  blocking.normalized_pairs = {blocking_detached_pair};
  blocking.decision = CollisionDecisionResult::kFail;
  blocking.pair_clearances.push_back({noncolliding_carried_pair, 0.0});
  local_engine->result_by_waypoint.emplace(0, std::move(blocking));

  const auto result =
      prepare_and_dispatch(local_kernel, make_test_request("reject-noncarried-start-contact"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(ended_as(local_kernel), "failed");
  EXPECT_EQ(local_execution.execute_count, 0);
  EXPECT_EQ(local_planner.plan_count, 1);
  const auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorValidationCollision);
  EXPECT_EQ(snapshot.error->details.at("dynamically_discovered_start_contact_count"), "0");
}

TEST_F(ExecutionKernelTest, Execute_AuthoritativeValidationNonOkCollisionDoesNotReplan) {
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
  MockFocusedPorts local_context = context;
  MockCollisionWorldPublisher local_publisher;
  auto local_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{local_planner, local_context,   controlled_execution,
                               local_clock,   local_scheduler, local_cache,
                               config};
  configure_authoritative_collision_validation(local_kernel, local_publisher, local_engine);
  auto non_ok_collision = make_authoritative_collision_result();
  non_ok_collision.engine_status = CollisionEngineStatus::kTimeout;
  local_engine->default_result = non_ok_collision;

  auto result = prepare_and_dispatch(local_kernel, make_test_request("replan_non_ok"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));

  EXPECT_EQ(ended_as(local_kernel), "failed");
  EXPECT_EQ(local_planner.plan_count, 1);
  EXPECT_EQ(controlled_execution.execute_count, 0);
  auto snapshot = local_kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("collision_result_engine_status"), "timeout");
}

// --- Execute requires bound acknowledgement from terminal success ---

TEST_F(ExecutionKernelTest, Execute_FromCompleted_RequiresAcknowledgement) {
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(ended_as(kernel), "completed");

  auto rejected = prepare_and_dispatch(kernel, make_test_request());
  EXPECT_FALSE(rejected.ok());
  EXPECT_EQ(ended_as(kernel), "completed");

  auto stale_ack = kernel.acknowledge_completed(first.value() + 1);
  EXPECT_FALSE(stale_ack.accepted);
  ASSERT_TRUE(stale_ack.error.has_value());
  EXPECT_EQ(stale_ack.error->details.at("reason_code"), "mismatched_execution_id");
  EXPECT_EQ(ended_as(kernel), "completed");

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
  EXPECT_EQ(ended_as(kernel), "completed");
}

TEST_F(ExecutionKernelTest, AcknowledgeCompleted_FromFailed_Rejected) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;
  auto result = execute_sync(make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(ended_as(kernel), "failed");

  auto ack = kernel.acknowledge_completed(result.value());
  EXPECT_FALSE(ack.accepted);
  ASSERT_TRUE(ack.error.has_value());
  EXPECT_EQ(ack.error->details.at("reason_code"), "wrong_state");
  EXPECT_EQ(ended_as(kernel), "failed");
}

// --- Version-bound recovery action API ---

TEST_F(ExecutionKernelTest, Execute_FromInterventionRequired_Rejected) {
  execution.next_terminal_state = ExecutionState::kInterventionRequired;
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(ended_as(kernel), "failed_effect_uncertain");

  auto rejected = prepare_and_dispatch(kernel, make_test_request());
  EXPECT_FALSE(rejected.ok());
  EXPECT_EQ(ended_as(kernel), "failed_effect_uncertain");
  ASSERT_TRUE(rejected.error().details.contains("reason_code"));
  EXPECT_EQ(rejected.error().details.at("reason_code"), "intervention_required");
}

TEST_F(ExecutionKernelTest, ApplyRecoveryAction_AcknowledgeAndContinueDoesNotResetBackend) {
  execution.next_terminal_state = ExecutionState::kInterventionRequired;
  execution.snapshot.error = ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                                      "operator stop total failure",
                                      {{"reason", "operator_stop_total_failure"},
                                       {"phase", "operator_stop"},
                                       {"requested_stop_mode", "hard_deceleration"},
                                       {"executed_stop_mode", "hard_deceleration"}}};
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(ended_as(kernel), "failed_effect_uncertain");
  EXPECT_EQ(execution.reset_count, 0);
  install_at_rest_recovery_observation();

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto ack =
      apply_recovery_action_after_projection(kernel, RecoveryAction::kAcknowledgeAndContinue,
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

TEST_F(ExecutionKernelTest, ApplyRecoveryAction_AcknowledgeAndContinueRejectsWhileMoving) {
  execution.next_terminal_state = ExecutionState::kInterventionRequired;
  execution.snapshot.joint_velocities_rad_s = {0.2, 0.0};
  execution.snapshot.error = ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                                      "operator stop not settled",
                                      {{"reason", "stop_not_settled_measured_velocity"},
                                       {"phase", "operator_stop"},
                                       {"requested_stop_mode", "soft_on_path"},
                                       {"executed_stop_mode", "soft_on_path"},
                                       {"settle_velocity_threshold_rad_s", "0.01"}}};
  auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(ended_as(kernel), "failed_effect_uncertain");
  execution.publish_observed_state(clock.now(), {1.0, 1.0}, {0.2, 0.0});

  auto ack =
      apply_recovery_action_after_projection(kernel, RecoveryAction::kAcknowledgeAndContinue,
                                             "operator_alice", "ui_acknowledge_and_continue");
  EXPECT_FALSE(ack.accepted);
  ASSERT_TRUE(ack.error.has_value());
  EXPECT_EQ(ack.error->details.at("reason"), reason_codes::kRecoveryObservedStateNotAtRest);
  EXPECT_EQ(ended_as(kernel), "failed_effect_uncertain");
  EXPECT_EQ(execution.reset_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_FromFailed_ResetsStateAndRetriesWithoutBackendReset) {
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_err;

  auto first = execute_sync(make_test_request("first_fails"));
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_EQ(execution.reset_count, 0);

  PlanResultDTO plan_ok;
  plan_ok.trajectory = make_test_trajectory();
  planner.next_result = plan_ok;

  auto retry = execute_sync(make_test_request("retry_after_failure"));
  ASSERT_TRUE(retry.ok());
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_EQ(execution.reset_count, 0);
  EXPECT_EQ(execution.sync_count, 2);
}

// --- Stop ---

TEST_F(ExecutionKernelTest,
       CollisionValidationAdmission_WithSharedRuntimeResourceDefersWithoutStoppingMonitor) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_immediate_query_budget_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 0;
  MockCollisionWorldPublisher local_publisher;
  auto shared_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher, shared_engine,
                                         shared_engine);

  auto result = prepare_and_dispatch(local_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  const int baseline_runtime_checks = shared_engine->check_count.load();
  const auto admission_error = local_kernel.collision_validation_admission_error();

  ASSERT_TRUE(admission_error.has_value());
  EXPECT_EQ(admission_error->code, kErrorValidationDeferred);
  EXPECT_EQ(admission_error->details.at("reason"), "validation_deferred_runtime_monitor_active");
  EXPECT_EQ(admission_error->details.at("resource_conflict_reason"), "same_engine_instance");
  EXPECT_EQ(controlled_execution.stop_attempt_count, 0);

  for (int i = 0; i < 3; ++i) {
    local_clock.advance(config.collision_monitor_period_ns);
    local_scheduler.advance_to(local_clock.now());
  }

  EXPECT_GE(shared_engine->check_count.load(), baseline_runtime_checks + 3);
  EXPECT_EQ(local_kernel.get_status().state, ExecutionState::kExecuting);
}

TEST_F(ExecutionKernelTest, RuntimeCollisionPublishedBundleFailureFailsClosedWithoutStaleFallback) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_immediate_query_budget_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 0;
  MockCollisionWorldPublisher local_publisher;
  auto validation_engine = std::make_shared<MockCollisionEngine>();
  auto runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher, validation_engine,
                                         runtime_engine);

  local_kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto result = prepare_and_dispatch(local_kernel, make_test_request("published_bundle_failure"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(local_kernel, ExecutionState::kExecuting));

  // Keep construction, admission, and dispatch publication healthy, then
  // fail the monitor tick's next leaf read. Derive the injection point after
  // dispatch so added fail-closed admission reads cannot silently turn this
  // into a stale call-count fixture. The tick must not fall back to the
  // compatibility store or call the runtime engine against an old world.
  local_publisher.throw_on_published_runtime_bundle_call_numbers = {
      local_publisher.published_runtime_bundle_call_count.load() + 1};
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
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
      return e.type == KernelEventType::kCollisionDetected && e.details.contains("reason") &&
             e.details.at("reason") == "published_runtime_collision_snapshot_exception";
    }));
  }

  controlled_execution.release();
  ASSERT_TRUE(local_kernel.wait_for_completion(1'000'000'000));
}

TEST_F(ExecutionKernelTest, RuntimeMonitorPrepareFailureIsCriticalFaultWithoutDispatch) {
  collision_runtime_engine->runtime_query_prepare_status =
      RuntimeCollisionQueryPrepareStatus::kEngineError;
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  const auto failed = execute_sync(make_test_request("monitor_prepare_failure"));

  ASSERT_TRUE(failed.ok());
  EXPECT_EQ(ended_as(kernel), "failed");
  EXPECT_EQ(execution.execute_count, 0) << "nothing may be sent without a prepared monitor";
  const auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->code, kErrorRuntimeCollisionMonitorPrepareFailed);
  EXPECT_EQ(snapshot.error->details.at("reason_code"),
            kErrorRuntimeCollisionMonitorPrepareFailed);
  EXPECT_EQ(snapshot.error->details.at("reason"), "runtime_collision_query_prepare_failed");
  EXPECT_EQ(snapshot.error->details.at("prepare_error_code"), kErrorValidationCollision);
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
      return e.type == KernelEventType::kParticipantSettled &&
             e.details.at("reason") == "execution_critical_fault" &&
             e.details.at("participant_effect") ==
                 to_string(ParticipantEffectKnowledge::kNoEffect);
    }));
    EXPECT_FALSE(std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
      return e.type == KernelEventType::kCollisionDetected;
    }));
  }

  // Recovery is the ordinary Reset path, not a Stop-fault latch.
  scheduler.advance_to(clock.now());
  const auto assessment = kernel.get_recovery_assessment();
  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kUnknownFault);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kResetRequired);
  EXPECT_FALSE(
      apply_recovery_action_after_projection(kernel, RecoveryAction::kRetryAfterSceneRepair)
          .accepted);
  const auto reset = kernel.reset_after_certified_containment(failed.value());
  ASSERT_TRUE(reset.accepted) << reset.error->message;
  EXPECT_EQ(ended_as(kernel), "working:Idle");
  EXPECT_TRUE(kernel.get_recovery_assessment().latch_id.empty());

  collision_runtime_engine->runtime_query_prepare_status =
      RuntimeCollisionQueryPrepareStatus::kReady;
  const auto next = execute_sync(make_test_request("after_monitor_prepare_reset"));
  ASSERT_TRUE(next.ok());
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_EQ(execution.execute_count, 1);
}

TEST_F(ExecutionKernelTest, RuntimeProtectionLeaseIsActiveBeforeBackendExecuteStarts) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_immediate_query_budget_ns = 10'000'000;
  config.runtime_observed_state_max_age_ns = 0;
  MockCollisionWorldPublisher local_publisher;
  auto shared_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel local_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(local_kernel, local_publisher, shared_engine,
                                         shared_engine);

  std::optional<ErrorDTO> admission_during_backend_execute;
  controlled_execution.on_execute = [&]() {
    admission_during_backend_execute = local_kernel.collision_validation_admission_error();
  };

  auto result = prepare_and_dispatch(local_kernel, make_test_request());

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(admission_during_backend_execute.has_value());
  EXPECT_EQ(admission_during_backend_execute->code, kErrorValidationDeferred);
  EXPECT_EQ(admission_during_backend_execute->details.at("reason"),
            "validation_deferred_runtime_monitor_active");
  EXPECT_EQ(admission_during_backend_execute->details.at("resource_conflict_reason"),
            "same_engine_instance");
}

// --- Reset ---

// --- Collision monitoring tick ---

TEST_F(ExecutionKernelTest, CollisionCheckTick_NotExecuting_NoOp) {
  auto result = kernel.collision_check_tick();
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(context.collision_count.load(), 0);
}

// Step 3 regression: the participant reports a runtime collision as a fact
// only. It keeps no local fault latch, commits no terminal and issues no
// backend command; the addressed Kernel owns containment. Retirement after the
// owner's certified containment is motion-free: it retires the prepared
// membership and never resets the backend, so positions are preserved.
TEST_F(ExecutionKernelTest, RuntimeCollisionIsAFactAndCertifiedRetirementIsMotionFree) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_immediate_query_budget_ns = 10'000'000;
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mu;
  std::condition_variable events_cv;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    {
      std::lock_guard<std::mutex> lock(events_mu);
      events.push_back(event);
    }
    events_cv.notify_all();
  });

  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{100, test_digest("addressed-owner-scene")};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));
  const auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_runtime_engine->default_result = make_authoritative_collision_result();
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));

  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  {
    std::unique_lock<std::mutex> lock(events_mu);
    ASSERT_TRUE(events_cv.wait_for(lock, std::chrono::seconds(1), [&]() {
      return std::any_of(events.begin(), events.end(), [](const KernelEvent& event) {
        return event.type == KernelEventType::kCollisionDetected;
      });
    }));
  }
  {
    std::lock_guard<std::mutex> lock(events_mu);
    EXPECT_TRUE(std::none_of(events.begin(), events.end(), [](const KernelEvent& event) {
      return event.type == KernelEventType::kInterruptLatched ||
             event.type == KernelEventType::kParticipantSettled;
    }));
    const auto collision = std::find_if(events.begin(), events.end(), [](const auto& event) {
      return event.type == KernelEventType::kCollisionDetected;
    });
    ASSERT_NE(collision, events.end());
    EXPECT_EQ(collision->details.at("in_collision"), "true");
  }
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_execution.controlled_stop_count_value(), 0);
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");

  // The owner fences and certifies containment; the backend then ends.
  ASSERT_EQ(controlled_kernel.fence_addressed_participant_stop(result.value()),
            AddressedParticipantStopFenceDisposition::kDispatchCommitted);
  controlled_execution.release();
  EXPECT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000));
  EXPECT_EQ(ended_as(controlled_kernel), "preempted_by_addressed_stop");
  const auto positions_before = controlled_execution.get_execution_snapshot().joint_positions_rad;
  const auto retired_before = controlled_execution.retire_count.load();
  const auto reset = controlled_kernel.reset_after_certified_containment(result.value());
  ASSERT_TRUE(reset.accepted) << reset.error->message;
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_execution.reset_count, 0);
  EXPECT_EQ(controlled_execution.retire_count.load(), retired_before + 1);
  EXPECT_EQ(controlled_execution.get_execution_snapshot().joint_positions_rad, positions_before);
  EXPECT_EQ(ended_as(controlled_kernel), "working:Idle");
  EXPECT_TRUE(controlled_kernel.get_recovery_assessment().latch_id.empty());
}

TEST_F(ExecutionKernelTest, AddressedStopFenceWinsLookaheadHandoffBeforeInterruptedProjection) {
  ControlledExecution controlled_execution;
  controlled_execution.enable_execution_progress_publication();
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_immediate_query_budget_ns = 10'000'000;
  config.runtime_collision_lookahead_cycle_budget_ns = 100'000'000;
  config.runtime_collision_lookahead_query_budget_ns = 75'000'000;
  config.runtime_collision_lookahead_horizon_ns = 2'000'000'000;
  config.runtime_collision_lookahead_max_samples = 8;
  config.runtime_collision_lookahead_workers = 1;
  config.runtime_collision_lookahead_workers_declared = true;
  config.runtime_collision_lookahead_min_waypoints_for_parallel = 2;
  MockCollisionWorldPublisher local_collision_world;
  auto local_validation_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  auto local_lookahead_engine = std::make_shared<MockCollisionEngine>();
  local_lookahead_engine->default_result = make_authoritative_collision_result();
  std::mutex events_mutex;
  std::condition_variable events_changed;
  std::vector<KernelEvent> events;
  std::atomic<std::uint64_t> addressed_execution_id{0U};
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_validation_engine, local_runtime_engine,
                                         {local_lookahead_engine});
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    const auto surface = event.details.find("surface");
    if (event.type == KernelEventType::kCollisionDetected && surface != event.details.end() &&
        surface->second == "addressed_runtime_collision_authority") {
      controlled_kernel.fence_addressed_participant_stop(
          addressed_execution_id.load(std::memory_order_acquire));
    }
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      events.push_back(event);
    }
    events_changed.notify_all();
  });

  const auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  addressed_execution_id.store(result.value(), std::memory_order_release);
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));
  const auto permit = drive_until_lookahead_permit_admitted(
      controlled_kernel, local_clock, local_scheduler, config.collision_monitor_period_ns);
  ASSERT_TRUE(permit.admitted);

  std::vector<KernelEvent> captured;
  {
    std::unique_lock<std::mutex> lock(events_mutex);
    ASSERT_TRUE(events_changed.wait_for(lock, std::chrono::seconds(5), [&]() {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        const auto surface = event.details.find("surface");
        return event.type == KernelEventType::kCollisionDetected &&
               surface != event.details.end() &&
               surface->second == "addressed_runtime_collision_authority";
      });
    }));
  }
  for (int attempt = 0; attempt < 1000; ++attempt) {
    if (controlled_kernel.get_metrics().runtime_collision_lookahead_stale_results > 0) {
      break;
    }
    ASSERT_LT(attempt, 999)
        << "fenced lookahead result did not settle as generation-stale after addressed handoff";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    captured = events;
  }
  const auto handoff = std::find_if(captured.begin(), captured.end(), [](const auto& event) {
    const auto surface = event.details.find("surface");
    return event.type == KernelEventType::kCollisionDetected && surface != event.details.end() &&
           surface->second == "addressed_runtime_collision_authority";
  });
  const auto interrupted = std::find_if(captured.begin(), captured.end(), [](const auto& event) {
    return event.type == KernelEventType::kStateTransition &&
           event.to_state == ExecutionState::kInterrupted;
  });
  ASSERT_NE(handoff, captured.end());
  EXPECT_EQ(interrupted, captured.end());
  EXPECT_EQ(handoff->details.at("addressed_stop_authority_handoff"), "true");
  EXPECT_TRUE(std::none_of(captured.begin(), captured.end(), [](const auto& event) {
    return event.type == KernelEventType::kInterruptLatched;
  }));
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.release();
  EXPECT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000));
}

TEST_F(ExecutionKernelTest, CollisionLookahead_FutureCollisionSignalsAuthorityBeforeTelemetry) {
  ControlledExecution controlled_execution;
  controlled_execution.enable_execution_progress_publication();
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_immediate_query_budget_ns = 10'000'000;
  config.runtime_collision_lookahead_cycle_budget_ns = 10'000'000;
  config.runtime_collision_lookahead_query_budget_ns = 10'000'000;
  config.runtime_collision_lookahead_horizon_ns = 2'000'000'000;
  config.runtime_collision_lookahead_max_samples = 8;
  config.runtime_collision_lookahead_workers = 1;
  config.runtime_collision_lookahead_workers_declared = true;
  config.runtime_collision_lookahead_min_waypoints_for_parallel = 2;
  MockCollisionWorldPublisher local_collision_world;
  auto local_validation_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  auto local_lookahead_engine = std::make_shared<MockCollisionEngine>();
  local_lookahead_engine->default_result = make_authoritative_collision_result();
  std::mutex events_mu;
  std::condition_variable events_cv;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_validation_engine, local_runtime_engine,
                                         {local_lookahead_engine});
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    {
      std::lock_guard<std::mutex> lock(events_mu);
      events.push_back(event);
    }
    events_cv.notify_all();
  });

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));

  const auto permit = drive_until_lookahead_permit_admitted(
      controlled_kernel, local_clock, local_scheduler, config.collision_monitor_period_ns);
  ASSERT_TRUE(permit.admitted)
      << "periodic clear permits were never admitted; try-lock misses="
      << permit.metrics.runtime_collision_lookahead_clear_permit_try_lock_misses;

  std::vector<KernelEvent> captured_events;
  {
    std::unique_lock<std::mutex> lock(events_mu);
    ASSERT_TRUE(events_cv.wait_for(lock, std::chrono::seconds(5), [&]() {
      return std::any_of(events.begin(), events.end(), [](const KernelEvent& event) {
        const auto surface = event.details.find("surface");
        return event.type == KernelEventType::kCollisionDetected &&
               surface != event.details.end() && surface->second == "observed_runtime_lookahead";
      });
    })) << "future collision did not reach kernel telemetry";
    captured_events = events;
  }

  const auto lookahead_event =
      std::find_if(captured_events.begin(), captured_events.end(), [](const KernelEvent& event) {
        const auto surface = event.details.find("surface");
        return event.type == KernelEventType::kCollisionDetected &&
               surface != event.details.end() && surface->second == "observed_runtime_lookahead";
      });
  ASSERT_NE(lookahead_event, captured_events.end());
  EXPECT_EQ(lookahead_event->details.at("stop_authority_signalled"), "true");
  EXPECT_EQ(lookahead_event->details.at("lookahead_checked_samples"), "8");
  EXPECT_FALSE(lookahead_event->details.at("lookahead_sample_times_ns").empty());
  EXPECT_TRUE(lookahead_event->details.contains("collision_result_first_failure_time_ns"));
  // The addressed-owner handoff fact precedes telemetry; the participant
  // latches nothing locally.
  const auto handoff_event =
      std::find_if(captured_events.begin(), captured_events.end(), [&](const KernelEvent& event) {
        const auto surface = event.details.find("surface");
        return event.type == KernelEventType::kCollisionDetected &&
               event.execution_id == result.value() && surface != event.details.end() &&
               surface->second == "addressed_runtime_collision_authority";
      });
  ASSERT_NE(handoff_event, captured_events.end());
  EXPECT_GT(handoff_event->sequence, 0u);
  EXPECT_LT(handoff_event->sequence, lookahead_event->sequence);
  EXPECT_EQ(handoff_event->details.at("in_collision"), "true");
  EXPECT_EQ(handoff_event->details.at("reason"), "runtime_lookahead_collision");
  EXPECT_EQ(handoff_event->details.at("lookahead_cursor_valid"), "true");
  EXPECT_EQ(handoff_event->details.at("lookahead_cursor_execution_generation"), "1");
  EXPECT_EQ(handoff_event->details.at("collision_world_revision_id"),
            lookahead_event->details.at("collision_world_revision_id"));
  EXPECT_EQ(handoff_event->details.at("lookahead_cycle_deadline_ns"),
            lookahead_event->details.at("lookahead_cycle_deadline_ns"));
  EXPECT_FALSE(handoff_event->details.contains("lookahead_sample_times_ns"));
  EXPECT_TRUE(std::none_of(captured_events.begin(), captured_events.end(), [](const auto& event) {
    return event.type == KernelEventType::kInterruptLatched;
  }));

  // Lookahead has atomically claimed this dispatch; a late natural-completion
  // callback must not publish a terminal progress marker, even though the
  // backend handle itself completes.
  const auto* progress = controlled_execution.execution_progress_publication();
  ASSERT_NE(progress, nullptr);
  controlled_execution.complete();
  const auto after_late_completion = progress->acquire();
  EXPECT_TRUE(after_late_completion.valid);
  EXPECT_FALSE(after_late_completion.terminal);

  for (int attempt = 0; attempt < 1000; ++attempt) {
    const auto metrics = controlled_kernel.get_metrics();
    if (metrics.runtime_collision_lookahead_jobs_collision > 0) {
      EXPECT_EQ(metrics.runtime_collision_monitor_period_ns, 10'000'000u);
      EXPECT_EQ(metrics.runtime_collision_immediate_query_budget_ns, 10'000'000u);
      EXPECT_EQ(metrics.runtime_collision_lookahead_cycle_budget_ns, 10'000'000u);
      EXPECT_EQ(metrics.runtime_collision_lookahead_query_budget_ns, 10'000'000u);
      EXPECT_EQ(metrics.runtime_collision_lookahead_execution_id, result.value());
      EXPECT_EQ(metrics.runtime_collision_lookahead_execution_generation, 1u);
      EXPECT_EQ(metrics.runtime_collision_lookahead_cursor_source_kind, "backend_commanded");
      EXPECT_EQ(metrics.runtime_collision_lookahead_cursor_authority_profile,
                "test_predicted_native");
      EXPECT_EQ(metrics.runtime_collision_lookahead_last_checked_samples, 8u);
      break;
    }
    ASSERT_LT(attempt, 999) << "lookahead metrics were not published";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_TRUE(controlled_kernel.wait_for_completion(2'000'000'000));
  EXPECT_EQ(ended_as(controlled_kernel), "completed");
  EXPECT_GE(local_runtime_engine->check_count.load(), 1);
  EXPECT_GE(local_lookahead_engine->check_count.load(), 1);
}

TEST_F(ExecutionKernelTest, CollisionLookahead_NaturalCompletionTerminalHandoffFencesRacingResult) {
  ControlledExecution controlled_execution;
  controlled_execution.enable_execution_progress_publication();
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_immediate_query_budget_ns = 10'000'000;
  config.runtime_collision_lookahead_cycle_budget_ns = 100'000'000;
  config.runtime_collision_lookahead_query_budget_ns = 75'000'000;
  config.runtime_collision_lookahead_horizon_ns = 2'000'000'000;
  config.runtime_collision_lookahead_max_samples = 8;
  config.runtime_collision_lookahead_workers = 1;
  config.runtime_collision_lookahead_workers_declared = true;
  MockCollisionWorldPublisher local_collision_world;
  auto local_validation_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  auto local_lookahead_engine = std::make_shared<MockCollisionEngine>();
  local_lookahead_engine->default_result = make_authoritative_collision_result();
  local_lookahead_engine->on_check = [&](const CollisionQuery&) {
    controlled_execution.complete();
  };
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_validation_engine, local_runtime_engine,
                                         {local_lookahead_engine});
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  const auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler));

  const auto permit = drive_until_lookahead_permit_admitted(
      controlled_kernel, local_clock, local_scheduler, config.collision_monitor_period_ns);
  ASSERT_TRUE(permit.admitted)
      << "periodic clear permits were never admitted; try-lock misses="
      << permit.metrics.runtime_collision_lookahead_clear_permit_try_lock_misses;
  ASSERT_TRUE(controlled_kernel.wait_for_completion(5'000'000'000));

  EXPECT_EQ(ended_as(controlled_kernel), "completed");
  EXPECT_GT(local_lookahead_engine->check_count.load(), 0);
  const auto* progress = controlled_execution.execution_progress_publication();
  ASSERT_NE(progress, nullptr);
  EXPECT_FALSE(progress->acquire().valid)
      << "kernel worker completion must retire the terminal generation";
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_FALSE(std::any_of(events.begin(), events.end(), [](const KernelEvent& event) {
      const auto surface = event.details.find("surface");
      return event.type == KernelEventType::kInterruptLatched ||
             (surface != event.details.end() && surface->second == "observed_runtime_lookahead" &&
              (event.type == KernelEventType::kCollisionDetected ||
               event.type == KernelEventType::kErrorRaised));
    }));
  }
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_ExpandsObservedStateBeforeEngineQuery) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 10'000'000;
  config.runtime_collision_immediate_query_budget_ns = 10'000'000;
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);

  context.current_state_value.joint_names = {"j1", "j2"};
  context.current_state_value.positions_rad = {0.0, 0.0};
  controlled_execution.snapshot.timestamp_ns = local_clock.now();

  TrajectoryDTO expanded;
  expanded.joint_names = {"j1", "j2", "gripper_master"};
  expanded.waypoints.push_back(TrajectoryWaypointDTO{0.0, {0.0, 0.0, 0.0081}, {}, {}});
  context.next_expand_result = Result<TrajectoryDTO>::Ok(expanded);

  std::vector<std::string> queried_joint_names;
  std::vector<double> queried_positions;
  local_runtime_engine->on_check = [&](const CollisionQuery& query) {
    queried_joint_names = query.robot_state.joint_names;
    queried_positions = query.robot_state.positions_rad;
  };

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  const int expand_count_after_dispatch = context.expand_count;
  auto live_snapshot = make_test_collision_world_snapshot();
  live_snapshot.revision = CollisionWorldRevision{100, test_digest("expanded-runtime-state")};
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(live_snapshot));

  ASSERT_TRUE(wait_for_pending_scheduler_work(local_scheduler))
      << "runtime collision monitor did not schedule its first tick";
  local_clock.advance(10'000'000);
  local_scheduler.advance_to(local_clock.now());
  for (int attempt = 0; attempt < 100 && local_runtime_engine->check_count.load() == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    local_scheduler.advance_to(local_clock.now());
  }

  controlled_execution.release();
  EXPECT_TRUE(controlled_kernel.wait_for_completion(1'000'000'000));

  EXPECT_GE(context.expand_count, 1);
  EXPECT_EQ(context.expand_count, expand_count_after_dispatch)
      << "runtime collision ticks must not call the observed-state provider";
  ASSERT_EQ(context.last_expand_trajectory.joint_names, (std::vector<std::string>{"j1", "j2"}));
  ASSERT_GE(context.last_expand_trajectory.waypoints.size(), 1u);
  EXPECT_EQ(context.last_expand_trajectory.waypoints.front().positions_rad,
            (std::vector<double>{0.0, 0.0}));
  EXPECT_EQ(queried_joint_names, (std::vector<std::string>{"j1", "j2", "gripper_master"}));
  ASSERT_EQ(queried_positions.size(), 3u);
  EXPECT_DOUBLE_EQ(queried_positions[0], 0.0);
  EXPECT_DOUBLE_EQ(queried_positions[1], 0.0);
  EXPECT_DOUBLE_EQ(queried_positions[2], 0.0081);
  EXPECT_EQ(ended_as(controlled_kernel), "completed");
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_UsesBackendObservedStateForRuntimeQuery) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  std::vector<double> queried_positions;
  local_runtime_engine->on_check = [&](const CollisionQuery& query) {
    queried_positions = query.robot_state.positions_rad;
  };

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  context.current_state_value.positions_rad = {99.0, 99.0};

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
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);

  TrajectoryDTO collision_trajectory;
  collision_trajectory.joint_names = {"j2", "j1"};
  collision_trajectory.waypoints.push_back(TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {}, {}});
  context.next_expand_result = Result<TrajectoryDTO>::Ok(std::move(collision_trajectory));

  std::vector<std::string> queried_joint_names;
  std::vector<double> queried_positions;
  local_runtime_engine->on_check = [&](const CollisionQuery& query) {
    queried_joint_names = query.robot_state.joint_names;
    queried_positions = query.robot_state.positions_rad;
  };

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
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
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
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
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  // The collision is a reported fact only: no local latch, no ending.
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");
  const auto latch = collisions.latest();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->at("reason"), "observed_state_incomplete");
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_StaleObservedStateFailsClosedBeforeEngineQuery) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.runtime_observed_state_max_age_ns = 10'000'000;  // 10 ms
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
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
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  // The collision is a reported fact only: no local latch, no ending.
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");
  const auto latch = collisions.latest();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->at("reason"), "stale_observed_robot_state");
  EXPECT_EQ(latch->at("observed_state_age_ns"), "20000000");
  EXPECT_EQ(latch->at("cause"), "stale_observed_robot_state");
  EXPECT_EQ(latch->at("monitor_reason"), "stale_observed_robot_state");
  EXPECT_EQ(latch->at("sample_age_ns"), "20000000");
  EXPECT_EQ(latch->at("in_collision"), "false");
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_SampleReceivedAfterTickBoundaryIsCurrent) {
  // The monitor takes its boundary before it acquires the sample, so a sample
  // the publisher stamps in between is newer than the boundary: it is current
  // (received_sample_age.hpp), not an invalid timestamp.
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.runtime_observed_state_max_age_ns = 10'000'000;  // 10 ms
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now() + 5'000'000);
  const int baseline_engine_checks = local_runtime_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(local_runtime_engine->check_count.load(), baseline_engine_checks + 1);
  const auto metrics = controlled_kernel.get_metrics();
  EXPECT_EQ(metrics.runtime_collision_ticks_completed, 1u);
  EXPECT_EQ(metrics.runtime_collision_ticks_faulted, 0u);
  EXPECT_FALSE(collisions.latest().has_value());

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_ReadsDedicatedPublicationWithoutLifecycleSnapshot) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.runtime_observed_state_max_age_ns = 10'000'000;  // 10 ms
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  bool lifecycle_snapshot_read = false;
  controlled_execution.on_get_snapshot = [&]() { lifecycle_snapshot_read = true; };
  const int baseline_engine_checks = local_runtime_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_FALSE(lifecycle_snapshot_read);
  EXPECT_GT(local_runtime_engine->check_count.load(), baseline_engine_checks);
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_RefreshesRuntimeSnapshotOnWorldRevisionChange) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);

  auto initial_snapshot = make_test_collision_world_snapshot();
  initial_snapshot.revision = CollisionWorldRevision{42, test_digest("dynamic-world-old")};
  initial_snapshot.bodies.push_back(make_dynamic_test_body());
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(initial_snapshot));

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  const int validation_bundle_calls_after_dispatch = local_collision_world.bundle_call_count.load();
  const int runtime_bundle_calls_before_tick =
      local_collision_world.published_runtime_bundle_call_count.load();

  local_clock.advance(20'000'000);
  const CollisionWorldRevision fresh_revision{43, test_digest("dynamic-world-fresh")};
  auto fresh_snapshot = make_test_collision_world_snapshot();
  fresh_snapshot.revision = fresh_revision;
  fresh_snapshot.bodies.push_back(make_dynamic_test_body());
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(fresh_snapshot));
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  const int baseline_engine_checks = local_runtime_engine->check_count.load();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(local_collision_world.bundle_call_count.load(), validation_bundle_calls_after_dispatch)
      << "runtime ticks must not borrow the validation snapshot bundle";
  EXPECT_GT(local_collision_world.published_runtime_bundle_call_count.load(),
            runtime_bundle_calls_before_tick);
  EXPECT_GT(local_runtime_engine->check_count.load(), baseline_engine_checks);
  expect_collision_world_revision(local_runtime_engine->last_collision_world_revision,
                                  fresh_revision);
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_ContactReleaseAllowsBoundedOngoingReleaseContact) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);

  const CollisionPairId pair{"scene/part/shape/0", "scene/support/shape/0"};
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

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = make_support_contact_collision_result(pair, -0.004);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  const int baseline_stop_count = controlled_execution.stop_count_value();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(controlled_execution.stop_count_value(), baseline_stop_count);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_ContactReleasePenetrationIncreaseFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  // Captured by the kernel's event callback, so this storage must outlive the
  // kernel.
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  const CollisionPairId pair{"scene/part/shape/0", "scene/support/shape/0"};
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
  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = make_support_contact_collision_result(pair, -0.004);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  ASSERT_EQ(controlled_execution.stop_count_value(), 0);

  local_runtime_engine->default_result = make_support_contact_collision_result(pair, -0.006);
  local_clock.advance(10'000'000);
  controlled_execution.set_progress(0.50, {0.50, 0.90}, local_clock.now());
  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  // The collision is a reported fact only: no local latch, no ending.
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");
  const auto latch = collisions.latest();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->at("reason"), "runtime_contact_release_penetration_increased");
  EXPECT_EQ(latch->at("contact_a"), pair.body_a);
  EXPECT_EQ(latch->at("contact_b"), pair.body_b);
  std::lock_guard<std::mutex> events_lock(events_mutex);
  const auto violation_event =
      std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
        return event.type == KernelEventType::kContactPolicyViolation;
      });
  ASSERT_NE(violation_event, events.end());
  EXPECT_EQ(violation_event->details.at("surface"), "observed_runtime_monitor");
  EXPECT_EQ(violation_event->details.at("policy_mode"), "contact_release");
  EXPECT_EQ(violation_event->details.at("release_evidence_mode"), "signed_penetration");
  EXPECT_EQ(violation_event->details.at("contact_body_a"), pair.body_a);
  EXPECT_EQ(violation_event->details.at("contact_body_b"), pair.body_b);
  EXPECT_EQ(violation_event->details.at("contact_present"), "true");
  EXPECT_EQ(violation_event->details.at("initial_contact_pair_count"), "1");
  EXPECT_EQ(violation_event->details.at("current_contact_pair_count"), "1");
  EXPECT_EQ(violation_event->details.at("distance_request_mode"), "selected_colliding_pairs");
  EXPECT_EQ(violation_event->details.at("evaluated_distance_pair_count"), "1");
  EXPECT_EQ(violation_event->details.at("response"), "preempt_trajectory");
  EXPECT_EQ(violation_event->details.at("classification"), "blocking_collision");
  EXPECT_DOUBLE_EQ(std::stod(violation_event->details.at("penetration_depth_m")), 0.006);
  EXPECT_EQ(violation_event->details.find("max_contact_penetration_m"),
            violation_event->details.end());
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_BoundedPenetrationEvaluatesLaterFirstContact) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  // Captured by the kernel's event callback, so this storage must outlive the
  // kernel.
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  const CollisionPairId pair{"scene/insert/shape/0", "scene/fixture/shape/0"};
  auto policy = make_test_collision_policy_snapshot();
  policy.policy_id = "bounded-runtime-contact";
  policy.require_full_diagnostics = true;
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
  entry.max_contact_penetration_m = 0.005;
  policy.support_contacts.push_back(entry);
  local_collision_world.current_policy = policy;
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  // Predispatch also starts clear and admits a later/final in-bound contact.
  local_collision_engine->result_by_waypoint[0] =
      make_support_contact_collision_result(pair, 0.002);
  local_collision_engine->result_by_waypoint[1] =
      make_support_contact_collision_result(pair, -0.004);
  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = make_support_contact_collision_result(pair, 0.002);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  ASSERT_EQ(controlled_execution.stop_count_value(), 0);

  local_runtime_engine->default_result = make_support_contact_collision_result(pair, -0.006);
  local_clock.advance(10'000'000);
  controlled_execution.set_progress(0.50, {0.50, 0.90}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);

  std::lock_guard<std::mutex> events_lock(events_mutex);
  const auto violation = std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
    return event.type == KernelEventType::kContactPolicyViolation &&
           event.details.at("policy_mode") == "bounded_penetration";
  });
  ASSERT_NE(violation, events.end());
  EXPECT_EQ(violation->details.at("contact_policy_id"), "bounded-runtime-contact");
  EXPECT_EQ(violation->details.at("initial_contact_pair_count"), "0");
  EXPECT_EQ(violation->details.at("current_contact_pair_count"), "1");
  EXPECT_EQ(violation->details.at("contact_present"), "true");
  EXPECT_EQ(violation->details.at("distance_request_mode"), "selected_colliding_pairs");
  EXPECT_EQ(violation->details.at("evaluated_distance_pair_count"), "1");
  EXPECT_DOUBLE_EQ(std::stod(violation->details.at("penetration_depth_m")), 0.006);
  EXPECT_DOUBLE_EQ(std::stod(violation->details.at("max_contact_penetration_m")), 0.005);
  EXPECT_EQ(violation->details.at("within_penetration_bound"), "false");
  EXPECT_EQ(violation->details.find("release_evidence_mode"), violation->details.end());
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_BoundedPenetrationMissingClearanceDoesNotClaimOverCap) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  const CollisionPairId pair{"scene/insert/shape/0", "scene/fixture/shape/0"};
  auto policy = make_test_collision_policy_snapshot();
  policy.policy_id = "bounded-runtime-missing-clearance";
  policy.require_full_diagnostics = true;
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
  entry.max_contact_penetration_m = 0.005;
  policy.support_contacts.push_back(entry);
  local_collision_world.current_policy = policy;
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  local_collision_engine->result_by_waypoint[0] =
      make_support_contact_collision_result(pair, 0.002);
  local_collision_engine->result_by_waypoint[1] =
      make_support_contact_collision_result(pair, 0.002);
  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  CollisionResult missing_clearance;
  missing_clearance.in_collision = true;
  missing_clearance.normalized_pairs = {pair};
  missing_clearance.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
  missing_clearance.decision = CollisionDecisionResult::kInconclusiveFail;
  local_runtime_engine->runtime_failure_reason =
      RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete;
  local_runtime_engine->default_result = std::move(missing_clearance);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);

  std::lock_guard<std::mutex> events_lock(events_mutex);
  const auto violation = std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
    const auto reason = event.details.find("violation_reason");
    return event.type == KernelEventType::kContactPolicyViolation &&
           reason != event.details.end() && reason->second == "missing_contact_release_clearance";
  });
  ASSERT_NE(violation, events.end());
  ASSERT_NE(violation->details.find("policy_mode"), violation->details.end());
  EXPECT_EQ(violation->details.at("policy_mode"), "bounded_penetration");
  EXPECT_EQ(violation->details.at("violation_reason"), "missing_contact_release_clearance");
  ASSERT_NE(violation->details.find("contact_policy_id"), violation->details.end());
  EXPECT_EQ(violation->details.at("contact_policy_id"), "bounded-runtime-missing-clearance");
  EXPECT_EQ(violation->details.at("zone_id"),
            "support_pair:scene/fixture/shape/0<->scene/insert/shape/0");
  EXPECT_EQ(violation->details.at("contact_body_a"), "scene/fixture/shape/0");
  EXPECT_EQ(violation->details.at("contact_body_b"), "scene/insert/shape/0");
  EXPECT_EQ(violation->details.at("contact_present"), "true");
  EXPECT_EQ(violation->details.at("distance_request_mode"), "selected_colliding_pairs");
  EXPECT_EQ(violation->details.at("evaluated_distance_pair_count"), "0");
  EXPECT_EQ(violation->details.at("classification"), "diagnostic_incomplete");
  ASSERT_NE(violation->details.find("max_contact_penetration_m"), violation->details.end());
  EXPECT_DOUBLE_EQ(std::stod(violation->details.at("max_contact_penetration_m")), 0.005);
  EXPECT_EQ(violation->details.find("within_penetration_bound"), violation->details.end());
  EXPECT_EQ(violation->details.find("penetration_depth_m"), violation->details.end());
  EXPECT_EQ(violation->details.find("current_penetration_m"), violation->details.end());
}

TEST_F(ExecutionKernelTest, Execute_ResolvedContactGraspInstallsExactEphemeralPolicy) {
  // SetUp installs the authoritative collision resources for the fixture's
  // Kernel. A second install is prohibited without an Apply fence.
  install_mock_contact_grasp_world(collision_world_publisher, "graspable",
                                   {"right_finger", "left_finger"});
  install_mock_detached_logical_object(collision_world_publisher, "graspable");

  const CollisionBodyId allowed_body("component/arm/model/arm_model/body/right_finger/collision/0");
  auto request = make_test_request("resolved_contact_grasp");
  request.contact_policy_id = "pick_grasp";
  request.runtime_object_bindings = {RuntimeObjectReferenceBindingDTO{
      RuntimeObjectId("selected_object"), SceneResourceId("graspable")}};
  request.resolved_contact_grasp =
      ResolvedContactGraspBindingDTO{ContactPolicyId("pick_grasp"),
                                     ContactPolicyRevision(kTestDigestA),
                                     RuntimeObjectId("selected_object"),
                                     SceneResourceId("graspable"),
                                     {allowed_body},
                                     ContactGraspEnvelopeDTO{0.002, 0.75}};

  const auto result = execute_sync(request);
  ASSERT_TRUE(result.ok()) << result.error().code << ": " << result.error().message;
  EXPECT_EQ(ended_as(kernel), "completed");
  ASSERT_TRUE(planner.last_request.contact_policy_id.has_value());
  EXPECT_EQ(planner.last_request.contact_policy_id->rfind("contact_grasp.bound.", 0), 0U);
  ASSERT_TRUE(planner.last_request.retained_collision_policy);
  const auto& effective_policy = *planner.last_request.retained_collision_policy;
  ASSERT_EQ(effective_policy.contact_grasps.size(), 1U);
  const auto& grasp = effective_policy.contact_grasps.front();
  EXPECT_EQ(grasp.policy_id, *planner.last_request.contact_policy_id);
  EXPECT_EQ(grasp.selected_object_id, "graspable");
  EXPECT_EQ(grasp.allowed_robot_links, std::vector<std::string>{allowed_body});
  EXPECT_DOUBLE_EQ(grasp.max_penetration_m, 0.002);
  EXPECT_DOUBLE_EQ(grasp.terminal_window_start_fraction, 0.75);

  ASSERT_TRUE(collision_world_publisher.current_policy.has_value());
  EXPECT_TRUE(collision_world_publisher.current_policy->contact_grasps.empty())
      << "operation-local binding must not mutate configured policy";
  ASSERT_TRUE(collision_world_publisher.current_logical_scene);
  ASSERT_EQ(collision_world_publisher.current_logical_scene->objects.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<CellFixedSceneObjectPlacementDTO>(
      collision_world_publisher.current_logical_scene->objects.front().placement));
}

TEST_F(ExecutionKernelTest, Execute_ResolvedContactReleaseInstallsOnlyExactDeparturePairs) {
  // SetUp installs the authoritative collision resources for the fixture's
  // Kernel. A second install is prohibited without an Apply fence.
  install_mock_contact_grasp_world(collision_world_publisher, "graspable",
                                   {"right_finger", "left_finger"});
  install_mock_detached_logical_object(collision_world_publisher, "graspable");

  const CollisionBodyId allowed_body("component/arm/model/arm_model/body/right_finger/collision/0");
  auto request = make_test_request("resolved_contact_grasp");
  request.contact_policy_id = "pick_grasp";
  request.runtime_object_bindings = {RuntimeObjectReferenceBindingDTO{
      RuntimeObjectId("selected_object"), SceneResourceId("graspable")}};
  request.resolved_contact_grasp =
      ResolvedContactGraspBindingDTO{ContactPolicyId("pick_grasp"),
                                     ContactPolicyRevision(kTestDigestA),
                                     RuntimeObjectId("selected_object"),
                                     SceneResourceId("graspable"),
                                     {allowed_body},
                                     ContactReleaseEnvelopeDTO{ContactReleaseEvidenceModeDTO::kSignedPenetration, true, 0.002}};

  const auto result = execute_sync(request);
  ASSERT_TRUE(result.ok()) << result.error().code << ": " << result.error().message;
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_FALSE(planner.last_request.contact_policy_id.has_value());
  ASSERT_TRUE(planner.last_request.retained_collision_policy);
  const auto& effective_policy = *planner.last_request.retained_collision_policy;
  EXPECT_TRUE(effective_policy.contact_grasps.empty());
  ASSERT_EQ(effective_policy.support_contacts.size(), 1U);
  const auto& release = effective_policy.support_contacts.front();
  EXPECT_EQ(release.mode, CollisionContactPolicyMode::kContactRelease);
  EXPECT_EQ(release.pair, (CollisionPairId{allowed_body, "scene/graspable/shape/0"}));
  EXPECT_DOUBLE_EQ(release.max_start_penetration_m, 0.002);
  EXPECT_TRUE(release.require_strictly_decreasing_penetration);
  EXPECT_TRUE(release.require_separation_by_end);
  EXPECT_FALSE(release.allow_recontact);

  ASSERT_TRUE(collision_world_publisher.current_policy.has_value());
  EXPECT_TRUE(collision_world_publisher.current_policy->support_contacts.empty())
      << "operation-local binding must not mutate configured policy";
  ASSERT_TRUE(collision_world_publisher.current_logical_scene);
  ASSERT_EQ(collision_world_publisher.current_logical_scene->objects.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<CellFixedSceneObjectPlacementDTO>(
      collision_world_publisher.current_logical_scene->objects.front().placement));
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_ContactGraspAllowsBoundedRuntimeContact) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId pair{"component/arm/model/arm_model/body/right_finger/collision/0",
                             "scene/graspable/shape/0"};
  auto policy = make_contact_grasp_policy_snapshot(kPolicyId, "graspable",
                                                   {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  install_mock_contact_grasp_world(local_collision_world, "graspable",
                                   {"right_finger", "left_finger"});
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_allowed");
  bind_resolved_contact_grasp(request, local_collision_world, kPolicyId, "graspable",
                              {"right_finger", "left_finger"}, 0.002);
  auto result = prepare_and_dispatch(controlled_kernel, request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = make_contact_grasp_collision_result(pair, -0.001);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  const int baseline_stop_count = controlled_execution.stop_count_value();

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  EXPECT_EQ(controlled_execution.stop_count_value(), baseline_stop_count);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_ContactGraspPenetrationExceededFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId pair{"component/arm/model/arm_model/body/right_finger/collision/0",
                             "scene/graspable/shape/0"};
  auto policy = make_contact_grasp_policy_snapshot(kPolicyId, "graspable",
                                                   {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  install_mock_contact_grasp_world(local_collision_world, "graspable",
                                   {"right_finger", "left_finger"});
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_over_limit");
  bind_resolved_contact_grasp(request, local_collision_world, kPolicyId, "graspable",
                              {"right_finger", "left_finger"}, 0.002);
  auto result = prepare_and_dispatch(controlled_kernel, request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = make_contact_grasp_collision_result(pair, -0.006);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  // The collision is a reported fact only: no local latch, no ending.
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");
  const auto latch = collisions.latest();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->at("reason"), "runtime_collision_detected");
  EXPECT_EQ(latch->at("collision_result_raw_contact_grasp_violation"),
            "grasp_contact_penetration_exceeded");
  EXPECT_TRUE(
      is_bound_contact_grasp_policy(latch->at("collision_result_raw_contact_grasp_policy_id")));

  std::lock_guard<std::mutex> events_lock(events_mutex);
  const auto violation = std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
    return event.type == KernelEventType::kContactPolicyViolation &&
           event.details.at("violation_reason") == "grasp_contact_penetration_exceeded";
  });
  ASSERT_NE(violation, events.end());
  EXPECT_TRUE(is_bound_contact_grasp_policy(violation->details.at("contact_policy_id")));
  EXPECT_EQ(violation->details.at("policy_mode"), "contact_grasp");
  EXPECT_EQ(violation->details.at("zone_id"),
            "contact_grasp_pair:component/arm/model/arm_model/body/"
            "right_finger/collision/0<->scene/graspable/shape/0");
  EXPECT_EQ(violation->details.at("contact_body_a"), pair.body_a);
  EXPECT_EQ(violation->details.at("contact_body_b"), pair.body_b);
  EXPECT_EQ(violation->details.at("contact_present"), "true");
  EXPECT_EQ(violation->details.at("distance_request_mode"), "selected_colliding_pairs");
  EXPECT_EQ(violation->details.at("evaluated_distance_pair_count"), "1");
  EXPECT_DOUBLE_EQ(std::stod(violation->details.at("max_penetration_m")), 0.002);
  EXPECT_DOUBLE_EQ(std::stod(violation->details.at("penetration_depth_m")), 0.006);
  EXPECT_EQ(violation->details.find("initial_contact_pair_count"), violation->details.end());
  EXPECT_EQ(violation->details.find("current_contact_pair_count"), violation->details.end());
  EXPECT_EQ(violation->details.find("within_penetration_bound"), violation->details.end());
  EXPECT_EQ(violation->details.at("response"), "preempt_trajectory");
  EXPECT_EQ(violation->details.at("classification"), "blocking_collision");
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
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId violating_pair{
      "component/arm/model/arm_model/body/right_finger/collision/0", "scene/graspable/shape/0"};
  const CollisionPairId allowed_pair{"component/arm/model/arm_model/body/left_finger/collision/0",
                                     "scene/graspable/shape/0"};
  auto policy = make_contact_grasp_policy_snapshot(kPolicyId, "graspable",
                                                   {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  install_mock_contact_grasp_world(local_collision_world, "graspable",
                                   {"right_finger", "left_finger"});
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_mixed_pairs");
  bind_resolved_contact_grasp(request, local_collision_world, kPolicyId, "graspable",
                              {"right_finger", "left_finger"}, 0.002);
  auto result = prepare_and_dispatch(controlled_kernel, request);
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
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  // The collision is a reported fact only: no local latch, no ending.
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");
  const auto latch = collisions.latest();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->at("collision_result_raw_contact_grasp_violation"),
            "grasp_contact_penetration_exceeded");
  EXPECT_EQ(latch->at("collision_result_first_failure_pair"),
            "component/arm/model/arm_model/body/right_finger/collision/0 <-> "
            "scene/graspable/shape/0");
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_ContactGraspMissingClearanceFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId pair{"component/arm/model/arm_model/body/right_finger/collision/0",
                             "scene/graspable/shape/0"};
  auto policy = make_contact_grasp_policy_snapshot(kPolicyId, "graspable",
                                                   {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  install_mock_contact_grasp_world(local_collision_world, "graspable",
                                   {"right_finger", "left_finger"});
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_missing_clearance");
  bind_resolved_contact_grasp(request, local_collision_world, kPolicyId, "graspable",
                              {"right_finger", "left_finger"}, 0.002);
  auto result = prepare_and_dispatch(controlled_kernel, request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  auto runtime_result = make_contact_grasp_collision_result(pair, -0.001);
  runtime_result.pair_clearances.clear();
  runtime_result.min_clearance_m.reset();
  runtime_result.min_clearance_pair.reset();
  runtime_result.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
  runtime_result.decision = CollisionDecisionResult::kInconclusiveFail;
  local_runtime_engine->runtime_failure_reason =
      RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete;
  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = runtime_result;
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  // The collision is a reported fact only: no local latch, no ending.
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");
  const auto latch = collisions.latest();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->at("collision_result_raw_contact_grasp_violation"),
            "missing_grasp_contact_clearance");

  std::lock_guard<std::mutex> events_lock(events_mutex);
  const auto violation = std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
    return event.type == KernelEventType::kContactPolicyViolation &&
           event.details.at("violation_reason") == "missing_grasp_contact_clearance";
  });
  ASSERT_NE(violation, events.end());
  EXPECT_TRUE(is_bound_contact_grasp_policy(violation->details.at("contact_policy_id")));
  EXPECT_EQ(violation->details.at("policy_mode"), "contact_grasp");
  EXPECT_EQ(violation->details.at("zone_id"),
            "contact_grasp_pair:component/arm/model/arm_model/body/"
            "right_finger/collision/0<->scene/graspable/shape/0");
  EXPECT_EQ(violation->details.at("contact_body_a"), pair.body_a);
  EXPECT_EQ(violation->details.at("contact_body_b"), pair.body_b);
  EXPECT_EQ(violation->details.at("contact_present"), "true");
  EXPECT_EQ(violation->details.at("distance_request_mode"), "selected_colliding_pairs");
  EXPECT_EQ(violation->details.at("evaluated_distance_pair_count"), "0");
  EXPECT_DOUBLE_EQ(std::stod(violation->details.at("max_penetration_m")), 0.002);
  EXPECT_EQ(violation->details.at("response"), "preempt_trajectory");
  EXPECT_EQ(violation->details.at("classification"), "diagnostic_incomplete");
  EXPECT_EQ(violation->details.find("penetration_depth_m"), violation->details.end());
  EXPECT_EQ(violation->details.find("initial_contact_pair_count"), violation->details.end());
  EXPECT_EQ(violation->details.find("current_contact_pair_count"), violation->details.end());
  EXPECT_EQ(violation->details.find("within_penetration_bound"), violation->details.end());
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_ContactGraspUnexpectedPairFailsClosed) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);
  controlled_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId unexpected_pair{
      "component/arm/model/arm_model/body/right_finger/collision/0", "scene/other_part/shape/0"};
  auto policy = make_contact_grasp_policy_snapshot(kPolicyId, "graspable",
                                                   {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  install_mock_contact_grasp_world(local_collision_world, "graspable",
                                   {"right_finger", "left_finger"});
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_unexpected_pair");
  bind_resolved_contact_grasp(request, local_collision_world, kPolicyId, "graspable",
                              {"right_finger", "left_finger"}, 0.002);
  auto result = prepare_and_dispatch(controlled_kernel, request);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  auto runtime_result = make_contact_grasp_collision_result(unexpected_pair, -0.001);
  runtime_result.pair_clearances.clear();
  runtime_result.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
  runtime_result.decision = CollisionDecisionResult::kInconclusiveFail;
  local_runtime_engine->runtime_failure_reason =
      RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete;
  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = std::move(runtime_result);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());

  auto tick = controlled_kernel.collision_check_tick();
  ASSERT_TRUE(tick.ok());
  local_scheduler.advance_to(local_clock.now());
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  // The collision is a reported fact only: no local latch, no ending.
  EXPECT_EQ(ended_as(controlled_kernel), "working:Executing");
  const auto latch = collisions.latest();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->at("reason"), "runtime_collision_monitor_inconclusive");
  EXPECT_EQ(latch->find("collision_result_raw_contact_grasp_violation"), latch->end());
  EXPECT_EQ(latch->find("collision_result_first_failure_pair"), latch->end());

  std::lock_guard<std::mutex> events_lock(events_mutex);
  EXPECT_EQ(std::find_if(events.begin(), events.end(),
                         [](const auto& event) {
                           return event.type == KernelEventType::kContactPolicyViolation;
                         }),
            events.end());
}

// F-3: a move keeps its admitted operation-local contact policy for the whole
// move; the published policy never carries it.
TEST_F(ExecutionKernelTest, CollisionMonitoring_ContactGraspHeldAcrossTicks) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId pair{"component/arm/model/arm_model/body/right_finger/collision/0",
                             "scene/graspable/shape/0"};
  auto policy = make_contact_grasp_policy_snapshot(kPolicyId, "graspable",
                                                   {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  install_mock_contact_grasp_world(local_collision_world, "graspable",
                                   {"right_finger", "left_finger"});
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_held");
  bind_resolved_contact_grasp(request, local_collision_world, kPolicyId, "graspable",
                              {"right_finger", "left_finger"}, 0.002);
  ASSERT_TRUE(prepare_and_dispatch(controlled_kernel, request).ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(local_collision_world.current_policy.has_value());
  ASSERT_FALSE(local_collision_world.current_policy->contact_grasps.empty());
  EXPECT_FALSE(is_bound_contact_grasp_policy(
      local_collision_world.current_policy->contact_grasps.front().policy_id))
      << "the published policy never carries the operation-local grasp";

  local_runtime_engine->result_by_waypoint.clear();
  const int baseline_checks = local_runtime_engine->check_count.load();
  for (const double clearance : {-0.0005, -0.001, -0.0015}) {
    local_clock.advance(10'000'000);
    local_runtime_engine->default_result = make_contact_grasp_collision_result(pair, clearance);
    controlled_execution.set_progress(0.5, {0.5, 0.5}, local_clock.now());
    ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
    EXPECT_FALSE(collisions.latest().has_value())
        << collisions.latest()->at("reason");
  }
  EXPECT_EQ(local_runtime_engine->check_count.load(), baseline_checks + 3);
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_ContactReleaseKeepsItsSupportContacts) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);

  const CollisionPairId pair{"component/arm/model/arm_model/body/right_finger/collision/0",
                             "scene/graspable/shape/0"};
  auto policy = make_test_collision_policy_snapshot();
  policy.require_full_diagnostics = true;
  local_collision_world.current_policy = policy;
  install_mock_contact_grasp_world(local_collision_world, "graspable", {"right_finger"});
  controlled_kernel.set_collision_validation_policy_snapshot(policy);
  local_collision_engine->result_by_waypoint[0] =
      make_support_contact_collision_result(pair, -0.001);
  local_collision_engine->result_by_waypoint[1] =
      make_support_contact_collision_result(pair, 0.002);

  // A Program release (end_rest): it starts 1 mm into the object it releases.
  auto request = make_test_request("runtime_contact_release_withdraw");
  bind_resolved_contact_grasp(request, local_collision_world, "pick_release", "graspable",
                              {"right_finger"}, 0.002);
  request.resolved_contact_grasp->envelope = ContactReleaseEnvelopeDTO{
      ContactReleaseEvidenceModeDTO::kSignedPenetration, true, 0.002};
  ASSERT_TRUE(prepare_and_dispatch(controlled_kernel, request).ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  ASSERT_TRUE(local_collision_world.current_policy.has_value());
  EXPECT_TRUE(local_collision_world.current_policy->support_contacts.empty());

  local_runtime_engine->result_by_waypoint.clear();
  for (const double clearance : {-0.001, -0.0005, 0.003}) {
    local_clock.advance(10'000'000);
    local_runtime_engine->default_result = make_support_contact_collision_result(pair, clearance);
    controlled_execution.set_progress(0.5, {0.5, 0.5}, local_clock.now());
    ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
    EXPECT_FALSE(collisions.latest().has_value())
        << collisions.latest()->at("reason");
  }
  EXPECT_EQ(controlled_execution.stop_count_value(), 0);
  EXPECT_EQ(controlled_kernel.get_status().state, ExecutionState::kExecuting);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest,
       CollisionMonitoring_ContactGraspFollowsNewWorldWithTheAdmittedPolicy) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);

  constexpr const char* kPolicyId = "contact_grasp/graspable";
  const CollisionPairId pair{"component/arm/model/arm_model/body/right_finger/collision/0",
                             "scene/graspable/shape/0"};
  auto policy = make_contact_grasp_policy_snapshot(kPolicyId, "graspable",
                                                   {"right_finger", "left_finger"}, 0.002);
  local_collision_world.current_policy = policy;
  install_mock_contact_grasp_world(local_collision_world, "graspable",
                                   {"right_finger", "left_finger"});
  controlled_kernel.set_collision_validation_policy_snapshot(policy);

  auto request = make_test_request("runtime_contact_grasp_new_world");
  bind_resolved_contact_grasp(request, local_collision_world, kPolicyId, "graspable",
                              {"right_finger", "left_finger"}, 0.002);
  ASSERT_TRUE(prepare_and_dispatch(controlled_kernel, request).ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));

  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = make_contact_grasp_collision_result(pair, -0.001);
  local_clock.advance(10'000'000);
  controlled_execution.set_progress(0.5, {0.5, 0.5}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  ASSERT_FALSE(collisions.latest().has_value()) << collisions.latest()->at("reason");

  // Mid-move a new Scene arrives: a new world revision with an extra body and
  // a new published policy revision without the authored grasp.
  auto fresh_world = *local_collision_world.current_snapshot;
  const CollisionWorldRevision fresh_revision{fresh_world.revision.revision_id + 1,
                                              test_digest("contact-grasp-world-fresh")};
  fresh_world.revision = fresh_revision;
  fresh_world.bodies.push_back(make_dynamic_test_body());
  std::sort(fresh_world.bodies.begin(), fresh_world.bodies.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.body_id < rhs.body_id; });
  fresh_world.body_count = static_cast<std::int64_t>(fresh_world.bodies.size());
  fresh_world.geometry_count = fresh_world.body_count;
  local_collision_world.current_snapshot =
      std::make_shared<const CollisionWorldSnapshot>(std::move(fresh_world));
  auto fresh_policy = make_test_collision_policy_snapshot();
  fresh_policy.revision.revision_id = 2;
  fresh_policy.revision.content_hash = test_digest("contact-grasp-policy-fresh");
  local_collision_world.current_policy = fresh_policy;

  for (const double clearance : {-0.001, -0.0015}) {
    local_clock.advance(10'000'000);
    local_runtime_engine->default_result = make_contact_grasp_collision_result(pair, clearance);
    controlled_execution.set_progress(0.5, {0.5, 0.5}, local_clock.now());
    ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
    EXPECT_FALSE(collisions.latest().has_value()) << collisions.latest()->at("reason");
    expect_collision_world_revision(local_runtime_engine->last_collision_world_revision,
                                    fresh_revision);
  }
  // The admitted grasp still bounds penetration on the new world.
  local_clock.advance(10'000'000);
  local_runtime_engine->default_result = make_contact_grasp_collision_result(pair, -0.006);
  controlled_execution.set_progress(0.5, {0.5, 0.5}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  const auto latch = collisions.latest();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->at("collision_result_raw_contact_grasp_violation"),
            "grasp_contact_penetration_exceeded");
  EXPECT_TRUE(
      is_bound_contact_grasp_policy(latch->at("collision_result_raw_contact_grasp_policy_id")));
  EXPECT_EQ(latch->at("collision_world_revision_id"),
            std::to_string(fresh_revision.revision_id));

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_NonContactMoveFollowsThePublishedPolicy) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);
  CollisionDetailsRecorder collisions(controlled_kernel);

  const CollisionPairId pair{"scene/part/shape/0", "scene/support/shape/0"};
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

  ASSERT_TRUE(prepare_and_dispatch(controlled_kernel, make_test_request()).ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(controlled_kernel, ExecutionState::kExecuting));
  local_runtime_engine->result_by_waypoint.clear();
  local_runtime_engine->default_result = make_support_contact_collision_result(pair, -0.004);
  controlled_execution.set_progress(0.25, {0.25, 0.75}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  ASSERT_FALSE(collisions.latest().has_value()) << collisions.latest()->at("reason");

  // A new published policy revision drops the support contact: a move without
  // an operation-local policy follows it, so the same contact now blocks.
  auto fresh_policy = make_test_collision_policy_snapshot();
  fresh_policy.require_full_diagnostics = true;
  fresh_policy.revision.revision_id = 2;
  fresh_policy.revision.content_hash = test_digest("published-policy-fresh");
  local_collision_world.current_policy = fresh_policy;
  local_runtime_engine->default_result = make_support_contact_collision_result(pair, -0.003);
  local_clock.advance(10'000'000);
  controlled_execution.set_progress(0.50, {0.50, 0.90}, local_clock.now());
  ASSERT_TRUE(controlled_kernel.collision_check_tick().ok());
  const auto latch = collisions.latest();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->at("published_collision_policy_revision_id"), "2");
  EXPECT_EQ(latch->at("contact_a"), pair.body_a);
  EXPECT_EQ(latch->at("contact_b"), pair.body_b);

  controlled_execution.complete();
  controlled_kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, CollisionMonitoring_UsesConfiguredPeriod) {
  ControlledExecution controlled_execution;
  ThreadSafeFakeClock local_clock;
  ThreadSafeManualScheduler local_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernelConfig config;
  config.collision_monitor_period_ns = 50'000'000;                  // 50 ms
  config.runtime_collision_immediate_query_budget_ns = 25'000'000;  // 25 ms
  MockCollisionWorldPublisher local_collision_world;
  auto local_collision_engine = std::make_shared<MockCollisionEngine>();
  auto local_runtime_engine = std::make_shared<MockCollisionEngine>();
  std::optional<std::int64_t> observed_timeout_budget_ns;
  local_runtime_engine->on_check = [&](const CollisionQuery& query) {
    observed_timeout_budget_ns = query.timeout_budget_ns;
  };
  ExecutionKernel controlled_kernel{
      planner, context, controlled_execution, local_clock, local_scheduler, local_cache, config};
  configure_runtime_collision_monitoring(controlled_kernel, local_collision_world,
                                         local_collision_engine, local_runtime_engine);

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
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
  EXPECT_EQ(ended_as(controlled_kernel), "completed");
}

// --- Full cycle test ---

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

  auto result = prepare_and_dispatch(kernel, make_test_request("cache_miss_move"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().execution_id, result.value());
  kernel.wait_for_completion();

  std::lock_guard<std::mutex> lock(events_mutex);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events[0].type, KernelEventType::kStateTransition);
  EXPECT_EQ(events[0].from_state, ExecutionState::kIdle);
  EXPECT_EQ(events[0].to_state, ExecutionState::kPlanning);
  const bool planned = std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
    return e.type == KernelEventType::kPlanStarted;
  });
  const bool cache_hit = std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
    return e.type == KernelEventType::kCacheHit;
  });
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

  auto result = prepare_and_dispatch(kernel, make_test_request("cached_move"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(kernel.get_status().execution_id, result.value());
  kernel.wait_for_completion();

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events[0].type, KernelEventType::kStateTransition);
    EXPECT_EQ(events[0].from_state, ExecutionState::kIdle);
    EXPECT_EQ(events[0].to_state, ExecutionState::kPlanning);
    const bool cache_hit = std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
      return e.type == KernelEventType::kCacheHit;
    });
    const bool planned = std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
      return e.type == KernelEventType::kPlanStarted;
    });
    const bool reached_validating =
        std::any_of(events.begin(), events.end(), [](const KernelEvent& e) {
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
  // The participant's final snapshot keeps the phase it ended in.
  EXPECT_EQ(snapshot.state, ExecutionState::kExecuting);
  EXPECT_EQ(ended_as(kernel), "completed");
  EXPECT_DOUBLE_EQ(snapshot.progress, 1.0);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 2u);
  EXPECT_EQ(snapshot.joint_positions_rad[0], 1.0);
  EXPECT_EQ(snapshot.joint_positions_rad[1], 1.0);
  EXPECT_GT(snapshot.sequence_number, 0u);
}

TEST_F(ExecutionKernelTest, GetExecutionSnapshot_WhileIdle_ServesLiveBackendSnapshot) {
  ASSERT_EQ(kernel.get_status().state, ExecutionState::kIdle);
  execution.snapshot.state = ExecutionState::kExecuting;
  execution.snapshot.execution_id = 99u;
  execution.snapshot.move_id = "backend_identity_is_not_authoritative";
  execution.snapshot.sequence_number = 41u;
  execution.snapshot.joint_positions_rad = {0.3, 0.7};
  execution.snapshot.state_source = "observed_ros2";

  const auto snapshot = kernel.get_execution_snapshot();

  EXPECT_EQ(snapshot.state, ExecutionState::kIdle);
  EXPECT_EQ(snapshot.execution_id, 0u);
  EXPECT_TRUE(snapshot.move_id.empty());
  EXPECT_EQ(snapshot.sequence_number, 41u);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 2u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad[0], 0.3);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad[1], 0.7);
  EXPECT_EQ(snapshot.state_source, "observed_ros2");
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
  ExecutionKernel controlled_kernel{planner,     context,         controlled_execution,
                                    local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(controlled_kernel, local_collision_world,
                                               local_collision_engine);

  auto result = prepare_and_dispatch(controlled_kernel, make_test_request("live_snapshot_move"));
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
  EXPECT_EQ(ended_as(controlled_kernel), "completed");
}

// ============================================================================
// Async behavior tests
// ============================================================================

TEST_F(ExecutionKernelTest, Execute_ReturnsImmediately) {
  auto result = prepare_and_dispatch(kernel, make_test_request());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 1u);
  // Don't check final state — it may or may not be done yet
  kernel.wait_for_completion();
}

TEST_F(ExecutionKernelTest, WaitForCompletion_BlocksUntilDone) {
  ASSERT_TRUE(prepare_and_dispatch(kernel, make_test_request()).ok());
  bool done = kernel.wait_for_completion();
  EXPECT_TRUE(done);
  EXPECT_EQ(ended_as(kernel), "completed");
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
  //   validation_passed -> participant_prepared -> state_transition ->
  //   execute_started -> execute_completed -> participant_settled (fact)
  ASSERT_GE(events.size(), 12u);
  EXPECT_EQ(events[0], KernelEventType::kStateTransition);
  EXPECT_EQ(events[1], KernelEventType::kCacheMiss);
  EXPECT_EQ(events[2], KernelEventType::kPlanStarted);
  EXPECT_EQ(events[3], KernelEventType::kPlanCompleted);
  EXPECT_EQ(events[4], KernelEventType::kStateTransition);
  EXPECT_EQ(events[5], KernelEventType::kValidationStarted);
  EXPECT_EQ(events[6], KernelEventType::kValidationPassed);
  EXPECT_EQ(events[7], KernelEventType::kParticipantPrepared);
  EXPECT_EQ(events[8], KernelEventType::kStateTransition);
  EXPECT_EQ(events[9], KernelEventType::kExecuteStarted);
  EXPECT_EQ(events[10], KernelEventType::kExecuteCompleted);
  EXPECT_EQ(events[11], KernelEventType::kParticipantSettled);
}

TEST_F(ExecutionKernelTest, AcknowledgeCompleted_CannotOvertakeTerminalPublication) {
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool execute_completed_callback_entered = false;
  bool release_execute_completed_callback = false;

  kernel.subscribe_events([&](const KernelEvent& event) {
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      events.push_back(event);
    }
    if (event.type != KernelEventType::kExecuteCompleted) {
      return;
    }
    std::unique_lock<std::mutex> lock(gate_mutex);
    execute_completed_callback_entered = true;
    gate_cv.notify_all();
    gate_cv.wait(lock, [&] { return release_execute_completed_callback; });
  });

  const auto execution =
      prepare_and_dispatch(kernel, make_test_request("terminal_publication_gate"));
  ASSERT_TRUE(execution.ok());

  bool callback_entered = false;
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    callback_entered = gate_cv.wait_for(lock, std::chrono::seconds(2),
                                        [&] { return execute_completed_callback_entered; });
  }

  std::optional<AcknowledgeCompletedResultDTO> early_ack;
  if (callback_entered) {
    EXPECT_EQ(kernel.get_status().state, ExecutionState::kExecuting);
    EXPECT_EQ(kernel.get_execution_snapshot().state, ExecutionState::kExecuting);
    early_ack = kernel.acknowledge_completed(execution.value());
  }
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release_execute_completed_callback = true;
  }
  gate_cv.notify_all();

  ASSERT_TRUE(callback_entered);
  ASSERT_TRUE(early_ack.has_value());
  EXPECT_FALSE(early_ack->accepted);
  ASSERT_TRUE(early_ack->error.has_value());
  EXPECT_EQ(early_ack->error->details.at("reason_code"),
            reason_codes::kTerminalPublicationInProgress);
  EXPECT_EQ(early_ack->error->details.at("retryable"), "true");

  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));
  const auto final_ack = kernel.acknowledge_completed(execution.value());
  ASSERT_TRUE(final_ack.accepted) << (final_ack.error.has_value() ? final_ack.error->message
                                                                  : std::string{});

  std::lock_guard<std::mutex> lock(events_mutex);
  const auto execute_completed = std::find_if(
      events.begin(), events.end(),
      [](const KernelEvent& event) { return event.type == KernelEventType::kExecuteCompleted; });
  const auto completed_transition =
      std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
        return event.type == KernelEventType::kParticipantSettled &&
               event.participant_fact.has_value() &&
               event.participant_fact->kind == ParticipantExecutionFactKind::kComputedCompletion;
      });
  const auto acknowledged =
      std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
        return event.type == KernelEventType::kCompletedAcknowledged;
      });
  const auto idle_transition =
      std::find_if(events.begin(), events.end(), [](const KernelEvent& event) {
        return event.type == KernelEventType::kStateTransition &&
               event.from_state == ExecutionState::kExecuting &&
               event.to_state == ExecutionState::kIdle;
      });
  ASSERT_NE(execute_completed, events.end());
  ASSERT_NE(completed_transition, events.end());
  ASSERT_NE(acknowledged, events.end());
  ASSERT_NE(idle_transition, events.end());
  EXPECT_LT(execute_completed, completed_transition);
  EXPECT_LT(completed_transition, acknowledged);
  EXPECT_LT(acknowledged, idle_transition);
}

TEST_F(ExecutionKernelTest, ExecuteAdmissionEventsFlushBeforeFastWorkerTerminalEvents) {
  std::mutex events_mutex;
  std::vector<KernelEventType> events;
  std::atomic<bool> delayed_first_admission_callback{false};

  kernel.subscribe_events([&](const KernelEvent& e) {
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      events.push_back(e.type);
    }
    if (e.type == KernelEventType::kStateTransition && e.to_state == ExecutionState::kPlanning &&
        !delayed_first_admission_callback.exchange(true)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });

  auto result = prepare_and_dispatch(kernel, make_test_request("admission_order_fast_worker"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(kernel.wait_for_completion(2'000'000'000));

  std::lock_guard<std::mutex> lock(events_mutex);
  const auto cache_miss = std::find(events.begin(), events.end(), KernelEventType::kCacheMiss);
  const auto plan_started = std::find(events.begin(), events.end(), KernelEventType::kPlanStarted);
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
  // Spec 230 §6.1: admission first enters Planning even when the request
  // later resolves to a cache hit.
  ASSERT_GE(events.size(), 10u);
  EXPECT_EQ(events[0], KernelEventType::kStateTransition);
  EXPECT_EQ(events[1], KernelEventType::kStateTransition);
  EXPECT_EQ(events[2], KernelEventType::kCacheHit);
  EXPECT_EQ(events[3], KernelEventType::kValidationStarted);
  EXPECT_EQ(events[4], KernelEventType::kValidationPassed);
  EXPECT_EQ(events[5], KernelEventType::kParticipantPrepared);
  EXPECT_EQ(events[6], KernelEventType::kStateTransition);
  EXPECT_EQ(events[7], KernelEventType::kExecuteStarted);
  EXPECT_EQ(events[8], KernelEventType::kExecuteCompleted);
  EXPECT_EQ(events[9], KernelEventType::kParticipantSettled);
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
  //   error_raised -> participant_settled (local-failure fact)
  ASSERT_GE(events.size(), 5u);
  EXPECT_EQ(events[0], KernelEventType::kStateTransition);
  EXPECT_EQ(events[1], KernelEventType::kCacheMiss);
  EXPECT_EQ(events[2], KernelEventType::kPlanStarted);
  EXPECT_EQ(events[3], KernelEventType::kErrorRaised);
  EXPECT_EQ(events[4], KernelEventType::kParticipantSettled);
}

TEST_F(ExecutionKernelTest, Events_ValidationFailure) {
  context.next_validation =
      ValidationResultDTO{false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

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
  //   error_raised -> participant_settled (local-failure fact)
  ASSERT_GE(events.size(), 9u);
  EXPECT_EQ(events[0], KernelEventType::kStateTransition);
  EXPECT_EQ(events[1], KernelEventType::kCacheMiss);
  EXPECT_EQ(events[2], KernelEventType::kPlanStarted);
  EXPECT_EQ(events[3], KernelEventType::kPlanCompleted);
  EXPECT_EQ(events[4], KernelEventType::kStateTransition);
  EXPECT_EQ(events[5], KernelEventType::kValidationStarted);
  EXPECT_EQ(events[6], KernelEventType::kValidationFailed);
  EXPECT_EQ(events[7], KernelEventType::kErrorRaised);
  EXPECT_EQ(events[8], KernelEventType::kParticipantSettled);
}

TEST_F(ExecutionKernelTest, Events_SubscribeUnsubscribe) {
  std::mutex events_mutex;
  std::vector<KernelEventType> events;
  auto handle = kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e.type);
  });

  const auto first = execute_sync(make_test_request());
  ASSERT_TRUE(first.ok());
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_FALSE(events.empty());
  }

  ASSERT_TRUE(kernel.acknowledge_completed(first.value()).accepted);
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
  std::vector<std::pair<ExecutionState, ParticipantExecutionFactKind>> endings;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition) {
      transitions.emplace_back(e.from_state, e.to_state);
    } else if (e.type == KernelEventType::kParticipantSettled) {
      ASSERT_TRUE(e.participant_fact.has_value());
      endings.emplace_back(e.to_state, e.participant_fact->kind);
    }
  }

  // Expected working phases Idle -> Planning -> Validating -> Executing,
  // then the completion fact reported from Executing (never a terminal).
  EXPECT_EQ(transitions.size(), 3u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].first, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].second, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].first, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].second, ExecutionState::kExecuting);
  ASSERT_EQ(endings.size(), 1u);
  EXPECT_EQ(endings[0].first, ExecutionState::kExecuting);
  EXPECT_EQ(endings[0].second, ParticipantExecutionFactKind::kComputedCompletion);
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
  std::vector<std::pair<ExecutionState, ParticipantExecutionFactKind>> endings;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition) {
      transitions.emplace_back(e.from_state, e.to_state);
    } else if (e.type == KernelEventType::kParticipantSettled) {
      ASSERT_TRUE(e.participant_fact.has_value());
      endings.emplace_back(e.to_state, e.participant_fact->kind);
    }
  }

  // Expected transitions (cache hit): admission enters Planning first so
  // stop/reset can interrupt slow preparation before the worker starts.
  // Idle -> Planning -> Validating -> Executing, then the completion fact.
  EXPECT_EQ(transitions.size(), 3u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].first, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].second, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].first, ExecutionState::kValidating);
  EXPECT_EQ(transitions[2].second, ExecutionState::kExecuting);
  ASSERT_EQ(endings.size(), 1u);
  EXPECT_EQ(endings[0].first, ExecutionState::kExecuting);
  EXPECT_EQ(endings[0].second, ParticipantExecutionFactKind::kComputedCompletion);
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
  std::vector<std::pair<ExecutionState, ParticipantExecutionFactKind>> endings;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition) {
      transitions.emplace_back(e.from_state, e.to_state);
    } else if (e.type == KernelEventType::kParticipantSettled) {
      ASSERT_TRUE(e.participant_fact.has_value());
      endings.emplace_back(e.to_state, e.participant_fact->kind);
    }
  }

  // Expected transitions (plan failure):
  // Idle -> Planning, then the local-failure fact reported from Planning.
  EXPECT_EQ(transitions.size(), 1u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
  ASSERT_EQ(endings.size(), 1u);
  EXPECT_EQ(endings[0].first, ExecutionState::kPlanning);
  EXPECT_EQ(endings[0].second, ParticipantExecutionFactKind::kLocalFailure);
}

TEST_F(ExecutionKernelTest, Events_StateTransition_ValidationFailure) {
  context.next_validation =
      ValidationResultDTO{false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

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
  std::vector<std::pair<ExecutionState, ParticipantExecutionFactKind>> endings;
  for (const auto& e : events) {
    if (e.type == KernelEventType::kStateTransition) {
      transitions.emplace_back(e.from_state, e.to_state);
    } else if (e.type == KernelEventType::kParticipantSettled) {
      ASSERT_TRUE(e.participant_fact.has_value());
      endings.emplace_back(e.to_state, e.participant_fact->kind);
    }
  }

  // Expected transitions (validation failure):
  // Idle -> Planning -> Validating, then the local-failure fact.
  EXPECT_EQ(transitions.size(), 2u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].first, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[1].second, ExecutionState::kValidating);
  ASSERT_EQ(endings.size(), 1u);
  EXPECT_EQ(endings[0].first, ExecutionState::kValidating);
  EXPECT_EQ(endings[0].second, ParticipantExecutionFactKind::kLocalFailure);
}

// ============================================================================
// Cancellation tests (spec 230 §6.2)
// ============================================================================

// ============================================================================
// New event tests (cache miss, cache invalidated, validation failed, collision
// detected)
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
  context.next_validation =
      ValidationResultDTO{false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

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
      EXPECT_EQ(e.details.at("reason"), "cache_revalidation_failed");
      break;
    }
  }
  EXPECT_TRUE(found_cache_invalidated);
}

TEST_F(ExecutionKernelTest, Events_CacheInvalidatedEventIncludesCompatibilityReason) {
  auto req = make_test_request("goal_changed_event");
  PoseGoalDTO pose_goal;
  pose_goal.goal = make_pose(0.45, -0.1, 0.2);
  req.motion = pose_goal;
  store_cache(req, make_test_trajectory());
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
  // Mock context returns invalid -> verify kValidationFailed emitted before
  // kErrorRaised
  context.next_validation =
      ValidationResultDTO{false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

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
      if (events[i].type == KernelEventType::kErrorRaised && error_raised_idx < 0) {
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
  EXPECT_LT(validation_failed_idx,
            error_raised_idx);  // validation_failed before error_raised
}

TEST_F(ExecutionKernelTest, Events_CollisionDetectedEvent) {
  // Call collision_check_tick() with collision -> verify kCollisionDetected
  // emitted First, we need to get into Executing state Since execution is
  // synchronous, we need a different approach We'll use a slow planner and
  // subscribe to events

  SlowMockPlanner slow_planner;
  FakeClock clock;
  ManualScheduler scheduler;
  TrajectoryCache cache{CacheConfig{10}};
  ExecutionKernel kernel{slow_planner, context, execution, clock, scheduler, cache};
  configure_authoritative_collision_validation(kernel, collision_world_publisher, collision_engine);

  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  // Start execution
  auto result = kernel.PrepareMotion(prepared_motion_request(make_test_request()));
  ASSERT_TRUE(result.ok());

  // Wait until planner is in the plan call
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Release the planner to let execution proceed
  slow_planner.release();
  ASSERT_TRUE(dispatch_when_prepared(kernel, result.value()).ok());

  // Wait for completion
  kernel.wait_for_completion();

  // Now we're in Completed state, we need to reset and try again
  // Actually, we need to be in Executing state to call collision_check_tick()
  // Let's use a different approach: manually set up the state

  // For now, let's just verify that collision_check_tick() works correctly
  // when called from Executing state by checking the collision_detections
  // metric
}

// ============================================================================
// Metrics tests
// ============================================================================

TEST_F(ExecutionKernelTest, MetricsCounters) {
  // Run a full execute cycle -> check get_metrics() returns correct counts
  execute_sync(make_test_request());

  auto metrics = kernel.get_metrics();
  // Terminal publication does not wait for the lower-priority lookahead
  // worker. Generation retirement cancels it, and it must become quiescent
  // within the bounded test window without delaying the terminal transition.
  for (int attempt = 0; attempt < 1000 && metrics.validation_collision_workers_active != 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    metrics = kernel.get_metrics();
  }
  EXPECT_EQ(metrics.cache_misses, 1u);
  EXPECT_EQ(metrics.plans_succeeded, 1u);
  EXPECT_EQ(metrics.validations_passed, 1u);
  EXPECT_EQ(metrics.executions_completed, 1u);
  EXPECT_EQ(metrics.validation_collision_workers_requested, 4u);
  EXPECT_EQ(metrics.validation_collision_workers_effective, 1u);
  EXPECT_EQ(metrics.validation_collision_jobs_queued, 0u);
  EXPECT_EQ(metrics.validation_collision_workers_active, 0u);
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
  context.next_validation =
      ValidationResultDTO{false, ErrorDTO{kErrorValidationCollision, "Collision", {}}};

  execute_sync(make_test_request());

  auto metrics = kernel.get_metrics();
  EXPECT_EQ(metrics.validations_failed, 1u);
}

TEST_F(ExecutionKernelTest, Metrics_RecordObserverLagOnSnapshotRead) {
  clock.set(1);
  execute_sync(make_test_request("metrics_lag_move"));

  auto before = kernel.get_metrics();
  const auto published = kernel.get_execution_snapshot();
  EXPECT_GT(published.sequence_number, 0u);

  clock.advance(25'000'000);              // 25 ms after publication
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
  configure_authoritative_collision_validation(kernel, collision_world_publisher, collision_engine);

  // Set up collision detection
  context.next_collision = CollisionCheckResultDTO{true, "link1", "box"};

  // Start execution
  auto result = kernel.PrepareMotion(prepared_motion_request(make_test_request()));
  ASSERT_TRUE(result.ok());

  // Wait until planner is in the plan call
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Release the planner to let execution proceed
  slow_planner.release();
  ASSERT_TRUE(dispatch_when_prepared(kernel, result.value()).ok());

  // Wait for completion
  kernel.wait_for_completion();

  // Note: collision_check_tick() is called externally, not during execute
  // So we need to test it separately
}

TEST_F(ExecutionKernelTest, EventSubscriberExceptionDoesNotBlockOtherSubscribers) {
  int delivered_to_healthy_subscriber = 0;
  bool threw_once = false;
  kernel.subscribe_events([&](const KernelEvent&) {
    if (!threw_once) {
      threw_once = true;
      throw std::runtime_error("subscriber failed");
    }
  });
  kernel.subscribe_events([&](const KernelEvent&) { ++delivered_to_healthy_subscriber; });

  execute_sync(make_test_request("subscriber_exception_metrics"));

  EXPECT_GT(delivered_to_healthy_subscriber, 0);
  EXPECT_EQ(kernel.get_metrics().event_subscriber_exceptions, 1u);
}

// =============================================================================
// Timeout Tests (W-1, W-2)
// =============================================================================

TEST_F(ExecutionKernelTest, ExecutionTimeout_UsesFinalManualDurationAndAppliedScale) {
  // Real Kernel worker, fake time. Each backend wait advances one virtual
  // second; no robot, wall-clock trajectory, or timing-sensitive test sleeps.
  enum class FeedbackFailure { kNone, kError, kThrow, kLostSupport };
  class TimedExecution final : public ControlledExecution {
   public:
    std::function<void()> tick;
    double applied_scale{1.0};
    FeedbackFailure failure{FeedbackFailure::kNone};
    bool timing_started{false};
    ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
      auto handle = ControlledExecution::execute_trajectory(trajectory);
      const auto wait = handle.wait_for_completion;
      handle.wait_for_completion = [this, wait](int64_t) {
        if (wait(1)) return true;
        timing_started = true;
        tick();
        return wait(1);
      };
      return handle;
    }
    RuntimeScaleMechanismStatusDTO runtime_velocity_scale_status() const override {
      RuntimeScaleMechanismStatusDTO result;
      result.supported = true;
      result.active = true;
      result.commanded_scale = 1.0;
      result.applied_scale = applied_scale;
      if (timing_started) {
        if (failure == FeedbackFailure::kThrow) throw std::runtime_error("timing feedback failed");
        if (failure == FeedbackFailure::kError) {
          result.error = ErrorDTO{kErrorInternalError, "timing feedback unavailable", {}};
        }
        if (failure == FeedbackFailure::kLostSupport) result.supported = false;
      }
      return result;
    }
  };
  struct Case {
    bool manual;
    double floor;
    double scale;
    int complete_at;
    int expected_ticks;
    bool hold_first;
    FeedbackFailure failure{FeedbackFailure::kNone};
  };
  for (const auto& test : {
           Case{true, 10.0, 1.0, 20, 20, false},    // A 20 s plan is not a 10 s failure.
           Case{true, 10.0, 1.0, 100, 32, false},   // Missing completion is still bounded.
           Case{false, 10.0, 1.0, 100, 10, false},  // Authored Program budget is unchanged.
           Case{true, 10.0, 0.25, 80, 80, false},   // Use applied, not requested scale.
           Case{true, 10.0, 1.0, 21, 21, true},     // Zero-scale hold consumes no budget.
           Case{false, 10.0, 1.0, 10, 10, false},   // Completion at the deadline wins.
           Case{true, 50.0, 1.0, 100, 50, false},   // Preserve a larger requested budget.
           // Item 40: timing feedback is never itself a failure (the branch's
           // `runtime_scale_timing_feedback_invalid` is removed). Without a
           // valid applied factor the accepted slider value (1.0 here) times
           // the motion, as `main` did, so the 32 s manual budget applies.
           Case{true, 10.0, std::numeric_limits<double>::quiet_NaN(), 100, 32, false},
           Case{true, 10.0, 1.0, 100, 32, false, FeedbackFailure::kError},
           Case{true, 10.0, 1.0, 100, 32, false, FeedbackFailure::kThrow},
           Case{true, 10.0, 1.0, 100, 32, false, FeedbackFailure::kLostSupport},
       }) {
    SCOPED_TRACE(test.expected_ticks);
    TimedExecution timed;
    timed.failure = test.failure;
    ThreadSafeFakeClock ts_clock;
    ThreadSafeManualScheduler ts_scheduler;
    TrajectoryCache local_cache{CacheConfig{10}};
    ExecutionKernel timed_kernel{planner, context, timed, ts_clock, ts_scheduler, local_cache};
    configure_authoritative_collision_validation(timed_kernel, collision_world_publisher,
                                                 collision_engine);
    planner.next_result.trajectory = make_test_trajectory();
    planner.next_result.trajectory->waypoints.back().time_from_start_s = 20.0;
    int ticks = 0;
    timed.tick = [&] {
      ++ticks;
      timed.applied_scale = test.hold_first && ticks == 1 ? 0.0 : test.scale;
      ts_clock.advance(test.hold_first && ticks == 1 ? 1'000'000'000'000 : 1'000'000'000);
      if (ticks >= test.complete_at) timed.complete();
    };
    auto request = prepared_motion_request(make_test_request()).execution_request;
    request.trajectory_relative_timeout = test.manual;
    request.timeouts.execution_timeout_s = test.floor;
    if (test.failure == FeedbackFailure::kLostSupport) {
      request.binding.runtime_scale_qualification =
          RuntimeScaleRouteQualificationDTO{request.binding.control_route_id,
                                            ScalingDomainId{"test-scaling"},
                                            EndpointRoleId{"scale-command"},
                                            EndpointRoleId{"scale-state"},
                                            std::nullopt,
                                            RuntimeScaleAcknowledgementKind::kAppliedState,
                                            4.0,
                                            100'000'000};
      const auto& binding = request.binding;
      request.initial_runtime_scale =
          ResolvedRuntimeScaleCommandDTO{binding.operation_id,
                                         OperationRevision{1},
                                         binding.participant_id,
                                         binding.controllable_component_id,
                                         binding.command_target_id,
                                         binding.capability_id,
                                         binding.control_domain_id,
                                         binding.control_domain_revision,
                                         binding.control_binding_id,
                                         binding.control_binding_revision,
                                         binding.controller_goal_id,
                                         binding.controller_goal_generation,
                                         binding.controller_topology_id,
                                         binding.topology_revision,
                                         binding.control_route_id,
                                         ScalingDomainId{"test-scaling"},
                                         std::nullopt,
                                         binding.resources,
                                         binding.active_control_configuration_id,
                                         binding.active_control_configuration_generation,
                                         RuntimeScaleGeneration{0},
                                         RuntimeScaleGeneration{0},
                                         1.0};
    }
    const auto accepted = prepare_and_dispatch(timed_kernel, request);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_TRUE(timed_kernel.wait_for_completion(2'000'000'000));
    EXPECT_EQ(ticks, test.expected_ticks);
    if (test.complete_at == test.expected_ticks) {
      EXPECT_EQ(ended_as(timed_kernel), "completed");
      EXPECT_EQ(timed.stop_count_value(), 0);
    } else {
      EXPECT_EQ(ended_as(timed_kernel), "failed");
      ASSERT_TRUE(timed_kernel.get_execution_snapshot().error.has_value());
      EXPECT_EQ(timed_kernel.get_execution_snapshot().error->code, kErrorExecutionTimeout);
      EXPECT_FALSE(timed_kernel.get_execution_snapshot().error->details.contains("reason"));
      EXPECT_GE(timed.stop_count_value(), 1);
    }
  }
}

// Item 40: an optional user-set zero-speed hold timeout. A hold at zero never
// trips the execution timeout (as `main`); when the hold lasts the user-set
// time, the participant asks its addressed owner, once, to end the motion
// Stopped like an operator Stop. Unset keeps `main`: nothing is requested.
TEST_F(ExecutionKernelTest, ZeroSpeedHoldTimeoutAsksTheAddressedOwnerToStopOnce) {
  class HeldExecution final : public ControlledExecution {
   public:
    std::function<void()> tick;
    std::atomic<double> applied_scale{0.0};
    ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
      auto handle = ControlledExecution::execute_trajectory(trajectory);
      const auto wait = handle.wait_for_completion;
      handle.wait_for_completion = [this, wait](int64_t) {
        if (wait(1)) return true;
        tick();
        return wait(1);
      };
      return handle;
    }
    RuntimeScaleMechanismStatusDTO runtime_velocity_scale_status() const override {
      RuntimeScaleMechanismStatusDTO result;
      result.supported = true;
      result.active = true;
      result.commanded_scale = applied_scale.load();
      result.applied_scale = applied_scale.load();
      return result;
    }
  };
  for (const auto hold_timeout_s : {std::optional<double>{}, std::optional<double>{3.0}}) {
    SCOPED_TRACE(hold_timeout_s.has_value() ? "hold timeout set" : "hold timeout unset");
    HeldExecution held;
    ThreadSafeFakeClock ts_clock;
    ThreadSafeManualScheduler ts_scheduler;
    TrajectoryCache local_cache{CacheConfig{10}};
    ExecutionKernel held_kernel{planner, context, held, ts_clock, ts_scheduler, local_cache};
    configure_authoritative_collision_validation(held_kernel, collision_world_publisher,
                                                 collision_engine);
    std::mutex events_mu;
    std::vector<KernelEvent> stop_requests;
    held_kernel.subscribe_events([&](const KernelEvent& event) {
      if (event.type != KernelEventType::kStopRequested) return;
      std::lock_guard lock(events_mu);
      stop_requests.push_back(event);
    });
    int ticks = 0;
    held.tick = [&] {
      ++ticks;
      ts_clock.advance(1'000'000'000);
      if (ticks == 6) held.applied_scale = 1.0;
      if (ticks >= 8) held.complete();
    };
    auto request = make_test_request("zero_speed_hold");
    request.timeouts.execution_timeout_s = 5.0;
    request.timeouts.zero_speed_hold_timeout_s = hold_timeout_s;
    const auto accepted = prepare_and_dispatch(held_kernel, request);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_TRUE(held_kernel.wait_for_completion(2'000'000'000));
    // Five held seconds plus two moving seconds never reach the 5 s budget.
    EXPECT_EQ(ended_as(held_kernel), "completed");
    EXPECT_EQ(held.stop_count_value(), 0);
    std::lock_guard lock(events_mu);
    if (!hold_timeout_s.has_value()) {
      EXPECT_TRUE(stop_requests.empty());
    } else {
      ASSERT_EQ(stop_requests.size(), 1U);
      EXPECT_EQ(stop_requests.front().details.at("reason"), "zero_speed_hold_timeout");
      EXPECT_EQ(stop_requests.front().execution_id, accepted.value());
    }
  }
}

TEST_F(ExecutionKernelTest, ZeroSpeedHoldTimeoutMustBePositiveWhenSet) {
  auto request = make_test_request("zero_speed_hold_invalid");
  request.timeouts.zero_speed_hold_timeout_s = 0.0;
  const auto result = prepare_and_dispatch(kernel, request);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason_code"), "invalid_zero_speed_hold_timeout");
}

// Item 17: a worker exception after dispatch leaves the command's effect
// uncertain (InterventionRequired at the addressed owner) and the Kernel
// issues the lane's hard stop, the primitive Reset uses.
TEST_F(ExecutionKernelTest, WorkerExceptionAfterDispatchIssuesTheLaneHardStop) {
  class ThrowingExecution final : public ControlledExecution {
   public:
    std::atomic_bool throw_next{false};
    ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
      auto handle = ControlledExecution::execute_trajectory(trajectory);
      const auto wait = handle.wait_for_completion;
      handle.wait_for_completion = [this, wait](int64_t timeout_ns) {
        if (throw_next.exchange(false)) throw std::runtime_error("worker fault after dispatch");
        return wait(timeout_ns);
      };
      return handle;
    }
  };
  ThrowingExecution throwing;
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel throwing_kernel{planner, context, throwing, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(throwing_kernel, collision_world_publisher,
                                               collision_engine);
  ASSERT_TRUE(prepare_and_dispatch(throwing_kernel, make_test_request("worker_exception")).ok());
  ASSERT_TRUE(throwing.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(throwing_kernel, ExecutionState::kExecuting));
  throwing.throw_next = true;
  ASSERT_TRUE(wait_for_ended(throwing_kernel, "failed_effect_uncertain", 5000));
  const auto status = throwing_kernel.get_status();
  ASSERT_TRUE(status.fact.has_value());
  ASSERT_TRUE(status.fact->error.has_value());
  EXPECT_EQ(status.fact->error->details.at("reason"), "worker_exception");
  EXPECT_EQ(throwing.stop_count_value(), 1);
}

// Item 18: a backend that ends the motion Stopped without an addressed Stop
// is a failure with its own reason, never a silent Stopped outcome.
TEST_F(ExecutionKernelTest, BackendStoppedWithoutAddressedStopFails) {
  ControlledExecution stopping;
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel stopping_kernel{planner, context, stopping, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(stopping_kernel, collision_world_publisher,
                                               collision_engine);
  ASSERT_TRUE(prepare_and_dispatch(stopping_kernel, make_test_request("backend_stopped")).ok());
  ASSERT_TRUE(stopping.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(stopping_kernel, ExecutionState::kExecuting));
  stopping.end_without_completion(ExecutionState::kStopped);
  ASSERT_TRUE(wait_for_ended(stopping_kernel, "failed", 5000));
  const auto status = stopping_kernel.get_status();
  ASSERT_TRUE(status.fact.has_value());
  EXPECT_EQ(status.fact->effect, ParticipantEffectKnowledge::kEffectSettled);
  ASSERT_TRUE(status.fact->error.has_value());
  EXPECT_EQ(status.fact->error->details.at("reason"), "backend_stopped_without_addressed_stop");
}

// Item 12: Reset after certified containment issues the lane's hard stop
// when the backend is still winding down and then completes; it never
// leaves an unresolved retirement behind.
TEST_F(ExecutionKernelTest, CertifiedResetHardStopsAWindingDownBackendThenCompletes) {
  ControlledExecution winding;
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel winding_kernel{planner, context, winding, ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(winding_kernel, collision_world_publisher,
                                               collision_engine);
  const auto result = prepare_and_dispatch(winding_kernel, make_test_request("reset_winding"));
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(winding.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(winding_kernel, ExecutionState::kExecuting));
  ASSERT_EQ(winding_kernel.fence_addressed_participant_stop(result.value()),
            AddressedParticipantStopFenceDisposition::kDispatchCommitted);
  winding.release();
  ASSERT_TRUE(winding_kernel.wait_for_completion(1'000'000'000));
  const auto stops_before = winding.stop_attempt_count_value();
  const auto retired_before = winding.retire_count.load();
  winding.retire_not_quiescent_refusals = 2;
  const auto reset = winding_kernel.reset_after_certified_containment(result.value());
  ASSERT_TRUE(reset.accepted) << reset.error->message;
  EXPECT_EQ(winding.stop_attempt_count_value(), stops_before + 1);
  EXPECT_EQ(winding.retire_count.load(), retired_before + 3);
  EXPECT_EQ(ended_as(winding_kernel), "working:Idle");
}

TEST_F(ExecutionKernelTest, ExecutionTimeout_TransitionsToFailed) {
  // ControlledExecution never completes — timeout should trigger
  ControlledExecution controlled_execution;
  // Need a separate kernel with ThreadSafe clock/scheduler for concurrency
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel timeout_kernel{planner,  context,      controlled_execution,
                                 ts_clock, ts_scheduler, local_cache};
  configure_authoritative_collision_validation(timeout_kernel, collision_world_publisher,
                                               collision_engine);

  auto req = make_test_request();
  req.timeouts.execution_timeout_s = 0.5;

  auto result = prepare_and_dispatch(timeout_kernel, req);
  ASSERT_TRUE(result.ok());

  // Wait until execution starts
  ASSERT_TRUE(controlled_execution.wait_until_execute_called())
      << "wait_until_execute_called timed out";
  ASSERT_TRUE(wait_for_state(timeout_kernel, ExecutionState::kExecuting));

  // Advance clock past the timeout
  ts_clock.advance(600'000'000);  // 600ms > 500ms timeout

  // The timeout check happens in the polling loop — need a brief real-time wait
  // for the worker thread to process
  const bool timed_out = wait_for_ended(timeout_kernel, "failed", 5000);
  if (!timed_out) {
    controlled_execution.release();
    (void)timeout_kernel.wait_for_completion(1'000'000'000);
  }
  ASSERT_TRUE(timed_out);

  auto status = timeout_kernel.get_status();
  EXPECT_EQ(status.state, ExecutionState::kExecuting);
  ASSERT_TRUE(status.fact.has_value());
  EXPECT_EQ(status.fact->effect, ParticipantEffectKnowledge::kEffectSettled);

  auto snapshot = timeout_kernel.get_execution_snapshot();
  EXPECT_TRUE(snapshot.error.has_value());
  if (snapshot.error) {
    EXPECT_EQ(snapshot.error->code, "execution_timeout");
  }

  // Backend stop should have been called
  EXPECT_GE(controlled_execution.stop_count_value(), 1);
}

TEST_F(ExecutionKernelTest, PlanningTimeout_IsPlanningFailureNotStop) {
  SlowMockPlanner slow_planner;
  ThreadSafeFakeClock ts_clock;
  ThreadSafeManualScheduler ts_scheduler;
  TrajectoryCache local_cache{CacheConfig{10}};
  ExecutionKernel timeout_kernel{slow_planner, context,      execution,
                                 ts_clock,     ts_scheduler, local_cache};
  configure_authoritative_collision_validation(timeout_kernel, collision_world_publisher,
                                               collision_engine);

  auto req = make_test_request();
  req.timeouts.planning_timeout_s = 0.1;

  auto result = timeout_kernel.PrepareMotion(prepared_motion_request(req));
  ASSERT_TRUE(result.ok());

  // Wait until planning starts
  ASSERT_TRUE(slow_planner.wait_until_in_plan()) << "wait_until_in_plan timed out";

  // Advance scheduler past the planning timeout: the watchdog records the
  // expiry and cancels planning; the worker commits the terminal.
  ts_scheduler.advance_to(ts_clock.now() + 200'000'000);  // 200ms > 100ms timeout
  EXPECT_GE(slow_planner.cancel_count, 1);

  // Release the planner (it returns, then the worker observes the expiry)
  slow_planner.release();

  // Wait for kernel to settle
  timeout_kernel.wait_for_completion();

  // P2: an expired planning deadline is the planning failure
  // `planning_timeout` with nothing dispatched, exactly as when the planner
  // backend reports the timeout; it is never a Stop and publishes no Stop
  // certificate.
  auto status = timeout_kernel.get_status();
  EXPECT_EQ(ended_as(status), "failed");
  ASSERT_TRUE(status.fact.has_value());
  EXPECT_EQ(status.fact->kind, ParticipantExecutionFactKind::kLocalFailure);
  EXPECT_EQ(status.fact->effect, ParticipantEffectKnowledge::kNoEffect);
  ASSERT_TRUE(status.fact->error.has_value());
  EXPECT_EQ(status.fact->error->code, kErrorPlanningTimeout);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_EQ(timeout_kernel.get_metrics().plans_failed, 1U);
  ts_scheduler.advance_to(ts_clock.now());
  const auto assessment = timeout_kernel.get_recovery_assessment();
  EXPECT_EQ(assessment.execution_state, ExecutionState::kFailed);
  EXPECT_NE(assessment.recovery_class, RecoveryClass::kOrdinaryOperatorStop);
}

TEST_F(ExecutionKernelTest, PlanningTimeout_BackendReportedMatchesKernelDeadline) {
  // P2: whichever timer notices the timeout first, the participant reports
  // the same planning failure fact.
  PlanResultDTO plan_err;
  plan_err.error = ErrorDTO{kErrorPlanningTimeout, "Planning timed out", {}};
  planner.next_result = plan_err;

  execute_sync(make_test_request());
  const auto status = kernel.get_status();
  EXPECT_EQ(ended_as(status), "failed");
  ASSERT_TRUE(status.fact.has_value());
  EXPECT_EQ(status.fact->kind, ParticipantExecutionFactKind::kLocalFailure);
  EXPECT_EQ(status.fact->effect, ParticipantEffectKnowledge::kNoEffect);
  ASSERT_TRUE(status.fact->error.has_value());
  EXPECT_EQ(status.fact->error->code, kErrorPlanningTimeout);
  EXPECT_EQ(execution.execute_count, 0);
}

TEST_F(ExecutionKernelTest, Execute_ProgramRunIdStampsEventsAndDoesNotLeakAcrossRuns) {
  // spec/500 §2.2/§4.1: the program_run_id of a prepared motion must reach
  // every kernel event for that execution and never leak onto the events of
  // a later execution.
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  auto request = make_test_request("program_run_correlated_move");
  request.binding.program_run_id = ProgramRunId("run-abc123");
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
          << static_cast<int>(event.type) << " execution_id=" << event.execution_id;
      EXPECT_EQ(it->second, "run-abc123");
    }
    events.clear();
  }

  // A later execution of another run must not inherit the stale id.
  auto next_run = make_test_request("next_run_move");
  next_run.binding.program_run_id = ProgramRunId("run-def456");
  auto second = execute_sync(next_run);
  ASSERT_TRUE(second.ok());
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    ASSERT_FALSE(events.empty());
    for (const auto& event : events) {
      const auto it = event.details.find("program_run_id");
      ASSERT_NE(it, event.details.end());
      EXPECT_EQ(it->second, "run-def456") << "stale program_run_id leaked onto a later execution";
    }
  }
}

TEST_F(ExecutionKernelTest, Execute_ActionSpanStampsEventsAndSnapshot) {
  // K6: behavior-owned action spans become the execution-layer correlation
  // key. The kernel must stamp the complete span tuple onto emitted events,
  // and observer snapshots, then clear it at
  // the acknowledgement boundary so the next action cannot inherit it.
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& e) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(e);
  });

  const auto span = make_test_action_span();
  auto request = make_test_request("span_correlated_move");
  request.binding.program_run_id = ProgramRunId(span.program_run_id);
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
  EXPECT_EQ(snapshot.action_span->parent_node_attempt_id, span.parent_node_attempt_id);
  EXPECT_TRUE(snapshot.contact_policy_id.empty());

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

TEST_F(ExecutionKernelTest, Execute_ContactPolicyStampsEventsAndFailedSnapshot) {
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
  request.binding.program_run_id = ProgramRunId(span.program_run_id);
  request.action_span = span;
  request.contact_policy_id = kContactPolicyId;

  auto result = execute_sync(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(ended_as(kernel), "failed");

  auto snapshot = kernel.get_execution_snapshot();
  ASSERT_TRUE(snapshot.action_span.has_value());
  EXPECT_EQ(snapshot.action_span->action_span_id, span.action_span_id);
  EXPECT_EQ(snapshot.action_span->node_attempt_id, span.node_attempt_id);
  EXPECT_EQ(snapshot.contact_policy_id, kContactPolicyId);

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    ASSERT_FALSE(events.empty());
    for (const auto& event : events) {
      expect_action_span_details(event.details, span);
      expect_detail(event.details, "contact_policy_id", kContactPolicyId);
    }
  }
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
  ExecutionKernel local_kernel{planner,     context,         controlled_execution,
                               local_clock, local_scheduler, local_cache};
  configure_authoritative_collision_validation(local_kernel, local_publisher, local_engine);

  std::mutex sequences_mutex;
  std::vector<std::uint64_t> sequences;
  auto handle = local_kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(sequences_mutex);
    sequences.push_back(event.sequence);
  });

  auto result = prepare_and_dispatch(local_kernel, make_test_request("sequence_contiguity"));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(controlled_execution.wait_until_execute_called());
  controlled_execution.complete();
  ASSERT_TRUE(local_kernel.wait_for_completion(2'000'000'000));
  ASSERT_TRUE(local_kernel.acknowledge_completed(result.value()).accepted);
  local_kernel.unsubscribe_events(handle);

  std::lock_guard<std::mutex> lock(sequences_mutex);
  ASSERT_GE(sequences.size(), 4u);
  for (std::size_t i = 0; i < sequences.size(); ++i) {
    EXPECT_EQ(sequences[i], static_cast<std::uint64_t>(i + 1))
        << "dispatch sequence must be contiguous from 1; index " << i;
  }
}

// Track 1 step 3: the participant reports facts, never operation terminals.
TEST_F(ExecutionKernelTest, ParticipantPublishesOnlyWorkingPhasesAndFacts) {
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });
  const auto completed = execute_sync(make_test_request("phases_complete"));
  ASSERT_TRUE(completed.ok());
  EXPECT_EQ(ended_as(kernel), "completed");
  ASSERT_TRUE(kernel.acknowledge_completed(completed.value()).accepted);
  PlanResultDTO plan_error;
  plan_error.error = ErrorDTO{kErrorPlanningFailed, "No solution", {}};
  planner.next_result = plan_error;
  ASSERT_TRUE(execute_sync(make_test_request("phases_fail")).ok());
  EXPECT_EQ(ended_as(kernel), "failed");

  std::lock_guard<std::mutex> lock(events_mutex);
  std::size_t facts = 0;
  for (const auto& event : events) {
    if (event.type == KernelEventType::kStateTransition) {
      EXPECT_TRUE(event.to_state == ExecutionState::kIdle ||
                  event.to_state == ExecutionState::kPlanning ||
                  event.to_state == ExecutionState::kValidating ||
                  event.to_state == ExecutionState::kExecuting)
          << to_string(event.to_state);
    }
    if (event.type == KernelEventType::kParticipantSettled) {
      ASSERT_TRUE(event.participant_fact.has_value());
      EXPECT_EQ(event.from_state, event.to_state);
      ++facts;
    }
  }
  EXPECT_EQ(facts, 2u);
}

// Track 1 step 3: an exact cancellation before dispatch is reported as the
// cancelled-before-dispatch fact, with nothing dispatched; the addressed
// Kernel owns the outcome.
TEST_F(ExecutionKernelTest, CancelBeforeDispatchIsReportedAsItsOwnFact) {
  std::mutex events_mutex;
  std::vector<KernelEvent> events;
  kernel.subscribe_events([&](const KernelEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back(event);
  });
  const auto prepared =
      kernel.PrepareMotion(prepared_motion_request(make_test_request("cancel_before_dispatch")));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  bool ready = false;
  for (int attempt = 0; attempt < 2000 && !ready; ++attempt) {
    const auto observed = kernel.ObservePreparedMotion(prepared.value());
    ready = observed.ok() &&
            observed.value().state == ResolvedPreparedMotionExecutionStateDTO::kPrepared;
    if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(ready);
  ASSERT_TRUE(kernel.CancelPreparedMotion(prepared.value()).ok());
  ASSERT_TRUE(wait_for_ended(kernel, "cancelled_before_dispatch"));
  const auto status = kernel.get_status();
  ASSERT_TRUE(status.fact.has_value());
  EXPECT_EQ(status.fact->effect, ParticipantEffectKnowledge::kNoEffect);
  EXPECT_FALSE(status.fact->error.has_value());
  EXPECT_NE(status.state, ExecutionState::kExecuting);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_EQ(execution.reset_count, 0);
  ASSERT_TRUE(kernel.wait_for_completion(1'000'000'000));
  std::lock_guard<std::mutex> lock(events_mutex);
  const auto settled = std::find_if(events.begin(), events.end(), [](const auto& event) {
    return event.type == KernelEventType::kParticipantSettled;
  });
  ASSERT_NE(settled, events.end());
  EXPECT_EQ(settled->participant_fact->kind, ParticipantExecutionFactKind::kCancelledBeforeDispatch);
  EXPECT_EQ(settled->execution_id, prepared.value());
}


// =============================================================================
// Core robot participant over IExecutionPort, driven by the real participant
// Kernel (shared execution design §4.1, §7.4). The fake port scripts every
// controller fact; the participant's waiter is the tick that advances fake
// time, so the Kernel worker's own timing runs deterministically.
// =============================================================================

class PortParticipantKernelTest : public ExecutionKernelTest {
 protected:
  void SetUp() override {
    ExecutionKernelTest::SetUp();
    ExecutionLaneFactsDTO facts;
    facts.joint_names = {"joint_1", "joint_2"};
    facts.supports_scale = true;
    facts.has_measured_velocity = true;
    facts.state_rate_hz = 100.0;
    facts.feedback_rate_hz = 10.0;
    facts.controller_update_rate_hz = 100.0;
    port->SetLaneFacts(facts);
    // The observed start state on the binding's compiled joint names.
    context.current_state_value.joint_names = {"joint_1", "joint_2"};
  }

  void TearDown() override {
    if (pkernel) {
      {
        std::lock_guard lock(hook_mutex);
        tick = nullptr;
      }
      // Let a motion still on the fake controller end so the worker exits.
      for (const auto& command : port->commands()) {
        if (const auto* submit = std::get_if<testing::FakeExecutionPort::SubmitCall>(&command)) {
          const auto goal = port->ReadGoal(submit->key);
          if (goal && goal->phase != ExecutionGoalPhase::kTerminal) {
            port->FinishGoal(submit->key, ExecutionGoalResult::kCanceled, std::nullopt,
                             pclock.now());
          }
        }
      }
      (void)pkernel->wait_for_completion(2'000'000'000);
    }
    pkernel.reset();
    participant.reset();
  }

  /// A fresh controller (fake port) and participant for the next case.
  void Reset() {
    TearDown();
    auto facts = port->ReadLaneFacts();
    port = std::make_shared<testing::FakeExecutionPort>();
    port->SetLaneFacts(facts);
    receipts = 0U;
    ticks = 0;
  }

  /// The fixture request on the binding's compiled joint names (the port's
  /// lane joint order), as a production planner returns them.
  static ExecuteRequestDTO PortRequest(const std::string& move_id) {
    auto request = make_test_request(move_id);
    request.binding.model_group_binding.compiled_joint_names = {"joint_1", "joint_2"};
    return request;
  }

  ExecutionPortParticipantConfig Config(StateAuthorityClass authority) {
    ExecutionPortParticipantConfig config;
    config.stop.trip_authority = authority;
    config.stop.stop_traj_delta_s = 0.05;
    config.stop.sample_period_s = 0.01;
    config.stop.replacement_start_allowance_s = 0.02;
    config.stop.max_deceleration_on_cancel_rad_s2 = {{"joint_1", 4.0}, {"joint_2", 4.0}};
    config.stop.hard_stop_completion_timeout_ns = 1'000'000'000;
    if (authority == StateAuthorityClass::kMeasured) {
      config.progress_freshness_budget_ns = 100'000'000;
      config.final_tolerance_rad = 0.01;
      config.start_tolerance_rad = 0.01;
      config.completion_state_timeout_ns = 500'000'000;
    }
    config.late_cycle_margin_s = 0.015;
    config.initial_speed_factor = 1.0;
    return config;
  }

  void Build(ExecutionPortParticipantConfig config) {
    participant = std::make_unique<ExecutionPortParticipant>(
        prepared_motion_binding(prepared_motion_stop_base_plan()), port, pclock, pscheduler,
        [this](TimePoint, const std::stop_token&) {
          {
            std::lock_guard lock(hook_mutex);
            ++ticks;
            if (tick) tick();
          }
          std::this_thread::sleep_for(std::chrono::microseconds(50));
        },
        ExecutionPortParticipantStateAuthority{&publication,
                                               [this] {
                                                 ++syncs;
                                                 return sync_hook ? sync_hook()
                                                                  : Result<void>::Ok();
                                               }},
        std::move(config));
    pkernel = std::make_unique<ExecutionKernel>(planner, context, *participant, pclock, pscheduler,
                                                pcache);
    configure_authoritative_collision_validation(*pkernel, collision_world_publisher,
                                                 collision_engine);
    planner.next_result.trajectory = PortTrajectory();
  }

  /// Both joints 0 -> 2 rad over 4 path seconds, complete P/V/A, at rest at
  /// both ends (a prepared trajectory with runtime-scale bounds).
  static TrajectoryDTO PortTrajectory() {
    TrajectoryDTO trajectory;
    trajectory.resources = {ResourceId("joint.1"), ResourceId("joint.2")};
    trajectory.joint_names = {"joint_1", "joint_2"};
    for (int index = 0; index <= 16; ++index) {
      const double t = 0.25 * index;
      const double v = index == 0 || index == 16 ? 0.0 : 0.5;
      trajectory.waypoints.push_back({t, {0.5 * t, 0.5 * t}, {v, v}, {0.0, 0.0}});
    }
    trajectory.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0};
    return trajectory;
  }

  std::optional<ExecutionGoalKey> SubmittedKey() const {
    for (const auto& command : port->commands()) {
      if (const auto* submit = std::get_if<testing::FakeExecutionPort::SubmitCall>(&command)) {
        return submit->key;
      }
    }
    return std::nullopt;
  }

  template <typename Call>
  std::vector<Call> Calls() const {
    std::vector<Call> calls;
    for (const auto& command : port->commands()) {
      if (const auto* call = std::get_if<Call>(&command)) calls.push_back(*call);
    }
    return calls;
  }

  void Executing(const ExecutionGoalKey& key) {
    ExecutionGoalObservationDTO goal;
    goal.key = key;
    goal.binding_epoch = port->ReadReadiness().binding_epoch;
    goal.phase = ExecutionGoalPhase::kExecuting;
    goal.mechanism_goal_id =
        MechanismGoalId("uuid-" + std::to_string(key.controller_goal_generation.value()));
    ExecutionGoalFeedbackDTO feedback;
    feedback.source_stamp_ns = pclock.now() - 1;
    feedback.local_receipt_stamp_ns = pclock.now() - 1;
    goal.latest_feedback = feedback;
    port->SetGoal(goal);
  }

  /// The newest replacement's final position (the level-3 brake's end).
  double BrakeEnd() const {
    const auto replaces = Calls<testing::FakeExecutionPort::ReplaceCall>();
    return replaces.back().trajectory.waypoints.back().positions_rad.front();
  }

  void Sample(double cursor, double position, double velocity, std::optional<double> factor = 1.0,
              double error = 0.0, std::optional<double> measured = std::nullopt) {
    ExecutionControllerSampleDTO sample;
    sample.source_stamp_ns = pclock.now();
    sample.local_receipt_stamp_ns = pclock.now();
    sample.local_receipt_count = ExecutionReceiptCount(++receipts);
    sample.binding_epoch = port->ReadReadiness().binding_epoch;
    sample.cursor_s = cursor;
    sample.applied_factor = factor;
    sample.reference_positions_rad = {position, position};
    sample.reference_velocities_rad_s = {velocity, velocity};
    sample.reference_accelerations_rad_s2 = {0.0, 0.0};
    sample.command_positions_rad = {position, position};
    sample.measured_positions_rad = {measured.value_or(position), measured.value_or(position)};
    sample.measured_velocities_rad_s = std::vector<double>{velocity, velocity};
    sample.error_positions_rad = {error, 0.0};
    port->SetSample(sample);
  }

  ResolvedParticipantStopStageDTO StageFor(std::uint64_t execution_id, std::size_t index) const {
    const auto observed = pkernel->ObservePreparedMotion(execution_id);
    if (!observed.ok() || !observed.value().last_dispatched_participant) {
      throw std::logic_error("no dispatched participant");
    }
    return StageForProduct(*observed.value().last_dispatched_participant, SubmittedKey().value(),
                           index);
  }

  static ResolvedParticipantStopStageDTO StageForProduct(
      const PreparedOperationParticipantDTO& participant, const ExecutionGoalKey& key,
      std::size_t index) {
    return ResolvedParticipantStopStageDTO{StopId("stop.port"),
                                           StopGeneration(1U),
                                           participant.operation_id,
                                           participant.operation_revision,
                                           participant.participant_id,
                                           key.controller_goal_id,
                                           key.controller_goal_generation,
                                           participant.controllable_component_id,
                                           participant.command_target_id,
                                           participant.capability_id,
                                           participant.binding_identity.control_domain_id,
                                           participant.binding_identity.control_domain_revision,
                                           participant.binding_identity.controller()->control_binding_id,
                                           participant.binding_identity.controller()->control_binding_revision,
                                           participant.controller().active_control_configuration_id,
                                           participant.controller().active_control_configuration_generation,
                                           participant.controller().controller_topology_id,
                                           participant.controller().topology_revision,
                                           participant.controller().control_route_id,
                                           std::nullopt,
                                           std::nullopt,
                                           participant.controller().control_provider.runtime_epoch,
                                           participant.resources,
                                           ContainmentGranularity::kResource,
                                           participant.controller().stop_evidence_policy_id,
                                           participant.controller().stop_evidence_policy_revision,
                                           participant.controller().ordinary_stop_plan,
                                           index,
                                           ContentDigest("scope.digest")};
  }

  /// Q51: the stage's own Stop-time values at the Stop request: a controlled
  /// command carries its own profile deadline, nothing else is shared.
  static RemainingStopBudgetDTO BudgetFor(const ResolvedParticipantStopStageDTO& stage) {
    RemainingStopBudgetDTO budget{stage.stop_id, stage.stop_generation, 0, std::nullopt};
    const auto& command = stage.participant_stop_plan.stage_commands.at(stage.stage_index);
    if (command.resolved_stage.containment_command.command_kind ==
            StopContainmentCommandKind::kMotionControlledStop &&
        command.validated_contract_parameters.has_value()) {
      budget.remaining_controlled_deadline_ns =
          controlled_stop_deadline_from_contract_parameters(*command.validated_contract_parameters);
    }
    return budget;
  }

  /// Dispatch one motion and wait until the controller goal was submitted.
  Result<std::uint64_t> Start(ExecuteRequestDTO request) {
    auto accepted = prepare_and_dispatch(*pkernel, std::move(request));
    if (accepted.ok()) {
      for (int attempt = 0; attempt < 2000 && !SubmittedKey(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    return accepted;
  }

  // Obtain an ordinary sealed fixture product through the real Kernel, then
  // use a fresh backend without a Kernel worker to script physical callbacks
  // deterministically. These tests exercise only the private backend contract.
  PreparedOperationParticipantDTO DirectBackendProduct(bool spring_return = false) {
    Build(Config(StateAuthorityClass::kCommanded));
    auto authority = prepared_motion_request(PortRequest("direct_backend_handover"));
    if (spring_return) {
      const auto& binding = authority.execution_request.binding;
      MotionInterruptStopProfileDTO profile;
      profile.profile_id = "direct.spring.return";
      profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
      profile.deceleration_time_s = 1.0;
      profile.jerk_limits_enabled = false;
      profile.allow_natural_completion_on_short_path = true;
      // The spring_back_hold return covers the forward braking distance in
      // the same half-time, so it needs more acceleration than the forward
      // braking (return peak 2.09 rad/s^2 here); 3.0 keeps it feasible.
      for (const auto& resource : authority.binding.value().resources)
        profile.per_joint_max_deceleration.emplace(*resource.source_joint_name,
                                                   AngularAccelerationDTO{3.0});
      auto plan = MakePreparedParticipantStopPlan(
          authority.stop_readiness.value().resolved_stop_base_plan,
          {binding.operation_id, authority.operation_revision, binding.participant_id,
           binding.program_id, binding.program_revision, binding.program_binding_id,
           binding.program_run_id, profile, ParticipantStopProfilePurposeDTO::kOperator,
           std::nullopt, authority.stop_node_category},
          authority.binding.value().resources, authority.retained_default_stop_limits);
      if (!plan.ok()) throw std::logic_error(plan.error().message);
      authority.ordinary_stop_plan =
          std::make_shared<const PreparedParticipantStopPlanDTO>(std::move(plan.value()));
    }
    const auto preparing = pkernel->PrepareMotion(std::move(authority));
    if (!preparing.ok()) throw std::logic_error(preparing.error().message);
    std::shared_ptr<const PreparedOperationParticipantDTO> product;
    for (int attempt = 0; attempt < 2000 && !product; ++attempt) {
      const auto observed = pkernel->ObservePreparedMotion(preparing.value());
      if (observed.ok() &&
          observed.value().state == ResolvedPreparedMotionExecutionStateDTO::kPrepared)
        product = observed.value().participant;
      if (!product) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!product) throw std::logic_error("fixture preparation did not settle");
    auto copied = *product;
    pkernel.reset();
    participant.reset();
    Build(Config(StateAuthorityClass::kCommanded));
    if (participant->Prepare(copied, 1U).state != PrepareParticipantResultStateDTO::kPrepared)
      throw std::logic_error("fresh backend rejected fixture product");
    return copied;
  }

  static PreparedParticipantStopPlanDTO DirectStopPlanFor(
      const PreparedOperationParticipantDTO& participant,
      const PreparedParticipantStopPlanDTO& previous,
      MotionInterruptNodeCategory category = MotionInterruptNodeCategory::kGeneric) {
    const auto authority = prepared_motion_request(PortRequest("direct_backend_stop_authority"));
    auto plan = MakePreparedParticipantStopPlan(
        authority.stop_readiness.value().resolved_stop_base_plan,
        {participant.operation_id, participant.operation_revision, participant.participant_id,
         participant.program_id, participant.program_revision, participant.program_binding_id,
         participant.program_run_id, previous.participant_stop_profile,
         previous.participant_stop_profile_purpose, previous.motion_interrupt_scope_id, category},
        authority.binding.value().resources, authority.retained_default_stop_limits);
    if (!plan.ok()) throw std::logic_error(plan.error().message);
    return std::move(plan.value());
  }

  static PreparedOperationParticipantDTO DirectSuccessor(
      const PreparedOperationParticipantDTO& original,
      const PreparedMotionReplacementSourceDTO& source) {
    auto successor = original;
    successor.operation_id = OperationId("operation.direct.successor");
    successor.prepared_contract_digest = ContentDigest(kTestDigestB);
    auto& motion = std::get<PreparedMotionExecutionDTO>(successor.payload);
    motion.plan_request.operation_id = successor.operation_id;
    successor.controller().ordinary_stop_plan =
        DirectStopPlanFor(successor, original.controller().ordinary_stop_plan);
    if (original.controller().fault_stop_plan)
      successor.controller().fault_stop_plan =
          DirectStopPlanFor(successor, *original.controller().fault_stop_plan);
    for (auto& scope : motion.interrupt_scopes)
      scope.stop_plan = DirectStopPlanFor(successor, scope.stop_plan, scope.node_category);
    const auto old = motion.trajectory;
    const double cursor = static_cast<double>(source.progress.trajectory_time_ns) / 1e9;
    auto future = slice_future_trajectory(old, cursor);
    if (!future.ok()) throw std::logic_error(future.error().message);
    motion.trajectory = std::move(future.value());
    motion.trajectory.runtime_scale_bounds = old.runtime_scale_bounds;
    const auto& request = motion.plan_request;
    motion.plan_request.moving_replacement =
        MovingReplacementInputDTO{source.goal,
                                  source.binding_epoch,
                                  cursor,
                                  old,
                                  cursor + 1.0,
                                  request.start_state,
                                  request.base_frame,
                                  request.tcp_frame,
                                  request.speed_profile,
                                  request.per_resource_dynamics_limits,
                                  1.0};
    return successor;
  }

  std::string Reason() const {
    auto error = pkernel->get_execution_snapshot().error;
    if (!error.has_value() && pkernel->get_status().fact.has_value()) {
      error = pkernel->get_status().fact->error;
    }
    if (!error.has_value()) return "none";
    auto found = error->details.find("reason_code");
    if (found == error->details.end()) found = error->details.find("reason");
    return found == error->details.end() ? error->code : found->second;
  }

  std::shared_ptr<testing::FakeExecutionPort> port = std::make_shared<testing::FakeExecutionPort>();
  FakeClock pclock;
  ThreadSafeManualScheduler pscheduler;
  TrajectoryCache pcache{CacheConfig{10}};
  ObservedRobotStatePublication publication{
      {"joint_1", "joint_2"},
      {ObservedRobotStateSourceConfig{
          ObservedRobotStateSourceKind::kJointState, "fixture_port_state",
          ObservedRobotStateAuthorityProfile::kMeasured, 1'000'000'000}}};
  std::mutex hook_mutex;
  std::function<void()> tick;
  std::atomic<int> ticks{0};
  std::atomic<std::uint64_t> receipts{0U};
  // M6 ADAPTERM: the lane's pre-planning sync (its re-engage), when set.
  std::function<Result<void>()> sync_hook;
  std::atomic<int> syncs{0};
  std::unique_ptr<ExecutionPortParticipant> participant;
  std::unique_ptr<ExecutionKernel> pkernel;
};

TEST_F(PortParticipantKernelTest, ExecutesWithCompletePathTimeTrajectoryCursorAndCompletion) {
  Build(Config(StateAuthorityClass::kCommanded));
  std::optional<ExecutionState> state_before_first_fact;
  double progress_at_half = -1.0;
  std::int64_t cursor_at_half_ns = -1;
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) {
      // Item 10: Executing is published at dispatch, before any controller fact.
      state_before_first_fact = participant->get_execution_snapshot().state;
      Executing(*key);
    }
    pclock.advance(250'000'000);
    const double cursor = std::min(4.0, 0.25 * step);
    Sample(cursor, 0.5 * cursor, 0.5);
    if (step == 9) {
      progress_at_half = participant->get_execution_snapshot().progress;
      cursor_at_half_ns = participant->execution_progress_publication()->acquire().trajectory_time_ns;
    }
    if (step == 17) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  };
  const auto accepted = Start(PortRequest("port_complete"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << ended_as(*pkernel) << " " << Reason();
  EXPECT_EQ(state_before_first_fact, ExecutionState::kExecuting);
  const auto submits = Calls<testing::FakeExecutionPort::SubmitCall>();
  ASSERT_EQ(submits.size(), 1U);
  const auto planned = PortTrajectory();
  ASSERT_EQ(submits.front().trajectory.waypoints.size(), planned.waypoints.size());
  for (std::size_t index = 0U; index < planned.waypoints.size(); ++index) {
    const auto& sent = submits.front().trajectory.waypoints[index];
    EXPECT_EQ(sent.time_from_start_s, planned.waypoints[index].time_from_start_s);
    EXPECT_EQ(sent.positions_rad, planned.waypoints[index].positions_rad);
    EXPECT_EQ(sent.velocities_rad_s, planned.waypoints[index].velocities_rad_s);
    EXPECT_EQ(sent.accelerations_rad_s2, planned.waypoints[index].accelerations_rad_s2);
  }
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
  // Progress is the controller's own cursor (§3.3), no second clock.
  EXPECT_DOUBLE_EQ(progress_at_half, 2.0 / 4.0);
  EXPECT_EQ(cursor_at_half_ns, 2'000'000'000);
  // Natural completion retains the original path's end for continuation.
  const auto observed = pkernel->ObservePreparedMotion(accepted.value());
  ASSERT_TRUE(observed.ok());
  EXPECT_EQ(observed.value().stopped_trajectory_time_s, 4.0);
  // Commanded authority (item 20): the commanded end state, no measured check.
  EXPECT_EQ(participant->get_execution_snapshot().joint_positions_rad,
            (std::vector<double>{2.0, 2.0}));
}

TEST_F(PortParticipantKernelTest, MeasuredDispatchChecksTheStartStateBeforeTheGoalIsSent) {
  // `main`'s observed-robot dispatch: a fresh complete measured sample within
  // the start tolerance of the first waypoint, else no goal is sent.
  struct Case {
    std::optional<double> start_measured;
    std::string expected;
    std::string reason;
  };
  for (const auto& test : {Case{0.005, "completed", "none"},
                           Case{0.05, "failed", "start_state_mismatch"},
                           Case{std::nullopt, "failed", "stale_controller_state"}}) {
    SCOPED_TRACE(test.reason);
    Reset();
    Build(Config(StateAuthorityClass::kMeasured));
    std::atomic<int> step{0};
    tick = [&] {
      pclock.advance(100'000'000);
      const auto key = SubmittedKey();
      if (!key) return;
      ++step;
      if (step == 1) Executing(*key);
      if (step < 3) {
        Sample(0.2 * step, 0.1 * step, 0.5);
      } else if (step == 3) {
        port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
      } else {
        Sample(4.0, 2.0, 0.0);
      }
    };
    if (test.start_measured) Sample(0.0, 0.0, 0.0, 1.0, 0.0, test.start_measured);
    ASSERT_TRUE(prepare_and_dispatch(*pkernel, PortRequest("port_start_state")).ok());
    ASSERT_TRUE(wait_for_ended(*pkernel, test.expected)) << ended_as(*pkernel) << " " << Reason();
    EXPECT_EQ(Reason(), test.reason);
    EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(),
              test.reason == "none" ? 1U : 0U);
    if (test.reason == "start_state_mismatch") {
      const auto error = pkernel->get_execution_snapshot().error;
      ASSERT_TRUE(error.has_value());
      EXPECT_EQ(error->code, "start_state_mismatch");
      EXPECT_EQ(error->details.at("start_tolerance_rad"), "0.010000");
      EXPECT_EQ(error->details.at("max_joint_error_rad"), "0.050000");
      // M6 ADAPTERM: e.g. a jog or Move-to target computed before a manual
      // move; the typed refusal tells the operator what to do.
      EXPECT_NE(error->message.find("preview again"), std::string::npos) << error->message;
    }
  }
}

// M6 ADAPTERM: an admitted motion after an idle move from the robot's own
// interface. The participant's pre-planning sync (the lane's re-engage) runs
// before planning; the robot then reports its current pose and the goal is
// sent from it.
TEST_F(PortParticipantKernelTest, AnIdleMoveIsReengagedBeforePlanningAndTheGoalStartsThere) {
  Build(Config(StateAuthorityClass::kMeasured));
  int plans_at_sync = -1;
  sync_hook = [&] {
    plans_at_sync = planner.plan_count;
    // The controllers are back: a fresh sample at the current pose.
    Sample(0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    return Result<void>::Ok();
  };
  std::atomic<int> step{0};
  tick = [&] {
    pclock.advance(100'000'000);
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) Executing(*key);
    if (step < 3) {
      Sample(0.2 * step, 0.1 * step, 0.5);
    } else if (step == 3) {
      port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
    } else {
      Sample(4.0, 2.0, 0.0);
    }
  };
  ASSERT_TRUE(prepare_and_dispatch(*pkernel, PortRequest("port_reengaged")).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << ended_as(*pkernel) << " " << Reason();
  EXPECT_EQ(syncs.load(), 1);
  EXPECT_EQ(plans_at_sync, 0);
  const auto submits = Calls<testing::FakeExecutionPort::SubmitCall>();
  ASSERT_EQ(submits.size(), 1U);
  EXPECT_EQ(submits.front().trajectory.waypoints.front().positions_rad,
            (std::vector<double>{0.0, 0.0}));
}

// M6 ADAPTERM: a re-engage that fails refuses the admitted motion typed,
// before planning; no goal is sent, the robot does not move.
TEST_F(PortParticipantKernelTest, AReengageFailureRefusesTheMotionBeforeAnyGoal) {
  Build(Config(StateAuthorityClass::kMeasured));
  sync_hook = [] {
    return Result<void>::Err(ErrorDTO{
        kErrorExecutionRejected,
        "The robot moved from its own interface was not re-engaged; the motion did not start",
        {{"reason", "motion_reengage_failed"}, {"cause", "xarm_reengage_not_confirmed"}}});
  };
  const auto refused = prepare_and_dispatch(*pkernel, PortRequest("port_not_reengaged"));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, kErrorExecutionRejected);
  EXPECT_EQ(refused.error().details.at("reason"), "motion_reengage_failed");
  EXPECT_EQ(refused.error().details.at("cause"), "xarm_reengage_not_confirmed");
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
}

// M6 (approval list 2, item 34): a jog/Move-to preview made while the arm
// counted as moved planned from the vendor pose. Execute keeps the existing
// flow: the pre-planning sync re-arms (ADAPTERM), then the participant checks
// the trajectory's start against the first fresh measured sample after the
// rebind. The fixture planner's trajectory starts at 0 rad, the pose it was
// planned from. Unchanged: the goal is sent from there. The fresh measured
// pose differs from that start: refused "preview again", no goal is sent.
TEST_F(PortParticipantKernelTest, AfterTheRearmTheGoalIsSentOnlyFromTheFreshMeasuredPose) {
  for (const double rearmed_at : {0.005, 0.05}) {
    const bool unchanged = rearmed_at < 0.01;
    SCOPED_TRACE(unchanged ? "unchanged" : "moved");
    Reset();
    syncs = 0;
    planner.plan_count = 0;  // Reset() keeps the planner's count across passes.
    Build(Config(StateAuthorityClass::kMeasured));
    int plans_at_sync = -1;
    sync_hook = [&] {
      plans_at_sync = planner.plan_count;
      // The re-arm: the controllers are back, the first fresh sample.
      Sample(0.0, 0.0, 0.0, 1.0, 0.0, rearmed_at);
      return Result<void>::Ok();
    };
    std::atomic<int> step{0};
    tick = [&] {
      pclock.advance(100'000'000);
      const auto key = SubmittedKey();
      if (!key) return;
      ++step;
      if (step == 1) Executing(*key);
      if (step < 3) {
        Sample(0.2 * step, 0.1 * step, 0.5);
      } else if (step == 3) {
        port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
      } else {
        Sample(4.0, 2.0, 0.0);
      }
    };
    ASSERT_TRUE(prepare_and_dispatch(*pkernel, PortRequest("port_live_preview")).ok());
    ASSERT_TRUE(wait_for_ended(*pkernel, unchanged ? "completed" : "failed"))
        << ended_as(*pkernel) << " " << Reason();
    EXPECT_EQ(syncs.load(), 1);
    EXPECT_EQ(plans_at_sync, 0);
    const auto submits = Calls<testing::FakeExecutionPort::SubmitCall>();
    if (unchanged) {
      ASSERT_EQ(submits.size(), 1U);
      EXPECT_EQ(submits.front().trajectory.waypoints.front().positions_rad,
                (std::vector<double>{0.0, 0.0}));
    } else {
      EXPECT_TRUE(submits.empty());
      EXPECT_EQ(Reason(), "start_state_mismatch");
      const auto error = pkernel->get_execution_snapshot().error;
      ASSERT_TRUE(error.has_value());
      EXPECT_EQ(error->code, "start_state_mismatch");
      EXPECT_NE(error->message.find("preview again"), std::string::npos) << error->message;
    }
    sync_hook = nullptr;
  }
}

TEST_F(PortParticipantKernelTest, DiagnosticContextCallbackPrecedesSendAndCannotChangeOutcome) {
  auto config = Config(StateAuthorityClass::kCommanded);
  std::atomic<int> observations{0};
  const auto request = PortRequest("trace_diagnostic_only");
  // Start seals this fixture request under its prepared operation identity.
  const auto expected_operation = prepared_motion_request(request).execution_request.binding.operation_id;
  config.observe_goal_context = [&](const auto& key, const auto& prepared, const auto epoch) {
    ++observations;
    EXPECT_EQ(prepared.operation_id, expected_operation);
    EXPECT_EQ(prepared.program_run_id, request.binding.program_run_id);
    EXPECT_EQ(key.controller_goal_id, request.binding.controller_goal_id);
    EXPECT_EQ(epoch, port->ReadReadiness().binding_epoch);
    EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
    throw std::runtime_error("diagnostic failure must not alter dispatch");
  };
  Build(std::move(config));
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    pclock.advance(250'000'000);
    if (step == 1) Executing(*key);
    if (step < 17) Sample(0.25 * step, 0.125 * step, 0.5);
    if (step == 17) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  };
  ASSERT_TRUE(Start(request).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << Reason();
  EXPECT_EQ(observations.load(), 1);
  EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
}

// M6 ADAPTERM TOOLCTX (live 2026-10-28): a TCP offset send leaves the
// controller's ready state; the lane re-arms the robot and binds the
// reactivated controller on a new port (a new epoch). The dispatch decision
// then runs again on that port: a fresh sample at the start pose sends the
// goal there; a robot that is no longer at the start refuses typed.
TEST_F(PortParticipantKernelTest, AToolContextRearmRevalidatesTheDispatchOnTheReboundPort) {
  for (const bool moved : {false, true}) {
    SCOPED_TRACE(moved ? "moved" : "at the start");
    Reset();
    auto config = Config(StateAuthorityClass::kMeasured);
    std::shared_ptr<testing::FakeExecutionPort> old_port;
    config.apply_tool_context = [&](const PreparedOperationParticipantDTO&) {
      old_port = port;
      auto next = std::make_shared<testing::FakeExecutionPort>();
      next->SetLaneFacts(port->ReadLaneFacts());
      auto readiness = port->ReadReadiness();
      readiness.binding_epoch = ExecutionBindingEpoch(readiness.binding_epoch.value() + 1U);
      next->SetReadiness(readiness);
      const auto rebound = participant->RebindPort(next);
      EXPECT_TRUE(rebound.ok());
      port = next;
      Sample(0.0, 0.0, 0.0, 1.0, 0.0, moved ? 0.05 : 0.0);
      return Result<void>::Ok();
    };
    Build(std::move(config));
    std::atomic<int> step{0};
    tick = [&] {
      pclock.advance(100'000'000);
      const auto key = SubmittedKey();
      if (!key) return;
      ++step;
      if (step == 1) Executing(*key);
      if (step < 3) {
        Sample(0.2 * step, 0.1 * step, 0.5);
      } else if (step == 3) {
        port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
      } else {
        Sample(4.0, 2.0, 0.0);
      }
    };
    Sample(0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    ASSERT_TRUE(prepare_and_dispatch(*pkernel, PortRequest(moved ? "rearm_moved" : "rearm_ok")).ok());
    // The worker applies the tool context after the dispatch returns: wait for the end.
    ASSERT_TRUE(wait_for_ended(*pkernel, moved ? "failed" : "completed"))
        << ended_as(*pkernel) << " " << Reason();
    ASSERT_TRUE(old_port);
    // Never a goal on the retired port.
    for (const auto& command : old_port->commands())
      EXPECT_FALSE(std::holds_alternative<testing::FakeExecutionPort::SubmitCall>(command));
    if (!moved) {
      EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
      continue;
    }
    EXPECT_EQ(Reason(), "start_state_mismatch");
    EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
  }
}

TEST_F(PortParticipantKernelTest, ToolContextIsAppliedAtRestBeforeTheSendAndAFailureRefusesTheMotion) {
  // TOOLCTX: the lane applies the robot's tool context once per new goal,
  // before Submit (nothing in flight), with the motion's own TCP. A context
  // the robot did not confirm sends no goal: the motion fails typed.
  for (const bool fails : {false, true}) {
    SCOPED_TRACE(fails ? "fails" : "applies");
    Reset();
    auto config = Config(StateAuthorityClass::kCommanded);
    std::atomic<int> applications{0};
    config.apply_tool_context = [&](const PreparedOperationParticipantDTO& prepared) {
      ++applications;
      EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
      EXPECT_FALSE(compiled_frame_name(
                       std::get<PreparedMotionExecutionDTO>(prepared.payload).plan_request.tcp_frame)
                       .empty());
      if (!fails) return Result<void>::Ok();
      return Result<void>::Err(ErrorDTO{
          "execution_rejected", "the robot did not confirm its TCP offset",
          {{"reason", "xarm_service_timeout"}, {"tool_context_field", "tcp_offset"}}});
    };
    Build(std::move(config));
    std::atomic<int> step{0};
    tick = [&] {
      const auto key = SubmittedKey();
      if (!key) return;
      ++step;
      pclock.advance(250'000'000);
      if (step == 1) Executing(*key);
      if (step < 17) Sample(0.25 * step, 0.125 * step, 0.5);
      if (step == 17) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
    };
    ASSERT_TRUE(Start(PortRequest(fails ? "tool_context_fails" : "tool_context_applies")).ok());
    if (!fails) {
      ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << Reason();
      EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
      EXPECT_EQ(applications.load(), 1);
      continue;
    }
    ASSERT_TRUE(wait_for_ended(*pkernel, "failed")) << ended_as(*pkernel);
    EXPECT_EQ(Reason(), "tool_context_not_applied");
    EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
    EXPECT_EQ(applications.load(), 1);
    auto error = pkernel->get_execution_snapshot().error;
    if (!error.has_value() && pkernel->get_status().fact.has_value())
      error = pkernel->get_status().fact->error;
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->details.at("cause"), "xarm_service_timeout");
    EXPECT_EQ(error->details.at("tool_context_field"), "tcp_offset");
  }
}

TEST_F(PortParticipantKernelTest, StopDuringASlowToolContextApplyWinsAndNoGoalIsSent) {
  // S1: the tool-context send runs without the participant lock. A Stop
  // pressed during a slow send enters at once (it does not wait behind the
  // send), and the goal is then refused with the Stop-wins outcome: no
  // Submit, before or after the Stop.
  const auto product = DirectBackendProduct();
  pkernel.reset();
  participant.reset();
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> applications{0};
  auto config = Config(StateAuthorityClass::kCommanded);
  config.apply_tool_context = [&, released](const PreparedOperationParticipantDTO&) {
    if (++applications == 1) entered.set_value();
    released.wait();  // a slow vendor send (e.g. set_tcp_offset)
    return Result<void>::Ok();
  };
  Build(std::move(config));
  ASSERT_EQ(participant->Prepare(product, 1U).state, PrepareParticipantResultStateDTO::kPrepared);
  const ExecutionGoalKey key{ControllerGoalId("slow_apply.goal"), ControllerGoalGeneration(1U)};
  ExecutionHandle handle;
  Result<void> applied = Result<void>::Ok();
  std::thread dispatcher([&] {
    // M6b: the tool-context action, then the dispatch (as the Kernel runs them).
    applied = participant->ApplyToolContext(product);
    handle = participant->Dispatch(product, key.controller_goal_id, key.controller_goal_generation);
  });
  entered.get_future().wait();
  // The Stop (cancelled right after its entry so this test ends) runs while
  // the send is still blocked.
  std::atomic<bool> stop_returned{false};
  std::thread stopper([&] {
    std::stop_source cancel;
    cancel.request_stop();
    const auto stage = StageForProduct(product, key, 0U);
    (void)participant->TranslateStopStage(stage, BudgetFor(stage), cancel.get_token());
    stop_returned = true;
  });
  for (int attempt = 0; attempt < 2000 && !stop_returned; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_TRUE(stop_returned.load()) << "the Stop waited behind the tool-context send";
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
  release.set_value();
  dispatcher.join();
  stopper.join();
  EXPECT_EQ(applications.load(), 1);
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
  ASSERT_FALSE(applied.ok());
  EXPECT_EQ(applied.error().details.at("reason"), "execution_port_stop_in_flight");
  EXPECT_EQ(handle.execution_generation, 0U);
  ASSERT_TRUE(handle.get_snapshot);
  const auto snapshot = handle.get_snapshot();
  EXPECT_EQ(snapshot.state, ExecutionState::kFailed);
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"), "execution_port_stop_in_flight");
}

TEST_F(PortParticipantKernelTest, AStopBeforeItsGoalIsSentIsContainedAndFencesThatGoal) {
  // M6b (maintainer rule): a Stop stage naming a goal this participant never
  // sent, with no goal live and no other Stop running, has nothing to stop. It
  // is contained without any controller sample or rest check (not a failed
  // uncontained Stop), and that goal is never sent afterwards.
  const auto product = DirectBackendProduct();
  pkernel.reset();
  participant.reset();
  Build(Config(StateAuthorityClass::kCommanded));
  ASSERT_EQ(participant->Prepare(product, 1U).state, PrepareParticipantResultStateDTO::kPrepared);
  // No controller sample at all (e.g. the joint state stream is off).
  const ExecutionGoalKey key{ControllerGoalId("unsent.goal"), ControllerGoalGeneration(1U)};
  const auto stage = StageForProduct(product, key, 0U);
  const auto result = participant->TranslateStopStage(stage, BudgetFor(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->details.at("reason") : "");
  EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSoftOnPath);
  EXPECT_FALSE(result.error.has_value());
  ASSERT_EQ(result.evidence_items.size(),
            stage.participant_stop_plan.stage_commands.at(0U).resolved_stage.evidence_requirements.size());
  for (const auto& item : result.evidence_items) {
    EXPECT_EQ(item.state, StopEvidenceItemStateDTO::kSatisfied);
  }
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
  EXPECT_FALSE(participant->GoalInFlight());

  const auto handle =
      participant->Dispatch(product, key.controller_goal_id, key.controller_goal_generation);
  EXPECT_EQ(handle.execution_generation, 0U);
  ASSERT_TRUE(handle.get_snapshot);
  const auto snapshot = handle.get_snapshot();
  EXPECT_EQ(snapshot.state, ExecutionState::kFailed);
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason"), "execution_port_stop_in_flight");
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());

  // No further checks: a sample reporting motion does not change it.
  Sample(0.0, 0.0, 0.5);
  const ExecutionGoalKey other{ControllerGoalId("unsent.goal.other"),
                               ControllerGoalGeneration(2U)};
  const auto other_stage = StageForProduct(product, other, 0U);
  const auto contained =
      participant->TranslateStopStage(other_stage, BudgetFor(other_stage), {});
  EXPECT_EQ(contained.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
}

TEST_F(PortParticipantKernelTest, ToolContextIsNeverSentForAMovingReplacement) {
  // TOOLCTX: the tool context is applied for a new goal only, never for a
  // moving replacement (Replace, or ReplaceScheduled on a lane that supports
  // it): the robot is moving then.
  for (const bool scheduled : {false, true}) {
    SCOPED_TRACE(scheduled ? "ReplaceScheduled" : "Replace");
    Reset();
    auto lane = port->ReadLaneFacts();
    lane.supports_scheduled_replacement = scheduled;
    port->SetLaneFacts(lane);
    const auto product = DirectBackendProduct();
    pkernel.reset();
    participant.reset();
    std::atomic<int> applications{0};
    auto config = Config(StateAuthorityClass::kCommanded);
    config.apply_tool_context = [&](const PreparedOperationParticipantDTO&) {
      ++applications;
      return Result<void>::Ok();
    };
    Build(std::move(config));
    ASSERT_EQ(participant->Prepare(product, 1U).state, PrepareParticipantResultStateDTO::kPrepared);
    Sample(0.0, 0.0, 0.0);
    const ExecutionGoalKey old{ControllerGoalId("toolctx.old"), ControllerGoalGeneration(1U)};
    ASSERT_TRUE(participant->ApplyToolContext(product).ok());
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    ASSERT_NE(handle.execution_generation, 0U);
    EXPECT_EQ(applications.load(), 1);
    pclock.advance(10'000'000);
    Executing(old);
    Sample(0.5, 0.25, 0.5);
    EXPECT_FALSE(handle.wait_for_completion(0));
    pclock.advance(5'000'000);
    const auto source = participant->CaptureReplacementSource(
        1U, handle.execution_generation, old, product.prepared_contract_digest);
    ASSERT_TRUE(source.ok()) << source.error().message;
    const auto successor = DirectSuccessor(product, source.value());
    ASSERT_EQ(participant->PrepareReplacement(successor, 2U, source.value()).state,
              PrepareParticipantResultStateDTO::kPrepared);
    const ExecutionGoalKey next{ControllerGoalId("toolctx.next"), ControllerGoalGeneration(2U)};
    const auto sent = participant->DispatchReplacement(source.value(), 2U, successor, next,
                                                       pclock.now() + 1'000'000'000);
    ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
    EXPECT_EQ(Calls<testing::FakeExecutionPort::ReplaceCall>().size(), 1U);
    EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
    EXPECT_EQ(applications.load(), 1);  // the original goal's send only
  }
}

// M6b (maintainer 2026-10-29): the tool-context action is its own step before
// the motion is sent, and the motion counts as running (Executing) only once
// it was sent: during the action the execution is still Validating, nothing
// was submitted and no execute-started event was published. A refused action
// never reaches Executing.
TEST_F(PortParticipantKernelTest, TheMotionCountsAsRunningOnlyAfterTheToolContextAction) {
  for (const bool fails : {false, true}) {
    SCOPED_TRACE(fails ? "refused" : "applied");
    Reset();
    std::mutex events_mutex;
    std::vector<KernelEvent> events;
    std::atomic<int> applications{0};
    std::optional<ExecutionState> state_during_action;
    bool started_during_action = true;
    auto config = Config(StateAuthorityClass::kCommanded);
    config.apply_tool_context = [&](const PreparedOperationParticipantDTO&) {
      ++applications;
      const auto state = pkernel->get_status().state;
      {
        std::lock_guard lock(events_mutex);
        state_during_action = state;
        started_during_action = std::any_of(events.begin(), events.end(), [](const auto& event) {
          return event.type == KernelEventType::kExecuteStarted ||
                 (event.type == KernelEventType::kStateTransition &&
                  event.to_state == ExecutionState::kExecuting);
        });
      }
      EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
      if (!fails) return Result<void>::Ok();
      return Result<void>::Err(ErrorDTO{
          "robot_controller_unresponsive", "the robot controller did not answer in time",
          {{"reason", "xarm_set_tcp_offset"}, {"tool_context_field", "tcp_offset"}}});
    };
    Build(std::move(config));
    const auto subscription = pkernel->subscribe_events([&](const KernelEvent& event) {
      std::lock_guard lock(events_mutex);
      events.push_back(event);
    });
    struct Unsubscribe final {
      ExecutionKernel& kernel;
      decltype(subscription) handle;
      ~Unsubscribe() { kernel.unsubscribe_events(handle); }
    } unsubscribe{*pkernel, subscription};
    std::atomic<int> step{0};
    tick = [&] {
      const auto key = SubmittedKey();
      if (!key) return;
      ++step;
      pclock.advance(250'000'000);
      if (step == 1) Executing(*key);
      if (step < 17) Sample(0.25 * step, 0.125 * step, 0.5);
      if (step == 17) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
    };
    ASSERT_TRUE(Start(PortRequest(fails ? "toolctx_action_refused" : "toolctx_action_applied")).ok());
    ASSERT_TRUE(wait_for_ended(*pkernel, fails ? "failed" : "completed"))
        << ended_as(*pkernel) << " " << Reason();
    EXPECT_EQ(applications.load(), 1);
    std::lock_guard lock(events_mutex);
    EXPECT_EQ(state_during_action, std::optional(ExecutionState::kValidating));
    EXPECT_FALSE(started_during_action);
    const auto started = std::count_if(events.begin(), events.end(), [](const auto& event) {
      return event.type == KernelEventType::kExecuteStarted;
    });
    if (!fails) {
      EXPECT_EQ(started, 1);
      EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
      continue;
    }
    EXPECT_EQ(started, 0);
    EXPECT_TRUE(Calls<testing::FakeExecutionPort::SubmitCall>().empty());
    EXPECT_EQ(Reason(), "tool_context_not_applied");
    auto error = pkernel->get_execution_snapshot().error;
    if (!error.has_value() && pkernel->get_status().fact.has_value())
      error = pkernel->get_status().fact->error;
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->details.at("cause"), "xarm_set_tcp_offset");
    EXPECT_EQ(error->details.at("tool_context_field"), "tcp_offset");
  }
}

TEST_F(PortParticipantKernelTest, CommandedDispatchHasNoMeasuredStartStateCheck) {
  // As `main`'s commanded simulation: the commanded state is the start.
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    pclock.advance(250'000'000);
    if (step == 1) Executing(*key);
    if (step < 17) Sample(0.25 * step, 0.125 * step, 0.5);
    if (step == 17) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  };
  // No sample at all before dispatch, and the measured position far away.
  ASSERT_TRUE(Start(PortRequest("port_commanded_start")).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << ended_as(*pkernel) << " " << Reason();
  EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
}

TEST_F(PortParticipantKernelTest, SampleReceivedBetweenTheClockAndSampleReadsIsNotStale) {
  // Stage F run 9: a controller_state receipt stamped on the participant's own
  // monotonic clock just after Evaluate read `now` failed the in-motion
  // maximum age check (stale_controller_state, sample_age_s=-0.000001). Here
  // every new sample's receipt lands 1 us after the reader's clock read.
  auto config = Config(StateAuthorityClass::kCommanded);
  config.stop.controller_state_max_age_ns = 250'000'000;
  Build(std::move(config));
  port->SetOnReadSample(
      [this, last = std::uint64_t{0}](std::optional<ExecutionControllerSampleDTO>& sample) mutable {
        if (!sample || sample->local_receipt_count.value() == last) return;
        last = sample->local_receipt_count.value();
        pclock.advance(1'000);
        sample->local_receipt_stamp_ns = pclock.now();
      });
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    pclock.advance(10'000'000);
    if (step == 1) Executing(*key);
    if (step < 17) Sample(0.25 * step, 0.125 * step, 0.5);
    if (step == 17) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  };
  Sample(0.0, 0.0, 0.0);
  ASSERT_TRUE(Start(PortRequest("port_receipt_between_reads")).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << ended_as(*pkernel) << " " << Reason();
  EXPECT_EQ(Reason(), "none");
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
  port->SetOnReadSample(nullptr);
}

TEST_F(PortParticipantKernelTest, MeasuredAuthorityRequiresTheStartTolerance) {
  auto config = Config(StateAuthorityClass::kMeasured);
  config.start_tolerance_rad.reset();
  EXPECT_THROW(Build(std::move(config)), std::invalid_argument);
  auto negative = Config(StateAuthorityClass::kMeasured);
  negative.start_tolerance_rad = -0.01;
  EXPECT_THROW(Build(std::move(negative)), std::invalid_argument);
}

TEST_F(PortParticipantKernelTest, MeasuredCompletionChecksTheFinalToleranceAfterTheResult) {
  struct Case {
    std::optional<double> final_measured;
    std::string expected;
    std::string reason;
  };
  for (const auto& test : {Case{2.005, "completed", "none"},
                           Case{2.05, "failed", "tracking_error_exceeded"},
                           Case{std::nullopt, "failed", "stale_controller_state"}}) {
    SCOPED_TRACE(test.reason);
    Reset();
    Build(Config(StateAuthorityClass::kMeasured));
    std::atomic<int> step{0};
    tick = [&] {
      const auto key = SubmittedKey();
      if (!key) return;
      ++step;
      pclock.advance(100'000'000);
      if (step == 1) Executing(*key);
      if (step < 3) {
        Sample(0.2 * step, 0.1 * step, 0.5);
      } else if (step == 3) {
        // The successful result; the measured gate needs a later sample.
        port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
      } else if (test.final_measured) {
        Sample(4.0, 2.0, 0.0, 1.0, 0.0, test.final_measured);
      }
    };
    Sample(0.0, 0.0, 0.0);  // the measured start state (start-state check)
    ASSERT_TRUE(Start(PortRequest("port_measured")).ok());
    ASSERT_TRUE(wait_for_ended(*pkernel, test.expected)) << ended_as(*pkernel) << " " << Reason();
    EXPECT_EQ(Reason(), test.reason);
    if (test.reason != "none") {
      EXPECT_EQ(pkernel->get_execution_snapshot().error->details.at("phase"),
                "terminal_result_state_gate");
    }
  }
}

// Merge 6 arrival ("finished and within tolerance"): every lane keeps its
// arrival tolerance as the JTC goal constraint, so the controller reports a
// trajectory that finished outside it as a goal-tolerance abort and holds a
// trajectory that has not settled. Samples at the target never complete the
// motion before the controller reports it finished; finished within tolerance
// (a succeeded result, plus the measured lane's own terminal sample) does.
// The measured lane's finished-but-out-of-tolerance sample is covered by
// MeasuredCompletionChecksTheFinalToleranceAfterTheResult.
TEST_F(PortParticipantKernelTest, ArrivalNeedsTheFinishedResultAndTheLaneTolerance) {
  for (const auto authority : {StateAuthorityClass::kCommanded, StateAuthorityClass::kMeasured}) {
    for (const auto result :
         {ExecutionGoalResult::kSucceeded, ExecutionGoalResult::kAbortedGoalTolerance}) {
      const bool measured = authority == StateAuthorityClass::kMeasured;
      const bool finished_within = result == ExecutionGoalResult::kSucceeded;
      SCOPED_TRACE(std::string(measured ? "measured" : "commanded") + " " + to_string(result));
      Reset();
      Build(Config(authority));
      std::atomic<int> step{0};
      std::atomic<bool> held_until_finished{true};
      tick = [&] {
        const auto key = SubmittedKey();
        if (!key) return;
        ++step;
        pclock.advance(100'000'000);
        if (step == 1) Executing(*key);
        if (step <= 6) {
          // Path time over and every joint at the target, but the controller
          // still runs the goal: within tolerance, not finished.
          Sample(4.0, 2.0, 0.0);
          if (step >= 2 &&
              participant->get_execution_snapshot().state != ExecutionState::kExecuting) {
            held_until_finished = false;
          }
        } else if (step == 7) {
          port->FinishGoal(*key, result, std::nullopt, pclock.now());
        } else {
          Sample(4.0, 2.0, 0.0);  // the measured lane's post-result sample
        }
      };
      if (measured) Sample(0.0, 0.0, 0.0);  // the measured start state
      ASSERT_TRUE(Start(PortRequest("port_arrival")).ok());
      ASSERT_TRUE(wait_for_ended(*pkernel, finished_within ? "completed" : "failed"))
          << ended_as(*pkernel) << " " << Reason();
      EXPECT_TRUE(held_until_finished);
      EXPECT_GE(step.load(), 7);
      if (finished_within) {
        EXPECT_EQ(Reason(), "none");
        EXPECT_EQ(participant->get_execution_snapshot().joint_positions_rad,
                  (std::vector<double>{2.0, 2.0}));
      } else {
        EXPECT_EQ(Reason(), "controller_result_failed");
        EXPECT_EQ(pkernel->get_execution_snapshot().error->details.at("goal_result"),
                  to_string(result));
      }
      EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
    }
  }
}

TEST_F(PortParticipantKernelTest, TrackingBreachCancelsTheGoalWithUncertainEffects) {
  for (const bool tracked : {true, false}) {
    SCOPED_TRACE(tracked ? "isaac/real lane" : "offline lane");
    Reset();
    auto config = Config(StateAuthorityClass::kCommanded);
    if (tracked) config.tracking = ExecutionTrackingPolicy{0.05, 0.2};
    Build(config);
    std::atomic<int> step{0};
    tick = [&] {
      const auto key = SubmittedKey();
      if (!key) return;
      ++step;
      pclock.advance(100'000'000);
      if (step == 1) Executing(*key);
      Sample(0.1 * step, 0.05 * step, 0.5, 1.0, step >= 2 ? 0.1 : 0.0);
      if (!tracked && step == 8) {
        port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
      }
    };
    ASSERT_TRUE(Start(PortRequest("port_tracking")).ok());
    if (tracked) {
      ASSERT_TRUE(wait_for_ended(*pkernel, "failed_effect_uncertain")) << ended_as(*pkernel);
      EXPECT_EQ(Reason(), "tracking_error_exceeded");
      const auto details = pkernel->get_execution_snapshot().error->details;
      std::string dump;
      for (const auto& [key, value] : details) dump += key + "=" + value + " ";
      ASSERT_TRUE(details.contains("joint_name")) << dump;
      EXPECT_EQ(details.at("joint_name"), "joint_1");
      EXPECT_EQ(details.at("tracking_error_tolerance_rad"), std::to_string(0.05));
      EXPECT_EQ(Calls<testing::FakeExecutionPort::CancelCall>().size(), 1U);
      const auto fact = pkernel->get_status().fact;
      ASSERT_TRUE(fact);
      EXPECT_EQ(fact->kind, ParticipantExecutionFactKind::kLocalFailure);
      EXPECT_EQ(fact->effect, ParticipantEffectKnowledge::kEffectUncertain);
      EXPECT_EQ(fact->error, pkernel->get_execution_snapshot().error);
      EXPECT_EQ(participant->get_execution_snapshot().state, ExecutionState::kInterventionRequired);
      const auto key = SubmittedKey();
      ASSERT_TRUE(key);
      const auto goal = port->ReadGoal(*key);
      ASSERT_TRUE(goal);
      EXPECT_EQ(goal->phase, ExecutionGoalPhase::kExecuting);
      EXPECT_FALSE(goal->cancel_ack_local_receipt_stamp);
      EXPECT_FALSE(goal->result);
    } else {
      // Offline: no feedback, no check (item 13).
      ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << ended_as(*pkernel) << " " << Reason();
      EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
    }
  }
}

TEST_F(PortParticipantKernelTest, ControllerResultsAndBindingLossMapAsMain) {
  struct Case {
    std::optional<ExecutionGoalResult> result;
    std::string reason;
  };
  for (const auto& test :
       {Case{ExecutionGoalResult::kAbortedGoalTolerance, "controller_result_failed"},
        Case{ExecutionGoalResult::kAbortedPathTolerance, "controller_result_failed"},
        Case{ExecutionGoalResult::kCanceled, "backend_stopped_without_addressed_stop"},
        Case{ExecutionGoalResult::kEndedUnknown, "controller_result_failed"},
        Case{std::nullopt, "controller_binding_lost"}}) {
    SCOPED_TRACE(test.reason);
    Reset();
    Build(Config(StateAuthorityClass::kCommanded));
    std::atomic<int> step{0};
    tick = [&] {
      const auto key = SubmittedKey();
      if (!key) return;
      ++step;
      if (step == 1) Executing(*key);
      if (step != 3) return;
      if (test.result) {
        port->FinishGoal(*key, *test.result, std::nullopt, pclock.now());
      } else {
        port->SetReadiness({false, true, true, ExecutionBindingEpoch(1U)});
      }
    };
    ASSERT_TRUE(Start(PortRequest("port_result")).ok());
    const bool uncertain = !test.result || *test.result == ExecutionGoalResult::kEndedUnknown;
    ASSERT_TRUE(wait_for_ended(*pkernel, uncertain ? "failed_effect_uncertain" : "failed"))
        << ended_as(*pkernel);
    const auto error = pkernel->get_execution_snapshot().error;
    ASSERT_TRUE(error.has_value());
    const auto reason = error->details.contains("reason_code") ? error->details.at("reason_code")
                                                               : error->details.at("reason");
    EXPECT_EQ(reason, test.reason);
    const auto fact = pkernel->get_status().fact;
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->kind, ParticipantExecutionFactKind::kLocalFailure);
    EXPECT_EQ(fact->effect, uncertain ? ParticipantEffectKnowledge::kEffectUncertain
                                      : ParticipantEffectKnowledge::kEffectSettled);
    EXPECT_EQ(fact->error, error);
    if (test.result == ExecutionGoalResult::kEndedUnknown) {
      EXPECT_EQ(error->details.at("goal_result"), "ended_unknown");
      EXPECT_EQ(participant->get_execution_snapshot().state, ExecutionState::kInterventionRequired);
    }
    EXPECT_EQ(Calls<testing::FakeExecutionPort::CancelCall>().size(),
              test.result.has_value() ? 0U : 1U);
  }
}

TEST_F(PortParticipantKernelTest, MissingGoalReportsUncertainEffectsWithoutAnotherCommand) {
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) Executing(*key);
    if (step != 3) return;
    // Simulate a lost goal observation using the existing bounded-record fake.
    // The participant never observes the original terminal result.
    port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
    for (unsigned index = 0; index < kExecutionGoalRecordCapacity; ++index) {
      ExecutionGoalObservationDTO retained;
      retained.key = {ControllerGoalId("later.goal." + std::to_string(index)),
                      ControllerGoalGeneration(100U + index)};
      retained.binding_epoch = port->ReadReadiness().binding_epoch;
      retained.phase = ExecutionGoalPhase::kTerminal;
      retained.result = ExecutionGoalResult::kSucceeded;
      port->SetGoal(std::move(retained));
    }
  };
  ASSERT_TRUE(Start(PortRequest("port_missing_goal")).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "failed_effect_uncertain")) << ended_as(*pkernel);
  EXPECT_EQ(Reason(), "controller_result_failed");
  const auto error = pkernel->get_execution_snapshot().error;
  ASSERT_TRUE(error);
  EXPECT_EQ(error->details.at("goal_result"), "unavailable");
  const auto fact = pkernel->get_status().fact;
  ASSERT_TRUE(fact);
  EXPECT_EQ(fact->kind, ParticipantExecutionFactKind::kLocalFailure);
  EXPECT_EQ(fact->effect, ParticipantEffectKnowledge::kEffectUncertain);
  EXPECT_EQ(fact->error, error);
  EXPECT_EQ(participant->get_execution_snapshot().state, ExecutionState::kInterventionRequired);
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
  EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::ReplaceCall>().empty());
  const auto key = SubmittedKey();
  ASSERT_TRUE(key);
  EXPECT_FALSE(port->ReadGoal(*key));
}

TEST_F(PortParticipantKernelTest, RunningStaleStateRemainsUncertainRegardlessOfCancelReceipt) {
  for (const auto dispatch :
       {ExecutionPortDispatch::kDispatched, ExecutionPortDispatch::kEffectUncertain,
        ExecutionPortDispatch::kNotDispatched}) {
    SCOPED_TRACE(static_cast<int>(dispatch));
    Reset();
    auto config = Config(StateAuthorityClass::kCommanded);
    config.stop.controller_state_max_age_ns = 50'000'000;
    Build(config);
    std::atomic<int> step{0};
    tick = [&] {
      const auto key = SubmittedKey();
      if (!key) return;
      ++step;
      pclock.advance(100'000'000);
      if (step == 1) {
        Executing(*key);
        Sample(0.1, 0.05, 0.5);
      } else if (step == 2) {
        port->QueueCommandResult({dispatch,
                                  dispatch == ExecutionPortDispatch::kNotDispatched
                                      ? std::optional(ExecutionPortRefusal::kNotReady)
                                      : std::nullopt,
                                  std::nullopt});
      }
      // No cancel acknowledgment, terminal result or newer sample is supplied.
    };
    Sample(0.0, 0.0, 0.0);
    ASSERT_TRUE(Start(PortRequest("port_stale_cancel_receipt")).ok());
    ASSERT_TRUE(wait_for_ended(*pkernel, "failed_effect_uncertain")) << ended_as(*pkernel);
    EXPECT_EQ(Reason(), "stale_controller_state");
    const auto fact = pkernel->get_status().fact;
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->kind, ParticipantExecutionFactKind::kLocalFailure);
    EXPECT_EQ(fact->effect, ParticipantEffectKnowledge::kEffectUncertain);
    EXPECT_EQ(fact->error, pkernel->get_execution_snapshot().error);
    EXPECT_EQ(participant->get_execution_snapshot().state, ExecutionState::kInterventionRequired);
    EXPECT_EQ(Calls<testing::FakeExecutionPort::CancelCall>().size(), 1U);
    const auto key = SubmittedKey();
    ASSERT_TRUE(key);
    const auto goal = port->ReadGoal(*key);
    ASSERT_TRUE(goal);
    EXPECT_EQ(goal->phase, ExecutionGoalPhase::kExecuting);
    EXPECT_FALSE(goal->cancel_ack_local_receipt_stamp);
    EXPECT_FALSE(goal->result);
  }
}

TEST_F(PortParticipantKernelTest, ExecutionTimeoutRunsOnTheAppliedFactorFromSamples) {
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) Executing(*key);
    pclock.advance(1'000'000'000);
    // Item 40: elapsed x applied factor (0.5) -> 2 s of budget per 4 s.
    Sample(0.5 * step, 0.25 * step, 0.5, 0.5);
    if (!Calls<testing::FakeExecutionPort::CancelCall>().empty()) {
      port->FinishGoal(*key, ExecutionGoalResult::kCanceled, std::nullopt, pclock.now());
    }
  };
  auto request = PortRequest("port_timeout");
  request.timeouts.execution_timeout_s = 2.0;
  ASSERT_TRUE(Start(request).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "failed")) << ended_as(*pkernel);
  const auto error = pkernel->get_execution_snapshot().error;
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, kErrorExecutionTimeout);
  // The hard-stop primitive is `main`'s goal cancel.
  EXPECT_EQ(Calls<testing::FakeExecutionPort::CancelCall>().size(), 1U);
  EXPECT_GE(step, 4);
  EXPECT_LE(step, 6);
}

TEST_F(PortParticipantKernelTest, ZeroSpeedHoldTimeoutAsksTheAddressedOwnerToStop) {
  Build(Config(StateAuthorityClass::kCommanded));
  std::mutex events_mu;
  std::vector<KernelEvent> stop_requests;
  pkernel->subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kStopRequested) return;
    std::lock_guard lock(events_mu);
    stop_requests.push_back(event);
  });
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) Executing(*key);
    pclock.advance(1'000'000'000);
    Sample(1.0, 0.5, 0.0, step >= 6 ? 1.0 : 0.0);  // the slider holds at zero
    if (step >= 8) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  };
  auto request = PortRequest("port_zero_hold");
  request.timeouts.execution_timeout_s = 5.0;
  request.timeouts.zero_speed_hold_timeout_s = 3.0;
  ASSERT_TRUE(Start(request).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << ended_as(*pkernel) << " " << Reason();
  std::lock_guard lock(events_mu);
  ASSERT_EQ(stop_requests.size(), 1U);
  EXPECT_EQ(stop_requests.front().details.at("reason"), "zero_speed_hold_timeout");
}

TEST_F(PortParticipantKernelTest, SpeedRampSlewsLinearlyOneValuePerTickAndReadsTheAppliedFactorBack) {
  // Item 28 / Q11: linear slew at the trajectory's max_rate_per_s (at most
  // 4/s), one value per controller-rate tick; no rate-change limit applies.
  struct Case {
    double max_rate_per_s;
    double max_rate_change_per_s2;
    double step;
  };
  for (const auto& test : {Case{4.0, 16.0, 0.04}, Case{2.0, 0.5, 0.02}, Case{8.0, 1.0, 0.04}}) {
    SCOPED_TRACE(test.max_rate_per_s);
    Reset();
    Build(Config(StateAuthorityClass::kCommanded));
    auto trajectory = PortTrajectory();
    trajectory.runtime_scale_bounds =
        TrajectoryRuntimeScaleBoundsDTO{test.max_rate_per_s, test.max_rate_change_per_s2};
    planner.next_result.trajectory = trajectory;
    tick = [&] {
      const auto key = SubmittedKey();
      if (!key) return;
      Executing(*key);
      Sample(0.5, 0.25, 0.5, 0.75);
    };
    ASSERT_TRUE(Start(PortRequest("port_ramp")).ok());
    for (int attempt = 0; attempt < 2000 && receipts == 0U; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    participant->set_runtime_velocity_scale(0.5);
    for (int step = 0; step < 100; ++step) {
      pclock.advance(10'000'000);
      pscheduler.advance_to(pclock.now());
    }
    const auto scales = Calls<testing::FakeExecutionPort::SetScaleCall>();
    ASSERT_GE(scales.size(), 2U);
    double previous = 1.0;
    for (std::size_t index = 0U; index + 1U < scales.size(); ++index) {
      EXPECT_NEAR(previous - scales[index].value, test.step, 1e-9) << index;
      previous = scales[index].value;
    }
    EXPECT_NEAR(scales.back().value, 0.5, 1e-12);
    EXPECT_LE(previous - scales.back().value, test.step + 1e-9);
    // Published is not applied: the applied factor is the sample's.
    const auto status = participant->runtime_velocity_scale_status();
    EXPECT_TRUE(status.supported);
    EXPECT_DOUBLE_EQ(status.commanded_scale, 0.5);
    EXPECT_DOUBLE_EQ(status.applied_scale, 0.75);
    EXPECT_NE(status.acknowledgement_sequence, 0U);
  }
}

TEST_F(PortParticipantKernelTest, TimingObservationKeepsLatenessApartFromTelemetryLoss) {
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) Executing(*key);
    ExecutionTimingDTO timing;
    timing.binding_epoch = ExecutionBindingEpoch(1U);
    timing.period_max_s = step == 2 ? 0.014 : 0.01;  // late, within the 15 ms margin
    timing.max_state_stamp_gap_s = step == 3 ? 0.3 : 0.01;  // telemetry loss
    timing.received_samples = 1U;
    port->SetTiming(timing);
    if (step == 6) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  };
  ASSERT_TRUE(Start(PortRequest("port_timing")).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << Reason();
  const auto metrics = participant->timing_metrics();
  EXPECT_EQ(metrics.windows_late_beyond_margin, 0U);
  EXPECT_DOUBLE_EQ(metrics.max_period_s, 0.014);
  EXPECT_DOUBLE_EQ(metrics.max_state_stamp_gap_s, 0.3);
  EXPECT_GT(metrics.windows, 0U);
  // Q50 "measure": the dispatched goal's recognition delay (dispatch to its
  // own first feedback, local clock).
  EXPECT_EQ(metrics.goals_recognized, 1U);
  EXPECT_GE(metrics.last_recognition_delay_ns, 0);
  EXPECT_EQ(metrics.max_recognition_delay_ns, metrics.last_recognition_delay_ns);
}

TEST_F(PortParticipantKernelTest, LateControlCycleBeyondTheMarginIsTheItem17Path) {
  // Q52: a controller cycle later than the lane margin is handled like item
  // 17: the Kernel issues the lane's hard stop (the goal cancel) and the
  // motion ends InterventionRequired with `late_control_cycle`.
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) Executing(*key);
    pclock.advance(10'000'000);
    Sample(0.01 * step, 0.005 * step, 0.5);
    ExecutionTimingDTO timing;
    timing.binding_epoch = ExecutionBindingEpoch(1U);
    timing.period_max_s = step == 3 ? 0.016 : 0.01;  // one cycle beyond the 15 ms margin
    port->SetTiming(timing);
  };
  ASSERT_TRUE(Start(PortRequest("port_late_cycle")).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "failed_effect_uncertain")) << ended_as(*pkernel);
  const auto status = pkernel->get_status();
  ASSERT_TRUE(status.fact.has_value() && status.fact->error.has_value());
  EXPECT_EQ(status.fact->error->details.at("reason"), "late_control_cycle");
  EXPECT_EQ(status.fact->error->details.at("late_cycle_margin_s"), std::to_string(0.015));
  EXPECT_EQ(Calls<testing::FakeExecutionPort::CancelCall>().size(), 1U);
  EXPECT_EQ(participant->get_execution_snapshot().state, ExecutionState::kInterventionRequired);
  EXPECT_EQ(participant->timing_metrics().windows_late_beyond_margin, 1U);
}

TEST_F(PortParticipantKernelTest, HostReportedMechanismFaultIsTheItem17Path) {
  // A host-reported mechanism fault ends in-flight motion through the Kernel's
  // hard stop, not as a controller-binding loss.
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) Executing(*key);
    pclock.advance(10'000'000);
    Sample(0.01 * step, 0.005 * step, 0.5);
    if (step == 3) {
      participant->ReportCriticalFault(ErrorDTO{
          "internal_error", "Controller mechanism fault",
          {{"reason", "controller_mechanism_fault"}}});
      // The controller deactivation the host requests next is observed later.
      ExecutionReadinessDTO inactive;
      inactive.binding_epoch = ExecutionBindingEpoch(1U);
      port->SetReadiness(inactive);
    }
  };
  ASSERT_TRUE(Start(PortRequest("port_guard_trip")).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "failed_effect_uncertain")) << ended_as(*pkernel);
  const auto status = pkernel->get_status();
  ASSERT_TRUE(status.fact.has_value() && status.fact->error.has_value());
  EXPECT_EQ(status.fact->error->details.at("reason"),
            "controller_mechanism_fault");
  EXPECT_EQ(participant->get_execution_snapshot().state, ExecutionState::kInterventionRequired);
  EXPECT_FALSE(participant->GoalInFlight() &&
               participant->get_execution_snapshot().state == ExecutionState::kExecuting);
  // Without motion in flight a report changes nothing.
  participant->ReportCriticalFault(ErrorDTO{"internal_error", "late", {}});
  EXPECT_EQ(participant->get_execution_snapshot().error->details.at("reason"),
            "controller_mechanism_fault");
}

TEST_F(PortParticipantKernelTest, SoftStopUnderTheAddressedFenceEndsTheExecutionStopped) {
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<bool> stopping{false};
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    if (!stopping) {
      ++step;
      if (step == 1) Executing(*key);
      Sample(1.0, 0.5, 0.5);
      return;
    }
    const auto replaces = Calls<testing::FakeExecutionPort::ReplaceCall>();
    if (replaces.empty()) return;
    const auto next = replaces.back().next_key;
    const auto goal = port->ReadGoal(next);
    if (goal && goal->phase == ExecutionGoalPhase::kPending) {
      Executing(next);
      port->FinishGoal(*key, ExecutionGoalResult::kPreemptedBy, next, pclock.now());
      Sample(0.01, 0.51, 0.3);
    } else if (goal && goal->phase != ExecutionGoalPhase::kTerminal) {
      pclock.advance(10'000'000);
      port->FinishGoal(next, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
      pclock.advance(1);
      Sample(0.0, 0.6, 0.0);
    } else {
      pclock.advance(1);
      Sample(0.0, 0.6, 0.0);
    }
  };
  const auto accepted = Start(PortRequest("port_soft_stop"));
  ASSERT_TRUE(accepted.ok());
  for (int attempt = 0; attempt < 2000 && step < 3; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(pkernel->fence_addressed_participant_stop(accepted.value()),
            AddressedParticipantStopFenceDisposition::kDispatchCommitted);
  const auto stage = StageFor(accepted.value(), 0U);
  stopping = true;
  const auto result = participant->TranslateStopStage(stage, BudgetFor(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->details.at("reason") : "");
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSoftOnPath);
  ASSERT_EQ(Calls<testing::FakeExecutionPort::ReplaceCall>().size(), 1U);
  ASSERT_TRUE(wait_for_ended(*pkernel, "preempted_by_addressed_stop")) << ended_as(*pkernel);
  EXPECT_EQ(participant->get_execution_snapshot().state, ExecutionState::kStopped);
  // §3.3: the soft Stop's end on the original path, mapped by the planner,
  // is retained for continuation (ahead of the 1.0 s trip cursor).
  const auto observed = pkernel->ObservePreparedMotion(accepted.value());
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().stopped_trajectory_time_s.has_value());
  EXPECT_GT(*observed.value().stopped_trajectory_time_s, 1.0);
  EXPECT_LE(*observed.value().stopped_trajectory_time_s, 4.0);
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
}

TEST_F(PortParticipantKernelTest, HardStopStageUnderTheFenceBrakesThroughTheHelper) {
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<bool> stopping{false};
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    if (!stopping) {
      ++step;
      if (step == 1) Executing(*key);
      Sample(1.0, 0.5, 0.5);
      return;
    }
    const auto replaces = Calls<testing::FakeExecutionPort::ReplaceCall>();
    if (replaces.empty()) return;
    const auto next = replaces.back().next_key;
    const auto goal = port->ReadGoal(next);
    pclock.advance(5'000'000);
    if (goal && goal->phase == ExecutionGoalPhase::kPending) {
      Executing(next);
      port->FinishGoal(*key, ExecutionGoalResult::kPreemptedBy, next, pclock.now());
      Sample(0.004, 0.55, 0.2);  // the brake's own reference, never zero
    } else if (goal && goal->phase != ExecutionGoalPhase::kTerminal) {
      port->FinishGoal(next, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
    } else {
      Sample(0.0, BrakeEnd(), 0.0);  // JTC success hold at the brake's end
    }
  };
  const auto accepted = Start(PortRequest("port_hard_stop"));
  ASSERT_TRUE(accepted.ok());
  for (int attempt = 0; attempt < 2000 && step < 3; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(pkernel->fence_addressed_participant_stop(accepted.value()),
            AddressedParticipantStopFenceDisposition::kDispatchCommitted);
  const auto stage = StageFor(accepted.value(), 1U);
  stopping = true;
  const auto result = participant->TranslateStopStage(stage, BudgetFor(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->details.at("reason") : "");
  const auto& evidence = std::get<JsonObjectDTO>(result.evidence_items.front().evidence->value);
  EXPECT_EQ(std::get<std::string>(evidence.at("hard_stop_level").value), "controlled_hard_stop");
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
  ASSERT_TRUE(wait_for_ended(*pkernel, "preempted_by_addressed_stop")) << ended_as(*pkernel);
}

TEST_F(PortParticipantKernelTest, RebindingTakesANewPortForTheNewEpochOnlyWhenQuiescent) {
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) Executing(*key);
    if (step == 4) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  };
  const auto first = Start(PortRequest("port_epoch_1"));
  ASSERT_TRUE(first.ok());
  auto next_port = std::make_shared<testing::FakeExecutionPort>();
  next_port->SetLaneFacts(port->ReadLaneFacts());
  next_port->SetReadiness({true, true, true, ExecutionBindingEpoch(2U)});
  if (ended_as(*pkernel) != "completed") {
    const auto busy = participant->RebindPort(next_port);
    ASSERT_FALSE(busy.ok());
    EXPECT_EQ(busy.error().details.at("reason"), "execution_port_rebind_busy");
  }
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << Reason();
  ASSERT_TRUE(pkernel->acknowledge_completed(first.value()).accepted);
  ASSERT_TRUE(participant->RebindPort(next_port).ok());

  // The next motion runs on the new epoch's port; the old port sees nothing.
  const auto old_commands = port->commands().size();
  const auto old_port = port;
  {
    std::lock_guard lock(hook_mutex);
    port = next_port;
    step = 0;
  }
  ASSERT_TRUE(Start(PortRequest("port_epoch_2")).ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << Reason();
  EXPECT_EQ(old_port->commands().size(), old_commands);
  EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
}

TEST_F(PortParticipantKernelTest, RebindKeepsTheOldPreparedMembershipUntilItsOwnerRetiresIt) {
  // A new binding epoch's port never receives an old prepared goal: its exact
  // replay returns the old handle, and once its owner retired it nothing of
  // it dispatches again.
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) Executing(*key);
    if (step == 3) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  };
  const auto accepted = Start(PortRequest("port_membership"));
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << Reason();
  const auto observed = pkernel->ObservePreparedMotion(accepted.value());
  ASSERT_TRUE(observed.ok() && observed.value().last_dispatched_participant);
  const auto dispatched = *observed.value().last_dispatched_participant;
  const auto key = SubmittedKey().value();

  auto next_port = std::make_shared<testing::FakeExecutionPort>();
  next_port->SetLaneFacts(port->ReadLaneFacts());
  next_port->SetReadiness({true, true, true, ExecutionBindingEpoch(2U)});
  ASSERT_TRUE(participant->RebindPort(next_port).ok());
  const auto replay =
      participant->Dispatch(dispatched, key.controller_goal_id, key.controller_goal_generation);
  EXPECT_NE(replay.execution_generation, 0U);
  EXPECT_TRUE(next_port->commands().empty());

  ASSERT_TRUE(participant->RetirePreparedExecution(accepted.value()).ok());
  const auto after = participant->Dispatch(dispatched, ControllerGoalId("goal.after.retire"),
                                           ControllerGoalGeneration(1U));
  EXPECT_EQ(after.execution_generation, 0U);
  EXPECT_EQ(after.get_snapshot().error->details.at("reason"), "prepared_participant_not_admitted");
  EXPECT_TRUE(next_port->commands().empty());
}

TEST_F(PortParticipantKernelTest, PrepareRequiresTheTrajectorysScaleBoundsAndDispatchIsReplaySafe) {
  Build(Config(StateAuthorityClass::kCommanded));
  auto unbounded = PortTrajectory();
  unbounded.runtime_scale_bounds.reset();
  planner.next_result.trajectory = unbounded;
  (void)prepare_and_dispatch(*pkernel, PortRequest("port_unbounded"));
  ASSERT_TRUE(wait_for_ended(*pkernel, "failed")) << ended_as(*pkernel);
  const auto status = pkernel->get_status();
  ASSERT_TRUE(status.fact.has_value() && status.fact->error.has_value());
  EXPECT_EQ(status.fact->error->details.at("reason"), "prepared_runtime_scale_bounds_missing");
  EXPECT_EQ(status.fact->effect, ParticipantEffectKnowledge::kNoEffect);
  EXPECT_TRUE(port->commands().empty());
}


TEST_F(PortParticipantKernelTest, DispatchReplayReturnsTheSameHandleAndRefusesAnotherGeneration) {
  Build(Config(StateAuthorityClass::kCommanded));
  std::atomic<bool> finish{false};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    Executing(*key);
    if (finish) port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  };
  const auto accepted = Start(PortRequest("port_replay"));
  ASSERT_TRUE(accepted.ok());
  const auto observed = pkernel->ObservePreparedMotion(accepted.value());
  ASSERT_TRUE(observed.ok() && observed.value().last_dispatched_participant);
  const auto& dispatched = *observed.value().last_dispatched_participant;
  const auto key = SubmittedKey().value();
  // Exact repetition: the same goal-bound handle, no second goal.
  const auto again = participant->Dispatch(dispatched, key.controller_goal_id,
                                           key.controller_goal_generation);
  EXPECT_NE(again.execution_generation, 0U);
  EXPECT_FALSE(prepared_execution_handle_field_error(again, dispatched, key.controller_goal_id,
                                                     key.controller_goal_generation)
                   .has_value());
  // Another generation of the same goal is a conflict, refused before effect.
  const auto conflict = participant->Dispatch(
      dispatched, key.controller_goal_id,
      ControllerGoalGeneration(key.controller_goal_generation.value() + 1U));
  EXPECT_EQ(conflict.execution_generation, 0U);
  EXPECT_EQ(conflict.get_snapshot().error->details.at("reason"),
            "controller_goal_identity_conflict");
  EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
  finish = true;
  ASSERT_TRUE(wait_for_ended(*pkernel, "completed")) << Reason();
}

TEST_F(PortParticipantKernelTest, StopIntentWinsWhileTheMeasuredCompletionGateIsPending) {
  Build(Config(StateAuthorityClass::kMeasured));
  std::atomic<bool> stopping{false};
  std::atomic<int> step{0};
  tick = [&] {
    const auto key = SubmittedKey();
    if (!key) return;
    ++step;
    if (step == 1) {
      Executing(*key);
      Sample(3.9, 1.95, 0.1);
    } else if (step == 2) {
      // The successful result; the measured gate waits for a later sample.
      port->FinishGoal(*key, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
    } else if (stopping) {
      pclock.advance(1'000'000);
      Sample(4.0, 2.0, 0.0);  // the post-result sample arrives under the Stop
    }
  };
  Sample(0.0, 0.0, 0.0);  // the measured start state (start-state check)
  const auto accepted = Start(PortRequest("port_gate_stop"));
  ASSERT_TRUE(accepted.ok());
  for (int attempt = 0; attempt < 2000 && step < 4; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(ended_as(*pkernel), "working:Executing");
  ASSERT_EQ(pkernel->fence_addressed_participant_stop(accepted.value()),
            AddressedParticipantStopFenceDisposition::kDispatchCommitted);
  const auto stage = StageFor(accepted.value(), 0U);
  stopping = true;
  const auto result = participant->TranslateStopStage(stage, BudgetFor(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->details.at("reason") : "");
  // The goal had already completed: natural completion inside the Stop.
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::ReplaceCall>().empty());
  ASSERT_TRUE(wait_for_ended(*pkernel, "preempted_by_addressed_stop")) << ended_as(*pkernel);
  const auto observed = pkernel->ObservePreparedMotion(accepted.value());
  ASSERT_TRUE(observed.ok());
  EXPECT_EQ(observed.value().stopped_trajectory_time_s, 4.0);
}

// Real port-participant handover tests. The sealed product originates in the
// existing Kernel fixture; all callback ordering below is explicitly scripted.
TEST_F(PortParticipantKernelTest,
       PendingReplacementCannotCompleteOldHandleOrLoseFaultCancellation) {
  const auto product = DirectBackendProduct();
  Sample(0.0, 0.0, 0.0);
  const ExecutionGoalKey old{ControllerGoalId("direct.old"), ControllerGoalGeneration(1U)};
  const auto handle =
      participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
  ASSERT_NE(handle.execution_generation, 0U);
  pclock.advance(10'000'000);
  Executing(old);
  Sample(0.5, 0.25, 0.5);
  EXPECT_FALSE(handle.wait_for_completion(0));
  const auto source = participant->CaptureReplacementSource(1U, handle.execution_generation, old,
                                                            product.prepared_contract_digest);
  ASSERT_TRUE(source.ok()) << source.error().message;
  const auto successor = DirectSuccessor(product, source.value());
  const auto prepared = participant->PrepareReplacement(successor, 2U, source.value());
  ASSERT_EQ(prepared.state, PrepareParticipantResultStateDTO::kPrepared)
      << (prepared.error ? prepared.error->message : "no preparation error")
      << (prepared.error ? ::testing::PrintToString(prepared.error->details) : "");
  const ExecutionGoalKey next{ControllerGoalId("direct.next"), ControllerGoalGeneration(2U)};
  const auto sent = participant->DispatchReplacement(source.value(), 2U, successor, next,
                                                     pclock.now() + 1'000'000'000);
  ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(sent.handle);
  // The original goal was a plain Submit; one replacement followed.
  const auto motion_replaces = Calls<testing::FakeExecutionPort::ReplaceCall>();
  ASSERT_EQ(motion_replaces.size(), 1U);
  // A successful predecessor result does not contain a still pending newcomer.
  port->FinishGoal(old, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  EXPECT_FALSE(handle.wait_for_completion(0));
  EXPECT_FALSE(sent.handle->wait_for_completion(0));
  EXPECT_EQ(handle.get_snapshot().state, ExecutionState::kExecuting);
  EXPECT_FALSE(participant->RetirePreparedExecution(2U).ok());
  // Losing readiness while both possible effects are retained cancels newest
  // first. No bare Cancel receipt is promoted to settlement.
  auto readiness = port->ReadReadiness();
  readiness.binding_epoch = ExecutionBindingEpoch(readiness.binding_epoch.value() + 1U);
  port->SetReadiness(readiness);
  EXPECT_TRUE(handle.wait_for_completion(0));
  EXPECT_EQ(handle.get_snapshot().state, ExecutionState::kInterventionRequired);
  const auto cancels = Calls<testing::FakeExecutionPort::CancelCall>();
  // After binding loss neither old-epoch key establishes current containment.
  ASSERT_EQ(cancels.size(), 2U);
  EXPECT_EQ(cancels.front().key, next);
  EXPECT_EQ(cancels.back().key, old);
}

TEST_F(PortParticipantKernelTest, ReplacementSourceCapturesOnlyScheduledControllerPeriod) {
  struct Lane { double rate; bool scheduled; std::int64_t expected_ns; };
  for (const auto lane : {Lane{100.0, true, 10'000'000},
                          Lane{150.0, true, 6'666'666}, Lane{100.0, false, 0}}) {
    SCOPED_TRACE(::testing::PrintToString(std::make_pair(lane.rate, lane.scheduled)));
    Reset();
    auto facts = port->ReadLaneFacts();
    facts.controller_update_rate_hz = lane.rate;
    facts.supports_scheduled_replacement = lane.scheduled;
    port->SetLaneFacts(facts);
    const auto product = DirectBackendProduct();
    Sample(0.0, 0.0, 0.0);
    const ExecutionGoalKey goal{ControllerGoalId("period.capture"), ControllerGoalGeneration(1U)};
    const auto handle = participant->Dispatch(product, goal.controller_goal_id,
                                               goal.controller_goal_generation);
    pclock.advance(10'000'000);
    Executing(goal);
    Sample(0.5, 0.25, 0.5);
    EXPECT_FALSE(handle.wait_for_completion(0));
    const auto source = participant->CaptureReplacementSource(
        1U, handle.execution_generation, goal, product.prepared_contract_digest);
    ASSERT_TRUE(source.ok()) << source.error().message;
    EXPECT_EQ(source.value().controller_period_ns, lane.expected_ns);
    EXPECT_DOUBLE_EQ(source.value().controller_sample.cursor_s, 0.5);
    EXPECT_EQ(source.value().controller_sample.local_receipt_stamp_ns, pclock.now());
  }
}

// A source capture pairs the last published progress with its actual sample,
// even when the port has already received a newer sample between normal ticks.
TEST_F(PortParticipantKernelTest,
       ReplacementSourceKeepsMatchedSampleUntilOrdinaryProgressTick) {
  const auto product = DirectBackendProduct();
  Sample(0.0, 0.0, 0.0);
  const ExecutionGoalKey goal{ControllerGoalId("sample.capture"),
                              ControllerGoalGeneration(1U)};
  const auto handle =
      participant->Dispatch(product, goal.controller_goal_id, goal.controller_goal_generation);
  ASSERT_NE(handle.execution_generation, 0U);
  pclock.advance(10'000'000);
  Executing(goal);
  Sample(0.5, 0.25, 0.5);
  // Dispatch's initial cursor publication has no associated controller pair.
  EXPECT_FALSE(participant->CaptureReplacementSource(
      1U, handle.execution_generation, goal, product.prepared_contract_digest).ok());
  ASSERT_FALSE(handle.wait_for_completion(0));  // Ordinary participant tick.
  const auto initial = participant->CaptureReplacementSource(
      1U, handle.execution_generation, goal, product.prepared_contract_digest);
  ASSERT_TRUE(initial.ok()) << initial.error().message;
  ASSERT_TRUE(initial.value().progress.valid);
  EXPECT_FALSE(initial.value().progress.terminal);

  // Same active goal, epoch and preparation; only a new controller sample
  // arrives. The progress publication still names the preceding tick's sample.
  pclock.advance(10'000'000);
  Sample(0.51, 0.255, 0.5);
  const auto latest = port->ReadSample();
  ASSERT_TRUE(latest);
  const auto observed = port->ReadGoal(goal);
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed->key, goal);
  EXPECT_EQ(observed->phase, ExecutionGoalPhase::kExecuting);
  EXPECT_EQ(observed->binding_epoch, initial.value().binding_epoch);
  EXPECT_EQ(latest->binding_epoch, initial.value().binding_epoch);
  EXPECT_EQ(latest->local_receipt_stamp_ns, pclock.now());
  const auto before = participant->execution_progress_publication()->acquire();
  EXPECT_EQ(before.source_timestamp_ns, initial.value().progress.source_timestamp_ns);
  EXPECT_EQ(before.received_timestamp_ns, initial.value().progress.received_timestamp_ns);
  EXPECT_LT(before.source_timestamp_ns, latest->source_stamp_ns);
  EXPECT_LT(before.received_timestamp_ns, latest->local_receipt_stamp_ns);

  const auto between_ticks = participant->CaptureReplacementSource(
      1U, handle.execution_generation, goal, product.prepared_contract_digest);
  ASSERT_TRUE(between_ticks.ok()) << between_ticks.error().message;
  EXPECT_EQ(between_ticks.value().controller_sample, initial.value().controller_sample);
  EXPECT_EQ(between_ticks.value().progress.sequence, before.sequence);
  EXPECT_EQ(between_ticks.value().progress.source_timestamp_ns, before.source_timestamp_ns);
  EXPECT_EQ(between_ticks.value().progress.received_timestamp_ns, before.received_timestamp_ns);
  EXPECT_EQ(participant->execution_progress_publication()->acquire().sequence, before.sequence);

  // Only the ordinary tick publishes the next matched sample/progress pair.
  ASSERT_FALSE(handle.wait_for_completion(0));
  const auto after_tick = participant->CaptureReplacementSource(
      1U, handle.execution_generation, goal, product.prepared_contract_digest);
  ASSERT_TRUE(after_tick.ok()) << after_tick.error().message;
  EXPECT_EQ(after_tick.value().execution_generation, initial.value().execution_generation);
  EXPECT_EQ(after_tick.value().goal, goal);
  EXPECT_EQ(after_tick.value().prepared_contract_digest, product.prepared_contract_digest);
  EXPECT_EQ(after_tick.value().controller_sample.local_receipt_count, latest->local_receipt_count);
  EXPECT_EQ(after_tick.value().progress.source_timestamp_ns, latest->source_stamp_ns);
  EXPECT_EQ(after_tick.value().progress.received_timestamp_ns, latest->local_receipt_stamp_ns);
  EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::ReplaceCall>().empty());
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
}

TEST_F(PortParticipantKernelTest, ReplacementSourceAppliesFreshnessToItsRetainedPair) {
  const auto product = DirectBackendProduct();
  pkernel.reset();
  participant.reset();
  auto config = Config(StateAuthorityClass::kCommanded);
  config.progress_freshness_budget_ns = 100'000'000;
  Build(config);
  ASSERT_EQ(participant->Prepare(product, 1U).state,
            PrepareParticipantResultStateDTO::kPrepared);
  Sample(0.0, 0.0, 0.0);
  const ExecutionGoalKey goal{ControllerGoalId("pair.freshness"), ControllerGoalGeneration(1U)};
  const auto handle =
      participant->Dispatch(product, goal.controller_goal_id, goal.controller_goal_generation);
  ASSERT_NE(handle.execution_generation, 0U);
  pclock.advance(10'000'000);
  Executing(goal);
  Sample(0.5, 0.25, 0.5);
  ASSERT_FALSE(handle.wait_for_completion(0));
  ASSERT_TRUE(participant->CaptureReplacementSource(
      1U, handle.execution_generation, goal, product.prepared_contract_digest).ok());

  pclock.advance(100'000'001);
  Sample(0.6, 0.3, 0.5);  // New port data cannot make the retained pair fresh.
  const auto expired = participant->CaptureReplacementSource(
      1U, handle.execution_generation, goal, product.prepared_contract_digest);
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().details.at("reason"), "motion_replacement_source_unavailable");
  ASSERT_FALSE(handle.wait_for_completion(0));
  const auto fresh = participant->CaptureReplacementSource(
      1U, handle.execution_generation, goal, product.prepared_contract_digest);
  ASSERT_TRUE(fresh.ok()) << fresh.error().message;
  EXPECT_EQ(fresh.value().progress.received_timestamp_ns, pclock.now());
  EXPECT_EQ(fresh.value().controller_sample.local_receipt_stamp_ns, pclock.now());
}

TEST_F(PortParticipantKernelTest,
       ReplacementSourceNeverReusesPairAfterProgressAuthorityRejectsPublication) {
  for (const int boundary : {0, 1, 2, 3}) {
    SCOPED_TRACE(boundary);
    const auto product = DirectBackendProduct();
    Sample(0.0, 0.0, 0.0);
    const ExecutionGoalKey goal{ControllerGoalId("pair.guard." + std::to_string(boundary)),
                                ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, goal.controller_goal_id, goal.controller_goal_generation);
    ASSERT_NE(handle.execution_generation, 0U);
    pclock.advance(10'000'000);
    Executing(goal);
    Sample(0.5, 0.25, 0.5);
    ASSERT_FALSE(handle.wait_for_completion(0));
    const auto initial = participant->CaptureReplacementSource(
        1U, handle.execution_generation, goal, product.prepared_contract_digest);
    ASSERT_TRUE(initial.ok());
    pclock.advance(10'000'000);
    if (boundary == 0) {
      ASSERT_EQ(participant->claim_execution_progress_hazard(handle.execution_generation),
                ExecutionProgressHazardClaimResult::kClaimed);
    } else if (boundary == 1) {
      participant->retire_execution_progress_generation(handle.execution_generation);
    } else if (boundary == 2) {
      port->FinishGoal(goal, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
    }
    const double cursor = boundary == 3 ? 0.4 : 0.6;
    Sample(cursor, cursor * 0.5, 0.5);
    static_cast<void>(handle.wait_for_completion(0));
    const auto refused = participant->CaptureReplacementSource(
        1U, handle.execution_generation, goal, product.prepared_contract_digest);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"), "motion_replacement_source_unavailable");
    if (boundary == 0 || boundary == 3) {
      EXPECT_EQ(participant->execution_progress_publication()->acquire().sequence,
                initial.value().progress.sequence);
    }
    if (boundary == 3) {
      // A failed regressing publication cleared the cache rather than serving
      // the old pair. Only a later valid ordinary publication restores it.
      pclock.advance(10'000'000);
      Sample(0.6, 0.3, 0.5);
      ASSERT_FALSE(handle.wait_for_completion(0));
      ASSERT_TRUE(participant->CaptureReplacementSource(
          1U, handle.execution_generation, goal, product.prepared_contract_digest).ok());
    }
    EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
    EXPECT_TRUE(Calls<testing::FakeExecutionPort::ReplaceCall>().empty());
    EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
    Reset();
  }
}

TEST_F(PortParticipantKernelTest, RejectedReplacementIsNoEffectAndCannotBeAdopted) {
  const auto product = DirectBackendProduct();
  Sample(0.0, 0.0, 0.0);
  const ExecutionGoalKey old{ControllerGoalId("reject.old"), ControllerGoalGeneration(1U)};
  const auto handle =
      participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
  pclock.advance(10'000'000);
  Executing(old);
  Sample(0.5, 0.25, 0.5);
  EXPECT_FALSE(handle.wait_for_completion(0));
  const auto source = participant->CaptureReplacementSource(1U, handle.execution_generation, old,
                                                            product.prepared_contract_digest);
  ASSERT_TRUE(source.ok());
  const auto successor = DirectSuccessor(product, source.value());
  const auto prepared = participant->PrepareReplacement(successor, 2U, source.value());
  ASSERT_EQ(prepared.state, PrepareParticipantResultStateDTO::kPrepared)
      << (prepared.error ? prepared.error->message : "no preparation error")
      << (prepared.error ? ::testing::PrintToString(prepared.error->details) : "");
  const ExecutionGoalKey next{ControllerGoalId("reject.next"), ControllerGoalGeneration(2U)};
  ASSERT_EQ(
      participant
          ->DispatchReplacement(source.value(), 2U, successor, next, pclock.now() + 1'000'000'000)
          .command.dispatch,
      ExecutionPortDispatch::kDispatched);
  port->FinishGoal(next, ExecutionGoalResult::kRejected, std::nullopt, pclock.now());
  const auto observed = participant->ObserveReplacement(source.value(), 2U);
  ASSERT_TRUE(observed.ok());
  EXPECT_EQ(observed.value().phase, PreparedMotionReplacementPhaseDTO::kNoEffect);
  int commits = 0;
  const PreparedMotionReplacementCommitDTO commit{&commits, [](void* context) noexcept {
                                                    ++*static_cast<int*>(context);
                                                    return Result<void>::Ok();
                                                  }};
  EXPECT_FALSE(participant->AdoptReplacement(source.value(), 2U, commit).ok());
  EXPECT_EQ(commits, 0);
  EXPECT_TRUE(participant->RetirePreparedExecution(2U).ok());
  // Candidate cleanup preserves the source preparation and original handle.
  EXPECT_EQ(participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation)
                .execution_generation,
            handle.execution_generation);
  EXPECT_EQ(Calls<testing::FakeExecutionPort::SubmitCall>().size(), 1U);
}

TEST_F(PortParticipantKernelTest, RestartedControllerCursorRefusesTheTakeoverWithoutASend) {
  // D8: between the controller's end hold (cursor back near 0) and the terminal
  // result, a takeover joined at that cursor would command a move back toward
  // the source start. It is refused before any send (the Kernel then runs the
  // configured Stop fallback).
  const auto product = DirectBackendProduct();
  Sample(0.0, 0.0, 0.0);
  const ExecutionGoalKey old{ControllerGoalId("restart.old"), ControllerGoalGeneration(1U)};
  const auto handle =
      participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
  pclock.advance(10'000'000);
  Executing(old);
  Sample(0.5, 0.25, 0.5);
  EXPECT_FALSE(handle.wait_for_completion(0));
  const auto source = participant->CaptureReplacementSource(1U, handle.execution_generation, old,
                                                            product.prepared_contract_digest);
  ASSERT_TRUE(source.ok());
  const auto successor = DirectSuccessor(product, source.value());
  ASSERT_EQ(participant->PrepareReplacement(successor, 2U, source.value()).state,
            PrepareParticipantResultStateDTO::kPrepared);
  pclock.advance(1'000'000);
  Sample(0.0, 0.0, 0.0);  // restarted cursor, goal not yet terminal
  const ExecutionGoalKey next{ControllerGoalId("restart.next"), ControllerGoalGeneration(2U)};
  const auto sent = participant->DispatchReplacement(source.value(), 2U, successor, next,
                                                     pclock.now() + 1'000'000'000);
  EXPECT_EQ(sent.command.dispatch, ExecutionPortDispatch::kNotDispatched);
  ASSERT_TRUE(sent.command.error);
  EXPECT_EQ(sent.command.error->details.at("reason"),
            "motion_replacement_scale_or_source_unqualified");
  EXPECT_EQ(sent.command.error->details.at("qualification_stage"), "baseline");
  EXPECT_EQ(sent.command.error->details.at("qualification_check"), "cursor_restart");
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::ReplaceCall>().empty());
}

TEST_F(PortParticipantKernelTest,
       ScheduledMovingReplacementPairsCanonicalOriginAcrossSuccessiveGoals) {
  for (const double factor : {1.0, 0.5, 0.0}) {
    SCOPED_TRACE(factor);
    auto lane = port->ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    port->SetLaneFacts(lane);
    const auto product = DirectBackendProduct();
    // Establish a flat factor before dispatch. This test does not qualify
    // phase prediction during an evolving ramp or an actual RT installation.
    participant->set_runtime_velocity_scale(factor);
    Sample(0.0, 0.0, 0.0, factor);
    const ExecutionGoalKey old{ControllerGoalId("scheduled.old"), ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    ASSERT_NE(handle.execution_generation, 0U);
    pclock.advance(10'000'000);
    Executing(old);
    Sample(0.5, 0.25, 0.5, factor);
    EXPECT_FALSE(handle.wait_for_completion(0));
    pclock.advance(5'000'000);
    const auto source = participant->CaptureReplacementSource(
        1U, handle.execution_generation, old, product.prepared_contract_digest);
    ASSERT_TRUE(source.ok());
    const auto successor = DirectSuccessor(product, source.value());
    ASSERT_EQ(participant->PrepareReplacement(successor, 2U, source.value()).state,
              PrepareParticipantResultStateDTO::kPrepared);
    const ExecutionGoalKey next{ControllerGoalId("scheduled.next"), ControllerGoalGeneration(2U)};
    const auto scales_before = Calls<testing::FakeExecutionPort::SetScaleCall>().size();
    const auto sent = participant->DispatchReplacement(
        source.value(), 2U, successor, next, pclock.now() + 1'000'000'000);
    ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
    ASSERT_TRUE(sent.handle);
    auto replacements = Calls<testing::FakeExecutionPort::ReplaceCall>();
    ASSERT_EQ(replacements.size(), 1U);
    ASSERT_TRUE(replacements.front().start);
    EXPECT_EQ(replacements.front().start->binding_epoch, source.value().binding_epoch);
    EXPECT_EQ(replacements.front().start->controller_start_stamp_ns,
              source.value().controller_sample.source_stamp_ns + 30'000'000);
    const double origin = 0.5 + factor * 0.03;
    EXPECT_NEAR(replacements.front().trajectory.waypoints.front().time_from_start_s, -origin, 1e-12);
    const auto before = sample_trajectory(std::get<PreparedMotionExecutionDTO>(product.payload).trajectory,
                                         origin);
    const auto after = sample_trajectory(replacements.front().trajectory, 0.0);
    ASSERT_TRUE(before.ok() && after.ok());
    for (std::size_t joint = 0; joint < 2U; ++joint) {
      EXPECT_NEAR(before.value().positions_rad[joint], after.value().positions_rad[joint], 1e-12);
      EXPECT_NEAR(before.value().velocities_rad_s[joint], after.value().velocities_rad_s[joint], 1e-11);
      EXPECT_NEAR(before.value().accelerations_rad_s2[joint], after.value().accelerations_rad_s2[joint],
                  1e-10);
    }
    // Replay has no second physical send, factor write, or new freeze policy.
    const auto replay = participant->DispatchReplacement(source.value(), 2U, successor, next, 0);
    EXPECT_EQ(replay.submitted_at_ns, sent.submitted_at_ns);
    EXPECT_EQ(Calls<testing::FakeExecutionPort::ReplaceCall>().size(), 1U);
    EXPECT_EQ(Calls<testing::FakeExecutionPort::SetScaleCall>().size(), scales_before);
    EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
    if (factor == 0.0) {
      // Existing moving admission permits zero; pairing does not add a new
      // refusal. Q45 controlled Stop remains separately cancel-only.
      Reset();
      continue;
    }
    port->FinishGoal(old, ExecutionGoalResult::kPreemptedBy, next, pclock.now());
    pclock.advance(30'000'000);
    Executing(next);
    Sample(0.1, 0.3 + 0.5 * factor * 0.03, 0.5, factor);
    ASSERT_EQ(participant->ObserveReplacement(source.value(), 2U).value().phase,
              PreparedMotionReplacementPhaseDTO::kReadyToCommit);
    ASSERT_TRUE(participant->AdoptReplacement(
        source.value(), 2U, {nullptr, [](void*) noexcept { return Result<void>::Ok(); }}).ok());
    ASSERT_TRUE(participant->RetirePreparedExecution(1U).ok());
    const auto source2 = participant->CaptureReplacementSource(
        2U, sent.handle->execution_generation, next, successor.prepared_contract_digest);
    ASSERT_TRUE(source2.ok());
    ASSERT_NEAR(static_cast<double>(source2.value().progress.trajectory_time_ns) / 1e9,
                0.1 + factor * 0.03, 1e-9);
    EXPECT_DOUBLE_EQ(source2.value().controller_sample.cursor_s, 0.1);
    const auto third_product = DirectSuccessor(successor, source2.value());
    ASSERT_EQ(participant->PrepareReplacement(third_product, 3U, source2.value()).state,
              PrepareParticipantResultStateDTO::kPrepared);
    const ExecutionGoalKey third{ControllerGoalId("scheduled.third"), ControllerGoalGeneration(3U)};
    const auto sent2 = participant->DispatchReplacement(
        source2.value(), 3U, third_product, third, pclock.now() + 1'000'000'000);
    ASSERT_EQ(sent2.command.dispatch, ExecutionPortDispatch::kDispatched);
    replacements = Calls<testing::FakeExecutionPort::ReplaceCall>();
    ASSERT_EQ(replacements.size(), 2U);
    ASSERT_TRUE(replacements.back().start);
    EXPECT_EQ(replacements.back().start->controller_start_stamp_ns,
              source2.value().controller_sample.source_stamp_ns + 20'000'000);
    // Rebase in the prepared product's canonical clock, not the raw 0.1s
    // controller cursor left by the first replacement.
    EXPECT_NEAR(replacements.back().trajectory.waypoints.front().time_from_start_s,
                -(0.1 + factor * 0.03 + factor * 0.02), 1e-9);
    Reset();
  }
}

TEST_F(PortParticipantKernelTest, ExperimentFlagsEachChangeOnlyTheirPartOfMovingReplacement) {
  // EXPERIMENTAL independent flags on the moving takeover: the timer adds only
  // a hold until T - P/2 - latency, mid-tick only the stamp phase (with D for
  // it), V0 only the prefix. T stays the existing whole-period start
  // (5 ms + 20 ms -> 30 ms). The hold runs before the final source checks.
  struct Case {
    bool timer, midtick, v0;
  };
  for (const auto flags : {Case{true, false, false}, Case{false, true, false},
                           Case{false, false, true}, Case{true, true, true}}) {
    SCOPED_TRACE(::testing::Message() << "timer=" << flags.timer << " midtick=" << flags.midtick
                                      << " v0=" << flags.v0);
    auto lane = port->ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    lane.experiment.release_timer = flags.timer;
    lane.experiment.midtick_stamp = flags.midtick;
    lane.experiment.v0_payload = flags.v0;
    port->SetLaneFacts(lane);
    const auto product = DirectBackendProduct();
    participant->set_runtime_velocity_scale(1.0);
    Sample(0.0, 0.0, 0.0, 1.0);
    const ExecutionGoalKey old{ControllerGoalId("flags.old"), ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    ASSERT_NE(handle.execution_generation, 0U);
    pclock.advance(10'000'000);
    Executing(old);
    Sample(0.5, 0.25, 0.5, 1.0);
    EXPECT_FALSE(handle.wait_for_completion(0));
    pclock.advance(5'000'000);
    const auto source = participant->CaptureReplacementSource(
        1U, handle.execution_generation, old, product.prepared_contract_digest);
    ASSERT_TRUE(source.ok());
    const auto successor = DirectSuccessor(product, source.value());
    ASSERT_EQ(participant->PrepareReplacement(successor, 2U, source.value()).state,
              PrepareParticipantResultStateDTO::kPrepared);
    const ExecutionGoalKey next{ControllerGoalId("flags.next"), ControllerGoalGeneration(2U)};
    const auto join_started = pclock.now();
    TimePoint unsent_until = 0;
    {
      std::lock_guard lock(hook_mutex);
      tick = [&] {  // the participant's waiter: 1 ms per wait while held
        if (Calls<testing::FakeExecutionPort::ReplaceCall>().empty()) unsent_until = pclock.now();
        pclock.advance(1'000'000);
      };
    }
    const auto sent = participant->DispatchReplacement(source.value(), 2U, successor, next,
                                                       pclock.now() + 1'000'000'000);
    {
      std::lock_guard lock(hook_mutex);
      tick = nullptr;
    }
    ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
    const auto replacements = Calls<testing::FakeExecutionPort::ReplaceCall>();
    ASSERT_EQ(replacements.size(), 1U);
    ASSERT_TRUE(replacements.front().start);
    const auto& start = *replacements.front().start;
    ASSERT_TRUE(start.experiment);
    const auto& sample = source.value().controller_sample;
    EXPECT_EQ(start.experiment->target_tick_ns, sample.source_stamp_ns + 30'000'000);
    const auto advance_ns = start.controller_start_stamp_ns - sample.source_stamp_ns;
    EXPECT_EQ(advance_ns, flags.midtick ? 35'000'000 : 30'000'000);
    EXPECT_EQ(start.experiment->release_local_ns.has_value(), flags.timer);
    if (flags.timer) {
      const auto release = sample.local_receipt_stamp_ns + 30'000'000 - 5'000'000 - 1'500'000;
      EXPECT_EQ(*start.experiment->release_local_ns, release);
      EXPECT_EQ(start.experiment->release_hold, ExecutionReleaseHold::kHeld);
      // Held on the injected waiter until the release, then sent.
      EXPECT_GE(unsent_until + 1'000'000, release);
      EXPECT_GE(*sent.submitted_at_ns, release);
      EXPECT_LE(*sent.submitted_at_ns - join_started, 20'000'000);
    } else {
      EXPECT_EQ(start.experiment->release_hold, ExecutionReleaseHold::kNone);
      EXPECT_EQ(*sent.submitted_at_ns, join_started);
    }
    EXPECT_EQ(start.experiment->v0_payload, flags.v0);
    const double origin = 0.5 + static_cast<double>(advance_ns) / 1e9;
    const auto& points = replacements.front().trajectory.waypoints;
    EXPECT_NEAR(points.front().time_from_start_s, flags.v0 ? 0.0 : -origin, 1e-12);
    const auto before =
        sample_trajectory(std::get<PreparedMotionExecutionDTO>(product.payload).trajectory, origin);
    const auto after = sample_trajectory(replacements.front().trajectory, 0.0);
    ASSERT_TRUE(before.ok() && after.ok());
    for (std::size_t joint = 0; joint < 2U; ++joint) {
      EXPECT_NEAR(before.value().positions_rad[joint], after.value().positions_rad[joint], 1e-12);
      EXPECT_NEAR(before.value().velocities_rad_s[joint], after.value().velocities_rad_s[joint],
                  1e-11);
    }
    Reset();
  }
}

TEST_F(PortParticipantKernelTest, ReleaseTimerChargesGridRoundingButPreservesOtherGuards) {
  enum class Mode { kTimer, kFlagsOff, kTimerOff, kDivergence, kPreparationDeadline };
  for (const auto mode : {Mode::kTimer, Mode::kFlagsOff, Mode::kTimerOff, Mode::kDivergence,
                          Mode::kPreparationDeadline}) {
    SCOPED_TRACE(static_cast<int>(mode));
    auto lane = port->ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    lane.controller_update_rate_hz = 150.0;
    lane.experiment.release_timer = mode != Mode::kFlagsOff && mode != Mode::kTimerOff;
    lane.experiment.midtick_stamp = mode != Mode::kFlagsOff;
    lane.experiment.v0_payload = mode != Mode::kFlagsOff;
    port->SetLaneFacts(lane);
    const auto product = DirectBackendProduct();
    participant->set_runtime_velocity_scale(1.0);
    Sample(0.0, 0.0, 0.0, 1.0);
    const ExecutionGoalKey old{ControllerGoalId("grid.old"), ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    pclock.advance(10'000'000);
    Executing(old);
    Sample(0.5, 0.25, 0.5, 1.0);
    EXPECT_FALSE(handle.wait_for_completion(0));
    pclock.advance(1'000'000);
    const auto source = participant->CaptureReplacementSource(
        1U, handle.execution_generation, old, product.prepared_contract_digest);
    ASSERT_TRUE(source.ok());
    auto successor = DirectSuccessor(product, source.value());
    if (mode == Mode::kDivergence)
      std::get<PreparedMotionExecutionDTO>(successor.payload)
          .plan_request.moving_replacement->divergence_cursor_s = 0.54;
    ASSERT_EQ(participant->PrepareReplacement(successor, 2U, source.value()).state,
              PrepareParticipantResultStateDTO::kPrepared);
    const auto join_started = pclock.now();
    int waits = 0;
    {
      std::lock_guard lock(hook_mutex);
      tick = [&] {
        ++waits;
        pclock.advance(1'000'000);
      };
    }
    int reads = 0;
    if (mode == Mode::kFlagsOff || mode == Mode::kTimerOff ||
        mode == Mode::kPreparationDeadline)
      port->SetOnReadSample([&](auto&) {
        if (++reads == 2) pclock.advance(21'000'000);
      });
    const ExecutionGoalKey next{ControllerGoalId("grid.next"), ControllerGoalGeneration(2U)};
    const auto sent = participant->DispatchReplacement(
        source.value(), 2U, successor, next,
        join_started + (mode == Mode::kPreparationDeadline ? 20'000'000 : 1'000'000'000));
    port->SetOnReadSample(nullptr);
    {
      std::lock_guard lock(hook_mutex);
      tick = nullptr;
    }
    const auto replacements = Calls<testing::FakeExecutionPort::ReplaceCall>();
    if (mode == Mode::kTimer) {
      ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
      ASSERT_EQ(replacements.size(), 1U);
      const auto& experiment = *replacements.front().start->experiment;
      // w=1 ms, A=20 ms: r=5.666664 ms > P/2+L=4.833333 ms.
      // The 20.833331 ms release used to be labeled deadline and sent early.
      EXPECT_EQ(experiment.release_hold, ExecutionReleaseHold::kHeld);
      EXPECT_EQ(*experiment.release_local_ns - join_started, 20'833'331);
      EXPECT_EQ(experiment.start_allowance_ns, 20'000'000);
      EXPECT_GE(*sent.submitted_at_ns, *experiment.release_local_ns);
      EXPECT_LE(*sent.submitted_at_ns - join_started, 26'666'666);
      EXPECT_GT(waits, 0);
    } else {
      EXPECT_EQ(sent.command.dispatch, ExecutionPortDispatch::kNotDispatched);
      ASSERT_TRUE(sent.command.error);
      EXPECT_EQ(sent.command.error->details.at("reason"),
                mode == Mode::kDivergence ? "motion_replacement_delivery_lead_expired" :
                mode == Mode::kPreparationDeadline ? "motion_replacement_preparation_deadline_expired" :
                                                    "motion_replacement_join_origin_expired");
      EXPECT_TRUE(replacements.empty());
      if (mode == Mode::kDivergence) EXPECT_GT(waits, 0);
      else EXPECT_EQ(waits, 0);
    }
    Reset();
  }
}

TEST_F(PortParticipantKernelTest, ReleaseHoldNeverCrossesADeadlineAndRechecksReadinessAfter) {
  // A release after the preparation deadline is not held (sent at once, same
  // refusal criteria); readiness lost during a hold is refused by the
  // existing final source check, before any send.
  for (const bool readiness_lost : {false, true}) {
    SCOPED_TRACE(readiness_lost ? "readiness lost during hold" : "deadline before release");
    auto lane = port->ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    lane.experiment.release_timer = true;
    port->SetLaneFacts(lane);
    const auto product = DirectBackendProduct();
    participant->set_runtime_velocity_scale(1.0);
    Sample(0.0, 0.0, 0.0, 1.0);
    const ExecutionGoalKey old{ControllerGoalId("hold.old"), ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    pclock.advance(10'000'000);
    Executing(old);
    Sample(0.5, 0.25, 0.5, 1.0);
    EXPECT_FALSE(handle.wait_for_completion(0));
    pclock.advance(5'000'000);
    const auto source = participant->CaptureReplacementSource(
        1U, handle.execution_generation, old, product.prepared_contract_digest);
    ASSERT_TRUE(source.ok());
    const auto successor = DirectSuccessor(product, source.value());
    ASSERT_EQ(participant->PrepareReplacement(successor, 2U, source.value()).state,
              PrepareParticipantResultStateDTO::kPrepared);
    const ExecutionGoalKey next{ControllerGoalId("hold.next"), ControllerGoalGeneration(2U)};
    const auto ready = port->ReadReadiness();
    {
      std::lock_guard lock(hook_mutex);
      tick = [&] {
        pclock.advance(1'000'000);
        if (readiness_lost) port->SetReadiness({false, false, false, ready.binding_epoch});
      };
    }
    // Release is 18.5 ms after the join; a 10 ms preparation deadline is earlier.
    const auto sent = participant->DispatchReplacement(
        source.value(), 2U, successor, next,
        pclock.now() + (readiness_lost ? 1'000'000'000 : 10'000'000));
    {
      std::lock_guard lock(hook_mutex);
      tick = nullptr;
    }
    const auto replacements = Calls<testing::FakeExecutionPort::ReplaceCall>();
    if (readiness_lost) {
      EXPECT_EQ(sent.command.dispatch, ExecutionPortDispatch::kNotDispatched);
      ASSERT_TRUE(sent.command.error);
      EXPECT_EQ(sent.command.error->details.at("qualification_stage"), "final");
      EXPECT_EQ(sent.command.error->details.at("qualification_check"), "readiness");
      EXPECT_TRUE(replacements.empty());
      port->SetReadiness(ready);
    } else {
      ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
      ASSERT_EQ(replacements.size(), 1U);
      EXPECT_EQ(replacements.front().start->experiment->release_hold,
                ExecutionReleaseHold::kDeadline);
      EXPECT_EQ(replacements.front().start->controller_start_stamp_ns,
                source.value().controller_sample.source_stamp_ns + 30'000'000);
    }
    Reset();
  }
}

TEST_F(PortParticipantKernelTest, ReleaseHoldWaitsWithoutTheParticipantLockAndRerunsTheDecision) {
  // SX-3475: the moving takeover waits for its release WITHOUT the participant
  // mutex, then reruns the existing dispatch decision. The waiter is paced 1:1
  // with real time (1 ms fake per 1 ms sleep) so lock delays are measurable.
  enum class Race { kStop, kRetired, kDuplicate, kOtherSuccessor, kOversleep };
  for (const auto race : {Race::kStop, Race::kRetired, Race::kDuplicate, Race::kOtherSuccessor,
                          Race::kOversleep}) {
    SCOPED_TRACE(static_cast<int>(race));
    auto lane = port->ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    lane.experiment.release_timer = true;
    port->SetLaneFacts(lane);
    const auto product = DirectBackendProduct();
    participant->set_runtime_velocity_scale(1.0);
    Sample(0.0, 0.0, 0.0, 1.0);
    const ExecutionGoalKey old{ControllerGoalId("race.old"), ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    pclock.advance(10'000'000);
    Executing(old);
    Sample(0.5, 0.25, 0.5, 1.0);
    EXPECT_FALSE(handle.wait_for_completion(0));
    pclock.advance(5'000'000);
    const auto source = participant->CaptureReplacementSource(
        1U, handle.execution_generation, old, product.prepared_contract_digest);
    ASSERT_TRUE(source.ok());
    const auto successor = DirectSuccessor(product, source.value());
    ASSERT_EQ(participant->PrepareReplacement(successor, 2U, source.value()).state,
              PrepareParticipantResultStateDTO::kPrepared);
    const ExecutionGoalKey next{ControllerGoalId("race.next"), ControllerGoalGeneration(2U)};
    const ExecutionGoalKey other{ControllerGoalId("race.other"), ControllerGoalGeneration(3U)};
    const auto join_started = pclock.now();
    const auto deadline = pclock.now() + 1'000'000'000;
    std::atomic<int> waits{0};
    {
      std::lock_guard lock(hook_mutex);
      tick = [&] {
        ++waits;
        // Oversleep: past the timer's 20 ms allowance plus one 10 ms period.
        pclock.advance(race == Race::kOversleep && waits == 1 ? 35'000'000 : 1'000'000);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      };
    }
    PreparedMotionReplacementDispatchDTO sent;
    std::thread dispatcher([&] {
      sent = participant->DispatchReplacement(source.value(), 2U, successor, next, deadline);
    });
    while (waits == 0) std::this_thread::yield();
    std::optional<PreparedMotionReplacementDispatchDTO> duplicate;
    double entry_ms = 0.0;
    const auto entered = std::chrono::steady_clock::now();
    if (race == Race::kStop) {
      // A Stop during the hold is accepted at once (cancelled right after its
      // entry so this test ends); the takeover then refuses on `stopping`.
      std::stop_source cancel;
      cancel.request_stop();
      const auto stage = StageForProduct(product, old, 0U);
      (void)participant->TranslateStopStage(stage, BudgetFor(stage), cancel.get_token());
    } else if (race == Race::kRetired) {
      EXPECT_TRUE(participant->RetirePreparedExecution(2U).ok());
    } else if (race == Race::kDuplicate) {
      duplicate = participant->DispatchReplacement(source.value(), 2U, successor, next, deadline);
    } else if (race == Race::kOtherSuccessor) {
      duplicate = participant->DispatchReplacement(source.value(), 2U, successor, other, deadline);
    }
    entry_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         entered)
                   .count();
    dispatcher.join();
    {
      std::lock_guard lock(hook_mutex);
      tick = nullptr;
    }
    const auto replacements = Calls<testing::FakeExecutionPort::ReplaceCall>();
    const auto reason = [](const PreparedMotionReplacementDispatchDTO& result) {
      return result.command.error ? result.command.error->details.at("reason") : std::string{};
    };
    switch (race) {
      case Race::kStop:
        std::printf("manyforge_release_hold_stop_entry_ms=%.3f hold_ms=18.5\n", entry_ms);
        RecordProperty("stop_entry_ms", std::to_string(entry_ms));
        EXPECT_LT(entry_ms, 10.0);  // not delayed by the 18.5 ms hold
        EXPECT_EQ(sent.command.dispatch, ExecutionPortDispatch::kNotDispatched);
        EXPECT_EQ(reason(sent), "motion_replacement_source_changed");
        EXPECT_TRUE(replacements.empty());
        break;
      case Race::kRetired:
        EXPECT_EQ(sent.command.dispatch, ExecutionPortDispatch::kNotDispatched);
        EXPECT_EQ(reason(sent), "motion_replacement_membership_changed");
        EXPECT_TRUE(replacements.empty());
        break;
      case Race::kDuplicate: {
        // Exactly one physical send; the other attempt replays its record.
        ASSERT_TRUE(duplicate);
        ASSERT_EQ(replacements.size(), 1U);
        EXPECT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
        EXPECT_EQ(duplicate->command.dispatch, ExecutionPortDispatch::kDispatched);
        EXPECT_EQ(sent.submitted_at_ns, duplicate->submitted_at_ns);
        // The sent payload pairs with the sent stamp (same attempt's origin).
        const auto& start = *replacements.front().start;
        const double origin =
            0.5 + static_cast<double>(start.controller_start_stamp_ns -
                                      source.value().controller_sample.source_stamp_ns) / 1e9;
        EXPECT_NEAR(replacements.front().trajectory.waypoints.front().time_from_start_s, -origin,
                    1e-12);
        EXPECT_EQ(start.experiment->release_hold, ExecutionReleaseHold::kHeld);
        break;
      }
      case Race::kOtherSuccessor: {
        // Two attempts with different successors: one physical send, and the
        // sent goal, the candidate key and the recorded keys are the SAME
        // attempt's; the other attempt is the existing goal conflict.
        ASSERT_TRUE(duplicate);
        ASSERT_EQ(replacements.size(), 1U);
        const auto& winner_key = replacements.front().next_key;
        ASSERT_TRUE(winner_key == next || winner_key == other);
        const auto& winner = winner_key == next ? sent : *duplicate;
        const auto& loser = winner_key == next ? *duplicate : sent;
        EXPECT_EQ(winner.command.dispatch, ExecutionPortDispatch::kDispatched);
        EXPECT_EQ(loser.command.dispatch, ExecutionPortDispatch::kNotDispatched);
        EXPECT_EQ(reason(loser), "motion_replacement_goal_conflict");
        const auto observed = participant->ObserveReplacement(source.value(), 2U);
        ASSERT_TRUE(observed.ok());
        ASSERT_TRUE(observed.value().successor);  // read through the candidate's own key
        EXPECT_EQ(observed.value().successor->key, winner_key);
        // A replay names the recorded goal: the winner replays, the loser conflicts.
        EXPECT_EQ(participant->DispatchReplacement(source.value(), 2U, successor, winner_key, 0)
                      .submitted_at_ns,
                  winner.submitted_at_ns);
        EXPECT_EQ(reason(participant->DispatchReplacement(
                      source.value(), 2U, successor, winner_key == next ? other : next, 0)),
                  "motion_replacement_goal_conflict");
        break;
      }
      case Race::kOversleep:
        // The existing allowance check charges the oversleep: refused, unsent.
        EXPECT_EQ(sent.command.dispatch, ExecutionPortDispatch::kNotDispatched);
        EXPECT_EQ(reason(sent), "motion_replacement_join_origin_expired");
        EXPECT_TRUE(replacements.empty());
        EXPECT_GT(pclock.now() - join_started, 30'000'000);
        break;
    }
    Reset();
  }
}

TEST_F(PortParticipantKernelTest, ReplacementDeadlineAndInheritedRampBoundsRefuseBeforeSdkSend) {
  for (const auto& [expired, scheduled] : {std::pair{false, false}, std::pair{true, false},
                                          std::pair{false, true}, std::pair{true, true}}) {
    SCOPED_TRACE(::testing::Message() << "expired=" << expired << ", scheduled=" << scheduled);
    auto lane = port->ReadLaneFacts();
    lane.supports_scheduled_replacement = scheduled;
    port->SetLaneFacts(lane);
    const auto product = DirectBackendProduct();
    Sample(0.0, 0.0, 0.0);
    const ExecutionGoalKey old{ControllerGoalId("bounds.old"), ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    participant->set_runtime_velocity_scale(0.5);
    pclock.advance(10'000'000);
    pscheduler.advance_to(pclock.now());
    Executing(old);
    Sample(0.5, 0.25, 0.5, 0.96);
    EXPECT_FALSE(handle.wait_for_completion(0));
    const auto source = participant->CaptureReplacementSource(1U, handle.execution_generation, old,
                                                              product.prepared_contract_digest);
    ASSERT_TRUE(source.ok());
    auto successor = DirectSuccessor(product, source.value());
    if (!expired) {
      // Ordinary bounds permit R=4; a successor admitting R=2 cannot inherit
      // that live slew by silently clamping it at ownership adoption.
      std::get<PreparedMotionExecutionDTO>(successor.payload)
          .trajectory.runtime_scale_bounds->max_rate_per_s = 2.0;
    }
    const auto prepared = participant->PrepareReplacement(successor, 2U, source.value());
    ASSERT_EQ(prepared.state, PrepareParticipantResultStateDTO::kPrepared)
        << (prepared.error ? prepared.error->message : "no preparation error")
        << (prepared.error ? ::testing::PrintToString(prepared.error->details) : "");
    const ExecutionGoalKey next{ControllerGoalId("bounds.next"), ControllerGoalGeneration(2U)};
    int reads = 0;
    if (expired)
      port->SetOnReadSample([&](auto&) {
        if (++reads == 2) pclock.advance(2'000'000);
      });
    const auto sent = participant->DispatchReplacement(
        source.value(), 2U, successor, next, pclock.now() + (expired ? 1'000'000 : 1'000'000'000));
    port->SetOnReadSample(nullptr);
    EXPECT_EQ(sent.command.dispatch, ExecutionPortDispatch::kNotDispatched);
    if (!expired) {
      ASSERT_TRUE(sent.command.error);
      const auto& details = sent.command.error->details;
      EXPECT_EQ(details.at("qualification_stage"), "baseline");
      EXPECT_EQ(details.at("qualification_check"), "scale_compatibility");
      EXPECT_EQ(details.at("candidate_rate_per_s"), "2");
      EXPECT_EQ(details.at("ramp_rate_per_s"), "4");
      EXPECT_EQ(details.at("scale_frozen"), "false");
    }
    EXPECT_TRUE(Calls<testing::FakeExecutionPort::ReplaceCall>().empty());
    EXPECT_EQ(handle.get_snapshot().state, ExecutionState::kExecuting);
    EXPECT_TRUE(participant->RetirePreparedExecution(2U).ok());
    Reset();
  }
}

TEST_F(PortParticipantKernelTest, PreparationHoldDefersSliderThroughAdoptedConnectorThenRebinds) {
  const auto product = DirectBackendProduct();
  participant->set_runtime_velocity_scale(1.0);
  Sample(0.0, 0.0, 0.0);
  const ExecutionGoalKey old{ControllerGoalId("hold.old"), ControllerGoalGeneration(1U)};
  const auto handle =
      participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
  pclock.advance(10'000'000);
  Executing(old);
  Sample(0.5, 0.25, 0.5);
  ASSERT_FALSE(handle.wait_for_completion(0));
  const auto source = participant->CaptureReplacementSource(1U, handle.execution_generation, old,
                                                            product.prepared_contract_digest);
  ASSERT_TRUE(source.ok());
  ASSERT_TRUE(participant
                  ->BeginReplacementPreparationScaleHold(source.value(), 2U,
                                                         pclock.now() + 1'000'000'000, {})
                  .ok());
  const auto count = Calls<testing::FakeExecutionPort::SetScaleCall>().size();
  participant->set_runtime_velocity_scale(0.5);
  pclock.advance(10'000'000);
  pscheduler.advance_to(pclock.now());
  EXPECT_EQ(Calls<testing::FakeExecutionPort::SetScaleCall>().size(), count);
  auto next_product = DirectSuccessor(product, source.value());
  auto& motion = std::get<PreparedMotionExecutionDTO>(next_product.payload);
  const auto& original = std::get<PreparedMotionExecutionDTO>(product.payload).trajectory;
  const auto point = [&](double time) {
    const auto sample = sample_trajectory(original, 0.5 + time);
    if (!sample.ok()) throw std::logic_error(sample.error().message);
    return TrajectoryWaypointDTO{time, sample.value().positions_rad,
                                 sample.value().velocities_rad_s,
                                 sample.value().accelerations_rad_s2};
  };
  motion.plan_request.moving_replacement->source_prefix_trajectory.waypoints = {point(0.0),
                                                                                point(1.0)};
  motion.trajectory.waypoints = {point(0.0), point(1.0), point(1.25), point(3.5)};
  motion.trajectory.runtime_scale_bounds->max_rate_per_s = 2.0;
  ASSERT_EQ(participant->PrepareReplacement(next_product, 2U, source.value()).state,
            PrepareParticipantResultStateDTO::kPrepared);
  const ExecutionGoalKey next{ControllerGoalId("hold.next"), ControllerGoalGeneration(2U)};
  const auto sent = participant->DispatchReplacement(source.value(), 2U, next_product, next,
                                                     pclock.now() + 1'000'000'000);
  ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched)
      << (sent.command.error ? sent.command.error->message : "");
  ASSERT_TRUE(sent.handle);
  port->FinishGoal(old, ExecutionGoalResult::kPreemptedBy, next, pclock.now());
  pclock.advance(30'000'000);
  Executing(next);
  Sample(0.1, 0.3, 0.5);
  ASSERT_EQ(participant->ObserveReplacement(source.value(), 2U).value().phase,
            PreparedMotionReplacementPhaseDTO::kReadyToCommit);
  ASSERT_TRUE(participant
                  ->AdoptReplacement(source.value(), 2U,
                                     {nullptr, [](void*) noexcept { return Result<void>::Ok(); }})
                  .ok());
  pclock.advance(10'000'000);
  Sample(0.4, 0.45, 0.5);
  EXPECT_FALSE(sent.handle->wait_for_completion(0));
  pscheduler.advance_to(pclock.now());
  EXPECT_EQ(Calls<testing::FakeExecutionPort::SetScaleCall>().size(), count);
  pclock.advance(10'000'000);
  Sample(1.4, 0.95, 0.5);
  EXPECT_FALSE(sent.handle->wait_for_completion(0));
  pclock.advance(10'000'000);
  pscheduler.advance_to(pclock.now());
  const auto scales = Calls<testing::FakeExecutionPort::SetScaleCall>();
  ASSERT_GT(scales.size(), count);
  EXPECT_NEAR(scales.back().value, 0.98, 1e-12);
}

TEST_F(PortParticipantKernelTest, ScheduledMovingReplacementKeepsExistingEvolvingRampAdmission) {
  auto lane = port->ReadLaneFacts();
  lane.supports_scheduled_replacement = true;
  port->SetLaneFacts(lane);
  const auto product = DirectBackendProduct();
  Sample(0.0, 0.0, 0.0);
  const ExecutionGoalKey old{ControllerGoalId("ramp.scheduled.old"), ControllerGoalGeneration(1U)};
  const auto handle =
      participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
  participant->set_runtime_velocity_scale(0.5);
  pclock.advance(10'000'000);
  pscheduler.advance_to(pclock.now());
  Executing(old);
  Sample(0.5, 0.25, 0.5, 0.96);
  EXPECT_FALSE(handle.wait_for_completion(0));
  const auto source = participant->CaptureReplacementSource(
      1U, handle.execution_generation, old, product.prepared_contract_digest);
  ASSERT_TRUE(source.ok());
  const auto successor = DirectSuccessor(product, source.value());
  ASSERT_EQ(participant->PrepareReplacement(successor, 2U, source.value()).state,
            PrepareParticipantResultStateDTO::kPrepared);
  const ExecutionGoalKey next{ControllerGoalId("ramp.scheduled.next"), ControllerGoalGeneration(2U)};
  const auto sent = participant->DispatchReplacement(
      source.value(), 2U, successor, next, pclock.now() + 1'000'000'000);
  ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
  const auto replacements = Calls<testing::FakeExecutionPort::ReplaceCall>();
  ASSERT_EQ(replacements.size(), 1U);
  ASSERT_TRUE(replacements.front().start);
  EXPECT_EQ(replacements.front().start->controller_start_stamp_ns,
            source.value().controller_sample.source_stamp_ns + 20'000'000);
  EXPECT_NEAR(replacements.front().trajectory.waypoints.front().time_from_start_s,
              -(0.5 + 0.96 * 0.02), 1e-12);
  // Preserve the admitted ramp and instantaneous extrapolation. This does
  // not assert equality with the controller's future integral or RT phase.
  pclock.advance(10'000'000);
  pscheduler.advance_to(pclock.now());
  const auto scales = Calls<testing::FakeExecutionPort::SetScaleCall>();
  ASSERT_FALSE(scales.empty());
  EXPECT_NEAR(scales.back().value, 0.92, 1e-12);
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
}

TEST_F(PortParticipantKernelTest,
       EstimatedRampJoinPreservesPolynomialsAndBoundsWithinExistingAllowance) {
  // Scripted phase evidence only. Actual JTC factor evolution, sample age and
  // installation timing still require measured qualification before live use.
  for (const auto delay_ns : {0LL, 10'000'000LL, 20'000'000LL, 20'000'001LL}) {
    SCOPED_TRACE(delay_ns);
    auto product = DirectBackendProduct();
    auto& motion = std::get<PreparedMotionExecutionDTO>(product.payload);
    motion.plan_request.per_resource_dynamics_limits = {
        {ResourceId("joint.1"), "joint_1", {1.0, 3.0, std::nullopt}},
        {ResourceId("joint.2"), "joint_2", {1.0, 3.0, std::nullopt}}};
    Sample(0.0, 0.0, 0.0);
    const ExecutionGoalKey old{ControllerGoalId("delay.old"), ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    participant->set_runtime_velocity_scale(0.5);
    pclock.advance(10'000'000);
    pscheduler.advance_to(pclock.now());
    Executing(old);
    Sample(0.5, 0.25, 0.5, 0.96);
    EXPECT_FALSE(handle.wait_for_completion(0));
    // A real source sample remains 5 ms old during side-effect-free preparation.
    pclock.advance(5'000'000);
    const auto source = participant->CaptureReplacementSource(1U, handle.execution_generation, old,
                                                              product.prepared_contract_digest);
    ASSERT_TRUE(source.ok());
    const auto successor = DirectSuccessor(product, source.value());
    const auto prepared = participant->PrepareReplacement(successor, 2U, source.value());
    ASSERT_EQ(prepared.state, PrepareParticipantResultStateDTO::kPrepared)
        << (prepared.error ? prepared.error->message : "no preparation error")
        << (prepared.error ? ::testing::PrintToString(prepared.error->details) : "");
    int reads = 0;
    port->SetOnReadSample([&](auto&) {
      // First read selects the join origin; second represents construction /
      // copying / source proof latency before the actual SDK boundary.
      if (++reads == 2) pclock.advance(delay_ns);
    });
    const ExecutionGoalKey next{ControllerGoalId("delay.next"), ControllerGoalGeneration(2U)};
    const auto sent = participant->DispatchReplacement(source.value(), 2U, successor, next,
                                                       pclock.now() + 1'000'000'000);
    port->SetOnReadSample(nullptr);
    const auto replaced = Calls<testing::FakeExecutionPort::ReplaceCall>();
    if (delay_ns > 20'000'000) {
      EXPECT_EQ(sent.command.dispatch, ExecutionPortDispatch::kNotDispatched);
      EXPECT_TRUE(replaced.empty());
    } else {
      ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
      ASSERT_EQ(replaced.size(), 1U);
      const auto& delivered = replaced.front().trajectory;
      const double join_origin = 0.5 + 0.96 * (0.005 + 0.02);
      // Evaluate the actual sent interpolant at non-knot retained occurrences.
      // For this scripted nonzero slew, wall V/A preserve the old values and
      // declared limits. This does not assert an observed installation clock.
      for (const double dt : {0.0, 0.01, 0.02}) {
        const double path = 0.55 + 0.8 * dt - 2.0 * dt * dt;
        const double factor = 0.8 - 4.0 * dt;
        const auto original = sample_trajectory(motion.trajectory, path);
        const auto wire = sample_trajectory(delivered, path - join_origin);
        ASSERT_TRUE(original.ok() && wire.ok());
        for (std::size_t joint = 0; joint != 2U; ++joint) {
          EXPECT_NEAR(wire.value().positions_rad[joint], original.value().positions_rad[joint],
                      1e-12);
          EXPECT_NEAR(wire.value().velocities_rad_s[joint],
                      original.value().velocities_rad_s[joint], 1e-11);
          EXPECT_NEAR(wire.value().accelerations_rad_s2[joint],
                      original.value().accelerations_rad_s2[joint], 1e-10);
          const double velocity = wire.value().velocities_rad_s[joint] * factor;
          const double acceleration = wire.value().accelerations_rad_s2[joint] * factor * factor -
                                      wire.value().velocities_rad_s[joint] * 4.0;
          EXPECT_LE(
              std::abs(velocity),
              *motion.plan_request.per_resource_dynamics_limits[joint].limit.max_velocity_rad_s);
          EXPECT_LE(std::abs(acceleration), *motion.plan_request.per_resource_dynamics_limits[joint]
                                                 .limit.max_acceleration_rad_s2);
        }
      }
      ASSERT_TRUE(delivered.runtime_scale_bounds);
      EXPECT_DOUBLE_EQ(delivered.runtime_scale_bounds->max_rate_per_s,
                       motion.trajectory.runtime_scale_bounds->max_rate_per_s);
      EXPECT_DOUBLE_EQ(delivered.runtime_scale_bounds->max_rate_change_per_s2,
                       motion.trajectory.runtime_scale_bounds->max_rate_change_per_s2);
    }
    Reset();
  }
}

TEST_F(PortParticipantKernelTest, ScopedStopRetiresPendingOnlyWithEveryRetainedEffectSettled) {
  for (const bool complete : {false, true}) {
    SCOPED_TRACE(complete);
    const auto product = DirectBackendProduct();
    Sample(0.0, 0.0, 0.0);
    const ExecutionGoalKey old{ControllerGoalId("stop.old"), ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    pclock.advance(10'000'000);
    Executing(old);
    Sample(0.5, 0.25, 0.5);
    EXPECT_FALSE(handle.wait_for_completion(0));
    const auto source = participant->CaptureReplacementSource(1U, handle.execution_generation, old,
                                                              product.prepared_contract_digest);
    ASSERT_TRUE(source.ok());
    const auto successor = DirectSuccessor(product, source.value());
    const auto prepared = participant->PrepareReplacement(successor, 2U, source.value());
    ASSERT_EQ(prepared.state, PrepareParticipantResultStateDTO::kPrepared)
        << (prepared.error ? prepared.error->message : "no preparation error")
        << (prepared.error ? ::testing::PrintToString(prepared.error->details) : "");
    const ExecutionGoalKey next{ControllerGoalId("stop.next"), ControllerGoalGeneration(2U)};
    ASSERT_EQ(
        participant
            ->DispatchReplacement(source.value(), 2U, successor, next, pclock.now() + 1'000'000'000)
            .command.dispatch,
        ExecutionPortDispatch::kDispatched);
    tick = [&] {
      pclock.advance(10'000'000);
      if (complete) {
        port->FinishGoal(old, ExecutionGoalResult::kPreemptedBy, next, pclock.now());
        port->MarkCancelSent(next, pclock.now() - 1);
        port->AcknowledgeCancel(next, pclock.now());
        port->FinishGoal(next, ExecutionGoalResult::kCanceled, std::nullopt, pclock.now());
        Sample(0.0, 0.3, 0.0, 0.0);
      }
    };
    const auto stage = StageForProduct(product, old, 1U);
    const auto result = participant->TranslateStopStage(stage, BudgetFor(stage), {});
    EXPECT_EQ(result.outcome, complete ? ParticipantStopStageOutcomeDTO::kSucceeded
                                       : ParticipantStopStageOutcomeDTO::kFailed);
    const auto cancels = Calls<testing::FakeExecutionPort::CancelCall>();
    ASSERT_EQ(cancels.size(), 2U);
    EXPECT_EQ(cancels.front().key, next);
    EXPECT_EQ(cancels.back().key, old);
    EXPECT_EQ(participant->RetirePreparedExecution(2U).ok(), complete);
    if (complete) {
      // Pending cleanup does not consume the source membership. Its original
      // handle is still exact and is retired only by the source owner.
      EXPECT_EQ(
          participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation)
              .execution_generation,
          handle.execution_generation);
      EXPECT_TRUE(participant->RetirePreparedExecution(1U).ok());
    } else {
      EXPECT_FALSE(participant->RetirePreparedExecution(1U).ok());
    }
    Reset();
  }
}

TEST_F(PortParticipantKernelTest,
       AcceptedReplacementWaitsForEvidenceWithoutLateRecognitionRefusal) {
  for (const bool late : {false, true}) {
    SCOPED_TRACE(late);
    const auto product = DirectBackendProduct();
    Sample(0.0, 0.0, 0.0);
    const ExecutionGoalKey old{ControllerGoalId("unknown.old"), ControllerGoalGeneration(1U)};
    const auto handle =
        participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
    pclock.advance(10'000'000);
    Executing(old);
    Sample(0.5, 0.25, 0.5);
    EXPECT_FALSE(handle.wait_for_completion(0));
    const auto source = participant->CaptureReplacementSource(1U, handle.execution_generation, old,
                                                              product.prepared_contract_digest);
    ASSERT_TRUE(source.ok());
    auto successor = DirectSuccessor(product, source.value());
    std::get<PreparedMotionExecutionDTO>(successor.payload)
        .plan_request.moving_replacement->divergence_cursor_s =
        static_cast<double>(source.value().progress.trajectory_time_ns) / 1e9 + 0.032;
    const auto prepared = participant->PrepareReplacement(successor, 2U, source.value());
    ASSERT_EQ(prepared.state, PrepareParticipantResultStateDTO::kPrepared)
        << (prepared.error ? prepared.error->message : "no preparation error")
        << (prepared.error ? ::testing::PrintToString(prepared.error->details) : "");
    const ExecutionGoalKey next{ControllerGoalId("unknown.next"), ControllerGoalGeneration(2U)};
    const auto sent = participant->DispatchReplacement(source.value(), 2U, successor, next,
                                                       pclock.now() + 2'000'000'000);
    ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
    ASSERT_TRUE(sent.submitted_at_ns);
    EXPECT_EQ(*sent.submitted_at_ns, pclock.now());
    port->FinishGoal(old, ExecutionGoalResult::kPreemptedBy, next, pclock.now());
    if (late) {
      ExecutionGoalObservationDTO accepted;
      accepted.key = next;
      accepted.binding_epoch = source.value().binding_epoch;
      accepted.phase = ExecutionGoalPhase::kAccepted;
      accepted.mechanism_goal_id = MechanismGoalId("accepted-delayed-feedback");
      accepted.accepted_stamp_ns = pclock.now();
      port->SetGoal(accepted);
      const auto before = participant->execution_progress_publication()->acquire();
      EXPECT_EQ(participant->ObserveReplacement(source.value(), 2U).value().phase,
                PreparedMotionReplacementPhaseDTO::kPossiblySent);
      int early_commits = 0;
      EXPECT_FALSE(participant
                       ->AdoptReplacement(source.value(), 2U,
                                          {&early_commits,
                                           [](void* count) noexcept {
                                             ++*static_cast<int*>(count);
                                             return Result<void>::Ok();
                                           }})
                       .ok());
      EXPECT_EQ(early_commits, 0);
      EXPECT_EQ(participant->execution_progress_publication()->acquire().sequence,
                before.sequence);
      pclock.advance(50'000'000);
      // Elapsed source lead without contradictory evidence is still pending.
      EXPECT_EQ(participant->ObserveReplacement(source.value(), 2U).value().phase,
                PreparedMotionReplacementPhaseDTO::kPossiblySent);
      Executing(next);
      Sample(0.1, 0.31, 0.5);
    } else {
      port->FinishGoal(next, ExecutionGoalResult::kEndedUnknown, std::nullopt, pclock.now());
    }
    const auto observed = participant->ObserveReplacement(source.value(), 2U);
    ASSERT_TRUE(observed.ok());
    EXPECT_EQ(observed.value().phase, late ? PreparedMotionReplacementPhaseDTO::kReadyToCommit
                                          : PreparedMotionReplacementPhaseDTO::kEffectUncertain);
    if (late) {
      ASSERT_TRUE(observed.value().recognition_delay_ns);
      EXPECT_GE(*observed.value().recognition_delay_ns, 49'999'999);
      EXPECT_FALSE(observed.value().error);
    }
    int commits = 0;
    EXPECT_EQ(participant
                  ->AdoptReplacement(source.value(), 2U,
                                     {&commits, [](void* count) noexcept {
                                        ++*static_cast<int*>(count);
                                        return Result<void>::Ok();
                                      }})
                  .ok(),
              late);
    EXPECT_EQ(commits, late ? 1 : 0);
    EXPECT_FALSE(participant->RetirePreparedExecution(2U).ok());
    const auto replay = participant->DispatchReplacement(source.value(), 2U, successor, next, 0);
    EXPECT_EQ(replay.submitted_at_ns, sent.submitted_at_ns);
    EXPECT_EQ(Calls<testing::FakeExecutionPort::ReplaceCall>().size(), 1U);
    Reset();
  }
}

TEST_F(PortParticipantKernelTest, RecognitionUsesOneOriginAndRetirementRestoresOrdinaryMembership) {
  const auto product = DirectBackendProduct();
  Sample(0.0, 0.0, 0.0);
  const ExecutionGoalKey old{ControllerGoalId("origin.old"), ControllerGoalGeneration(1U)};
  const auto handle =
      participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
  pclock.advance(10'000'000);
  Executing(old);
  Sample(0.5, 0.25, 0.5);
  EXPECT_FALSE(handle.wait_for_completion(0));
  const auto source = participant->CaptureReplacementSource(1U, handle.execution_generation, old,
                                                            product.prepared_contract_digest);
  ASSERT_TRUE(source.ok());
  const auto successor = DirectSuccessor(product, source.value());
  const auto prepared = participant->PrepareReplacement(successor, 2U, source.value());
  ASSERT_EQ(prepared.state, PrepareParticipantResultStateDTO::kPrepared)
      << (prepared.error ? prepared.error->message : "no preparation error")
      << (prepared.error ? ::testing::PrintToString(prepared.error->details) : "");
  int reads = 0;
  port->SetOnReadSample([&](auto&) {
    if (++reads == 2) pclock.advance(20'000'000);
  });
  const ExecutionGoalKey next{ControllerGoalId("origin.next"), ControllerGoalGeneration(2U)};
  const auto sent = participant->DispatchReplacement(source.value(), 2U, successor, next,
                                                     pclock.now() + 1'000'000'000);
  port->SetOnReadSample(nullptr);
  ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(sent.handle);
  port->FinishGoal(old, ExecutionGoalResult::kPreemptedBy, next, pclock.now());
  // Recognition is 990 ms after join start, but only 970 ms after the SDK
  // call: before divergence. Counting the 20 ms join work twice would refuse.
  pclock.advance(970'000'000);
  Executing(next);
  Sample(0.95, 0.735, 0.5);
  ASSERT_EQ(participant->ObserveReplacement(source.value(), 2U).value().phase,
            PreparedMotionReplacementPhaseDTO::kReadyToCommit);
  ASSERT_TRUE(participant
                  ->AdoptReplacement(source.value(), 2U,
                                     {nullptr, [](void*) noexcept { return Result<void>::Ok(); }})
                  .ok());
  port->FinishGoal(next, ExecutionGoalResult::kSucceeded, std::nullopt, pclock.now());
  EXPECT_TRUE(sent.handle->wait_for_completion(0));
  EXPECT_EQ(sent.handle->get_snapshot().state, ExecutionState::kCompleted);
  ASSERT_TRUE(participant->RetirePreparedExecution(2U).ok());

  auto ordinary = product;
  ordinary.prepared_contract_digest = ContentDigest(kTestDigestC);
  ASSERT_EQ(participant->Prepare(ordinary, 3U).state, PrepareParticipantResultStateDTO::kPrepared);
  const ExecutionGoalKey third{ControllerGoalId("origin.ordinary"), ControllerGoalGeneration(3U)};
  const auto ordinary_handle =
      participant->Dispatch(ordinary, third.controller_goal_id, third.controller_goal_generation);
  ASSERT_NE(ordinary_handle.execution_generation, 0U);
  pclock.advance(10'000'000);
  Executing(third);
  Sample(0.25, 0.125, 0.5);
  EXPECT_FALSE(ordinary_handle.wait_for_completion(0));
  const auto ordinary_source = participant->CaptureReplacementSource(
      3U, ordinary_handle.execution_generation, third, ordinary.prepared_contract_digest);
  ASSERT_TRUE(ordinary_source.ok());
  const auto fourth = DirectSuccessor(ordinary, ordinary_source.value());
  // The retired candidate's receipt must not masquerade as a live pending
  // handover or prevent preparing a replacement of this ordinary goal.
  EXPECT_EQ(participant->PrepareReplacement(fourth, 4U, ordinary_source.value()).state,
            PrepareParticipantResultStateDTO::kPrepared);
  EXPECT_TRUE(participant->RetirePreparedExecution(4U).ok());
  auto readiness = port->ReadReadiness();
  readiness.hardware_active = false;
  port->SetReadiness(readiness);
  EXPECT_TRUE(ordinary_handle.wait_for_completion(0));
  const auto cancels = Calls<testing::FakeExecutionPort::CancelCall>();
  ASSERT_EQ(cancels.size(), 1U);
  EXPECT_EQ(cancels.front().key, third);
}

TEST_F(PortParticipantKernelTest, TimelyAdoptionPreservesRampDeliveryClockAndQualifiesSpringPoint) {
  const auto product = DirectBackendProduct(true);
  Sample(0.0, 0.0, 0.0);
  const ExecutionGoalKey old{ControllerGoalId("adopt.old"), ControllerGoalGeneration(1U)};
  const auto old_handle =
      participant->Dispatch(product, old.controller_goal_id, old.controller_goal_generation);
  participant->set_runtime_velocity_scale(0.5);
  pclock.advance(10'000'000);
  pscheduler.advance_to(pclock.now());
  Executing(old);
  Sample(0.5, 0.25, 0.5, 0.96);
  EXPECT_FALSE(old_handle.wait_for_completion(0));
  const auto source = participant->CaptureReplacementSource(1U, old_handle.execution_generation,
                                                            old, product.prepared_contract_digest);
  ASSERT_TRUE(source.ok());
  const auto successor = DirectSuccessor(product, source.value());
  const auto prepared = participant->PrepareReplacement(successor, 2U, source.value());
  ASSERT_EQ(prepared.state, PrepareParticipantResultStateDTO::kPrepared)
      << (prepared.error ? prepared.error->message : "no preparation error")
      << (prepared.error ? ::testing::PrintToString(prepared.error->details) : "");
  const ExecutionGoalKey next{ControllerGoalId("adopt.next"), ControllerGoalGeneration(2U)};
  const auto sent = participant->DispatchReplacement(source.value(), 2U, successor, next,
                                                     pclock.now() + 1'000'000'000);
  ASSERT_EQ(sent.command.dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(sent.handle);
  // Source completion must be exact supersession by this successor, with
  // that successor's own feedback and controller sample, before the owner CAS.
  port->FinishGoal(old, ExecutionGoalResult::kPreemptedBy, next, pclock.now());
  EXPECT_EQ(participant->ObserveReplacement(source.value(), 2U).value().phase,
            PreparedMotionReplacementPhaseDTO::kPossiblySent);
  pclock.advance(10'000'000);
  pscheduler.advance_to(pclock.now());
  Executing(next);
  Sample(0.1, 0.31, 0.5, 0.92);
  EXPECT_FALSE(old_handle.wait_for_completion(0));
  // A successor sample must not overwrite predecessor progress before CAS.
  EXPECT_EQ(participant->execution_progress_publication()->acquire().trajectory_time_ns,
            source.value().progress.trajectory_time_ns);
  EXPECT_EQ(participant->ObserveReplacement(source.value(), 2U).value().phase,
            PreparedMotionReplacementPhaseDTO::kReadyToCommit);
  int commits = 0;
  const auto adopted =
      participant->AdoptReplacement(source.value(), 2U, {&commits, [](void* context) noexcept {
                                                           ++*static_cast<int*>(context);
                                                           return Result<void>::Ok();
                                                         }});
  ASSERT_TRUE(adopted.ok()) << adopted.error().message;
  EXPECT_EQ(commits, 1);
  const auto progress = participant->execution_progress_publication()->acquire();
  EXPECT_EQ(progress.execution_generation, sent.handle->execution_generation);
  // The wire starts later than captured canonical zero; retain its actual
  // origin shift rather than publishing the raw wire cursor as prepared time.
  EXPECT_NEAR(static_cast<double>(progress.trajectory_time_ns) / 1e9, 0.1 + 0.96 * 0.02, 1e-9);
  EXPECT_TRUE(old_handle.wait_for_completion(0));
  EXPECT_EQ(old_handle.get_snapshot().state, ExecutionState::kStopped);
  EXPECT_TRUE(participant->RetirePreparedExecution(1U).ok());
  EXPECT_EQ(
      participant->Dispatch(successor, next.controller_goal_id, next.controller_goal_generation)
          .execution_generation,
      sent.handle->execution_generation);
  pclock.advance(10'000'000);
  pscheduler.advance_to(pclock.now());
  const auto scales = Calls<testing::FakeExecutionPort::SetScaleCall>();
  ASSERT_FALSE(scales.empty());
  EXPECT_NEAR(scales.back().value, 0.88, 1e-12);
  EXPECT_TRUE(Calls<testing::FakeExecutionPort::CancelCall>().empty());
  const auto recaptured = participant->CaptureReplacementSource(
      2U, sent.handle->execution_generation, next, successor.prepared_contract_digest);
  ASSERT_TRUE(recaptured.ok());
  EXPECT_EQ(recaptured.value().goal, next);
  EXPECT_EQ(recaptured.value().progress.sequence, progress.sequence);
  EXPECT_EQ(recaptured.value().progress.source_timestamp_ns,
            recaptured.value().controller_sample.source_stamp_ns);
  EXPECT_EQ(recaptured.value().progress.received_timestamp_ns,
            recaptured.value().controller_sample.local_receipt_stamp_ns);
  EXPECT_FALSE(participant->CaptureReplacementSource(
      1U, old_handle.execution_generation, old, product.prepared_contract_digest).ok());
  EXPECT_DOUBLE_EQ(recaptured.value().controller_sample.cursor_s, 0.1);
  EXPECT_EQ(recaptured.value().progress.trajectory_time_ns, progress.trajectory_time_ns);

  // A subsequent takeover fallback sees the delivery occurrence unchanged,
  // plus the separately qualified canonical occurrence from this same owner.
  std::stop_source cancel;
  class PointObserver final : public IStopStageSpringReturnObserver {
   public:
    explicit PointObserver(std::stop_source& source) : cancel(source) {}
    bool SpringReturnPointReady(const SpringBackReturnPointDTO& value) noexcept override {
      try {
        point = value;
      } catch (...) {
        return false;
      }
      cancel.request_stop();  // stop this test after the early point publication
      return true;
    }
    std::stop_source& cancel;
    std::optional<SpringBackReturnPointDTO> point;
  } observer(cancel);
  pclock.advance(1);
  Sample(0.1, 0.5 * (0.5 + 0.1 + 0.96 * 0.02), 0.5, scales.back().value);
  const auto stage = StageForProduct(successor, next, 0U);
  // The participant's waiter is this case's only passage of time (1 ms per
  // wait), and the Stop replacement shows its own feedback once sent (the fake
  // port never infers it). Without this the lead-in wait can neither be
  // satisfied nor expire on the FakeClock.
  const auto replaced = Calls<testing::FakeExecutionPort::ReplaceCall>().size();
  {
    std::lock_guard lock(hook_mutex);
    tick = [&] {
      pclock.advance(1'000'000);
      const auto replaces = Calls<testing::FakeExecutionPort::ReplaceCall>();
      if (replaces.size() > replaced) Executing(replaces.back().next_key);
    };
  }
  const auto stopped = participant->TranslateStopStageForTakeover(
      stage, BudgetFor(stage), cancel.get_token(), observer);
  {
    std::lock_guard lock(hook_mutex);
    tick = nullptr;
  }
  ASSERT_TRUE(observer.point) << (stopped.error ? stopped.error->message : "no Stop error")
                             << (stopped.error ? ::testing::PrintToString(stopped.error->details)
                                               : "");
  EXPECT_NEAR(observer.point->initial.command_path_time_s, 0.1, 1e-12);
  ASSERT_TRUE(observer.point->canonical_return_path_time_s);
  EXPECT_NEAR(*observer.point->canonical_return_path_time_s, 0.1 + 0.96 * 0.02, 1e-12);
  EXPECT_DOUBLE_EQ(observer.point->trip.cursor_s, 0.1);
}

// This fixture adds source capture only. It deliberately retains the ordinary
// backend's unsupported replacement dispatch, so no test can accidentally
// qualify a second physical presenter through the ordinary idle path.
class ReplacementObservationExecution : public ControlledExecution {
 public:
  mutable std::mutex observations_mutex;
  mutable std::condition_variable observations_changed;
  int polls{};
  double replacement_factor{1.0};
  std::int64_t replacement_controller_period_ns{0};

  ExecutionHandle Dispatch(const PreparedOperationParticipantDTO& product,
                           const ControllerGoalId& goal,
                           ControllerGoalGeneration generation) override {
    auto handle = ControlledExecution::Dispatch(product, goal, generation);
    auto wait = std::move(handle.wait_for_completion);
    handle.wait_for_completion = [this, wait = std::move(wait)](std::int64_t timeout) {
      {
        std::lock_guard lock(observations_mutex);
        ++polls;
      }
      observations_changed.notify_all();
      return wait(timeout);
    };
    return handle;
  }

  Result<PreparedMotionReplacementSourceDTO> CaptureReplacementSource(
      std::uint64_t id, std::uint64_t generation, const ExecutionGoalKey& key,
      const ContentDigest& digest) const override {
    auto progress = execution_progress_publication()->acquire();
    ExecutionControllerSampleDTO sample;
    sample.binding_epoch = ExecutionBindingEpoch(1U);
    sample.applied_factor = replacement_factor;
    sample.cursor_s = static_cast<double>(progress.trajectory_time_ns) / 1e9;
    sample.source_stamp_ns = progress.source_timestamp_ns;
    sample.local_receipt_stamp_ns = progress.received_timestamp_ns;
    return Result<PreparedMotionReplacementSourceDTO>::Ok(
        {id, generation, key, sample.binding_epoch, digest, std::move(sample), progress,
         replacement_controller_period_ns});
  }
};

TEST_F(ExecutionKernelTest,
       MovingReplacementComputationKeepsIncumbentObservationAndCancelsWithoutDispatch) {
  planner.next_result.trajectory = make_test_trajectory();
  for (auto& point : planner.next_result.trajectory->waypoints) {
    point.velocities_rad_s.assign(point.positions_rad.size(), 0.0);
    point.accelerations_rad_s2.assign(point.positions_rad.size(), 0.0);
  }
  struct PlanningGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool released{};
    bool candidate_completed{};
  } gate;
  ReplacementObservationExecution backend;
  backend.enable_execution_progress_publication();
  ExecutionKernel owner(planner, context, backend, clock, scheduler, cache);
  owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
  owner.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                {collision_engine});
  auto incumbent = make_test_request("replacement.incumbent");
  incumbent.action_span = make_test_action_span();
  const auto started = prepare_and_dispatch(owner, prepared_motion_request(incumbent));
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(backend.wait_until_execute_called());
  {
    std::unique_lock lock(backend.observations_mutex);
    ASSERT_TRUE(backend.observations_changed.wait_for(lock, std::chrono::seconds(2),
                                                      [&]() { return backend.polls > 0; }));
  }
  const auto source = owner.CaptureMotionReplacementSource(started.value());
  ASSERT_TRUE(source.ok());

  struct ReleasePlanning {
    PlanningGate& gate;
    ~ReleasePlanning() {
      {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
      }
      gate.changed.notify_all();
    }
  } release{gate};
  const auto completion_subscription = owner.subscribe_events([&](const KernelEvent& event) {
    if (event.type != KernelEventType::kParticipantPrepared ||
        event.execution_id == started.value())
      return;
    {
      std::lock_guard lock(gate.mutex);
      gate.candidate_completed = true;
    }
    gate.changed.notify_all();
  });
  struct UnsubscribeCompletion {
    ExecutionKernel& owner;
    SubscriptionHandle subscription;
    ~UnsubscribeCompletion() { owner.unsubscribe_events(subscription); }
  } unsubscribe_completion{owner, completion_subscription};
  planner.on_plan = [&](int count) {
    if (count != 2) return;
    std::unique_lock lock(gate.mutex);
    gate.entered = true;
    gate.changed.notify_all();
    (void)gate.changed.wait_for(lock, std::chrono::seconds(2), [&]() { return gate.released; });
  };
  auto newcomer_request = make_test_request("replacement.newcomer");
  newcomer_request.action_span = make_test_action_span();
  auto newcomer = prepared_motion_request(newcomer_request);
  newcomer.execution_request.binding.controller_goal_id = ControllerGoalId("replacement.new.goal");
  newcomer.replacement = ResolvedPreparedMotionReplacementContextDTO{source.value(), 250'000'000,
                                                                     std::nullopt, 100'000'000};
  const auto preparing = owner.PrepareMotion(std::move(newcomer));
  ASSERT_TRUE(preparing.ok());
  {
    std::unique_lock lock(gate.mutex);
    ASSERT_TRUE(
        gate.changed.wait_for(lock, std::chrono::seconds(2), [&]() { return gate.entered; }));
  }
  {
    std::unique_lock lock(backend.observations_mutex);
    const auto before = backend.polls;
    EXPECT_TRUE(backend.observations_changed.wait_for(
        lock, std::chrono::seconds(2), [&]() { return backend.polls >= before + 2; }));
  }
  // The hold precedes the planner and remains owned until explicit cleanup,
  // even though cancellation prevents preparing a backend candidate product.
  EXPECT_EQ(backend.replacement_hold_begins.load(), 1);
  EXPECT_EQ(backend.replacement_hold_candidate.load(), preparing.value());
  EXPECT_EQ(backend.replacement_hold_deadline_ns.load(), clock.now() + 250'000'000);
  EXPECT_EQ(backend.replacement_hold_ends.load(), 0);
  ASSERT_TRUE(planner.last_request.moving_replacement);
  const auto& moving = *planner.last_request.moving_replacement;
  ASSERT_TRUE(moving.remaining_planning_budget_s);
  EXPECT_GT(*moving.remaining_planning_budget_s, 0.0);
  EXPECT_LE(*moving.remaining_planning_budget_s, 0.1);
  EXPECT_GT(moving.remaining_preparation_budget_s, *moving.remaining_planning_budget_s);
  // The preparation setting, not the earlier planning cap, still chooses the cut.
  EXPECT_NEAR(moving.divergence_cursor_s - moving.source_cursor_s,
              0.25 * *source.value().controller_sample.applied_factor, 1e-9);
  EXPECT_EQ(owner.get_status().execution_id, started.value());
  EXPECT_FALSE(owner.get_status().fact.has_value());
  EXPECT_EQ(backend.execute_count, 1);
  EXPECT_TRUE(owner.CancelPreparedMotion(preparing.value()).ok());
  const auto cancelled = owner.ObservePreparedMotion(preparing.value());
  ASSERT_TRUE(cancelled.ok());
  EXPECT_EQ(cancelled.value().state,
            ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch);
  {
    std::lock_guard lock(gate.mutex);
    gate.released = true;
  }
  gate.changed.notify_all();
  {
    std::unique_lock lock(gate.mutex);
    ASSERT_TRUE(gate.changed.wait_for(lock, std::chrono::seconds(2),
                                      [&]() { return gate.candidate_completed; }));
  }
  EXPECT_EQ(backend.replacement_hold_ends.load(), 0);
  // Cancellation stays observable until the owner explicitly accepts cleanup.
  ASSERT_TRUE(owner.ObservePreparedMotion(preparing.value()).ok());
  const auto retired = owner.RetireCancelledPreparedMotion(preparing.value());
  ASSERT_TRUE(retired.ok());
  EXPECT_TRUE(retired.value());
  EXPECT_EQ(backend.replacement_hold_ends.load(), 1);
  EXPECT_EQ(backend.replacement_hold_ended_candidate.load(), preparing.value());
  EXPECT_FALSE(owner.ObservePreparedMotion(preparing.value()).ok());
  EXPECT_TRUE(owner.ObservePreparedMotion(started.value()).ok());
  backend.complete();
  ASSERT_TRUE(owner.wait_for_completion(2'000'000'000));
  EXPECT_EQ(backend.execute_count, 1);
  EXPECT_EQ(backend.stop_count, 0);
}

TEST_F(ExecutionKernelTest, MovingReplacementPlansOneUnscaledPeriodAheadAcrossRatesAndFactors) {
  constexpr std::int64_t budget_ns = 250'000'000;
  for (const auto period_ns : {10'000'000LL, 6'666'666LL}) {
    for (const double factor : {1.0, 0.5, 0.1}) {
      for (const auto elapsed_ns : {0LL, 125'000'000LL, budget_ns - 1LL}) {
        SCOPED_TRACE(::testing::PrintToString(std::make_tuple(period_ns, factor, elapsed_ns)));
        planner.on_plan = {};
        planner.next_result.trajectory = make_test_trajectory();
        for (auto &point : planner.next_result.trajectory->waypoints) {
          point.velocities_rad_s.assign(point.positions_rad.size(), 0.0);
          point.accelerations_rad_s2.assign(point.positions_rad.size(), 0.0);
        }
        ReplacementObservationExecution backend;
        backend.replacement_factor = factor;
        backend.replacement_controller_period_ns = period_ns;
        backend.enable_execution_progress_publication();
        std::promise<MovingReplacementInputDTO> planned;
        auto result = planned.get_future();
        std::promise<void> release;
        auto released = release.get_future().share();
        ExecutionKernel owner(planner, context, backend, clock, scheduler, cache);
        owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
        owner.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                      {collision_engine});
        // Release before owner destruction even when an assertion fails.
        struct ReleasePlanner { std::promise<void> &gate; ~ReleasePlanner() { gate.set_value(); } } guard{release};
        auto original = make_test_request("period.incumbent");
        original.cache_mode = CacheMode::kDisable;
        original.action_span = make_test_action_span();
        const auto started = prepare_and_dispatch(owner, prepared_motion_request(original));
        ASSERT_TRUE(started.ok());
        {
          std::unique_lock lock(backend.observations_mutex);
          ASSERT_TRUE(backend.observations_changed.wait_for(lock, std::chrono::seconds(2),
                                                          [&] { return backend.polls > 0; }));
        }
        const auto source = owner.CaptureMotionReplacementSource(started.value());
        ASSERT_TRUE(source.ok());
        planner.on_plan = [&](int) {
          clock.advance(elapsed_ns);
          planned.set_value(planner.last_request.moving_replacement.value());
          released.wait();
        };
        auto next = prepared_motion_request(make_test_request("period.newcomer"));
        next.execution_request.cache_mode = CacheMode::kDisable;
        next.execution_request.action_span = make_test_action_span();
        next.execution_request.binding.controller_goal_id = ControllerGoalId("period.new.goal");
        next.replacement = ResolvedPreparedMotionReplacementContextDTO{source.value(), budget_ns};
        const auto preparing = owner.PrepareMotion(std::move(next));
        ASSERT_TRUE(preparing.ok());
        ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        const auto moving = result.get();
        const double period = static_cast<double>(period_ns) / 1e9;
        const double budget = static_cast<double>(budget_ns) / 1e9;
        const double source_cursor = static_cast<double>(source.value().progress.trajectory_time_ns) / 1e9;
        EXPECT_NEAR(moving.divergence_cursor_s, source_cursor + factor * budget + period, 1e-12);
        ASSERT_FALSE(moving.source_prefix_trajectory.waypoints.empty());
        EXPECT_NEAR(moving.source_prefix_trajectory.waypoints.back().time_from_start_s,
                    moving.divergence_cursor_s - source_cursor, 1e-12);
        const auto cut = sample_trajectory(backend.last_execute_trajectory, moving.divergence_cursor_s);
        ASSERT_TRUE(cut.ok()) << cut.error().message;
        const auto &last = moving.source_prefix_trajectory.waypoints.back();
        EXPECT_EQ(last.positions_rad, cut.value().positions_rad);
        EXPECT_EQ(last.velocities_rad_s, cut.value().velocities_rad_s);
        EXPECT_EQ(last.accelerations_rad_s2, cut.value().accelerations_rad_s2);
        // These are model checks against the ACTUAL planner input, not a live
        // install guarantee: one nominal quantized tick can follow the send.
        for (const double phase : {0.0, period / 2.0, period}) {
          const double elapsed = static_cast<double>(elapsed_ns) / 1e9;
          const double command_cursor = source_cursor + factor * (elapsed + phase) + period;
          EXPECT_GE(moving.divergence_cursor_s + 1e-12, command_cursor - factor * period);
        }
        // Extra jitter or an external factor ramp can exceed the nominal
        // bound. Quantify those residuals; do not turn them into a refusal.
        constexpr double jitter = 0.0008;
        const double jitter_previous = source_cursor + factor * (budget + jitter) + period;
        EXPECT_NEAR(jitter_previous - moving.divergence_cursor_s, factor * jitter, 1e-12);
        const double ramp_rate = factor == 1.0 ? -0.05 : 0.05;
        const double ramp_previous = source_cursor + factor * budget + 0.5 * ramp_rate * budget * budget + period;
        EXPECT_NEAR(ramp_previous - moving.divergence_cursor_s,
                    0.5 * ramp_rate * budget * budget, 1e-12);
        RecordProperty("margin_" + std::to_string(period_ns) + "_" + std::to_string(factor) +
                           "_" + std::to_string(elapsed_ns),
                       ::testing::PrintToString(std::make_tuple(moving.divergence_cursor_s,
                           factor * jitter, ramp_previous - moving.divergence_cursor_s)));
        EXPECT_EQ(backend.execute_count, 1);
        EXPECT_EQ(owner.get_status().execution_id, started.value());
        ASSERT_TRUE(owner.CancelPreparedMotion(preparing.value()).ok());
        EXPECT_EQ(owner.ObservePreparedMotion(preparing.value()).value().state,
                  ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch);
        backend.complete();
      }
    }
  }
}

TEST_F(ExecutionKernelTest,
       MovingReplacementMarginInclusiveEndBoundaryRefusesBeforePlanningOrDispatch) {
  constexpr std::int64_t budget_ns = 250'000'000;
  constexpr std::int64_t source_end_ns = 1'000'000'000;
  struct Rate {
    int hz;
    std::int64_t period_ns;
  };
  for (const auto rate : {Rate{100, 10'000'000}, Rate{150, 6'666'666}}) {
    for (const double factor : {1.0, 0.5, 0.1}) {
      const auto scaled_budget_ns =
          static_cast<std::int64_t>(static_cast<double>(budget_ns) * factor);
      for (const std::int64_t boundary_offset_ns : {-1LL, 0LL, 1LL}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::make_tuple(rate.hz, factor, boundary_offset_ns)));
        planner.on_plan = {};
        planner.next_result.trajectory = make_test_trajectory();
        for (auto& point : planner.next_result.trajectory->waypoints) {
          point.velocities_rad_s.assign(point.positions_rad.size(), 0.0);
          point.accelerations_rad_s2.assign(point.positions_rad.size(), 0.0);
        }
        ReplacementObservationExecution backend;
        backend.replacement_factor = factor;
        backend.replacement_controller_period_ns = rate.period_ns;
        backend.enable_execution_progress_publication();
        std::promise<MovingReplacementInputDTO> planned;
        auto planner_input = planned.get_future();
        std::promise<void> release;
        auto released = release.get_future().share();
        bool release_needed = boundary_offset_ns < 0;
        ExecutionKernel owner(planner, context, backend, clock, scheduler, cache);
        struct ReleasePlanner {
          std::promise<void>& release;
          bool& needed;
          ~ReleasePlanner() {
            if (needed) release.set_value();
          }
        } release_planner{release, release_needed};
        owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
        owner.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                      {collision_engine});
        auto original = make_test_request("end.boundary.incumbent");
        original.cache_mode = CacheMode::kDisable;
        original.action_span = make_test_action_span();
        const auto started = prepare_and_dispatch(owner, prepared_motion_request(original));
        ASSERT_TRUE(started.ok()) << started.error().message;
        {
          std::unique_lock lock(backend.observations_mutex);
          ASSERT_TRUE(backend.observations_changed.wait_for(lock, std::chrono::seconds(2),
                                                            [&] { return backend.polls > 0; }));
        }
        auto source = owner.CaptureMotionReplacementSource(started.value());
        ASSERT_TRUE(source.ok()) << source.error().message;
        const auto cursor_ns =
            source_end_ns - scaled_budget_ns - rate.period_ns + boundary_offset_ns;
        source.value().progress.trajectory_time_ns = cursor_ns;
        source.value().controller_sample.cursor_s = static_cast<double>(cursor_ns) / 1e9;
        const double unmargined_divergence = source.value().controller_sample.cursor_s +
                                             factor * static_cast<double>(budget_ns) / 1e9;
        const double margin_inclusive_divergence =
            unmargined_divergence + static_cast<double>(rate.period_ns) / 1e9;
        EXPECT_LT(unmargined_divergence, 1.0);
        EXPECT_NEAR(margin_inclusive_divergence,
                    1.0 + static_cast<double>(boundary_offset_ns) / 1e9, 1e-15);

        const int plans_before_candidate = planner.plan_count;
        if (boundary_offset_ns < 0) {
          planner.on_plan = [&](int count) {
            if (count != plans_before_candidate + 1) return;
            planned.set_value(*planner.last_request.moving_replacement);
            released.wait();
          };
        }
        auto next = prepared_motion_request(make_test_request("end.boundary.newcomer"));
        next.execution_request.cache_mode = CacheMode::kDisable;
        next.execution_request.action_span = make_test_action_span();
        next.execution_request.binding.controller_goal_id =
            ControllerGoalId("end.boundary.new.goal");
        next.replacement = ResolvedPreparedMotionReplacementContextDTO{source.value(), budget_ns};
        const auto preparing = owner.PrepareMotion(std::move(next));
        ASSERT_TRUE(preparing.ok()) << preparing.error().message;

        if (boundary_offset_ns < 0) {
          ASSERT_EQ(planner_input.wait_for(std::chrono::seconds(2)), std::future_status::ready);
          const auto moving = planner_input.get();
          EXPECT_NEAR(moving.divergence_cursor_s, margin_inclusive_divergence, 1e-15);
          EXPECT_EQ(planner.plan_count, plans_before_candidate + 1);
          EXPECT_TRUE(owner.CancelPreparedMotion(preparing.value()).ok());
          release.set_value();
          release_needed = false;
        } else {
          std::optional<ResolvedPreparedMotionExecutionSnapshotDTO> observed;
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
          while (std::chrono::steady_clock::now() < deadline) {
            auto snapshot = owner.ObservePreparedMotion(preparing.value());
            ASSERT_TRUE(snapshot.ok()) << snapshot.error().message;
            observed = std::move(snapshot.value());
            if (observed->state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed)
              break;
            std::this_thread::yield();
          }
          ASSERT_TRUE(observed);
          ASSERT_EQ(observed->state, ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed);
          ASSERT_TRUE(observed->error);
          EXPECT_EQ(observed->error->code, kErrorExecutionRejected);
          EXPECT_EQ(observed->error->details.at("reason"),
                    "motion_replacement_future_lead_unavailable");
          EXPECT_FALSE(observed->last_dispatched_participant);
          EXPECT_EQ(planner.plan_count, plans_before_candidate);
        }
        EXPECT_EQ(backend.replacement_hold_begins.load(), 1);
        EXPECT_EQ(backend.execute_count, 1);
        EXPECT_EQ(owner.get_status().execution_id, started.value());
        EXPECT_EQ(owner.get_status().state, ExecutionState::kExecuting);
        EXPECT_FALSE(owner.get_status().fact.has_value());
        const auto incumbent = owner.ObservePreparedMotion(started.value());
        ASSERT_TRUE(incumbent.ok());
        EXPECT_TRUE(incumbent.value().last_dispatched_participant);
        backend.complete();
        ASSERT_TRUE(owner.wait_for_completion(2'000'000'000));
      }
    }
  }
}

class KernelReplacementCommitExecution : public ReplacementObservationExecution {
 public:
  IClock* watchdog_clock{};
  std::function<void()> before_ready;
  mutable bool recognition_delayed{};
  ExecutionHandle current_handle;
  TrajectoryDTO candidate_curve;
  std::uint64_t next_generation{};
  int replacements{};
  int adoptions{};

  ExecutionHandle Dispatch(const PreparedOperationParticipantDTO& product,
                           const ControllerGoalId& goal,
                           ControllerGoalGeneration generation) override {
    current_handle = ReplacementObservationExecution::Dispatch(product, goal, generation);
    return current_handle;
  }

  PrepareParticipantResultDTO PrepareReplacement(
      const PreparedOperationParticipantDTO& product, std::uint64_t id,
      const PreparedMotionReplacementSourceDTO&) override {
    return Prepare(product, id);
  }

  PreparedMotionReplacementDispatchDTO DispatchReplacement(
      const PreparedMotionReplacementSourceDTO& source, std::uint64_t,
      const PreparedOperationParticipantDTO& product, const ExecutionGoalKey& goal,
      std::int64_t) override {
    ++replacements;
    candidate_curve = std::get<PreparedMotionExecutionDTO>(product.payload).trajectory;
    auto handle = current_handle;
    next_generation = source.execution_generation + 1U;
    handle.execution_generation = next_generation;
    handle.controller_goal_id = goal.controller_goal_id;
    handle.controller_goal_generation = goal.controller_goal_generation;
    handle.prepared_contract_digest = product.prepared_contract_digest;
    return {{ExecutionPortDispatch::kDispatched, {}, {}}, std::move(handle), watchdog_clock->now()};
  }

  Result<PreparedMotionReplacementObservationDTO> ObserveReplacement(
      const PreparedMotionReplacementSourceDTO&, std::uint64_t) const override {
    // Model a backend that already proved exact supersession and its own
    // association. The real participant contract is tested separately.
    if (before_ready && !recognition_delayed) {
      recognition_delayed = true;
      before_ready();
    }
    return Result<PreparedMotionReplacementObservationDTO>::Ok(
        {PreparedMotionReplacementPhaseDTO::kReadyToCommit, {}, {}, 1, {}});
  }

  Result<void> AdoptReplacement(const PreparedMotionReplacementSourceDTO&, std::uint64_t,
                                PreparedMotionReplacementCommitDTO commit) override {
    ++adoptions;
    auto accepted = commit.owner_commit(commit.context);
    if (!accepted.ok()) return accepted;
    std::swap(last_execute_trajectory, candidate_curve);
    auto* progress = const_cast<ExecutionProgressPublication*>(execution_progress_publication());
    (void)progress->publish(next_generation, 0, 0, 0);
    return Result<void>::Ok();
  }
};

TEST_F(ExecutionKernelTest,
       MovingReplacementAdoptionPreservesWorkerAndRejectsPriorHazardOrOwnerRefusal) {
  // 0: accepted handover; 1: a hazard already won before the owner CAS;
  // 2: the existing owner CAS rejects. Each is deterministically ordered by
  // the synchronous worker-ready event, with no clock sleeps.
  for (const int mode : {0, 1, 2, 3}) {
    SCOPED_TRACE(mode);
    planner.on_plan = {};
    context.on_validate = {};
    planner.next_result.trajectory = make_test_trajectory();
    for (auto& point : planner.next_result.trajectory->waypoints) {
      point.velocities_rad_s.assign(point.positions_rad.size(), 0.0);
      point.accelerations_rad_s2.assign(point.positions_rad.size(), 0.0);
    }
    context.next_validation = ValidationResultDTO{true, std::nullopt};
    KernelReplacementCommitExecution backend;
    backend.watchdog_clock = &clock;
    backend.enable_execution_progress_publication();
    ExecutionKernel owner(planner, context, backend, clock, scheduler, cache);
    owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
    owner.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                  {collision_engine});
    auto original = make_test_request("worker.original");
    original.cache_mode = CacheMode::kDisable;
    original.action_span = make_test_action_span();
    auto ordinary_request = prepared_motion_request(original);
    EXPECT_FALSE(ordinary_request.execution_request.initial_runtime_scale);
    EXPECT_FALSE(backend.runtime_velocity_scale_status().supported);
    const auto started = prepare_and_dispatch(owner, std::move(ordinary_request));
    ASSERT_TRUE(started.ok());
    {
      std::unique_lock lock(backend.observations_mutex);
      ASSERT_TRUE(backend.observations_changed.wait_for(lock, std::chrono::seconds(2),
                                                        [&]() { return backend.polls > 0; }));
    }
    const auto source = owner.CaptureMotionReplacementSource(started.value());
    ASSERT_TRUE(source.ok());
    planner.on_plan = [&](int) {
      const auto& moving = *planner.last_request.moving_replacement;
      auto curve = moving.source_prefix_trajectory;
      const auto end = curve.waypoints.back().time_from_start_s + 1.0;
      curve.waypoints.push_back({end, {1.0, 1.0}, {0.0, 0.0}, {0.0, 0.0}});
      curve.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{1.0, 1.0, 1.0};
      planner.next_result.trajectory = std::move(curve);
    };
    context.on_validate = [&]() {
      const auto seam = context.last_validation_options.retained_prefix->end_time_s;
      const auto end = planner.next_result.trajectory->waypoints.back().time_from_start_s;
      const auto metrics = [](double duration) {
        return TrajectoryValidationMetricsDTO{duration, {0.0, 0.0}, {0.0, 0.0}, 0.0};
      };
      context.next_validation = ValidationResultDTO{true, {}, metrics(end)};
      context.next_validation.moving_replacement =
          MovingReplacementValidationEvidenceDTO{metrics(seam), metrics(end - seam)};
    };
    std::mutex result_mutex;
    std::condition_variable result_changed;
    std::optional<Result<bool>> result;
    std::atomic<int> owner_commits{0};
    const auto subscription = owner.subscribe_events([&](const KernelEvent& event) {
      if (event.type != KernelEventType::kParticipantPrepared ||
          event.execution_id == started.value())
        return;
      {
        std::lock_guard lock(result_mutex);
        if (result) return;
      }
      const auto observed = owner.ObservePreparedMotion(event.execution_id);
      if (!observed.ok()) return;
      if (observed.value().state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed) {
        std::lock_guard lock(result_mutex);
        result = Result<bool>::Err(observed.value().error.value());
        result_changed.notify_all();
        return;
      }
      if (!observed.value().replacement ||
          observed.value().replacement->phase != PreparedMotionReplacementPhaseDTO::kReadyToCommit)
        return;
      if (mode == 1)
        (void)backend.claim_execution_progress_hazard(source.value().execution_generation);
      struct CommitState {
        std::atomic<int>& calls;
        int mode;
      } state{owner_commits, mode};
      const PreparedMotionReplacementCommitDTO commit{
          &state, [](void* opaque) noexcept -> Result<void> {
            auto& value = *static_cast<CommitState*>(opaque);
            ++value.calls;
            return value.mode == 2 ? Result<void>::Err(ErrorCode::kStaleGeneration)
                                   : Result<void>::Ok();
          }};
      auto committed =
          owner.CommitPreparedMotionReplacement(event.execution_id, source.value(), commit);
      {
        std::lock_guard lock(result_mutex);
        if (!result) result = std::move(committed);
      }
      result_changed.notify_all();
    });
    struct UnsubscribeBeforeLocals {
      ExecutionKernel& owner;
      SubscriptionHandle handle;
      bool active{true};
      ~UnsubscribeBeforeLocals() {
        if (active) owner.unsubscribe_events(handle);
      }
    } unsubscribe{owner, subscription};
    auto newcomer_request = make_test_request("worker.successor");
    newcomer_request.action_span = make_test_action_span();
    if (mode == 3) {
      // Timely submission is followed by recognition after the 250 ms
      // preparation deadline. Adoption still succeeds; another 60 ms must
      // exhaust the 350 ms execution watchdog measured from actual send.
      newcomer_request.timeouts.execution_timeout_s = 0.35;
      backend.before_ready = [&]() { clock.advance(300'000'000); };
    }
    auto newcomer = prepared_motion_request(newcomer_request);
    EXPECT_FALSE(newcomer.execution_request.initial_runtime_scale);
    newcomer.execution_request.binding.controller_goal_id =
        ControllerGoalId("worker.successor.goal");
    newcomer.replacement = ResolvedPreparedMotionReplacementContextDTO{source.value(), 250'000'000};
    const auto candidate = prepare_and_dispatch(owner, std::move(newcomer));
    ASSERT_TRUE(candidate.ok());
    {
      std::unique_lock lock(result_mutex);
      ASSERT_TRUE(result_changed.wait_for(lock, std::chrono::seconds(2),
                                          [&]() { return result.has_value(); }));
      ASSERT_TRUE((mode != 0 && mode != 3) || result->ok())
          << (result->ok() ? "" : result->error().message);
      EXPECT_EQ(result->ok(), mode == 0 || mode == 3);
    }
    EXPECT_EQ(backend.replacements, 1);
    EXPECT_EQ(owner_commits.load(), mode == 1 ? 0 : 1);
    EXPECT_EQ(backend.adoptions, mode == 1 ? 0 : 1);
    EXPECT_EQ(owner.get_status().execution_id,
              (mode == 0 || mode == 3) ? candidate.value() : started.value());
    owner.unsubscribe_events(subscription);
    unsubscribe.active = false;
    if (mode == 0)
      backend.complete();
    else if (mode == 3)
      clock.advance(60'000'000);
    else {
      (void)owner.fence_addressed_participant_stop(started.value());
      (void)backend.stop();
    }
    ASSERT_TRUE(owner.wait_for_completion(2'000'000'000));
    if (mode == 3) {
      ASSERT_TRUE(owner.get_status().fact.has_value());
      ASSERT_TRUE(owner.get_status().fact->error.has_value());
      EXPECT_EQ(owner.get_status().fact->error->code, kErrorExecutionTimeout);
    }
    EXPECT_EQ(backend.execute_count, 1);  // no second ordinary Execute/worker
    EXPECT_EQ(backend.runtime_velocity_scale_set_count, 0);
  }
}


class ScaleReplacementCommitExecution final : public KernelReplacementCommitExecution {
 public:
  mutable std::mutex scale_mutex;
  std::function<void()> scale_hook;
  std::atomic<int> scale_calls{0};
  double target{0.5};
  double applied{0.5};
  bool held{true};
  std::uint64_t acknowledgement{7U};
  void set_runtime_velocity_scale(double scale) override {
    ++scale_calls;
    if (scale_hook) scale_hook();
    std::lock_guard lock(scale_mutex);
    target = scale;
    if (!held) applied = scale;
  }
  RuntimeScaleMechanismStatusDTO runtime_velocity_scale_status() const override {
    std::lock_guard lock(scale_mutex);
    return {true, true, target, applied, 0.0, acknowledgement, 1, {}};
  }
  void release_hold() {
    std::lock_guard lock(scale_mutex);
    held = false;
    applied = target;
    ++acknowledgement;
  }
};

ResolvedPreparedMotionExecutionRequestDTO scale_replacement_request(const std::string& name,
                                                                    double seed) {
  auto request = prepared_motion_request(make_test_request(name));
  auto& binding = request.execution_request.binding;
  binding.operation_id = OperationId(name);
  binding.controller_goal_id = ControllerGoalId(name + ".goal");
  binding.runtime_scale_qualification = RuntimeScaleRouteQualificationDTO{
      binding.control_route_id, ScalingDomainId("scale.test"), EndpointRoleId("scale.command"),
      EndpointRoleId("scale.state"), {}, RuntimeScaleAcknowledgementKind::kAppliedState,
      4.0, 100'000'000};
  request.execution_request.cache_mode = CacheMode::kDisable;
  request.execution_request.action_span = make_test_action_span();
  request.execution_request.initial_runtime_scale = ResolvedRuntimeScaleCommandDTO{
      binding.operation_id, request.operation_revision, binding.participant_id,
      binding.controllable_component_id, binding.command_target_id, binding.capability_id,
      binding.control_domain_id, binding.control_domain_revision, binding.control_binding_id,
      binding.control_binding_revision, binding.controller_goal_id, binding.controller_goal_generation,
      binding.controller_topology_id, binding.topology_revision, binding.control_route_id,
      ScalingDomainId("scale.test"), {}, binding.resources, binding.active_control_configuration_id,
      binding.active_control_configuration_generation, RuntimeScaleGeneration(0U),
      RuntimeScaleGeneration(0U), seed};
  const auto& old = *request.ordinary_stop_plan;
  auto stop = MakePreparedParticipantStopPlan(
      request.stop_readiness.value().resolved_stop_base_plan,
      PreparedParticipantStopPlanContextDTO{
          binding.operation_id, request.operation_revision, binding.participant_id, old.program_id,
          old.program_revision, old.program_binding_id, old.program_run_id,
          old.participant_stop_profile, old.participant_stop_profile_purpose,
          old.motion_interrupt_scope_id, request.stop_node_category},
      request.binding.value().resources, request.retained_default_stop_limits);
  if (!stop.ok()) throw std::logic_error(stop.error().message);
  request.ordinary_stop_plan =
      std::make_shared<const PreparedParticipantStopPlanDTO>(std::move(stop.value()));
  return request;
}

TEST_F(ExecutionKernelTest,
       MovingReplacementTransfersLatestScaleWithoutSetScaleAndFencesPriorCommands) {
  // Same target, target changed during freeze, refused owner CAS, busy old
  // submission. All ordering uses worker callbacks and explicit gates.
  for (const int mode : {0, 1, 2, 3}) {
    SCOPED_TRACE(mode);
    planner.on_plan = {};
    context.on_validate = {};
    planner.next_result.trajectory = make_test_trajectory();
    for (auto& point : planner.next_result.trajectory->waypoints) {
      point.velocities_rad_s.assign(point.positions_rad.size(), 0.0);
      point.accelerations_rad_s2.assign(point.positions_rad.size(), 0.0);
    }
    context.next_validation = ValidationResultDTO{true, {}};
    ScaleReplacementCommitExecution backend;
    backend.watchdog_clock = &clock;
    backend.enable_execution_progress_publication();
    ExecutionKernel owner(planner, context, backend, clock, scheduler, cache);
    owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
    owner.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                  {collision_engine});
    auto original = scale_replacement_request("scale.original", 0.5);
    const auto original_command = *original.execution_request.initial_runtime_scale;
    const auto started = prepare_and_dispatch(owner, original);
    ASSERT_TRUE(started.ok()) << started.error().message;
    {
      std::unique_lock lock(backend.observations_mutex);
      ASSERT_TRUE(backend.observations_changed.wait_for(lock, std::chrono::seconds(2),
                                                        [&] { return backend.polls > 0; }));
    }
    const auto source = owner.CaptureMotionReplacementSource(started.value());
    ASSERT_TRUE(source.ok());
    planner.on_plan = [&](int) {
      auto curve = planner.last_request.moving_replacement->source_prefix_trajectory;
      const auto end = curve.waypoints.back().time_from_start_s + 1.0;
      curve.waypoints.push_back({end, {1.0, 1.0}, {0.0, 0.0}, {0.0, 0.0}});
      curve.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{1.0, 1.0, 1.0};
      planner.next_result.trajectory = std::move(curve);
    };
    context.on_validate = [&] {
      const auto seam = context.last_validation_options.retained_prefix->end_time_s;
      const auto end = planner.next_result.trajectory->waypoints.back().time_from_start_s;
      const auto metrics = [](double duration) {
        return TrajectoryValidationMetricsDTO{duration, {0.0, 0.0}, {0.0, 0.0}, 0.0};
      };
      context.next_validation = ValidationResultDTO{true, {}, metrics(end)};
      context.next_validation.moving_replacement =
          MovingReplacementValidationEvidenceDTO{metrics(seam), metrics(end - seam)};
    };
    auto candidate_request = scale_replacement_request("scale.successor", 0.5);
    const auto successor_command = *candidate_request.execution_request.initial_runtime_scale;
    candidate_request.replacement =
        ResolvedPreparedMotionReplacementContextDTO{source.value(), 250'000'000};
    std::mutex result_mutex;
    std::condition_variable changed;
    std::optional<Result<bool>> result;
    std::future<Result<ResolvedRuntimeScaleStatusDTO>> simultaneous_status;
    std::barrier query_started(2);
    const double latest = mode == 0 ? 0.5 : 0.8;
    const auto subscription = owner.subscribe_events([&](const KernelEvent& event) {
      if (event.type != KernelEventType::kParticipantPrepared || event.execution_id == started.value())
        return;
      {
        std::lock_guard lock(result_mutex);
        if (result) return;
      }
      const auto observed = owner.ObservePreparedMotion(event.execution_id);
      if (!observed.ok() || !observed.value().replacement ||
          observed.value().replacement->phase != PreparedMotionReplacementPhaseDTO::kReadyToCommit)
        return;
      auto update = original_command;
      update.requested_runtime_scale_generation = RuntimeScaleGeneration(1U);
      update.requested_scale = latest;
      std::future<Result<ResolvedRuntimeScaleAcceptedDTO>> in_flight;
      std::mutex gate_mutex;
      std::condition_variable gate_changed;
      bool entered = false, released = false;
      if (mode == 3) {
        backend.scale_hook = [&] {
          std::unique_lock lock(gate_mutex);
          entered = true;
          gate_changed.notify_all();
          gate_changed.wait(lock, [&] { return released; });
        };
        in_flight = std::async(std::launch::async, [&] { return owner.set_runtime_scale(update); });
        std::unique_lock lock(gate_mutex);
        gate_changed.wait(lock, [&] { return entered; });
      } else {
        EXPECT_TRUE(owner.set_runtime_scale(update).ok());
      }
      struct Context {
        int mode;
        ExecutionKernel* owner;
        const ResolvedRuntimeScaleCommandDTO* command;
        std::future<Result<ResolvedRuntimeScaleStatusDTO>>* observed;
        std::barrier<>* query_started;
      } state{mode, &owner, &successor_command, &simultaneous_status, &query_started};
      const PreparedMotionReplacementCommitDTO callback{
          &state, [](void* opaque) noexcept -> Result<void> {
            auto& state = *static_cast<Context*>(opaque);
            if (state.mode == 2) return Result<void>::Err(ErrorCode::kStaleGeneration);
            // Rendezvous before the query: adoption still owns the scale
            // mutex, so this exact successor read must remain pending.
            *state.observed = std::async(
                std::launch::async, [owner = state.owner, command = state.command,
                                    started = state.query_started] {
                  started->arrive_and_wait();
                  return owner->runtime_scale_status(command->operation_id,
                                                       command->control_binding_id);
                });
            state.query_started->arrive_and_wait();
            EXPECT_EQ(state.observed->wait_for(std::chrono::milliseconds(20)),
                      std::future_status::timeout);
            return Result<void>::Ok();
          }};
      auto adopted = owner.CommitPreparedMotionReplacement(event.execution_id, source.value(), callback);
      if (mode == 3) {
        EXPECT_TRUE(adopted.ok());
        if (adopted.ok()) { EXPECT_FALSE(adopted.value()); }
        EXPECT_EQ(backend.adoptions, 0);
        {
          std::lock_guard lock(gate_mutex);
          released = true;
        }
        gate_changed.notify_all();
        EXPECT_TRUE(in_flight.get().ok());
        backend.scale_hook = {};
        adopted = owner.CommitPreparedMotionReplacement(event.execution_id, source.value(), callback);
      }
      {
        std::lock_guard lock(result_mutex);
        result = std::move(adopted);
      }
      changed.notify_all();
    });
    struct Unsubscribe {
      ExecutionKernel& owner;
      SubscriptionHandle handle;
      ~Unsubscribe() { owner.unsubscribe_events(handle); }
    } unsubscribe{owner, subscription};
    const auto candidate = prepare_and_dispatch(owner, std::move(candidate_request));
    ASSERT_TRUE(candidate.ok()) << candidate.error().message;
    {
      std::unique_lock lock(result_mutex);
      ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return result.has_value(); }));
      EXPECT_EQ(result->ok(), mode != 2);
      if (result->ok()) { EXPECT_TRUE(result->value()); }
    }
    EXPECT_EQ(backend.scale_calls.load(), 2);  // initial and operator, never adoption
    const auto& active = mode == 2 ? original_command : successor_command;
    auto status = owner.runtime_scale_status(active.operation_id, active.control_binding_id);
    ASSERT_TRUE(status.ok()) << status.error().message;
    EXPECT_EQ(status.value().accepted_generation, RuntimeScaleGeneration(mode == 2 ? 1U : 0U));
    EXPECT_DOUBLE_EQ(status.value().applied_scale, 0.5);
    EXPECT_EQ(status.value().acknowledgement_sequence, 7U);
    EXPECT_EQ(status.value().state, latest == 0.5 ? RuntimeScaleApplicationState::kApplied
                                                 : RuntimeScaleApplicationState::kPending);
    if (mode != 2) {
      ASSERT_TRUE(simultaneous_status.valid());
      const auto simultaneous = simultaneous_status.get();
      ASSERT_TRUE(simultaneous.ok()) << simultaneous.error().message;
      EXPECT_EQ(simultaneous.value().operation_id, successor_command.operation_id);
      EXPECT_EQ(simultaneous.value().accepted_generation, RuntimeScaleGeneration(0U));
      auto stale = original_command;
      stale.expected_runtime_scale_generation = RuntimeScaleGeneration(1U);
      stale.requested_runtime_scale_generation = RuntimeScaleGeneration(2U);
      const auto refused = owner.set_runtime_scale(stale);
      ASSERT_FALSE(refused.ok());
      EXPECT_EQ(refused.error().details.at("reason"), "resolved_runtime_scale_binding_mismatch");
      backend.release_hold();
      status = owner.runtime_scale_status(active.operation_id, active.control_binding_id);
      ASSERT_TRUE(status.ok());
      EXPECT_EQ(status.value().state, RuntimeScaleApplicationState::kApplied);
      EXPECT_DOUBLE_EQ(status.value().applied_scale, latest);
      auto next = successor_command;
      next.requested_runtime_scale_generation = RuntimeScaleGeneration(1U);
      next.requested_scale = 0.6;
      EXPECT_TRUE(owner.set_runtime_scale(next).ok());
    }
    backend.complete();
    ASSERT_TRUE(owner.wait_for_completion(2'000'000'000));
  }
}

// Kernel-level fallback fixtures use the real preparation, sealed-plan and
// settlement gates. The controlled backend simulates physical return only;
// controller/adapter qualification remains in the participant fixture above.
class ColdPreparationExecution final : public ReplacementObservationExecution {
 public:
  std::atomic<int> preparations{0};
  PrepareParticipantResultDTO Prepare(const PreparedOperationParticipantDTO& product,
                                      std::uint64_t id) override {
    ++preparations;
    return ControlledExecution::Prepare(product, id);
  }
};

ResolvedPreparedMotionExecutionRequestDTO kernel_spring_authority(
    const std::string& name, std::optional<ProgramRunId> run_id = std::nullopt) {
  auto request = make_test_request(name);
  request.action_span = make_test_action_span();
  if (run_id) {
    request.binding.program_run_id = *run_id;
    request.action_span->program_run_id = run_id->value();
  }
  auto authority = prepared_motion_request(std::move(request));
  auto& binding = authority.execution_request.binding;
  binding.operation_id = OperationId(name);
  binding.controller_goal_id = ControllerGoalId(name + ".goal");
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "kernel.spring.return";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.deceleration_time_s = 1.0;
  profile.jerk_limits_enabled = false;
  for (const auto& resource : authority.binding.value().resources)
    profile.per_joint_max_deceleration.emplace(*resource.source_joint_name,
                                               AngularAccelerationDTO{2.0});
  auto plan = MakePreparedParticipantStopPlan(
      authority.stop_readiness.value().resolved_stop_base_plan,
      {binding.operation_id, authority.operation_revision, binding.participant_id,
       binding.program_id, binding.program_revision, binding.program_binding_id,
       binding.program_run_id, profile, ParticipantStopProfilePurposeDTO::kOperator, std::nullopt,
       authority.stop_node_category},
      authority.binding.value().resources, authority.retained_default_stop_limits);
  if (!plan.ok()) throw std::logic_error(plan.error().message);
  authority.ordinary_stop_plan =
      std::make_shared<const PreparedParticipantStopPlanDTO>(std::move(plan.value()));
  return authority;
}

ResolvedParticipantStopStageDTO kernel_spring_stage(
    const PreparedOperationParticipantDTO& product, const ExecutionGoalKey& goal,
    const ResolvedPreparedMotionExecutionRequestDTO& authority,
    StopId stop_id = StopId("kernel.spring.stop"),
    StopGeneration stop_generation = StopGeneration(1U)) {
  auto stage = ResolvedParticipantStopStageDTO{
      stop_id,
      stop_generation,
      product.operation_id,
      product.operation_revision,
      product.participant_id,
      goal.controller_goal_id,
      goal.controller_goal_generation,
      product.controllable_component_id,
      product.command_target_id,
      product.capability_id,
      product.binding_identity.control_domain_id,
      product.binding_identity.control_domain_revision,
      product.binding_identity.controller()->control_binding_id,
      product.binding_identity.controller()->control_binding_revision,
      product.controller().active_control_configuration_id,
      product.controller().active_control_configuration_generation,
      product.controller().controller_topology_id,
      product.controller().topology_revision,
      product.controller().control_route_id,
      std::nullopt,
      std::nullopt,
      product.controller().control_provider.runtime_epoch,
      product.resources,
      product.controller()
          .ordinary_stop_plan.stage_commands.front()
          .resolved_stage.containment_granularity,
      product.controller().stop_evidence_policy_id,
      product.controller().stop_evidence_policy_revision,
      product.controller().ordinary_stop_plan,
      0U,
      ContentDigest(kTestDigestA)};
  auto sealed = MakeResolvedParticipantStopStage(
      std::move(stage), {std::move(stop_id), stop_generation, 0, 1'500'000'000},
      authority.stop_readiness.value().resolved_stop_base_plan, authority.binding,
      authority.retained_default_stop_limits, authority.stop_node_category, product.resources);
  if (!sealed.ok()) throw std::logic_error(sealed.error().message);
  return std::move(sealed.value());
}

ParticipantStopStageResultDTO kernel_spring_result(const ResolvedParticipantStopStageDTO& stage) {
  ParticipantStopStageResultDTO result{stage,
                                       StopStageAttemptDispositionDTO::kDispatched,
                                       ParticipantStopStageOutcomeDTO::kSucceeded,
                                       MotionControlledStopMode::kSpringBackHold,
                                       std::nullopt,
                                       1U,
                                       stage.provider_runtime_epoch,
                                       1U,
                                       300'000'000,
                                       {},
                                       std::nullopt};
  for (const auto& requirement :
       stage.participant_stop_plan.stage_commands.front().resolved_stage.evidence_requirements) {
    result.evidence_items.push_back(
        {requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
         StopEvidenceItemStateDTO::kSatisfied, requirement.resolved_source,
         requirement.resolved_source.runtime_target.provider_runtime_epoch, 1U, 0,
         JsonValueDTO{true}, std::nullopt});
  }
  return result;
}

StopCertificateDTO kernel_spring_certificate(const ParticipantStopStageResultDTO& result) {
  const auto& stage = result.request;
  const StopScopeEntryKeyDTO key{
      stage.stop_id, stage.stop_generation,
      StopParticipantScopeKeyDTO{stage.operation_id, stage.participant_id}};
  const OperationRevision accepted_revision(stage.operation_revision.value() + 1U);
  const StopScopeEntryDTO entry{
      key,
      StopParticipantScopeFactsDTO{
          accepted_revision, stage.controllable_component_id, stage.command_target_id,
          stage.capability_id, stage.control_domain_id, stage.control_domain_revision,
          stage.control_binding_id, stage.control_binding_revision, stage.controller_goal_id,
          stage.controller_goal_generation, stage.active_control_configuration_id,
          stage.active_control_configuration_generation, stage.controller_topology_id,
          stage.topology_revision, stage.control_route_id, stage.provider_runtime_epoch,
          stage.resources, stage.containment_granularity,
          StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled},
      StopScopeEntryDispositionDTO::kStoppable,
      StopScopeDispatchStateDTO::kDispatched,
      stage.stop_evidence_policy_id,
      stage.stop_evidence_policy_revision,
      stage.participant_stop_plan.participant_stop_invocation_plan_digest,
      0};
  const auto reduced = ReduceStopTerminal(
      {StopCertificateId("kernel.spring.certificate"),
       StopCertificateRevision(1U),
       stage.stop_id,
       stage.stop_generation,
       {StopRequestedSelectorKindDTO::kOrdinaryOperations, ContentDigest(kTestDigestA),
        StopSelectorDTO{std::vector<StopOperationTargetDTO>{
            {stage.operation_id, stage.operation_revision, StopGeneration(0U)}}},
        std::nullopt},
       StopResolutionDispositionDTO::kExact,
       StopScopeResolutionReasonDTO::kExactCurrentScope,
       {},
       {{1U,
         StopResolutionDispositionDTO::kExact,
         StopScopeResolutionReasonDTO::kExactCurrentScope,
         ContentDigest(kTestDigestB),
         {},
         {{stage.operation_id, accepted_revision}},
         stage.resources}},
       {},
       {entry},
       {{key, 1U, 300'000'000, StopStageResultAuthorityDTO::kAuthoritative, result}},
       std::nullopt,
       std::nullopt,
       std::nullopt,
       300'000'000,
       std::nullopt});
  if (!reduced.ok()) throw std::logic_error(reduced.error().message);
  return reduced.value();
}

TEST_F(ExecutionKernelTest,
       AddressedContainmentAcknowledgesSettledFailureWithoutMakingItRetryable) {
  execution.next_terminal_state = ExecutionState::kFailed;
  execution.snapshot.error = ErrorDTO{kErrorExecutionAborted, "Controller aborted",
                                     {{"reason", "aborted_path_tolerance"}}};
  execution.execution_generation = 1U;
  ExecutionKernel owner(planner, context, execution, clock, scheduler, cache);
  owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
  owner.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                {collision_engine});

  const auto authority = kernel_spring_authority("ack.settled-failure");
  const auto started = prepare_and_dispatch(owner, authority);
  ASSERT_TRUE(started.ok()) << started.error().message;
  ASSERT_TRUE(owner.wait_for_completion(2'000'000'000));
  const auto failed = owner.get_status();
  ASSERT_EQ(failed.state, ExecutionState::kExecuting);
  ASSERT_TRUE(failed.fact.has_value());
  const auto original_fact = *failed.fact;
  ASSERT_EQ(original_fact.kind, ParticipantExecutionFactKind::kLocalFailure);
  ASSERT_EQ(original_fact.effect, ParticipantEffectKnowledge::kEffectSettled);
  ASSERT_TRUE(original_fact.error.has_value());

  const auto observed = owner.ObservePreparedMotion(started.value());
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  ASSERT_TRUE(observed.value().last_dispatched_participant);
  const auto& binding = authority.execution_request.binding;
  const auto stage = kernel_spring_stage(
      *observed.value().last_dispatched_participant,
      {binding.controller_goal_id, binding.controller_goal_generation}, authority,
      StopId("ack.settled-failure.stop"), StopGeneration(1U));
  EXPECT_EQ(owner.fence_addressed_participant_stop(started.value()),
            AddressedParticipantStopFenceDisposition::kDispatchCommitted);
  const auto containment = kernel_spring_result(stage);
  const auto certificate = kernel_spring_certificate(containment);
  ASSERT_EQ(certificate.result, StopCertificateResultDTO::kSucceededNominal);
  const auto backend_containment = execution.execute_controlled_stop(
      *authority.ordinary_stop_plan->participant_stop_profile);
  ASSERT_TRUE(backend_containment.backend_stop_confirmed);
  const StopCertificateRefDTO reference{
      certificate.stop_certificate_id, certificate.stop_certificate_revision,
      certificate.stop_id, certificate.stop_generation};

  const auto settled = owner.settle_addressed_participant_stop(
      {started.value(), reference, containment, OperationParticipantPayloadKind::kMotion, false});
  ASSERT_TRUE(settled.ok()) << settled.error().message;
  EXPECT_EQ(settled.value().terminal_fact, std::optional(original_fact));
  const auto& assessment = settled.value().recovery_assessment;
  EXPECT_EQ(assessment.execution_id, started.value());
  EXPECT_EQ(assessment.execution_state, ExecutionState::kFailed);
  EXPECT_FALSE(assessment.retry_admissible);
  EXPECT_FALSE(assessment.retry_eligibility.eligible);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kActionRequired);
  EXPECT_TRUE(assessment.eligible_close);
  EXPECT_TRUE(assessment.backend_stop_confirmed);
  EXPECT_EQ(assessment.stop_certificate_id, reference.stop_certificate_id.value());
  EXPECT_EQ(assessment.stop_certificate_revision,
            reference.stop_certificate_revision.value());
  EXPECT_EQ(assessment.stop_owner_generation, reference.stop_generation.value());
  EXPECT_TRUE(std::find(assessment.allowed_actions.begin(), assessment.allowed_actions.end(),
                        RecoveryAction::kCloseOperation) != assessment.allowed_actions.end());
  EXPECT_EQ(owner.get_status().state, failed.state);
  EXPECT_EQ(owner.get_status().fact, std::optional(original_fact));

  const auto replay = owner.settle_addressed_participant_stop(
      {started.value(), reference, containment, OperationParticipantPayloadKind::kMotion, false});
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().terminal_fact, std::optional(original_fact));
  EXPECT_EQ(replay.value().recovery_assessment.stop_certificate_id,
            reference.stop_certificate_id.value());
  auto conflicting = reference;
  conflicting.stop_certificate_revision = StopCertificateRevision(
      reference.stop_certificate_revision.value() + 1U);
  const auto conflict = owner.settle_addressed_participant_stop(
      {started.value(), conflicting, containment, OperationParticipantPayloadKind::kMotion, false});
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().details.at("reason"), "addressed_motion_stop_identity_conflict");

  const KernelRecoveryActionRequestDTO close{
      "ack.settled-failure.close", RecoveryAction::kCloseOperation, "operator",
      assessment.assessment_revision, assessment.control_command_epoch, assessment.execution_id,
      assessment.latch_id, assessment.stop_certificate_id, assessment.stop_owner_generation,
      "acknowledge contained failure", false};
  const auto resets = execution.reset_count;
  const auto stops = execution.stop_count;
  const auto retired = owner.apply_recovery_action(close);
  ASSERT_TRUE(retired.accepted) << (retired.error ? retired.error->message : "closure refused");
  EXPECT_EQ(owner.get_status().state, ExecutionState::kIdle);
  EXPECT_EQ(execution.execute_count, 1);
  EXPECT_EQ(execution.reset_count, resets);
  EXPECT_EQ(execution.stop_count, stops);
  EXPECT_EQ(execution.retire_count, 1);
}

// F-6a (W): the real engine after a nominal addressed Stop settles a moving
// motion (a first-stage gate or Program-requested Stop). The execute worker's
// terminal cleanup is held after the backend ends: settlement is accepted, but
// the assessment says waiting (terminal_cleanup_pending), not retry_ready
// with eligible_close false, so the Runner rereads instead of retaining the
// operation (`participant_engine_busy`). Cleanup completion republishes it.
class HeldCleanupExecution final : public ReplacementObservationExecution {
 public:
  std::mutex hold_mutex;
  std::condition_variable hold_changed;
  bool ended{};
  bool released{};

  ExecutionHandle Dispatch(const PreparedOperationParticipantDTO& product,
                           const ControllerGoalId& goal,
                           ControllerGoalGeneration generation) override {
    auto handle = ReplacementObservationExecution::Dispatch(product, goal, generation);
    auto wait = std::move(handle.wait_for_completion);
    handle.wait_for_completion = [this, wait = std::move(wait)](std::int64_t timeout) {
      const bool done = wait(timeout);
      if (!done) return false;
      std::unique_lock lock(hold_mutex);
      ended = true;
      hold_changed.notify_all();
      hold_changed.wait(lock, [this] { return released; });
      return true;
    };
    return handle;
  }
  bool WaitEnded() {
    std::unique_lock lock(hold_mutex);
    return hold_changed.wait_for(lock, std::chrono::seconds(2), [this] { return ended; });
  }
  void Release() {
    std::lock_guard lock(hold_mutex);
    released = true;
    hold_changed.notify_all();
  }
};

TEST_F(ExecutionKernelTest, AddressedNominalStopWaitsForTerminalCleanupThenIsCloseEligible) {
  HeldCleanupExecution backend;
  backend.enable_execution_progress_publication();
  struct ReleaseOnExit {
    HeldCleanupExecution& backend;
    ~ReleaseOnExit() { backend.Release(); }
  } release_on_exit{backend};
  ExecutionKernel owner(planner, context, backend, clock, scheduler, cache);
  owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
  owner.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                {collision_engine});
  const auto authority = kernel_spring_authority("gate.moving");
  const auto started = prepare_and_dispatch(owner, authority);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(backend.wait_until_execute_called());
  ASSERT_TRUE(wait_for_state(owner, ExecutionState::kExecuting));
  const auto observed = owner.ObservePreparedMotion(started.value());
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().last_dispatched_participant);
  const auto& binding = authority.execution_request.binding;
  const auto stage = kernel_spring_stage(
      *observed.value().last_dispatched_participant,
      {binding.controller_goal_id, binding.controller_goal_generation}, authority,
      StopId("gate.moving.stop"), StopGeneration(1U));
  EXPECT_EQ(owner.fence_addressed_participant_stop(started.value()),
            AddressedParticipantStopFenceDisposition::kDispatchCommitted);
  const auto containment = kernel_spring_result(stage);
  const auto certificate = kernel_spring_certificate(containment);
  ASSERT_EQ(certificate.result, StopCertificateResultDTO::kSucceededNominal);
  backend.publish_observed_state(clock.now(), {0.0, 0.0}, {0.0, 0.0});
  ASSERT_TRUE(
      backend.execute_controlled_stop(*authority.ordinary_stop_plan->participant_stop_profile)
          .backend_stop_confirmed);
  ASSERT_TRUE(backend.WaitEnded());  // The backend ended; worker cleanup is held.
  const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                        certificate.stop_certificate_revision, certificate.stop_id,
                                        certificate.stop_generation};
  const auto settled = owner.settle_addressed_participant_stop(
      {started.value(), reference, containment, OperationParticipantPayloadKind::kMotion, false});
  // Settlement accepts the certified Stop while cleanup is pending.
  ASSERT_TRUE(settled.ok()) << settled.error().message;
  const auto& held = settled.value().recovery_assessment;
  EXPECT_EQ(held.execution_state, ExecutionState::kStopped);
  EXPECT_EQ(held.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(held.reason_code, reason_codes::kRecoveryTerminalCleanupPending);
  EXPECT_TRUE(stopped_awaiting_terminal_cleanup(held));
  EXPECT_TRUE(held.backend_stop_confirmed);
  EXPECT_EQ(held.stop_certificate_id, reference.stop_certificate_id.value());
  EXPECT_FALSE(held.eligible_close);
  EXPECT_FALSE(held.retry_admissible);
  backend.Release();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto current = owner.get_recovery_assessment();
  // The worker's terminal cleanup schedules the cached assessment's refresh.
  while (!current.eligible_close && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    scheduler.advance_to(clock.now());
    current = owner.get_recovery_assessment();
  }
  EXPECT_TRUE(current.eligible_close);
  EXPECT_EQ(current.status, RecoveryAssessmentStatus::kRetryReady);
  EXPECT_EQ(current.execution_state, ExecutionState::kStopped);
  EXPECT_EQ(current.stop_certificate_id, reference.stop_certificate_id.value());
}

TEST_F(ExecutionKernelTest, SpringColdProductWaitsBeyondPreparationBudgetWithoutReplanning) {
  ColdPreparationExecution backend;
  backend.enable_execution_progress_publication();
  ExecutionKernel owner(planner, context, backend, clock, scheduler, cache);
  owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
  owner.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                {collision_engine});
  const auto authority = kernel_spring_authority("cold.incumbent");
  const auto started = prepare_and_dispatch(owner, authority);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(backend.wait_until_execute_called());
  {
    std::unique_lock lock(backend.observations_mutex);
    ASSERT_TRUE(backend.observations_changed.wait_for(lock, std::chrono::seconds(2),
                                                      [&]() { return backend.polls > 0; }));
  }
  const auto source = owner.CaptureMotionReplacementSource(started.value());
  ASSERT_TRUE(source.ok());
  const auto observed = owner.ObservePreparedMotion(started.value());
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().last_dispatched_participant);
  const auto stage = kernel_spring_stage(*observed.value().last_dispatched_participant,
                                         source.value().goal, authority);
  SpringBackReturnPointDTO point;
  point.source_goal = source.value().goal;
  point.binding_epoch = source.value().binding_epoch;
  point.trip = source.value().controller_sample;
  point.initial.joint_names = {"j1", "j2"};
  point.initial.path_time_rate = 1.0;
  point.initial.spring_back_return_point = {0.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
  point.canonical_return_path_time_s = 0.0;
  EXPECT_EQ(owner.fence_addressed_participant_stop(started.value()),
            AddressedParticipantStopFenceDisposition::kDispatchCommitted);

  auto candidate = kernel_spring_authority("cold.successor");
  candidate.replacement = ResolvedPreparedMotionReplacementContextDTO{
      {},
      250'000'000,
      ResolvedPreparedMotionSpringReturnContextDTO{
          std::make_shared<const ResolvedParticipantStopStageDTO>(stage),
          std::make_shared<const SpringBackReturnPointDTO>(point)}};
  // The raw helper point is insufficient even while the exact Stop is fenced.
  auto unqualified = candidate;
  point.canonical_return_path_time_s.reset();
  unqualified.replacement->spring_return->point =
      std::make_shared<const SpringBackReturnPointDTO>(point);
  EXPECT_FALSE(owner.PrepareMotion(std::move(unqualified)).ok());
  std::atomic<bool> cold_completed{false};
  const auto completion = owner.subscribe_events([&](const KernelEvent& event) {
    if (event.type == KernelEventType::kParticipantPrepared &&
        event.execution_id != started.value())
      cold_completed.store(true);
  });
  struct RemoveColdSubscription {
    ExecutionKernel& owner;
    SubscriptionHandle handle;
    ~RemoveColdSubscription() { owner.unsubscribe_events(handle); }
  } remove_completion{owner, completion};
  // Actual scene retention consumes 50 ms of this explicit 250 ms budget
  // before the spring candidate is constructed. The planner must receive
  // the residual 200 ms, rather than its authored ten-second timeout.
  const auto submitting_thread = std::this_thread::get_id();
  const auto cold_retention_elapsed = std::make_shared<std::atomic<bool>>(false);
  collision_world_publisher.on_lease_retain = [this, submitting_thread, cold_retention_elapsed] {
    if (std::this_thread::get_id() != submitting_thread &&
        !cold_retention_elapsed->exchange(true))
      clock.advance(50'000'000);
  };
  const auto preparing = owner.PrepareMotion(std::move(candidate));
  ASSERT_TRUE(preparing.ok());
  std::shared_ptr<const PreparedOperationParticipantDTO> cold_product;
  const auto cold_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < cold_deadline) {
    const auto snapshot = owner.ObservePreparedMotion(preparing.value());
    ASSERT_TRUE(snapshot.ok());
    ASSERT_FALSE(snapshot.value().error) << snapshot.value().error->message;
    if (snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kColdPrepared &&
        cold_completed.load()) {
      cold_product = snapshot.value().participant;
      EXPECT_FALSE(snapshot.value().backend_prepare_result);
      break;
    }
    std::this_thread::yield();
  }
  ASSERT_TRUE(cold_product);
  collision_world_publisher.on_lease_retain = {};
  EXPECT_TRUE(cold_retention_elapsed->load());
  const auto& cold_motion = std::get<PreparedMotionExecutionDTO>(cold_product->payload);
  EXPECT_DOUBLE_EQ(cold_motion.plan_request.planning_timeout_s, 0.2);
  EXPECT_DOUBLE_EQ(planner.last_request.planning_timeout_s, 0.2);
  EXPECT_FALSE(cold_motion.plan_request.moving_replacement.has_value());
  EXPECT_EQ(planner.plan_count, 2);
  EXPECT_EQ(backend.preparations.load(), 1);
  EXPECT_EQ(backend.execute_count, 1);
  EXPECT_EQ(owner.get_status().execution_id, started.value());
  const auto containment = kernel_spring_result(stage);
  const auto certificate = kernel_spring_certificate(containment);
  ASSERT_EQ(certificate.result, StopCertificateResultDTO::kSucceededNominal);
  EXPECT_FALSE(owner.PrepareColdMotionAfterStop(preparing.value(), certificate).ok());

  // Computation completed before 250 ms. Physical spring return finishes
  // another 300 ms later, inside its independent Stop deadline; this must not expire the
  // already sealed cold product or start another planner call (DX1179).
  clock.advance(300'000'000);
  backend.publish_observed_state(clock.now(), {0.0, 0.0}, {0.0, 0.0});
  ASSERT_TRUE(
      backend.execute_controlled_stop(*authority.ordinary_stop_plan->participant_stop_profile)
          .backend_stop_confirmed);
  const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                        certificate.stop_certificate_revision, certificate.stop_id,
                                        certificate.stop_generation};
  const auto settled = owner.settle_addressed_participant_stop(
      {started.value(), reference, containment, OperationParticipantPayloadKind::kMotion, false});
  ASSERT_TRUE(settled.ok()) << settled.error().message;
  const auto first_settlement = settled.value();
  const auto replay = owner.settle_addressed_participant_stop(
      {started.value(), reference, containment, OperationParticipantPayloadKind::kMotion, false});
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().aggregate_stop_certificate, first_settlement.aggregate_stop_certificate);
  auto conflicting_reference = reference;
  conflicting_reference.stop_certificate_revision = StopCertificateRevision(2U);
  const auto conflict = owner.settle_addressed_participant_stop(
      {started.value(), conflicting_reference, containment,
       OperationParticipantPayloadKind::kMotion, false});
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().details.at("reason"), "addressed_motion_stop_identity_conflict");
  auto wrong_certificate = certificate;
  wrong_certificate.stop_certificate_revision = StopCertificateRevision(2U);
  EXPECT_FALSE(owner.PrepareColdMotionAfterStop(preparing.value(), wrong_certificate).ok());
  const auto activated = owner.PrepareColdMotionAfterStop(preparing.value(), certificate);
  ASSERT_TRUE(activated.ok()) << activated.error().message;
  ASSERT_TRUE(activated.value());
  // Activation starts the ordinary worker; ColdPrepared remains observable
  // until its backend Prepare finishes. The ordinary-only helper above stops
  // waiting on that state, so observe the real handoff before releasing it.
  std::shared_ptr<const PreparedOperationParticipantDTO> ordinary_product;
  const auto prepared_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < prepared_deadline) {
    const auto snapshot = owner.ObservePreparedMotion(preparing.value());
    ASSERT_TRUE(snapshot.ok()) << snapshot.error().message;
    ASSERT_FALSE(snapshot.value().error) << snapshot.value().error->message;
    if (snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kPrepared) {
      ASSERT_TRUE(snapshot.value().backend_prepare_result);
      ordinary_product = snapshot.value().participant;
      break;
    }
    ASSERT_TRUE(snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kColdPrepared ||
                snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kPreparing);
    std::this_thread::yield();
  }
  ASSERT_TRUE(ordinary_product);
  ASSERT_EQ(ordinary_product->prepared_contract_digest, cold_product->prepared_contract_digest);
  const auto released = owner.DispatchPreparedMotion(
      preparing.value(), ordinary_product->prepared_contract_digest);
  ASSERT_TRUE(released.ok()) << released.error().message;
  ASSERT_TRUE(backend.wait_until_execute_count(2));
  ASSERT_TRUE(wait_for_state(owner, ExecutionState::kExecuting));
  EXPECT_EQ(collision_runtime_engine->runtime_query_prepare_count.load(), 2);
  const auto tick = owner.collision_check_tick();
  ASSERT_TRUE(tick.ok()) << tick.error().message;
  EXPECT_EQ(owner.get_metrics().runtime_collision_ticks_completed, 1U);
  EXPECT_EQ(owner.get_metrics().runtime_collision_ticks_faulted, 0U);
  EXPECT_FALSE(owner.get_status().fact);
  EXPECT_EQ(planner.plan_count, 2);
  EXPECT_EQ(backend.preparations.load(), 2);
  const auto reused = owner.ObservePreparedMotion(preparing.value());
  ASSERT_TRUE(reused.ok());
  ASSERT_TRUE(reused.value().participant);
  EXPECT_EQ(reused.value().participant->prepared_contract_digest,
            cold_product->prepared_contract_digest);
  EXPECT_EQ(owner.get_status().execution_id, preparing.value());
  backend.complete();
  ASSERT_TRUE(owner.wait_for_completion(2'000'000'000));
  EXPECT_EQ(backend.execute_count, 2);
  const auto acknowledged = owner.acknowledge_completed(preparing.value());
  ASSERT_TRUE(acknowledged.accepted);
  EXPECT_EQ(owner.get_status().state, ExecutionState::kIdle);

  // A Program Reset with an empty operation scope invokes no participant
  // cleanup. Reuse this same participant for the next Run without inventing a
  // local reset that would hide retention of the first Run's Stop receipt.
  for (const bool next_run : {false, true}) {
    SCOPED_TRACE(next_run ? "distinct Run" : "same Run");
    const auto next_authority = kernel_spring_authority(
        next_run ? "cold.next_run.incumbent" : "cold.same_run.incumbent",
        next_run ? ProgramRunId("cold.next.run")
                 : authority.execution_request.binding.program_run_id);
    backend.publish_observed_state(clock.now(), {0.0, 0.0}, {0.0, 0.0});
    const auto next_started = prepare_and_dispatch(owner, next_authority);
    ASSERT_TRUE(next_started.ok()) << next_started.error().message;
    ASSERT_TRUE(backend.wait_until_execute_count(next_run ? 4 : 3));
    ASSERT_TRUE(wait_for_state(owner, ExecutionState::kExecuting));
    // Executing is published by the backend just before the worker records
    // the dispatched product; wait for that record, not only for the state.
    auto next_observed = owner.ObservePreparedMotion(next_started.value());
    const auto record_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (next_observed.ok() && !next_observed.value().last_dispatched_participant &&
           std::chrono::steady_clock::now() < record_deadline) {
      std::this_thread::yield();
      next_observed = owner.ObservePreparedMotion(next_started.value());
    }
    ASSERT_TRUE(next_observed.ok()) << next_observed.error().message;
    ASSERT_TRUE(next_observed.value().last_dispatched_participant);
    const auto& next_binding = next_authority.execution_request.binding;
    const auto next_stage = kernel_spring_stage(
        *next_observed.value().last_dispatched_participant,
        {next_binding.controller_goal_id, next_binding.controller_goal_generation}, next_authority,
        StopId(next_run ? "kernel.spring.next_run.stop" : "kernel.spring.same_run.stop"),
        StopGeneration(next_run ? 4U : 3U));
    EXPECT_EQ(owner.fence_addressed_participant_stop(next_started.value()),
              AddressedParticipantStopFenceDisposition::kDispatchCommitted);
    ASSERT_TRUE(backend.execute_controlled_stop(
                           *next_authority.ordinary_stop_plan->participant_stop_profile)
                    .backend_stop_confirmed);
    const auto next_containment = kernel_spring_result(next_stage);
    const auto next_certificate = kernel_spring_certificate(next_containment);
    const StopCertificateRefDTO next_reference{
        next_certificate.stop_certificate_id, next_certificate.stop_certificate_revision,
        next_certificate.stop_id, next_certificate.stop_generation};
    const auto next_settled = owner.settle_addressed_participant_stop(
        {next_started.value(), next_reference, next_containment,
         OperationParticipantPayloadKind::kMotion, false});
    ASSERT_TRUE(next_settled.ok()) << next_settled.error().message;
    EXPECT_EQ(next_settled.value().execution_id, next_started.value());
    EXPECT_EQ(next_settled.value().aggregate_stop_certificate, next_reference);
    EXPECT_NE(next_started.value(), first_settlement.execution_id);
    EXPECT_EQ(next_authority.execution_request.binding.program_run_id ==
                  authority.execution_request.binding.program_run_id,
              !next_run);
    const auto old_replay = owner.settle_addressed_participant_stop(
        {started.value(), reference, containment, OperationParticipantPayloadKind::kMotion, false});
    ASSERT_FALSE(old_replay.ok());
    EXPECT_EQ(old_replay.error().details.at("reason"), "addressed_motion_stop_execution_mismatch");
    EXPECT_EQ(first_settlement.execution_id, started.value());
    EXPECT_EQ(first_settlement.aggregate_stop_certificate, reference);
    EXPECT_EQ(first_settlement.operation_id, stage.operation_id);
  }
}

// SDK-like identity retention is deliberately stricter than the historical
// controlled backend: retirement never makes a previously sent goal reusable.
class StrictSentSpringExecution final : public ReplacementObservationExecution {
 public:
  IClock* source_clock{};
  std::atomic<int> preparations{0};
  std::atomic<int> replacement_sends{0};
  std::atomic<bool> effects_contained{false};
  std::atomic<bool> permit_sent_retirement{false};
  std::atomic<int> sent_retirement_refusals{0};
  std::atomic<std::uint64_t> sent_preparation_id{0U};
  std::size_t maximum_live_goals{};

  PrepareParticipantResultDTO Prepare(const PreparedOperationParticipantDTO& product,
                                      std::uint64_t id) override {
    {
      std::lock_guard lock(keys_mutex_);
      if (id <= highest_preparation_id_)
        return MakePrepareParticipantResult(
                   product, PrepareParticipantResultStateDTO::kRejected,
                   ErrorDTO{
                       kErrorExecutionRejected, "Preparation identity was already retired", {}})
            .value();
      highest_preparation_id_ = id;
    }
    ++preparations;
    return ControlledExecution::Prepare(product, id);
  }
  PrepareParticipantResultDTO PrepareReplacement(
      const PreparedOperationParticipantDTO& product, std::uint64_t id,
      const PreparedMotionReplacementSourceDTO&) override {
    return Prepare(product, id);
  }
  ExecutionHandle Dispatch(const PreparedOperationParticipantDTO& product,
                           const ControllerGoalId& goal,
                           ControllerGoalGeneration generation) override {
    remember_key({goal, generation});
    current_handle_ = ReplacementObservationExecution::Dispatch(product, goal, generation);
    return current_handle_;
  }
  PreparedMotionReplacementDispatchDTO DispatchReplacement(
      const PreparedMotionReplacementSourceDTO& source, std::uint64_t id,
      const PreparedOperationParticipantDTO& product, const ExecutionGoalKey& goal,
      std::int64_t) override {
    remember_key(goal);
    sent_preparation_id.store(id);
    ++replacement_sends;
    auto handle = current_handle_;
    handle.execution_generation = source.execution_generation + 1U;
    handle.controller_goal_id = goal.controller_goal_id;
    handle.controller_goal_generation = goal.controller_goal_generation;
    handle.prepared_contract_digest = product.prepared_contract_digest;
    return {{ExecutionPortDispatch::kDispatched, {}, {}}, std::move(handle), source_clock->now()};
  }
  Result<PreparedMotionReplacementObservationDTO> ObserveReplacement(
      const PreparedMotionReplacementSourceDTO&, std::uint64_t) const override {
    return Result<PreparedMotionReplacementObservationDTO>::Ok(
        {PreparedMotionReplacementPhaseDTO::kPossiblySent, {}, {}, {}, {}});
  }
  ControlledStopResultDTO execute_controlled_stop(
      const MotionInterruptStopProfileDTO& profile) override {
    auto result = ControlledExecution::execute_controlled_stop(profile);
    if (result.backend_stop_confirmed && !result.error) {
      effects_contained.store(true);
      std::lock_guard lock(keys_mutex_);
      live_goals_ = 0U;
    }
    return result;
  }
  Result<void> RetirePreparedExecution(std::uint64_t id) override {
    if (id == sent_preparation_id.load() &&
        (!effects_contained.load() || !permit_sent_retirement.load())) {
      ++sent_retirement_refusals;
      return Result<void>::Err(
          ErrorDTO{kErrorExecutionRejected, "The sent physical membership is not retired", {}});
    }
    return ControlledExecution::RetirePreparedExecution(id);
  }

 private:
  void remember_key(const ExecutionGoalKey& key) {
    std::lock_guard lock(keys_mutex_);
    const auto identity =
        std::make_pair(key.controller_goal_id.value(), key.controller_goal_generation.value());
    if (!sent_keys_.insert(identity).second)
      throw std::logic_error("SDK goal key cannot be reused after retirement");
    maximum_live_goals = std::max(maximum_live_goals, ++live_goals_);
  }
  std::mutex keys_mutex_;
  std::set<std::pair<std::string, std::uint64_t>> sent_keys_;
  std::size_t live_goals_{};
  std::uint64_t highest_preparation_id_{};
  ExecutionHandle current_handle_;
};

TEST_F(ExecutionKernelTest, SentSpringColdResultSharesOwnerUntilExactRetirementAndFreshDispatch) {
  // Successful promotion and operator abandonment share the exact sent debt.
  for (const bool abandon : {false, true}) {
    SCOPED_TRACE(abandon);
    planner.on_plan = {};
    context.on_validate = {};
    planner.next_result.trajectory = make_test_trajectory();
    for (auto& waypoint : planner.next_result.trajectory->waypoints) {
      waypoint.velocities_rad_s.assign(2U, 0.0);
      waypoint.accelerations_rad_s2.assign(2U, 0.0);
    }
    context.next_validation = ValidationResultDTO{true, std::nullopt};
    StrictSentSpringExecution backend;
    backend.source_clock = &clock;
    backend.enable_execution_progress_publication();
    ExecutionKernel owner(planner, context, backend, clock, scheduler, cache);
    owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
    owner.set_collision_resources(collision_world_publisher, collision_runtime_engine,
                                  {collision_engine});
    auto original = kernel_spring_authority("sent.spring.original");
    original.execution_request.cache_mode = CacheMode::kDisable;
    const auto started = prepare_and_dispatch(owner, original);
    ASSERT_TRUE(started.ok());
    ASSERT_TRUE(backend.wait_until_execute_called());
    {
      std::unique_lock lock(backend.observations_mutex);
      ASSERT_TRUE(backend.observations_changed.wait_for(lock, std::chrono::seconds(2),
                                                        [&] { return backend.polls > 0; }));
    }
    const auto source = owner.CaptureMotionReplacementSource(started.value());
    ASSERT_TRUE(source.ok());
    const auto source_snapshot = owner.ObservePreparedMotion(started.value());
    ASSERT_TRUE(source_snapshot.ok());
    planner.on_plan = [&](int) {
      if (!planner.last_request.moving_replacement) return;
      auto curve = planner.last_request.moving_replacement->source_prefix_trajectory;
      curve.waypoints.push_back(
          {curve.waypoints.back().time_from_start_s + 1.0, {1.0, 1.0}, {0.0, 0.0}, {0.0, 0.0}});
      curve.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{1.0, 1.0, 1.0};
      planner.next_result.trajectory = std::move(curve);
    };
    context.on_validate = [&] {
      if (!context.last_validation_options.retained_prefix) {
        context.next_validation = ValidationResultDTO{true, std::nullopt};
        return;
      }
      const auto seam = context.last_validation_options.retained_prefix->end_time_s;
      const auto end = planner.next_result.trajectory->waypoints.back().time_from_start_s;
      const auto metrics = [](double duration) {
        return TrajectoryValidationMetricsDTO{duration, {0.0, 0.0}, {0.0, 0.0}, 0.0};
      };
      context.next_validation = ValidationResultDTO{true, {}, metrics(end)};
      context.next_validation.moving_replacement =
          MovingReplacementValidationEvidenceDTO{metrics(seam), metrics(end - seam)};
    };
    auto sent_authority = kernel_spring_authority("sent.spring.candidate");
    sent_authority.execution_request.cache_mode = CacheMode::kDisable;
    sent_authority.replacement =
        ResolvedPreparedMotionReplacementContextDTO{source.value(), 250'000'000};
    const auto sent = prepare_and_dispatch(owner, sent_authority);
    ASSERT_TRUE(sent.ok());
    const auto send_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (backend.replacement_sends.load() == 0 &&
           std::chrono::steady_clock::now() < send_deadline)
      std::this_thread::yield();
    ASSERT_EQ(backend.replacement_sends.load(), 1);
    const auto old = owner.ObservePreparedMotion(sent.value());
    ASSERT_TRUE(old.ok());
    ASSERT_TRUE(old.value().backend_prepare_result);
    ASSERT_TRUE(old.value().last_dispatched_participant);
    ASSERT_TRUE(old.value().replacement);
    EXPECT_EQ(old.value().replacement->phase, PreparedMotionReplacementPhaseDTO::kPossiblySent);
    const auto stage = kernel_spring_stage(*source_snapshot.value().last_dispatched_participant,
                                           source.value().goal, original);
    SpringBackReturnPointDTO point;
    point.source_goal = source.value().goal;
    point.binding_epoch = source.value().binding_epoch;
    point.trip = source.value().controller_sample;
    point.initial.joint_names = {"j1", "j2"};
    point.initial.path_time_rate = 1.0;
    point.initial.spring_back_return_point = {0.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    point.canonical_return_path_time_s = 0.0;
    EXPECT_EQ(owner.fence_addressed_participant_stop(started.value()),
              AddressedParticipantStopFenceDisposition::kDispatchCommitted);
    auto future = sent_authority;
    // A shorter authored planning timeout remains the cap for the actual
    // attached cold candidate, independent of its full preparation budget.
    future.execution_request.timeouts.planning_timeout_s = 0.1;
    future.execution_request.binding.controller_goal_generation = ControllerGoalGeneration(2U);
    future.replacement = ResolvedPreparedMotionReplacementContextDTO{
        {},
        250'000'000,
        ResolvedPreparedMotionSpringReturnContextDTO{
            std::make_shared<const ResolvedParticipantStopStageDTO>(stage),
            std::make_shared<const SpringBackReturnPointDTO>(point), sent.value()}};
    const int before_cold = planner.plan_count;
    const auto attached = owner.PrepareMotion(future);
    ASSERT_TRUE(attached.ok()) << attached.error().message;
    EXPECT_EQ(attached.value(), sent.value());
    ResolvedPreparedMotionSpringColdResultDTO cold{};
    bool cold_observed = false;
    const auto cold_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < cold_deadline) {
      const auto snapshot = owner.ObservePreparedMotion(sent.value());
      ASSERT_TRUE(snapshot.ok());
      ASSERT_TRUE(snapshot.value().spring_cold_result);
      cold = *snapshot.value().spring_cold_result;
      cold_observed = true;
      ASSERT_FALSE(cold.error) << cold.error->message;
      EXPECT_EQ(snapshot.value().backend_prepare_result, old.value().backend_prepare_result);
      EXPECT_EQ(snapshot.value().last_dispatched_participant,
                old.value().last_dispatched_participant);
      EXPECT_EQ(snapshot.value().state, old.value().state);
      if (cold.state == ResolvedPreparedMotionExecutionStateDTO::kColdPrepared) break;
      std::this_thread::yield();
    }
    ASSERT_TRUE(cold_observed);
    ASSERT_EQ(cold.state, ResolvedPreparedMotionExecutionStateDTO::kColdPrepared);
    ASSERT_TRUE(cold.participant);
    const auto& cold_motion = std::get<PreparedMotionExecutionDTO>(cold.participant->payload);
    EXPECT_DOUBLE_EQ(cold_motion.plan_request.planning_timeout_s, 0.1);
    EXPECT_DOUBLE_EQ(planner.last_request.planning_timeout_s, 0.1);
    EXPECT_FALSE(cold_motion.plan_request.moving_replacement.has_value());
    EXPECT_GT(cold.reserved_execution_id, sent.value());
    EXPECT_FALSE(owner.ObservePreparedMotion(cold.reserved_execution_id).ok());
    EXPECT_FALSE(owner.CancelPreparedMotion(cold.reserved_execution_id).ok());
    EXPECT_FALSE(owner
                     .DispatchPreparedMotion(cold.reserved_execution_id,
                                             cold.participant->prepared_contract_digest)
                     .ok());
    EXPECT_EQ(backend.preparations.load(), 2);  // original + sent smooth; no cold backend Prepare
    EXPECT_EQ(backend.execute_count, 1);
    EXPECT_EQ(backend.maximum_live_goals, 2U);
    EXPECT_FALSE(owner.RetireCancelledPreparedMotion(sent.value()).ok());
    ASSERT_TRUE(owner.ObservePreparedMotion(sent.value()).ok());
    if (abandon) {
      EXPECT_FALSE(owner.CancelPreparedMotion(sent.value()).ok());  // sent debt remains
      const auto cancelled = owner.ObservePreparedMotion(sent.value());
      ASSERT_TRUE(cancelled.ok());
      EXPECT_EQ(cancelled.value().state, old.value().state);
      EXPECT_EQ(cancelled.value().spring_cold_result->state,
                ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch);
    }
    const auto result = kernel_spring_result(stage);
    const auto certificate = kernel_spring_certificate(result);
    const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                          certificate.stop_certificate_revision,
                                          certificate.stop_id, certificate.stop_generation};
    clock.advance(300'000'000);  // timely product survives independent spring return wait
    backend.publish_observed_state(clock.now(), {0.0, 0.0}, {0.0, 0.0});
    ASSERT_TRUE(
        backend.execute_controlled_stop(*original.ordinary_stop_plan->participant_stop_profile)
            .backend_stop_confirmed);
    ASSERT_TRUE(
        owner
            .settle_addressed_participant_stop({started.value(), reference, result,
                                                OperationParticipantPayloadKind::kMotion, false})
            .ok());
    ASSERT_TRUE(owner.wait_for_completion(2'000'000'000));
    // ColdPrepared publication precedes its task's final notification. Wait
    // for that existing task to drain before asserting the backend refusal.
    bool retirement_refused = false;
    const auto retirement_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < retirement_deadline) {
      const auto attempt = owner.RetireCancelledPreparedMotion(sent.value());
      if (!attempt.ok()) {
        retirement_refused = true;
        break;
      }
      ASSERT_FALSE(attempt.value());
      std::this_thread::yield();
    }
    ASSERT_TRUE(retirement_refused);
    EXPECT_GT(backend.sent_retirement_refusals.load(), 0);
    ASSERT_TRUE(owner.ObservePreparedMotion(sent.value()).ok());
    backend.permit_sent_retirement.store(true);
    const auto retired = owner.RetireCancelledPreparedMotion(sent.value());
    ASSERT_TRUE(retired.ok()) << retired.error().message;
    ASSERT_TRUE(retired.value());
    EXPECT_FALSE(owner.ObservePreparedMotion(sent.value()).ok());
    if (abandon) {
      EXPECT_FALSE(owner.ObservePreparedMotion(cold.reserved_execution_id).ok());
      EXPECT_EQ(backend.preparations.load(), 2);
    } else {
      const auto promoted = owner.ObservePreparedMotion(cold.reserved_execution_id);
      ASSERT_TRUE(promoted.ok());
      EXPECT_EQ(promoted.value().state, ResolvedPreparedMotionExecutionStateDTO::kColdPrepared);
      EXPECT_EQ(promoted.value().participant, cold.participant);
      EXPECT_FALSE(promoted.value().backend_prepare_result);
      const auto replay = owner.RetireCancelledPreparedMotion(sent.value());
      ASSERT_TRUE(replay.ok());
      ASSERT_TRUE(replay.value());  // old-ID receipt
      ASSERT_TRUE(owner.ObservePreparedMotion(cold.reserved_execution_id).ok());
      EXPECT_THROW(
          backend.Dispatch(*old.value().participant,
                           sent_authority.execution_request.binding.controller_goal_id,
                           sent_authority.execution_request.binding.controller_goal_generation),
          std::logic_error);
      const auto activated =
          owner.PrepareColdMotionAfterStop(cold.reserved_execution_id, certificate);
      ASSERT_TRUE(activated.ok()) << activated.error().message;
      ASSERT_TRUE(activated.value());
      std::shared_ptr<const PreparedOperationParticipantDTO> ready;
      const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < ready_deadline) {
        const auto snapshot = owner.ObservePreparedMotion(cold.reserved_execution_id);
        ASSERT_TRUE(snapshot.ok());
        ASSERT_FALSE(snapshot.value().error) << snapshot.value().error->message;
        if (snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kPrepared) {
          ready = snapshot.value().participant;
          break;
        }
        std::this_thread::yield();
      }
      ASSERT_TRUE(ready);
      EXPECT_EQ(ready->operation_id, old.value().participant->operation_id);
      EXPECT_EQ(ready->program_run_id, old.value().participant->program_run_id);
      EXPECT_EQ(ready->prepared_contract_digest, cold.participant->prepared_contract_digest);
      ASSERT_TRUE(
          owner.DispatchPreparedMotion(cold.reserved_execution_id, ready->prepared_contract_digest)
              .ok());
      ASSERT_TRUE(backend.wait_until_execute_count(2));
      bool dispatch_observed = false;
      const auto dispatch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < dispatch_deadline) {
        const auto dispatched = owner.ObservePreparedMotion(cold.reserved_execution_id);
        ASSERT_TRUE(dispatched.ok());
        if (dispatched.value().state == ResolvedPreparedMotionExecutionStateDTO::kDispatched &&
            dispatched.value().last_dispatched_participant) {
          dispatch_observed = true;
          break;
        }
        std::this_thread::yield();
      }
      ASSERT_TRUE(dispatch_observed);
      EXPECT_EQ(backend.preparations.load(), 3);
      EXPECT_EQ(backend.maximum_live_goals, 2U);
      backend.complete();
      ASSERT_TRUE(owner.wait_for_completion(2'000'000'000));
    }
    EXPECT_EQ(planner.plan_count, before_cold + 1);  // sealed computation reused, never replanned
    planner.on_plan = {};
    context.on_validate = {};
  }
}

}  // namespace
}  // namespace manyforge::core
