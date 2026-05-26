#include <manyforge_core/interrupt/interrupt_manager.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace manyforge::core {
namespace {

ErrorDTO interrupt_error(std::string reason, std::string message) {
  return ErrorDTO{kErrorInvalidRequest, std::move(message), {{"reason", std::move(reason)}}};
}

std::string bool_string(bool value) {
  return value ? "true" : "false";
}

std::string signal_value_to_string(const InterruptSignalValue& value) {
  if (const auto* b = std::get_if<bool>(&value)) {
    return bool_string(*b);
  }
  if (const auto* d = std::get_if<double>(&value)) {
    return std::to_string(*d);
  }
  return "structured";
}

bool value_is_active(const InterruptSignalValue& value,
                     const std::optional<InterruptSignalValue>& hysteresis) {
  if (const auto* b = std::get_if<bool>(&value)) {
    return *b;
  }
  if (const auto* d = std::get_if<double>(&value)) {
    double threshold = 0.0;
    if (hysteresis) {
      if (const auto* h = std::get_if<double>(&*hysteresis)) {
        threshold = *h;
      }
    }
    return *d > threshold;
  }
  return !std::get<InterruptStructuredValue>(value).empty();
}

bool value_is_release(const InterruptSignalValue& value,
                      const std::optional<InterruptSignalValue>& hysteresis) {
  return !value_is_active(value, hysteresis);
}

std::string preemption_action_string(InterruptPreemptionAction action) {
  switch (action) {
    using enum InterruptPreemptionAction;
    case kCancelPlanning: return "cancel_planning";
    case kCancelValidation: return "cancel_validation";
    case kPreemptTrajectory: return "preempt_trajectory";
    case kPreemptStreaming: return "preempt_streaming";
    case kLatchHeld: return "latch_held";
    case kLatchFaulted: return "latch_faulted";
    case kCompleteMoveOnSignal: return "complete_move_on_signal";
  }
  return "unknown";
}

bool result_binding_key_first_char_ok(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool result_binding_key_body_char_ok(char c) {
  return result_binding_key_first_char_ok(c) ||
         (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-';
}

/// Spec/260 §2.6: `result_binding_key`, when present, MUST be a UTF-8
/// string of 1-128 bytes, contain no NUL, and match
/// `[A-Za-z_][A-Za-z0-9_.:-]*`. Because the allowed character set is
/// pure ASCII the byte-length and character-length coincide and we can
/// reject any byte outside the set without a full UTF-8 decode.
/// Phase 4.5D-1: validate the shape of a `MotionInterruptStopProfileDTO`.
/// Mode-and-policy membership checks live in `build_scope_binding_locked`;
/// this helper catches malformed fields that would otherwise pass the
/// presence + membership gates and arm a fundamentally-broken profile.
///
/// Returns an error reason string when the profile is malformed, or
/// `std::nullopt` when the profile is well-formed.
std::optional<std::string> validate_stop_profile_shape(
    const MotionInterruptStopProfileDTO& profile) {
  // 1. profile_id is non-empty. The audit trail uses this string;
  //    an empty id breaks correlation between the outcome record and
  //    the originating deployment / motion-node profile.
  if (profile.profile_id.empty()) {
    return "stop_profile_id_required";
  }
  // 2. Production profiles MUST require backend confirmation (spec/210
  //    §3.11). `false` is reserved for diagnostic simulators and
  //    explicitly cannot produce a `signal_trip` outcome; the kernel
  //    fails closed at arm time so a diagnostic profile never reaches
  //    a production dispatch path by accident.
  if (!profile.require_backend_confirmation) {
    return "stop_profile_require_backend_confirmation_must_be_true";
  }
  // 3. soft_on_path REQUIRES deceleration_time_s (spec/210 §3.11).
  //    Without it the backend cannot compute the path-preserving
  //    ramp-down trajectory.
  if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath &&
      !profile.deceleration_time_s) {
    return "soft_on_path_requires_deceleration_time_s";
  }
  if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath &&
      profile.per_joint_max_deceleration_rad_s2.empty()) {
    return "soft_on_path_requires_per_joint_deceleration_limits";
  }
  // 4. Optional timing fields, when present, MUST be strictly positive.
  //    Zero or negative values are configuration errors: a zero ramp
  //    is degenerate, a negative ramp is meaningless, and the
  //    arithmetic in 4.5D-2's ramp generation would silently misbehave.
  if (profile.deceleration_time_s && *profile.deceleration_time_s <= 0.0) {
    return "stop_profile_deceleration_time_s_must_be_positive";
  }
  if (profile.max_stop_time_s && *profile.max_stop_time_s <= 0.0) {
    return "stop_profile_max_stop_time_s_must_be_positive";
  }
  // 5. Per-joint deceleration limits, when provided inline, MUST all
  //    be strictly positive. Negative or zero per-axis limits would
  //    let the backend produce an unbounded stop.
  for (const auto& [joint_name, limit] :
       profile.per_joint_max_deceleration_rad_s2) {
    if (limit <= 0.0) {
      return "stop_profile_per_joint_max_deceleration_must_be_positive";
    }
    if (joint_name.empty()) {
      return "stop_profile_per_joint_max_deceleration_joint_name_required";
    }
  }
  return std::nullopt;
}

bool result_binding_key_is_valid(const std::string& key) {
  if (key.empty() || key.size() > 128) {
    return false;
  }
  if (!result_binding_key_first_char_ok(key.front())) {
    return false;
  }
  for (std::size_t i = 1; i < key.size(); ++i) {
    if (!result_binding_key_body_char_ok(key[i])) {
      return false;
    }
  }
  return true;
}

std::string activation_mode_string(MotionInterruptActivationMode mode) {
  switch (mode) {
    using enum MotionInterruptActivationMode;
    case kRequireInactiveAtArm: return "require_inactive_at_arm";
    case kFreshEdgeAfterArm: return "fresh_edge_after_arm";
    case kTripIfAlreadyActive: return "trip_if_already_active";
    case kMaintainLevelWithHysteresis: return "maintain_level_with_hysteresis";
  }
  return "unknown";
}

bool is_fault_action(InterruptPreemptionAction action) {
  using enum InterruptPreemptionAction;
  return action == kPreemptTrajectory || action == kLatchHeld ||
         action == kLatchFaulted;
}

std::string preemption_result_string(InterruptPreemptionResult result) {
  switch (result) {
    using enum InterruptPreemptionResult;
    case kPending: return "pending";
    case kSucceeded: return "succeeded";
    case kPartial: return "partial";
    case kBackendPreemptFailed: return "backend_preempt_failed";
    case kNoneRequired: return "none_required";
  }
  return "unknown";
}

std::string disposition_action_string(InterruptDispositionAction action) {
  switch (action) {
    using enum InterruptDispositionAction;
    case kAcknowledge: return "acknowledge";
    case kResume: return "resume";
    case kRestartSubtree: return "restart_subtree";
    case kRestartRun: return "restart_run";
    case kReset: return "reset";
    case kHold: return "hold";
    case kFaultTerminal: return "fault_terminal";
    case kDelegateToRecovery: return "delegate_to_recovery";
  }
  return "unknown";
}

}  // namespace

InterruptManager::InterruptManager(InterruptManagerConfig config)
    : config_(config) {
  if (config_.queue_depth == 0) {
    config_.queue_depth = 1;
  }
}

