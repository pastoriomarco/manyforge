#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {
namespace {
ErrorDTO global_recovery_error(const char* reason) {
  return {"invalid_state_transition", "Global rule recovery is not currently eligible",
          {{"reason", reason}}};
}
}

Result<void> ResourceAddressedExecutionKernel::refresh_global_latched_condition_locked(
    std::size_t index) {
  auto& row = global_io_rules_.at(index);
  if (row.state.state != IoGlobalRuleState::kLatched || !global_io_rule_service_ ||
      !row.latch_registration || !row.latched_evidence)
    return Result<void>::Err(global_recovery_error("io_global_rule_latch_stale"));
  if (row.registration) {
    const auto actual = global_io_rule_service_->ReadRuleEvidence(*row.registration);
    if (!actual.ok()) return Result<void>::Err(actual.error());
    row.evidence = actual.value();
    if (!actual.value().source_losses.empty()) {
      row.source_loss_latch = true;
      row.last_source_loss = actual.value();
      if (row.latch_registration == row.registration) row.latched_evidence = actual.value();
    }
  }
  // A submitted recovery owns its exact successor until the durable outcome
  // is resolved. Keep every subsequent trip/loss on that token for atomic
  // post-ACK processing; replacing it here would make an uncertain write
  // impossible to replay even though the old latch stayed closed.
  if (row.registration && std::any_of(stop_fault_recovery_replay_ledger_.begin(),
          stop_fault_recovery_replay_ledger_.end(), [&](const auto& replay) {
            return replay.pending_global_record &&
                   replay.pending_global_record->successor_registration == *row.registration;
          }))
    return Result<void>::Ok();
  // First loss is immutable evidence on an old registration. A later genuine
  // source recovery needs an actual new observation registration, while the
  // original latch identity/evidence/scope remain in this same row.
  if (row.registration && row.evidence && row.evidence->source_losses.empty() &&
      !row.evidence->evaluation_error)
    return Result<void>::Ok();
  if (row.registration) {
    const auto retired = global_io_rule_service_->RetireRule(*row.registration);
    if (!retired.ok()) return Result<void>::Err(retired.error());
    row.evidence = retired.value();
    row.registration.reset();
  }
  const auto armed = arm_global_io_rule_locked(global_io_rule_catalog_->rules.at(index),
      global_io_rule_catalog_->context, global_io_rule_service_, false);
  if (!armed.ok()) {
    row.qualification_error = armed.error();
    return Result<void>::Err(armed.error());
  }
  row.registration = armed.value();
  if (!global_rule_registration_matches(*row.registration, global_io_rule_catalog_->context,
                                         row.state.rule_id))
    return Result<void>::Err(global_recovery_error("io_global_rule_service_identity_mismatch"));
  const auto evidence = global_io_rule_service_->ReadRuleEvidence(*row.registration);
  if (!evidence.ok()) return Result<void>::Err(evidence.error());
  row.evidence = evidence.value();
  row.qualification_error.reset();
  return Result<void>::Ok();
}

Result<std::vector<IoGlobalRuleRecoveryOfferDTO>>
ResourceAddressedExecutionKernel::ReadGlobalIoRuleRecoveryOffers() const {
  std::lock_guard lock(state_mutex_);
  std::vector<IoGlobalRuleRecoveryOfferDTO> offers;
  if (!global_io_rule_catalog_ || !global_io_rule_service_)
    return Result<std::vector<IoGlobalRuleRecoveryOfferDTO>>::Ok(std::move(offers));
  offers.reserve(global_io_rules_.size());
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    const auto& row = global_io_rules_[index];
    if (row.state.state != IoGlobalRuleState::kLatched || !row.latch_registration) continue;
    std::optional<bool> current;
    if (row.registration) {
      const auto observed = global_io_rule_service_->ReadRuleConditionSnapshot(*row.registration);
      if (observed.ok()) current = observed.value().value;
    }
    offers.push_back({{global_io_rule_catalog_->context, row.state.rule_id,
                        row.latch_registration->registration_id}, current,
                       row.source_loss_latch
                           ? InterruptPostLatchDisposition::kAwaitOperatorAck
                           : global_io_rule_catalog_->rules[index].post_latch_disposition,
                       row.source_loss_latch ? IoGlobalRuleLatchCauseDTO::kSourceLoss
                                             : IoGlobalRuleLatchCauseDTO::kCondition});
  }
  return Result<std::vector<IoGlobalRuleRecoveryOfferDTO>>::Ok(std::move(offers));
}

