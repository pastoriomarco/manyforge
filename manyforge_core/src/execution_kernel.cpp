#include <manyforge_core/execution_kernel.hpp>

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <limits>
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

  auto controlled_result =
      execution_backend_.execute_controlled_stop(profile, stop_token);
  if (!operator_stop_worker_is_current(generation, stop_token)) {
    return;
  }

  const bool controlled_ok =
      controlled_result.accepted &&
      controlled_result.backend_stop_confirmed &&
      !controlled_result.error.has_value();

  std::optional<StopResultDTO> fallback_legacy_result;
  if (!controlled_ok) {
    fallback_legacy_result = execution_backend_.stop();
    const std::string requested_mode = stop_mode_string(profile.stop_mode);
    const std::string executed_mode =
        stop_mode_string(controlled_result.executed_stop_mode);
    const std::string fallback_outcome =
        fallback_legacy_result->accepted
            ? std::string("legacy_stop_succeeded")
            : std::string("legacy_stop_failed");
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id, clock_.now(), {}, {},
                std::optional<ErrorDTO>{ErrorDTO{
                    std::string(kErrorMotionInterruptBackendStopFailed),
                    "operator stop profile failed; emergency legacy "
                    "backend.stop() fallback engaged",
                    {{"reason", "operator_stop_profile_failed"},
                     {"requested_stop_mode", requested_mode},
                     {"executed_stop_mode", executed_mode},
                     {"emergency_fallback", fallback_outcome},
                     {"phase", "operator_stop"}}}},
                {{"phase", "operator_stop"},
                 {"requested_stop_mode", requested_mode},
                 {"executed_stop_mode", executed_mode},
                 {"emergency_fallback", fallback_outcome}}});
  }

  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!operator_stop_worker_is_current(generation, stop_token)) {
    return;
  }

  const std::string requested_mode = stop_mode_string(profile.stop_mode);
  const std::string executed_mode =
      stop_mode_string(controlled_result.executed_stop_mode);

  if (!controlled_ok && fallback_legacy_result &&
      !fallback_legacy_result->accepted) {
    ErrorDTO total_failure{
        std::string(kErrorMotionInterruptBackendStopFailed),
        "operator stop failed: controlled-stop refused and emergency "
        "legacy backend.stop() also failed",
        {{"reason", "operator_stop_total_failure"},
         {"requested_stop_mode", requested_mode},
         {"executed_stop_mode", executed_mode},
         {"emergency_fallback", "legacy_stop_failed"},
         {"phase", "operator_stop"}}};
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
    auto snapshot = get_execution_snapshot();
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
                {{"phase", "operator_stop"},
                 {"requested_stop_mode", requested_mode},
                 {"executed_stop_mode", executed_mode},
                 {"emergency_fallback", "legacy_stop_failed"}}});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                pre_completion_state,
                ExecutionState::kInterventionRequired,
                std::nullopt,
                {{"reason", "operator_stop_total_failure"}}});
    return;
  }

  auto transition = sm_.try_transition(ExecutionState::kStopped);
  if (!transition.ok() && sm_.status().state != ExecutionState::kStopped) {
    return;
  }

  auto snapshot = get_execution_snapshot();
  snapshot.timestamp_ns = clock_.now();
  snapshot.state = sm_.status().state;
  if (!controlled_ok && fallback_legacy_result &&
      fallback_legacy_result->accepted) {
    ErrorDTO degraded_stop_error{
        std::string(kErrorMotionInterruptBackendStopFailed),
        "operator stop profile failed; emergency legacy backend.stop() "
        "guaranteed physical stop",
        {{"reason", "operator_stop_profile_failed"},
         {"requested_stop_mode", requested_mode},
         {"executed_stop_mode", executed_mode},
         {"emergency_fallback", "legacy_stop_succeeded"}}};
    {
      std::lock_guard<std::mutex> error_lock(operator_stop_error_mutex_);
      operator_stop_terminal_error_ = degraded_stop_error;
    }
    snapshot.error = std::move(degraded_stop_error);
  }
  publish_execution_snapshot(std::move(snapshot));
  emit_event({KernelEventType::kStopIssued, sm_.status().execution_id,
              clock_.now(), {}, {}, std::nullopt, {}});
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
      preempt_event_details["preempt_path"] = "controlled_stop";
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

