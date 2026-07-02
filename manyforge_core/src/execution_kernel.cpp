#include <manyforge_core/execution_kernel.hpp>

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace manyforge::core {
namespace {

// Forward declarations of the controlled-stop string helpers defined
// later in this file (in the anonymous namespace that starts near
// line 1735). They are needed earlier, in the fault-stop preempt path
// inside `apply_latched_interrupt_preemption`, so the
// emitted error events can carry the same `requested_stop_mode` /
// `executed_stop_mode` / `stop_command_source` strings the desired-
// trip path uses.
const char* stop_mode_string(MotionInterruptStopMode mode);
const char* stop_command_source_string(MotionInterruptStopCommandSource s);

constexpr int64_t kExecutionPollNs = 10'000'000;  // 10 ms
constexpr int kExecutionDrainMaxPolls = 100;      // bounded 1 s software drain
constexpr double kExecutionDrainTimeoutS =
    static_cast<double>(kExecutionPollNs * kExecutionDrainMaxPolls) / 1e9;
thread_local bool t_inside_collision_monitor_callback = false;

struct HardStopFloorAttempt {
  std::mutex mutex;
  bool attempted = false;
  StopResultDTO result;
};

StopResultDTO request_hard_stop_floor_once(
    IExecutionBackend& backend,
    const std::shared_ptr<HardStopFloorAttempt>& attempt) {
  std::lock_guard<std::mutex> lock(attempt->mutex);
  if (!attempt->attempted) {
    attempt->result = backend.stop();
    attempt->attempted = true;
  }
  return attempt->result;
}

std::optional<StopResultDTO> hard_stop_floor_result_if_attempted(
    const std::shared_ptr<HardStopFloorAttempt>& attempt) {
  std::lock_guard<std::mutex> lock(attempt->mutex);
  if (!attempt->attempted) {
    return std::nullopt;
  }
  return attempt->result;
}

std::string backend_hard_stop_level(IExecutionBackend& backend) {
  try {
    const auto cap = backend.get_motion_interrupt_stop_capability();
    return cap.hard_stop_level.value_or("unknown");
  } catch (const std::exception&) {
    return "unknown";
  }
}

std::string hard_stop_floor_role_from_level(const std::string& level) {
  if (level == "hard_deceleration") {
    return "hard_deceleration";
  }
  if (level == "native_immediate_hold" ||
      level == "jtc_cancel_unprofiled_hold") {
    return "immediate_hard_stop";
  }
  return "immediate_hard_stop";
}

std::string hard_stop_floor_command_source_from_level(
    const std::string& level) {
  if (level == "hard_deceleration") {
    return "jtc_cancel_decelerate_on_cancel";
  }
  if (level == "native_immediate_hold") {
    return "native_simulated_stop";
  }
  if (level == "jtc_cancel_unprofiled_hold") {
    return "jtc_cancel_unprofiled_hold";
  }
  return "backend_stop";
}

void add_hard_stop_floor_details(
    std::map<std::string, std::string>& details,
    IExecutionBackend& backend) {
  const auto level = backend_hard_stop_level(backend);
  details["hard_stop_level"] = level;
  details["hard_stop_floor"] = hard_stop_floor_role_from_level(level);
  details["hard_stop_floor_command_source"] =
      hard_stop_floor_command_source_from_level(level);
}

void add_optional_double_detail(std::map<std::string, std::string>& details,
                                const char* key,
                                const std::optional<double>& value) {
  if (value.has_value()) {
    details[key] = std::to_string(*value);
  }
}

void merge_controlled_stop_diagnostics(
    std::map<std::string, std::string>& details,
    const InterruptStructuredValue& diagnostics) {
  for (const auto& [key, value] : diagnostics) {
    if (details.contains(key)) {
      details["controlled_stop_detail_" + key] = value;
    } else {
      details[key] = value;
    }
  }
}

std::map<std::string, std::string> controlled_stop_success_event_details(
    const MotionInterruptStopProfileDTO& profile,
    const ControlledStopResultDTO& result,
    IExecutionBackend& backend,
    const std::string& phase) {
  std::map<std::string, std::string> details{
      {"phase", phase},
      {"controlled_stop_success", "true"},
      {"requested_stop_profile_id", profile.profile_id},
      {"requested_stop_mode", stop_mode_string(profile.stop_mode)},
      {"executed_stop_mode", stop_mode_string(result.executed_stop_mode)},
      {"stop_command_source",
       stop_command_source_string(result.stop_command_source)},
      {"backend_stop_confirmed",
       result.backend_stop_confirmed ? "true" : "false"},
      {"completed_by_natural_completion",
       result.completed_by_natural_completion ? "true" : "false"},
      {"confirmation_latency_ns",
       std::to_string(result.confirmation_latency_ns)},
      {"deceleration_time_s",
       std::to_string(motion_interrupt_deceleration_time_or_default(profile))},
      {"emergency_fallback", "none"},
      {"hard_stop_transitioned", "false"}};
  if (profile.max_stop_time_s.has_value()) {
    details["max_stop_time_s"] = std::to_string(*profile.max_stop_time_s);
  }
  if (result.fallback_reason.has_value()) {
    details["fallback_reason"] = *result.fallback_reason;
  }
  add_optional_double_detail(details, "stop_distance_rad",
                             result.stop_distance_rad);
  add_optional_double_detail(details, "path_deviation_max_rad",
                             result.path_deviation_max_rad);
  add_optional_double_detail(details, "initial_path_deviation_rad",
                             result.initial_path_deviation_rad);
  merge_controlled_stop_diagnostics(details, result.diagnostics);
  add_hard_stop_floor_details(details, backend);
  return details;
}

std::int64_t effective_controlled_stop_deadline_ns(
    const MotionInterruptStopProfileDTO& profile,
    Duration stop_completion_tolerance_ns) {
  if (profile.max_stop_time_s.has_value()) {
    return static_cast<std::int64_t>(*profile.max_stop_time_s * 1e9);
  }
  const auto deceleration_ns = static_cast<std::int64_t>(
      motion_interrupt_deceleration_time_or_default(profile) * 1e9);
  return deceleration_ns + std::max<Duration>(0, stop_completion_tolerance_ns);
}

std::chrono::steady_clock::duration effective_controlled_stop_deadline_duration(
    const MotionInterruptStopProfileDTO& profile,
    Duration stop_completion_tolerance_ns) {
  const auto deadline_ns =
      std::max<std::int64_t>(
          0, effective_controlled_stop_deadline_ns(
                 profile, stop_completion_tolerance_ns));
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::nanoseconds(deadline_ns));
}

std::optional<double> measured_max_abs_velocity_from_snapshot(
    const ExecutionSnapshot& snapshot) {
  if (snapshot.joint_velocities_rad_s.empty() ||
      snapshot.joint_velocities_rad_s.size() !=
          snapshot.joint_positions_rad.size()) {
    return std::nullopt;
  }
  double max_abs = 0.0;
  for (double v : snapshot.joint_velocities_rad_s) {
    max_abs = std::max(max_abs, std::abs(v));
  }
  return max_abs;
}

bool all_joint_positions_finite(const ExecutionSnapshot& snapshot) {
  if (snapshot.joint_positions_rad.empty()) {
    return false;
  }
  for (double q : snapshot.joint_positions_rad) {
    if (!std::isfinite(q)) {
      return false;
    }
  }
  return true;
}

bool all_joint_velocities_finite_when_present(
    const ExecutionSnapshot& snapshot) {
  for (double v : snapshot.joint_velocities_rad_s) {
    if (!std::isfinite(v)) {
      return false;
    }
  }
  return true;
}

std::optional<double> parse_positive_double_detail(
    const std::map<std::string, std::string>& details,
    const std::string& key) {
  const auto it = details.find(key);
  if (it == details.end()) {
    return std::nullopt;
  }
  try {
    std::size_t parsed = 0;
    const double value = std::stod(it->second, &parsed);
    if (parsed != it->second.size() || !std::isfinite(value) ||
        value <= 0.0) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool is_classified_stop_intervention(
    const std::map<std::string, std::string>& details) {
  const auto reason_it = details.find("reason");
  const std::string reason =
      reason_it != details.end() ? reason_it->second : std::string{};
  if (reason == "stop_not_settled_measured_velocity" ||
      reason == "operator_stop_total_failure" ||
      reason == "in_stop_completion_timeout" ||
      reason == "stop_path_collision" ||
      reason == "hard_deceleration_not_settled") {
    return true;
  }
  const auto phase_it = details.find("phase");
  const bool stop_phase =
      phase_it != details.end() &&
      (phase_it->second == "operator_stop" ||
       phase_it->second == "motion_trip_stop" ||
       phase_it->second == "motion_trip_stop_path_collision");
  const bool has_stop_mode =
      details.contains("requested_stop_mode") ||
      details.contains("executed_stop_mode") ||
      details.contains("hard_stop_transitioned");
  return stop_phase && has_stop_mode;
}

struct MeasuredRestWaitResult {
  std::optional<double> max_abs_velocity_rad_s;
  bool settle_signal_available = false;
  bool at_rest = true;
  bool timed_out = false;
};

bool is_command_authoritative_stop_snapshot(const ExecutionSnapshot& snapshot) {
  return snapshot.state_source.rfind("commanded_sim", 0) == 0 ||
         snapshot.state_source == "controlled_stop_command_reference";
}

MeasuredRestWaitResult wait_for_measured_rest_after_confirmed_stop(
    IExecutionBackend& backend,
    double settle_velocity_threshold_rad_s,
    std::chrono::steady_clock::time_point deadline,
    std::stop_token stop_token) {
  MeasuredRestWaitResult result;
  while (true) {
    const auto snapshot = backend.get_execution_snapshot();
    if (is_command_authoritative_stop_snapshot(snapshot)) {
      // In commanded-sim lanes the backend command/reference trajectory is the
      // execution authority; measured velocities are diagnostic evidence and
      // can lag the accepted command stream. Tracking/path-deviation monitors
      // remain responsible for proving the simulated/real plant followed the
      // command within tolerance.
      result.at_rest = true;
      return result;
    }
    const auto max_abs_velocity =
        measured_max_abs_velocity_from_snapshot(snapshot);
    if (!max_abs_velocity.has_value()) {
      // Settle UNKNOWN falls back to the backend's own confirmation. Never
      // fabricate a failed measured-rest verdict from absent velocity data.
      return result;
    }
    result.settle_signal_available = true;
    result.max_abs_velocity_rad_s = *max_abs_velocity;
    result.at_rest = *max_abs_velocity <= settle_velocity_threshold_rad_s;
    if (result.at_rest) {
      return result;
    }
    if (stop_token.stop_requested()) {
      result.timed_out = true;
      return result;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      result.timed_out = true;
      return result;
    }
    const auto remaining = deadline - now;
    const auto sleep_for =
        std::min(std::chrono::milliseconds(10),
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     remaining));
    std::this_thread::sleep_for(
        sleep_for > std::chrono::milliseconds(0)
            ? sleep_for
            : std::chrono::milliseconds(1));
  }
}

double clamp_unit_interval(double value) {
  return std::max(0.0, std::min(1.0, value));
}

bool operator_controlled_stop_allowed(ExecutionState state) {
  return state == ExecutionState::kExecuting ||
         state == ExecutionState::kPaused ||
         state == ExecutionState::kStopping;
}

std::string comma_join_strings(const std::vector<std::string>& values) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << values[i];
  }
  return out.str();
}

std::string comma_join_doubles(const std::vector<double>& values) {
  std::ostringstream out;
  out << std::setprecision(17);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << values[i];
  }
  return out.str();
}

std::string format_double(double value) {
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
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
  // Motion-scoped outcomes (spec/210 §5.2): each motion-interrupt
  // error has its own ErrorCode so the state-machine
  // `last_error` surface distinguishes them rather than collapsing all
  // motion-interrupt failures to `internal_error`. Audit / release
  // evidence relies on the state-machine code, not just the outcome
  // DTO, to classify a terminal execution.
  if (error.code == kErrorMotionInterruptArmFailed) {
    return ErrorCode::kMotionInterruptArmFailed;
  }
  if (error.code == kErrorMotionInterruptSourceActiveAtArm) {
    return ErrorCode::kMotionInterruptSourceActiveAtArm;
  }
  if (error.code == kErrorMotionInterruptBackendStopFailed) {
    return ErrorCode::kMotionInterruptBackendStopFailed;
  }
  if (error.code == kErrorMotionInterruptStopStateInvalid) {
    return ErrorCode::kMotionInterruptStopStateInvalid;
  }
  return ErrorCode::kInternalError;
}

std::string collision_engine_status_name(CollisionEngineStatus status) {
  switch (status) {
    case CollisionEngineStatus::kOk:
      return "ok";
    case CollisionEngineStatus::kTimeout:
      return "timeout";
    case CollisionEngineStatus::kUnsupportedGeometry:
      return "unsupported_geometry";
    case CollisionEngineStatus::kDiagnosticIncomplete:
      return "diagnostic_incomplete";
    case CollisionEngineStatus::kCancelled:
      return "cancelled";
    case CollisionEngineStatus::kEngineError:
      return "engine_error";
  }
  return "unknown";
}

std::string collision_decision_result_name(CollisionDecisionResult decision) {
  switch (decision) {
    case CollisionDecisionResult::kPass:
      return "pass";
    case CollisionDecisionResult::kFail:
      return "fail";
    case CollisionDecisionResult::kWarn:
      return "warn";
    case CollisionDecisionResult::kStopRequested:
      return "stop_requested";
    case CollisionDecisionResult::kInconclusiveFail:
      return "inconclusive_fail";
  }
  return "unknown";
}

std::string collision_pair_classification_name(
    CollisionPairClassification classification) {
  switch (classification) {
    case CollisionPairClassification::kBlockingCollision:
      return "blocking_collision";
    case CollisionPairClassification::kAllowedStaticStatic:
      return "allowed_static_static";
    case CollisionPairClassification::kSupportContactStart:
      return "support_contact_start";
    case CollisionPairClassification::kSupportContactContinued:
      return "support_contact_continued";
    case CollisionPairClassification::kSupportContactSeparated:
      return "support_contact_separated";
    case CollisionPairClassification::kSupportContactRecontactViolation:
      return "support_contact_recontact_violation";
    case CollisionPairClassification::kAllowedTouchLink:
      return "allowed_touch_link";
    case CollisionPairClassification::kDiagnosticIncomplete:
      return "diagnostic_incomplete";
  }
  return "unknown";
}

bool collision_pair_classification_blocks_dispatch(
    CollisionPairClassification classification) {
  switch (classification) {
    case CollisionPairClassification::kBlockingCollision:
    case CollisionPairClassification::kSupportContactRecontactViolation:
    case CollisionPairClassification::kDiagnosticIncomplete:
      return true;
    case CollisionPairClassification::kAllowedStaticStatic:
    case CollisionPairClassification::kSupportContactStart:
    case CollisionPairClassification::kSupportContactContinued:
    case CollisionPairClassification::kSupportContactSeparated:
    case CollisionPairClassification::kAllowedTouchLink:
      return false;
  }
  return true;
}

std::string collision_pair_name(const CollisionPairId& pair) {
  return pair.body_a + " <-> " + pair.body_b;
}

CollisionPairId canonical_collision_pair(CollisionPairId pair) {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return pair;
}

std::string collision_pair_key(const CollisionPairId& pair) {
  const auto canonical = canonical_collision_pair(pair);
  return canonical.body_a + "\n" + canonical.body_b;
}

std::optional<double> pair_clearance_for_result(
    const CollisionResult& result,
    const CollisionPairId& pair) {
  const auto canonical = canonical_collision_pair(pair);
  for (const auto& clearance : result.pair_clearances) {
    if (canonical_collision_pair(clearance.pair) == canonical) {
      return clearance.signed_clearance_m;
    }
  }
  if (result.min_clearance_pair &&
      canonical_collision_pair(*result.min_clearance_pair) == canonical &&
      result.min_clearance_m) {
    return *result.min_clearance_m;
  }
  return std::nullopt;
}

void append_collision_result_observation(
    std::map<std::string, std::string>& details,
    const CollisionResult& result) {
  details["collision_result_engine_id"] = result.engine_id;
  details["collision_result_engine_version"] = result.engine_version;
  details["collision_result_engine_status"] =
      collision_engine_status_name(result.engine_status);
  details["collision_result_decision"] =
      collision_decision_result_name(result.decision);
  details["collision_result_in_collision"] =
      result.in_collision ? "true" : "false";
  details["collision_result_pair_clearance_count"] =
      std::to_string(result.pair_clearances.size());
  details["collision_result_normalized_pair_count"] =
      std::to_string(result.normalized_pairs.size());
  if (result.min_clearance_m) {
    details["collision_result_min_clearance_m"] =
        std::to_string(*result.min_clearance_m);
  }
  if (result.min_clearance_pair) {
    details["collision_result_min_clearance_pair"] =
        collision_pair_name(*result.min_clearance_pair);
  }
  if (!result.pair_clearances.empty()) {
    details["collision_result_first_clearance_pair"] =
        collision_pair_name(result.pair_clearances.front().pair);
    details["collision_result_first_clearance_m"] =
        std::to_string(result.pair_clearances.front().signed_clearance_m);
  }
  std::size_t emitted = 0;
  for (const auto& [key, value] : result.raw_diagnostics) {
    if (emitted >= 8) {
      details["collision_result_raw_diagnostics_truncated"] = "true";
      break;
    }
    details["collision_result_raw_" + key] = value;
    ++emitted;
  }
}

bool snapshot_too_old(std::int64_t now_ns,
                      std::int64_t timestamp_ns,
                      std::int64_t max_age_ns,
                      std::map<std::string, std::string>& details,
                      const std::string& timestamp_key,
                      const std::string& age_key,
                      const std::string& budget_key) {
  if (max_age_ns <= 0) {
    return false;
  }
  details[timestamp_key] = std::to_string(timestamp_ns);
  details[budget_key] = std::to_string(max_age_ns);
  if (timestamp_ns < 0) {
    details[age_key] = "invalid_negative_timestamp";
    return true;
  }
  if (timestamp_ns > now_ns) {
    details[age_key] = "timestamp_in_future";
    details["clock_skew_ns"] = std::to_string(timestamp_ns - now_ns);
    return true;
  }
  const auto age_ns = now_ns - timestamp_ns;
  details[age_key] = std::to_string(age_ns);
  return age_ns > max_age_ns;
}

std::optional<std::string> validate_dynamic_body_freshness(
    const CollisionWorldSnapshot& snapshot,
    std::int64_t now_ns,
    std::int64_t runtime_cap_ns,
    std::map<std::string, std::string>& details) {
  for (const auto& body : snapshot.bodies) {
    if (!body.source_freshness_budget_ns.has_value()) {
      continue;
    }
    details["dynamic_body_id"] = body.body_id;
    details["dynamic_body_declared_freshness_budget_ns"] =
        std::to_string(*body.source_freshness_budget_ns);
    const auto effective_budget_ns =
        runtime_cap_ns > 0
            ? std::min(*body.source_freshness_budget_ns, runtime_cap_ns)
            : *body.source_freshness_budget_ns;
    details["dynamic_body_effective_freshness_budget_ns"] =
        std::to_string(effective_budget_ns);
    if (!body.source_timestamp_ns.has_value()) {
      return "dynamic_body_missing_source_timestamp";
    }
    details["dynamic_body_source_timestamp_ns"] =
        std::to_string(*body.source_timestamp_ns);
    if (*body.source_timestamp_ns < 0) {
      details["dynamic_body_age_ns"] = "invalid_negative_timestamp";
      return "stale_dynamic_collision_body";
    }
    if (*body.source_timestamp_ns > now_ns) {
      details["dynamic_body_age_ns"] = "timestamp_in_future";
      details["dynamic_body_clock_skew_ns"] =
          std::to_string(*body.source_timestamp_ns - now_ns);
      return "stale_dynamic_collision_body";
    }
    const auto age_ns = now_ns - *body.source_timestamp_ns;
    details["dynamic_body_age_ns"] = std::to_string(age_ns);
    if (effective_budget_ns <= 0 || age_ns > effective_budget_ns) {
      return "stale_dynamic_collision_body";
    }
  }
  return std::nullopt;
}

void append_collision_failure_diagnostics(
    std::map<std::string, std::string>& details,
    const CollisionValidationDecisionBundle& bundle) {
  const CollisionValidationWaypointRecord* selected = nullptr;
  if (bundle.decision.first_failing_waypoint) {
    for (const auto& record : bundle.waypoint_records) {
      if (record.waypoint_index == *bundle.decision.first_failing_waypoint) {
        selected = &record;
        break;
      }
    }
  }
  if (selected == nullptr) {
    for (const auto& record : bundle.waypoint_records) {
      if (record.result.decision != CollisionDecisionResult::kPass ||
          record.result.engine_status != CollisionEngineStatus::kOk) {
        selected = &record;
        break;
      }
    }
  }
  if (selected == nullptr) {
    return;
  }

  const auto& result = selected->result;
  details["collision_result_waypoint_index"] =
      std::to_string(selected->waypoint_index);
  if (result.query_id) {
    details["collision_result_query_id"] = *result.query_id;
  }
  details["collision_result_engine_id"] = result.engine_id;
  details["collision_result_engine_version"] = result.engine_version;
  details["collision_result_engine_status"] =
      collision_engine_status_name(result.engine_status);
  details["collision_result_decision"] =
      collision_decision_result_name(result.decision);
  details["collision_result_in_collision"] =
      result.in_collision ? "true" : "false";
  details["collision_result_normalized_pair_count"] =
      std::to_string(result.normalized_pairs.size());
  if (!result.normalized_pairs.empty()) {
    details["collision_result_first_pair"] =
        collision_pair_name(result.normalized_pairs.front());
  }
  details["collision_result_classified_pair_count"] =
      std::to_string(result.normalized_pairs_classified.size());
  const CollisionPairResult* first_failure_pair = nullptr;
  for (const auto& classified : result.normalized_pairs_classified) {
    if (classified.is_first_failure) {
      first_failure_pair = &classified;
      break;
    }
  }
  if (first_failure_pair == nullptr) {
    for (const auto& classified : result.normalized_pairs_classified) {
      if (collision_pair_classification_blocks_dispatch(
              classified.classification)) {
        first_failure_pair = &classified;
        break;
      }
    }
  }
  if (first_failure_pair != nullptr) {
    details["collision_result_first_failure_pair"] =
        collision_pair_name(first_failure_pair->pair);
    details["collision_result_first_failure_classification"] =
        collision_pair_classification_name(first_failure_pair->classification);
  }
  std::size_t emitted = 0;
  for (const auto& [key, value] : result.raw_diagnostics) {
    if (emitted >= 8) {
      details["collision_result_raw_diagnostics_truncated"] = "true";
      break;
    }
    details["collision_result_raw_" + key] = value;
    ++emitted;
  }
}

const CollisionValidationWaypointRecord* select_collision_failure_record(
    const CollisionValidationDecisionBundle& bundle) {
  if (bundle.decision.first_failing_waypoint) {
    for (const auto& record : bundle.waypoint_records) {
      if (record.waypoint_index == *bundle.decision.first_failing_waypoint) {
        return &record;
      }
    }
  }
  for (const auto& record : bundle.waypoint_records) {
    if (record.result.decision != CollisionDecisionResult::kPass ||
        record.result.engine_status != CollisionEngineStatus::kOk) {
      return &record;
    }
  }
  return nullptr;
}

void append_legacy_collision_compare_diagnostics(
    std::map<std::string, std::string>& details,
    const CollisionValidationDecisionBundle& bundle,
    const TrajectoryDTO& trajectory,
    IPlanningContext& context) {
  const auto* selected = select_collision_failure_record(bundle);
  if (selected == nullptr || selected->waypoint_index < 0) {
    return;
  }
  const auto waypoint_index = static_cast<std::size_t>(selected->waypoint_index);
  if (waypoint_index >= trajectory.waypoints.size()) {
    return;
  }

  const auto& waypoint = trajectory.waypoints[waypoint_index];
  CollisionQuery query;
  query.query_id = "legacy_compare_" +
                   (selected->result.query_id
                        ? *selected->result.query_id
                        : std::to_string(selected->waypoint_index));
  query.surface = CollisionSurface::kDiagnosticCompare;
  query.collision_world_revision = bundle.borrowed_world_revision;
  query.policy_revision = selected->result.policy_revision_used;
  query.robot_state.joint_names = trajectory.joint_names;
  query.robot_state.positions_rad = waypoint.positions_rad;
  query.robot_state.velocities_rad_s = waypoint.velocities_rad_s;
  query.waypoint_index = selected->waypoint_index;
  query.state_time_ns = selected->time_from_start_ns;
  query.sampling_mode = CollisionSamplingMode::kWaypointOnly;

  try {
    const auto legacy = context.collision_check_normalized(query);
    details["legacy_collision_compare_engine_id"] = legacy.engine_id;
    details["legacy_collision_compare_engine_status"] =
        collision_engine_status_name(legacy.engine_status);
    details["legacy_collision_compare_decision"] =
        collision_decision_result_name(legacy.decision);
    details["legacy_collision_compare_in_collision"] =
        legacy.in_collision ? "true" : "false";
    details["legacy_collision_compare_normalized_pair_count"] =
        std::to_string(legacy.normalized_pairs.size());
    details["legacy_collision_compare_in_collision_match"] =
        legacy.in_collision == selected->result.in_collision ? "true" : "false";
    details["legacy_collision_compare_decision_match"] =
        legacy.decision == selected->result.decision ? "true" : "false";
    details["legacy_collision_compare_normalized_pairs_match"] =
        legacy.normalized_pairs == selected->result.normalized_pairs ? "true"
                                                                     : "false";
    if (legacy.collision_world_revision.has_value()) {
      details["legacy_collision_compare_world_revision_id"] =
          std::to_string(legacy.collision_world_revision->revision_id);
      details["legacy_collision_compare_world_content_hash"] =
          legacy.collision_world_revision->content_hash;
    }
    if (!legacy.normalized_pairs.empty()) {
      details["legacy_collision_compare_first_pair"] =
          collision_pair_name(legacy.normalized_pairs.front());
    }
    const CollisionPairResult* legacy_failure = nullptr;
    for (const auto& classified : legacy.normalized_pairs_classified) {
      if (classified.is_first_failure) {
        legacy_failure = &classified;
        break;
      }
    }
    if (legacy_failure == nullptr) {
      for (const auto& classified : legacy.normalized_pairs_classified) {
        if (collision_pair_classification_blocks_dispatch(
                classified.classification)) {
          legacy_failure = &classified;
          break;
        }
      }
    }
    if (legacy_failure != nullptr) {
      details["legacy_collision_compare_first_failure_pair"] =
          collision_pair_name(legacy_failure->pair);
      details["legacy_collision_compare_first_failure_classification"] =
          collision_pair_classification_name(legacy_failure->classification);
    }
  } catch (const std::exception& e) {
    details["legacy_collision_compare_error"] = e.what();
  } catch (...) {
    details["legacy_collision_compare_error"] = "unknown_exception";
  }
}

