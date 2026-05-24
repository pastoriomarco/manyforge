#include <manyforge_core/execution_kernel.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace manyforge::core {
namespace {

constexpr int64_t kExecutionPollNs = 10'000'000;  // 10 ms
constexpr int kExecutionDrainMaxPolls = 100;      // bounded 1 s software drain
constexpr double kExecutionDrainTimeoutS =
    static_cast<double>(kExecutionPollNs * kExecutionDrainMaxPolls) / 1e9;

double clamp_unit_interval(double value) {
  return std::max(0.0, std::min(1.0, value));
}

Pose3DTO multiply_poses(const Pose3DTO& a, const Pose3DTO& b) {
  auto pose_to_matrix = [](const Pose3DTO& pose) {
    std::array<std::array<double, 4>, 4> m{{
        {{1.0, 0.0, 0.0, 0.0}},
        {{0.0, 1.0, 0.0, 0.0}},
        {{0.0, 0.0, 1.0, 0.0}},
        {{0.0, 0.0, 0.0, 1.0}},
    }};
    const double x = pose.orientation.x;
    const double y = pose.orientation.y;
    const double z = pose.orientation.z;
    const double w = pose.orientation.w;
    m[0][0] = 1 - 2 * y * y - 2 * z * z;
    m[0][1] = 2 * x * y - 2 * z * w;
    m[0][2] = 2 * x * z + 2 * y * w;
    m[1][0] = 2 * x * y + 2 * z * w;
    m[1][1] = 1 - 2 * x * x - 2 * z * z;
    m[1][2] = 2 * y * z - 2 * x * w;
    m[2][0] = 2 * x * z - 2 * y * w;
    m[2][1] = 2 * y * z + 2 * x * w;
    m[2][2] = 1 - 2 * x * x - 2 * y * y;
    m[0][3] = pose.position_m.x_m;
    m[1][3] = pose.position_m.y_m;
    m[2][3] = pose.position_m.z_m;
    return m;
  };

  auto matrix_to_pose = [](const std::array<std::array<double, 4>, 4>& m) {
    Pose3DTO pose;
    pose.position_m = {m[0][3], m[1][3], m[2][3]};

    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0) {
      const double s = std::sqrt(trace + 1.0) * 2.0;
      pose.orientation.w = 0.25 * s;
      pose.orientation.x = (m[2][1] - m[1][2]) / s;
      pose.orientation.y = (m[0][2] - m[2][0]) / s;
      pose.orientation.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
      const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
      pose.orientation.w = (m[2][1] - m[1][2]) / s;
      pose.orientation.x = 0.25 * s;
      pose.orientation.y = (m[0][1] + m[1][0]) / s;
      pose.orientation.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
      const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
      pose.orientation.w = (m[0][2] - m[2][0]) / s;
      pose.orientation.x = (m[0][1] + m[1][0]) / s;
      pose.orientation.y = 0.25 * s;
      pose.orientation.z = (m[1][2] + m[2][1]) / s;
    } else {
      const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
      pose.orientation.w = (m[1][0] - m[0][1]) / s;
      pose.orientation.x = (m[0][2] + m[2][0]) / s;
      pose.orientation.y = (m[1][2] + m[2][1]) / s;
      pose.orientation.z = 0.25 * s;
    }
    return pose;
  };

  const auto ma = pose_to_matrix(a);
  const auto mb = pose_to_matrix(b);
  std::array<std::array<double, 4>, 4> out{};
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (size_t k = 0; k < 4; ++k) {
        sum += ma[i][k] * mb[k][j];
      }
      out[i][j] = sum;
    }
  }
  return matrix_to_pose(out);
}

Pose3DTO pose_with_position_and_orientation(
    const Vector3DTO& position,
    const QuaternionDTO& orientation) {
  Pose3DTO pose;
  pose.position_m = position;
  pose.orientation = orientation;
  return pose;
}

ErrorCode error_code_from_dto(const ErrorDTO& error) {
  if (error.code == kErrorStartStateMismatch) {
    return ErrorCode::kStartStateMismatch;
  }
  if (error.code == kErrorMissingReference) {
    return ErrorCode::kMissingReference;
  }
  if (error.code == kErrorAmbiguousReference) {
    return ErrorCode::kAmbiguousReference;
  }
  if (error.code == kErrorValidationLimitsViolation) {
    return ErrorCode::kValidationLimitsViolation;
  }
  if (error.code == kErrorValidationCollision) {
    return ErrorCode::kValidationCollision;
  }
  if (error.code == kErrorExecutionTimeout) {
    return ErrorCode::kExecutionTimeout;
  }
  if (error.code == kErrorStopFailed) {
    return ErrorCode::kStopFailed;
  }
  if (error.code == kErrorPauseFailed) {
    return ErrorCode::kPauseFailed;
  }
  if (error.code == kErrorResumeFailed) {
    return ErrorCode::kResumeFailed;
  }
  if (error.code == kErrorResetFailed) {
    return ErrorCode::kResetFailed;
  }
  if (error.code == kErrorInvalidRequest) {
    return ErrorCode::kInvalidRequest;
  }
  if (error.code == kErrorInvariantViolation) {
    return ErrorCode::kInvariantViolation;
  }
  return ErrorCode::kInternalError;
}

bool collision_world_revision_is_set(const CollisionWorldRevision& revision) {
  return revision.revision_id != 0 || !revision.content_hash.empty();
}

bool same_collision_world_revision(const CollisionWorldRevision& a,
                                   const CollisionWorldRevision& b) {
  return a.revision_id == b.revision_id &&
         a.content_hash == b.content_hash;
}

constexpr const char* kRuntimeCollisionFaultSourceId =
    "manyforge.kernel.runtime_collision_fault";
constexpr const char* kRuntimeCollisionFaultPolicyId =
    "manyforge.kernel.runtime_collision_fault.preempt_trajectory";

bool validation_cancelled_by_interrupt(const ErrorDTO& error) {
  if (error.code != kErrorValidationCollision) {
    return false;
  }
  const auto it = error.details.find("reason");
  return it != error.details.end() &&
         it->second == "collision_validation_cancelled";
}

const char* streaming_collision_reason_code(
    const CollisionValidationDecisionBundle& bundle) {
  if (!bundle.cancelled) {
    return "streaming_collision_rejected";
  }
  if (bundle.decision.first_failing_waypoint &&
      bundle.cancelled_at_waypoint_index &&
      *bundle.decision.first_failing_waypoint <
          *bundle.cancelled_at_waypoint_index) {
    return "streaming_collision_rejected";
  }
  return "streaming_collision_cancelled";
}

std::optional<KernelEventType> interrupt_event_type_from_name(
    const std::string& name) {
  static const std::map<std::string, KernelEventType> kMap{
      {"interrupt_signal_ingested", KernelEventType::kInterruptSignalIngested},
      {"interrupt_signal_rejected_stale",
       KernelEventType::kInterruptSignalRejectedStale},
      {"interrupt_signal_rejected_class_violation",
       KernelEventType::kInterruptSignalRejectedClassViolation},
      {"interrupt_signal_rejected", KernelEventType::kInterruptSignalRejected},
      {"interrupt_latched", KernelEventType::kInterruptLatched},
      {"interrupt_suppressed_by_active_mask",
       KernelEventType::kInterruptSuppressedByActiveMask},
      {"interrupt_queued", KernelEventType::kInterruptQueued},
      {"interrupt_dropped_queue_overflow",
       KernelEventType::kInterruptDroppedQueueOverflow},
      {"interrupt_preempt_applied", KernelEventType::kInterruptPreemptApplied},
      {"interrupt_disposition_applied",
       KernelEventType::kInterruptDispositionApplied},
      {"interrupt_cleared", KernelEventType::kInterruptCleared},
      {"interrupt_ingest_overflow", KernelEventType::kInterruptIngestOverflow},
      {"interrupt_source_class_violation",
       KernelEventType::kInterruptSourceClassViolation},
      {"interrupt_recovery_handler_started",
       KernelEventType::kInterruptRecoveryHandlerStarted},
      {"interrupt_recovery_handler_completed",
       KernelEventType::kInterruptRecoveryHandlerCompleted},
  };
  const auto it = kMap.find(name);
  if (it == kMap.end()) {
    return std::nullopt;
  }
  return it->second;
}

InterruptIngestResult reject_interrupts_without_active_phase(
    const std::vector<InterruptSignalUpdate>& updates) {
  InterruptIngestResult result;
  result.rejected = true;
  result.reason = "no_active_execution_phase";
  for (const auto& update : updates) {
    result.events.push_back(
        {"interrupt_signal_rejected",
         {{"source_id", update.source_id},
          {"sequence_id", std::to_string(update.sequence_id)},
          {"reason", result.reason}},
         std::nullopt});
  }
  return result;
}

ErrorDTO collision_world_changed_error(const CollisionWorldRevision& before,
                                       const CollisionWorldRevision& after,
                                       const std::string& phase) {
  return ErrorDTO{
      kErrorInvariantViolation,
      "Collision world changed during trajectory validation",
      {{"reason", "scene_changed_during_validation"},
       {"phase", phase},
       {"before_revision_id", std::to_string(before.revision_id)},
       {"before_content_hash", before.content_hash},
       {"after_revision_id", std::to_string(after.revision_id)},
       {"after_content_hash", after.content_hash}}};
}

ErrorDTO authoritative_collision_validation_config_error(
    std::string reason,
    std::map<std::string, std::string> details = {},
    std::string phase = "pre_execution_validation") {
  details.emplace("reason", std::move(reason));
  details.emplace("phase", std::move(phase));
  return ErrorDTO{
      kErrorInvariantViolation,
      "Authoritative collision validation pipeline is not configured",
      std::move(details)};
}

InterruptSourceDefinition runtime_collision_fault_source_definition() {
  InterruptSourceDefinition source;
  source.source_id = kRuntimeCollisionFaultSourceId;
  source.adapter = InterruptAdapter::kKernel;
  source.signal = "runtime_collision_fault";
  source.source_class = InterruptSourceClass::kFastExecutionRelevant;
  source.kind = InterruptSourceKind::kEdge;
  source.priority = 1000;
  source.maskable = false;
  source.freshness_budget_ns = 1'000'000;
  source.description =
      "Kernel-owned runtime collision monitor fault source";
  return source;
}

InterruptPolicyDefinition runtime_collision_fault_policy_definition() {
  InterruptPolicyDefinition policy;
  policy.policy_id = kRuntimeCollisionFaultPolicyId;
  policy.source_id = kRuntimeCollisionFaultSourceId;
  policy.priority = 1000;
  policy.maskable = false;
  policy.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  policy.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  policy.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  policy.description =
      "Preempt trajectory execution when runtime collision monitoring detects a collision";
  return policy;
}

bool drain_execution_handle_bounded(ExecutionHandle& handle) {
  if (!handle.wait_for_completion) {
    return true;
  }
  for (int poll = 0; poll < kExecutionDrainMaxPolls; ++poll) {
    if (handle.wait_for_completion(kExecutionPollNs)) {
      return true;
    }
  }
  return false;
}

ErrorDTO operator_intervention_error(ErrorCode code,
                                     std::string message,
                                     std::string phase,
                                     ExecutionState previous_state,
                                     uint64_t execution_id,
                                     const std::string& move_id) {
  return ErrorDTO{
      std::string(to_string(code)),
      std::move(message),
      {{"requires_operator_intervention", "true"},
       {"phase", std::move(phase)},
       {"previous_state", to_string(previous_state)},
       {"execution_id", std::to_string(execution_id)},
       {"move_id", move_id},
       {"stop_completion_timeout_s", std::to_string(kExecutionDrainTimeoutS)}}};
}

ErrorDTO ensure_operator_intervention_details(ErrorDTO error,
                                              std::string phase,
                                              ExecutionState previous_state,
                                              uint64_t execution_id,
                                              const std::string& move_id) {
  error.details.try_emplace("requires_operator_intervention", "true");
  error.details.try_emplace("phase", std::move(phase));
  error.details.try_emplace("previous_state", to_string(previous_state));
  error.details.try_emplace("execution_id", std::to_string(execution_id));
  error.details.try_emplace("move_id", move_id);
  error.details.try_emplace("stop_completion_timeout_s",
                            std::to_string(kExecutionDrainTimeoutS));
  return error;
}

double quaternion_angular_distance_rad(const QuaternionDTO& a,
                                       const QuaternionDTO& b) {
  const double norm_a = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
  const double norm_b = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
  if (norm_a <= 0.0 || norm_b <= 0.0) {
    return std::numeric_limits<double>::infinity();
  }
  const double dot = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) /
                     (norm_a * norm_b);
  const double clamped = std::min(1.0, std::max(-1.0, std::abs(dot)));
  return 2.0 * std::acos(clamped);
}

double translation_distance_m(const Pose3DTO& a, const Pose3DTO& b) {
  const double dx = a.position_m.x_m - b.position_m.x_m;
  const double dy = a.position_m.y_m - b.position_m.y_m;
  const double dz = a.position_m.z_m - b.position_m.z_m;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool positions_match_by_index(const std::vector<double>& a,
                              const std::vector<double>& b,
                              double tolerance_rad) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i] - b[i]) > tolerance_rad) {
      return false;
    }
  }
  return true;
}

bool names_cover_positions(const std::vector<std::string>& names,
                           size_t position_count) {
  return !names.empty() && names.size() == position_count &&
         std::all_of(names.begin(), names.end(),
                     [](const std::string& name) { return !name.empty(); });
}

struct CacheCompatibilityResult {
  bool compatible = false;
  std::string reason;
};

CacheCompatibilityResult compatible_cache() {
  return CacheCompatibilityResult{true, ""};
}

CacheCompatibilityResult incompatible_cache(std::string reason) {
  return CacheCompatibilityResult{false, std::move(reason)};
}

CacheCompatibilityResult cached_joint_goal_matches_request(
    const TrajectoryWaypointDTO& end_wp,
    const std::vector<std::string>& trajectory_joint_names,
    const JointGoalDTO& joint_goal,
    double tolerance_rad) {
  if (end_wp.positions_rad.size() != joint_goal.positions_rad.size()) {
    return incompatible_cache("goal_changed");
  }

  const bool trajectory_names_complete =
      names_cover_positions(trajectory_joint_names, end_wp.positions_rad.size());
  const bool goal_names_complete =
      names_cover_positions(joint_goal.joint_names, joint_goal.positions_rad.size());

  if (trajectory_names_complete && goal_names_complete) {
    for (size_t goal_idx = 0; goal_idx < joint_goal.joint_names.size(); ++goal_idx) {
      const auto& goal_name = joint_goal.joint_names[goal_idx];
      const auto matches =
          std::count(trajectory_joint_names.begin(), trajectory_joint_names.end(), goal_name);
      if (matches != 1) {
        return incompatible_cache("goal_unverifiable");
      }
      const auto traj_it =
          std::find(trajectory_joint_names.begin(), trajectory_joint_names.end(), goal_name);
      const auto traj_idx =
          static_cast<size_t>(std::distance(trajectory_joint_names.begin(), traj_it));
      if (std::abs(end_wp.positions_rad[traj_idx] -
                   joint_goal.positions_rad[goal_idx]) > tolerance_rad) {
        return incompatible_cache("goal_changed");
      }
    }
    return compatible_cache();
  }

  if (trajectory_joint_names.empty() && joint_goal.joint_names.empty() &&
      positions_match_by_index(end_wp.positions_rad, joint_goal.positions_rad, tolerance_rad)) {
    return compatible_cache();
  }

  if (trajectory_joint_names.empty() && joint_goal.joint_names.empty()) {
    return incompatible_cache("goal_changed");
  }
  return incompatible_cache("goal_unverifiable");
}

CacheCompatibilityResult cached_pose_goal_matches_request(
    IPlanningContext& context,
    const TrajectoryDTO& trajectory,
    const TrajectoryWaypointDTO& end_wp,
    const Pose3DTO& requested_goal,
    const ExecuteRequestDTO& request) {
  if (request.tcp_frame.empty() || request.base_frame.empty()) {
    return incompatible_cache("goal_unverifiable");
  }
  RobotStateDTO final_state;
  final_state.joint_names = trajectory.joint_names;
  final_state.positions_rad = end_wp.positions_rad;
  final_state.velocities_rad_s = end_wp.velocities_rad_s;

  try {
    const auto final_tcp_pose = context.forward_kinematics(
        FkRequestDTO{final_state, request.tcp_frame, request.base_frame});
    if (translation_distance_m(final_tcp_pose, requested_goal) >
        request.tolerances.translation_tol_m) {
      return incompatible_cache("goal_changed");
    }
    if (quaternion_angular_distance_rad(final_tcp_pose.orientation,
                                        requested_goal.orientation) >
        request.tolerances.rotation_tol_rad) {
      return incompatible_cache("goal_changed");
    }
  } catch (...) {
    return incompatible_cache("fk_failed");
  }

  return compatible_cache();
}

CacheCompatibilityResult cached_trajectory_matches_request_goal(
    IPlanningContext& context,
    const TrajectoryDTO& trajectory,
    const ExecuteRequestDTO& request) {
  if (trajectory.waypoints.empty()) {
    return incompatible_cache("empty_trajectory");
  }

  const auto& end_wp = trajectory.waypoints.back();
  if (const auto* joint_goal = std::get_if<JointGoalDTO>(&request.motion)) {
    return cached_joint_goal_matches_request(
        end_wp, trajectory.joint_names, *joint_goal,
        request.tolerances.joint_position_tol_rad);
  }

  if (const auto* pose_goal = std::get_if<PoseGoalDTO>(&request.motion)) {
    return cached_pose_goal_matches_request(
        context, trajectory, end_wp, pose_goal->goal, request);
  }

  if (const auto* cartesian_path = std::get_if<CartesianPathDTO>(&request.motion)) {
    if (cartesian_path->waypoints.empty()) {
      return incompatible_cache("goal_unverifiable");
    }
    return cached_pose_goal_matches_request(
        context, trajectory, end_wp, cartesian_path->waypoints.back(), request);
  }

  return incompatible_cache("goal_unverifiable");
}

