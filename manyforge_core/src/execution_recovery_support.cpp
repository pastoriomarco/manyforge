#include <manyforge_core/execution_recovery_support.hpp>

#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/types/reason_codes.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace manyforge::core {

std::optional<ObservedStateEvidenceDTO> capture_observed_state_evidence(
    const ObservedRobotStatePublication* publication,
    std::int64_t now_ns,
    double at_rest_velocity_threshold_rad_s) {
  if (publication == nullptr) {
    return std::nullopt;
  }
  auto view = publication->acquire(now_ns);
  if (!view) {
    return std::nullopt;
  }
  ObservedStateEvidenceDTO sample;
  sample.sequence = view.sequence();
  sample.sampled_at_ns = view.received_timestamp_ns();
  sample.freshness_budget_ns = view.freshness_budget_ns();
  if (sample.freshness_budget_ns == 0) {
    sample.freshness_budget_ns =
        std::numeric_limits<std::int64_t>::max() -
        std::max<std::int64_t>(0, sample.sampled_at_ns);
  }
  sample.complete = view.complete();
  sample.joint_order_token = std::string(view.joint_order_token());
  sample.source_identity = std::string(view.source_id());
  sample.authority_profile = std::string(view.authority_profile_id());
  sample.measured_authority =
      view.authority_profile() ==
      ObservedRobotStateAuthorityProfile::kMeasured;
  sample.at_rest = view.velocities_complete();
  if (sample.at_rest) {
    for (const double velocity : view.velocities_rad_s()) {
      if (!std::isfinite(velocity) ||
          std::abs(velocity) > at_rest_velocity_threshold_rad_s) {
        sample.at_rest = false;
        break;
      }
    }
  }
  return sample;
}

void apply_observed_state_to_stop_certificate(
    const ObservedRobotStatePublication* publication,
    std::int64_t observed_at_ns,
    double at_rest_velocity_threshold_rad_s,
    KernelStopCertificateDTO& certificate) {
  certificate.observed_state_sequence = 0;
  certificate.observed_state_source_id.clear();
  certificate.observed_state_source_timestamp_ns = 0;
  certificate.observed_state_received_timestamp_ns = 0;
  certificate.observed_state_fresh = false;
  certificate.observed_state_at_rest = false;
  certificate.at_rest_evidence = AtRestEvidence::kUnavailable;
  if (publication == nullptr) {
    return;
  }
  auto view = publication->acquire(observed_at_ns);
  if (!view) {
    return;
  }
  certificate.observed_state_sequence = view.sequence();
  certificate.observed_state_source_id = std::string(view.source_id());
  certificate.observed_state_source_timestamp_ns = view.source_timestamp_ns();
  certificate.observed_state_received_timestamp_ns =
      view.received_timestamp_ns();
  const auto budget_ns = view.freshness_budget_ns();
  certificate.observed_state_fresh =
      view.complete() && observed_at_ns >= view.received_timestamp_ns() &&
      (budget_ns == 0 ||
       observed_at_ns - view.received_timestamp_ns() < budget_ns);
  certificate.observed_state_at_rest =
      certificate.observed_state_fresh && view.velocities_complete();
  if (certificate.observed_state_at_rest) {
    for (const double velocity : view.velocities_rad_s()) {
      if (!std::isfinite(velocity) ||
          std::abs(velocity) > at_rest_velocity_threshold_rad_s) {
        certificate.observed_state_at_rest = false;
        break;
      }
    }
  }
  if (certificate.observed_state_at_rest) {
    certificate.at_rest_evidence =
        view.authority_profile() == ObservedRobotStateAuthorityProfile::kMeasured
            ? AtRestEvidence::kMeasuredVelocity
            : AtRestEvidence::kQualifiedCommandedState;
  }
}

const char* stop_owner_kind_name(StopOwnerKind kind) {
  switch (kind) {
    case StopOwnerKind::kFaultPreemptWorker:
      return "fault_preempt_worker";
    case StopOwnerKind::kOperatorStopWorker:
      return "operator_stop_worker";
    case StopOwnerKind::kMotionTripStop:
      return "motion_trip_stop";
  }
  return "unknown";
}