CollisionCheckResultDTO collision_check_result_from_bundle(
    const CollisionValidationDecisionBundle& bundle) {
  CollisionCheckResultDTO check;
  const auto* record = select_collision_failure_record(bundle);
  if (record == nullptr) {
    return check;
  }

  check.in_collision = record->result.in_collision;
  if (!record->result.normalized_pairs.empty()) {
    check.contact_body_a = record->result.normalized_pairs.front().body_a;
    check.contact_body_b = record->result.normalized_pairs.front().body_b;
  }
  for (const auto& classified : record->result.normalized_pairs_classified) {
    if (!collision_pair_classification_blocks_dispatch(
            classified.classification)) {
      continue;
    }
    check.contact_body_a = classified.pair.body_a;
    check.contact_body_b = classified.pair.body_b;
    break;
  }
  if (record->result.min_clearance_m &&
      *record->result.min_clearance_m < 0.0) {
    check.penetration_depth_m = -*record->result.min_clearance_m;
  }
  return check;
}

bool collision_world_revision_is_set(const CollisionWorldRevision& revision) {
  return revision.revision_id != 0 || !revision.content_hash.empty();
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

bool authoritative_collision_replan_candidate(const ErrorDTO& error) {
  if (error.code != kErrorValidationCollision ||
      validation_cancelled_by_interrupt(error)) {
    return false;
  }
  const auto decision_it = error.details.find("collision_result_decision");
  if (decision_it == error.details.end() || decision_it->second != "fail") {
    return false;
  }
  const auto status_it = error.details.find("collision_result_engine_status");
  return status_it != error.details.end() && status_it->second == "ok";
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
      // Motion-scoped interrupt event surface (spec/260 §7).
      // Manager emits the names; the kernel maps them
      // to typed events for subscribers (Composer, BT runtime, audit).
      {"motion_interrupt_scope_armed",
       KernelEventType::kMotionInterruptScopeArmed},
      {"motion_interrupt_scope_reaped",
       KernelEventType::kMotionInterruptScopeReaped},
      {"motion_interrupt_queued_update_dropped",
       KernelEventType::kMotionInterruptQueuedUpdateDropped},
      {"motion_trip_secondary_signal",
       KernelEventType::kMotionTripSecondarySignal},
      {"motion_trip_failed", KernelEventType::kMotionTripFailed},
      {"motion_trip_completed", KernelEventType::kMotionTripCompleted},
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
      stop_policy_(sm_, scheduler, clock, [this]() { cache_.invalidate_all(); }) {
  if (config_.collision_monitor_period_ns <= 0) {
    throw std::invalid_argument("collision_monitor_period_ns must be > 0");
  }
  if (config_.runtime_collision_query_timeout_ns <= 0) {
    throw std::invalid_argument(
        "runtime_collision_query_timeout_ns must be > 0");
  }
  if (config_.native_reset_completion_timeout_s <= 0.0 ||
      !std::isfinite(config_.native_reset_completion_timeout_s)) {
    throw std::invalid_argument(
        "native_reset_completion_timeout_s must be finite and > 0");
  }
  if (config_.single_shot_ik_timeout_s <= 0.0 ||
      !std::isfinite(config_.single_shot_ik_timeout_s)) {
    throw std::invalid_argument(
        "single_shot_ik_timeout_s must be finite and > 0");
  }
  if (config_.planner_max_planning_time_s <= 0.0 ||
      !std::isfinite(config_.planner_max_planning_time_s)) {
    throw std::invalid_argument(
        "planner_max_planning_time_s must be finite and > 0");
  }
  if (config_.authoritative_validation_collision_replan_attempts < 0) {
    config_.authoritative_validation_collision_replan_attempts = 0;
  }
  if (config_.validation_collision_workers < 1) {
    config_.validation_collision_workers = 1;
  }
  if (config_.validation_collision_min_waypoints_for_parallel < 1) {
    config_.validation_collision_min_waypoints_for_parallel = 1;
  }
  // The operator-stop profile is invoked by `ExecutionKernel::stop()` on the
  // dedicated operator-stop worker so controlled-stop failures can be inspected
  // and escalated. The stop-policy callback stays the legacy hard-stop path
  // used by system faults and explicit hard-stop escalation.
  stop_policy_.set_stop_callback([this]() {
    (void)execution_backend_.stop();
  });
  // Spec/230 §6.1 pause chain: the Stopping → Paused transition fires
  // inside the scheduled lambda of stop_policy_.request_pause(). This
  // callback emits the terminal state_transition event so external
  // observers see the full pause boundary.
  stop_policy_.set_pause_completed_callback([this]() {
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id, clock_.now(),
                ExecutionState::kStopping, ExecutionState::kPaused,
                std::nullopt, {}});
  });
  install_builtin_interrupt_sources();
  sync_snapshot_from_context();
}

ExecutionKernel::~ExecutionKernel() {
  // Scheduled collision callbacks capture `this`; cancel them before any
  // member teardown so destruction cannot race a monitor tick.
  stop_collision_monitoring();

  cancel_operator_stop_worker(/*invalidate_generation=*/true);
  if (operator_stop_thread_.joinable()) {
    operator_stop_thread_.request_stop();
    operator_stop_thread_.join();
  }

  // Join before member destruction so worker callbacks cannot notify the
  // condition variable after it has been destroyed. This is intentionally
  // explicit instead of relying on std::jthread member teardown order.
  if (worker_thread_.joinable()) {
    worker_thread_.request_stop();
    completion_cv_.notify_all();
    worker_thread_.join();
  }
}

bool ExecutionKernel::operator_stop_worker_is_current(
    std::uint64_t generation, std::stop_token stop_token) const {
  if (stop_token.stop_requested()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(operator_stop_mutex_);
  return operator_stop_worker_active_.load(std::memory_order_acquire) &&
         generation == operator_stop_generation_;
}

bool ExecutionKernel::cancel_operator_stop_worker(
    bool invalidate_generation) {
  std::lock_guard<std::mutex> lock(operator_stop_mutex_);
  const bool active =
      operator_stop_worker_active_.load(std::memory_order_acquire);
  if (active && operator_stop_thread_.joinable()) {
    operator_stop_thread_.request_stop();
  }
  if (invalidate_generation) {
    ++operator_stop_generation_;
  }
  return active;
}

void ExecutionKernel::join_finished_operator_stop_worker() {
  std::jthread finished;
  {
    std::lock_guard<std::mutex> lock(operator_stop_mutex_);
    if (!operator_stop_thread_.joinable() ||
        operator_stop_worker_active_.load(std::memory_order_acquire)) {
      return;
    }
    finished = std::move(operator_stop_thread_);
  }
  finished.join();
}

bool ExecutionKernel::wait_for_operator_stop_worker(
    std::chrono::steady_clock::duration timeout) {
  std::unique_lock<std::mutex> lock(operator_stop_mutex_);
  if (!operator_stop_worker_active_.load(std::memory_order_acquire)) {
    return true;
  }
  return operator_stop_cv_.wait_for(lock, timeout, [this]() {
    return !operator_stop_worker_active_.load(std::memory_order_acquire);
  });
}

bool ExecutionKernel::start_operator_stop_worker(
    MotionInterruptStopProfileDTO profile,
    ExecutionState pre_completion_state) {
  join_finished_operator_stop_worker();
  std::lock_guard<std::mutex> lock(operator_stop_mutex_);
  if (operator_stop_worker_active_.load(std::memory_order_acquire)) {
    return false;
  }
  const auto generation = ++operator_stop_generation_;
  operator_stop_worker_active_.store(true, std::memory_order_release);
  operator_stop_thread_ = std::jthread(
      [this, generation, profile = std::move(profile),
       pre_completion_state](std::stop_token stop_token) mutable {
        run_operator_stop_worker(generation, std::move(profile),
                                 pre_completion_state, stop_token);
      });
  return true;
}

void ExecutionKernel::run_operator_stop_worker(
    std::uint64_t generation,
    MotionInterruptStopProfileDTO profile,
    ExecutionState pre_completion_state,
    std::stop_token stop_token) {
  struct ActiveGuard {
    ExecutionKernel* self;
    ~ActiveGuard() {
      {
        std::lock_guard<std::mutex> lock(self->operator_stop_mutex_);
        self->operator_stop_worker_active_.store(false,
                                                 std::memory_order_release);
      }
      self->operator_stop_cv_.notify_all();
    }
  } active_guard{this};

  begin_runtime_velocity_scale_freeze();
  const auto runtime_scale_freeze_scope =
      std::shared_ptr<void>(nullptr, [this](void*) {
        end_runtime_velocity_scale_freeze();
      });

  const std::int64_t effective_deadline_ns =
      effective_controlled_stop_deadline_ns(
          profile, config_.complete_move_on_signal_timeout_ns);
  auto hard_stop_floor_attempt = std::make_shared<HardStopFloorAttempt>();
  auto controlled_stop_source = std::make_shared<std::stop_source>();
  auto deadline_watchdog_fired = std::make_shared<std::atomic_bool>(false);
  std::stop_callback worker_stop_notify(stop_token, [controlled_stop_source]() {
    controlled_stop_source->request_stop();
  });
  ScheduleHandle stop_deadline_handle = 0;
  if (effective_deadline_ns > 0) {
    const auto requested_mode = std::string(stop_mode_string(profile.stop_mode));
    stop_deadline_handle = scheduler_.schedule(
        clock_.now() + effective_deadline_ns,
        [this, controlled_stop_source, hard_stop_floor_attempt,
         deadline_watchdog_fired, requested_mode]() {
          deadline_watchdog_fired->store(true, std::memory_order_release);
          const auto legacy =
              request_hard_stop_floor_once(execution_backend_,
                                           hard_stop_floor_attempt);
          controlled_stop_source->request_stop();
          const auto fallback_outcome =
              legacy.accepted ? std::string("legacy_stop_succeeded")
                              : std::string("legacy_stop_failed");
          std::map<std::string, std::string> details{
              {"reason", "in_stop_completion_timeout"},
              {"phase", "operator_stop_deadline_watchdog"},
              {"requested_stop_mode", requested_mode},
              {"emergency_fallback", fallback_outcome},
              {"hard_stop_transitioned", "true"}};
          add_hard_stop_floor_details(details, execution_backend_);
          emit_event({KernelEventType::kErrorRaised,
                      sm_.status().execution_id,
                      clock_.now(),
                      {},
                      {},
                      ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                               "operator controlled stop exceeded "
                               "effective stop deadline; emergency hard-stop "
                               "floor issued by deadline watchdog",
                               details},
                      details});
        },
        SchedulerPriority::Monitoring);
  }

  const auto stop_wall_start = std::chrono::steady_clock::now();
  const auto stop_wall_deadline =
      effective_deadline_ns > 0
          ? stop_wall_start + std::chrono::nanoseconds(effective_deadline_ns)
          : stop_wall_start;
  auto controlled_result =
      execution_backend_.execute_controlled_stop(
          profile, controlled_stop_source->get_token());
  if (stop_deadline_handle != 0) {
    scheduler_.cancel(stop_deadline_handle);
  }
  if (!operator_stop_worker_is_current(generation, stop_token)) {
    return;
  }

  bool controlled_ok =
      controlled_result.accepted &&
      controlled_result.backend_stop_confirmed &&
      !controlled_result.error.has_value();
  bool confirmed_but_not_settled = false;
  std::string stop_failure_reason = "operator_stop_profile_failed";
  std::string stop_failure_message =
      "operator stop profile failed; emergency legacy backend.stop() "
      "fallback engaged";
  std::map<std::string, std::string> settle_details;

  if (controlled_ok) {
    const double threshold = profile.settle_velocity_threshold_rad_s.value_or(
        kDefaultSettleVelocityThresholdRadS);
    const auto settle_result = wait_for_measured_rest_after_confirmed_stop(
        execution_backend_, threshold, stop_wall_deadline, stop_token);
    if (settle_result.settle_signal_available &&
        !settle_result.at_rest) {
      controlled_ok = false;
      confirmed_but_not_settled = true;
      stop_failure_reason = "stop_not_settled_measured_velocity";
      stop_failure_message =
          "operator controlled stop confirmed but measured velocity did not "
          "settle within the effective stop deadline";
      controlled_result.error = ErrorDTO{
          std::string(kErrorMotionInterruptBackendStopFailed),
          stop_failure_message,
          {{"reason", stop_failure_reason},
           {"measured_max_abs_velocity_rad_s",
            std::to_string(
                settle_result.max_abs_velocity_rad_s.value_or(0.0))},
           {"settle_velocity_threshold_rad_s", std::to_string(threshold)}}};
      settle_details["measured_max_abs_velocity_rad_s"] =
          std::to_string(
              settle_result.max_abs_velocity_rad_s.value_or(0.0));
      settle_details["settle_velocity_threshold_rad_s"] =
          std::to_string(threshold);
      settle_details["settle_wait_timed_out"] =
          settle_result.timed_out ? "true" : "false";
    }
  }

  std::optional<StopResultDTO> fallback_legacy_result;
  if (deadline_watchdog_fired->load(std::memory_order_acquire)) {
    fallback_legacy_result =
        hard_stop_floor_result_if_attempted(hard_stop_floor_attempt);
    if (!fallback_legacy_result.has_value()) {
      fallback_legacy_result = request_hard_stop_floor_once(
          execution_backend_, hard_stop_floor_attempt);
    }
    controlled_ok = false;
    stop_failure_reason = "in_stop_completion_timeout";
    stop_failure_message =
        "operator controlled stop exceeded the effective stop deadline; "
        "emergency hard-stop floor was issued by the deadline watchdog";
    controlled_result.error = ErrorDTO{
        std::string(kErrorMotionInterruptBackendStopFailed),
        stop_failure_message,
        {{"reason", stop_failure_reason},
         {"phase", "operator_stop_deadline_watchdog"}}};
  }
  if (!controlled_ok && !confirmed_but_not_settled) {
    fallback_legacy_result = request_hard_stop_floor_once(
        execution_backend_, hard_stop_floor_attempt);
  }
  if (!controlled_ok) {
    const std::string requested_mode = stop_mode_string(profile.stop_mode);
    const std::string executed_mode =
        stop_mode_string(controlled_result.executed_stop_mode);
    const std::string fallback_outcome =
        confirmed_but_not_settled
            ? std::string("not_attempted_controlled_stop_already_confirmed")
            : (fallback_legacy_result && fallback_legacy_result->accepted
                   ? std::string("legacy_stop_succeeded")
                   : std::string("legacy_stop_failed"));
    // Propagate the underlying controlled-stop failure cause into the
    // escalation event. A downgraded operator stop MUST be self-describing on
    // the event stream: it has to carry WHY soft_on_path was refused (the
    // planner error code + reason + the soft_on_path->spring_back fallback
    // pair), not only the fact that a downgrade happened.
    std::map<std::string, std::string> escalation_details{
        {"reason", stop_failure_reason},
        {"requested_stop_mode", requested_mode},
        {"executed_stop_mode", executed_mode},
        {"emergency_fallback", fallback_outcome},
        {"hard_stop_transitioned",
         confirmed_but_not_settled ? "false" : "true"},
        {"phase", "operator_stop"}};
    if (!confirmed_but_not_settled) {
      add_hard_stop_floor_details(escalation_details, execution_backend_);
    }
    for (const auto& [key, value] : settle_details) {
      escalation_details[key] = value;
    }
    if (controlled_result.error.has_value()) {
      escalation_details["controlled_stop_error_code"] =
          controlled_result.error->code;
      const auto reason_it = controlled_result.error->details.find("reason");
      if (reason_it != controlled_result.error->details.end()) {
        escalation_details["controlled_stop_error_reason"] = reason_it->second;
      }
    }
    // Carry the whole soft_on_path / fallback diagnostic surface (error code,
    // reason, per-detail jerk/accel/joint, and the fallback pairing) so the
    // escalation event is fully self-describing — nothing about WHY the
    // requested stop was refused is left only in dropped backend state.
    for (const auto& [diag_key, diag_value] : controlled_result.diagnostics) {
      if (diag_key.rfind("soft_on_path", 0) == 0 ||
          diag_key.rfind("fallback", 0) == 0 ||
          diag_key.rfind("hard_stop", 0) == 0) {
        escalation_details[diag_key] = diag_value;
      }
    }
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id, clock_.now(), {}, {},
                std::optional<ErrorDTO>{ErrorDTO{
                    std::string(kErrorMotionInterruptBackendStopFailed),
                    stop_failure_message,
                    escalation_details}},
                escalation_details});
  }

  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!operator_stop_worker_is_current(generation, stop_token)) {
    return;
  }

  const std::string requested_mode = stop_mode_string(profile.stop_mode);
  const std::string executed_mode =
      stop_mode_string(controlled_result.executed_stop_mode);

  if (!controlled_ok && confirmed_but_not_settled) {
    ErrorDTO unsettled_stop_error{
        std::string(kErrorMotionInterruptBackendStopFailed),
        "operator controlled stop was accepted and confirmed, but measured "
        "velocity did not settle within the effective stop deadline; redundant "
        "JTC cancel "
        "was not attempted",
        {{"reason", "stop_not_settled_measured_velocity"},
         {"controlled_stop_error_reason", stop_failure_reason},
         {"requested_stop_mode", requested_mode},
         {"executed_stop_mode", executed_mode},
         {"emergency_fallback",
          "not_attempted_controlled_stop_already_confirmed"},
         {"hard_stop_transitioned", "false"},
         {"phase", "operator_stop"}}};
    for (const auto& [key, value] : settle_details) {
      unsettled_stop_error.details[key] = value;
    }
    {
      std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
      operator_stop_intervention_error_ = unsettled_stop_error;
    }
    sm_.set_last_error(error_code_from_dto(unsettled_stop_error));
    stop_collision_monitoring();
    cache_.invalidate_all();
    auto transition = sm_.try_transition(ExecutionState::kInterventionRequired);
    if (!transition.ok()) {
      return;
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    snapshot.error = unsettled_stop_error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                unsettled_stop_error,
                unsettled_stop_error.details});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                pre_completion_state,
                ExecutionState::kInterventionRequired,
                std::nullopt,
                {{"reason", "stop_not_settled_measured_velocity"}}});
    return;
  }

  if (!controlled_ok && fallback_legacy_result &&
      !fallback_legacy_result->accepted) {
    ErrorDTO total_failure{
        std::string(kErrorMotionInterruptBackendStopFailed),
        "operator stop failed: controlled-stop refused and emergency "
        "legacy backend.stop() also failed",
        {{"reason", "operator_stop_total_failure"},
         {"controlled_stop_error_reason", stop_failure_reason},
         {"requested_stop_mode", requested_mode},
         {"executed_stop_mode", executed_mode},
         {"emergency_fallback", "legacy_stop_failed"},
         {"hard_stop_transitioned", "true"},
         {"phase", "operator_stop"}}};
    add_hard_stop_floor_details(total_failure.details, execution_backend_);
    for (const auto& [key, value] : settle_details) {
      total_failure.details[key] = value;
    }
    if (controlled_result.error.has_value()) {
      total_failure.details["controlled_stop_error_code"] =
          controlled_result.error->code;
      const auto reason_it = controlled_result.error->details.find("reason");
      if (reason_it != controlled_result.error->details.end()) {
        total_failure.details["controlled_stop_error_reason"] =
            reason_it->second;
      }
      for (const auto& [key, value] : controlled_result.error->details) {
        total_failure.details["controlled_stop_detail_" + key] = value;
      }
    }
    for (const auto& [diag_key, diag_value] :
         controlled_result.diagnostics) {
      if (diag_key.rfind("soft_on_path", 0) == 0 ||
          diag_key.rfind("fallback", 0) == 0 ||
          diag_key.rfind("hard_stop", 0) == 0) {
        total_failure.details[diag_key] = diag_value;
      }
    }
    if (fallback_legacy_result && fallback_legacy_result->error.has_value()) {
      total_failure.details["legacy_stop_error_code"] =
          fallback_legacy_result->error->code;
      const auto reason_it =
          fallback_legacy_result->error->details.find("reason");
      if (reason_it != fallback_legacy_result->error->details.end()) {
        total_failure.details["legacy_stop_error_reason"] =
            reason_it->second;
      }
      for (const auto& [key, value] :
           fallback_legacy_result->error->details) {
        total_failure.details["legacy_stop_detail_" + key] = value;
      }
    }
    {
      std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
      operator_stop_intervention_error_ = total_failure;
    }
    sm_.set_last_error(error_code_from_dto(total_failure));
    stop_collision_monitoring();
    cache_.invalidate_all();
    auto transition = sm_.try_transition(ExecutionState::kInterventionRequired);
    if (!transition.ok()) {
      return;
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    snapshot.error = total_failure;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                total_failure,
                total_failure.details});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                pre_completion_state,
                ExecutionState::kInterventionRequired,
                std::nullopt,
                {{"reason", "operator_stop_total_failure"}}});
    return;
  }

  auto snapshot = execution_backend_.get_execution_snapshot();
  snapshot.timestamp_ns = clock_.now();

  auto transition = sm_.try_transition(ExecutionState::kStopped);
  if (!transition.ok() && sm_.status().state != ExecutionState::kStopped) {
    ErrorDTO err{
        std::string(kErrorMotionInterruptBackendStopFailed),
        "operator controlled stop completed, but terminal authority was "
        "already claimed by another state before kStopped could be recorded",
        {{"reason", "operator_stop_terminal_transition_lost"},
         {"current_state", to_string(sm_.status().state)},
         {"requested_stop_mode", requested_mode},
         {"executed_stop_mode", executed_mode},
         {"phase", "operator_stop"}}};
    sm_.set_last_error(error_code_from_dto(err));
    auto prev = sm_.status().state;
    auto intervention =
        sm_.try_transition(ExecutionState::kInterventionRequired);
    if (intervention.ok()) {
      auto failed_snapshot = execution_backend_.get_execution_snapshot();
      failed_snapshot.timestamp_ns = clock_.now();
      failed_snapshot.state = sm_.status().state;
      failed_snapshot.error = err;
      publish_execution_snapshot(std::move(failed_snapshot));
      emit_event({KernelEventType::kErrorRaised,
                  sm_.status().execution_id,
                  clock_.now(),
                  {},
                  {},
                  err,
                  err.details});
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id,
                  clock_.now(),
                  prev,
                  ExecutionState::kInterventionRequired,
                  std::nullopt,
                  {{"reason", "operator_stop_terminal_transition_lost"}}});
    }
    return;
  }

  snapshot.state = sm_.status().state;
  if (!controlled_ok && fallback_legacy_result &&
      fallback_legacy_result->accepted) {
    ErrorDTO degraded_stop_error{
        std::string(kErrorMotionInterruptBackendStopFailed),
        stop_failure_message,
        {{"reason", stop_failure_reason},
         {"requested_stop_mode", requested_mode},
         {"executed_stop_mode", executed_mode},
         {"emergency_fallback", "legacy_stop_succeeded"},
         {"hard_stop_transitioned", "true"}}};
    add_hard_stop_floor_details(degraded_stop_error.details,
                                execution_backend_);
    for (const auto& [key, value] : settle_details) {
      degraded_stop_error.details[key] = value;
    }
    {
      std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
      operator_stop_terminal_error_ = degraded_stop_error;
    }
    snapshot.error = std::move(degraded_stop_error);
  }
  publish_execution_snapshot(std::move(snapshot));
  std::map<std::string, std::string> stop_issued_details;
  if (controlled_ok) {
    stop_issued_details = controlled_stop_success_event_details(
        profile, controlled_result, execution_backend_, "operator_stop");
  }
  emit_event({KernelEventType::kStopIssued, sm_.status().execution_id,
              clock_.now(), {}, {}, std::nullopt,
              std::move(stop_issued_details)});
  emit_event({KernelEventType::kStateTransition, sm_.status().execution_id,
              clock_.now(), pre_completion_state, sm_.status().state,
              std::nullopt, {}});
  std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
  metrics_.stops_issued++;
}

