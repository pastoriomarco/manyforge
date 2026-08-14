#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

template <typename Value>
[[nodiscard]] core::Result<Value> reject(std::string reason,
                                         std::string message) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}}});
}

[[nodiscard]] core::ErrorDTO transition_error(std::string reason,
                                              std::string message) {
  return {std::string(core::to_string(core::ErrorCode::kUnsupportedCapability)),
          std::move(message),
          {{"reason", std::move(reason)}}};
}

[[nodiscard]] bool exact_occurrence(
    const core::ResolvedLifecycleProviderOccurrenceDTO &occurrence,
    const core::BrandLifecycleObservationQueryDTO &query) noexcept {
  return occurrence.control_domain_id == query.control_domain_id &&
         occurrence.control_domain_revision == query.control_domain_revision &&
         occurrence.provider_instance_id == query.provider_instance_id &&
         occurrence.provider_manifest_id == query.provider_manifest_id &&
         occurrence.provider_manifest_revision ==
             query.provider_manifest_revision &&
         occurrence.provider_runtime_unit_id ==
             query.provider_runtime_unit_id &&
         occurrence.provider_runtime_epoch == query.provider_runtime_epoch;
}

[[nodiscard]] bool exact_occurrence(
    const core::ResolvedLifecycleProviderOccurrenceDTO &occurrence,
    const core::BrandLifecyclePrimitiveIntentDTO &intent) noexcept {
  return occurrence.control_domain_id == intent.control_domain_id &&
         occurrence.control_domain_revision == intent.control_domain_revision &&
         occurrence.provider_instance_id == intent.provider_instance_id &&
         occurrence.provider_manifest_id == intent.provider_manifest_id &&
         occurrence.provider_manifest_revision ==
             intent.provider_manifest_revision &&
         occurrence.provider_runtime_unit_id ==
             intent.provider_runtime_unit_id &&
         occurrence.provider_runtime_epoch == intent.provider_runtime_epoch;
}

[[nodiscard]] const core::ControllerActivationSetDTO *selected_activation(
    const core::ControllerLifecycleExpectedDomainDTO &expected) noexcept {
  const core::ControllerActivationSetDTO *selected = nullptr;
  for (const auto &activation : expected.controller_topology.activation_sets) {
    if (activation.activation_set_id !=
        expected.desired_configuration.activation_set_id) {
      continue;
    }
    if (selected != nullptr) {
      return nullptr;
    }
    selected = &activation;
  }
  return selected;
}

[[nodiscard]] bool selected_route(
    const core::ControllerLifecycleExpectedDomainDTO &expected,
    const core::ControllerLifecycleExpectedEndpointDTO &endpoint) noexcept {
  return std::any_of(
      expected.desired_configuration.binding_routes.begin(),
      expected.desired_configuration.binding_routes.end(),
      [&](const auto &route) {
        return route.control_binding_id == endpoint.control_binding_id &&
               route.control_binding_revision ==
                   endpoint.control_binding_revision &&
               route.control_route_id == endpoint.control_route_id;
      });
}

[[nodiscard]] std::set<core::ResourceId>
binding_resources(const core::ControllerLifecycleExpectedBindingDTO &binding) {
  std::set<core::ResourceId> result;
  for (const auto &interface : binding.claimed_command_interfaces) {
    result.insert(interface.resource_id);
  }
  for (const auto &interface : binding.claimed_state_interfaces) {
    result.insert(interface.resource_id);
  }
  return result;
}

[[nodiscard]] std::set<core::ResourceId>
participant_resources(const core::RuntimeParticipantBindingDTO &binding) {
  std::set<core::ResourceId> result;
  for (const auto &resource : binding.resources) {
    result.insert(resource.resource_id);
  }
  return result;
}