const char* stop_certificate_outcome_name(StopCertificateOutcome outcome) {
  switch (outcome) {
    case StopCertificateOutcome::kSucceeded:
      return "succeeded";
    case StopCertificateOutcome::kPartial:
      return "partial";
    case StopCertificateOutcome::kBackendPreemptFailed:
      return "backend_preempt_failed";
  }
  return "unknown";
}

const char* at_rest_evidence_name(AtRestEvidence evidence) {
  switch (evidence) {
    case AtRestEvidence::kUnavailable:
      return "unavailable";
    case AtRestEvidence::kQualifiedCommandedState:
      return "qualified_commanded_state";
    case AtRestEvidence::kMeasuredVelocity:
      return "measured_velocity";
  }
  return "unknown";
}

const char* recovery_class_name(RecoveryClass recovery_class) {
  switch (recovery_class) {
    case RecoveryClass::kNone:
      return "none";
    case RecoveryClass::kRuntimeCollision:
      return "runtime_collision";
    case RecoveryClass::kStopFaultIntervention:
      return "stop_fault_intervention";
    case RecoveryClass::kFailedValidation:
      return "failed_validation";
    case RecoveryClass::kOrdinaryOperatorStop:
      return "ordinary_operator_stop";
    case RecoveryClass::kUnknownFault:
      return "unknown_fault";
  }
  return "unknown";
}

const char* recovery_action_name(RecoveryAction action) {
  switch (action) {
    case RecoveryAction::kRetryMotionFromCurrentState:
      return "retry_motion_from_current_state";
    case RecoveryAction::kAcknowledgeAndContinue:
      return "acknowledge_and_continue";
    case RecoveryAction::kRetryAfterSceneRepair:
      return "retry_after_scene_repair";
    case RecoveryAction::kReplanFromStoppedState:
      return "replan_from_stopped_state";
    case RecoveryAction::kResetCycle:
      return "reset_cycle";
  }
  return "unknown";
}

const char* recovery_assessment_status_name(RecoveryAssessmentStatus status) {
  switch (status) {
    case RecoveryAssessmentStatus::kUnavailable:
      return "unavailable";
    case RecoveryAssessmentStatus::kWaiting:
      return "waiting";
    case RecoveryAssessmentStatus::kRetryReady:
      return "retry_ready";
    case RecoveryAssessmentStatus::kActionRequired:
      return "action_required";
    case RecoveryAssessmentStatus::kResetRequired:
      return "reset_required";
  }
  return "unknown";
}

std::size_t RecoveryCertificateStore::owner_index(StopOwnerKind kind) {
  switch (kind) {
    case StopOwnerKind::kFaultPreemptWorker:
      return 0;
    case StopOwnerKind::kOperatorStopWorker:
      return 1;
    case StopOwnerKind::kMotionTripStop:
      return 2;
  }
  return 2;
}

std::uint64_t RecoveryCertificateStore::publish(
    KernelStopCertificateDTO certificate) {
  certificate.certificate_revision = ++next_revision_;
  const auto index = owner_index(certificate.stop_owner);
  auto& current = authoritative_[index];
  // Strictly monotonic authority: a completion from an older (or equal but
  // different) owner session is retained as evidence but never displaces the
  // newer authoritative record. A same-session republication takes the later
  // value so ordering remains deterministic.
  const bool same_owner_session =
      current.has_value() &&
      certificate.stop_owner_generation == current->stop_owner_generation &&
      certificate.execution_id == current->execution_id &&
      certificate.latch_id == current->latch_id;
  if (!current.has_value() ||
      certificate.stop_owner_generation > current->stop_owner_generation ||
      same_owner_session) {
    current = certificate;
  }
  evidence_ring_.push_back(std::move(certificate));
  while (evidence_ring_.size() > kEvidenceRingCapacity) {
    evidence_ring_.pop_front();
  }
  return next_revision_;
}

std::optional<std::uint64_t>
RecoveryCertificateStore::publish_owner_session_progress(
    KernelStopCertificateDTO certificate) {
  const auto existing = find_authoritative(
      certificate.stop_owner, certificate.execution_id,
      certificate.latch_id, certificate.stop_owner_generation);
  if (existing.has_value()) {
    const bool upgrading_to_success =
        existing->completion_result != StopCertificateOutcome::kSucceeded &&
        certificate.completion_result == StopCertificateOutcome::kSucceeded;
    if (!upgrading_to_success) {
      return std::nullopt;
    }
  }
  return publish(std::move(certificate));
}

