#include "resource_addressed_execution_kernel_internal.hpp"

#include <manyforge_core/io_rule_stop_response.hpp>
#include <manyforge_core/time/stuck_drain_wait.hpp>
#include <manyforge_core/types/program_io_condition.hpp>
#include <type_traits>

namespace manyforge::core {

Result<void> ResourceAddressedExecutionKernel::reconfigure_runtime_participants(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  return reconfigure_runtime_participants_impl(std::move(registrations),
                                               std::move(participant_resolver), false);
}

Result<void> ResourceAddressedExecutionKernel::reconfigure_runtime_product(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver,
    std::function<Result<void>()> commit_publication) {
  return reconfigure_runtime_participants_impl(
      std::move(registrations), std::move(participant_resolver), false, nullptr, nullptr,
      nullptr, nullptr, nullptr, commit_publication ? &commit_publication : nullptr);
}

Result<void> ResourceAddressedExecutionKernel::reconfigure_runtime_component_set(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver,
    RetainedBundleId candidate_retained_bundle_id, GraphRevision candidate_graph_revision,
    const ConfigurationFenceId& apply_fence_id, const ConfigurationFenceScopeDTO& scope,
    const std::vector<ControllableComponentId>& retained_components,
    std::function<Result<void>()> commit_publication,
    std::span<const ResolvedLifecycleControlDomainScopeDTO> retained_provider_domains,
    std::span<const ResolvedLifecycleProviderOccurrenceDTO> retained_provider_occurrences,
    const CellAdmissionClaimDTO* actual_parent_claim) {
  if (candidate_retained_bundle_id.empty() || candidate_graph_revision.empty() ||
      apply_fence_id.empty() || !scope.valid() ||
      !std::is_sorted(retained_components.begin(), retained_components.end()) ||
      std::adjacent_find(retained_components.begin(), retained_components.end()) !=
          retained_components.end()) {
    return reject<void>("component-set transition has no exact Apply fence scope",
                        "runtime_component_set_scope_invalid", ErrorCode::kInvalidRequest);
  }
  return reconfigure_runtime_participants_impl(
      std::move(registrations), std::move(participant_resolver), false, &apply_fence_id, &scope,
      &retained_components, &candidate_retained_bundle_id, &candidate_graph_revision,
      commit_publication ? &commit_publication : nullptr, retained_provider_domains,
      retained_provider_occurrences, actual_parent_claim);
}

Result<void>
ResourceAddressedExecutionKernel::stage_current_runtime_configuration_facts_under_apply_fence(
    const ConfigurationFenceScopeDTO& scope) {
  if (!scope.valid() || scope.resources.empty()) {
    return reject<void>("candidate configuration staging scope is invalid",
                        "runtime_configuration_staging_scope_invalid", ErrorCode::kInvalidRequest);
  }
  if (config_.operation_admission_gate == nullptr) {
    return reject<void>("candidate configuration staging has no durable admission gate",
                        "runtime_configuration_staging_gate_missing", ErrorCode::kMissingReference);
  }

  try {
    std::lock_guard submit_lock(submission_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    if (config_.operation_admission_gate->current_fenced_resources(scope.resources) !=
        scope.resources) {
      return reject<void>("candidate configuration staging is not covered by its Apply fence",
                          "runtime_configuration_staging_fence_missing",
                          ErrorCode::kExecutionRejected);
    }

    const auto declared = extract_active_control_configurations(registrations_);
    auto admission = resource_slots_->admission_snapshot({});
    auto staged = admission.active_control_configurations;
    for (const auto& domain : scope.domains) {
      const auto existing = std::lower_bound(
          staged.begin(), staged.end(), domain.control_domain_id,
          [](const auto& row, const auto& domain_id) { return row.control_domain_id < domain_id; });
      if (existing != staged.end() && existing->control_domain_id == domain.control_domain_id) {
        staged.erase(existing);
      }
      // Provider domains have no physical active-configuration fact. Their
      // actual metadata and readiness are published independently.
      if (!domain.has_controller_candidate()) {
        continue;
      }
      const auto candidate = std::lower_bound(
          declared.begin(), declared.end(), domain.control_domain_id,
          [](const auto& row, const auto& domain_id) { return row.control_domain_id < domain_id; });
      if (candidate == declared.end() || candidate->control_domain_id != domain.control_domain_id) {
        return reject<void>("candidate configuration staging has no installed domain fact",
                            "runtime_configuration_staging_domain_missing",
                            ErrorCode::kMissingReference);
      }
      const bool revision_matches =
          std::any_of(registrations_.begin(), registrations_.end(), [&](const auto& registration) {
            const auto& binding = registration.binding.value();
            return binding.control_path.has_value() &&
                   binding.control_path->control_domain_id == domain.control_domain_id &&
                   binding.control_path->control_domain_revision ==
                       *domain.candidate_control_domain_revision;
          });
      if (!revision_matches) {
        return reject<void>("candidate configuration staging crossed its retained domain revision",
                            "runtime_configuration_staging_domain_revision_mismatch",
                            ErrorCode::kInvalidRequest);
      }
      staged.insert(std::lower_bound(staged.begin(), staged.end(), candidate->control_domain_id,
                                     [](const auto& row, const auto& domain_id) {
                                       return row.control_domain_id < domain_id;
                                     }),
                    *candidate);
    }
    return resource_slots_->reconfigure_runtime_inventory(
        registry_.resource_inventory(), std::move(staged),
        std::move(admission.route_published_active_control_configurations));
  } catch (const std::exception& exception) {
    return reject<void>(std::string("candidate configuration staging failed: ") + exception.what(),
                        "runtime_configuration_staging_failed", ErrorCode::kInvariantViolation);
  } catch (...) {
    return reject<void>("candidate configuration staging failed",
                        "runtime_configuration_staging_failed", ErrorCode::kInvariantViolation);
  }
}

Result<void> ResourceAddressedExecutionKernel::reconfigure_runtime_participants_impl(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver,
    const bool retain_unready_configuration_facts, const ConfigurationFenceId* apply_fence_id,
    const ConfigurationFenceScopeDTO* apply_scope,
    const std::vector<ControllableComponentId>* retained_components,
    const RetainedBundleId* candidate_retained_bundle_id,
    const GraphRevision* candidate_graph_revision,
    const std::function<Result<void>()>* commit_publication,
    std::span<const ResolvedLifecycleControlDomainScopeDTO> retained_provider_domains,
    std::span<const ResolvedLifecycleProviderOccurrenceDTO> retained_provider_occurrences,
    const CellAdmissionClaimDTO* actual_parent_claim) {
  if (retained_provider_domains.size() != retained_provider_occurrences.size() ||
      (!retained_provider_domains.empty() &&
       (apply_scope == nullptr || retained_components == nullptr ||
        actual_parent_claim == nullptr || candidate_retained_bundle_id == nullptr ||
        candidate_graph_revision == nullptr))) {
    return reject<void>("retained provider reseal has no exact bounded membership/parent",
                        "runtime_provider_reseal_witness_invalid", ErrorCode::kInvalidRequest);
  }
  if ((candidate_retained_bundle_id == nullptr) !=
      (candidate_graph_revision == nullptr)) {
    return reject<void>("component-set transition has incomplete product identity",
                        "runtime_component_set_product_invalid",
                        ErrorCode::kInvalidRequest);
  }
  if (candidate_retained_bundle_id != nullptr &&
      std::any_of(registrations.begin(), registrations.end(),
                  [&](const auto& registration) {
                    const auto& binding = registration.binding.value();
                    return binding.retained_bundle_id !=
                               *candidate_retained_bundle_id ||
                           binding.graph_revision != *candidate_graph_revision;
                  })) {
    return reject<void>("component-set participants differ from candidate product",
                        "runtime_component_set_product_mismatch",
                        ErrorCode::kInvalidRequest);
  }
  if (qualified_execution_engine_count(registrations) > prequalified_max_live_stop_participants_) {
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInvalidRequest)),
              "candidate runtime participants exceed prequalified Stop-stage isolation",
              "stop_stage_attempt_prequalification_exceeded"));
  }
  std::optional<RuntimeParticipantRegistry> candidate_registry;
  try {
    candidate_registry.emplace(extract_bindings(registrations));
  } catch (const std::exception& exception) {
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInvalidRequest)),
              std::string("candidate runtime participant registry is invalid: ") + exception.what(),
              "runtime_participant_reconfiguration_invalid"));
  }
  std::vector<DomainActiveControlConfigurationDTO> candidate_configurations;
  std::vector<DomainActiveControlConfigurationDTO> declared_candidate_configurations;
  std::vector<DomainActiveControlConfigurationDTO> table_configurations;
  std::vector<DomainActiveControlConfigurationDTO> table_route_publications;
  std::optional<StopStaticFacts> candidate_stop_facts;
  std::set<ControlDomainId> changed_domains;
  try {
    declared_candidate_configurations = extract_active_control_configurations(registrations);
    candidate_configurations = extract_published_active_control_configurations(registrations);
    changed_domains = changed_registration_domains(registrations_, registrations);
    if (apply_scope != nullptr) {
      for (const auto& domain : apply_scope->domains) {
        if (std::holds_alternative<ProviderConfigurationFenceDomainDTO>(domain.payload))
          changed_domains.insert(domain.control_domain_id);
      }
    }
    table_configurations = candidate_configurations;
    // A route published for one domain must not erase a durable predecessor
    // fact adopted for an unchanged domain merely because that other domain
    // is still awaiting fresh Stop-readiness publication.  A first-time
    // unqualified domain has no table fact to retain, while any registration
    // change still removes or replaces its old fact below.
    for (const auto& current :
         resource_slots_->admission_snapshot({}).active_control_configurations) {
      if (changed_domains.contains(current.control_domain_id) ||
          std::any_of(table_configurations.begin(), table_configurations.end(),
                      [&](const auto& candidate) {
                        return candidate.control_domain_id == current.control_domain_id;
                      })) {
        continue;
      }
      table_configurations.push_back(current);
    }
    std::sort(table_configurations.begin(), table_configurations.end(),
              [](const auto& lhs, const auto& rhs) {
                return lhs.control_domain_id < rhs.control_domain_id;
              });
    if (registrations.empty()) {
      candidate_stop_facts.emplace(StopStaticFacts{
          candidate_retained_bundle_id != nullptr
              ? *candidate_retained_bundle_id
              : *stop_retained_bundle_id_,
          candidate_graph_revision != nullptr ? *candidate_graph_revision
                                              : *stop_graph_revision_,
          {}});
    } else {
      candidate_stop_facts.emplace(
          derive_stop_static_facts(registrations, config_, apply_scope == nullptr));
      if (candidate_retained_bundle_id != nullptr &&
          (candidate_stop_facts->retained_bundle_id !=
               *candidate_retained_bundle_id ||
           candidate_stop_facts->graph_revision !=
               *candidate_graph_revision)) {
        throw std::invalid_argument(
            "component-set Stop facts differ from candidate product");
      }
    }
  } catch (const std::exception& exception) {
    return Result<void>::Err(error(
        std::string(to_string(ErrorCode::kInvalidRequest)),
        std::string("candidate runtime control configuration is invalid: ") + exception.what(),
        "runtime_participant_reconfiguration_invalid"));
  }

  std::lock_guard submit_lock(submission_mutex_);
  if (apply_scope != nullptr) {
    if (apply_fence_id == nullptr || config_.operation_admission_gate == nullptr ||
        !config_.operation_admission_gate->exact_fence_is_held(*apply_fence_id,
                                                                *apply_scope) ||
        (!apply_scope->resources.empty() &&
         !config_.operation_admission_gate->exact_fence_covers(
             *apply_fence_id, apply_scope->resources))) {
      return reject<void>("component-set transition is outside its exact Apply fence",
                          "runtime_component_set_fence_missing",
                          ErrorCode::kExecutionRejected);
    }
    const auto check_changed = [&](const auto& source, const auto& other,
                                   const bool predecessor_side) {
      for (const auto& registration : source) {
        if (std::any_of(other.begin(), other.end(), [&](const auto& row) {
              return registration_truth_equal(registration, row);
            })) {
          continue;
        }
        const auto& binding = registration.binding.value();
        if (retained_components != nullptr &&
            std::binary_search(retained_components->begin(), retained_components->end(),
                               binding.controllable_component_id) &&
            std::any_of(other.begin(), other.end(), [&](const auto& row) {
              return registration_controller_equal_after_product_reseal(registration, row);
            })) {
          continue;
        }
        if (binding.io_output_binding) {
          const auto &output = *binding.io_output_binding;
          const auto domain =
              std::find_if(apply_scope->domains.begin(),
                           apply_scope->domains.end(), [&](const auto &row) {
                             return row.control_domain_id ==
                                    output.provider.control_domain_id;
                           });
          const auto *proof =
              domain == apply_scope->domains.end()
                  ? nullptr
                  : std::get_if<ProviderConfigurationFenceDomainDTO>(
                        &domain->payload);
          if (!std::binary_search(
                  apply_scope->controllable_component_ids.begin(),
                  apply_scope->controllable_component_ids.end(),
                  binding.controllable_component_id) ||
              !output.signal.output_resource || !proof)
            return false;
          const auto &revision =
              predecessor_side ? domain->predecessor_control_domain_revision
                               : domain->candidate_control_domain_revision;
          const auto &provider = predecessor_side ? proof->predecessor_provider
                                                  : proof->candidate_provider;
          if (revision !=
                  std::optional(output.provider.control_domain_revision) ||
              !provider ||
              provider->controllable_component_id !=
                  binding.controllable_component_id ||
              provider->provider_instance_id !=
                  output.provider.provider_instance_id ||
              provider->signal_configuration_digest !=
                  output.signal_configuration_digest ||
              !std::binary_search(provider->output_resources.begin(),
                                  provider->output_resources.end(),
                                  *output.signal.output_resource) ||
              !std::binary_search(apply_scope->resources.begin(),
                                  apply_scope->resources.end(),
                                  *output.signal.output_resource))
            return false;
          continue;
        }
        if (!std::binary_search(apply_scope->controllable_component_ids.begin(),
                                apply_scope->controllable_component_ids.end(),
                                binding.controllable_component_id) ||
            !binding.control_path.has_value() ||
            std::any_of(binding.resources.begin(), binding.resources.end(),
                        [&](const auto& row) {
                          return !std::binary_search(apply_scope->resources.begin(),
                                                     apply_scope->resources.end(),
                                                     row.resource_id);
                        })) {
          return false;
        }
        const auto& control_path = *binding.control_path;
        const auto domain_scope = std::find_if(
            apply_scope->domains.begin(), apply_scope->domains.end(), [&](const auto& row) {
              return row.control_domain_id == control_path.control_domain_id;
            });
        const auto binding_scope = std::find_if(
            apply_scope->bindings.begin(), apply_scope->bindings.end(), [&](const auto& row) {
              return row.control_binding_id == control_path.control_binding_id;
            });
        if (domain_scope == apply_scope->domains.end() ||
            binding_scope == apply_scope->bindings.end() ||
            (predecessor_side
                 ? domain_scope->predecessor_control_domain_revision
                 : domain_scope->candidate_control_domain_revision) !=
                std::optional(control_path.control_domain_revision) ||
            (predecessor_side
                 ? binding_scope->predecessor_control_binding_revision
                 : binding_scope->candidate_control_binding_revision) !=
                std::optional(control_path.control_binding_revision)) {
          return false;
        }
      }
      return true;
    };
    if (!check_changed(registrations_, registrations, true) ||
        !check_changed(registrations, registrations_, false)) {
      return reject<void>("component-set transition changes an unfenced participant",
                          "runtime_component_set_scope_incomplete",
                          ErrorCode::kExecutionRejected);
    }
  }
  std::vector<EngineSubscription> added_subscriptions;
  try {
    for (const auto& registration : registrations) {
      const auto already_subscribed = std::any_of(
          engine_subscriptions_.begin(), engine_subscriptions_.end(),
          [&](const EngineSubscription& item) { return item.engine == registration.engine; });
      const auto newly_subscribed = std::any_of(
          added_subscriptions.begin(), added_subscriptions.end(),
          [&](const EngineSubscription& item) { return item.engine == registration.engine; });
      if (already_subscribed || newly_subscribed) {
        continue;
      }
      auto* engine = registration.engine;
      auto ingress = engine_event_ingress_;
      const auto handle = engine->subscribe_events(
          [ingress = std::move(ingress), engine](const KernelEvent& event) {
            auto entered = ingress->Enter();
            if (auto* owner = entered.owner()) owner->on_engine_event(engine, event);
          });
      added_subscriptions.push_back(EngineSubscription{engine, handle});
    }
  } catch (const std::exception& exception) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInternalError)),
              std::string("candidate runtime event subscription failed: ") + exception.what(),
              "runtime_participant_subscription_failed"));
  } catch (...) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInternalError)),
                                   "candidate runtime event subscription failed",
                                   "runtime_participant_subscription_failed"));
  }

  try {
    engine_subscriptions_.reserve(engine_subscriptions_.size() + added_subscriptions.size());
  } catch (...) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInternalError)),
                                   "candidate runtime subscription table reached capacity",
                                   "runtime_participant_subscription_capacity_unavailable"));
  }

  std::vector<IResolvedParticipantExecutionEngine*> control_engines;
  bool control_reconfiguration_busy = false;
  bool retain_control_executor = false;
  bool update_control_executor_in_place = false;
  {
    std::lock_guard state_lock(state_mutex_);
    if (operation_control_reconfiguration_busy_locked()) {
      control_reconfiguration_busy = true;
    } else {
      control_engines = operation_control_engines_locked(registrations);
      const auto current_engines = operation_control_engines_locked(registrations_);
      retain_control_executor =
          (operation_control_executor_ != nullptr || control_engines.empty()) &&
          std::set<IResolvedParticipantExecutionEngine*>(control_engines.begin(),
                                                          control_engines.end()) ==
              std::set<IResolvedParticipantExecutionEngine*>(current_engines.begin(),
                                                              current_engines.end());
      update_control_executor_in_place = !retain_control_executor &&
                                         operation_control_executor_ != nullptr &&
                                         !control_engines.empty();
      operation_control_reconfiguration_in_progress_ = true;
    }
  }
  if (control_reconfiguration_busy) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                   "operation-control work prevents lane reconfiguration",
                                   "operation_control_reconfiguration_busy"));
  }

  std::unique_ptr<BoundedOperationControlExecutor> candidate_control_executor;
  try {
    if (!retain_control_executor && !update_control_executor_in_place &&
        !control_engines.empty()) {
      candidate_control_executor = std::make_unique<BoundedOperationControlExecutor>(
          std::move(control_engines),
          static_cast<IOperationControlEffectCompletionReceiver&>(*this));
      if (!candidate_control_executor->prequalified_ready()) {
        throw std::runtime_error("candidate operation-control lanes are not ready");
      }
    }
  } catch (const std::exception& exception) {
    {
      std::lock_guard state_lock(state_mutex_);
      operation_control_reconfiguration_in_progress_ = false;
    }
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(
        std::string(to_string(ErrorCode::kInternalError)),
        std::string("candidate operation-control lane construction failed: ") + exception.what(),
        "operation_control_reconfiguration_failed"));
  }

  std::optional<ErrorDTO> table_error;
  std::optional<ErrorDTO> control_lane_error;
  std::optional<ErrorDTO> publication_commit_error;
  std::unique_ptr<BoundedOperationControlExecutor> retired_control_executor;
  std::map<ControlDomainId, RuntimeDomainConfigurationReadbackDTO> provider_reanchors;
  {
    std::lock_guard admission_lock(*admission_mutex_);
    // The accounted parent is read before state, in the same admission
    // exclusion as claim release. No owner callback runs under state.
    std::optional<ErrorDTO> provider_parent_error;
    if (actual_parent_claim != nullptr) {
      const auto held = cell_admission_owner_->GetCellAdmissionRecoveryReceipt(
          {actual_parent_claim->intent, *actual_parent_claim, std::nullopt});
      if (!held.ok()) {
        provider_parent_error = held.error();
      } else if (!ValidateCellAdmissionRecoveryReceipt(held.value()).ok() ||
                 held.value().status != CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
                 held.value().matched_claim != std::optional(*actual_parent_claim) ||
                 (actual_parent_claim->intent.claim_kind !=
                      CellAdmissionClaimKindDTO::kManagerApply &&
                  actual_parent_claim->intent.claim_kind !=
                      CellAdmissionClaimKindDTO::kAuthoringMutation) ||
                 std::none_of(held.value().current_owner_epochs.begin(),
                              held.value().current_owner_epochs.end(), [&](const auto& owner) {
                                return owner.owner_role == actual_parent_claim->intent.owner_role &&
                                       owner.owner_epoch == actual_parent_claim->intent.owner_epoch;
                              })) {
        provider_parent_error =
            error(std::string(to_string(ErrorCode::kExecutionRejected)),
                  "retained provider product reseal lost its exact parent claim",
                  "runtime_provider_reseal_parent_not_held");
      }
    }
    std::lock_guard state_lock(state_mutex_);
    const auto stage_provider_reanchors = [&]() -> Result<void> {
      if (provider_parent_error) return Result<void>::Err(*provider_parent_error);
      const bool changes_product = candidate_retained_bundle_id && stop_retained_bundle_id_ &&
                                   stop_graph_revision_ &&
                                   (*candidate_retained_bundle_id != *stop_retained_bundle_id_ ||
                                    *candidate_graph_revision != *stop_graph_revision_);
      if (!changes_product && retained_provider_domains.empty()) return Result<void>::Ok();
      if (!stop_retained_bundle_id_ || !stop_graph_revision_)
        return reject<void>("retained provider reseal has no installed predecessor product",
                            "runtime_provider_reseal_predecessor_missing",
                            ErrorCode::kExecutionRejected);
      if (retained_provider_domains.size() > runtime_route_configuration_readbacks_.size())
        return reject<void>("retained provider witnesses exceed the actual domain inventory",
                            "runtime_provider_reseal_witness_invalid", ErrorCode::kInvalidRequest);
      const auto unchanged_fenced_provider = [&](
          const ResolvedLifecycleControlDomainScopeDTO& domain) {
        if (apply_scope == nullptr || retained_components == nullptr) return false;
        const auto scoped = std::find_if(
            apply_scope->domains.begin(), apply_scope->domains.end(),
            [&](const auto& row) { return row.control_domain_id == domain.control_domain_id; });
        const auto* sides = scoped == apply_scope->domains.end()
                                ? nullptr
                                : std::get_if<ProviderConfigurationFenceDomainDTO>(
                                      &scoped->payload);
        const auto* provider = domain.provider();
        if (sides == nullptr || provider == nullptr ||
            !sides->predecessor_provider || !sides->candidate_provider ||
            *sides->predecessor_provider != *sides->candidate_provider ||
            scoped->predecessor_control_domain_revision !=
                std::optional(domain.control_domain_revision) ||
            scoped->candidate_control_domain_revision !=
                std::optional(domain.control_domain_revision) ||
            !std::binary_search(retained_components->begin(), retained_components->end(),
                                provider->provider.controllable_component_id) ||
            !std::binary_search(apply_scope->controllable_component_ids.begin(),
                                apply_scope->controllable_component_ids.end(),
                                provider->provider.controllable_component_id))
          return false;
        const auto& proof = *sides->predecessor_provider;
        return proof.controllable_component_id == provider->provider.controllable_component_id &&
               proof.provider_instance_id == provider->provider.provider_instance_id &&
               proof.signal_configuration_digest ==
                   provider->provider.signal_configuration_digest &&
               proof.output_resources == domain.resource_ids();
      };
      try {
        for (std::size_t index = 0U; index < retained_provider_domains.size(); ++index) {
          const auto& domain = retained_provider_domains[index];
          const auto& occurrence = retained_provider_occurrences[index];
          const auto* provider = domain.provider();
          const auto found = runtime_route_configuration_readbacks_.find(domain.control_domain_id);
          const auto* current = found == runtime_route_configuration_readbacks_.end()
                                    ? nullptr
                                    : found->second.provider();
          const bool changed = changed_domains.contains(domain.control_domain_id);
          // Apply provider domains are marked changed for fencing even when
          // their exact predecessor and candidate provider are identical.
          const bool retained_fenced_provider =
              changed && unchanged_fenced_provider(domain);
          if (!domain.locally_valid() || !occurrence.locally_valid() || !provider ||
              (index != 0U && !(retained_provider_domains[index - 1U].control_domain_id <
                                domain.control_domain_id)) ||
              (changed && !retained_fenced_provider) ||
              !current || !current->locally_valid() || current->domain != domain ||
              current->provider_occurrence != occurrence ||
              current->retained_bundle_id != *stop_retained_bundle_id_ ||
              current->graph_revision != *stop_graph_revision_) {
            return reject<void>(
                "retained provider witness differs from the exact installed occurrence",
                "runtime_provider_reseal_witness_mismatch", ErrorCode::kExecutionRejected);
          }
          auto next = *current;
          next.retained_bundle_id = *candidate_retained_bundle_id;
          next.graph_revision = *candidate_graph_revision;
          provider_reanchors.emplace(domain.control_domain_id, std::move(next));
        }
        if (changes_product) {
          for (const auto& [id, readback] : runtime_route_configuration_readbacks_) {
            if (!readback.provider() || changed_domains.contains(id)) continue;
            if (!actual_parent_claim || !provider_reanchors.contains(id))
              return reject<void>(
                  "unchanged provider product reseal lacks its exact retained witness",
                  "runtime_provider_reseal_witness_missing", ErrorCode::kExecutionRejected);
          }
        }
      } catch (...) {
        return reject<void>("retained provider product reseal could not be staged before commit",
                            "runtime_provider_reseal_capacity_unavailable",
                            ErrorCode::kInternalError);
      }
      return Result<void>::Ok();
    };
    const auto provider_staging = stage_provider_reanchors();
    table_route_publications.reserve(runtime_route_configuration_readbacks_.size());
    for (const auto& [domain_id, readback] : runtime_route_configuration_readbacks_) {
      if (readback.provider() != nullptr) continue;
      if (changed_domains.contains(domain_id)) {
        const auto candidate =
            std::find_if(candidate_configurations.begin(), candidate_configurations.end(),
                         [&](const auto& item) { return item.control_domain_id == domain_id; });
        if (candidate == candidate_configurations.end() ||
            candidate->active_control_configuration_id !=
                readback.controller().active_configuration.active_control_configuration_id ||
            candidate->active_control_configuration_generation !=
                readback.controller().active_configuration.active_control_configuration_generation) {
          continue;
        }
      }
      table_route_publications.push_back(DomainActiveControlConfigurationDTO{
          domain_id, readback.controller().active_configuration.active_control_configuration_id,
          readback.controller().active_configuration.active_control_configuration_generation});
    }
    // Preparation can create subscriptions and worker lanes outside admission.
    // Verify the delegation again under the lock that also serializes release,
    // before changing any inventory or invoking the publication callback.
    const bool authority_current =
        apply_scope == nullptr ||
        (config_.operation_admission_gate->exact_fence_is_held(
             *apply_fence_id, *apply_scope) &&
         (apply_scope->resources.empty() ||
          config_.operation_admission_gate->exact_fence_covers(
              *apply_fence_id, apply_scope->resources)));
    auto table_result =
        !provider_staging.ok() ? Result<void>::Err(provider_staging.error())
        : authority_current
            ? resource_slots_->reconfigure_runtime_inventory(
                  candidate_registry->resource_inventory(),
                  retain_unready_configuration_facts ? declared_candidate_configurations
                                                     : table_configurations,
                  table_route_publications)
            : reject<void>("component-set transition lost its exact Apply fence",
                           "runtime_component_set_fence_missing", ErrorCode::kExecutionRejected);
    if (!table_result.ok()) {
      table_error = table_result.error();
    } else {
      for (auto readback = runtime_route_configuration_readbacks_.begin();
           readback != runtime_route_configuration_readbacks_.end();) {
        if (!changed_domains.contains(readback->first)) {
          ++readback;
          continue;
        }
        if (readback->second.provider() != nullptr) {
          // A changed provider domain is reinstalled from its actual lifecycle
          // snapshot, never reinterpreted as a controller configuration.
          readback = runtime_route_configuration_readbacks_.erase(readback);
          continue;
        }
        const auto candidate = std::find_if(
            candidate_configurations.begin(), candidate_configurations.end(),
            [&](const auto& item) { return item.control_domain_id == readback->first; });
        if (candidate == candidate_configurations.end() ||
            candidate->active_control_configuration_id !=
                readback->second.controller().active_configuration.active_control_configuration_id ||
            candidate->active_control_configuration_generation !=
                readback->second.controller().active_configuration.active_control_configuration_generation) {
          readback = runtime_route_configuration_readbacks_.erase(readback);
        } else {
          ++readback;
        }
      }
      for (auto required = runtime_route_configuration_publication_required_.begin();
           required != runtime_route_configuration_publication_required_.end();) {
        if (!changed_domains.contains(*required)) {
          ++required;
          continue;
        }
        const auto present = std::any_of(
            declared_candidate_configurations.begin(), declared_candidate_configurations.end(),
            [&](const auto& item) { return item.control_domain_id == *required; });
        required = present ? std::next(required)
                           : runtime_route_configuration_publication_required_.erase(required);
      }
      for (const auto& configuration : declared_candidate_configurations) {
        if (!changed_domains.contains(configuration.control_domain_id)) {
          continue;
        }
        const auto current =
            runtime_route_configuration_readbacks_.find(configuration.control_domain_id);
        if (current == runtime_route_configuration_readbacks_.end()) {
          runtime_route_configuration_publication_required_.insert(configuration.control_domain_id);
        }
      }
      if (commit_publication != nullptr && *commit_publication) {
        try {
          auto committed = (*commit_publication)();
          if (!committed.ok()) {
            const bool under_apply_fence = apply_scope != nullptr;
            publication_commit_error = error(
                std::string(to_string(ErrorCode::kInvariantViolation)),
                std::string("candidate publication commit failed after Kernel inventory update; ") +
                    (under_apply_fence
                         ? "admission remains closed and the Apply fence must remain held: "
                         : "Kernel admission remains closed: ") +
                    committed.error().message,
                under_apply_fence ? "runtime_component_set_publication_commit_failed"
                                  : "runtime_product_publication_commit_failed");
          }
        } catch (const std::exception& exception) {
          const bool under_apply_fence = apply_scope != nullptr;
          publication_commit_error = error(
              std::string(to_string(ErrorCode::kInvariantViolation)),
              std::string("candidate publication commit threw after Kernel inventory update; ") +
                  (under_apply_fence
                       ? "admission remains closed and the Apply fence must remain held: "
                       : "Kernel admission remains closed: ") +
                  exception.what(),
              under_apply_fence ? "runtime_component_set_publication_commit_failed"
                                : "runtime_product_publication_commit_failed");
        } catch (...) {
          const bool under_apply_fence = apply_scope != nullptr;
          publication_commit_error = error(
              std::string(to_string(ErrorCode::kInvariantViolation)),
              std::string("candidate publication commit threw after Kernel inventory update; ") +
                  (under_apply_fence
                       ? "admission remains closed and the Apply fence must remain held"
                       : "Kernel admission remains closed"),
              under_apply_fence ? "runtime_component_set_publication_commit_failed"
                                : "runtime_product_publication_commit_failed");
        }
      }
      if (publication_commit_error.has_value()) {
        motion_admission_closed_ = true;
      } else {
        // Product identity changes with this same successful inventory commit.
        // All node allocations preceded mutation; preserve every actual driver
        // and lifecycle field, including open/closed state and watermarks.
        while (!provider_reanchors.empty()) {
          auto node = provider_reanchors.extract(provider_reanchors.begin());
          runtime_route_configuration_readbacks_.erase(node.key());
          runtime_route_configuration_readbacks_.insert(std::move(node));
        }
        registry_ = std::move(*candidate_registry);
        registrations_ = std::move(registrations);
        // D6: a new product ends any Close of the previous one; a latch of the
        // previous product is now carried (stop_fault_admission_scope_locked).
        if (stop_retained_bundle_id_ != candidate_stop_facts->retained_bundle_id ||
            stop_graph_revision_ != candidate_stop_facts->graph_revision) {
          close_retired_product_ = false;
          force_close_stop_generation_.reset();
        }
        stop_retained_bundle_id_ = std::move(candidate_stop_facts->retained_bundle_id);
        stop_graph_revision_ = std::move(candidate_stop_facts->graph_revision);
        config_.graph_revision = stop_graph_revision_;
        if (candidate_retained_bundle_id != nullptr) {
          config_.empty_runtime_retained_bundle_id = *candidate_retained_bundle_id;
          config_.empty_runtime_graph_revision = *candidate_graph_revision;
        }
        stop_component_scope_ = std::move(candidate_stop_facts->components);
        config_.participant_resolver = std::move(participant_resolver);
        engine_subscriptions_.insert(engine_subscriptions_.end(),
                                     std::make_move_iterator(added_subscriptions.begin()),
                                     std::make_move_iterator(added_subscriptions.end()));
        if (update_control_executor_in_place) {
          const auto update = operation_control_executor_->ReconfigureEngines(control_engines);
          if (update != OperationControlLaneUpdateStatus::kApplied) {
            // Inventory may already have changed. Do not publish a partial lane
            // update as a successful installation or release the Apply fence.
            motion_admission_closed_ = true;
            control_lane_error =
                error(std::string(to_string(ErrorCode::kInvariantViolation)),
                      "incremental operation-control lane update could not be verified",
                      "operation_control_incremental_update_failed");
          }
        } else if (!retain_control_executor) {
          retired_control_executor = std::move(operation_control_executor_);
          operation_control_executor_ = std::move(candidate_control_executor);
        }
      }
    }
    operation_control_reconfiguration_in_progress_ = false;
  }
  if (table_error.has_value()) {
    if (candidate_control_executor != nullptr) {
      candidate_control_executor->CloseIngress();
      WaitForDrain("kernel_runtime.candidate_control_executor", std::chrono::seconds(5),
                   [&](std::chrono::nanoseconds bound) { return candidate_control_executor->DrainFor(bound); });
      candidate_control_executor.reset();
    }
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(std::move(*table_error));
  }
  if (publication_commit_error.has_value()) {
    if (candidate_control_executor != nullptr) {
      candidate_control_executor->CloseIngress();
      WaitForDrain("kernel_runtime.candidate_control_executor", std::chrono::seconds(5),
                   [&](std::chrono::nanoseconds bound) { return candidate_control_executor->DrainFor(bound); });
      candidate_control_executor.reset();
    }
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(std::move(*publication_commit_error));
  }
  if (control_lane_error.has_value()) {
    return Result<void>::Err(std::move(*control_lane_error));
  }
  if (retired_control_executor != nullptr) {
    retired_control_executor->CloseIngress();
    WaitForDrain("kernel_runtime.retired_control_executor", std::chrono::seconds(5),
                 [&](std::chrono::nanoseconds bound) { return retired_control_executor->DrainFor(bound); });
    retired_control_executor.reset();
  }
  return Result<void>::Ok();
}

