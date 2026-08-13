#include <algorithm>
#include <limits>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <set>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId& value) noexcept {
  return !value.empty() && value.value().size() <= kMaxLifecycleIdentityBytes;
}

[[nodiscard]] bool error_valid(const std::optional<ErrorDTO>& error) noexcept {
  if (!error.has_value()) {
    return true;
  }
  if (error->code.empty() || error->code.size() > kMaxLifecycleErrorCodeBytes ||
      error->message.size() > kMaxLifecycleErrorMessageBytes ||
      error->details.size() > kMaxLifecycleErrorDetails) {
    return false;
  }
  return std::all_of(error->details.begin(), error->details.end(), [](const auto& detail) {
    return !detail.first.empty() && detail.first.size() <= kMaxLifecycleErrorDetailKeyBytes &&
           detail.second.size() <= kMaxLifecycleErrorDetailValueBytes;
  });
}

[[nodiscard]] bool portable_token(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaxLifecycleFactTokenBytes) {
    return false;
  }
  const auto alpha_numeric = [](const unsigned char character) noexcept {
    return (character >= static_cast<unsigned char>('A') &&
            character <= static_cast<unsigned char>('Z')) ||
           (character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('z')) ||
           (character >= static_cast<unsigned char>('0') &&
            character <= static_cast<unsigned char>('9'));
  };
  if (!alpha_numeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [&](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return alpha_numeric(byte) || character == '.' || character == '_' || character == '-';
  });
}

template <typename Value, typename Less>
[[nodiscard]] bool sorted_unique(const std::vector<Value>& values, Less less) noexcept {
  return std::is_sorted(values.begin(), values.end(), less) &&
         std::adjacent_find(values.begin(), values.end(),
                            [&](const Value& left, const Value& right) {
                              return !less(left, right) && !less(right, left);
                            }) == values.end();
}

template <typename Value, typename Key>
[[nodiscard]] bool exact_rows_are_subset(const std::vector<Value>& subset,
                                         const std::vector<Value>& superset, Key key) noexcept {
  return std::all_of(subset.begin(), subset.end(), [&](const Value& expected) {
    const auto expected_key = key(expected);
    const auto found = std::lower_bound(superset.begin(), superset.end(), expected_key,
                                        [&](const Value& candidate, const auto& candidate_key) {
                                          return key(candidate) < candidate_key;
                                        });
    return found != superset.end() && key(*found) == expected_key && *found == expected;
  });
}

