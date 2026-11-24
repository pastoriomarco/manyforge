#include <algorithm>
#include <cmath>
#include <iomanip>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/types/reason_codes.hpp>
#include <sstream>

namespace manyforge::core {

namespace {

CollisionPairId canonical_collision_pair(CollisionPairId pair) {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return pair;
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

std::string collision_pair_classification_name(CollisionPairClassification classification) {
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
    case CollisionPairClassification::kAllowedContactRule:
      return "allowed_contact_rule";
    case CollisionPairClassification::kDiagnosticIncomplete:
      return "diagnostic_incomplete";
  }
  return "unknown";
}

bool collision_pair_classification_blocks_dispatch(CollisionPairClassification classification) {
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
    case CollisionPairClassification::kAllowedContactRule:
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

/// Reason code of a refused sample of a pair under a contact rule with
/// an end window, from the coordinator's raw diagnostic.
std::string_view native_windowed_contact_violation_reason(std::string_view raw_reason) noexcept {
  if (raw_reason == reason_codes::kMissingSignedClearance) {
    return reason_codes::kMissingGraspContactClearance;
  }
  if (raw_reason == reason_codes::kInitialSampleInContact) {
    return reason_codes::kInitialSampleInContact;
  }
  if (raw_reason == reason_codes::kContactBeforeTerminalWindow) {
    return "grasp_contact_before_terminal_window";
  }
  return raw_reason;
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

RuntimeCollisionUnavailableReason classify_runtime_collision_unavailable_reason(
    std::string_view reason) noexcept {
  if (reason == "missing_collision_world_snapshot_publisher" ||
      reason == "missing_runtime_collision_query_engine" ||
      reason.find("collision_resources") != std::string_view::npos) {
    return RuntimeCollisionUnavailableReason::kCollisionResources;
  }
  if (reason.find("snapshot") != std::string_view::npos ||
      reason.find("collision_policy") != std::string_view::npos) {
    return RuntimeCollisionUnavailableReason::kCollisionSnapshot;
  }
  if (reason == "observed_state_unavailable" || reason == kComponentStreamAbsentCode) {
    return RuntimeCollisionUnavailableReason::kObservedStateUnavailable;
  }
  if (reason == "stale_observed_robot_state" || reason == kComponentStreamStaleCode) {
    return RuntimeCollisionUnavailableReason::kObservedStateStale;
  }
  if (reason == "observed_state_incomplete") {
    return RuntimeCollisionUnavailableReason::kObservedStateIncomplete;
  }
  if (reason.find("runtime_collision_projection") != std::string_view::npos) {
    return RuntimeCollisionUnavailableReason::kObservedStateProjection;
  }
  if (reason.find("engine") != std::string_view::npos ||
      reason.find("inconclusive") != std::string_view::npos) {
    return RuntimeCollisionUnavailableReason::kCollisionQuery;
  }
  return RuntimeCollisionUnavailableReason::kOther;
}

const char* runtime_collision_unavailable_reason_name(
    RuntimeCollisionUnavailableReason reason) noexcept {
  switch (reason) {
    case RuntimeCollisionUnavailableReason::kCollisionResources:
      return "collision_resources";
    case RuntimeCollisionUnavailableReason::kCollisionSnapshot:
      return "collision_snapshot";
    case RuntimeCollisionUnavailableReason::kObservedStateUnavailable:
      return "observed_state_unavailable";
    case RuntimeCollisionUnavailableReason::kObservedStateStale:
      return "observed_state_stale";
    case RuntimeCollisionUnavailableReason::kObservedStateIncomplete:
      return "observed_state_incomplete";
    case RuntimeCollisionUnavailableReason::kObservedStateProjection:
      return "observed_state_projection";
    case RuntimeCollisionUnavailableReason::kCollisionQuery:
      return "collision_query";
    case RuntimeCollisionUnavailableReason::kOther:
      return "other";
  }
  return "other";
}

bool snapshot_too_old(std::int64_t now_ns, std::int64_t timestamp_ns, std::int64_t max_age_ns,
                      std::map<std::string, std::string>& details, const std::string& timestamp_key,
                      const std::string& age_key, const std::string& budget_key) {
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

Result<RuntimeCollisionStateProjection> build_runtime_collision_state_projection(
    const TrajectoryDTO& source_trajectory, const TrajectoryDTO& collision_trajectory,
    const CellModelStateSources* cell_sources) {
  if (collision_trajectory.joint_names.empty()) {
    return Result<RuntimeCollisionStateProjection>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Runtime collision state projection cannot be built without collision joint names",
                 {{"reason", "runtime_collision_projection_missing_query_joint_names"}}});
  }
  if (collision_trajectory.waypoints.empty()) {
    return Result<RuntimeCollisionStateProjection>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Runtime collision state projection cannot be built without a template waypoint",
                 {{"reason", "runtime_collision_projection_missing_template_waypoint"}}});
  }
  const auto& template_positions = collision_trajectory.waypoints.front().positions_rad;
  if (template_positions.size() != collision_trajectory.joint_names.size()) {
    return Result<RuntimeCollisionStateProjection>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Runtime collision state projection template shape is invalid",
                 {{"reason", "runtime_collision_projection_template_shape_mismatch"},
                  {"query_joint_count", std::to_string(collision_trajectory.joint_names.size())},
                  {"template_position_count", std::to_string(template_positions.size())}}});
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
      return Result<RuntimeCollisionStateProjection>::Err(
          ErrorDTO{kErrorValidationCollision,
                   "Runtime collision state projection cannot map unnamed observed positions",
                   {{"reason", "runtime_collision_projection_missing_source_joint_names"}}});
    }
  }
  if (!source_trajectory.waypoints.empty() &&
      source_trajectory.waypoints.front().positions_rad.size() !=
          projection.source_joint_names.size()) {
    return Result<RuntimeCollisionStateProjection>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Runtime collision state projection source shape is invalid",
                 {{"reason", "runtime_collision_projection_source_shape_mismatch"},
                  {"source_joint_count", std::to_string(projection.source_joint_names.size())},
                  {"source_position_count",
                   std::to_string(source_trajectory.waypoints.front().positions_rad.size())}}});
  }

  projection.source_to_query_indices.reserve(projection.source_joint_names.size());
  for (const auto& source_joint : projection.source_joint_names) {
    const auto it = std::find(projection.query_joint_names.begin(),
                              projection.query_joint_names.end(), source_joint);
    if (it == projection.query_joint_names.end()) {
      return Result<RuntimeCollisionStateProjection>::Err(
          ErrorDTO{kErrorValidationCollision,
                   "Runtime collision state projection source joint is absent from collision query",
                   {{"reason", "runtime_collision_projection_joint_not_in_query"},
                    {"joint", source_joint}}});
    }
    projection.source_to_query_indices.push_back(
        static_cast<std::size_t>(std::distance(projection.query_joint_names.begin(), it)));
  }
  if (cell_sources != nullptr) {
    projection.live_sources = bind_cell_model_state(*cell_sources, projection.query_joint_names,
                                                    projection.source_joint_names);
  }
  return Result<RuntimeCollisionStateProjection>::Ok(std::move(projection));
}

