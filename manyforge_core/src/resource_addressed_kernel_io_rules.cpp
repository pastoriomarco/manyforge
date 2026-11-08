#include <manyforge_core/io_rule_stop_response.hpp>
#include <tuple>

#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {
namespace {

ErrorDTO rule_error(const char* code, const char* reason) {
  return {
      code, "Motion interrupt rule could not complete its required response", {{"reason", reason}}};
}

std::optional<std::uint64_t> successful_stop_stage(const StopCertificateDTO& certificate,
                                                   const OperationId& operation,
                                                   const OperationParticipantId& participant,
                                                   const PreparedParticipantStopPlanDTO& plan) {
  std::optional<std::uint64_t> selected;
  for (const auto& attempt : certificate.attempted_stage_results) {
    const auto* subject = std::get_if<StopParticipantScopeKeyDTO>(&attempt.scope_key.subject);
    const auto& request = attempt.result.request;
    if (!subject || subject->operation_id != operation || subject->participant_id != participant ||
        attempt.authority != StopStageResultAuthorityDTO::kAuthoritative ||
        attempt.result.outcome != ParticipantStopStageOutcomeDTO::kSucceeded ||
        attempt.result.error || request.stop_id != certificate.stop_id ||
        request.stop_generation != certificate.stop_generation ||
        request.participant_stop_plan != plan || request.stage_index >= plan.stage_commands.size())
      continue;
    if (!selected || request.stage_index > *selected) selected = request.stage_index;
  }
  return selected;
}

// Read only actual authoritative nominal-stage evidence from the existing Stop
// worker. A permitted natural finish is different from a braking signal trip.
std::optional<bool> natural_completion_evidence(const StopCertificateDTO& certificate,
                                                const OperationId& operation,
                                                const OperationParticipantId& participant,
                                                std::uint64_t stage_index) {
  std::optional<bool> natural;
  for (const auto& attempt : certificate.attempted_stage_results) {
    const auto* subject = std::get_if<StopParticipantScopeKeyDTO>(&attempt.scope_key.subject);
    if (!subject || subject->operation_id != operation || subject->participant_id != participant ||
        attempt.authority != StopStageResultAuthorityDTO::kAuthoritative ||
        attempt.result.request.stage_index != stage_index ||
        attempt.result.outcome != ParticipantStopStageOutcomeDTO::kSucceeded)
      continue;
    for (const auto& item : attempt.result.evidence_items) {
      if (!item.evidence || item.state != StopEvidenceItemStateDTO::kSatisfied) continue;
      const auto* object = std::get_if<JsonObjectDTO>(&item.evidence->value);
      if (!object) continue;
      const auto found = object->find("completed_by_natural_completion");
      if (found == object->end()) continue;
      const auto* value = std::get_if<bool>(&found->second.value);
      if (!value || (natural && *natural != *value)) return std::nullopt;
      natural = *value;
    }
  }
  return natural;
}

}  // namespace

bool ResourceAddressedExecutionKernel::io_rule_scope_current_locked(
    const ActiveOperation& operation, const ActiveOperationParticipant& participant,
    const IoRuleRegistrationDTO& registration) const {
  const auto* scope = std::get_if<IoOperationRuleScopeDTO>(&registration.scope);
  if (!scope || registration.owner_epoch != config_.request_owner_epoch ||
      participant.ownership_transferred || !participant.prepared_participant ||
      scope->operation_id != operation.state.operation_id ||
      scope->operation_revision != participant.prepared_participant->operation_revision ||
      scope->participant_id != participant.slot_owner.participant_id ||
      scope->preparation_id != participant.preparation_id ||
      scope->prepared_contract_digest !=
          participant.prepared_participant->prepared_contract_digest ||
      scope->controllable_component_id != participant.binding.value().controllable_component_id ||
      scope->command_target_id != participant.binding.value().command_target_id ||
      !stop_retained_bundle_id_ || !stop_graph_revision_ ||
      participant.prepared_participant->retained_bundle_id != *stop_retained_bundle_id_ ||
      participant.prepared_participant->graph_revision != *stop_graph_revision_)
    return false;
  const auto row = std::find_if(
      operation.state.participants.begin(), operation.state.participants.end(),
      [&](const auto& value) { return value.participant_id == scope->participant_id; });
  return row != operation.state.participants.end() &&
         row->command_id == scope->controller_goal_id &&
         row->command_generation == scope->controller_goal_generation;
}