void ExecutionKernel::emit_event(const KernelEvent& event) {
  KernelEvent event_to_emit = event;
  if (event_to_emit.error.has_value()) {
    for (const auto& [key, value] : event_to_emit.error->details) {
      event_to_emit.details.emplace(key, value);
    }
  }

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
      callback(event_to_emit);
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
    InterruptPreemptionResult result,
    const std::map<std::string, std::string>& extra_details) {
  auto latch = interrupt_manager_.active_latch();
  if (!latch) {
    return;
  }
  const auto now = clock_.now();
  auto marked = interrupt_manager_.mark_active_preemption_result(
      result, now - latch->latched_at_ns, now);
  if (marked.ok()) {
    auto marked_result = marked.value();
    if (!extra_details.empty()) {
      for (auto& event : marked_result.events) {
        if (event.type == "interrupt_preempt_applied") {
          for (const auto& [key, value] : extra_details) {
            event.details[key] = value;
          }
        }
      }
    }
    emit_interrupt_events(marked_result);
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
    const CollisionCheckResultDTO& check,
    const std::map<std::string, std::string>& metadata) {
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
  for (const auto& [key, value] : metadata) {
    signal.metadata[key] = value;
  }
  return publish_fast_interrupt_signal(signal);
}

void ExecutionKernel::apply_latched_interrupt_preemption(
    const InterruptIngestResult& result) {
  if (!result.latched || !result.active_latch) {
    return;
  }
  const auto& latch = *result.active_latch;
  if (latch.preemption_action_requested !=
      InterruptPreemptionAction::kPreemptTrajectory) {
    return;
  }
  stop_collision_monitoring();
  cache_.invalidate_all();

  // Per spec/260 §2.2: when the active fault policy carries a
  // `stop_profile`, route the preempt through the same controlled-
  // stop machinery the desired-trip path uses
  // (`execute_controlled_stop` with the same
  // `ControlledStopResultDTO` surface). When no profile is set,
  // fall back to the legacy `execution_backend_.stop()`.
  std::optional<ControlledStopResultDTO> controlled_stop;
  std::optional<MotionInterruptStopProfileDTO> fault_stop_profile;
  // Per-policy stop_profile wins over the kernel-level default. If the
  // policy doesn't carry one but the deployment configured a kernel-
  // level `default_fault_stop_profile`, use that. Falls through to the
  // legacy `execution_backend_.stop()` only when both are unset.
  if (auto active = interrupt_manager_.active_policy();
      active.has_value() && active->stop_profile) {
    fault_stop_profile = *active->stop_profile;
  } else if (config_.default_fault_stop_profile) {
    fault_stop_profile = *config_.default_fault_stop_profile;
  }
  bool preempt_accepted = false;
  std::optional<ErrorDTO> preempt_error;
  std::map<std::string, std::string> preempt_event_details{
      {"phase", "preempt_trajectory"}};
  if (fault_stop_profile.has_value()) {
    controlled_stop =
        execution_backend_.execute_controlled_stop(*fault_stop_profile);
    const bool controlled_ok =
        controlled_stop->accepted &&
        controlled_stop->backend_stop_confirmed &&
        !controlled_stop->error.has_value();
    preempt_event_details["fault_stop_profile_id"] =
        fault_stop_profile->profile_id;
    preempt_event_details["requested_stop_mode"] =
        stop_mode_string(fault_stop_profile->stop_mode);
    preempt_event_details["executed_stop_mode"] =
        stop_mode_string(controlled_stop->executed_stop_mode);
    preempt_event_details["stop_command_source"] =
        stop_command_source_string(controlled_stop->stop_command_source);
    if (controlled_ok) {
      preempt_accepted = true;
      preempt_event_details = controlled_stop_success_event_details(
          *fault_stop_profile, *controlled_stop, execution_backend_,
          "fault_preempt");
      preempt_event_details["preempt_path"] = "controlled_stop";
      preempt_event_details["fault_stop_profile_id"] =
          fault_stop_profile->profile_id;
    } else {
      // Emergency fallback: industrial guarantee that a fault
      // preempt physically stops the robot even when the
      // configured stop_profile is unsupported, refused, times out,
      // or otherwise fails. Without this fallback a deployment with
      // a misconfigured fault profile (or a backend missing the
      // capability) would latch the fault but leave the controller
      // running. The controlled-stop failure is preserved in
      // diagnostics so the operator/audit trail sees the degradation;
      // the legacy stop result decides whether the preempt is
      // considered accepted.
      auto legacy_stop = execution_backend_.stop();
      preempt_accepted = legacy_stop.accepted;
      const std::string controlled_reason =
          controlled_stop->error.has_value() &&
                  controlled_stop->error->details.contains("reason")
              ? controlled_stop->error->details.at("reason")
              : std::string("fault_stop_profile_not_confirmed");
      preempt_error = legacy_stop.accepted
                          ? std::optional<ErrorDTO>(ErrorDTO{
                                std::string(
                                    kErrorMotionInterruptBackendStopFailed),
                                "fault stop profile failed; emergency "
                                "legacy backend.stop() guaranteed "
                                "physical stop",
                                {{"reason",
                                  "fault_stop_profile_failed_legacy_recovered"},
                                 {"controlled_stop_reason",
                                  controlled_reason},
                                 {"emergency_fallback",
                                  "legacy_stop_succeeded"}}})
                          : std::optional<ErrorDTO>(ErrorDTO{
                                std::string(
                                    kErrorMotionInterruptBackendStopFailed),
                                "fault stop failed: controlled-stop "
                                "refused and emergency legacy "
                                "backend.stop() also failed",
                                {{"reason", "fault_stop_total_failure"},
                                 {"controlled_stop_reason",
                                  controlled_reason}}});
      preempt_event_details["preempt_path"] =
          legacy_stop.accepted
              ? "controlled_stop_failed_legacy_fallback_succeeded"
              : "controlled_stop_failed_legacy_fallback_failed";
      preempt_event_details["emergency_fallback"] =
          legacy_stop.accepted ? "legacy_stop_succeeded"
                                : "legacy_stop_failed";
    }
  } else {
    auto stop = execution_backend_.stop();
    preempt_accepted = stop.accepted;
    preempt_error = stop.error;
    preempt_event_details["preempt_path"] = "legacy_backend_stop";
  }

  mark_active_interrupt_preemption_result(
      preempt_accepted ? InterruptPreemptionResult::kSucceeded
                       : InterruptPreemptionResult::kBackendPreemptFailed,
      preempt_event_details);
  if (!preempt_accepted && preempt_error) {
    sm_.set_last_error(error_code_from_dto(*preempt_error));
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = ExecutionState::kInterrupted;
    snapshot.error = preempt_error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                preempt_error,
                preempt_event_details});
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

void ExecutionKernel::emit_interrupt_manager_events(
    const std::vector<InterruptManagerEvent>& events,
    std::uint64_t execution_id) {
  for (const auto& event : events) {
    auto event_type = interrupt_event_type_from_name(event.type);
    if (!event_type) {
      continue;
    }
    emit_event({*event_type, execution_id, clock_.now(), {}, {},
                std::nullopt, event.details});
  }
}

namespace {

const char* terminal_cause_string(MotionExecutionTerminalCause cause) {
  switch (cause) {
    using enum MotionExecutionTerminalCause;
    case kCompletedNormally: return "completed_normally";
    case kSignalTrip: return "signal_trip";
    case kNoSignalTrip: return "no_signal_trip";
    case kStopped: return "stopped";
    case kFailed: return "failed";
    case kInterruptedFault: return "interrupted_fault";
  }
  return "unknown";
}

}  // namespace

Result<void> ExecutionKernel::arm_motion_scopes_for_dispatch(
    const ExecuteRequestDTO& request,
    std::uint64_t execution_id,
    ActiveMotionExecution& motion_state) {
  motion_state.execution_id = execution_id;
  motion_state.move_id = request.move_id;
  motion_state.had_scopes = !request.motion_interrupt_scopes.empty();
  motion_state.arm_succeeded = false;
  motion_state.motion_trip_latch.reset();
  motion_state.outcome_emitted = false;
  const bool has_configured_stop_profiles =
      config_.operator_stop_profile || config_.default_fault_stop_profile;
  if (!motion_state.had_scopes && !has_configured_stop_profiles) {
    return Result<void>::Ok();
  }
  // Spec/260 §5.6 dual-boundary capability validation:
  // pre-dispatch backend-capability check. The InterruptManager
  // validates policy-level constraints (allowed_stop_modes,
  // node-category fallback rules, stop_profile presence). The
  // backend's controlled-stop capability is the second independent
  // gate: every requested stop_mode and every element of
  // allowed_fallback_modes MUST also be in the backend's
  // supported_stop_modes set. Trip-time validation catches
  // live-controller divergence; this gate stops the request
  // before arming if the backend has not declared support.
  const auto capability =
      execution_backend_.get_motion_interrupt_stop_capability();
  const auto validate_profile_against_capability =
      [&capability](const MotionInterruptStopProfileDTO& profile,
                    const std::string& profile_slot,
                    const std::string& scope_id,
                    const std::string& policy_id)
          -> std::optional<ErrorDTO> {
    if (!capability.supported_stop_modes.contains(profile.stop_mode)) {
      std::map<std::string, std::string> details{
          {"reason", "stop_mode_not_supported_by_backend"},
          {"profile_slot", profile_slot},
          {"requested_stop_mode", stop_mode_string(profile.stop_mode)}};
      if (!scope_id.empty()) {
        details["scope_id"] = scope_id;
      }
      if (!policy_id.empty()) {
        details["policy_id"] = policy_id;
      }
      return ErrorDTO{
          kErrorMotionInterruptArmFailed,
          "configured stop_profile stop_mode is not in backend supported_stop_modes",
          std::move(details)};
    }
    for (const auto& fallback : profile.allowed_fallback_modes) {
      if (!capability.supported_stop_modes.contains(fallback)) {
        std::map<std::string, std::string> details{
            {"reason", "fallback_mode_not_supported_by_backend"},
            {"profile_slot", profile_slot},
            {"requested_stop_mode", stop_mode_string(profile.stop_mode)},
            {"fallback_stop_mode", stop_mode_string(fallback)}};
        if (!scope_id.empty()) {
          details["scope_id"] = scope_id;
        }
        if (!policy_id.empty()) {
          details["policy_id"] = policy_id;
        }
        return ErrorDTO{
            kErrorMotionInterruptArmFailed,
            "configured stop_profile fallback is not in backend supported_stop_modes",
            std::move(details)};
      }
    }
    return std::nullopt;
  };
  if (config_.operator_stop_profile) {
    if (auto error = validate_profile_against_capability(
            *config_.operator_stop_profile, "operator_stop_profile", "", "")) {
      return Result<void>::Err(*error);
    }
  }
  if (config_.default_fault_stop_profile) {
    if (auto error = validate_profile_against_capability(
            *config_.default_fault_stop_profile, "default_fault_stop_profile",
            "", "")) {
      return Result<void>::Err(*error);
    }
  }
  if (!motion_state.had_scopes) {
    return Result<void>::Ok();
  }
  for (const auto& scope : request.motion_interrupt_scopes) {
    if (!scope.stop_profile) {
      // Manager will reject this with stop_profile_required if the
      // bound policy is complete_move_on_signal; otherwise the
      // backend gate has nothing to check.
      continue;
    }
    const auto& profile = *scope.stop_profile;
    if (auto error = validate_profile_against_capability(
            profile, "motion_interrupt_scope", scope.scope_id,
            scope.policy_id)) {
      return Result<void>::Err(*error);
    }
    // Arm-time soft_on_path feasibility pre-check removed: the feasibility +
    // fallback disposition (hard_stop / spring_back / natural completion) is
    // decided at TRIP time by plan_soft_on_path_stop, which both backends now
    // honor. Arm time keeps only the backend-capability gate (above) + profile
    // well-formedness; per-emitted-trajectory accel/jerk consistency is checked
    // when the stop trajectory is generated, not pre-proven against every trip
    // point here.
  }
  auto arm = interrupt_manager_.arm_motion_interrupt_scopes(
      execution_id, request.move_id,
      request.motion_interrupt_scopes, clock_.now());
  if (!arm.ok()) {
    // Promote the manager's `details["reason"]` (e.g.
    // `motion_interrupt_arm_failed`, `motion_interrupt_source_active_at_arm`)
    // to the canonical ErrorDTO.code. The manager wraps every error as
    // `invalid_request` for legacy callers, but spec/210 §5.2 treats the
    // motion-interrupt identifiers as first-class error codes. Without
    // this promotion, `error_code_from_dto` collapses every arm failure
    // to `kInvalidRequest` and the state-machine surface loses fidelity.
    ErrorDTO err = arm.error();
    if (err.details.contains("reason")) {
      const auto& reason = err.details.at("reason");
      if (reason.rfind("motion_interrupt_", 0) == 0) {
        err.code = reason;
      }
    }
    emit_event({KernelEventType::kMotionInterruptArmFailed,
                execution_id, clock_.now(), {}, {}, err,
                {{"move_id", request.move_id},
                 {"reason",
                  err.details.contains("reason")
                      ? err.details.at("reason")
                      : err.code}}});
    return Result<void>::Err(std::move(err));
  }
  // Surface scope_armed events; the manager has already validated each
  // binding atomically, so we just relay.
  emit_interrupt_manager_events(arm.value().events, execution_id);
  motion_state.arm_succeeded = true;
  return Result<void>::Ok();
}

void ExecutionKernel::finalize_motion_execution_outcome(
    ActiveMotionExecution& motion_state,
    MotionExecutionTerminalCause terminal_cause,
    std::optional<ErrorDTO> error,
    std::optional<MotionTripOutcomeDTO> trip,
    std::string reap_reason) {
  if (motion_state.outcome_emitted || motion_state.execution_id == 0) {
    return;
  }
  motion_state.outcome_emitted = true;

  // Reap any armed bindings for this execution before publishing the
  // outcome. Reaping always runs even if no scopes were carried —
  // cheap when the binding map is empty and keeps the invariant that
  // every execution_id terminates with a clean binding table.
  if (motion_state.arm_succeeded) {
    auto reap = interrupt_manager_.reap_motion_interrupt_scopes(
        motion_state.execution_id, reap_reason, clock_.now());
    emit_interrupt_manager_events(reap.events, motion_state.execution_id);
  }

  // Invariant enforcement (spec/210 §3.11). The kernel publishes one
  // immutable outcome per terminal execution; mis-shaped requests are
  // coerced to the closest valid taxonomy AND logged via a kErrorRaised
  // event so the operator surface sees the coercion in audit.
  MotionExecutionOutcomeDTO outcome;
  outcome.execution_id = motion_state.execution_id;
  outcome.move_id = motion_state.move_id;
  outcome.terminal_cause = terminal_cause;

  const auto coerce = [&](MotionExecutionTerminalCause to,
                           const char* reason) {
    emit_event({KernelEventType::kErrorRaised,
                motion_state.execution_id, clock_.now(), {}, {},
                ErrorDTO{kErrorInvariantViolation,
                         "motion outcome coerced", {{"reason", reason}}},
                {{"from", terminal_cause_string(outcome.terminal_cause)},
                 {"to", terminal_cause_string(to)}}});
    outcome.terminal_cause = to;
  };

  switch (outcome.terminal_cause) {
    using enum MotionExecutionTerminalCause;
    case kSignalTrip:
      if (!trip || error) {
        coerce(kFailed, "signal_trip_requires_trip_and_forbids_error");
      }
      break;
    case kNoSignalTrip:
      if (!motion_state.had_scopes) {
        coerce(kCompletedNormally, "no_signal_trip_requires_scopes");
      }
      if (trip || error) {
        coerce(kFailed, "no_signal_trip_forbids_trip_and_error");
      }
      break;
    case kCompletedNormally:
      if (motion_state.had_scopes) {
        // Reached endpoint with scopes but no trip → no_signal_trip.
        outcome.terminal_cause = kNoSignalTrip;
      }
      if (trip || error) {
        coerce(kFailed, "completed_normally_forbids_trip_and_error");
      }
      break;
    case kStopped:
    case kInterruptedFault:
      if (trip || error) {
        coerce(kFailed, "terminal_cause_forbids_trip_and_error");
      }
      break;
    case kFailed:
      if (!error) {
        error = ErrorDTO{kErrorInternalError,
                         "Motion execution failed without a captured error",
                         {{"reason", "missing_error_on_failed_outcome"}}};
      }
      break;
  }

  if (outcome.terminal_cause == MotionExecutionTerminalCause::kSignalTrip) {
    outcome.trip = std::move(trip);
  } else if (outcome.terminal_cause == MotionExecutionTerminalCause::kFailed) {
    outcome.error = std::move(error);
  }

  // Store and evict FIFO when over the retention bound.
  {
    std::lock_guard<std::mutex> lock(motion_outcomes_mutex_);
    motion_execution_outcomes_.insert_or_assign(
        outcome.execution_id, outcome);
    motion_outcome_order_.push_back(outcome.execution_id);
    while (motion_outcome_order_.size() >
           config_.motion_execution_outcome_retention) {
      const auto evict_id = motion_outcome_order_.front();
      motion_outcome_order_.erase(motion_outcome_order_.begin());
      motion_execution_outcomes_.erase(evict_id);
    }
  }

  std::map<std::string, std::string> details{
      {"execution_id", std::to_string(outcome.execution_id)},
      {"move_id", outcome.move_id},
      {"terminal_cause", terminal_cause_string(outcome.terminal_cause)},
      {"had_scopes", motion_state.had_scopes ? "true" : "false"},
      {"arm_succeeded", motion_state.arm_succeeded ? "true" : "false"},
  };
  if (outcome.trip) {
    details.emplace("latch_id", outcome.trip->latch_id);
    details.emplace("source_id", outcome.trip->source_id);
    details.emplace("policy_id", outcome.trip->policy_id);
    details.emplace("scope_id", outcome.trip->scope_id);
  }
  if (outcome.error) {
    details.emplace("error_code", outcome.error->code);
    if (outcome.error->details.contains("reason")) {
      details.emplace("error_reason", outcome.error->details.at("reason"));
    }
  }
  emit_event({KernelEventType::kMotionExecutionOutcomeRecorded,
              outcome.execution_id, clock_.now(), {}, {}, outcome.error,
              std::move(details)});
}

namespace {

const char* stop_mode_string(MotionInterruptStopMode mode) {
  switch (mode) {
    using enum MotionInterruptStopMode;
    case kSpringBackHold: return "spring_back_hold";
    case kSoftOnPath: return "soft_on_path";
    case kHardDeceleration: return "hard_deceleration";
  }
  return "unknown";
}

const char* stop_command_source_string(MotionInterruptStopCommandSource s) {
  switch (s) {
    using enum MotionInterruptStopCommandSource;
    case kFjtPreemptHold: return "fjt_preempt_hold";
    case kFjtPreemptPathRamp: return "fjt_preempt_path_ramp";
    case kJtcSpeedScalingRamp: return "jtc_speed_scaling_ramp";
    case kJtcCancelDecelerateOnCancel: return "jtc_cancel_decelerate_on_cancel";
    case kJtcCancelUnprofiledHold: return "jtc_cancel_unprofiled_hold";
    case kNativeSimulatedStop: return "native_simulated_stop";
    case kNativeSimulatedPathRamp: return "native_simulated_path_ramp";
    case kRobotNativeStop: return "robot_native_stop";
    case kDiagnosticOther: return "diagnostic_other";
  }
  return "unknown";
}

struct StopFallbackQualificationRow {
  MotionInterruptStopMode requested;
  MotionInterruptStopMode executed;
  MotionInterruptNodeCategory node_category;
  std::array<MotionInterruptStopCommandSource, 5> allowed_command_sources;
  std::size_t allowed_command_source_count;
  const char* qualification_id;
  const char* rationale;
  const char* telemetry;
  const char* evidence_status;
};

constexpr std::array<StopFallbackQualificationRow, 1>
    kStopFallbackQualificationMatrix{{
        {MotionInterruptStopMode::kSoftOnPath,
         MotionInterruptStopMode::kSpringBackHold,
         MotionInterruptNodeCategory::kGeneric,
         {MotionInterruptStopCommandSource::kDiagnosticOther,
          MotionInterruptStopCommandSource::kFjtPreemptHold,
          MotionInterruptStopCommandSource::kNativeSimulatedStop,
          MotionInterruptStopCommandSource::kFjtPreemptPathRamp,
          MotionInterruptStopCommandSource::kNativeSimulatedPathRamp},
         5,
         "4.5G-soft_on_path-to-spring_back_hold-generic-v1",
         "path-preserving soft stop unavailable; generic node accepts the qualified spring-back stop",
         "fallback_reason, requested/executed modes, command source, stop snapshot, stop timing",
         "in_tree_qualified; live_adapter_evidence_required_for_release"},
    }};

const StopFallbackQualificationRow* qualified_stop_fallback_pair(
    MotionInterruptStopMode requested,
    MotionInterruptStopMode executed,
    MotionInterruptNodeCategory node_category) {
  for (const auto& row : kStopFallbackQualificationMatrix) {
    if (row.requested == requested && row.executed == executed &&
        row.node_category == node_category) {
      return &row;
    }
  }
  return nullptr;
}

bool fallback_row_allows_command_source(
    const StopFallbackQualificationRow& row,
    MotionInterruptStopCommandSource source) {
  for (std::size_t i = 0; i < row.allowed_command_source_count; ++i) {
    if (row.allowed_command_sources[i] == source) {
      return true;
    }
  }
  return false;
}

/// Spec/210 §3.7 RobotStateDTO size/ordering invariant: joint_names and
/// positions_rad MUST have the same non-zero length and no empty
/// joint names. Used as the spec/210 §3.11 ExecutionStopSnapshotDTO
/// validation gate.
bool robot_state_size_ordering_ok(const RobotStateDTO& state) {
  if (state.joint_names.empty()) {
    return false;
  }
  if (state.joint_names.size() != state.positions_rad.size()) {
    return false;
  }
  for (const auto& name : state.joint_names) {
    if (name.empty()) {
      return false;
    }
  }
  return true;
}

/// Spec/210 §3.11 ExecutionStopSnapshotDTO source-priority capture.
/// The post-stop snapshot is fetched FRESH after the backend confirms
/// the controlled stop and
/// `joint_names` are recovered from the planning context so spec/210
/// §3.7 size/ordering validation can actually run.
///
/// Returns the snapshot if validation succeeds; populates
/// `failure_reason` when the snapshot is invalid for the node category
/// (e.g. `planning_context_state` source for a precision-contact node,
/// or invalid `robot_state`).
std::optional<ExecutionStopSnapshotDTO> capture_stop_snapshot(
    const ExecutionSnapshot& post_stop_backend_snapshot,
    const std::optional<RobotStateDTO>& planning_context_state,
    bool backend_stop_confirmed,
    MotionInterruptNodeCategory node_category,
    std::int64_t captured_at_ns,
    double settle_velocity_threshold_rad_s,
    std::string& failure_reason) {
  ExecutionStopSnapshotDTO snap;
  snap.captured_at_ns = captured_at_ns;
  snap.backend_stop_confirmed = backend_stop_confirmed;
  const bool command_authoritative =
      is_command_authoritative_stop_snapshot(post_stop_backend_snapshot);

  // Settle signal: derive max |velocity| and the at-rest verdict from whatever
  // measured velocities the chosen state source carries. Absent velocities
  // leave both fields unset (settle UNKNOWN) — never silently "at rest".
  const auto apply_settle_signal = [&](ExecutionStopSnapshotDTO& s) {
    if (s.robot_state.velocities_rad_s.empty() ||
        s.robot_state.velocities_rad_s.size() !=
            s.robot_state.positions_rad.size()) {
      return;
    }
    double max_abs = 0.0;
    for (double v : s.robot_state.velocities_rad_s) {
      max_abs = std::max(max_abs, std::abs(v));
    }
    s.max_abs_velocity_rad_s = max_abs;
    s.at_rest = command_authoritative ||
                max_abs <= settle_velocity_threshold_rad_s;
  };

  // Priority 1: execution-backend feedback (preferred). Joint names
  // come from the planning context (the IExecutionBackend's snapshot
  // DTO does not carry them); we MUST be able to align name-by-name
  // for spec/210 §3.7 validation.
  if (!post_stop_backend_snapshot.joint_positions_rad.empty() &&
      planning_context_state &&
      planning_context_state->joint_names.size() ==
          post_stop_backend_snapshot.joint_positions_rad.size()) {
    snap.source = ExecutionStopSnapshotSource::kExecutionBackendFeedback;
    snap.robot_state.joint_names = planning_context_state->joint_names;
    snap.robot_state.positions_rad =
        post_stop_backend_snapshot.joint_positions_rad;
    if (post_stop_backend_snapshot.joint_velocities_rad_s.size() ==
        snap.robot_state.positions_rad.size()) {
      snap.robot_state.velocities_rad_s =
          post_stop_backend_snapshot.joint_velocities_rad_s;
    }
    if (post_stop_backend_snapshot.progress > 0.0 &&
        post_stop_backend_snapshot.progress <= 1.0) {
      snap.trajectory_progress = post_stop_backend_snapshot.progress;
    }
    if (!robot_state_size_ordering_ok(snap.robot_state)) {
      failure_reason = "stop_snapshot_robot_state_size_ordering_invalid";
      return std::nullopt;
    }
    apply_settle_signal(snap);
    return snap;
  }

  // Priority 2: kernel execution snapshot. Same source path as
  // Priority 1 — the worker's `get_execution_snapshot()` proxies the
  // backend's snapshot — but if the backend snapshot was unusable we
  // fall through to the planning-context fallback rather than
  // synthesizing a partial RobotStateDTO.

  // Priority 3: planning-context state is rejected for precision-
  // contact node categories (spec/210 §3.11 ExecutionStopSnapshotDTO
  // semantic rules). Generic nodes may accept it as a last resort
  // when the backend snapshot is unusable.
  if (node_category == MotionInterruptNodeCategory::kPrecisionContact) {
    failure_reason = "stop_snapshot_planning_context_rejected_for_precision_contact";
    return std::nullopt;
  }
  if (planning_context_state) {
    snap.source = ExecutionStopSnapshotSource::kPlanningContextState;
    snap.robot_state = *planning_context_state;
    if (!robot_state_size_ordering_ok(snap.robot_state)) {
      failure_reason = "stop_snapshot_robot_state_size_ordering_invalid";
      return std::nullopt;
    }
    apply_settle_signal(snap);
    return snap;
  }
  failure_reason = "stop_snapshot_no_usable_state_available";
  return std::nullopt;
}

}  // namespace

void ExecutionKernel::handle_motion_trip_latch(
    const ExecuteRequestDTO& request,
    const InterruptLatch& latch,
    std::uint64_t exec_id,
    const ExecutionSnapshot& /*latest_backend_snapshot*/,
    ActiveMotionExecution& motion_state) {
  // Locate the originating scope so we can recover the
  // MotionInterruptStopProfileDTO and node_category. The latch carries
  // the scope id; the request carries the scope vector. A trip latch
  // without a matching scope is a manager invariant violation and is
  // treated as a backend stop failure (intervention path: the robot
  // state is uncertain).
  motion_state.motion_trip_latch = latch;
  stop_collision_monitoring();
  const MotionInterruptScopeDTO* matching_scope = nullptr;
  if (latch.motion_interrupt_scope_id) {
    for (const auto& s : request.motion_interrupt_scopes) {
      if (s.scope_id == *latch.motion_interrupt_scope_id) {
        matching_scope = &s;
        break;
      }
    }
  }
  if (!matching_scope || !matching_scope->stop_profile) {
    const auto prev_state_trip = sm_.status().state;
    sm_.set_last_error(ErrorCode::kMotionInterruptBackendStopFailed);
    if (prev_state_trip == ExecutionState::kInterrupted) {
      sm_.try_transition(ExecutionState::kStopping);
    }
    sm_.try_transition(ExecutionState::kInterventionRequired);
    ErrorDTO err{
        kErrorMotionInterruptBackendStopFailed,
        "complete_move_on_signal latch missing matching scope/stop_profile",
        {{"reason", "motion_interrupt_scope_lookup_failed"},
         {"latch_id", latch.latch_id}}};
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                 {}, {}, err, {}});
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                 prev_state_trip, ExecutionState::kInterventionRequired,
                 std::nullopt, {}});
    finalize_motion_execution_outcome(
        motion_state, MotionExecutionTerminalCause::kFailed,
        std::move(err), std::nullopt, "motion_trip_scope_lookup_failed");
    // Latch persists for operator intervention-acknowledge; do NOT
    // auto-clear on this failure path.
    return;
  }
  const auto& profile = *matching_scope->stop_profile;
  const auto node_category = matching_scope->node_category;

  // Move kernel to Stopping while the controlled stop is in flight.
  // The latch remains visible; spec/230 §3.4 forbids parking in
  // Interrupted during the controlled-stop window.
  const auto prev_state_trip = sm_.status().state;
  if (prev_state_trip == ExecutionState::kInterrupted) {
    auto t = sm_.try_transition(ExecutionState::kStopping);
    if (t.ok()) {
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state_trip, ExecutionState::kStopping,
                   std::nullopt,
                   {{"reason", "motion_interrupt_trip_controlled_stop"}}});
    }
  }

  // Issue the controlled stop. The backend performs the stop and
  // reports what it actually did. Timing is captured around the call
  // so the kernel can validate against the effective stop deadline:
  // explicit `max_stop_time_s`, or `deceleration_time_s` (defaulted)
  // plus the deployment stop-confirmation tolerance when unset. Embed
  // latch->call and call->confirm latencies in the trip outcome.
  // Keep the runtime collision monitor running over the decelerating robot for
  // the duration of the stop. A collision on the stop path signals the trip
  // handler (via the stop token) to escalate to a hard stop. The monitor is
  // observed-state based, so this is the same monitoring a normal motion gets;
  // the only difference is the stop trajectory is not pre-validated (it stays
  // on the already-validated path).
  const std::int64_t effective_deadline_ns =
      effective_controlled_stop_deadline_ns(
          profile, config_.complete_move_on_signal_timeout_ns);

  std::stop_source trip_stop_source;
  {
    std::lock_guard<std::mutex> lk(motion_trip_stop_mutex_);
    motion_trip_stop_source_ = trip_stop_source;
    motion_trip_stop_collision_.store(false, std::memory_order_release);
  }
  motion_trip_stop_active_.store(true, std::memory_order_release);
  start_collision_monitoring();
  begin_runtime_velocity_scale_freeze();
  const auto runtime_scale_freeze_scope =
      std::shared_ptr<void>(nullptr, [this](void*) {
        end_runtime_velocity_scale_freeze();
      });
  auto hard_stop_floor_attempt = std::make_shared<HardStopFloorAttempt>();
  auto deadline_watchdog_fired = std::make_shared<std::atomic_bool>(false);

  // Kernel-enforced stop deadline. If the backend blocks past the effective
  // deadline, the Monitoring lane requests cancellation AND issues the
  // hard-stop floor.
  // The worker still owns final state/outcome publication when the backend
  // returns, but the physical hard-stop command is not delayed by that return.
  ScheduleHandle stop_deadline_handle = 0;
  if (effective_deadline_ns > 0) {
    const auto requested_mode = std::string(stop_mode_string(profile.stop_mode));
    const auto latch_id = latch.latch_id;
    const auto source_id = latch.source_id;
    const auto policy_id = latch.policy_id;
    const auto scope_id = latch.motion_interrupt_scope_id.value_or("");
    stop_deadline_handle = scheduler_.schedule(
        clock_.now() + effective_deadline_ns,
        [this, hard_stop_floor_attempt, requested_mode, latch_id, source_id,
         policy_id, scope_id, deadline_watchdog_fired]() {
          deadline_watchdog_fired->store(true, std::memory_order_release);
          const auto legacy =
              request_hard_stop_floor_once(execution_backend_,
                                           hard_stop_floor_attempt);
          {
            std::lock_guard<std::mutex> lk(motion_trip_stop_mutex_);
            if (motion_trip_stop_source_.has_value()) {
              motion_trip_stop_source_->request_stop();
            }
          }
          const auto fallback_outcome =
              legacy.accepted ? std::string("legacy_stop_succeeded")
                              : std::string("legacy_stop_failed");
          std::map<std::string, std::string> details{
              {"reason", "in_stop_completion_timeout"},
              {"phase", "motion_trip_deadline_watchdog"},
              {"requested_stop_mode", requested_mode},
              {"latch_id", latch_id},
              {"source_id", source_id},
              {"policy_id", policy_id},
              {"scope_id", scope_id},
              {"emergency_fallback", fallback_outcome},
              {"hard_stop_transitioned", "true"}};
          emit_event({KernelEventType::kErrorRaised,
                      sm_.status().execution_id,
                      clock_.now(),
                      {},
                      {},
                      ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                               "motion-scoped controlled stop exceeded "
                               "effective stop deadline; emergency hard-stop "
                               "floor issued by deadline watchdog",
                               details},
                      details});
        },
        SchedulerPriority::Monitoring);
  }

  const auto call_start_ns = clock_.now();
  const auto stop_wall_start = std::chrono::steady_clock::now();
  const auto stop_wall_deadline =
      effective_deadline_ns > 0
          ? stop_wall_start + std::chrono::nanoseconds(effective_deadline_ns)
          : stop_wall_start;
  auto stop_result = execution_backend_.execute_controlled_stop(
      profile, trip_stop_source.get_token());
  const auto call_end_ns = clock_.now();
  const bool deadline_watchdog_expired =
      deadline_watchdog_fired->load(std::memory_order_acquire);
  if (deadline_watchdog_expired) {
    stop_result.backend_stop_confirmed = false;
    stop_result.error = ErrorDTO{
        std::string(kErrorMotionInterruptBackendStopFailed),
        "motion-scoped controlled stop exceeded the effective stop deadline; "
        "emergency hard-stop floor was issued by the deadline watchdog",
        {{"reason", "in_stop_completion_timeout"},
         {"phase", "motion_trip_deadline_watchdog"}}};
  }

  if (stop_deadline_handle != 0) {
    scheduler_.cancel(stop_deadline_handle);
  }
  motion_trip_stop_active_.store(false, std::memory_order_release);
  stop_collision_monitoring();
  bool stop_path_collision = false;
  {
    std::lock_guard<std::mutex> lk(motion_trip_stop_mutex_);
    stop_path_collision =
        motion_trip_stop_collision_.load(std::memory_order_acquire);
    motion_trip_stop_source_.reset();
  }

  // Common failure-path downgrader. Industrial behavior (spec/230
  // §3.4 lines 563-564): backend-stop and stop-snapshot failures
  // transition to `InterventionRequired`, not `Failed`. The latch
  // persists for operator/runtime intervention-acknowledge; it is
  // NOT auto-cleared on the failure paths. The outcome record's
  // `terminal_cause` is still `failed` per spec/210 §3.11.
  const auto downgrade_to_intervention =
      [&](std::string reason, std::string message,
          std::string error_code =
              std::string(kErrorMotionInterruptBackendStopFailed),
          std::map<std::string, std::string> extra_details = {}) {
        sm_.set_last_error(
            error_code == kErrorMotionInterruptStopStateInvalid
                ? ErrorCode::kMotionInterruptStopStateInvalid
                : ErrorCode::kMotionInterruptBackendStopFailed);
        const auto prev = sm_.status().state;
        sm_.try_transition(ExecutionState::kInterventionRequired);
        ErrorDTO err{
            std::move(error_code), std::move(message),
            {{"reason", std::move(reason)},
             {"latch_id", latch.latch_id},
             {"source_id", latch.source_id},
             {"policy_id", latch.policy_id},
             {"requested_stop_mode", stop_mode_string(profile.stop_mode)},
             {"executed_stop_mode",
              stop_mode_string(stop_result.executed_stop_mode)}}};
        // Escalation evidence (e.g. hard_stop_transitioned) must ride the
        // terminal outcome, not only the transient event stream, so industrial
        // audit can reconstruct the disposition from the durable record alone.
        for (auto& [k, v] : extra_details) {
          err.details[k] = std::move(v);
        }
        emit_event({KernelEventType::kMotionTripFailed, exec_id,
                     clock_.now(), {}, {}, err, err.details});
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kInterventionRequired,
                     std::nullopt, {}});
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kFailed,
            std::move(err), std::nullopt, "motion_trip_failed");
        // Latch persists. Do NOT auto-clear.
      };

  // Build a fully self-describing escalation detail map for the scoped
  // motion-trip failure/escalation paths. Carries the controlled-stop
  // error code/reason and the whole soft_on_path/fallback diagnostic
  // surface so escalation events on the event stream describe WHY the
  // requested stop was refused or escalated — not only the fact that an
  // escalation happened. Mirrors the operator-stop worker enrichment.
  const auto build_escalation_details =
      [&](std::string reason, std::string phase) {
        std::map<std::string, std::string> details{
            {"reason", std::move(reason)},
            {"phase", std::move(phase)},
            {"requested_stop_mode", stop_mode_string(profile.stop_mode)},
            {"executed_stop_mode",
             stop_mode_string(stop_result.executed_stop_mode)},
            {"latch_id", latch.latch_id},
            {"source_id", latch.source_id},
            {"policy_id", latch.policy_id},
            {"scope_id", latch.motion_interrupt_scope_id.value_or("")}};
        if (stop_result.error.has_value()) {
          details["controlled_stop_error_code"] = stop_result.error->code;
          const auto reason_it = stop_result.error->details.find("reason");
          if (reason_it != stop_result.error->details.end()) {
            details["controlled_stop_error_reason"] = reason_it->second;
          }
        }
        for (const auto& [k, v] : stop_result.diagnostics) {
          if (k.rfind("soft_on_path", 0) == 0 ||
              k.rfind("fallback", 0) == 0 ||
              k.rfind("hard_stop", 0) == 0) {
            details[k] = v;
          }
        }
        return details;
      };

  // First-class hard-stop floor for the scoped motion-trip path. The legacy
  // `backend.stop()` IS the hard stop the escalation ladder calls for when a
  // controlled stop is refused, not confirmed within the deadline, or did not
  // reach measured rest. The escalation is marked `hard_stop_transitioned` so
  // the monitor/audit stream sees it as a hard-stop transition, not a generic
  // failure. Returns the durable escalation evidence (hard_stop_transitioned,
  // emergency_fallback) so the caller can fold it into the terminal outcome.
  const auto emergency_stop_floor =
      [&](std::string reason,
          std::string phase) -> std::map<std::string, std::string> {
    const auto legacy = request_hard_stop_floor_once(
        execution_backend_, hard_stop_floor_attempt);
    auto details =
        build_escalation_details(std::move(reason), std::move(phase));
    details["emergency_fallback"] =
        legacy.accepted ? "legacy_stop_succeeded" : "legacy_stop_failed";
    details["hard_stop_transitioned"] = "true";
    add_hard_stop_floor_details(details, execution_backend_);
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {},
                 std::optional<ErrorDTO>{ErrorDTO{
                     std::string(kErrorMotionInterruptBackendStopFailed),
                     "scoped motion-trip controlled stop escalated to the "
                     "emergency hard-stop floor",
                     details}},
                 details});
    std::map<std::string, std::string> evidence{
            {"hard_stop_transitioned", "true"},
            {"emergency_fallback", details["emergency_fallback"]}};
    evidence["hard_stop_level"] = details["hard_stop_level"];
    evidence["hard_stop_floor"] = details["hard_stop_floor"];
    evidence["hard_stop_floor_command_source"] =
        details["hard_stop_floor_command_source"];
    return evidence;
  };

  // Stop-path collision escalation (checked first). A dynamic obstacle entered
  // the stop path during the controlled stop, so the runtime monitor cancelled
  // the soft stop mid-ramp — the robot may still be moving toward the obstacle.
  // Escalate to the hard-stop floor (first-class) and hold for operator
  // acknowledgment.
  if (stop_path_collision) {
    auto floor_evidence = emergency_stop_floor("stop_path_collision",
                                                "motion_trip_stop_path_collision");
    downgrade_to_intervention(
        "stop_path_collision",
        "collision detected on the stop path during the controlled stop; "
        "escalated to a hard stop",
        std::string(kErrorMotionInterruptBackendStopFailed),
        std::move(floor_evidence));
    return;
  }

  // Short-path policy: a generic soft_on_path scope may
  // explicitly allow the original trajectory to complete naturally
  // when a useful wall-time replacement ramp would overshoot the
  // accepted endpoint. This is NOT a successful controlled stop; the
  // terminal outcome remains no_signal_trip semantics after the
  // backend proves the original motion is terminal. Precision-contact
  // nodes and profiles without the explicit policy fail closed.
  if (stop_result.completed_by_natural_completion) {
    if (profile.stop_mode != MotionInterruptStopMode::kSoftOnPath ||
        !profile.allow_natural_completion_on_short_path ||
        node_category == MotionInterruptNodeCategory::kPrecisionContact ||
        !stop_result.accepted || stop_result.error.has_value()) {
      downgrade_to_intervention(
          "natural_completion_not_allowed_for_scope",
          "backend reported natural completion for a scope that cannot "
          "accept short-path natural completion");
      return;
    }
    if (effective_deadline_ns > 0 &&
        stop_result.confirmation_latency_ns > effective_deadline_ns) {
      downgrade_to_intervention(
          "natural_completion_exceeded_max_stop_time_s",
          "soft_on_path natural completion exceeded the effective stop deadline");
      return;
    }

    auto completion_transition = sm_.try_transition(ExecutionState::kCompleted);
    if (!completion_transition.ok()) {
      downgrade_to_intervention(
          "natural_completion_completed_transition_rejected",
          "soft_on_path natural completion could not commit Completed as "
          "the terminal authority");
      return;
    }
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                ExecutionState::kStopping, ExecutionState::kCompleted,
                std::nullopt,
                {{"reason", "motion_trip_short_path_natural_completion"},
                 {"latch_id", latch.latch_id},
                 {"scope_id", latch.motion_interrupt_scope_id.value_or("")},
                 {"requested_stop_mode", stop_mode_string(profile.stop_mode)}}});
    finalize_motion_execution_outcome(
        motion_state, MotionExecutionTerminalCause::kNoSignalTrip,
        std::nullopt, std::nullopt, "motion_trip_natural_completion");
    (void)interrupt_manager_.clear_active_latch(
        "auto_clear_after_motion_outcome", clock_.now());
    return;
  }

  // (1) Backend accepted the call. A refused controlled stop means the
  //     robot may still be in motion, so engage the emergency hard-stop
  //     floor before downgrading to intervention.
  if (!stop_result.accepted) {
    auto reason = stop_result.error && stop_result.error->details.contains(
                                            "reason")
                      ? stop_result.error->details.at("reason")
                      : std::string("controlled_stop_not_accepted");
    auto msg = stop_result.error ? stop_result.error->message
                                  : "execute_controlled_stop returned accepted=false";
    auto floor_evidence = emergency_stop_floor(
        "scoped_trip_controlled_stop_refused",
        "motion_trip_controlled_stop_refused");
    downgrade_to_intervention(std::move(reason), std::move(msg),
                              std::string(kErrorMotionInterruptBackendStopFailed),
                              std::move(floor_evidence));
    return;
  }
  // (2) Backend confirmed the stop within the effective stop deadline. An
  //     unconfirmed stop means the robot may still be in motion: engage the
  //     emergency hard-stop floor before downgrading.
  if (!stop_result.backend_stop_confirmed) {
    // In-stop completion timeout: the controlled stop did not reach confirmed
    // rest within max_stop_time_s. First-class escalation to the hard-stop
    // floor (the robot may still be moving), then intervention.
    auto floor_evidence = emergency_stop_floor(
        "in_stop_completion_timeout",
        "motion_trip_controlled_stop_unconfirmed");
    downgrade_to_intervention(
        "backend_stop_not_confirmed",
        "backend did not confirm controlled stop within the effective stop "
        "deadline",
        std::string(kErrorMotionInterruptBackendStopFailed),
        std::move(floor_evidence));
    return;
  }
  if (effective_deadline_ns > 0 &&
      stop_result.confirmation_latency_ns > effective_deadline_ns) {
    downgrade_to_intervention(
        "backend_stop_confirmation_exceeded_max_stop_time_s",
        "controlled-stop confirmation exceeded the effective stop deadline");
    return;
  }
  const double settle_velocity_threshold_rad_s =
      profile.settle_velocity_threshold_rad_s.value_or(
          kDefaultSettleVelocityThresholdRadS);
  const auto settle_result = wait_for_measured_rest_after_confirmed_stop(
      execution_backend_, settle_velocity_threshold_rad_s, stop_wall_deadline,
      trip_stop_source.get_token());
  if (settle_result.settle_signal_available && !settle_result.at_rest) {
    std::map<std::string, std::string> details{
        {"hard_stop_transitioned", "false"},
        {"emergency_fallback",
         "not_attempted_controlled_stop_already_confirmed"},
        {"settle_velocity_threshold_rad_s",
         std::to_string(settle_velocity_threshold_rad_s)},
        {"settle_wait_timed_out",
         settle_result.timed_out ? "true" : "false"}};
    if (settle_result.max_abs_velocity_rad_s) {
      details["measured_max_abs_velocity_rad_s"] =
          std::to_string(*settle_result.max_abs_velocity_rad_s);
    }
    downgrade_to_intervention(
        "stop_not_settled_measured_velocity",
        "backend confirmed the stop but measured velocity did not settle "
        "within the effective stop deadline; redundant hard-stop cancel was "
        "not attempted",
        std::string(kErrorMotionInterruptBackendStopFailed),
        std::move(details));
    return;
  }
  // (3) Executed mode matches requested OR is an allowed fallback.
  const StopFallbackQualificationRow* fallback_qualification_row = nullptr;
  if (stop_result.executed_stop_mode != profile.stop_mode) {
    // Explicit hard-stop escalation. When soft_on_path is infeasible the
    // backend escalates to a hard deceleration: a fail-closed,
    // operator-visible intervention — NOT a silent qualified fallback
    // like spring_back_hold. The robot is at rest here (past the
    // accepted+confirmed gates), so no emergency floor is needed, but
    // this escalation is encoded and traced explicitly instead of
    // falling through to the generic "fallback_pair_not_qualified"
    // downgrade.
    //
    // Disposition decision: an executed stop more aggressive than the
    // requested mode is HELD for operator acknowledgment (intervention),
    // never auto-cleared as a qualified-success outcome. An escalation
    // past the requested stop mode must not be silently accepted, so it
    // is not promoted to an auto-clearing outcome (with settle-signal
    // confirmation and a resume policy) on this path.
    if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath &&
        stop_result.executed_stop_mode ==
            MotionInterruptStopMode::kHardDeceleration) {
      auto details = build_escalation_details(
          "soft_on_path_escalated_to_hard_deceleration",
          "motion_trip_hard_decel_escalation");
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {},
                   {},
                   std::optional<ErrorDTO>{ErrorDTO{
                       std::string(kErrorMotionInterruptBackendStopFailed),
                       "soft_on_path stop infeasible; backend escalated to a "
                       "hard deceleration (operator intervention required)",
                       details}},
                   details});
      downgrade_to_intervention(
          "soft_on_path_escalated_to_hard_deceleration",
          "soft_on_path stop infeasible; backend escalated to a hard "
          "deceleration (operator intervention required)");
      return;
    }
    const auto& allowed = profile.allowed_fallback_modes;
    const bool fallback_allowed =
        std::find(allowed.begin(), allowed.end(),
                  stop_result.executed_stop_mode) != allowed.end();
    if (!fallback_allowed) {
      downgrade_to_intervention(
          "executed_mode_not_in_allowed_fallback_modes",
          "backend executed a stop_mode not in "
          "stop_profile.allowed_fallback_modes");
      return;
    }
    if (node_category == MotionInterruptNodeCategory::kPrecisionContact) {
      downgrade_to_intervention(
          "fallback_not_allowed_for_node_category",
          "precision-contact node category cannot accept a stop-mode fallback");
      return;
    }
    fallback_qualification_row = qualified_stop_fallback_pair(
        profile.stop_mode, stop_result.executed_stop_mode, node_category);
    if (fallback_qualification_row == nullptr) {
      downgrade_to_intervention(
          "fallback_pair_not_qualified",
          "requested/executed stop-mode fallback pair is not in the "
          "Phase 4.5G qualification matrix");
      return;
    }
    if (!fallback_row_allows_command_source(
            *fallback_qualification_row, stop_result.stop_command_source)) {
      downgrade_to_intervention(
          "fallback_command_source_not_qualified",
          "controlled-stop fallback command source is not in the "
          "Phase 4.5G qualification matrix row");
      return;
    }
    if (!stop_result.fallback_reason) {
      downgrade_to_intervention(
          "fallback_reason_required",
          "controlled-stop fallback executed without fallback_reason");
      return;
    }
  }
  // (4) Capture a FRESH post-stop snapshot. Using the worker's last
  // pre-trip poll would record a stale state. Pull the snapshot
  // directly from the backend now that it has confirmed the stop, and
  // recover joint names from the planning context so spec/210 §3.7
  // size/ordering validation can actually run.
  const auto post_stop_backend_snapshot =
      execution_backend_.get_execution_snapshot();
  std::optional<RobotStateDTO> planning_state;
  try {
    planning_state = context_.get_current_state();
  } catch (...) {
    planning_state.reset();
  }
  std::string snap_failure_reason;
  auto snap_opt = capture_stop_snapshot(
      post_stop_backend_snapshot, planning_state,
      stop_result.backend_stop_confirmed, node_category, clock_.now(),
      settle_velocity_threshold_rad_s, snap_failure_reason);
  if (!snap_opt) {
    downgrade_to_intervention(
        std::move(snap_failure_reason),
        "controlled-stop snapshot capture or validation failed",
        std::string(kErrorMotionInterruptStopStateInvalid));
    return;
  }
  // (4b) Measured-stop confirmation. A backend-confirmed stop must also show
  // measured rest. When the settle signal is available and the robot is NOT at
  // rest even after the bounded wait above, the reported stop did not actually
  // quiesce the robot. At this point the stop command was already accepted and
  // confirmed, so issuing another JTC cancel would be redundant and possibly
  // misleading; hold intervention for explicit recovery instead. An absent
  // settle signal (UNKNOWN) falls back to the backend's confirmation and does
  // not block the outcome.
  if (snap_opt->at_rest.has_value() && !*snap_opt->at_rest) {
    std::map<std::string, std::string> settle_evidence{
        {"hard_stop_transitioned", "false"},
        {"emergency_fallback",
         "not_attempted_controlled_stop_already_confirmed"},
        {"settle_velocity_threshold_rad_s",
         std::to_string(settle_velocity_threshold_rad_s)}};
    if (snap_opt->max_abs_velocity_rad_s) {
      settle_evidence["measured_max_abs_velocity_rad_s"] =
          std::to_string(*snap_opt->max_abs_velocity_rad_s);
    }
    downgrade_to_intervention(
        "stop_not_settled_measured_velocity",
        "backend confirmed the stop but measured velocity shows the robot is "
        "not at rest; redundant hard-stop cancel was not attempted",
        std::string(kErrorMotionInterruptBackendStopFailed),
        std::move(settle_evidence));
    return;
  }
  // (5) Build and publish the signal_trip outcome.
  MotionTripOutcomeDTO trip;
  trip.latch_id = latch.latch_id;
  trip.source_id = latch.source_id;
  trip.policy_id = latch.policy_id;
  trip.scope_id = latch.motion_interrupt_scope_id.value_or("");
  trip.signal_snapshot = latch.signal_snapshot;
  trip.stop_snapshot = std::move(*snap_opt);
  trip.requested_stop_profile = profile;
  trip.executed_stop_mode = stop_result.executed_stop_mode;
  trip.stop_command_source = stop_result.stop_command_source;
  // signal_to_latch_ns measures time from when the source published
  // the signal (latch.signal_snapshot.timestamp_ns) to when the
  // manager accepted the latch (latch.latched_at_ns). Using the
  // handler's `clock_.now()` here would always be negative because
  // the latch already exists by the time we get called.
  trip.signal_to_latch_ns = std::max<std::int64_t>(
      0, latch.latched_at_ns - latch.signal_snapshot.timestamp_ns);
  trip.latch_to_backend_stop_call_ns =
      std::max<std::int64_t>(0, call_start_ns - latch.latched_at_ns);
  trip.latch_to_stop_confirmed_ns =
      std::max<std::int64_t>(0, call_end_ns - latch.latched_at_ns);
  trip.collision_world_revision = latch.borrowed_collision_world_revision;
  trip.stop_distance_rad = stop_result.stop_distance_rad;
  trip.path_deviation_max_rad = stop_result.path_deviation_max_rad;
  trip.initial_path_deviation_rad = stop_result.initial_path_deviation_rad;
  trip.fallback_reason = stop_result.fallback_reason;
  trip.diagnostics = stop_result.diagnostics;
  if (fallback_qualification_row != nullptr) {
    trip.diagnostics["fallback_qualification_id"] =
        fallback_qualification_row->qualification_id;
    trip.diagnostics["fallback_qualification_rationale"] =
        fallback_qualification_row->rationale;
    trip.diagnostics["fallback_qualification_required_telemetry"] =
        fallback_qualification_row->telemetry;
    trip.diagnostics["fallback_qualification_evidence_status"] =
        fallback_qualification_row->evidence_status;
    trip.diagnostics["fallback_pair"] =
        std::string(stop_mode_string(profile.stop_mode)) + "->" +
        stop_mode_string(stop_result.executed_stop_mode);
  }
  // Copy the scope's result_binding_key onto the trip outcome so
  // BT/runtime consumers
  // can correlate the outcome with the blackboard slot the motion
  // node owns. The key is the only stable identifier the BT layer
  // can use to dispatch the result downstream.
  if (matching_scope && matching_scope->result_binding_key) {
    trip.result_binding_key = *matching_scope->result_binding_key;
  }

  // Transition Stopping -> Completed BEFORE finalize so the outcome
  // record reflects the terminal state. The latch is cleared after
  // the outcome is durable via the auto_clear_after_motion_outcome
  // disposition path.
  auto completion_transition = sm_.try_transition(ExecutionState::kCompleted);
  if (!completion_transition.ok()) {
    downgrade_to_intervention(
        "motion_trip_completed_transition_rejected",
        "motion-trip controlled stop could not commit Completed as the "
        "terminal authority");
    return;
  }
  auto trip_success_details = controlled_stop_success_event_details(
      profile, stop_result, execution_backend_, "motion_trip");
  trip_success_details["latch_id"] = latch.latch_id;
  trip_success_details["source_id"] = latch.source_id;
  trip_success_details["policy_id"] = latch.policy_id;
  trip_success_details["scope_id"] = trip.scope_id;
  trip_success_details["signal_to_latch_ns"] =
      std::to_string(trip.signal_to_latch_ns);
  trip_success_details["latch_to_backend_stop_call_ns"] =
      std::to_string(trip.latch_to_backend_stop_call_ns);
  trip_success_details["latch_to_stop_confirmed_ns"] =
      std::to_string(trip.latch_to_stop_confirmed_ns);
  if (trip.stop_snapshot.max_abs_velocity_rad_s.has_value()) {
    trip_success_details["stop_snapshot_max_abs_velocity_rad_s"] =
        std::to_string(*trip.stop_snapshot.max_abs_velocity_rad_s);
  }
  if (trip.stop_snapshot.at_rest.has_value()) {
    trip_success_details["stop_snapshot_at_rest"] =
        *trip.stop_snapshot.at_rest ? "true" : "false";
  }
  emit_event({KernelEventType::kMotionTripCompleted, exec_id, clock_.now(),
               {}, {}, std::nullopt,
               std::move(trip_success_details)});
  emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
               ExecutionState::kStopping, ExecutionState::kCompleted,
               std::nullopt, {}});

  finalize_motion_execution_outcome(
      motion_state, MotionExecutionTerminalCause::kSignalTrip,
      std::nullopt, std::move(trip), "motion_trip_completed");

  // Auto-clear only on the success path. Failure paths leave the
  // latch in place for operator intervention-acknowledge per
  // spec/230 §3.4 / spec/260 §5.6.
  (void)interrupt_manager_.clear_active_latch(
      "auto_clear_after_motion_outcome", clock_.now());
}