Result<void> InterruptManager::register_source(
    const InterruptSourceDefinition& source) {
  if (source.source_id.empty()) {
    return Result<void>::Err(interrupt_error("empty_source_id",
                                             "interrupt source_id is required"));
  }
  if (source.signal.empty()) {
    return Result<void>::Err(interrupt_error("empty_signal",
                                             "interrupt signal is required"));
  }
  if (source.priority < 0 || source.priority > 1000) {
    return Result<void>::Err(interrupt_error("invalid_priority",
                                             "interrupt source priority outside [0,1000]"));
  }
  if (source.debounce_ns && *source.debounce_ns < 0) {
    return Result<void>::Err(interrupt_error("invalid_debounce",
                                             "interrupt debounce_ns cannot be negative"));
  }
  if (source.freshness_budget_ns && *source.freshness_budget_ns < 0) {
    return Result<void>::Err(interrupt_error("invalid_freshness_budget",
                                             "interrupt freshness_budget_ns cannot be negative"));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (sources_.contains(source.source_id)) {
    return Result<void>::Err(interrupt_error("duplicate_source_id",
                                             "interrupt source_id already registered"));
  }
  sources_.emplace(source.source_id, source);
  return Result<void>::Ok();
}

Result<void> InterruptManager::register_policy(
    const InterruptPolicyDefinition& policy) {
  if (policy.policy_id.empty()) {
    return Result<void>::Err(interrupt_error("empty_policy_id",
                                             "interrupt policy_id is required"));
  }
  if (policy.source_id.empty()) {
    return Result<void>::Err(interrupt_error("empty_source_id",
                                             "interrupt policy source_id is required"));
  }
  if (policy.priority && (*policy.priority < 0 || *policy.priority > 1000)) {
    return Result<void>::Err(interrupt_error("invalid_priority",
                                             "interrupt policy priority outside [0,1000]"));
  }
  if (policy.affected_phases.empty()) {
    return Result<void>::Err(interrupt_error("empty_affected_phases",
                                             "interrupt policy needs affected phases"));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!sources_.contains(policy.source_id)) {
    return Result<void>::Err(interrupt_error("unknown_source_id",
                                             "interrupt policy references unknown source"));
  }
  if (policies_by_id_.contains(policy.policy_id)) {
    return Result<void>::Err(interrupt_error("duplicate_policy_id",
                                             "interrupt policy_id already registered"));
  }
  // Phase 4.5D-1 defense in depth: allowed_stop_modes is only valid on
  // complete_move_on_signal policies. Runs at the top level (independent
  // of scope_kind) so a session-scoped policy with a smuggled stop-mode
  // set is rejected at registration, not silently ignored at arm time.
  if (!policy.allowed_stop_modes.empty() &&
      policy.preemption_action !=
          InterruptPreemptionAction::kCompleteMoveOnSignal) {
    return Result<void>::Err(interrupt_error(
        "allowed_stop_modes_requires_complete_move_on_signal",
        "allowed_stop_modes is only valid on complete_move_on_signal policies"));
  }
  // Phase 4.5B: move-scoped invariants (spec/260 §2.2).
  if (auto vr = validate_move_scoped_policy_locked(policy); !vr.ok()) {
    return Result<void>::Err(vr.error());
  }
  policies_by_id_.emplace(policy.policy_id, policy);
  policy_ids_by_source_[policy.source_id].push_back(policy.policy_id);
  std::sort(policy_ids_by_source_[policy.source_id].begin(),
            policy_ids_by_source_[policy.source_id].end());
  return Result<void>::Ok();
}

Result<void> InterruptManager::validate_move_scoped_policy_locked(
    const InterruptPolicyDefinition& policy) const {
  using enum InterruptPreemptionAction;
  using enum InterruptPostLatchDisposition;
  using enum InterruptPolicyScopeKind;
  // complete_move_on_signal is reserved for move-scoped policies.
  if (policy.preemption_action == kCompleteMoveOnSignal &&
      policy.scope_kind != kMove) {
    return Result<void>::Err(interrupt_error(
        "complete_move_on_signal_requires_move_scope",
        "complete_move_on_signal is reserved for scope_kind=move policies"));
  }
  if (policy.scope_kind == kMove) {
    if (!policy.affected_phases.contains(
            InterruptExecutionPhase::kTrajectoryExecution)) {
      return Result<void>::Err(interrupt_error(
          "move_scoped_policy_missing_trajectory_execution_phase",
          "scope_kind=move policies must include trajectory_execution in affected_phases"));
    }
    // await_operator_ack is forbidden for all move-scoped policies
    // (spec/260 §2.2): a move-scoped policy whose latch outlives the
    // motion execution would block the execution surface waiting for an
    // operator; the kernel's motion-outcome lifecycle already provides
    // the clear path. Same-source fault behavior belongs on a session
    // policy ranked above the move-scoped trip.
    if (policy.post_latch_disposition == kAwaitOperatorAck) {
      return Result<void>::Err(interrupt_error(
          "move_scoped_policy_disposition_not_allowed",
          "scope_kind=move policies cannot use await_operator_ack"));
    }
    // complete_move_on_signal requires fast-execution-relevant source.
    if (policy.preemption_action == kCompleteMoveOnSignal) {
      const auto src_it = sources_.find(policy.source_id);
      if (src_it != sources_.end() &&
          src_it->second.source_class !=
              InterruptSourceClass::kFastExecutionRelevant) {
        return Result<void>::Err(interrupt_error(
            "complete_move_on_signal_requires_fast_source",
            "complete_move_on_signal must bind a fast_execution_relevant source"));
      }
      if (policy.post_latch_disposition !=
          InterruptPostLatchDisposition::kAutoClearAfterMotionOutcome) {
        return Result<void>::Err(interrupt_error(
            "complete_move_on_signal_requires_auto_clear_after_motion_outcome",
            "complete_move_on_signal must use post_latch_disposition=auto_clear_after_motion_outcome"));
      }
      // Phase 4.5D-1 (spec/260 §2.2): complete_move_on_signal policies
      // MUST declare a non-empty allowed_stop_modes set. The deployment
      // policy gate exists independently of any backend's capability;
      // arm-time validation enforces both. Without this, a malformed
      // policy could let a scope's stop_profile slip through with no
      // deployment-level constraint.
      if (policy.allowed_stop_modes.empty()) {
        return Result<void>::Err(interrupt_error(
            "complete_move_on_signal_requires_allowed_stop_modes",
            "complete_move_on_signal must declare a non-empty allowed_stop_modes set"));
      }
    }
  }

  // Same-source priority constraint (spec/260 §2.2 move-scoped invariants):
  // a complete_move_on_signal policy MUST have strictly LOWER effective
  // priority than any same-source session-scoped policy with a fault action
  // (preempt_trajectory / latch_held / latch_faulted). The kernel rejects
  // equal-or-lower-priority safety/fault configurations.
  const int new_effective = policy.priority.value_or(
      sources_.at(policy.source_id).priority);
  for (const auto& other_id :
       policy_ids_by_source_.contains(policy.source_id)
           ? policy_ids_by_source_.at(policy.source_id)
           : std::vector<std::string>{}) {
    const auto& other = policies_by_id_.at(other_id);
    const int other_effective = other.priority.value_or(
        sources_.at(other.source_id).priority);
    // Trip vs fault interlock.
    const bool new_is_trip = policy.preemption_action == kCompleteMoveOnSignal;
    const bool other_is_trip = other.preemption_action == kCompleteMoveOnSignal;
    const bool new_is_fault = is_fault_action(policy.preemption_action) &&
                               policy.scope_kind == kSession;
    const bool other_is_fault = is_fault_action(other.preemption_action) &&
                                 other.scope_kind == kSession;
    if (new_is_trip && other_is_fault &&
        other_effective <= new_effective) {
      return Result<void>::Err(interrupt_error(
          "move_scoped_trip_priority_not_below_fault",
          "complete_move_on_signal must have strictly lower priority than same-source fault policy"));
    }
    if (other_is_trip && new_is_fault &&
        new_effective <= other_effective) {
      return Result<void>::Err(interrupt_error(
          "move_scoped_trip_priority_not_below_fault",
          "same-source fault policy must have strictly higher priority than complete_move_on_signal"));
    }
    // Equal-priority same-source policy registration rejected unless
    // both actions and dispositions match exactly.
    if (other_effective == new_effective &&
        (other.preemption_action != policy.preemption_action ||
         other.post_latch_disposition != policy.post_latch_disposition)) {
      return Result<void>::Err(interrupt_error(
          "same_source_equal_priority_action_mismatch",
          "same-source policies at equal effective priority must share preemption_action and post_latch_disposition"));
    }
  }
  return Result<void>::Ok();
}

InterruptIngestResult InterruptManager::ingest(
    const InterruptSignalUpdate& update,
    InterruptSourceClass attempted_lane,
    const InterruptRuntimeContext& context,
    std::int64_t kernel_now_ns) {
  return ingest_batch({update}, attempted_lane, context, kernel_now_ns);
}

InterruptIngestResult InterruptManager::ingest_batch(
    const std::vector<InterruptSignalUpdate>& updates,
    InterruptSourceClass attempted_lane,
    const InterruptRuntimeContext& context,
    std::int64_t kernel_now_ns) {
  InterruptIngestResult result;
  if (updates.empty()) {
    result.rejected = true;
    result.reason = "empty_batch";
    return result;
  }

  struct Candidate {
    InterruptSignalUpdate update;
    ResolvedPolicy resolved;
  };
  std::vector<Candidate> candidates;

  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& update : updates) {
    // Phase 4.5B: source-level data-quality gates (lane + freshness)
    // run before sample seeding so a stale or wrong-lane update can
    // never shift the baseline that drives activation-mode decisions
    // on armed move-scoped bindings. "No matching policy" is NOT a
    // data-quality issue, so we still seed before resolving.
    const auto src_it = sources_.find(update.source_id);
    if (src_it == sources_.end()) {
      result.rejected = true;
      result.reason = "unknown_source_id";
      result.events.push_back(
          {"interrupt_signal_rejected",
           {{"source_id", update.source_id},
            {"sequence_id", std::to_string(update.sequence_id)},
            {"reason", result.reason}},
           std::nullopt});
      continue;
    }
    if (attempted_lane != src_it->second.source_class) {
      result.rejected = true;
      result.reason = "source_class_violation";
      const auto fresh = freshness_ns(update, kernel_now_ns);
      result.events.push_back(
          {"interrupt_signal_rejected_class_violation",
           {{"source_id", update.source_id},
            {"sequence_id", std::to_string(update.sequence_id)},
            {"freshness_ns", fresh ? std::to_string(*fresh) : ""},
            {"reason", result.reason}},
           std::nullopt});
      continue;
    }
    const auto fresh = freshness_ns(update, kernel_now_ns);
    if (src_it->second.freshness_budget_ns && fresh &&
        *fresh > *src_it->second.freshness_budget_ns) {
      result.rejected = true;
      result.reason = "stale";
      result.events.push_back(
          {"interrupt_signal_rejected_stale",
           {{"source_id", update.source_id},
            {"sequence_id", std::to_string(update.sequence_id)},
            {"freshness_ns", fresh ? std::to_string(*fresh) : ""},
            {"reason", result.reason}},
           std::nullopt});
      continue;
    }
    // Data-quality OK: seed the baseline so an inactive sample arriving
    // before arming is observable to subsequent arm calls. Out-of-order
    // replays are filtered inside update_source_sample_locked.
    update_source_sample_locked(update);

    auto resolved = resolve_policy_locked(update, context);
    if (!resolved.ok()) {
      result.rejected = true;
      result.reason = resolved.error().details.contains("reason")
                          ? resolved.error().details.at("reason")
                          : resolved.error().code;
      result.events.push_back(
          {"interrupt_signal_rejected", {{"source_id", update.source_id},
                                         {"sequence_id", std::to_string(update.sequence_id)},
                                         {"reason", result.reason}}, std::nullopt});
      continue;
    }
    auto validation =
        validate_update_locked(update, attempted_lane, resolved.value(), kernel_now_ns);
    if (!validation.ok()) {
      result.rejected = true;
      result.reason = validation.error().details.contains("reason")
                          ? validation.error().details.at("reason")
                          : validation.error().code;
      const std::string event_type =
          result.reason == "stale" ? "interrupt_signal_rejected_stale"
                                   : (result.reason == "source_class_violation"
                                          ? "interrupt_signal_rejected_class_violation"
                                          : "interrupt_signal_rejected");
      const auto fresh2 = freshness_ns(update, kernel_now_ns);
      result.events.push_back(
          {event_type,
           {{"source_id", update.source_id},
            {"sequence_id", std::to_string(update.sequence_id)},
            {"freshness_ns", fresh2 ? std::to_string(*fresh2) : ""},
            {"reason", result.reason}},
           std::nullopt});
      continue;
    }
    if (!signal_requests_latch(resolved.value().source, update)) {
      if (active_latch_ &&
          active_latch_->source_id == update.source_id &&
          signal_requests_release(resolved.value().source, update) &&
          resolved.value().policy.post_latch_disposition ==
              InterruptPostLatchDisposition::kAutoClearOnSignalRelease) {
        auto clear = clear_active_latch_locked("auto_signal_release", kernel_now_ns);
        result.events.insert(result.events.end(),
                             clear.value().events.begin(),
                             clear.value().events.end());
        result.accepted = true;
        result.active_latch = active_latch_;
        continue;
      }
      result.accepted = true;
      result.events.push_back(
          {"interrupt_signal_ingested", {{"source_id", update.source_id},
                                         {"sequence_id", std::to_string(update.sequence_id)},
                                         {"accepted", "true"},
                                         {"latch_requested", "false"}},
           std::nullopt});
      continue;
    }
    candidates.push_back({update, resolved.value()});
  }

  if (candidates.empty()) {
    result.active_latch = active_latch_;
    return result;
  }

  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                     const Candidate& b) {
    if (a.resolved.effective_priority != b.resolved.effective_priority) {
      return a.resolved.effective_priority > b.resolved.effective_priority;
    }
    if (a.update.source_id != b.update.source_id) {
      return a.update.source_id < b.update.source_id;
    }
    return a.update.sequence_id < b.update.sequence_id;
  });
  const bool tied =
      candidates.size() > 1 &&
      candidates[0].resolved.effective_priority ==
          candidates[1].resolved.effective_priority;

  auto first = ingest_resolved_locked(
      candidates.front().update,
      candidates.front().resolved,
      context,
      kernel_now_ns,
      tied ? std::optional<InterruptTieBreak>{InterruptTieBreak::kLexicographic}
           : std::nullopt);
  result.accepted = result.accepted || first.accepted;
  result.latched = first.latched;
  result.queued = result.queued || first.queued;
  result.suppressed = result.suppressed || first.suppressed;
  result.rejected = result.rejected || first.rejected;
  result.reason = first.reason.empty() ? result.reason : first.reason;
  result.events.insert(result.events.end(), first.events.begin(), first.events.end());
  result.active_latch = first.active_latch;

  // Phase 4.5B: when the winning latch is a move-scoped desired trip,
  // same-window losing candidates that are themselves move-scoped trips
  // emit `motion_trip_secondary_signal` and are NOT queued. Operator
  // dispositioning does not apply to desired-trip outcomes (spec/260
  // §D.3 and §7). Other losing candidates (session policies, fault
  // policies) follow the legacy queue path.
  const auto winning_policy = active_policy_locked();
  const bool winning_is_move_trip =
      winning_policy &&
      winning_policy->scope_kind == InterruptPolicyScopeKind::kMove &&
      winning_policy->preemption_action ==
          InterruptPreemptionAction::kCompleteMoveOnSignal;
  for (std::size_t idx = 1; idx < candidates.size(); ++idx) {
    const auto& cand = candidates[idx];
    const bool cand_is_move_trip =
        cand.resolved.policy.scope_kind == InterruptPolicyScopeKind::kMove &&
        cand.resolved.policy.preemption_action ==
            InterruptPreemptionAction::kCompleteMoveOnSignal;
    if (winning_is_move_trip && cand_is_move_trip && active_latch_) {
      const auto& winner_resolved = candidates.front().resolved;
      std::string arbitration_reason;
      if (cand.resolved.effective_priority < winner_resolved.effective_priority) {
        arbitration_reason = "priority";
      } else if (cand.update.source_id != active_latch_->source_id) {
        arbitration_reason = "source_lex";
      } else {
        arbitration_reason = "sequence_lex";
      }
      result.events.push_back(
          {"motion_trip_secondary_signal",
           {{"winning_latch_id", active_latch_->latch_id},
            {"losing_source_id", cand.update.source_id},
            {"losing_policy_id", cand.resolved.policy.policy_id},
            {"losing_sequence_id", std::to_string(cand.update.sequence_id)},
            {"arbitration_reason", arbitration_reason}},
           *active_latch_});
      continue;
    }
    InterruptIngestResult queued;
    queue_update_locked(cand.update,
                        cand.resolved,
                        context,
                        kernel_now_ns,
                        queued.events,
                        queued);
    result.queued = result.queued || queued.queued;
    result.rejected = result.rejected || queued.rejected;
    result.events.insert(result.events.end(), queued.events.begin(), queued.events.end());
  }
  result.active_latch = active_latch_;
  return result;
}