Result<RuntimeRouteConfigurationReadbackDTO>
ResourceAddressedExecutionKernel::adopt_runtime_route_configuration_predecessor(
    ActiveControlConfigurationDTO configuration,
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  if (!configuration.locally_valid() ||
      configuration.state != ActiveControlConfigurationStateDTO::kReconciled) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "durable route predecessor is not one reconciled configuration",
        "runtime_route_predecessor_invalid", ErrorCode::kInvalidRequest);
  }

  std::vector<DomainActiveControlConfigurationDTO> declared;
  try {
    declared = extract_active_control_configurations(registrations);
  } catch (const std::exception& exception) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        std::string("durable route predecessor registrations are invalid: ") + exception.what(),
        "runtime_route_predecessor_registration_invalid", ErrorCode::kInvalidRequest);
  }
  const auto selected = std::find_if(declared.begin(), declared.end(), [&](const auto& row) {
    return row.control_domain_id == configuration.control_domain_id;
  });
  if (selected == declared.end() ||
      selected->active_control_configuration_id != configuration.active_control_configuration_id ||
      selected->active_control_configuration_generation !=
          configuration.active_control_configuration_generation) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "durable route predecessor differs from its runtime registrations",
        "runtime_route_predecessor_registration_mismatch", ErrorCode::kInvalidRequest);
  }
  bool found_domain = false;
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value() ||
        binding.control_path->control_domain_id != configuration.control_domain_id) {
      continue;
    }
    found_domain = true;
    const auto& path = *binding.control_path;
    const auto route = std::find_if(
        configuration.binding_routes.begin(), configuration.binding_routes.end(),
        [&](const auto& row) { return row.control_binding_id == path.control_binding_id; });
    if (registration.stop_readiness.has_value() ||
        path.provider_runtime_unit_id != configuration.source_runtime_unit_id ||
        path.control_domain_revision != configuration.control_domain_revision ||
        path.controller_topology_id != configuration.controller_topology_id ||
        path.topology_revision != configuration.topology_revision ||
        path.activation_set_id != configuration.activation_set_id ||
        route == configuration.binding_routes.end() ||
        route->control_binding_revision != path.control_binding_revision ||
        route->control_route_id != path.control_route_id) {
      return reject<RuntimeRouteConfigurationReadbackDTO>(
          "durable route predecessor crosses its current runtime binding",
          "runtime_route_predecessor_binding_mismatch", ErrorCode::kInvalidRequest);
    }
  }
  if (!found_domain) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "durable route predecessor has no runtime participant",
        "runtime_route_predecessor_registration_missing", ErrorCode::kMissingReference);
  }

  RuntimeRouteConfigurationReadbackDTO candidate{std::move(configuration), {}, false};
  std::map<ControlDomainId, RuntimeDomainConfigurationReadbackDTO> staged;
  try {
    staged.emplace(candidate.active_configuration.control_domain_id, candidate);
  } catch (...) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "durable route predecessor readback could not be staged",
        "runtime_route_predecessor_capacity_unavailable", ErrorCode::kInternalError);
  }
  auto staged_node = staged.extract(staged.begin());

  auto reconfigured = reconfigure_runtime_participants_impl(
      std::move(registrations), std::move(participant_resolver), true, nullptr);
  if (!reconfigured.ok()) {
    return Result<RuntimeRouteConfigurationReadbackDTO>::Err(reconfigured.error());
  }
  {
    std::lock_guard submit_lock(submission_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    auto publications =
        route_publication_identities(runtime_route_configuration_readbacks_, &candidate);
    auto publication_inventory =
        resource_slots_->replace_route_publication_inventory(std::move(publications));
    if (!publication_inventory.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(publication_inventory.error());
    }
    runtime_route_configuration_readbacks_.erase(candidate.active_configuration.control_domain_id);
    runtime_route_configuration_readbacks_.insert(std::move(staged_node));
    runtime_route_configuration_publication_required_.insert(
        candidate.active_configuration.control_domain_id);
  }
  return Result<RuntimeRouteConfigurationReadbackDTO>::Ok(std::move(candidate));
}