Result<void> ResourceAddressedExecutionKernel::prepare_global_io_rule_recovery_locked(
    const IoGlobalRuleRecoveryIntentDTO& intent, bool automatic) {
  if (!global_io_rule_catalog_ || !global_io_rule_service_ ||
      global_io_rule_catalog_->context != intent.selector.context)
    return Result<void>::Err(global_recovery_error("io_global_rule_latch_stale"));
  const auto policy = std::lower_bound(global_io_rule_catalog_->rules.begin(),
      global_io_rule_catalog_->rules.end(), intent.selector.rule_id,
      [](const auto& rule, const auto& id) { return rule.policy_id < id; });
  if (policy == global_io_rule_catalog_->rules.end() || policy->policy_id != intent.selector.rule_id)
    return Result<void>::Err(global_recovery_error("io_global_rule_latch_stale"));
  const auto index = static_cast<std::size_t>(policy - global_io_rule_catalog_->rules.begin());
  auto& row = global_io_rules_.at(index);
  if (row.state.state != IoGlobalRuleState::kLatched || !row.latch_registration ||
      !row.latched_evidence || row.latch_registration->registration_id !=
          intent.selector.expected_rule_registration_id)
    return Result<void>::Err(global_recovery_error("io_global_rule_latch_stale"));
  // The latch stays closed throughout observation replacement and SQL. A new
  // inactive registration is necessary to retain an actual successor edge;
  // clearing the old first-trip fact would manufacture that edge instead.
  if (row.registration) {
    const auto retired = global_io_rule_service_->RetireRule(*row.registration);
    if (!retired.ok()) return Result<void>::Err(retired.error());
    row.evidence = retired.value();
    if (!retired.value().source_losses.empty()) {
      row.source_loss_latch = true;
      row.last_source_loss = retired.value();
      if (row.latch_registration == row.registration) row.latched_evidence = retired.value();
    }
    row.registration.reset();
  }
  if (automatic && (row.source_loss_latch || policy->post_latch_disposition !=
          InterruptPostLatchDisposition::kAutoClearOnSignalRelease))
    return Result<void>::Err(global_recovery_error("io_global_rule_operator_recovery_required"));
  const auto armed = arm_global_io_rule_locked(*policy, global_io_rule_catalog_->context,
                                               global_io_rule_service_, true);
  if (!armed.ok()) {
    row.qualification_error = armed.error();
    return Result<void>::Err(armed.error());
  }
  row.registration = armed.value();
  if (!global_rule_registration_matches(*row.registration, global_io_rule_catalog_->context,
                                         row.state.rule_id))
    return Result<void>::Err(global_recovery_error("io_global_rule_service_identity_mismatch"));
  const auto inactive = global_io_rule_service_->ReadRuleConditionSnapshot(*row.registration);
  if (!inactive.ok()) return Result<void>::Err(inactive.error());
  const auto evidence = global_io_rule_service_->ReadRuleEvidence(*row.registration);
  if (!evidence.ok()) return Result<void>::Err(evidence.error());
  row.evidence = evidence.value();
  if (inactive.value().value != std::optional(false) || inactive.value().evaluated_at_ns < 0 ||
      evidence.value().trip || !evidence.value().source_losses.empty() ||
      evidence.value().evaluation_error)
    return Result<void>::Err(global_recovery_error("io_global_rule_condition_not_inactive"));
  const IoGlobalRuleRecoveryResultDTO result{intent.request_context.request_id,
      RequestReplayStatusDTO::kAcceptedNew, true, intent.selector.context,
      {row.state.rule_id, IoGlobalRuleState::kClear, std::nullopt}, std::nullopt};
  const IoGlobalRuleRecoveryRecordDTO record{intent, result, row.state, *row.latched_evidence,
                                             inactive.value(), *row.registration,
                                             row.source_loss_latch ? row.last_source_loss
                                                                   : std::nullopt};
  stop_fault_recovery_replay_ledger_.clear();
  stop_fault_recovery_replay_ledger_.push_back(StopFaultRecoveryReplayEntry{intent, result, record});
  row.qualification_error.reset();
  return Result<void>::Ok();
}