[[nodiscard]] bool known(ComponentLifecycleIntentKindDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleIntentKindDTO::kPrepare:
    case ComponentLifecycleIntentKindDTO::kEnable:
    case ComponentLifecycleIntentKindDTO::kDisable:
    case ComponentLifecycleIntentKindDTO::kRecover:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(ComponentLifecycleStateDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleStateDTO::kUndeclared:
    case ComponentLifecycleStateDTO::kDeclared:
    case ComponentLifecycleStateDTO::kPrepared:
    case ComponentLifecycleStateDTO::kActive:
    case ComponentLifecycleStateDTO::kDegraded:
    case ComponentLifecycleStateDTO::kFaulted:
    case ComponentLifecycleStateDTO::kDisabling:
    case ComponentLifecycleStateDTO::kDisabled:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(LifecyclePrimitiveDispatchStateDTO value) noexcept {
  switch (value) {
    case LifecyclePrimitiveDispatchStateDTO::kNotDispatched:
    case LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown:
    case LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(LifecyclePrimitiveKnownResultDTO value) noexcept {
  switch (value) {
    case LifecyclePrimitiveKnownResultDTO::kSucceeded:
    case LifecyclePrimitiveKnownResultDTO::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(LifecyclePrimitiveEffectDTO value) noexcept {
  switch (value) {
    case LifecyclePrimitiveEffectDTO::kNone:
    case LifecyclePrimitiveEffectDTO::kPossible:
    case LifecyclePrimitiveEffectDTO::kObserved:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(LifecyclePostconditionResultDTO value) noexcept {
  switch (value) {
    case LifecyclePostconditionResultDTO::kSatisfied:
    case LifecyclePostconditionResultDTO::kNotSatisfied:
    case LifecyclePostconditionResultDTO::kInconclusive:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(ComponentLifecycleDomainActionStateDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleDomainActionStateDTO::kReserved:
    case ComponentLifecycleDomainActionStateDTO::kNotStarted:
    case ComponentLifecycleDomainActionStateDTO::kExecuting:
    case ComponentLifecycleDomainActionStateDTO::kVerifying:
    case ComponentLifecycleDomainActionStateDTO::kReconciling:
    case ComponentLifecycleDomainActionStateDTO::kSucceeded:
    case ComponentLifecycleDomainActionStateDTO::kFailed:
    case ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(ComponentLifecycleDomainEffectDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleDomainEffectDTO::kNone:
    case ComponentLifecycleDomainEffectDTO::kKnownNoEffect:
    case ComponentLifecycleDomainEffectDTO::kEffectObserved:
    case ComponentLifecycleDomainEffectDTO::kPartialEffectObserved:
    case ComponentLifecycleDomainEffectDTO::kEffectUncertain:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(ComponentLifecycleTransactionPhaseDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleTransactionPhaseDTO::kAccepted:
    case ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains:
    case ComponentLifecycleTransactionPhaseDTO::kContaining:
    case ComponentLifecycleTransactionPhaseDTO::kExecuting:
    case ComponentLifecycleTransactionPhaseDTO::kVerifying:
    case ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown:
    case ComponentLifecycleTransactionPhaseDTO::kReconciling:
    case ComponentLifecycleTransactionPhaseDTO::kSucceeded:
    case ComponentLifecycleTransactionPhaseDTO::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(ComponentLifecycleTerminalResultDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleTerminalResultDTO::kSucceeded:
    case ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect:
    case ComponentLifecycleTerminalResultDTO::kFailedPartialEffect:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(ConfigurationFenceState value) noexcept {
  switch (value) {
    case ConfigurationFenceState::kHeld:
    case ConfigurationFenceState::kReleasePending:
    case ConfigurationFenceState::kReleased:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(DisableHandoffStateDTO value) noexcept {
  switch (value) {
    case DisableHandoffStateDTO::kStopping:
    case DisableHandoffStateDTO::kReady:
    case DisableHandoffStateDTO::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(DisableHandoffAttemptDispositionDTO value) noexcept {
  switch (value) {
    case DisableHandoffAttemptDispositionDTO::kAcceptedExact:
    case DisableHandoffAttemptDispositionDTO::kNotSeen:
    case DisableHandoffAttemptDispositionDTO::kConflicting:
    case DisableHandoffAttemptDispositionDTO::kUncertain:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(DisableHandoffScopeRequestKindDTO value) noexcept {
  switch (value) {
    case DisableHandoffScopeRequestKindDTO::kResolvedComponentScope:
    case DisableHandoffScopeRequestKindDTO::kFullStopRequired:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(DisableHandoffScopeResolutionDTO value) noexcept {
  switch (value) {
    case DisableHandoffScopeResolutionDTO::kExact:
    case DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved:
    case DisableHandoffScopeResolutionDTO::kPromotedToFull:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(DisableHandoffEffectDispositionDTO value) noexcept {
  switch (value) {
    case DisableHandoffEffectDispositionDTO::kAccepted:
    case DisableHandoffEffectDispositionDTO::kProvenNotSeen:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(DisableFenceReleaseAttemptDispositionDTO value) noexcept {
  switch (value) {
    case DisableFenceReleaseAttemptDispositionDTO::kReleasedExact:
    case DisableFenceReleaseAttemptDispositionDTO::kNotReleasedHeld:
    case DisableFenceReleaseAttemptDispositionDTO::kConflicting:
    case DisableFenceReleaseAttemptDispositionDTO::kUncertain:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(DisableFenceReleaseEffectDispositionDTO value) noexcept {
  switch (value) {
    case DisableFenceReleaseEffectDispositionDTO::kReleased:
    case DisableFenceReleaseEffectDispositionDTO::kProvenNotReleasedHeld:
      return true;
  }
  return false;
}

[[nodiscard]] bool context_valid(const PublicRequestContextDTO& context) noexcept {
  return bounded_identity(context.request_id) && bounded_identity(context.request_owner_epoch) &&
         !context.source_label.empty() &&
         context.source_label.size() <= kMaxLifecycleRequestLabelBytes &&
         (!context.correlation_id.has_value() ||
          (!context.correlation_id->empty() &&
           context.correlation_id->size() <= kMaxLifecycleRequestLabelBytes));
}

[[nodiscard]] bool component_requested_state_matches_intent(
    const ComponentLifecycleRequestedStateDTO& requested,
    ComponentLifecycleIntentKindDTO intent) noexcept {
  if (!requested.locally_valid()) {
    return false;
  }
  switch (intent) {
    case ComponentLifecycleIntentKindDTO::kPrepare:
      return requested.presence == ComponentLifecycleRequestedStatePresenceDTO::kResolved &&
             requested.state == ComponentLifecycleStateDTO::kPrepared;
    case ComponentLifecycleIntentKindDTO::kEnable:
      return requested.presence == ComponentLifecycleRequestedStatePresenceDTO::kResolved &&
             requested.state == ComponentLifecycleStateDTO::kActive;
    case ComponentLifecycleIntentKindDTO::kDisable:
      return requested.presence == ComponentLifecycleRequestedStatePresenceDTO::kResolved &&
             requested.state == ComponentLifecycleStateDTO::kDisabled;
    case ComponentLifecycleIntentKindDTO::kRecover:
      return requested.presence == ComponentLifecycleRequestedStatePresenceDTO::kUnresolved ||
             (requested.state.has_value() &&
              (*requested.state == ComponentLifecycleStateDTO::kDeclared ||
               *requested.state == ComponentLifecycleStateDTO::kPrepared ||
               *requested.state == ComponentLifecycleStateDTO::kDisabled));
  }
  return false;
}

[[nodiscard]] bool expected_domains_equal(
    const std::optional<ExpectedControlDomainRevisionDTO>& actual,
    const ExpectedControlDomainRevisionDTO& expected) noexcept {
  return actual.has_value() && *actual == expected;
}

[[nodiscard]] bool active_configuration_bounded(
    const ActiveControlConfigurationDTO& configuration) noexcept {
  if (!bounded_identity(configuration.active_control_configuration_id) ||
      !bounded_identity(configuration.control_domain_id) ||
      !bounded_identity(configuration.control_domain_revision) ||
      !bounded_identity(configuration.activation_set_id) ||
      !bounded_identity(configuration.controller_topology_id) ||
      !bounded_identity(configuration.topology_revision) ||
      !bounded_identity(configuration.source_runtime_unit_id) ||
      !bounded_identity(configuration.source_runtime_epoch) ||
      configuration.kinematic_resource_source_projections.size() >
          kMaxLifecycleKinematicSourceProjections) {
    return false;
  }
  for (const auto& route : configuration.binding_routes) {
    if (!bounded_identity(route.control_binding_id) ||
        !bounded_identity(route.control_binding_revision) ||
        !bounded_identity(route.control_route_id)) {
      return false;
    }
  }
  std::size_t contributing_count = 0U;
  for (const auto& projection : configuration.kinematic_resource_source_projections) {
    if (!bounded_identity(projection.resource_id) ||
        !bounded_identity(projection.provider_instance_id) ||
        !bounded_identity(projection.provider_manifest_id) ||
        !bounded_identity(projection.provider_manifest_revision) ||
        !bounded_identity(projection.state_authority_id) ||
        contributing_count > kMaxLifecycleKinematicContributingBindings -
                                 projection.contributing_control_bindings.size()) {
      return false;
    }
    contributing_count += projection.contributing_control_bindings.size();
    for (const auto& source : projection.contributing_control_bindings) {
      if (!bounded_identity(source.control_binding_id) ||
          !bounded_identity(source.control_binding_revision)) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool fence_scope_bounded(const ConfigurationFenceScopeDTO& scope) noexcept {
  if (scope.controllable_component_ids.size() > kMaxLifecycleDisableDomainUnion ||
      scope.domains.size() > kMaxLifecycleDisableDomainUnion ||
      scope.bindings.size() > kMaxLifecycleStatusBindingUnion ||
      scope.resources.size() > kMaxLifecycleCurrentResources) {
    return false;
  }
  for (const auto& component : scope.controllable_component_ids) {
    if (!bounded_identity(component)) {
      return false;
    }
  }
  for (const auto& domain : scope.domains) {
    if (!bounded_identity(domain.control_domain_id) ||
        (domain.predecessor_control_domain_revision.has_value() &&
         !bounded_identity(*domain.predecessor_control_domain_revision)) ||
        (domain.candidate_control_domain_revision.has_value() &&
         !bounded_identity(*domain.candidate_control_domain_revision))) {
      return false;
    }
  }
  for (const auto& binding : scope.bindings) {
    if (!bounded_identity(binding.control_binding_id) ||
        (binding.predecessor_control_binding_revision.has_value() &&
         !bounded_identity(*binding.predecessor_control_binding_revision)) ||
        (binding.candidate_control_binding_revision.has_value() &&
         !bounded_identity(*binding.candidate_control_binding_revision))) {
      return false;
    }
  }
  return std::all_of(scope.resources.begin(), scope.resources.end(),
                     [](const ResourceId& resource) { return bounded_identity(resource); });
}

[[nodiscard]] bool fence_scope_contains_component(
    const ConfigurationFenceScopeDTO& scope, const ControllableComponentId& component_id) noexcept {
  return std::binary_search(scope.controllable_component_ids.begin(),
                            scope.controllable_component_ids.end(), component_id);
}

[[nodiscard]] bool fence_scope_is_exact_subset(
    const ConfigurationFenceScopeDTO& subset, const ConfigurationFenceScopeDTO& superset) noexcept {
  return std::includes(
             superset.controllable_component_ids.begin(), superset.controllable_component_ids.end(),
             subset.controllable_component_ids.begin(), subset.controllable_component_ids.end()) &&
         exact_rows_are_subset(subset.domains, superset.domains,
                               [](const auto& row) { return row.control_domain_id; }) &&
         exact_rows_are_subset(subset.bindings, superset.bindings,
                               [](const auto& row) { return row.control_binding_id; }) &&
         std::includes(superset.resources.begin(), superset.resources.end(),
                       subset.resources.begin(), subset.resources.end());
}

[[nodiscard]] bool fence_scope_membership_is_subset(
    const ConfigurationFenceScopeDTO& subset, const ConfigurationFenceScopeDTO& superset) noexcept {
  const auto row_ids_are_subset = [](const auto& left, const auto& right, const auto& key) {
    return std::all_of(left.begin(), left.end(), [&](const auto& item) {
      const auto found = std::lower_bound(right.begin(), right.end(), key(item),
                                          [&](const auto& candidate, const auto& requested_key) {
                                            return key(candidate) < requested_key;
                                          });
      return found != right.end() && key(*found) == key(item);
    });
  };
  return std::includes(
             superset.controllable_component_ids.begin(), superset.controllable_component_ids.end(),
             subset.controllable_component_ids.begin(), subset.controllable_component_ids.end()) &&
         row_ids_are_subset(subset.domains, superset.domains,
                            [](const auto& row) { return row.control_domain_id; }) &&
         row_ids_are_subset(subset.bindings, superset.bindings,
                            [](const auto& row) { return row.control_binding_id; }) &&
         std::includes(superset.resources.begin(), superset.resources.end(),
                       subset.resources.begin(), subset.resources.end());
}

[[nodiscard]] bool domain_requested_postcondition_satisfied(
    const ComponentLifecycleDomainStatusDTO& domain) noexcept {
  if (domain.requested_state.presence != ComponentLifecycleRequestedStatePresenceDTO::kResolved ||
      !domain.requested_state.state.has_value() || !domain.current_state.has_value() ||
      *domain.current_state != *domain.requested_state.state) {
    return false;
  }
  return !domain.reconciliation_evidence.has_value() ||
         (domain.reconciliation_evidence->observed_state == *domain.current_state &&
          domain.reconciliation_evidence->postcondition_result ==
              LifecyclePostconditionResultDTO::kSatisfied);
}

[[nodiscard]] bool same_fence_identity_and_scope(
    const ConfigurationFenceHandleDTO& left, const ConfigurationFenceHandleDTO& right) noexcept {
  return left.configuration_fence_id == right.configuration_fence_id &&
         left.configuration_fence_generation == right.configuration_fence_generation &&
         left.lifecycle_transaction_id == right.lifecycle_transaction_id &&
         left.lifecycle_transaction_generation == right.lifecycle_transaction_generation &&
         left.controllable_component_id == right.controllable_component_id &&
         left.scope == right.scope && left.reason == right.reason;
}

}  // namespace

bool CCLCEffectAttemptDTO::locally_valid() const noexcept {
  if (request_kind != CCLCRequestKindDTO::kComponentLifecycle ||
      !bounded_identity(lifecycle_transaction_id) || !lifecycle_transaction_generation.nonzero() ||
      effect_sequence == 0U ||
      (state != CCLCEffectAttemptStateDTO::kPrepared &&
       state != CCLCEffectAttemptStateDTO::kCompleted)) {
    return false;
  }
  const bool completed = state == CCLCEffectAttemptStateDTO::kCompleted;
  switch (effect_kind) {
    case CCLCEffectKindDTO::kBrandLifecyclePrimitive:
      return lifecycle_primitive_intent.has_value() &&
             lifecycle_primitive_intent->locally_valid() &&
             lifecycle_primitive_intent->lifecycle_transaction_id == lifecycle_transaction_id &&
             lifecycle_primitive_intent->lifecycle_transaction_generation ==
                 lifecycle_transaction_generation &&
             lifecycle_primitive_result.has_value() == completed &&
             (!completed || lifecycle_primitive_result->matches(*lifecycle_primitive_intent)) &&
             !disable_handoff_request.has_value() && !disable_handoff_result.has_value() &&
             !disable_fence_release_request.has_value() &&
             !disable_fence_release_result.has_value();
    case CCLCEffectKindDTO::kKernelDisableHandoff:
      return disable_handoff_request.has_value() && disable_handoff_request->locally_valid() &&
             disable_handoff_request->lifecycle_transaction_id == lifecycle_transaction_id &&
             disable_handoff_request->lifecycle_transaction_generation ==
                 lifecycle_transaction_generation &&
             disable_handoff_result.has_value() == completed &&
             (!completed || (disable_handoff_result->locally_valid() &&
                             disable_handoff_result->request == *disable_handoff_request)) &&
             !lifecycle_primitive_intent.has_value() && !lifecycle_primitive_result.has_value() &&
             !disable_fence_release_request.has_value() &&
             !disable_fence_release_result.has_value();
    case CCLCEffectKindDTO::kKernelDisableFenceRelease:
      return disable_fence_release_request.has_value() &&
             disable_fence_release_request->locally_valid() &&
             disable_fence_release_request->lifecycle_transaction_id == lifecycle_transaction_id &&
             disable_fence_release_request->lifecycle_transaction_generation ==
                 lifecycle_transaction_generation &&
             disable_fence_release_result.has_value() == completed &&
             (!completed ||
              (disable_fence_release_result->locally_valid() &&
               disable_fence_release_result->request == *disable_fence_release_request)) &&
             !lifecycle_primitive_intent.has_value() && !lifecycle_primitive_result.has_value() &&
             !disable_handoff_request.has_value() && !disable_handoff_result.has_value();
  }
  return false;
}

bool CCLCTransactionEffectSequenceWatermarkDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_transaction_id) && lifecycle_transaction_generation.nonzero();
}

bool CCLCJournalRecoveryDTO::locally_valid() const noexcept {
  if (component_watermarks.size() > kMaximumCCLCComponentWatermarks ||
      interrupted_transactions.size() > kMaximumCCLCActiveTransactions ||
      effect_sequence_watermarks.size() > kMaximumCCLCActiveTransactions ||
      unresolved_effect_attempts.size() > kMaximumCCLCActiveTransactions ||
      active_configuration_watermarks.size() > kMaximumCCLCActiveConfigurationWatermarks ||
      current_active_control_configurations.size() > kMaximumCCLCActiveConfigurationWatermarks ||
      interrupted_route_selections.size() > kMaximumCCLCActiveTransactions ||
      route_effect_sequence_watermarks.size() > kMaximumCCLCActiveTransactions ||
      unresolved_route_effect_attempts.size() > kMaximumCCLCActiveTransactions ||
      interrupted_transactions.size() + interrupted_route_selections.size() >
          kMaximumCCLCActiveTransactions ||
      !std::is_sorted(component_watermarks.begin(), component_watermarks.end(),
                      [](const ComponentLifecycleGenerationWatermarkDTO& lhs,
                         const ComponentLifecycleGenerationWatermarkDTO& rhs) noexcept {
                        return lhs.controllable_component_id < rhs.controllable_component_id;
                      }) ||
      !std::is_sorted(interrupted_transactions.begin(), interrupted_transactions.end(),
                      [](const ComponentLifecycleStatusDTO& lhs,
                         const ComponentLifecycleStatusDTO& rhs) noexcept {
                        return lhs.acceptance.lifecycle_transaction_generation <
                               rhs.acceptance.lifecycle_transaction_generation;
                      }) ||
      !std::is_sorted(effect_sequence_watermarks.begin(), effect_sequence_watermarks.end(),
                      [](const CCLCTransactionEffectSequenceWatermarkDTO& lhs,
                         const CCLCTransactionEffectSequenceWatermarkDTO& rhs) noexcept {
                        return lhs.lifecycle_transaction_generation <
                               rhs.lifecycle_transaction_generation;
                      }) ||
      !std::is_sorted(
          unresolved_effect_attempts.begin(), unresolved_effect_attempts.end(),
          [](const CCLCEffectAttemptDTO& lhs, const CCLCEffectAttemptDTO& rhs) noexcept {
            return std::tie(lhs.lifecycle_transaction_generation, lhs.effect_sequence) <
                   std::tie(rhs.lifecycle_transaction_generation, rhs.effect_sequence);
          }) ||
      !std::is_sorted(
          active_configuration_watermarks.begin(), active_configuration_watermarks.end(),
          [](const ControlDomainActiveConfigurationGenerationWatermarkDTO& lhs,
             const ControlDomainActiveConfigurationGenerationWatermarkDTO& rhs) noexcept {
            return lhs.control_domain_id < rhs.control_domain_id;
          }) ||
      !std::is_sorted(current_active_control_configurations.begin(),
                      current_active_control_configurations.end(),
                      [](const ActiveControlConfigurationDTO& lhs,
                         const ActiveControlConfigurationDTO& rhs) noexcept {
                        return lhs.control_domain_id < rhs.control_domain_id;
                      }) ||
      !std::is_sorted(
          interrupted_route_selections.begin(), interrupted_route_selections.end(),
          [](const RouteSelectionStatusDTO& lhs, const RouteSelectionStatusDTO& rhs) noexcept {
            return lhs.acceptance.route_selection_transaction_generation <
                   rhs.acceptance.route_selection_transaction_generation;
          }) ||
      !std::is_sorted(route_effect_sequence_watermarks.begin(),
                      route_effect_sequence_watermarks.end(),
                      [](const CCLCRouteEffectSequenceWatermarkDTO& lhs,
                         const CCLCRouteEffectSequenceWatermarkDTO& rhs) noexcept {
                        return lhs.route_selection_transaction_generation <
                               rhs.route_selection_transaction_generation;
                      }) ||
      !std::is_sorted(
          unresolved_route_effect_attempts.begin(), unresolved_route_effect_attempts.end(),
          [](const CCLCRouteEffectAttemptDTO& lhs, const CCLCRouteEffectAttemptDTO& rhs) noexcept {
            return std::tie(lhs.route_selection_transaction_generation, lhs.effect_sequence) <
                   std::tie(rhs.route_selection_transaction_generation, rhs.effect_sequence);
          })) {
    return false;
  }

  for (std::size_t index = 0U; index < component_watermarks.size(); ++index) {
    if (!component_watermarks[index].locally_valid() ||
        (index != 0U && component_watermarks[index - 1U].controllable_component_id ==
                            component_watermarks[index].controllable_component_id)) {
      return false;
    }
  }

  std::uint64_t prior_transaction_generation = 0U;
  if (effect_sequence_watermarks.size() != interrupted_transactions.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < interrupted_transactions.size(); ++index) {
    const auto& status = interrupted_transactions[index];
    const auto generation = status.acceptance.lifecycle_transaction_generation.value();
    if (!status.locally_valid() || status.terminal_result.has_value() || generation == 0U ||
        generation <= prior_transaction_generation ||
        generation > highest_lifecycle_transaction_generation) {
      return false;
    }
    const auto& watermark = effect_sequence_watermarks[index];
    if (!watermark.locally_valid() ||
        watermark.lifecycle_transaction_id != status.acceptance.lifecycle_transaction_id ||
        watermark.lifecycle_transaction_generation !=
            status.acceptance.lifecycle_transaction_generation) {
      return false;
    }
    prior_transaction_generation = generation;
  }
  std::set<std::pair<LifecycleTransactionId, std::uint64_t>> attempt_ids;
  std::set<LifecycleTransactionId> unresolved_transactions;
  for (const auto& attempt : unresolved_effect_attempts) {
    const auto status = std::find_if(
        interrupted_transactions.begin(), interrupted_transactions.end(), [&](const auto& row) {
          return row.acceptance.lifecycle_transaction_id == attempt.lifecycle_transaction_id &&
                 row.acceptance.lifecycle_transaction_generation ==
                     attempt.lifecycle_transaction_generation;
        });
    if (!attempt.locally_valid() || attempt.state != CCLCEffectAttemptStateDTO::kPrepared ||
        status == interrupted_transactions.end() ||
        !attempt_ids.insert({attempt.lifecycle_transaction_id, attempt.effect_sequence}).second ||
        !unresolved_transactions.insert(attempt.lifecycle_transaction_id).second ||
        attempt.lifecycle_transaction_generation.value() >
            highest_lifecycle_transaction_generation ||
        std::none_of(effect_sequence_watermarks.begin(), effect_sequence_watermarks.end(),
                     [&](const auto& watermark) {
                       return watermark.lifecycle_transaction_id ==
                                  attempt.lifecycle_transaction_id &&
                              watermark.lifecycle_transaction_generation ==
                                  attempt.lifecycle_transaction_generation &&
                              watermark.highest_effect_sequence == attempt.effect_sequence;
                     }) ||
        status->accepted_intent.controllable_component_id !=
            (attempt.effect_kind == CCLCEffectKindDTO::kBrandLifecyclePrimitive
                 ? attempt.lifecycle_primitive_intent->controllable_component_id
             : attempt.effect_kind == CCLCEffectKindDTO::kKernelDisableHandoff
                 ? attempt.disable_handoff_request->controllable_component_id
                 : attempt.disable_fence_release_request->controllable_component_id)) {
      return false;
    }
    if (attempt.effect_kind == CCLCEffectKindDTO::kBrandLifecyclePrimitive) {
      const auto& primitive = *attempt.lifecycle_primitive_intent;
      if (std::none_of(status->domains.begin(), status->domains.end(), [&](const auto& domain) {
            return domain.control_domain_id == primitive.control_domain_id &&
                   domain.resolved_provider_occurrence.has_value() &&
                   domain.resolved_provider_occurrence->provider_instance_id ==
                       primitive.provider_instance_id &&
                   domain.resolved_provider_occurrence->provider_manifest_id ==
                       primitive.provider_manifest_id &&
                   domain.resolved_provider_occurrence->provider_manifest_revision ==
                       primitive.provider_manifest_revision &&
                   domain.resolved_provider_occurrence->provider_runtime_unit_id ==
                       primitive.provider_runtime_unit_id &&
                   domain.resolved_provider_occurrence->provider_runtime_epoch ==
                       primitive.provider_runtime_epoch;
          })) {
        return false;
      }
    } else if (status->accepted_intent.intent != ComponentLifecycleIntentKindDTO::kDisable) {
      return false;
    } else if (attempt.effect_kind == CCLCEffectKindDTO::kKernelDisableHandoff) {
      if (status->disable_handoff.has_value() || status->configuration_fence.has_value()) {
        return false;
      }
    } else {
      const auto& release = *attempt.disable_fence_release_request;
      if (status->disable_handoff != std::optional(release.ready_handoff) ||
          !status->configuration_fence.has_value() ||
          status->configuration_fence->state != ConfigurationFenceState::kHeld ||
          status->disable_fence_released.has_value()) {
        return false;
      }
    }
  }

  for (std::size_t index = 0U; index < active_configuration_watermarks.size(); ++index) {
    if (!active_configuration_watermarks[index].locally_valid() ||
        (index != 0U && active_configuration_watermarks[index - 1U].control_domain_id ==
                            active_configuration_watermarks[index].control_domain_id)) {
      return false;
    }
  }

  for (std::size_t index = 0U; index < current_active_control_configurations.size(); ++index) {
    const auto& configuration = current_active_control_configurations[index];
    const auto watermark =
        std::lower_bound(active_configuration_watermarks.begin(),
                         active_configuration_watermarks.end(), configuration.control_domain_id,
                         [](const ControlDomainActiveConfigurationGenerationWatermarkDTO& row,
                            const ControlDomainId& domain_id) noexcept {
                           return row.control_domain_id < domain_id;
                         });
    if (!configuration.locally_valid() ||
        (index != 0U && current_active_control_configurations[index - 1U].control_domain_id ==
                            configuration.control_domain_id) ||
        watermark == active_configuration_watermarks.end() ||
        watermark->control_domain_id != configuration.control_domain_id ||
        configuration.active_control_configuration_generation.value() >
            watermark->active_control_configuration_generation.value()) {
      return false;
    }
  }

  if (route_effect_sequence_watermarks.size() != interrupted_route_selections.size()) {
    return false;
  }
  std::uint64_t prior_route_generation = 0U;
  for (std::size_t index = 0U; index < interrupted_route_selections.size(); ++index) {
    const auto& status = interrupted_route_selections[index];
    const auto generation = status.acceptance.route_selection_transaction_generation.value();
    const auto& watermark = route_effect_sequence_watermarks[index];
    if (!status.locally_valid() || status.terminal_result.has_value() || generation == 0U ||
        generation <= prior_route_generation ||
        generation > highest_route_selection_transaction_generation || !watermark.locally_valid() ||
        watermark.route_selection_transaction_id !=
            status.acceptance.route_selection_transaction_id ||
        watermark.route_selection_transaction_generation !=
            status.acceptance.route_selection_transaction_generation ||
        std::none_of(active_configuration_watermarks.begin(), active_configuration_watermarks.end(),
                     [&](const auto& row) {
                       return row.control_domain_id ==
                                  status.acceptance.desired_configuration.control_domain_id &&
                              row.active_control_configuration_generation.value() >=
                                  status.acceptance
                                      .candidate_active_control_configuration_generation.value();
                     })) {
      return false;
    }
    prior_route_generation = generation;
  }

  std::set<std::pair<RouteSelectionTransactionId, std::uint64_t>> route_attempt_ids;
  std::set<RouteSelectionTransactionId> unresolved_route_transactions;
  for (const auto& attempt : unresolved_route_effect_attempts) {
    const auto status =
        std::find_if(interrupted_route_selections.begin(), interrupted_route_selections.end(),
                     [&](const auto& row) {
                       return row.acceptance.route_selection_transaction_id ==
                                  attempt.route_selection_transaction_id &&
                              row.acceptance.route_selection_transaction_generation ==
                                  attempt.route_selection_transaction_generation;
                     });
    if (!attempt.locally_valid() || attempt.state != CCLCRouteEffectAttemptStateDTO::kPrepared ||
        status == interrupted_route_selections.end() ||
        !route_attempt_ids.insert({attempt.route_selection_transaction_id, attempt.effect_sequence})
             .second ||
        !unresolved_route_transactions.insert(attempt.route_selection_transaction_id).second ||
        attempt.route_selection_transaction_generation.value() >
            highest_route_selection_transaction_generation ||
        std::none_of(route_effect_sequence_watermarks.begin(),
                     route_effect_sequence_watermarks.end(), [&](const auto& watermark) {
                       return watermark.route_selection_transaction_id ==
                                  attempt.route_selection_transaction_id &&
                              watermark.route_selection_transaction_generation ==
                                  attempt.route_selection_transaction_generation &&
                              watermark.highest_effect_sequence == attempt.effect_sequence;
                     })) {
      return false;
    }
  }
  return true;
}

bool CCLCLifecycleRequestResultDTO::locally_valid() const noexcept {
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
  ComponentLifecycleAcceptedDTO persisted = status->acceptance;
  persisted.replay_status = replay_status;
  return persisted == *acceptance;
}

bool ComponentLifecycleRequestedStateDTO::locally_valid() const noexcept {
  switch (presence) {
    case ComponentLifecycleRequestedStatePresenceDTO::kUnresolved:
      return !state.has_value();
    case ComponentLifecycleRequestedStatePresenceDTO::kResolved:
      return state.has_value() && known(*state);
  }
  return false;
}

bool ExpectedActiveControlConfigurationDTO::locally_valid() const noexcept {
  switch (presence) {
    case ExpectedActiveControlConfigurationPresenceDTO::kAbsent:
      return !active_control_configuration_id.has_value() &&
             !active_control_configuration_generation.has_value();
    case ExpectedActiveControlConfigurationPresenceDTO::kPresent:
      return active_control_configuration_id.has_value() &&
             bounded_identity(*active_control_configuration_id) &&
             active_control_configuration_generation.has_value() &&
             active_control_configuration_generation->nonzero();
  }
  return false;
}

bool ExpectedControlBindingRevisionDTO::locally_valid() const noexcept {
  return bounded_identity(control_binding_id) && bounded_identity(control_binding_revision) &&
         bounded_identity(control_route_id);
}

bool ExpectedControlDomainRevisionDTO::locally_valid() const noexcept {
  if (!bounded_identity(control_domain_id) || !bounded_identity(control_domain_revision) ||
      !bounded_identity(controller_topology_id) || !bounded_identity(topology_revision) ||
      !expected_active_control_configuration.locally_valid() || expected_control_bindings.empty() ||
      expected_control_bindings.size() > kMaxLifecycleExpectedBindings) {
    return false;
  }
  const auto less = [](const ExpectedControlBindingRevisionDTO& left,
                       const ExpectedControlBindingRevisionDTO& right) {
    return left.control_binding_id < right.control_binding_id;
  };
  return sorted_unique(expected_control_bindings, less) &&
         std::all_of(expected_control_bindings.begin(), expected_control_bindings.end(),
                     [](const ExpectedControlBindingRevisionDTO& binding) {
                       return binding.locally_valid();
                     });
}

bool ComponentLifecycleIntentDTO::locally_valid() const noexcept {
  if (!context_valid(request_context) || !bounded_identity(controllable_component_id) ||
      !known(intent) || !bounded_identity(expected_graph_revision) ||
      !expected_component_lifecycle_generation.nonzero() || expected_control_domains.empty() ||
      expected_control_domains.size() > kMaxLifecycleExpectedDomains ||
      (reason.has_value() && reason->size() > kMaxLifecycleReasonBytes) ||
      !attribution.locally_valid() ||
      (attribution.principal_id.has_value() &&
       attribution.principal_id->size() > kMaxLifecycleRequestLabelBytes)) {
    return false;
  }
  const auto less = [](const ExpectedControlDomainRevisionDTO& left,
                       const ExpectedControlDomainRevisionDTO& right) {
    return left.control_domain_id < right.control_domain_id;
  };
  if (!sorted_unique(expected_control_domains, less)) {
    return false;
  }
  std::size_t binding_count = 0U;
  for (const auto& domain : expected_control_domains) {
    if (!domain.locally_valid() ||
        binding_count > kMaxLifecycleExpectedBindings - domain.expected_control_bindings.size()) {
      return false;
    }
    binding_count += domain.expected_control_bindings.size();
  }
  return true;
}

bool ComponentLifecycleIntentDTO::replay_equal(
    const ComponentLifecycleIntentDTO& other) const noexcept {
  return request_context == other.request_context &&
         controllable_component_id == other.controllable_component_id && intent == other.intent &&
         expected_graph_revision == other.expected_graph_revision &&
         expected_component_lifecycle_generation == other.expected_component_lifecycle_generation &&
         expected_control_domains == other.expected_control_domains && reason == other.reason;
}

bool ComponentLifecycleAcceptedDTO::locally_valid() const noexcept {
  return bounded_identity(request_id) && IsAcceptedRequestReplayStatus(replay_status) &&
         bounded_identity(lifecycle_transaction_id) && lifecycle_transaction_generation.nonzero() &&
         component_lifecycle_generation.nonzero() && accepted_at_ns >= 0 &&
         timeout_duration_ns > 0 && timeout_duration_ns <= kMaxLifecycleTransactionTimeoutNs;
}

bool LifecycleFactValueDTO::locally_valid() const noexcept {
  switch (kind) {
    case LifecycleFactValueKindDTO::kUnknown:
      return !boolean_value.has_value() && !integer_value.has_value() && !token_value.has_value();
    case LifecycleFactValueKindDTO::kBoolean:
      return boolean_value.has_value() && !integer_value.has_value() && !token_value.has_value();
    case LifecycleFactValueKindDTO::kInteger:
      return !boolean_value.has_value() && integer_value.has_value() && !token_value.has_value();
    case LifecycleFactValueKindDTO::kToken:
      return !boolean_value.has_value() && !integer_value.has_value() && token_value.has_value() &&
             portable_token(*token_value);
  }
  return false;
}

bool LifecycleFactDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_fact_id) && value.locally_valid();
}

bool LifecycleFactPredicateDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_fact_id) && expected_value.locally_valid() &&
         expected_value.kind != LifecycleFactValueKindDTO::kUnknown;
}

bool LifecyclePredicateDTO::locally_valid() const noexcept {
  if (allowed_states.empty() ||
      allowed_states.size() >
          static_cast<std::size_t>(ComponentLifecycleStateDTO::kDisabled) + 1U ||
      fact_predicates.size() > kMaxLifecyclePredicateFacts ||
      !std::is_sorted(allowed_states.begin(), allowed_states.end()) ||
      std::adjacent_find(allowed_states.begin(), allowed_states.end()) != allowed_states.end() ||
      std::any_of(allowed_states.begin(), allowed_states.end(),
                  [](ComponentLifecycleStateDTO state) { return !known(state); })) {
    return false;
  }
  const auto less = [](const LifecycleFactPredicateDTO& left,
                       const LifecycleFactPredicateDTO& right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  };
  return sorted_unique(fact_predicates, less) &&
         std::all_of(
             fact_predicates.begin(), fact_predicates.end(),
             [](const LifecycleFactPredicateDTO& predicate) { return predicate.locally_valid(); });
}

LifecyclePostconditionResultDTO EvaluateLifecyclePredicate(
    const LifecyclePredicateDTO& predicate,
    const BrandLifecycleObservationDTO& observation) noexcept {
  if (!predicate.locally_valid() || !observation.locally_valid()) {
    return LifecyclePostconditionResultDTO::kInconclusive;
  }
  if (!std::binary_search(predicate.allowed_states.begin(), predicate.allowed_states.end(),
                          observation.observed_state)) {
    return LifecyclePostconditionResultDTO::kNotSatisfied;
  }
  for (const auto& expected : predicate.fact_predicates) {
    const auto fact = std::lower_bound(
        observation.facts.begin(), observation.facts.end(), expected.lifecycle_fact_id,
        [](const LifecycleFactDTO& candidate, const LifecycleFactId& id) {
          return candidate.lifecycle_fact_id < id;
        });
    if (fact == observation.facts.end() || fact->lifecycle_fact_id != expected.lifecycle_fact_id ||
        fact->value.kind == LifecycleFactValueKindDTO::kUnknown) {
      return LifecyclePostconditionResultDTO::kInconclusive;
    }
    if (fact->value != expected.expected_value) {
      return LifecyclePostconditionResultDTO::kNotSatisfied;
    }
  }
  return LifecyclePostconditionResultDTO::kSatisfied;
}

bool LifecyclePrimitiveTransitionDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_primitive_id) && eligibility_predicate.locally_valid() &&
         postcondition_predicate.locally_valid();
}

bool LifecycleIntentPlanDTO::locally_valid() const noexcept {
  if (!known(intent) || !known(requested_state) || !requested_postcondition.locally_valid() ||
      timeout_duration_ns <= 0 || timeout_duration_ns > kMaxLifecycleTransactionTimeoutNs ||
      transitions.size() > kMaxLifecycleTransitionsPerPlan ||
      (recover_target_eligibility.has_value() && !recover_target_eligibility->locally_valid())) {
    return false;
  }
  const bool target_valid = [&]() {
    switch (intent) {
      case ComponentLifecycleIntentKindDTO::kPrepare:
        return requested_state == ComponentLifecycleStateDTO::kPrepared &&
               !recover_target_eligibility.has_value();
      case ComponentLifecycleIntentKindDTO::kEnable:
        return requested_state == ComponentLifecycleStateDTO::kActive &&
               !recover_target_eligibility.has_value();
      case ComponentLifecycleIntentKindDTO::kDisable:
        return requested_state == ComponentLifecycleStateDTO::kDisabled &&
               !recover_target_eligibility.has_value();
      case ComponentLifecycleIntentKindDTO::kRecover:
        return recover_target_eligibility.has_value() &&
               (requested_state == ComponentLifecycleStateDTO::kDeclared ||
                requested_state == ComponentLifecycleStateDTO::kPrepared ||
                requested_state == ComponentLifecycleStateDTO::kDisabled);
    }
    return false;
  }();
  if (!target_valid) {
    return false;
  }
  std::set<LifecyclePrimitiveId> primitive_ids;
  return std::all_of(transitions.begin(), transitions.end(),
                     [&](const LifecyclePrimitiveTransitionDTO& transition) {
                       return transition.locally_valid() &&
                              primitive_ids.insert(transition.lifecycle_primitive_id).second;
                     });
}

bool BrandLifecyclePlanCatalogDTO::locally_valid() const noexcept {
  if (!bounded_identity(provider_manifest_id) || !bounded_identity(provider_manifest_revision) ||
      !bounded_identity(catalog_digest) || plans.empty() ||
      plans.size() > kMaxLifecycleCatalogPlans) {
    return false;
  }
  const auto less = [](const LifecycleIntentPlanDTO& left, const LifecycleIntentPlanDTO& right) {
    if (left.intent != right.intent) {
      return left.intent < right.intent;
    }
    return left.requested_state < right.requested_state;
  };
  if (!sorted_unique(plans, less)) {
    return false;
  }
  std::size_t transition_count = 0U;
  std::size_t recover_count = 0U;
  for (const auto& plan : plans) {
    if (!plan.locally_valid() ||
        transition_count > kMaxLifecycleTransitionsPerCatalog - plan.transitions.size()) {
      return false;
    }
    transition_count += plan.transitions.size();
    if (plan.intent == ComponentLifecycleIntentKindDTO::kRecover && ++recover_count > 3U) {
      return false;
    }
  }
  return true;
}

bool BrandLifecycleObservationQueryDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_transaction_id) && lifecycle_transaction_generation.nonzero() &&
         bounded_identity(controllable_component_id) && bounded_identity(control_domain_id) &&
         bounded_identity(control_domain_revision) && bounded_identity(provider_instance_id) &&
         bounded_identity(provider_manifest_id) && bounded_identity(provider_manifest_revision) &&
         bounded_identity(provider_runtime_unit_id) && bounded_identity(provider_runtime_epoch);
}

bool BrandLifecycleObservationDTO::locally_valid() const noexcept {
  if (!BrandLifecycleObservationQueryDTO{
          lifecycle_transaction_id, lifecycle_transaction_generation, controllable_component_id,
          control_domain_id, control_domain_revision, provider_instance_id, provider_manifest_id,
          provider_manifest_revision, provider_runtime_unit_id, provider_runtime_epoch}
           .locally_valid() ||
      source_sequence == 0U || observation_generation == 0U || source_timestamp_ns < 0 ||
      received_timestamp_ns < 0 || fresh_until_ns < 0 ||
      source_timestamp_ns > received_timestamp_ns || received_timestamp_ns >= fresh_until_ns ||
      !known(observed_state) || facts.size() > kMaxLifecycleFacts) {
    return false;
  }
  const auto less = [](const LifecycleFactDTO& left, const LifecycleFactDTO& right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  };
  return sorted_unique(facts, less) &&
         std::all_of(facts.begin(), facts.end(),
                     [](const LifecycleFactDTO& fact) { return fact.locally_valid(); });
}

bool BrandLifecycleObservationDTO::matches(
    const BrandLifecycleObservationQueryDTO& query) const noexcept {
  return locally_valid() && query.locally_valid() &&
         lifecycle_transaction_id == query.lifecycle_transaction_id &&
         lifecycle_transaction_generation == query.lifecycle_transaction_generation &&
         controllable_component_id == query.controllable_component_id &&
         control_domain_id == query.control_domain_id &&
         control_domain_revision == query.control_domain_revision &&
         provider_instance_id == query.provider_instance_id &&
         provider_manifest_id == query.provider_manifest_id &&
         provider_manifest_revision == query.provider_manifest_revision &&
         provider_runtime_unit_id == query.provider_runtime_unit_id &&
         provider_runtime_epoch == query.provider_runtime_epoch;
}

bool BrandLifecyclePrimitiveIntentDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_transaction_id) && lifecycle_transaction_generation.nonzero() &&
         bounded_identity(controllable_component_id) && bounded_identity(control_domain_id) &&
         bounded_identity(control_domain_revision) && bounded_identity(provider_instance_id) &&
         bounded_identity(provider_manifest_id) && bounded_identity(provider_manifest_revision) &&
         bounded_identity(provider_runtime_unit_id) && bounded_identity(provider_runtime_epoch) &&
         bounded_identity(lifecycle_primitive_id) &&
         bounded_identity(lifecycle_primitive_attempt_id) &&
         expected_observation_generation != 0U && remaining_duration_ns > 0 &&
         remaining_duration_ns <= kMaxLifecycleTransactionTimeoutNs;
}

bool BrandLifecyclePrimitiveResultDTO::locally_valid() const noexcept {
  if (bounded_identity(lifecycle_transaction_id) && lifecycle_transaction_generation.nonzero() &&
      bounded_identity(controllable_component_id) && bounded_identity(control_domain_id) &&
      bounded_identity(control_domain_revision) && bounded_identity(provider_instance_id) &&
      bounded_identity(provider_manifest_id) && bounded_identity(provider_manifest_revision) &&
      bounded_identity(provider_runtime_unit_id) && bounded_identity(provider_runtime_epoch) &&
      bounded_identity(lifecycle_primitive_id) &&
      bounded_identity(lifecycle_primitive_attempt_id) && expected_observation_generation != 0U &&
      known(dispatch_state) && known(effect) && error_valid(error) &&
      (!known_result.has_value() || known(*known_result))) {
    switch (dispatch_state) {
      case LifecyclePrimitiveDispatchStateDTO::kNotDispatched:
        return effect == LifecyclePrimitiveEffectDTO::kNone && !known_result.has_value() &&
               error.has_value();
      case LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown:
        return known_result.has_value() &&
               ((*known_result == LifecyclePrimitiveKnownResultDTO::kSucceeded &&
                 !error.has_value()) ||
                (*known_result == LifecyclePrimitiveKnownResultDTO::kFailed && error.has_value()));
      case LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain:
        return effect != LifecyclePrimitiveEffectDTO::kNone && !known_result.has_value() &&
               error.has_value();
    }
  }
  return false;
}

bool BrandLifecyclePrimitiveResultDTO::matches(
    const BrandLifecyclePrimitiveIntentDTO& intent) const noexcept {
  return locally_valid() && intent.locally_valid() &&
         lifecycle_transaction_id == intent.lifecycle_transaction_id &&
         lifecycle_transaction_generation == intent.lifecycle_transaction_generation &&
         controllable_component_id == intent.controllable_component_id &&
         control_domain_id == intent.control_domain_id &&
         control_domain_revision == intent.control_domain_revision &&
         provider_instance_id == intent.provider_instance_id &&
         provider_manifest_id == intent.provider_manifest_id &&
         provider_manifest_revision == intent.provider_manifest_revision &&
         provider_runtime_unit_id == intent.provider_runtime_unit_id &&
         provider_runtime_epoch == intent.provider_runtime_epoch &&
         lifecycle_primitive_id == intent.lifecycle_primitive_id &&
         lifecycle_primitive_attempt_id == intent.lifecycle_primitive_attempt_id &&
         expected_observation_generation == intent.expected_observation_generation;
}

bool LifecycleReconciliationEvidenceDTO::locally_valid() const noexcept {
  return bounded_identity(provider_instance_id) && bounded_identity(provider_manifest_id) &&
         bounded_identity(provider_manifest_revision) &&
         bounded_identity(provider_runtime_unit_id) && bounded_identity(provider_runtime_epoch) &&
         source_sequence != 0U && observation_generation != 0U && known(observed_state) &&
         known(postcondition_result) && observed_at_ns >= 0;
}

bool ResolvedLifecycleControlDomainScopeDTO::locally_valid() const noexcept {
  if (!bounded_identity(control_domain_id) || !bounded_identity(control_domain_revision) ||
      !bounded_identity(controller_topology_id) || !bounded_identity(topology_revision) ||
      current_control_bindings.empty() ||
      current_control_bindings.size() > kMaxLifecycleResolvedBindings ||
      current_resources.empty() || current_resources.size() > kMaxLifecycleCurrentResources) {
    return false;
  }
  const auto binding_less = [](const ExpectedControlBindingRevisionDTO& left,
                               const ExpectedControlBindingRevisionDTO& right) {
    return left.control_binding_id < right.control_binding_id;
  };
  if (!sorted_unique(current_control_bindings, binding_less) ||
      !std::all_of(current_control_bindings.begin(), current_control_bindings.end(),
                   [](const ExpectedControlBindingRevisionDTO& binding) {
                     return binding.locally_valid();
                   }) ||
      !std::is_sorted(current_resources.begin(), current_resources.end()) ||
      std::adjacent_find(current_resources.begin(), current_resources.end()) !=
          current_resources.end() ||
      std::any_of(current_resources.begin(), current_resources.end(),
                  [](const ResourceId& resource) { return !bounded_identity(resource); })) {
    return false;
  }
  if (!active_control_configuration.has_value()) {
    return true;
  }
  const auto& configuration = *active_control_configuration;
  if (!configuration.locally_valid() || !active_configuration_bounded(configuration) ||
      configuration.control_domain_id != control_domain_id ||
      configuration.control_domain_revision != control_domain_revision ||
      configuration.controller_topology_id != controller_topology_id ||
      configuration.topology_revision != topology_revision ||
      configuration.binding_routes.size() != current_control_bindings.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < current_control_bindings.size(); ++index) {
    const auto& current = current_control_bindings[index];
    const auto& configured = configuration.binding_routes[index];
    if (current.control_binding_id != configured.control_binding_id ||
        current.control_binding_revision != configured.control_binding_revision ||
        current.control_route_id != configured.control_route_id) {
      return false;
    }
  }
  return true;
}

bool ResolvedLifecycleProviderOccurrenceDTO::locally_valid() const noexcept {
  return bounded_identity(control_domain_id) && bounded_identity(control_domain_revision) &&
         bounded_identity(provider_instance_id) && bounded_identity(provider_manifest_id) &&
         bounded_identity(provider_manifest_revision) &&
         bounded_identity(provider_runtime_unit_id) && bounded_identity(provider_runtime_epoch) &&
         bounded_identity(lifecycle_plan_catalog_digest);
}

bool ComponentLifecycleCurrentSnapshotDTO::locally_valid() const noexcept {
  if (!bounded_identity(snapshot_digest) || !bounded_identity(controllable_component_id) ||
      !component_lifecycle_generation.nonzero() || !known(component_lifecycle_state) ||
      !active_publication_receipt.valid() || domains.empty() ||
      domains.size() > kMaxLifecycleResolvedDomains ||
      provider_occurrences.size() != domains.size() ||
      provider_occurrences.size() > kMaxLifecycleProviderOccurrences) {
    return false;
  }
  const auto domain_less = [](const ResolvedLifecycleControlDomainScopeDTO& left,
                              const ResolvedLifecycleControlDomainScopeDTO& right) {
    return left.control_domain_id < right.control_domain_id;
  };
  const auto provider_less = [](const ResolvedLifecycleProviderOccurrenceDTO& left,
                                const ResolvedLifecycleProviderOccurrenceDTO& right) {
    return left.control_domain_id < right.control_domain_id;
  };
  if (!sorted_unique(domains, domain_less) || !sorted_unique(provider_occurrences, provider_less)) {
    return false;
  }
  std::size_t binding_count = 0U;
  std::size_t resource_count = 0U;
  for (std::size_t index = 0U; index < domains.size(); ++index) {
    const auto& domain = domains[index];
    const auto& provider = provider_occurrences[index];
    if (!domain.locally_valid() || !provider.locally_valid() ||
        domain.control_domain_id != provider.control_domain_id ||
        domain.control_domain_revision != provider.control_domain_revision ||
        binding_count > kMaxLifecycleResolvedBindings - domain.current_control_bindings.size() ||
        resource_count > kMaxLifecycleCurrentResources - domain.current_resources.size()) {
      return false;
    }
    binding_count += domain.current_control_bindings.size();
    resource_count += domain.current_resources.size();
  }
  return true;
}

bool ExpectedDomainMatchesResolvedScope(
    const ExpectedControlDomainRevisionDTO& expected,
    const ResolvedLifecycleControlDomainScopeDTO& current) noexcept {
  if (!expected.locally_valid() || !current.locally_valid() ||
      expected.control_domain_id != current.control_domain_id ||
      expected.control_domain_revision != current.control_domain_revision ||
      expected.controller_topology_id != current.controller_topology_id ||
      expected.topology_revision != current.topology_revision ||
      expected.expected_control_bindings != current.current_control_bindings) {
    return false;
  }
  const auto& expected_configuration = expected.expected_active_control_configuration;
  if (expected_configuration.presence == ExpectedActiveControlConfigurationPresenceDTO::kAbsent) {
    return !current.active_control_configuration.has_value();
  }
  return current.active_control_configuration.has_value() &&
         *expected_configuration.active_control_configuration_id ==
             current.active_control_configuration->active_control_configuration_id &&
         *expected_configuration.active_control_configuration_generation ==
             current.active_control_configuration->active_control_configuration_generation;
}

bool DisableHandoffPreservesOrWidensScope(const DisableHandoffRequestDTO& request,
                                          const DisableHandoffStatusDTO& status) noexcept {
  return request.locally_valid() && status.locally_valid() &&
         status.lifecycle_transaction_id == request.lifecycle_transaction_id &&
         status.lifecycle_transaction_generation == request.lifecycle_transaction_generation &&
         status.controllable_component_id == request.controllable_component_id &&
         fence_scope_membership_is_subset(request.affected_scope, status.scope) &&
         (request.scope_resolution != DisableHandoffScopeResolutionDTO::kPromotedToFull ||
          status.scope_resolution == DisableHandoffScopeResolutionDTO::kPromotedToFull);
}

bool ConfigurationFenceHandleDTO::locally_valid() const noexcept {
  if (!bounded_identity(configuration_fence_id) || !configuration_fence_generation.nonzero() ||
      !bounded_identity(lifecycle_transaction_id) || !lifecycle_transaction_generation.nonzero() ||
      !bounded_identity(controllable_component_id) || !scope.valid() ||
      !fence_scope_bounded(scope) || reason.empty() || reason.size() > kMaxLifecycleReasonBytes ||
      !known(state) || !fence_scope_contains_component(scope, controllable_component_id)) {
    return false;
  }
  return std::all_of(scope.domains.begin(), scope.domains.end(),
                     [](const ConfigurationFenceDomainScopeDTO& domain) {
                       return domain.predecessor_control_domain_revision.has_value() &&
                              !domain.candidate_control_domain_revision.has_value();
                     }) &&
         std::all_of(scope.bindings.begin(), scope.bindings.end(),
                     [](const ConfigurationFenceBindingScopeDTO& binding) {
                       return binding.predecessor_control_binding_revision.has_value() &&
                              !binding.candidate_control_binding_revision.has_value();
                     });
}

bool CurrentDomainActiveControlConfigurationDTO::locally_valid() const noexcept {
  return bounded_identity(control_domain_id) && active_control_configuration.locally_valid();
}

bool DisableHandoffComponentScopeExpectationDTO::locally_valid() const noexcept {
  if (!bounded_identity(component_snapshot_digest) || !active_publication_generation.nonzero() ||
      !bounded_identity(active_public_pointer_digest) || !bounded_identity(retained_bundle_id) ||
      !bounded_identity(graph_revision) || !component_lifecycle_generation.nonzero() ||
      !component_scope.valid() || !fence_scope_bounded(component_scope) ||
      component_scope.controllable_component_ids.size() != 1U ||
      current_active_control_configurations.size() != component_scope.domains.size()) {
    return false;
  }
  const auto less = [](const CurrentDomainActiveControlConfigurationDTO& left,
                       const CurrentDomainActiveControlConfigurationDTO& right) {
    return left.control_domain_id < right.control_domain_id;
  };
  if (!sorted_unique(current_active_control_configurations, less)) {
    return false;
  }
  for (std::size_t index = 0U; index < current_active_control_configurations.size(); ++index) {
    if (!current_active_control_configurations[index].locally_valid() ||
        current_active_control_configurations[index].control_domain_id !=
            component_scope.domains[index].control_domain_id) {
      return false;
    }
  }
  return true;
}

bool DisableHandoffAdmissionQueryDTO::locally_valid() const noexcept {
  if (!bounded_identity(controllable_component_id) || !known(request_kind) ||
      (component_expectation.has_value() &&
       (!component_expectation->locally_valid() ||
        component_expectation->component_scope.controllable_component_ids.front() !=
            controllable_component_id))) {
    return false;
  }
  switch (request_kind) {
    case DisableHandoffScopeRequestKindDTO::kResolvedComponentScope:
      return component_expectation.has_value();
    case DisableHandoffScopeRequestKindDTO::kFullStopRequired:
      return !component_expectation.has_value();
  }
  return false;
}

bool DisableHandoffAdmissionSnapshotDTO::locally_valid() const noexcept {
  if (!query.locally_valid() || !bounded_identity(request_owner_epoch) ||
      !known(scope_resolution) || !current_scope.valid() || !fence_scope_bounded(current_scope) ||
      !active_publication_generation.nonzero() || !bounded_identity(active_public_pointer_digest) ||
      !bounded_identity(retained_bundle_id) || !bounded_identity(graph_revision) ||
      current_active_control_configurations.size() != current_scope.domains.size() ||
      !bounded_identity(snapshot_digest)) {
    return false;
  }
  if (query.request_kind == DisableHandoffScopeRequestKindDTO::kFullStopRequired &&
      scope_resolution != DisableHandoffScopeResolutionDTO::kPromotedToFull) {
    return false;
  }
  if (scope_resolution != DisableHandoffScopeResolutionDTO::kPromotedToFull &&
      (query.request_kind != DisableHandoffScopeRequestKindDTO::kResolvedComponentScope ||
       current_scope.controllable_component_ids.size() != 1U ||
       current_scope.controllable_component_ids.front() != query.controllable_component_id)) {
    return false;
  }
  const auto less = [](const CurrentDomainActiveControlConfigurationDTO& left,
                       const CurrentDomainActiveControlConfigurationDTO& right) {
    return left.control_domain_id < right.control_domain_id;
  };
  if (!sorted_unique(current_active_control_configurations, less)) {
    return false;
  }
  for (std::size_t index = 0U; index < current_active_control_configurations.size(); ++index) {
    if (!current_active_control_configurations[index].locally_valid() ||
        current_active_control_configurations[index].control_domain_id !=
            current_scope.domains[index].control_domain_id) {
      return false;
    }
  }
  return true;
}

bool DisableHandoffRequestDTO::locally_valid() const noexcept {
  if (!context_valid(owner_request_context) || !bounded_identity(initiating_public_request_id) ||
      !bounded_identity(lifecycle_transaction_id) || !lifecycle_transaction_generation.nonzero() ||
      !bounded_identity(controllable_component_id) || !known(scope_resolution) ||
      !affected_scope.valid() || !fence_scope_bounded(affected_scope) ||
      !bounded_identity(disable_admission_snapshot_digest) ||
      !expected_active_publication_generation.nonzero() ||
      !bounded_identity(expected_active_public_pointer_digest) ||
      !bounded_identity(expected_graph_revision) ||
      !bounded_identity(expected_retained_bundle_id) ||
      !expected_component_lifecycle_generation.nonzero() ||
      expected_active_control_configurations.size() != affected_scope.domains.size()) {
    return false;
  }
  if (scope_resolution != DisableHandoffScopeResolutionDTO::kPromotedToFull &&
      (affected_scope.controllable_component_ids.size() != 1U ||
       affected_scope.controllable_component_ids.front() != controllable_component_id)) {
    return false;
  }
  if (!std::all_of(affected_scope.domains.begin(), affected_scope.domains.end(),
                   [](const ConfigurationFenceDomainScopeDTO& domain) {
                     return domain.predecessor_control_domain_revision.has_value() &&
                            !domain.candidate_control_domain_revision.has_value();
                   }) ||
      !std::all_of(affected_scope.bindings.begin(), affected_scope.bindings.end(),
                   [](const ConfigurationFenceBindingScopeDTO& binding) {
                     return binding.predecessor_control_binding_revision.has_value() &&
                            !binding.candidate_control_binding_revision.has_value();
                   })) {
    return false;
  }
  const auto less = [](const CurrentDomainActiveControlConfigurationDTO& left,
                       const CurrentDomainActiveControlConfigurationDTO& right) {
    return left.control_domain_id < right.control_domain_id;
  };
  if (!sorted_unique(expected_active_control_configurations, less)) {
    return false;
  }
  for (std::size_t index = 0U; index < expected_active_control_configurations.size(); ++index) {
    const auto& configuration = expected_active_control_configurations[index];
    if (!configuration.locally_valid() ||
        configuration.control_domain_id != affected_scope.domains[index].control_domain_id) {
      return false;
    }
  }
  return true;
}

bool DisableHandoffAcceptedDTO::locally_valid() const noexcept {
  return bounded_identity(request_id) && IsAcceptedRequestReplayStatus(replay_status) &&
         bounded_identity(lifecycle_transaction_id) && lifecycle_transaction_generation.nonzero() &&
         bounded_identity(controllable_component_id) && known(scope_resolution) &&
         bounded_identity(component_scope_digest) && bounded_identity(configuration_fence_id) &&
         configuration_fence_generation.nonzero() &&
         stop_id.has_value() == stop_generation.has_value() &&
         (!stop_id.has_value() || (bounded_identity(*stop_id) && stop_generation->nonzero()));
}

bool DisableHandoffAcceptedDTO::matches(const DisableHandoffRequestDTO& request) const noexcept {
  return locally_valid() && request.locally_valid() &&
         request_id == request.owner_request_context.request_id &&
         lifecycle_transaction_id == request.lifecycle_transaction_id &&
         lifecycle_transaction_generation == request.lifecycle_transaction_generation &&
         controllable_component_id == request.controllable_component_id &&
         (request.scope_resolution != DisableHandoffScopeResolutionDTO::kPromotedToFull ||
          scope_resolution == DisableHandoffScopeResolutionDTO::kPromotedToFull);
}

bool DisableHandoffStatusDTO::locally_valid() const noexcept {
  if (!bounded_identity(lifecycle_transaction_id) || !lifecycle_transaction_generation.nonzero() ||
      !bounded_identity(controllable_component_id) || !known(scope_resolution) ||
      !bounded_identity(component_scope_digest) || !bounded_identity(configuration_fence_id) ||
      !configuration_fence_generation.nonzero() || !known(state) || !scope.valid() ||
      !configuration_fence_handle.locally_valid() ||
      configuration_fence_handle.configuration_fence_id != configuration_fence_id ||
      configuration_fence_handle.configuration_fence_generation != configuration_fence_generation ||
      configuration_fence_handle.lifecycle_transaction_id != lifecycle_transaction_id ||
      configuration_fence_handle.lifecycle_transaction_generation !=
          lifecycle_transaction_generation ||
      configuration_fence_handle.controllable_component_id != controllable_component_id ||
      configuration_fence_handle.scope != scope ||
      configuration_fence_handle.state != ConfigurationFenceState::kHeld ||
      (stop_certificate.has_value() && (!stop_certificate->locally_valid() ||
                                        !bounded_identity(stop_certificate->stop_certificate_id) ||
                                        !bounded_identity(stop_certificate->stop_id))) ||
      cancelled_queued_operations.size() > kMaxLifecycleCancelledOperations ||
      !std::is_sorted(cancelled_queued_operations.begin(), cancelled_queued_operations.end()) ||
      std::adjacent_find(cancelled_queued_operations.begin(), cancelled_queued_operations.end()) !=
          cancelled_queued_operations.end() ||
      std::any_of(
          cancelled_queued_operations.begin(), cancelled_queued_operations.end(),
          [](const OperationId& operation_id) { return !bounded_identity(operation_id); }) ||
      !error_valid(error)) {
    return false;
  }
  switch (state) {
    case DisableHandoffStateDTO::kStopping:
      return !stop_certificate.has_value() && !error.has_value();
    case DisableHandoffStateDTO::kReady:
      return !error.has_value();
    case DisableHandoffStateDTO::kFailed:
      return !stop_certificate.has_value() && error.has_value();
  }
  return false;
}

bool DisableHandoffAttemptReconciliationDTO::locally_valid() const noexcept {
  if (!request.locally_valid() || !known(disposition) || !error_valid(error) ||
      (acceptance.has_value() && !acceptance->matches(request)) ||
      (status.has_value() &&
       (!status->locally_valid() || !DisableHandoffPreservesOrWidensScope(request, *status)))) {
    return false;
  }
  const bool exact_pair =
      acceptance.has_value() && status.has_value() &&
      status->lifecycle_transaction_id == request.lifecycle_transaction_id &&
      status->lifecycle_transaction_generation == request.lifecycle_transaction_generation &&
      status->controllable_component_id == request.controllable_component_id &&
      status->scope_resolution == acceptance->scope_resolution &&
      status->component_scope_digest == acceptance->component_scope_digest &&
      status->configuration_fence_id == acceptance->configuration_fence_id &&
      status->configuration_fence_generation == acceptance->configuration_fence_generation;
  switch (disposition) {
    case DisableHandoffAttemptDispositionDTO::kAcceptedExact:
      return exact_pair && !error.has_value();
    case DisableHandoffAttemptDispositionDTO::kNotSeen:
      return !acceptance.has_value() && !status.has_value() && !error.has_value();
    case DisableHandoffAttemptDispositionDTO::kConflicting:
    case DisableHandoffAttemptDispositionDTO::kUncertain:
      return !exact_pair && error.has_value();
  }
  return false;
}

bool DisableHandoffEffectResultDTO::locally_valid() const noexcept {
  if (!request.locally_valid() || !known(disposition) ||
      (acceptance.has_value() && !acceptance->matches(request))) {
    return false;
  }
  switch (disposition) {
    case DisableHandoffEffectDispositionDTO::kAccepted:
      return acceptance.has_value();
    case DisableHandoffEffectDispositionDTO::kProvenNotSeen:
      return !acceptance.has_value();
  }
  return false;
}

bool DisableFenceReleaseRequestDTO::locally_valid() const noexcept {
  if (!context_valid(owner_request_context) || !bounded_identity(initiating_public_request_id) ||
      !bounded_identity(lifecycle_transaction_id) || !lifecycle_transaction_generation.nonzero() ||
      !bounded_identity(controllable_component_id) || !ready_handoff.locally_valid() ||
      ready_handoff.state != DisableHandoffStateDTO::kReady ||
      ready_handoff.lifecycle_transaction_id != lifecycle_transaction_id ||
      ready_handoff.lifecycle_transaction_generation != lifecycle_transaction_generation ||
      ready_handoff.controllable_component_id != controllable_component_id ||
      ready_handoff.configuration_fence_handle.state != ConfigurationFenceState::kHeld ||
      release_precondition_status_sequence == 0U ||
      !bounded_identity(release_precondition_status_digest) ||
      !current_component_expectation.locally_valid() ||
      current_component_expectation.component_scope.controllable_component_ids.front() !=
          controllable_component_id ||
      !current_admission_snapshot.locally_valid() ||
      current_admission_snapshot.query.controllable_component_id != controllable_component_id ||
      current_admission_snapshot.request_owner_epoch != owner_request_context.request_owner_epoch ||
      current_admission_snapshot.current_scope != ready_handoff.scope ||
      current_admission_snapshot.configuration_fence_generation !=
          ready_handoff.configuration_fence_generation ||
      current_admission_snapshot.active_publication_generation !=
          current_component_expectation.active_publication_generation ||
      current_admission_snapshot.active_public_pointer_digest !=
          current_component_expectation.active_public_pointer_digest ||
      current_admission_snapshot.retained_bundle_id !=
          current_component_expectation.retained_bundle_id ||
      current_admission_snapshot.graph_revision != current_component_expectation.graph_revision) {
    return false;
  }
  if (ready_handoff.scope_resolution == DisableHandoffScopeResolutionDTO::kPromotedToFull) {
    return current_admission_snapshot.query.request_kind ==
               DisableHandoffScopeRequestKindDTO::kFullStopRequired &&
           current_admission_snapshot.scope_resolution ==
               DisableHandoffScopeResolutionDTO::kPromotedToFull &&
           fence_scope_is_exact_subset(current_component_expectation.component_scope,
                                       current_admission_snapshot.current_scope) &&
           exact_rows_are_subset(
               current_component_expectation.current_active_control_configurations,
               current_admission_snapshot.current_active_control_configurations,
               [](const auto& row) { return row.control_domain_id; });
  }
  return current_admission_snapshot.query.request_kind ==
             DisableHandoffScopeRequestKindDTO::kResolvedComponentScope &&
         current_admission_snapshot.query.component_expectation == current_component_expectation &&
         current_admission_snapshot.scope_resolution !=
             DisableHandoffScopeResolutionDTO::kPromotedToFull &&
         current_component_expectation.component_scope == ready_handoff.scope &&
         current_admission_snapshot.current_active_control_configurations ==
             current_component_expectation.current_active_control_configurations;
}

bool DisableFenceReleasedDTO::locally_valid() const noexcept {
  return request.locally_valid() && IsAcceptedRequestReplayStatus(replay_status) &&
         released_at_ns >= 0 && bounded_identity(configuration_fence_id) &&
         configuration_fence_generation.nonzero() &&
         configuration_fence_id == request.ready_handoff.configuration_fence_id &&
         configuration_fence_generation == request.ready_handoff.configuration_fence_generation &&
         bounded_identity(scope_digest) &&
         scope_digest == request.ready_handoff.component_scope_digest &&
         bounded_identity(receipt_digest);
}

bool DisableFenceReleasedDTO::matches(
    const DisableFenceReleaseRequestDTO& candidate) const noexcept {
  return locally_valid() && candidate.locally_valid() && request == candidate;
}

bool DisableFenceReleaseAttemptReconciliationDTO::locally_valid() const noexcept {
  if (!request.locally_valid() || !known(disposition) || !error_valid(error) ||
      (release_receipt.has_value() && !release_receipt->matches(request)) ||
      (current_handoff_status.has_value() && !current_handoff_status->locally_valid())) {
    return false;
  }
  switch (disposition) {
    case DisableFenceReleaseAttemptDispositionDTO::kReleasedExact:
      return release_receipt.has_value() && !error.has_value();
    case DisableFenceReleaseAttemptDispositionDTO::kNotReleasedHeld:
      return !release_receipt.has_value() && !error.has_value() &&
             current_handoff_status == request.ready_handoff;
    case DisableFenceReleaseAttemptDispositionDTO::kConflicting:
    case DisableFenceReleaseAttemptDispositionDTO::kUncertain:
      return !release_receipt.has_value() && error.has_value();
  }
  return false;
}

bool DisableFenceReleaseEffectResultDTO::locally_valid() const noexcept {
  if (!request.locally_valid() || !known(disposition) ||
      (release_receipt.has_value() && !release_receipt->matches(request))) {
    return false;
  }
  switch (disposition) {
    case DisableFenceReleaseEffectDispositionDTO::kReleased:
      return release_receipt.has_value();
    case DisableFenceReleaseEffectDispositionDTO::kProvenNotReleasedHeld:
      return !release_receipt.has_value();
  }
  return false;
}

bool ComponentLifecycleDomainStatusDTO::locally_valid() const noexcept {
  if (!bounded_identity(control_domain_id) ||
      (!expected_domain.has_value() && !current_scope.has_value()) ||
      (expected_domain.has_value() && (!expected_domain->locally_valid() ||
                                       expected_domain->control_domain_id != control_domain_id)) ||
      (current_scope.has_value() && (!current_scope->locally_valid() ||
                                     current_scope->control_domain_id != control_domain_id)) ||
      current_scope.has_value() != resolved_provider_occurrence.has_value() ||
      (resolved_provider_occurrence.has_value() &&
       (!resolved_provider_occurrence->locally_valid() ||
        resolved_provider_occurrence->control_domain_id != control_domain_id ||
        resolved_provider_occurrence->control_domain_revision !=
            current_scope->control_domain_revision)) ||
      previous_state.has_value() != current_state.has_value() ||
      current_scope.has_value() != current_state.has_value() ||
      (previous_state.has_value() && !known(*previous_state)) ||
      (current_state.has_value() && !known(*current_state)) || !requested_state.locally_valid() ||
      !known(action_state) || !known(effect) ||
      primitive_attempts.size() > kMaxLifecycleAttemptsPerDomain || !error_valid(error)) {
    return false;
  }
  std::set<LifecyclePrimitiveAttemptId> attempts;
  bool any_possible_effect = false;
  bool any_observed_effect = false;
  for (const auto& attempt : primitive_attempts) {
    if (!attempt.locally_valid() || attempt.control_domain_id != control_domain_id ||
        !resolved_provider_occurrence.has_value() ||
        attempt.control_domain_revision != resolved_provider_occurrence->control_domain_revision ||
        attempt.provider_instance_id != resolved_provider_occurrence->provider_instance_id ||
        attempt.provider_manifest_id != resolved_provider_occurrence->provider_manifest_id ||
        attempt.provider_manifest_revision !=
            resolved_provider_occurrence->provider_manifest_revision ||
        attempt.provider_runtime_unit_id !=
            resolved_provider_occurrence->provider_runtime_unit_id ||
        attempt.provider_runtime_epoch != resolved_provider_occurrence->provider_runtime_epoch ||
        !attempts.insert(attempt.lifecycle_primitive_attempt_id).second) {
      return false;
    }
    any_possible_effect = any_possible_effect ||
                          attempt.effect == LifecyclePrimitiveEffectDTO::kPossible ||
                          attempt.effect == LifecyclePrimitiveEffectDTO::kObserved;
    any_observed_effect =
        any_observed_effect || attempt.effect == LifecyclePrimitiveEffectDTO::kObserved;
  }
  if (reconciliation_evidence.has_value()) {
    if (!reconciliation_evidence->locally_valid() || !resolved_provider_occurrence.has_value() ||
        reconciliation_evidence->provider_instance_id !=
            resolved_provider_occurrence->provider_instance_id ||
        reconciliation_evidence->provider_manifest_id !=
            resolved_provider_occurrence->provider_manifest_id ||
        reconciliation_evidence->provider_manifest_revision !=
            resolved_provider_occurrence->provider_manifest_revision ||
        reconciliation_evidence->provider_runtime_unit_id !=
            resolved_provider_occurrence->provider_runtime_unit_id ||
        reconciliation_evidence->provider_runtime_epoch !=
            resolved_provider_occurrence->provider_runtime_epoch) {
      return false;
    }
  }
  if ((any_possible_effect && effect == ComponentLifecycleDomainEffectDTO::kNone) ||
      (any_observed_effect && effect != ComponentLifecycleDomainEffectDTO::kEffectObserved &&
       effect != ComponentLifecycleDomainEffectDTO::kPartialEffectObserved &&
       effect != ComponentLifecycleDomainEffectDTO::kEffectUncertain) ||
      (any_possible_effect && !any_observed_effect &&
       effect == ComponentLifecycleDomainEffectDTO::kKnownNoEffect &&
       !reconciliation_evidence.has_value())) {
    return false;
  }
  switch (action_state) {
    case ComponentLifecycleDomainActionStateDTO::kSucceeded:
      return !error.has_value() && domain_requested_postcondition_satisfied(*this) &&
             (effect == ComponentLifecycleDomainEffectDTO::kKnownNoEffect ||
              effect == ComponentLifecycleDomainEffectDTO::kEffectObserved);
    case ComponentLifecycleDomainActionStateDTO::kFailed:
      return error.has_value() && effect != ComponentLifecycleDomainEffectDTO::kEffectUncertain;
    case ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown:
      return error.has_value() && effect == ComponentLifecycleDomainEffectDTO::kEffectUncertain;
    case ComponentLifecycleDomainActionStateDTO::kReconciling:
      return reconciliation_evidence.has_value();
    case ComponentLifecycleDomainActionStateDTO::kReserved:
    case ComponentLifecycleDomainActionStateDTO::kNotStarted:
    case ComponentLifecycleDomainActionStateDTO::kExecuting:
    case ComponentLifecycleDomainActionStateDTO::kVerifying:
      return true;
  }
  return false;
}

bool ComponentLifecycleStatusDTO::locally_valid() const noexcept {
  const auto expected_component_generation =
      accepted_intent.expected_component_lifecycle_generation.value();
  const auto accepted_component_generation = acceptance.component_lifecycle_generation.value();
  const bool component_generation_relation_valid =
      expected_component_generation != std::numeric_limits<std::uint64_t>::max() &&
      (accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable
           ? accepted_component_generation > expected_component_generation
           : accepted_component_generation == expected_component_generation + 1U);
  if (!acceptance.locally_valid() || !accepted_intent.locally_valid() ||
      acceptance.request_id != accepted_intent.request_context.request_id ||
      status_sequence == 0U || !bounded_identity(status_owner_epoch) ||
      previous_state.has_value() != current_state.has_value() ||
      (previous_state.has_value() && !known(*previous_state)) ||
      (current_state.has_value() && !known(*current_state)) ||
      !component_requested_state_matches_intent(requested_state, accepted_intent.intent) ||
      !known(phase) || domains.empty() ||
      domains.size() > (accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable
                            ? kMaxLifecycleDisableDomainUnion
                            : kMaxLifecycleResolvedDomains) ||
      (disable_fence_released.has_value() && !disable_fence_released->locally_valid()) ||
      !error_valid(error) || accepted_at_ns != acceptance.accepted_at_ns || accepted_at_ns < 0 ||
      latest_updated_at_ns < 0 ||
      (status_owner_epoch == accepted_intent.request_context.request_owner_epoch &&
       latest_updated_at_ns < accepted_at_ns) ||
      (status_owner_epoch != accepted_intent.request_context.request_owner_epoch &&
       remaining_duration_ns != 0) ||
      remaining_duration_ns < 0 || remaining_duration_ns > acceptance.timeout_duration_ns ||
      !component_generation_relation_valid) {
    return false;
  }

  const auto domain_less = [](const ComponentLifecycleDomainStatusDTO& left,
                              const ComponentLifecycleDomainStatusDTO& right) {
    return left.control_domain_id < right.control_domain_id;
  };
  if (!sorted_unique(domains, domain_less)) {
    return false;
  }

  std::size_t expected_index = 0U;
  std::size_t current_domain_count = 0U;
  std::size_t expected_binding_count = 0U;
  std::size_t current_binding_count = 0U;
  std::size_t attempt_count = 0U;
  std::size_t kinematic_projection_count = 0U;
  std::size_t kinematic_contributing_binding_count = 0U;
  bool any_uncertain = false;
  bool any_observed_or_partial = false;
  bool all_known_no_effect = true;
  bool all_succeeded = true;

  for (const auto& domain : domains) {
    if (!domain.locally_valid() || domain.requested_state != requested_state) {
      return false;
    }
    if (domain.current_scope.has_value()) {
      ++current_domain_count;
      current_binding_count += domain.current_scope->current_control_bindings.size();
      if (current_binding_count > kMaxLifecycleResolvedBindings) {
        return false;
      }
      if (domain.current_scope->active_control_configuration.has_value()) {
        const auto& configuration = *domain.current_scope->active_control_configuration;
        if (kinematic_projection_count >
            kMaxLifecycleKinematicSourceProjections -
                configuration.kinematic_resource_source_projections.size()) {
          return false;
        }
        kinematic_projection_count += configuration.kinematic_resource_source_projections.size();
        for (const auto& projection : configuration.kinematic_resource_source_projections) {
          if (kinematic_contributing_binding_count >
              kMaxLifecycleKinematicContributingBindings -
                  projection.contributing_control_bindings.size()) {
            return false;
          }
          kinematic_contributing_binding_count += projection.contributing_control_bindings.size();
        }
      }
    }
    if (domain.expected_domain.has_value()) {
      expected_binding_count += domain.expected_domain->expected_control_bindings.size();
      if (expected_binding_count > kMaxLifecycleExpectedBindings) {
        return false;
      }
    }
    if (attempt_count > kMaxLifecycleAttemptsPerStatus - domain.primitive_attempts.size()) {
      return false;
    }
    attempt_count += domain.primitive_attempts.size();
    for (const auto& attempt : domain.primitive_attempts) {
      if (attempt.lifecycle_transaction_id != acceptance.lifecycle_transaction_id ||
          attempt.lifecycle_transaction_generation != acceptance.lifecycle_transaction_generation ||
          attempt.controllable_component_id != accepted_intent.controllable_component_id) {
        return false;
      }
    }

    if (expected_index < accepted_intent.expected_control_domains.size() &&
        accepted_intent.expected_control_domains[expected_index].control_domain_id <
            domain.control_domain_id) {
      return false;
    }
    if (expected_index < accepted_intent.expected_control_domains.size() &&
        accepted_intent.expected_control_domains[expected_index].control_domain_id ==
            domain.control_domain_id) {
      if (!expected_domains_equal(domain.expected_domain,
                                  accepted_intent.expected_control_domains[expected_index])) {
        return false;
      }
      ++expected_index;
    } else if (domain.expected_domain.has_value()) {
      return false;
    }

    if (accepted_intent.intent != ComponentLifecycleIntentKindDTO::kDisable &&
        (!domain.expected_domain.has_value() || !domain.current_scope.has_value() ||
         !ExpectedDomainMatchesResolvedScope(*domain.expected_domain, *domain.current_scope))) {
      return false;
    }

    any_uncertain =
        any_uncertain || domain.effect == ComponentLifecycleDomainEffectDTO::kEffectUncertain;
    any_observed_or_partial =
        any_observed_or_partial ||
        domain.effect == ComponentLifecycleDomainEffectDTO::kEffectObserved ||
        domain.effect == ComponentLifecycleDomainEffectDTO::kPartialEffectObserved;
    all_known_no_effect =
        all_known_no_effect && (domain.effect == ComponentLifecycleDomainEffectDTO::kNone ||
                                domain.effect == ComponentLifecycleDomainEffectDTO::kKnownNoEffect);
    all_succeeded = all_succeeded &&
                    domain.action_state == ComponentLifecycleDomainActionStateDTO::kSucceeded &&
                    !domain.error.has_value();
  }

  if (expected_index != accepted_intent.expected_control_domains.size() ||
      current_domain_count > kMaxLifecycleResolvedDomains ||
      expected_binding_count + current_binding_count > kMaxLifecycleStatusBindingUnion) {
    return false;
  }

  if ((current_domain_count != 0U) != current_state.has_value()) {
    return false;
  }

  const bool terminal_phase = phase == ComponentLifecycleTransactionPhaseDTO::kSucceeded ||
                              phase == ComponentLifecycleTransactionPhaseDTO::kFailed;
  if (terminal_phase != terminal_result.has_value() ||
      (terminal_result.has_value() && !known(*terminal_result)) ||
      ((phase == ComponentLifecycleTransactionPhaseDTO::kSucceeded) !=
       (terminal_result == ComponentLifecycleTerminalResultDTO::kSucceeded)) ||
      (phase == ComponentLifecycleTransactionPhaseDTO::kSucceeded &&
       (!all_succeeded || error.has_value())) ||
      ((phase == ComponentLifecycleTransactionPhaseDTO::kFailed ||
        phase == ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown) &&
       !error.has_value()) ||
      (terminal_result == ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect &&
       !all_known_no_effect) ||
      (terminal_result == ComponentLifecycleTerminalResultDTO::kFailedPartialEffect &&
       (!any_observed_or_partial || any_uncertain)) ||
      (any_uncertain && (phase != ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown &&
                         phase != ComponentLifecycleTransactionPhaseDTO::kReconciling)) ||
      (any_uncertain && terminal_result.has_value()) ||
      (terminal_phase && remaining_duration_ns != 0)) {
    return false;
  }

  if (accepted_intent.intent != ComponentLifecycleIntentKindDTO::kDisable) {
    return !configuration_fence.has_value() && !disable_handoff.has_value() &&
           !disable_fence_released.has_value();
  }

  if (phase == ComponentLifecycleTransactionPhaseDTO::kAccepted ||
      phase == ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains) {
    return !configuration_fence.has_value() && !disable_handoff.has_value() &&
           !disable_fence_released.has_value();
  }
  if (phase == ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown &&
      !disable_handoff.has_value()) {
    return reconciliation_required && !configuration_fence.has_value() &&
           !disable_fence_released.has_value();
  }
  if (!disable_handoff.has_value() || !disable_handoff->locally_valid() ||
      disable_handoff->lifecycle_transaction_id != acceptance.lifecycle_transaction_id ||
      disable_handoff->lifecycle_transaction_generation !=
          acceptance.lifecycle_transaction_generation ||
      disable_handoff->controllable_component_id != accepted_intent.controllable_component_id) {
    return false;
  }
  if (disable_handoff->state == DisableHandoffStateDTO::kReady) {
    if (!configuration_fence.has_value() || !configuration_fence->locally_valid() ||
        !same_fence_identity_and_scope(*configuration_fence,
                                       disable_handoff->configuration_fence_handle)) {
      return false;
    }
    if (!disable_fence_released.has_value()) {
      return !terminal_phase &&
             (configuration_fence->state == ConfigurationFenceState::kHeld ||
              configuration_fence->state == ConfigurationFenceState::kReleasePending);
    }
    const auto& receipt = *disable_fence_released;
    return terminal_phase && !reconciliation_required &&
           configuration_fence->state == ConfigurationFenceState::kReleased &&
           receipt.request.ready_handoff == *disable_handoff &&
           receipt.request.lifecycle_transaction_id == acceptance.lifecycle_transaction_id &&
           receipt.request.lifecycle_transaction_generation ==
               acceptance.lifecycle_transaction_generation &&
           receipt.request.controllable_component_id == accepted_intent.controllable_component_id &&
           receipt.request.release_precondition_status_sequence < status_sequence &&
           receipt.configuration_fence_id == configuration_fence->configuration_fence_id &&
           receipt.configuration_fence_generation ==
               configuration_fence->configuration_fence_generation;
  }
  return !terminal_phase && !configuration_fence.has_value() && !disable_fence_released.has_value();
}

}  // namespace manyforge::core