Result<RuntimeRouteConfigurationReadbackDTO>
ResourceAddressedExecutionKernel::publish_runtime_route_configuration(
    ActiveControlConfigurationDTO configuration,
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  if (!configuration.locally_valid()) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route publication has an invalid active configuration",
        "runtime_route_configuration_invalid", ErrorCode::kInvalidRequest);
  }

  std::vector<RouteReadinessPublicationRowDTO> readiness_rows;
  const bool reconciled = configuration.state == ActiveControlConfigurationStateDTO::kReconciled;
  if (reconciled) {
    auto projected = derive_route_readiness_projection(configuration, registrations);
    if (!projected.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(projected.error());
    }
    readiness_rows = std::move(projected.value());
  } else if (configuration.state !=
                 ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled ||
             !registrations.empty() || participant_resolver) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "unreconciled route publication cannot install runtime participants",
        "runtime_route_unreconciled_registration_present", ErrorCode::kInvalidRequest);
  }

  RuntimeRouteConfigurationReadbackDTO candidate{std::move(configuration),
                                                 std::move(readiness_rows), reconciled};
  std::map<ControlDomainId, RuntimeDomainConfigurationReadbackDTO> staged_readback;
  try {
    staged_readback.emplace(candidate.active_configuration.control_domain_id, candidate);
  } catch (...) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route readback could not be staged before publication",
        "runtime_route_readback_capacity_unavailable", ErrorCode::kInternalError);
  }
  auto staged_node = staged_readback.extract(staged_readback.begin());
  if (reconciled) {
    auto installed =
        reconfigure_runtime_participants(std::move(registrations), std::move(participant_resolver));
    if (!installed.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(installed.error());
    }
  }
  std::lock_guard submit_lock(submission_mutex_);
  if (reconciled) {
    auto current_rows =
        derive_route_readiness_projection(candidate.active_configuration, registrations_);
    if (!current_rows.ok() || current_rows.value() != candidate.readiness_rows) {
      return reject<RuntimeRouteConfigurationReadbackDTO>(
          "runtime registration changed before route publication readback",
          "runtime_route_publication_raced", ErrorCode::kInvariantViolation);
    }
  }
  {
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    auto publications =
        route_publication_identities(runtime_route_configuration_readbacks_, &candidate);
    auto publication_inventory =
        resource_slots_->replace_route_publication_inventory(std::move(publications));
    if (!publication_inventory.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(publication_inventory.error());
    }
    runtime_route_configuration_readbacks_.erase(candidate.active_configuration.control_domain_id);
    runtime_route_configuration_readbacks_.insert(std::move(staged_node));
    runtime_route_configuration_publication_required_.erase(
        candidate.active_configuration.control_domain_id);
  }
  return Result<RuntimeRouteConfigurationReadbackDTO>::Ok(std::move(candidate));
}

Result<RuntimeRouteConfigurationReadbackDTO>
ResourceAddressedExecutionKernel::runtime_route_configuration_readback(
    const ControlDomainId& control_domain_id,
    const ActiveControlConfigurationId& active_configuration_id,
    const ActiveControlConfigurationGeneration active_configuration_generation) const {
  if (control_domain_id.empty() || active_configuration_id.empty() ||
      !active_configuration_generation.nonzero()) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route readback query has an invalid identity",
        "runtime_route_readback_query_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  const auto found = runtime_route_configuration_readbacks_.find(control_domain_id);
  if (found == runtime_route_configuration_readbacks_.end() || found->second.provider() != nullptr ||
      found->second.controller().active_configuration.active_control_configuration_id !=
          active_configuration_id ||
      found->second.controller().active_configuration.active_control_configuration_generation !=
          active_configuration_generation) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route configuration readback is absent or stale", "runtime_route_readback_missing",
        ErrorCode::kMissingReference);
  }
  return Result<RuntimeRouteConfigurationReadbackDTO>::Ok(found->second.controller());
}