std::optional<MotionExecutionOutcomeDTO>
ExecutionKernel::get_execution_outcome(std::uint64_t execution_id) const {
  std::lock_guard<std::mutex> lock(motion_outcomes_mutex_);
  const auto it = motion_execution_outcomes_.find(execution_id);
  if (it == motion_execution_outcomes_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<MotionInterruptScopeStatusDTO>
ExecutionKernel::active_motion_interrupt_scopes() const {
  return interrupt_manager_.active_motion_interrupt_scopes();
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
  reset_runtime_contact_release_state();
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
  std::unique_lock<std::mutex> lock(collision_monitor_mutex_);
  collision_monitor_active_ = false;
  ++collision_monitor_generation_;
  if (collision_monitor_handle_ != 0) {
    scheduler_.cancel(collision_monitor_handle_);
    collision_monitor_handle_ = 0;
  }
  if (!t_inside_collision_monitor_callback) {
    (void)collision_monitor_cv_.wait_for(
        lock, std::chrono::milliseconds(250), [this]() {
          return collision_monitor_callbacks_in_flight_ == 0;
        });
  }
  lock.unlock();
  reset_runtime_contact_release_state();
}

void ExecutionKernel::reset_runtime_contact_release_state() {
  std::lock_guard<std::mutex> lock(runtime_contact_release_mutex_);
  runtime_contact_release_state_.clear();
}

std::optional<std::map<std::string, std::string>>
ExecutionKernel::update_runtime_contact_release_state(
    const CollisionPolicySnapshot& policy_snapshot,
    const CollisionValidationDecisionBundle& bundle) {
  if (policy_snapshot.support_contacts.empty() ||
      bundle.waypoint_records.empty()) {
    return std::nullopt;
  }
  const auto& result = bundle.waypoint_records.front().result;
  std::lock_guard<std::mutex> lock(runtime_contact_release_mutex_);
  for (const auto& entry : policy_snapshot.support_contacts) {
    std::map<std::string, std::string> details;
    details["reason_code"] = "runtime_contact_release_monitor";
    details["contact_release_pair"] = collision_pair_name(entry.pair);
    details["contact_a"] = entry.pair.body_a;
    details["contact_b"] = entry.pair.body_b;
    details["contact_release_tolerance_m"] =
        std::to_string(entry.release_tolerance_m);
    details["contact_release_enforce_max_start_penetration"] =
        entry.enforce_max_start_penetration ? "true" : "false";
    details["contact_release_max_start_penetration_m"] =
        std::to_string(entry.max_start_penetration_m);
    details["contact_release_min_separation_clearance_m"] =
        std::to_string(entry.min_separation_clearance_m);

    if (entry.mode == CollisionContactPolicyMode::kContactProcess) {
      details["reason"] = "runtime_contact_process_policy_not_implemented";
      return details;
    }

    auto clearance = pair_clearance_for_result(result, entry.pair);
    if (!clearance.has_value() || !std::isfinite(*clearance)) {
      details["reason"] = "runtime_contact_release_missing_clearance";
      append_collision_result_observation(details, result);
      return details;
    }
    const double tolerance = std::max(0.0, entry.release_tolerance_m);
    const double depth = *clearance < 0.0 ? -*clearance : 0.0;
    const bool separated =
        *clearance >= std::max(0.0, entry.min_separation_clearance_m);
    details["contact_release_signed_clearance_m"] =
        std::to_string(*clearance);
    details["contact_release_current_penetration_m"] = std::to_string(depth);

    const auto key = collision_pair_key(entry.pair);
    auto it = runtime_contact_release_state_.find(key);
    if (it == runtime_contact_release_state_.end()) {
      if (entry.enforce_max_start_penetration &&
          depth > std::max(0.0, entry.max_start_penetration_m) + tolerance) {
        details["reason"] = "runtime_contact_release_initial_penetration_exceeds_policy";
        return details;
      }
      RuntimeContactReleasePairState state;
      state.initial_penetration_m = depth;
      state.last_penetration_m = depth;
      state.separated = separated;
      runtime_contact_release_state_.emplace(key, state);
      continue;
    }

    auto& state = it->second;
    details["contact_release_initial_penetration_m"] =
        std::to_string(state.initial_penetration_m);
    details["contact_release_previous_penetration_m"] =
        std::to_string(state.last_penetration_m);

    if (state.separated && depth > tolerance && !entry.allow_recontact) {
      details["reason"] = "runtime_contact_release_recontact";
      return details;
    }
    if (depth > state.last_penetration_m + tolerance) {
      details["reason"] = "runtime_contact_release_penetration_increased";
      return details;
    }
    state.last_penetration_m = depth;
    state.separated = state.separated || separated;
  }
  return std::nullopt;
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
          ++collision_monitor_callbacks_in_flight_;
        }

        struct CallbackGuard {
          ExecutionKernel* self;
          bool was_inside = t_inside_collision_monitor_callback;
          CallbackGuard(ExecutionKernel* kernel) : self(kernel) {
            t_inside_collision_monitor_callback = true;
          }
          ~CallbackGuard() {
            t_inside_collision_monitor_callback = was_inside;
            {
              std::lock_guard<std::mutex> lock(
                  self->collision_monitor_mutex_);
              if (self->collision_monitor_callbacks_in_flight_ > 0) {
                --self->collision_monitor_callbacks_in_flight_;
              }
            }
            self->collision_monitor_cv_.notify_all();
          }
        } callback_guard{this};

        (void)collision_check_tick();

        std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
        if (!collision_monitor_active_ ||
            collision_monitor_generation_ != generation ||
            (sm_.status().state != ExecutionState::kExecuting &&
             !motion_trip_stop_active_.load(std::memory_order_acquire))) {
          return;
        }
        schedule_next_collision_monitor_tick(generation);
      });
}