CacheCompatibilityResult cached_trajectory_matches_current_start(
    const TrajectoryDTO& trajectory,
    const RobotStateDTO& current_state,
    double tolerance_rad) {
  if (trajectory.waypoints.empty()) {
    return incompatible_cache("empty_trajectory");
  }
  const auto& start_wp = trajectory.waypoints.front();
  if (start_wp.positions_rad.size() != current_state.positions_rad.size()) {
    return incompatible_cache("start_state_changed");
  }

  const bool trajectory_names_complete =
      names_cover_positions(trajectory.joint_names, start_wp.positions_rad.size());
  const bool state_names_complete =
      names_cover_positions(current_state.joint_names, current_state.positions_rad.size());

  if (trajectory_names_complete && state_names_complete) {
    for (size_t state_idx = 0; state_idx < current_state.joint_names.size(); ++state_idx) {
      const auto& state_name = current_state.joint_names[state_idx];
      const auto matches =
          std::count(trajectory.joint_names.begin(), trajectory.joint_names.end(), state_name);
      if (matches != 1) {
        return incompatible_cache("start_state_changed");
      }
      const auto traj_it =
          std::find(trajectory.joint_names.begin(), trajectory.joint_names.end(), state_name);
      const auto traj_idx =
          static_cast<size_t>(std::distance(trajectory.joint_names.begin(), traj_it));
      if (std::abs(start_wp.positions_rad[traj_idx] -
                   current_state.positions_rad[state_idx]) > tolerance_rad) {
        return incompatible_cache("start_state_changed");
      }
    }
    return compatible_cache();
  }

  if (trajectory.joint_names.empty() && current_state.joint_names.empty()) {
    for (size_t i = 0; i < start_wp.positions_rad.size(); ++i) {
      if (std::abs(start_wp.positions_rad[i] - current_state.positions_rad[i]) >
          tolerance_rad) {
        return incompatible_cache("start_state_changed");
      }
    }
    return compatible_cache();
  }
  return incompatible_cache("start_state_changed");
}

struct PreparedExecutionStart {
  bool cache_hit = false;
  bool cache_miss = false;
  bool cache_invalidated = false;
  std::string cache_invalidation_reason;
  TrajectoryDTO trajectory;
};

}  // namespace

ExecutionKernel::ExecutionKernel(IPlannerBackend& planner,
                                 IPlanningContext& context,
                                 IExecutionBackend& execution_backend,
                                 IClock& clock,
                                 IScheduler& scheduler,
                                 TrajectoryCache& cache,
                                 ExecutionKernelConfig config)
    : planner_(planner),
      context_(context),
      execution_backend_(execution_backend),
      clock_(clock),
      scheduler_(scheduler),
      cache_(cache),
      config_(std::move(config)),
      sm_(clock),
      stop_policy_(sm_, scheduler, clock, [this]() { cache_.invalidate_all(); }),
      collision_policy_(stop_policy_,
                        [this]() { cache_.invalidate_all(); }) {
  if (config_.streaming_ik_timeout_s <= 0.0 ||
      !std::isfinite(config_.streaming_ik_timeout_s)) {
    throw std::invalid_argument("streaming_ik_timeout_s must be finite and > 0");
  }
  if (config_.streaming_collision_timeout_s <= 0.0 ||
      !std::isfinite(config_.streaming_collision_timeout_s)) {
    throw std::invalid_argument(
        "streaming_collision_timeout_s must be finite and > 0");
  }
  if (config_.native_reset_completion_timeout_s <= 0.0 ||
      !std::isfinite(config_.native_reset_completion_timeout_s)) {
    throw std::invalid_argument(
        "native_reset_completion_timeout_s must be finite and > 0");
  }
  if (config_.native_streaming_stop_completion_timeout_s <= 0.0 ||
      !std::isfinite(config_.native_streaming_stop_completion_timeout_s)) {
    throw std::invalid_argument(
        "native_streaming_stop_completion_timeout_s must be finite and > 0");
  }
  if (config_.validation_collision_workers < 1) {
    config_.validation_collision_workers = 1;
  }
  if (config_.validation_collision_min_waypoints_for_parallel < 1) {
    config_.validation_collision_min_waypoints_for_parallel = 1;
  }
  stop_policy_.set_stop_callback([this]() { (void)execution_backend_.stop(); });
  install_builtin_interrupt_sources();
  sync_snapshot_from_context();
}

ExecutionKernel::~ExecutionKernel() {
  // Scheduled collision callbacks capture `this`; cancel them before any
  // member teardown so destruction cannot race a monitor tick.
  stop_collision_monitoring();

  // Join before member destruction so worker callbacks cannot notify the
  // condition variable after it has been destroyed. This is intentionally
  // explicit instead of relying on std::jthread member teardown order.
  if (worker_thread_.joinable()) {
    worker_thread_.request_stop();
    completion_cv_.notify_all();
    worker_thread_.join();
  }
}

void ExecutionKernel::emit_event(const KernelEvent& event) {
  std::vector<EventCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    callbacks.reserve(subscribers_.size());
    for (const auto& [id, callback] : subscribers_) {
      (void)id;
      callbacks.push_back(callback);
    }
  }

  for (const auto& callback : callbacks) {
    try {
      callback(event);
    } catch (...) {
      // Ignore exceptions from callbacks
    }
  }
}

SubscriptionHandle ExecutionKernel::subscribe_events(EventCallback callback) {
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  uint64_t id = next_subscription_id_++;
  subscribers_[id] = std::move(callback);
  return SubscriptionHandle{id};
}

void ExecutionKernel::unsubscribe_events(SubscriptionHandle handle) {
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  subscribers_.erase(handle.id);
}

Result<void> ExecutionKernel::register_interrupt_source(
    const InterruptSourceDefinition& source) {
  return interrupt_manager_.register_source(source);
}

Result<void> ExecutionKernel::register_interrupt_policy(
    const InterruptPolicyDefinition& policy) {
  return interrupt_manager_.register_policy(policy);
}

CollisionWorldRevision
ExecutionKernel::current_authoritative_collision_world_revision() const {
  ICollisionWorldSnapshotPublisher* publisher = nullptr;
  {
    std::scoped_lock lock(coordinator_mutex_);
    publisher = collision_snapshot_publisher_;
  }
  if (publisher == nullptr) {
    return {};
  }
  try {
    auto snapshot = publisher->snapshot();
    if (snapshot) {
      return snapshot->revision;
    }
  } catch (...) {
    // Callers use the default revision as "no authoritative revision
    // available"; validation itself reports the concrete publisher error.
  }
  return {};
}

std::shared_ptr<CollisionCancellationToken>
ExecutionKernel::install_validation_cancel_token() {
  auto token = std::make_shared<CollisionCancellationToken>();
  {
    std::scoped_lock lock(validation_cancel_mutex_);
    active_validation_cancel_token_ = token;
  }
  if (auto latch = interrupt_manager_.active_latch();
      latch &&
      latch->preemption_action_requested ==
          InterruptPreemptionAction::kCancelValidation) {
    token->request_cancel("interrupt:" + latch->latch_id);
  }
  return token;
}

void ExecutionKernel::clear_validation_cancel_token(
    const std::shared_ptr<CollisionCancellationToken>& token) {
  std::scoped_lock lock(validation_cancel_mutex_);
  auto active = active_validation_cancel_token_.lock();
  if (active && active == token) {
    active_validation_cancel_token_.reset();
  }
}

std::shared_ptr<CollisionCancellationToken>
ExecutionKernel::install_streaming_target_cancel_token(
    std::uint64_t validation_sequence) {
  auto token = std::make_shared<CollisionCancellationToken>();
  std::scoped_lock lock(streaming_cancel_mutex_);
  if (auto previous = active_streaming_target_cancel_token_.lock()) {
    previous->request_cancel("superseded_by_newer_streaming_target");
  }
  if (validation_sequence != streaming_target_validation_sequence_.load(
                                 std::memory_order_acquire)) {
    token->request_cancel("superseded_by_newer_streaming_target");
  }
  active_streaming_target_cancel_token_ = token;
  return token;
}

void ExecutionKernel::clear_streaming_target_cancel_token(
    const std::shared_ptr<CollisionCancellationToken>& token) {
  std::scoped_lock lock(streaming_cancel_mutex_);
  auto active = active_streaming_target_cancel_token_.lock();
  if (active && active == token) {
    active_streaming_target_cancel_token_.reset();
  }
}

void ExecutionKernel::cancel_active_streaming_target_validation(
    std::string reason) {
  streaming_target_validation_sequence_.fetch_add(
      1, std::memory_order_acq_rel);
  std::scoped_lock lock(streaming_cancel_mutex_);
  if (auto active = active_streaming_target_cancel_token_.lock()) {
    active->request_cancel(std::move(reason));
  }
}

void ExecutionKernel::request_validation_cancel_from_interrupt(
    const InterruptIngestResult& result) {
  if (!result.latched || !result.active_latch ||
      result.active_latch->preemption_action_requested !=
          InterruptPreemptionAction::kCancelValidation) {
    return;
  }
  std::shared_ptr<CollisionCancellationToken> token;
  {
    std::scoped_lock lock(validation_cancel_mutex_);
    token = active_validation_cancel_token_.lock();
  }
  if (token) {
    token->request_cancel("interrupt:" + result.active_latch->latch_id);
  }
}

void ExecutionKernel::mark_active_interrupt_preemption_result(
    InterruptPreemptionResult result) {
  auto latch = interrupt_manager_.active_latch();
  if (!latch) {
    return;
  }
  const auto now = clock_.now();
  auto marked = interrupt_manager_.mark_active_preemption_result(
      result, now - latch->latched_at_ns, now);
  if (marked.ok()) {
    emit_interrupt_events(marked.value());
  }
}

void ExecutionKernel::install_builtin_interrupt_sources() {
  auto source = interrupt_manager_.register_source(
      runtime_collision_fault_source_definition());
  if (!source.ok()) {
    throw std::logic_error(
        "failed to register kernel runtime collision interrupt source: " +
        source.error().message);
  }
  auto policy = interrupt_manager_.register_policy(
      runtime_collision_fault_policy_definition());
  if (!policy.ok()) {
    throw std::logic_error(
        "failed to register kernel runtime collision interrupt policy: " +
        policy.error().message);
  }
}

InterruptIngestResult
ExecutionKernel::publish_runtime_collision_fault_interrupt(
    const CollisionCheckResultDTO& check) {
  InterruptSignalUpdate signal;
  signal.source_id = kRuntimeCollisionFaultSourceId;
  signal.sequence_id =
      runtime_collision_fault_sequence_.fetch_add(1, std::memory_order_relaxed) +
      1;
  signal.timestamp_ns = clock_.now();
  signal.freshness_ns = 0;
  signal.value = true;
  signal.metadata["surface"] = "observed_runtime_monitor";
  signal.metadata["in_collision"] = check.in_collision ? "true" : "false";
  if (check.contact_body_a) {
    signal.metadata["contact_a"] = *check.contact_body_a;
  }
  if (check.contact_body_b) {
    signal.metadata["contact_b"] = *check.contact_body_b;
  }
  return publish_fast_interrupt_signal(signal);
}

void ExecutionKernel::apply_latched_interrupt_preemption(
    const InterruptIngestResult& result) {
  if (!result.latched || !result.active_latch) {
    return;
  }
  const auto& latch = *result.active_latch;
  if (latch.preemption_action_requested ==
      InterruptPreemptionAction::kPreemptStreaming) {
    cancel_active_streaming_target_validation("interrupt:" + latch.latch_id);
    std::uint64_t kernel_session_id = 0;
    std::uint64_t backend_session_id = 0;
    SoftStopStrategy strategy = SoftStopStrategy::kSpringBack;
    {
      std::lock_guard<std::mutex> lock(streaming_session_mutex_);
      kernel_session_id = active_streaming_session_id_;
      backend_session_id = active_backend_session_id_;
      strategy = active_soft_stop_strategy_;
    }

    if (kernel_session_id == 0 || backend_session_id == 0) {
      mark_active_interrupt_preemption_result(
          InterruptPreemptionResult::kSucceeded);
      return;
    }

    auto end_result =
        execution_backend_.end_streaming_session(backend_session_id, strategy);
    if (end_result.accepted) {
      {
        std::lock_guard<std::mutex> lock(streaming_session_mutex_);
        if (active_streaming_session_id_ == kernel_session_id) {
          active_streaming_session_id_ = 0;
          active_backend_session_id_ = 0;
          ++active_streaming_generation_;
          active_streaming_config_ = StreamingSessionConfigDTO{};
          active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
          last_accepted_cartesian_pos_valid_ = false;
          last_accepted_cartesian_ts_ns_ = 0;
          last_commanded_joints_valid_ = false;
          last_commanded_joints_.clear();
        }
      }
      emit_event({KernelEventType::kStreamingSessionEnded,
                  0,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"session_id", std::to_string(kernel_session_id)},
                   {"backend_session_id", std::to_string(backend_session_id)},
                   {"end_reason", "interrupt_preempt_streaming"},
                   {"soft_stop_strategy", to_string(strategy)}}});
      mark_active_interrupt_preemption_result(
          InterruptPreemptionResult::kSucceeded);
    } else {
      mark_active_interrupt_preemption_result(
          InterruptPreemptionResult::kBackendPreemptFailed);
      if (end_result.error) {
        sm_.set_last_error(error_code_from_dto(*end_result.error));
        auto snapshot = get_execution_snapshot();
        snapshot.timestamp_ns = clock_.now();
        snapshot.state = ExecutionState::kInterrupted;
        snapshot.error = end_result.error;
        publish_execution_snapshot(std::move(snapshot));
        emit_event({KernelEventType::kErrorRaised,
                    sm_.status().execution_id,
                    clock_.now(),
                    {},
                    {},
                    end_result.error,
                    {{"phase", "preempt_streaming"},
                     {"session_id", std::to_string(kernel_session_id)}}});
      }
    }
    return;
  }
  if (latch.preemption_action_requested !=
      InterruptPreemptionAction::kPreemptTrajectory) {
    return;
  }
  stop_collision_monitoring();
  cache_.invalidate_all();
  auto stop = execution_backend_.stop();
  mark_active_interrupt_preemption_result(
      stop.accepted ? InterruptPreemptionResult::kSucceeded
                    : InterruptPreemptionResult::kBackendPreemptFailed);
  if (!stop.accepted && stop.error) {
    sm_.set_last_error(error_code_from_dto(*stop.error));
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = ExecutionState::kInterrupted;
    snapshot.error = stop.error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                stop.error,
                {{"phase", "preempt_trajectory"}}});
  }
}

InterruptRuntimeContext ExecutionKernel::current_interrupt_context() const {
  InterruptRuntimeContext context;
  const auto status = sm_.status();
  const auto phase = interrupt_phase_from_state(status.state);
  context.phase = phase.value_or(InterruptExecutionPhase::kHeld);
  if (status.execution_id != 0) {
    context.execution_id = status.execution_id;
  }
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    if (!active_move_id_.empty()) {
      context.move_id = active_move_id_;
    }
  }
  {
    std::lock_guard<std::mutex> streaming_lock(streaming_session_mutex_);
    if (active_streaming_session_id_ != 0) {
      context.streaming_session_id = active_streaming_session_id_;
    }
  }
  // Interrupt latches record the publisher's live revision at signal time.
  // A validation bundle that is cancelled by this latch may carry an older
  // borrowed revision because validation intentionally runs against one
  // immutable snapshot per pass.
  const auto revision = current_authoritative_collision_world_revision();
  if (collision_world_revision_is_set(revision)) {
    context.borrowed_collision_world_revision = revision;
  }
  return context;
}

void ExecutionKernel::emit_interrupt_events(const InterruptIngestResult& result) {
  for (const auto& event : result.events) {
    auto event_type = interrupt_event_type_from_name(event.type);
    if (!event_type) {
      continue;
    }
    emit_event({*event_type,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                event.details});
  }
}

InterruptIngestResult ExecutionKernel::publish_fast_interrupt_signal(
    const InterruptSignalUpdate& update) {
  return publish_fast_interrupt_signals({update});
}

InterruptIngestResult ExecutionKernel::publish_supervisory_interrupt_signal(
    const InterruptSignalUpdate& update) {
  if (!interrupt_phase_from_state(sm_.status().state)) {
    auto result = reject_interrupts_without_active_phase({update});
    emit_interrupt_events(result);
    return result;
  }
  auto result = interrupt_manager_.ingest(
      update,
      InterruptSourceClass::kLowRateSupervisory,
      current_interrupt_context(),
      clock_.now());
  request_validation_cancel_from_interrupt(result);
  if (result.latched) {
    const auto prev_state = sm_.status().state;
    if (prev_state != ExecutionState::kInterrupted) {
      auto transition = sm_.try_transition(ExecutionState::kInterrupted);
      if (transition.ok()) {
        auto snapshot = get_execution_snapshot();
        snapshot.timestamp_ns = clock_.now();
        snapshot.state = ExecutionState::kInterrupted;
        publish_execution_snapshot(std::move(snapshot));
        emit_event({KernelEventType::kStateTransition,
                    sm_.status().execution_id,
                    clock_.now(),
                    prev_state,
                    ExecutionState::kInterrupted,
                    std::nullopt,
                    {{"reason", "interrupt_latched"}}});
      }
    }
  }
  emit_interrupt_events(result);
  apply_latched_interrupt_preemption(result);
  return result;
}