Result<std::optional<RuntimeRouteConfigurationReadbackDTO>>
ResourceAddressedExecutionKernel::current_runtime_route_configuration(
    const ControlDomainId& control_domain_id) const {
  if (control_domain_id.empty()) {
    return reject<std::optional<RuntimeRouteConfigurationReadbackDTO>>(
        "runtime route current-fact query has an empty domain identity",
        "runtime_route_current_query_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  const auto found = runtime_route_configuration_readbacks_.find(control_domain_id);
  if (found == runtime_route_configuration_readbacks_.end() || found->second.provider() != nullptr) {
    return Result<std::optional<RuntimeRouteConfigurationReadbackDTO>>::Ok(std::nullopt);
  }
  return Result<std::optional<RuntimeRouteConfigurationReadbackDTO>>::Ok(found->second.controller());
}

Result<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>
ResourceAddressedExecutionKernel::publish_runtime_provider_configuration(
    const ComponentCommandReadinessPublicationDTO& authority,
    const ComponentLifecycleCurrentSnapshotDTO& actual_snapshot,
    const std::vector<BrandLifecycleObservationDTO>& actual_driver_observations) {
  using Reply = Result<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>;
  const auto verified = VerifyComponentLifecycleCurrentSnapshot(actual_snapshot);
  if (!verified.ok()) return Reply::Err(verified.error());
  const bool apply = authority.authority == ComponentReadinessAuthorityDTO::kParentApplyFence;
  const auto& apply_scope = authority.parent_apply_scope;
  if (!authority.locally_valid() || apply != apply_scope.has_value() ||
      (apply_scope && !apply_scope->valid()) ||
      authority.controllable_component_id != actual_snapshot.controllable_component_id ||
      authority.component_lifecycle_generation != actual_snapshot.component_lifecycle_generation) {
    return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
        "provider publication differs from the actual lifecycle observation",
        "runtime_provider_publication_invalid", ErrorCode::kInvalidRequest);
  }
  return publish_runtime_provider_configuration_impl(
      authority,
      actual_snapshot.active_publication_receipt.request.candidate_projection_revision
          .retained_bundle_id,
      actual_snapshot.active_publication_receipt.request.candidate_projection_revision
          .graph_revision,
      actual_snapshot.domains, actual_snapshot.provider_occurrences, actual_driver_observations,
      nullptr);
}

Result<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>
ResourceAddressedExecutionKernel::publish_installed_provider_candidate_closed(
    const ConfigurationFenceStatusQueryDTO& held_fence,
    const CellAdmissionClaimDTO& actual_parent_claim,
    const InstalledProviderCandidateFactsDTO& facts) {
  if (!held_fence.valid() || facts.retained_bundle_id.empty() || facts.graph_revision.empty() ||
      facts.controllable_component_id.empty() || !facts.component_lifecycle_generation.nonzero() ||
      facts.domains.empty() || facts.domains.size() > kMaxLifecycleExpectedDomains ||
      facts.domains.size() != facts.provider_occurrences.size())
    return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
        "installed provider candidate facts have an invalid bounded shape",
        "runtime_provider_candidate_facts_invalid", ErrorCode::kInvalidRequest);
  const bool source_matches = std::visit(
      [&](const auto& source) {
        using Source = std::decay_t<decltype(source)>;
        if (source.lifecycle_owner_epoch.empty()) return false;
        if constexpr (std::is_same_v<Source, InstalledApplyChildCandidateSourceDTO>) {
          const ApplyComponentBringUpAuthorityDTO original{source.parent, actual_parent_claim,
                                                           held_fence};
          return original.valid() && source.acceptance.locally_valid() &&
                 source.acceptance.component_lifecycle_generation ==
                     facts.component_lifecycle_generation &&
                 !source.original_request_payload_digest.empty();
        } else {
          const auto& claim = actual_parent_claim.intent;
          const bool manager = claim.claim_kind == CellAdmissionClaimKindDTO::kManagerApply &&
                               claim.owner_role == CellAdmissionOwnerRoleDTO::kManager &&
                               claim.owner_transaction_id == source.apply.apply_id.value();
          const bool authoring =
              claim.claim_kind == CellAdmissionClaimKindDTO::kAuthoringMutation &&
              claim.owner_role == CellAdmissionOwnerRoleDTO::kAuthoringOwner;
          return source.apply.valid() &&
                 source.apply.apply_id == held_fence.owner_transaction.apply_id &&
                 source.apply.apply_generation == held_fence.owner_transaction.apply_generation &&
                 (manager || authoring);
        }
      },
      facts.source);
  if (!source_matches)
    return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
        "installed provider candidate source differs from its original Apply",
        "runtime_provider_candidate_source_mismatch", ErrorCode::kInvalidRequest);
  for (std::size_t index = 0U; index < facts.domains.size(); ++index) {
    const auto& domain = facts.domains[index];
    const auto& occurrence = facts.provider_occurrences[index];
    if (!domain.locally_valid() || !occurrence.locally_valid() ||
        domain.control_domain_id != occurrence.control_domain_id ||
        domain.control_domain_revision != occurrence.control_domain_revision ||
        (index != 0U && !(facts.domains[index - 1U].control_domain_id < domain.control_domain_id)))
      return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
          "installed provider candidate membership is not exact and canonical",
          "runtime_provider_candidate_membership_invalid", ErrorCode::kInvalidRequest);
  }
  const ComponentCommandReadinessPublicationDTO closed{
      ComponentReadinessAuthorityDTO::kParentApplyFence,
      std::nullopt,
      held_fence.configuration_fence_id,
      facts.controllable_component_id,
      facts.component_lifecycle_generation,
      false,
      held_fence.expected_scope};
  return publish_runtime_provider_configuration_impl(
      closed, facts.retained_bundle_id, facts.graph_revision, facts.domains,
      facts.provider_occurrences, {}, &actual_parent_claim);
}

Result<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>
ResourceAddressedExecutionKernel::publish_runtime_provider_configuration_impl(
    const ComponentCommandReadinessPublicationDTO& authority,
    const RetainedBundleId& retained_bundle_id, const GraphRevision& graph_revision,
    std::span<const ResolvedLifecycleControlDomainScopeDTO> domains,
    std::span<const ResolvedLifecycleProviderOccurrenceDTO> provider_occurrences,
    std::span<const BrandLifecycleObservationDTO> actual_driver_observations,
    const CellAdmissionClaimDTO* actual_parent_claim) {
  using Reply = Result<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>;
  const auto& apply_scope = authority.parent_apply_scope;
  // CCLC publishes terminal readiness before committing its aggregate journal
  // state. The verified source proves actual membership/generation; only fresh
  // driver observation plus the held publication authority proves Active.
  const bool qualified_driver = !actual_driver_observations.empty();
  if (authority.command_eligible && !qualified_driver)
    return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
        "command eligibility requires actual driver observations",
        "runtime_provider_driver_observation_not_current", ErrorCode::kInvalidRequest);
  const auto observed_now = clock_.now();
  std::vector<RuntimeProviderDomainConfigurationReadbackDTO> result;
  std::map<ControlDomainId, RuntimeDomainConfigurationReadbackDTO> staged;
  try {
    for (std::size_t index = 0; index < domains.size(); ++index) {
      const auto& domain = domains[index];
      if (domain.provider() == nullptr) continue;
      const auto& provider = *domain.provider();
      const auto& occurrence = provider_occurrences[index];
      if (provider.provider.controllable_component_id != authority.controllable_component_id ||
          !ProviderDomainMatchesOccurrence(domain.control_domain_id, domain.control_domain_revision,
                                           provider.provider, provider.provider_runtime_epoch,
                                           occurrence)) {
        return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
            "provider publication has a conflicting domain occurrence",
            "runtime_provider_occurrence_mismatch", ErrorCode::kInvalidRequest);
      }
      const auto observation = std::find_if(
          actual_driver_observations.begin(), actual_driver_observations.end(),
          [&](const auto& value) { return value.control_domain_id == domain.control_domain_id; });
      if (qualified_driver &&
          (observation == actual_driver_observations.end() ||
           std::count_if(actual_driver_observations.begin(), actual_driver_observations.end(),
                         [&](const auto& value) {
                           return value.control_domain_id == domain.control_domain_id;
                         }) != 1 ||
           !observation->locally_valid() ||
           (authority.lifecycle_transaction_id &&
            observation->lifecycle_transaction_id != *authority.lifecycle_transaction_id) ||
           observation->controllable_component_id != authority.controllable_component_id ||
           observation->control_domain_revision != domain.control_domain_revision ||
           observation->provider_instance_id != occurrence.provider_instance_id ||
           observation->provider_manifest_id != occurrence.provider_manifest_id ||
           observation->provider_manifest_revision != occurrence.provider_manifest_revision ||
           observation->provider_runtime_unit_id != occurrence.provider_runtime_unit_id ||
           observation->provider_runtime_epoch != occurrence.provider_runtime_epoch ||
           (authority.command_eligible &&
            observation->observed_state != ComponentLifecycleStateDTO::kActive) ||
           observation->received_timestamp_ns > observed_now ||
           observed_now >= observation->fresh_until_ns)) {
        return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
            "provider publication lacks an exact fresh actual driver observation",
            "runtime_provider_driver_observation_not_current", ErrorCode::kInvalidRequest);
      }
      RuntimeProviderDomainConfigurationReadbackDTO row{
          retained_bundle_id,
          graph_revision,
          authority.component_lifecycle_generation,
          domain,
          occurrence,
          qualified_driver && authority.command_eligible &&
              observation->observed_state == ComponentLifecycleStateDTO::kActive,
          qualified_driver ? observation->source_sequence : 0U,
          qualified_driver ? observation->observation_generation : 0U};
      if (!row.locally_valid())
        return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
            "provider publication has an invalid current-fact shape",
            "runtime_provider_readback_invalid", ErrorCode::kInvalidRequest);
      staged.emplace(domain.control_domain_id, row);
      result.push_back(std::move(row));
    }
  } catch (...) {
    return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
        "provider current facts could not be staged before publication",
        "runtime_provider_readback_capacity_unavailable", ErrorCode::kInternalError);
  }
  if (result.empty() || (qualified_driver && actual_driver_observations.size() != result.size())) {
    return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
        "provider publication has no genuine provider domain", "runtime_provider_domain_missing",
        ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  if (actual_parent_claim != nullptr) {
    // The host retains the original accounted Apply effect and composition
    // pin. Release/reconciliation use this same admission mutex, so the exact
    // held claim cannot change between this read and the closed map commit.
    // The short admission-owner read ends before state is taken; no owner
    // callback re-enters Kernel or retains an owner mutex over this function.
    const auto held = cell_admission_owner_->GetCellAdmissionRecoveryReceipt(
        {actual_parent_claim->intent, *actual_parent_claim, std::nullopt});
    if (!held.ok()) return Reply::Err(held.error());
    if (!ValidateCellAdmissionRecoveryReceipt(held.value()).ok() ||
        held.value().status != CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
        held.value().matched_claim != std::optional(*actual_parent_claim) ||
        std::none_of(held.value().current_owner_epochs.begin(),
                     held.value().current_owner_epochs.end(), [&](const auto& owner) {
                       return owner.owner_role == actual_parent_claim->intent.owner_role &&
                              owner.owner_epoch == actual_parent_claim->intent.owner_epoch;
                     }))
      return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
          "installed provider candidate lost its exact original Apply claim",
          "runtime_provider_candidate_parent_not_held");
  }
  if (authority.authority == ComponentReadinessAuthorityDTO::kLifecycleClaim) {
    const auto held = require_held_lifecycle_claim_locked(*authority.lifecycle_transaction_id);
    if (!held.ok()) return Reply::Err(held.error());
  } else if (authority.authority == ComponentReadinessAuthorityDTO::kRestartExclusion) {
    if (!cell_admission_owner_->route_recovery_pending() ||
        cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kComponentLifecycle)) {
      return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
          "provider restart publication requires its held reconstruction exclusion",
          "runtime_provider_restart_exclusion_not_held");
    }
  } else {
    if (config_.operation_admission_gate == nullptr ||
        cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kComponentLifecycle) ||
        !config_.operation_admission_gate->exact_fence_is_held(*authority.parent_apply_fence_id,
                                                               *apply_scope) ||
        !std::binary_search(apply_scope->controllable_component_ids.begin(),
                            apply_scope->controllable_component_ids.end(),
                            authority.controllable_component_id)) {
      return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
          "provider Apply publication requires the exact held component fence",
          "runtime_provider_apply_fence_not_held");
    }
    for (const auto& row : result) {
      const auto scoped = std::find_if(
          apply_scope->domains.begin(), apply_scope->domains.end(), [&](const auto& candidate) {
            return candidate.control_domain_id == row.domain.control_domain_id &&
                   candidate.candidate_control_domain_revision ==
                       row.domain.control_domain_revision;
          });
      const auto* fenced_provider =
          scoped == apply_scope->domains.end()
              ? nullptr
              : std::get_if<ProviderConfigurationFenceDomainDTO>(&scoped->payload);
      const auto& actual = *row.domain.provider();
      if (fenced_provider == nullptr || !fenced_provider->candidate_provider ||
          fenced_provider->candidate_provider->controllable_component_id !=
              authority.controllable_component_id ||
          fenced_provider->candidate_provider->provider_instance_id !=
              actual.provider.provider_instance_id ||
          fenced_provider->candidate_provider->signal_configuration_digest !=
              actual.provider.signal_configuration_digest ||
          fenced_provider->candidate_provider->output_resources != actual.output_resources) {
        return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
            "provider Apply publication crosses its fenced domain occurrence",
            "runtime_provider_apply_scope_mismatch");
      }
    }
  }
  if (auto recovery = require_io_output_recovery_lifecycle_publication(authority); !recovery.ok())
    return Reply::Err(recovery.error());
  std::lock_guard state_lock(state_mutex_);
  if (auto idle = require_idle_for_component_readiness_locked(
          authority.authority == ComponentReadinessAuthorityDTO::kLifecycleClaim
              ? &authority.controllable_component_id
              : nullptr);
      !idle.ok())
    return Reply::Err(idle.error());
  for (const auto& row : result) {
    if (!stop_retained_bundle_id_ || !stop_graph_revision_ ||
        row.retained_bundle_id != *stop_retained_bundle_id_ ||
        row.graph_revision != *stop_graph_revision_) {
      return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
          "provider publication does not name the installed retained product",
          "runtime_provider_product_not_current");
    }
    const auto found = runtime_route_configuration_readbacks_.find(row.domain.control_domain_id);
    if (found != runtime_route_configuration_readbacks_.end()) {
      const auto* current = found->second.provider();
      if (current == nullptr ||
          current->component_lifecycle_generation.value() >
              row.component_lifecycle_generation.value() ||
          (current->component_lifecycle_generation == row.component_lifecycle_generation &&
           (current->retained_bundle_id != row.retained_bundle_id ||
            current->graph_revision != row.graph_revision || current->domain != row.domain ||
            current->provider_occurrence != row.provider_occurrence ||
            (current->operation_admission_open != row.operation_admission_open &&
             current->driver_observation_generation != 0U) ||
            current->driver_source_sequence > row.driver_source_sequence ||
            current->driver_observation_generation > row.driver_observation_generation))) {
        return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
            "provider publication conflicts with a newer or different current fact",
            "runtime_provider_readback_not_current");
      }
    }
  }
  const auto commit_now = clock_.now();
  for (const auto& observation : actual_driver_observations) {
    if (observation.received_timestamp_ns > commit_now ||
        commit_now >= observation.fresh_until_ns) {
      return reject<std::vector<RuntimeProviderDomainConfigurationReadbackDTO>>(
          "provider driver observation expired before currentness publication",
          "runtime_provider_driver_observation_not_current");
    }
  }
  // Every allocation precedes mutation. No source callback or provider call
  // occurs under Kernel serialization, and no sample is manufactured here.
  while (!staged.empty()) {
    auto node = staged.extract(staged.begin());
    runtime_route_configuration_readbacks_.erase(node.key());
    runtime_route_configuration_readbacks_.insert(std::move(node));
  }
  return Reply::Ok(std::move(result));
}

