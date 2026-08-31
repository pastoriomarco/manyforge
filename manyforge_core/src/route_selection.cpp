#include <algorithm>
#include <limits>
#include <manyforge_core/route_selection.hpp>
#include <tuple>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId& value) noexcept {
  return !value.empty() && value.value().size() <= kMaxRouteSelectionIdentityBytes;
}

[[nodiscard]] bool request_context_valid(const PublicRequestContextDTO& context) noexcept {
  return bounded_identity(context.request_id) && bounded_identity(context.request_owner_epoch) &&
         !context.source_label.empty() &&
         context.source_label.size() <= kMaxRouteSelectionSourceLabelBytes &&
         (!context.correlation_id.has_value() ||
          (!context.correlation_id->empty() &&
           context.correlation_id->size() <= kMaxRouteSelectionCorrelationBytes));
}

[[nodiscard]] bool error_valid(const ErrorDTO& error) noexcept {
  if (error.code.empty() || error.code.size() > 128U || error.message.size() > 1024U ||
      error.details.size() > 8U) {
    return false;
  }
  return std::all_of(error.details.begin(), error.details.end(), [](const auto& detail) {
    return !detail.first.empty() && detail.first.size() <= 64U && detail.second.size() <= 256U;
  });
}

template <typename Value, typename Less>
[[nodiscard]] bool sorted_unique(const std::vector<Value>& values, Less less) noexcept {
  return std::is_sorted(values.begin(), values.end(), less) &&
         std::adjacent_find(values.begin(), values.end(), [&](const Value& lhs, const Value& rhs) {
           return !less(lhs, rhs) && !less(rhs, lhs);
         }) == values.end();
}

template <typename StrongId>
[[nodiscard]] bool canonical_id_set(const std::vector<StrongId>& values, std::size_t maximum,
                                    bool may_be_empty = false) noexcept {
  return (may_be_empty || !values.empty()) && values.size() <= maximum &&
         std::is_sorted(values.begin(), values.end()) &&
         std::adjacent_find(values.begin(), values.end()) == values.end() &&
         std::all_of(values.begin(), values.end(),
                     [](const StrongId& value) { return bounded_identity(value); });
}

[[nodiscard]] bool desired_route_less(const DesiredControlBindingRouteDTO& lhs,
                                      const DesiredControlBindingRouteDTO& rhs) noexcept {
  return std::tie(lhs.control_binding_id, lhs.control_binding_revision, lhs.control_route_id) <
         std::tie(rhs.control_binding_id, rhs.control_binding_revision, rhs.control_route_id);
}

[[nodiscard]] bool desired_configuration_valid(
    const DesiredControlConfigurationDTO& configuration) noexcept {
  return bounded_identity(configuration.control_domain_id) &&
         bounded_identity(configuration.control_domain_revision) &&
         bounded_identity(configuration.controller_topology_id) &&
         bounded_identity(configuration.topology_revision) &&
         bounded_identity(configuration.activation_set_id) &&
         !configuration.binding_routes.empty() &&
         configuration.binding_routes.size() <= kMaxRouteSelectionAffectedBindings &&
         sorted_unique(configuration.binding_routes, desired_route_less) &&
         std::all_of(configuration.binding_routes.begin(), configuration.binding_routes.end(),
                     [](const DesiredControlBindingRouteDTO& route) {
                       return bounded_identity(route.control_binding_id) &&
                              bounded_identity(route.control_binding_revision) &&
                              bounded_identity(route.control_route_id);
                     }) &&
         std::adjacent_find(configuration.binding_routes.begin(),
                            configuration.binding_routes.end(),
                            [](const DesiredControlBindingRouteDTO& lhs,
                               const DesiredControlBindingRouteDTO& rhs) {
                              return lhs.control_binding_id == rhs.control_binding_id;
                            }) == configuration.binding_routes.end();
}

[[nodiscard]] bool component_expectations_valid(
    const std::vector<RouteSelectionComponentExpectationDTO>& values) noexcept {
  return !values.empty() && values.size() <= kMaxRouteSelectionComponents &&
         sorted_unique(values, CanonicalRouteSelectionComponentExpectationLess) &&
         std::all_of(values.begin(), values.end(),
                     [](const auto& value) { return value.locally_valid(); });
}

[[nodiscard]] bool active_configuration_matches_expectation(
    const std::optional<ActiveControlConfigurationDTO>& configuration,
    const ExpectedActiveControlConfigurationDTO& expectation) noexcept {
  if (!expectation.locally_valid()) {
    return false;
  }
  switch (expectation.presence) {
    case ExpectedActiveControlConfigurationPresenceDTO::kAbsent:
      return !configuration.has_value();
    case ExpectedActiveControlConfigurationPresenceDTO::kPresent:
      return configuration.has_value() && configuration->locally_valid() &&
             configuration->active_control_configuration_id ==
                 expectation.active_control_configuration_id &&
             configuration->active_control_configuration_generation ==
                 expectation.active_control_configuration_generation;
  }
  return false;
}