bool runtime_collision_projection_continues(
    const RuntimeCollisionStateProjection& live,
    const RuntimeCollisionStateProjection& replacement) noexcept {
  if (live.source_joint_names != replacement.source_joint_names ||
      live.query_joint_names != replacement.query_joint_names ||
      live.source_to_query_indices != replacement.source_to_query_indices ||
      live.template_positions_rad.size() != replacement.template_positions_rad.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < replacement.template_positions_rad.size(); ++index) {
    if (std::find(replacement.source_to_query_indices.begin(),
                  replacement.source_to_query_indices.end(),
                  index) == replacement.source_to_query_indices.end() &&
        !cell_model_state_binds(live.live_sources, index) &&
        live.template_positions_rad[index] != replacement.template_positions_rad[index]) {
      return false;
    }
  }
  return true;
}

void RuntimeCollisionStopCoverageTracker::update_max(std::atomic<std::uint64_t>& target,
                                                     std::uint64_t value) noexcept {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
  }
}

void RuntimeCollisionStopCoverageTracker::start(std::uint64_t generation) noexcept {
  current_ticks_.store(0, std::memory_order_relaxed);
  current_queries_.store(0, std::memory_order_relaxed);
  current_unavailable_.store(0, std::memory_order_relaxed);
  for (auto& count : current_unavailable_by_reason_) {
    count.store(0, std::memory_order_relaxed);
  }
  current_max_unavailable_ns_.store(0, std::memory_order_relaxed);
  unavailable_since_ns_.store(-1, std::memory_order_relaxed);
  unavailable_reason_hash_.store(0, std::memory_order_relaxed);
  unavailable_last_event_ns_.store(-1, std::memory_order_relaxed);
  generation_.store(generation, std::memory_order_release);
  windows_started_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeCollisionStopCoverageTracker::record_tick(std::uint64_t generation) noexcept {
  if (generation == 0 || active_generation() != generation) {
    return;
  }
  current_ticks_.fetch_add(1, std::memory_order_relaxed);
  total_ticks_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeCollisionStopCoverageTracker::record_observed_source(
    ObservedRobotStateSourceKind source_kind, std::uint64_t source_age_ns) noexcept {
  freshest_source_age_ns_.store(source_age_ns, std::memory_order_relaxed);
  freshest_source_kind_.store(static_cast<int>(source_kind), std::memory_order_relaxed);
}

void RuntimeCollisionStopCoverageTracker::record_query(std::uint64_t generation,
                                                       std::int64_t now_ns) noexcept {
  if (generation == 0 || active_generation() != generation) {
    return;
  }
  current_queries_.fetch_add(1, std::memory_order_relaxed);
  total_queries_.fetch_add(1, std::memory_order_relaxed);
  const auto unavailable_since = unavailable_since_ns_.exchange(-1, std::memory_order_acq_rel);
  if (unavailable_since >= 0 && now_ns > unavailable_since) {
    const auto duration = static_cast<std::uint64_t>(now_ns - unavailable_since);
    update_max(current_max_unavailable_ns_, duration);
    update_max(longest_unavailable_ns_, duration);
  }
}

void RuntimeCollisionStopCoverageTracker::record_unavailable(
    std::uint64_t generation, RuntimeCollisionUnavailableReason reason,
    std::int64_t now_ns) noexcept {
  if (generation == 0 || active_generation() != generation) {
    return;
  }
  current_unavailable_.fetch_add(1, std::memory_order_relaxed);
  const auto reason_index = static_cast<std::size_t>(reason);
  if (reason_index < current_unavailable_by_reason_.size()) {
    current_unavailable_by_reason_[reason_index].fetch_add(1, std::memory_order_relaxed);
  }
  total_unavailable_.fetch_add(1, std::memory_order_relaxed);
  std::int64_t no_interval = -1;
  (void)unavailable_since_ns_.compare_exchange_strong(
      no_interval, now_ns, std::memory_order_acq_rel, std::memory_order_relaxed);
}

bool RuntimeCollisionStopCoverageTracker::should_emit_unavailable(std::uint64_t reason_hash,
                                                                  std::int64_t now_ns) noexcept {
  const auto previous_hash =
      unavailable_reason_hash_.exchange(reason_hash, std::memory_order_acq_rel);
  const auto previous_event_ns = unavailable_last_event_ns_.load(std::memory_order_acquire);
  constexpr std::int64_t kEventPeriodNs = 1'000'000'000;
  if (previous_hash == reason_hash && previous_event_ns >= 0 &&
      now_ns - previous_event_ns < kEventPeriodNs) {
    unavailable_events_suppressed_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  unavailable_last_event_ns_.store(now_ns, std::memory_order_release);
  return true;
}

std::optional<RuntimeCollisionStopCoverageSummary> RuntimeCollisionStopCoverageTracker::complete(
    std::uint64_t generation, std::int64_t now_ns) noexcept {
  if (generation == 0) {
    return std::nullopt;
  }
  std::uint64_t expected = generation;
  if (!generation_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
    return std::nullopt;
  }
  const auto unavailable_since = unavailable_since_ns_.exchange(-1, std::memory_order_acq_rel);
  if (unavailable_since >= 0 && now_ns > unavailable_since) {
    const auto duration = static_cast<std::uint64_t>(now_ns - unavailable_since);
    update_max(current_max_unavailable_ns_, duration);
    update_max(longest_unavailable_ns_, duration);
  }
  RuntimeCollisionStopCoverageSummary summary;
  summary.generation = generation;
  summary.ticks_attempted = current_ticks_.load(std::memory_order_acquire);
  summary.queries_executed = current_queries_.load(std::memory_order_acquire);
  summary.unavailable_ticks = current_unavailable_.load(std::memory_order_acquire);
  summary.longest_unavailable_ns = current_max_unavailable_ns_.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < summary.unavailable_by_reason.size(); ++i) {
    summary.unavailable_by_reason[i] =
        current_unavailable_by_reason_[i].load(std::memory_order_acquire);
  }
  last_generation_.store(generation, std::memory_order_relaxed);
  last_ticks_.store(summary.ticks_attempted, std::memory_order_relaxed);
  last_queries_.store(summary.queries_executed, std::memory_order_relaxed);
  last_unavailable_.store(summary.unavailable_ticks, std::memory_order_relaxed);
  last_max_unavailable_ns_.store(summary.longest_unavailable_ns, std::memory_order_relaxed);
  windows_completed_.fetch_add(1, std::memory_order_relaxed);
  if (summary.ticks_attempted == 0 || summary.queries_executed == 0) {
    zero_check_windows_.fetch_add(1, std::memory_order_relaxed);
  }
  return summary;
}

std::uint64_t RuntimeCollisionStopCoverageTracker::active_generation() const noexcept {
  return generation_.load(std::memory_order_acquire);
}

RuntimeCollisionStopCoverageMetrics RuntimeCollisionStopCoverageTracker::metrics() const noexcept {
  RuntimeCollisionStopCoverageMetrics result;
  result.windows_started = windows_started_.load(std::memory_order_relaxed);
  result.windows_completed = windows_completed_.load(std::memory_order_relaxed);
  result.ticks_attempted = total_ticks_.load(std::memory_order_relaxed);
  result.queries_executed = total_queries_.load(std::memory_order_relaxed);
  result.unavailable_ticks = total_unavailable_.load(std::memory_order_relaxed);
  result.longest_unavailable_ns = longest_unavailable_ns_.load(std::memory_order_relaxed);
  result.zero_check_windows = zero_check_windows_.load(std::memory_order_relaxed);
  result.last_window = {
      last_generation_.load(std::memory_order_relaxed),
      last_ticks_.load(std::memory_order_relaxed),
      last_queries_.load(std::memory_order_relaxed),
      last_unavailable_.load(std::memory_order_relaxed),
      last_max_unavailable_ns_.load(std::memory_order_relaxed),
  };
  result.freshest_source_age_ns = freshest_source_age_ns_.load(std::memory_order_relaxed);
  result.freshest_source_kind = freshest_source_kind_.load(std::memory_order_relaxed);
  result.unavailable_events_suppressed =
      unavailable_events_suppressed_.load(std::memory_order_relaxed);
  return result;
}

Result<CollisionWorldPolicySnapshotBundle> load_collision_world_policy_snapshot_bundle(
    const ICollisionWorldSnapshotPublisher& publisher) {
  CollisionWorldPolicySnapshotBundle bundle;
  try {
    bundle = publisher.collision_world_policy_snapshot_bundle();
  } catch (const std::exception& e) {
    return Result<CollisionWorldPolicySnapshotBundle>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Collision world snapshot publisher failed",
        {{"reason", "collision_world_snapshot_publisher_exception"}, {"exception", e.what()}}});
  } catch (...) {
    return Result<CollisionWorldPolicySnapshotBundle>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Collision world snapshot publisher failed",
                 {{"reason", "collision_world_snapshot_publisher_exception"},
                  {"exception", reason_codes::kUnknownNonStdException}}});
  }
  if (!bundle.world) {
    return Result<CollisionWorldPolicySnapshotBundle>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Collision world snapshot unavailable",
                 {{"reason", "collision_world_snapshot_unavailable"}}});
  }
  return Result<CollisionWorldPolicySnapshotBundle>::Ok(std::move(bundle));
}