InterruptIngestResult ExecutionKernel::publish_fast_interrupt_signals(
    const std::vector<InterruptSignalUpdate>& updates) {
  if (!interrupt_phase_from_state(sm_.status().state)) {
    auto result = reject_interrupts_without_active_phase(updates);
    emit_interrupt_events(result);
    return result;
  }
  auto result = interrupt_manager_.ingest_batch(
      updates,
      InterruptSourceClass::kFastExecutionRelevant,
      current_interrupt_context(),
      clock_.now());
  request_validation_cancel_from_interrupt(result);
  if (result.latched) {
    const auto prev_state = sm_.status().state;
    if (prev_state != ExecutionState::kInterrupted) {
      auto transition = sm_.try_transition(ExecutionState::kInterrupted);
      if (transition.ok()) {
        auto snapshot = get_execution_snapshot();
        snapshot.timestamp_ns = clock_.now();
        snapshot.state = ExecutionState::kInterrupted;
        publish_execution_snapshot(std::move(snapshot));
        emit_event({KernelEventType::kStateTransition,
                    sm_.status().execution_id,
                    clock_.now(),
                    prev_state,
                    ExecutionState::kInterrupted,
                    std::nullopt,
                    {{"reason", "interrupt_latched"}}});
      }
    }
  }
  emit_interrupt_events(result);
  apply_latched_interrupt_preemption(result);
  return result;
}

std::optional<InterruptLatch> ExecutionKernel::get_active_interrupt_latch() const {
  return interrupt_manager_.active_latch();
}

Result<InterruptIngestResult> ExecutionKernel::apply_interrupt_disposition(
    const InterruptDisposition& disposition) {
  auto result = interrupt_manager_.apply_disposition(disposition, clock_.now());
  if (!result.ok()) {
    return result;
  }
  emit_interrupt_events(result.value());
  if (!interrupt_manager_.active_latch() &&
      sm_.status().state == ExecutionState::kInterrupted) {
    const auto prev_state = sm_.status().state;
    auto transition = sm_.try_transition(ExecutionState::kInterventionRequired);
    if (transition.ok()) {
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.state = ExecutionState::kInterventionRequired;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id,
                  clock_.now(),
                  prev_state,
                  ExecutionState::kInterventionRequired,
                  std::nullopt,
                  {{"reason", "interrupt_disposition"}}});
    }
  }
  return result;
}

void ExecutionKernel::publish_execution_snapshot(ExecutionSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  if (snapshot.timestamp_ns == 0) {
    snapshot.timestamp_ns = clock_.now();
  }
  snapshot.sequence_number = next_snapshot_sequence_++;
  published_snapshot_ = std::move(snapshot);

  {
    std::lock_guard<std::mutex> mlock(metrics_mutex_);
    metrics_.snapshots_published++;
  }
}

bool ExecutionKernel::wait_for_completion(int64_t timeout_ns) {
  std::unique_lock<std::mutex> lock(completion_mutex_);
  if (timeout_ns > 0) {
    return completion_cv_.wait_for(
        lock, std::chrono::nanoseconds(timeout_ns),
        [this]() { return execution_complete_.load(); });
  }
  completion_cv_.wait(lock, [this]() { return execution_complete_.load(); });
  return true;
}

void ExecutionKernel::start_collision_monitoring() {
  std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
  collision_monitor_active_ = true;
  ++collision_monitor_generation_;
  if (collision_monitor_handle_ != 0) {
    scheduler_.cancel(collision_monitor_handle_);
    collision_monitor_handle_ = 0;
  }
  schedule_next_collision_monitor_tick(collision_monitor_generation_);
}

void ExecutionKernel::stop_collision_monitoring() {
  std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
  collision_monitor_active_ = false;
  ++collision_monitor_generation_;
  if (collision_monitor_handle_ != 0) {
    scheduler_.cancel(collision_monitor_handle_);
    collision_monitor_handle_ = 0;
  }
}

void ExecutionKernel::schedule_next_collision_monitor_tick(uint64_t generation) {
  collision_monitor_handle_ = scheduler_.schedule(
      clock_.now() + config_.collision_monitor_period_ns, [this, generation]() {
        {
          std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
          if (!collision_monitor_active_ || collision_monitor_generation_ != generation) {
            return;
          }
          collision_monitor_handle_ = 0;
        }

        (void)collision_check_tick();

        std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
        if (!collision_monitor_active_ ||
            collision_monitor_generation_ != generation ||
            sm_.status().state != ExecutionState::kExecuting) {
          return;
        }
        schedule_next_collision_monitor_tick(generation);
      });
}

std::vector<double> ExecutionKernel::interpolate_joint_positions(
    const TrajectoryDTO& trajectory, double time_s) const {
  if (trajectory.waypoints.empty()) {
    return {};
  }

  if (trajectory.waypoints.size() == 1 ||
      time_s <= trajectory.waypoints.front().time_from_start_s) {
    return trajectory.waypoints.front().positions_rad;
  }

  if (time_s >= trajectory.waypoints.back().time_from_start_s) {
    return trajectory.waypoints.back().positions_rad;
  }

  for (size_t i = 1; i < trajectory.waypoints.size(); ++i) {
    const auto& prev = trajectory.waypoints[i - 1];
    const auto& next = trajectory.waypoints[i];
    if (time_s > next.time_from_start_s) {
      continue;
    }

    const double dt = next.time_from_start_s - prev.time_from_start_s;
    if (dt <= 1e-9) {
      return next.positions_rad;
    }

    const double alpha =
        clamp_unit_interval((time_s - prev.time_from_start_s) / dt);
    std::vector<double> interpolated(prev.positions_rad.size(), 0.0);
    for (size_t joint = 0; joint < prev.positions_rad.size(); ++joint) {
      interpolated[joint] =
          prev.positions_rad[joint] +
          alpha * (next.positions_rad[joint] - prev.positions_rad[joint]);
    }
    return interpolated;
  }

  return trajectory.waypoints.back().positions_rad;
}

TrajectoryDTO ExecutionKernel::build_remaining_trajectory(
    const TrajectoryDTO& trajectory, const ExecutionSnapshot& snapshot) const {
  if (trajectory.waypoints.empty()) {
    return trajectory;
  }

  TrajectoryDTO remaining;
  remaining.joint_names = trajectory.joint_names;

  const double total_duration_s = trajectory.waypoints.back().time_from_start_s;
  const double elapsed_s =
      total_duration_s <= 1e-9
          ? 0.0
          : clamp_unit_interval(snapshot.progress) * total_duration_s;

  std::vector<double> current_positions = snapshot.joint_positions_rad;
  if (current_positions.empty()) {
    current_positions = interpolate_joint_positions(trajectory, elapsed_s);
  }

  TrajectoryWaypointDTO current_wp;
  current_wp.time_from_start_s = 0.0;
  current_wp.positions_rad = current_positions;
  remaining.waypoints.push_back(current_wp);

  for (const auto& waypoint : trajectory.waypoints) {
    if (waypoint.time_from_start_s <= elapsed_s + 1e-9) {
      continue;
    }
    auto shifted = waypoint;
    shifted.time_from_start_s -= elapsed_s;
    remaining.waypoints.push_back(std::move(shifted));
  }

  if (remaining.waypoints.size() == 1) {
    auto final_wp = trajectory.waypoints.back();
    final_wp.time_from_start_s = std::max(0.0, total_duration_s - elapsed_s);
    if (final_wp.positions_rad != current_positions ||
        final_wp.time_from_start_s > 1e-9) {
      remaining.waypoints.push_back(std::move(final_wp));
    }
  }

  return remaining;
}