Result<IoGlobalRuleRecoveryResultDTO>
ResourceAddressedExecutionKernel::commit_global_io_rule_recovery_locked(
    const IoGlobalRuleRecoveryRecordDTO& record, StopWorkHandoff& handoff) {
  if (!global_io_rule_catalog_ || !global_io_rule_service_ ||
      global_io_rule_catalog_->context != record.intent.selector.context)
    return Result<IoGlobalRuleRecoveryResultDTO>::Err(
        global_recovery_error("io_global_rule_recovery_postcommit_stale"));
  const auto found = std::find_if(global_io_rules_.begin(), global_io_rules_.end(),
      [&](const auto& row) { return row.state.rule_id == record.intent.selector.rule_id; });
  if (found == global_io_rules_.end() || found->state != record.prior_latched_state ||
      !found->latch_registration ||
      !global_rule_registration_matches(*found->latch_registration,
          record.intent.selector.context, record.intent.selector.rule_id) ||
      found->latch_registration->registration_id !=
          record.intent.selector.expected_rule_registration_id ||
      found->latched_evidence != record.prior_latched_evidence ||
      found->registration != record.successor_registration)
    return Result<IoGlobalRuleRecoveryResultDTO>::Err(
        global_recovery_error("io_global_rule_recovery_postcommit_stale"));
  const auto original_latch_registration = *found->latch_registration;
  // SQL may span an actual freshness deadline even without another source
  // frame. Evaluate the same service at the commit cut before consuming its
  // retained successor; a delayed expiry callback cannot open admission.
  const auto evaluated = global_io_rule_service_->EvaluateRules();
  if (!evaluated.ok()) return Result<IoGlobalRuleRecoveryResultDTO>::Err(evaluated.error());
  const auto latest = global_io_rule_service_->ReadRuleEvidence(*found->registration);
  if (!latest.ok()) return Result<IoGlobalRuleRecoveryResultDTO>::Err(latest.error());
  found->evidence = latest.value();
  const auto index = static_cast<std::size_t>(found - global_io_rules_.begin());
  // The old latch remains published until the successor response is known.
  found->response_consumed = false;
  // This bounded same-state call consumes a successor match/loss BEFORE any
  // command admission can observe the cleared old latch. Provider/Stop work
  // remains in the existing handoff until after state/admission unlock.
  const auto processed = process_global_io_rule_evidence_locked(handoff, nullptr, index);
  if (!processed.ok()) {
    // A response error cannot open admission over retained successor truth.
    // Keep the exact original restriction/evidence, with no fabricated cause.
    found->state = record.prior_latched_state;
    found->latch_registration = original_latch_registration;
    found->latched_evidence = record.prior_latched_evidence;
    found->qualification_error = processed.error();
    return Result<IoGlobalRuleRecoveryResultDTO>::Err(processed.error());
  }
  if (found->state.state != IoGlobalRuleState::kLatched) {
    found->latch_registration.reset();
    found->latched_evidence.reset();
    found->source_loss_latch = false;
  }
  return Result<IoGlobalRuleRecoveryResultDTO>::Ok(record.result);
}

Result<IoGlobalRuleRecoveryResultDTO> ResourceAddressedExecutionKernel::recover_global_io_rule(
    const IoGlobalRuleRecoveryIntentDTO& intent) {
  return recover_global_io_rule_impl(intent, false);
}

