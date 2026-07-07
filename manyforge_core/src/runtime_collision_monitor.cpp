#include <manyforge_core/runtime_collision_monitor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>

#include <manyforge_core/types/reason_codes.hpp>

namespace manyforge::core {
namespace {

thread_local bool t_inside_runtime_collision_monitor_callback = false;
thread_local RuntimeCollisionMonitor* t_runtime_collision_monitor_instance =
    nullptr;
thread_local std::uint64_t t_runtime_collision_monitor_generation = 0;

constexpr auto kDrainCallbacksTimeout = std::chrono::seconds(1);

[[noreturn]] void terminate_runtime_monitor_drain_timeout(
    const char* phase, const std::function<void()>& dump_hook) {
  // Fail closed: monitor callbacks can own borrowed kernel state. Once stop()
  // is draining for teardown, continuing after the bound risks UAF and an
  // unbounded wait can hang shutdown.
  std::cerr << "manyforge_runtime_collision_monitor_drain_timeout"
            << " phase=" << phase
            << " timeout_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   kDrainCallbacksTimeout)
                   .count()
            << '\n';
  if (dump_hook) {
    // Best-effort flight-evidence flush before dying; never let the hook
    // turn a fail-closed teardown into an exception escape.
    try {
      dump_hook();
    } catch (...) {
    }
  }
  std::terminate();
}

CollisionPairId canonical_collision_pair(CollisionPairId pair) {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return pair;
}

std::string collision_pair_name(const CollisionPairId& pair) {
  return pair.body_a + " <-> " + pair.body_b;
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

bool collision_body_contains_segment(const std::string& body,
                                     const std::string& segment) {
  return !segment.empty() && body.find(segment) != std::string::npos;
}

bool policy_revisions_equal(const CollisionPolicyRevision& a,
                            const CollisionPolicyRevision& b) {
  return a.revision_id == b.revision_id && a.content_hash == b.content_hash;
}

bool policy_revision_is_set(const CollisionPolicyRevision& revision) {
  return revision.revision_id != 0 || !revision.content_hash.empty();
}

bool contact_grasp_pair_matches(const CollisionPairId& pair,
                                const ContactGraspPolicyEntry& entry) {
  auto object_matches = [&](const std::string& body) {
    return collision_body_contains_segment(
        body, "object/" + entry.selected_object_id + "/");
  };
  auto link_matches = [&](const std::string& body) {
    for (const auto& link : entry.allowed_robot_links) {
      if (collision_body_contains_segment(
              body, "/link/" + link + "/collision/")) {
        return true;
      }
    }
    return false;
  };
  return (object_matches(pair.body_a) && link_matches(pair.body_b)) ||
         (object_matches(pair.body_b) && link_matches(pair.body_a));
}

const ContactGraspPolicyEntry* find_contact_grasp_policy(
    const CollisionPolicySnapshot& policy_snapshot,
    const std::string& contact_policy_id) {
  if (contact_policy_id.empty()) {
    return nullptr;
  }
  for (const auto& entry : policy_snapshot.contact_grasps) {
    if (entry.policy_id == contact_policy_id) {
      return &entry;
    }
  }
  return nullptr;
}

bool contact_grasp_policy_valid(const ContactGraspPolicyEntry& entry) {
  return !entry.policy_id.empty() && !entry.selected_object_id.empty() &&
         !entry.allowed_robot_links.empty() &&
         std::isfinite(entry.max_penetration_m) &&
         entry.max_penetration_m >= 0.0 &&
         std::isfinite(entry.terminal_window_start_fraction) &&
         entry.terminal_window_start_fraction >= 0.0 &&
         entry.terminal_window_start_fraction <= 1.0;
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

CollisionDecisionSeverity severity_for_runtime_collision_decision(
    CollisionDecisionResult decision) {
  switch (decision) {
    case CollisionDecisionResult::kPass:
      return CollisionDecisionSeverity::kNone;
    case CollisionDecisionResult::kWarn:
      return CollisionDecisionSeverity::kWarn;
    case CollisionDecisionResult::kFail:
    case CollisionDecisionResult::kStopRequested:
    case CollisionDecisionResult::kInconclusiveFail:
      return CollisionDecisionSeverity::kBlock;
  }
  return CollisionDecisionSeverity::kBlock;
}

CollisionDecisionAction action_for_runtime_collision_decision(
    CollisionDecisionResult decision) {
  switch (decision) {
    case CollisionDecisionResult::kPass:
      return CollisionDecisionAction::kDispatch;
    case CollisionDecisionResult::kWarn:
      return CollisionDecisionAction::kWarnOnly;
    case CollisionDecisionResult::kInconclusiveFail:
      return CollisionDecisionAction::kInconclusive;
    case CollisionDecisionResult::kFail:
    case CollisionDecisionResult::kStopRequested:
      return CollisionDecisionAction::kReject;
  }
  return CollisionDecisionAction::kReject;
}

void recompute_collision_result_from_classifications(CollisionResult& result) {
  CollisionPairResult* first_failure = nullptr;
  bool any_blocking = false;
  bool any_inconclusive = false;
  for (auto& classified : result.normalized_pairs_classified) {
    classified.is_first_failure = false;
    if (!collision_pair_classification_blocks_dispatch(
            classified.classification)) {
      continue;
    }
    if (first_failure == nullptr) {
      first_failure = &classified;
    }
    if (classified.classification ==
        CollisionPairClassification::kDiagnosticIncomplete) {
      any_inconclusive = true;
    } else {
      any_blocking = true;
    }
  }
  if (first_failure != nullptr) {
    first_failure->is_first_failure = true;
  }
  if (any_inconclusive && !any_blocking) {
    result.decision = CollisionDecisionResult::kInconclusiveFail;
  } else if (any_blocking) {
    result.decision = CollisionDecisionResult::kFail;
  } else {
    result.decision = CollisionDecisionResult::kPass;
  }
}

void recompute_collision_validation_decision(
    CollisionValidationDecisionBundle& bundle) {
  CollisionDecisionResult worst_decision = CollisionDecisionResult::kPass;
  CollisionDecisionSeverity worst_severity = CollisionDecisionSeverity::kNone;
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;

  for (const auto& record : bundle.waypoint_records) {
    const auto severity =
        severity_for_runtime_collision_decision(record.result.decision);
    if (severity > worst_severity ||
        (severity == worst_severity &&
         record.result.decision != CollisionDecisionResult::kPass &&
         worst_decision == CollisionDecisionResult::kPass)) {
      worst_decision = record.result.decision;
      worst_severity = severity;
    }
    if (record.result.decision != CollisionDecisionResult::kPass &&
        !first_failing_waypoint.has_value()) {
      first_failing_waypoint = record.waypoint_index;
      first_failing_time_ns = record.time_from_start_ns;
    }
  }

  bundle.decision.accepted =
      worst_decision == CollisionDecisionResult::kPass && !bundle.cancelled;
  bundle.decision.severity = worst_severity;
  bundle.decision.action = action_for_runtime_collision_decision(worst_decision);
  bundle.decision.first_failing_waypoint = first_failing_waypoint;
  bundle.decision.first_failing_time_ns = first_failing_time_ns;
  if (!bundle.borrowed_world_revision.content_hash.empty() ||
      bundle.borrowed_world_revision.revision_id != 0) {
    bundle.decision.collision_world_revision = bundle.borrowed_world_revision;
  }
}

}  // namespace

RuntimeCollisionMonitor::RuntimeCollisionMonitor(
    IClock& clock,
    IScheduler& scheduler,
    Config config,
    TickCallback tick_callback,
    ShouldContinueCallback should_continue_callback)
    : clock_(clock),
      scheduler_(scheduler),
      config_(config),
      tick_callback_(std::move(tick_callback)),
      should_continue_callback_(std::move(should_continue_callback)) {}

RuntimeCollisionMonitor::~RuntimeCollisionMonitor() {
  stop(/*drain_callbacks=*/true);
}

void RuntimeCollisionMonitor::set_fail_closed_dump_hook(
    std::function<void()> hook) {
  fail_closed_dump_hook_ = std::move(hook);
}

void RuntimeCollisionMonitor::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = true;
  ++generation_;
  if (handle_ != 0) {
    scheduler_.cancel(handle_);
    handle_ = 0;
  }
  schedule_next_locked(generation_);
}

void RuntimeCollisionMonitor::stop(bool drain_callbacks) {
  if (!t_inside_runtime_collision_monitor_callback) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_ = false;
      ++generation_;
      if (handle_ != 0) {
        scheduler_.cancel(handle_);
        handle_ = 0;
      }
    }
    if (drain_callbacks) {
      if (!scheduler_.drain_for(SchedulerPriority::Safety,
                                kDrainCallbacksTimeout)) {
        terminate_runtime_monitor_drain_timeout("scheduler_safety_lane",
                                                 fail_closed_dump_hook_);
      }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    auto no_callbacks_in_flight = [this]() {
      return callbacks_in_flight_ == 0;
    };
    if (drain_callbacks) {
      if (!cv_.wait_for(lock, kDrainCallbacksTimeout, no_callbacks_in_flight)) {
        terminate_runtime_monitor_drain_timeout("monitor_callbacks",
                                                 fail_closed_dump_hook_);
      }
    } else {
      (void)cv_.wait_for(lock, std::chrono::milliseconds(250),
                         no_callbacks_in_flight);
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    ++generation_;
    if (handle_ != 0) {
      scheduler_.cancel(handle_);
      handle_ = 0;
    }
  }
}

void RuntimeCollisionMonitor::set_active_contact_policy_id(
    std::string contact_policy_id) {
  std::lock_guard<std::mutex> lock(contact_policy_mutex_);
  active_contact_policy_id_ = std::move(contact_policy_id);
}

void RuntimeCollisionMonitor::clear_active_contact_policy_id() {
  std::lock_guard<std::mutex> lock(contact_policy_mutex_);
  active_contact_policy_id_.clear();
}

std::string RuntimeCollisionMonitor::active_contact_policy_id() const {
  std::lock_guard<std::mutex> lock(contact_policy_mutex_);
  return active_contact_policy_id_;
}

RuntimeCollisionPolicyView RuntimeCollisionMonitor::runtime_policy_snapshot(
    std::shared_ptr<const CollisionPolicySnapshot> policy_snapshot) const {
  RuntimeCollisionPolicyView empty_view;
  if (!policy_snapshot) {
    auto empty_snapshot = std::make_shared<CollisionPolicySnapshot>();
    empty_view.snapshot = empty_snapshot;
    empty_view.index = build_collision_policy_index(*empty_snapshot);
    return empty_view;
  }

  const bool cacheable = policy_revision_is_set(policy_snapshot->revision);
  if (cacheable) {
    std::lock_guard<std::mutex> lock(runtime_policy_cache_mutex_);
    if (runtime_policy_cache_revision_ &&
        policy_revisions_equal(*runtime_policy_cache_revision_,
                               policy_snapshot->revision)) {
      return runtime_policy_cache_view_;
    }
  }

  auto adjusted =
      std::make_shared<CollisionPolicySnapshot>(*policy_snapshot);
  for (auto& entry : adjusted->support_contacts) {
    if (entry.mode == CollisionContactPolicyMode::kContactRelease) {
      // Runtime ticks are single observed states, not complete trajectories.
      // The per-move validation proves final separation; the runtime monitor
      // enforces bounded/non-increasing penetration across ticks below.
      entry.require_separation_by_end = false;
      entry.require_strictly_decreasing_penetration = false;
    }
  }
  RuntimeCollisionPolicyView view;
  view.snapshot = adjusted;
  view.index = build_collision_policy_index(*adjusted);
  if (!cacheable) {
    return view;
  }
  std::lock_guard<std::mutex> lock(runtime_policy_cache_mutex_);
  runtime_policy_cache_revision_ = policy_snapshot->revision;
  runtime_policy_cache_view_ = view;
  return runtime_policy_cache_view_;
}

std::optional<std::map<std::string, std::string>>
RuntimeCollisionMonitor::apply_active_contact_grasp_policy(
    const CollisionPolicySnapshot& policy_snapshot,
    CollisionValidationDecisionBundle& bundle) const {
  const auto policy_id = active_contact_policy_id();
  if (policy_id.empty()) {
    return std::nullopt;
  }

  std::map<std::string, std::string> config_fault{
      {"reason_code", reason_codes::kRuntimeContactGraspMonitor},
      {"contact_policy_id", policy_id},
  };
  const auto* active = find_contact_grasp_policy(policy_snapshot, policy_id);
  if (active == nullptr) {
    config_fault["reason"] = reason_codes::kRuntimeContactGraspUnknownPolicy;
    return config_fault;
  }
  if (!contact_grasp_policy_valid(*active)) {
    config_fault["reason"] = reason_codes::kRuntimeContactGraspInvalidPolicy;
    return config_fault;
  }
  if (bundle.waypoint_records.empty()) {
    return std::nullopt;
  }

  bool mutated = false;
  for (auto& record : bundle.waypoint_records) {
    if (record.result.engine_status != CollisionEngineStatus::kOk) {
      continue;
    }
    bool record_mutated = false;
    for (auto& classified : record.result.normalized_pairs_classified) {
      if (classified.classification !=
          CollisionPairClassification::kBlockingCollision) {
        continue;
      }
      if (!contact_grasp_pair_matches(classified.pair, *active)) {
        continue;
      }

      record.result.raw_diagnostics["contact_grasp_policy_id"] =
          active->policy_id;
      record.result.raw_diagnostics["contact_grasp_selected_object_id"] =
          active->selected_object_id;
      record.result.raw_diagnostics["contact_grasp_pair"] =
          collision_pair_name(classified.pair);
      record.result.raw_diagnostics["contact_grasp_max_penetration_m"] =
          std::to_string(active->max_penetration_m);

      const auto clearance =
          pair_clearance_for_result(record.result, classified.pair);
      if (!clearance.has_value() || !std::isfinite(*clearance)) {
        record.result.raw_diagnostics["contact_grasp_violation"] =
            reason_codes::kMissingGraspContactClearance;
        continue;
      }
      const double depth = std::max(0.0, -*clearance);
      record.result.raw_diagnostics["contact_grasp_signed_clearance_m"] =
          std::to_string(*clearance);
      record.result.raw_diagnostics["contact_grasp_current_penetration_m"] =
          std::to_string(depth);

      if (depth > active->max_penetration_m + 1e-9) {
        record.result.raw_diagnostics["contact_grasp_violation"] =
            reason_codes::kGraspContactPenetrationExceeded;
        continue;
      }

      classified.classification =
          CollisionPairClassification::kAllowedContactGrasp;
      classified.is_first_failure = false;
      record.result.raw_diagnostics["contact_grasp_allowed_pair"] =
          collision_pair_name(classified.pair);
      record_mutated = true;
      mutated = true;
    }
    if (record_mutated) {
      recompute_collision_result_from_classifications(record.result);
      record.produced_decision_change =
          record.result.decision != CollisionDecisionResult::kPass;
    }
  }

  if (mutated) {
    recompute_collision_validation_decision(bundle);
  }
  return std::nullopt;
}

void RuntimeCollisionMonitor::reset_contact_release_state() {
  std::lock_guard<std::mutex> lock(contact_policy_mutex_);
  contact_release_state_.clear();
}

std::optional<std::map<std::string, std::string>>
RuntimeCollisionMonitor::update_contact_release_state(
    const CollisionPolicySnapshot& policy_snapshot,
    const CollisionValidationDecisionBundle& bundle) {
  if (policy_snapshot.support_contacts.empty() ||
      bundle.waypoint_records.empty()) {
    return std::nullopt;
  }
  const auto& result = bundle.waypoint_records.front().result;
  if (t_inside_runtime_collision_monitor_callback &&
      t_runtime_collision_monitor_instance == this) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ ||
        generation_ != t_runtime_collision_monitor_generation) {
      return std::nullopt;
    }
  }
  std::lock_guard<std::mutex> lock(contact_policy_mutex_);
  for (const auto& entry : policy_snapshot.support_contacts) {
    auto make_details = [&]() {
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
      return details;
    };

    if (entry.mode == CollisionContactPolicyMode::kContactProcess) {
      auto details = make_details();
      details["reason"] = "runtime_contact_process_policy_not_implemented";
      return details;
    }

    auto clearance = pair_clearance_for_result(result, entry.pair);
    if (!clearance.has_value() || !std::isfinite(*clearance)) {
      auto details = make_details();
      details["reason"] = "runtime_contact_release_missing_clearance";
      return details;
    }
    const double tolerance = std::max(0.0, entry.release_tolerance_m);
    const double depth = *clearance < 0.0 ? -*clearance : 0.0;
    const bool separated =
        *clearance >= std::max(0.0, entry.min_separation_clearance_m);
    auto make_depth_details = [&]() {
      auto details = make_details();
      details["contact_release_signed_clearance_m"] =
          std::to_string(*clearance);
      details["contact_release_current_penetration_m"] =
          std::to_string(depth);
      return details;
    };

    const auto key = collision_pair_key(entry.pair);
    auto it = contact_release_state_.find(key);
    if (it == contact_release_state_.end()) {
      if (entry.enforce_max_start_penetration &&
          depth > std::max(0.0, entry.max_start_penetration_m) + tolerance) {
        auto details = make_depth_details();
        details["reason"] =
            "runtime_contact_release_initial_penetration_exceeds_policy";
        return details;
      }
      ContactReleasePairState state;
      state.initial_penetration_m = depth;
      state.last_penetration_m = depth;
      state.separated = separated;
      contact_release_state_.emplace(key, state);
      continue;
    }

    auto& state = it->second;
    auto make_state_details = [&]() {
      auto details = make_depth_details();
      details["contact_release_initial_penetration_m"] =
          std::to_string(state.initial_penetration_m);
      details["contact_release_previous_penetration_m"] =
          std::to_string(state.last_penetration_m);
      return details;
    };

    if (state.separated && depth > tolerance && !entry.allow_recontact) {
      auto details = make_state_details();
      details["reason"] = "runtime_contact_release_recontact";
      return details;
    }
    if (depth > state.last_penetration_m + tolerance) {
      auto details = make_state_details();
      details["reason"] = "runtime_contact_release_penetration_increased";
      return details;
    }
    state.last_penetration_m = depth;
    state.separated = state.separated || separated;
  }
  return std::nullopt;
}