void ExecutionKernel::async_execute_worker(
    std::stop_token st, ExecuteRequestDTO request, uint64_t exec_id,
    std::optional<TrajectoryDTO> cached_trajectory) {
  // Worker-local motion-execution state. Lives on this
  // stack frame so two overlapping workers (should never happen under
  // the lifecycle mutex, but defense in depth) cannot share state.
  // The RAII guard guarantees exactly one outcome is emitted per
  // terminal execution even if a new terminal path is added later and
  // forgets to call `finalize_motion_execution_outcome` explicitly.
  ActiveMotionExecution motion_state;
  motion_state.execution_id = exec_id;
  motion_state.move_id = request.move_id;
  struct MotionOutcomeGuard {
    ExecutionKernel* self;
    ActiveMotionExecution& state;
    ~MotionOutcomeGuard() {
      if (state.outcome_emitted) {
        return;
      }
      // Defensive: an unexpected return path missed the explicit
      // finalize call. Emit `failed` with a diagnostic reason so we
      // never silently drop the outcome record.
      self->finalize_motion_execution_outcome(
          state, MotionExecutionTerminalCause::kFailed,
          ErrorDTO{kErrorInternalError,
                   "Worker exited without explicit motion outcome",
                   {{"reason", "worker_exit_without_outcome"}}},
          std::nullopt, "worker_exit_without_outcome");
    }
  };
  MotionOutcomeGuard outcome_guard{this, motion_state};
  const auto finish_worker = [this]() {
    execution_complete_.store(true);
    completion_cv_.notify_all();
  };

  try {
    // Capture initial state for correct prev_state in events (B-2)
    const auto worker_entry_state = sm_.status().state;

    // Check for early cancellation
    if (st.stop_requested()) {
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kStopped,
          std::nullopt, std::nullopt, "early_stop_requested");
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    TrajectoryDTO trajectory;
    const bool cache_hit = cached_trajectory.has_value();
    bool active_trajectory_from_cache = cache_hit;
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
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kStopped,
            std::nullopt, std::nullopt, "planning_stop");
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
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kStopped,
            std::nullopt, std::nullopt, "planning_reset");
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
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kFailed,
            plan_result.error(), std::nullopt, "planning_failed");
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
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kFailed,
            ErrorDTO{kErrorInternalError,
                     "Failed to transition to Validating", {}},
            std::nullopt, "validating_transition_failed");
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

    const int max_authoritative_validation_attempts =
        std::max(1, 1 + config_.authoritative_validation_collision_replan_attempts);
    int authoritative_validation_attempt = 0;
    Result<CollisionWorldRevision> val_result =
        Result<CollisionWorldRevision>::Err(ErrorCode::kInternalError);
    while (true) {
      emit_event({KernelEventType::kValidationStarted, exec_id, clock_.now(),
                  {}, {}, std::nullopt,
                  {{"attempt_index",
                    std::to_string(authoritative_validation_attempt)},
                   {"max_attempts",
                    std::to_string(max_authoritative_validation_attempts)}}});

      val_result = validate_trajectory(trajectory, request);
      if (val_result.ok()) {
        break;
      }
      const auto& validation_error = val_result.error();
      const bool can_replan_authoritative_collision =
          authoritative_collision_replan_candidate(validation_error) &&
          authoritative_validation_attempt + 1 <
              max_authoritative_validation_attempts &&
          sm_.status().state == ExecutionState::kValidating &&
          !st.stop_requested() && !stop_policy_.stop_requested();
      if (!can_replan_authoritative_collision) {
        break;
      }

      std::map<std::string, std::string> retry_details =
          validation_error.details;
      retry_details["reason"] = "authoritative_validation_replan";
      retry_details["attempt_index"] =
          std::to_string(authoritative_validation_attempt);
      retry_details["next_attempt_index"] =
          std::to_string(authoritative_validation_attempt + 1);
      retry_details["max_attempts"] =
          std::to_string(max_authoritative_validation_attempts);
      retry_details["move_id"] = request.move_id;
      emit_event({KernelEventType::kValidationFailed, exec_id, clock_.now(),
                  {}, {}, std::nullopt, std::move(retry_details)});

      if (request.cache_mode != CacheMode::kDisable &&
          cache_.contains(request.move_id)) {
        cache_.invalidate(request.move_id);
        emit_event({KernelEventType::kCacheInvalidated,
                    exec_id,
                    clock_.now(),
                    {},
                    {},
                    std::nullopt,
                    {{"move_id", request.move_id},
                     {"reason", "authoritative_validation_replan"}}});
        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.cache_invalidations++;
        }
      }

      auto retry_transition_from = sm_.status().state;
      auto retry_transition = sm_.try_transition(ExecutionState::kPlanning);
      if (!retry_transition.ok()) {
        break;
      }
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  retry_transition_from,
                  ExecutionState::kPlanning,
                  std::nullopt,
                  {{"reason", "authoritative_validation_replan"},
                   {"attempt_index",
                    std::to_string(authoritative_validation_attempt + 1)}}});
      auto planning_retry_snapshot = get_execution_snapshot();
      planning_retry_snapshot.timestamp_ns = clock_.now();
      planning_retry_snapshot.execution_id = exec_id;
      planning_retry_snapshot.move_id = request.move_id;
      planning_retry_snapshot.state = ExecutionState::kPlanning;
      planning_retry_snapshot.progress = 0.0;
      publish_execution_snapshot(std::move(planning_retry_snapshot));

      ++authoritative_validation_attempt;
      emit_event({KernelEventType::kPlanStarted,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"reason", "authoritative_validation_replan"},
                   {"attempt_index",
                    std::to_string(authoritative_validation_attempt)}}});
      ScheduleHandle replan_timeout_handle = 0;
      const auto replan_start_ns = clock_.now();
      const int64_t replan_timeout_ns =
          (request.timeouts.planning_timeout_s > 0)
              ? static_cast<int64_t>(request.timeouts.planning_timeout_s * 1e9)
              : 0;
      if (replan_timeout_ns > 0) {
        const int64_t deadline = replan_start_ns + replan_timeout_ns;
        replan_timeout_handle = scheduler_.schedule(
            deadline,
            [this]() { stop_policy_.request_hard_stop(); },
            SchedulerPriority::Monitoring);
      }
      auto replan_result = plan_trajectory(request);
      const bool replan_timed_out =
          replan_timeout_ns > 0 &&
          (clock_.now() - replan_start_ns) >= replan_timeout_ns;
      if (replan_timeout_handle != 0) {
        scheduler_.cancel(replan_timeout_handle);
      }

      const auto preserve_replan_terminal_authority =
          [&](const std::string& reason) -> bool {
        const auto current = sm_.status().state;
        if (current == ExecutionState::kStopped ||
            current == ExecutionState::kStopping) {
          if (current == ExecutionState::kStopping) {
            (void)sm_.try_transition(ExecutionState::kStopped);
          }
          auto stopped_snapshot = get_execution_snapshot();
          stopped_snapshot.timestamp_ns = clock_.now();
          stopped_snapshot.execution_id = exec_id;
          stopped_snapshot.move_id = request.move_id;
          stopped_snapshot.state = ExecutionState::kStopped;
          publish_execution_snapshot(std::move(stopped_snapshot));
          finalize_motion_execution_outcome(
              motion_state, MotionExecutionTerminalCause::kStopped,
              std::nullopt, std::nullopt, reason);
          finish_worker();
          return true;
        }
        if (current == ExecutionState::kInterrupted ||
            interrupt_manager_.active_latch().has_value()) {
          if (current != ExecutionState::kInterrupted) {
            const auto before_interrupt = current;
            if (sm_.try_transition(ExecutionState::kInterrupted).ok()) {
              emit_event({KernelEventType::kStateTransition,
                          exec_id,
                          clock_.now(),
                          before_interrupt,
                          ExecutionState::kInterrupted,
                          std::nullopt,
                          {{"reason", reason}}});
            }
          }
          auto interrupted_snapshot = get_execution_snapshot();
          interrupted_snapshot.timestamp_ns = clock_.now();
          interrupted_snapshot.execution_id = exec_id;
          interrupted_snapshot.move_id = request.move_id;
          interrupted_snapshot.state = ExecutionState::kInterrupted;
          publish_execution_snapshot(std::move(interrupted_snapshot));
          finalize_motion_execution_outcome(
              motion_state, MotionExecutionTerminalCause::kInterruptedFault,
              std::nullopt, std::nullopt, reason);
          finish_worker();
          return true;
        }
        if (current == ExecutionState::kInterventionRequired) {
          auto intervention_snapshot = get_execution_snapshot();
          intervention_snapshot.timestamp_ns = clock_.now();
          intervention_snapshot.execution_id = exec_id;
          intervention_snapshot.move_id = request.move_id;
          intervention_snapshot.state = ExecutionState::kInterventionRequired;
          if (!intervention_snapshot.error.has_value()) {
            intervention_snapshot.error =
                ErrorDTO{kErrorStopFailed,
                         "Authoritative validation replan lost to intervention",
                         {{"reason", reason}}};
          }
          const auto intervention_error = *intervention_snapshot.error;
          publish_execution_snapshot(std::move(intervention_snapshot));
          finalize_motion_execution_outcome(
              motion_state, MotionExecutionTerminalCause::kFailed,
              intervention_error, std::nullopt, reason);
          finish_worker();
          return true;
        }
        return false;
      };

      const auto post_replan_state = sm_.status().state;
      if (replan_timed_out || stop_policy_.stop_requested() ||
          post_replan_state == ExecutionState::kStopped ||
          post_replan_state == ExecutionState::kStopping) {
        auto prev = sm_.status().state;
        const auto message = replan_timed_out
                                 ? "Planning timed out"
                                 : "Planning interrupted by stop";
        if (replan_timed_out) {
          sm_.set_last_error(ErrorCode::kPlanningTimeout);
          emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                       {}, {},
                       ErrorDTO{kErrorPlanningTimeout, message, {}},
                       {}});
        }
        sm_.try_transition(ExecutionState::kStopped);
        auto stopped_snapshot = get_execution_snapshot();
        stopped_snapshot.timestamp_ns = clock_.now();
        stopped_snapshot.execution_id = exec_id;
        stopped_snapshot.move_id = request.move_id;
        stopped_snapshot.state = ExecutionState::kStopped;
        if (replan_timed_out) {
          stopped_snapshot.error =
              ErrorDTO{kErrorPlanningTimeout, message, {}};
        } else {
          stopped_snapshot.error.reset();
        }
        publish_execution_snapshot(std::move(stopped_snapshot));
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kStopped, std::nullopt,
                     {{"reason", "authoritative_validation_replan_stop"}}});
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kStopped,
            std::nullopt, std::nullopt, "validation_replan_stop");
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      if (sm_.status().state == ExecutionState::kIdle) {
        auto idle_snapshot = get_execution_snapshot();
        idle_snapshot.timestamp_ns = clock_.now();
        idle_snapshot.execution_id = 0;
        idle_snapshot.move_id.clear();
        idle_snapshot.state = ExecutionState::kIdle;
        idle_snapshot.progress = 0.0;
        publish_execution_snapshot(std::move(idle_snapshot));
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kStopped,
            std::nullopt, std::nullopt, "validation_replan_reset");
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      if (preserve_replan_terminal_authority(
              "validation_replan_lost_to_terminal_authority")) {
        return;
      }
      if (!replan_result.ok()) {
        if (preserve_replan_terminal_authority(
                "validation_replan_failure_lost_to_terminal_authority")) {
          return;
        }
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, replan_result.error(), {}});
        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.plans_failed++;
        }
        sm_.set_last_error(error_code_from_dto(replan_result.error()));
        sm_.try_transition(ExecutionState::kFailed);
        auto failed_snapshot = get_execution_snapshot();
        failed_snapshot.timestamp_ns = clock_.now();
        failed_snapshot.execution_id = exec_id;
        failed_snapshot.move_id = request.move_id;
        failed_snapshot.state = ExecutionState::kFailed;
        failed_snapshot.error = replan_result.error();
        publish_execution_snapshot(std::move(failed_snapshot));
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     ExecutionState::kPlanning, ExecutionState::kFailed,
                     std::nullopt,
                     {{"reason", "authoritative_validation_replan_failed"}}});
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kFailed,
            replan_result.error(), std::nullopt, "validation_replan_failed");
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      trajectory = std::move(replan_result.value());
      active_trajectory_from_cache = false;
      emit_event({KernelEventType::kPlanCompleted,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"reason", "authoritative_validation_replan"},
                   {"attempt_index",
                    std::to_string(authoritative_validation_attempt)}}});
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.plans_succeeded++;
      }
      auto retry_validating_from = sm_.status().state;
      auto retry_validating_transition =
          sm_.try_transition(ExecutionState::kValidating);
      if (!retry_validating_transition.ok()) {
        if (preserve_replan_terminal_authority(
                "validation_replan_transition_lost_to_terminal_authority")) {
          return;
        }
        sm_.set_last_error(ErrorCode::kInternalError);
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kStateTransition,
                    exec_id,
                    clock_.now(),
                    retry_validating_from,
                    ExecutionState::kFailed,
                    std::nullopt,
                    {{"reason", "authoritative_replan_validating_failed"}}});
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kFailed,
            ErrorDTO{kErrorInternalError,
                     "Failed to transition retry to Validating", {}},
            std::nullopt, "validation_replan_transition_failed");
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  retry_validating_from,
                  ExecutionState::kValidating,
                  std::nullopt,
                  {{"reason", "authoritative_validation_replan"},
                   {"attempt_index",
                    std::to_string(authoritative_validation_attempt)}}});
      auto retry_validating_snapshot = get_execution_snapshot();
      retry_validating_snapshot.timestamp_ns = clock_.now();
      retry_validating_snapshot.execution_id = exec_id;
      retry_validating_snapshot.move_id = request.move_id;
      retry_validating_snapshot.state = ExecutionState::kValidating;
      retry_validating_snapshot.progress = 0.0;
      publish_execution_snapshot(std::move(retry_validating_snapshot));
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
        // Validation cancelled by a fault session policy interrupting
        // the move (spec/260 §B.4). Cause is interrupted_fault.
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kInterruptedFault,
            std::nullopt, std::nullopt, "validation_cancelled_by_interrupt");
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
          cache_.contains(request.move_id)) {
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
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kFailed,
          val_result.error(), std::nullopt, "validation_failed");
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
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kStopped,
          std::nullopt, std::nullopt, "validation_stop");
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
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kStopped,
          std::nullopt, std::nullopt, "validation_reset");
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    // Store in cache.
    //
    // Per-joint dynamics caps (Phase 4-P5.1K) disable BOTH cache lookup (the
    // lookup path above declines and invalidates any pre-existing entry) and
    // cache store: a trajectory planned under specific caps must never become a
    // reusable entry that a later uncapped request — or a request with
    // different caps — could pick up by move_id. The TrajectoryCache key does
    // not carry a dynamics-limit fingerprint, so the cache-safe behavior is to
    // not populate it from capped requests at all.
    if (request.cache_mode != CacheMode::kDisable &&
        !active_trajectory_from_cache &&
        request.per_joint_dynamics_limits.empty() && val_result.ok() &&
        collision_world_revision_is_set(val_result.value())) {
      // Tag every cache entry with the immutable snapshot revision that
      // validation actually used. If live pose-source updates publish newer
      // revisions before the next lookup, lookup_with_revision declines the hit.
      cache_.store(request.move_id, trajectory, val_result.value());
    }

    // Arm move-scoped interrupt bindings atomically with dispatch
    // (spec/230 §3.4). Runs AFTER validation passes and
    // BEFORE the Validating->Executing transition. On failure we fail
    // closed: emit a `failed` outcome and never enter Executing or
    // call the backend. Empty `motion_interrupt_scopes` preserves the
    // legacy free-move dispatch path.
    {
      auto arm_result = arm_motion_scopes_for_dispatch(
          request, exec_id, motion_state);
      if (!arm_result.ok()) {
        auto prev = sm_.status().state;
        // Stamp the state machine with the specific arm-failure code
        // so audit/release evidence sees `motion_interrupt_arm_failed`
        // (or `motion_interrupt_source_active_at_arm`) rather than a
        // generic `internal_error`.
        sm_.set_last_error(error_code_from_dto(arm_result.error()));
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, arm_result.error(), {}});
        auto arm_fail_snapshot = get_execution_snapshot();
        arm_fail_snapshot.timestamp_ns = clock_.now();
        arm_fail_snapshot.execution_id = exec_id;
        arm_fail_snapshot.move_id = request.move_id;
        arm_fail_snapshot.state = ExecutionState::kFailed;
        arm_fail_snapshot.error = arm_result.error();
        publish_execution_snapshot(std::move(arm_fail_snapshot));
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kFailed, std::nullopt, {}});
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kFailed,
            arm_result.error(), std::nullopt, "arm_failed");
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
    }

    // Validating -> Executing
    auto prev_state3 = sm_.status().state;
    auto r3 = sm_.try_transition(ExecutionState::kExecuting);
    if (!r3.ok()) {
      sm_.set_last_error(ErrorCode::kInternalError);
      sm_.try_transition(ExecutionState::kFailed);
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state3, ExecutionState::kFailed, std::nullopt, {}});
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kFailed,
          ErrorDTO{kErrorInternalError,
                   "Failed to transition to Executing", {}},
          std::nullopt, "state_transition_failed");
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
        if (operator_stop_worker_active_.load(std::memory_order_acquire)) {
          const auto wait_budget =
              config_.operator_stop_profile
                  ? effective_controlled_stop_deadline_duration(
                        *config_.operator_stop_profile,
                        config_.complete_move_on_signal_timeout_ns)
                  : std::chrono::steady_clock::duration::zero();
          const auto bounded_wait =
              wait_budget > std::chrono::steady_clock::duration::zero()
                  ? wait_budget + std::chrono::milliseconds(250)
                  : std::chrono::milliseconds(250);
          (void)wait_for_operator_stop_worker(bounded_wait);
          const auto state_after_stop = sm_.status().state;
          if (state_after_stop == ExecutionState::kStopped ||
              state_after_stop == ExecutionState::kIdle) {
            finalize_motion_execution_outcome(
                motion_state, MotionExecutionTerminalCause::kStopped,
                std::nullopt, std::nullopt,
                "execution_timeout_deferred_to_operator_stop");
            finish_worker();
            return;
          }
          if (state_after_stop == ExecutionState::kInterventionRequired) {
            auto intervention_snapshot = get_execution_snapshot();
            intervention_snapshot.timestamp_ns = clock_.now();
            intervention_snapshot.execution_id = exec_id;
            intervention_snapshot.move_id = request.move_id;
            intervention_snapshot.state = ExecutionState::kInterventionRequired;
            if (!intervention_snapshot.error.has_value()) {
              intervention_snapshot.error =
                  ErrorDTO{kErrorStopFailed,
                           "Execution timeout deferred to operator stop, "
                           "which entered InterventionRequired without "
                           "captured error",
                           {{"reason",
                             "execution_timeout_operator_stop_intervention"}}};
            }
            const auto intervention_error = *intervention_snapshot.error;
            publish_execution_snapshot(std::move(intervention_snapshot));
            finalize_motion_execution_outcome(
                motion_state, MotionExecutionTerminalCause::kFailed,
                intervention_error, std::nullopt,
                "execution_timeout_operator_stop_intervention");
            finish_worker();
            return;
          }
        }
        if (interrupt_manager_.active_latch().has_value() ||
            sm_.status().state == ExecutionState::kInterrupted) {
          if (sm_.status().state != ExecutionState::kInterrupted) {
            (void)sm_.try_transition(ExecutionState::kInterrupted);
          }
          auto interrupted_snapshot = get_execution_snapshot();
          interrupted_snapshot.timestamp_ns = clock_.now();
          interrupted_snapshot.execution_id = exec_id;
          interrupted_snapshot.move_id = request.move_id;
          interrupted_snapshot.state = ExecutionState::kInterrupted;
          publish_execution_snapshot(std::move(interrupted_snapshot));
          finalize_motion_execution_outcome(
              motion_state, MotionExecutionTerminalCause::kInterruptedFault,
              std::nullopt, std::nullopt,
              "execution_timeout_deferred_to_interrupt");
          finish_worker();
          return;
        }
        (void)execution_backend_.stop();
        const bool stop_completed = drain_execution_handle_bounded(execution_handle);
        auto prev = sm_.status().state;
        sm_.set_last_error(stop_completed ? ErrorCode::kExecutionTimeout
                                           : ErrorCode::kStopFailed);
        auto terminal_state = stop_completed ? ExecutionState::kFailed
                                             : ExecutionState::kInterventionRequired;
        auto timeout_transition = sm_.try_transition(terminal_state);
        if (!timeout_transition.ok()) {
          const auto current = sm_.status().state;
          if (current == ExecutionState::kStopped ||
              current == ExecutionState::kIdle) {
            finalize_motion_execution_outcome(
                motion_state, MotionExecutionTerminalCause::kStopped,
                std::nullopt, std::nullopt,
                "execution_timeout_lost_to_stop");
            finish_worker();
            return;
          }
          if (current == ExecutionState::kInterrupted ||
              interrupt_manager_.active_latch().has_value()) {
            finalize_motion_execution_outcome(
                motion_state,
                MotionExecutionTerminalCause::kInterruptedFault,
                std::nullopt, std::nullopt,
                "execution_timeout_lost_to_interrupt");
            finish_worker();
            return;
          }
          terminal_state = ExecutionState::kInterventionRequired;
          auto intervention_transition = sm_.try_transition(terminal_state);
          if (!intervention_transition.ok()) {
            terminal_state = sm_.status().state;
          }
        }

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
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kFailed,
            terminal_error, std::nullopt, "execution_timeout");
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
        auto latch = interrupt_manager_.active_latch();
        if (latch &&
            latch->preemption_action_requested ==
                InterruptPreemptionAction::kPreemptTrajectory) {
          // Fault session policy preempted the move. The scope (if
          // any) is reaped as part of finalize; terminal_cause is
          // `interrupted_fault` (spec/260 §B.4). The fault's own
          // latch surface is the authoritative record; this outcome
          // exists for symmetric BT consumption.
          stop_collision_monitoring();
          finalize_motion_execution_outcome(
              motion_state, MotionExecutionTerminalCause::kInterruptedFault,
              std::nullopt, std::nullopt, "interrupted_fault");
          execution_complete_.store(true);
          completion_cv_.notify_all();
          return;
        }
        if (latch &&
            latch->preemption_action_requested ==
                InterruptPreemptionAction::kCompleteMoveOnSignal) {
          // Move-scoped trip handling. The helper
          // resolves the matching scope/stop_profile, transitions
          // Interrupted -> Stopping, invokes the controlled-stop
          // backend port, captures the validated stop snapshot,
          // enforces the conjunctive fallback rule, and transitions
          // Stopping -> Completed (signal_trip) or Stopping -> Failed.
          handle_motion_trip_latch(request, *latch, exec_id,
                                     latest_backend_snapshot, motion_state);
          execution_complete_.store(true);
          completion_cv_.notify_all();
          return;
        }
      }

      if (latest_backend_snapshot.state == ExecutionState::kPaused ||
          loop_state == ExecutionState::kPaused) {
        stop_collision_monitoring();

        if (resume_requested_.exchange(false)) {
          auto finish_resume_interrupted = [&](ExecutionState terminal_state,
                                               ErrorDTO error,
                                               const char* reap_reason) {
            sm_.set_last_error(error_code_from_dto(error));
            if (terminal_state != sm_.status().state) {
              sm_.try_transition(terminal_state);
            }
            auto terminal_snapshot = latest_backend_snapshot;
            terminal_snapshot.timestamp_ns = clock_.now();
            terminal_snapshot.execution_id =
                terminal_state == ExecutionState::kIdle ? 0 : exec_id;
            terminal_snapshot.move_id =
                terminal_state == ExecutionState::kIdle ? std::string{} : request.move_id;
            terminal_snapshot.state = terminal_state;
            terminal_snapshot.error = error;
            publish_execution_snapshot(std::move(terminal_snapshot));
            emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                        ExecutionState::kValidating, terminal_state,
                        std::nullopt, {{"reason", reap_reason}}});
            finalize_motion_execution_outcome(
                motion_state, MotionExecutionTerminalCause::kStopped,
                std::nullopt, std::nullopt, reap_reason);
            execution_complete_.store(true);
            completion_cv_.notify_all();
          };

          auto fail_resume_closed = [&](ErrorDTO error,
                                        const char* failure_reason,
                                        const char* outcome_reap_reason,
                                        bool count_plan_failure,
                                        bool count_validation_failure) {
            if (count_validation_failure) {
              emit_event({KernelEventType::kValidationFailed, exec_id, clock_.now(),
                          {}, {}, std::nullopt,
                          {{"reason", failure_reason},
                           {"resume_phase", "validation"},
                           {"resume_replan", "true"}}});
            }
            emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                        {}, {}, error, {}});
            {
              std::lock_guard<std::mutex> lock(metrics_mutex_);
              if (count_plan_failure) {
                metrics_.plans_failed++;
              }
              if (count_validation_failure) {
                metrics_.validations_failed++;
              }
            }
            sm_.set_last_error(error_code_from_dto(error));
            (void)execution_backend_.stop();
            const bool stop_completed =
                drain_execution_handle_bounded(execution_handle);
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
                ? error
                : operator_intervention_error(
                      ErrorCode::kStopFailed,
                      "Resume failed and software stop completion was not observed",
                      "resume_failure_stop_drain",
                      prev_state,
                      exec_id,
                      request.move_id);
            publish_execution_snapshot(std::move(failed_snapshot));
            emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                        prev_state, terminal_state, std::nullopt, {}});
            finalize_motion_execution_outcome(
                motion_state, MotionExecutionTerminalCause::kFailed,
                stop_completed
                    ? error
                    : ErrorDTO{kErrorStopFailed,
                               "Resume failed and stop drain did not complete",
                               {{"reason", "resume_failure_stop_drain"}}},
                std::nullopt, outcome_reap_reason);
            execution_complete_.store(true);
            completion_cv_.notify_all();
          };

          emit_event({KernelEventType::kPlanStarted, exec_id, clock_.now(),
                      {}, {}, std::nullopt,
                      {{"reason", "resume_replan"},
                       {"resume_phase", "planning"}}});
          auto resume_plan = plan_resume_trajectory(request);
          if (!resume_plan.ok()) {
            fail_resume_closed(resume_plan.error(),
                               "resume planning failed",
                               "resume_planning_failed",
                               true,
                               false);
            return;
          }

          auto resumed_trajectory = std::move(resume_plan.value());
          emit_event({KernelEventType::kPlanCompleted, exec_id, clock_.now(),
                      {}, {}, std::nullopt,
                      {{"reason", "resume_replan"},
                       {"resume_phase", "planning"}}});
          {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            metrics_.plans_succeeded++;
          }

          if (stop_policy_.stop_requested() ||
              sm_.status().state == ExecutionState::kStopped) {
            finish_resume_interrupted(
                ExecutionState::kStopped,
                ErrorDTO{kErrorInternalError,
                         "Resume planning interrupted by stop",
                         {{"reason", "resume_planning_stop"}}},
                "resume_planning_stop");
            return;
          }
          if (sm_.status().state == ExecutionState::kIdle) {
            finish_resume_interrupted(
                ExecutionState::kIdle,
                ErrorDTO{kErrorInternalError,
                         "Resume planning interrupted by reset",
                         {{"reason", "resume_planning_reset"}}},
                "resume_planning_reset");
            return;
          }

          emit_event({KernelEventType::kValidationStarted, exec_id, clock_.now(),
                      {}, {}, std::nullopt,
                      {{"reason", "resume_replan"},
                       {"resume_phase", "validation"}}});

          Result<CollisionWorldRevision> resume_validation =
              validate_trajectory(resumed_trajectory, request);
          if (!resume_validation.ok()) {
            if (validation_cancelled_by_interrupt(resume_validation.error()) &&
                sm_.status().state == ExecutionState::kInterrupted) {
              emit_event({KernelEventType::kValidationFailed, exec_id, clock_.now(),
                          {}, {}, std::nullopt,
                          {{"reason", "resume validation cancelled by interrupt"},
                           {"resume_phase", "validation"},
                           {"resume_replan", "true"}}});
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
              finalize_motion_execution_outcome(
                  motion_state,
                  MotionExecutionTerminalCause::kInterruptedFault,
                  std::nullopt, std::nullopt,
                  "resume_validation_cancelled_by_interrupt");
              execution_complete_.store(true);
              completion_cv_.notify_all();
              return;
            }
            fail_resume_closed(resume_validation.error(),
                               "resume validation failed",
                               "resume_validation_failed",
                               false,
                               true);
            return;
          }

          emit_event({KernelEventType::kValidationPassed, exec_id, clock_.now(),
                      {}, {}, std::nullopt,
                      {{"reason", "resume_replan"},
                       {"resume_phase", "validation"}}});
          {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            metrics_.validations_passed++;
          }

          if (stop_policy_.stop_requested() ||
              sm_.status().state == ExecutionState::kStopped) {
            finish_resume_interrupted(
                ExecutionState::kStopped,
                ErrorDTO{kErrorInternalError,
                         "Resume validation interrupted by stop",
                         {{"reason", "resume_validation_stop"}}},
                "resume_validation_stop");
            return;
          }
          if (sm_.status().state == ExecutionState::kIdle) {
            finish_resume_interrupted(
                ExecutionState::kIdle,
                ErrorDTO{kErrorInternalError,
                         "Resume validation interrupted by reset",
                         {{"reason", "resume_validation_reset"}}},
                "resume_validation_reset");
            return;
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
            finalize_motion_execution_outcome(
                motion_state, MotionExecutionTerminalCause::kFailed,
                ErrorDTO{kErrorResumeFailed,
                         "Resume transition to Executing failed", {}},
                std::nullopt, "resume_transition_failed");
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
          start_collision_monitoring();
        }

        if (completed) {
          // Some real backends implement pause by cancelling the active
          // controller goal. Their execution handle is then terminal even
          // though the kernel state is intentionally kPaused. Keep the kernel
          // worker alive so a later resume() can replan, validate, and
          // fresh-dispatch the original motion intent; otherwise the state
          // machine can advance to kValidating with no worker left to consume
          // resume_requested_.
          //
          // Audit 6D (finding 2.19): condvar wait instead of a bare sleep —
          // resume()/stop()/reset() notify `completion_cv_`, so the worker
          // wakes immediately on intent instead of up to one poll period
          // later. The poll period remains only as the deadline fallback.
          std::unique_lock<std::mutex> wake_lock(completion_mutex_);
          completion_cv_.wait_for(
              wake_lock, std::chrono::nanoseconds(kExecutionPollNs),
              [this]() {
                return resume_requested_.load(std::memory_order_acquire) ||
                       lifecycle_stop_requested_while_busy_.load(
                           std::memory_order_seq_cst) ||
                       lifecycle_reset_requested_while_busy_.load(
                           std::memory_order_acquire);
              });
        }
        continue;
      }

      if (completed) {
        break;
      }
    }

    stop_collision_monitoring();

    const auto finalize_competing_terminal_authority =
        [&](const char* reason) -> bool {
      if (motion_state.outcome_emitted) {
        finish_worker();
        return true;
      }

      if (operator_stop_worker_active_.load(std::memory_order_acquire)) {
        const auto wait_budget =
            config_.operator_stop_profile
                ? effective_controlled_stop_deadline_duration(
                      *config_.operator_stop_profile,
                      config_.complete_move_on_signal_timeout_ns)
                : std::chrono::steady_clock::duration::zero();
        const auto bounded_wait =
            wait_budget > std::chrono::steady_clock::duration::zero()
                ? wait_budget + std::chrono::milliseconds(250)
                : std::chrono::milliseconds(250);
        (void)wait_for_operator_stop_worker(bounded_wait);
      }

      const auto current = sm_.status().state;
      if (motion_state.outcome_emitted) {
        finish_worker();
        return true;
      }
      if (current == ExecutionState::kStopped ||
          current == ExecutionState::kIdle) {
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kStopped,
            std::nullopt, std::nullopt, reason);
        finish_worker();
        return true;
      }
      if (current == ExecutionState::kInterventionRequired) {
        auto intervention_snapshot = get_execution_snapshot();
        intervention_snapshot.timestamp_ns = clock_.now();
        intervention_snapshot.execution_id = exec_id;
        intervention_snapshot.move_id = request.move_id;
        intervention_snapshot.state = ExecutionState::kInterventionRequired;
        if (!intervention_snapshot.error.has_value()) {
          std::optional<ErrorDTO> operator_stop_error;
          {
            std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
            operator_stop_error = operator_stop_intervention_error_;
          }
          intervention_snapshot.error =
              operator_stop_error.value_or(
                  ErrorDTO{kErrorStopFailed,
                           "Execution terminal authority moved to "
                           "InterventionRequired without captured error",
                           {{"reason", "terminal_authority_intervention"}}});
        }
        const auto intervention_error = *intervention_snapshot.error;
        publish_execution_snapshot(std::move(intervention_snapshot));
        {
          std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
          metrics_.executions_failed++;
        }
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kFailed,
            intervention_error, std::nullopt, reason);
        finish_worker();
        return true;
      }
      if (current == ExecutionState::kInterrupted ||
          interrupt_manager_.active_latch().has_value()) {
        const auto prev = current;
        if (current != ExecutionState::kInterrupted) {
          (void)sm_.try_transition(ExecutionState::kInterrupted);
        }
        auto interrupted_snapshot = get_execution_snapshot();
        interrupted_snapshot.timestamp_ns = clock_.now();
        interrupted_snapshot.execution_id = exec_id;
        interrupted_snapshot.move_id = request.move_id;
        interrupted_snapshot.state = ExecutionState::kInterrupted;
        publish_execution_snapshot(std::move(interrupted_snapshot));
        if (prev != ExecutionState::kInterrupted &&
            sm_.status().state == ExecutionState::kInterrupted) {
          emit_event({KernelEventType::kStateTransition,
                      exec_id,
                      clock_.now(),
                      prev,
                      ExecutionState::kInterrupted,
                      std::nullopt,
                      {{"reason", "terminal_authority_interrupt_latch"}}});
        }
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kInterruptedFault,
            std::nullopt, std::nullopt, reason);
        finish_worker();
        return true;
      }
      if (current == ExecutionState::kStopping ||
          operator_stop_worker_active_.load(std::memory_order_acquire)) {
        ErrorDTO err{
            kErrorStopFailed,
            "Execution backend reported terminal completion while stop "
            "authority was still active",
            {{"reason", "terminal_completion_stop_authority_active"},
             {"phase", reason},
             {"state", to_string(current)}}};
        sm_.set_last_error(error_code_from_dto(err));
        const auto prev = sm_.status().state;
        auto transition =
            sm_.try_transition(ExecutionState::kInterventionRequired);
        if (transition.ok()) {
          auto snapshot = get_execution_snapshot();
          snapshot.timestamp_ns = clock_.now();
          snapshot.execution_id = exec_id;
          snapshot.move_id = request.move_id;
          snapshot.state = ExecutionState::kInterventionRequired;
          snapshot.error = err;
          publish_execution_snapshot(std::move(snapshot));
          emit_event({KernelEventType::kErrorRaised,
                      exec_id,
                      clock_.now(),
                      {},
                      {},
                      err,
                      err.details});
          emit_event({KernelEventType::kStateTransition,
                      exec_id,
                      clock_.now(),
                      prev,
                      ExecutionState::kInterventionRequired,
                      std::nullopt,
                      {{"reason",
                        "terminal_completion_stop_authority_active"}}});
        }
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kFailed,
            err, std::nullopt, reason);
        finish_worker();
        return true;
      }
      return false;
    };

    if (finalize_competing_terminal_authority(
            "terminal_authority_pre_final_snapshot")) {
      return;
    }

    if (sm_.status().state == ExecutionState::kInterventionRequired) {
      auto intervention_snapshot = get_execution_snapshot();
      intervention_snapshot.timestamp_ns = clock_.now();
      intervention_snapshot.execution_id = exec_id;
      intervention_snapshot.move_id = request.move_id;
      intervention_snapshot.state = ExecutionState::kInterventionRequired;
      if (!intervention_snapshot.error.has_value()) {
        std::optional<ErrorDTO> operator_stop_error;
        {
          std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
          operator_stop_error = operator_stop_intervention_error_;
        }
        intervention_snapshot.error =
            operator_stop_error.value_or(
                ErrorDTO{kErrorStopFailed,
                         "Execution entered InterventionRequired without "
                         "captured error",
                         {{"reason", "missing_error_on_intervention"}}});
      }
      const auto intervention_error = *intervention_snapshot.error;
      publish_execution_snapshot(std::move(intervention_snapshot));
      {
        std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
        metrics_.executions_failed++;
      }
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kFailed,
          intervention_error, std::nullopt, "execution_intervention_required");
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    if (execution_handle.get_snapshot) {
      final_snapshot = execution_handle.get_snapshot();
      final_snapshot.execution_id = exec_id;
      final_snapshot.move_id = request.move_id;
      if ((final_snapshot.state == ExecutionState::kStopped ||
           sm_.status().state == ExecutionState::kStopped) &&
          !final_snapshot.error.has_value()) {
        std::optional<ErrorDTO> operator_stop_error;
        {
          std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
          operator_stop_error = operator_stop_terminal_error_;
        }
        if (operator_stop_error.has_value()) {
          final_snapshot.error = *operator_stop_error;
        }
      }
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
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kStopped,
          std::nullopt, std::nullopt, "execution_stop_terminal");
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
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kFailed,
          final_snapshot.error.value_or(
              ErrorDTO{kErrorInternalError,
                       "Execution failed without captured error",
                       {{"reason", "missing_error_on_failed"}}}),
          std::nullopt, "execution_failed");
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
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kFailed,
          final_snapshot.error.value_or(
              ErrorDTO{kErrorStopFailed,
                       "Execution required intervention without captured "
                       "error",
                       {{"reason", "missing_error_on_intervention"}}}),
          std::nullopt, "execution_intervention_required");
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    auto prev_state4 = sm_.status().state;
    auto completed_transition = sm_.try_transition(ExecutionState::kCompleted);
    if (!completed_transition.ok()) {
      if (finalize_competing_terminal_authority(
              "normal_completion_transition_rejected")) {
        return;
      }
      ErrorDTO err{
          kErrorInternalError,
          "Execution backend reported completion, but the kernel could not "
          "commit Completed as terminal authority",
          {{"reason", "completed_transition_rejected"},
           {"from_state", to_string(prev_state4)}}};
      sm_.set_last_error(error_code_from_dto(err));
      auto failure_transition =
          sm_.try_transition(ExecutionState::kInterventionRequired);
      auto rejected_snapshot = final_snapshot;
      rejected_snapshot.timestamp_ns = clock_.now();
      rejected_snapshot.execution_id = exec_id;
      rejected_snapshot.move_id = request.move_id;
      rejected_snapshot.state = sm_.status().state;
      rejected_snapshot.error = err;
      publish_execution_snapshot(std::move(rejected_snapshot));
      emit_event({KernelEventType::kErrorRaised,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  err,
                  err.details});
      if (failure_transition.ok()) {
        emit_event({KernelEventType::kStateTransition,
                    exec_id,
                    clock_.now(),
                    prev_state4,
                    ExecutionState::kInterventionRequired,
                    std::nullopt,
                    {{"reason", "completed_transition_rejected"}}});
      }
      {
        std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
        metrics_.executions_failed++;
      }
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kFailed,
          err, std::nullopt, "normal_completion_transition_rejected");
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    emit_event({KernelEventType::kExecuteCompleted, exec_id, clock_.now(),
                {}, {}, std::nullopt, {}});

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
    // Trajectory reached its planned endpoint. Cause is
    // `completed_normally` for free moves; the finalize helper rewrites
    // to `no_signal_trip` when the request carried scopes (spec/210
    // §3.11 invariants).
    finalize_motion_execution_outcome(
        motion_state, MotionExecutionTerminalCause::kCompletedNormally,
        std::nullopt, std::nullopt, "execution_completed");

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
    finalize_motion_execution_outcome(
        motion_state, MotionExecutionTerminalCause::kFailed,
        ErrorDTO{kErrorInternalError, e.what(),
                 {{"reason", "worker_exception"}}},
        std::nullopt, "worker_exception");
  }

  execution_complete_.store(true);
  completion_cv_.notify_all();
}

