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
  }
  return "unknown";
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
  policies_by_id_.emplace(policy.policy_id, policy);
  policy_ids_by_source_[policy.source_id].push_back(policy.policy_id);
  std::sort(policy_ids_by_source_[policy.source_id].begin(),
            policy_ids_by_source_[policy.source_id].end());
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
      const auto fresh = freshness_ns(update, kernel_now_ns);
      result.events.push_back(
          {event_type,
           {{"source_id", update.source_id},
            {"sequence_id", std::to_string(update.sequence_id)},
            {"freshness_ns", fresh ? std::to_string(*fresh) : ""},
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

  for (std::size_t idx = 1; idx < candidates.size(); ++idx) {
    InterruptIngestResult queued;
    queue_update_locked(candidates[idx].update,
                        candidates[idx].resolved,
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

  auto next = pop_next_queued_locked();
  if (next) {
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

}  // namespace manyforge::core