Result<std::optional<RuntimeProviderDomainConfigurationReadbackDTO>>
ResourceAddressedExecutionKernel::current_runtime_provider_configuration(
    const ControlDomainId& control_domain_id) const {
  if (control_domain_id.empty()) {
    return reject<std::optional<RuntimeProviderDomainConfigurationReadbackDTO>>(
        "provider current-fact query has an empty domain identity",
        "runtime_provider_current_query_invalid", ErrorCode::kInvalidRequest);
  }
  // All domain-map writers commit under state_mutex_. This read may run
  // under the existing shared admission mutex and must not invert the
  // submission -> admission order by acquiring submission_mutex_.
  std::lock_guard state_lock(state_mutex_);
  const auto found = runtime_route_configuration_readbacks_.find(control_domain_id);
  if (found == runtime_route_configuration_readbacks_.end() || found->second.provider() == nullptr)
    return Result<std::optional<RuntimeProviderDomainConfigurationReadbackDTO>>::Ok(std::nullopt);
  return Result<std::optional<RuntimeProviderDomainConfigurationReadbackDTO>>::Ok(
      *found->second.provider());
}

Result<void> ResourceAddressedExecutionKernel::require_held_lifecycle_claim_locked(
    const LifecycleTransactionId& lifecycle_transaction_id) const {
  const auto claim = cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kComponentLifecycle);
  if (lifecycle_transaction_id.empty() || !claim ||
      claim->intent.owner_transaction_id != lifecycle_transaction_id.value() ||
      claim->intent.owner_role != CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
    return reject<void>("component command readiness requires its exact held CCLC claim",
                        "component_readiness_claim_not_held");
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::require_io_output_recovery_lifecycle_publication(
    const ComponentCommandReadinessPublicationDTO& publication) const {
  {
    std::lock_guard state_lock(state_mutex_);
    if (active_operations_.empty()) return Result<void>::Ok();
  }
  if (publication.authority != ComponentReadinessAuthorityDTO::kLifecycleClaim)
    return Result<void>::Ok();  // Other authorities retain the original idle check.
  if (!publication.lifecycle_transaction_id)
    return reject<void>("Resident output recovery requires its exact lifecycle claim",
                        "io_output_recovery_lifecycle_claim_not_held");
  return cell_admission_owner_->ValidateHeldIoOutputRecoveryLifecycleClaim(
      *publication.lifecycle_transaction_id,
      [&](const auto&, auto held, const auto& readiness, const auto&) {
        std::lock_guard state_lock(state_mutex_);
        return validate_io_output_recovery_claims_locked(publication.controllable_component_id,
                                                         held, readiness);
      });
}

Result<void> ResourceAddressedExecutionKernel::require_idle_for_component_readiness_locked(
    const ControllableComponentId* io_recovery_component) const {
  if (io_recovery_component && !active_operations_.empty()) {
    const auto qualified = qualify_io_output_recovery_operation_locked(*io_recovery_component);
    if (!qualified.ok()) return Result<void>::Err(qualified.error());
    return Result<void>::Ok();
  }
  // The exact lifecycle claim (or the Apply fence during composition) is
  // whole-cell and exclusive, so no command can be admitted meanwhile. Verify
  // that nothing was resident when it was taken.
  if (!active_operations_.empty() || operation_control_reconfiguration_busy_locked()) {
    return reject<void>("component command readiness changed under resident physical work",
                        "component_readiness_effects_not_settled");
  }
  const auto slots = resource_slots_->snapshot();
  if (std::any_of(slots.begin(), slots.end(), [](const auto& slot) {
        return slot.state != ResourceSlotState::kFree || slot.owner.has_value();
      })) {
    return reject<void>("component command readiness has reserved physical resources",
                        "component_readiness_resources_not_settled");
  }
  return Result<void>::Ok();
}

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::component_command_refusal_locked(
    const RuntimeParticipantExecutionRegistration& registration) const {
  const auto& binding = registration.binding.value();
  if (const auto global = global_io_rule_command_refusal_locked(binding)) return global;
  if (const auto hold = component_stream_holds_.find(binding.controllable_component_id);
      hold != component_stream_holds_.end())
    return hold->second;
  // A component without motion of its own (e.g. a pose source) holds new
  // motion of every component; I/O outputs stay governed by their own facts.
  if (!binding.io_output_binding && !cell_stream_holds_.empty())
    return cell_stream_holds_.begin()->second;
  if (binding.io_output_binding) {
    const auto& output = *binding.io_output_binding;
    const auto current =
        runtime_route_configuration_readbacks_.find(output.provider.control_domain_id);
    const auto* row = current == runtime_route_configuration_readbacks_.end()
                          ? nullptr
                          : current->second.provider();
    const auto readiness = component_command_readiness_.find(binding.controllable_component_id);
    if (!row || !row->operation_admission_open || row->driver_source_sequence == 0U ||
        row->driver_observation_generation == 0U ||
        row->retained_bundle_id != binding.retained_bundle_id ||
        row->graph_revision != binding.graph_revision ||
        row->provider_occurrence != output.provider ||
        row->component_lifecycle_generation != output.component_lifecycle_generation ||
        !row->domain.provider() ||
        row->domain.provider()->provider.signal_configuration_digest !=
            output.signal_configuration_digest ||
        !output.signal.output_resource ||
        !std::binary_search(row->domain.provider()->output_resources.begin(),
                            row->domain.provider()->output_resources.end(),
                            *output.signal.output_resource) ||
        readiness == component_command_readiness_.end() || !readiness->second.command_eligible ||
        readiness->second.component_lifecycle_generation != output.component_lifecycle_generation) {
      return error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                   "the output provider is not current and enabled for this exact channel",
                   "runtime_io_output_provider_not_current");
    }
    return std::nullopt;
  }
  if (!binding.control_path.has_value() ||
      !runtime_route_configuration_readbacks_.contains(binding.control_path->control_domain_id)) {
    // No route authority governs this domain, so there is no lifecycle
    // projection to consult; the route checks decide admission as before.
    return std::nullopt;
  }
  const auto found = component_command_readiness_.find(binding.controllable_component_id);
  const auto* readiness =
      registration.stop_readiness.has_value() ? &registration.stop_readiness->value() : nullptr;
  if (found == component_command_readiness_.end() || !found->second.command_eligible ||
      readiness == nullptr ||
      found->second.component_lifecycle_generation != readiness->component_lifecycle_generation) {
    ErrorDTO refusal =
        error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
              "the component is not enabled for commands", "component_command_not_eligible");
    refusal.details.emplace("controllable_component_id", binding.controllable_component_id.value());
    if (found != component_command_readiness_.end() &&
        found->second.component_lifecycle_generation.has_value()) {
      refusal.details.emplace(
          "published_component_lifecycle_generation",
          std::to_string(found->second.component_lifecycle_generation->value()));
    }
    if (readiness != nullptr) {
      refusal.details.emplace("participant_component_lifecycle_generation",
                              std::to_string(readiness->component_lifecycle_generation.value()));
    }
    return refusal;
  }
  return std::nullopt;
}

Result<void> ResourceAddressedExecutionKernel::require_component_routes_current_locked(
    const ControllableComponentId& controllable_component_id, const bool command_eligible) const {
  if (!command_eligible) {
    return Result<void>::Ok();
  }
  return require_installed_component_routes_current_locked(controllable_component_id);
}

Result<void> ResourceAddressedExecutionKernel::require_installed_component_routes_current_locked(
    const ControllableComponentId& controllable_component_id) const {
  bool any_domain = false;
  for (const auto& registration : registrations_) {
    const auto& binding = registration.binding.value();
    if (binding.controllable_component_id != controllable_component_id ||
        !binding.control_path.has_value()) {
      continue;
    }
    any_domain = true;
    const auto& domain = binding.control_path->control_domain_id;
    const auto readback = runtime_route_configuration_readbacks_.find(domain);
    if (!registration.stop_readiness.has_value() ||
        runtime_route_configuration_publication_required_.contains(domain) ||
        readback == runtime_route_configuration_readbacks_.end() || readback->second.provider() != nullptr ||
        readback->second.controller().active_configuration.state !=
            ActiveControlConfigurationStateDTO::kReconciled ||
        !readback->second.controller().operation_admission_open ||
        readback->second.controller().active_configuration.active_control_configuration_id !=
            binding.control_path->active_control_configuration_id ||
        readback->second.controller().active_configuration.active_control_configuration_generation !=
            binding.control_path->active_control_configuration_generation) {
      ErrorDTO refusal = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                               "an enabled component requires every one of its routes to be current",
                               "component_readiness_route_not_current");
      refusal.details.emplace("control_domain_id", domain.value());
      return Result<void>::Err(std::move(refusal));
    }
  }
  for (const auto& [domain_id, readback] : runtime_route_configuration_readbacks_) {
    const auto* provider = readback.provider();
    if (provider == nullptr ||
        provider->domain.provider()->provider.controllable_component_id != controllable_component_id)
      continue;
    any_domain = true;
    if (!provider->operation_admission_open ||
        !stop_retained_bundle_id_ || provider->retained_bundle_id != *stop_retained_bundle_id_ ||
        !stop_graph_revision_ || provider->graph_revision != *stop_graph_revision_) {
      auto refusal = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                           "an enabled component requires its actual provider to be ready",
                           "component_readiness_provider_not_current");
      refusal.details.emplace("control_domain_id", domain_id.value());
      return Result<void>::Err(std::move(refusal));
    }
  }
  if (!any_domain) {
    return reject<void>("an enabled component requires installed route and Stop readiness",
                        "component_readiness_participants_missing", ErrorCode::kMissingReference);
  }
  return Result<void>::Ok();
}

