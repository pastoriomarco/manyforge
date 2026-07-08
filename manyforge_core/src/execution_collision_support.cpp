#include <manyforge_core/execution_collision_support.hpp>

#include <manyforge_core/types/reason_codes.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace manyforge::core {

namespace {

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
    case CollisionPairClassification::kAllowedContactGrasp:
      return "allowed_contact_grasp";
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
    case CollisionPairClassification::kAllowedContactGrasp:
      return false;
  }
  return true;
}

std::string collision_pair_name(const CollisionPairId& pair) {
  return pair.body_a + " <-> " + pair.body_b;
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

}  // namespace

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

Result<RuntimeCollisionStateProjection> build_runtime_collision_state_projection(
    const TrajectoryDTO& source_trajectory,
    const TrajectoryDTO& collision_trajectory) {
  if (collision_trajectory.joint_names.empty()) {
    return Result<RuntimeCollisionStateProjection>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Runtime collision state projection cannot be built without collision joint names",
        {{"reason", "runtime_collision_projection_missing_query_joint_names"}}});
  }
  if (collision_trajectory.waypoints.empty()) {
    return Result<RuntimeCollisionStateProjection>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Runtime collision state projection cannot be built without a template waypoint",
        {{"reason", "runtime_collision_projection_missing_template_waypoint"}}});
  }
  const auto& template_positions =
      collision_trajectory.waypoints.front().positions_rad;
  if (template_positions.size() != collision_trajectory.joint_names.size()) {
    return Result<RuntimeCollisionStateProjection>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Runtime collision state projection template shape is invalid",
        {{"reason", "runtime_collision_projection_template_shape_mismatch"},
         {"query_joint_count",
          std::to_string(collision_trajectory.joint_names.size())},
         {"template_position_count",
          std::to_string(template_positions.size())}}});
  }

  RuntimeCollisionStateProjection projection;
  projection.query_joint_names = collision_trajectory.joint_names;
  projection.template_positions_rad = template_positions;

  projection.source_joint_names = source_trajectory.joint_names;
  if (projection.source_joint_names.empty()) {
    if (!source_trajectory.waypoints.empty() &&
        source_trajectory.waypoints.front().positions_rad.size() ==
            projection.query_joint_names.size()) {
      projection.source_joint_names = projection.query_joint_names;
    } else {
      return Result<RuntimeCollisionStateProjection>::Err(ErrorDTO{
          kErrorValidationCollision,
          "Runtime collision state projection cannot map unnamed observed positions",
          {{"reason", "runtime_collision_projection_missing_source_joint_names"}}});
    }
  }
  if (!source_trajectory.waypoints.empty() &&
      source_trajectory.waypoints.front().positions_rad.size() !=
          projection.source_joint_names.size()) {
    return Result<RuntimeCollisionStateProjection>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Runtime collision state projection source shape is invalid",
        {{"reason", "runtime_collision_projection_source_shape_mismatch"},
         {"source_joint_count",
          std::to_string(projection.source_joint_names.size())},
         {"source_position_count",
          std::to_string(source_trajectory.waypoints.front()
                             .positions_rad.size())}}});
  }

  projection.source_to_query_indices.reserve(
      projection.source_joint_names.size());
  for (const auto& source_joint : projection.source_joint_names) {
    const auto it = std::find(projection.query_joint_names.begin(),
                              projection.query_joint_names.end(),
                              source_joint);
    if (it == projection.query_joint_names.end()) {
      return Result<RuntimeCollisionStateProjection>::Err(ErrorDTO{
          kErrorValidationCollision,
          "Runtime collision state projection source joint is absent from collision query",
          {{"reason", "runtime_collision_projection_joint_not_in_query"},
           {"joint", source_joint}}});
    }
    projection.source_to_query_indices.push_back(
        static_cast<std::size_t>(
            std::distance(projection.query_joint_names.begin(), it)));
  }
  return Result<RuntimeCollisionStateProjection>::Ok(std::move(projection));
}