std::optional<KernelStopCertificateDTO> RecoveryCertificateStore::authoritative(
    StopOwnerKind kind) const {
  return authoritative_[owner_index(kind)];
}

std::optional<KernelStopCertificateDTO>
RecoveryCertificateStore::find_authoritative(
    StopOwnerKind kind,
    std::uint64_t execution_id,
    const std::string& latch_id,
    std::uint64_t stop_owner_generation) const {
  const auto& current = authoritative_[owner_index(kind)];
  if (!current.has_value()) {
    return std::nullopt;
  }
  if (current->execution_id != execution_id ||
      current->latch_id != latch_id ||
      current->stop_owner_generation != stop_owner_generation) {
    return std::nullopt;
  }
  return current;
}

std::uint64_t StopOwnerIdentityTracker::begin(std::uint64_t execution_id) {
  selected_generation_ = ++generation_counter_;
  selected_execution_id_ = execution_id;
  return selected_generation_;
}

std::uint64_t StopOwnerIdentityTracker::ensure(std::uint64_t execution_id) {
  if (selected_generation_ != 0 && selected_execution_id_ == execution_id) {
    return selected_generation_;
  }
  return begin(execution_id);
}

std::uint64_t StopOwnerIdentityTracker::generation_for_execution(
    std::uint64_t execution_id) const {
  return selected_execution_id_ == execution_id ? selected_generation_ : 0;
}

bool ObservedReadinessTracker::sample_safe(
    const ObservedStateEvidenceDTO& sample,
    const std::string& expected_token,
    const RecoveryReadinessPolicy& policy,
    std::int64_t now_ns) const {
  if (sample.sequence == 0 || sample.source_identity.empty() ||
      !sample.complete || !sample.at_rest) {
    return false;
  }
  if (!expected_token.empty() && sample.joint_order_token != expected_token) {
    return false;
  }
  if (policy.require_measured_observation &&
      (!sample.measured_authority || sample.authority_profile.empty())) {
    return false;
  }
  if (sample.freshness_budget_ns <= 0) {
    return false;
  }
  if (sample.sampled_at_ns < 0 || now_ns < sample.sampled_at_ns) {
    return false;
  }
  // valid_until is exclusive: the deadline callback at exactly
  // sampled_at+budget must revoke readiness, not leave it ready forever.
  return (now_ns - sample.sampled_at_ns) < sample.freshness_budget_ns;
}

bool ObservedReadinessTracker::update(
    const ObservedStateEvidenceDTO& sample,
    const std::string& expected_joint_order_token,
    const RecoveryReadinessPolicy& policy,
    std::int64_t now_ns) {
  const bool sequence_regressed =
      last_sample_.has_value() && sample.sequence < last_counted_sequence_;
  const bool same_sequence_changed =
      last_sample_.has_value() && sample.sequence == last_counted_sequence_ &&
      (sample.sampled_at_ns != last_sample_->sampled_at_ns ||
       sample.complete != last_sample_->complete ||
       sample.at_rest != last_sample_->at_rest ||
       sample.joint_order_token != last_sample_->joint_order_token ||
       sample.source_identity != last_sample_->source_identity ||
       sample.authority_profile != last_sample_->authority_profile ||
       sample.measured_authority != last_sample_->measured_authority);
  if (sequence_regressed || same_sequence_changed ||
      !sample_safe(sample, expected_joint_order_token, policy, now_ns)) {
    // Safe-to-unsafe is immediate and resets the restoration qualifier.
    last_sample_ = sample;
    continuous_safe_since_ns_ = -1;
    consecutive_safe_samples_ = 0;
    last_counted_sequence_ = sample.sequence;
    return false;
  }
  const bool source_changed =
      last_sample_.has_value() &&
      (sample.source_identity != last_sample_->source_identity ||
       sample.joint_order_token != last_sample_->joint_order_token ||
       sample.authority_profile != last_sample_->authority_profile ||
       sample.measured_authority != last_sample_->measured_authority);
  const bool new_sequence =
      !last_sample_.has_value() || sample.sequence > last_counted_sequence_;
  last_sample_ = sample;
  if (continuous_safe_since_ns_ < 0 || source_changed) {
    continuous_safe_since_ns_ = now_ns;
    consecutive_safe_samples_ = 1;
  } else if (new_sequence) {
    // Long-running high-rate deployments can exceed uint32_t samples. Keep
    // qualification monotonic instead of wrapping to zero and spuriously
    // revoking an otherwise continuous safe observation chain.
    if (consecutive_safe_samples_ <
        std::numeric_limits<std::uint32_t>::max()) {
      ++consecutive_safe_samples_;
    }
  }
  if (new_sequence) {
    last_counted_sequence_ = sample.sequence;
  }
  return evaluate(policy, now_ns);
}