Result<ComponentCommandReadinessDTO>
ResourceAddressedExecutionKernel::close_component_command_eligibility(
    const LifecycleTransactionId& lifecycle_transaction_id,
    const ControllableComponentId& controllable_component_id) {
  if (lifecycle_transaction_id.empty() || controllable_component_id.empty()) {
    return reject<ComponentCommandReadinessDTO>("component readiness closure identity is invalid",
                                                "component_readiness_identity_invalid",
                                                ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  if (auto held = require_held_lifecycle_claim_locked(lifecycle_transaction_id); !held.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(held.error());
  }
  std::lock_guard state_lock(state_mutex_);
  return close_component_command_eligibility_locked(controllable_component_id);
}

Result<ComponentCommandReadinessDTO>
ResourceAddressedExecutionKernel::close_component_command_eligibility_locked(
    const ControllableComponentId& controllable_component_id) {
  try {
    auto [entry, inserted] = component_command_readiness_.try_emplace(
        controllable_component_id,
        ComponentCommandReadinessDTO{controllable_component_id, std::nullopt, false});
    static_cast<void>(inserted);
    entry->second.command_eligible = false;
    return Result<ComponentCommandReadinessDTO>::Ok(entry->second);
  } catch (...) {
    return reject<ComponentCommandReadinessDTO>("component readiness closure could not be recorded",
                                                "component_readiness_capacity_unavailable",
                                                ErrorCode::kInternalError);
  }
}

Result<ComponentCommandReadinessDTO>
ResourceAddressedExecutionKernel::close_component_command_eligibility(
    const CellAdmissionClaimDTO& selected_claim, const SelectedComponentBringUpOfferDTO& offer) {
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  const auto held =
      cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kInterventionOrRecovery);
  if (!held || *held != selected_claim) {
    return reject<ComponentCommandReadinessDTO>(
        "selected component readiness requires its exact full held claim",
        "component_readiness_claim_not_held");
  }
  if (auto valid = validate_selected_bring_up_predecessor_locked(selected_claim.intent, offer);
      !valid.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(valid.error());
  }
  std::lock_guard state_lock(state_mutex_);
  return close_component_command_eligibility_locked(offer.predecessor.controllable_component_id);
}
Result<ComponentCommandReadinessDTO>
ResourceAddressedExecutionKernel::publish_component_command_readiness(
    const ComponentCommandReadinessPublicationDTO& publication,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  if (!publication.locally_valid()) {
    return reject<ComponentCommandReadinessDTO>("component readiness publication is invalid",
                                                "component_readiness_publication_invalid",
                                                ErrorCode::kInvalidRequest);
  }
  const auto& component = publication.controllable_component_id;
  // Each authority is proven positively; absence of another claim is never
  // permission to enable commands.
  const auto authorize_locked = [&]() -> Result<void> {
    if (publication.authority == ComponentReadinessAuthorityDTO::kLifecycleClaim) {
      const auto held = require_held_lifecycle_claim_locked(*publication.lifecycle_transaction_id);
      if (!held.ok()) return held;
      return require_io_output_recovery_lifecycle_publication(publication);
    }
    if (cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kComponentLifecycle)) {
      return reject<void>("composition cannot publish readiness under a lifecycle claim",
                          "component_readiness_lifecycle_claim_held");
    }
    if (publication.authority == ComponentReadinessAuthorityDTO::kRestartExclusion) {
      if (!cell_admission_owner_->route_recovery_pending()) {
        return reject<void>("restart readiness requires the Kernel route-recovery exclusion",
                            "component_readiness_restart_exclusion_not_held");
      }
      return Result<void>::Ok();
    }
    // Parent Apply: this Apply's own held fence (not merely any fence) must
    // cover every resource of the component while readiness changes.
    std::vector<ResourceId> resources;
    for (const auto& registration : registrations_) {
      if (registration.binding.value().controllable_component_id != component) continue;
      for (const auto& resource : registration.binding.value().resources) {
        resources.push_back(resource.resource_id);
      }
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    const bool provider_scope_held = resources.empty() && publication.parent_apply_scope &&
        !publication.parent_apply_scope->domains.empty() &&
        std::binary_search(publication.parent_apply_scope->controllable_component_ids.begin(),
                           publication.parent_apply_scope->controllable_component_ids.end(), component) &&
        config_.operation_admission_gate != nullptr &&
        config_.operation_admission_gate->exact_fence_is_held(*publication.parent_apply_fence_id,
                                                            *publication.parent_apply_scope);
    if (config_.operation_admission_gate == nullptr ||
        (resources.empty() ? !provider_scope_held :
         !config_.operation_admission_gate->exact_fence_covers(*publication.parent_apply_fence_id,
                                                              resources))) {
      return reject<void>("Apply readiness requires the held parent fence over the component",
                          "component_readiness_apply_fence_not_held");
    }
    return Result<void>::Ok();
  };

  // Close first and re-seal outside the short serialization boundary. Any
  // failure below leaves the component command-closed.
  std::vector<RuntimeParticipantExecutionRegistration> rebound;
  bool generation_changed = false;
  {
    std::lock_guard submit_lock(submission_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    if (auto authorized = authorize_locked(); !authorized.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(authorized.error());
    }
    std::lock_guard state_lock(state_mutex_);
    // Composition reads durable facts before publishing. A newer lifecycle
    // result published meanwhile must never be overtaken by that read.
    if (const auto current = component_command_readiness_.find(component);
        publication.authority != ComponentReadinessAuthorityDTO::kLifecycleClaim &&
        current != component_command_readiness_.end() &&
        current->second.component_lifecycle_generation.has_value() &&
        current->second.component_lifecycle_generation->value() >
            publication.component_lifecycle_generation.value()) {
      return reject<ComponentCommandReadinessDTO>(
          "composition readiness is older than the published component generation",
          "component_readiness_generation_stale", ErrorCode::kInvalidStateTransition);
    }
    if (auto idle = require_idle_for_component_readiness_locked(
            publication.authority == ComponentReadinessAuthorityDTO::kLifecycleClaim ? &component
                                                                                     : nullptr);
        !idle.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(idle.error());
    }
    if (auto routes = require_component_routes_current_locked(component,
                                                              publication.command_eligible);
        !routes.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(routes.error());
    }
    try {
      auto [entry, inserted] = component_command_readiness_.try_emplace(
          component, ComponentCommandReadinessDTO{component, std::nullopt, false});
      static_cast<void>(inserted);
      entry->second.command_eligible = false;
      rebound = registrations_;
    } catch (...) {
      return reject<ComponentCommandReadinessDTO>(
          "component readiness publication could not be staged",
          "component_readiness_capacity_unavailable", ErrorCode::kInternalError);
    }
  }
  for (auto& registration : rebound) {
    if (registration.binding.value().controllable_component_id == component &&
        registration.binding.value().io_output_binding &&
        registration.binding.value().io_output_binding->component_lifecycle_generation !=
            publication.component_lifecycle_generation) {
      auto value = registration.binding.value();
      value.io_output_binding->component_lifecycle_generation = publication.component_lifecycle_generation;
      auto refreshed = verify_runtime_participant_binding(std::move(value));
      if (!refreshed.ok()) return Result<ComponentCommandReadinessDTO>::Err(refreshed.error());
      registration.binding = std::move(refreshed.value());
      generation_changed = true;
    }
    if (registration.binding.value().controllable_component_id != component ||
        !registration.stop_readiness.has_value()) {
      continue;
    }
    if (registration.stop_readiness->value().component_lifecycle_generation ==
        publication.component_lifecycle_generation) {
      continue;
    }
    auto refreshed = rebind_runtime_participant_stop_readiness_generation(
        *registration.stop_readiness, publication.component_lifecycle_generation);
    if (!refreshed.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(refreshed.error());
    }
    registration.stop_readiness = std::move(refreshed.value());
    generation_changed = true;
  }
  if (generation_changed) {
    if (!participant_resolver && config_.participant_resolver) {
      return reject<ComponentCommandReadinessDTO>(
          "a re-sealed component requires its rebuilt participant resolver",
          "component_readiness_resolver_missing", ErrorCode::kMissingReference);
    }
    // Reuses the same engines and route configurations: an unchanged
    // configuration keeps its route readback and generation.
    auto reconfigured = reconfigure_runtime_participants_impl(
        std::move(rebound), std::move(participant_resolver), false, nullptr);
    if (!reconfigured.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(reconfigured.error());
    }
  }

  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  if (auto authorized = authorize_locked(); !authorized.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(authorized.error());
  }
  std::lock_guard state_lock(state_mutex_);
  if (auto idle = require_idle_for_component_readiness_locked(
          publication.authority == ComponentReadinessAuthorityDTO::kLifecycleClaim ? &component
                                                                                   : nullptr);
      !idle.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(idle.error());
  }
  return commit_component_command_readiness_locked(
      component, publication.component_lifecycle_generation, publication.command_eligible);
}

Result<ComponentCommandReadinessDTO>
ResourceAddressedExecutionKernel::commit_component_command_readiness_locked(
    const ControllableComponentId& component, ComponentLifecycleGeneration generation,
    bool command_eligible) {
  std::set<ControlDomainId> component_domains;
  for (const auto& registration : registrations_) {
    const auto& binding = registration.binding.value();
    if (binding.controllable_component_id == component && binding.io_output_binding &&
        binding.io_output_binding->component_lifecycle_generation != generation)
      return reject<ComponentCommandReadinessDTO>("output binding changed during readiness publication",
          "component_readiness_generation_changed", ErrorCode::kInvalidStateTransition);
    if (binding.controllable_component_id != component || !binding.control_path.has_value()) {
      continue;
    }
    if (registration.stop_readiness.has_value() &&
        registration.stop_readiness->value().component_lifecycle_generation !=
            generation) {
      return reject<ComponentCommandReadinessDTO>(
          "component participants changed during readiness publication",
          "component_readiness_generation_raced", ErrorCode::kInvariantViolation);
    }
    component_domains.insert(binding.control_path->control_domain_id);
  }
  for (const auto& [domain, readback] : runtime_route_configuration_readbacks_) {
    static_cast<void>(domain);
    if (const auto* provider = readback.provider(); provider != nullptr &&
        provider->domain.provider()->provider.controllable_component_id == component &&
        provider->component_lifecycle_generation != generation) {
      return reject<ComponentCommandReadinessDTO>(
          "provider readiness belongs to a different actual lifecycle generation",
          "component_readiness_provider_generation_not_current");
    }
  }
  if (auto routes = require_component_routes_current_locked(component, command_eligible);
      !routes.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(routes.error());
  }
  // Refresh each affected route's readiness rows to the published generation
  // without changing its configuration or route generation.
  std::map<ControlDomainId, std::vector<RouteReadinessPublicationRowDTO>> refreshed_rows;
  for (const auto& domain : component_domains) {
    const auto readback = runtime_route_configuration_readbacks_.find(domain);
    if (readback == runtime_route_configuration_readbacks_.end() || readback->second.provider() != nullptr ||
        readback->second.controller().active_configuration.state !=
            ActiveControlConfigurationStateDTO::kReconciled ||
        !readback->second.controller().operation_admission_open ||
        runtime_route_configuration_publication_required_.contains(domain)) {
      continue;
    }
    auto rows = derive_route_readiness_projection(readback->second.controller().active_configuration,
                                                  registrations_);
    if (!rows.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(rows.error());
    }
    refreshed_rows.emplace(domain, std::move(rows.value()));
  }
  for (auto& [domain, rows] : refreshed_rows) {
    runtime_route_configuration_readbacks_.at(domain).controller().readiness_rows = std::move(rows);
  }
  auto& entry = component_command_readiness_.at(component);
  entry.component_lifecycle_generation = generation;
  entry.command_eligible = command_eligible;
  return Result<ComponentCommandReadinessDTO>::Ok(entry);
}

Result<ComponentCommandReadinessDTO>
ResourceAddressedExecutionKernel::publish_component_command_readiness(
    const CellAdmissionClaimDTO& selected_claim, const SelectedComponentBringUpOfferDTO& offer,
    const ComponentLifecycleStatusDTO& status,
    const SelectedControllerOccurrenceDTO& installed_occurrence,
    const std::vector<RuntimeParticipantExecutionRegistration>& installed,
    const std::vector<ActiveControlConfigurationDTO>& configurations, bool command_eligible) {
  if (command_eligible || !status.locally_valid() || !status.selected_bring_up ||
      status.apply_bring_up || !installed_occurrence.locally_valid() || installed.empty() ||
      status.phase == ComponentLifecycleTransactionPhaseDTO::kAccepted ||
      status.status_owner_epoch != selected_claim.intent.owner_epoch) {
    return reject<ComponentCommandReadinessDTO>(
        "selected successor publication requires original live false-only holding facts",
        "selected_successor_publication_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  const auto held = cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kInterventionOrRecovery);
  if (!held || *held != selected_claim) {
    return reject<ComponentCommandReadinessDTO>("selected successor claim is not exactly held",
                                                "component_readiness_claim_not_held");
  }
  if (auto valid = validate_selected_bring_up_offer_identity(selected_claim.intent, offer);
      !valid.ok()) return Result<ComponentCommandReadinessDTO>::Err(valid.error());
  const auto& evidence = *status.selected_bring_up;
  const auto& component = offer.predecessor.controllable_component_id;
  const auto scope = SelectedComponentBringUpFenceScope(offer);
  if (!scope.ok()) return Result<ComponentCommandReadinessDTO>::Err(scope.error());
  if (evidence.selected_claim != selected_claim ||
      evidence.offer_digest != selected_claim.intent.request_payload_digest ||
      evidence.original_request_payload_digest != offer.original_request_payload_digest ||
      evidence.predecessor_snapshot_digest != offer.predecessor.snapshot_digest ||
      evidence.incident_id != offer.incident_id ||
      evidence.containment_stop_id != offer.containment.stop_id ||
      evidence.containment_stop_generation != offer.containment.stop_generation ||
      evidence.candidate_projection_revision != offer.candidate_projection_revision ||
      evidence.candidate_retained_bundle_digest != offer.candidate_retained_bundle_digest ||
      evidence.candidate_retained_bundle_bytes != offer.candidate_retained_bundle_bytes ||
      status.acceptance.lifecycle_transaction_id != offer.lifecycle_transaction_id ||
      status.acceptance.lifecycle_transaction_generation != offer.lifecycle_transaction_generation ||
      status.accepted_intent.request_context != offer.request_context ||
      status.accepted_intent.controllable_component_id != component ||
      status.accepted_intent.expected_component_lifecycle_generation !=
          offer.predecessor.component_lifecycle_generation ||
      evidence.accepted_fence.captured_scope != scope.value() ||
      config_.operation_admission_gate == nullptr ||
      !config_.operation_admission_gate->exact_fence_is_held(
          evidence.accepted_fence.configuration_fence_id, scope.value())) {
    return reject<ComponentCommandReadinessDTO>("selected successor proof/held fence is not exact",
                                                "selected_successor_authority_not_exact");
  }
  std::lock_guard state_lock(state_mutex_);
  const auto containment = std::find_if(incident_containments_.begin(), incident_containments_.end(),
      [&](const auto& row) { return row.incident_id == offer.incident_id; });
  const auto terminal = std::find_if(stop_terminal_history_.begin(), stop_terminal_history_.end(),
      [&](const auto& row) {
        return row->status.accepted.stop_id == offer.containment.stop_id &&
               row->status.accepted.stop_generation == offer.containment.stop_generation;
      });
  if (!is_active_stop_fault_latch(active_stop_fault_latch_) || !motion_admission_closed_ ||
      containment == incident_containments_.end() ||
      containment->controllable_component_id != component || containment->receipt != offer.containment ||
      terminal == stop_terminal_history_.end() || !(*terminal)->status.unsettled.empty() ||
      pending_program_run_admission_ ||
      std::any_of(stop_transactions_.begin(), stop_transactions_.end(),
                  [](const auto& stop) { return stop->active && !stop->terminal; }) ||
      motion_admission_unreconciled_ || manager_liveness_motion_inhibited_) {
    return reject<ComponentCommandReadinessDTO>("selected successor containment is not settled/current",
                                                "selected_bring_up_effects_not_settled");
  }
  if (auto idle = require_idle_for_component_readiness_locked(); !idle.ok())
    return Result<ComponentCommandReadinessDTO>::Err(idle.error());
  if (auto peers = validate_bring_up_candidate_peer_closure_locked(
          component, offer.candidate_participants, offer.candidate_provider_domains,
          offer.fence_admission); !peers.ok())
    return Result<ComponentCommandReadinessDTO>::Err(peers.error());
  const auto readiness = component_command_readiness_.find(component);
  const auto generation = status.acceptance.component_lifecycle_generation;
  if (readiness == component_command_readiness_.end() || readiness->second.command_eligible ||
      (readiness->second.component_lifecycle_generation != offer.predecessor.component_lifecycle_generation &&
       readiness->second.component_lifecycle_generation != generation)) {
    return reject<ComponentCommandReadinessDTO>("selected successor requires the already closed component",
                                                "selected_successor_generation_not_current");
  }
  std::vector<ControlDomainId> domains;
  bool has_arm = false;
  std::size_t current_count = 0U;
  for (const auto& current : registrations_) {
    if (current.binding.value().controllable_component_id != component) continue;
    ++current_count;
    if (std::none_of(installed.begin(), installed.end(), [&](const auto& row) {
          return registration_truth_equal(row, current);
        })) {
      return reject<ComponentCommandReadinessDTO>("selected successor inventory changed",
                                                  "selected_successor_registration_not_current");
    }
  }
  if (current_count != installed.size() || installed.size() != offer.candidate_participants.size()) {
    return reject<ComponentCommandReadinessDTO>("selected successor inventory is incomplete",
                                                "selected_successor_registration_not_current");
  }
  for (const auto& row : installed) {
    const auto& binding = row.binding.value();
    if (binding.controllable_component_id != component || !binding.control_path ||
        !row.stop_readiness || row.stop_readiness->value().component_lifecycle_generation != generation ||
        row.engine == nullptr || row.stop_execution_port == nullptr ||
        std::count(offer.candidate_participants.begin(), offer.candidate_participants.end(), binding) != 1 ||
        std::count_if(installed.begin(), installed.end(), [&](const auto& other) {
          return other.binding.value() == binding;
        }) != 1) {
      return reject<ComponentCommandReadinessDTO>("selected successor is not the sealed installed candidate",
                                                  "selected_successor_registration_not_current");
    }
    domains.push_back(binding.control_path->control_domain_id);
    has_arm = has_arm || row.prepared_motion_engine != nullptr;
  }
  std::sort(domains.begin(), domains.end());
  domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
  const auto successor = ComputeSelectedControllerSuccessorOccurrence(
      evidence.predecessor_occurrence, domains, has_arm);
  if (!successor.ok()) return Result<ComponentCommandReadinessDTO>::Err(successor.error());
  if (successor.value() != evidence.successor_occurrence ||
      installed_occurrence.arm_binding_epoch != successor.value().arm_binding_epoch ||
      installed_occurrence.domains.size() != domains.size() || configurations.size() != domains.size()) {
    return reject<ComponentCommandReadinessDTO>("selected successor occurrence is not the owner-issued successor",
                                                "selected_successor_occurrence_not_current");
  }
  for (std::size_t i = 0U; i < domains.size(); ++i) {
    const auto& observed = installed_occurrence.domains[i];
    const auto domain = std::find_if(status.domains.begin(), status.domains.end(),
        [&](const auto& row) { return row.control_domain_id == domains[i]; });
    const auto route = runtime_route_configuration_readbacks_.find(domains[i]);
    if (observed.control_domain_id != domains[i] || observed.observation_generation == 0U ||
        observed.observation_generation < successor.value().domains[i].observation_generation ||
        configurations[i].control_domain_id != domains[i] ||
        route == runtime_route_configuration_readbacks_.end() || route->second.provider() != nullptr ||
        route->second.controller().active_configuration != configurations[i] ||
        domain == status.domains.end() || !domain->current_scope ||
        domain->current_scope->provider() != nullptr ||
        domain->current_scope->controller().active_control_configuration != configurations[i] ||
        domain->current_state != ComponentLifecycleStateDTO::kActive ||
        !domain->observed_after || !domain->resolved_provider_occurrence ||
        domain->observed_after->observation_generation != observed.observation_generation ||
        domain->observed_after->observed_state != ComponentLifecycleStateDTO::kActive) {
      return reject<ComponentCommandReadinessDTO>("selected successor lacks exact current holding observations",
                                                  "selected_successor_holding_not_current");
    }
    for (const auto& row : installed) {
      const auto& path = *row.binding.value().control_path;
      if (path.control_domain_id != domains[i]) continue;
      // V-B2: the holding observation is the one recorded after the domain's
      // transition; it was taken from the domain's resolved provider.
      const auto& occurrence = *domain->resolved_provider_occurrence;
      if (occurrence.provider_instance_id != path.provider_instance_id ||
          occurrence.provider_manifest_id != path.provider_manifest_id ||
          occurrence.provider_manifest_revision != path.provider_manifest_revision ||
          occurrence.provider_runtime_unit_id != path.provider_runtime_unit_id ||
          occurrence.provider_runtime_epoch != path.provider_runtime_epoch) {
        return reject<ComponentCommandReadinessDTO>("selected successor observation belongs to another occurrence",
                                                    "selected_successor_holding_not_current");
      }
    }
  }
  if (auto routes = require_installed_component_routes_current_locked(component); !routes.ok())
    return Result<ComponentCommandReadinessDTO>::Err(routes.error());
  return commit_component_command_readiness_locked(component, generation, false);
}

Result<ComponentCommandReadinessDTO> ResourceAddressedExecutionKernel::component_command_readiness(
    const ControllableComponentId& controllable_component_id) const {
  if (controllable_component_id.empty()) {
    return reject<ComponentCommandReadinessDTO>("component readiness query identity is invalid",
                                                "component_readiness_identity_invalid",
                                                ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard state_lock(state_mutex_);
  const auto found = component_command_readiness_.find(controllable_component_id);
  return Result<ComponentCommandReadinessDTO>::Ok(
      found == component_command_readiness_.end()
          ? ComponentCommandReadinessDTO{controllable_component_id, std::nullopt, false}
          : found->second);
}

bool ResourceAddressedExecutionKernel::participant_engine_in_use(
    const IResolvedParticipantExecutionEngine* engine) const {
  if (engine == nullptr) {
    return false;
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard state_lock(state_mutex_);
  return std::any_of(
             active_operations_.begin(), active_operations_.end(),
             [&](const auto& item) { return item.second->participant_for(engine) != nullptr; }) ||
         std::any_of(registrations_.begin(), registrations_.end(),
                     [&](const auto& registration) { return registration.engine == engine; });
}

bool ResourceAddressedExecutionKernel::operation_control_reconfiguration_busy_locked() const {
  return operation_control_reconfiguration_in_progress_ ||
         (operation_control_executor_ != nullptr &&
          operation_control_executor_->has_active_calls()) ||
         std::any_of(active_operations_.begin(), active_operations_.end(), [](const auto& item) {
           return item.second->pending_control.has_value() ||
                  item.second->ordinary_commit_in_flight ||
                  item.second->recovery_effect_in_flight ||
                  item.second->coordinated_cancellation_in_flight ||
                  std::any_of(item.second->participants.begin(), item.second->participants.end(),
                              [](const auto& participant) {
                                return participant.preparation_borrow_in_flight();
                              });
         });
}

std::vector<IResolvedParticipantExecutionEngine*>
ResourceAddressedExecutionKernel::operation_control_engines_locked(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) const {
  auto engines = distinct_execution_engines(registrations);
  // A replaced provider remains responsible for cleanup of its resident
  // operations. Retiring an unrelated provider must not remove those lanes.
  for (const auto& [operation_id, operation] : active_operations_) {
    (void)operation_id;
    for (const auto& participant : operation->participants) {
      if (std::find(engines.begin(), engines.end(), participant.engine) == engines.end())
        engines.push_back(participant.engine);
    }
  }
  return engines;
}

Result<void> ResourceAddressedExecutionKernel::release_retired_engine(
    IResolvedParticipantExecutionEngine* engine) {
  if (engine == nullptr) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInvalidRequest)),
                                   "retired participant engine is absent",
                                   "retired_participant_engine_absent"));
  }
  std::lock_guard submit_lock(submission_mutex_);
  const auto subscription =
      std::find_if(engine_subscriptions_.begin(), engine_subscriptions_.end(),
                   [&](const EngineSubscription& item) { return item.engine == engine; });
  if (subscription == engine_subscriptions_.end()) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kMissingReference)),
                                   "retired participant engine has no subscription",
                                   "retired_participant_engine_subscription_absent"));
  }
  bool control_reconfiguration_busy = false;
  std::vector<IResolvedParticipantExecutionEngine*> control_engines;
  {
    std::lock_guard state_lock(state_mutex_);
    if (std::any_of(
            active_operations_.begin(), active_operations_.end(),
            [&](const auto& item) { return item.second->participant_for(engine) != nullptr; }) ||
        std::any_of(registrations_.begin(), registrations_.end(),
                    [&](const auto& registration) { return registration.engine == engine; })) {
      return Result<void>::Err(error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                     "retired participant engine remains in use",
                                     "retired_participant_engine_in_use"));
    }
    control_reconfiguration_busy = operation_control_reconfiguration_busy_locked();
    if (!control_reconfiguration_busy) {
      control_engines = operation_control_engines_locked(registrations_);
      operation_control_reconfiguration_in_progress_ = true;
    }
  }
  if (control_reconfiguration_busy) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                   "operation-control work prevents engine retirement",
                                   "operation_control_reconfiguration_busy"));
  }

  std::unique_ptr<BoundedOperationControlExecutor> retired_control_executor;
  {
    std::lock_guard state_lock(state_mutex_);
    if (control_engines.empty()) {
      retired_control_executor = std::move(operation_control_executor_);
    } else if (operation_control_executor_ == nullptr ||
               operation_control_executor_->ReconfigureEngines(std::move(control_engines)) !=
                   OperationControlLaneUpdateStatus::kApplied) {
      operation_control_reconfiguration_in_progress_ = false;
      return Result<void>::Err(error(
          std::string(to_string(ErrorCode::kExecutionRejected)),
          "retired engine cleanup could not retain the current operation-control lanes",
          "operation_control_reconfiguration_failed"));
    }
    operation_control_reconfiguration_in_progress_ = false;
  }
  if (retired_control_executor != nullptr) {
    retired_control_executor->CloseIngress();
    WaitForDrain("kernel_runtime.retired_control_executor", std::chrono::seconds(5),
                 [&](std::chrono::nanoseconds bound) { return retired_control_executor->DrainFor(bound); });
    retired_control_executor.reset();
  }
  engine->unsubscribe_events(subscription->handle);
  engine_subscriptions_.erase(subscription);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_preview_participant(
    const OperationIntentDTO& intent, const RuntimeParticipantExecutionRegistration& registration) {
  // The caller holds ordinary submission ordering, never Stop's mutex across
  // capture resolution or allocation. Preview grants no execution authority.
  std::optional<ResolvedManualProgramHandover> handover;
  if (registration.intrinsic_direct_control) {
    auto current = resolve_manual_program_handover(intent);
    if (!current.ok()) return Result<void>::Err(current.error());
    handover = std::move(current.value());
    std::optional<ManualProgramHandoverContextDTO> expected;
    if (handover) {
      const auto& capture = handover->capture;
      expected = ManualProgramHandoverContextDTO{capture.program_runner_owner_epoch,
                                                 capture.program_run_id, capture.claim_generation,
                                                 handover->selection_revision};
    }
    if (expected != intent.manual_handover)
      return reject<void>("manual preview must name the current held Program context",
                          "manual_handover_scope_stale");
  } else if (intent.manual_handover) {
    return reject<void>("manual preview requires an intrinsic direct-control target",
                        "direct_control_product_mismatch");
  }
  std::vector<ResourceId> resources;
  resources.reserve(registration.binding.value().resources.size());
  for (const auto& resource : registration.binding.value().resources)
    resources.push_back(resource.resource_id);
  std::sort(resources.begin(), resources.end());
  std::lock_guard state_lock(state_mutex_);
  if (motion_admission_closed_ || motion_admission_unreconciled_)
    return reject<void>("preview is unavailable while motion admission is closed",
                        "motion_admission_closed", ErrorCode::kInvalidStateTransition);
  if (handover) {
    const auto held = validate_manual_program_handover_locked(*handover);
    if (!held.ok()) return held;
  }
  for (const auto& [id, operation] : active_operations_) {
    (void)id;
    // Effect-free preview of a correction for a held, never-dispatched
    // failure is the recovery workflow. Execution admission stays exclusive.
    if (operation->held_without_physical_effect()) continue;
    for (const auto& participant : operation->participants) {
      if (participant.engine == registration.engine ||
          std::any_of(participant.resources.begin(), participant.resources.end(),
                      [&](const auto& resource) {
                        return std::binary_search(resources.begin(), resources.end(), resource);
                      })) {
        return reject<void>(
            "preview target still owns an unsettled operation",
            std::holds_alternative<MotionParticipantIntentDTO>(intent.participants.front().payload)
                ? "motion_preview_operation_busy"
                : "capability_command_preview_operation_busy");
      }
    }
  }
  return Result<void>::Ok();
}