Result<uint64_t> ExecutionKernel::execute(const ExecuteRequestDTO& request) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  lifecycle_stop_requested_while_busy_.store(false,
                                             std::memory_order_seq_cst);
  lifecycle_stop_requested_event_emitted_.store(false,
                                               std::memory_order_seq_cst);
  lifecycle_reset_requested_while_busy_.store(false, std::memory_order_release);
  if (lifecycle_reset_waiting_for_stop_.load(std::memory_order_acquire)) {
    return Result<uint64_t>::Err(ErrorDTO{
        kErrorExecutionRejected,
        "Reset is waiting for stop completion",
        {{"reason_code", "reset_waiting_for_stop"}}});
  }

  // `kInterventionRequired` MUST be cleared by an explicit recovery action,
  // not silently by `execute()`. An implicit clear here would erase the
  // operator intervention boundary: any caller could re-issue `execute` to
  // bypass the intervention gate with no audit event.
  if (sm_.status().state == ExecutionState::kInterventionRequired) {
    return Result<uint64_t>::Err(ErrorDTO{
        kErrorInvalidRequest,
        "Execution is in InterventionRequired; explicit recovery action required",
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

  uint64_t exec_id = next_execution_id_++;
  auto prev_state = sm_.status().state;
  sm_.set_execution_id(exec_id);
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    active_move_id_ = request.move_id;
  }
  auto transition = sm_.try_transition(ExecutionState::kPlanning);
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
              prev_state, ExecutionState::kPlanning, std::nullopt, {}});

  ExecutionSnapshot admission_snapshot;
  admission_snapshot.timestamp_ns = clock_.now();
  admission_snapshot.execution_id = exec_id;
  admission_snapshot.move_id = request.move_id;
  admission_snapshot.state = ExecutionState::kPlanning;
  admission_snapshot.progress = 0.0;
  publish_execution_snapshot(std::move(admission_snapshot));

  const auto finish_admission_without_worker = [&]() {
    execute_admission_interruptible_.store(false, std::memory_order_seq_cst);
    execution_complete_.store(true);
    completion_cv_.notify_all();
  };

  const auto fail_admission = [&](ErrorDTO error) -> Result<uint64_t> {
    sm_.set_last_error(error_code_from_dto(error));
    const auto failed_from_state = sm_.status().state;
    auto fail_transition = sm_.try_transition(ExecutionState::kFailed);
    auto failed_snapshot = get_execution_snapshot();
    failed_snapshot.timestamp_ns = clock_.now();
    failed_snapshot.execution_id = exec_id;
    failed_snapshot.move_id = request.move_id;
    failed_snapshot.state = sm_.status().state;
    failed_snapshot.error = error;
    publish_execution_snapshot(std::move(failed_snapshot));
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {},
                error, {}});
    if (fail_transition.ok()) {
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  failed_from_state,
                  ExecutionState::kFailed,
                  std::nullopt,
                  {{"reason", "execute_admission_failed"}}});
    }
    finish_admission_without_worker();
    return Result<uint64_t>::Err(std::move(error));
  };

  const auto reject_admission = [&](ErrorDTO error) -> Result<uint64_t> {
    const auto rejected_from_state = sm_.status().state;
    (void)stop_policy_.request_reset();
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      active_move_id_.clear();
    }
    sm_.set_execution_id(0);
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    if (rejected_from_state != ExecutionState::kIdle) {
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  rejected_from_state,
                  ExecutionState::kIdle,
                  std::nullopt,
                  {{"reason", "execute_admission_rejected"}}});
    }
    finish_admission_without_worker();
    return Result<uint64_t>::Err(std::move(error));
  };

  execute_admission_interruptible_.store(true, std::memory_order_seq_cst);

  auto sync_result = execution_backend_.sync_observed_state_before_planning();
  if (!sync_result.ok()) {
    return reject_admission(sync_result.error());
  }

  ExecuteRequestDTO resolved_request(request);
  auto resolved_motion = resolve_motion_spec(request);
  if (!resolved_motion.ok()) {
    if (request.cache_mode != CacheMode::kDisable &&
        cache_.contains(request.move_id)) {
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
    return reject_admission(resolved_motion.error());
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
      // Per-joint dynamics caps (Phase 4-P5.1K) disable cache lookup: a request
      // carrying non-empty caps never serves from cache. It also invalidates any
      // pre-existing entry for this move_id (which may have been planned under
      // no caps, or different caps) so a stale trajectory cannot linger. The
      // companion store guard (see "Store in cache" below) ensures a capped
      // request also never populates the cache. Together these give cache safety
      // without a dynamics-limit fingerprint on the cache key.
      const bool has_dynamics_limits = !request.per_joint_dynamics_limits.empty();
      const auto goal_match =
          cached_trajectory_matches_request_goal(context_, *cached, resolved_request);
      const auto start_match =
          synced_state.has_value()
              ? cached_trajectory_matches_current_start(
                    *cached, *synced_state, request.tolerances.joint_position_tol_rad)
              : incompatible_cache("start_state_changed");
      if (!has_dynamics_limits && goal_match.compatible && start_match.compatible) {
        prepared.cache_hit = true;
        prepared.trajectory = std::move(*cached);
      } else {
        cache_.invalidate(request.move_id);
        prepared.cache_invalidated = true;
        prepared.cache_invalidation_reason =
            has_dynamics_limits
                ? "per_joint_dynamics_limits_present"
                : (!start_match.compatible ? start_match.reason : goal_match.reason);
      }
    } else {
      prepared.cache_miss = true;
    }
  }

  execute_admission_interruptible_.store(false, std::memory_order_seq_cst);

  const bool pending_reset =
      lifecycle_reset_requested_while_busy_.exchange(false,
                                                     std::memory_order_acq_rel);
  const bool pending_stop =
      lifecycle_stop_requested_while_busy_.exchange(false,
                                                    std::memory_order_seq_cst);
  if (pending_reset) {
    // Execute admission has not launched a motion worker or operator-stop
    // worker yet; reset can clear the admission without touching any selected
    // stop profile. Active operator stops are handled by reset() in Stopping.
    auto backend_reset = execution_backend_.reset();
    if (!backend_reset.accepted) {
      return fail_admission(backend_reset.error.value_or(
          ErrorDTO{kErrorResetFailed,
                   "Execution backend reset failed during pending reset",
                   {{"reason", "pending_reset_backend_failed"}}}));
    }
    auto result = stop_policy_.request_reset();
    if (!result.ok()) {
      return fail_admission(result.error());
    }
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      active_move_id_.clear();
    }
    sm_.set_execution_id(0);
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                ExecutionState::kPlanning,
                ExecutionState::kIdle,
                std::nullopt,
                {{"reason", "pending_reset_during_execute_admission"}}});
    finish_admission_without_worker();
    return Result<uint64_t>::Ok(exec_id);
  }
  if (pending_stop) {
    auto result = stop_policy_.request_hard_stop();
    if (!result.ok()) {
      return fail_admission(result.error());
    }
    stop_collision_monitoring();
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    const bool stop_requested_already_emitted =
        lifecycle_stop_requested_event_emitted_.exchange(
            false, std::memory_order_seq_cst);
    if (!stop_requested_already_emitted) {
      emit_event({KernelEventType::kStopRequested, exec_id, clock_.now(), {},
                  {}, std::nullopt,
                  {{"reason", "pending_stop_during_execute_admission"}}});
    }
    emit_event({KernelEventType::kStopIssued, exec_id, clock_.now(), {}, {},
                std::nullopt, {}});
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                ExecutionState::kPlanning,
                sm_.status().state,
                std::nullopt,
                {{"reason", "pending_stop_during_execute_admission"}}});
    finish_admission_without_worker();
    return Result<uint64_t>::Ok(exec_id);
  }

  const auto initial_state =
      prepared.cache_hit ? ExecutionState::kValidating
                         : ExecutionState::kPlanning;
  if (initial_state != ExecutionState::kPlanning) {
    auto cache_hit_transition = sm_.try_transition(initial_state);
    if (!cache_hit_transition.ok()) {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      active_move_id_.clear();
      sm_.set_execution_id(0);
      finish_admission_without_worker();
      return Result<uint64_t>::Err(ErrorCode::kInternalError);
    }
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                ExecutionState::kPlanning, initial_state, std::nullopt, {}});
  }

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

  execute_admission_interruptible_.store(false, std::memory_order_seq_cst);
  return Result<uint64_t>::Ok(exec_id);
}