Result<RuntimeCollisionQueryBundlePtr> load_published_runtime_collision_query_bundle(
    const ICollisionWorldSnapshotPublisher& publisher) {
  RuntimeCollisionQueryBundlePtr bundle;
  try {
    bundle = publisher.published_runtime_collision_query_bundle();
  } catch (const std::exception& e) {
    return Result<RuntimeCollisionQueryBundlePtr>::Err(ErrorDTO{
        kErrorValidationCollision,
        "Published runtime collision snapshot unavailable",
        {{"reason", "published_runtime_collision_snapshot_exception"}, {"exception", e.what()}}});
  } catch (...) {
    return Result<RuntimeCollisionQueryBundlePtr>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Published runtime collision snapshot unavailable",
                 {{"reason", "published_runtime_collision_snapshot_exception"},
                  {"exception", reason_codes::kUnknownNonStdException}}});
  }
  return Result<RuntimeCollisionQueryBundlePtr>::Ok(std::move(bundle));
}

RuntimeCollisionQueryBundlePtr build_runtime_collision_query_bundle(
    CollisionWorldPolicySnapshotBundle bundle,
    std::optional<ProjectionRevisionDTO> projection_revision) {
  auto runtime_bundle = std::make_shared<RuntimeCollisionQueryBundle>();
  runtime_bundle->projection_revision = std::move(projection_revision);
  runtime_bundle->distance_selection_order_valid = true;
  runtime_bundle->world = std::move(bundle.world);
  runtime_bundle->lookup_order_valid =
      runtime_bundle->world && runtime_collision_lookup_order_is_valid(*runtime_bundle->world);
  if (bundle.policy) {
    auto policy = std::make_shared<CollisionPolicySnapshot>(std::move(*bundle.policy));
    if (runtime_bundle->world) {
      runtime_bundle->selected_distance_pairs =
          build_selected_collision_distance_pairs(*policy);
      auto selection_valid = [](const std::vector<CollisionPairId>& pairs) {
        for (std::size_t i = 0; i < pairs.size(); ++i) {
          const auto& pair = pairs[i];
          if (pair.body_a.empty() || pair.body_b.empty() || !(pair.body_a < pair.body_b) ||
              (i > 0 && !(pairs[i - 1] < pair))) {
            return false;
          }
        }
        return true;
      };
      runtime_bundle->distance_selection_order_valid =
          selection_valid(runtime_bundle->selected_distance_pairs);
    }
    runtime_bundle->policy_index = build_collision_policy_index(*policy);
    runtime_bundle->policy = std::move(policy);
  }
  return runtime_bundle;
}

