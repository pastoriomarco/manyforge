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
        (rule.post_latch_disposition != InterruptPostLatchDisposition::kAwaitOperatorAck &&
         rule.post_latch_disposition != InterruptPostLatchDisposition::kAutoClearOnSignalRelease) ||
        (rule.idle_behaviour && *rule.idle_behaviour != IoRuleIdleBehaviourDTO::kLatch &&
         *rule.idle_behaviour != IoRuleIdleBehaviourDTO::kIgnore))
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
    if (!rule.waiting_for_program_keys.empty()) {
      rows.push_back({{rule.policy_id, IoGlobalRuleState::kClear, std::nullopt}, std::nullopt,
                      std::nullopt, std::nullopt, false, false});
      continue;  // No invented value, observer, or externally projected clear.
    }
    const auto armed = arm_global_io_rule_locked(rule, candidate.context, expected_service, false);
    if (!armed.ok()) {
      const auto reason = armed.error().details.find("reason");
      if (reason != armed.error().details.end() && reason->second == "io_rule_baseline_unavailable") {
        // Catalog installation and source qualification are distinct. A real
        // first sample can arm this same row on the existing ingress schedule.
        // This neither manufactures clear-source readiness nor adds admission.
        rows.push_back({{rule.policy_id, IoGlobalRuleState::kClear, std::nullopt}, std::nullopt,
                        std::nullopt, armed.error(), false, false});
        continue;
      }
      const auto retired = retire_staged_global_io_rules_locked();
      return retired.ok() ? Result<void>::Err(armed.error()) : retired;
    }
    registrations.push_back(armed.value());
    const auto& token = armed.value();
    if (token.owner_epoch != candidate.context.owner_epoch ||
        token.catalog_generation != candidate.context.catalog_generation ||
        token.policy_id != rule.policy_id ||
        token.scope != IoRuleScopeDTO{IoActivationRuleScopeDTO{
                           candidate.context.retained_bundle_id, candidate.context.graph_revision}}) {
      const auto retired = retire_staged_global_io_rules_locked();
      if (!retired.ok()) return retired;
      return Result<void>::Err(global_rule_error("io_global_rule_service_identity_mismatch"));
    }
    const auto evidence = expected_service->ReadRuleEvidence(token);
    if (!evidence.ok()) {
      const auto retired = retire_staged_global_io_rules_locked();
      return retired.ok() ? Result<void>::Err(evidence.error()) : retired;
    }
    rows.push_back({{rule.policy_id, IoGlobalRuleState::kClear, std::nullopt}, token,
                    evidence.value(), std::nullopt, false, false});
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
  registrations.clear();  // Registration ownership moved to the active rows.
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    const auto& evidence = global_io_rules_[index].evidence;
    if (evidence && evidence->trip)
      set_global_io_rule_state_locked(index, IoGlobalRuleState::kTripped,
                                      evidence->trip->triggering_acquisition);
  }
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
  if (global_io_rule_waiting_for_program_) {
    if ((global_io_rule_projection_in_flight_ || !global_io_rule_projection_queue_.empty()) &&
        stop_projection_worker_) stop_projection_worker_->PublishGlobalRuleStates();
    return IoRuleStateProjectionDTO{IoRuleStateProjectionStatusDTO::kPending, std::nullopt,
                                    nullptr, std::nullopt, global_io_rule_waiting_for_program_};
  }
  if (!global_io_rule_catalog_) {
    if (!global_io_rule_applied_context_) return std::nullopt;
    const bool pending = global_io_rule_projection_in_flight_ ||
                         !global_io_rule_projection_queue_.empty();
    if (pending && stop_projection_worker_) stop_projection_worker_->PublishGlobalRuleStates();
    return IoRuleStateProjectionDTO{
        pending ? IoRuleStateProjectionStatusDTO::kPending : IoRuleStateProjectionStatusDTO::kReady,
        std::nullopt, nullptr, global_io_rule_applied_context_};
  }
  if (global_io_rule_projection_in_flight_ || !global_io_rule_projection_queue_.empty() ||
      std::any_of(global_io_rules_.begin(), global_io_rules_.end(),
                  [](const auto& row) {
                    // A retained factual latch remains qualified history even
                    // while its lost observer cannot yet be rearmed. This is
                    // not a current source reading or permission to clear.
                    return !row.registration &&
                           !(row.state.state == IoGlobalRuleState::kLatched &&
                             row.latch_registration && row.latched_evidence);
                  })) {
    if (stop_projection_worker_) stop_projection_worker_->PublishGlobalRuleStates();
    return IoRuleStateProjectionDTO{IoRuleStateProjectionStatusDTO::kPending, std::nullopt,
                                    global_io_rule_catalog_};
  }
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
    std::optional<IoRuleAcquisitionKeyDTO> acquisition) {
  auto& row = global_io_rules_.at(index);
  if (state == IoGlobalRuleState::kClear) acquisition.reset();
  const IoRuleStateDTO next{row.state.rule_id, state, std::move(acquisition)};
  if (row.state == next) return;
  if (state == IoGlobalRuleState::kLatched && row.state.state != IoGlobalRuleState::kLatched) {
    row.latch_registration = row.registration;
    row.latched_evidence = row.evidence;
  }
  row.state = next;  // Actual latch/condition truth never waits for journal I/O.
  if (global_io_rule_projection_queue_.size() == config_.request_replay_capacity) {
    // The bounded forensic queue cannot block Stop. An omitted transition is
    // covered by the next actual ready baseline after older work drains; it
    // will never be appended later and overwrite that baseline.
    global_io_rule_projection_gap_ = true;
  } else {
    global_io_rule_projection_queue_.push_back(
        {global_io_rule_catalog_->context, next, clock_.now()});
  }
  stop_projection_worker_->PublishGlobalRuleStates();
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

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::global_io_rule_command_refusal_locked(
    const RuntimeParticipantBindingDTO& binding, const ActiveOperation* operation) const {
  if (!global_io_rule_catalog_) return std::nullopt;
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    const auto& row = global_io_rules_[index];
    const auto& rule = global_io_rule_catalog_->rules[index];
    if (!global_io_rule_applies_locked(rule, binding)) continue;
    if (row.state.state == IoGlobalRuleState::kLatched)
      return ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                      "Global interrupt rule " + rule.policy_id.value() + " is latched",
                      {{"reason", "global_interrupt_latched"},
                       {"rule_id", rule.policy_id.value()}}};
    const bool reads_blackboard = std::any_of(rule.condition->inputs().begin(),
        rule.condition->inputs().end(), [](const auto& input) {
          return std::holds_alternative<IoConditionBlackboardKeyDTO>(input.source);
        });
    const bool predecessor_only = operation && reads_blackboard &&
        !program_blackboard_source_matches_operation_locked(*operation);
    // Contract106 excludes key operands from contract75 first-reading
    // admission. A whole condition can still wait for its BB baseline while
    // its actual signal operands are independently fresh. Do not manufacture
    // a condition value or registration to admit that motion. The current-Run
    // source fence above and actual armed source-loss containment remain.
    const bool waiting_for_key_baseline = reads_blackboard && !row.waiting_for_inactive &&
        row.qualification_error && [&] {
          const auto reason = row.qualification_error->details.find("reason");
          return reason != row.qualification_error->details.end() &&
                 reason->second == "io_rule_baseline_unavailable";
        }();
    // A consumed rule still requires its actual inactive rearm. Other failed
    // qualifications also retain their existing missing-registration refusal.
    bool unavailable = predecessor_only || (!row.registration && !waiting_for_key_baseline);
    for (const auto& input : rule.condition->inputs()) {
      const auto* signal = std::get_if<IoSignalId>(&input.source);
      if (!signal) continue;
      const auto observed = global_io_rule_service_->ReadSignal(*signal);
      unavailable = unavailable || !observed.ok() ||
          observed.value().availability != IoSignalAvailability::kFresh;
    }
    bool actual_loss = row.last_source_loss.has_value();
    if (row.registration) {
      const auto actual = global_io_rule_service_->ReadRuleEvidence(*row.registration);
      unavailable = unavailable || !actual.ok();
      actual_loss = actual_loss || (actual.ok() &&
          (!actual.value().source_losses.empty() || actual.value().evaluation_error));
    }
    unavailable = unavailable || actual_loss;
    if (unavailable) {
      const bool initial = !actual_loss;
      // Contract75 first-reading readiness gates motion, not a genuine typed
      // logical output. Actual Everything latches (including armed loss86)
      // still close its full scope above; normal output lifecycle admission
      // remains independently required by component_command_refusal_locked.
      if (initial && binding.io_output_binding) continue;
      return ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
          "Rule " + rule.policy_id.value() + " waiting for its source",
          {{"reason", initial ? "global_interrupt_source_not_ready"
                              : "global_interrupt_source_unavailable"},
           {"rule_id", rule.policy_id.value()}}};
    }
  }
  return std::nullopt;
}