StopResultDTO ExecutionKernel::stop() {
  join_finished_operator_stop_worker();
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_,
                                              std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
      lifecycle_stop_requested_while_busy_.store(true,
                                                 std::memory_order_seq_cst);
      lifecycle_stop_requested_event_emitted_.store(
          true, std::memory_order_seq_cst);
      emit_event({KernelEventType::kStopRequested,
                  sm_.status().execution_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"reason", "lifecycle_busy"}}});
      if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
        return StopResultDTO{true, std::nullopt};
      }
    }
    lifecycle_lock.lock();
  }
  if (lifecycle_stop_requested_event_emitted_.exchange(
          false, std::memory_order_seq_cst) &&
      !lifecycle_stop_requested_while_busy_.load(std::memory_order_seq_cst) &&
      sm_.status().state == ExecutionState::kStopped) {
    return StopResultDTO{true, std::nullopt};
  }
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);

  // Per spec/230 §6.1 Stop chain:
  //   stop_requested → state_transition(Stopping) → stop_issued
  //                  → state_transition(Stopped)
  // kStopRequested marks operator intent at the API boundary; the
  // intermediate kStopping covers the controlled-stop deceleration
  // window (1-2 s for soft-on-path), making the "stopping…" period
  // observable to the UI and to trace reconstruction.
  const auto prev_state = sm_.status().state;
  emit_event({KernelEventType::kStopRequested, sm_.status().execution_id,
              clock_.now(), {}, {}, std::nullopt, {}});

  // Pre-transition to kStopping when the source state has a
  // deceleration window. The state machine allows kStopping from
  // Executing/Paused; Planning/Validating cancel direct to Stopped
  // (no intermediate to observe). stop_policy_.request_hard_stop()
  // handles the final Stopping → Stopped step.
  const bool transitioned_to_stopping =
      (prev_state == ExecutionState::kExecuting ||
       prev_state == ExecutionState::kPaused) &&
      sm_.try_transition(ExecutionState::kStopping).ok();
  if (transitioned_to_stopping) {
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id, clock_.now(),
                prev_state, ExecutionState::kStopping,
                std::nullopt, {}});
  }
  // `pre_completion_state` is the from-state for any subsequent
  // state-transition event so the trace remains causal.
  const ExecutionState pre_completion_state =
      transitioned_to_stopping ? ExecutionState::kStopping : prev_state;

  const bool operator_profile_applicable =
      config_.operator_stop_profile &&
      operator_controlled_stop_allowed(prev_state);

  // Repeated Stop while an operator controlled stop is already in flight is
  // idempotent. Escalation to hard stop is an explicit reset/timeout path, not
  // an accidental semantic change from a second Stop button press.
  if (prev_state == ExecutionState::kStopping &&
      operator_stop_worker_active_.load(std::memory_order_acquire)) {
    lifecycle_stop_requested_while_busy_.store(false,
                                               std::memory_order_seq_cst);
    return StopResultDTO{true, std::nullopt};
  }

  if (operator_profile_applicable) {
    stop_collision_monitoring();
    cache_.invalidate_all();
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    if (!start_operator_stop_worker(*config_.operator_stop_profile,
                                    pre_completion_state)) {
      auto result = stop_policy_.request_hard_stop();
      if (!result.ok()) {
        return StopResultDTO{false, result.error()};
      }
      auto hard_snapshot = get_execution_snapshot();
      hard_snapshot.timestamp_ns = clock_.now();
      hard_snapshot.state = sm_.status().state;
      publish_execution_snapshot(std::move(hard_snapshot));
      emit_event({KernelEventType::kStopIssued, sm_.status().execution_id,
                  clock_.now(), {}, {}, std::nullopt, {}});
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id, clock_.now(),
                  pre_completion_state, sm_.status().state, std::nullopt,
                  {{"reason", "operator_stop_worker_unavailable_hard_stop"}}});
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.stops_issued++;
    }
    lifecycle_stop_requested_while_busy_.store(false,
                                               std::memory_order_seq_cst);
    return StopResultDTO{true, std::nullopt};
  }

  auto result = stop_policy_.request_hard_stop();
  if (!result.ok()) {
    return StopResultDTO{false, result.error()};
  }

  stop_collision_monitoring();
  auto snapshot = get_execution_snapshot();
  snapshot.timestamp_ns = clock_.now();
  snapshot.state = sm_.status().state;
  publish_execution_snapshot(std::move(snapshot));
  // Emit kStopIssued event
  emit_event({KernelEventType::kStopIssued, sm_.status().execution_id, clock_.now(),
              {}, {}, std::nullopt, {}});
  // Emit terminal state transition. `pre_completion_state` is kStopping
  // when the source state had a deceleration window (Executing/Paused),
  // matching the spec/230 §6.1 stop chain; for Planning/Validating
  // cancellations it is the original prev_state and the transition goes
  // direct to kStopped.
  auto new_state = sm_.status().state;
  emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
              pre_completion_state, new_state, std::nullopt, {}});
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  metrics_.stops_issued++;
  lifecycle_stop_requested_while_busy_.store(false,
                                             std::memory_order_seq_cst);
  return StopResultDTO{true, std::nullopt};
}

PauseResultDTO ExecutionKernel::pause() {
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_,
                                              std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
      return PauseResultDTO{
          false,
          ErrorDTO{kErrorInvalidRequest,
                   "Pause is only valid during Executing",
                   {{"reason_code", "pause_not_executing"},
                    {"state", to_string(sm_.status().state)}}}};
    }
    lifecycle_lock.lock();
  }

  // Per spec/230 §6.1 Pause chain:
  //   pause_requested → state_transition(Stopping) → state_transition(Paused)
  // The Executing → Stopping transition is synchronous inside
  // stop_policy_.request_pause(); the Stopping → Paused transition fires
  // asynchronously via the scheduled lambda there. The
  // pause_completed_callback (wired in the constructor) emits the
  // terminal kStateTransition(Paused) event when that lambda runs.
  const auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_pause();
  if (result.ok()) {
    stop_collision_monitoring();
    auto backend_pause = execution_backend_.pause();
    if (!backend_pause.accepted) {
      auto error = backend_pause.error.value_or(
          ErrorDTO{kErrorStopFailed,
                   "Execution backend refused pause",
                   {{"reason", "backend_pause_refused"}}});
      if (!error.details.contains("reason")) {
        error.details["reason"] = "backend_pause_refused";
      }
      sm_.set_last_error(error_code_from_dto(error));
      auto transition =
          sm_.try_transition(ExecutionState::kInterventionRequired);
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.state = sm_.status().state;
      snapshot.error = error;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kErrorRaised,
                  sm_.status().execution_id,
                  clock_.now(),
                  {},
                  {},
                  error,
                  error.details});
      if (transition.ok()) {
        emit_event({KernelEventType::kStateTransition,
                    sm_.status().execution_id,
                    clock_.now(),
                    ExecutionState::kStopping,
                    ExecutionState::kInterventionRequired,
                    std::nullopt,
                    {{"reason", "backend_pause_refused"}}});
      }
      return PauseResultDTO{false, error};
    }
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kPauseRequested, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt, {}});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id, clock_.now(),
                prev_state, ExecutionState::kStopping,
                std::nullopt, {}});
    return PauseResultDTO{true, std::nullopt};
  }
  auto error = result.error();
  if (error.code == to_string(ErrorCode::kInvalidStateTransition)) {
    error = ErrorDTO{kErrorInvalidRequest,
                     "Pause is only valid during Executing",
                     {{"reason_code", "pause_not_executing"},
                      {"state", to_string(prev_state)}}};
  }
  return PauseResultDTO{false, error};
}

ResumeResultDTO ExecutionKernel::resume() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_resume();
  if (result.ok()) {
    resume_requested_.store(true);
    // Audit 6D: wake the paused worker immediately (it waits on
    // completion_cv_ keyed on resume_requested_ instead of sleep-polling).
    completion_cv_.notify_all();
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

MotionInterruptStopCapabilityDTO
ExecutionKernel::get_motion_interrupt_stop_capability() const {
  return execution_backend_.get_motion_interrupt_stop_capability();
}

ResetResultDTO ExecutionKernel::reset() {
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_,
                                              std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
      lifecycle_reset_requested_while_busy_.store(true,
                                                  std::memory_order_release);
      if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
        return ResetResultDTO{true, std::nullopt};
      }
    }
    lifecycle_lock.lock();
  }
  lifecycle_stop_requested_while_busy_.store(false,
                                             std::memory_order_seq_cst);
  lifecycle_stop_requested_event_emitted_.store(false,
                                               std::memory_order_seq_cst);
  lifecycle_reset_requested_while_busy_.store(false, std::memory_order_release);

  auto prev_state = sm_.status().state;
  if (get_active_interrupt_latch()) {
    auto clear = interrupt_manager_.clear_active_latch("reset", clock_.now());
    if (clear.ok()) {
      emit_interrupt_events(clear.value());
    }
  }

  const auto clear_operator_stop_errors = [&]() {
    std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
    operator_stop_terminal_error_.reset();
    operator_stop_intervention_error_.reset();
  };

  const auto enter_reset_intervention =
      [&](ErrorDTO error, ExecutionState from_state) -> ResetResultDTO {
    error = ensure_operator_intervention_details(std::move(error),
                                                 "backend_reset",
                                                 from_state,
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
    if (from_state != ExecutionState::kInterventionRequired) {
      emit_event({KernelEventType::kStateTransition, sm_.status().execution_id,
                  clock_.now(), from_state,
                  ExecutionState::kInterventionRequired, std::nullopt, {}});
    }
    return ResetResultDTO{false, error};
  };

  const auto publish_idle_after_reset =
      [&](ExecutionState from_state, bool emit_transition) -> ResetResultDTO {
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
    clear_operator_stop_errors();
    lifecycle_stop_requested_while_busy_.store(false,
                                               std::memory_order_seq_cst);
    lifecycle_stop_requested_event_emitted_.store(false,
                                                 std::memory_order_seq_cst);
    lifecycle_reset_requested_while_busy_.store(false,
                                                std::memory_order_release);
    if (emit_transition) {
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id, clock_.now(), from_state,
                  ExecutionState::kIdle, std::nullopt, {}});
    }
    return ResetResultDTO{true, std::nullopt};
  };

  const auto reset_backend_and_state =
      [&](ExecutionState from_state, bool emit_transition) -> ResetResultDTO {
    auto backend_reset = execution_backend_.reset();
    if (!backend_reset.accepted) {
      return enter_reset_intervention(
          backend_reset.error.value_or(
              operator_intervention_error(ErrorCode::kResetFailed,
                                          "Execution backend reset failed",
                                          "backend_reset",
                                          from_state,
                                          sm_.status().execution_id,
                                          "")),
          from_state);
    }

    if (sm_.status().state != ExecutionState::kIdle) {
      auto result = stop_policy_.request_reset();
      if (!result.ok()) {
        return ResetResultDTO{false, result.error()};
      }
    }
    sm_.set_execution_id(0);
    return publish_idle_after_reset(from_state, emit_transition);
  };

  if (prev_state == ExecutionState::kIdle) {
    auto backend_reset = execution_backend_.reset();
    if (!backend_reset.accepted) {
      return backend_reset;
    }
    return publish_idle_after_reset(prev_state, /*emit_transition=*/false);
  }

  stop_collision_monitoring();
  resume_requested_.store(false);

  const auto operator_stop_timeout = [&]() {
    if (config_.operator_stop_profile) {
      return effective_controlled_stop_deadline_duration(
          *config_.operator_stop_profile,
          config_.complete_move_on_signal_timeout_ns);
    }
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::seconds(1));
  };

  const auto hard_stop_before_reset =
      [&](ExecutionState from_state, std::string reason) -> std::optional<ErrorDTO> {
    auto result = stop_policy_.request_hard_stop();
    if (!result.ok()) {
      return result.error();
    }
    stop_collision_monitoring();
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStopIssued, sm_.status().execution_id,
                clock_.now(), {}, {}, std::nullopt, {}});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id, clock_.now(), from_state,
                sm_.status().state, std::nullopt, {{"reason", reason}}});
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.stops_issued++;
    return std::nullopt;
  };

  const auto wait_for_selected_stop =
      [&](ExecutionState from_state) -> std::optional<ErrorDTO> {
    lifecycle_reset_waiting_for_stop_.store(true, std::memory_order_release);
    lifecycle_lock.unlock();
    const bool stopped = wait_for_operator_stop_worker(operator_stop_timeout());
    lifecycle_lock.lock();
    lifecycle_reset_waiting_for_stop_.store(false, std::memory_order_release);
    join_finished_operator_stop_worker();

    if (!stopped) {
      cancel_operator_stop_worker(/*invalidate_generation=*/true);
      auto hard_stop_error = hard_stop_before_reset(
          from_state, "reset_operator_stop_timeout_hard_stop");
      lifecycle_reset_waiting_for_stop_.store(true,
                                              std::memory_order_release);
      lifecycle_lock.unlock();
      const bool drained =
          wait_for_operator_stop_worker(std::chrono::milliseconds(100));
      lifecycle_lock.lock();
      lifecycle_reset_waiting_for_stop_.store(false,
                                              std::memory_order_release);
      join_finished_operator_stop_worker();
      if (!drained) {
        return ErrorDTO{
            kErrorStopFailed,
            "Operator stop worker did not quiesce after reset hard-stop escalation",
            {{"reason", "operator_stop_worker_stuck_after_hard_stop"}}};
      }
      return hard_stop_error;
    }
    if (sm_.status().state == ExecutionState::kInterventionRequired) {
      std::optional<ErrorDTO> error;
      {
        std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
        error = operator_stop_intervention_error_;
      }
      return error.value_or(ErrorDTO{
          kErrorStopFailed,
          "Operator stop entered InterventionRequired before reset",
          {{"reason", "operator_stop_intervention_before_reset"}}});
    }
    if (sm_.status().state != ExecutionState::kStopped) {
      return hard_stop_before_reset(from_state,
                                    "reset_operator_stop_unexpected_state_hard_stop");
    }
    return std::nullopt;
  };

  if ((prev_state == ExecutionState::kExecuting ||
       prev_state == ExecutionState::kPaused) &&
      config_.operator_stop_profile &&
      operator_controlled_stop_allowed(prev_state)) {
    emit_event({KernelEventType::kStopRequested, sm_.status().execution_id,
                clock_.now(), {}, {}, std::nullopt,
                {{"reason", "reset_stop_before_reset"}}});
    const bool transitioned_to_stopping =
        sm_.try_transition(ExecutionState::kStopping).ok();
    if (transitioned_to_stopping) {
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id, clock_.now(), prev_state,
                  ExecutionState::kStopping, std::nullopt,
                  {{"reason", "reset_stop_before_reset"}}});
    }
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    if (start_operator_stop_worker(
            *config_.operator_stop_profile,
            transitioned_to_stopping ? ExecutionState::kStopping : prev_state)) {
      if (auto error = wait_for_selected_stop(
              transitioned_to_stopping ? ExecutionState::kStopping
                                       : prev_state)) {
        return enter_reset_intervention(*error, prev_state);
      }
    } else {
      auto hard_stop_error = hard_stop_before_reset(
          transitioned_to_stopping ? ExecutionState::kStopping : prev_state,
          "reset_operator_stop_worker_unavailable_hard_stop");
      if (operator_stop_worker_active_.load(std::memory_order_acquire)) {
        return enter_reset_intervention(
            ErrorDTO{kErrorStopFailed,
                     "Operator stop worker remained active after reset hard-stop fallback",
                     {{"reason", "operator_stop_worker_active_after_hard_stop"}}},
            prev_state);
      }
      if (hard_stop_error) {
        return enter_reset_intervention(*hard_stop_error, prev_state);
      }
    }
  } else if (prev_state == ExecutionState::kStopping &&
             operator_stop_worker_active_.load(std::memory_order_acquire)) {
    if (auto error = wait_for_selected_stop(ExecutionState::kStopping)) {
      return enter_reset_intervention(*error, prev_state);
    }
  } else if (prev_state == ExecutionState::kExecuting ||
             prev_state == ExecutionState::kPaused ||
             prev_state == ExecutionState::kStopping) {
    emit_event({KernelEventType::kStopRequested, sm_.status().execution_id,
                clock_.now(), {}, {}, std::nullopt,
                {{"reason", "reset_hard_stop_before_reset"}}});
    if (auto error = hard_stop_before_reset(
            prev_state == ExecutionState::kStopping
                ? ExecutionState::kStopping
                : prev_state,
            "reset_hard_stop_before_reset")) {
      return enter_reset_intervention(*error, prev_state);
    }
  }

  const auto reset_from_state = sm_.status().state;
  return reset_backend_and_state(reset_from_state, /*emit_transition=*/true);
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
                 {{"reason_code", "wrong_state"},
                  {"state", to_string(status.state)}}}};
  }
  if (status.execution_id != execution_id) {
    return AcknowledgeCompletedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Execution acknowledgement id mismatch",
                 {{"reason_code", "mismatched_execution_id"},
                  {"current_execution_id", std::to_string(status.execution_id)},
                  {"requested_execution_id", std::to_string(execution_id)}}}};
  }

  auto result = sm_.reset();
  if (!result.ok()) {
    return AcknowledgeCompletedResultDTO{false, result.error()};
  }
  ExecutionSnapshot last_completed_snapshot;
  {
    std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
    last_completed_snapshot = published_snapshot_;
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
  snapshot.joint_positions_rad = std::move(last_completed_snapshot.joint_positions_rad);
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
  {
    std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
    operator_stop_terminal_error_.reset();
    operator_stop_intervention_error_.reset();
  }

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

AcknowledgeInterventionResultDTO
ExecutionKernel::acknowledge_intervention_for_retry(
    std::string principal_id, std::string reason) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  const auto status = sm_.status();
  if (principal_id.empty()) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "acknowledge_intervention_for_retry requires principal_id",
                 {{"state", to_string(status.state)}}}};
  }
  if (status.state != ExecutionState::kInterventionRequired) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "acknowledge_intervention_for_retry is valid only from "
                 "InterventionRequired",
                 {{"state", to_string(status.state)}}}};
  }

  ExecutionSnapshot snapshot;
  try {
    snapshot = execution_backend_.get_execution_snapshot();
  } catch (const std::exception& exc) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Cannot acknowledge intervention for retry: backend state is "
                 "unreadable",
                 {{"reason", "backend_snapshot_unreadable"},
                  {"exception", exc.what()}}}};
  }
  if (snapshot.state == ExecutionState::kStopping) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Cannot acknowledge intervention for retry while backend is "
                 "still stopping",
                 {{"reason", "backend_still_stopping"}}}};
  }
  if (!all_joint_positions_finite(snapshot)) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Cannot acknowledge intervention for retry: backend pose is "
                 "missing or non-finite",
                 {{"reason", "backend_pose_unreadable"}}}};
  }
  if (!all_joint_velocities_finite_when_present(snapshot)) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Cannot acknowledge intervention for retry: measured "
                 "velocity is non-finite",
                 {{"reason", "backend_velocity_non_finite"}}}};
  }

  std::optional<ErrorDTO> intervention_error;
  {
    std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
    intervention_error = operator_stop_intervention_error_;
  }
  if (!intervention_error.has_value() && snapshot.error.has_value()) {
    intervention_error = snapshot.error;
  }
  if (!intervention_error.has_value()) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Cannot acknowledge intervention for retry: missing "
                 "classified intervention error",
                 {{"reason", "intervention_error_missing"}}}};
  }
  if (!is_classified_stop_intervention(intervention_error->details)) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Cannot acknowledge intervention for retry: intervention is "
                 "not a classified stop/fault boundary",
                 {{"reason", "intervention_not_retryable"},
                  {"error_code", intervention_error->code}}}};
  }

  try {
    const auto cap = execution_backend_.get_motion_interrupt_stop_capability();
    if (!cap.hard_stop_level.has_value() || cap.hard_stop_level->empty()) {
      return AcknowledgeInterventionResultDTO{
          false,
          ErrorDTO{kErrorInvalidRequest,
                   "Cannot acknowledge intervention for retry: backend hard "
                   "stop floor is not reportable",
                   {{"reason", "hard_stop_floor_unreportable"}}}};
    }
  } catch (const std::exception& exc) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Cannot acknowledge intervention for retry: backend stop "
                 "capability is unreadable",
                 {{"reason", "stop_capability_unreadable"},
                  {"exception", exc.what()}}}};
  }

  const double settle_threshold =
      parse_positive_double_detail(intervention_error->details,
                                   "settle_velocity_threshold_rad_s")
          .value_or(kDefaultSettleVelocityThresholdRadS);
  const auto max_abs_velocity =
      measured_max_abs_velocity_from_snapshot(snapshot);
  if (max_abs_velocity.has_value() && *max_abs_velocity > settle_threshold) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Cannot acknowledge intervention for retry: robot is not "
                 "settled",
                 {{"reason", "robot_not_settled"},
                  {"measured_max_abs_velocity_rad_s",
                   std::to_string(*max_abs_velocity)},
                  {"settle_velocity_threshold_rad_s",
                   std::to_string(settle_threshold)}}}};
  }
  const auto reason_it = intervention_error->details.find("reason");
  if (reason_it != intervention_error->details.end() &&
      reason_it->second == "stop_not_settled_measured_velocity" &&
      !max_abs_velocity.has_value() &&
      !is_command_authoritative_stop_snapshot(snapshot)) {
    return AcknowledgeInterventionResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Cannot acknowledge measured-settle intervention for retry: "
                 "measured velocity is unavailable",
                 {{"reason", "settle_signal_unavailable"}}}};
  }

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

  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = 0;
  snapshot.move_id.clear();
  snapshot.state = ExecutionState::kIdle;
  snapshot.progress = 0.0;
  snapshot.error.reset();
  publish_execution_snapshot(std::move(snapshot));
  {
    std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
    operator_stop_terminal_error_.reset();
    operator_stop_intervention_error_.reset();
  }

  emit_event({KernelEventType::kInterventionAcknowledged,
              prev_execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"principal_id", principal_id},
               {"reason", reason},
               {"prev_state", to_string(prev_state)},
               {"recovery_mode", "acknowledge_and_continue"},
               {"backend_reset_performed", "false"}}});
  emit_event({KernelEventType::kStateTransition,
              prev_execution_id,
              clock_.now(),
              prev_state,
              ExecutionState::kIdle,
              std::nullopt,
              {{"reason", "intervention_acknowledged_for_retry"}}});

  return AcknowledgeInterventionResultDTO{true, std::nullopt};
}