SoftOnPathPlannerConfig soft_on_path_planner_config_from_capability(
    const MotionInterruptStopCapabilityDTO& capability) {
  SoftOnPathPlannerConfig config;
  if (!capability.soft_on_path_planner_config.has_value()) {
    return config;
  }
  const auto& declared = *capability.soft_on_path_planner_config;
  config.stop_traj_delta_s = declared.stop_traj_delta_s;
  config.sample_period_s = declared.sample_period_s;
  config.allow_model_velocity_fallback =
      declared.allow_model_velocity_fallback;
  config.max_initial_path_deviation_rad =
      declared.max_initial_path_deviation_rad;
  config.max_initial_velocity_deviation_rad_s =
      declared.max_initial_velocity_deviation_rad_s;
  config.per_joint_max_jerk_rad_s3 =
      declared.per_joint_max_jerk_rad_s3;
  return config;
}

}  // namespace

Result<void> ExecutionKernel::arm_motion_scopes_for_dispatch(
    const ExecuteRequestDTO& request,
    const TrajectoryDTO& trajectory,
    std::uint64_t execution_id,
    ActiveMotionExecution& motion_state) {
  motion_state.execution_id = execution_id;
  motion_state.move_id = request.move_id;
  motion_state.had_scopes = !request.motion_interrupt_scopes.empty();
  motion_state.arm_succeeded = false;
  motion_state.motion_trip_latch.reset();
  motion_state.outcome_emitted = false;
  if (!motion_state.had_scopes) {
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
  for (const auto& scope : request.motion_interrupt_scopes) {
    if (!scope.stop_profile) {
      // Manager will reject this with stop_profile_required if the
      // bound policy is complete_move_on_signal; otherwise the
      // backend gate has nothing to check.
      continue;
    }
    const auto& profile = *scope.stop_profile;
    if (!capability.supported_stop_modes.contains(profile.stop_mode)) {
      return Result<void>::Err(ErrorDTO{
          kErrorMotionInterruptArmFailed,
          "requested stop_mode is not in backend supported_stop_modes",
          {{"reason", "stop_mode_not_supported_by_backend"},
           {"scope_id", scope.scope_id},
           {"policy_id", scope.policy_id}}});
    }
    for (const auto& fallback : profile.allowed_fallback_modes) {
      if (!capability.supported_stop_modes.contains(fallback)) {
        return Result<void>::Err(ErrorDTO{
            kErrorMotionInterruptArmFailed,
            "allowed_fallback_modes entry is not in backend supported_stop_modes",
            {{"reason", "fallback_mode_not_supported_by_backend"},
             {"scope_id", scope.scope_id},
             {"policy_id", scope.policy_id}}});
      }
    }
    if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath) {
      const auto planner_config =
          soft_on_path_planner_config_from_capability(capability);
      const auto envelope = validate_soft_on_path_feasibility_envelope(
          trajectory, profile, planner_config);
      if (!envelope.ok()) {
        ErrorDTO error = envelope.error();
        error.code = kErrorMotionInterruptArmFailed;
        error.details["scope_id"] = scope.scope_id;
        error.details["policy_id"] = scope.policy_id;
        const bool recommends_spring_back =
            error.details.contains("recommended_fallback") &&
            error.details.at("recommended_fallback") == "spring_back_hold";
        const bool allows_spring_back =
            std::find(profile.allowed_fallback_modes.begin(),
                      profile.allowed_fallback_modes.end(),
                      MotionInterruptStopMode::kSpringBackHold) !=
            profile.allowed_fallback_modes.end();
        if (!recommends_spring_back || !allows_spring_back) {
          return Result<void>::Err(std::move(error));
        }
      }
    }
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
  std::array<MotionInterruptStopCommandSource, 3> allowed_command_sources;
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
          MotionInterruptStopCommandSource::kNativeSimulatedStop},
         3,
         "4.5G-soft_on_path-to-spring_back_hold-generic-v1",
         "path-preserving ramp unavailable; generic node accepts observed-state halt",
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
    std::string& failure_reason) {
  ExecutionStopSnapshotDTO snap;
  snap.captured_at_ns = captured_at_ns;
  snap.backend_stop_confirmed = backend_stop_confirmed;

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
    if (post_stop_backend_snapshot.progress > 0.0 &&
        post_stop_backend_snapshot.progress <= 1.0) {
      snap.trajectory_progress = post_stop_backend_snapshot.progress;
    }
    if (!robot_state_size_ordering_ok(snap.robot_state)) {
      failure_reason = "stop_snapshot_robot_state_size_ordering_invalid";
      return std::nullopt;
    }
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
  // so the kernel can validate against `max_stop_time_s` (with the
  // deployment default `complete_move_on_signal_timeout_ns` as the
  // fallback when the scope leaves the field unset) and embed
  // latch->call and call->confirm latencies in the trip outcome.
  const auto call_start_ns = clock_.now();
  auto stop_result = execution_backend_.execute_controlled_stop(profile);
  const auto call_end_ns = clock_.now();

  // Common failure-path downgrader. Industrial behavior (spec/230
  // §3.4 lines 563-564): backend-stop and stop-snapshot failures
  // transition to `InterventionRequired`, not `Failed`. The latch
  // persists for operator/runtime intervention-acknowledge; it is
  // NOT auto-cleared on the failure paths. The outcome record's
  // `terminal_cause` is still `failed` per spec/210 §3.11.
  const auto downgrade_to_intervention =
      [&](std::string reason, std::string message,
          std::string error_code = std::string(
              kErrorMotionInterruptBackendStopFailed)) {
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

  const std::int64_t effective_deadline_ns =
      profile.max_stop_time_s
          ? static_cast<std::int64_t>(*profile.max_stop_time_s * 1e9)
          : config_.complete_move_on_signal_timeout_ns;

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
          "soft_on_path natural completion exceeded max_stop_time_s");
      return;
    }

    sm_.try_transition(ExecutionState::kCompleted);
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

  // (1) Backend accepted the call.
  if (!stop_result.accepted) {
    auto reason = stop_result.error && stop_result.error->details.contains(
                                            "reason")
                      ? stop_result.error->details.at("reason")
                      : std::string("controlled_stop_not_accepted");
    auto msg = stop_result.error ? stop_result.error->message
                                  : "execute_controlled_stop returned accepted=false";
    downgrade_to_intervention(std::move(reason), std::move(msg));
    return;
  }
  // (2) Backend confirmed the stop within max_stop_time_s (or the
  //     deployment default `complete_move_on_signal_timeout_ns` when
  //     the profile leaves `max_stop_time_s` unset).
  if (!stop_result.backend_stop_confirmed) {
    downgrade_to_intervention(
        "backend_stop_not_confirmed",
        "backend did not confirm controlled stop within deadline");
    return;
  }
  if (effective_deadline_ns > 0 &&
      stop_result.confirmation_latency_ns > effective_deadline_ns) {
    downgrade_to_intervention(
        "backend_stop_confirmation_exceeded_max_stop_time_s",
        "controlled-stop confirmation exceeded max_stop_time_s");
    return;
  }
  // (3) Executed mode matches requested OR is an allowed fallback.
  const StopFallbackQualificationRow* fallback_qualification_row = nullptr;
  if (stop_result.executed_stop_mode != profile.stop_mode) {
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
      snap_failure_reason);
  if (!snap_opt) {
    downgrade_to_intervention(
        std::move(snap_failure_reason),
        "controlled-stop snapshot capture or validation failed",
        std::string(kErrorMotionInterruptStopStateInvalid));
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
  sm_.try_transition(ExecutionState::kCompleted);
  emit_event({KernelEventType::kMotionTripCompleted, exec_id, clock_.now(),
               {}, {}, std::nullopt,
               {{"latch_id", latch.latch_id},
                {"scope_id", trip.scope_id},
                {"executed_stop_mode",
                 stop_mode_string(trip.executed_stop_mode)},
                {"stop_command_source",
                 stop_command_source_string(trip.stop_command_source)}}});
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
  std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
  collision_monitor_active_ = false;
  ++collision_monitor_generation_;
  if (collision_monitor_handle_ != 0) {
    scheduler_.cancel(collision_monitor_handle_);
    collision_monitor_handle_ = 0;
  }
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
      if (depth > std::max(0.0, entry.max_start_penetration_m) + tolerance) {
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

  emit_event({KernelEventType::kValidationStarted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});

    auto val_result = validate_trajectory(trajectory, request);
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
    if (request.cache_mode != CacheMode::kDisable && !cache_hit &&
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
          request, trajectory, exec_id, motion_state);
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
          std::this_thread::sleep_for(std::chrono::nanoseconds(kExecutionPollNs));
        }
        continue;
      }

      if (completed) {
        break;
      }
    }

    stop_collision_monitoring();

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
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_release);
  lifecycle_stop_requested_event_emitted_.store(false,
                                               std::memory_order_release);
  lifecycle_reset_requested_while_busy_.store(false, std::memory_order_release);
  if (lifecycle_reset_waiting_for_stop_.load(std::memory_order_acquire)) {
    return Result<uint64_t>::Err(ErrorDTO{
        kErrorExecutionRejected,
        "Reset is waiting for stop completion",
        {{"reason_code", "reset_waiting_for_stop"}}});
  }

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
    execute_admission_interruptible_.store(false, std::memory_order_release);
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

  execute_admission_interruptible_.store(true, std::memory_order_release);

  auto sync_result = execution_backend_.sync_observed_state_before_planning();
  if (!sync_result.ok()) {
    return reject_admission(sync_result.error());
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

  execute_admission_interruptible_.store(false, std::memory_order_release);

  const bool pending_reset =
      lifecycle_reset_requested_while_busy_.exchange(false,
                                                     std::memory_order_acq_rel);
  const bool pending_stop =
      lifecycle_stop_requested_while_busy_.exchange(false,
                                                    std::memory_order_acq_rel);
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
            false, std::memory_order_acq_rel);
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

  execute_admission_interruptible_.store(false, std::memory_order_release);
  return Result<uint64_t>::Ok(exec_id);
}

