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
    std::function<Result<void>()> commit_publication) {
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
      commit_publication ? &commit_publication : nullptr);
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
      // Provider domains have no physical active-configuration fact.
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
    const std::function<Result<void>()>* commit_publication) {
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
  const auto candidate_stop_participants = qualified_execution_engine_count(registrations);
  if (candidate_stop_participants > prequalified_max_live_stop_participants_) {
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInvalidRequest)),
              "candidate runtime participants exceed prequalified Stop-stage isolation",
              "stop_stage_attempt_prequalification_exceeded"));
  }
  // The Stop-stage attempt pool follows the candidate cell. A resized pool is
  // built (workers pre-started) here, before any Kernel lock; the commit
  // swaps it in; whichever pool is left over (the retired one, or an unused
  // candidate) is drained and joined by `stop_pool_release` after every
  // Kernel lock below is released. These locals precede the locks, so their
  // destructors run after the locks' destructors on every return path.
  const auto required_stop_pool_participants =
      std::max<std::size_t>(1U, candidate_stop_participants);
  std::unique_ptr<BoundedStopStageAttemptExecutor> candidate_stop_pool;
  std::unique_ptr<BoundedStopStageAttemptExecutor> stop_pool_to_release;
  struct StopPoolRelease final {
    std::unique_ptr<BoundedStopStageAttemptExecutor>& candidate;
    std::unique_ptr<BoundedStopStageAttemptExecutor>& retired;
    ~StopPoolRelease() {
      for (auto* pool : {&retired, &candidate}) {
        if (*pool == nullptr) continue;
        (*pool)->CloseIngress();
        WaitForDrain("kernel_runtime.retired_stop_stage_attempt_executor",
                     std::chrono::seconds(5),
                     [&](std::chrono::nanoseconds bound) { return (*pool)->DrainFor(bound); });
        pool->reset();
      }
    }
  } stop_pool_release{candidate_stop_pool, stop_pool_to_release};
  if (stop_stage_attempt_pool_participants() != required_stop_pool_participants) {
    try {
      candidate_stop_pool = std::make_unique<BoundedStopStageAttemptExecutor>(
          required_stop_pool_participants * kMaxStopEscalationStages,
          required_stop_pool_participants,
          static_cast<IStopStageAttemptCompletionReceiver&>(*this));
      if (!candidate_stop_pool->prequalified_ready()) {
        throw std::runtime_error("candidate Stop-stage attempt workers are not ready");
      }
    } catch (const std::exception& exception) {
      return Result<void>::Err(error(
          std::string(to_string(ErrorCode::kInternalError)),
          std::string("candidate Stop-stage attempt pool construction failed: ") +
              exception.what(),
          "stop_stage_attempt_pool_construction_failed"));
    }
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
      auto component_id = registration.binding.value().controllable_component_id.value();
      if (next_engine_subscription_id_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("engine subscription identity exhausted");
      const auto sequence_id = next_engine_subscription_id_++;
      auto ingress = engine_event_ingress_;
      const auto handle = engine->subscribe_events(
          [ingress = std::move(ingress), engine, sequence_id](const KernelEvent& event) {
            auto entered = ingress->Enter();
            if (auto* owner = entered.owner()) owner->on_engine_event(engine, sequence_id, event);
          });
      added_subscriptions.push_back(EngineSubscription{engine, handle, sequence_id, std::move(component_id)});
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

  for (auto& registration : registrations) {
    const auto existing = std::find_if(engine_subscriptions_.begin(), engine_subscriptions_.end(),
        [&](const EngineSubscription& value) { return value.engine == registration.engine; });
    if (existing != engine_subscriptions_.end()) {
      registration.engine_subscription_id = existing->sequence_id;
    } else {
      const auto added = std::find_if(added_subscriptions.begin(), added_subscriptions.end(),
          [&](const EngineSubscription& value) { return value.engine == registration.engine; });
      registration.engine_subscription_id = added->sequence_id;
    }
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
  {
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    table_route_publications.reserve(runtime_route_configuration_readbacks_.size());
    for (const auto& [domain_id, readback] : runtime_route_configuration_readbacks_) {
      if (changed_domains.contains(domain_id)) {
        const auto candidate =
            std::find_if(candidate_configurations.begin(), candidate_configurations.end(),
                         [&](const auto& item) { return item.control_domain_id == domain_id; });
        if (candidate == candidate_configurations.end() ||
            candidate->active_control_configuration_id !=
                readback.active_configuration.active_control_configuration_id ||
            candidate->active_control_configuration_generation !=
                readback.active_configuration.active_control_configuration_generation) {
          continue;
        }
      }
      table_route_publications.push_back(DomainActiveControlConfigurationDTO{
          domain_id, readback.active_configuration.active_control_configuration_id,
          readback.active_configuration.active_control_configuration_generation});
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
    // Swap the Stop-stage attempt pool only while no live Stop transaction
    // owns an attempt of the current pool (its handles stay valid there).
    // A busy current pool that is already large enough is kept.
    bool swap_stop_pool = false;
    bool stop_pool_busy = false;
    if (candidate_stop_pool != nullptr) {
      const auto current_participants =
          stop_stage_attempt_executor_->prequalified_max_live_participants();
      if (current_participants != required_stop_pool_participants) {
        if (!stop_stage_attempts_owned_locked()) {
          swap_stop_pool = true;
        } else if (current_participants < required_stop_pool_participants) {
          stop_pool_busy = true;
        }
      }
    }
    auto table_result =
        stop_pool_busy
            ? reject<void>("a live Stop holds the Stop-stage attempt pool the candidate cell "
                           "must grow; retry after the Stop settles",
                           "stop_stage_attempt_pool_busy", ErrorCode::kExecutionRejected)
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
        const auto candidate = std::find_if(
            candidate_configurations.begin(), candidate_configurations.end(),
            [&](const auto& item) { return item.control_domain_id == readback->first; });
        if (candidate == candidate_configurations.end() ||
            candidate->active_control_configuration_id !=
                readback->second.active_configuration.active_control_configuration_id ||
            candidate->active_control_configuration_generation !=
                readback->second.active_configuration.active_control_configuration_generation) {
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
      if (!publication_commit_error.has_value()) {
        registry_ = std::move(*candidate_registry);
        registrations_ = std::move(registrations);
        stop_retained_bundle_id_ = std::move(candidate_stop_facts->retained_bundle_id);
        stop_graph_revision_ = std::move(candidate_stop_facts->graph_revision);
        config_.graph_revision = stop_graph_revision_;
        if (candidate_retained_bundle_id != nullptr) {
          config_.empty_runtime_retained_bundle_id = *candidate_retained_bundle_id;
          config_.empty_runtime_graph_revision = *candidate_graph_revision;
        }
        stop_component_scope_ = std::move(candidate_stop_facts->components);
        if (swap_stop_pool) {
          // Pointer swap under state_mutex_, which every Stop-path user of
          // the pool holds; the retired pool drains after the locks.
          stop_pool_to_release = std::move(stop_stage_attempt_executor_);
          stop_stage_attempt_executor_ = std::move(candidate_stop_pool);
        }
        config_.participant_resolver = std::move(participant_resolver);
        engine_subscriptions_.insert(engine_subscriptions_.end(),
                                     std::make_move_iterator(added_subscriptions.begin()),
                                     std::make_move_iterator(added_subscriptions.end()));
        if (update_control_executor_in_place) {
          const auto update = operation_control_executor_->ReconfigureEngines(control_engines);
          if (update != OperationControlLaneUpdateStatus::kApplied) {
            // Inventory may already have changed. Do not publish a partial lane
            // update as a successful installation or release the Apply fence.
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
  std::map<ControlDomainId, RuntimeRouteConfigurationReadbackDTO> staged;
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
  std::map<ControlDomainId, RuntimeRouteConfigurationReadbackDTO> staged_readback;
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
  if (found == runtime_route_configuration_readbacks_.end() ||
      found->second.active_configuration.active_control_configuration_id !=
          active_configuration_id ||
      found->second.active_configuration.active_control_configuration_generation !=
          active_configuration_generation) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route configuration readback is absent or stale", "runtime_route_readback_missing",
        ErrorCode::kMissingReference);
  }
  return Result<RuntimeRouteConfigurationReadbackDTO>::Ok(found->second);
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
  if (found == runtime_route_configuration_readbacks_.end()) {
    return Result<std::optional<RuntimeRouteConfigurationReadbackDTO>>::Ok(std::nullopt);
  }
  return Result<std::optional<RuntimeRouteConfigurationReadbackDTO>>::Ok(found->second);
}

Result<void> ResourceAddressedExecutionKernel::require_idle_for_component_readiness_locked()
    const {
  // The Apply fence during composition is whole-cell and exclusive, so no
  // command can be admitted meanwhile. Verify that nothing was resident when
  // it was taken.
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

bool ResourceAddressedExecutionKernel::component_enabled_locked(
    const ControllableComponentId& component) const {
  return std::any_of(component_states_.begin(), component_states_.end(), [&](const auto& state) {
    return state.component_id == component.value() &&
           state.lifecycle == ComponentLifecycle::kEnabled;
  });
}

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::component_command_refusal_locked(
    const RuntimeParticipantExecutionRegistration& registration) const {
  const auto& binding = registration.binding.value();
  if (const auto global = global_io_rule_command_refusal_locked(binding)) return global;
  if (const auto hold = component_stream_holds_.find(binding.controllable_component_id);
      hold != component_stream_holds_.end())
    return hold->second;
  // A component without motion of its own (e.g. a pose source) holds new
  // motion of every component.
  if (!cell_stream_holds_.empty()) return cell_stream_holds_.begin()->second;
  // A component accepts commands exactly while its own
  // state reports `enabled`. A domain without route authority has no
  // component to consult; the route checks decide admission as before.
  const bool governed =
      binding.control_path.has_value() &&
      runtime_route_configuration_readbacks_.contains(binding.control_path->control_domain_id);
  if (!governed) return std::nullopt;
  if (!component_enabled_locked(binding.controllable_component_id) ||
      !registration.stop_readiness.has_value()) {
    ErrorDTO refusal = error("component_not_enabled", "the component is not enabled",
                             "component_not_enabled");
    refusal.details.emplace("controllable_component_id", binding.controllable_component_id.value());
    return refusal;
  }
  // A simulated component whose simulation isn't playing stays
  // enabled; its new commands are refused with that reason (never a fault).
  const auto state =
      std::find_if(component_states_.begin(), component_states_.end(), [&](const auto& row) {
        return row.component_id == binding.controllable_component_id.value();
      });
  if (state != component_states_.end() && state->reason &&
      state->reason->code == "simulation_not_playing") {
    ErrorDTO refusal = error("simulation_not_playing", "the simulation is not playing",
                             "simulation_not_playing");
    refusal.details.emplace("controllable_component_id", binding.controllable_component_id.value());
    return refusal;
  }
  return std::nullopt;
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
         runtime_scale_effect_calls_in_flight_ != 0U ||
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

namespace {
// Emit one JSON string without allocating or allowing a component identifier
// to inject a second log line. This runs only on the retirement lane.
void write_retired_component_id(std::string_view value) noexcept {
  static constexpr char hex[] = "0123456789abcdef";
  std::fputc('"', stderr);
  for (const unsigned char ch : value) {
    if (ch == '"' || ch == '\\') {
      std::fputc('\\', stderr);
      std::fputc(ch, stderr);
    } else if (ch < 0x20U) {
      std::fputs("\\u00", stderr);
      std::fputc(hex[ch >> 4U], stderr);
      std::fputc(hex[ch & 0x0fU], stderr);
    } else {
      std::fputc(ch, stderr);
    }
  }
  std::fputc('"', stderr);
}
}  // namespace

Result<void> ResourceAddressedExecutionKernel::release_retired_engine(
    IResolvedParticipantExecutionEngine* engine) {
  if (engine == nullptr) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInvalidRequest)),
                                   "retired participant engine is absent",
                                   "retired_participant_engine_absent"));
  }
  std::unique_lock submit_lock(submission_mutex_);
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
  const auto sequence_id = subscription->sequence_id;
  auto component_id = std::move(subscription->component_id);
  engine_subscriptions_.erase(subscription);
  submit_lock.unlock();
  flockfile(stderr);
  std::fputs("{\"level\":\"info\",\"event\":\"retired_engine_subscription_release\",\"component_id\":", stderr);
  write_retired_component_id(component_id);
  std::fprintf(stderr, ",\"subscription_sequence_id\":%llu,\"result\":\"released\"}\n",
               static_cast<unsigned long long>(sequence_id));
  funlockfile(stderr);
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
  if (motion_admission_closed_locked() || motion_admission_unreconciled_)
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
        "preview requires one independent motion participant",
        "i6_preview_primary_motion_invalid", ErrorCode::kInvalidRequest);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    if (motion_admission_closed_locked()) {
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
        "command preview requires one independent capability command",
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
  const auto matching_registration = std::find_if(registrations_.begin(), registrations_.end(),
      [&](const auto& item) { return item.engine == resolved.value().engine; });
  resolved.value().engine_subscription_id = matching_registration->engine_subscription_id;
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
            return effect.effectful && effect.target == participant.target &&
                   effect.capability_id == participant.required_capability_id;
          }))
    return reject<RuntimeParticipantExecutionRegistration>(
        "node condition has no exact captured effectful target association",
        "program_execution_scope_effect_mismatch");
  // This is a pure captured-product projection, also used by effect-free
  // preview. Execute retains its existing held-node and pre-effect checks.
  const auto& binding = registration.binding.value();
  const auto target = std::find_if(capture->binding->target_bindings.begin(),
      capture->binding->target_bindings.end(), [&](const auto& row) {
        return row.target == semantics.target;
      });
  if (target == capture->binding->target_bindings.end() ||
      semantics.target != participant.target ||
      target->target.component_id != binding.controllable_component_id ||
      target->target.command_target_id != binding.command_target_id || !binding.control_path ||
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

std::size_t ResourceAddressedExecutionKernel::stop_stage_attempt_pool_participants() const {
  std::lock_guard state_lock(state_mutex_);
  return stop_stage_attempt_executor_ == nullptr
             ? 0U
             : stop_stage_attempt_executor_->prequalified_max_live_participants();
}

}  // namespace manyforge::core
