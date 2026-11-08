#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_core/io_rule_stop_response.hpp>
#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {
namespace {

ErrorDTO global_rule_error(const char* reason) {
  return {"invalid_state_transition", "Global interrupt rule activation is not current",
          {{"reason", reason}}};
}

ErrorDTO global_activation_stale_error(const char* stage) {
  auto error = global_rule_error("io_global_rule_activation_stale");
  error.message += " [";
  error.message += stage;
  error.message += "]";
  error.details.emplace("activation_stage", stage);
  return error;
}

template <class T>
bool canonical_global_names(const std::vector<T>& values) {
  return std::is_sorted(values.begin(), values.end()) &&
         std::adjacent_find(values.begin(), values.end()) == values.end();
}

template <class T>
bool global_identity(const T& value) {
  return !value.empty() && value.value().find('\0') == std::string::npos;
}

Result<void> validate_global_definitions(
    const std::vector<IoActivationRulePolicyBindingDTO>& rules,
    const std::optional<ProgramBindingId>& program_binding_id,
    const std::vector<std::string>& referenced_keys) {
  std::set<std::string> keys;
  const InterruptPolicyId* previous = nullptr;
  for (const auto& rule : rules) {
    const bool waiting = !rule.waiting_for_program_keys.empty();
    if (!global_identity(rule.policy_id) || (static_cast<bool>(rule.condition) == waiting) ||
        (previous && !(*previous < rule.policy_id)) || rule.effective_priority < 0 ||
        rule.effective_priority > 100 || !canonical_global_names(rule.required_signal_sources) ||
        std::any_of(rule.required_signal_sources.begin(), rule.required_signal_sources.end(),
                    [](const auto& id) { return !global_identity(id); }) ||
        (rule.stop_entry_stage_id && !global_identity(*rule.stop_entry_stage_id)) ||
        (rule.on_trip == IoRuleOnTripDTO::kNotify &&
         (rule.after_stop != IoRuleAfterStopDTO::kHold || rule.stop_entry_stage_id)))
      return Result<void>::Err(global_rule_error("io_global_rule_definition_invalid"));
    previous = &rule.policy_id;
    if (waiting && (program_binding_id || !canonical_global_names(rule.waiting_for_program_keys) ||
        std::any_of(rule.waiting_for_program_keys.begin(), rule.waiting_for_program_keys.end(),
                    [](const auto& key) {
                      return key.empty() || key.find('\0') != std::string::npos ||
                             key == "node_outcome" || key.starts_with("node_outcome.") ||
                             key == "interrupt_state" || key.starts_with("interrupt_state.");
                    })))
      return Result<void>::Err(global_rule_error("io_global_rule_waiting_definition_invalid"));
    if (rule.condition) for (const auto& input : rule.condition->inputs())
      if (const auto* key = std::get_if<IoConditionBlackboardKeyDTO>(&input.source))
        keys.insert(key->name);
    if (const auto* group = std::get_if<IoActivationExecutionGroupsScopeDTO>(&rule.scope)) {
      if (!program_binding_id || group->program_binding_id != program_binding_id ||
          !global_identity(group->program_binding_id) || group->group_ids.empty() ||
          !canonical_global_names(group->group_ids) ||
          std::any_of(group->group_ids.begin(), group->group_ids.end(),
                      [](const auto& id) { return id.empty() || id.find('\0') != std::string::npos; }) ||
          !canonical_global_names(group->subjects))
        return Result<void>::Err(global_rule_error("io_global_rule_group_scope_invalid"));
      for (const auto& subject : group->subjects)
        if (!global_identity(subject.controllable_component_id) ||
            !global_identity(subject.command_target_id) || !global_identity(subject.control_domain_id) ||
            !global_identity(subject.control_binding_id) ||
            !global_identity(subject.control_binding_revision))
          return Result<void>::Err(global_rule_error("io_global_rule_group_subject_invalid"));
    }
  }
  if (std::vector<std::string>(keys.begin(), keys.end()) !=
      referenced_keys)
    return Result<void>::Err(global_rule_error("io_global_rule_key_union_invalid"));
  return Result<void>::Ok();
}

Result<void> validate_global_catalog(const IoActivationRuleCatalogDTO& catalog,
                                     const IoRuleServiceConfigurationDTO& service) {
  if (io_rule_state_context_field_error(catalog.context) ||
      catalog.context.owner_epoch != service.owner_epoch ||
      catalog.context.catalog_generation != service.catalog_generation ||
      catalog.rules.size() > service.maximum_registrations ||
      service.maximum_registrations == 0U)
    return Result<void>::Err(global_rule_error("io_global_rule_catalog_invalid"));
  return validate_global_definitions(catalog.rules, catalog.program_binding_id,
                                     catalog.referenced_program_blackboard_keys);
}

Result<void> validate_global_installation(
    const IoRuleAppliedContextDTO& context,
    const std::shared_ptr<const IoActivationRuleCatalogDTO>& catalog,
    const std::shared_ptr<IIoSignalService>& service,
    const std::shared_ptr<const IoRuleWaitingForProgramCatalogDTO>& waiting) {
  if (!global_identity(context.owner_epoch) || !global_identity(context.retained_bundle_id) ||
      !global_identity(context.graph_revision) ||
      !context.active_publication_generation.nonzero() ||
      context.active_publication_generation.value() > 9'007'199'254'740'991ULL ||
      static_cast<bool>(catalog) != static_cast<bool>(service) ||
      (waiting && (catalog || service || waiting->context != context || waiting->rules.empty())))
    return Result<void>::Err(global_rule_error("io_global_rule_candidate_invalid"));
  if (waiting) {
    const auto valid = validate_global_definitions(waiting->rules, std::nullopt, {});
    if (!valid.ok()) return valid;
    if (std::any_of(waiting->rules.begin(), waiting->rules.end(), [](const auto& rule) {
          return rule.waiting_for_program_keys.empty() || !rule.required_signal_sources.empty();
        }))
      return Result<void>::Err(global_rule_error("io_global_rule_waiting_definition_invalid"));
    return Result<void>::Ok();
  }
  // Null/null is a trusted, actually qualified empty Applied product. No
  // service/catalog generation is manufactured for that case.
  if (!catalog) return Result<void>::Ok();
  const auto& actual = catalog->context;
  if (context != IoRuleAppliedContextDTO{actual.owner_epoch,
                                        actual.active_publication_generation,
                                        actual.retained_bundle_id, actual.graph_revision})
    return Result<void>::Err(global_rule_error("io_global_rule_candidate_context_mismatch"));
  const auto configuration = service->RuleServiceConfiguration();
  if (!configuration.ok()) return Result<void>::Err(configuration.error());
  return validate_global_catalog(*catalog, configuration.value());
}

ErrorDTO rule_holds_error(const InterruptPolicyId& rule_id, const char* why) {
  return {"rule_holds", "Rule " + rule_id.value() + " holds its scope (" + why + ")",
          {{"rule_id", rule_id.value()}}};
}

bool reads_program_keys(const IoActivationRulePolicyBindingDTO& rule) {
  return rule.condition &&
         std::any_of(rule.condition->inputs().begin(), rule.condition->inputs().end(),
                     [](const auto& input) {
                       return std::holds_alternative<IoConditionBlackboardKeyDTO>(input.source);
                     });
}

bool qualification_reason_is(const std::optional<ErrorDTO>& error, std::string_view reason) {
  if (!error) return false;
  const auto found = error->details.find("reason");
  return found != error->details.end() && found->second == reason;
}

// Before the first Program key baseline a key rule has nothing to watch
// yet: the run publishes its keys first. Not a loss, not a hold.
template <class Row>
bool key_baseline_pending(const Row& row, const IoActivationRulePolicyBindingDTO& rule) {
  return !row.watched && !row.registration && reads_program_keys(rule) &&
         qualification_reason_is(row.qualification_error, "io_rule_baseline_unavailable");
}

}  // namespace