Result<RobotStateDTO> project_runtime_collision_observed_state(
    const RuntimeCollisionStateProjection& projection,
    const std::vector<double>& observed_positions_rad) {
  RobotStateDTO state;
  state.joint_names = projection.query_joint_names;
  if (observed_positions_rad.size() == projection.query_joint_names.size() &&
      projection.source_joint_names == projection.query_joint_names) {
    state.positions_rad = observed_positions_rad;
    return Result<RobotStateDTO>::Ok(std::move(state));
  }
  if (observed_positions_rad.size() != projection.source_joint_names.size()) {
    return Result<RobotStateDTO>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Runtime collision monitor observed state shape does not match projection",
        {{"reason", "runtime_collision_projection_observed_shape_mismatch"},
         {"observed_position_count",
          std::to_string(observed_positions_rad.size())},
         {"source_joint_count",
          std::to_string(projection.source_joint_names.size())},
         {"query_joint_count",
          std::to_string(projection.query_joint_names.size())}}});
  }

  state.positions_rad = projection.template_positions_rad;
  for (std::size_t i = 0; i < observed_positions_rad.size(); ++i) {
    state.positions_rad[projection.source_to_query_indices[i]] =
        observed_positions_rad[i];
  }
  return Result<RobotStateDTO>::Ok(std::move(state));
}

void PublishedRuntimeCollisionSnapshotStore::publish(
    CollisionWorldPolicySnapshotBundle bundle) {
  std::lock_guard<std::mutex> lock(mutex_);
  bundle_ = std::move(bundle);
  ++epoch_;
}

std::uint64_t PublishedRuntimeCollisionSnapshotStore::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  bundle_ = {};
  return ++epoch_;
}

PublishedRuntimeCollisionSnapshot PublishedRuntimeCollisionSnapshotStore::snapshot()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  return PublishedRuntimeCollisionSnapshot{bundle_, epoch_};
}

Result<CollisionWorldPolicySnapshotBundle>
load_collision_world_policy_snapshot_bundle(
    const ICollisionWorldSnapshotPublisher& publisher) {
  CollisionWorldPolicySnapshotBundle bundle;
  try {
    bundle = publisher.collision_world_policy_snapshot_bundle();
  } catch (const std::exception& e) {
    return Result<CollisionWorldPolicySnapshotBundle>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Collision world snapshot publisher failed",
        {{"reason", "collision_world_snapshot_publisher_exception"},
         {"exception", e.what()}}});
  } catch (...) {
    return Result<CollisionWorldPolicySnapshotBundle>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Collision world snapshot publisher failed",
        {{"reason", "collision_world_snapshot_publisher_exception"},
         {"exception", reason_codes::kUnknownNonStdException}}});
  }
  if (!bundle.world) {
    return Result<CollisionWorldPolicySnapshotBundle>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Collision world snapshot unavailable",
        {{"reason", "collision_world_snapshot_unavailable"}}});
  }
  return Result<CollisionWorldPolicySnapshotBundle>::Ok(std::move(bundle));
}

Result<CollisionWorldPolicySnapshotBundle>
load_published_runtime_collision_snapshot_bundle(
    const ICollisionWorldSnapshotPublisher& publisher) {
  CollisionWorldPolicySnapshotBundle bundle;
  try {
    bundle = publisher.published_runtime_collision_snapshot_bundle();
  } catch (const std::exception& e) {
    return Result<CollisionWorldPolicySnapshotBundle>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Published runtime collision snapshot unavailable",
        {{"reason", "published_runtime_collision_snapshot_exception"},
         {"exception", e.what()}}});
  } catch (...) {
    return Result<CollisionWorldPolicySnapshotBundle>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Published runtime collision snapshot unavailable",
        {{"reason", "published_runtime_collision_snapshot_exception"},
         {"exception", reason_codes::kUnknownNonStdException}}});
  }
  return Result<CollisionWorldPolicySnapshotBundle>::Ok(std::move(bundle));
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
    details["legacy_collision_compare_error"] =
        reason_codes::kUnknownNonStdException;
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

bool collision_world_revisions_equal(const CollisionWorldRevision& lhs,
                                     const CollisionWorldRevision& rhs) {
  return lhs.revision_id == rhs.revision_id &&
         lhs.content_hash == rhs.content_hash;
}

bool engines_require_single_context_exclusivity(
    const std::vector<std::shared_ptr<ICollisionQueryEngine>>& engines) {
  for (const auto& engine : engines) {
    if (engine &&
        engine->capability().thread_safety ==
            CollisionEngineThreadSafetyClass::kSingleContextExclusive) {
      return true;
    }
  }
  return false;
}

bool runtime_monitor_may_hold_collision_engine(ExecutionState state) {
  return state == ExecutionState::kExecuting ||
         state == ExecutionState::kPaused ||
         state == ExecutionState::kStopping;
}

}  // namespace manyforge::core