[[nodiscard]] bool route_scope_matches(
    const ConfigurationFenceScopeDTO& scope, const DesiredControlConfigurationDTO& configuration,
    const std::vector<RouteSelectionComponentExpectationDTO>& components) noexcept {
  if (!scope.valid() || scope.controllable_component_ids.size() != components.size() ||
      scope.domains.size() != 1U || scope.bindings.size() != configuration.binding_routes.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < components.size(); ++index) {
    if (scope.controllable_component_ids[index] != components[index].controllable_component_id) {
      return false;
    }
  }
  const auto& domain = scope.domains.front();
  if (domain.control_domain_id != configuration.control_domain_id ||
      domain.predecessor_control_domain_revision != configuration.control_domain_revision ||
      domain.candidate_control_domain_revision != configuration.control_domain_revision) {
    return false;
  }
  for (std::size_t index = 0U; index < configuration.binding_routes.size(); ++index) {
    const auto& expected = configuration.binding_routes[index];
    const auto& actual = scope.bindings[index];
    if (actual.control_binding_id != expected.control_binding_id ||
        actual.predecessor_control_binding_revision != expected.control_binding_revision ||
        actual.candidate_control_binding_revision != expected.control_binding_revision) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool observation_query_matches_configuration(
    const ControllerLifecycleObservationQueryDTO& query,
    const DesiredControlConfigurationDTO& configuration) noexcept {
  return query.locally_valid() && query.control_domain_id == configuration.control_domain_id &&
         query.control_domain_revision == configuration.control_domain_revision &&
         query.controller_topology_id == configuration.controller_topology_id &&
         query.topology_revision == configuration.topology_revision;
}

[[nodiscard]] bool known(RouteConfigurationFenceBusyDispositionDTO value) noexcept {
  return value == RouteConfigurationFenceBusyDispositionDTO::kRejectIfAffectedBusy;
}

[[nodiscard]] bool known(RouteConfigurationFenceStateDTO value) noexcept {
  switch (value) {
    case RouteConfigurationFenceStateDTO::kHeld:
    case RouteConfigurationFenceStateDTO::kReleasePending:
    case RouteConfigurationFenceStateDTO::kReleased:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(RouteSelectionTransactionPhaseDTO value) noexcept {
  switch (value) {
    case RouteSelectionTransactionPhaseDTO::kAccepted:
    case RouteSelectionTransactionPhaseDTO::kAcquiringFence:
    case RouteSelectionTransactionPhaseDTO::kObserving:
    case RouteSelectionTransactionPhaseDTO::kDeactivating:
    case RouteSelectionTransactionPhaseDTO::kActivating:
    case RouteSelectionTransactionPhaseDTO::kVerifying:
    case RouteSelectionTransactionPhaseDTO::kPublishing:
    case RouteSelectionTransactionPhaseDTO::kReleasingFence:
    case RouteSelectionTransactionPhaseDTO::kOutcomeUnknown:
    case RouteSelectionTransactionPhaseDTO::kReconciling:
    case RouteSelectionTransactionPhaseDTO::kSucceeded:
    case RouteSelectionTransactionPhaseDTO::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(RouteSelectionEffectDTO value) noexcept {
  switch (value) {
    case RouteSelectionEffectDTO::kNone:
    case RouteSelectionEffectDTO::kKnownNoEffect:
    case RouteSelectionEffectDTO::kEffectObserved:
    case RouteSelectionEffectDTO::kPartialEffectObserved:
    case RouteSelectionEffectDTO::kEffectUncertain:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(RouteSelectionTerminalResultDTO value) noexcept {
  switch (value) {
    case RouteSelectionTerminalResultDTO::kSucceeded:
    case RouteSelectionTerminalResultDTO::kFailedKnownNoEffect:
    case RouteSelectionTerminalResultDTO::kFailedPartialEffect:
      return true;
  }
  return false;
}

[[nodiscard]] bool active_configuration_matches_desired(
    const ActiveControlConfigurationDTO& configuration,
    const DesiredControlConfigurationDTO& desired) noexcept {
  if (!configuration.locally_valid() ||
      configuration.control_domain_id != desired.control_domain_id ||
      configuration.control_domain_revision != desired.control_domain_revision ||
      configuration.controller_topology_id != desired.controller_topology_id ||
      configuration.topology_revision != desired.topology_revision ||
      configuration.activation_set_id != desired.activation_set_id ||
      configuration.binding_routes.size() != desired.binding_routes.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < desired.binding_routes.size(); ++index) {
    const auto& actual = configuration.binding_routes[index];
    const auto& expected = desired.binding_routes[index];
    if (actual.control_binding_id != expected.control_binding_id ||
        actual.control_binding_revision != expected.control_binding_revision ||
        actual.control_route_id != expected.control_route_id) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool predecessor_configuration_matches_domain(
    const ActiveControlConfigurationDTO& configuration,
    const ControllerLifecycleExpectedDomainDTO& expected) noexcept {
  if (configuration.binding_routes.size() != expected.desired_configuration.binding_routes.size() ||
      std::none_of(expected.controller_topology.activation_sets.begin(),
                   expected.controller_topology.activation_sets.end(),
                   [&](const ControllerActivationSetDTO& set) {
                     return set.activation_set_id == configuration.activation_set_id;
                   })) {
    return false;
  }
  for (std::size_t index = 0U; index < configuration.binding_routes.size(); ++index) {
    const auto& actual = configuration.binding_routes[index];
    const auto& desired = expected.desired_configuration.binding_routes[index];
    if (actual.control_binding_id != desired.control_binding_id ||
        actual.control_binding_revision != desired.control_binding_revision) {
      return false;
    }
    const auto route = std::find_if(expected.controller_topology.legal_routes.begin(),
                                    expected.controller_topology.legal_routes.end(),
                                    [&](const ControlRouteDTO& candidate) {
                                      return candidate.control_route_id == actual.control_route_id;
                                    });
    if (route == expected.controller_topology.legal_routes.end() ||
        !std::binary_search(route->compatible_activation_set_ids.begin(),
                            route->compatible_activation_set_ids.end(),
                            configuration.activation_set_id)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool accepted_fence_matches_route(
    const RouteConfigurationFenceAcceptedDTO& fence,
    const RouteSelectionAcceptedDTO& acceptance) noexcept {
  return fence.locally_valid() &&
         fence.route_selection_transaction_id == acceptance.route_selection_transaction_id &&
         fence.route_selection_transaction_generation ==
             acceptance.route_selection_transaction_generation &&
         fence.captured_scope == acceptance.configuration_fence_scope &&
         fence.candidate_active_control_configuration_id ==
             acceptance.candidate_active_control_configuration_id &&
         fence.candidate_active_control_configuration_generation ==
             acceptance.candidate_active_control_configuration_generation;
}

[[nodiscard]] Result<PreparedRouteSelectionAdmission> reject(std::string code, std::string message,
                                                             std::string reason) {
  return Result<PreparedRouteSelectionAdmission>::Err(
      ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}});
}

}  // namespace

bool RouteSelectionComponentExpectationDTO::locally_valid() const noexcept {
  return bounded_identity(controllable_component_id) &&
         expected_component_lifecycle_generation.nonzero() &&
         (expected_component_lifecycle_state == ComponentLifecycleStateDTO::kPrepared ||
          expected_component_lifecycle_state == ComponentLifecycleStateDTO::kActive);
}

bool CanonicalRouteSelectionComponentExpectationLess(
    const RouteSelectionComponentExpectationDTO& lhs,
    const RouteSelectionComponentExpectationDTO& rhs) noexcept {
  return lhs.controllable_component_id < rhs.controllable_component_id;
}

bool RouteSelectionIntentDTO::locally_valid() const noexcept {
  return request_context_valid(request_context) && bounded_identity(expected_retained_bundle_id) &&
         bounded_identity(expected_graph_revision) &&
         desired_configuration_valid(desired_configuration) &&
         expected_active_control_configuration.locally_valid() &&
         component_expectations_valid(component_expectations) && attribution.locally_valid();
}

bool RouteSelectionIntentDTO::replay_equal(const RouteSelectionIntentDTO& other) const noexcept {
  return request_context == other.request_context &&
         expected_retained_bundle_id == other.expected_retained_bundle_id &&
         expected_graph_revision == other.expected_graph_revision &&
         desired_configuration == other.desired_configuration &&
         expected_active_control_configuration == other.expected_active_control_configuration &&
         component_expectations == other.component_expectations;
}

bool RouteConfigurationFenceAdmissionQueryDTO::locally_valid() const noexcept {
  return bounded_identity(route_selection_transaction_id) &&
         route_selection_transaction_generation.nonzero() && scope.valid() &&
         !scope.controllable_component_ids.empty() && scope.domains.size() == 1U &&
         bounded_identity(expected_retained_bundle_id) &&
         bounded_identity(expected_graph_revision) &&
         expected_active_control_configuration.locally_valid();
}

bool RouteConfigurationFenceAdmissionSnapshotDTO::locally_valid() const noexcept {
  return query.locally_valid() && execution_host_epoch.valid() &&
         active_projection_revision.valid() &&
         active_projection_revision.retained_bundle_id == query.expected_retained_bundle_id &&
         active_projection_revision.graph_revision == query.expected_graph_revision &&
         active_publication_generation.nonzero() &&
         active_publication_generation == execution_host_epoch.publication_generation &&
         bounded_identity(active_public_pointer_digest) &&
         current_active_control_configuration.locally_valid() &&
         current_active_control_configuration == query.expected_active_control_configuration &&
         bounded_identity(snapshot_digest);
}

bool RouteConfigurationFenceIntentDTO::locally_valid() const noexcept {
  return request_context_valid(owner_request_context) &&
         bounded_identity(initiating_public_request_id) &&
         bounded_identity(route_selection_transaction_id) &&
         route_selection_transaction_generation.nonzero() && scope.valid() &&
         !scope.controllable_component_ids.empty() && scope.domains.size() == 1U &&
         bounded_identity(admission_snapshot_digest) && known(busy_disposition) &&
         bounded_identity(expected_retained_bundle_id) &&
         bounded_identity(expected_graph_revision) &&
         expected_active_control_configuration.locally_valid() &&
         bounded_identity(candidate_active_control_configuration_id) &&
         candidate_active_control_configuration_generation.nonzero() && !reason.empty() &&
         reason.size() <= kMaxRouteSelectionReasonBytes;
}

bool RouteConfigurationFenceIntentDTO::matches(
    const RouteConfigurationFenceAdmissionSnapshotDTO& snapshot) const noexcept {
  return locally_valid() && snapshot.locally_valid() &&
         owner_request_context.request_owner_epoch ==
             snapshot.execution_host_epoch.request_owner_epoch &&
         route_selection_transaction_id == snapshot.query.route_selection_transaction_id &&
         route_selection_transaction_generation ==
             snapshot.query.route_selection_transaction_generation &&
         scope == snapshot.query.scope && admission_snapshot_digest == snapshot.snapshot_digest &&
         expected_retained_bundle_id == snapshot.query.expected_retained_bundle_id &&
         expected_graph_revision == snapshot.query.expected_graph_revision &&
         expected_operation_resource_generation == snapshot.operation_resource_generation &&
         expected_fence_generation == snapshot.configuration_fence_generation &&
         expected_active_control_configuration == snapshot.current_active_control_configuration &&
         snapshot.affected_queued_operation_count == 0U &&
         snapshot.affected_active_operation_count == 0U;
}

bool RouteConfigurationFenceAcceptedDTO::locally_valid() const noexcept {
  return bounded_identity(request_id) && IsAcceptedRequestReplayStatus(replay_status) &&
         bounded_identity(route_selection_transaction_id) &&
         route_selection_transaction_generation.nonzero() &&
         bounded_identity(configuration_fence_id) && configuration_fence_generation.nonzero() &&
         captured_scope.valid() && !captured_scope.controllable_component_ids.empty() &&
         bounded_identity(candidate_active_control_configuration_id) &&
         candidate_active_control_configuration_generation.nonzero() && accepted_at_ns >= 0 &&
         bounded_identity(acceptance_digest);
}

bool RouteConfigurationFenceAcceptedDTO::matches(
    const RouteConfigurationFenceIntentDTO& intent) const noexcept {
  if (!locally_valid() || !intent.locally_valid() ||
      intent.expected_fence_generation.value() == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  return request_id == intent.owner_request_context.request_id &&
         route_selection_transaction_id == intent.route_selection_transaction_id &&
         route_selection_transaction_generation == intent.route_selection_transaction_generation &&
         configuration_fence_generation.value() == intent.expected_fence_generation.value() + 1U &&
         captured_scope == intent.scope &&
         candidate_active_control_configuration_id ==
             intent.candidate_active_control_configuration_id &&
         candidate_active_control_configuration_generation ==
             intent.candidate_active_control_configuration_generation;
}

bool RouteConfigurationFenceStatusQueryDTO::locally_valid() const noexcept {
  return bounded_identity(route_selection_transaction_id) &&
         route_selection_transaction_generation.nonzero() &&
         bounded_identity(configuration_fence_id) && configuration_fence_generation.nonzero() &&
         expected_scope.valid() && !expected_scope.controllable_component_ids.empty();
}

bool RouteConfigurationFenceReconciliationEvidenceDTO::locally_valid() const noexcept {
  return execution_host_epoch.valid() && bounded_identity(route_selection_transaction_id) &&
         route_selection_transaction_generation.nonzero() &&
         bounded_identity(configuration_fence_id) && configuration_fence_generation.nonzero() &&
         captured_scope.valid() && !captured_scope.controllable_component_ids.empty() &&
         bounded_identity(active_retained_bundle_id) && bounded_identity(active_graph_revision) &&
         active_projection_revision.valid() &&
         active_projection_revision.retained_bundle_id == active_retained_bundle_id &&
         active_projection_revision.graph_revision == active_graph_revision &&
         active_publication_generation.nonzero() &&
         active_publication_generation == execution_host_epoch.publication_generation &&
         bounded_identity(active_public_pointer_digest) &&
         current_active_control_configuration.locally_valid() && bounded_identity(evidence_digest);
}

bool RouteConfigurationFenceReconciliationEvidenceDTO::matches(
    const RouteConfigurationFenceStatusQueryDTO& query) const noexcept {
  return locally_valid() && query.locally_valid() &&
         route_selection_transaction_id == query.route_selection_transaction_id &&
         route_selection_transaction_generation == query.route_selection_transaction_generation &&
         configuration_fence_id == query.configuration_fence_id &&
         configuration_fence_generation == query.configuration_fence_generation &&
         captured_scope == query.expected_scope;
}

bool RouteConfigurationFenceReleaseRequestDTO::locally_valid() const noexcept {
  if (!request_context_valid(owner_request_context) ||
      !bounded_identity(initiating_public_request_id) ||
      !bounded_identity(route_selection_transaction_id) ||
      !route_selection_transaction_generation.nonzero() || !accepted_fence.locally_valid() ||
      release_precondition_status_sequence == 0U ||
      !bounded_identity(release_precondition_status_digest) ||
      !release_active_control_configuration.locally_valid() || !held_evidence.locally_valid() ||
      held_evidence.affected_queued_operation_count != 0U ||
      held_evidence.affected_active_operation_count != 0U) {
    return false;
  }
  return owner_request_context.request_owner_epoch ==
             held_evidence.execution_host_epoch.request_owner_epoch &&
         route_selection_transaction_id == accepted_fence.route_selection_transaction_id &&
         route_selection_transaction_generation ==
             accepted_fence.route_selection_transaction_generation &&
         route_selection_transaction_id == held_evidence.route_selection_transaction_id &&
         route_selection_transaction_generation ==
             held_evidence.route_selection_transaction_generation &&
         accepted_fence.configuration_fence_id == held_evidence.configuration_fence_id &&
         accepted_fence.configuration_fence_generation ==
             held_evidence.configuration_fence_generation &&
         accepted_fence.captured_scope == held_evidence.captured_scope &&
         release_active_control_configuration == held_evidence.current_active_control_configuration;
}

bool RouteConfigurationFenceReleasedDTO::locally_valid() const noexcept {
  return request.locally_valid() && IsAcceptedRequestReplayStatus(replay_status) &&
         released_at_ns >= 0 &&
         configuration_fence_id == request.accepted_fence.configuration_fence_id &&
         configuration_fence_generation == request.accepted_fence.configuration_fence_generation &&
         bounded_identity(released_scope_digest) && bounded_identity(receipt_digest);
}

bool RouteConfigurationFenceStatusDTO::locally_valid() const noexcept {
  if (!query.locally_valid() || !evidence.matches(query) || !known(state) ||
      !bounded_identity(result_digest) || (error.has_value() && !error_valid(*error))) {
    return false;
  }
  switch (state) {
    case RouteConfigurationFenceStateDTO::kHeld:
      return admission_fenced && !release_receipt.has_value() &&
             required_recovery_direction.empty() && !error.has_value() &&
             evidence.affected_queued_operation_count == 0U &&
             evidence.affected_active_operation_count == 0U;
    case RouteConfigurationFenceStateDTO::kReleasePending:
      return admission_fenced && !release_receipt.has_value() &&
             !required_recovery_direction.empty() && error.has_value();
    case RouteConfigurationFenceStateDTO::kReleased:
      return !admission_fenced && release_receipt.has_value() && release_receipt->locally_valid() &&
             release_receipt->request.held_evidence == evidence &&
             required_recovery_direction.empty() && !error.has_value();
  }
  return false;
}

bool RouteConfigurationFenceAcquireAttemptReconciliationDTO::locally_valid() const noexcept {
  if (!intent.locally_valid() || (error.has_value() && !error_valid(*error)) ||
      (acceptance.has_value() && !acceptance->matches(intent)) ||
      (current_status.has_value() && !current_status->locally_valid())) {
    return false;
  }
  switch (disposition) {
    case RouteConfigurationFenceAcquireAttemptDispositionDTO::kAcceptedExact:
      return acceptance.has_value() && current_status.has_value() && !error.has_value() &&
             current_status->state == RouteConfigurationFenceStateDTO::kHeld &&
             current_status->query.route_selection_transaction_id ==
                 intent.route_selection_transaction_id &&
             current_status->query.route_selection_transaction_generation ==
                 intent.route_selection_transaction_generation &&
             current_status->query.configuration_fence_id == acceptance->configuration_fence_id &&
             current_status->query.configuration_fence_generation ==
                 acceptance->configuration_fence_generation &&
             current_status->query.expected_scope == acceptance->captured_scope;
    case RouteConfigurationFenceAcquireAttemptDispositionDTO::kNotSeen:
      return !acceptance.has_value() && !current_status.has_value() && !error.has_value();
    case RouteConfigurationFenceAcquireAttemptDispositionDTO::kConflicting:
    case RouteConfigurationFenceAcquireAttemptDispositionDTO::kUncertain:
      return !acceptance.has_value() && error.has_value();
  }
  return false;
}

bool RouteConfigurationFenceAcquireEffectResultDTO::locally_valid() const noexcept {
  if (!intent.locally_valid() || (acceptance.has_value() && !acceptance->matches(intent))) {
    return false;
  }
  switch (disposition) {
    case RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted:
      return acceptance.has_value();
    case RouteConfigurationFenceAcquireEffectDispositionDTO::kProvenNotSeen:
      return !acceptance.has_value();
  }
  return false;
}

bool RouteConfigurationFenceReleaseAttemptReconciliationDTO::locally_valid() const noexcept {
  if (!request.locally_valid() || (error.has_value() && !error_valid(*error)) ||
      (receipt.has_value() && (!receipt->locally_valid() || receipt->request != request)) ||
      (current_status.has_value() && !current_status->locally_valid())) {
    return false;
  }
  switch (disposition) {
    case RouteConfigurationFenceReleaseAttemptDispositionDTO::kReleasedExact:
      return receipt.has_value() && !error.has_value() &&
             (!current_status.has_value() ||
              (current_status->state == RouteConfigurationFenceStateDTO::kReleased &&
               current_status->release_receipt == receipt));
    case RouteConfigurationFenceReleaseAttemptDispositionDTO::kNotReleasedHeld:
      return !receipt.has_value() && current_status.has_value() && !error.has_value() &&
             current_status->state == RouteConfigurationFenceStateDTO::kHeld &&
             current_status->evidence == request.held_evidence;
    case RouteConfigurationFenceReleaseAttemptDispositionDTO::kConflicting:
    case RouteConfigurationFenceReleaseAttemptDispositionDTO::kUncertain:
      return !receipt.has_value() && error.has_value();
  }
  return false;
}

bool RouteConfigurationFenceReleaseEffectResultDTO::locally_valid() const noexcept {
  if (!request.locally_valid() ||
      (receipt.has_value() && (!receipt->locally_valid() || receipt->request != request))) {
    return false;
  }
  switch (disposition) {
    case RouteConfigurationFenceReleaseEffectDispositionDTO::kReleased:
      return receipt.has_value();
    case RouteConfigurationFenceReleaseEffectDispositionDTO::kProvenNotReleasedHeld:
      return !receipt.has_value();
  }
  return false;
}

bool RouteSelectionAcceptedDTO::locally_valid() const noexcept {
  if (!bounded_identity(request_id) || !IsAcceptedRequestReplayStatus(replay_status) ||
      !bounded_identity(route_selection_transaction_id) ||
      !route_selection_transaction_generation.nonzero() ||
      !bounded_identity(accepted_retained_bundle_id) ||
      !bounded_identity(accepted_graph_revision) ||
      !desired_configuration_valid(desired_configuration) ||
      !component_expectations_valid(component_expectations) ||
      (predecessor_active_control_configuration.has_value() &&
       (!predecessor_active_control_configuration->locally_valid() ||
        predecessor_active_control_configuration->state !=
            ActiveControlConfigurationStateDTO::kReconciled)) ||
      !route_scope_matches(configuration_fence_scope, desired_configuration,
                           component_expectations) ||
      !observation_query_matches_configuration(controller_lifecycle_observation_query,
                                               desired_configuration) ||
      !bounded_identity(candidate_active_control_configuration_id) ||
      !candidate_active_control_configuration_generation.nonzero() || accepted_at_ns < 0 ||
      timeout_duration_ns <= 0 || timeout_duration_ns > kMaxRouteSelectionTimeoutNs) {
    return false;
  }

  const auto predecessor_generation = predecessor_active_control_configuration.has_value()
                                          ? predecessor_active_control_configuration
                                                ->active_control_configuration_generation.value()
                                          : 0U;
  // The acceptance does not carry the journal's private generation watermark.
  // Failed known-no-effect candidates burn their generations without replacing
  // the published predecessor, so the next accepted candidate may legitimately
  // be more than one generation after that predecessor.  The journal CAS proves
  // that it is exactly one after the durable watermark.
  if (predecessor_generation == std::numeric_limits<std::uint64_t>::max() ||
      candidate_active_control_configuration_generation.value() <= predecessor_generation) {
    return false;
  }
  if (!predecessor_active_control_configuration.has_value()) {
    return true;
  }
  const auto& predecessor = *predecessor_active_control_configuration;
  return predecessor.control_domain_id == desired_configuration.control_domain_id &&
         predecessor.control_domain_revision == desired_configuration.control_domain_revision &&
         predecessor.controller_topology_id == desired_configuration.controller_topology_id &&
         predecessor.topology_revision == desired_configuration.topology_revision &&
         predecessor.source_runtime_unit_id ==
             controller_lifecycle_observation_query.source_runtime_unit_id;
}

bool RouteSelectionStatusDTO::locally_valid() const noexcept {
  const bool affected_sets_empty =
      affected_control_binding_ids.empty() && affected_controller_role_ids.empty();
  const bool exactly_one_affected_set_empty =
      affected_control_binding_ids.empty() != affected_controller_role_ids.empty();
  if (!acceptance.locally_valid() || !accepted_intent.locally_valid() || status_sequence == 0U ||
      !bounded_identity(status_owner_epoch) || !known(phase) || !known(effect) ||
      accepted_at_ns != acceptance.accepted_at_ns || latest_updated_at_ns < 0 ||
      remaining_duration_ns < 0 || remaining_duration_ns > acceptance.timeout_duration_ns ||
      (status_owner_epoch == accepted_intent.request_context.request_owner_epoch &&
       latest_updated_at_ns < accepted_at_ns) ||
      acceptance.request_id != accepted_intent.request_context.request_id ||
      acceptance.accepted_retained_bundle_id != accepted_intent.expected_retained_bundle_id ||
      acceptance.accepted_graph_revision != accepted_intent.expected_graph_revision ||
      acceptance.desired_configuration != accepted_intent.desired_configuration ||
      acceptance.component_expectations != accepted_intent.component_expectations ||
      !active_configuration_matches_expectation(
          acceptance.predecessor_active_control_configuration,
          accepted_intent.expected_active_control_configuration) ||
      (error.has_value() && !error_valid(*error)) ||
      (terminal_result.has_value() && !known(*terminal_result)) || exactly_one_affected_set_empty ||
      !canonical_id_set(affected_control_binding_ids, kMaxRouteSelectionAffectedBindings,
                        affected_sets_empty) ||
      !canonical_id_set(affected_controller_role_ids, kMaxRouteSelectionAffectedControllers,
                        affected_sets_empty) ||
      controller_transition_results.size() > kMaxRouteSelectionTransitionResults) {
    return false;
  }

  if (controller_observation.has_value() &&
      (!controller_observation->locally_valid() ||
       controller_observation->query != acceptance.controller_lifecycle_observation_query)) {
    return false;
  }
  if (controller_reconciliation.has_value() &&
      (!controller_reconciliation->locally_valid() || !controller_observation.has_value() ||
       controller_reconciliation->query != controller_observation->query ||
       controller_reconciliation->source_sequence != controller_observation->source_sequence)) {
    return false;
  }
  if (affected_sets_empty && (!controller_reconciliation.has_value() ||
                              controller_reconciliation->disposition !=
                                  ControllerLifecycleReconciliationDispositionDTO::kReconciled ||
                              controller_reconciliation->desired_activation_set_id !=
                                  acceptance.desired_configuration.activation_set_id ||
                              controller_reconciliation->desired_binding_routes !=
                                  acceptance.desired_configuration.binding_routes)) {
    return false;
  }
  if (controller_transition_plan.has_value() &&
      (!controller_transition_plan->locally_valid() || !controller_observation.has_value() ||
       controller_transition_plan->query != controller_observation->query ||
       controller_transition_plan->expected_source_sequence !=
           controller_observation->source_sequence)) {
    return false;
  }
  if (!std::all_of(controller_transition_results.begin(), controller_transition_results.end(),
                   [&](const ControllerLifecycleTransitionResultDTO& result) {
                     return result.locally_valid() &&
                            result.intent.query ==
                                acceptance.controller_lifecycle_observation_query;
                   })) {
    return false;
  }

  if (accepted_fence.has_value() && !accepted_fence_matches_route(*accepted_fence, acceptance)) {
    return false;
  }
  if (released_fence.has_value()) {
    if (!accepted_fence.has_value() || !released_fence->locally_valid() ||
        released_fence->request.accepted_fence != *accepted_fence) {
      return false;
    }
  }
  if (resulting_active_control_configuration.has_value()) {
    const auto& resulting = *resulting_active_control_configuration;
    if (!resulting.locally_valid() ||
        resulting.active_control_configuration_id !=
            acceptance.candidate_active_control_configuration_id ||
        resulting.active_control_configuration_generation !=
            acceptance.candidate_active_control_configuration_generation ||
        resulting.control_domain_id != acceptance.desired_configuration.control_domain_id ||
        resulting.control_domain_revision !=
            acceptance.desired_configuration.control_domain_revision ||
        resulting.controller_topology_id !=
            acceptance.desired_configuration.controller_topology_id ||
        resulting.topology_revision != acceptance.desired_configuration.topology_revision ||
        resulting.source_runtime_unit_id !=
            acceptance.controller_lifecycle_observation_query.source_runtime_unit_id ||
        resulting.source_runtime_epoch !=
            acceptance.controller_lifecycle_observation_query.source_runtime_epoch) {
      return false;
    }
  }

  const bool early_phase = phase == RouteSelectionTransactionPhaseDTO::kAccepted ||
                           phase == RouteSelectionTransactionPhaseDTO::kAcquiringFence;
  const bool fence_only_known_no_controller_effect =
      effect == RouteSelectionEffectDTO::kKnownNoEffect && accepted_fence.has_value() &&
      !controller_observation.has_value() && !controller_reconciliation.has_value() &&
      !controller_transition_plan.has_value() && controller_transition_results.empty() &&
      !resulting_active_control_configuration.has_value();
  if (early_phase &&
      (controller_observation.has_value() || controller_reconciliation.has_value() ||
       controller_transition_plan.has_value() || !controller_transition_results.empty() ||
       resulting_active_control_configuration.has_value())) {
    return false;
  }
  if ((phase == RouteSelectionTransactionPhaseDTO::kDeactivating ||
       phase == RouteSelectionTransactionPhaseDTO::kActivating) &&
      (!controller_observation.has_value() || !controller_transition_plan.has_value())) {
    return false;
  }
  if ((phase == RouteSelectionTransactionPhaseDTO::kVerifying ||
       phase == RouteSelectionTransactionPhaseDTO::kPublishing ||
       phase == RouteSelectionTransactionPhaseDTO::kReleasingFence) &&
      (!controller_observation.has_value() || !controller_reconciliation.has_value()) &&
      !fence_only_known_no_controller_effect) {
    return false;
  }
  if (phase == RouteSelectionTransactionPhaseDTO::kObserving ||
      phase == RouteSelectionTransactionPhaseDTO::kDeactivating ||
      phase == RouteSelectionTransactionPhaseDTO::kActivating ||
      phase == RouteSelectionTransactionPhaseDTO::kVerifying ||
      phase == RouteSelectionTransactionPhaseDTO::kPublishing ||
      phase == RouteSelectionTransactionPhaseDTO::kReleasingFence) {
    if (!accepted_fence.has_value()) {
      return false;
    }
  }
  if ((phase == RouteSelectionTransactionPhaseDTO::kOutcomeUnknown ||
       phase == RouteSelectionTransactionPhaseDTO::kReconciling) != reconciliation_required) {
    return false;
  }
  if (reconciliation_required && !error.has_value()) {
    return false;
  }

  const bool terminal_phase = phase == RouteSelectionTransactionPhaseDTO::kSucceeded ||
                              phase == RouteSelectionTransactionPhaseDTO::kFailed;
  if (terminal_phase != terminal_result.has_value() ||
      (terminal_phase && remaining_duration_ns != 0)) {
    return false;
  }
  if (!terminal_phase) {
    return !released_fence.has_value() && !terminal_result.has_value();
  }
  if (effect == RouteSelectionEffectDTO::kEffectUncertain) {
    return false;
  }

  switch (*terminal_result) {
    case RouteSelectionTerminalResultDTO::kSucceeded:
      return phase == RouteSelectionTransactionPhaseDTO::kSucceeded && !error.has_value() &&
             accepted_fence.has_value() && released_fence.has_value() &&
             resulting_active_control_configuration.has_value() &&
             resulting_active_control_configuration->state ==
                 ActiveControlConfigurationStateDTO::kReconciled &&
             active_configuration_matches_desired(*resulting_active_control_configuration,
                                                  acceptance.desired_configuration) &&
             (effect == RouteSelectionEffectDTO::kNone ||
              effect == RouteSelectionEffectDTO::kKnownNoEffect ||
              effect == RouteSelectionEffectDTO::kEffectObserved);
    case RouteSelectionTerminalResultDTO::kFailedKnownNoEffect: {
      if (phase != RouteSelectionTransactionPhaseDTO::kFailed || !error.has_value() ||
          resulting_active_control_configuration.has_value() ||
          (effect != RouteSelectionEffectDTO::kNone &&
           effect != RouteSelectionEffectDTO::kKnownNoEffect)) {
        return false;
      }
      if (accepted_fence.has_value()) {
        return released_fence.has_value();
      }
      return !released_fence.has_value() && !controller_observation.has_value() &&
             !controller_reconciliation.has_value() && !controller_transition_plan.has_value() &&
             controller_transition_results.empty();
    }
    case RouteSelectionTerminalResultDTO::kFailedPartialEffect:
      return phase == RouteSelectionTransactionPhaseDTO::kFailed && error.has_value() &&
             accepted_fence.has_value() && released_fence.has_value() &&
             resulting_active_control_configuration.has_value() &&
             resulting_active_control_configuration->state ==
                 ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled &&
             (effect == RouteSelectionEffectDTO::kEffectObserved ||
              effect == RouteSelectionEffectDTO::kPartialEffectObserved);
  }
  return false;
}

bool CCLCRouteEffectAttemptDTO::locally_valid() const noexcept {
  if (!bounded_identity(route_selection_transaction_id) ||
      !route_selection_transaction_generation.nonzero() || effect_sequence == 0U) {
    return false;
  }

  const auto request_matches_owner = [&](const auto& request) {
    using Request = std::decay_t<decltype(request)>;
    if constexpr (std::is_same_v<Request, RouteConfigurationFenceIntentDTO> ||
                  std::is_same_v<Request, RouteConfigurationFenceReleaseRequestDTO>) {
      return request.locally_valid() &&
             request.route_selection_transaction_id == route_selection_transaction_id &&
             request.route_selection_transaction_generation ==
                 route_selection_transaction_generation;
    } else {
      return request.locally_valid();
    }
  };
  if (!std::visit(request_matches_owner, request)) {
    return false;
  }

  bool kind_matches = false;
  switch (effect_kind) {
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire:
      kind_matches = std::holds_alternative<RouteConfigurationFenceIntentDTO>(request);
      break;
    case CCLCRouteEffectKindDTO::kControllerLifecycleTransition:
      kind_matches = std::holds_alternative<ControllerLifecycleTransitionIntentDTO>(request);
      break;
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease:
      kind_matches = std::holds_alternative<RouteConfigurationFenceReleaseRequestDTO>(request);
      break;
  }
  if (!kind_matches) {
    return false;
  }
  switch (state) {
    case CCLCRouteEffectAttemptStateDTO::kPrepared:
      return !result.has_value();
    case CCLCRouteEffectAttemptStateDTO::kCompleted:
      break;
  }
  if (!result.has_value()) {
    return false;
  }

  switch (effect_kind) {
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire: {
      const auto* actual = std::get_if<RouteConfigurationFenceAcquireEffectResultDTO>(&*result);
      return actual != nullptr && actual->locally_valid() &&
             actual->intent == std::get<RouteConfigurationFenceIntentDTO>(request);
    }
    case CCLCRouteEffectKindDTO::kControllerLifecycleTransition: {
      const auto* actual = std::get_if<ControllerLifecycleTransitionResultDTO>(&*result);
      return actual != nullptr && actual->locally_valid() &&
             actual->intent == std::get<ControllerLifecycleTransitionIntentDTO>(request);
    }
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease: {
      const auto* actual = std::get_if<RouteConfigurationFenceReleaseEffectResultDTO>(&*result);
      return actual != nullptr && actual->locally_valid() &&
             actual->request == std::get<RouteConfigurationFenceReleaseRequestDTO>(request);
    }
  }
  return false;
}

bool CCLCRouteEffectSequenceWatermarkDTO::locally_valid() const noexcept {
  return bounded_identity(route_selection_transaction_id) &&
         route_selection_transaction_generation.nonzero();
}

bool ControlDomainActiveConfigurationGenerationWatermarkDTO::locally_valid() const noexcept {
  return bounded_identity(control_domain_id) && active_control_configuration_generation.nonzero();
}

bool CCLCRouteSelectionRequestResultDTO::locally_valid() const noexcept {
  if (!IsKnownRequestReplayStatus(replay_status)) {
    return false;
  }
  const bool accepted = replay_status == RequestReplayStatusDTO::kAcceptedNew ||
                        replay_status == RequestReplayStatusDTO::kReplayedExact;
  if (!accepted) {
    return !acceptance.has_value() && !status.has_value();
  }
  if (!acceptance.has_value() || !status.has_value() || !acceptance->locally_valid() ||
      !status->locally_valid() || acceptance->replay_status != replay_status) {
    return false;
  }
  RouteSelectionAcceptedDTO persisted = status->acceptance;
  persisted.replay_status = replay_status;
  return persisted == *acceptance;
}

Result<PreparedRouteSelectionAdmission> PrepareRouteSelectionAdmission(
    const RouteSelectionIntentDTO& intent, const RouteSelectionAdmissionCandidate& candidate) {
  if (!intent.locally_valid() || !bounded_identity(candidate.route_selection_transaction_id) ||
      !candidate.route_selection_transaction_generation.nonzero() ||
      !bounded_identity(candidate.candidate_active_control_configuration_id) ||
      !candidate.candidate_active_control_configuration_generation.nonzero() ||
      candidate.accepted_at_ns < 0 || candidate.timeout_duration_ns <= 0 ||
      candidate.timeout_duration_ns > kMaxRouteSelectionTimeoutNs ||
      !bounded_identity(candidate.current_retained_bundle_id) ||
      !bounded_identity(candidate.current_graph_revision) ||
      !component_expectations_valid(candidate.current_component_expectations) ||
      !candidate.expected_controller_domain.locally_valid() ||
      !candidate.configuration_fence_admission_snapshot.locally_valid()) {
    return reject("route_selection_admission_invalid",
                  "the complete-domain route admission candidate is invalid", "candidate_invalid");
  }

  if (candidate.current_retained_bundle_id != intent.expected_retained_bundle_id ||
      candidate.current_graph_revision != intent.expected_graph_revision) {
    return reject("route_selection_current_snapshot_mismatch",
                  "the current retained bundle or graph differs from the request",
                  "bundle_or_graph_changed");
  }
  if (candidate.current_component_expectations != intent.component_expectations) {
    return reject("route_selection_current_snapshot_mismatch",
                  "the complete current contributing-component set differs from the request",
                  "component_expectations_changed");
  }
  if (candidate.expected_controller_domain.desired_configuration != intent.desired_configuration ||
      !observation_query_matches_configuration(
          MakeControllerLifecycleObservationQuery(candidate.expected_controller_domain),
          intent.desired_configuration)) {
    return reject("route_selection_controller_closure_mismatch",
                  "the verified controller-domain closure differs from the complete request",
                  "desired_domain_or_query_mismatch");
  }
  if (!active_configuration_matches_expectation(candidate.current_active_control_configuration,
                                                intent.expected_active_control_configuration)) {
    return reject("route_selection_current_snapshot_mismatch",
                  "the current active control configuration differs from the request",
                  "active_configuration_changed");
  }

  std::uint64_t expected_predecessor_generation = 0U;
  if (candidate.current_active_control_configuration.has_value()) {
    const auto& current = *candidate.current_active_control_configuration;
    if (current.state != ActiveControlConfigurationStateDTO::kReconciled ||
        current.control_domain_id != intent.desired_configuration.control_domain_id ||
        current.control_domain_revision != intent.desired_configuration.control_domain_revision ||
        current.controller_topology_id != intent.desired_configuration.controller_topology_id ||
        current.topology_revision != intent.desired_configuration.topology_revision ||
        current.source_runtime_unit_id !=
            candidate.expected_controller_domain.source_runtime_unit_id) {
      return reject("route_selection_configuration_stale",
                    "the predecessor configuration is not one reconciled current domain fact",
                    "predecessor_identity_or_runtime_changed");
    }
    if (!predecessor_configuration_matches_domain(current, candidate.expected_controller_domain)) {
      return reject("route_selection_configuration_stale",
                    "the predecessor configuration is not the complete retained domain",
                    "predecessor_domain_closure_changed");
    }
    expected_predecessor_generation = current.active_control_configuration_generation.value();
  }
  if (candidate.durable_active_configuration_generation_watermark.value() <
      expected_predecessor_generation) {
    return reject("route_selection_current_snapshot_mismatch",
                  "the durable active-configuration watermark precedes the current publication",
                  "active_configuration_watermark_regressed");
  }
  if (candidate.durable_active_configuration_generation_watermark.value() ==
          std::numeric_limits<std::uint64_t>::max() ||
      candidate.candidate_active_control_configuration_generation.value() !=
          candidate.durable_active_configuration_generation_watermark.value() + 1U) {
    return reject("route_selection_generation_overflow",
                  "the candidate active-configuration generation is not the immediate successor",
                  "active_configuration_generation_not_successor");
  }
  if (!route_scope_matches(candidate.configuration_fence_scope, intent.desired_configuration,
                           intent.component_expectations)) {
    return reject("route_selection_scope_invalid",
                  "the route fence is not the complete desired domain closure",
                  "configuration_fence_scope_mismatch");
  }

  const RouteConfigurationFenceAdmissionQueryDTO expected_fence_query{
      candidate.route_selection_transaction_id, candidate.route_selection_transaction_generation,
      candidate.configuration_fence_scope,      intent.expected_retained_bundle_id,
      intent.expected_graph_revision,           intent.expected_active_control_configuration};
  const auto& fence_snapshot = candidate.configuration_fence_admission_snapshot;
  if (fence_snapshot.query != expected_fence_query ||
      fence_snapshot.affected_queued_operation_count != 0U ||
      fence_snapshot.affected_active_operation_count != 0U) {
    return reject("route_selection_scope_busy",
                  "the complete route scope changed or has affected operations",
                  "kernel_admission_snapshot_mismatch_or_busy");
  }

  const auto query = MakeControllerLifecycleObservationQuery(candidate.expected_controller_domain);
  RouteSelectionAcceptedDTO acceptance{intent.request_context.request_id,
                                       RequestReplayStatusDTO::kAcceptedNew,
                                       candidate.route_selection_transaction_id,
                                       candidate.route_selection_transaction_generation,
                                       intent.expected_retained_bundle_id,
                                       intent.expected_graph_revision,
                                       intent.desired_configuration,
                                       intent.component_expectations,
                                       candidate.current_active_control_configuration,
                                       candidate.configuration_fence_scope,
                                       query,
                                       candidate.candidate_active_control_configuration_id,
                                       candidate.candidate_active_control_configuration_generation,
                                       candidate.accepted_at_ns,
                                       candidate.timeout_duration_ns};

  std::vector<ControlBindingId> affected_bindings;
  affected_bindings.reserve(intent.desired_configuration.binding_routes.size());
  for (const auto& route : intent.desired_configuration.binding_routes) {
    affected_bindings.push_back(route.control_binding_id);
  }
  std::vector<ControllerRoleId> affected_controllers;
  affected_controllers.reserve(candidate.expected_controller_domain.expected_controllers.size());
  for (const auto& controller : candidate.expected_controller_domain.expected_controllers) {
    affected_controllers.push_back(controller.controller_role_id);
  }

  RouteSelectionStatusDTO initial_status{acceptance,
                                         intent,
                                         1U,
                                         intent.request_context.request_owner_epoch,
                                         RouteSelectionTransactionPhaseDTO::kAccepted,
                                         RouteSelectionEffectDTO::kNone,
                                         std::move(affected_bindings),
                                         std::move(affected_controllers),
                                         std::nullopt,
                                         std::nullopt,
                                         std::nullopt,
                                         {},
                                         std::nullopt,
                                         std::nullopt,
                                         std::nullopt,
                                         false,
                                         std::nullopt,
                                         std::nullopt,
                                         candidate.accepted_at_ns,
                                         candidate.accepted_at_ns,
                                         candidate.timeout_duration_ns};

  std::vector<ControlDomainId> reserved_domains{intent.desired_configuration.control_domain_id};
  if (!acceptance.locally_valid() || !initial_status.locally_valid()) {
    return reject("route_selection_admission_invariant_failed",
                  "the derived route acceptance is not a closed valid value",
                  "derived_acceptance_invalid");
  }
  return Result<PreparedRouteSelectionAdmission>::Ok(
      {std::move(acceptance), std::move(initial_status), std::move(reserved_domains),
       candidate.expected_controller_domain});
}

}  // namespace manyforge::core