bool ObservedReadinessTracker::evaluate(const RecoveryReadinessPolicy& policy,
                                        std::int64_t now_ns) const {
  if (!last_sample_.has_value() || continuous_safe_since_ns_ < 0) {
    return false;
  }
  if (policy.require_measured_observation &&
      (!last_sample_->measured_authority ||
       last_sample_->authority_profile.empty())) {
    return false;
  }
  // Freshness re-check at evaluation time: an expired sample chain is not
  // qualified regardless of dwell history.
  if (now_ns < last_sample_->sampled_at_ns ||
      (now_ns - last_sample_->sampled_at_ns) >=
          last_sample_->freshness_budget_ns) {
    return false;
  }
  if (consecutive_safe_samples_ < policy.ready_min_samples) {
    return false;
  }
  return (now_ns - continuous_safe_since_ns_) >= policy.ready_dwell_ns;
}

void ObservedReadinessTracker::reset() {
  last_sample_.reset();
  continuous_safe_since_ns_ = -1;
  consecutive_safe_samples_ = 0;
  last_counted_sequence_ = 0;
}

namespace {

void finish_assessment(KernelRecoveryAssessmentDTO& assessment,
                       std::uint64_t previous_revision) {
  assessment.assessment_revision = previous_revision + 1;
}

bool certificate_matches_latch(const KernelStopCertificateDTO& certificate,
                               const RecoveryAuthorityCapture& capture) {
  if (!capture.active_latch.has_value()) {
    return false;
  }
  const auto& latch = *capture.active_latch;
  const auto latch_execution =
      latch.active_execution_id.value_or(capture.execution_id);
  return certificate.latch_id == latch.latch_id &&
         certificate.execution_id == latch_execution &&
         certificate.stop_owner_generation ==
             capture.fault_preempt_owner_generation;
}

}  // namespace