void ExecutionKernel::async_execute_worker(
    std::stop_token st, ExecuteRequestDTO request, uint64_t exec_id,
    std::optional<TrajectoryDTO> cached_trajectory) {
  try {
    // Capture initial state for correct prev_state in events (B-2)
    const auto worker_entry_state = sm_.status().state;

    // Check for early cancellation
    if (st.stop_requested()) {
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    TrajectoryDTO trajectory;
    const bool cache_hit = cached_trajectory.has_value();
    if (cache_hit) {
      trajectory = std::move(*cached_trajectory);
    } else {
      // Schedule planning timeout (W-2)
      ScheduleHandle planning_timeout_handle = 0;
      const auto planning_start_ns = clock_.now();
      const int64_t planning_timeout_ns =
          (request.timeouts.planning_timeout_s > 0)
              ? static_cast<int64_t>(request.timeouts.planning_timeout_s * 1e9)
              : 0;
      if (planning_timeout_ns > 0) {
        const int64_t deadline = planning_start_ns + planning_timeout_ns;
        // Monitoring lane: the watchdog must fire even when the planner
        // and collision-monitor are blocked behind the scene lock. See
        // spec/220 threading invariants and ADR-0019.
        planning_timeout_handle = scheduler_.schedule(
            deadline,
            [this]() { stop_policy_.request_hard_stop(); },
            SchedulerPriority::Monitoring);
      }

      auto plan_result = plan_trajectory(request);
      const bool planning_timed_out =
          planning_timeout_ns > 0 &&
          (clock_.now() - planning_start_ns) >= planning_timeout_ns;

      // Cancel planning timeout (may have already fired)
      if (planning_timeout_handle != 0) {
        scheduler_.cancel(planning_timeout_handle);
      }

      // Check for stop/reset after planning (spec 230 §6.2)
      if (planning_timed_out || stop_policy_.stop_requested()) {
        auto prev = worker_entry_state;  // B-2: use captured state
        const auto message = planning_timed_out
                                 ? "Planning timed out"
                                 : "Planning interrupted by stop";
        sm_.set_last_error(ErrorCode::kPlanningTimeout);
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorPlanningTimeout, message, {}}, {}});
        sm_.try_transition(ExecutionState::kStopped);
        auto stopped_snapshot = get_execution_snapshot();
        stopped_snapshot.timestamp_ns = clock_.now();
        stopped_snapshot.execution_id = exec_id;
        stopped_snapshot.move_id = request.move_id;
        stopped_snapshot.state = ExecutionState::kStopped;
        stopped_snapshot.error = ErrorDTO{kErrorPlanningTimeout, message, {}};
        publish_execution_snapshot(std::move(stopped_snapshot));
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kStopped, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      // Check if reset was requested (state would be Idle)
      if (sm_.status().state == ExecutionState::kIdle) {
        auto prev = worker_entry_state;  // B-2: use captured state
        sm_.set_last_error(ErrorCode::kPlanningTimeout);
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorPlanningTimeout, "Planning interrupted by reset", {}}, {}});
        auto idle_snapshot = get_execution_snapshot();
        idle_snapshot.timestamp_ns = clock_.now();
        idle_snapshot.execution_id = 0;
        idle_snapshot.move_id.clear();
        idle_snapshot.state = ExecutionState::kIdle;
        idle_snapshot.progress = 0.0;
        idle_snapshot.error = ErrorDTO{kErrorPlanningTimeout, "Planning interrupted by reset", {}};
        publish_execution_snapshot(std::move(idle_snapshot));
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kIdle, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      if (!plan_result.ok()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, plan_result.error(), {}});
        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.plans_failed++;
        }
        sm_.set_last_error(error_code_from_dto(plan_result.error()));
        sm_.try_transition(ExecutionState::kFailed);
        auto failed_snapshot = get_execution_snapshot();
        failed_snapshot.timestamp_ns = clock_.now();
        failed_snapshot.execution_id = exec_id;
        failed_snapshot.move_id = request.move_id;
        failed_snapshot.state = ExecutionState::kFailed;
        failed_snapshot.error = plan_result.error();
        publish_execution_snapshot(std::move(failed_snapshot));
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     ExecutionState::kPlanning, ExecutionState::kFailed, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      trajectory = std::move(plan_result.value());

      emit_event({KernelEventType::kPlanCompleted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.plans_succeeded++;
      }

      // Planning -> Validating
      auto prev_state2 = sm_.status().state;
      auto r2 = sm_.try_transition(ExecutionState::kValidating);
      if (!r2.ok()) {
        sm_.set_last_error(ErrorCode::kInternalError);
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev_state2, ExecutionState::kFailed, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state2, ExecutionState::kValidating, std::nullopt, {}});
      auto validating_snapshot = get_execution_snapshot();
      validating_snapshot.timestamp_ns = clock_.now();
      validating_snapshot.execution_id = exec_id;
      validating_snapshot.move_id = request.move_id;
      validating_snapshot.state = ExecutionState::kValidating;
      validating_snapshot.progress = 0.0;
      publish_execution_snapshot(std::move(validating_snapshot));
    }

    emit_event({KernelEventType::kValidationStarted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});

    auto val_result = validate_trajectory(trajectory, request);
    const auto validation_revision_after =
        current_authoritative_collision_world_revision();
    if (val_result.ok() &&
        collision_world_revision_is_set(validation_revision_after) &&
        !same_collision_world_revision(val_result.value(),
                                       validation_revision_after)) {
      val_result = Result<CollisionWorldRevision>::Err(collision_world_changed_error(
          val_result.value(), validation_revision_after,
          "pre_execution_validation"));
    }
    if (!val_result.ok()) {
      if (validation_cancelled_by_interrupt(val_result.error()) &&
          sm_.status().state == ExecutionState::kInterrupted) {
        emit_event({KernelEventType::kValidationFailed, exec_id, clock_.now(),
                     {}, {}, std::nullopt,
                     {{"reason", "validation cancelled by interrupt"}}});
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, val_result.error(), {}});
        mark_active_interrupt_preemption_result(
            InterruptPreemptionResult::kSucceeded);
        auto interrupted_snapshot = get_execution_snapshot();
        interrupted_snapshot.timestamp_ns = clock_.now();
        interrupted_snapshot.execution_id = exec_id;
        interrupted_snapshot.move_id = request.move_id;
        interrupted_snapshot.state = ExecutionState::kInterrupted;
        interrupted_snapshot.error = val_result.error();
        publish_execution_snapshot(std::move(interrupted_snapshot));
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      // Emit validation failed event BEFORE error raised
      emit_event({KernelEventType::kValidationFailed, exec_id, clock_.now(),
                   {}, {}, std::nullopt, {{"reason", "validation failed"}}});
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                   {}, {}, val_result.error(), {}});
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.validations_failed++;
      }
      sm_.set_last_error(error_code_from_dto(val_result.error()));
      sm_.try_transition(ExecutionState::kFailed);
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kFailed;
      snapshot.error = val_result.error();
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   ExecutionState::kValidating, ExecutionState::kFailed, std::nullopt, {}});
      if (request.cache_mode != CacheMode::kDisable &&
          cache_.lookup(request.move_id).has_value()) {
        cache_.invalidate(request.move_id);
        // Emit cache invalidated event
        emit_event({KernelEventType::kCacheInvalidated, exec_id, clock_.now(),
                     {}, {}, std::nullopt,
                     {{"move_id", request.move_id},
                      {"reason", "validation_failed"}}});
        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.cache_invalidations++;
        }
      }
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    emit_event({KernelEventType::kValidationPassed, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.validations_passed++;
    }

    // Check for stop/reset after validation (spec 230 §6.2)
    if (stop_policy_.stop_requested()) {
      auto prev = sm_.status().state;
      // Stop -> go to Stopped with kInternalError
      sm_.set_last_error(ErrorCode::kInternalError);
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                   {}, {}, ErrorDTO{kErrorInternalError, "Validation interrupted by stop", {}}, {}});
      sm_.try_transition(ExecutionState::kStopped);
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kStopped;
      snapshot.error = ErrorDTO{kErrorInternalError, "Validation interrupted by stop", {}};
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev, ExecutionState::kStopped, std::nullopt, {}});
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }
    // Check if reset was requested (state would be Idle)
    if (sm_.status().state == ExecutionState::kIdle) {
      auto prev = sm_.status().state;
      sm_.set_last_error(ErrorCode::kInternalError);
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                   {}, {}, ErrorDTO{kErrorInternalError, "Validation interrupted by reset", {}}, {}});
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = 0;
      snapshot.move_id.clear();
      snapshot.state = ExecutionState::kIdle;
      snapshot.progress = 0.0;
      snapshot.error = ErrorDTO{kErrorInternalError, "Validation interrupted by reset", {}};
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev, ExecutionState::kIdle, std::nullopt, {}});
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    // Store in cache
    if (request.cache_mode != CacheMode::kDisable && !cache_hit &&
        val_result.ok() &&
        collision_world_revision_is_set(val_result.value())) {
      // Chunk 2: tag every cache entry with the collision-world revision
      // that validation actually used. The post-validation equality check above
      // ensures a trajectory validated at revision N is never stored as N+1.
      cache_.store(request.move_id, trajectory, val_result.value());
    }

    // Validating -> Executing
    auto prev_state3 = sm_.status().state;
    auto r3 = sm_.try_transition(ExecutionState::kExecuting);
    if (!r3.ok()) {
      sm_.set_last_error(ErrorCode::kInternalError);
      sm_.try_transition(ExecutionState::kFailed);
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state3, ExecutionState::kFailed, std::nullopt, {}});
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                 prev_state3, ExecutionState::kExecuting, std::nullopt, {}});

    emit_event({KernelEventType::kExecuteStarted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = ExecutionState::kExecuting;
    publish_execution_snapshot(std::move(snapshot));

    // Execution timeout enforcement (W-1)
    const auto exec_start_ns = clock_.now();
    const int64_t exec_timeout_ns = (request.timeouts.execution_timeout_s > 0)
        ? static_cast<int64_t>(request.timeouts.execution_timeout_s * 1e9) : 0;

    auto execution_handle = execution_backend_.execute_trajectory(trajectory);
    auto active_trajectory = trajectory;
    core::ExecutionSnapshot final_snapshot;
    core::ExecutionSnapshot latest_backend_snapshot;
    start_collision_monitoring();

    bool pause_in_progress = false;
    int64_t last_runtime_scale_sample_ns = exec_start_ns;
    double runtime_scaled_execution_ns = 0.0;

    while (true) {
      bool completed = true;
      if (execution_handle.wait_for_completion) {
        completed = execution_handle.wait_for_completion(kExecutionPollNs);
      }

      if (execution_handle.get_snapshot) {
        latest_backend_snapshot = execution_handle.get_snapshot();
        latest_backend_snapshot.execution_id = exec_id;
        latest_backend_snapshot.move_id = request.move_id;
        publish_execution_snapshot(latest_backend_snapshot);
        sm_.set_progress(latest_backend_snapshot.progress);
      }

      const auto now_ns = clock_.now();
      const bool paused =
          latest_backend_snapshot.state == ExecutionState::kPaused ||
          sm_.status().state == ExecutionState::kPaused;
      if (paused) {
        if (!pause_in_progress) {
          pause_in_progress = true;
        }
      } else if (pause_in_progress) {
        pause_in_progress = false;
      }

      if (!paused) {
        const int64_t dt_ns = std::max<int64_t>(0, now_ns - last_runtime_scale_sample_ns);
        const double scale = clamp_unit_interval(
            runtime_velocity_scale_.load(std::memory_order_acquire));
        runtime_scaled_execution_ns += static_cast<double>(dt_ns) * scale;
      }
      last_runtime_scale_sample_ns = now_ns;

      // Check execution timeout (W-1)
      if (exec_timeout_ns > 0 && runtime_scaled_execution_ns >= static_cast<double>(exec_timeout_ns)) {
        stop_collision_monitoring();
        (void)execution_backend_.stop();
        const bool stop_completed = drain_execution_handle_bounded(execution_handle);
        auto prev = sm_.status().state;
        sm_.set_last_error(stop_completed ? ErrorCode::kExecutionTimeout
                                           : ErrorCode::kStopFailed);
        const auto terminal_state = stop_completed ? ExecutionState::kFailed
                                                   : ExecutionState::kInterventionRequired;
        sm_.try_transition(terminal_state);

        auto to_snapshot = latest_backend_snapshot;
        to_snapshot.timestamp_ns = clock_.now();
        to_snapshot.execution_id = exec_id;
        to_snapshot.move_id = request.move_id;
        to_snapshot.state = terminal_state;
        ErrorDTO terminal_error = stop_completed
            ? ErrorDTO{kErrorExecutionTimeout, "Execution timeout exceeded", {}}
            : operator_intervention_error(
                  ErrorCode::kStopFailed,
                  "Execution timeout exceeded and software stop completion was not observed",
                  "execution_timeout_stop_drain",
                  prev,
                  exec_id,
                  request.move_id);
        to_snapshot.error = terminal_error;
        publish_execution_snapshot(std::move(to_snapshot));

        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, terminal_error, {}});
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, terminal_state, std::nullopt, {}});

        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.executions_failed++;
        }
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      const auto loop_state = sm_.status().state;
      if (loop_state == ExecutionState::kStopped ||
          loop_state == ExecutionState::kIdle ||
          loop_state == ExecutionState::kFailed ||
          loop_state == ExecutionState::kInterventionRequired) {
        break;
      }
      if (loop_state == ExecutionState::kInterrupted) {
        if (auto latch = interrupt_manager_.active_latch();
            latch &&
            latch->preemption_action_requested ==
                InterruptPreemptionAction::kPreemptTrajectory) {
          stop_collision_monitoring();
          execution_complete_.store(true);
          completion_cv_.notify_all();
          return;
        }
      }

      if (latest_backend_snapshot.state == ExecutionState::kPaused ||
          loop_state == ExecutionState::kPaused) {
        stop_collision_monitoring();

        if (resume_requested_.exchange(false)) {
          emit_event({KernelEventType::kValidationStarted, exec_id, clock_.now(),
                      {}, {}, std::nullopt, {}});

          auto resumed_trajectory =
              build_remaining_trajectory(active_trajectory, latest_backend_snapshot);
          auto resume_validation = validate_trajectory(resumed_trajectory, request);
          const auto resume_revision_after =
              current_authoritative_collision_world_revision();
          if (resume_validation.ok() &&
              collision_world_revision_is_set(resume_revision_after) &&
              !same_collision_world_revision(resume_validation.value(),
                                             resume_revision_after)) {
            resume_validation = Result<CollisionWorldRevision>::Err(
                collision_world_changed_error(
                    resume_validation.value(), resume_revision_after,
                    "resume_validation"));
          }
          if (!resume_validation.ok()) {
            if (validation_cancelled_by_interrupt(resume_validation.error()) &&
                sm_.status().state == ExecutionState::kInterrupted) {
              emit_event({KernelEventType::kValidationFailed, exec_id, clock_.now(),
                          {}, {}, std::nullopt,
                          {{"reason", "resume validation cancelled by interrupt"}}});
              emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                          {}, {}, resume_validation.error(), {}});
              mark_active_interrupt_preemption_result(
                  InterruptPreemptionResult::kSucceeded);
              auto interrupted_snapshot = latest_backend_snapshot;
              interrupted_snapshot.timestamp_ns = clock_.now();
              interrupted_snapshot.execution_id = exec_id;
              interrupted_snapshot.move_id = request.move_id;
              interrupted_snapshot.state = ExecutionState::kInterrupted;
              interrupted_snapshot.error = resume_validation.error();
              publish_execution_snapshot(std::move(interrupted_snapshot));
              execution_complete_.store(true);
              completion_cv_.notify_all();
              return;
            }
            emit_event({KernelEventType::kValidationFailed, exec_id, clock_.now(),
                        {}, {}, std::nullopt,
                        {{"reason", "resume validation failed"}}});
            emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                        {}, {}, resume_validation.error(), {}});
            {
              std::lock_guard<std::mutex> lock(metrics_mutex_);
              metrics_.validations_failed++;
            }
            sm_.set_last_error(error_code_from_dto(resume_validation.error()));
            (void)execution_backend_.stop();
            const bool stop_completed = drain_execution_handle_bounded(execution_handle);
            auto prev_state = sm_.status().state;
            const auto terminal_state = stop_completed
                ? ExecutionState::kFailed
                : ExecutionState::kInterventionRequired;
            if (!stop_completed) {
              sm_.set_last_error(ErrorCode::kStopFailed);
            }
            sm_.try_transition(terminal_state);
            auto failed_snapshot = latest_backend_snapshot;
            failed_snapshot.timestamp_ns = clock_.now();
            failed_snapshot.execution_id = exec_id;
            failed_snapshot.move_id = request.move_id;
            failed_snapshot.state = terminal_state;
            failed_snapshot.error = stop_completed
                ? resume_validation.error()
                : operator_intervention_error(
                      ErrorCode::kStopFailed,
                      "Resume validation failed and software stop completion was not observed",
                      "resume_validation_stop_drain",
                      prev_state,
                      exec_id,
                      request.move_id);
            publish_execution_snapshot(std::move(failed_snapshot));
            emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                        prev_state, terminal_state, std::nullopt, {}});
            execution_complete_.store(true);
            completion_cv_.notify_all();
            return;
          }

          emit_event({KernelEventType::kValidationPassed, exec_id, clock_.now(),
                      {}, {}, std::nullopt, {}});
          {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            metrics_.validations_passed++;
          }

          auto prev_state = sm_.status().state;
          auto resume_transition = sm_.try_transition(ExecutionState::kExecuting);
          if (!resume_transition.ok()) {
            sm_.set_last_error(ErrorCode::kResumeFailed);
            emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                        {}, {},
                        ErrorDTO{kErrorResumeFailed,
                                 "Resume transition to Executing failed", {}},
                        {}});
            sm_.try_transition(ExecutionState::kFailed);
            auto failed_snapshot = latest_backend_snapshot;
            failed_snapshot.timestamp_ns = clock_.now();
            failed_snapshot.execution_id = exec_id;
            failed_snapshot.move_id = request.move_id;
            failed_snapshot.state = ExecutionState::kFailed;
            failed_snapshot.error = ErrorDTO{kErrorResumeFailed,
                                             "Resume transition to Executing failed", {}};
            publish_execution_snapshot(std::move(failed_snapshot));
            emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                        prev_state, ExecutionState::kFailed, std::nullopt, {}});
            execution_complete_.store(true);
            completion_cv_.notify_all();
            return;
          }

          emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                      prev_state, ExecutionState::kExecuting, std::nullopt, {}});
          emit_event({KernelEventType::kExecuteStarted, exec_id, clock_.now(),
                      {}, {}, std::nullopt, {}});

          auto resumed_snapshot = latest_backend_snapshot;
          resumed_snapshot.timestamp_ns = clock_.now();
          resumed_snapshot.execution_id = exec_id;
          resumed_snapshot.move_id = request.move_id;
          resumed_snapshot.state = ExecutionState::kExecuting;
          resumed_snapshot.error.reset();
          publish_execution_snapshot(std::move(resumed_snapshot));

          execution_handle = execution_backend_.resume(resumed_trajectory);
          active_trajectory = std::move(resumed_trajectory);
          start_collision_monitoring();
        }

        if (completed) {
          // Some real backends implement pause by cancelling the active
          // controller goal. Their execution handle is then terminal even
          // though the kernel state is intentionally kPaused. Keep the kernel
          // worker alive so a later resume() can revalidate and dispatch the
          // remaining trajectory; otherwise the state machine can advance to
          // kValidating with no worker left to consume resume_requested_.
          std::this_thread::sleep_for(std::chrono::nanoseconds(kExecutionPollNs));
        }
        continue;
      }

      if (completed) {
        break;
      }
    }

    stop_collision_monitoring();

    if (execution_handle.get_snapshot) {
      final_snapshot = execution_handle.get_snapshot();
      final_snapshot.execution_id = exec_id;
      final_snapshot.move_id = request.move_id;
      publish_execution_snapshot(final_snapshot);
      sm_.set_progress(final_snapshot.progress);
    } else {
      final_snapshot.execution_id = exec_id;
      final_snapshot.move_id = request.move_id;
      final_snapshot.state = ExecutionState::kCompleted;
      final_snapshot.progress = 1.0;
      publish_execution_snapshot(final_snapshot);
      sm_.set_progress(1.0);
    }

    if (final_snapshot.state == ExecutionState::kStopped ||
        sm_.status().state == ExecutionState::kStopped ||
        sm_.status().state == ExecutionState::kIdle) {
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    if (final_snapshot.state == ExecutionState::kFailed) {
      if (final_snapshot.error.has_value()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                    {}, {}, final_snapshot.error, {}});
        sm_.set_last_error(error_code_from_dto(*final_snapshot.error));
      } else {
        sm_.set_last_error(ErrorCode::kInternalError);
      }
      auto prev_state4 = sm_.status().state;
      sm_.try_transition(ExecutionState::kFailed);
      auto snapshot = final_snapshot;
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kFailed;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                  prev_state4, ExecutionState::kFailed, std::nullopt, {}});
      {
        std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
        metrics_.executions_failed++;
      }
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    if (final_snapshot.state == ExecutionState::kInterventionRequired) {
      if (final_snapshot.error.has_value()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                    {}, {}, final_snapshot.error, {}});
        sm_.set_last_error(error_code_from_dto(*final_snapshot.error));
      } else {
        sm_.set_last_error(ErrorCode::kStopFailed);
      }
      auto prev_state4 = sm_.status().state;
      sm_.try_transition(ExecutionState::kInterventionRequired);
      auto snapshot = final_snapshot;
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kInterventionRequired;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                  prev_state4, ExecutionState::kInterventionRequired,
                  std::nullopt, {}});
      {
        std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
        metrics_.executions_failed++;
      }
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    emit_event({KernelEventType::kExecuteCompleted, exec_id, clock_.now(),
                {}, {}, std::nullopt, {}});

    auto prev_state4 = sm_.status().state;
    sm_.try_transition(ExecutionState::kCompleted);
    auto snapshot2 = final_snapshot;
    snapshot2.timestamp_ns = clock_.now();
    snapshot2.execution_id = exec_id;
    snapshot2.move_id = request.move_id;
    snapshot2.state = ExecutionState::kCompleted;
    snapshot2.progress = 1.0;
    publish_execution_snapshot(std::move(snapshot2));
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                prev_state4, ExecutionState::kCompleted, std::nullopt, {}});
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_completed++;
    }

  } catch (const std::exception& e) {
    stop_collision_monitoring();
    sm_.set_last_error(ErrorCode::kInternalError);
    auto prev_state = sm_.status().state;
    sm_.try_transition(ExecutionState::kFailed);
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = ExecutionState::kFailed;
    snapshot.error = ErrorDTO{kErrorInternalError, e.what(), {}};
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                 prev_state, ExecutionState::kFailed, std::nullopt, {}});
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_failed++;
    }
  }

  execution_complete_.store(true);
  completion_cv_.notify_all();
}

Result<uint64_t> ExecutionKernel::execute(const ExecuteRequestDTO& request) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  // `kInterventionRequired` MUST be cleared by an explicit
  // `acknowledge_intervention(principal_id, reason)` call, not silently by
  // `execute()`. An implicit clear here would erase the operator
  // intervention boundary — any caller (including LLMs) could re-issue
  // `execute` to bypass the intervention gate with no audit event. The
  // policy decision about whether the leftover state is safe to clear
  // lives in the caller (cycle manager Run-side auto-ack, MCP, operator UI).
  if (sm_.status().state == ExecutionState::kInterventionRequired) {
    return Result<uint64_t>::Err(ErrorDTO{
        kErrorInvalidRequest,
        "Execution is in InterventionRequired; explicit acknowledge_intervention required",
        {{"requires_operator_intervention", "true"},
         {"reason_code", "intervention_required"},
         {"state", to_string(sm_.status().state)}}});
  }

  if (sm_.status().state == ExecutionState::kFailed ||
      sm_.status().state == ExecutionState::kStopped) {
    const auto prev_state = sm_.status().state;
    auto reset_result = stop_policy_.request_reset();
    if (!reset_result.ok()) {
      return Result<uint64_t>::Err(reset_result.error());
    }
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      active_move_id_.clear();
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id,
                clock_.now(), prev_state, ExecutionState::kIdle, std::nullopt, {}});
  }

  if (sm_.status().state != ExecutionState::kIdle) {
    return Result<uint64_t>::Err(ErrorCode::kExecutionRejected);
  }

  auto sync_result = execution_backend_.sync_observed_state_before_planning();
  if (!sync_result.ok()) {
    return Result<uint64_t>::Err(sync_result.error());
  }

  ExecuteRequestDTO resolved_request(request);
  auto resolved_motion = resolve_motion_spec(request);
  if (!resolved_motion.ok()) {
    if (request.cache_mode != CacheMode::kDisable &&
        cache_.lookup(request.move_id).has_value()) {
      cache_.invalidate(request.move_id);
      emit_event({KernelEventType::kCacheInvalidated,
                  0,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"move_id", request.move_id},
                   {"reason", "goal_unverifiable"}}});
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.cache_invalidations++;
    }
    return Result<uint64_t>::Err(resolved_motion.error());
  }
  resolved_request.motion = std::move(resolved_motion.value());

  std::optional<RobotStateDTO> synced_state;
  try {
    synced_state = context_.get_current_state();
  } catch (...) {
    synced_state.reset();
  }

  PreparedExecutionStart prepared;
  if (request.cache_mode != CacheMode::kDisable) {
    // Gate cache hits on the authoritative snapshot revision, not on the
    // planner context. A cached trajectory validated under revision N MUST NOT
    // be served when the collision-world snapshot is at revision N+1.
    // A default revision means the authoritative publisher is unavailable; the
    // cache then declines to serve a hit and validation fails closed later.
    const auto active_revision =
        current_authoritative_collision_world_revision();
    auto cached =
        cache_.lookup_with_revision(request.move_id, active_revision);
    if (cached.has_value()) {
      const auto goal_match =
          cached_trajectory_matches_request_goal(context_, *cached, resolved_request);
      const auto start_match =
          synced_state.has_value()
              ? cached_trajectory_matches_current_start(
                    *cached, *synced_state, request.tolerances.joint_position_tol_rad)
              : incompatible_cache("start_state_changed");
      if (goal_match.compatible && start_match.compatible) {
        prepared.cache_hit = true;
        prepared.trajectory = std::move(*cached);
      } else {
        cache_.invalidate(request.move_id);
        prepared.cache_invalidated = true;
        prepared.cache_invalidation_reason =
            !start_match.compatible ? start_match.reason : goal_match.reason;
      }
    } else {
      prepared.cache_miss = true;
    }
  }

  uint64_t exec_id = next_execution_id_++;
  auto prev_state = sm_.status().state;
  sm_.set_execution_id(exec_id);
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    active_move_id_ = request.move_id;
  }
  const auto initial_state =
      prepared.cache_hit ? ExecutionState::kValidating
                         : ExecutionState::kPlanning;
  auto transition = sm_.try_transition(initial_state);
  if (!transition.ok()) {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    active_move_id_.clear();
    sm_.set_execution_id(0);
    return Result<uint64_t>::Err(ErrorCode::kInternalError);
  }

  execution_complete_.store(false);
  resume_requested_.store(false);
  stop_collision_monitoring();

  emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
              prev_state, initial_state, std::nullopt, {}});

  if (prepared.cache_invalidated) {
    emit_event({KernelEventType::kCacheInvalidated,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"move_id", request.move_id},
                 {"reason", prepared.cache_invalidation_reason}}});
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.cache_invalidations++;
  }

  if (prepared.cache_hit) {
    emit_event(
        {KernelEventType::kCacheHit, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.cache_hits++;
  } else {
    if (prepared.cache_miss) {
      emit_event({KernelEventType::kCacheMiss, exec_id, clock_.now(),
                  {}, {}, std::nullopt, {{"move_id", request.move_id}}});
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.cache_misses++;
    }
    emit_event({KernelEventType::kPlanStarted, exec_id, clock_.now(),
                {}, {}, std::nullopt, {}});
  }

  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = exec_id;
  snapshot.move_id = request.move_id;
  snapshot.state = initial_state;
  snapshot.progress = 0.0;
  try {
    snapshot.joint_positions_rad = context_.get_current_state().positions_rad;
  } catch (...) {
    // Leave joints empty if the planning context cannot provide state here.
  }
  publish_execution_snapshot(std::move(snapshot));

  worker_thread_ = std::jthread(
      [this, request_copy = ExecuteRequestDTO(resolved_request),
       exec_id,
       cached_trajectory = prepared.cache_hit
                               ? std::make_optional(std::move(prepared.trajectory))
                               : std::nullopt](std::stop_token st) mutable {
        async_execute_worker(st, std::move(request_copy), exec_id,
                             std::move(cached_trajectory));
      });

  return Result<uint64_t>::Ok(exec_id);
}

StopResultDTO ExecutionKernel::stop() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  // ADR-0016: stop() from kStreaming maps to a soft-end of the active
  // streaming session using the session's configured soft-stop strategy
  // (spring_back by default; velocity_scaling when qualification permits).
  // Distinct from the hard cancel path used by reset().
  uint64_t streaming_session_to_end = 0;
  SoftStopStrategy streaming_strategy = SoftStopStrategy::kSpringBack;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (sm_.status().state == ExecutionState::kStreaming &&
        active_streaming_session_id_ != 0) {
      streaming_session_to_end = active_streaming_session_id_;
      streaming_strategy = active_soft_stop_strategy_;
    }
  }
  if (streaming_session_to_end != 0) {
    auto end_result = end_streaming_session_unlocked(streaming_session_to_end,
                                                     streaming_strategy);
    if (end_result.accepted) {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.stops_issued++;
    }
    return StopResultDTO{end_result.accepted, end_result.error};
  }

  auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_hard_stop();
  if (result.ok()) {
    stop_collision_monitoring();
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    // Emit kStopIssued event
    emit_event({KernelEventType::kStopIssued, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt, {}});
    // Emit state transition event
    auto new_state = sm_.status().state;
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                prev_state, new_state, std::nullopt, {}});
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.stops_issued++;
    return StopResultDTO{true, std::nullopt};
  }
  return StopResultDTO{false, result.error()};
}