bool runtime_collision_lookup_order_is_valid(const CollisionWorldSnapshot& world) noexcept {
  return std::adjacent_find(world.bodies.begin(), world.bodies.end(),
                            [](const auto& lhs, const auto& rhs) {
                              return lhs.body_id >= rhs.body_id;
                            }) == world.bodies.end();
}

void append_collision_failure_diagnostics(std::map<std::string, std::string>& details,
                                          const CollisionValidationDecisionBundle& bundle,
                                          const CollisionClearanceMargins* clearance_margins) {
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
  details["collision_result_waypoint_index"] = std::to_string(selected->waypoint_index);
  if (result.query_id) {
    details["collision_result_query_id"] = *result.query_id;
  }
  details["collision_result_engine_id"] = result.engine_id;
  details["collision_result_engine_version"] = result.engine_version;
  details["collision_result_engine_status"] = collision_engine_status_name(result.engine_status);
  details["collision_result_decision"] = collision_decision_result_name(result.decision);
  details["collision_result_in_collision"] = result.in_collision ? "true" : "false";
  details["collision_result_normalized_pair_count"] =
      std::to_string(result.normalized_pairs.size());
  if (!result.normalized_pairs.empty()) {
    details["collision_result_first_pair"] = collision_pair_name(result.normalized_pairs.front());
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
      if (collision_pair_classification_blocks_dispatch(classified.classification)) {
        first_failure_pair = &classified;
        break;
      }
    }
  }
  if (first_failure_pair != nullptr) {
    details["collision_result_first_failure_pair"] = collision_pair_name(first_failure_pair->pair);
    details["collision_result_first_failure_classification"] =
        collision_pair_classification_name(first_failure_pair->classification);
    // A pair that collides only through its clearance margin keeps the
    // validation_collision code; the classification tells it apart.
    const auto clearance = std::find_if(
        result.clearance_violations.begin(), result.clearance_violations.end(),
        [&](const CollisionPairClearance& violation) {
          return violation.pair == first_failure_pair->pair;
        });
    if (clearance != result.clearance_violations.end()) {
      details["classification"] = "clearance";
      details["signed_clearance_m"] = format_double(clearance->signed_clearance_m);
      // The clearance rule that holds the pair, and its value.
      if (clearance_margins != nullptr) {
        const auto pair = canonical_collision_pair(first_failure_pair->pair);
        if (const auto* margin = clearance_margins->find(pair)) {
          details["collision_rule_id"] = std::string(clearance_margins->rule_id_for(pair));
          details["clearance_m"] = format_double(margin->margin_m);
        }
      }
    }
  }
  if (const auto violation = result.raw_diagnostics.find("contact_release_violation");
      violation != result.raw_diagnostics.end()) {
    details["collision_result_contact_release_violation"] = violation->second;
  }
  if (const auto pair = result.raw_diagnostics.find("contact_release_pair");
      pair != result.raw_diagnostics.end()) {
    details["collision_result_contact_release_pair"] = pair->second;
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

std::optional<std::map<std::string, std::string>> contact_rule_violation_details(
    const CollisionValidationDecisionBundle& bundle, const CollisionPolicySnapshot& policy,
    std::string_view response, std::optional<std::size_t> initial_contact_pair_count) {
  const auto* selected = select_collision_failure_record(bundle);
  if (selected == nullptr) {
    return std::nullopt;
  }
  const auto violation = selected->result.raw_diagnostics.find("contact_release_violation");
  if (violation == selected->result.raw_diagnostics.end()) {
    // A pair under a contact rule with an end window.
    const auto rule_violation = selected->result.raw_diagnostics.find("contact_rule_violation");
    const auto rule_pair = selected->result.raw_diagnostics.find("contact_rule_pair");
    if (rule_violation == selected->result.raw_diagnostics.end() ||
        rule_pair == selected->result.raw_diagnostics.end()) {
      return std::nullopt;
    }

    const CollisionPairContactRule* matched_rule = nullptr;
    std::optional<CollisionPairId> matched_pair;
    for (const auto& observed : selected->result.normalized_pairs) {
      const auto canonical = canonical_collision_pair(observed);
      if (collision_pair_name(canonical) != rule_pair->second) {
        continue;
      }
      const auto* rule = policy.contact_rule_for(canonical);
      if (rule != nullptr && contact_rule_kind(*rule) == ContactRuleKind::kWindowedContact) {
        matched_rule = rule;
        matched_pair = canonical;
      }
      break;
    }
    if (matched_rule == nullptr) {
      return std::nullopt;
    }
    const auto& contact = *matched_rule->contact();

    std::string classification =
        selected->result.decision == CollisionDecisionResult::kInconclusiveFail
            ? "diagnostic_incomplete"
            : "blocking_collision";
    for (const auto& classified : selected->result.normalized_pairs_classified) {
      if (canonical_collision_pair(classified.pair) == *matched_pair) {
        classification = collision_pair_classification_name(classified.classification);
        break;
      }
    }

    std::map<std::string, std::string> details{
        {"contact_rule_violation",
         std::string(native_windowed_contact_violation_reason(rule_violation->second))},
        {"collision_rule_id", matched_rule->rule_id},
        {"policy_mode", "contact"},
        {"contact_body_a", matched_pair->body_a},
        {"contact_body_b", matched_pair->body_b},
        {"contact_present", "true"},
        {"end_from_fraction", format_double(contact.end_from_fraction)},
        {"distance_request_mode",
         collision_distance_request_mode_name(selected->result.distance_request_mode)},
        {"evaluated_distance_pair_count",
         std::to_string(selected->result.evaluated_distance_pair_count)},
        {"response", std::string(response)},
        {"classification", std::move(classification)}};
    if (contact.max_penetration_m) {
      details["max_penetration_m"] = format_double(*contact.max_penetration_m);
    }
    if (rule_violation->second == "contact_penetration_exceeded") {
      details["within_penetration_bound"] = "false";
    }
    if (contact.max_compression_step_m) {
      details["max_compression_step_m"] = format_double(*contact.max_compression_step_m);
    }

    const auto copy_diagnostic = [&](const char* source, const char* destination) {
      const auto it = selected->result.raw_diagnostics.find(source);
      if (it != selected->result.raw_diagnostics.end()) {
        details[destination] = it->second;
      }
    };
    copy_diagnostic("contact_rule_sample_fraction", "sample_fraction");
    copy_diagnostic("contact_rule_inside_window", "inside_window");
    copy_diagnostic("contact_rule_previous_penetration_m", "previous_penetration_m");

    std::optional<double> signed_clearance;
    for (const auto& clearance : selected->result.pair_clearances) {
      if (canonical_collision_pair(clearance.pair) == *matched_pair &&
          std::isfinite(clearance.signed_clearance_m) && clearance.signed_clearance_m <= 0.0) {
        signed_clearance = clearance.signed_clearance_m;
        break;
      }
    }
    if (!signed_clearance.has_value() && selected->result.min_clearance_pair.has_value() &&
        canonical_collision_pair(*selected->result.min_clearance_pair) == *matched_pair &&
        selected->result.min_clearance_m.has_value() &&
        std::isfinite(*selected->result.min_clearance_m) &&
        *selected->result.min_clearance_m <= 0.0) {
      signed_clearance = *selected->result.min_clearance_m;
    }
    if (signed_clearance.has_value()) {
      const auto penetration = format_double(-*signed_clearance);
      details["penetration_depth_m"] = penetration;
      details["current_penetration_m"] = penetration;
    }
    return details;
  }

  // A tracked pair: a release rule, or a whole-move contact rule with a
  // depth limit.
  const CollisionPairContactRule* matched = nullptr;
  const auto diagnostic_pair = selected->result.raw_diagnostics.find("contact_release_pair");
  if (diagnostic_pair != selected->result.raw_diagnostics.end()) {
    for (const auto& entry : policy.contact_rules) {
      if (contact_rule_is_tracked(entry) &&
          collision_pair_name(canonical_collision_pair(entry.pair)) == diagnostic_pair->second) {
        matched = &entry;
        break;
      }
    }
  }
  if (matched == nullptr) {
    return std::nullopt;
  }
  const auto* release = matched->release();

  const auto pair = canonical_collision_pair(matched->pair);
  const auto pair_is_present = [&](const CollisionValidationWaypointRecord& record) {
    return std::any_of(record.result.normalized_pairs.begin(), record.result.normalized_pairs.end(),
                       [&](const CollisionPairId& observed) {
                         return canonical_collision_pair(observed) == pair;
                       });
  };
  const auto contact_pair_count = [&](const CollisionValidationWaypointRecord& record) {
    std::size_t count = 0;
    for (const auto& observed : record.result.normalized_pairs) {
      const auto* rule = policy.contact_rule_for(canonical_collision_pair(observed));
      if (rule != nullptr && contact_rule_is_tracked(*rule)) {
        ++count;
      }
    }
    return count;
  };

  if (!initial_contact_pair_count.has_value()) {
    initial_contact_pair_count = bundle.waypoint_records.empty()
                                     ? std::size_t{0}
                                     : contact_pair_count(bundle.waypoint_records.front());
  }
  const bool contact_present = pair_is_present(*selected);

  std::string classification =
      selected->result.decision == CollisionDecisionResult::kInconclusiveFail
          ? "diagnostic_incomplete"
          : "blocking_collision";
  for (const auto& classified : selected->result.normalized_pairs_classified) {
    if (canonical_collision_pair(classified.pair) == pair) {
      classification = collision_pair_classification_name(classified.classification);
      break;
    }
  }

  std::map<std::string, std::string> details{
      {"contact_rule_violation", violation->second},
      {"collision_rule_id", matched->rule_id},
      {"policy_mode", release == nullptr ? "contact" : "release"},
      {"contact_body_a", pair.body_a},
      {"contact_body_b", pair.body_b},
      {"contact_present", contact_present ? "true" : "false"},
      {"initial_contact_pair_count", std::to_string(*initial_contact_pair_count)},
      {"current_contact_pair_count", std::to_string(contact_pair_count(*selected))},
      {"distance_request_mode",
       collision_distance_request_mode_name(selected->result.distance_request_mode)},
      {"evaluated_distance_pair_count",
       std::to_string(selected->result.evaluated_distance_pair_count)},
      {"response", std::string(response)},
      {"classification", std::move(classification)}};

  if (release != nullptr) {
    details["release_evidence_mode"] =
        release->release_evidence_mode == ContactReleaseEvidenceMode::kBinaryContact
            ? "binary_contact"
            : "signed_penetration";
    if (violation->second == "initial_penetration_exceeds_policy" &&
        release->release_evidence_mode == ContactReleaseEvidenceMode::kSignedPenetration &&
        release->enforce_max_start_penetration) {
      details["max_penetration_m"] = format_double(release->max_start_penetration_m);
    }
  } else {
    details["max_penetration_m"] = format_double(*matched->contact()->max_penetration_m);
    if (violation->second == "contact_penetration_exceeded") {
      details["within_penetration_bound"] = "false";
    }
    if (const auto& step = matched->contact()->max_compression_step_m) {
      details["max_compression_step_m"] = format_double(*step);
    }
  }

  // Metric evidence is emitted only for the violating pair when it is
  // actually present and the engine supplied a finite signed clearance.
  // Binary contact and absent-pair violations deliberately carry no synthetic
  // penetration value.
  if (contact_present && contact_rule_needs_signed_distance(*matched)) {
    for (const auto& clearance : selected->result.pair_clearances) {
      if (canonical_collision_pair(clearance.pair) == pair &&
          std::isfinite(clearance.signed_clearance_m) && clearance.signed_clearance_m <= 0.0) {
        details["penetration_depth_m"] = format_double(-clearance.signed_clearance_m);
        break;
      }
    }
  }
  return details;
}

std::string pre_dispatch_collision_validation_message(
    const std::map<std::string, std::string>& details, CollisionDecisionAction action) {
  const auto value = [&details](const char* key) -> std::string_view {
    const auto it = details.find(key);
    return it == details.end() ? std::string_view{} : it->second;
  };

  const auto reason = value("reason");
  if (reason == "collision_validation_cancelled") {
    return "Pre-dispatch collision validation cancelled";
  }
  if (reason == "validation_deferred_runtime_monitor_active") {
    return "Pre-dispatch collision validation deferred";
  }

  const auto contact_release_violation = value("collision_result_contact_release_violation");
  const auto contact_release_pair = value("collision_result_contact_release_pair");
  const auto with_contact_pair = [&](std::string message) {
    if (!contact_release_pair.empty()) {
      message += ": " + std::string(contact_release_pair);
    }
    return message;
  };
  if (contact_release_violation == "final_sample_not_separated") {
    return with_contact_pair("Release pair did not separate by the final waypoint");
  }
  if (contact_release_violation == "initial_penetration_exceeds_policy") {
    return with_contact_pair("Release pair starts deeper than its rule allows");
  }
  if (contact_release_violation == "missing_contact_release_clearance") {
    return with_contact_pair("Contact depth of the pair is unavailable");
  }
  if (contact_release_violation == "penetration_increased") {
    return with_contact_pair("Penetration increased during the release");
  }
  if (contact_release_violation == "contact_penetration_exceeded") {
    return with_contact_pair("Contact is deeper than its rule allows");
  }
  if (contact_release_violation == "contact_compression_step_exceeded") {
    return with_contact_pair("Spring compressed in a jump (a step or an obstacle on the path)");
  }
  if (contact_release_violation == "insufficient_release_progress") {
    return with_contact_pair("Release made too little progress");
  }
  if (contact_release_violation == "new_active_contact_after_clear_start") {
    return with_contact_pair("Release pair came into contact after the trajectory started clear");
  }
  if (contact_release_violation == "recontact_after_release" ||
      contact_release_violation == "recontact_penetration_exceeds_policy") {
    return with_contact_pair("Release pair touched again after release");
  }

  if (value("classification") == "clearance") {
    return "Trajectory comes closer than the collision clearance: " +
           std::string(value("collision_result_first_failure_pair"));
  }

  const auto status = value("collision_result_engine_status");
  if (status == "timeout") {
    return "Pre-dispatch collision validation timed out";
  }
  if (status == "unsupported_geometry") {
    return "Pre-dispatch collision validation unsupported geometry";
  }
  if (status == "diagnostic_incomplete") {
    return "Pre-dispatch collision validation diagnostics incomplete";
  }
  if (status == "engine_error") {
    return "Pre-dispatch collision validation engine error";
  }
  if (status == "cancelled") {
    return "Pre-dispatch collision validation cancelled";
  }
  if (action == CollisionDecisionAction::kInconclusive) {
    return "Collision validation inconclusive";
  }
  const auto pair = value("collision_result_first_failure_pair");
  return pair.empty() ? std::string("Collision validation rejected trajectory")
                      : "Collision validation rejected trajectory: " + std::string(pair);
}

void append_collision_compare_diagnostics(std::map<std::string, std::string>& details,
                                          const CollisionValidationDecisionBundle& bundle,
                                          const TrajectoryDTO& trajectory,
                                          const ICollisionDiagnosticComparator& comparator) {
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
  query.query_id = "diagnostic_compare_" + (selected->result.query_id
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
    const auto compared = comparator.compare(query);
    details["collision_diagnostic_compare_engine_id"] = compared.engine_id;
    details["collision_diagnostic_compare_engine_status"] =
        collision_engine_status_name(compared.engine_status);
    details["collision_diagnostic_compare_decision"] =
        collision_decision_result_name(compared.decision);
    details["collision_diagnostic_compare_in_collision"] = compared.in_collision ? "true" : "false";
    details["collision_diagnostic_compare_normalized_pair_count"] =
        std::to_string(compared.normalized_pairs.size());
    details["collision_diagnostic_compare_in_collision_match"] =
        compared.in_collision == selected->result.in_collision ? "true" : "false";
    details["collision_diagnostic_compare_decision_match"] =
        compared.decision == selected->result.decision ? "true" : "false";
    details["collision_diagnostic_compare_normalized_pairs_match"] =
        compared.normalized_pairs == selected->result.normalized_pairs ? "true" : "false";
    if (compared.collision_world_revision.has_value()) {
      details["collision_diagnostic_compare_world_revision_id"] =
          std::to_string(compared.collision_world_revision->revision_id);
      details["collision_diagnostic_compare_world_content_hash"] =
          compared.collision_world_revision->content_hash;
    }
    if (!compared.normalized_pairs.empty()) {
      details["collision_diagnostic_compare_first_pair"] =
          collision_pair_name(compared.normalized_pairs.front());
    }
    const CollisionPairResult* compared_failure = nullptr;
    for (const auto& classified : compared.normalized_pairs_classified) {
      if (classified.is_first_failure) {
        compared_failure = &classified;
        break;
      }
    }
    if (compared_failure == nullptr) {
      for (const auto& classified : compared.normalized_pairs_classified) {
        if (collision_pair_classification_blocks_dispatch(classified.classification)) {
          compared_failure = &classified;
          break;
        }
      }
    }
    if (compared_failure != nullptr) {
      details["collision_diagnostic_compare_first_failure_pair"] =
          collision_pair_name(compared_failure->pair);
      details["collision_diagnostic_compare_first_failure_classification"] =
          collision_pair_classification_name(compared_failure->classification);
    }
  } catch (const std::exception& e) {
    details["collision_diagnostic_compare_error"] = e.what();
  } catch (...) {
    details["collision_diagnostic_compare_error"] = reason_codes::kUnknownNonStdException;
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
    if (!collision_pair_classification_blocks_dispatch(classified.classification)) {
      continue;
    }
    check.contact_body_a = classified.pair.body_a;
    check.contact_body_b = classified.pair.body_b;
    break;
  }
  if (record->result.min_clearance_m && *record->result.min_clearance_m < 0.0) {
    check.penetration_depth_m = -*record->result.min_clearance_m;
  }
  return check;
}

bool collision_world_revision_is_set(const CollisionWorldRevision& revision) {
  return revision.revision_id != 0 || !revision.content_hash.empty();
}

bool engines_require_single_context_exclusivity(
    const std::vector<std::shared_ptr<ICollisionQueryEngine>>& engines) {
  for (const auto& engine : engines) {
    if (engine && engine->capability().thread_safety ==
                      CollisionEngineThreadSafetyClass::kSingleContextExclusive) {
      return true;
    }
  }
  return false;
}

bool runtime_monitor_may_hold_collision_engine(ExecutionState state) {
  return state == ExecutionState::kExecuting ||
         state == ExecutionState::kStopping;
}

bool collision_policy_has_spring_rule(const CollisionPolicySnapshot& policy) noexcept {
  return std::any_of(policy.contact_rules.begin(), policy.contact_rules.end(),
                     [](const CollisionPairContactRule& rule) {
                       return rule.contact() != nullptr &&
                              rule.contact()->max_compression_step_m.has_value();
                     });
}

Result<TrajectoryDTO> densify_for_spring_contact(
    const TrajectoryDTO& trajectory,
    const std::function<Result<Vector3DTO>(const RobotStateDTO&)>& tcp_position,
    const double max_travel_m, const std::size_t max_samples) {
  const auto failed = [](std::string reason, std::string detail) {
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        kErrorValidationCollision,
        "The move's samples for its spring contact rule cannot be prepared",
        {{"reason", std::move(reason)}, {"detail", std::move(detail)}}});
  };
  if (trajectory.waypoints.size() < 2U || !(max_travel_m > 0.0)) {
    return Result<TrajectoryDTO>::Ok(trajectory);
  }
  RobotStateDTO state;
  state.joint_names = trajectory.joint_names;
  std::vector<Vector3DTO> positions;
  positions.reserve(trajectory.waypoints.size());
  for (const auto& waypoint : trajectory.waypoints) {
    state.positions_rad = waypoint.positions_rad;
    auto position = tcp_position(state);
    if (!position.ok()) {
      return failed("spring_sample_densification_failed", position.error().message);
    }
    positions.push_back(position.value());
  }
  // The same sub-step count for every segment keeps the fractions.
  std::size_t steps = 1U;
  for (std::size_t index = 1; index < positions.size(); ++index) {
    const auto& a = positions[index - 1U];
    const auto& b = positions[index];
    const double travel = std::sqrt((b.x_m - a.x_m) * (b.x_m - a.x_m) +
                                    (b.y_m - a.y_m) * (b.y_m - a.y_m) +
                                    (b.z_m - a.z_m) * (b.z_m - a.z_m));
    const double needed = std::ceil(travel / max_travel_m - 1e-9);
    if (!std::isfinite(needed) ||
        needed * static_cast<double>(trajectory.waypoints.size() - 1U) >
            static_cast<double>(max_samples)) {
      return failed("spring_sample_budget_exceeded",
                    "more than " + std::to_string(max_samples) + " samples");
    }
    steps = std::max(steps, static_cast<std::size_t>(needed));
  }
  if (steps == 1U) {
    return Result<TrajectoryDTO>::Ok(trajectory);
  }
  const auto lerp = [](const std::vector<double>& from, const std::vector<double>& to,
                       const double alpha) {
    if (from.size() != to.size()) {
      return std::vector<double>{};
    }
    std::vector<double> result(from.size());
    for (std::size_t joint = 0; joint < from.size(); ++joint) {
      result[joint] = from[joint] + alpha * (to[joint] - from[joint]);
    }
    return result;
  };
  TrajectoryDTO dense = trajectory;
  dense.waypoints.clear();
  dense.waypoints.reserve((trajectory.waypoints.size() - 1U) * steps + 1U);
  dense.waypoints.push_back(trajectory.waypoints.front());
  for (std::size_t index = 1; index < trajectory.waypoints.size(); ++index) {
    const auto& from = trajectory.waypoints[index - 1U];
    const auto& to = trajectory.waypoints[index];
    for (std::size_t step = 1; step < steps; ++step) {
      const double alpha = static_cast<double>(step) / static_cast<double>(steps);
      TrajectoryWaypointDTO sample;
      sample.time_from_start_s =
          from.time_from_start_s + alpha * (to.time_from_start_s - from.time_from_start_s);
      sample.positions_rad = lerp(from.positions_rad, to.positions_rad, alpha);
      sample.velocities_rad_s = lerp(from.velocities_rad_s, to.velocities_rad_s, alpha);
      sample.accelerations_rad_s2 =
          lerp(from.accelerations_rad_s2, to.accelerations_rad_s2, alpha);
      dense.waypoints.push_back(std::move(sample));
    }
    dense.waypoints.push_back(to);
  }
  return Result<TrajectoryDTO>::Ok(std::move(dense));
}

}  // namespace manyforge::core