KernelRecoveryAssessmentDTO build_recovery_assessment(
    const RecoveryAuthorityCapture& capture,
    const RecoveryReadinessPolicy& policy,
    std::uint64_t previous_revision) {
  KernelRecoveryAssessmentDTO assessment;
  assessment.assessed_at_ns = capture.captured_at_ns;
  assessment.control_command_epoch = capture.control_command_epoch;
  assessment.execution_id = capture.execution_id;
  assessment.execution_state = capture.execution_state;
  assessment.observed_state_available = capture.observed_state.has_value();
  if (capture.observed_state.has_value()) {
    const auto& observed = *capture.observed_state;
    assessment.observed_state_sequence = observed.sequence;
    assessment.observed_state_source_id = observed.source_identity;
    assessment.observed_state_authority_profile = observed.authority_profile;
    assessment.observed_state_measured = observed.measured_authority;
    if (observed.freshness_budget_ns > 0 && observed.sampled_at_ns >= 0 &&
        observed.sampled_at_ns <=
            std::numeric_limits<std::int64_t>::max() -
                observed.freshness_budget_ns) {
      assessment.observed_state_valid_until_ns =
          observed.sampled_at_ns + observed.freshness_budget_ns;
    }
    assessment.observed_state_fresh = capture.observed_state_qualified;
    assessment.observed_state_at_rest =
        capture.observed_state_qualified && observed.at_rest;
  }
  assessment.stronger_authority_active = capture.stronger_authority_active;
  assessment.queued_latch_present = capture.queued_latch_present;

  const bool any_stop_owner_active = capture.fault_preempt_stop_active ||
                                     capture.fault_preempt_worker_active ||
                                     capture.operator_stop_active ||
                                     capture.motion_trip_stop_active;
  assessment.stop_owner_active = any_stop_owner_active;

  const auto require_qualified_observation = [&]() -> const char* {
    if (!policy.require_measured_observation) {
      return nullptr;
    }
    if (!capture.observed_state.has_value()) {
      return reason_codes::kRecoveryObservedStateUnavailable;
    }
    if (!capture.observed_state->measured_authority ||
        capture.observed_state->authority_profile.empty()) {
      return reason_codes::kRecoveryObservedStateAuthorityNotMeasured;
    }
    if (!capture.observed_state_qualified) {
      return capture.observed_state->at_rest
                 ? reason_codes::kRecoveryObservedStateStale
                 : reason_codes::kRecoveryObservedStateNotAtRest;
    }
    return nullptr;
  };

  const auto add_reset_action = [&]() {
    assessment.allowed_actions.push_back(RecoveryAction::kResetCycle);
  };

  switch (capture.execution_state) {
    case ExecutionState::kInterrupted: {
      if (!capture.active_latch.has_value()) {
        assessment.recovery_class = RecoveryClass::kUnknownFault;
        assessment.status = RecoveryAssessmentStatus::kResetRequired;
        assessment.reason_code = reason_codes::kRecoveryLatchMissing;
        add_reset_action();
        break;
      }
      const auto& latch = *capture.active_latch;
      assessment.latch_id = latch.latch_id;
      assessment.source_id = latch.source_id;
      assessment.policy_id = latch.policy_id;
      assessment.preemption_result = latch.preemption_result;
      assessment.stop_owner_generation =
          capture.fault_preempt_owner_generation;
      if (latch.source_id != kRuntimeCollisionFaultSourceId) {
        assessment.recovery_class = RecoveryClass::kUnknownFault;
        assessment.status = RecoveryAssessmentStatus::kResetRequired;
        assessment.reason_code = reason_codes::kRecoveryLatchNotRetryable;
        add_reset_action();
        break;
      }
      assessment.recovery_class = RecoveryClass::kRuntimeCollision;
      add_reset_action();
      if (capture.stronger_authority_active) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = reason_codes::kRecoveryStrongerAuthorityActive;
        break;
      }
      if (latch.preemption_result == InterruptPreemptionResult::kPending) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = reason_codes::kRecoveryStopNotCertified;
        break;
      }
      if (latch.preemption_result != InterruptPreemptionResult::kSucceeded) {
        // Failed/partial preemption escalates through the fault-preempt
        // worker; the latch is never retry-eligible, whether or not the
        // stop owner is still active (retryable=false dominates).
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = reason_codes::kRecoveryStopFailedEscalating;
        break;
      }
      if (capture.queued_latch_present) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = reason_codes::kRecoveryQueuedLatchPresent;
        break;
      }
      if (capture.fault_preempt_stop_active ||
          capture.fault_preempt_worker_active) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = reason_codes::kRecoveryStopNotCertified;
        break;
      }
      const auto& certificate = capture.fault_preempt_certificate;
      if (!certificate.has_value() ||
          certificate->completion_result !=
              StopCertificateOutcome::kSucceeded ||
          !certificate_matches_latch(*certificate, capture)) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = reason_codes::kRecoveryStopNotCertified;
        break;
      }
      assessment.stop_certificate_id = certificate->certificate_id;
      assessment.backend_stop_confirmed = certificate->backend_stop_confirmed;
      if (const char* observation_reason = require_qualified_observation()) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = observation_reason;
        break;
      }
      assessment.status = RecoveryAssessmentStatus::kRetryReady;
      assessment.retry_admissible = true;
      assessment.allowed_actions.insert(
          assessment.allowed_actions.begin(),
          RecoveryAction::kRetryMotionFromCurrentState);
      assessment.reason_code = reason_codes::kRecoveryStopCertified;
      assessment.evidence_refs.push_back(certificate->certificate_id);
      break;
    }
    case ExecutionState::kInterventionRequired: {
      if (capture.active_latch.has_value()) {
        assessment.latch_id = capture.active_latch->latch_id;
        assessment.source_id = capture.active_latch->source_id;
        assessment.preemption_result =
            capture.active_latch->preemption_result;
      }
      if (!capture.stop_fault_intervention_classified) {
        assessment.recovery_class = RecoveryClass::kUnknownFault;
        assessment.status = RecoveryAssessmentStatus::kResetRequired;
        assessment.reason_code = reason_codes::kRecoveryLatchNotRetryable;
        add_reset_action();
        break;
      }
      assessment.recovery_class = RecoveryClass::kStopFaultIntervention;
      add_reset_action();
      if (any_stop_owner_active) {
        // A still-active stop owner defers even the explicit choice.
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = reason_codes::kRecoveryStopOwnerActive;
        break;
      }
      if (capture.stronger_authority_active || capture.queued_latch_present) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = capture.stronger_authority_active
                                     ? reason_codes::kRecoveryStrongerAuthorityActive
                                     : reason_codes::kRecoveryQueuedLatchPresent;
        break;
      }
      if (const char* observation_reason = require_qualified_observation()) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = observation_reason;
        break;
      }
      assessment.status = RecoveryAssessmentStatus::kActionRequired;
      assessment.reason_code =
          reason_codes::kRecoveryInterventionChoiceRequired;
      assessment.allowed_actions.insert(
          assessment.allowed_actions.begin(),
          RecoveryAction::kAcknowledgeAndContinue);
      break;
    }
    case ExecutionState::kFailed: {
      if (!capture.failed_validation_retryable) {
        assessment.recovery_class = RecoveryClass::kUnknownFault;
        assessment.status = RecoveryAssessmentStatus::kResetRequired;
        assessment.reason_code = reason_codes::kRecoveryFailedNotRetryable;
        add_reset_action();
        break;
      }
      assessment.recovery_class = RecoveryClass::kFailedValidation;
      add_reset_action();
      if (any_stop_owner_active || capture.stronger_authority_active ||
          capture.queued_latch_present) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code =
            any_stop_owner_active
                ? reason_codes::kRecoveryStopOwnerActive
                : (capture.stronger_authority_active
                       ? reason_codes::kRecoveryStrongerAuthorityActive
                       : reason_codes::kRecoveryQueuedLatchPresent);
        break;
      }
      assessment.status = RecoveryAssessmentStatus::kRetryReady;
      assessment.retry_admissible = true;
      assessment.allowed_actions.insert(
          assessment.allowed_actions.begin(),
          RecoveryAction::kRetryAfterSceneRepair);
      assessment.reason_code =
          reason_codes::kRecoveryFailedValidationRetryable;
      break;
    }
    case ExecutionState::kStopped: {
      assessment.recovery_class = RecoveryClass::kOrdinaryOperatorStop;
      assessment.stop_owner_generation =
          capture.operator_stop_owner_generation;
      add_reset_action();
      const auto& certificate = capture.operator_stop_certificate;
      if (any_stop_owner_active || capture.stronger_authority_active ||
          capture.queued_latch_present) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code =
            any_stop_owner_active
                ? reason_codes::kRecoveryStopOwnerActive
                : (capture.stronger_authority_active
                       ? reason_codes::kRecoveryStrongerAuthorityActive
                       : reason_codes::kRecoveryQueuedLatchPresent);
        break;
      }
      if (capture.active_latch.has_value()) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = reason_codes::kRecoveryLatchNotRetryable;
        break;
      }
      if (!certificate.has_value() ||
          certificate->completion_result !=
              StopCertificateOutcome::kSucceeded ||
          !certificate->backend_stop_confirmed ||
          certificate->execution_id != capture.execution_id ||
          certificate->stop_owner_generation !=
              capture.operator_stop_owner_generation) {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = reason_codes::kRecoveryStopNotCertified;
        break;
      }
      assessment.stop_certificate_id = certificate->certificate_id;
      assessment.backend_stop_confirmed = certificate->backend_stop_confirmed;
      assessment.status = RecoveryAssessmentStatus::kRetryReady;
      assessment.retry_admissible = true;
      assessment.allowed_actions.insert(
          assessment.allowed_actions.begin(),
          RecoveryAction::kReplanFromStoppedState);
      assessment.reason_code = reason_codes::kRecoveryStopCertified;
      assessment.evidence_refs.push_back(certificate->certificate_id);
      break;
    }
    default: {
      assessment.recovery_class = RecoveryClass::kNone;
      assessment.status = RecoveryAssessmentStatus::kUnavailable;
      assessment.reason_code = reason_codes::kRecoveryNoneActive;
      break;
    }
  }
  finish_assessment(assessment, previous_revision);
  return assessment;
}

