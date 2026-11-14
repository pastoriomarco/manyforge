#include <manyforge_core/io_rule_stop_response.hpp>
#include <tuple>

#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {
namespace {

ErrorDTO rule_error(const char* code, const char* reason) {
  return {
      code, "Motion interrupt rule could not complete its required response", {{"reason", reason}}};
}

// Read only actual authoritative nominal-stage evidence of the participant's
// own Stop attempt. A permitted natural finish is different from a braking
// signal trip.
std::optional<bool> natural_completion_evidence(const StopParticipantWork& work,
                                                std::uint64_t stage_index) {
  std::optional<bool> natural;
  for (const auto& retained : work.retained_results) {
    if (!retained.authoritative || retained.result.request.stage_index != stage_index ||
        retained.result.outcome != ParticipantStopStageOutcomeDTO::kSucceeded)
      continue;
    for (const auto& item : retained.result.evidence_items) {
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

// One rule's candidate response for one participant (S9), chosen under the
// Kernel lock; the plan is the participant's own immutable prepared plan.
struct RuleStopCandidate final {
  IoRuleRegistrationDTO registration;
  IoRuleEvidenceDTO evidence;
  const PreparedParticipantStopPlanDTO* plan{};
  InterruptPreemptionAction action{InterruptPreemptionAction::kCompleteMoveOnSignal};
  int priority{};
  IoRuleStopScopeDTO stop_scope{};
  IoRulePreparedStopEntryDTO stop_entry;
  IoRulePreparedStopEntryDTO success_through;
  OperationParticipantId participant_id;
};

}  // namespace

bool ResourceAddressedExecutionKernel::io_rule_scope_current_locked(
    const ActiveOperation& operation, const ActiveOperationParticipant& participant,
    const IoRuleRegistrationDTO& registration) const {
  const auto* scope = std::get_if<IoOperationRuleScopeDTO>(&registration.scope);
  if (!scope || participant.ownership_transferred || !participant.prepared_participant ||
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
         row->command_id == scope->controller_goal_id;
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
                       participant.io_rule_registrations.end(), [&](const auto& slot) {
                         return io_rule_scope_current_locked(operation, participant, slot.token);
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
      participant.io_rule_registrations.push_back({armed.value()});
      if (armed.value().scope != IoRuleScopeDTO(exact) ||
          armed.value().policy_id != scope.interrupt_policy_id)
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
  for (const auto& slot : participant.io_rule_registrations) {
    const auto& registration = slot.token;
    const auto evidence = io_signal_service_->RetireRule(registration);
    if (!evidence.ok()) return Result<void>::Err(evidence.error());
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
          if (evidence.value().trip || !evidence.value().source_losses.empty() ||
              evidence.value().evaluation_error ||
              outcome->evidence.state != IoRuleRegistrationState::kRetired)
            outcome->evidence = evidence.value();
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

std::vector<ErrorDTO> ResourceAddressedExecutionKernel::retire_all_io_rules_locked() {
  // A failed retire is a defect: its error is kept and the teardown goes on.
  std::vector<ErrorDTO> failures;
  const auto keep = [&](const Result<void>& retired) {
    if (!retired.ok()) failures.push_back(retired.error());
  };
  const auto retire_operation = [&](ActiveOperation& operation) {
    for (auto& participant : operation.participants)
      keep(retire_participant_io_rules_locked(participant));
  };
  for (auto& [id, operation] : active_operations_) {
    (void)id;
    retire_operation(*operation);
    if (!operation->pending_motion_takeover.empty())
      retire_operation(*operation->pending_motion_takeover.mapped());
    if (!operation->next_motion_takeover.empty())
      retire_operation(*operation->next_motion_takeover.mapped());
  }
  keep(retire_global_io_rules_locked());
  keep(retire_staged_global_io_rules_locked());
  return failures;
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
  // Obtained before the Kernel lock (it may allocate on this thread's first
  // Stop); completed after each step's unlock.
  StopHandoff& handoff = thread_stop_handoff();
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
  // Several rule Stops in one step all publish: the hand-off is a bit set.
  for (std::size_t step = 0; step < work_bound; ++step) {
    Result<void> processed = Result<void>::Ok();
    {
      std::lock_guard lock(state_mutex_);
      processed = process_io_rule_evidence_locked(handoff);
      if (processed.ok()) processed = process_global_io_rule_evidence_locked(handoff);
    }
    complete_stop_handoff(handoff);
    if (!processed.ok()) return processed;
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::process_io_rule_evidence_locked(
    StopHandoff& handoff, const OperationNumber* selected_operation) {
  // Interrupt evaluation is paused while the cell is latched.
  if (cell_latched_locked()) return Result<void>::Ok();
  for (auto& [id, operation] : active_operations_) {
    if (selected_operation && id != *selected_operation) continue;
    if (terminal(operation->state.state)) continue;
    std::optional<RuleStopCandidate> selection;
    int selected_priority = -1;
    std::optional<RuleSourceOrder> selected_source;
    std::vector<ResourceId> selected_resources;
    std::vector<IoRuleRegistrationDTO> eligible;
    std::vector<RuleStopCandidate> participant_selections;
    // The winning registration's stored Stop key (its slot outlives this pass).
    std::optional<RequestKey>* selection_stop_key = nullptr;
    for (auto& participant : operation->participants) {
      if (participant.ownership_transferred || !participant.prepared_participant) continue;
      // A Stop attempt already contains this participant (its own physical
      // response is fixed by the Stop that started it).
      const bool resident = participant.stop.phase == StopPhase::kStopping;
      const auto actual_row = std::find_if(operation->state.participants.begin(),
          operation->state.participants.end(), [&](const auto& row) {
            return row.participant_id == participant.slot_owner.participant_id;
          });
      // A sibling excluded from a scoped global Stop keeps its actual phase
      // and executing-only node guard, including after that Stop ended.
      const bool unselected_global_sibling = global_io_rule_catalog_ &&
          operation->state.stop_event_id && !participant.stop_owns_reconciliation() &&
          actual_row != operation->state.participants.end() &&
          actual_row->state == OperationStateKind::kExecuting;
      if (operation->state.stop_event_id && !resident && !unselected_global_sibling) continue;
      const auto* motion =
          std::get_if<PreparedMotionExecutionDTO>(&participant.prepared_participant->payload);
      if (!motion) continue;
      for (auto& slot : participant.io_rule_registrations) {
        const auto& token = slot.token;
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
        // The rule response already active on this participant (its outcome
        // names a pending Stop event), and its priority.
        const IoMotionRuleOutcomeDTO* active_winner = nullptr;
        int active_priority = -1;
        if (resident) {
          const auto found = std::find_if(
              row->io_rule_outcomes.begin(), row->io_rule_outcomes.end(), [](const auto& value) {
                return value.arbitration_disposition == IoRuleArbitrationDispositionDTO::kActive &&
                       value.stop_event_id && !value.stop_result;
              });
          if (found != row->io_rule_outcomes.end()) {
            const auto active_policy = std::find_if(
                participant.io_motion_rule_policies.begin(),
                participant.io_motion_rule_policies.end(), [&](const auto& value) {
                  return value.policy_id == found->registration.policy_id;
                });
            if (active_policy != participant.io_motion_rule_policies.end()) {
              active_winner = &*found;
              active_priority = active_policy->effective_priority;
            }
          }
        }
        if (resident && !source_failure) {
          // A global rule's Stop already containing it, by its attribution.
          const auto& attribution = participant.stop.attribution;
          const int global_priority =
              (attribution.rule_index & kStopAttributionGlobalRule) != 0U &&
                      attribution.attribution_class != StopAttributionClass::kOperator
                  ? attribution.rule_priority
                  : -1;
          if (global_priority >= policy->effective_priority) continue;
        }
        if (resident && !source_failure && active_winner &&
            policy->effective_priority <= active_priority) {
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
          // Join the actual containment (the attempt's starting event); retain
          // all first source failures even if a stronger logical fault
          // already owns arbitration.
          outcome->stop_event_id = participant.stop.started_by;
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
        RuleStopCandidate candidate{token,
                                    evidence.value(),
                                    &plan,
                                    semantics.preemption_action,
                                    policy->effective_priority,
                                    policy->maximum_scope->stop_scope,
                                    *scope->stop_entry,
                                    *scope->success_through,
                                    participant.slot_owner.participant_id};
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
          selection_stop_key = &slot.stop_key;
          selected_priority = policy->effective_priority;
          selected_source = source;
          selected_resources = resources;
        }
      }
    }
    if (!selection) continue;
    std::erase_if(participant_selections,
                  [&](const auto& value) { return value.registration == selection->registration; });
    const bool source_failure = !selection->evidence.source_losses.empty();
    const bool full_cell = selection->stop_scope == IoRuleStopScopeDTO::kFullCell ||
                           std::any_of(participant_selections.begin(), participant_selections.end(),
                                       [](const auto& value) {
                                         return value.stop_scope == IoRuleStopScopeDTO::kFullCell;
                                       });
    // S9: the winner and each other participant's own winner, as prepared
    // interrupt entries into the participant's own plan.
    std::vector<StopInterruptSelection> selections;
    selections.reserve(1U + participant_selections.size());
    const auto add_selection = [&](const RuleStopCandidate& candidate) {
      selections.push_back(StopInterruptSelection{
          id, candidate.participant_id.ordinal - 1U, candidate.plan,
          static_cast<std::uint8_t>(candidate.stop_entry.stage_index),
          static_cast<std::uint8_t>(candidate.success_through.stage_index)});
    };
    add_selection(*selection);
    for (const auto& candidate : participant_selections) add_selection(candidate);
    // The whole operation is stopped, as today (the whole cell for a
    // full-cell scope); the selections give its tripped robots the rule's
    // prepared entry.
    const StopOperationTargetDTO target{id, operation->state.operation_revision};
    StopRequest request;
    request.request_key = owned_kernel_request_key(*selection_stop_key);
    request.kind = StopKind::kInterruptRule;
    request.selector = full_cell ? StopSelectorKind::kCell : StopSelectorKind::kOperations;
    request.operations = std::span<const StopOperationTargetDTO>(&target, 1U);
    request.interrupt_selections = selections;
    request.source_label = "kernel.io_rule";
    request.reason = source_failure ? "interrupt_source_unavailable"
                     : selection->action == InterruptPreemptionAction::kCompleteMoveOnSignal
                         ? "motion_interrupt_signal_trip"
                         : "interrupt_fault";
    // A trip is the Program's own Stop (its rule's priority); a lost source
    // is not (as today's acceptance attributed them).
    request.attribution_rank =
        source_failure ? StopAttribution{StopAttributionClass::kOperator, 0, 0U}
                       : StopAttribution{StopAttributionClass::kProgramOwn, selection->priority, 0U};
    const auto record = request_stop_locked(request, handoff);
    const StopEventId event_id = stop_events_.records[record].event_id;
    for (auto& row : operation->state.participants) {
      const RuleStopCandidate* row_winner =
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
          if (outcome.stop_event_id && !outcome.stop_result &&
              outcome.arbitration_disposition == IoRuleArbitrationDispositionDTO::kActive) {
            outcome.arbitration_disposition = IoRuleArbitrationDispositionDTO::kSuperseded;
            outcome.stop_event_id = event_id;
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
        outcome.stop_event_id = event_id;
        outcome.stop_result.reset();
        outcome.arbitration_disposition = IoRuleArbitrationDispositionDTO::kActive;
      }
    }
    // Continue the bounded scan for independent operations: every new Stop
    // sets its lane bits in the same hand-off.
  }
  return Result<void>::Ok();
}

// P4 for one participant (called by the Stop lane under the Kernel lock when
// the participant's attempt ended): every rule outcome of the participant
// not yet finalized belongs to this attempt (the one that started it, or a
// later event that covered it while it was stopping), so each takes this
// attempt's result. Memory only.
void ResourceAddressedExecutionKernel::finalize_io_rule_outcomes_locked(
    ActiveOperation& operation, const std::size_t participant_ordinal) {
  const auto found_participant = std::find_if(
      operation.participants.begin(), operation.participants.end(), [&](const auto& value) {
        // `participant_ordinal` is the index in `participants` (the Stop
        // core's convention); the participant id's ordinal is 1 + it.
        return value.slot_owner.participant_id.ordinal == participant_ordinal + 1U;
      });
  if (found_participant == operation.participants.end()) return;
  auto& participant = *found_participant;
  const auto& stop = participant.stop;
  if (stop.phase != StopPhase::kStopped && stop.phase != StopPhase::kFailed) return;
  const auto row =
      std::find_if(operation.state.participants.begin(), operation.state.participants.end(),
                   [&](const auto& value) {
                     return value.participant_id == participant.slot_owner.participant_id;
                   });
  if (row == operation.state.participants.end()) return;
  // The attempt ended: nothing of this participant is pending elsewhere.
  if (!retire_participant_io_rules_locked(participant).ok()) return;
  const auto result = stop.phase == StopPhase::kFailed ? StopEventResult::kSomeFailed
                      : stop.escalated                 ? StopEventResult::kStoppedEscalated
                                                       : StopEventResult::kStopped;
  // Contained (nominal or escalated: an escalated attempt's degraded-Stop
  // error does not make it uncontained) and the effect known.
  const bool contained_settled =
      stop.phase == StopPhase::kStopped && row->terminal_fact &&
      (row->terminal_fact->effect == ParticipantEffectKnowledge::kEffectSettled ||
       row->terminal_fact->effect == ParticipantEffectKnowledge::kNoEffect);
  const bool source_failure =
      std::any_of(row->io_rule_outcomes.begin(), row->io_rule_outcomes.end(),
                  [](const auto& outcome) { return !outcome.evidence.source_losses.empty(); });

  // The per-operation rule whose response this participant's Stop carried:
  // its active outcome, with the response re-read from the participant's own
  // prepared interrupt scope.
  const IoMotionRuleOutcomeDTO* winner = nullptr;
  const IoMotionRulePolicyBindingDTO* winner_policy = nullptr;
  const PreparedMotionInterruptScopeDTO* winner_scope = nullptr;
  const auto* motion = participant.prepared_participant
                           ? std::get_if<PreparedMotionExecutionDTO>(
                                 &participant.prepared_participant->payload)
                           : nullptr;
  for (const auto& outcome : row->io_rule_outcomes) {
    if (outcome.arbitration_disposition != IoRuleArbitrationDispositionDTO::kActive ||
        !outcome.stop_event_id || outcome.stop_result)
      continue;
    const auto policy = std::find_if(
        participant.io_motion_rule_policies.begin(), participant.io_motion_rule_policies.end(),
        [&](const auto& value) { return value.policy_id == outcome.registration.policy_id; });
    const auto& exact = std::get<IoOperationRuleScopeDTO>(outcome.registration.scope);
    const auto scope =
        motion ? std::find_if(motion->interrupt_scopes.begin(), motion->interrupt_scopes.end(),
                              [&](const auto& value) {
                                return value.scope_id == exact.motion_interrupt_scope_id;
                              })
               : std::vector<PreparedMotionInterruptScopeDTO>::const_iterator{};
    if (policy == participant.io_motion_rule_policies.end() || !policy->semantics || !motion ||
        scope == motion->interrupt_scopes.end() || !scope->stop_entry || !scope->success_through)
      continue;
    winner = &outcome;
    winner_policy = &*policy;
    winner_scope = &*scope;
    break;
  }
  const auto winner_action = winner ? winner_policy->semantics->preemption_action
                                    : InterruptPreemptionAction::kCompleteMoveOnSignal;
  const PreparedParticipantStopPlanDTO* winner_plan =
      !winner ? nullptr
      : winner_action == InterruptPreemptionAction::kCompleteMoveOnSignal
          ? &winner_scope->stop_plan
          : &participant.prepared_participant->controller().ordinary_stop_plan;

  // The global rule whose Stop holds this participant's attribution.
  const auto& attribution = stop.attribution;
  const bool global_attribution =
      (attribution.rule_index & kStopAttributionGlobalRule) != 0U &&
      attribution.attribution_class != StopAttributionClass::kOperator &&
      global_io_rule_catalog_ &&
      (attribution.rule_index & kStopAttributionRuleIndexMask) < global_io_rule_catalog_->rules.size();
  const bool global_watch_lost =
      global_attribution && (attribution.rule_index & kStopAttributionWatchLost) != 0U;
  const bool global_fail_node =
      global_attribution && !global_watch_lost &&
      attribution.attribution_class == StopAttributionClass::kProgramOwn;
  const bool global_fault_winner =
      global_attribution && (!winner || attribution.rule_priority >= winner_policy->effective_priority);
  ErrorDTO global_fault_error{};
  if (global_fault_winner) {
    const auto& rule_id =
        global_io_rule_catalog_->rules[attribution.rule_index & kStopAttributionRuleIndexMask].policy_id;
    global_fault_error = global_watch_lost
        ? ErrorDTO{"interrupt_source_unavailable", "Global interrupt rule can't watch its signals",
                   {{"reason", "io_global_rule_cant_watch"}, {"stopped_by", "rule"},
                    {"rule_id", rule_id.value()}}}
        : ErrorDTO{"interrupt_fault", "Global interrupt rule matched",
                   {{"reason", global_fail_node ? "io_global_rule_fail_node"
                                                : "io_global_rule_hold"},
                    {"stopped_by", "rule"}, {"rule_id", rule_id.value()}}};
  }
  const bool fault_winner = winner && winner->evidence.trip &&
                            winner_action == InterruptPreemptionAction::kPreemptTrajectory;
  for (auto& outcome : row->io_rule_outcomes) {
    // Finalized by an earlier attempt of this participant.
    if (outcome.stop_result) continue;
    const bool selected = winner && winner->registration == outcome.registration;
    if (!outcome.stop_event_id) outcome.stop_event_id = stop.started_by;
    outcome.stop_result = result;
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
      outcome.terminal_cause = desired && contained_settled
          ? MotionExecutionTerminalCause::kInterruptedFault
          : MotionExecutionTerminalCause::kFailed;
      if (desired && contained_settled) outcome.error.reset();
      else outcome.error = row->terminal_error.value_or(global_fault_error);
    } else if (fault_winner && selected) {
      // Operation FaultTerminal is a real failed operation with its own
      // diagnostic, not a cell latch or a desired-trip success.
      const auto source = rule_source_order(winner->evidence);
      outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
      outcome.error =
          ErrorDTO{"interrupt_fault",
                   "Configured operation fault rule matched",
                   {{"reason", "io_rule_fault_terminal"},
                    {"policy_id", winner->registration.policy_id.value()},
                    {"source_id", source ? source->source_id.value() : std::string{}},
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
      outcome.error = rule_error("interrupt_source_unavailable", "io_rule_required_source_lost");
      if (row->state == OperationStateKind::kStopped) {
        row->state = OperationStateKind::kFailed;
        row->terminal_error = outcome.error;
      }
    } else if (!selected) {
      outcome.terminal_cause = MotionExecutionTerminalCause::kStopped;
    } else if (source_failure) {
      outcome.terminal_cause = MotionExecutionTerminalCause::kFailed;
      outcome.error = rule_error("interrupt_source_unavailable", "io_rule_required_source_lost");
      if (row->state == OperationStateKind::kStopped) {
        row->state = OperationStateKind::kFailed;
        row->terminal_error = outcome.error;
      }
    } else if (contained_settled && row->state == OperationStateKind::kStopped) {
      // The containing stage of this attempt counts only when the attempt
      // executed the winner's own plan.
      const bool executed_winner_plan =
          stop.plan != nullptr && (stop.plan == winner_plan || *stop.plan == *winner_plan);
      const std::optional<std::uint64_t> successful =
          executed_winner_plan && stop.containing_stage != kNoStopStage &&
                  stop.containing_stage < winner_plan->stage_commands.size()
              ? std::optional<std::uint64_t>(stop.containing_stage)
              : std::nullopt;
      const auto& stop_entry = *winner_scope->stop_entry;
      const auto& success_through = *winner_scope->success_through;
      const bool no_effect = row->terminal_fact->effect == ParticipantEffectKnowledge::kNoEffect;
      const bool entry_proved = VerifyPreparedIoRuleStopEntry(*winner_plan, stop_entry).ok();
      const bool threshold_proved =
          VerifyPreparedIoRuleSuccessThrough(*winner_plan, stop_entry, success_through).ok();
      const bool response_proved =
          entry_proved && ((successful && *successful >= stop_entry.stage_index) || no_effect);
      const bool within_success_threshold =
          threshold_proved && (!successful || *successful <= success_through.stage_index);
      const auto actual_index = successful.value_or(stop_entry.stage_index);
      const bool controlled_row = actual_index < winner_plan->stage_commands.size() &&
                                  winner_plan->stage_commands[actual_index]
                                          .resolved_stage.containment_command.command_kind ==
                                      StopContainmentCommandKind::kMotionControlledStop;
      const auto natural = controlled_row
                               ? natural_completion_evidence(participant.stop_work, actual_index)
                               : std::nullopt;
      const bool natural_allowed = winner_scope->stop_profile &&
                                   winner_scope->stop_profile->allow_natural_completion_on_short_path;
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
  // A Stop the Program's own configuration requested (carried object lost,
  // a rule trip), settled at its intended stage, is the Program's ordinary
  // failure. The row keeps its actual Stopped outcome; an escalated or
  // uncertain Stop, or one an operator Stop covered, is relabelled Failed and
  // held.
  const bool ordinary_program_stop =
      attribution.attribution_class == StopAttributionClass::kProgramOwn &&
      stop.phase == StopPhase::kStopped && !stop.escalated && !stop.error &&
      row->state == OperationStateKind::kStopped && row->terminal_fact &&
      row->terminal_fact->kind == ParticipantExecutionFactKind::kPreemptedByAddressedStop &&
      row->terminal_fact->effect == ParticipantEffectKnowledge::kEffectSettled;
  // A global rule's Stop: with `fail_node` the interrupted motion node fails
  // with `interrupt_fault` and the tree handles it. Its ordinary Stop (the
  // Program's own, settled at its first stage) keeps the row Stopped with that
  // typed cause, as the carried-object-lost Stop keeps its own: the operation
  // closes after its terminal cleanup, so the tree's next motion on that
  // robot is admitted. An escalated or uncertain one is relabelled Failed and
  // held. `hold` (and a rule that can't watch) is the operator's Stop and
  // keeps the row Stopped for Resume, its fact naming the rule
  // (`stopped_by: rule`).
  if (global_fault_winner && row->state == OperationStateKind::kStopped) {
    if (global_fail_node) {
      if (!ordinary_program_stop) row->state = OperationStateKind::kFailed;
      row->terminal_error = global_fault_error;
    } else if (row->terminal_fact) {
      row->terminal_fact->error = global_fault_error;
    }
  }
  // The carried-object-lost Stop left its settled row Stopped with the typed
  // cause; only its ordinary outcome stays Stopped.
  if (row->state == OperationStateKind::kStopped && row->terminal_error &&
      row->terminal_error->code == "carried_object_lost" && !ordinary_program_stop)
    row->state = OperationStateKind::kFailed;
  if (operation.state.state != OperationStateKind::kInterventionRequired)
    operation.state.state =
        std::any_of(operation.participants.begin(), operation.participants.end(),
                    [&](const auto& candidate) {
                      if (!candidate.stop_owns_reconciliation()) return false;
                      const auto state = std::find_if(
                          operation.state.participants.begin(), operation.state.participants.end(),
                          [&](const auto& value) {
                            return value.participant_id == candidate.slot_owner.participant_id;
                          });
                      return state == operation.state.participants.end() || !terminal(state->state);
                    })
            ? OperationStateKind::kStopping
            : aggregate_participant_state(operation.state.participants);
}

}  // namespace manyforge::core