PauseResultDTO ExecutionKernel::pause() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  auto result = stop_policy_.request_pause();
  if (result.ok()) {
    stop_collision_monitoring();
    (void)execution_backend_.pause();
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    // Emit kPauseRequested event
    emit_event({KernelEventType::kPauseRequested, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt, {}});
    return PauseResultDTO{true, std::nullopt};
  }
  return PauseResultDTO{false, result.error()};
}

ResumeResultDTO ExecutionKernel::resume() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_resume();
  if (result.ok()) {
    resume_requested_.store(true);
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = ExecutionState::kValidating;
    snapshot.error.reset();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    // Emit kResumeRequested event
    emit_event({KernelEventType::kResumeRequested, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt, {}});
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                prev_state, sm_.status().state, std::nullopt, {}});
    return ResumeResultDTO{true, std::nullopt};
  }
  return ResumeResultDTO{false, result.error()};
}

ResetResultDTO ExecutionKernel::reset() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  auto prev_state = sm_.status().state;
  cancel_active_streaming_target_validation("streaming_session_reset");
  if (get_active_interrupt_latch()) {
    auto clear = interrupt_manager_.clear_active_latch("reset", clock_.now());
    if (clear.ok()) {
      emit_interrupt_events(clear.value());
    }
  }

  // ADR-0016: reset() is the HARD-cancel path (max-decel cancel via
  // backend.reset(), no soft-stop trajectory). Clear streaming session
  // bookkeeping unconditionally when set — covers the kStreaming case
  // and the recovery-from-InterventionRequired case (an error during
  // streaming might leave the session id non-zero even after the state
  // machine transitioned away from kStreaming). Without this clear, a
  // subsequent begin_streaming_session would be rejected with
  // "session already active".
  uint64_t closed_streaming_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (active_streaming_session_id_ != 0) {
      closed_streaming_id = active_streaming_session_id_;
      active_streaming_session_id_ = 0;
      active_backend_session_id_ = 0;
      ++active_streaming_generation_;
      active_streaming_config_ = StreamingSessionConfigDTO{};
      active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
      last_accepted_cartesian_pos_valid_ = false;
      last_accepted_cartesian_ts_ns_ = 0;
      last_commanded_joints_valid_ = false;
      last_commanded_joints_.clear();
    }
  }
  if (closed_streaming_id != 0) {
    emit_event({KernelEventType::kStreamingSessionEnded,
                0,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"session_id", std::to_string(closed_streaming_id)},
                 {"end_reason", "reset_hard_cancel"}}});
  }

  if (prev_state == ExecutionState::kIdle) {
    auto backend_reset = execution_backend_.reset();
    if (!backend_reset.accepted) {
      return backend_reset;
    }
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      active_move_id_.clear();
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    return ResetResultDTO{true, std::nullopt};
  }
  stop_collision_monitoring();
  resume_requested_.store(false);

  auto backend_reset = execution_backend_.reset();
  if (!backend_reset.accepted) {
    auto error = backend_reset.error.value_or(
        operator_intervention_error(ErrorCode::kResetFailed,
                                    "Execution backend reset failed",
                                    "backend_reset",
                                    prev_state,
                                    sm_.status().execution_id,
                                    ""));
    error = ensure_operator_intervention_details(std::move(error),
                                                 "backend_reset",
                                                 prev_state,
                                                 sm_.status().execution_id,
                                                 "");
    sm_.set_last_error(error_code_from_dto(error));
    sm_.try_transition(ExecutionState::kInterventionRequired);

    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = ExecutionState::kInterventionRequired;
    snapshot.error = error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised, sm_.status().execution_id,
                clock_.now(), {}, {}, error, {}});
    if (prev_state != ExecutionState::kInterventionRequired) {
      emit_event({KernelEventType::kStateTransition, sm_.status().execution_id,
                  clock_.now(), prev_state, ExecutionState::kInterventionRequired,
                  std::nullopt, {}});
    }
    return ResetResultDTO{false, error};
  }

  auto result = stop_policy_.request_reset();
  if (result.ok()) {
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      active_move_id_.clear();
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    // Emit state transition event to Idle
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                prev_state, ExecutionState::kIdle, std::nullopt, {}});
    return ResetResultDTO{true, std::nullopt};
  }
  return ResetResultDTO{false, result.error()};
}

AcknowledgeCompletedResultDTO ExecutionKernel::acknowledge_completed(
    uint64_t execution_id) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  const auto status = sm_.status();
  if (status.state != ExecutionState::kCompleted) {
    return AcknowledgeCompletedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Can only acknowledge a Completed execution",
                 {{"state", to_string(status.state)}}}};
  }
  if (status.execution_id != execution_id) {
    return AcknowledgeCompletedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Execution acknowledgement id mismatch",
                 {{"current_execution_id", std::to_string(status.execution_id)},
                  {"requested_execution_id", std::to_string(execution_id)}}}};
  }

  auto result = sm_.reset();
  if (!result.ok()) {
    return AcknowledgeCompletedResultDTO{false, result.error()};
  }
  sm_.set_execution_id(0);
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    active_move_id_.clear();
  }

  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = 0;
  snapshot.state = ExecutionState::kIdle;
  snapshot.progress = 0.0;
  publish_execution_snapshot(std::move(snapshot));

  emit_event({KernelEventType::kCompletedAcknowledged,
              execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"acknowledged_execution_id", std::to_string(execution_id)}}});
  emit_event({KernelEventType::kStateTransition,
              execution_id,
              clock_.now(),
              ExecutionState::kCompleted,
              ExecutionState::kIdle,
              std::nullopt,
              {}});

  return AcknowledgeCompletedResultDTO{true, std::nullopt};
}

AcknowledgeInterventionResultDTO ExecutionKernel::acknowledge_intervention(
    std::string principal_id, std::string reason) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  const auto status = sm_.status();
  if (status.state != ExecutionState::kInterventionRequired) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "acknowledge_intervention is valid only from InterventionRequired",
                 {{"state", to_string(status.state)}}}};
  }

  // Resync the backend before clearing the intervention flag. Without this,
  // a downstream `execute()` call would race against backend state that
  // still reflects the pre-intervention error. Mirrors the previous implicit
  // path in execute() but with explicit operator authority.
  auto backend_reset = execution_backend_.reset();
  if (!backend_reset.accepted) {
    return AcknowledgeInterventionResultDTO{
        false,
        backend_reset.error.value_or(
            ErrorDTO{kErrorResetFailed,
                     "Execution backend could not resync from intervention_required",
                     {{"requires_operator_intervention", "true"},
                      {"resync_required", "true"},
                      {"phase", "acknowledge_intervention_resync"}}})};
  }

  // Use `stop_policy_.request_reset()` rather than `sm_.reset()` directly
  // (which is what `acknowledge_completed` does) because intervention can
  // be entered with a pending scheduled soft-stop or a sticky `stop_requested_`
  // flag in StopPolicy. `request_reset()` cancels the scheduled callback
  // and clears the flag in addition to driving `sm_.reset()`; from
  // `kCompleted` there is no such pending work to cancel, hence the
  // sibling acknowledge_completed uses the narrower `sm_.reset()` directly.
  auto reset_result = stop_policy_.request_reset();
  if (!reset_result.ok()) {
    return AcknowledgeInterventionResultDTO{false, reset_result.error()};
  }

  const auto prev_state = status.state;
  const auto prev_execution_id = status.execution_id;
  sm_.set_execution_id(0);
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    active_move_id_.clear();
  }

  ExecutionSnapshot snapshot = execution_backend_.get_execution_snapshot();
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = 0;
  snapshot.move_id.clear();
  snapshot.state = ExecutionState::kIdle;
  snapshot.progress = 0.0;
  snapshot.error.reset();
  publish_execution_snapshot(std::move(snapshot));

  emit_event({KernelEventType::kInterventionAcknowledged,
              prev_execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"principal_id", principal_id},
               {"reason", reason},
               {"prev_state", to_string(prev_state)}}});
  emit_event({KernelEventType::kStateTransition,
              prev_execution_id,
              clock_.now(),
              prev_state,
              ExecutionState::kIdle,
              std::nullopt,
              {}});

  return AcknowledgeInterventionResultDTO{true, std::nullopt};
}

ExecutionStatus ExecutionKernel::get_status() const {
  auto status = sm_.status();
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    status.active_move_id = active_move_id_;
  }
  return status;
}

ExecutionSnapshot ExecutionKernel::get_execution_snapshot() const {
  const auto kernel_state = sm_.status().state;
  if (kernel_state == ExecutionState::kStreaming ||
      kernel_state == ExecutionState::kStopping) {
    auto backend_snapshot = execution_backend_.get_execution_snapshot();
    backend_snapshot.state = kernel_state;
    if (backend_snapshot.timestamp_ns == 0) {
      backend_snapshot.timestamp_ns = clock_.now();
    }
    if (backend_snapshot.timestamp_ns > 0) {
      const auto now_ns = clock_.now();
      if (now_ns >= backend_snapshot.timestamp_ns) {
        const auto lag_ns =
            static_cast<uint64_t>(now_ns - backend_snapshot.timestamp_ns);
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.observer_lag_max_ns =
            std::max(metrics_.observer_lag_max_ns, lag_ns);
      }
    }
    return backend_snapshot;
  }

  ExecutionSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot = published_snapshot_;
  }
  if (snapshot.timestamp_ns > 0) {
    const auto now_ns = clock_.now();
    if (now_ns >= snapshot.timestamp_ns) {
      const auto lag_ns =
          static_cast<uint64_t>(now_ns - snapshot.timestamp_ns);
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.observer_lag_max_ns =
          std::max(metrics_.observer_lag_max_ns, lag_ns);
    }
  }
  return snapshot;
}

void ExecutionKernel::sync_snapshot_from_context() {
  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = sm_.status().execution_id;
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    snapshot.move_id = active_move_id_;
  }
  snapshot.state = sm_.status().state;
  snapshot.progress = sm_.status().progress;
  try {
    snapshot.joint_positions_rad = context_.get_current_state().positions_rad;
  } catch (...) {
    snapshot.joint_positions_rad.clear();
  }
  publish_execution_snapshot(std::move(snapshot));
}

MetricsSnapshot ExecutionKernel::get_metrics() const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  return metrics_;
}

Result<void> ExecutionKernel::collision_check_tick() {
  auto prev_state = sm_.status().state;
  if (prev_state != ExecutionState::kExecuting) {
    return Result<void>::Ok();
  }

  try {
    auto state = context_.get_current_state();
    CollisionCheckRequestDTO req;
    req.robot_state = state;
    auto check = context_.collision_check(req);

    if (check.in_collision) {
      KernelEvent collision_event;
      collision_event.type = KernelEventType::kCollisionDetected;
      collision_event.execution_id = sm_.status().execution_id;
      collision_event.timestamp_ns = clock_.now();
      collision_event.details["surface"] = "observed_runtime_monitor";
      collision_event.details["contact_a"] = check.contact_body_a.value_or("");
      collision_event.details["contact_b"] = check.contact_body_b.value_or("");
      emit_event(collision_event);
      {
        std::lock_guard<std::mutex> collision_metrics_lock(metrics_mutex_);
        metrics_.collision_detections++;
      }
      cache_.invalidate_all();
      auto interrupt = publish_runtime_collision_fault_interrupt(check);
      if (interrupt.rejected) {
        return Result<void>::Err(
            ErrorDTO{kErrorInvalidRequest,
                     "Runtime collision interrupt signal rejected",
                     {{"reason", interrupt.reason}}});
      }
    }
    return Result<void>::Ok();
  } catch (const std::exception& e) {
    return Result<void>::Err(
        ErrorDTO{kErrorInternalError,
                 std::string("Collision check exception: ") + e.what(), {}});
  }
}

Result<TrajectoryDTO> ExecutionKernel::plan_trajectory(
    const ExecuteRequestDTO& request) {
  auto current_state = context_.get_current_state();
  auto resolved_motion = resolve_motion_spec(request);
  if (!resolved_motion.ok()) {
    return Result<TrajectoryDTO>::Err(resolved_motion.error());
  }

  PlanRequestDTO plan_req;
  plan_req.robot_id = request.robot_id;
  plan_req.group_id = request.group_id;
  plan_req.move_id = request.move_id;
  plan_req.base_frame = request.base_frame;
  plan_req.tcp_frame = request.tcp_frame;
  plan_req.start_state = current_state;
  plan_req.motion = std::move(resolved_motion.value());
  plan_req.tolerances = request.tolerances;
  plan_req.speed_profile = request.speed_profile;
  plan_req.planning_timeout_s = request.timeouts.planning_timeout_s;

  auto result = planner_.plan(plan_req);

  if (result.trajectory.has_value()) {
    return Result<TrajectoryDTO>::Ok(std::move(*result.trajectory));
  }

  if (result.error.has_value()) {
    return Result<TrajectoryDTO>::Err(std::move(*result.error));
  }

  return Result<TrajectoryDTO>::Err(ErrorCode::kPlanningFailed);
}

Result<MotionSpecDTO> ExecutionKernel::resolve_motion_spec(
    const ExecuteRequestDTO& request) {
  if (auto* relative_move = std::get_if<RelativeMoveDTO>(&request.motion)) {
    auto target_pose =
        context_.resolve_reference_pose(relative_move->target_reference, request.base_frame);
    if (!target_pose.ok()) {
      return Result<MotionSpecDTO>::Err(target_pose.error());
    }

    Pose3DTO offset_basis_pose = target_pose.value();
    if (relative_move->offset_reference.has_value()) {
      auto offset_reference_pose = context_.resolve_reference_pose(
          *relative_move->offset_reference, request.base_frame);
      if (!offset_reference_pose.ok()) {
        return Result<MotionSpecDTO>::Err(offset_reference_pose.error());
      }
      offset_basis_pose = offset_reference_pose.value();
    }

    PoseGoalDTO pose_goal;
    pose_goal.goal = multiply_poses(
        pose_with_position_and_orientation(
            target_pose.value().position_m,
            offset_basis_pose.orientation),
        relative_move->offset);
    return Result<MotionSpecDTO>::Ok(MotionSpecDTO{pose_goal});
  }

  return Result<MotionSpecDTO>::Ok(request.motion);
}