bool recovery_assessment_semantically_differs(
    const KernelRecoveryAssessmentDTO& previous,
    const KernelRecoveryAssessmentDTO& next) {
  return previous.control_command_epoch != next.control_command_epoch ||
         previous.execution_id != next.execution_id ||
         previous.execution_state != next.execution_state ||
         previous.recovery_class != next.recovery_class ||
         previous.status != next.status ||
         previous.latch_id != next.latch_id ||
         previous.source_id != next.source_id ||
         previous.policy_id != next.policy_id ||
         previous.preemption_result != next.preemption_result ||
         previous.stop_owner_active != next.stop_owner_active ||
         previous.stop_owner_generation != next.stop_owner_generation ||
         previous.backend_stop_confirmed != next.backend_stop_confirmed ||
         previous.stop_certificate_id != next.stop_certificate_id ||
         previous.observed_state_available !=
             next.observed_state_available ||
         previous.observed_state_fresh != next.observed_state_fresh ||
         previous.observed_state_at_rest != next.observed_state_at_rest ||
         previous.observed_state_source_id !=
             next.observed_state_source_id ||
         previous.observed_state_authority_profile !=
             next.observed_state_authority_profile ||
         previous.observed_state_measured != next.observed_state_measured ||
         previous.stronger_authority_active !=
             next.stronger_authority_active ||
         previous.queued_latch_present != next.queued_latch_present ||
         previous.retry_admissible != next.retry_admissible ||
         previous.allowed_actions != next.allowed_actions ||
         previous.reason_code != next.reason_code;
}