[[nodiscard]] bool exact_binding_match(
    const core::ControllerLifecycleExpectedDomainDTO &expected,
    const core::ControllerLifecycleExpectedBindingDTO &expected_binding,
    const core::DesiredControlBindingRouteDTO &desired_route,
    const core::RuntimeParticipantBindingDTO &physical) {
  if (!physical.control_path.has_value()) {
    return false;
  }
  const auto &path = *physical.control_path;
  return path.control_domain_id == expected.control_domain_id &&
         path.control_domain_revision == expected.control_domain_revision &&
         path.control_binding_id == expected_binding.control_binding_id &&
         path.control_binding_revision ==
             expected_binding.control_binding_revision &&
         path.control_interface_contract_id ==
             expected_binding.control_interface_contract_id &&
         path.control_interface_revision ==
             expected_binding.control_interface_revision &&
         path.controller_topology_id == expected.controller_topology_id &&
         path.topology_revision == expected.topology_revision &&
         path.control_route_id == desired_route.control_route_id &&
         path.activation_set_id ==
             expected.desired_configuration.activation_set_id &&
         path.provider_runtime_unit_id == expected.source_runtime_unit_id &&
         path.provider_runtime_epoch == expected.source_runtime_epoch &&
         participant_resources(physical) == binding_resources(expected_binding);
}

[[nodiscard]] core::Result<void> verify_physical_binding_coverage(
    const core::ControllerLifecycleExpectedDomainDTO &expected,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings) {
  if (!expected.locally_valid() || selected_activation(expected) == nullptr ||
      physical_bindings.size() != expected.expected_bindings.size()) {
    return reject<void>(
        "external_controller_binding_coverage_incomplete",
        "external controller observation requires one exact physical "
        "representative per retained binding");
  }
  std::set<std::pair<core::ControlBindingId, core::ControlBindingRevision>>
      matched;
  for (const auto &expected_binding : expected.expected_bindings) {
    const auto desired =
        std::find_if(expected.desired_configuration.binding_routes.begin(),
                     expected.desired_configuration.binding_routes.end(),
                     [&](const auto &route) {
                       return route.control_binding_id ==
                                  expected_binding.control_binding_id &&
                              route.control_binding_revision ==
                                  expected_binding.control_binding_revision;
                     });
    if (desired == expected.desired_configuration.binding_routes.end()) {
      return reject<void>(
          "external_controller_desired_binding_missing",
          "external controller binding has no exact desired route");
    }
    const auto matches =
        std::count_if(physical_bindings.begin(), physical_bindings.end(),
                      [&](const auto &physical) {
                        return exact_binding_match(expected, expected_binding,
                                                   *desired, physical.value());
                      });
    if (matches != 1 || !matched
                             .emplace(expected_binding.control_binding_id,
                                      expected_binding.control_binding_revision)
                             .second) {
      return reject<void>(
          "external_controller_binding_identity_mismatch",
          "external controller binding does not resolve one exact physical "
          "representative");
    }
  }
  return core::Result<void>::Ok();
}

[[nodiscard]] bool snapshot_successor(
    const RetainedExternallyManagedDomainSnapshot &candidate,
    const std::uint64_t last_generation,
    const std::optional<RetainedExternallyManagedDomainSnapshot> &last) {
  if (!candidate.locally_valid() ||
      candidate.source_generation < last_generation) {
    return false;
  }
  return candidate.source_generation != last_generation || !last.has_value() ||
         candidate == *last;
}

[[nodiscard]] core::BrandLifecyclePrimitiveResultDTO
brand_result(const core::BrandLifecyclePrimitiveIntentDTO &intent,
             std::string reason, std::string message) {
  return {intent.lifecycle_transaction_id,
          intent.lifecycle_transaction_generation,
          intent.controllable_component_id,
          intent.control_domain_id,
          intent.control_domain_revision,
          intent.provider_instance_id,
          intent.provider_manifest_id,
          intent.provider_manifest_revision,
          intent.provider_runtime_unit_id,
          intent.provider_runtime_epoch,
          intent.lifecycle_primitive_id,
          intent.lifecycle_primitive_attempt_id,
          intent.expected_observation_generation,
          core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
          core::LifecyclePrimitiveEffectDTO::kNone,
          std::nullopt,
          transition_error(std::move(reason), std::move(message))};
}

} // namespace

bool RetainedExternallyManagedDomainSnapshot::locally_valid() const noexcept {
  if (source_generation == 0U || core::to_string(component_state).empty() ||
      facts.size() > core::kMaxLifecycleFacts ||
      (exact_controller_configuration_active &&
       component_state != core::ComponentLifecycleStateDTO::kActive)) {
    return false;
  }
  const auto less = [](const auto &left, const auto &right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  };
  return std::is_sorted(facts.begin(), facts.end(), less) &&
         std::adjacent_find(facts.begin(), facts.end(),
                            [&](const auto &left, const auto &right) {
                              return !less(left, right) && !less(right, left);
                            }) == facts.end() &&
         std::all_of(facts.begin(), facts.end(),
                     [](const auto &fact) { return fact.locally_valid(); });
}