void RuntimeCollisionMonitor::schedule_next_locked(std::uint64_t generation) {
  handle_ = scheduler_.schedule(
      clock_.now() + config_.period_ns, [this, generation]() {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (!active_ || generation_ != generation) {
            return;
          }
          handle_ = 0;
          ++callbacks_in_flight_;
        }

        struct CallbackGuard {
          RuntimeCollisionMonitor* self;
          bool was_inside = t_inside_runtime_collision_monitor_callback;
          RuntimeCollisionMonitor* previous_instance =
              t_runtime_collision_monitor_instance;
          std::uint64_t previous_generation =
              t_runtime_collision_monitor_generation;

          explicit CallbackGuard(RuntimeCollisionMonitor* monitor,
                                 std::uint64_t generation)
              : self(monitor) {
            t_inside_runtime_collision_monitor_callback = true;
            t_runtime_collision_monitor_instance = monitor;
            t_runtime_collision_monitor_generation = generation;
          }

          ~CallbackGuard() {
            t_inside_runtime_collision_monitor_callback = was_inside;
            t_runtime_collision_monitor_instance = previous_instance;
            t_runtime_collision_monitor_generation = previous_generation;
            {
              std::lock_guard<std::mutex> lock(self->mutex_);
              if (self->callbacks_in_flight_ > 0) {
                --self->callbacks_in_flight_;
              }
            }
            self->cv_.notify_all();
          }
        } callback_guard{this, generation};

        if (tick_callback_) {
          (void)tick_callback_();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || generation_ != generation ||
            (should_continue_callback_ && !should_continue_callback_())) {
          return;
        }
        schedule_next_locked(generation);
      });
}

}  // namespace manyforge::core