RecoveryAssessmentPublication prepare_recovery_assessment_publication(
    const KernelRecoveryAssessmentDTO& previous,
    const RecoveryAuthorityCapture& capture,
    const RecoveryReadinessPolicy& policy,
    std::string_view trigger) {
  RecoveryAssessmentPublication publication;
  publication.candidate = build_recovery_assessment(
      capture, policy, previous.assessment_revision);
  publication.changed = recovery_assessment_semantically_differs(
      previous, publication.candidate);
  if (!publication.changed) {
    return publication;
  }
  const auto& next = publication.candidate;
  publication.event_details = {
      {"assessment_revision", std::to_string(next.assessment_revision)},
      {"recovery_class", recovery_class_name(next.recovery_class)},
      {"status", recovery_assessment_status_name(next.status)},
      {"reason_code", next.reason_code},
      {"retry_admissible", next.retry_admissible ? "true" : "false"},
      {"stop_certificate_id", next.stop_certificate_id},
      {"latch_id", next.latch_id},
      {"source_id", next.source_id},
      {"policy_id", next.policy_id},
      {"queued_latch_present",
       next.queued_latch_present ? "true" : "false"},
      {"stronger_authority_active",
       next.stronger_authority_active ? "true" : "false"},
      {"trigger", std::string(trigger)},
  };
  return publication;
}