Result<InterruptIngestResult> InterruptManager::apply_disposition(
    const InterruptDisposition& disposition,
    std::int64_t kernel_now_ns) {
  if (disposition.principal_id.empty()) {
    return Result<InterruptIngestResult>::Err(
        interrupt_error("empty_principal_id",
                        "interrupt disposition requires principal_id"));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  InterruptIngestResult result;
  if (!active_latch_ || active_latch_->latch_id != disposition.latch_id) {
    result.rejected = true;
    result.reason = "stale_latch";
    result.events.push_back(
        {"interrupt_disposition_applied",
         {{"latch_id", disposition.latch_id},
          {"disposition.action", disposition_action_string(disposition.action)},
          {"principal_id", disposition.principal_id},
          {"result", "stale_latch"}},
         std::nullopt});
    return Result<InterruptIngestResult>::Ok(std::move(result));
  }
  if (!disposition_allowed(*active_latch_, disposition)) {
    result.rejected = true;
    result.reason = "rejected_policy";
    result.events.push_back(
        {"interrupt_disposition_applied",
         {{"latch_id", disposition.latch_id},
          {"disposition.action", disposition_action_string(disposition.action)},
          {"principal_id", disposition.principal_id},
          {"result", "rejected_policy"}},
         *active_latch_});
    return Result<InterruptIngestResult>::Ok(std::move(result));
  }

  active_latch_->disposition = disposition;
  result.events.push_back(
      {"interrupt_disposition_applied",
       {{"latch_id", disposition.latch_id},
        {"disposition.action", disposition_action_string(disposition.action)},
        {"principal_id", disposition.principal_id},
        {"result", "accepted"}},
       *active_latch_});

  std::string clear_reason = "operator_ack";
  switch (disposition.action) {
    case InterruptDispositionAction::kReset:
      clear_reason = "reset";
      break;
    case InterruptDispositionAction::kDelegateToRecovery:
      clear_reason = "recovery_delegated";
      break;
    case InterruptDispositionAction::kFaultTerminal:
      clear_reason = "fault_terminal";
      break;
    default:
      break;
  }
  auto clear = clear_active_latch_locked(std::move(clear_reason), kernel_now_ns);
  result.accepted = true;
  result.events.insert(result.events.end(),
                       clear.value().events.begin(),
                       clear.value().events.end());
  result.active_latch = active_latch_;
  return Result<InterruptIngestResult>::Ok(std::move(result));
}

Result<InterruptIngestResult> InterruptManager::clear_active_latch(
    std::string clear_reason,
    std::int64_t kernel_now_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  return clear_active_latch_locked(std::move(clear_reason), kernel_now_ns);
}

Result<InterruptIngestResult> InterruptManager::mark_active_preemption_result(
    InterruptPreemptionResult result,
    std::int64_t preemption_latency_ns,
    std::int64_t /*kernel_now_ns*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_latch_) {
    return Result<InterruptIngestResult>::Err(
        interrupt_error("no_active_latch",
                        "Cannot mark interrupt preemption without an active latch"));
  }

  active_latch_->preemption_result = result;
  active_latch_->preemption_latency_ns =
      std::max<std::int64_t>(0, preemption_latency_ns);

  InterruptIngestResult ingest_result;
  ingest_result.accepted = true;
  ingest_result.active_latch = active_latch_;
  ingest_result.events.push_back(
      {"interrupt_preempt_applied",
       {{"latch_id", active_latch_->latch_id},
        {"preemption_action",
         preemption_action_string(active_latch_->preemption_action_requested)},
        {"preemption_result", preemption_result_string(result)},
        {"preemption_latency_ns",
         std::to_string(active_latch_->preemption_latency_ns.value_or(0))}},
       active_latch_});
  return Result<InterruptIngestResult>::Ok(std::move(ingest_result));
}

Result<InterruptIngestResult> InterruptManager::clear_active_latch_locked(
    std::string clear_reason,
    std::int64_t kernel_now_ns) {
  InterruptIngestResult result;
  if (!active_latch_) {
    result.rejected = true;
    result.reason = "no_active_latch";
    return Result<InterruptIngestResult>::Ok(std::move(result));
  }
  InterruptLatch cleared = *active_latch_;
  cleared.cleared_at_ns = kernel_now_ns;
  cleared.clear_reason = clear_reason;
  const auto clear_latency = kernel_now_ns - cleared.latched_at_ns;
  result.events.push_back(
      {"interrupt_cleared",
       {{"latch_id", cleared.latch_id},
        {"clear_reason", clear_reason},
        {"clear_latency_ns", std::to_string(clear_latency)}},
       cleared});
  active_latch_.reset();

  // Spec/260 §B.2 defense-in-depth: queued updates carry a
  // pre-resolved policy snapshot. If the binding behind that policy
  // was reaped while the update sat in `queue_`, replaying it through
  // `ingest_resolved_locked` would latch a move-scoped policy with no
  // live scope binding. `reap_motion_interrupt_scopes` purges such
  // entries directly, but we re-verify here so a future reorder or
  // bug cannot reintroduce the leak.
  while (true) {
    auto next = pop_next_queued_locked();
    if (!next) {
      break;
    }
    if (next->resolved.policy.scope_kind == InterruptPolicyScopeKind::kMove) {
      const auto it = active_scope_id_by_policy_id_.find(
          next->resolved.policy.policy_id);
      if (it == active_scope_id_by_policy_id_.end()) {
        InterruptStructuredValue drop_details{
            {"source_id", next->update.source_id},
            {"sequence_id", std::to_string(next->update.sequence_id)},
            {"policy_id", next->resolved.policy.policy_id},
            {"reason", "scope_reaped_before_replay"},
        };
        result.events.push_back({"motion_interrupt_queued_update_dropped",
                                 std::move(drop_details), std::nullopt});
        continue;
      }
    }
    auto latched = ingest_resolved_locked(next->update,
                                          next->resolved,
                                          next->context,
                                          kernel_now_ns,
                                          next->tie_break);
    result.events.insert(result.events.end(),
                         latched.events.begin(),
                         latched.events.end());
    result.accepted = true;
    result.latched = latched.latched;
    result.active_latch = active_latch_;
    break;
  }
  return Result<InterruptIngestResult>::Ok(std::move(result));
}

std::optional<InterruptLatch> InterruptManager::active_latch() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_latch_;
}

std::size_t InterruptManager::queued_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

Result<InterruptManager::ResolvedPolicy> InterruptManager::resolve_policy_locked(
    const InterruptSignalUpdate& update,
    const InterruptRuntimeContext& context) const {
  const auto source_it = sources_.find(update.source_id);
  if (source_it == sources_.end()) {
    return Result<ResolvedPolicy>::Err(
        interrupt_error("unknown_source_id", "interrupt update source is unknown"));
  }
  const auto policy_ids_it = policy_ids_by_source_.find(update.source_id);
  if (policy_ids_it == policy_ids_by_source_.end()) {
    return Result<ResolvedPolicy>::Err(
        interrupt_error("no_policy", "interrupt update source has no policy"));
  }
  std::optional<ResolvedPolicy> best;
  for (const auto& policy_id : policy_ids_it->second) {
    const auto policy_it = policies_by_id_.find(policy_id);
    if (policy_it == policies_by_id_.end()) {
      continue;
    }
    const auto& policy = policy_it->second;
    if (!policy.affected_phases.contains(context.phase)) {
      continue;
    }
    // Phase 4.5B: move-scoped policies are inactive unless armed for
    // the active execution+move, and the binding's activation mode
    // must permit a latch given the current update.
    if (policy.scope_kind == InterruptPolicyScopeKind::kMove) {
      const auto scope_id_it = active_scope_id_by_policy_id_.find(policy_id);
      if (scope_id_it == active_scope_id_by_policy_id_.end()) {
        continue;
      }
      const auto& binding =
          active_scope_bindings_by_scope_id_.at(scope_id_it->second);
      if (!context.execution_id ||
          binding.execution_id != *context.execution_id) {
        continue;
      }
      if (!context.move_id || binding.move_id != *context.move_id) {
        continue;
      }
      if (!scope_binding_allows_latch_locked(binding, update)) {
        continue;
      }
    }
    ResolvedPolicy candidate;
    candidate.source = source_it->second;
    candidate.policy = policy;
    candidate.effective_priority = policy.priority.value_or(candidate.source.priority);
    candidate.effective_maskable =
        policy.maskable.value_or(candidate.source.maskable.value_or(false));
    if (!best || candidate.effective_priority > best->effective_priority ||
        (candidate.effective_priority == best->effective_priority &&
         candidate.policy.policy_id < best->policy.policy_id)) {
      best = candidate;
    }
  }
  if (!best) {
    return Result<ResolvedPolicy>::Err(
        interrupt_error("muted_phase",
                        "interrupt update has no policy for active phase"));
  }
  return Result<ResolvedPolicy>::Ok(*best);
}

bool InterruptManager::signal_requests_latch(
    const InterruptSourceDefinition& source,
    const InterruptSignalUpdate& update) const {
  return value_is_active(update.value, source.hysteresis_value);
}

bool InterruptManager::signal_requests_release(
    const InterruptSourceDefinition& source,
    const InterruptSignalUpdate& update) const {
  return value_is_release(update.value, source.hysteresis_value);
}

std::optional<std::int64_t> InterruptManager::freshness_ns(
    const InterruptSignalUpdate& update,
    std::int64_t kernel_now_ns) const {
  if (update.freshness_ns) {
    return *update.freshness_ns;
  }
  if (update.timestamp_ns <= 0) {
    return std::nullopt;
  }
  return std::max<std::int64_t>(0, kernel_now_ns - update.timestamp_ns);
}

Result<void> InterruptManager::validate_update_locked(
    const InterruptSignalUpdate& update,
    InterruptSourceClass attempted_lane,
    const ResolvedPolicy& resolved,
    std::int64_t kernel_now_ns) const {
  if (attempted_lane != resolved.source.source_class) {
    return Result<void>::Err(interrupt_error(
        "source_class_violation",
        "interrupt update used the wrong ingestion lane"));
  }
  const auto fresh = freshness_ns(update, kernel_now_ns);
  if (resolved.source.freshness_budget_ns && fresh &&
      *fresh > *resolved.source.freshness_budget_ns) {
    return Result<void>::Err(
        interrupt_error("stale", "interrupt update exceeded freshness budget"));
  }
  if (resolved.source.kind == InterruptSourceKind::kEdge &&
      resolved.source.debounce_ns && signal_requests_latch(resolved.source, update)) {
    const auto last_it = last_latch_time_by_source_.find(update.source_id);
    if (last_it != last_latch_time_by_source_.end() &&
        kernel_now_ns - last_it->second < *resolved.source.debounce_ns) {
      return Result<void>::Err(
          interrupt_error("debounced", "interrupt edge rejected by debounce"));
    }
  }
  return Result<void>::Ok();
}

InterruptIngestResult InterruptManager::ingest_resolved_locked(
    const InterruptSignalUpdate& update,
    const ResolvedPolicy& resolved,
    const InterruptRuntimeContext& context,
    std::int64_t kernel_now_ns,
    std::optional<InterruptTieBreak> tie_break) {
  InterruptIngestResult result;
  result.accepted = true;
  result.events.push_back(
      {"interrupt_signal_ingested",
       {{"source_id", update.source_id},
        {"sequence_id", std::to_string(update.sequence_id)},
        {"accepted", "true"},
        {"freshness_ns",
         freshness_ns(update, kernel_now_ns)
             ? std::to_string(*freshness_ns(update, kernel_now_ns))
             : ""}},
       std::nullopt});

  if (!active_latch_) {
    active_latch_ =
        make_latch_locked(update, resolved, context, kernel_now_ns, tie_break);
    last_latch_time_by_source_[update.source_id] = kernel_now_ns;
    append_latched_events_locked(result, *active_latch_);
    result.latched = true;
    result.active_latch = active_latch_;
    return result;
  }

  const auto active_policy = active_policy_locked();
  const int active_priority =
      active_policy ? active_policy->priority.value_or(
                          sources_.at(active_latch_->source_id).priority)
                    : 0;
  // Phase 4.5B (spec/260 §7): when both the active latch and the
  // incoming candidate are move-scoped desired-trip policies, the
  // arbitration loser emits `motion_trip_secondary_signal` and is
  // dropped (not queued). Move-scoped desired-trip outcomes don't
  // need operator dispositioning, so queueing the loser is wrong.
  const bool active_is_move_trip =
      active_policy &&
      active_policy->scope_kind == InterruptPolicyScopeKind::kMove &&
      active_policy->preemption_action ==
          InterruptPreemptionAction::kCompleteMoveOnSignal;
  const bool candidate_is_move_trip =
      resolved.policy.scope_kind == InterruptPolicyScopeKind::kMove &&
      resolved.policy.preemption_action ==
          InterruptPreemptionAction::kCompleteMoveOnSignal;
  if (active_is_move_trip && candidate_is_move_trip &&
      resolved.effective_priority <= active_priority) {
    const std::string arbitration_reason =
        resolved.effective_priority < active_priority
            ? "priority"
            : (active_latch_->source_id != update.source_id ? "source_lex"
                                                            : "sequence_lex");
    result.suppressed = true;
    result.reason = "motion_trip_secondary_signal";
    result.events.push_back(
        {"motion_trip_secondary_signal",
         {{"winning_latch_id", active_latch_->latch_id},
          {"losing_source_id", update.source_id},
          {"losing_policy_id", resolved.policy.policy_id},
          {"losing_sequence_id", std::to_string(update.sequence_id)},
          {"arbitration_reason", arbitration_reason}},
         *active_latch_});
    result.active_latch = active_latch_;
    return result;
  }
  if (resolved.effective_priority > active_priority) {
    InterruptLatch cleared = *active_latch_;
    cleared.cleared_at_ns = kernel_now_ns;
    cleared.clear_reason = "higher_priority_preempt";
    result.events.push_back(
        {"interrupt_cleared",
         {{"latch_id", cleared.latch_id},
          {"clear_reason", "higher_priority_preempt"},
          {"clear_latency_ns", std::to_string(kernel_now_ns - cleared.latched_at_ns)}},
         cleared});
    active_latch_ =
        make_latch_locked(update, resolved, context, kernel_now_ns, tie_break);
    last_latch_time_by_source_[update.source_id] = kernel_now_ns;
    append_latched_events_locked(result, *active_latch_);
    result.latched = true;
    result.active_latch = active_latch_;
    return result;
  }

  if (resolved.effective_maskable &&
      std::find(resolved.policy.maskable_by.begin(),
                resolved.policy.maskable_by.end(),
                active_latch_->policy_id) != resolved.policy.maskable_by.end()) {
    result.suppressed = true;
    result.reason = "suppressed_by_active_mask";
    result.events.push_back(
        {"interrupt_suppressed_by_active_mask",
         {{"source_id", update.source_id},
          {"policy_id", resolved.policy.policy_id},
          {"active_latch_id", active_latch_->latch_id}},
         *active_latch_});
    result.active_latch = active_latch_;
    return result;
  }

  queue_update_locked(update, resolved, context, kernel_now_ns, result.events, result);
  result.active_latch = active_latch_;
  return result;
}

InterruptLatch InterruptManager::make_latch_locked(
    const InterruptSignalUpdate& update,
    const ResolvedPolicy& resolved,
    const InterruptRuntimeContext& context,
    std::int64_t kernel_now_ns,
    std::optional<InterruptTieBreak> tie_break) {
  InterruptLatch latch;
  latch.latch_id = "interrupt-" + std::to_string(next_latch_id_++);
  latch.source_id = update.source_id;
  latch.policy_id = resolved.policy.policy_id;
  latch.latched_at_ns = kernel_now_ns;
  latch.execution_phase_at_latch = context.phase;
  latch.active_execution_id = context.execution_id;
  latch.active_move_id = context.move_id;
  latch.active_streaming_session_id = context.streaming_session_id;
  latch.signal_snapshot = update;
  latch.preemption_action_requested = resolved.policy.preemption_action;
  latch.borrowed_collision_world_revision =
      context.borrowed_collision_world_revision;
  latch.tie_break = tie_break;
  if (resolved.policy.preemption_action == InterruptPreemptionAction::kLatchHeld ||
      resolved.policy.preemption_action == InterruptPreemptionAction::kLatchFaulted) {
    latch.preemption_result = InterruptPreemptionResult::kNoneRequired;
    latch.preemption_latency_ns = 0;
  } else {
    latch.preemption_result = InterruptPreemptionResult::kPending;
  }
  // Phase 4.5B: when the matched policy is move-scoped, link the latch
  // to its armed binding via scope_id so the kernel can correlate the
  // outcome record (spec/260 §2.4).
  if (resolved.policy.scope_kind == InterruptPolicyScopeKind::kMove) {
    const auto it = active_scope_id_by_policy_id_.find(resolved.policy.policy_id);
    if (it != active_scope_id_by_policy_id_.end()) {
      latch.motion_interrupt_scope_id = it->second;
    }
  }
  return latch;
}

void InterruptManager::queue_update_locked(
    const InterruptSignalUpdate& update,
    const ResolvedPolicy& resolved,
    const InterruptRuntimeContext& context,
    std::int64_t kernel_now_ns,
    std::vector<InterruptManagerEvent>& events,
    InterruptIngestResult& result) {
  if (queue_.size() < config_.queue_depth) {
    queue_.push_back({update, resolved, context, kernel_now_ns, next_queue_order_++, std::nullopt});
    result.queued = true;
    events.push_back({"interrupt_queued",
                      {{"source_id", update.source_id},
                       {"sequence_id", std::to_string(update.sequence_id)},
                       {"queue_depth_after", std::to_string(queue_.size())}},
                      active_latch_});
    return;
  }

  auto replace_it = queue_.end();
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (it->resolved.effective_priority <= resolved.effective_priority &&
        (replace_it == queue_.end() || it->order < replace_it->order)) {
      replace_it = it;
    }
  }
  if (replace_it != queue_.end()) {
    const auto dropped = *replace_it;
    *replace_it = {update, resolved, context, kernel_now_ns, next_queue_order_++, std::nullopt};
    result.queued = true;
    events.push_back(
        {"interrupt_dropped_queue_overflow",
         {{"dropped_source_id", dropped.update.source_id},
          {"dropped_sequence_id", std::to_string(dropped.update.sequence_id)},
          {"replaced_by_source_id", update.source_id},
          {"overflow_action", "replaced_oldest"}},
         active_latch_});
    return;
  }
  result.rejected = true;
  result.reason = "queue_overflow";
  events.push_back(
      {"interrupt_dropped_queue_overflow",
       {{"dropped_source_id", update.source_id},
        {"dropped_sequence_id", std::to_string(update.sequence_id)},
        {"replaced_by_source_id", ""},
        {"overflow_action", "rejected_new"}},
       active_latch_});
}

std::optional<InterruptManager::QueuedUpdate>
InterruptManager::pop_next_queued_locked() {
  if (queue_.empty()) {
    return std::nullopt;
  }
  const auto it = std::min_element(queue_.begin(), queue_.end(),
                                   [](const QueuedUpdate& a,
                                      const QueuedUpdate& b) {
                                     return a.order < b.order;
                                   });
  QueuedUpdate value = *it;
  queue_.erase(it);
  return value;
}

void InterruptManager::append_latched_events_locked(
    InterruptIngestResult& result,
    const InterruptLatch& latch) {
  InterruptStructuredValue details{
      {"latch_id", latch.latch_id},
      {"source_id", latch.source_id},
      {"policy_id", latch.policy_id},
      {"execution_phase_at_latch", to_string(latch.execution_phase_at_latch)},
      {"preemption_action", preemption_action_string(latch.preemption_action_requested)},
      {"preemption_result", preemption_result_string(latch.preemption_result)},
      {"signal_value", signal_value_to_string(latch.signal_snapshot.value)}};
  if (latch.tie_break) {
    details.emplace("tie_break", "lexicographic");
  }
  result.events.push_back({"interrupt_latched", details, latch});
  if (latch.preemption_result != InterruptPreemptionResult::kPending) {
    result.events.push_back(
        {"interrupt_preempt_applied",
         {{"latch_id", latch.latch_id},
          {"preemption_action", preemption_action_string(latch.preemption_action_requested)},
          {"preemption_result", preemption_result_string(latch.preemption_result)},
          {"preemption_latency_ns", std::to_string(latch.preemption_latency_ns.value_or(0))}},
         latch});
  }
}

bool InterruptManager::disposition_allowed(
    const InterruptLatch& latch,
    const InterruptDisposition& disposition) const {
  const auto policy_it = policies_by_id_.find(latch.policy_id);
  if (policy_it == policies_by_id_.end()) {
    return false;
  }
  switch (policy_it->second.post_latch_disposition) {
    using enum InterruptPostLatchDisposition;
    case kAwaitOperatorAck:
      return disposition.action == InterruptDispositionAction::kAcknowledge ||
             disposition.action == InterruptDispositionAction::kReset ||
             disposition.action == InterruptDispositionAction::kHold;
    case kAwaitRecoveryHandler:
      return disposition.action == InterruptDispositionAction::kDelegateToRecovery ||
             disposition.action == InterruptDispositionAction::kReset ||
             disposition.action == InterruptDispositionAction::kFaultTerminal;
    case kAutoClearOnSignalRelease:
      return disposition.action == InterruptDispositionAction::kReset ||
             disposition.action == InterruptDispositionAction::kHold;
    case kAutoClearAfterMotionOutcome:
      // Phase 4.5B / spec/260 §5.6: the latch clears automatically
      // after the motion-outcome record is durable. Operator
      // disposition is not used; only Reset is accepted from the
      // disposition surface (it's the kernel-level escape hatch).
      return disposition.action == InterruptDispositionAction::kReset;
    case kFaultTerminal:
      return disposition.action == InterruptDispositionAction::kReset ||
             disposition.action == InterruptDispositionAction::kFaultTerminal;
  }
  return false;
}

std::optional<InterruptPolicyDefinition> InterruptManager::active_policy_locked() const {
  if (!active_latch_) {
    return std::nullopt;
  }
  const auto it = policies_by_id_.find(active_latch_->policy_id);
  if (it == policies_by_id_.end()) {
    return std::nullopt;
  }
  return it->second;
}

// ----- Phase 4.5B: motion-scoped binding lifecycle (spec/260 §2.6) -----

void InterruptManager::update_source_sample_locked(
    const InterruptSignalUpdate& update) {
  auto& slot = last_sample_by_source_[update.source_id];
  // Only advance the recorded sample if this update is fresher (higher
  // monotonic sequence_id). Out-of-order replays are ignored for
  // baseline-tracking purposes; this matches how the rest of the
  // manager treats sequence_id as the canonical ordering.
  if (update.sequence_id < slot.sequence_id) {
    return;
  }
  const auto src_it = sources_.find(update.source_id);
  std::optional<InterruptSignalValue> hysteresis;
  if (src_it != sources_.end()) {
    hysteresis = src_it->second.hysteresis_value;
  }
  slot.sequence_id = update.sequence_id;
  slot.timestamp_ns = update.timestamp_ns;
  slot.active = value_is_active(update.value, hysteresis);
  // Mirror the sample into any armed binding tracking the source so
  // diagnostics and activation-mode evaluation stay current.
  for (auto& [_, binding] : active_scope_bindings_by_scope_id_) {
    if (binding.source_id == update.source_id) {
      binding.last_signal_sequence_id = update.sequence_id;
    }
  }
}

bool InterruptManager::scope_binding_allows_latch_locked(
    const ActiveScopeBinding& binding,
    const InterruptSignalUpdate& update) const {
  const auto src_it = sources_.find(binding.source_id);
  std::optional<InterruptSignalValue> hysteresis;
  if (src_it != sources_.end()) {
    hysteresis = src_it->second.hysteresis_value;
  }
  const bool incoming_active = value_is_active(update.value, hysteresis);
  using enum MotionInterruptActivationMode;
  switch (binding.activation_mode) {
    case kRequireInactiveAtArm:
      // Arming already verified the baseline was inactive; latch on
      // any active value.
      return incoming_active;
    case kFreshEdgeAfterArm:
      // Only latch on an active value whose sequence_id is strictly
      // greater than the baseline taken at arm time.
      if (!incoming_active) {
        return false;
      }
      if (binding.baseline_sequence_id &&
          update.sequence_id <= *binding.baseline_sequence_id) {
        return false;
      }
      return true;
    case kTripIfAlreadyActive:
      // Latch on any active value, including the arm-time baseline.
      return incoming_active;
    case kMaintainLevelWithHysteresis:
      // v1 manager does not implement maintenance bands.
      return false;
  }
  return false;
}

Result<InterruptManager::ActiveScopeBinding>
InterruptManager::build_scope_binding_locked(
    const MotionInterruptScopeDTO& scope,
    std::uint64_t execution_id,
    const std::string& move_id,
    std::int64_t kernel_now_ns) const {
  if (scope.scope_id.empty()) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "empty_scope_id", "MotionInterruptScopeDTO.scope_id is required"));
  }
  if (scope.policy_id.empty()) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "empty_policy_id", "MotionInterruptScopeDTO.policy_id is required"));
  }
  if (!scope.required) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "scope_required_must_be_true",
        "v1 MotionInterruptScopeDTO.required must be true"));
  }
  if (scope.result_binding_key &&
      !result_binding_key_is_valid(*scope.result_binding_key)) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "motion_interrupt_result_binding_key_invalid",
        "result_binding_key must match spec/260 §2.6 format "
        "(1-128 bytes, no NUL, matches [A-Za-z_][A-Za-z0-9_.:-]*)"));
  }
  const auto policy_it = policies_by_id_.find(scope.policy_id);
  if (policy_it == policies_by_id_.end()) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "motion_interrupt_policy_not_found",
        "MotionInterruptScopeDTO.policy_id is not registered"));
  }
  const auto& policy = policy_it->second;
  if (policy.scope_kind != InterruptPolicyScopeKind::kMove) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "motion_interrupt_policy_not_move_scoped",
        "MotionInterruptScopeDTO bound to a non-move-scoped policy"));
  }
  // Activation-mode allowed by the policy?
  if (!policy.allowed_activation_modes.empty() &&
      !policy.allowed_activation_modes.contains(scope.activation_mode)) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "motion_interrupt_activation_mode_not_allowed",
        "policy does not permit the requested activation mode"));
  }
  if (policy.allowed_activation_modes.empty() &&
      scope.activation_mode !=
          MotionInterruptActivationMode::kRequireInactiveAtArm) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "motion_interrupt_activation_mode_not_allowed",
        "policy default only accepts require_inactive_at_arm"));
  }
  // Phase 4.5D-1 (spec/260 §5.6 + spec/210 §3.11): pre-dispatch
  // policy-level capability validation. The kernel layer adds the
  // backend-capability check on top. Three independent gates here:
  //   1. complete_move_on_signal MUST carry a MotionInterruptStopProfileDTO.
  //   2. The requested stop_mode MUST be in policy.allowed_stop_modes.
  //   3. Every element of allowed_fallback_modes MUST be in
  //      policy.allowed_stop_modes; precision-contact node families
  //      MUST keep allowed_fallback_modes empty unless an explicit
  //      qualification fixture has marked the node generic.
  if (policy.preemption_action ==
      InterruptPreemptionAction::kCompleteMoveOnSignal) {
    if (!scope.stop_profile) {
      return Result<ActiveScopeBinding>::Err(interrupt_error(
          "stop_profile_required",
          "complete_move_on_signal scope MUST carry a MotionInterruptStopProfileDTO"));
    }
    const auto& profile = *scope.stop_profile;
    // Phase 4.5D-1 (codex review follow-up): shape-validate the profile
    // before membership checks. A profile that passes presence + mode
    // membership but carries an empty profile_id, non-positive timing
    // fields, missing soft_on_path ramp duration, or
    // require_backend_confirmation=false would otherwise reach the
    // 4.5D-2 dispatch path and fail mid-execution. Fail closed here.
    if (auto shape_error = validate_stop_profile_shape(profile)) {
      return Result<ActiveScopeBinding>::Err(interrupt_error(
          *shape_error,
          "MotionInterruptStopProfileDTO failed shape validation"));
    }
    if (!policy.allowed_stop_modes.contains(profile.stop_mode)) {
      return Result<ActiveScopeBinding>::Err(interrupt_error(
          "stop_mode_not_allowed_by_policy",
          "stop_profile.stop_mode is not in policy.allowed_stop_modes"));
    }
    for (const auto& fallback : profile.allowed_fallback_modes) {
      if (!policy.allowed_stop_modes.contains(fallback)) {
        return Result<ActiveScopeBinding>::Err(interrupt_error(
            "fallback_mode_not_allowed_by_policy",
            "a stop_profile.allowed_fallback_modes entry is not in policy.allowed_stop_modes"));
      }
    }
    if (scope.node_category ==
            MotionInterruptNodeCategory::kPrecisionContact &&
        !profile.allowed_fallback_modes.empty()) {
      return Result<ActiveScopeBinding>::Err(interrupt_error(
          "fallback_not_allowed_for_node_category",
          "precision-contact node categories default to no fallback; scope must clear allowed_fallback_modes"));
    }
  } else if (scope.stop_profile) {
    // Defense in depth: stop_profile only makes sense for
    // complete_move_on_signal. Reject mismatched configurations
    // before they reach arm-time state mutation.
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "stop_profile_requires_complete_move_on_signal",
        "stop_profile is only valid on complete_move_on_signal scopes"));
  }
  // Source must be fast-execution-relevant.
  const auto src_it = sources_.find(policy.source_id);
  if (src_it == sources_.end()) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "motion_interrupt_source_not_found",
        "policy source is not registered"));
  }
  if (src_it->second.source_class !=
      InterruptSourceClass::kFastExecutionRelevant) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "motion_interrupt_source_not_fast",
        "scope_kind=move policies must bind a fast_execution_relevant source"));
  }
  // Baseline sampling. The kernel must have a fresh sample for the
  // require_inactive_at_arm and fresh_edge_after_arm modes; otherwise
  // arming fails closed per spec/260 §B.1.
  const auto sample_it = last_sample_by_source_.find(policy.source_id);
  const auto arm_timeout_ns =
      scope.arm_timeout_ns.value_or(config_.motion_scope_arm_timeout_ns);
  using enum MotionInterruptActivationMode;
  ActiveScopeBinding binding;
  binding.scope_id = scope.scope_id;
  binding.policy_id = scope.policy_id;
  binding.source_id = policy.source_id;
  binding.execution_id = execution_id;
  binding.move_id = move_id;
  binding.armed_at_ns = kernel_now_ns;
  binding.activation_mode = scope.activation_mode;
  binding.result_binding_key = scope.result_binding_key;
  if (sample_it != last_sample_by_source_.end()) {
    binding.baseline_sequence_id = sample_it->second.sequence_id;
    binding.baseline_active = sample_it->second.active;
    binding.last_signal_sequence_id = sample_it->second.sequence_id;
    const auto sample_age_ns =
        std::max<std::int64_t>(0, kernel_now_ns - sample_it->second.timestamp_ns);
    binding.arm_baseline_age_ns = sample_age_ns;
    if (arm_timeout_ns > 0 && sample_age_ns > arm_timeout_ns) {
      return Result<ActiveScopeBinding>::Err(interrupt_error(
          "motion_interrupt_arm_failed",
          "baseline sample exceeds arm_timeout_ns"));
    }
  } else {
    // No baseline available — only acceptable for trip_if_already_active.
    if (binding.activation_mode != kTripIfAlreadyActive) {
      return Result<ActiveScopeBinding>::Err(interrupt_error(
          "motion_interrupt_arm_failed",
          "no fresh baseline sample available for the source"));
    }
  }
  if (binding.activation_mode == kRequireInactiveAtArm &&
      binding.baseline_active) {
    return Result<ActiveScopeBinding>::Err(interrupt_error(
        "motion_interrupt_source_active_at_arm",
        "source was active at arm time but require_inactive_at_arm was requested"));
  }
  return Result<ActiveScopeBinding>::Ok(binding);
}

Result<InterruptIngestResult> InterruptManager::arm_motion_interrupt_scopes(
    std::uint64_t execution_id,
    const std::string& move_id,
    const std::vector<MotionInterruptScopeDTO>& scopes,
    std::int64_t kernel_now_ns) {
  InterruptIngestResult result;
  if (move_id.empty()) {
    return Result<InterruptIngestResult>::Err(interrupt_error(
        "empty_move_id",
        "arm_motion_interrupt_scopes requires a non-empty move_id"));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ActiveScopeBinding> staged;
  staged.reserve(scopes.size());
  // Reject duplicates inside the same request and against any
  // already-armed bindings (any execution_id).
  for (const auto& scope : scopes) {
    if (active_scope_bindings_by_scope_id_.contains(scope.scope_id)) {
      return Result<InterruptIngestResult>::Err(interrupt_error(
          "duplicate_scope_id",
          "scope_id already armed: " + scope.scope_id));
    }
    if (active_scope_id_by_policy_id_.contains(scope.policy_id)) {
      return Result<InterruptIngestResult>::Err(interrupt_error(
          "duplicate_policy_binding",
          "policy already bound by an active scope: " + scope.policy_id));
    }
    for (const auto& other : staged) {
      if (other.scope_id == scope.scope_id) {
        return Result<InterruptIngestResult>::Err(interrupt_error(
            "duplicate_scope_id_in_request",
            "duplicate scope_id in execute request: " + scope.scope_id));
      }
      if (other.policy_id == scope.policy_id) {
        return Result<InterruptIngestResult>::Err(interrupt_error(
            "duplicate_policy_binding_in_request",
            "duplicate policy_id in execute request: " + scope.policy_id));
      }
    }
    auto build = build_scope_binding_locked(scope, execution_id, move_id,
                                            kernel_now_ns);
    if (!build.ok()) {
      return Result<InterruptIngestResult>::Err(build.error());
    }
    staged.push_back(std::move(build).value());
  }
  // All scopes validated; commit atomically.
  for (auto& binding : staged) {
    active_scope_id_by_policy_id_[binding.policy_id] = binding.scope_id;
    InterruptStructuredValue details{
        {"scope_id", binding.scope_id},
        {"policy_id", binding.policy_id},
        {"source_id", binding.source_id},
        {"execution_id", std::to_string(binding.execution_id)},
        {"move_id", binding.move_id},
        {"activation_mode", activation_mode_string(binding.activation_mode)},
        {"armed_at_ns", std::to_string(binding.armed_at_ns)},
        {"baseline_active", bool_string(binding.baseline_active)},
    };
    // Spec/260 §7: `motion_interrupt_scope_armed` advertises
    // `arm_baseline_age_ns` so observers can confirm the baseline was
    // within the deployment's `arm_timeout_ns`. Sequence IDs are an
    // internal arbitration detail and intentionally NOT exported here.
    if (binding.arm_baseline_age_ns) {
      details["arm_baseline_age_ns"] =
          std::to_string(*binding.arm_baseline_age_ns);
    }
    result.events.push_back(
        {"motion_interrupt_scope_armed", std::move(details), std::nullopt});
    active_scope_bindings_by_scope_id_[binding.scope_id] = std::move(binding);
  }
  result.accepted = true;
  return Result<InterruptIngestResult>::Ok(result);
}

InterruptIngestResult InterruptManager::reap_motion_interrupt_scopes(
    std::uint64_t execution_id,
    const std::string& reap_reason,
    std::int64_t kernel_now_ns) {
  InterruptIngestResult result;
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> reaped_scope_ids;
  std::vector<std::string> reaped_policy_ids;
  for (const auto& [scope_id, binding] :
       active_scope_bindings_by_scope_id_) {
    if (binding.execution_id == execution_id) {
      reaped_scope_ids.push_back(scope_id);
      reaped_policy_ids.push_back(binding.policy_id);
    }
  }
  for (const auto& scope_id : reaped_scope_ids) {
    auto it = active_scope_bindings_by_scope_id_.find(scope_id);
    if (it == active_scope_bindings_by_scope_id_.end()) {
      continue;
    }
    InterruptStructuredValue details{
        {"scope_id", it->second.scope_id},
        {"policy_id", it->second.policy_id},
        {"source_id", it->second.source_id},
        {"execution_id", std::to_string(it->second.execution_id)},
        {"move_id", it->second.move_id},
        {"reap_reason", reap_reason},
        {"reaped_at_ns", std::to_string(kernel_now_ns)},
    };
    result.events.push_back(
        {"motion_interrupt_scope_reaped", std::move(details), std::nullopt});
    active_scope_id_by_policy_id_.erase(it->second.policy_id);
    active_scope_bindings_by_scope_id_.erase(it);
  }
  // Spec/260 §B.2: any queued update whose pre-resolved policy belonged
  // to a reaped binding MUST be dropped. Otherwise a queued
  // complete_move_on_signal candidate that lost the original arbitration
  // could be replayed by `clear_active_latch_locked` after the scope
  // was reaped — latching a policy with no live binding and stranding
  // a motion-outcome record. Emit a diagnostic event per dropped entry
  // so observers can correlate queue churn with reap actions.
  if (!reaped_policy_ids.empty()) {
    auto queue_it = queue_.begin();
    while (queue_it != queue_.end()) {
      const auto& policy_id = queue_it->resolved.policy.policy_id;
      const bool policy_reaped =
          std::find(reaped_policy_ids.begin(), reaped_policy_ids.end(),
                    policy_id) != reaped_policy_ids.end();
      if (policy_reaped) {
        InterruptStructuredValue drop_details{
            {"source_id", queue_it->update.source_id},
            {"sequence_id", std::to_string(queue_it->update.sequence_id)},
            {"policy_id", policy_id},
            {"execution_id", std::to_string(execution_id)},
            {"reap_reason", reap_reason},
        };
        result.events.push_back({"motion_interrupt_queued_update_dropped",
                                 std::move(drop_details), std::nullopt});
        queue_it = queue_.erase(queue_it);
      } else {
        ++queue_it;
      }
    }
  }
  result.accepted = !reaped_scope_ids.empty();
  return result;
}

std::vector<MotionInterruptScopeStatusDTO>
InterruptManager::active_motion_interrupt_scopes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MotionInterruptScopeStatusDTO> out;
  out.reserve(active_scope_bindings_by_scope_id_.size());
  for (const auto& [_, binding] : active_scope_bindings_by_scope_id_) {
    MotionInterruptScopeStatusDTO row;
    row.scope_id = binding.scope_id;
    row.policy_id = binding.policy_id;
    row.source_id = binding.source_id;
    row.execution_id = binding.execution_id;
    row.move_id = binding.move_id;
    row.armed_at_ns = binding.armed_at_ns;
    row.activation_mode = binding.activation_mode;
    row.baseline_sequence_id = binding.baseline_sequence_id;
    row.baseline_active = binding.baseline_active;
    row.last_signal_sequence_id = binding.last_signal_sequence_id;
    out.push_back(std::move(row));
  }
  return out;
}

}  // namespace manyforge::core