StopResultDTO ExecutionKernel::stop() {
  join_finished_operator_stop_worker();
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_,
                                              std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    if (execute_admission_interruptible_.load(std::memory_order_acquire)) {
      lifecycle_stop_requested_while_busy_.store(true,
                                                 std::memory_order_release);
      lifecycle_stop_requested_event_emitted_.store(
          true, std::memory_order_release);
      emit_event({KernelEventType::kStopRequested,
                  sm_.status().execution_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"reason", "lifecycle_busy"}}});
      if (execute_admission_interruptible_.load(std::memory_order_acquire)) {
        return StopResultDTO{true, std::nullopt};
      }
    }
    lifecycle_lock.lock();
  }
  if (lifecycle_stop_requested_event_emitted_.exchange(
          false, std::memory_order_acq_rel) &&
      !lifecycle_stop_requested_while_busy_.load(std::memory_order_acquire) &&
      sm_.status().state == ExecutionState::kStopped) {
    return StopResultDTO{true, std::nullopt};
  }
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_release);

  // Per spec/230 §6.1 Stop chain:
  //   stop_requested → state_transition(Stopping) → stop_issued
  //                  → state_transition(Stopped)
  // kStopRequested marks operator intent at the API boundary; the
  // intermediate kStopping covers the controlled-stop deceleration
  // window (1-2 s for soft-on-path), making the "stopping…" period
  // observable to the HMI and to trace reconstruction.
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
                                               std::memory_order_release);
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
                                               std::memory_order_release);
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
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_release);
  return StopResultDTO{true, std::nullopt};
}