AcknowledgeInterruptedResultDTO
ExecutionKernel::acknowledge_interrupted_for_retry(
    std::string principal_id, std::string reason) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  const auto status = sm_.status();
  if (principal_id.empty()) {
    return AcknowledgeInterruptedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "acknowledge_interrupted_for_retry requires principal_id",
                 {{"state", to_string(status.state)}}}};
  }
  if (status.state != ExecutionState::kInterrupted) {
    return AcknowledgeInterruptedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "acknowledge_interrupted_for_retry is valid only from Interrupted",
                 {{"state", to_string(status.state)}}}};
  }

  auto latch = interrupt_manager_.active_latch();
  if (!latch) {
    return AcknowledgeInterruptedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Interrupted execution has no active interrupt latch",
                 {{"state", to_string(status.state)}}}};
  }
  if (latch->source_id != kRuntimeCollisionFaultSourceId) {
    return AcknowledgeInterruptedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Only runtime collision interrupts may be acknowledged for retry",
                 {{"latch_id", latch->latch_id},
                  {"source_id", latch->source_id},
                  {"policy_id", latch->policy_id}}}};
  }

  InterruptDisposition disposition;
  disposition.latch_id = latch->latch_id;
  disposition.action = InterruptDispositionAction::kAcknowledge;
  disposition.principal_id = std::move(principal_id);
  disposition.reason = std::move(reason);

  auto disposition_result =
      interrupt_manager_.apply_disposition(disposition, clock_.now());
  if (!disposition_result.ok()) {
    return AcknowledgeInterruptedResultDTO{false, disposition_result.error()};
  }
  emit_interrupt_events(disposition_result.value());
  if (disposition_result.value().rejected) {
    return AcknowledgeInterruptedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Interrupt disposition rejected",
                 {{"reason", disposition_result.value().reason},
                  {"latch_id", disposition.latch_id}}}};
  }

  // Important: do not call execution_backend_.reset() here. This path is an
  // operator retry after the collision condition was corrected; the robot must
  // stay at its observed stopped pose so the next execute plans from there.
  auto reset_result = stop_policy_.request_reset();
  if (!reset_result.ok()) {
    return AcknowledgeInterruptedResultDTO{false, reset_result.error()};
  }

  const auto prev_execution_id = status.execution_id;
  sm_.set_execution_id(0);
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

  emit_event({KernelEventType::kStateTransition,
              prev_execution_id,
              clock_.now(),
              ExecutionState::kInterrupted,
              ExecutionState::kIdle,
              std::nullopt,
              {{"reason", "interrupted_retry_acknowledged"}}});

  return AcknowledgeInterruptedResultDTO{true, std::nullopt};
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
  const auto kernel_status = sm_.status();
  const auto kernel_state = kernel_status.state;
  // Live backend read during kStopping (1B: the worker may be blocked inside
  // execute_controlled_stop and cannot pump the cache, yet stop observability
  // is safety-relevant) AND during kExecuting (visual/observer freshness: the
  // backend samples the trajectory every kExecutionTick, and serving the
  // cached copy makes display consumers see joints up to a publish-cycle
  // stale — the native-viewer flicker). The kernel remains the lifecycle
  // authority: state and execution identity are stamped from the state
  // machine, never trusted from the backend's view.
  if (kernel_state == ExecutionState::kStopping ||
      kernel_state == ExecutionState::kExecuting) {
    auto backend_snapshot = execution_backend_.get_execution_snapshot();
    backend_snapshot.state = kernel_state;
    backend_snapshot.execution_id = kernel_status.execution_id;
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      backend_snapshot.move_id = active_move_id_;
    }
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
  if (kernel_state == ExecutionState::kStopped ||
      kernel_state == ExecutionState::kCompleted ||
      kernel_state == ExecutionState::kFailed ||
      kernel_state == ExecutionState::kInterventionRequired ||
      kernel_state == ExecutionState::kInterrupted ||
      kernel_state == ExecutionState::kIdle) {
    snapshot.state = kernel_state;
    snapshot.execution_id = kernel_status.execution_id;
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      snapshot.move_id = active_move_id_;
    }
  }
  if (!snapshot.error.has_value() &&
      (kernel_state == ExecutionState::kStopped ||
       kernel_state == ExecutionState::kInterventionRequired)) {
    std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
    if (kernel_state == ExecutionState::kStopped &&
        operator_stop_terminal_error_.has_value()) {
      snapshot.error = operator_stop_terminal_error_;
    } else if (kernel_state == ExecutionState::kInterventionRequired &&
               operator_stop_intervention_error_.has_value()) {
      snapshot.error = operator_stop_intervention_error_;
    }
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
  const bool trip_stop_active =
      motion_trip_stop_active_.load(std::memory_order_acquire);
  // The monitor runs over the decelerating robot during a motion-trip stop
  // (state kStopping) as well as during normal execution (kExecuting).
  if (!trip_stop_active && prev_state != ExecutionState::kExecuting) {
    return Result<void>::Ok();
  }

  auto publish_runtime_monitor_fault =
      [this](CollisionCheckResultDTO check,
             std::map<std::string, std::string> details) -> Result<void> {
    details["surface"] = "observed_runtime_monitor";
    // Stop-path collision: during an active motion-trip stop, a collision (or
    // any monitor fault that means the stop path cannot be verified clear) must
    // NOT latch a new fault or transition the state machine — the trip handler
    // owns the lifecycle and is mid-stop. Signal it to escalate to a hard stop
    // (monitor signals, worker acts) and cancel the in-flight stop.
    if (motion_trip_stop_active_.load(std::memory_order_acquire)) {
      // Only an ACTUAL detected collision escalates the in-flight stop. A
      // monitor availability/config fault (no contact) must NOT disrupt a
      // stop already in progress — the post-stop confirmation gates still
      // apply — but it MUST be auditable: emit a diagnostic so "collision
      // monitor unavailable during stop" is on the record, then fall through
      // without escalating.
      if (!check.in_collision) {
        details["reason_during_stop"] = "stop_monitor_unavailable";
        KernelEvent monitor_fault_event;
        monitor_fault_event.type = KernelEventType::kErrorRaised;
        monitor_fault_event.execution_id = sm_.status().execution_id;
        monitor_fault_event.timestamp_ns = clock_.now();
        monitor_fault_event.details = details;
        monitor_fault_event.error = ErrorDTO{
            std::string(kErrorExecutionAborted),
            "runtime collision monitor unavailable during controlled stop",
            details};
        emit_event(monitor_fault_event);
        return Result<void>::Ok();
      }
      details["reason_during_stop"] = "stop_path_collision";
      KernelEvent stop_collision_event;
      stop_collision_event.type = KernelEventType::kCollisionDetected;
      stop_collision_event.execution_id = sm_.status().execution_id;
      stop_collision_event.timestamp_ns = clock_.now();
      stop_collision_event.details = details;
      if (check.contact_body_a) {
        stop_collision_event.details["contact_a"] = *check.contact_body_a;
      }
      if (check.contact_body_b) {
        stop_collision_event.details["contact_b"] = *check.contact_body_b;
      }
      emit_event(stop_collision_event);
      {
        std::lock_guard<std::mutex> lk(motion_trip_stop_mutex_);
        motion_trip_stop_collision_.store(true, std::memory_order_release);
        if (motion_trip_stop_source_.has_value()) {
          motion_trip_stop_source_->request_stop();
        }
      }
      return Result<void>::Ok();
    }
    KernelEvent collision_event;
    collision_event.type = KernelEventType::kCollisionDetected;
    collision_event.execution_id = sm_.status().execution_id;
    collision_event.timestamp_ns = clock_.now();
    collision_event.details = details;
    if (check.contact_body_a) {
      collision_event.details["contact_a"] = *check.contact_body_a;
    }
    if (check.contact_body_b) {
      collision_event.details["contact_b"] = *check.contact_body_b;
    }
    emit_event(collision_event);
    {
      std::lock_guard<std::mutex> collision_metrics_lock(metrics_mutex_);
      metrics_.collision_detections++;
    }
    cache_.invalidate_all();
    auto interrupt = publish_runtime_collision_fault_interrupt(
        check, collision_event.details);
    if (interrupt.rejected) {
      return Result<void>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "Runtime collision interrupt signal rejected",
                   {{"reason", interrupt.reason}}});
    }
    return Result<void>::Ok();
  };

  std::optional<CollisionPolicySnapshot> policy_snapshot;
  ICollisionWorldSnapshotPublisher* snapshot_publisher = nullptr;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines;
  {
    std::scoped_lock lock(coordinator_mutex_);
    policy_snapshot = coordinator_policy_snapshot_;
    snapshot_publisher = collision_snapshot_publisher_;
    validation_engines = collision_validation_engines_;
  }

  std::map<std::string, std::string> monitor_details;
  monitor_details["reason_code"] = "runtime_collision_monitor";
  if (snapshot_publisher == nullptr) {
    monitor_details["reason"] = "missing_collision_world_snapshot_publisher";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }
  if (validation_engines.empty()) {
    monitor_details["reason"] = "missing_collision_query_engines";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }

  CollisionWorldSnapshotPtr world_snapshot;
  try {
    world_snapshot = snapshot_publisher->snapshot();
  } catch (const std::exception& e) {
    monitor_details["reason"] = "snapshot_publisher_exception";
    monitor_details["exception"] = e.what();
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  } catch (...) {
    monitor_details["reason"] = "snapshot_publisher_exception";
    monitor_details["exception"] = "unknown";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }
  if (!world_snapshot) {
    monitor_details["reason"] = "snapshot_unavailable";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }

  try {
    if (auto publisher_policy =
            snapshot_publisher->collision_policy_snapshot()) {
      policy_snapshot = std::move(publisher_policy);
    }
  } catch (const std::exception& e) {
    monitor_details["reason"] = "policy_publisher_exception";
    monitor_details["exception"] = e.what();
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  } catch (...) {
    monitor_details["reason"] = "policy_publisher_exception";
    monitor_details["exception"] = "unknown";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }
  if (!policy_snapshot) {
    monitor_details["reason"] = "missing_collision_policy_snapshot";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }

  ICollisionQueryEngine* engine = nullptr;
  for (const auto& candidate : validation_engines) {
    if (candidate) {
      engine = candidate.get();
      break;
    }
  }
  if (engine == nullptr) {
    monitor_details["reason"] = "missing_collision_query_engines";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }

  const auto dynamic_body_now_ns = clock_.now();
  if (auto freshness_reason = validate_dynamic_body_freshness(
          *world_snapshot,
          dynamic_body_now_ns,
          config_.runtime_dynamic_body_max_age_cap_ns,
          monitor_details)) {
    monitor_details["reason"] = *freshness_reason;
    monitor_details["collision_world_revision_id"] =
        std::to_string(world_snapshot->revision.revision_id);
    monitor_details["collision_world_content_hash"] =
        world_snapshot->revision.content_hash;
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{},
                                         monitor_details);
  }

  ExecutionSnapshot observed_snapshot;
  try {
    observed_snapshot = execution_backend_.get_execution_snapshot();
  } catch (const std::exception& e) {
    monitor_details["reason"] = "observed_state_unavailable";
    monitor_details["exception"] = e.what();
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  } catch (...) {
    monitor_details["reason"] = "observed_state_unavailable";
    monitor_details["exception"] = "unknown";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }
  const auto observed_now_ns = clock_.now();
  if (snapshot_too_old(observed_now_ns,
                       observed_snapshot.timestamp_ns,
                       config_.runtime_observed_state_max_age_ns,
                       monitor_details,
                       "observed_state_timestamp_ns",
                       "observed_state_age_ns",
                       "observed_state_freshness_budget_ns")) {
    monitor_details["reason"] = "stale_observed_robot_state";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }
  if (observed_snapshot.joint_positions_rad.empty()) {
    monitor_details["reason"] = "observed_state_missing_joint_positions";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }

  RobotStateDTO context_state;
  try {
    context_state = context_.get_current_state();
  } catch (const std::exception& e) {
    monitor_details["reason"] = "joint_name_state_unavailable";
    monitor_details["exception"] = e.what();
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  } catch (...) {
    monitor_details["reason"] = "joint_name_state_unavailable";
    monitor_details["exception"] = "unknown";
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }
  if (context_state.joint_names.size() !=
      observed_snapshot.joint_positions_rad.size()) {
    monitor_details["reason"] = "observed_state_joint_count_mismatch";
    monitor_details["joint_name_count"] =
        std::to_string(context_state.joint_names.size());
    monitor_details["observed_joint_position_count"] =
        std::to_string(observed_snapshot.joint_positions_rad.size());
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }

  TrajectoryDTO one_state;
  one_state.joint_names = context_state.joint_names;
  one_state.waypoints.push_back(TrajectoryWaypointDTO{
      0.0,
      observed_snapshot.joint_positions_rad,
      {},
      {}});
  auto expanded_one_state_result =
      context_.expand_trajectory_for_collision_validation(one_state);
  if (!expanded_one_state_result.ok()) {
    const auto error = expanded_one_state_result.error();
    monitor_details["reason"] = "runtime_state_expansion_failed";
    monitor_details["expansion_error_code"] = error.code;
    monitor_details["expansion_error_message"] = error.message;
    for (const auto& [key, value] : error.details) {
      monitor_details["expansion_" + key] = value;
    }
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{},
                                         monitor_details);
  }
  one_state = std::move(expanded_one_state_result.value());

  auto runtime_policy_snapshot = *policy_snapshot;
  for (auto& entry : runtime_policy_snapshot.support_contacts) {
    if (entry.mode == CollisionContactPolicyMode::kContactRelease) {
      // Runtime ticks are single observed states, not complete trajectories.
      // The per-move validation proves final separation; the runtime monitor
      // enforces bounded/non-increasing penetration across ticks below.
      entry.require_separation_by_end = false;
      entry.require_strictly_decreasing_penetration = false;
    }
  }

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = runtime_policy_snapshot;
  cfg.require_pose_source_borrow = false;
  cfg.surface = CollisionSurface::kObservedRuntimeMonitor;
  cfg.per_state_timeout_ns =
      static_cast<std::int64_t>(config_.runtime_collision_query_timeout_ns);
  CollisionValidationCoordinator coord(std::move(cfg));
  CollisionCancellationToken cancel;
  CollisionValidationDecisionBundle bundle;
  try {
    bundle = coord.validate_trajectory(*world_snapshot, *engine, one_state, cancel);
  } catch (const std::exception& e) {
    monitor_details["reason"] = "runtime_collision_engine_exception";
    monitor_details["exception"] = e.what();
    monitor_details["collision_world_revision_id"] =
        std::to_string(world_snapshot->revision.revision_id);
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  } catch (...) {
    monitor_details["reason"] = "runtime_collision_engine_exception";
    monitor_details["exception"] = "unknown";
    monitor_details["collision_world_revision_id"] =
        std::to_string(world_snapshot->revision.revision_id);
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, monitor_details);
  }

  if (bundle.decision.accepted) {
    if (auto release_fault =
            update_runtime_contact_release_state(*policy_snapshot, bundle)) {
      if (release_fault->find("reason") == release_fault->end()) {
        (*release_fault)["reason"] = "runtime_contact_release_monitor";
      }
      (*release_fault)["collision_world_revision_id"] =
          std::to_string(bundle.borrowed_world_revision.revision_id);
      (*release_fault)["collision_world_content_hash"] =
          bundle.borrowed_world_revision.content_hash;
      append_collision_failure_diagnostics(*release_fault, bundle);
      CollisionCheckResultDTO check;
      check.in_collision = true;
      auto contact_a = release_fault->find("contact_a");
      if (contact_a != release_fault->end()) {
        check.contact_body_a = contact_a->second;
      }
      auto contact_b = release_fault->find("contact_b");
      if (contact_b != release_fault->end()) {
        check.contact_body_b = contact_b->second;
      }
      return publish_runtime_monitor_fault(check, std::move(*release_fault));
    }
    return Result<void>::Ok();
  }

  monitor_details["reason"] =
      bundle.decision.action == CollisionDecisionAction::kInconclusive
          ? "runtime_collision_monitor_inconclusive"
          : "runtime_collision_detected";
  monitor_details["collision_world_revision_id"] =
      std::to_string(bundle.borrowed_world_revision.revision_id);
  monitor_details["collision_world_content_hash"] =
      bundle.borrowed_world_revision.content_hash;
  append_collision_failure_diagnostics(monitor_details, bundle);
  return publish_runtime_monitor_fault(collision_check_result_from_bundle(bundle),
                                       std::move(monitor_details));
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
  plan_req.per_joint_dynamics_limits = request.per_joint_dynamics_limits;
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

Result<TrajectoryDTO> ExecutionKernel::plan_resume_trajectory(
    const ExecuteRequestDTO& request) {
  auto sync_result = execution_backend_.sync_observed_state_before_planning();
  if (!sync_result.ok()) {
    auto error = sync_result.error();
    error.details["resume_phase"] = "state_sync";
    return Result<TrajectoryDTO>::Err(std::move(error));
  }

  try {
    auto planned = plan_trajectory(request);
    if (!planned.ok()) {
      auto error = planned.error();
      error.details["resume_phase"] = "planning";
      return Result<TrajectoryDTO>::Err(std::move(error));
    }
    return Result<TrajectoryDTO>::Ok(std::move(planned.value()));
  } catch (const std::exception& e) {
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        kErrorPlanningFailed,
        "Resume planning could not read the fresh current state",
        {{"reason", "resume_current_state_unavailable"},
         {"resume_phase", "current_state"},
         {"exception", e.what()}}});
  } catch (...) {
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        kErrorPlanningFailed,
        "Resume planning could not read the fresh current state",
        {{"reason", "resume_current_state_unavailable"},
         {"resume_phase", "current_state"}}});
  }
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
  // spec 230 §3.3). The manyforge_collision pipeline is the only
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

  auto collision_trajectory_result =
      context_.expand_trajectory_for_collision_validation(trajectory);
  if (!collision_trajectory_result.ok()) {
    return Result<CollisionWorldRevision>::Err(
        std::move(collision_trajectory_result.error()));
  }
  auto collision_trajectory = std::move(collision_trajectory_result.value());

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

  struct AuthoritativeValidationAttempt {
    CollisionValidationDecisionBundle bundle;
    ParallelValidationTelemetry telemetry;
  };

  auto run_authoritative_validation_once =
      [&]() -> Result<AuthoritativeValidationAttempt> {
    CollisionWorldSnapshotPtr world_snapshot;
    try {
      world_snapshot = snapshot_publisher->snapshot();
    } catch (const std::exception& e) {
      return Result<AuthoritativeValidationAttempt>::Err(
          authoritative_collision_validation_config_error(
              "snapshot_publisher_exception",
              {{"exception", e.what()}}));
    } catch (...) {
      return Result<AuthoritativeValidationAttempt>::Err(
          authoritative_collision_validation_config_error(
              "snapshot_publisher_exception",
              {{"exception", "unknown"}}));
    }
    if (!world_snapshot) {
      return Result<AuthoritativeValidationAttempt>::Err(
          authoritative_collision_validation_config_error(
              "snapshot_unavailable"));
    }

    auto active_policy = snapshot;
    try {
      if (auto publisher_policy =
              snapshot_publisher->collision_policy_snapshot()) {
        active_policy = std::move(publisher_policy);
      }
    } catch (const std::exception& e) {
      return Result<AuthoritativeValidationAttempt>::Err(
          authoritative_collision_validation_config_error(
              "policy_publisher_exception",
              {{"exception", e.what()}}));
    } catch (...) {
      return Result<AuthoritativeValidationAttempt>::Err(
          authoritative_collision_validation_config_error(
              "policy_publisher_exception",
              {{"exception", "unknown"}}));
    }
    if (!active_policy) {
      return Result<AuthoritativeValidationAttempt>::Err(
          authoritative_collision_validation_config_error(
              "missing_collision_policy_snapshot"));
    }

    CollisionValidationPassConfig cfg;
    cfg.policy_snapshot = *active_policy;
    cfg.pose_source_retry = retry;
    cfg.per_state_timeout_ns = per_state_timeout_ns;
    cfg.surface = CollisionSurface::kPreExecutionValidation;

    ParallelCollisionValidationWorkerPool::Config pool_cfg;
    pool_cfg.pass_config = std::move(cfg);
    pool_cfg.worker_count = config_.validation_collision_workers;
    pool_cfg.min_waypoints_for_parallel =
        config_.validation_collision_min_waypoints_for_parallel;
    ParallelCollisionValidationWorkerPool pool(std::move(pool_cfg));

    AuthoritativeValidationAttempt attempt;
    attempt.bundle =
        pool.validate_trajectory(*world_snapshot, engine_ptrs,
                                 collision_trajectory, *cancel);
    attempt.telemetry = pool.last_telemetry();
    return Result<AuthoritativeValidationAttempt>::Ok(std::move(attempt));
  };

  auto attempt_result = run_authoritative_validation_once();
  if (!attempt_result.ok()) {
    return Result<CollisionWorldRevision>::Err(attempt_result.error());
  }

  auto attempt = std::move(attempt_result.value());
  auto bundle = std::move(attempt.bundle);
  auto telemetry = attempt.telemetry;

  if (bundle.decision.accepted) {
    return Result<CollisionWorldRevision>::Ok(bundle.borrowed_world_revision);
  }
  std::map<std::string, std::string> details;
  if (bundle.decision.first_failing_waypoint) {
    details["first_failing_waypoint"] =
        std::to_string(*bundle.decision.first_failing_waypoint);
    if (*bundle.decision.first_failing_waypoint >= 0 &&
        static_cast<std::size_t>(*bundle.decision.first_failing_waypoint) <
            collision_trajectory.waypoints.size()) {
      const auto first_failure_index =
          static_cast<std::size_t>(*bundle.decision.first_failing_waypoint);
      const auto& waypoint = collision_trajectory.waypoints[first_failure_index];
      details["collision_result_first_failure_joint_names"] =
          comma_join_strings(collision_trajectory.joint_names);
      details["collision_result_first_failure_positions_rad"] =
          comma_join_doubles(waypoint.positions_rad);
      details["collision_result_first_failure_time_from_start_s"] =
          std::to_string(waypoint.time_from_start_s);
    }
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
      std::to_string(telemetry.workers_actually_used);
  if (!telemetry.fallback_reason.empty()) {
    details["parallel_fallback_reason"] = telemetry.fallback_reason;
  }
  append_collision_failure_diagnostics(details, bundle);
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
  if (config_.enable_legacy_collision_compare_diagnostics) {
    append_legacy_collision_compare_diagnostics(details, bundle, trajectory,
                                                context_);
  }
  return Result<CollisionWorldRevision>::Err(
      ErrorDTO{kErrorValidationCollision,
               "Collision validation rejected trajectory",
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

void ExecutionKernel::begin_runtime_velocity_scale_freeze() {
  std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
  ++runtime_velocity_scale_freeze_depth_;
}

void ExecutionKernel::end_runtime_velocity_scale_freeze() {
  std::optional<double> deferred_scale;
  std::optional<std::string> deferred_principal_id;
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    if (runtime_velocity_scale_freeze_depth_ <= 0) {
      return;
    }
    --runtime_velocity_scale_freeze_depth_;
    if (runtime_velocity_scale_freeze_depth_ == 0 &&
        deferred_runtime_velocity_scale_.has_value()) {
      deferred_scale = deferred_runtime_velocity_scale_;
      deferred_principal_id = deferred_runtime_velocity_scale_principal_id_;
      deferred_runtime_velocity_scale_.reset();
      deferred_runtime_velocity_scale_principal_id_.reset();
    }
  }
  if (deferred_scale.has_value()) {
    set_runtime_velocity_scale(*deferred_scale, deferred_principal_id);
  }
}

void ExecutionKernel::set_runtime_velocity_scale(
    double scale, std::optional<std::string> principal_id) {
  // ADR-0018: UI runtime velocity scale → backend runtime register.
  // Validate finiteness here; clamp to [0, 1]; forward to the backend's
  // atomic store. No lifecycle_mutex_: this is a high-rate operator surface
  // and the backend's atomic store handles concurrency.
  if (!std::isfinite(scale)) {
    // K2-style defense in depth: refuse NaN/Inf rather than
    // propagating into the worker.
    return;
  }
  if (scale < 0.0) scale = 0.0;
  if (scale > 1.0) scale = 1.0;
  const auto status = sm_.status();
  double previous = 1.0;
  bool deferred = false;
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    previous = runtime_velocity_scale_.load(std::memory_order_acquire);
    if (runtime_velocity_scale_freeze_depth_ > 0) {
      deferred_runtime_velocity_scale_ = scale;
      deferred_runtime_velocity_scale_principal_id_ = principal_id;
      deferred = true;
    } else {
      runtime_velocity_scale_.store(scale, std::memory_order_release);
    }
  }
  if (!deferred) {
    execution_backend_.set_runtime_velocity_scale(scale);
  }
  std::map<std::string, std::string> details{
      {"previous_scale", format_double(previous)},
      {"new_scale", format_double(scale)},
      {"applied_scale", format_double(deferred ? previous : scale)},
      {"backend_status", deferred ? "deferred_controlled_stop_freeze"
                                  : "forwarded"},
      {"source", "kernel_api"},
      {"state", to_string(status.state)}};
  if (principal_id.has_value() && !principal_id->empty()) {
    details["principal_id"] = *principal_id;
  }
  emit_event({KernelEventType::kRuntimeVelocityScaleChanged,
              status.execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              std::move(details)});
}

}  // namespace manyforge::core