Result<std::unique_ptr<IAddressedMotionPreviewSession>>
ResourceAddressedExecutionKernel::begin_motion_preview_session(
    const OperationIntentDTO& primary_motion) {
  std::unique_lock submission_lock(submission_mutex_);
  auto validation = validate_operation_intent(primary_motion);
  if (!validation.ok()) {
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(validation.error());
  }
  if (primary_motion.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(
        error("stale_request_owner_epoch", "preview request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }
  if (primary_motion.participants.size() != 1U ||
      primary_motion.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      !std::holds_alternative<MotionParticipantIntentDTO>(
          primary_motion.participants.front().payload)) {
    return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
        "I6 preview requires one independent motion participant",
        "i6_preview_primary_motion_invalid", ErrorCode::kInvalidRequest);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    if (motion_admission_closed_) {
      return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
          "new motion admission closed while the operation was being prepared",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (emergency_stop_reserve_.find(primary_motion.request_context.request_id.value())
            .has_value()) {
      return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(
          error("request_id_conflict", "operation request ID became retained by emergency Stop",
                "request_id_conflict"));
    }
  }
  auto resolved = resolve_registration(primary_motion, 0U);
  if (!resolved.ok()) {
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(resolved.error());
  }
  const auto available = validate_preview_participant(primary_motion, resolved.value());
  if (!available.ok())
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(available.error());
  auto* engine = resolved.value().engine;
  auto* motion = dynamic_cast<IMotionPreview*>(engine);
  if (motion == nullptr) {
    return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
        "resolved participant has no non-dispatching motion preview",
        "motion_preview_engine_unavailable", ErrorCode::kUnsupportedCapability);
  }
  if (!resolved.value().participant_policy.has_value()) {
    return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
        "resolved participant has no sealed motion policy",
        "runtime_participant_policy_missing", ErrorCode::kMissingReference);
  }
  std::unique_ptr<IAddressedMotionPreviewSession> session = std::make_unique<PreviewSession>(
      *this, std::move(resolved.value().binding), std::move(*resolved.value().participant_policy),
      *engine, *motion, std::move(submission_lock));
  return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Ok(std::move(session));
}

Result<CapabilityCommandPreviewResultDTO>
ResourceAddressedExecutionKernel::preview_capability_command(const OperationIntentDTO& intent) {
  std::lock_guard submit_lock(submission_mutex_);
  auto validation = validate_operation_intent(intent);
  if (!validation.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(validation.error());
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(error(
        "stale_request_owner_epoch", "capability-command preview names a stale Kernel owner epoch",
        "stale_request_owner_epoch"));
  }
  if (intent.participants.size() != 1U ||
      intent.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      !std::holds_alternative<CapabilityCommandIntentDTO>(intent.participants.front().payload)) {
    return reject<CapabilityCommandPreviewResultDTO>(
        "I6 command preview requires one independent capability command",
        "i6_preview_capability_command_invalid", ErrorCode::kInvalidRequest);
  }
  auto resolved = resolve_registration(intent, 0U);
  if (!resolved.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(resolved.error());
  }
  const auto available = validate_preview_participant(intent, resolved.value());
  if (!available.ok()) return Result<CapabilityCommandPreviewResultDTO>::Err(available.error());
  auto* engine = resolved.value().engine;
  if (engine == nullptr) {
    return reject<CapabilityCommandPreviewResultDTO>(
        "resolved command participant has no registered subordinate engine",
        "runtime_participant_engine_missing", ErrorCode::kInvariantViolation);
  }
  const auto& command = std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload);
  const auto& binding = resolved.value().binding.value();
  auto request = build_command_request(
      command, binding, engine->gripper_actuation_kind(binding.command_target_id.value()),
      engine->gripper_output_mode(binding.command_target_id.value()));
  if (!request.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(request.error());
  }
  auto preflight = engine->validate_gripper_command(request.value());
  if (!preflight.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(preflight.error());
  }
  const auto status = engine->get_gripper_status(request.value().gripper_id);
  if (status.state == GripperCommandState::kExecuting) {
    return reject<CapabilityCommandPreviewResultDTO>(
        "gripper command preview cannot validate a busy target", "gripper_busy");
  }
  return Result<CapabilityCommandPreviewResultDTO>::Ok(CapabilityCommandPreviewResultDTO{
      resolved.value().binding.value(), std::move(request.value())});
}