Result<CollisionWorldRevision> ExecutionKernel::validate_trajectory(
    const TrajectoryDTO& trajectory, const ExecuteRequestDTO& request) {
  // Start-state gate
  auto current_state = context_.get_current_state();
  if (!trajectory.waypoints.empty()) {
    const auto start_match = cached_trajectory_matches_current_start(
        trajectory, current_state, request.tolerances.joint_position_tol_rad);
    if (!start_match.compatible) {
      return Result<CollisionWorldRevision>::Err(
          ErrorDTO{kErrorStartStateMismatch,
                   "Start state mismatch",
                   {{"reason", start_match.reason}}});
    }
  }

  std::optional<CollisionPolicySnapshot> snapshot;
  PoseSourceRetryConfig retry;
  std::int64_t per_state_timeout_ns = 0;
  ICollisionWorldSnapshotPublisher* snapshot_publisher = nullptr;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines;
  {
    std::scoped_lock lock(coordinator_mutex_);
    snapshot = coordinator_policy_snapshot_;
    retry = coordinator_retry_config_;
    per_state_timeout_ns = coordinator_per_state_timeout_ns_;
    snapshot_publisher = collision_snapshot_publisher_;
    validation_engines = collision_validation_engines_;
  }

  // Full non-collision trajectory validation (including TCP speed limit per
  // spec 230 §3.3). Phase 3A makes the manyforge_collision pipeline the only
  // collision authority, so the backend validator must not run its legacy
  // collision sweep first and reject contacts the active policy would allow.
  ValidationOptionsDTO opts;
  opts.tcp_frame = request.tcp_frame;
  opts.base_frame = request.base_frame;
  opts.max_tcp_linear_speed_mps =
      request.speed_profile.max_tcp_linear_speed_mps.value_or(0.0);
  opts.run_collision_checks = false;
  auto val_result = context_.validate_trajectory(trajectory, opts);
  if (!val_result.valid) {
    if (val_result.error.has_value()) {
      return Result<CollisionWorldRevision>::Err(std::move(*val_result.error));
    }
    return Result<CollisionWorldRevision>::Err(ErrorCode::kValidationCollision);
  }

  if (snapshot_publisher == nullptr) {
    return Result<CollisionWorldRevision>::Err(authoritative_collision_validation_config_error(
        "missing_collision_world_snapshot_publisher"));
  }
  if (validation_engines.empty()) {
    return Result<CollisionWorldRevision>::Err(authoritative_collision_validation_config_error(
        "missing_collision_query_engines"));
  }

  CollisionWorldSnapshotPtr world_snapshot;
  try {
    world_snapshot = snapshot_publisher->snapshot();
  } catch (const std::exception& e) {
    return Result<CollisionWorldRevision>::Err(authoritative_collision_validation_config_error(
        "snapshot_publisher_exception",
        {{"exception", e.what()}}));
  } catch (...) {
    return Result<CollisionWorldRevision>::Err(authoritative_collision_validation_config_error(
        "snapshot_publisher_exception",
        {{"exception", "unknown"}}));
  }
  if (!world_snapshot) {
    return Result<CollisionWorldRevision>::Err(authoritative_collision_validation_config_error(
        "snapshot_unavailable"));
  }

  try {
    if (auto publisher_policy =
            snapshot_publisher->collision_policy_snapshot()) {
      snapshot = std::move(publisher_policy);
    }
  } catch (const std::exception& e) {
    return Result<CollisionWorldRevision>::Err(authoritative_collision_validation_config_error(
        "policy_publisher_exception",
        {{"exception", e.what()}}));
  } catch (...) {
    return Result<CollisionWorldRevision>::Err(authoritative_collision_validation_config_error(
        "policy_publisher_exception",
        {{"exception", "unknown"}}));
  }
  if (!snapshot) {
    return Result<CollisionWorldRevision>::Err(authoritative_collision_validation_config_error(
        "missing_collision_policy_snapshot"));
  }

  std::vector<ICollisionQueryEngine*> engine_ptrs;
  engine_ptrs.reserve(validation_engines.size());
  for (const auto& engine : validation_engines) {
    if (engine) {
      engine_ptrs.push_back(engine.get());
    }
  }
  if (engine_ptrs.empty()) {
    return Result<CollisionWorldRevision>::Err(authoritative_collision_validation_config_error(
        "missing_collision_query_engines"));
  }

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = *snapshot;
  cfg.pose_source_retry = retry;
  cfg.per_state_timeout_ns = per_state_timeout_ns;
  cfg.surface = CollisionSurface::kPreExecutionValidation;

  ParallelCollisionValidationWorkerPool::Config pool_cfg;
  pool_cfg.pass_config = std::move(cfg);
  pool_cfg.worker_count = config_.validation_collision_workers;
  pool_cfg.min_waypoints_for_parallel =
      config_.validation_collision_min_waypoints_for_parallel;
  auto cancel = install_validation_cancel_token();
  struct ValidationCancelTokenGuard {
    ExecutionKernel* kernel = nullptr;
    std::shared_ptr<CollisionCancellationToken> token;
    ~ValidationCancelTokenGuard() {
      if (kernel) {
        kernel->clear_validation_cancel_token(token);
      }
    }
  } cancel_guard{this, cancel};
  ParallelCollisionValidationWorkerPool pool(std::move(pool_cfg));
  auto bundle =
      pool.validate_trajectory(*world_snapshot, engine_ptrs, trajectory, *cancel);
  if (bundle.decision.accepted) {
    return Result<CollisionWorldRevision>::Ok(bundle.borrowed_world_revision);
  }
  std::map<std::string, std::string> details;
  if (bundle.decision.first_failing_waypoint) {
    details["first_failing_waypoint"] =
        std::to_string(*bundle.decision.first_failing_waypoint);
  }
  if (bundle.decision.first_failing_time_ns) {
    details["first_failing_time_ns"] =
        std::to_string(*bundle.decision.first_failing_time_ns);
  }
  details["collision_world_revision_id"] =
      std::to_string(bundle.borrowed_world_revision.revision_id);
  details["collision_world_content_hash"] =
      bundle.borrowed_world_revision.content_hash;
  details["workers_used"] =
      std::to_string(pool.last_telemetry().workers_actually_used);
  if (!pool.last_telemetry().fallback_reason.empty()) {
    details["parallel_fallback_reason"] = pool.last_telemetry().fallback_reason;
  }
  if (bundle.scene_changed_during_validation) {
    return Result<CollisionWorldRevision>::Err(
        ErrorDTO{kErrorSceneChangedDuringValidation,
                 "Collision-world revision changed during validation",
                 std::move(details)});
  }
  if (bundle.cancelled) {
    if (bundle.cancelled_at_waypoint_index) {
      details["cancelled_at_waypoint_index"] =
          std::to_string(*bundle.cancelled_at_waypoint_index);
    }
    if (bundle.cancellation_reason) {
      details["cancellation_reason"] = *bundle.cancellation_reason;
    }
    details["reason"] = "collision_validation_cancelled";
    return Result<CollisionWorldRevision>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Collision validation cancelled by interrupt",
                 std::move(details)});
  }
  if (bundle.decision.action == CollisionDecisionAction::kInconclusive) {
    return Result<CollisionWorldRevision>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Collision validation inconclusive",
                 std::move(details)});
  }
  return Result<CollisionWorldRevision>::Err(
      ErrorDTO{kErrorValidationCollision,
               "Collision validation rejected trajectory",
               std::move(details)});
}

Result<CollisionWorldRevision>
ExecutionKernel::validate_streaming_target_collision(
    const StreamingSessionConfigDTO& active_config,
    const StreamingTargetDTO& resolved_target,
    std::uint64_t validation_sequence,
    std::uint64_t session_id,
    std::uint64_t source_sequence) {
  const auto still_latest = [this, validation_sequence]() {
    return validation_sequence == streaming_target_validation_sequence_.load(
                                      std::memory_order_acquire);
  };

  if (!still_latest()) {
    return Result<CollisionWorldRevision>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "streaming target superseded by a newer target",
                 {{"reason_code", "superseded_by_newer_streaming_target"},
                  {"phase", "streaming_target_validation"},
                  {"validation_sequence", std::to_string(validation_sequence)},
                  {"current_validation_sequence",
                   std::to_string(streaming_target_validation_sequence_.load(
                       std::memory_order_acquire))}}});
  }

  if (resolved_target.kind != StreamingInputMode::kJoint ||
      resolved_target.joint_positions_rad.size() !=
          active_config.joint_names.size()) {
    return Result<CollisionWorldRevision>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "streaming target collision validation requires a full "
                 "session-order joint target",
                 {{"reason_code", "malformed_frame"},
                  {"phase", "streaming_target_validation"},
                  {"session_id", std::to_string(session_id)}}});
  }

  std::optional<CollisionPolicySnapshot> policy_snapshot;
  ICollisionWorldSnapshotPublisher* snapshot_publisher = nullptr;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines;
  {
    std::scoped_lock lock(coordinator_mutex_);
    policy_snapshot = coordinator_policy_snapshot_;
    snapshot_publisher = collision_snapshot_publisher_;
    validation_engines = collision_validation_engines_;
  }

  if (snapshot_publisher == nullptr) {
    return Result<CollisionWorldRevision>::Err(
        authoritative_collision_validation_config_error(
            "missing_collision_world_snapshot_publisher",
            {{"reason_code", "collision_pipeline_not_configured"}},
            "streaming_target_validation"));
  }
  if (validation_engines.empty()) {
    return Result<CollisionWorldRevision>::Err(
        authoritative_collision_validation_config_error(
            "missing_collision_query_engines",
            {{"reason_code", "collision_pipeline_not_configured"}},
            "streaming_target_validation"));
  }

  CollisionWorldSnapshotPtr world_snapshot;
  try {
    world_snapshot = snapshot_publisher->snapshot();
  } catch (const std::exception& e) {
    return Result<CollisionWorldRevision>::Err(
        authoritative_collision_validation_config_error(
            "snapshot_publisher_exception",
            {{"reason_code", "collision_pipeline_not_configured"},
             {"exception", e.what()}},
            "streaming_target_validation"));
  } catch (...) {
    return Result<CollisionWorldRevision>::Err(
        authoritative_collision_validation_config_error(
            "snapshot_publisher_exception",
            {{"reason_code", "collision_pipeline_not_configured"},
             {"exception", "unknown"}},
            "streaming_target_validation"));
  }
  if (!world_snapshot) {
    return Result<CollisionWorldRevision>::Err(
        authoritative_collision_validation_config_error(
            "snapshot_unavailable",
            {{"reason_code", "collision_pipeline_not_configured"}},
            "streaming_target_validation"));
  }

  try {
    if (auto publisher_policy =
            snapshot_publisher->collision_policy_snapshot()) {
      policy_snapshot = std::move(publisher_policy);
    }
  } catch (const std::exception& e) {
    return Result<CollisionWorldRevision>::Err(
        authoritative_collision_validation_config_error(
            "policy_publisher_exception",
            {{"reason_code", "collision_pipeline_not_configured"},
             {"exception", e.what()}},
            "streaming_target_validation"));
  } catch (...) {
    return Result<CollisionWorldRevision>::Err(
        authoritative_collision_validation_config_error(
            "policy_publisher_exception",
            {{"reason_code", "collision_pipeline_not_configured"},
             {"exception", "unknown"}},
            "streaming_target_validation"));
  }
  if (!policy_snapshot) {
    return Result<CollisionWorldRevision>::Err(
        authoritative_collision_validation_config_error(
            "missing_collision_policy_snapshot",
            {{"reason_code", "collision_pipeline_not_configured"}},
            "streaming_target_validation"));
  }

  ICollisionQueryEngine* engine = nullptr;
  for (const auto& candidate : validation_engines) {
    if (candidate) {
      engine = candidate.get();
      break;
    }
  }
  if (engine == nullptr) {
    return Result<CollisionWorldRevision>::Err(
        authoritative_collision_validation_config_error(
            "missing_collision_query_engines",
            {{"reason_code", "collision_pipeline_not_configured"}},
            "streaming_target_validation"));
  }

  TrajectoryDTO one_state;
  one_state.joint_names = active_config.joint_names;
  one_state.waypoints.push_back(TrajectoryWaypointDTO{
      resolved_target.min_time_from_start_s,
      resolved_target.joint_positions_rad,
      {},
      {}});

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = *policy_snapshot;
  cfg.require_pose_source_borrow = false;
  cfg.surface = CollisionSurface::kStreamingTargetValidation;
  cfg.per_state_timeout_ns = static_cast<std::int64_t>(
      config_.streaming_collision_timeout_s * 1'000'000'000.0);

  auto cancel = install_streaming_target_cancel_token(validation_sequence);
  struct StreamingCancelTokenGuard {
    ExecutionKernel* kernel = nullptr;
    std::shared_ptr<CollisionCancellationToken> token;
    ~StreamingCancelTokenGuard() {
      if (kernel) {
        kernel->clear_streaming_target_cancel_token(token);
      }
    }
  } cancel_guard{this, cancel};

  CollisionValidationDecisionBundle bundle;
  std::chrono::steady_clock::time_point started;
  {
    std::unique_lock<std::mutex> validation_lock(
        streaming_collision_validation_mutex_);
    if (!still_latest()) {
      cancel->request_cancel("superseded_by_newer_streaming_target");
    }
    if (cancel->is_cancel_requested()) {
      return Result<CollisionWorldRevision>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "streaming target superseded by a newer target",
                   {{"reason_code", "superseded_by_newer_streaming_target"},
                    {"phase", "streaming_target_validation"},
                    {"session_id", std::to_string(session_id)},
                    {"source_sequence", std::to_string(source_sequence)}}});
    }
    CollisionValidationCoordinator coord(std::move(cfg));
    started = std::chrono::steady_clock::now();
    try {
      bundle = coord.validate_trajectory(*world_snapshot, *engine, one_state,
                                         *cancel);
    } catch (const std::exception& e) {
      return Result<CollisionWorldRevision>::Err(
          ErrorDTO{kErrorValidationCollision,
                   "streaming collision validation engine threw",
                   {{"reason_code", "streaming_collision_engine_exception"},
                    {"phase", "streaming_target_validation"},
                    {"exception", e.what()},
                    {"session_id", std::to_string(session_id)},
                    {"source_sequence", std::to_string(source_sequence)},
                    {"collision_world_revision_id",
                     std::to_string(world_snapshot->revision.revision_id)}}});
    } catch (...) {
      return Result<CollisionWorldRevision>::Err(
          ErrorDTO{kErrorValidationCollision,
                   "streaming collision validation engine threw",
                   {{"reason_code", "streaming_collision_engine_exception"},
                    {"phase", "streaming_target_validation"},
                    {"exception", "unknown"},
                    {"session_id", std::to_string(session_id)},
                    {"source_sequence", std::to_string(source_sequence)},
                    {"collision_world_revision_id",
                     std::to_string(world_snapshot->revision.revision_id)}}});
    }
  }
  const auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started)
          .count();

  if (!still_latest()) {
    cancel->request_cancel("superseded_by_newer_streaming_target");
    return Result<CollisionWorldRevision>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "streaming target superseded by a newer target",
                 {{"reason_code", "superseded_by_newer_streaming_target"},
                  {"phase", "streaming_target_validation"},
                  {"session_id", std::to_string(session_id)},
                  {"source_sequence", std::to_string(source_sequence)}}});
  }

  const auto budget_ns = static_cast<std::int64_t>(
      config_.streaming_collision_timeout_s * 1'000'000'000.0);
  if (elapsed_ns > budget_ns) {
    return Result<CollisionWorldRevision>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "streaming collision validation exceeded per-frame budget",
                 {{"reason_code", "streaming_collision_timeout"},
                  {"phase", "streaming_target_validation"},
                  {"timeout_ns", std::to_string(budget_ns)},
                  {"elapsed_ns", std::to_string(elapsed_ns)},
                  {"session_id", std::to_string(session_id)},
                  {"source_sequence", std::to_string(source_sequence)},
                  {"collision_world_revision_id",
                   std::to_string(world_snapshot->revision.revision_id)}}});
  }

  if (bundle.decision.accepted) {
    return Result<CollisionWorldRevision>::Ok(bundle.borrowed_world_revision);
  }

  std::map<std::string, std::string> details{
      {"reason_code", streaming_collision_reason_code(bundle)},
      {"phase", "streaming_target_validation"},
      {"session_id", std::to_string(session_id)},
      {"source_sequence", std::to_string(source_sequence)},
      {"collision_world_revision_id",
       std::to_string(bundle.borrowed_world_revision.revision_id)},
      {"collision_world_content_hash",
       bundle.borrowed_world_revision.content_hash}};
  if (bundle.decision.first_failing_waypoint) {
    details["first_failing_waypoint"] =
        std::to_string(*bundle.decision.first_failing_waypoint);
  }
  if (bundle.cancelled_at_waypoint_index) {
    details["cancelled_at_waypoint_index"] =
        std::to_string(*bundle.cancelled_at_waypoint_index);
  }
  if (bundle.cancellation_reason) {
    details["cancellation_reason"] = *bundle.cancellation_reason;
  }
  return Result<CollisionWorldRevision>::Err(
      ErrorDTO{kErrorValidationCollision,
               bundle.cancelled
                   ? "streaming collision validation cancelled"
                   : "streaming target rejected by collision validation",
               std::move(details)});
}

void ExecutionKernel::set_collision_validation_policy_snapshot(
    std::optional<CollisionPolicySnapshot> snapshot) {
  std::scoped_lock lock(coordinator_mutex_);
  coordinator_policy_snapshot_ = std::move(snapshot);
}

void ExecutionKernel::set_collision_validation_pipeline(
    ICollisionWorldSnapshotPublisher& publisher,
    std::vector<std::shared_ptr<ICollisionQueryEngine>> engines) {
  std::scoped_lock lock(coordinator_mutex_);
  collision_snapshot_publisher_ = &publisher;
  collision_validation_engines_ = std::move(engines);
}

void ExecutionKernel::set_collision_validation_retry_config(
    PoseSourceRetryConfig retry, std::int64_t per_state_timeout_ns) {
  std::scoped_lock lock(coordinator_mutex_);
  coordinator_retry_config_ = retry;
  coordinator_per_state_timeout_ns_ = per_state_timeout_ns;
}

CollisionValidationDecisionBundle
ExecutionKernel::run_predicted_path_monitor_diagnostic(
    const std::optional<RobotStateDTO>& predicted_state) {
  std::optional<CollisionPolicySnapshot> snapshot;
  PoseSourceRetryConfig retry;
  std::int64_t per_state_timeout_ns = 0;
  {
    std::scoped_lock lock(coordinator_mutex_);
    snapshot = coordinator_policy_snapshot_;
    retry = coordinator_retry_config_;
    per_state_timeout_ns = coordinator_per_state_timeout_ns_;
  }
  CollisionValidationDecisionBundle bundle;
  if (!snapshot) {
    bundle.decision.accepted = true;
    bundle.decision.action = CollisionDecisionAction::kDispatch;
    return bundle;
  }
  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = *snapshot;
  cfg.pose_source_retry = retry;
  cfg.per_state_timeout_ns = per_state_timeout_ns;
  cfg.require_pose_source_borrow = false;
  cfg.surface = CollisionSurface::kPredictedRuntimeMonitor;
  CollisionValidationCoordinator coord(std::move(cfg));

  try {
    RobotStateDTO state = predicted_state.value_or(context_.get_current_state());
    bundle = coord.validate_state(context_, state);
  } catch (const std::exception& e) {
    bundle.decision.accepted = false;
    bundle.decision.severity = CollisionDecisionSeverity::kBlock;
    bundle.decision.action = CollisionDecisionAction::kInconclusive;
    KernelEvent ev;
    ev.type = KernelEventType::kCollisionDetected;
    ev.execution_id = sm_.status().execution_id;
    ev.timestamp_ns = clock_.now();
    ev.details["surface"] = "runtime_predicted_diagnostic";
    ev.details["diagnostic_error"] = e.what();
    emit_event(ev);
    return bundle;
  } catch (...) {
    bundle.decision.accepted = false;
    bundle.decision.severity = CollisionDecisionSeverity::kBlock;
    bundle.decision.action = CollisionDecisionAction::kInconclusive;
    KernelEvent ev;
    ev.type = KernelEventType::kCollisionDetected;
    ev.execution_id = sm_.status().execution_id;
    ev.timestamp_ns = clock_.now();
    ev.details["surface"] = "runtime_predicted_diagnostic";
    ev.details["diagnostic_error"] = "unknown";
    emit_event(ev);
    return bundle;
  }

  if (!bundle.decision.accepted) {
    KernelEvent ev;
    ev.type = KernelEventType::kCollisionDetected;
    ev.execution_id = sm_.status().execution_id;
    ev.timestamp_ns = clock_.now();
    ev.details["surface"] = "runtime_predicted_diagnostic";
    if (bundle.decision.first_failing_waypoint) {
      ev.details["first_failing_waypoint"] =
          std::to_string(*bundle.decision.first_failing_waypoint);
    }
    if (bundle.borrowed_world_revision.revision_id != 0) {
      ev.details["collision_world_revision_id"] =
          std::to_string(bundle.borrowed_world_revision.revision_id);
    }
    emit_event(ev);
  }
  return bundle;
}