RecoveryAdmissionDecision evaluate_recovery_admission(
    const KernelRecoveryActionRequestDTO& request,
    const RecoveryAuthorityCapture& capture,
    const KernelRecoveryAssessmentDTO& assessment) {
  RecoveryAdmissionDecision decision;
  if (request.request_id.empty()) {
    decision.rejection_reason = reason_codes::kRecoveryRequestIdMissing;
    return decision;
  }
  if (request.principal_id.empty()) {
    decision.rejection_reason = reason_codes::kRecoveryPrincipalMissing;
    return decision;
  }
  if (request.expected_control_command_epoch !=
      capture.control_command_epoch) {
    decision.rejection_reason = reason_codes::kRecoveryStaleEpoch;
    return decision;
  }
  if (request.expected_assessment_revision !=
      assessment.assessment_revision) {
    decision.rejection_reason = reason_codes::kRecoveryStaleAssessment;
    return decision;
  }
  if (request.action == RecoveryAction::kResetCycle) {
    decision.rejection_reason = reason_codes::kRecoveryActionNotAllowed;
    return decision;
  }
  const auto allowed = std::find(assessment.allowed_actions.begin(),
                                 assessment.allowed_actions.end(),
                                 request.action);
  if (assessment.status != RecoveryAssessmentStatus::kRetryReady &&
      assessment.status != RecoveryAssessmentStatus::kActionRequired) {
    if (assessment.reason_code.empty()) {
      decision.rejection_reason = reason_codes::kRecoveryStopNotCertified;
    } else {
      decision.rejection_reason = assessment.reason_code;
    }
    return decision;
  }
  if (allowed == assessment.allowed_actions.end()) {
    decision.rejection_reason = reason_codes::kRecoveryActionNotAllowed;
    return decision;
  }
  if (assessment.stop_owner_active) {
    decision.rejection_reason = reason_codes::kRecoveryStopOwnerActive;
    return decision;
  }
  if (assessment.stronger_authority_active) {
    decision.rejection_reason = reason_codes::kRecoveryStrongerAuthorityActive;
    return decision;
  }
  if (assessment.queued_latch_present) {
    decision.rejection_reason = reason_codes::kRecoveryQueuedLatchPresent;
    return decision;
  }
  if (request.expected_execution_id != assessment.execution_id) {
    decision.rejection_reason = reason_codes::kRecoveryExecutionChanged;
    return decision;
  }
  if (request.expected_latch_id != assessment.latch_id) {
    decision.rejection_reason = reason_codes::kRecoveryLatchChanged;
    return decision;
  }
  if (request.expected_stop_certificate_id !=
      assessment.stop_certificate_id) {
    decision.rejection_reason = reason_codes::kRecoveryStopCertificateChanged;
    return decision;
  }
  if (request.expected_stop_owner_generation !=
      assessment.stop_owner_generation) {
    decision.rejection_reason =
        reason_codes::kRecoveryStopOwnerGenerationChanged;
    return decision;
  }
  decision.admitted = true;
  return decision;
}

ErrorDTO recovery_action_rejection_error(
    const KernelRecoveryActionRequestDTO& request,
    const KernelRecoveryAssessmentDTO& assessment,
    std::string_view rejection_reason) {
  ErrorDTO error;
  error.code = kErrorRecoveryActionRejected;
  error.message = "Recovery action rejected";
  error.details = {
      {"reason", rejection_reason.empty() ? "recovery_action_rejected"
                                           : std::string(rejection_reason)},
      {"request_id", request.request_id},
      {"action", recovery_action_name(request.action)},
      {"principal_id", request.principal_id},
      {"expected_assessment_revision",
       std::to_string(request.expected_assessment_revision)},
      {"current_assessment_revision",
       std::to_string(assessment.assessment_revision)},
      {"assessment_status",
       recovery_assessment_status_name(assessment.status)},
      {"assessment_reason", assessment.reason_code},
      {"recovery_class", recovery_class_name(assessment.recovery_class)},
      {"execution_state", to_string(assessment.execution_state)},
  };
  if (!assessment.latch_id.empty()) {
    error.details["latch_id"] = assessment.latch_id;
  }
  if (!assessment.stop_certificate_id.empty()) {
    error.details["stop_certificate_id"] = assessment.stop_certificate_id;
  }
  error.details["execution_id"] = std::to_string(assessment.execution_id);
  if (assessment.recovery_class ==
      RecoveryClass::kRuntimeCollision) {
    error.details["preemption_result"] =
        interrupt_preemption_result_name(assessment.preemption_result);
    error.details["required_preemption_result"] = "succeeded";
    error.details["stop_owner_active"] =
        assessment.stop_owner_active ? "true" : "false";
  }
  // `retryable` mirrors spec/210 §3.12: true when the same action can become
  // admissible without a reset (waiting states); false for identity/stale
  // mismatches, which need a freshly bound request instead.
  const std::string_view reason = rejection_reason;
  const bool retryable =
      reason == reason_codes::kRecoveryStopNotCertified ||
      reason == reason_codes::kRecoveryStopOwnerActive ||
      reason == reason_codes::kRecoveryObservedStateUnavailable ||
      reason == reason_codes::kRecoveryObservedStateStale ||
      reason == reason_codes::kRecoveryObservedStateNotAtRest ||
      reason == reason_codes::kRecoveryObservedStateAuthorityNotMeasured;
  error.details["retryable"] = retryable ? "true" : "false";
  return error;
}

}  // namespace manyforge::core