Result<void> ResourceAddressedExecutionKernel::StageGlobalIoRuleCatalog(
    IoRuleAppliedContextDTO context,
    std::shared_ptr<const IoActivationRuleCatalogDTO> catalog,
    std::shared_ptr<IIoSignalService> service,
    const ConfigurationFenceStatusQueryDTO& held_fence,
    const CellAdmissionClaimDTO& actual_parent_claim,
    std::shared_ptr<const IoRuleWaitingForProgramCatalogDTO> waiting_for_program) {
  if (!held_fence.valid())
    return Result<void>::Err(global_rule_error("io_global_rule_candidate_invalid"));
  const auto valid = validate_global_installation(context, catalog, service, waiting_for_program);
  if (!valid.ok()) return valid;
  auto staged = std::make_unique<StagedGlobalIoRuleCatalog>(StagedGlobalIoRuleCatalog{
      std::move(context), std::move(catalog), std::move(service), held_fence, actual_parent_claim,
      {}, std::move(waiting_for_program)});
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  // The original host Apply active-effect and composition pin remain held.
  // This short owner read ends before Kernel state; no nested host/ledger call.
  const auto held = cell_admission_owner_->GetCellAdmissionRecoveryReceipt(
      {actual_parent_claim.intent, actual_parent_claim, std::nullopt});
  if (!held.ok()) return Result<void>::Err(held.error());
  if (!ValidateCellAdmissionRecoveryReceipt(held.value()).ok() ||
      held.value().status != CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
      held.value().matched_claim != std::optional(actual_parent_claim) ||
      std::none_of(held.value().current_owner_epochs.begin(), held.value().current_owner_epochs.end(),
                   [&](const auto& owner) {
                     return owner.owner_role == actual_parent_claim.intent.owner_role &&
                            owner.owner_epoch == actual_parent_claim.intent.owner_epoch;
                   }) ||
      !config_.operation_admission_gate ||
      !config_.operation_admission_gate->exact_fence_is_held(
          held_fence.configuration_fence_id, held_fence.expected_scope))
    return Result<void>::Err(global_rule_error("io_global_rule_candidate_parent_not_held"));
  std::lock_guard lock(state_mutex_);
  const auto& installed = staged->context;
  if (installed.owner_epoch != config_.request_owner_epoch ||
      installed.retained_bundle_id != stop_retained_bundle_id_ ||
      installed.graph_revision != stop_graph_revision_ ||
      (global_io_rule_applied_context_ && installed.active_publication_generation <=
              global_io_rule_applied_context_->active_publication_generation))
    return Result<void>::Err(global_rule_error("io_global_rule_candidate_stale"));
  if (staged_global_io_rule_catalog_ &&
      (staged_global_io_rule_catalog_->fence != held_fence ||
       staged_global_io_rule_catalog_->parent != actual_parent_claim ||
       staged_global_io_rule_catalog_->context != staged->context ||
       static_cast<bool>(staged_global_io_rule_catalog_->catalog) != static_cast<bool>(staged->catalog) ||
       (staged->catalog && *staged_global_io_rule_catalog_->catalog != *staged->catalog) ||
       staged_global_io_rule_catalog_->service != staged->service ||
       static_cast<bool>(staged_global_io_rule_catalog_->waiting_for_program) !=
           static_cast<bool>(staged->waiting_for_program) ||
       (staged->waiting_for_program && *staged_global_io_rule_catalog_->waiting_for_program !=
           *staged->waiting_for_program)))
    return Result<void>::Err(global_rule_error("io_global_rule_candidate_conflict"));
  if (staged_global_io_rule_catalog_) return Result<void>::Ok();
  staged_global_io_rule_catalog_ = std::move(staged);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::DiscardStagedGlobalIoRuleCatalog(
    const ConfigurationFenceStatusQueryDTO& held_fence,
    const CellAdmissionClaimDTO& actual_parent_claim) {
  // A host service destructor may tear down transport. Keep the retired
  // candidate alive until all Kernel/admission-owner locks have been released.
  std::unique_ptr<StagedGlobalIoRuleCatalog> discarded;
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  const auto held = cell_admission_owner_->GetCellAdmissionRecoveryReceipt(
      {actual_parent_claim.intent, actual_parent_claim, std::nullopt});
  if (!held.ok()) return Result<void>::Err(held.error());
  if (!ValidateCellAdmissionRecoveryReceipt(held.value()).ok() ||
      held.value().status != CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
      held.value().matched_claim != std::optional(actual_parent_claim) ||
      std::none_of(held.value().current_owner_epochs.begin(), held.value().current_owner_epochs.end(),
                   [&](const auto& owner) {
                     return owner.owner_role == actual_parent_claim.intent.owner_role &&
                            owner.owner_epoch == actual_parent_claim.intent.owner_epoch;
                   }))
    return Result<void>::Err(global_rule_error("io_global_rule_candidate_parent_not_held"));
  std::lock_guard lock(state_mutex_);
  if (!staged_global_io_rule_catalog_) return Result<void>::Ok();
  if (staged_global_io_rule_catalog_->fence != held_fence ||
      staged_global_io_rule_catalog_->parent != actual_parent_claim)
    return Result<void>::Err(global_rule_error("io_global_rule_candidate_stale"));
  const auto retired = retire_staged_global_io_rules_locked();
  if (!retired.ok()) return retired;
  // Candidate-only state acquired no claim, latch or event.
  discarded = std::move(staged_global_io_rule_catalog_);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::ActivateGlobalIoRuleCatalog(
    const IoRuleAppliedContextDTO& actual_applied_context,
    const std::shared_ptr<IIoSignalService>& expected_service) {
  std::shared_ptr<IIoSignalService> prior_global_service;
  std::shared_ptr<IIoSignalService> prior_io_service;
  std::unique_ptr<StagedGlobalIoRuleCatalog> consumed;
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  if (!config_.operation_admission_gate ||
      config_.operation_admission_gate->has_held_configuration_fences())
    return Result<void>::Err(global_rule_error("io_global_rule_apply_fence_held"));
  auto owner = cell_admission_owner_->AcquireManagedProductAdmission();
  if (!owner.ok()) return Result<void>::Err(owner.error());
  if (owner.value().retained_bundle_id() != actual_applied_context.retained_bundle_id) {
    auto error = global_activation_stale_error("managed_owner_bundle");
    error.details.emplace("candidate_bundle", actual_applied_context.retained_bundle_id.value());
    error.details.emplace("observed_bundle", owner.value().retained_bundle_id().value());
    return Result<void>::Err(std::move(error));
  }
  std::lock_guard lock(state_mutex_);
  if (global_io_rule_applied_context_ == actual_applied_context &&
      global_io_rule_service_ == expected_service)
    return Result<void>::Ok();
  if (!staged_global_io_rule_catalog_ ||
      staged_global_io_rule_catalog_->context != actual_applied_context ||
      staged_global_io_rule_catalog_->service != expected_service)
    return Result<void>::Err(global_activation_stale_error("staged_candidate"));
  prior_global_service = global_io_rule_service_;
  prior_io_service = io_signal_service_;
  const auto installed = install_global_io_rule_catalog_locked(*staged_global_io_rule_catalog_);
  if (installed.ok()) consumed = std::move(staged_global_io_rule_catalog_);
  return installed;
}

Result<void> ResourceAddressedExecutionKernel::RestoreGlobalIoRuleCatalog(
    IoRuleAppliedContextDTO actual_applied_context,
    std::shared_ptr<const IoActivationRuleCatalogDTO> catalog,
    std::shared_ptr<IIoSignalService> service,
    std::shared_ptr<const IoRuleWaitingForProgramCatalogDTO> waiting_for_program) {
  const auto valid = validate_global_installation(actual_applied_context, catalog, service,
                                                   waiting_for_program);
  if (!valid.ok()) return valid;
  StagedGlobalIoRuleCatalog candidate{std::move(actual_applied_context), std::move(catalog),
                                     std::move(service), std::nullopt, std::nullopt, {},
                                     std::move(waiting_for_program)};
  std::shared_ptr<IIoSignalService> prior_global_service;
  std::shared_ptr<IIoSignalService> prior_io_service;
  std::unique_ptr<StagedGlobalIoRuleCatalog> consumed;
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  // SetRouteRecoveryPending uses this same admission mutex. Read that predicate
  // before state, then retain the actual Manager-health guard through bounded
  // state/signal commit; no host callback or journal/effect runs under it.
  if (!cell_admission_owner_->route_recovery_pending() ||
      !config_.operation_admission_gate ||
      config_.operation_admission_gate->has_held_configuration_fences())
    return Result<void>::Err(global_rule_error("io_global_rule_restore_not_excluded"));
  auto owner = cell_admission_owner_->AcquireManagedProductAdmission();
  if (!owner.ok()) return Result<void>::Err(owner.error());
  if (owner.value().retained_bundle_id() != candidate.context.retained_bundle_id) {
    auto error = global_activation_stale_error("restore_managed_owner_bundle");
    error.details.emplace("candidate_bundle", candidate.context.retained_bundle_id.value());
    error.details.emplace("observed_bundle", owner.value().retained_bundle_id().value());
    return Result<void>::Err(std::move(error));
  }
  std::lock_guard lock(state_mutex_);
  if (global_io_rule_applied_context_ == candidate.context &&
      global_io_rule_service_ == candidate.service) {
    if (static_cast<bool>(global_io_rule_waiting_for_program_) !=
            static_cast<bool>(candidate.waiting_for_program) ||
        (candidate.waiting_for_program &&
         *global_io_rule_waiting_for_program_ != *candidate.waiting_for_program))
      return Result<void>::Err(global_rule_error("io_global_rule_candidate_conflict"));
    return Result<void>::Ok();
  }
  // A partially failed installation retains its owned registrations in the
  // same staged slot; no hidden handle is orphaned on a returned error.
  if (staged_global_io_rule_catalog_) {
    const auto& retained = *staged_global_io_rule_catalog_;
    if (retained.fence || retained.parent || retained.context != candidate.context ||
        retained.service != candidate.service ||
        static_cast<bool>(retained.catalog) != static_cast<bool>(candidate.catalog) ||
        (candidate.catalog && *retained.catalog != *candidate.catalog) ||
        static_cast<bool>(retained.waiting_for_program) != static_cast<bool>(candidate.waiting_for_program) ||
        (candidate.waiting_for_program && *retained.waiting_for_program != *candidate.waiting_for_program))
      return Result<void>::Err(global_rule_error("io_global_rule_candidate_conflict"));
  } else {
    staged_global_io_rule_catalog_ =
        std::make_unique<StagedGlobalIoRuleCatalog>(std::move(candidate));
  }
  prior_global_service = global_io_rule_service_;
  prior_io_service = io_signal_service_;
  const auto installed = install_global_io_rule_catalog_locked(*staged_global_io_rule_catalog_);
  if (installed.ok()) consumed = std::move(staged_global_io_rule_catalog_);
  return installed;
}

Result<void> ResourceAddressedExecutionKernel::install_global_io_rule_catalog_locked(
    StagedGlobalIoRuleCatalog& staged) {
  const auto& actual_applied_context = staged.context;
  const auto& expected_service = staged.service;
  if (actual_applied_context.owner_epoch != config_.request_owner_epoch ||
      actual_applied_context.retained_bundle_id != stop_retained_bundle_id_ ||
      actual_applied_context.graph_revision != stop_graph_revision_ ||
      (global_io_rule_applied_context_ && actual_applied_context.active_publication_generation <=
          global_io_rule_applied_context_->active_publication_generation))
    return Result<void>::Err(global_activation_stale_error("installed_product"));
  // Preserve the existing service replacement exclusions. This is not an
  // alternate route for replacing a live captured Run or operation rule.
  if (has_active_io_rules_locked() || (program_cycle_claim_run_ && program_cycle_claim_run_->capture))
    return Result<void>::Err(global_rule_error("io_signal_service_rules_active"));
  const auto cleanup = retire_staged_global_io_rules_locked();
  if (!cleanup.ok()) return cleanup;
  const auto& catalog = staged.catalog;
  auto& registrations = staged.registrations;
  const auto rule_count = catalog ? catalog->rules.size() : 0U;
  registrations.reserve(rule_count);
  std::vector<GlobalIoRuleRuntime> rows;
  rows.reserve(rule_count);
  for (std::size_t index = 0; index < rule_count; ++index) {
    const auto& candidate = *catalog;
    const auto& rule = candidate.rules[index];
    // Load starts every rule at its `active_at_start`.
    GlobalIoRuleRuntime row{{rule.policy_id, rule.active_at_start ? IoGlobalRuleState::kClear
                                                                  : IoGlobalRuleState::kInactive},
                            std::nullopt, std::nullopt, std::nullopt, rule.active_at_start, false,
                            false};
    if (!rule.active_at_start || !rule.waiting_for_program_keys.empty()) {
      rows.push_back(std::move(row));
      continue;  // Not watching: no observer, no invented value.
    }
    const auto armed = arm_global_io_rule_locked(rule, candidate.context, expected_service);
    if (!armed.ok()) {
      // Can't watch yet: a real first sample arms this row on the existing
      // ingress schedule; until then it gates its scope.
      row.state = {rule.policy_id, IoGlobalRuleState::kUnknown, std::nullopt, armed.error()};
      row.qualification_error = armed.error();
      rows.push_back(std::move(row));
      continue;
    }
    registrations.push_back(armed.value());
    if (!global_rule_registration_matches(armed.value(), candidate.context, rule.policy_id)) {
      const auto retired = retire_staged_global_io_rules_locked();
      if (!retired.ok()) return retired;
      return Result<void>::Err(global_rule_error("io_global_rule_service_identity_mismatch"));
    }
    const auto evidence = expected_service->ReadRuleEvidence(armed.value());
    if (!evidence.ok()) {
      const auto retired = retire_staged_global_io_rules_locked();
      return retired.ok() ? Result<void>::Err(evidence.error()) : retired;
    }
    row.registration = armed.value();
    row.evidence = evidence.value();
    row.watched = true;
    rows.push_back(std::move(row));
  }
  const auto retired = retire_global_io_rules_locked();
  if (!retired.ok()) {
    const auto cleaned = retire_staged_global_io_rules_locked();
    return cleaned.ok() ? retired : cleaned;
  }
  const auto retired_bb = retire_program_blackboard_source_locked();
  if (!retired_bb.ok()) {
    const auto cleaned = retire_staged_global_io_rules_locked();
    return cleaned.ok() ? retired_bb : cleaned;
  }
  applied_program_blackboard_context_.reset();
  program_blackboard_source_.reset();
  program_blackboard_source_retired_ = false;
  global_io_rule_applied_context_ = actual_applied_context;
  global_io_rule_catalog_ = staged.catalog;
  global_io_rule_waiting_for_program_ = staged.waiting_for_program;
  global_io_rule_service_ = expected_service;
  io_signal_service_ = expected_service;
  global_io_rules_ = std::move(rows);
  std::erase_if(planned_disabled_io_sources_, [&](const auto& source) {
    return !global_io_rule_catalog_ ||
           std::none_of(global_io_rule_catalog_->rules.begin(),
                        global_io_rule_catalog_->rules.end(), [&](const auto& rule) {
                          return std::find(rule.required_signal_sources.begin(),
                                           rule.required_signal_sources.end(),
                                           source) != rule.required_signal_sources.end();
                        });
  });
  registrations.clear();  // Registration ownership moved to the active rows.
  // A condition that already holds at Load trips at once. Nothing moves while
  // a catalog installs, so the trip only publishes `tripped` (and gates).
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    auto& row = global_io_rules_[index];
    if (!row.registration || !row.evidence || !row.evidence->trip) continue;
    row.trip_consumed = true;
    set_global_io_rule_state_locked(index, IoGlobalRuleState::kTripped,
                                    row.evidence->trip->triggering_acquisition);
  }
  execution_event_change_signal().Notify();  // New catalog for event streams.
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::retire_staged_global_io_rules_locked() {
  if (!staged_global_io_rule_catalog_) return Result<void>::Ok();
  auto& staged = *staged_global_io_rule_catalog_;
  while (!staged.registrations.empty()) {
    const auto retired = staged.service->RetireRule(staged.registrations.back());
    if (!retired.ok()) return Result<void>::Err(retired.error());
    staged.registrations.pop_back();
  }
  return Result<void>::Ok();
}

std::shared_ptr<const IoActivationRuleCatalogDTO>
ResourceAddressedExecutionKernel::installed_global_io_rule_catalog() const {
  std::lock_guard lock(state_mutex_);
  return global_io_rule_catalog_;
}

std::optional<IoRuleStateProjectionDTO>
ResourceAddressedExecutionKernel::ReadInterruptRuleStateBaseline() const {
  std::lock_guard lock(state_mutex_);
  const bool projecting =
      global_io_rule_projection_in_flight_ || !global_io_rule_projection_queue_.empty();
  if (projecting && stop_projection_worker_) stop_projection_worker_->PublishGlobalRuleStates();
  if (global_io_rule_waiting_for_program_)
    return IoRuleStateProjectionDTO{IoRuleStateProjectionStatusDTO::kPending, std::nullopt,
                                    nullptr, std::nullopt, global_io_rule_waiting_for_program_};
  if (!global_io_rule_catalog_) {
    if (!global_io_rule_applied_context_) return std::nullopt;
    return IoRuleStateProjectionDTO{
        projecting ? IoRuleStateProjectionStatusDTO::kPending : IoRuleStateProjectionStatusDTO::kReady,
        std::nullopt, nullptr, global_io_rule_applied_context_};
  }
  // A rule waiting for its Program has no state yet; every other row always
  // has one (inactive, clear, tripped, or withdrawn while it can't watch).
  if (projecting || std::any_of(global_io_rule_catalog_->rules.begin(),
                                global_io_rule_catalog_->rules.end(), [](const auto& rule) {
                                  return !rule.waiting_for_program_keys.empty();
                                }))
    return IoRuleStateProjectionDTO{IoRuleStateProjectionStatusDTO::kPending, std::nullopt,
                                    global_io_rule_catalog_};
  IoRuleStateBaselineDTO baseline{global_io_rule_catalog_->context,
                                  global_io_rule_through_sequence_, {}};
  baseline.states.reserve(global_io_rules_.size());
  for (const auto& row : global_io_rules_) baseline.states.push_back(row.state);
  return IoRuleStateProjectionDTO{IoRuleStateProjectionStatusDTO::kReady, std::move(baseline),
                                  global_io_rule_catalog_};
}

Result<void> ResourceAddressedExecutionKernel::retire_global_io_rules_locked() {
  if (!global_io_rule_service_) return Result<void>::Ok();
  for (auto& row : global_io_rules_) {
    if (!row.registration) continue;
    const auto retired = global_io_rule_service_->RetireRule(*row.registration);
    if (!retired.ok()) {
      row.qualification_error = retired.error();
      return Result<void>::Err(retired.error());
    }
    row.evidence = retired.value();
    row.registration.reset();
  }
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::set_global_io_rule_state_locked(
    std::size_t index, IoGlobalRuleState state,
    std::optional<IoRuleAcquisitionKeyDTO> acquisition,
    std::optional<ErrorDTO> unavailable_reason) {
  auto& row = global_io_rules_.at(index);
  if (state != IoGlobalRuleState::kTripped) acquisition.reset();
  if (state != IoGlobalRuleState::kUnknown) unavailable_reason.reset();
  const IoRuleStateDTO next{row.state.rule_id, state, std::move(acquisition),
                            std::move(unavailable_reason)};
  if (row.state == next) return;
  row.state = next;
  if (global_io_rule_projection_queue_.size() == config_.request_replay_capacity) {
    // The bounded forensic queue cannot block Stop. An omitted transition is
    // covered by the next ready baseline after older work drains.
    global_io_rule_projection_gap_ = true;
  } else {
    global_io_rule_projection_queue_.push_back(
        {global_io_rule_catalog_->context, next, clock_.now()});
  }
  stop_projection_worker_->PublishGlobalRuleStates();
  execution_event_change_signal().Notify();  // State key and readiness changed.
}

Result<void> ResourceAddressedExecutionKernel::unwatch_global_io_rule_locked(
    std::size_t index, ErrorDTO reason) {
  auto& row = global_io_rules_.at(index);
  if (row.registration) {
    const auto retired = global_io_rule_service_->RetireRule(*row.registration);
    if (!retired.ok()) return Result<void>::Err(retired.error());
    row.evidence = retired.value();
    row.registration.reset();
  }
  row.trip_consumed = false;
  row.qualification_error = reason;
  set_global_io_rule_state_locked(index, IoGlobalRuleState::kUnknown, std::nullopt,
                                  std::move(reason));
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::ProjectGlobalIoRuleStates() noexcept {
  for (std::size_t work = 0; work < config_.request_replay_capacity; ++work) {
    std::optional<IoRuleStateChangeDTO> change;
    {
      std::lock_guard lock(state_mutex_);
      if (global_io_rule_projection_queue_.empty()) {
        global_io_rule_projection_gap_ = false;
        return;
      }
      global_io_rule_projection_in_flight_ = true;
      change = global_io_rule_projection_queue_.front();
    }
    auto recorded = Result<std::uint64_t>::Err(
        global_rule_error("io_global_rule_projection_exception"));
    try {
      recorded = journal_.RecordInterruptRuleStateChanged(*change);
    } catch (...) {
    }
    std::lock_guard lock(state_mutex_);
    global_io_rule_projection_in_flight_ = false;
    if (!recorded.ok()) {
      global_io_rule_projection_error_ = recorded.error();
      return;  // Existing events cadence retries; never spin on failed storage.
    }
    if (global_io_rule_projection_queue_.empty() ||
        global_io_rule_projection_queue_.front() != *change ||
        recorded.value() <= global_io_rule_through_sequence_ ||
        recorded.value() > kMaximumKernelJsonInteger) {
      global_io_rule_projection_error_ = global_rule_error("io_global_rule_projection_conflict");
      return;
    }
    global_io_rule_through_sequence_ = recorded.value();
    global_io_rule_projection_queue_.erase(global_io_rule_projection_queue_.begin());
    global_io_rule_projection_error_.reset();
    // The baseline may now be ready: event streams re-read it (the journal
    // commit above signalled before this cut moved).
    execution_event_change_signal().Notify();
  }
  std::lock_guard lock(state_mutex_);
  if (!global_io_rule_projection_queue_.empty())
    stop_projection_worker_->PublishGlobalRuleStates();
}

bool ResourceAddressedExecutionKernel::global_io_rule_applies_locked(
    const IoActivationRulePolicyBindingDTO& rule, const RuntimeParticipantBindingDTO& binding) const {
  if (!rule.waiting_for_program_keys.empty()) return false;
  if (std::holds_alternative<IoActivationEverythingScopeDTO>(rule.scope)) return true;
  if (binding.capability_id.value() != "execution.trajectory.execute" || !binding.control_path)
    return false;
  if (std::holds_alternative<IoActivationAllMotionScopeDTO>(rule.scope)) return true;
  const auto& group = std::get<IoActivationExecutionGroupsScopeDTO>(rule.scope);
  const auto& path = *binding.control_path;
  // These are static Applied group subjects, valid also for matching manual
  // motion without a Run. A manual binding need not pretend to be that Program.
  const IoActivationMotionSubjectDTO subject{binding.controllable_component_id,
      binding.command_target_id, path.control_domain_id, path.control_binding_id,
      path.control_binding_revision};
  return std::binary_search(group.subjects.begin(), group.subjects.end(), subject);
}

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::global_io_rule_hold_locked(
    const std::size_t index, const bool output_write) const {
  const auto& row = global_io_rules_[index];
  const auto& rule = global_io_rule_catalog_->rules[index];
  if (!row.active || !rule.waiting_for_program_keys.empty()) return std::nullopt;
  // Before its first successful arm (no reading yet) a rule refuses motion in
  // its scope but not output writes.
  if (!row.watched && output_write) return std::nullopt;
  // Can't watch: any referenced signal without a fresh reading right now.
  for (const auto& input : rule.condition->inputs()) {
    const auto* signal = std::get_if<IoSignalId>(&input.source);
    if (!signal) continue;
    const auto observed = global_io_rule_service_->ReadSignal(*signal);
    if (!observed.ok() || observed.value().availability != IoSignalAvailability::kFresh)
      return rule_holds_error(rule.policy_id, "a signal is not fresh");
  }
  if (key_baseline_pending(row, rule)) return std::nullopt;
  if (!row.registration) return rule_holds_error(rule.policy_id, "it can't watch its signals");
  const auto evidence = global_io_rule_service_->ReadRuleEvidence(*row.registration);
  if (!evidence.ok() || !evidence.value().source_losses.empty() ||
      evidence.value().evaluation_error)
    return rule_holds_error(rule.policy_id, "it can't watch its signals");
  if (rule.on_trip == IoRuleOnTripDTO::kStop && row.state.state == IoGlobalRuleState::kTripped)
    return rule_holds_error(rule.policy_id, "its condition holds");
  return std::nullopt;
}

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::global_io_rule_command_refusal_locked(
    const RuntimeParticipantBindingDTO& binding, const ActiveOperation* operation) const {
  if (!global_io_rule_catalog_) return std::nullopt;
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    const auto& rule = global_io_rule_catalog_->rules[index];
    if (!global_io_rule_applies_locked(rule, binding)) continue;
    if (auto hold = global_io_rule_hold_locked(index, false)) return hold;
    // A key rule watches only the current Run's published keys.
    if (operation && global_io_rules_[index].active && reads_program_keys(rule) &&
        !program_blackboard_source_matches_operation_locked(*operation))
      return rule_holds_error(rule.policy_id, "it can't watch this Run's keys");
  }
  return std::nullopt;
}

std::vector<ErrorDTO> ResourceAddressedExecutionKernel::ReadRuleHolds() const {
  std::lock_guard lock(state_mutex_);
  std::vector<ErrorDTO> holds;
  if (!global_io_rule_catalog_) return holds;
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    auto hold = global_io_rule_hold_locked(index, false);
    if (!hold) continue;
    // The rule's scope, so readiness blocks Execute/Resume only when it meets
    // the Program's components.
    const auto& scope = global_io_rule_catalog_->rules[index].scope;
    if (std::holds_alternative<IoActivationEverythingScopeDTO>(scope)) {
      hold->details["scope"] = "everything";
    } else if (std::holds_alternative<IoActivationAllMotionScopeDTO>(scope)) {
      hold->details["scope"] = "all_motion";
    } else {
      std::set<std::string> ids;
      for (const auto& subject : std::get<IoActivationExecutionGroupsScopeDTO>(scope).subjects)
        ids.insert(subject.controllable_component_id.value());
      std::string joined;
      for (const auto& id : ids) joined += (joined.empty() ? "" : ",") + id;
      hold->details["scope"] = "execution_groups";
      hold->details["component_ids"] = std::move(joined);
    }
    holds.push_back(std::move(*hold));
  }
  return holds;
}

Result<IoRuleRegistrationDTO> ResourceAddressedExecutionKernel::arm_global_io_rule_locked(
    const IoActivationRulePolicyBindingDTO& rule, const IoRuleStateContextDTO& context,
    const std::shared_ptr<IIoSignalService>& service) {
  if (std::any_of(
          rule.required_signal_sources.begin(), rule.required_signal_sources.end(),
          [&](const auto& source) { return planned_disabled_io_sources_.contains(source); }))
    return Result<IoRuleRegistrationDTO>::Err({kErrorMotionInterruptArmFailed,
                                               "Input source is intentionally disabled",
                                               {{"reason", "io_rule_source_disabled"}}});
  if (!rule.waiting_for_program_keys.empty())
    return Result<IoRuleRegistrationDTO>::Err(global_rule_error("io_global_rule_waiting_for_program"));
  auto sources = rule.required_signal_sources;
  if (reads_program_keys(rule)) {
    // The producer retains literal key references, never a caller-selected BB
    // source. Join only this owner's actual admitted occurrence on this service.
    if (!program_blackboard_source_ || program_blackboard_source_retired_ ||
        io_signal_service_ != service ||
        program_blackboard_source_->scope.active_publication_generation !=
            context.active_publication_generation)
      return Result<IoRuleRegistrationDTO>::Err(
          {kErrorMotionInterruptArmFailed, "Blackboard baseline is unavailable",
           {{"reason", "io_rule_baseline_unavailable"}}});
    sources.push_back(program_blackboard_source_->source_id);
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
  }
  // A condition that already holds trips at once (activation, rearm).
  return service->ArmRule(
      {rule.policy_id, IoActivationRuleScopeDTO{context.retained_bundle_id, context.graph_revision},
       rule.condition, MotionInterruptActivationMode::kTripIfAlreadyActive, std::move(sources)});
}

Result<void> ResourceAddressedExecutionKernel::SetGlobalIoSourcesPlannedDisabled(
    const std::shared_ptr<IIoSignalService>& expected_service,
    std::span<const InterruptSourceId> sources, bool disabled) {
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  std::lock_guard lock(state_mutex_);
  if (expected_service != io_signal_service_)
    return Result<void>::Err(global_rule_error("io_global_rule_service_identity_mismatch"));
  if (!disabled) {
    for (const auto& source : sources) planned_disabled_io_sources_.erase(source);
    return Result<void>::Ok();  // Only actual fresh acquisitions may rearm.
  }
  const auto idle = require_idle_for_component_readiness_locked();
  if (!idle.ok()) return idle;
  if (program_cycle_claim_run_)
    return Result<void>::Err(global_rule_error("io_rule_source_disable_program_active"));
  if (!global_io_rule_catalog_) return Result<void>::Ok();
  for (const auto& source : sources)
    if (source.empty()) return Result<void>::Err(global_rule_error("io_rule_source_invalid"));
  const ErrorDTO disabled_reason{kErrorMotionInterruptArmFailed,
                                 "Input source is intentionally disabled",
                                 {{"reason", "io_rule_source_disabled"}}};
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    const auto& rule = global_io_rule_catalog_->rules[index];
    if (std::none_of(rule.required_signal_sources.begin(), rule.required_signal_sources.end(),
                     [&](const auto& source) {
                       return std::find(sources.begin(), sources.end(), source) != sources.end();
                     }))
      continue;
    // Only dependencies of the installed global catalog need an arm fence.
    for (const auto& source : sources)
      if (std::find(rule.required_signal_sources.begin(), rule.required_signal_sources.end(),
                    source) != rule.required_signal_sources.end())
        planned_disabled_io_sources_.insert(source);
    // An active rule whose source is off can't watch and gates its scope.
    if (!global_io_rules_[index].active) continue;
    const auto unwatched = unwatch_global_io_rule_locked(index, disabled_reason);
    if (!unwatched.ok()) return unwatched;
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::arm_pending_global_io_rules_locked() {
  if (!global_io_rule_catalog_) return Result<void>::Ok();
  if (!global_io_rule_service_ || global_io_rules_.size() != global_io_rule_catalog_->rules.size())
    return Result<void>::Err(global_rule_error("io_global_rule_owner_incomplete"));
  const auto& context = global_io_rule_catalog_->context;
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    auto& row = global_io_rules_[index];
    const auto& rule = global_io_rule_catalog_->rules[index];
    if (!row.active || row.registration || !rule.waiting_for_program_keys.empty()) continue;
    const auto armed = arm_global_io_rule_locked(rule, context, global_io_rule_service_);
    if (!armed.ok()) {
      row.qualification_error = armed.error();
      set_global_io_rule_state_locked(index, IoGlobalRuleState::kUnknown, std::nullopt,
                                      armed.error());
      continue;  // Can't watch yet; a later fresh sample rearms it.
    }
    row.registration = armed.value();  // Retain the actual handle even on a bad response.
    if (!global_rule_registration_matches(armed.value(), context, rule.policy_id))
      return Result<void>::Err(global_rule_error("io_global_rule_service_identity_mismatch"));
    row.qualification_error.reset();
    row.trip_consumed = false;
    row.watched = true;
    if (row.state.state == IoGlobalRuleState::kUnknown)
      set_global_io_rule_state_locked(index, IoGlobalRuleState::kClear);
  }
  return Result<void>::Ok();
}

Result<bool> ResourceAddressedExecutionKernel::global_io_rule_supersedes_node_locked(
    const IoMotionRulePolicyBindingDTO& node, const IoRuleEvidenceDTO& evidence,
    const ActiveOperationParticipant& participant) const {
  if (!global_io_rule_catalog_) return Result<bool>::Ok(false);
  auto node_resources = participant.resources;
  std::sort(node_resources.begin(), node_resources.end());
  const auto node_source = rule_source_order(evidence);
  if (!node_source) return Result<bool>::Err(global_rule_error("io_rule_ordering_evidence_missing"));
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    const auto& row = global_io_rules_[index];
    const auto& rule = global_io_rule_catalog_->rules[index];
    if (!row.active || !row.registration || rule.on_trip != IoRuleOnTripDTO::kStop ||
        !global_io_rule_applies_locked(rule, participant.binding.value()))
      continue;
    const auto actual = global_io_rule_service_->ReadRuleEvidence(*row.registration);
    if (!actual.ok()) return Result<bool>::Err(actual.error());
    if (!actual.value().trip) continue;
    if (rule.effective_priority != node.effective_priority) {
      if (rule.effective_priority > node.effective_priority) return Result<bool>::Ok(true);
      continue;
    }
    std::vector<ResourceId> resources;
    for (const auto& registration : registrations_) {
      if (!global_io_rule_applies_locked(rule, registration.binding.value())) continue;
      for (const auto& resource : registration.binding.value().resources)
        resources.push_back(resource.resource_id);
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    if (resources != node_resources) {
      if (resources < node_resources) return Result<bool>::Ok(true);
      continue;
    }
    const auto source = rule_source_order(actual.value());
    if (!source) return Result<bool>::Err(global_rule_error("io_rule_ordering_evidence_missing"));
    if (source_order_less(*source, *node_source) ||
        (!source_order_less(*node_source, *source) &&
         rule.policy_id < node.policy_id)) return Result<bool>::Ok(true);
  }
  return Result<bool>::Ok(false);
}

// Per active rule:
// - can't watch (source loss, evaluation error, setup failed): the operator's
//   Stop on the rule's scope for motion moving in it, then withdraw the key
//   and gate until a fresh rearm;
// - a new trip: `notify` publishes `tripped`; `stop` issues the operator's Stop
//   on the rule's scope (if motion in scope is executing), publishes
//   `tripped`, and gates while the condition holds;
// - condition false after a trip: renew the registration, publish `clear`.
// Paused while the cell is latched. One Stop handoff per call.
Result<void> ResourceAddressedExecutionKernel::process_global_io_rule_evidence_locked(
    StopWorkHandoff& handoff, const OperationId* dispatch_boundary) {
  if (!global_io_rule_catalog_ || cell_latched_locked()) return Result<void>::Ok();
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    if (!global_io_rules_[index].hold_run) continue;
    const auto held = hold_program_run_for_rule_locked(index, handoff);
    if (!held.ok() || handoff.control || handoff.projection) return held;
  }
  const auto pending = arm_pending_global_io_rules_locked();
  if (!pending.ok()) return pending;
  struct CandidateOrder final {
    std::size_t index;
    std::vector<ResourceId> resources;
    std::optional<RuleSourceOrder> source;
  };
  std::vector<CandidateOrder> order;
  order.reserve(global_io_rules_.size());
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    auto& row = global_io_rules_[index];
    const auto& rule = global_io_rule_catalog_->rules[index];
    if (!row.active || !rule.waiting_for_program_keys.empty() || key_baseline_pending(row, rule))
      continue;
    if (row.registration) {
      const auto actual = global_io_rule_service_->ReadRuleEvidence(*row.registration);
      if (!actual.ok()) return Result<void>::Err(actual.error());
      row.evidence = actual.value();
    }
    std::vector<ResourceId> resources;
    for (const auto& registration : registrations_) {
      if (!global_io_rule_applies_locked(rule, registration.binding.value())) continue;
      for (const auto& resource : registration.binding.value().resources)
        resources.push_back(resource.resource_id);
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    order.push_back({index, std::move(resources),
                     row.registration ? rule_source_order(*row.evidence) : std::nullopt});
  }
  std::sort(order.begin(), order.end(), [&](const auto& left, const auto& right) {
    const auto& a = global_io_rule_catalog_->rules[left.index];
    const auto& b = global_io_rule_catalog_->rules[right.index];
    if (a.effective_priority != b.effective_priority) return a.effective_priority > b.effective_priority;
    if (left.resources != right.resources) return left.resources < right.resources;
    if (left.source && right.source) {
      if (source_order_less(*left.source, *right.source)) return true;
      if (source_order_less(*right.source, *left.source)) return false;
    } else if (left.source.has_value() != right.source.has_value()) return left.source.has_value();
    return a.policy_id < b.policy_id;
  });
  // The operator's Stop on the rule's scope for motion moving in it (Full Stop
  // for `everything`). True when a Stop was accepted.
  const auto stop_scope = [&](const CandidateOrder& candidate, bool watch_lost) -> Result<bool> {
    const auto index = candidate.index;
    auto& row = global_io_rules_[index];
    const auto& policy = global_io_rule_catalog_->rules[index];
    GlobalInterruptStopSelection selection{policy.policy_id, row.registration,
        row.registration ? row.evidence : std::optional<IoRuleEvidenceDTO>{}, watch_lost,
        std::holds_alternative<IoActivationEverythingScopeDTO>(policy.scope),
        !watch_lost && policy.after_stop == IoRuleAfterStopDTO::kFailNode,
        policy.effective_priority, {}, candidate.resources};
    bool eligible_effect = false;
    std::vector<StopOperationTargetDTO> operations;
    for (auto& [operation_id, operation] : active_operations_) {
      if (terminal(operation->state.state)) continue;
      bool operation_selected = false;
      for (auto& participant : operation->participants) {
        if (participant.ownership_transferred ||
            !global_io_rule_applies_locked(policy, participant.binding.value())) continue;
        const auto public_row = std::find_if(operation->state.participants.begin(),
            operation->state.participants.end(), [&](const auto& value) {
              return value.participant_id == participant.slot_owner.participant_id;
            });
        if (public_row == operation->state.participants.end() || public_row->terminal_fact) continue;
        const bool boundary = dispatch_boundary && operation_id == *dispatch_boundary &&
                              participant.prepared_participant &&
                              !participant.prepared_dispatch_released;
        auto* resident = io_rule_response_owner_locked(*operation, participant);
        if (resident && !std::all_of(participant.resources.begin(), participant.resources.end(),
              [&](const auto& resource) {
                return std::binary_search(resident->resources.begin(), resident->resources.end(), resource);
              })) resident = nullptr;
        const bool executing = public_row->state == OperationStateKind::kExecuting || boundary;
        if (!resident && !executing) continue;
        if (resident) {
          // Existing stopping-phase arbitration admits strictly higher rules
          // only; an equal or lower one is already contained by that Stop.
          int active_priority = -1;
          for (const auto& winner : resident->interrupt_winners) {
            const auto& scope = std::get<IoOperationRuleScopeDTO>(winner.registration.scope);
            if (scope.operation_id == operation_id &&
                scope.participant_id == participant.slot_owner.participant_id)
              active_priority = std::max(active_priority, winner.priority);
          }
          for (const auto& winner : resident->global_interrupt_winners)
            if (winner.everything || std::any_of(winner.participants.begin(),
                winner.participants.end(), [&](const auto& selected) {
                  return selected.operation_id == operation_id &&
                         selected.participant_id == participant.slot_owner.participant_id;
                })) active_priority = std::max(active_priority, winner.priority);
          if (policy.effective_priority <= active_priority) continue;
        }
        eligible_effect = true;
        operation_selected = true;
        if (participant.payload_kind != OperationParticipantPayloadKind::kMotion ||
            participant.binding.value().capability_id.value() != "execution.trajectory.execute")
          continue;  // Everything retains ordinary gripper/output FullStop.
        if (!participant.prepared_participant || !participant.ordinary_stop_plan ||
            public_row->command_id.empty() ||
            !public_row->command_generation.nonzero())
          return Result<bool>::Err(global_rule_error("io_global_rule_response_membership_stale"));
        const auto entry = ResolvePreparedIoRuleStopEntry(*participant.ordinary_stop_plan,
                                                          policy.stop_entry_stage_id);
        if (!entry.ok()) return Result<bool>::Err(entry.error());
        selection.participants.push_back({operation_id, participant.slot_owner.participant_id,
            participant.preparation_id, participant.prepared_participant->prepared_contract_digest,
            public_row->command_id, public_row->command_generation,
            *participant.ordinary_stop_plan, entry.value()});
      }
      if (operation_selected) operations.push_back({operation_id,
          operation->state.operation_revision,
          operation->state.active_stop_generation.value_or(StopGeneration(0U))});
    }
    if (!eligible_effect || (selection.participants.empty() && !selection.everything))
      return Result<bool>::Ok(false);  // Nothing in scope moves.
    std::sort(selection.participants.begin(), selection.participants.end(),
        [](const auto& a, const auto& b) {
          return std::tie(a.operation_id, a.participant_id) <
                 std::tie(b.operation_id, b.participant_id);
        });
    std::string material = config_.request_owner_epoch.value() + "\nglobal-rule-stop\n" +
                           policy.policy_id.value() + (watch_lost ? "\nwatch-lost" : "\ntrip");
    if (row.registration) material += "\n" + std::to_string(row.registration->registration_id);
    for (const auto& operation : operations)
      material += "\n" + operation.operation_id.value() + "\n" +
                  std::to_string(operation.expected_operation_revision.value());
    // The rule id rides on the Stop as its correlation (run end: stopped by rule).
    const PublicRequestContextDTO request_context{
        RequestId(content_identity::sha256_digest(material)), config_.request_owner_epoch,
        "kernel.global_io_rule", policy.policy_id.value()};
    EmergencyStopProjectedIntentDTO intent = selection.everything
        ? EmergencyStopProjectedIntentDTO{FullStopIntentDTO{request_context, cell_stop_generation_,
              RequestAttributionDTO{RequestTransportDTO::kInternal,
                                    RequestAssuranceDTO::kUnverified, std::nullopt}}}
        : EmergencyStopProjectedIntentDTO{StopIntentDTO{request_context, std::move(operations),
              std::string(watch_lost ? "interrupt_source_unavailable" : "interrupt_fault")}};
    const auto stopped = accept_stop_locked(std::move(intent), handoff, clock_.now(), nullptr,
        nullptr, nullptr, nullptr, {}, &selection);
    if (!stopped.ok()) return Result<bool>::Err(stopped.error());
    for (const auto& selected : selection.participants) {
      auto* owner = find_active_operation_locked(selected.operation_id);
      if (!owner) continue;
      for (auto& participant : owner->state.participants) {
        if (participant.participant_id != selected.participant_id) continue;
        for (auto& outcome : participant.io_rule_outcomes) {
          if (outcome.arbitration_disposition == IoRuleArbitrationDispositionDTO::kActive)
            outcome.arbitration_disposition = IoRuleArbitrationDispositionDTO::kSuperseded;
          outcome.stop_id = stopped.value().stop_id;
          outcome.stop_generation = stopped.value().stop_generation;
        }
      }
    }
    return Result<bool>::Ok(true);
  };
  const auto& context = global_io_rule_catalog_->context;
  for (const auto& candidate_order : order) {
    const auto index = candidate_order.index;
    auto& row = global_io_rules_[index];
    const auto& policy = global_io_rule_catalog_->rules[index];
    if (!row.registration) {
      // Before its first reading a rule only refuses motion (outputs pass, see
      // the hold); once it has watched, a failed rearm is a lost watch.
      if (!row.watched) continue;
      const auto stopped = stop_scope(candidate_order, true);
      if (!stopped.ok()) return Result<void>::Err(stopped.error());
      if (stopped.value()) return Result<void>::Ok();
      continue;
    }
    const auto& evidence = *row.evidence;
    if (!evidence.source_losses.empty() || evidence.evaluation_error) {
      auto reason = evidence.evaluation_error.value_or(ErrorDTO{kErrorMotionInterruptArmFailed,
          "Rule input is unavailable", {{"reason", "io_rule_source_unavailable"}}});
      const auto stopped = stop_scope(candidate_order, true);
      if (!stopped.ok()) return Result<void>::Err(stopped.error());
      const auto unwatched = unwatch_global_io_rule_locked(index, std::move(reason));
      if (!unwatched.ok()) return unwatched;
      if (stopped.value()) return Result<void>::Ok();
      continue;
    }
    const auto snapshot = global_io_rule_service_->ReadRuleConditionSnapshot(*row.registration);
    if (!snapshot.ok()) return Result<void>::Err(snapshot.error());
    const bool released = snapshot.value().value == std::optional(false);
    if (row.trip_consumed && released) {
      // The trip is over: a new registration observes the next edge.
      const auto retired = global_io_rule_service_->RetireRule(*row.registration);
      if (!retired.ok()) return Result<void>::Err(retired.error());
      row.evidence = retired.value();
      row.registration.reset();
      row.trip_consumed = false;
      const auto armed = arm_global_io_rule_locked(policy, context, global_io_rule_service_);
      if (!armed.ok()) {
        row.qualification_error = armed.error();
        set_global_io_rule_state_locked(index, IoGlobalRuleState::kUnknown, std::nullopt,
                                        armed.error());
        continue;
      }
      row.registration = armed.value();
      if (!global_rule_registration_matches(armed.value(), context, policy.policy_id))
        return Result<void>::Err(global_rule_error("io_global_rule_service_identity_mismatch"));
      set_global_io_rule_state_locked(index, IoGlobalRuleState::kClear);
      continue;
    }
    if (!evidence.trip || row.trip_consumed) {
      if (!evidence.trip && released) set_global_io_rule_state_locked(index, IoGlobalRuleState::kClear);
      continue;
    }
    const auto acquisition = evidence.trip->triggering_acquisition;
    row.trip_consumed = true;
    if (policy.on_trip == IoRuleOnTripDTO::kNotify) {
      set_global_io_rule_state_locked(index, IoGlobalRuleState::kTripped, acquisition);
      continue;
    }
    // `hold` is the operator's Stop: the run is held too (below, or
    // on the next step when this one used its Stop handoff).
    row.hold_run = policy.after_stop == IoRuleAfterStopDTO::kHold;
    const auto stopped = stop_scope(candidate_order, false);
    if (!stopped.ok()) {
      row.hold_run = false;
      row.trip_consumed = false;  // Retried on the next pass.
      return Result<void>::Err(stopped.error());
    }
    set_global_io_rule_state_locked(index, IoGlobalRuleState::kTripped, acquisition);
    if (stopped.value()) return Result<void>::Ok();  // One bounded Stop handoff per step.
    if (row.hold_run) {
      const auto held = hold_program_run_for_rule_locked(index, handoff);
      if (!held.ok()) return held;
      if (handoff.control || handoff.projection) return Result<void>::Ok();
    }
  }
  return Result<void>::Ok();
}

// `stop` + `hold` is the operator's Stop. The run is held at once,
// whatever moves, as the Stop button holds it (group `all`), also when nothing
// in the rule's scope moves; Resume and Execute then wait for the rule
// (`rule_holds`). `fail_node` leaves the run going: its next motion in scope
// fails. Without a Program run there is nothing to hold.
Result<void> ResourceAddressedExecutionKernel::hold_program_run_for_rule_locked(
    const std::size_t index, StopWorkHandoff& handoff) {
  auto& row = global_io_rules_[index];
  row.hold_run = false;
  if (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture)
    return Result<void>::Ok();
  const auto& policy = global_io_rule_catalog_->rules[index];
  std::string material = config_.request_owner_epoch.value() + "\nglobal-rule-hold\n" +
                         policy.policy_id.value() + "\n" +
                         program_cycle_claim_run_->program_run_id.value();
  if (row.registration) material += "\n" + std::to_string(row.registration->registration_id);
  const ProgramExecutionSelectionRequestDTO hold{
      PublicRequestContextDTO{RequestId(content_identity::sha256_digest(material)),
                              config_.request_owner_epoch, "kernel.global_io_rule",
                              policy.policy_id.value()},
      program_cycle_claim_run_->program_runner_owner_epoch,
      program_cycle_claim_run_->claim_generation,
      program_cycle_claim_run_->program_run_id,
      {"all"},
      RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                            std::nullopt}};
  const auto held = stop_program_selection_locked(hold, handoff, clock_.now());
  if (!held.ok()) return Result<void>::Err(held.error());
  // The run end names the rule (stopped_by: rule) until Resume releases it.
  const auto fence =
      std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                   [&](const auto& row) { return row.program_run_id == hold.program_run_id; });
  if (fence != program_run_stop_fences_.end()) fence->held_by_rule = policy.policy_id;
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_global_io_rule_stop_locked(
    const GlobalInterruptStopSelection& selection) const {
  if (!selection.everything && selection.participants.empty())
    return Result<void>::Err(global_rule_error("io_global_rule_response_membership_invalid"));
  if (!global_io_rule_catalog_ || !global_io_rule_service_)
    return Result<void>::Err(global_rule_error("io_global_rule_response_stale"));
  const auto policy = std::lower_bound(global_io_rule_catalog_->rules.begin(),
      global_io_rule_catalog_->rules.end(), selection.policy_id,
      [](const auto& rule, const auto& id) { return rule.policy_id < id; });
  if (policy == global_io_rule_catalog_->rules.end() ||
      policy->policy_id != selection.policy_id ||
      (!selection.watch_lost && policy->on_trip != IoRuleOnTripDTO::kStop) ||
      policy->effective_priority != selection.priority ||
      std::holds_alternative<IoActivationEverythingScopeDTO>(policy->scope) != selection.everything ||
      (!selection.watch_lost && policy->after_stop == IoRuleAfterStopDTO::kFailNode) !=
          selection.fail_node)
    return Result<void>::Err(global_rule_error("io_global_rule_response_policy_stale"));
  const auto index = static_cast<std::size_t>(policy - global_io_rule_catalog_->rules.begin());
  const auto& row = global_io_rules_.at(index);
  const bool watching = selection.registration && selection.evidence &&
      row.registration == selection.registration && row.evidence == selection.evidence &&
      global_rule_registration_matches(*selection.registration, global_io_rule_catalog_->context,
                                       selection.policy_id);
  const bool evidence_current = selection.watch_lost
      ? (selection.registration
             ? watching && (!selection.evidence->source_losses.empty() ||
                            selection.evidence->evaluation_error)
             : !row.registration && row.qualification_error && !key_baseline_pending(row, *policy))
      : watching && selection.evidence->trip;
  if (!row.active || !evidence_current)
    return Result<void>::Err(global_rule_error("io_global_rule_response_evidence_stale"));
  const GlobalInterruptStopParticipant* previous = nullptr;
  for (const auto& selected : selection.participants) {
    if (previous && std::tie(previous->operation_id, previous->participant_id) >=
                        std::tie(selected.operation_id, selected.participant_id))
      return Result<void>::Err(global_rule_error("io_global_rule_response_membership_invalid"));
    previous = &selected;
    const auto* operation = find_active_operation_locked(selected.operation_id);
    if (!operation || terminal(operation->state.state))
      return Result<void>::Err(global_rule_error("io_global_rule_response_membership_stale"));
    const auto participant = std::find_if(operation->participants.begin(), operation->participants.end(),
        [&](const auto& value) { return value.slot_owner.participant_id == selected.participant_id; });
    const auto public_row = std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
        [&](const auto& value) { return value.participant_id == selected.participant_id; });
    if (participant == operation->participants.end() || participant->ownership_transferred ||
        !participant->prepared_participant || !participant->ordinary_stop_plan ||
        participant->payload_kind != OperationParticipantPayloadKind::kMotion ||
        participant->binding.value().capability_id.value() != "execution.trajectory.execute" ||
        !global_io_rule_applies_locked(*policy, participant->binding.value()) ||
        participant->preparation_id != selected.preparation_id ||
        participant->prepared_participant->prepared_contract_digest != selected.prepared_contract_digest ||
        *participant->ordinary_stop_plan != selected.plan ||
        public_row == operation->state.participants.end() ||
        public_row->command_id != selected.controller_goal_id ||
        public_row->command_generation != selected.controller_goal_generation)
      return Result<void>::Err(global_rule_error("io_global_rule_response_membership_stale"));
    const auto entry = ResolvePreparedIoRuleStopEntry(selected.plan, policy->stop_entry_stage_id);
    if (!entry.ok() || entry.value() != selected.stop_entry)
      return Result<void>::Err(global_rule_error("io_global_rule_response_plan_stale"));
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::set_global_io_rule_active_locked(
    const std::size_t index, const bool active) {
  auto& row = global_io_rules_.at(index);
  if (row.active == active) return Result<void>::Ok();
  row.active = active;
  row.trip_consumed = false;
  row.watched = false;
  row.qualification_error.reset();
  if (!active) {
    // An inactive rule doesn't watch, trip, stop or gate.
    if (row.registration) {
      const auto retired = global_io_rule_service_->RetireRule(*row.registration);
      if (!retired.ok()) return Result<void>::Err(retired.error());
      row.evidence = retired.value();
      row.registration.reset();
    }
    set_global_io_rule_state_locked(index, IoGlobalRuleState::kInactive);
    return Result<void>::Ok();
  }
  set_global_io_rule_state_locked(index, IoGlobalRuleState::kClear);
  // Arms with trip-if-already-active: a holding condition trips at the next
  // evaluation, which the caller runs at once.
  return arm_pending_global_io_rules_locked();
}

Result<void> ResourceAddressedExecutionKernel::set_rule_active(const InterruptPolicyId& rule_id,
                                                                const bool active) {
  {
    std::lock_guard lock(state_mutex_);
    const auto found = !global_io_rule_catalog_ ? global_io_rules_.end()
        : std::find_if(global_io_rules_.begin(), global_io_rules_.end(),
                       [&](const auto& row) { return row.state.rule_id == rule_id; });
    if (found == global_io_rules_.end())
      return Result<void>::Err({"rule_unknown", "Rule " + rule_id.value() + " is not in the Deployment",
                                {{"rule_id", rule_id.value()}}});
    const auto changed = set_global_io_rule_active_locked(
        static_cast<std::size_t>(found - global_io_rules_.begin()), active);
    if (!changed.ok()) return changed;
  }
  // The trip of a rule activated while its condition holds is evaluated now;
  // an evaluation failure surfaces on the host's next ProcessIoRuleEvidence.
  (void)ProcessIoRuleEvidence();
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::reset_rule_activation() {
  {
    std::lock_guard lock(state_mutex_);
    if (!global_io_rule_catalog_) return Result<void>::Ok();
    for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
      const auto changed = set_global_io_rule_active_locked(
          index, global_io_rule_catalog_->rules[index].active_at_start);
      if (!changed.ok()) return changed;
    }
  }
  (void)ProcessIoRuleEvidence();
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::clear_outside_change(const IoSignalId& output) {
  std::lock_guard lock(state_mutex_);
  if (!io_signal_service_)
    return Result<void>::Err({"io_signal_unknown", "No I/O signals are installed",
                              {{"signal_component_id", output.controllable_component_id.value()},
                               {"signal_name", output.name}}});
  return io_signal_service_->ClearOutsideChange(output);
}

Result<void> ResourceAddressedExecutionKernel::admit_output_write(
    const IoSignalId& output, const bool manual) const {
  std::lock_guard lock(state_mutex_);
  if (auto refusal = cell_latch_refusal_locked()) {
    refusal->details.emplace("signal_component_id", output.controllable_component_id.value());
    refusal->details.emplace("signal_name", output.name);
    return Result<void>::Err(std::move(*refusal));
  }
  // A manual write waits only while motion executes
  // in the cell: an operation (arm motion or gripper command) dispatching,
  // executing or stopping, or a running immediate step. A queued, planning or
  // prepared operation has not moved yet; last write wins otherwise.
  const auto moving = [](const auto& row) {
    const auto state = row.second->state.state;
    return state == OperationStateKind::kDispatching ||
           state == OperationStateKind::kExecuting || state == OperationStateKind::kStopping;
  };
  if (manual && (!immediate_step_engines_.empty() ||
                 std::any_of(active_operations_.begin(), active_operations_.end(), moving)))
    return Result<void>::Err(
        {"io_manual_write_motion_executing", "Motion is executing",
         {{"signal_component_id", output.controllable_component_id.value()},
          {"signal_name", output.name}}});
  if (!global_io_rule_catalog_) return Result<void>::Ok();
  // Only an `everything` rule covers an output.
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    const auto& rule = global_io_rule_catalog_->rules[index];
    if (!std::holds_alternative<IoActivationEverythingScopeDTO>(rule.scope) ||
        !rule.waiting_for_program_keys.empty())
      continue;
    if (auto hold = global_io_rule_hold_locked(index, true)) return Result<void>::Err(*hold);
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