Result<void> ResourceAddressedExecutionKernel::arm_io_rules_locked(ActiveOperation& operation) {
  for (auto& participant : operation.participants) {
    if (!participant.prepared_participant) continue;
    const auto* motion =
        std::get_if<PreparedMotionExecutionDTO>(&participant.prepared_participant->payload);
    if (!motion || motion->interrupt_scopes.empty()) continue;
    if (!participant.motion_request || participant.motion_request->motion_interrupt_scopes.size() !=
                                           motion->interrupt_scopes.size())
      return Result<void>::Err(
          rule_error(kErrorMotionInterruptArmFailed, "io_rule_prepared_scope_mismatch"));
    const auto admitted = validate_motion_interrupt_scope_admission(
        participant.motion_request->motion_interrupt_scopes, participant.io_motion_rule_policies,
        &participant.binding.value());
    if (!admitted.ok()) return admitted;
    for (const auto& prepared_scope : motion->interrupt_scopes) {
      const auto& requested = participant.motion_request->motion_interrupt_scopes;
      const auto original = std::find_if(
          requested.begin(), requested.end(),
          [&](const auto& value) { return value.scope_id == prepared_scope.scope_id.value(); });
      if (original == requested.end() ||
          original->policy_id != prepared_scope.interrupt_policy_id.value() ||
          original->activation_mode != prepared_scope.activation_mode ||
          original->arm_timeout_ns != prepared_scope.arm_timeout_ns ||
          original->required != prepared_scope.required ||
          original->stop_profile != prepared_scope.stop_profile ||
          original->node_category != prepared_scope.node_category ||
          original->metadata != prepared_scope.metadata)
        return Result<void>::Err(
            rule_error(kErrorMotionInterruptArmFailed, "io_rule_prepared_scope_mismatch"));
    }
    if (!io_signal_service_)
      return Result<void>::Err(
          rule_error(kErrorMotionInterruptArmFailed, "io_rule_service_unavailable"));
    if (!participant.io_rule_registrations.empty()) {
      if (participant.io_rule_registrations.size() != motion->interrupt_scopes.size() ||
          !std::all_of(participant.io_rule_registrations.begin(),
                       participant.io_rule_registrations.end(), [&](const auto& token) {
                         return io_rule_scope_current_locked(operation, participant, token);
                       }))
        return Result<void>::Err(
            rule_error(kErrorMotionInterruptArmFailed, "io_rule_membership_stale"));
      continue;
    }
    const auto row =
        std::find_if(operation.state.participants.begin(), operation.state.participants.end(),
                     [&](const auto& value) {
                       return value.participant_id == participant.slot_owner.participant_id;
                     });
    if (row == operation.state.participants.end())
      return Result<void>::Err(
          rule_error(kErrorMotionInterruptArmFailed, "io_rule_participant_missing"));
    // Current snapshot retains this prepared membership only; older executions
    // remain in the existing ordered operation journal, never a growing array.
    row->io_rule_outcomes.clear();
    for (const auto& scope : motion->interrupt_scopes) {
      const auto policy = std::find_if(
          participant.io_motion_rule_policies.begin(), participant.io_motion_rule_policies.end(),
          [&](const auto& value) { return value.policy_id == scope.interrupt_policy_id; });
      if (policy == participant.io_motion_rule_policies.end() || !policy->semantics ||
          scope.preemption_action != policy->semantics->preemption_action)
        return Result<void>::Err(
            rule_error(kErrorMotionInterruptArmFailed, "io_rule_policy_not_installed"));
      const auto expected_entry =
          ResolvePreparedIoRuleStopEntry(scope.stop_plan, policy->stop_entry_stage_id);
      const auto expected_success =
          expected_entry.ok()
              ? ResolvePreparedIoRuleSuccessThrough(scope.stop_plan, expected_entry.value(),
                                                    policy->success_through_stage_id)
              : Result<IoRulePreparedStopEntryDTO>::Err(expected_entry.error());
      if (!expected_entry.ok() || !expected_success.ok() || !scope.stop_entry ||
          !scope.success_through || *scope.stop_entry != expected_entry.value() ||
          *scope.success_through != expected_success.value())
        return Result<void>::Err(
            rule_error(kErrorMotionInterruptArmFailed, "io_rule_prepared_stop_response_mismatch"));
      IoOperationRuleScopeDTO exact{operation.state.operation_id,
                                    participant.prepared_participant->operation_revision,
                                    row->participant_id,
                                    row->controllable_component_id,
                                    row->command_target_id,
                                    row->command_id,
                                    row->command_generation,
                                    participant.preparation_id,
                                    participant.prepared_participant->prepared_contract_digest,
                                    scope.scope_id};
      if (std::any_of(policy->condition->inputs().begin(), policy->condition->inputs().end(),
              [](const auto& input) {
                return std::holds_alternative<IoConditionBlackboardKeyDTO>(input.source);
              }) && !program_blackboard_source_matches_operation_locked(operation))
        return Result<void>::Err(
            rule_error(kErrorMotionInterruptArmFailed, "io_rule_baseline_unavailable"));
      auto required_sources = policy->required_sources;
      if (std::any_of(policy->condition->inputs().begin(), policy->condition->inputs().end(),
              [](const auto& input) {
                return std::holds_alternative<IoConditionBlackboardKeyDTO>(input.source);
              })) {
        // The exact current Run baseline was checked above. Only its actual
        // installed source can qualify the compiled blackboard inputs.
        required_sources.push_back(program_blackboard_source_->source_id);
        std::sort(required_sources.begin(), required_sources.end());
        required_sources.erase(std::unique(required_sources.begin(), required_sources.end()),
                               required_sources.end());
      }
      const auto armed =
          io_signal_service_->ArmRule({scope.interrupt_policy_id, exact, policy->condition,
                                       scope.activation_mode, std::move(required_sources)});
      if (!armed.ok()) return Result<void>::Err(armed.error());
      // Retain every minted token before validation so a failed acknowledgement
      // is still retired by the shared partial-arm cleanup path.
      participant.io_rule_registrations.push_back(armed.value());
      if (armed.value().scope != IoRuleScopeDTO(exact) ||
          armed.value().policy_id != scope.interrupt_policy_id ||
          armed.value().owner_epoch != config_.request_owner_epoch)
        return Result<void>::Err(
            rule_error(kErrorMotionInterruptArmFailed, "io_rule_arm_identity_mismatch"));
      row->io_rule_outcomes.push_back(
          {armed.value(), {IoRuleRegistrationState::kArmed, std::nullopt, {}, std::nullopt}});
    }
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::retire_participant_io_rules_locked(
    ActiveOperationParticipant& participant) {
  if (participant.io_rule_registrations.empty()) return Result<void>::Ok();
  if (!io_signal_service_)
    return Result<void>::Err(
        rule_error(kErrorInternalError, "io_rule_service_missing_on_retirement"));
  auto* operation = find_active_operation_locked(participant.slot_owner.operation_id);
  if (!operation)
    operation = find_pending_motion_operation_locked(participant.slot_owner.operation_id);
  for (const auto& registration : participant.io_rule_registrations) {
    const auto retired = io_signal_service_->RetireRule(registration);
    if (!retired.ok()) return Result<void>::Err(retired.error());
    if (operation) {
      const auto row =
          std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                       [&](const auto& value) {
                         return value.participant_id == participant.slot_owner.participant_id;
                       });
      if (row != operation->state.participants.end()) {
        const auto outcome =
            std::find_if(row->io_rule_outcomes.begin(), row->io_rule_outcomes.end(),
                         [&](const auto& value) { return value.registration == registration; });
        if (outcome != row->io_rule_outcomes.end()) {
          // An already-retired retry cannot erase retained first evidence.
          if (retired.value().trip || !retired.value().source_losses.empty() ||
              retired.value().evaluation_error ||
              outcome->evidence.state != IoRuleRegistrationState::kRetired)
            outcome->evidence = retired.value();
          outcome->evidence.state = IoRuleRegistrationState::kRetired;
        }
      }
    }
  }
  participant.io_rule_registrations.clear();
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::retire_io_rules_locked(ActiveOperation& operation) {
  for (auto& participant : operation.participants) {
    const auto retired = retire_participant_io_rules_locked(participant);
    if (!retired.ok()) return retired;
  }
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::retire_all_io_rules_locked() {
  for (auto& [id, operation] : active_operations_) {
    (void)id;
    if (!retire_io_rules_locked(*operation).ok()) std::terminate();
    if (!operation->pending_motion_takeover.empty() &&
        !retire_io_rules_locked(*operation->pending_motion_takeover.mapped()).ok())
      std::terminate();
    if (!operation->next_motion_takeover.empty() &&
        !retire_io_rules_locked(*operation->next_motion_takeover.mapped()).ok())
      std::terminate();
  }
  if (!retire_global_io_rules_locked().ok() || !retire_staged_global_io_rules_locked().ok())
    std::terminate();
}

bool ResourceAddressedExecutionKernel::has_active_io_rules_locked() const {
  for (const auto& [id, operation] : active_operations_) {
    (void)id;
    const auto armed = [](const auto& value) {
      return std::any_of(value.participants.begin(), value.participants.end(),
                         [](const auto& row) { return !row.io_rule_registrations.empty(); });
    };
    if (armed(*operation) ||
        (!operation->pending_motion_takeover.empty() &&
         armed(*operation->pending_motion_takeover.mapped())) ||
        (!operation->next_motion_takeover.empty() &&
         armed(*operation->next_motion_takeover.mapped())))
      return true;
  }
  return false;
}

Result<std::optional<TimePoint>> ResourceAddressedExecutionKernel::NextIoRuleExpiry() const {
  std::lock_guard lock(state_mutex_);
  if (!has_active_io_rules_locked() &&
      (!global_io_rule_catalog_ || global_io_rules_.empty()))
    return Result<std::optional<TimePoint>>::Ok(std::nullopt);
  if (!io_signal_service_)
    return Result<std::optional<TimePoint>>::Err(
        rule_error(kErrorInternalError, "io_rule_service_missing"));
  return io_signal_service_->NextRuleExpiry();
}

Result<void> ResourceAddressedExecutionKernel::ProcessIoRuleEvidence() {
  std::size_t work_bound{};
  {
    std::lock_guard lock(state_mutex_);
    // Interrupt evaluation is paused while the cell is latched.
    if (cell_latched_locked()) return Result<void>::Ok();
    if (!has_active_io_rules_locked() &&
        (!global_io_rule_catalog_ || global_io_rules_.empty())) return Result<void>::Ok();
    if (!io_signal_service_)
      return Result<void>::Err(rule_error(kErrorInternalError, "io_rule_service_missing"));
    const auto evaluated = io_signal_service_->EvaluateRules();
    if (!evaluated.ok()) return evaluated;
    for (const auto& [id, operation] : active_operations_) {
      (void)id;
      for (const auto& participant : operation->participants)
        work_bound += participant.io_rule_registrations.size();
    }
    work_bound += 2U * global_io_rules_.size();
  }
  for (std::size_t step = 0; step < work_bound; ++step) {
    StopWorkHandoff handoff;
    Result<void> processed = Result<void>::Ok();
    {
      std::lock_guard lock(state_mutex_);
      processed = process_io_rule_evidence_locked(handoff);
      if (processed.ok() && !handoff.control && !handoff.projection)
        processed = process_global_io_rule_evidence_locked(handoff);
    }
    publish_stop_work(handoff);
    if (!processed.ok()) return processed;
  }
  return Result<void>::Ok();
}

ResourceAddressedExecutionKernel::StopTransaction*
ResourceAddressedExecutionKernel::io_rule_response_owner_locked(
    const ActiveOperation& operation, const ActiveOperationParticipant& participant) const {
  const auto row =
      std::find_if(operation.state.participants.begin(), operation.state.participants.end(),
                   [&](const auto& value) {
                     return value.participant_id == participant.slot_owner.participant_id;
                   });
  if (row != operation.state.participants.end()) {
    for (const auto& outcome : row->io_rule_outcomes) {
      if (outcome.arbitration_disposition != IoRuleArbitrationDispositionDTO::kActive ||
          !outcome.stop_id || outcome.stop_certificate_id)
        continue;
      for (const auto& transaction : stop_transactions_) {
        if (!transaction || !transaction->active || transaction->terminal ||
            transaction->stop_id != outcome.stop_id ||
            transaction->stop_generation != outcome.stop_generation)
          continue;
        if (std::any_of(
                transaction->interrupt_winners.begin(), transaction->interrupt_winners.end(),
                [&](const auto& winner) { return winner.registration == outcome.registration; }))
          return transaction.get();
      }
    }
  }
  for (const auto& transaction : stop_transactions_) {
    if (transaction && transaction->active && !transaction->terminal &&
        transaction->stop_id == operation.state.active_stop_id &&
        transaction->stop_generation == operation.state.active_stop_generation &&
        std::binary_search(transaction->operations.begin(), transaction->operations.end(),
                           operation.state.operation_id) &&
        !scoped_global_stop_excludes_participant(*transaction, operation.state.operation_id,
            participant.slot_owner.participant_id, participant.resources))
      return transaction.get();
  }
  return nullptr;
}

Result<void> ResourceAddressedExecutionKernel::process_io_rule_evidence_locked(
    StopWorkHandoff& handoff, const OperationId* selected_operation) {
  // Interrupt evaluation is paused while the cell is latched.
  if (cell_latched_locked()) return Result<void>::Ok();
  for (auto& [id, operation] : active_operations_) {
    if (selected_operation && id != *selected_operation) continue;
    if (terminal(operation->state.state)) continue;
    std::optional<InterruptStopSelection> selection;
    int selected_priority = -1;
    std::optional<RuleSourceOrder> selected_source;
    std::vector<ResourceId> selected_resources;
    std::vector<IoRuleRegistrationDTO> eligible;
    std::vector<InterruptStopSelection> participant_selections;
    for (auto& participant : operation->participants) {
      if (participant.ownership_transferred || !participant.prepared_participant) continue;
      auto* resident = io_rule_response_owner_locked(*operation, participant);
      const auto actual_row = std::find_if(operation->state.participants.begin(),
          operation->state.participants.end(), [&](const auto& row) {
            return row.participant_id == participant.slot_owner.participant_id;
          });
      // A sibling excluded from a scoped global Stop keeps its actual phase
      // and executing-only node guard, including after that Stop's certificate.
      const bool unselected_global_sibling = global_io_rule_catalog_ &&
          operation->state.active_stop_id && !participant.stop_owns_reconciliation() &&
          actual_row != operation->state.participants.end() &&
          actual_row->state == OperationStateKind::kExecuting;
      if (operation->state.active_stop_id && !resident && !unselected_global_sibling) continue;
      const auto* motion =
          std::get_if<PreparedMotionExecutionDTO>(&participant.prepared_participant->payload);
      if (!motion) continue;
      for (const auto& token : participant.io_rule_registrations) {
        if (!io_rule_scope_current_locked(*operation, participant, token)) {
          return Result<void>::Err(rule_error(kErrorInternalError, "io_rule_membership_stale"));
        }
        const auto evidence = io_signal_service_->ReadRuleEvidence(token);
        if (!evidence.ok()) return Result<void>::Err(evidence.error());
        if (evidence.value().evaluation_error)
          return Result<void>::Err(*evidence.value().evaluation_error);
        const auto row =
            std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                         [&](const auto& value) {
                           return value.participant_id == participant.slot_owner.participant_id;
                         });
        if (row == operation->state.participants.end())
          return Result<void>::Err(rule_error(kErrorInternalError, "io_rule_participant_missing"));
        const auto outcome =
            std::find_if(row->io_rule_outcomes.begin(), row->io_rule_outcomes.end(),
                         [&](const auto& value) { return value.registration == token; });
        if (outcome == row->io_rule_outcomes.end())
          return Result<void>::Err(rule_error(kErrorInternalError, "io_rule_outcome_missing"));
        outcome->evidence = evidence.value();
        if (!evidence.value().trip && evidence.value().source_losses.empty()) continue;
        const auto& exact = std::get<IoOperationRuleScopeDTO>(token.scope);
        const auto scope = std::find_if(
            motion->interrupt_scopes.begin(), motion->interrupt_scopes.end(),
            [&](const auto& value) { return value.scope_id == exact.motion_interrupt_scope_id; });
        const auto policy = std::find_if(
            participant.io_motion_rule_policies.begin(), participant.io_motion_rule_policies.end(),
            [&](const auto& value) { return value.policy_id == token.policy_id; });
        if (scope == motion->interrupt_scopes.end() ||
            policy == participant.io_motion_rule_policies.end())
          return Result<void>::Err(rule_error(kErrorInternalError, "io_rule_policy_scope_missing"));
        if (!policy->semantics)
          return Result<void>::Err(rule_error(kErrorInternalError, "io_rule_policy_unqualified"));
        if (!scope->stop_entry || !scope->success_through ||
            !VerifyPreparedIoRuleSuccessThrough(scope->stop_plan, *scope->stop_entry,
                                                *scope->success_through)
                 .ok())
          return Result<void>::Err(
              rule_error(kErrorInternalError, "io_rule_prepared_stop_response_mismatch"));
        const auto& semantics = *policy->semantics;
        const bool source_failure = !evidence.value().source_losses.empty();
        // The pre-effect arm acknowledgement is the exact dispatch boundary;
        // a configured already-active baseline can stop it before any effect.
        const bool arm_boundary =
            selected_operation && !resident && !participant.prepared_dispatch_released;
        const auto phase = resident       ? OperationStateKind::kStopping
                           : arm_boundary ? OperationStateKind::kExecuting
                           : unselected_global_sibling ? actual_row->state
                                                       : operation->state.state;
        // A source failure is not a desired trip and cannot be muted merely
        // because another Stop already owns containment.
        if (!source_failure && !semantics.affected_phases.contains(phase)) continue;
        const InterruptStopSelection* active_winner = nullptr;
        if (resident) {
          const auto found = std::find_if(
              resident->interrupt_winners.begin(), resident->interrupt_winners.end(),
              [&](const auto& winner) {
                const auto& owner = std::get<IoOperationRuleScopeDTO>(winner.registration.scope);
                return owner.operation_id == id && owner.participant_id == exact.participant_id;
              });
          if (found != resident->interrupt_winners.end()) active_winner = &*found;
        }
        if (resident && !source_failure) {
          int global_priority = -1;
          for (const auto& global : resident->global_interrupt_winners) {
            if (std::any_of(global.participants.begin(), global.participants.end(),
                [&](const auto& selected) {
                  return selected.operation_id == id && selected.participant_id == exact.participant_id;
                })) global_priority = std::max(global_priority, global.priority);
          }
          if (global_priority >= policy->effective_priority) continue;
        }
        if (resident && !source_failure && active_winner &&
            policy->effective_priority <= active_winner->priority) {
          const bool suppressed =
              semantics.effective_maskable &&
              std::find(semantics.maskable_by.begin(), semantics.maskable_by.end(),
                        active_winner->registration.policy_id) != semantics.maskable_by.end();
          if (outcome->arbitration_disposition != IoRuleArbitrationDispositionDTO::kSuperseded &&
              outcome->arbitration_disposition != IoRuleArbitrationDispositionDTO::kActive)
            outcome->arbitration_disposition =
                suppressed ? IoRuleArbitrationDispositionDTO::kSuppressedByActiveMask
                           : IoRuleArbitrationDispositionDTO::kQueued;
          continue;
        }
        if (resident && source_failure) {
          // Join the actual containment reference; retain all first source
          // failures even if a stronger logical fault already owns arbitration.
          outcome->stop_id = resident->stop_id;
          outcome->stop_generation = resident->stop_generation;
          continue;
        }
        if (!resident) {
          const auto global = global_io_rule_supersedes_node_locked(*policy, evidence.value(),
                                                                     participant);
          if (!global.ok()) return Result<void>::Err(global.error());
          if (global.value()) continue;
        }
        eligible.push_back(token);
        const auto source = rule_source_order(evidence.value());
        if (!source)
          return Result<void>::Err(
              rule_error(kErrorInternalError, "io_rule_ordering_evidence_missing"));
        auto resources = participant.resources;
        std::sort(resources.begin(), resources.end());
        const bool same_priority = selection && policy->effective_priority == selected_priority;
        const bool canonical_before =
            same_priority &&
            (resources < selected_resources ||
             (resources == selected_resources && source_order_less(*source, *selected_source)));
        const bool identical_key = same_priority && resources == selected_resources &&
                                   !source_order_less(*source, *selected_source) &&
                                   !source_order_less(*selected_source, *source);
        const bool better = !selection || policy->effective_priority > selected_priority ||
                            canonical_before ||
                            (identical_key && token.policy_id < selection->registration.policy_id);
        const auto& plan =
            semantics.preemption_action == InterruptPreemptionAction::kCompleteMoveOnSignal
                ? scope->stop_plan
                : participant.prepared_participant->controller().ordinary_stop_plan;
        InterruptStopSelection candidate{token,
                                         evidence.value(),
                                         plan,
                                         source->source_id,
                                         semantics.preemption_action,
                                         policy->effective_priority,
                                         policy->maximum_scope->stop_scope,
                                         *scope->stop_entry,
                                         *scope->success_through};
        const auto independent = std::find_if(
            participant_selections.begin(), participant_selections.end(), [&](const auto& value) {
              const auto& owner = std::get<IoOperationRuleScopeDTO>(value.registration.scope);
              return owner.participant_id == exact.participant_id;
            });
        if (independent == participant_selections.end()) {
          participant_selections.push_back(candidate);
        } else {
          const auto prior_source = rule_source_order(independent->evidence);
          const bool before =
              prior_source && (source_order_less(*source, *prior_source) ||
                               (!source_order_less(*prior_source, *source) &&
                                token.policy_id < independent->registration.policy_id));
          if (policy->effective_priority > independent->priority ||
              (policy->effective_priority == independent->priority && before))
            *independent = candidate;
        }
        if (better) {
          selection = std::move(candidate);
          selected_priority = policy->effective_priority;
          selected_source = source;
          selected_resources = resources;
        }
      }
    }
    if (!selection) continue;
    std::erase_if(participant_selections,
                  [&](const auto& value) { return value.registration == selection->registration; });
    const auto request_id = content_identity::sha256_digest(
        config_.request_owner_epoch.value() + "\nio-rule-stop\n" + id.value() + "\n" +
        std::to_string(selection->registration.registration_id));
    const bool source_failure = !selection->evidence.source_losses.empty();
    StopIntentDTO intent{
        PublicRequestContextDTO{RequestId(request_id), config_.request_owner_epoch,
                                "kernel.io_rule", std::nullopt},
        std::vector<StopOperationTargetDTO>{
            {id, operation->state.operation_revision,
             operation->state.active_stop_generation.value_or(StopGeneration(0U))}},
        source_failure ? "interrupt_source_unavailable"
        : selection->action == InterruptPreemptionAction::kCompleteMoveOnSignal
            ? "motion_interrupt_signal_trip"
            : "interrupt_fault"};
    const bool full_cell = selection->stop_scope == IoRuleStopScopeDTO::kFullCell ||
                           std::any_of(participant_selections.begin(), participant_selections.end(),
                                       [](const auto& value) {
                                         return value.stop_scope == IoRuleStopScopeDTO::kFullCell;
                                       });
    EmergencyStopProjectedIntentDTO projected =
        full_cell ? EmergencyStopProjectedIntentDTO{FullStopIntentDTO{
                        intent.request_context, cell_stop_generation_, intent.attribution}}
                  : EmergencyStopProjectedIntentDTO{std::move(intent)};
    const auto stopped =
        accept_stop_locked(std::move(projected), handoff, clock_.now(), nullptr, nullptr,
                           nullptr, &*selection, participant_selections);
    if (!stopped.ok()) return Result<void>::Err(stopped.error());
    for (auto& row : operation->state.participants) {
      const InterruptStopSelection* row_winner =
          std::get<IoOperationRuleScopeDTO>(selection->registration.scope).participant_id ==
                  row.participant_id
              ? &*selection
              : nullptr;
      if (!row_winner) {
        const auto found = std::find_if(
            participant_selections.begin(), participant_selections.end(), [&](const auto& value) {
              return std::get<IoOperationRuleScopeDTO>(value.registration.scope).participant_id ==
                     row.participant_id;
            });
        if (found != participant_selections.end()) row_winner = &*found;
      }
      if (!row_winner) continue;
      for (auto& outcome : row.io_rule_outcomes) {
        if (outcome.registration != row_winner->registration) {
          if (outcome.stop_id && !outcome.stop_certificate_id &&
              outcome.arbitration_disposition == IoRuleArbitrationDispositionDTO::kActive) {
            outcome.arbitration_disposition = IoRuleArbitrationDispositionDTO::kSuperseded;
            outcome.stop_id = stopped.value().stop_id;
            outcome.stop_generation = stopped.value().stop_generation;
          }
          if (outcome.arbitration_disposition == IoRuleArbitrationDispositionDTO::kSuperseded ||
              std::find(eligible.begin(), eligible.end(), outcome.registration) == eligible.end())
            continue;
          const auto participant =
              std::find_if(operation->participants.begin(), operation->participants.end(),
                           [&](const auto& value) {
                             return value.slot_owner.participant_id == row.participant_id;
                           });
          if (participant == operation->participants.end()) continue;
          const auto policy = std::find_if(
              participant->io_motion_rule_policies.begin(),
              participant->io_motion_rule_policies.end(),
              [&](const auto& value) { return value.policy_id == outcome.registration.policy_id; });
          if (policy == participant->io_motion_rule_policies.end() || !policy->semantics) continue;
          const auto& semantics = *policy->semantics;
          const bool suppressed =
              semantics.effective_maskable &&
              std::find(semantics.maskable_by.begin(), semantics.maskable_by.end(),
                        row_winner->registration.policy_id) != semantics.maskable_by.end();
          outcome.arbitration_disposition =
              suppressed ? IoRuleArbitrationDispositionDTO::kSuppressedByActiveMask
                         : IoRuleArbitrationDispositionDTO::kQueued;
          continue;
        }
        outcome.stop_id = stopped.value().stop_id;
        outcome.stop_generation = stopped.value().stop_generation;
        outcome.arbitration_disposition = IoRuleArbitrationDispositionDTO::kActive;
      }
    }
    // A join needs no second worker wake or deadline. Continue bounded scans
    // for independent operations; a new Stop uses the existing publication slot.
    if (handoff.control || handoff.projection) return Result<void>::Ok();
  }
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::finalize_io_rule_stop_locked(
    StopTransaction& transaction, const StopCertificateDTO& certificate) {
  for (const auto& id : stop_operation_snapshot_ids(transaction)) {
    auto* operation = find_active_operation_locked(id);
    if (!operation) continue;
    for (auto& participant : operation->participants) {
      if (scoped_global_stop_excludes_participant(transaction, id,
              participant.slot_owner.participant_id, participant.resources))
        continue;
      const auto row =
          std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                       [&](const auto& value) {
                         return value.participant_id == participant.slot_owner.participant_id;
                       });
      if (row == operation->state.participants.end()) continue;
      const bool pending_elsewhere = std::any_of(
          row->io_rule_outcomes.begin(), row->io_rule_outcomes.end(), [&](const auto& outcome) {
            return outcome.stop_id && !outcome.stop_certificate_id &&
                   (outcome.stop_id != certificate.stop_id ||
                    outcome.stop_generation != certificate.stop_generation);
          });
      if (!pending_elsewhere) {
        const auto retired = retire_participant_io_rules_locked(participant);
        if (!retired.ok()) {
          continue;
        }
      }
      const bool source_failure =
          std::any_of(row->io_rule_outcomes.begin(), row->io_rule_outcomes.end(),
                      [](const auto& outcome) { return !outcome.evidence.source_losses.empty(); });
      const auto selected_winner = std::find_if(
          transaction.interrupt_winners.begin(), transaction.interrupt_winners.end(),
          [&](const auto& value) {
            const auto& owner = std::get<IoOperationRuleScopeDTO>(value.registration.scope);
            return owner.operation_id == id && owner.participant_id == row->participant_id;
          });
      const auto* winner =
          selected_winner == transaction.interrupt_winners.end() ? nullptr : &*selected_winner;
      const GlobalInterruptStopSelection* global_winner = nullptr;
      for (const auto& candidate : transaction.global_interrupt_winners) {
        const bool member = std::any_of(candidate.participants.begin(), candidate.participants.end(),
            [&](const auto& value) {
              return value.operation_id == id && value.participant_id == row->participant_id &&
                     value.controller_goal_id == row->command_id &&
                     value.controller_goal_generation == row->command_generation;
            });
        if (!member) continue;
        const auto precedes = [&](const auto& left, const auto& right) {
          if (left.priority != right.priority) return left.priority > right.priority;
          if (left.scope_resources != right.scope_resources)
            return left.scope_resources < right.scope_resources;
          const auto left_source =
              left.evidence ? rule_source_order(*left.evidence) : std::nullopt;
          const auto right_source =
              right.evidence ? rule_source_order(*right.evidence) : std::nullopt;
          if (left_source && right_source) {
            if (source_order_less(*left_source, *right_source)) return true;
            if (source_order_less(*right_source, *left_source)) return false;
          } else if (left_source.has_value() != right_source.has_value()) {
            return left_source.has_value();
          }
          return left.policy_id < right.policy_id;
        };
        if (!global_winner || precedes(candidate, *global_winner)) global_winner = &candidate;
      }
      const bool global_fault_winner =
          global_winner && (!winner || global_winner->priority >= winner->priority);
      ErrorDTO global_fault_error{};
      if (global_fault_winner) global_fault_error = global_winner->watch_lost
          ? ErrorDTO{"interrupt_source_unavailable", "Global interrupt rule can't watch its signals",
                     {{"reason", "io_global_rule_cant_watch"}, {"stopped_by", "rule"},
                      {"rule_id", global_winner->policy_id.value()}}}
          : ErrorDTO{"interrupt_fault", "Global interrupt rule matched",
                     {{"reason", global_winner->fail_node ? "io_global_rule_fail_node"
                                                          : "io_global_rule_hold"},
                      {"stopped_by", "rule"}, {"rule_id", global_winner->policy_id.value()}}};
      const bool fault_winner =
          winner && winner->evidence.trip &&
          winner->action == InterruptPreemptionAction::kPreemptTrajectory &&
          std::get<IoOperationRuleScopeDTO>(winner->registration.scope).operation_id == id &&
          std::get<IoOperationRuleScopeDTO>(winner->registration.scope).participant_id ==
              row->participant_id;
      for (auto& outcome : row->io_rule_outcomes) {
        // A broader accepted Stop owns this logical response. The predecessor
        // still owns physical cleanup, but cannot terminalize the newer fact.
        if (outcome.stop_id && (outcome.stop_id != certificate.stop_id ||
                                outcome.stop_generation != certificate.stop_generation))
          continue;
        const bool selected = winner && winner->registration == outcome.registration;
        outcome.stop_id = certificate.stop_id;
        outcome.stop_generation = certificate.stop_generation;
        outcome.stop_certificate_id = certificate.stop_certificate_id;
        outcome.stop_certificate_revision = certificate.stop_certificate_revision;
        outcome.stop_result = certificate.result;
        if (global_fault_winner && !outcome.evidence.source_losses.empty()) {
          outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
          outcome.error = rule_error("interrupt_source_unavailable", "io_rule_required_source_lost");
          if (row->state == OperationStateKind::kStopped) {
            row->state = OperationStateKind::kFailed;
            row->terminal_error = outcome.error;
          }
        } else if (global_fault_winner) {
          const auto policy = std::find_if(participant.io_motion_rule_policies.begin(),
              participant.io_motion_rule_policies.end(), [&](const auto& value) {
                return value.policy_id == outcome.registration.policy_id;
              });
          const bool desired = policy != participant.io_motion_rule_policies.end() &&
              policy->semantics && policy->semantics->preemption_action ==
                  InterruptPreemptionAction::kCompleteMoveOnSignal;
          const bool contained = (certificate.result == StopCertificateResultDTO::kSucceededNominal ||
              certificate.result == StopCertificateResultDTO::kSucceededEscalated) &&
              !certificate.terminal_error && row->terminal_fact &&
              (row->terminal_fact->effect == ParticipantEffectKnowledge::kEffectSettled ||
               row->terminal_fact->effect == ParticipantEffectKnowledge::kNoEffect);
          outcome.terminal_cause = desired && contained
              ? MotionExecutionTerminalCause::kInterruptedFault
              : MotionExecutionTerminalCause::kFailed;
          if (desired && contained) outcome.error.reset();
          else outcome.error = row->terminal_error.value_or(global_fault_error);
        } else if (fault_winner && selected) {
          // Operation FaultTerminal is a real failed operation with its own
          // diagnostic, not a cell latch or a desired-trip success.
          outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
          outcome.error =
              ErrorDTO{"interrupt_fault",
                       "Configured operation fault rule matched",
                       {{"reason", "io_rule_fault_terminal"},
                        {"policy_id", winner->registration.policy_id.value()},
                        {"source_id", winner->source_id.value()},
                        {"registration_id", std::to_string(winner->registration.registration_id)}}};
          if (terminal(row->state) && row->state != OperationStateKind::kInterventionRequired) {
            row->state = OperationStateKind::kFailed;
            row->terminal_error = outcome.error;
          }
        } else if (fault_winner &&
                   outcome.registration.policy_id != winner->registration.policy_id) {
          const auto policy =
              std::find_if(participant.io_motion_rule_policies.begin(),
                           participant.io_motion_rule_policies.end(), [&](const auto& candidate) {
                             return candidate.policy_id == outcome.registration.policy_id;
                           });
          if (policy != participant.io_motion_rule_policies.end() && policy->semantics &&
              policy->semantics->preemption_action ==
                  InterruptPreemptionAction::kCompleteMoveOnSignal) {
            outcome.terminal_cause = MotionExecutionTerminalCause::kInterruptedFault;
            outcome.error.reset();
          } else {
            outcome.terminal_cause = MotionExecutionTerminalCause::kStopped;
          }
        } else if (!outcome.evidence.source_losses.empty()) {
          outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
          outcome.error =
              rule_error("interrupt_source_unavailable", "io_rule_required_source_lost");
          if (row->state == OperationStateKind::kStopped) {
            row->state = OperationStateKind::kFailed;
            row->terminal_error = outcome.error;
          }
        } else if (!selected) {
          outcome.terminal_cause = MotionExecutionTerminalCause::kStopped;
        } else if (source_failure) {
          outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
          outcome.error =
              rule_error("interrupt_source_unavailable", "io_rule_required_source_lost");
          if (row->state == OperationStateKind::kStopped) {
            row->state = OperationStateKind::kFailed;
            row->terminal_error = outcome.error;
          }
        } else if ((certificate.result == StopCertificateResultDTO::kSucceededNominal ||
                    certificate.result == StopCertificateResultDTO::kSucceededEscalated) &&
                   !certificate.terminal_error && row->state == OperationStateKind::kStopped &&
                   row->terminal_fact &&
                   (row->terminal_fact->effect == ParticipantEffectKnowledge::kEffectSettled ||
                    row->terminal_fact->effect == ParticipantEffectKnowledge::kNoEffect)) {
          const auto successful =
              successful_stop_stage(certificate, id, row->participant_id, winner->plan);
          const bool no_effect =
              row->terminal_fact->effect == ParticipantEffectKnowledge::kNoEffect;
          const bool entry_proved =
              VerifyPreparedIoRuleStopEntry(winner->plan, winner->stop_entry).ok();
          const bool threshold_proved =
              VerifyPreparedIoRuleSuccessThrough(winner->plan, winner->stop_entry,
                                                 winner->success_through)
                  .ok();
          const bool response_proved =
              entry_proved &&
              ((successful && *successful >= winner->stop_entry.stage_index) || no_effect);
          const bool within_success_threshold =
              threshold_proved &&
              (!successful || *successful <= winner->success_through.stage_index);
          const auto actual_index = successful.value_or(winner->stop_entry.stage_index);
          const auto* motion = participant.prepared_participant
                                   ? std::get_if<PreparedMotionExecutionDTO>(
                                         &participant.prepared_participant->payload)
                                   : nullptr;
          const auto& exact = std::get<IoOperationRuleScopeDTO>(outcome.registration.scope);
          const auto scope =
              motion
                  ? std::find_if(motion->interrupt_scopes.begin(), motion->interrupt_scopes.end(),
                                 [&](const auto& value) {
                                   return value.scope_id == exact.motion_interrupt_scope_id;
                                 })
                  : std::vector<PreparedMotionInterruptScopeDTO>::const_iterator{};
          const bool controlled_row = actual_index < winner->plan.stage_commands.size() &&
                                      winner->plan.stage_commands[actual_index]
                                              .resolved_stage.containment_command.command_kind ==
                                          StopContainmentCommandKind::kMotionControlledStop;
          const auto natural =
              controlled_row
                  ? natural_completion_evidence(certificate, id, row->participant_id, actual_index)
                  : std::nullopt;
          const bool natural_allowed = motion && scope != motion->interrupt_scopes.end() &&
                                       scope->stop_profile &&
                                       scope->stop_profile->allow_natural_completion_on_short_path;
          if (!response_proved || !within_success_threshold) {
            outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
            outcome.error = rule_error(kErrorMotionInterruptBackendStopFailed,
                                       !response_proved ? "io_rule_requested_stop_entry_not_proved"
                                                        : "io_rule_success_through_exceeded");
          } else if (natural && *natural) {
            if (natural_allowed) {
              outcome.terminal_cause = MotionExecutionTerminalCause::kNoSignalTrip;
            } else {
              outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
              outcome.error = rule_error(kErrorMotionInterruptStopStateInvalid,
                                         "io_rule_natural_completion_not_allowed");
            }
          } else if (controlled_row && natural_allowed && !natural &&
                     row->terminal_fact->effect != ParticipantEffectKnowledge::kNoEffect) {
            outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
            outcome.error = rule_error(kErrorMotionInterruptBackendStopFailed,
                                       "io_rule_natural_completion_proof_missing");
          } else {
            outcome.terminal_cause = MotionExecutionTerminalCause::kSignalTrip;
          }
        } else {
          outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
          outcome.error = row->terminal_error.value_or(rule_error(
              kErrorMotionInterruptBackendStopFailed, "io_rule_nominal_stop_not_proved"));
        }
      }
      // A Stop the Program's own configuration requested
      // (carried object lost), settled at its first stage, is the Program's
      // ordinary failure. The row keeps its actual Stopped outcome; an
      // escalated, joined or uncertain Stop is relabelled Failed and held.
      const bool this_stop = operation->state.active_stop_id == certificate.stop_id &&
                             operation->state.active_stop_generation == certificate.stop_generation;
      const bool ordinary_program_stop =
          operation->program_requested_stop && this_stop &&
          certificate.result == StopCertificateResultDTO::kSucceededNominal &&
          !certificate.terminal_error &&
          row->state == OperationStateKind::kStopped && row->terminal_fact &&
          row->terminal_fact->kind == ParticipantExecutionFactKind::kPreemptedByAddressedStop &&
          row->terminal_fact->effect == ParticipantEffectKnowledge::kEffectSettled;
      // A global rule's Stop: with `fail_node` the interrupted
      // motion node fails with `interrupt_fault` and the tree handles it. Its
      // ordinary Stop (the Program's own, settled at its first stage) keeps
      // the row Stopped with that typed cause, as the carried-object-lost Stop
      // keeps its own: the operation closes after its terminal cleanup, so the
      // tree's next motion on that robot is admitted. An escalated, joined or
      // uncertain one is relabelled Failed and held. `hold` (and a rule that
      // can't watch) is the operator's Stop and keeps the row Stopped for
      // Resume, its fact naming the rule (`stopped_by: rule`).
      if (global_fault_winner && row->state == OperationStateKind::kStopped) {
        if (global_winner->fail_node) {
          if (!ordinary_program_stop) row->state = OperationStateKind::kFailed;
          row->terminal_error = global_fault_error;
        } else if (row->terminal_fact) {
          row->terminal_fact->error = global_fault_error;
        }
      }
      // The carried-object-lost Stop left its settled row Stopped with the
      // typed cause; only its ordinary outcome stays Stopped.
      if (this_stop && row->state == OperationStateKind::kStopped && row->terminal_error &&
          row->terminal_error->code == "carried_object_lost" && !ordinary_program_stop)
        row->state = OperationStateKind::kFailed;
    }
    if (operation->state.state != OperationStateKind::kInterventionRequired)
      operation->state.state =
          std::any_of(operation->participants.begin(), operation->participants.end(),
                      [&](const auto& participant) {
                        if (!participant.stop_owns_reconciliation()) return false;
                        const auto row = std::find_if(
                            operation->state.participants.begin(), operation->state.participants.end(),
                            [&](const auto& state) {
                              return state.participant_id == participant.slot_owner.participant_id;
                            });
                        return row == operation->state.participants.end() || !terminal(row->state);
                      })
              ? OperationStateKind::kStopping
              : aggregate_participant_state(operation->state.participants);
  }
}

}  // namespace manyforge::core