PauseResultDTO ExecutionKernel::pause() {
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_,
                                              std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    if (execute_admission_interruptible_.load(std::memory_order_acquire)) {
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
    (void)execution_backend_.pause();
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
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_,
                                              std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    if (execute_admission_interruptible_.load(std::memory_order_acquire)) {
      lifecycle_reset_requested_while_busy_.store(true,
                                                  std::memory_order_release);
      if (execute_admission_interruptible_.load(std::memory_order_acquire)) {
        return ResetResultDTO{true, std::nullopt};
      }
    }
    lifecycle_lock.lock();
  }
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_release);
  lifecycle_stop_requested_event_emitted_.store(false,
                                               std::memory_order_release);
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
                                               std::memory_order_release);
    lifecycle_stop_requested_event_emitted_.store(false,
                                                 std::memory_order_release);
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
    double seconds = 1.0;
    if (config_.operator_stop_profile) {
      if (config_.operator_stop_profile->max_stop_time_s.has_value()) {
        seconds = *config_.operator_stop_profile->max_stop_time_s;
      } else if (config_.operator_stop_profile->deceleration_time_s.has_value()) {
        seconds = *config_.operator_stop_profile->deceleration_time_s + 0.25;
      }
    }
    seconds = std::max(0.05, seconds);
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
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

  auto publish_runtime_monitor_fault =
      [this](CollisionCheckResultDTO check,
             std::map<std::string, std::string> details) -> Result<void> {
    details["surface"] = "observed_runtime_monitor";
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
        pool.validate_trajectory(*world_snapshot, engine_ptrs, trajectory, *cancel);
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
            trajectory.waypoints.size()) {
      const auto first_failure_index =
          static_cast<std::size_t>(*bundle.decision.first_failing_waypoint);
      const auto& waypoint = trajectory.waypoints[first_failure_index];
      details["collision_result_first_failure_joint_names"] =
          comma_join_strings(trajectory.joint_names);
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
  append_legacy_collision_compare_diagnostics(details, bundle, trajectory,
                                              context_);
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

void ExecutionKernel::set_runtime_velocity_scale(
    double scale, std::optional<std::string> principal_id) {
  // ADR-0018: HMI runtime velocity scale → backend runtime register.
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
  const double previous =
      runtime_velocity_scale_.load(std::memory_order_acquire);
  runtime_velocity_scale_.store(scale, std::memory_order_release);
  execution_backend_.set_runtime_velocity_scale(scale);
  const auto status = sm_.status();
  std::map<std::string, std::string> details{
      {"previous_scale", format_double(previous)},
      {"new_scale", format_double(scale)},
      {"applied_scale", format_double(scale)},
      {"backend_status", "forwarded"},
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
