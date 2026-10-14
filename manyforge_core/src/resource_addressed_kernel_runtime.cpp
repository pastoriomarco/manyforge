#include "resource_addressed_execution_kernel_internal.hpp"

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
    RetainedBundleId candidate_retained_bundle_id,
    GraphRevision candidate_graph_revision,
    const ConfigurationFenceId& apply_fence_id,
    const ConfigurationFenceScopeDTO& scope,
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
      &retained_components, &candidate_retained_bundle_id,
      &candidate_graph_revision, commit_publication ? &commit_publication : nullptr);
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
      if (!domain.candidate_control_domain_revision.has_value()) {
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
    const bool retain_unready_configuration_facts,
    const ConfigurationFenceId* apply_fence_id,
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
    auto table_result = authority_current
        ? resource_slots_->reconfigure_runtime_inventory(
              candidate_registry->resource_inventory(),
              retain_unready_configuration_facts ? declared_candidate_configurations
                                                 : table_configurations,
              table_route_publications)
        : reject<void>("component-set transition lost its exact Apply fence",
                       "runtime_component_set_fence_missing",
                       ErrorCode::kExecutionRejected);
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
      if (publication_commit_error.has_value()) {
        motion_admission_closed_ = true;
      } else {
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
      if (!candidate_control_executor->DrainFor(std::chrono::seconds(5))) {
        std::terminate();
      }
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
      if (!candidate_control_executor->DrainFor(std::chrono::seconds(5))) {
        std::terminate();
      }
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
    if (!retired_control_executor->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
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

Result<void> ResourceAddressedExecutionKernel::require_idle_for_component_readiness_locked() const {
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
    ErrorDTO refusal = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                             "the component is not enabled for commands",
                             "component_command_not_eligible");
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
        readback == runtime_route_configuration_readbacks_.end() ||
        readback->second.active_configuration.state !=
            ActiveControlConfigurationStateDTO::kReconciled ||
        !readback->second.operation_admission_open ||
        readback->second.active_configuration.active_control_configuration_id !=
            binding.control_path->active_control_configuration_id ||
        readback->second.active_configuration.active_control_configuration_generation !=
            binding.control_path->active_control_configuration_generation) {
      ErrorDTO refusal = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                               "an enabled component requires every one of its routes to be current",
                               "component_readiness_route_not_current");
      refusal.details.emplace("control_domain_id", domain.value());
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
      return require_held_lifecycle_claim_locked(*publication.lifecycle_transaction_id);
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
    if (config_.operation_admission_gate == nullptr || resources.empty() ||
        !config_.operation_admission_gate->exact_fence_covers(*publication.parent_apply_fence_id,
                                                              resources)) {
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
    if (auto idle = require_idle_for_component_readiness_locked(); !idle.ok()) {
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
  if (auto idle = require_idle_for_component_readiness_locked(); !idle.ok()) {
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
  if (auto routes = require_component_routes_current_locked(component, command_eligible);
      !routes.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(routes.error());
  }
  // Refresh each affected route's readiness rows to the published generation
  // without changing its configuration or route generation.
  std::map<ControlDomainId, std::vector<RouteReadinessPublicationRowDTO>> refreshed_rows;
  for (const auto& domain : component_domains) {
    const auto readback = runtime_route_configuration_readbacks_.find(domain);
    if (readback == runtime_route_configuration_readbacks_.end() ||
        readback->second.active_configuration.state !=
            ActiveControlConfigurationStateDTO::kReconciled ||
        !readback->second.operation_admission_open ||
        runtime_route_configuration_publication_required_.contains(domain)) {
      continue;
    }
    auto rows = derive_route_readiness_projection(readback->second.active_configuration,
                                                  registrations_);
    if (!rows.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(rows.error());
    }
    refreshed_rows.emplace(domain, std::move(rows.value()));
  }
  for (auto& [domain, rows] : refreshed_rows) {
    runtime_route_configuration_readbacks_.at(domain).readiness_rows = std::move(rows);
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
          component, offer.candidate_participants, offer.fence_admission); !peers.ok())
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
        route == runtime_route_configuration_readbacks_.end() ||
        route->second.active_configuration != configurations[i] ||
        domain == status.domains.end() || !domain->current_scope ||
        domain->current_scope->active_control_configuration != configurations[i] ||
        domain->current_state != ComponentLifecycleStateDTO::kActive ||
        !domain->reconciliation_evidence ||
        domain->reconciliation_evidence->observation_generation != observed.observation_generation ||
        domain->reconciliation_evidence->observed_state != ComponentLifecycleStateDTO::kActive ||
        domain->reconciliation_evidence->postcondition_result != LifecyclePostconditionResultDTO::kSatisfied) {
      return reject<ComponentCommandReadinessDTO>("selected successor lacks exact current holding observations",
                                                  "selected_successor_holding_not_current");
    }
    for (const auto& row : installed) {
      const auto& path = *row.binding.value().control_path;
      if (path.control_domain_id != domains[i]) continue;
      const auto& observation = *domain->reconciliation_evidence;
      if (observation.provider_instance_id != path.provider_instance_id ||
          observation.provider_manifest_id != path.provider_manifest_id ||
          observation.provider_manifest_revision != path.provider_manifest_revision ||
          observation.provider_runtime_unit_id != path.provider_runtime_unit_id ||
          observation.provider_runtime_epoch != path.provider_runtime_epoch) {
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
    if (!retired_control_executor->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
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
  auto request = build_command_request(command, resolved.value().binding.value());
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
    return Result<RuntimeParticipantExecutionRegistration>::Ok(*registration);
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
  return resolved;
}

}  // namespace manyforge::core