Result<IoGlobalRuleRecoveryResultDTO> ResourceAddressedExecutionKernel::recover_global_io_rule_impl(
    const IoGlobalRuleRecoveryIntentDTO& intent, bool automatic) {
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch ||
      intent.selector.context.owner_epoch != config_.request_owner_epoch ||
      intent.request_context.request_id.empty() || intent.selector.rule_id.empty() ||
      intent.request_context.request_id.value().find('\0') != std::string::npos ||
      intent.selector.rule_id.value().find('\0') != std::string::npos ||
      intent.request_context.source_label.empty() ||
      intent.request_context.source_label.size() > kMaximumStopRequestLabelBytes ||
      (intent.request_context.correlation_id &&
          (intent.request_context.correlation_id->empty() ||
           intent.request_context.correlation_id->size() > kMaximumStopRequestLabelBytes)) ||
      (intent.reason && (intent.reason->empty() || intent.reason->size() > kMaximumStopReasonBytes)) ||
      intent.selector.expected_rule_registration_id == 0U ||
      io_rule_state_context_field_error(intent.selector.context))
    return Result<IoGlobalRuleRecoveryResultDTO>::Err(
        global_recovery_error("io_global_rule_recovery_intent_invalid"));
  std::lock_guard recovery_lock(stop_fault_recovery_mutex_);
  std::optional<IoGlobalRuleRecoveryRecordDTO> prepared;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(stop_fault_recovery_replay_ledger_.begin(),
        stop_fault_recovery_replay_ledger_.end(), [&](const auto& row) {
          return row.request_id() == intent.request_context.request_id;
        });
    if (replay != stop_fault_recovery_replay_ledger_.end()) {
      if (!std::holds_alternative<IoGlobalRuleRecoveryIntentDTO>(replay->intent) ||
          std::get<IoGlobalRuleRecoveryIntentDTO>(replay->intent) != intent)
        return Result<IoGlobalRuleRecoveryResultDTO>::Err(
            global_recovery_error("request_id_conflict"));
      if (!replay->pending_global_record) {
        auto result = std::get<IoGlobalRuleRecoveryResultDTO>(replay->result);
        result.replay_status = RequestReplayStatusDTO::kReplayedExact;
        return Result<IoGlobalRuleRecoveryResultDTO>::Ok(std::move(result));
      }
      prepared = replay->pending_global_record;
    }
  }

  // An exact unresolved retry first resolves its original durable payload.
  // Current-product qualification is required for a new transition, not for
  // reading the outcome of an already submitted authorization.
  if (!prepared) {
    std::lock_guard submit_lock(submission_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    if (!config_.operation_admission_gate ||
        config_.operation_admission_gate->has_held_configuration_fences())
      return Result<IoGlobalRuleRecoveryResultDTO>::Err(
          global_recovery_error("io_global_rule_recovery_configuration_fenced"));
    auto owner = cell_admission_owner_->AcquireManagedProductAdmission();
    if (!owner.ok()) return Result<IoGlobalRuleRecoveryResultDTO>::Err(owner.error());
    if (owner.value().retained_bundle_id() != intent.selector.context.retained_bundle_id)
      return Result<IoGlobalRuleRecoveryResultDTO>::Err(
          global_recovery_error("io_global_rule_latch_stale"));
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(stop_fault_recovery_replay_ledger_.begin(),
        stop_fault_recovery_replay_ledger_.end(), [&](const auto& row) {
          return row.request_id() == intent.request_context.request_id;
        });
    if (replay != stop_fault_recovery_replay_ledger_.end()) {
      if (!std::holds_alternative<IoGlobalRuleRecoveryIntentDTO>(replay->intent) ||
          std::get<IoGlobalRuleRecoveryIntentDTO>(replay->intent) != intent)
        return Result<IoGlobalRuleRecoveryResultDTO>::Err(
            global_recovery_error("request_id_conflict"));
      if (!replay->pending_global_record) {
        auto result = std::get<IoGlobalRuleRecoveryResultDTO>(replay->result);
        result.replay_status = RequestReplayStatusDTO::kReplayedExact;
        return Result<IoGlobalRuleRecoveryResultDTO>::Ok(std::move(result));
      }
      prepared = replay->pending_global_record;
    } else {
      if (std::any_of(stop_fault_recovery_replay_ledger_.begin(),
          stop_fault_recovery_replay_ledger_.end(),
          [](const auto& row) { return row.pending_global_record.has_value(); }))
        return Result<IoGlobalRuleRecoveryResultDTO>::Err(
            global_recovery_error("io_global_rule_recovery_pending"));
      if (ordinary_request_id_retained_locked(intent.request_context.request_id) ||
          emergency_stop_reserve_.find(intent.request_context.request_id.value()))
        return Result<IoGlobalRuleRecoveryResultDTO>::Err(
            global_recovery_error("request_id_conflict"));
      const auto evaluated = global_io_rule_service_
          ? global_io_rule_service_->EvaluateRules()
          : Result<void>::Err(global_recovery_error("io_global_rule_latch_stale"));
      if (!evaluated.ok()) return Result<IoGlobalRuleRecoveryResultDTO>::Err(evaluated.error());
      const auto made = prepare_global_io_rule_recovery_locked(intent, automatic);
      if (!made.ok()) return Result<IoGlobalRuleRecoveryResultDTO>::Err(made.error());
      prepared = stop_fault_recovery_replay_ledger_.back().pending_global_record;
    }
  }
  // No Kernel/admission/health/signal lock spans this ordinary durable writer.
  auto persisted = Result<std::uint64_t>::Err(
      global_recovery_error("io_global_rule_recovery_journal_exception"));
  try { persisted = journal_.RecordGlobalIoRuleCleared(*prepared); } catch (...) {}
  if (!persisted.ok()) return Result<IoGlobalRuleRecoveryResultDTO>::Err(persisted.error());
  {
    std::lock_guard state_lock(state_mutex_);
    const auto& old = prepared->intent.selector.context;
    if (global_io_rule_applied_context_ &&
        global_io_rule_applied_context_->owner_epoch == old.owner_epoch &&
        global_io_rule_applied_context_->active_publication_generation >
            old.active_publication_generation) {
      // The exact journal ACK establishes the historical authorization. A
      // genuine successful newer Applied installation already retired the old
      // registration. This completes only its retained replay entry: never a
      // Clear, event, source mutation or response in the new catalog.
      const auto entry = std::find_if(stop_fault_recovery_replay_ledger_.begin(),
          stop_fault_recovery_replay_ledger_.end(), [&](const auto& row) {
            return row.request_id() == intent.request_context.request_id;
          });
      if (entry == stop_fault_recovery_replay_ledger_.end() ||
          entry->pending_global_record != prepared)
        return Result<IoGlobalRuleRecoveryResultDTO>::Err(
            global_recovery_error("io_global_rule_recovery_postcommit_stale"));
      entry->result = prepared->result;
      entry->pending_global_record.reset();
      return Result<IoGlobalRuleRecoveryResultDTO>::Ok(prepared->result);
    }
  }
  StopWorkHandoff handoff;
  auto committed = Result<IoGlobalRuleRecoveryResultDTO>::Err(
      global_recovery_error("io_global_rule_recovery_postcommit_stale"));
  {
    std::lock_guard submit_lock(submission_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    if (!config_.operation_admission_gate ||
        config_.operation_admission_gate->has_held_configuration_fences())
      return Result<IoGlobalRuleRecoveryResultDTO>::Err(
          global_recovery_error("io_global_rule_recovery_configuration_fenced"));
    auto owner = cell_admission_owner_->AcquireManagedProductAdmission();
    if (!owner.ok()) return Result<IoGlobalRuleRecoveryResultDTO>::Err(owner.error());
    if (owner.value().retained_bundle_id() != intent.selector.context.retained_bundle_id)
      return Result<IoGlobalRuleRecoveryResultDTO>::Err(
          global_recovery_error("io_global_rule_latch_stale"));
    std::lock_guard state_lock(state_mutex_);
    committed = commit_global_io_rule_recovery_locked(*prepared, handoff);
    if (committed.ok()) {
      const auto entry = std::find_if(stop_fault_recovery_replay_ledger_.begin(),
          stop_fault_recovery_replay_ledger_.end(), [&](const auto& row) {
            return row.request_id() == intent.request_context.request_id;
          });
      if (entry != stop_fault_recovery_replay_ledger_.end()) {
        entry->result = committed.value();
        entry->pending_global_record.reset();
      }
    }
  }
  publish_stop_work(handoff);
  return committed;
}

}  // namespace manyforge::core