struct RetainedExternallyManagedBrandLifecyclePort::State final {
  State(core::ResolvedLifecycleProviderOccurrenceDTO occurrence_in,
        RetainedExternallyManagedDomainSnapshotSource source_in,
        core::IClock &clock_in, const core::Duration freshness_ns_in)
      : occurrence(std::move(occurrence_in)), source(std::move(source_in)),
        clock(&clock_in), freshness_ns(freshness_ns_in) {}

  core::ResolvedLifecycleProviderOccurrenceDTO occurrence;
  RetainedExternallyManagedDomainSnapshotSource source;
  core::IClock *clock;
  core::Duration freshness_ns;
  std::uint64_t next_source_sequence{1U};
  std::uint64_t last_external_generation{};
  std::optional<RetainedExternallyManagedDomainSnapshot> last_external_snapshot;
  std::mutex mutex;
};

RetainedExternallyManagedBrandLifecyclePort::
    RetainedExternallyManagedBrandLifecyclePort(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

core::Result<std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort>>
RetainedExternallyManagedBrandLifecyclePort::Create(
    core::ResolvedLifecycleProviderOccurrenceDTO provider_occurrence,
    RetainedExternallyManagedDomainSnapshotSource snapshot_source,
    core::IClock &clock, const core::Duration observation_freshness_ns) {
  if (!provider_occurrence.locally_valid() || !snapshot_source ||
      observation_freshness_ns <= 0 ||
      observation_freshness_ns > core::kMaxLifecycleTransactionTimeoutNs) {
    return reject<std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort>>(
        "external_brand_lifecycle_input_invalid",
        "external brand lifecycle observation inputs are invalid");
  }
  return core::
      Result<std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort>>::Ok(
          std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort>(
              new RetainedExternallyManagedBrandLifecyclePort(
                  std::make_shared<State>(std::move(provider_occurrence),
                                          std::move(snapshot_source), clock,
                                          observation_freshness_ns))));
}

core::Result<core::BrandLifecycleObservationDTO>
RetainedExternallyManagedBrandLifecyclePort::Observe(
    const core::BrandLifecycleObservationQueryDTO &query) {
  const auto now = state_->clock->now();
  if (!query.locally_valid() || !exact_occurrence(state_->occurrence, query) ||
      now < 0 ||
      now >
          std::numeric_limits<core::TimePoint>::max() - state_->freshness_ns) {
    return reject<core::BrandLifecycleObservationDTO>(
        "external_brand_lifecycle_query_invalid",
        "external brand lifecycle query or observation clock is invalid");
  }
  core::Result<RetainedExternallyManagedDomainSnapshot> sampled = [&]() {
    try {
      return state_->source(state_->occurrence.control_domain_id, now);
    } catch (...) {
      return reject<RetainedExternallyManagedDomainSnapshot>(
          "external_lifecycle_source_exception",
          "external lifecycle observation source raised an exception");
    }
  }();
  if (!sampled.ok()) {
    return core::Result<core::BrandLifecycleObservationDTO>::Err(
        sampled.error());
  }

  std::lock_guard lock(state_->mutex);
  if (!snapshot_successor(sampled.value(), state_->last_external_generation,
                          state_->last_external_snapshot) ||
      state_->next_source_sequence == 0U ||
      state_->next_source_sequence ==
          std::numeric_limits<std::uint64_t>::max()) {
    return reject<core::BrandLifecycleObservationDTO>(
        "external_brand_lifecycle_generation_invalid",
        "external brand lifecycle generation regressed, conflicted, or "
        "exhausted");
  }
  state_->last_external_generation = sampled.value().source_generation;
  state_->last_external_snapshot = sampled.value();
  const auto source_sequence = state_->next_source_sequence++;
  core::BrandLifecycleObservationDTO result{
      query.lifecycle_transaction_id,
      query.lifecycle_transaction_generation,
      query.controllable_component_id,
      query.control_domain_id,
      query.control_domain_revision,
      query.provider_instance_id,
      query.provider_manifest_id,
      query.provider_manifest_revision,
      query.provider_runtime_unit_id,
      query.provider_runtime_epoch,
      source_sequence,
      sampled.value().source_generation,
      now,
      now,
      now + state_->freshness_ns,
      sampled.value().component_state,
      sampled.value().facts};
  if (!result.locally_valid()) {
    return reject<core::BrandLifecycleObservationDTO>(
        "external_brand_lifecycle_observation_invalid",
        "external brand lifecycle source produced an invalid observation");
  }
  return core::Result<core::BrandLifecycleObservationDTO>::Ok(
      std::move(result));
}

core::BrandLifecyclePrimitiveResultDTO
RetainedExternallyManagedBrandLifecyclePort::Dispatch(
    const core::BrandLifecyclePrimitiveIntentDTO &intent,
    const std::stop_token cancellation) {
  if (!intent.locally_valid() ||
      !exact_occurrence(state_->occurrence, intent)) {
    return brand_result(intent, "external_brand_lifecycle_intent_invalid",
                        "external lifecycle intent is invalid or stale");
  }
  if (cancellation.stop_requested()) {
    return brand_result(intent, "external_brand_lifecycle_cancelled",
                        "external lifecycle observation was cancelled");
  }
  return brand_result(
      intent, "external_brand_lifecycle_observer_only",
      "ManyForge observes but does not mutate the external product lifecycle");
}

RetainedExternallyManagedControllerLifecyclePort::
    RetainedExternallyManagedControllerLifecyclePort(
        core::ControllerLifecycleExpectedDomain expected_domain,
        RetainedExternallyManagedDomainSnapshotSource snapshot_source,
        core::IClock &clock, const core::Duration observation_freshness_ns)
    : expected_domain_(std::move(expected_domain)),
      snapshot_source_(std::move(snapshot_source)), clock_(clock),
      observation_freshness_ns_(observation_freshness_ns) {}

core::Result<std::shared_ptr<RetainedExternallyManagedControllerLifecyclePort>>
RetainedExternallyManagedControllerLifecyclePort::Create(
    core::ControllerLifecycleExpectedDomain expected_domain,
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    RetainedExternallyManagedDomainSnapshotSource snapshot_source,
    core::IClock &clock, const core::Duration observation_freshness_ns) {
  if (!snapshot_source || observation_freshness_ns <= 0 ||
      observation_freshness_ns > core::kMaxLifecycleTransactionTimeoutNs) {
    return reject<
        std::shared_ptr<RetainedExternallyManagedControllerLifecyclePort>>(
        "external_controller_lifecycle_input_invalid",
        "external controller lifecycle observation inputs are invalid");
  }
  const auto verified = verify_physical_binding_coverage(
      expected_domain.value(), physical_bindings);
  if (!verified.ok()) {
    return core::Result<std::shared_ptr<
        RetainedExternallyManagedControllerLifecyclePort>>::Err(verified
                                                                    .error());
  }
  return core::Result<
      std::shared_ptr<RetainedExternallyManagedControllerLifecyclePort>>::
      Ok(std::shared_ptr<RetainedExternallyManagedControllerLifecyclePort>(
          new RetainedExternallyManagedControllerLifecyclePort(
              std::move(expected_domain), std::move(snapshot_source), clock,
              observation_freshness_ns)));
}

core::Result<core::ControllerLifecycleObservationDTO>
RetainedExternallyManagedControllerLifecyclePort::Observe(
    const core::ControllerLifecycleObservationQueryDTO &query) {
  const auto exact_query =
      core::MakeControllerLifecycleObservationQuery(expected_domain_.value());
  const auto now = clock_.now();
  if (!query.locally_valid() || query != exact_query || now < 0 ||
      now > std::numeric_limits<core::TimePoint>::max() -
                observation_freshness_ns_) {
    return reject<core::ControllerLifecycleObservationDTO>(
        "external_controller_lifecycle_query_invalid",
        "external controller lifecycle query or observation clock is invalid");
  }
  core::Result<RetainedExternallyManagedDomainSnapshot> sampled = [&]() {
    try {
      return snapshot_source_(expected_domain_.value().control_domain_id, now);
    } catch (...) {
      return reject<RetainedExternallyManagedDomainSnapshot>(
          "external_controller_source_exception",
          "external controller observation source raised an exception");
    }
  }();
  if (!sampled.ok()) {
    return core::Result<core::ControllerLifecycleObservationDTO>::Err(
        sampled.error());
  }

  std::lock_guard lock(mutex_);
  if (!snapshot_successor(sampled.value(), last_external_generation_,
                          last_external_snapshot_) ||
      next_source_sequence_ == 0U ||
      next_source_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    return reject<core::ControllerLifecycleObservationDTO>(
        "external_controller_lifecycle_generation_invalid",
        "external controller generation regressed, conflicted, or exhausted");
  }
  last_external_generation_ = sampled.value().source_generation;
  last_external_snapshot_ = sampled.value();

  const auto &expected = expected_domain_.value();
  const auto *activation = selected_activation(expected);
  if (activation == nullptr) {
    return reject<core::ControllerLifecycleObservationDTO>(
        "external_controller_activation_missing",
        "retained controller domain has no exact desired activation set");
  }
  const bool ready = sampled.value().exact_controller_configuration_active;
  std::optional<core::ControllerLifecycleObservedConfigurationDTO>
      active_configuration;
  if (ready) {
    std::vector<core::ActiveControlConfigurationBindingRouteDTO> routes;
    routes.reserve(expected.desired_configuration.binding_routes.size());
    for (const auto &route : expected.desired_configuration.binding_routes) {
      routes.push_back({route.control_binding_id,
                        route.control_binding_revision,
                        route.control_route_id});
    }
    active_configuration = core::ControllerLifecycleObservedConfigurationDTO{
        activation->activation_set_id, std::move(routes)};
  }

  std::vector<core::ControllerLifecycleObservedControllerDTO> controllers;
  controllers.reserve(expected.expected_controllers.size());
  for (const auto &controller : expected.expected_controllers) {
    const bool selected =
        std::binary_search(activation->active_controller_role_ids.begin(),
                           activation->active_controller_role_ids.end(),
                           controller.controller_role_id);
    const auto inactive_activity =
        sampled.value().component_state ==
                core::ComponentLifecycleStateDTO::kDisabled
            ? core::ControllerLifecycleActivityDTO::kInactive
            : core::ControllerLifecycleActivityDTO::kUnknown;
    controllers.push_back(
        {controller, ready && selected
                         ? core::ControllerLifecycleActivityDTO::kActive
                         : inactive_activity});
  }

  std::vector<core::ControllerLifecycleObservedEndpointDTO> endpoints;
  endpoints.reserve(expected.expected_endpoints.size());
  for (const auto &endpoint : expected.expected_endpoints) {
    endpoints.push_back(
        {endpoint, ready && selected_route(expected, endpoint)});
  }

  core::ControllerLifecycleObservationDTO result{
      exact_query,
      next_source_sequence_++,
      now,
      now,
      now + observation_freshness_ns_,
      std::move(active_configuration),
      std::move(controllers),
      ready ? activation->active_chain_edges
            : std::vector<core::ControllerChainEdgeRefDTO>{},
      std::move(endpoints)};
  if (!result.locally_valid()) {
    return reject<core::ControllerLifecycleObservationDTO>(
        "external_controller_lifecycle_observation_invalid",
        "external controller source produced an invalid observation");
  }
  return core::Result<core::ControllerLifecycleObservationDTO>::Ok(
      std::move(result));
}

core::ControllerLifecycleTransitionResultDTO
RetainedExternallyManagedControllerLifecyclePort::ApplyTransitionStep(
    const core::ControllerLifecycleTransitionIntentDTO &intent,
    const std::stop_token cancellation) {
  const auto exact_query =
      core::MakeControllerLifecycleObservationQuery(expected_domain_.value());
  if (!intent.locally_valid() || intent.query != exact_query) {
    return {intent,
            core::ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched,
            std::nullopt,
            transition_error("external_controller_transition_invalid",
                             "external controller transition intent is "
                             "invalid or stale")};
  }
  if (cancellation.stop_requested()) {
    return {intent,
            core::ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched,
            std::nullopt,
            transition_error("external_controller_transition_cancelled",
                             "external controller transition was cancelled "
                             "before dispatch")};
  }
  return {intent,
          core::ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched,
          std::nullopt,
          transition_error(
              "external_controller_observer_only",
              "ManyForge observes but does not mutate the externally managed "
              "controller")};
}

} // namespace manyforge::roboplan_backend