// -----------------------------------------------------------------
// Streaming-session API (ADR-0016).
//
// Lifecycle invariants enforced here:
//   - begin requires state == kIdle; transitions to kStreaming on accept.
//   - push requires state == kStreaming and session_id match.
//   - end transitions kStreaming -> kStopping -> kStopped (after the
//     backend soft-stop completes).
//
// Concurrency: streaming_session_mutex_ protects the active session
// metadata. We do NOT hold it across the backend call (the backend has
// its own internal locks; we mustn't invert lock order with
// lifecycle_mutex_).
// -----------------------------------------------------------------

StreamingSessionBeginResultDTO ExecutionKernel::begin_streaming_session(
    const StreamingSessionConfigDTO& config) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  StreamingSessionBeginResultDTO result;

  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (active_streaming_session_id_ != 0) {
      result.accepted = false;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidStateTransition)),
          "streaming session already active",
          {{"active_session_id",
            std::to_string(active_streaming_session_id_)}}};
      return result;
    }
  }

  const auto prev_state = sm_.status().state;
  if (prev_state != ExecutionState::kIdle) {
    result.accepted = false;
    result.error = ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidStateTransition)),
        "begin_streaming_session valid only from Idle",
        {{"state", to_string(prev_state)}}};
    return result;
  }

  // Defense-in-depth: if the caller did not specify the session's
  // joint list, fall back to the backend's current joint snapshot.
  // The frontend SHOULD pass `joint_names` explicitly (the canonical
  // path), but if it ever forgets, the push-validation chain would
  // reject every partial frame as `unknown_joint_name`. Populating
  // from the live snapshot here keeps the session functional with
  // the natural ordering the backend already uses.
  StreamingSessionConfigDTO populated_config = config;
  if (populated_config.joint_names.empty()) {
    auto current_state = context_.get_current_state();
    if (!current_state.joint_names.empty()) {
      populated_config.joint_names = current_state.joint_names;
    }
  }
  if (!populated_config.joint_names.empty()) {
    const double unbounded_lower = -std::numeric_limits<double>::infinity();
    const double unbounded_upper = std::numeric_limits<double>::infinity();
    populated_config.per_joint_position_lower.assign(
        populated_config.joint_names.size(), unbounded_lower);
    populated_config.per_joint_position_upper.assign(
        populated_config.joint_names.size(), unbounded_upper);

    try {
      auto limits = context_.get_joint_limits("", populated_config.group_id);
      const bool have_position_limits =
          limits.joint_names.size() == limits.position_lower_rad.size() &&
          limits.joint_names.size() == limits.position_upper_rad.size();
      if (have_position_limits) {
        for (size_t si = 0; si < populated_config.joint_names.size(); ++si) {
          const auto& joint_name = populated_config.joint_names[si];
          auto lit = std::find(limits.joint_names.begin(),
                               limits.joint_names.end(), joint_name);
          if (lit == limits.joint_names.end()) {
            continue;
          }
          const auto li = static_cast<size_t>(
              std::distance(limits.joint_names.begin(), lit));
          populated_config.per_joint_position_lower[si] =
              limits.position_lower_rad[li];
          populated_config.per_joint_position_upper[si] =
              limits.position_upper_rad[li];
        }
      }
    } catch (...) {
      // Some lightweight test contexts/backends cannot expose limits. Keep
      // entries unbounded rather than rejecting begin; push-time enforcement
      // still applies for every finite limit the context can provide.
    }
  }

  auto backend_result =
      execution_backend_.begin_streaming_session(populated_config);
  if (!backend_result.accepted) {
    return backend_result;
  }

  auto sm_result = sm_.try_transition(ExecutionState::kStreaming);
  if (!sm_result.ok()) {
    // Roll back the backend session — kernel state machine refused the
    // transition. End the session with spring-back as a defensive default.
    (void)execution_backend_.end_streaming_session(
        backend_result.session_id, SoftStopStrategy::kSpringBack);
    result.accepted = false;
    result.error = sm_result.error();
    return result;
  }

  uint64_t kernel_session_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    kernel_session_id = next_streaming_session_id_++;
    active_streaming_session_id_ = kernel_session_id;
    active_backend_session_id_ = backend_result.session_id;
    ++active_streaming_generation_;
    active_streaming_config_ = populated_config;
    active_soft_stop_strategy_ = populated_config.default_soft_stop_strategy;
    last_accepted_cartesian_pos_valid_ = false;
    last_accepted_cartesian_ts_ns_ = 0;
    last_commanded_joints_valid_ = false;
    last_commanded_joints_.clear();
  }

  emit_event({KernelEventType::kStreamingSessionStarted,
              0,  // no execution_id for streaming
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"session_id", std::to_string(kernel_session_id)},
               {"backend_session_id",
                std::to_string(backend_result.session_id)},
               {"group_id", config.group_id},
               {"input_mode", to_string(config.default_input_mode)},
               {"principal_id", config.principal_id}}});
  emit_event({KernelEventType::kStateTransition,
              0,
              clock_.now(),
              prev_state,
              ExecutionState::kStreaming,
              std::nullopt,
              {{"session_id", std::to_string(kernel_session_id)}}});

  result.accepted = true;
  result.session_id = kernel_session_id;
  return result;
}

void ExecutionKernel::set_runtime_velocity_scale(double scale) {
  // ADR-0018: HMI runtime velocity scale → backend runtime register.
  // Validate finiteness here; clamp to [0, 1]; forward to the backend's
  // atomic store. No lifecycle_mutex_: this is a high-rate operator surface,
  // peer to push_streaming_target. The backend's atomic handles concurrency.
  if (!std::isfinite(scale)) {
    // K2-style defense in depth: refuse NaN/Inf rather than
    // propagating into the worker.
    return;
  }
  if (scale < 0.0) scale = 0.0;
  if (scale > 1.0) scale = 1.0;
  runtime_velocity_scale_.store(scale, std::memory_order_release);
  execution_backend_.set_runtime_velocity_scale(scale);
}

void ExecutionKernel::set_streaming_velocity_scale(double scale) {
  set_runtime_velocity_scale(scale);
}