Result<IoRuleRegistrationDTO> ResourceAddressedExecutionKernel::arm_global_io_rule_locked(
    const IoActivationRulePolicyBindingDTO& rule, const IoRuleStateContextDTO& context,
    const std::shared_ptr<IIoSignalService>& service, bool require_inactive) {
  if (!rule.waiting_for_program_keys.empty())
    return Result<IoRuleRegistrationDTO>::Err(global_rule_error("io_global_rule_waiting_for_program"));
  auto sources = rule.required_signal_sources;
  const bool reads_blackboard = std::any_of(
      rule.condition->inputs().begin(), rule.condition->inputs().end(), [](const auto& input) {
        return std::holds_alternative<IoConditionBlackboardKeyDTO>(input.source);
      });
  if (reads_blackboard) {
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
  return service->ArmRule(
      {rule.policy_id, IoActivationRuleScopeDTO{context.retained_bundle_id, context.graph_revision},
       rule.condition, require_inactive ? MotionInterruptActivationMode::kRequireInactiveAtArm
                                        : MotionInterruptActivationMode::kTripIfAlreadyActive,
       std::move(sources)});
}

Result<void> ResourceAddressedExecutionKernel::arm_pending_global_io_rules_locked() {
  if (!global_io_rule_catalog_) return Result<void>::Ok();
  if (!global_io_rule_service_ || global_io_rules_.size() != global_io_rule_catalog_->rules.size())
    return Result<void>::Err(global_rule_error("io_global_rule_owner_incomplete"));
  for (std::size_t index = 0; index < global_io_rules_.size(); ++index) {
    auto& row = global_io_rules_[index];
    if (row.registration || row.state.state == IoGlobalRuleState::kLatched) continue;
    const auto& rule = global_io_rule_catalog_->rules[index];
    if (!rule.waiting_for_program_keys.empty()) continue;
    const auto& context = global_io_rule_catalog_->context;
    const auto armed = arm_global_io_rule_locked(
        rule, context, global_io_rule_service_, row.waiting_for_inactive);
    if (!armed.ok()) {
      row.qualification_error = armed.error();
      const auto reason = armed.error().details.find("reason");
      if (reason != armed.error().details.end() &&
          (reason->second == "io_rule_baseline_unavailable" ||
           (row.waiting_for_inactive && reason->second == "io_rule_active_at_arm")))
        continue;
      return Result<void>::Err(armed.error());
    }
    row.registration = armed.value(); // Retain actual handle even on a bad adapter response.
    if (armed.value().owner_epoch != context.owner_epoch ||
        armed.value().catalog_generation != context.catalog_generation ||
        armed.value().policy_id != rule.policy_id ||
        armed.value().scope != IoRuleScopeDTO{
            IoActivationRuleScopeDTO{context.retained_bundle_id, context.graph_revision}})
      return Result<void>::Err(global_rule_error("io_global_rule_service_identity_mismatch"));
    row.qualification_error.reset();
    row.response_consumed = false;
    row.response.reset();
    row.waiting_for_inactive = false;
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
    if (!row.registration || !global_io_rule_applies_locked(rule, participant.binding.value()))
      continue;
    const auto actual = global_io_rule_service_->ReadRuleEvidence(*row.registration);
    if (!actual.ok()) return Result<bool>::Err(actual.error());
    if (!actual.value().trip && actual.value().source_losses.empty()) continue;
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

Result<void> ResourceAddressedExecutionKernel::process_global_io_rule_evidence_locked(
    StopWorkHandoff& handoff, const OperationId* dispatch_boundary,
    std::optional<std::size_t> recovering_index) {
  if (!global_io_rule_catalog_) return Result<void>::Ok();
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
    if (recovering_index && index != *recovering_index) continue;
    if (!global_io_rule_catalog_->rules[index].waiting_for_program_keys.empty()) continue;
    auto& row = global_io_rules_[index];
    if (row.registration) {
      const auto actual = global_io_rule_service_->ReadRuleEvidence(*row.registration);
      if (!actual.ok()) return Result<void>::Err(actual.error());
      row.evidence = actual.value();
    }
    std::vector<ResourceId> resources;
    for (const auto& registration : registrations_) {
      if (!global_io_rule_applies_locked(global_io_rule_catalog_->rules[index],
                                         registration.binding.value())) continue;
      for (const auto& resource : registration.binding.value().resources)
        resources.push_back(resource.resource_id);
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    order.push_back({index, std::move(resources),
                     row.evidence ? rule_source_order(*row.evidence) : std::nullopt});
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
  for (const auto& candidate_order : order) {
    const auto index = candidate_order.index;
    auto& row = global_io_rules_[index];
    const auto& policy = global_io_rule_catalog_->rules[index];
    if (row.state.state == IoGlobalRuleState::kLatched && !recovering_index) {
      const auto refreshed = refresh_global_latched_condition_locked(index);
      if (!refreshed.ok()) row.qualification_error = refreshed.error();
      continue;
    }
    if (!row.registration) continue;
    const auto observed = global_io_rule_service_->ReadRuleEvidence(*row.registration);
    if (!observed.ok()) return Result<void>::Err(observed.error());
    row.evidence = observed.value();
    const auto current = global_io_rule_service_->ReadRuleConditionSnapshot(*row.registration);
    if (!current.ok()) return Result<void>::Err(current.error());
    const bool loss = !row.evidence->source_losses.empty();
    if (row.evidence->evaluation_error && !loss && !row.evidence->trip)
      return Result<void>::Err(*row.evidence->evaluation_error);
    const bool matched = row.evidence->trip.has_value();
    // Defer all row/event publication until the response decision has either
    // succeeded or proved no effect is needed. Recovery keeps its old scoped
    // latch in place even while the successor's Stop is being accepted.
    const auto publish_fact = [&](bool latch) {
      if (recovering_index) set_global_io_rule_state_locked(index, IoGlobalRuleState::kClear);
      if (matched) set_global_io_rule_state_locked(index, IoGlobalRuleState::kTripped,
          row.evidence->trip->triggering_acquisition);
      if (latch) {
        row.source_loss_latch = loss;
        if (loss) row.last_source_loss = row.evidence;
        set_global_io_rule_state_locked(index, IoGlobalRuleState::kLatched,
            matched ? row.evidence->trip->triggering_acquisition : std::nullopt);
      }
    };
    if (!matched && !loss) {
      if (recovering_index || current.value().value == std::optional(false))
        set_global_io_rule_state_locked(index, IoGlobalRuleState::kClear);
      continue;
    }
    if (!loss && !recovering_index && row.response_consumed &&
        current.value().value == std::optional(false)) {
      // Retain the factual trip in history before releasing its nonlatched
      // level. A new exact inactive registration can observe the next edge.
      publish_fact(false);
      const auto retired = global_io_rule_service_->RetireRule(*row.registration);
      if (!retired.ok()) return Result<void>::Err(retired.error());
      row.evidence = retired.value();
      row.registration.reset();
      row.waiting_for_inactive = true;
      set_global_io_rule_state_locked(index, IoGlobalRuleState::kClear);
      continue;
    }
    GlobalInterruptStopSelection selection{*row.registration, *row.evidence,
        std::holds_alternative<IoActivationEverythingScopeDTO>(policy.scope),
        policy.effective_priority, {}, candidate_order.resources};
    bool scoped_motion = false;
    bool scoped_stopping = false;
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
        const bool stopping = resident != nullptr;
        const bool executing = public_row->state == OperationStateKind::kExecuting || boundary;
        if (!stopping && !executing) continue;
        scoped_motion = scoped_motion || executing;
        scoped_stopping = scoped_stopping || stopping;
        int active_priority = -1;
        if (resident) {
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
          // Existing stopping-phase arbitration admits strictly higher fault
          // rules only. Equal/lower condition facts are tripped/history, never
          // queued. Actual required-source loss still joins this same Stop as
          // failure evidence; it cannot become desired-trip success.
          if (matched && !loss && policy.effective_priority <= active_priority) continue;
        }
        eligible_effect = true;
        operation_selected = true;
        if (participant.payload_kind != OperationParticipantPayloadKind::kMotion ||
            participant.binding.value().capability_id.value() != "execution.trajectory.execute")
          continue;  // Everything retains ordinary gripper/output FullStop.
        if (!participant.prepared_participant || !participant.ordinary_stop_plan ||
            public_row->command_id.empty() ||
            !public_row->command_generation.nonzero())
          return Result<void>::Err(global_rule_error("io_global_rule_response_membership_stale"));
        const auto entry = ResolvePreparedIoRuleStopEntry(*participant.ordinary_stop_plan,
                                                          policy.stop_entry_stage_id);
        if (!entry.ok()) return Result<void>::Err(entry.error());
        selection.participants.push_back({operation_id, participant.slot_owner.participant_id,
            participant.preparation_id, participant.prepared_participant->prepared_contract_digest,
            public_row->command_id, public_row->command_generation,
            *participant.ordinary_stop_plan, entry.value()});
      }
      if (operation_selected) operations.push_back({operation_id,
          operation->state.operation_revision,
          operation->state.active_stop_generation.value_or(StopGeneration(0U))});
    }
    std::sort(selection.participants.begin(), selection.participants.end(),
        [](const auto& a, const auto& b) {
          return std::tie(a.operation_id, a.participant_id) <
                 std::tie(b.operation_id, b.participant_id);
        });
    if (eligible_effect && (!selection.participants.empty() || selection.everything)) {
      const auto request = content_identity::sha256_digest(config_.request_owner_epoch.value() +
          "\nglobal-rule-stop\n" + std::to_string(row.registration->catalog_generation) + "\n" +
          std::to_string(row.registration->registration_id));
      const PublicRequestContextDTO context{RequestId(request), config_.request_owner_epoch,
                                             "kernel.global_io_rule", std::nullopt};
      const std::string reason = loss ? "interrupt_source_unavailable" : "interrupt_fault";
      EmergencyStopProjectedIntentDTO intent = selection.everything
          ? EmergencyStopProjectedIntentDTO{FullStopIntentDTO{context, cell_stop_generation_,
                RequestAttributionDTO{RequestTransportDTO::kInternal,
                                      RequestAssuranceDTO::kUnverified, std::nullopt}}}
          : EmergencyStopProjectedIntentDTO{StopIntentDTO{context, std::move(operations), reason}};
      const auto stopped = accept_stop_locked(std::move(intent), handoff, clock_.now(), nullptr,
          nullptr, std::nullopt, nullptr, nullptr, {}, &selection);
      if (!stopped.ok()) return Result<void>::Err(stopped.error());
      row.response = stopped.value();
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
      row.response_consumed = true;
      // Trip and real armed-loss latches share this row and its exact scope.
      // A loss latch requires operator recovery and never invents Tripped.
      publish_fact(matched || loss);
      if (loss) row.last_source_loss = row.evidence;
      return Result<void>::Ok();  // One existing bounded Stop handoff per step.
    }
    if (scoped_stopping && !scoped_motion) {
      publish_fact(loss);
      row.response_consumed = true;
      continue;  // No queued action; normal idle is evaluated after Stop.
    }
    if (loss) {
      // Contract86: actual armed loss also latches while idle/no Run. Startup
      // without a first qualified reading never had a registration and never
      // reaches this branch. No Tripped state or Stop identity is invented.
      publish_fact(true);
      row.response_consumed = true;
      continue;
    }
    const bool run_in_flight = program_cycle_claim_run_.has_value();
    const bool idle_latch = policy.idle_behaviour
        ? *policy.idle_behaviour == IoRuleIdleBehaviourDTO::kLatch : run_in_flight;
    publish_fact(idle_latch);
    row.response_consumed = true;
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_global_io_rule_stop_locked(
    const GlobalInterruptStopSelection& selection) const {
  if (!selection.everything && selection.participants.empty())
    return Result<void>::Err(global_rule_error("io_global_rule_response_membership_invalid"));
  if (!global_io_rule_catalog_ || !global_io_rule_service_ ||
      selection.registration.owner_epoch != global_io_rule_catalog_->context.owner_epoch ||
      selection.registration.catalog_generation != global_io_rule_catalog_->context.catalog_generation)
    return Result<void>::Err(global_rule_error("io_global_rule_response_stale"));
  const auto policy = std::lower_bound(global_io_rule_catalog_->rules.begin(),
      global_io_rule_catalog_->rules.end(), selection.registration.policy_id,
      [](const auto& rule, const auto& id) { return rule.policy_id < id; });
  if (policy == global_io_rule_catalog_->rules.end() ||
      policy->policy_id != selection.registration.policy_id ||
      policy->effective_priority != selection.priority ||
      std::holds_alternative<IoActivationEverythingScopeDTO>(policy->scope) != selection.everything)
    return Result<void>::Err(global_rule_error("io_global_rule_response_policy_stale"));
  const auto index = static_cast<std::size_t>(policy - global_io_rule_catalog_->rules.begin());
  const auto& row = global_io_rules_.at(index);
  if (row.registration != selection.registration || !row.evidence ||
      *row.evidence != selection.evidence ||
      (!selection.evidence.trip && selection.evidence.source_losses.empty() &&
       !selection.evidence.evaluation_error))
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

}  // namespace manyforge::core