const RuntimeParticipantExecutionRegistration* ResourceAddressedExecutionKernel::registration_for(
    const RuntimeParticipantBinding& binding) const noexcept {
  const auto found = std::find_if(registrations_.begin(), registrations_.end(),
                                  [&](const RuntimeParticipantExecutionRegistration& registration) {
                                    return registration.binding.value() == binding.value();
                                  });
  return found == registrations_.end() ? nullptr : &*found;
}

Result<RuntimeParticipantExecutionRegistration>
ResourceAddressedExecutionKernel::resolve_registration(const OperationIntentDTO& operation,
                                                       const std::size_t participant_index) const {
  if (!config_.participant_resolver) {
    auto binding = registry_.resolve(operation, participant_index);
    if (!binding.ok()) {
      return Result<RuntimeParticipantExecutionRegistration>::Err(binding.error());
    }
    const auto* registration = registration_for(binding.value());
    if (registration == nullptr || registration->engine == nullptr) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "resolved participant has no registered subordinate engine",
          "runtime_participant_engine_missing", ErrorCode::kInvariantViolation);
    }
    return bind_captured_node_io_rule(operation, participant_index, *registration);
  }

  Result<RuntimeParticipantExecutionRegistration> resolved =
      Result<RuntimeParticipantExecutionRegistration>::Err(
          error(std::string(to_string(ErrorCode::kInternalError)),
                "product-backed participant resolver did not return a result",
                "runtime_participant_resolver_failed"));
  try {
    resolved = config_.participant_resolver(operation, participant_index);
  } catch (const std::exception& exception) {
    return Result<RuntimeParticipantExecutionRegistration>::Err(
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              std::string("product-backed participant resolution failed: ") + exception.what(),
              "runtime_participant_resolver_failed"));
  } catch (...) {
    return Result<RuntimeParticipantExecutionRegistration>::Err(error(
        std::string(to_string(ErrorCode::kInvariantViolation)),
        "product-backed participant resolution failed", "runtime_participant_resolver_failed"));
  }
  if (!resolved.ok()) {
    return resolved;
  }
  auto validated = RuntimeParticipantRegistry::validate_resolved(operation, participant_index,
                                                                 resolved.value().binding);
  if (!validated.ok()) {
    return Result<RuntimeParticipantExecutionRegistration>::Err(validated.error());
  }
  const auto engine_is_pinned =
      std::any_of(registrations_.begin(), registrations_.end(), [&](const auto& item) {
        return item.engine != nullptr && item.engine == resolved.value().engine;
      });
  if (!engine_is_pinned) {
    return reject<RuntimeParticipantExecutionRegistration>(
        "product-backed participant selected an unpinned subordinate engine",
        "runtime_participant_engine_unpinned", ErrorCode::kInvariantViolation);
  }
  const auto& inventory = registry_.resource_inventory();
  for (const auto& resource : resolved.value().binding.value().resources) {
    if (!std::binary_search(inventory.begin(), inventory.end(), resource.resource_id)) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "product-backed participant widened the active resource inventory",
          "runtime_participant_resource_outside_inventory", ErrorCode::kInvariantViolation);
    }
  }
  return bind_captured_node_io_rule(operation, participant_index, std::move(resolved.value()));
}

Result<RuntimeParticipantExecutionRegistration>
ResourceAddressedExecutionKernel::bind_captured_node_io_rule(
    const OperationIntentDTO& operation, const std::size_t participant_index,
    RuntimeParticipantExecutionRegistration registration) const {
  using Reply = Result<RuntimeParticipantExecutionRegistration>;
  registration.captured_node_interrupt_scope.reset();
  if (participant_index >= operation.participants.size())
    return reject<RuntimeParticipantExecutionRegistration>(
        "captured node participant is absent", "captured_node_interrupt_scope_invalid");
  const auto& participant = operation.participants[participant_index];
  if (!participant.execution_scope ||
      !std::holds_alternative<MotionParticipantIntentDTO>(participant.payload))
    return Reply::Ok(std::move(registration));

  std::optional<ProgramRunExecutionCaptureDTO> capture;
  {
    std::scoped_lock state_lock(state_mutex_);
    if (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
        program_cycle_claim_run_->program_run_id != operation.program_run_id ||
        !program_cycle_claim_run_->capture->io_conditions)
      return Reply::Ok(std::move(registration));
    const auto& current = *program_cycle_claim_run_->capture;
    if (current.program_id != operation.program_id ||
        current.program_revision != operation.program_revision ||
        current.program_binding_id != operation.program_binding_id ||
        current.retained_bundle_id != operation.expected_retained_bundle_id ||
        current.graph_revision != operation.expected_graph_revision)
      return reject<RuntimeParticipantExecutionRegistration>(
          "operation differs from its captured Program/Binding",
          "program_execution_capture_mismatch");
    capture = current;
  }
  const auto& nodes = capture->io_conditions->program_node_conditions;
  const auto selected = std::find_if(nodes.begin(), nodes.end(), [&](const auto& node) {
    return node.node_instance_id == participant.execution_scope->node_instance_id;
  });
  if (selected == nodes.end()) return Reply::Ok(std::move(registration));
  if (std::count_if(nodes.begin(), nodes.end(), [&](const auto& node) {
        return node.node_instance_id == selected->node_instance_id;
      }) != 1 || !selected->node_rule || !selected->condition || selected->policy_id.empty())
    return reject<RuntimeParticipantExecutionRegistration>(
        "captured node condition is not qualified exactly once",
        "captured_node_interrupt_rule_unqualified");
  const auto& semantics = *selected->node_rule;
  const auto table_node = std::lower_bound(capture->table->nodes.begin(), capture->table->nodes.end(),
      selected->node_instance_id,
      [](const auto& row, const auto& id) { return row.instance_id < id; });
  if (table_node == capture->table->nodes.end() ||
      table_node->instance_id != selected->node_instance_id ||
      !std::any_of(table_node->target_effects.begin(), table_node->target_effects.end(),
          [&](const auto& effect) {
            return effect.effectful && effect.target_role_id == participant.target_role_id &&
                   effect.capability_id == participant.required_capability_id;
          }))
    return reject<RuntimeParticipantExecutionRegistration>(
        "node condition has no exact captured effectful target association",
        "program_execution_scope_effect_mismatch");
  // This is a pure captured-product projection, also used by effect-free
  // preview. Execute retains its existing held-node and pre-effect checks.
  const auto& binding = registration.binding.value();
  const auto target = std::find_if(capture->binding->target_role_bindings.begin(),
      capture->binding->target_role_bindings.end(), [&](const auto& row) {
        return row.target_role_id == semantics.target_role_id;
      });
  if (target == capture->binding->target_role_bindings.end() ||
      semantics.target_role_id != participant.target_role_id ||
      binding.target_role_id != semantics.target_role_id ||
      target->component_role_id != participant.component_role_id ||
      target->controllable_component_id != binding.controllable_component_id ||
      target->command_target_id != binding.command_target_id || !binding.control_path ||
      binding.capability_id != CapabilityId("execution.trajectory.execute") ||
      !registration.stop_readiness || semantics.priority < 0 || semantics.priority > 100)
    return reject<RuntimeParticipantExecutionRegistration>(
        "captured node condition differs from its exact motion target",
        "captured_node_interrupt_target_mismatch");
  const auto capability = std::find_if(target->capability_bindings.begin(),
      target->capability_bindings.end(), [&](const auto& row) {
        return row.capability_id == binding.capability_id &&
               row.capability_definition_revision == binding.capability_definition_revision;
      });
  const auto& control = *binding.control_path;
  if (capability == target->capability_bindings.end() || !capability->control_binding ||
      capability->control_binding->control_binding_id != control.control_binding_id ||
      capability->control_binding->control_binding_revision != control.control_binding_revision ||
      capture->program_binding_id != binding.program_binding_id ||
      capture->program_id != binding.program_id ||
      capture->program_revision != binding.program_revision ||
      capture->retained_bundle_id != binding.retained_bundle_id ||
      capture->graph_revision != binding.graph_revision)
    return reject<RuntimeParticipantExecutionRegistration>(
        "captured node control binding differs from the retained participant",
        "captured_node_interrupt_binding_mismatch");

  const auto& plan = registration.stop_readiness->value().resolved_stop_base_plan;
  if (plan.escalation_stages.empty() ||
      plan.escalation_stages.front().containment_command.command_kind !=
          StopContainmentCommandKind::kMotionControlledStop ||
      !plan.escalation_stages.front().default_contract_parameters)
    return reject<RuntimeParticipantExecutionRegistration>(
        "captured node has no retained nominal controlled Stop profile",
        "captured_node_interrupt_stop_profile_unqualified");
  auto profile = map_motion_controlled_stop_contract_parameters_to_profile(
      *plan.escalation_stages.front().default_contract_parameters, binding.resources,
      selected->policy_id.value());
  if (!profile.ok()) return Reply::Err(profile.error());
  profile = SelectIoRuleStopProfile(profile.value(), semantics.stop_mode);
  if (!profile.ok()) return Reply::Err(profile.error());
  std::vector<IoRuleStopEntryRowDTO> rows;
  rows.reserve(plan.escalation_stages.size());
  for (const auto& stage : plan.escalation_stages)
    rows.push_back({stage.stop_escalation_stage_id, stage.stage_kind,
                    stage.containment_command.command_kind,
                    stage.command_qualification.stop_action_class});
  const auto response = ResolveIoRuleSuccessThroughIndex(
      rows, semantics.stop_entry_stage_id, semantics.success_through_stage_id);
  if (!response.ok()) return Reply::Err(response.error());

  IoRulePolicyScopeDTO maximum{{binding.controllable_component_id}, {binding.command_target_id},
      {control.control_domain_id}, {control.control_binding_id}, {},
      IoRuleStopScopeDTO::kAffectedResources};
  for (const auto& resource : binding.resources) maximum.resource_ids.push_back(resource.resource_id);
  std::sort(maximum.resource_ids.begin(), maximum.resource_ids.end());
  IoMotionRulePolicyBindingDTO policy{selected->policy_id, selected->condition,
      selected->required_signal_sources, {semantics.activation_mode}, {semantics.stop_mode},
      semantics.priority};
  for (const auto mode : profile.value().fallback_stop_modes)
    policy.allowed_stop_modes.insert(mode);
  policy.semantics = IoMotionRulePolicySemanticsDTO{InterruptPolicyScopeKind::kMove,
      InterruptPreemptionAction::kCompleteMoveOnSignal, {OperationStateKind::kExecuting},
      InterruptPostLatchDisposition::kAutoClearAfterMotionOutcome, false, {}};
  policy.maximum_scope = std::move(maximum);
  policy.stop_entry_stage_id = semantics.stop_entry_stage_id;
  policy.success_through_stage_id = semantics.success_through_stage_id;
  MotionInterruptScopeDTO scope;
  scope.scope_id = selected->policy_id.value();
  scope.policy_id = selected->policy_id.value();
  scope.activation_mode = semantics.activation_mode;
  scope.stop_profile = std::move(profile.value());
  scope.node_category = semantics.node_category;
  registration.stop_node_category = semantics.node_category;
  registration.io_motion_rule_policies = {std::move(policy)};
  registration.captured_node_interrupt_scope = std::move(scope);
  return Reply::Ok(std::move(registration));
}

}  // namespace manyforge::core