StreamingPushResultDTO ExecutionKernel::push_streaming_target(
    uint64_t session_id,
    const StreamingTargetDTO& target) {
  StreamingPushResultDTO result;

  if (sm_.status().state != ExecutionState::kStreaming) {
    result.accepted = false;
    result.error = ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidStateTransition)),
        "push_streaming_target valid only from Streaming",
        {{"state", to_string(sm_.status().state)}}};
    return result;
  }

  StreamingSessionConfigDTO active_config;
  uint64_t backend_session_id = 0;
  uint64_t session_generation = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (active_streaming_session_id_ == 0 ||
        active_streaming_session_id_ != session_id) {
      result.accepted = false;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "session_id does not match active streaming session",
          {{"requested_session_id", std::to_string(session_id)},
           {"active_session_id",
            std::to_string(active_streaming_session_id_)}}};
      return result;
    }
    active_config = active_streaming_config_;
    backend_session_id = active_backend_session_id_;
    session_generation = active_streaming_generation_;
  }
  const auto validation_sequence =
      streaming_target_validation_sequence_.fetch_add(
          1, std::memory_order_acq_rel) +
      1;
  const auto streaming_target_is_latest = [this, validation_sequence]() {
    return validation_sequence == streaming_target_validation_sequence_.load(
                                      std::memory_order_acquire);
  };

  auto reject_malformed_frame =
      [&](const std::string& message,
          std::map<std::string, std::string> details = {}) {
        details.emplace("reason_code", "malformed_frame");
        result.accepted = false;
        result.backend_sequence = 0;
        result.error = ErrorDTO{
            std::string(to_string(ErrorCode::kInvalidRequest)),
            message,
            details};
        emit_event({KernelEventType::kStreamingTargetRejected,
                    0,
                    clock_.now(),
                    {},
                    {},
                    result.error,
                    {{"session_id", std::to_string(session_id)},
                     {"source_sequence",
                      std::to_string(target.source_sequence)},
                     {"kind", "malformed_frame"}}});
        return result;
      };

  auto reject_streaming_collision =
      [&](ErrorDTO error,
          std::map<std::string, std::string> event_details = {}) {
        event_details.emplace("session_id", std::to_string(session_id));
        event_details.emplace("source_sequence",
                              std::to_string(target.source_sequence));
        event_details.emplace("kind", "streaming_collision_validation");
        if (auto it = error.details.find("reason_code");
            it != error.details.end()) {
          event_details.emplace("reason_code", it->second);
        }
        result.accepted = false;
        result.backend_sequence = 0;
        result.error = std::move(error);
        emit_event({KernelEventType::kStreamingTargetRejected,
                    0,
                    clock_.now(),
                    {},
                    {},
                    result.error,
                    std::move(event_details)});
        return result;
      };

  if (!std::isfinite(target.min_time_from_start_s)
      || target.min_time_from_start_s < 0.0) {
    return reject_malformed_frame(
        "streaming target min_time_from_start_s must be finite and >= 0",
        {{"field", "min_time_from_start_s"}});
  }

  StreamingTargetDTO resolved_target = target;
  if (target.kind == StreamingInputMode::kCartesian) {
    for (size_t i = 0; i < target.position_m.size(); ++i) {
      if (!std::isfinite(target.position_m[i])) {
        return reject_malformed_frame(
            "Cartesian streaming target contains non-finite position",
            {{"field", "position_m"}, {"index", std::to_string(i)}});
      }
    }
    double quat_norm_sq = 0.0;
    for (size_t i = 0; i < target.orientation_xyzw.size(); ++i) {
      const double value = target.orientation_xyzw[i];
      if (!std::isfinite(value)) {
        return reject_malformed_frame(
            "Cartesian streaming target contains non-finite orientation",
            {{"field", "orientation_xyzw"}, {"index", std::to_string(i)}});
      }
      quat_norm_sq += value * value;
    }
    if (quat_norm_sq < 1e-12) {
      return reject_malformed_frame(
          "Cartesian streaming target quaternion norm is too small",
          {{"field", "orientation_xyzw"}});
    }
    const double quat_norm = std::sqrt(quat_norm_sq);
    for (double& value : resolved_target.orientation_xyzw) {
      value /= quat_norm;
    }
  } else if (target.kind == StreamingInputMode::kJoint) {
    for (size_t i = 0; i < target.joint_positions_rad.size(); ++i) {
      if (!std::isfinite(target.joint_positions_rad[i])) {
        return reject_malformed_frame(
            "Joint streaming target contains non-finite position",
            {{"field", "joint_positions_rad"}, {"index", std::to_string(i)}});
      }
    }
  }

  // ADR-0015 / ADR-0016: enforce the deployment's Cartesian speed cap
  // on the ROBOT's commanded motion, NOT on the operator's input rate.
  // The operator can drag the gizmo at any speed; the robot tracks at
  // ≤ the cap and the gizmo simply gets ahead during fast drags (the
  // visual non-coincidence is documented UX — same as the legacy
  // direct-jog path). We translate the cap into a minimum
  // `time_from_start` for the backend's anchored FJT goal:
  //   dt_min_cart = distance(prev_target, this_target) / max_speed_mps
  // and the backend's dispatch worker max'es this with its own joint-
  // velocity-cap-derived floor. The result is that the FJT goal is
  // always scheduled to take long enough for the cap to hold.
  if (target.kind == StreamingInputMode::kCartesian &&
      active_config.max_tcp_linear_speed_mps > 0.0) {
    std::array<double, 3> last_pos{0.0, 0.0, 0.0};
    bool have_last = false;
    {
      std::lock_guard<std::mutex> lock(streaming_session_mutex_);
      last_pos = last_accepted_cartesian_pos_;
      have_last = last_accepted_cartesian_pos_valid_;
    }
    if (have_last) {
      const double dx = target.position_m[0] - last_pos[0];
      const double dy = target.position_m[1] - last_pos[1];
      const double dz = target.position_m[2] - last_pos[2];
      const double distance_m = std::sqrt(dx * dx + dy * dy + dz * dz);
      const double dt_min_cart_s =
          distance_m / active_config.max_tcp_linear_speed_mps;
      if (dt_min_cart_s > resolved_target.min_time_from_start_s) {
        resolved_target.min_time_from_start_s = dt_min_cart_s;
      }
    }
  }

  // ADR-0016 IK pre-resolution: Cartesian targets are resolved to joint
  // targets here via the planner's single-shot IK, then forwarded to the
  // backend as joint targets. This keeps execution backends free of IK
  // logic (per ADR-0007 scene-ownership boundary) and lets every backend
  // implement streaming uniformly against joint targets.
  if (target.kind == StreamingInputMode::kCartesian) {
    auto current_state = context_.get_current_state();
    IkRequestDTO ik_req;
    Pose3DTO pose;
    pose.position_m.x_m = target.position_m[0];
    pose.position_m.y_m = target.position_m[1];
    pose.position_m.z_m = target.position_m[2];
    pose.orientation.x = resolved_target.orientation_xyzw[0];
    pose.orientation.y = resolved_target.orientation_xyzw[1];
    pose.orientation.z = resolved_target.orientation_xyzw[2];
    pose.orientation.w = resolved_target.orientation_xyzw[3];
    ik_req.goal_pose = pose;
    ik_req.base_frame = active_config.cartesian_reference_frame.empty()
                             ? std::string("base")
                             : active_config.cartesian_reference_frame;
    ik_req.tcp_frame = active_config.tcp_frame;
    if (ik_req.tcp_frame.empty()) {
      // No TCP frame on the session — Cartesian streaming is not addressable.
      result.accepted = false;
      result.backend_sequence = 0;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "Cartesian streaming target on a session that did not configure "
          "tcp_frame — set StreamingSessionConfigDTO.tcp_frame at begin time",
          {}};
      emit_event({KernelEventType::kStreamingTargetRejected,
                  0,
                  clock_.now(),
                  {},
                  {},
                  result.error,
                  {{"session_id", std::to_string(session_id)},
                   {"source_sequence",
                    std::to_string(target.source_sequence)},
                   {"kind", "missing_tcp_frame"}}});
      return result;
    }
    ik_req.seed_state = current_state;
    ik_req.timeout_s = config_.streaming_ik_timeout_s;

    const auto ik_started = std::chrono::steady_clock::now();
    auto ik_result = planner_.solve_ik(ik_req);
    const double ik_elapsed_s =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - ik_started)
            .count();
    if (ik_elapsed_s > config_.streaming_ik_timeout_s) {
      result.accepted = false;
      result.backend_sequence = 0;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kIkFailed)),
          "streaming IK exceeded configured timeout",
          {{"reason_code", "ik_timeout"},
           {"timeout_s", std::to_string(config_.streaming_ik_timeout_s)},
           {"elapsed_s", std::to_string(ik_elapsed_s)},
           {"base_frame", ik_req.base_frame},
           {"tcp_frame", ik_req.tcp_frame}}};
      emit_event({KernelEventType::kStreamingTargetRejected,
                  0,
                  clock_.now(),
                  {},
                  {},
                  result.error,
                  {{"session_id", std::to_string(session_id)},
                   {"source_sequence",
                    std::to_string(target.source_sequence)},
                   {"kind", "cartesian_ik_timeout"},
                   {"elapsed_s", std::to_string(ik_elapsed_s)}}});
      return result;
    }
    if (!ik_result.ok) {
      result.accepted = false;
      result.backend_sequence = 0;
      result.error = ik_result.error.value_or(ErrorDTO{
          std::string(to_string(ErrorCode::kIkFailed)),
          "IK failed for streaming cartesian target",
          {}});
      emit_event({KernelEventType::kStreamingTargetRejected,
                  0,
                  clock_.now(),
                  {},
                  {},
                  result.error,
                  {{"session_id", std::to_string(session_id)},
                   {"source_sequence",
                    std::to_string(target.source_sequence)},
                   {"kind", "cartesian_ik_failed"}}});
      return result;
    }

    // Reorder IK output into session joint order. IK returns whatever
    // order the planner uses; the rest of the kernel + backends rely
    // on session-order vectors (so cartesian-then-joint sequences
    // overlay correctly onto last_commanded_joints_).
    std::vector<double> ordered_positions(active_config.joint_names.size());
    for (size_t i = 0; i < active_config.joint_names.size(); ++i) {
      const auto& sname = active_config.joint_names[i];
      auto it = std::find(ik_result.joint_names.begin(),
                          ik_result.joint_names.end(), sname);
      if (it == ik_result.joint_names.end()) {
        result.accepted = false;
        result.backend_sequence = 0;
        result.error = ErrorDTO{
            std::string(to_string(ErrorCode::kIkFailed)),
            "IK result missing session joint: " + sname,
            {{"reason_code", "ik_failed"}, {"joint_name", sname}}};
        emit_event({KernelEventType::kStreamingTargetRejected,
                    0,
                    clock_.now(),
                    {},
                    {},
                    result.error,
                    {{"session_id", std::to_string(session_id)},
                     {"source_sequence",
                      std::to_string(target.source_sequence)},
                     {"kind", "cartesian_ik_failed"}}});
        return result;
      }
      auto idx = static_cast<size_t>(
          std::distance(ik_result.joint_names.begin(), it));
      ordered_positions[i] = ik_result.positions_rad[idx];
    }
    resolved_target.kind = StreamingInputMode::kJoint;
    resolved_target.joint_names = active_config.joint_names;
    resolved_target.joint_positions_rad = ordered_positions;
  } else if (target.kind == StreamingInputMode::kJoint) {
    // Joint-mode normalization per spec/200 §2. Partial joint maps are
    // valid input; the kernel overlays them onto the session's last
    // commanded full vector before backend dispatch. Validation chain
    // runs BEFORE any state mutation so rejected frames cannot move
    // last_commanded_joints_.
    if (target.joint_names.size() != target.joint_positions_rad.size()) {
      result.accepted = false;
      result.backend_sequence = 0;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "joint_names.size() != joint_positions_rad.size()",
          {{"reason_code", "malformed_frame"},
           {"names_size", std::to_string(target.joint_names.size())},
           {"positions_size",
            std::to_string(target.joint_positions_rad.size())}}};
      emit_event({KernelEventType::kStreamingTargetRejected,
                  0,
                  clock_.now(),
                  {},
                  {},
                  result.error,
                  {{"session_id", std::to_string(session_id)},
                   {"source_sequence",
                    std::to_string(target.source_sequence)},
                   {"kind", "malformed_frame"}}});
      return result;
    }

    // Duplicate-name check. Allowed-name check fused in (any partial
    // entry whose name isn't in the session's joint_names is also
    // implicitly a duplicate-or-unknown). We separate them so the
    // operator sees the correct reason code.
    std::map<std::string, size_t> seen;
    for (size_t pi = 0; pi < target.joint_names.size(); ++pi) {
      const auto& name = target.joint_names[pi];
      auto inserted = seen.emplace(name, pi);
      if (!inserted.second) {
        result.accepted = false;
        result.backend_sequence = 0;
        result.error = ErrorDTO{
            std::string(to_string(ErrorCode::kInvalidRequest)),
            "duplicate joint name in streaming target: " + name,
            {{"reason_code", "malformed_frame"}, {"joint_name", name}}};
        emit_event({KernelEventType::kStreamingTargetRejected,
                    0,
                    clock_.now(),
                    {},
                    {},
                    result.error,
                    {{"session_id", std::to_string(session_id)},
                     {"source_sequence",
                      std::to_string(target.source_sequence)},
                     {"kind", "malformed_frame"}}});
        return result;
      }
    }

    // Unknown-name check.
    for (const auto& name : target.joint_names) {
      if (std::find(active_config.joint_names.begin(),
                    active_config.joint_names.end(), name)
          == active_config.joint_names.end()) {
        result.accepted = false;
        result.backend_sequence = 0;
        result.error = ErrorDTO{
            std::string(to_string(ErrorCode::kUnknownJointName)),
            "joint name not in session: " + name,
            {{"joint_name", name}}};
        emit_event({KernelEventType::kStreamingTargetRejected,
                    0,
                    clock_.now(),
                    {},
                    {},
                    result.error,
                    {{"session_id", std::to_string(session_id)},
                     {"source_sequence",
                      std::to_string(target.source_sequence)},
                     {"kind", "unknown_joint_name"}}});
        return result;
      }
    }

    // Build the full session-order vector. Reuse last_commanded_joints_
    // when valid; otherwise seed from the backend's current snapshot.
    // A snapshot that doesn't cover every session joint is treated as
    // stale and rejects the push without seeding.
    std::vector<double> commanded_full;
    bool need_seed = false;
    {
      std::lock_guard<std::mutex> lock(streaming_session_mutex_);
      if (last_commanded_joints_valid_
          && last_commanded_joints_.size() == active_config.joint_names.size()) {
        commanded_full = last_commanded_joints_;
      } else {
        need_seed = true;
      }
    }
    if (need_seed) {
      auto current_state = context_.get_current_state();
      commanded_full.assign(active_config.joint_names.size(), 0.0);
      for (size_t i = 0; i < active_config.joint_names.size(); ++i) {
        const auto& sname = active_config.joint_names[i];
        auto cit = std::find(current_state.joint_names.begin(),
                             current_state.joint_names.end(), sname);
        if (cit == current_state.joint_names.end()) {
          result.accepted = false;
          result.backend_sequence = 0;
          result.error = ErrorDTO{
              std::string(to_string(ErrorCode::kInvalidRequest)),
              "current robot state does not cover session joint: " + sname,
              {{"reason_code", "stale_state"}, {"joint_name", sname}}};
          emit_event({KernelEventType::kStreamingTargetRejected,
                      0,
                      clock_.now(),
                      {},
                      {},
                      result.error,
                      {{"session_id", std::to_string(session_id)},
                       {"source_sequence",
                        std::to_string(target.source_sequence)},
                       {"kind", "stale_state"}}});
          return result;
        }
        auto cidx = static_cast<size_t>(
            std::distance(current_state.joint_names.begin(), cit));
        commanded_full[i] = current_state.positions_rad[cidx];
      }
    }

    // Overlay partial onto full. Names already validated as present in
    // active_config.joint_names; the lookup is guaranteed to succeed.
    for (size_t pi = 0; pi < target.joint_names.size(); ++pi) {
      auto sit = std::find(active_config.joint_names.begin(),
                           active_config.joint_names.end(),
                           target.joint_names[pi]);
      auto sidx = static_cast<size_t>(
          std::distance(active_config.joint_names.begin(), sit));
      commanded_full[sidx] = target.joint_positions_rad[pi];
    }

    resolved_target.kind = StreamingInputMode::kJoint;
    resolved_target.joint_names = active_config.joint_names;
    resolved_target.joint_positions_rad = commanded_full;
  }

  if (resolved_target.kind == StreamingInputMode::kJoint &&
      resolved_target.joint_positions_rad.size()
          == active_config.joint_names.size() &&
      active_config.per_joint_position_lower.size()
          == active_config.joint_names.size() &&
      active_config.per_joint_position_upper.size()
          == active_config.joint_names.size()) {
    constexpr double kLimitTolerance = 1e-9;
    for (size_t i = 0; i < resolved_target.joint_positions_rad.size(); ++i) {
      const double value = resolved_target.joint_positions_rad[i];
      const double lower = active_config.per_joint_position_lower[i];
      const double upper = active_config.per_joint_position_upper[i];
      if ((std::isfinite(lower) && value < lower - kLimitTolerance) ||
          (std::isfinite(upper) && value > upper + kLimitTolerance)) {
        result.accepted = false;
        result.backend_sequence = 0;
        result.error = ErrorDTO{
            std::string(to_string(ErrorCode::kValidationLimitsViolation)),
            "streaming target joint position exceeds configured limits",
            {{"reason_code", "joint_position_limit"},
             {"joint_name", active_config.joint_names[i]},
             {"position_rad", std::to_string(value)},
             {"limit_lower", std::to_string(lower)},
             {"limit_upper", std::to_string(upper)}}};
        emit_event({KernelEventType::kStreamingTargetRejected,
                    0,
                    clock_.now(),
                    {},
                    {},
                    result.error,
                    {{"session_id", std::to_string(session_id)},
                     {"source_sequence",
                      std::to_string(target.source_sequence)},
                     {"kind", "joint_position_limit"}}});
        return result;
      }
    }
  }

  if (!streaming_target_is_latest()) {
    return reject_streaming_collision(
        ErrorDTO{kErrorInvalidRequest,
                 "streaming target superseded by a newer target",
                 {{"reason_code", "superseded_by_newer_streaming_target"},
                  {"phase", "streaming_target_validation"}}});
  }

  auto collision_validation = validate_streaming_target_collision(
      active_config, resolved_target, validation_sequence, session_id,
      target.source_sequence);
  if (!collision_validation.ok()) {
    return reject_streaming_collision(collision_validation.error());
  }

  // Forward to backend using the BACKEND's session_id (assigned at begin
  // time), not the kernel's. Kernel and backend counters are independent;
  // mapping must use the backend's view.
  auto backend_result =
      execution_backend_.push_streaming_target(backend_session_id,
                                                resolved_target);
  result = backend_result;

  if (result.accepted) {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (active_streaming_session_id_ != session_id ||
        active_backend_session_id_ != backend_session_id ||
        active_streaming_generation_ != session_generation) {
      result.accepted = false;
      result.backend_sequence = 0;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "streaming session changed while target was being processed",
          {{"reason_code", "session_lost"},
           {"requested_session_id", std::to_string(session_id)}}};
      emit_event({KernelEventType::kStreamingTargetRejected,
                  0,
                  clock_.now(),
                  {},
                  {},
                  result.error,
                  {{"session_id", std::to_string(session_id)},
                   {"source_sequence",
                    std::to_string(target.source_sequence)},
                   {"kind", "session_lost"}}});
      return result;
    }
  }

  // On accept: record the cartesian position as the new
  // last-accepted-frame anchor for speed-limit enforcement on the next
  // push. Track the kernel clock at acceptance time, not the
  // source_timestamp_ns — the speed limit is on what the kernel
  // actually commanded, not what the upstream client claimed.
  if (result.accepted && target.kind == StreamingInputMode::kCartesian) {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    last_accepted_cartesian_pos_ = target.position_m;
    last_accepted_cartesian_pos_valid_ = true;
    last_accepted_cartesian_ts_ns_ = clock_.now();
  }

  // Update last_commanded_joints_ on backend ACCEPT (target register
  // updated) for both joint and IK-resolved-cartesian targets, so a
  // cartesian drag followed by a partial joint slider overlays onto
  // the post-cartesian joint configuration rather than a stale
  // pre-cartesian one. Spec/200 §2: last-commanded state moves on
  // accept, not on theoretical controller dispatch.
  if (result.accepted
      && resolved_target.kind == StreamingInputMode::kJoint
      && !resolved_target.joint_positions_rad.empty()
      && resolved_target.joint_positions_rad.size()
             == active_config.joint_names.size()) {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    last_commanded_joints_ = resolved_target.joint_positions_rad;
    last_commanded_joints_valid_ = true;
  }

  // Per-target events are coalescible / sampled; spec 200 §4 explicitly
  // allows them to not be 1:1 with calls. Emit on accept and on reject so
  // the failure archive captures rejections.
  if (result.accepted) {
    emit_event({KernelEventType::kStreamingTargetReceived,
                0,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"session_id", std::to_string(session_id)},
                 {"backend_sequence",
                  std::to_string(result.backend_sequence)},
                 {"source_sequence",
                  std::to_string(target.source_sequence)},
                 {"kind", to_string(target.kind)}}});
    // For the v0.1 backends the push call IS the dispatch (the backend's
    // dispatcher worker reads the register at the next tick). Emit
    // kStreamingTargetDispatched so the failure archive / event stream
    // can correlate operator frame → controller goal submission. Future
    // backends with deeper queueing should emit this separately when the
    // goal actually reaches the controller.
    emit_event({KernelEventType::kStreamingTargetDispatched,
                0,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"session_id", std::to_string(session_id)},
                 {"backend_sequence",
                  std::to_string(result.backend_sequence)},
                 {"source_sequence",
                  std::to_string(target.source_sequence)},
                 {"resolved_kind",
                  to_string(resolved_target.kind)}}});
  } else {
    emit_event({KernelEventType::kStreamingTargetRejected,
                0,
                clock_.now(),
                {},
                {},
                result.error,
                {{"session_id", std::to_string(session_id)},
                 {"source_sequence",
                  std::to_string(target.source_sequence)},
                 {"kind", to_string(target.kind)}}});
  }
  return result;
}

StreamingSessionEndResultDTO ExecutionKernel::end_streaming_session(
    uint64_t session_id,
    SoftStopStrategy strategy) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  return end_streaming_session_unlocked(session_id, strategy);
}

StreamingSessionEndResultDTO ExecutionKernel::end_streaming_session_unlocked(
    uint64_t session_id,
    SoftStopStrategy strategy) {
  StreamingSessionEndResultDTO result;

  const auto prev_state = sm_.status().state;
  if (prev_state != ExecutionState::kStreaming) {
    result.accepted = false;
    result.error = ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidStateTransition)),
        "end_streaming_session valid only from Streaming",
        {{"state", to_string(prev_state)}}};
    return result;
  }

  uint64_t backend_session_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (active_streaming_session_id_ == 0 ||
        active_streaming_session_id_ != session_id) {
      result.accepted = false;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "session_id does not match active streaming session",
          {{"requested_session_id", std::to_string(session_id)},
           {"active_session_id",
            std::to_string(active_streaming_session_id_)}}};
      return result;
    }
    backend_session_id = active_backend_session_id_;
  }
  cancel_active_streaming_target_validation("streaming_session_ending");

  // Move through Stopping during the backend soft-stop. The backend's
  // soft-stop trajectory still has to play out; we report Stopping until
  // the backend acks, then transition to Stopped.
  auto stopping_result = sm_.try_transition(ExecutionState::kStopping);
  if (!stopping_result.ok()) {
    result.accepted = false;
    result.error = stopping_result.error();
    return result;
  }
  emit_event({KernelEventType::kStateTransition,
              0,
              clock_.now(),
              prev_state,
              ExecutionState::kStopping,
              std::nullopt,
              {{"session_id", std::to_string(session_id)},
               {"soft_stop_strategy", to_string(strategy)}}});

  auto backend_result =
      execution_backend_.end_streaming_session(backend_session_id, strategy);
  if (!backend_result.accepted) {
    // The soft-stop itself failed; route to InterventionRequired since
    // the backend may still be commanding motion in an unknown state.
    (void)sm_.try_transition(ExecutionState::kInterventionRequired);
    emit_event({KernelEventType::kErrorRaised,
                0,
                clock_.now(),
                {},
                {},
                backend_result.error,
                {{"session_id", std::to_string(session_id)}}});
    // Still clear local bookkeeping — the kernel can't recover this
    // session anyway, and a future begin must be permitted once the
    // operator hits reset.
    {
      std::lock_guard<std::mutex> lock(streaming_session_mutex_);
      active_streaming_session_id_ = 0;
      active_backend_session_id_ = 0;
      ++active_streaming_generation_;
      active_streaming_config_ = StreamingSessionConfigDTO{};
      active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
      last_accepted_cartesian_pos_valid_ = false;
      last_accepted_cartesian_ts_ns_ = 0;
      last_commanded_joints_valid_ = false;
      last_commanded_joints_.clear();
    }
    return backend_result;
  }

  auto final_backend_snapshot = execution_backend_.get_execution_snapshot();

  auto stopped_result = sm_.try_transition(ExecutionState::kStopped);
  if (!stopped_result.ok()) {
    // State machine refused the Stopped transition — surface as
    // InterventionRequired rather than papering over.
    (void)sm_.try_transition(ExecutionState::kInterventionRequired);
    result.accepted = false;
    result.error = stopped_result.error();
    {
      std::lock_guard<std::mutex> lock(streaming_session_mutex_);
      active_streaming_session_id_ = 0;
      active_backend_session_id_ = 0;
      ++active_streaming_generation_;
      active_streaming_config_ = StreamingSessionConfigDTO{};
      active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
      last_accepted_cartesian_pos_valid_ = false;
      last_accepted_cartesian_ts_ns_ = 0;
      last_commanded_joints_valid_ = false;
      last_commanded_joints_.clear();
    }
    return result;
  }

  uint64_t closed_kernel_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    closed_kernel_id = active_streaming_session_id_;
    active_streaming_session_id_ = 0;
    active_backend_session_id_ = 0;
    ++active_streaming_generation_;
    active_streaming_config_ = StreamingSessionConfigDTO{};
    active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
    last_accepted_cartesian_pos_valid_ = false;
    last_accepted_cartesian_ts_ns_ = 0;
    last_commanded_joints_valid_ = false;
    last_commanded_joints_.clear();
  }

  emit_event({KernelEventType::kStreamingSessionEnded,
              0,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"session_id", std::to_string(closed_kernel_id)},
               {"soft_stop_strategy", to_string(strategy)}}});
  emit_event({KernelEventType::kStateTransition,
              0,
              clock_.now(),
              ExecutionState::kStopping,
              ExecutionState::kStopped,
              std::nullopt,
              {{"session_id", std::to_string(closed_kernel_id)}}});

  // ADR-0016: streaming-session end is operator-acknowledged at release
  // time (mouseUp ≡ "I'm done"). Auto-return to kIdle so the next
  // begin_streaming_session is admitted without requiring an explicit
  // reset click. Distinct from discrete trajectories, which require
  // acknowledge_completed.
  auto idle_result = sm_.try_transition(ExecutionState::kIdle);
  if (idle_result.ok()) {
    emit_event({KernelEventType::kStateTransition,
                0,
                clock_.now(),
                ExecutionState::kStopped,
                ExecutionState::kIdle,
                std::nullopt,
                {{"session_id", std::to_string(closed_kernel_id)},
                 {"reason", "streaming_session_acknowledged"}}});
  }

  final_backend_snapshot.state = sm_.status().state;
  final_backend_snapshot.progress = 0.0;
  final_backend_snapshot.execution_id = 0;
  final_backend_snapshot.move_id.clear();
  final_backend_snapshot.timestamp_ns = clock_.now();
  publish_execution_snapshot(std::move(final_backend_snapshot));

  result.accepted = true;
  return result;
}

}  // namespace manyforge::core
