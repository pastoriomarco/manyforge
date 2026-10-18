#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <set>
#include <stdexcept>
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

template <typename Value, typename Less>
[[nodiscard]] bool sorted_unique(const std::vector<Value>& values, Less less) noexcept {
  return std::is_sorted(values.begin(), values.end(), less) &&
         std::adjacent_find(values.begin(), values.end(),
                            [&](const Value& left, const Value& right) {
                              return !less(left, right) && !less(right, left);
                            }) == values.end();
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

// These joins establish durable data consistency only. The original live
// private offer and independently qualified retained bundle own effect entry;
// replay/owner replacement must remain observation and reconciliation only.
[[nodiscard]] bool fence_matches_expected_provider(
    const ConfigurationFenceDomainScopeDTO& fence,
    const ExpectedControlDomainRevisionDTO& expected) noexcept {
  const auto* payload = std::get_if<ProviderConfigurationFenceDomainDTO>(&fence.payload);
  const auto* expected_provider = expected.provider();
  const auto* proof =
      payload && payload->candidate_provider ? &*payload->candidate_provider : nullptr;
  if (static_cast<bool>(proof) != static_cast<bool>(expected_provider)) return false;
  return !proof ||
         (proof->controllable_component_id ==
              expected_provider->provider.controllable_component_id &&
          proof->provider_instance_id == expected_provider->provider.provider_instance_id &&
          proof->signal_configuration_digest ==
              expected_provider->provider.signal_configuration_digest);
}

[[nodiscard]] bool fence_matches_current_provider(
    const ConfigurationFenceDomainScopeDTO& fence,
    const ResolvedLifecycleControlDomainScopeDTO& current, bool candidate) noexcept {
  const auto* payload = std::get_if<ProviderConfigurationFenceDomainDTO>(&fence.payload);
  const auto* proof =
      payload ? (candidate ? &payload->candidate_provider : &payload->predecessor_provider)
              : nullptr;
  const auto* provider = current.provider();
  if (static_cast<bool>(proof && proof->has_value()) != static_cast<bool>(provider)) return false;
  return !provider ||
         ((*proof)->controllable_component_id == provider->provider.controllable_component_id &&
          (*proof)->provider_instance_id == provider->provider.provider_instance_id &&
          (*proof)->signal_configuration_digest == provider->provider.signal_configuration_digest &&
          (*proof)->output_resources == provider->output_resources);
}

[[nodiscard]] bool fence_provider_within_parent(
    const ConfigurationFenceDomainScopeDTO& child,
    const ConfigurationFenceDomainScopeDTO& parent) noexcept {
  const auto* child_payload = std::get_if<ProviderConfigurationFenceDomainDTO>(&child.payload);
  const auto* parent_payload = std::get_if<ProviderConfigurationFenceDomainDTO>(&parent.payload);
  const std::optional<ProviderConfigurationFenceOccurrenceDTO> absent;
  return (!child.predecessor_control_domain_revision ||
          (child_payload ? child_payload->predecessor_provider : absent) ==
              (parent_payload ? parent_payload->predecessor_provider : absent)) &&
         (!child.candidate_control_domain_revision ||
          (child_payload ? child_payload->candidate_provider : absent) ==
              (parent_payload ? parent_payload->candidate_provider : absent));
}

[[nodiscard]] bool selected_status_evidence_valid(const ComponentLifecycleStatusDTO& status) {
  const auto& evidence = *status.selected_bring_up;
  const auto& claim = evidence.selected_claim;
  const auto& intent = claim.intent;
  const auto& scope = evidence.accepted_fence.captured_scope;
  const auto& accepted = status.acceptance;
  const auto& request = status.accepted_intent.request_context;
  if (status.accepted_intent.intent != ComponentLifecycleIntentKindDTO::kEnable ||
      status.accepted_intent.program_node_evidence ||
      !IsExactLowercaseSha256Digest(evidence.offer_digest.value()) ||
      !IsExactLowercaseSha256Digest(evidence.original_request_payload_digest.value()) ||
      !IsExactLowercaseSha256Digest(evidence.predecessor_snapshot_digest.value()) ||
      evidence.incident_id.empty() || evidence.incident_id.size() > kMaxLifecycleIdentityBytes ||
      !bounded_identity(evidence.containment_stop_id) ||
      !evidence.containment_stop_generation.nonzero() ||
      !evidence.candidate_projection_revision.valid() ||
      !bounded_identity(evidence.candidate_projection_revision.retained_bundle_id) ||
      !bounded_identity(evidence.candidate_projection_revision.graph_revision) ||
      !IsExactLowercaseSha256Digest(evidence.candidate_retained_bundle_digest.value()) ||
      evidence.candidate_retained_bundle_bytes == 0U ||
      !evidence.predecessor_occurrence.locally_valid() ||
      !evidence.successor_occurrence.locally_valid() ||
      !ValidateCellAdmissionRecoveryReceiptQuery({intent, claim, std::nullopt}).ok() ||
      intent.claim_kind != CellAdmissionClaimKindDTO::kInterventionOrRecovery ||
      intent.owner_role != CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner ||
      intent.owner_epoch != request.request_owner_epoch ||
      intent.request_id != request.request_id ||
      intent.owner_transaction_id != accepted.lifecycle_transaction_id.value() ||
      intent.owner_transaction_id !=
          "selected-lifecycle." +
              std::to_string(accepted.lifecycle_transaction_generation.value()) ||
      intent.request_payload_digest != evidence.offer_digest ||
      intent.expected_admission_generation !=
          status.accepted_intent.expected_admission_generation ||
      (std::any_of(scope.domains.begin(), scope.domains.end(),
                   [](const auto& domain) { return domain.has_controller_side(); }) &&
       (intent.scope.resources.empty() || intent.scope.configurations.empty())) ||
      intent.program_cycle_evidence || intent.program_node_evidence ||
      !intent.intervention_evidence ||
      intent.intervention_evidence->parent_program_cycle_claim_generation ||
      !evidence.accepted_fence.valid() ||
      !bounded_identity(evidence.accepted_fence.configuration_fence_id) ||
      scope.controllable_component_ids !=
          std::vector<ControllableComponentId>{status.accepted_intent.controllable_component_id} ||
      scope.resources != intent.scope.resources || scope.domains.size() != status.domains.size() ||
      scope.domains.size() > kMaxLifecycleDisableDomainUnion ||
      scope.bindings.size() > kMaxLifecycleStatusBindingUnion ||
      scope.resources.size() > kMaximumCellAdmissionScopeEntries ||
      evidence.successor_occurrence.domains.size() !=
          status.accepted_intent.expected_control_domains.size())
    return false;

  std::vector<ControlDomainId> candidate_domains;
  for (const auto& domain : status.accepted_intent.expected_control_domains)
    candidate_domains.push_back(domain.control_domain_id);
  const auto successor = ComputeSelectedControllerSuccessorOccurrence(
      evidence.predecessor_occurrence, candidate_domains,
      evidence.successor_occurrence.arm_binding_epoch.has_value());
  if (!successor.ok() || successor.value() != evidence.successor_occurrence) return false;

  const bool accepted_phase = status.phase == ComponentLifecycleTransactionPhaseDTO::kAccepted;
  std::size_t predecessor_index = 0U;
  std::size_t candidate_index = 0U;
  std::map<ControlBindingId, ControlBindingRevision> candidate_bindings;
  for (std::size_t index = 0U; index < scope.domains.size(); ++index) {
    const auto& fenced = scope.domains[index];
    const auto& domain = status.domains[index];
    if (fenced.control_domain_id != domain.control_domain_id) return false;
    if (fenced.predecessor_control_domain_revision) {
      if (predecessor_index >= evidence.predecessor_occurrence.domains.size() ||
          evidence.predecessor_occurrence.domains[predecessor_index++].control_domain_id !=
              fenced.control_domain_id) return false;
    }
    if (fenced.candidate_control_domain_revision) {
      if (candidate_index >= status.accepted_intent.expected_control_domains.size()) return false;
      const auto& expected = status.accepted_intent.expected_control_domains[candidate_index++];
      if (expected.control_domain_id != fenced.control_domain_id ||
          expected.control_domain_revision != *fenced.candidate_control_domain_revision ||
          !fence_matches_expected_provider(fenced, expected) || !domain.expected_domain ||
          *domain.expected_domain != expected)
        return false;
      for (const auto& binding : expected.control_bindings()) {
        const auto [entry, inserted] = candidate_bindings.emplace(
            binding.control_binding_id, binding.control_binding_revision);
        if (!inserted && entry->second != binding.control_binding_revision) return false;
      }
    } else if (domain.expected_domain) return false;

    // Accepted records precede installation: every predecessor is observed,
    // and a new-only domain has no fabricated current observation.
    if (accepted_phase && domain.current_scope.has_value() !=
                              fenced.predecessor_control_domain_revision.has_value()) return false;
    if (!domain.current_scope) continue;  // No fabricated observation for a newcomer.
    const auto& current = *domain.current_scope;
    const bool predecessor = fenced.predecessor_control_domain_revision ==
                             current.control_domain_revision;
    const bool candidate = !accepted_phase &&
                           fenced.candidate_control_domain_revision == current.control_domain_revision;
    if (!predecessor && !candidate) return false;
    if (!((predecessor && fence_matches_current_provider(fenced, current, false)) ||
          (candidate && fence_matches_current_provider(fenced, current, true))))
      return false;
    for (const auto& binding : current.control_bindings()) {
      const auto entry = std::lower_bound(scope.bindings.begin(), scope.bindings.end(),
          binding.control_binding_id, [](const auto& row, const auto& id) {
            return row.control_binding_id < id;
          });
      if (entry == scope.bindings.end() || entry->control_binding_id != binding.control_binding_id ||
          !((predecessor && entry->predecessor_control_binding_revision ==
                                binding.control_binding_revision) ||
            (candidate && entry->candidate_control_binding_revision ==
                              binding.control_binding_revision))) return false;
    }
    for (const auto& resource : current.resource_ids())
      if (!std::binary_search(scope.resources.begin(), scope.resources.end(), resource)) return false;

    // Configurations in the selected claim are the adopted predecessor facts.
    // Accepted records compare them even when both revisions are unchanged.
    // A later successor status cannot reconstruct or replace those facts.
    if (predecessor && !candidate) {
      const auto configured = std::lower_bound(intent.scope.configurations.begin(),
          intent.scope.configurations.end(), domain.control_domain_id,
          [](const auto& row, const auto& id) { return row.control_domain_id < id; });
      const bool claimed = configured != intent.scope.configurations.end() &&
                           configured->control_domain_id == domain.control_domain_id;
      const auto active = current.configuration_identity();
      if (claimed != active.has_value()) return false;
      if (claimed &&
          (configured->active_configuration_id != active->active_control_configuration_id ||
           configured->active_configuration_generation !=
               active->active_control_configuration_generation))
        return false;
    }
  }
  if (predecessor_index != evidence.predecessor_occurrence.domains.size() ||
      candidate_index != status.accepted_intent.expected_control_domains.size()) return false;
  for (const auto& configured : intent.scope.configurations) {
    const auto predecessor = std::lower_bound(evidence.predecessor_occurrence.domains.begin(),
        evidence.predecessor_occurrence.domains.end(), configured.control_domain_id,
        [](const auto& row, const auto& id) { return row.control_domain_id < id; });
    if (predecessor == evidence.predecessor_occurrence.domains.end() ||
        predecessor->control_domain_id != configured.control_domain_id) return false;
  }
  std::size_t candidate_binding_count = 0U;
  for (const auto& fenced : scope.bindings) {
    if (!fenced.candidate_control_binding_revision) continue;
    ++candidate_binding_count;
    const auto binding = candidate_bindings.find(fenced.control_binding_id);
    if (binding == candidate_bindings.end() ||
        binding->second != *fenced.candidate_control_binding_revision) return false;
  }
  return candidate_binding_count == candidate_bindings.size();
}

// Q91 data closure only. A decoded reference/floor never proves that the
// parent claim/fence remains held, nor installs a successor or grants motion.
[[nodiscard]] bool apply_status_evidence_valid(const ComponentLifecycleStatusDTO& status) try {
  const auto& evidence = *status.apply_bring_up;
  const auto& authority = evidence.parent_authority;
  const auto& scope = evidence.selected_scope;
  const auto& parent_scope = authority.held_fence_query.expected_scope;
  const auto& intent = status.accepted_intent;
  if (status.selected_bring_up ||
      (intent.intent != ComponentLifecycleIntentKindDTO::kPrepare &&
       intent.intent != ComponentLifecycleIntentKindDTO::kEnable &&
       intent.intent != ComponentLifecycleIntentKindDTO::kDisable) || intent.program_node_evidence ||
      !authority.valid() || !ValidateCellAdmissionRecoveryReceiptQuery(
          {authority.parent_claim.intent, authority.parent_claim, std::nullopt}).ok() ||
      !IsExactLowercaseSha256Digest(authority.parent.plan_digest.value()) ||
      !IsExactLowercaseSha256Digest(evidence.original_request_payload_digest.value()) ||
      !evidence.candidate_projection_revision.valid() ||
      !IsExactLowercaseSha256Digest(evidence.candidate_retained_bundle_digest.value()) ||
      evidence.candidate_retained_bundle_bytes == 0U ||
      !bounded_identity(evidence.original_execution_host.runtime_unit_id) ||
      !bounded_identity(evidence.original_execution_host.runtime_epoch) ||
      evidence.predecessor_occurrence.has_value() != evidence.successor_occurrence_floor.has_value() ||
      !scope.valid() || scope.controllable_component_ids !=
          std::vector<ControllableComponentId>{intent.controllable_component_id} ||
      scope.domains.size() != status.domains.size() ||
      scope.domains.size() > kMaxLifecycleDisableDomainUnion ||
      scope.bindings.size() > kMaxLifecycleStatusBindingUnion ||
      scope.resources.size() > kMaximumCellAdmissionScopeEntries ||
      !std::binary_search(parent_scope.controllable_component_ids.begin(),
                          parent_scope.controllable_component_ids.end(),
                          intent.controllable_component_id)) return false;

  for (const auto& resource : scope.resources)
    if (!std::binary_search(parent_scope.resources.begin(), parent_scope.resources.end(), resource))
      return false;
  for (const auto& binding : scope.bindings) {
    const auto parent = std::lower_bound(parent_scope.bindings.begin(), parent_scope.bindings.end(),
        binding.control_binding_id, [](const auto& row, const auto& id) {
          return row.control_binding_id < id;
        });
    if (parent == parent_scope.bindings.end() || parent->control_binding_id != binding.control_binding_id ||
        (binding.predecessor_control_binding_revision && binding.predecessor_control_binding_revision !=
                                                       parent->predecessor_control_binding_revision) ||
        (binding.candidate_control_binding_revision && binding.candidate_control_binding_revision !=
                                                     parent->candidate_control_binding_revision)) return false;
  }

  const bool accepted_phase = status.phase == ComponentLifecycleTransactionPhaseDTO::kAccepted;
  std::size_t candidate_index = 0U;
  std::vector<ControlDomainId> predecessor_domains;
  std::vector<ControlDomainId> candidate_domains;
  std::map<ControlBindingId, ControlBindingRevision> candidate_bindings;
  for (std::size_t index = 0U; index < scope.domains.size(); ++index) {
    const auto& fenced = scope.domains[index];
    const auto& domain = status.domains[index];
    const auto parent = std::lower_bound(parent_scope.domains.begin(), parent_scope.domains.end(),
        fenced.control_domain_id, [](const auto& row, const auto& id) {
          return row.control_domain_id < id;
        });
    if (fenced.control_domain_id != domain.control_domain_id ||
        parent == parent_scope.domains.end() ||
        parent->control_domain_id != fenced.control_domain_id ||
        (fenced.predecessor_control_domain_revision &&
         fenced.predecessor_control_domain_revision !=
             parent->predecessor_control_domain_revision) ||
        (fenced.candidate_control_domain_revision &&
         fenced.candidate_control_domain_revision != parent->candidate_control_domain_revision) ||
        !fence_provider_within_parent(fenced, *parent))
      return false;
    if (fenced.predecessor_control_domain_revision) predecessor_domains.push_back(fenced.control_domain_id);
    if (fenced.candidate_control_domain_revision) {
      candidate_domains.push_back(fenced.control_domain_id);
      if (candidate_index >= intent.expected_control_domains.size()) return false;
      const auto& expected = intent.expected_control_domains[candidate_index++];
      if (expected.control_domain_id != fenced.control_domain_id ||
          expected.control_domain_revision != *fenced.candidate_control_domain_revision ||
          !fence_matches_expected_provider(fenced, expected) || !domain.expected_domain ||
          *domain.expected_domain != expected)
        return false;
      for (const auto& binding : expected.control_bindings()) {
        const auto [entry, inserted] = candidate_bindings.emplace(
            binding.control_binding_id, binding.control_binding_revision);
        if (!inserted && entry->second != binding.control_binding_revision) return false;
      }
    } else if (domain.expected_domain) return false;
    if (accepted_phase && domain.current_scope.has_value() !=
                              fenced.predecessor_control_domain_revision.has_value()) return false;
    if (!domain.current_scope) continue;
    const auto& current = *domain.current_scope;
    const bool predecessor = fenced.predecessor_control_domain_revision == current.control_domain_revision;
    const bool candidate = !accepted_phase &&
                          fenced.candidate_control_domain_revision == current.control_domain_revision;
    if (!predecessor && !candidate) return false;
    if (!((predecessor && fence_matches_current_provider(fenced, current, false)) ||
          (candidate && fence_matches_current_provider(fenced, current, true))))
      return false;
    for (const auto& binding : current.control_bindings()) {
      const auto entry = std::lower_bound(scope.bindings.begin(), scope.bindings.end(),
          binding.control_binding_id, [](const auto& row, const auto& id) {
            return row.control_binding_id < id;
          });
      if (entry == scope.bindings.end() || entry->control_binding_id != binding.control_binding_id ||
          !((predecessor && entry->predecessor_control_binding_revision ==
                                binding.control_binding_revision) ||
            (candidate && entry->candidate_control_binding_revision ==
                              binding.control_binding_revision))) return false;
    }
    for (const auto& resource : current.resource_ids())
      if (!std::binary_search(scope.resources.begin(), scope.resources.end(), resource)) return false;
    if (accepted_phase && domain.expected_domain &&
        fenced.predecessor_control_domain_revision == fenced.candidate_control_domain_revision) {
      const auto& expected = domain.expected_domain->expected_configuration();
      const bool present = expected.presence == ExpectedActiveControlConfigurationPresenceDTO::kPresent;
      const auto active = current.configuration_identity();
      if (present != active.has_value()) return false;
      if (present &&
          (expected.active_control_configuration_id != active->active_control_configuration_id ||
           expected.active_control_configuration_generation !=
               active->active_control_configuration_generation))
        return false;
    }
  }
  if (candidate_index != intent.expected_control_domains.size()) return false;
  std::size_t candidate_binding_count = 0U;
  for (const auto& binding : scope.bindings) {
    if (!binding.candidate_control_binding_revision) continue;
    ++candidate_binding_count;
    const auto expected = candidate_bindings.find(binding.control_binding_id);
    if (expected == candidate_bindings.end() ||
        expected->second != *binding.candidate_control_binding_revision) return false;
  }
  if (candidate_binding_count != candidate_bindings.size()) return false;

  if (evidence.recovery &&
      (!IsExactLowercaseSha256Digest(evidence.recovery->predecessor_snapshot_digest.value()) ||
       evidence.recovery->incident_id.empty() ||
       evidence.recovery->incident_id.size() > kMaxLifecycleIdentityBytes ||
       !bounded_identity(evidence.recovery->containment_stop_id) ||
       !evidence.recovery->containment_stop_generation.nonzero() ||
       !evidence.predecessor_occurrence || !evidence.successor_occurrence_floor)) return false;
  if (evidence.predecessor_occurrence) {
    if (!evidence.predecessor_occurrence->locally_valid() ||
        evidence.predecessor_occurrence->domains.size() != predecessor_domains.size()) return false;
    for (std::size_t index = 0U; index < predecessor_domains.size(); ++index)
      if (evidence.predecessor_occurrence->domains[index].control_domain_id != predecessor_domains[index])
        return false;
  }
  if (evidence.successor_occurrence_floor) {
    if (!evidence.successor_occurrence_floor->locally_valid() ||
        evidence.successor_occurrence_floor->domains.size() != candidate_domains.size()) return false;
    for (std::size_t index = 0U; index < candidate_domains.size(); ++index)
      if (evidence.successor_occurrence_floor->domains[index].control_domain_id != candidate_domains[index] ||
          evidence.successor_occurrence_floor->domains[index].observation_generation == 0U) return false;
  }
  if (evidence.predecessor_occurrence && evidence.successor_occurrence_floor) {
    const auto expected = ComputeSelectedControllerSuccessorOccurrence(*evidence.predecessor_occurrence,
        candidate_domains, evidence.successor_occurrence_floor->arm_binding_epoch.has_value());
    if (!expected.ok() || expected.value() != *evidence.successor_occurrence_floor) return false;
  }
  return evidence.original_request_payload_digest == ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      intent, evidence.candidate_projection_revision, evidence.candidate_retained_bundle_digest,
      evidence.candidate_retained_bundle_bytes, evidence.original_execution_host.runtime_unit_id,
      evidence.original_execution_host.runtime_epoch, authority.held_fence_query.manager_runtime_epoch,
      authority.parent);
} catch (...) {
  return false;
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
             (!completed || lifecycle_primitive_result->matches(*lifecycle_primitive_intent));
  }
  return false;
}

bool CCLCTransactionEffectSequenceWatermarkDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_transaction_id) && lifecycle_transaction_generation.nonzero();
}

bool CCLCJournalRecoveryDTO::locally_valid() const noexcept {
  if (!bounded_identity(request_owner_epoch) ||
      component_watermarks.size() > kMaximumCCLCComponentWatermarks ||
      interrupted_transactions.size() > kMaximumCCLCActiveTransactions ||
      effect_sequence_watermarks.size() > kMaximumCCLCActiveTransactions ||
      unresolved_effect_attempts.size() > kMaximumCCLCActiveTransactions ||
      active_configuration_watermarks.size() > kMaximumCCLCActiveConfigurationWatermarks ||
      current_active_control_configurations.size() > kMaximumCCLCActiveConfigurationWatermarks ||
      interrupted_route_selections.size() > kMaximumCCLCActiveTransactions ||
      route_effect_sequence_watermarks.size() > kMaximumCCLCActiveTransactions ||
      unresolved_route_effect_attempts.size() > kMaximumCCLCActiveTransactions ||
      cell_claims.size() > kMaximumCCLCActiveTransactions ||
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
  for (const auto& row : cell_claims) {
    const bool selected =
        row.intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery &&
        row.intent.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner &&
        row.intent.owner_transaction_id.starts_with("selected-lifecycle.");
    if (selected != row.original_request_payload_digest.has_value() ||
        (row.original_request_payload_digest &&
         !IsExactLowercaseSha256Digest(row.original_request_payload_digest->value())) ||
        !ValidateCellAdmissionClaimIntent(row.intent).ok() ||
        (row.claim.has_value() &&
         (!ValidateCellAdmissionClaimIntent(row.claim->intent).ok() ||
          row.claim->intent != row.intent)) ||
        (row.release_intent.has_value() &&
         !ValidateCellAdmissionClaimRelease(*row.release_intent).ok()) ||
        (row.release_receipt_recorded && !row.release_intent.has_value() &&
         row.claim.has_value())) {
      return false;
    }
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
    if (status.selected_bring_up) {
      const auto& selected_claim = status.selected_bring_up->selected_claim;
      const auto row = std::find_if(cell_claims.begin(), cell_claims.end(), [&](const auto& item) {
        return item.intent.owner_transaction_id ==
               status.acceptance.lifecycle_transaction_id.value();
      });
      if (row == cell_claims.end() || !row->transaction_accepted ||
          row->intent != selected_claim.intent || row->claim != selected_claim ||
          row->original_request_payload_digest !=
              status.selected_bring_up->original_request_payload_digest ||
          std::count_if(cell_claims.begin(), cell_claims.end(), [&](const auto& item) {
            return item.intent.owner_transaction_id ==
                   status.acceptance.lifecycle_transaction_id.value();
          }) != 1) return false;
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
            attempt.lifecycle_primitive_intent->controllable_component_id) {
      return false;
    }
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
  if (incident_recovery) {
    return accepted && !acceptance && !status && !rejection &&
           incident_recovery->locally_valid();
  }
  if (rejection) {
    return accepted && !acceptance && !status && error_valid(rejection) &&
           rejection->details.contains("effect") &&
           rejection->details.at("effect") == "not_started";
  }
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
  if (payload.valueless_by_exception() || !bounded_identity(control_domain_id) ||
      !bounded_identity(control_domain_revision))
    return false;
  if (const auto* value = provider())
    return value->provider.locally_valid() &&
           value->expected_active_control_configuration.locally_valid();
  const auto& [controller_topology_id, topology_revision, expected_active_control_configuration,
               expected_control_bindings] = controller();
  if (!bounded_identity(controller_topology_id) || !bounded_identity(topology_revision) ||
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

bool ApplyComponentQualifiedPredecessorDTO::locally_valid() const noexcept {
  return generation.nonzero() &&
      generation.value() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
      IsExactLowercaseSha256Digest(snapshot_digest.value());
}

bool ApplyComponentGenerationSelectionDTO::locally_valid() const noexcept {
  const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if ((predecessor_generation && (!predecessor_generation->nonzero() ||
                                predecessor_generation->value() > limit)) ||
      (owner_qualified_predecessor && (!owner_qualified_predecessor->locally_valid() ||
          (predecessor_generation && *predecessor_generation != owner_qualified_predecessor->generation))) ||
      prior_reservation_generation > limit) return false;
  const auto predecessor = predecessor_generation ? predecessor_generation->value()
      : owner_qualified_predecessor ? owner_qualified_predecessor->generation.value() : 0U;
  const auto floor = std::max(predecessor, prior_reservation_generation);
  return floor < limit && candidate_generation.value() == floor + 1U;
}

bool ComponentLifecycleIntentDTO::locally_valid() const noexcept {
  if (close_incident_recovery &&
      (intent != ComponentLifecycleIntentKindDTO::kRecover ||
       !reason || *reason != "cell closed by operator" ||
       program_node_evidence || apply_generation_selection ||
       close_incident_recovery->expected_incident_id.empty() ||
       close_incident_recovery->expected_incident_id.size() > kMaxLifecycleIdentityBytes))
    return false;
  const bool apply_generation_valid = apply_generation_selection &&
      apply_generation_selection->locally_valid() && !program_node_evidence &&
      (intent == ComponentLifecycleIntentKindDTO::kPrepare ||
       intent == ComponentLifecycleIntentKindDTO::kEnable ||
       intent == ComponentLifecycleIntentKindDTO::kDisable) &&
      expected_component_lifecycle_generation.value() ==
          (apply_generation_selection->predecessor_generation
               ? apply_generation_selection->predecessor_generation->value()
               : apply_generation_selection->owner_qualified_predecessor
                   ? apply_generation_selection->owner_qualified_predecessor->generation.value() : 0U);
  if (!context_valid(request_context) || !bounded_identity(controllable_component_id) ||
      !known(intent) || !bounded_identity(expected_graph_revision) ||
      (apply_generation_selection ? !apply_generation_valid
                                  : !expected_component_lifecycle_generation.nonzero()) ||
      expected_control_domains.empty() ||
      !expected_admission_generation.nonzero() ||
      (program_node_evidence.has_value() &&
       (!program_node_evidence->locally_valid() ||
        program_node_evidence->owner_api != "cclc.lifecycle")) ||
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
        binding_count > kMaxLifecycleExpectedBindings - domain.control_bindings().size()) {
      return false;
    }
    binding_count += domain.control_bindings().size();
  }
  return true;
}

bool ComponentLifecycleIntentDTO::replay_equal(
    const ComponentLifecycleIntentDTO& other) const noexcept {
  return request_context == other.request_context &&
         controllable_component_id == other.controllable_component_id && intent == other.intent &&
         expected_graph_revision == other.expected_graph_revision &&
         expected_component_lifecycle_generation == other.expected_component_lifecycle_generation &&
         expected_control_domains == other.expected_control_domains && reason == other.reason &&
         expected_admission_generation == other.expected_admission_generation &&
         program_node_evidence == other.program_node_evidence &&
         apply_generation_selection == other.apply_generation_selection &&
         close_incident_recovery == other.close_incident_recovery;
}

bool ComponentLifecycleAcceptedDTO::locally_valid() const noexcept {
  return bounded_identity(request_id) && IsAcceptedRequestReplayStatus(replay_status) &&
         bounded_identity(lifecycle_transaction_id) && lifecycle_transaction_generation.nonzero() &&
         component_lifecycle_generation.nonzero() && accepted_at_ns >= 0 &&
         timeout_duration_ns > 0 && timeout_duration_ns <= kMaxLifecycleTransactionTimeoutNs;
}

namespace {

[[nodiscard]] bool normalized_health_valid(
    ComponentLifecycleStateDTO state, const std::vector<LifecycleFactDTO>& facts,
    const std::optional<ErrorDTO>& diagnostic) noexcept {
  if (!known(state) || facts.size() > kMaxLifecycleFacts || !error_valid(diagnostic)) {
    return false;
  }
  const auto less = [](const LifecycleFactDTO& left, const LifecycleFactDTO& right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  };
  return sorted_unique(facts, less) &&
         std::all_of(facts.begin(), facts.end(),
                     [](const LifecycleFactDTO& fact) { return fact.locally_valid(); });
}

[[nodiscard]] LifecyclePostconditionResultDTO evaluate_normalized_predicate(
    const LifecyclePredicateDTO& predicate, ComponentLifecycleStateDTO observed_state,
    const std::vector<LifecycleFactDTO>& facts) noexcept {
  if (!std::binary_search(predicate.allowed_states.begin(), predicate.allowed_states.end(),
                          observed_state)) {
    return LifecyclePostconditionResultDTO::kNotSatisfied;
  }
  for (const auto& expected : predicate.fact_predicates) {
    const auto fact = std::lower_bound(
        facts.begin(), facts.end(), expected.lifecycle_fact_id,
        [](const LifecycleFactDTO& candidate, const LifecycleFactId& id) {
          return candidate.lifecycle_fact_id < id;
        });
    if (fact == facts.end() || fact->lifecycle_fact_id != expected.lifecycle_fact_id ||
        fact->value.kind == LifecycleFactValueKindDTO::kUnknown) {
      return LifecyclePostconditionResultDTO::kInconclusive;
    }
    if (fact->value != expected.expected_value) {
      return LifecyclePostconditionResultDTO::kNotSatisfied;
    }
  }
  return LifecyclePostconditionResultDTO::kSatisfied;
}

}  // namespace

bool LifecycleDomainHealthDTO::locally_valid() const noexcept {
  return normalized_health_valid(observed_state, facts, diagnostic);
}

LifecyclePostconditionResultDTO EvaluateLifecyclePredicate(
    const LifecyclePredicateDTO& predicate,
    const BrandLifecycleObservationDTO& observation) noexcept {
  if (!predicate.locally_valid() || !observation.locally_valid()) {
    return LifecyclePostconditionResultDTO::kInconclusive;
  }
  return evaluate_normalized_predicate(predicate, observation.observed_state, observation.facts);
}

LifecyclePostconditionResultDTO EvaluateLifecyclePredicate(
    const LifecyclePredicateDTO& predicate, const LifecycleDomainHealthDTO& health) noexcept {
  if (!predicate.locally_valid() || !health.locally_valid()) {
    return LifecyclePostconditionResultDTO::kInconclusive;
  }
  return evaluate_normalized_predicate(predicate, health.observed_state, health.facts);
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
      !normalized_health_valid(observed_state, facts, diagnostic)) {
    return false;
  }
  return true;
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
  if (payload.valueless_by_exception() || !bounded_identity(control_domain_id) ||
      !bounded_identity(control_domain_revision))
    return false;
  if (const auto* value = provider()) {
    if (!value->provider.locally_valid() || !bounded_identity(value->provider_runtime_epoch))
      return false;
    if (value->output_resources.size() > kMaxLifecycleCurrentResources ||
        !std::is_sorted(value->output_resources.begin(), value->output_resources.end()) ||
        std::adjacent_find(value->output_resources.begin(), value->output_resources.end()) !=
            value->output_resources.end() ||
        std::any_of(value->output_resources.begin(), value->output_resources.end(),
                    [](const auto& id) { return !bounded_identity(id); }))
      return false;
    if (!value->active_control_configuration) return true;
    const auto& configuration = *value->active_control_configuration;
    return configuration.locally_valid() &&
           configuration.source_runtime_unit_id == value->provider.provider_runtime_unit_id &&
           configuration.source_runtime_epoch == value->provider_runtime_epoch &&
           configuration.signal_configuration_digest == value->provider.signal_configuration_digest;
  }
  const auto& [controller_topology_id, topology_revision, active_control_configuration,
               current_control_bindings, current_resources] = controller();
  if (!bounded_identity(controller_topology_id) || !bounded_identity(topology_revision) ||
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

bool ProviderDomainActiveConfigurationDTO::locally_valid() const noexcept {
  return bounded_identity(active_control_configuration_id) &&
         active_control_configuration_generation.nonzero() &&
         bounded_identity(source_runtime_unit_id) && bounded_identity(source_runtime_epoch) &&
         source_sequence != 0U && IsExactLowercaseSha256Digest(signal_configuration_digest.value());
}

bool SelectedControllerOccurrenceDTO::locally_valid() const noexcept {
  if ((arm_binding_epoch && !arm_binding_epoch->nonzero()) || domains.empty() ||
      domains.size() > kMaxLifecycleResolvedDomains) return false;
  return sorted_unique(domains, [](const auto& lhs, const auto& rhs) {
           return lhs.control_domain_id < rhs.control_domain_id;
         }) && std::all_of(domains.begin(), domains.end(), [](const auto& domain) {
           return bounded_identity(domain.control_domain_id);
         });
}

Result<SelectedControllerOccurrenceDTO> ComputeSelectedControllerSuccessorOccurrence(
    const SelectedControllerOccurrenceDTO& predecessor,
    const std::vector<ControlDomainId>& candidate_domains, bool candidate_has_arm) {
  const auto invalid = [](std::string reason) {
    return Result<SelectedControllerOccurrenceDTO>::Err(
        ErrorDTO{"selected_controller_occurrence_invalid",
                 "selected controller occurrence cannot advance", {{"reason", std::move(reason)}}});
  };
  if (!predecessor.locally_valid() || candidate_domains.empty() ||
      candidate_domains.size() > kMaxLifecycleResolvedDomains ||
      !sorted_unique(candidate_domains, std::less<ControlDomainId>{}) ||
      !std::all_of(candidate_domains.begin(), candidate_domains.end(), [](const auto& domain) {
        return bounded_identity(domain);
      })) return invalid("predecessor_or_candidate_domains_invalid");
  SelectedControllerOccurrenceDTO successor{std::nullopt, {}};
  if (candidate_has_arm) {
    const auto previous = predecessor.arm_binding_epoch
                              ? predecessor.arm_binding_epoch->value() : 0U;
    if (previous == std::numeric_limits<std::uint64_t>::max())
      return invalid("arm_binding_epoch_exhausted");
    successor.arm_binding_epoch = ExecutionBindingEpoch(previous + 1U);
  }
  for (const auto& domain : candidate_domains) {
    const auto prior = std::lower_bound(predecessor.domains.begin(), predecessor.domains.end(), domain,
                                       [](const auto& row, const auto& id) {
                                         return row.control_domain_id < id;
                                       });
    const auto previous = prior != predecessor.domains.end() && prior->control_domain_id == domain
                              ? prior->observation_generation : 0U;
    if (previous == std::numeric_limits<std::uint64_t>::max())
      return invalid("domain_observation_generation_exhausted");
    successor.domains.push_back({domain, previous + 1U});
  }
  return Result<SelectedControllerOccurrenceDTO>::Ok(std::move(successor));
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
        binding_count > kMaxLifecycleResolvedBindings - domain.control_bindings().size() ||
        resource_count > kMaxLifecycleCurrentResources - domain.resource_ids().size()) {
      return false;
    }
    if (const auto* driver = domain.provider();
        driver && (driver->provider.controllable_component_id != controllable_component_id ||
                   !ProviderDomainMatchesOccurrence(
                       domain.control_domain_id, domain.control_domain_revision, driver->provider,
                       driver->provider_runtime_epoch, provider)))
      return false;
    binding_count += domain.control_bindings().size();
    resource_count += domain.resource_ids().size();
  }
  return true;
}

bool ExpectedDomainProofMatchesResolvedScope(
    const ExpectedControlDomainRevisionDTO& expected,
    const ResolvedLifecycleControlDomainScopeDTO& current) noexcept {
  if (!expected.locally_valid() || !current.locally_valid() ||
      expected.control_domain_id != current.control_domain_id ||
      expected.control_domain_revision != current.control_domain_revision ||
      expected.payload.index() != current.payload.index()) {
    return false;
  }
  if (const auto* provider = expected.provider()) {
    if (provider->provider != current.provider()->provider) return false;
  } else if (expected.controller().controller_topology_id !=
                 current.controller().controller_topology_id ||
             expected.controller().topology_revision != current.controller().topology_revision ||
             expected.control_bindings() != current.control_bindings()) {
    return false;
  }
  return true;
}

bool ExpectedDomainMatchesResolvedScope(
    const ExpectedControlDomainRevisionDTO& expected,
    const ResolvedLifecycleControlDomainScopeDTO& current) noexcept {
  if (!ExpectedDomainProofMatchesResolvedScope(expected, current)) return false;
  const auto& expected_configuration = expected.expected_configuration();
  const auto actual = current.configuration_identity();
  if (expected_configuration.presence == ExpectedActiveControlConfigurationPresenceDTO::kAbsent) {
    return !actual.has_value();
  }
  return actual.has_value() &&
         *expected_configuration.active_control_configuration_id ==
             actual->active_control_configuration_id &&
         *expected_configuration.active_control_configuration_generation ==
             actual->active_control_configuration_generation;
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
  if (current_scope && current_scope->provider()) {
    const auto& driver = *current_scope->provider();
    if (!ProviderDomainMatchesOccurrence(control_domain_id, current_scope->control_domain_revision,
                                         driver.provider, driver.provider_runtime_epoch,
                                         *resolved_provider_occurrence))
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
  const bool selected = selected_bring_up.has_value();
  const bool delegated = apply_bring_up.has_value();
  // A genuinely initial delegated operation has no predecessor aggregate.
  // Installed domains still report their own observed states independently.
  const bool initial_delegated = delegated && !apply_bring_up->recovery &&
      !apply_bring_up->predecessor_occurrence &&
      !apply_bring_up->successor_occurrence_floor &&
      std::none_of(apply_bring_up->selected_scope.domains.begin(),
                   apply_bring_up->selected_scope.domains.end(), [](const auto& domain) {
        return domain.predecessor_control_domain_revision.has_value();
      });
  const auto expected_component_generation =
      accepted_intent.expected_component_lifecycle_generation.value();
  const auto accepted_component_generation = acceptance.component_lifecycle_generation.value();
  const bool component_generation_relation_valid =
      accepted_intent.apply_generation_selection
          ? delegated && !selected &&
                accepted_component_generation ==
                    accepted_intent.apply_generation_selection->candidate_generation.value()
          : expected_component_generation != std::numeric_limits<std::uint64_t>::max() &&
      (accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable
           ? accepted_component_generation > expected_component_generation
           : accepted_component_generation == expected_component_generation + 1U);
  if (!acceptance.locally_valid() || !accepted_intent.locally_valid() ||
      acceptance.request_id != accepted_intent.request_context.request_id ||
      status_sequence == 0U || !bounded_identity(status_owner_epoch) ||
      (initial_delegated ? previous_state.has_value()
                         : previous_state.has_value() != current_state.has_value()) ||
      (previous_state.has_value() && !known(*previous_state)) ||
      (current_state.has_value() && !known(*current_state)) ||
      !component_requested_state_matches_intent(requested_state, accepted_intent.intent) ||
      !known(phase) || domains.empty() ||
      domains.size() > (selected || delegated || accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable
                            ? kMaxLifecycleDisableDomainUnion
                            : kMaxLifecycleResolvedDomains) ||
      !error_valid(error) || accepted_at_ns != acceptance.accepted_at_ns || accepted_at_ns < 0 ||
      latest_updated_at_ns < 0 ||
      (status_owner_epoch == accepted_intent.request_context.request_owner_epoch &&
       latest_updated_at_ns < accepted_at_ns) ||
      (status_owner_epoch != accepted_intent.request_context.request_owner_epoch &&
       remaining_duration_ns != 0) ||
      remaining_duration_ns < 0 || remaining_duration_ns > acceptance.timeout_duration_ns ||
      !component_generation_relation_valid ||
      (selected && !selected_status_evidence_valid(*this)) ||
      (delegated && (!apply_status_evidence_valid(*this) ||
          (accepted_intent.apply_generation_selection &&
           accepted_intent.apply_generation_selection->owner_qualified_predecessor && apply_bring_up->recovery &&
           accepted_intent.apply_generation_selection->owner_qualified_predecessor->snapshot_digest !=
               apply_bring_up->recovery->predecessor_snapshot_digest)))) {
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
      current_binding_count += domain.current_scope->control_bindings().size();
      if (current_binding_count > kMaxLifecycleResolvedBindings) {
        return false;
      }
      if (!domain.current_scope->provider() &&
          domain.current_scope->controller().active_control_configuration.has_value()) {
        const auto& configuration =
            *domain.current_scope->controller().active_control_configuration;
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
      expected_binding_count += domain.expected_domain->control_bindings().size();
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

    if (!selected && !delegated && accepted_intent.intent != ComponentLifecycleIntentKindDTO::kDisable &&
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
    const bool retired_selected_domain =
        (selected || delegated) && !domain.expected_domain.has_value() && !domain.error.has_value() &&
        domain.effect == ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
    all_succeeded = all_succeeded &&
                    (retired_selected_domain ||
                     (domain.action_state == ComponentLifecycleDomainActionStateDTO::kSucceeded &&
                      !domain.error.has_value()));
  }

  if (expected_index != accepted_intent.expected_control_domains.size() ||
      current_domain_count > (selected || delegated ? kMaxLifecycleDisableDomainUnion :
                                          kMaxLifecycleResolvedDomains) ||
      expected_binding_count + current_binding_count > kMaxLifecycleStatusBindingUnion) {
    return false;
  }

  if (initial_delegated) {
    if ((phase == ComponentLifecycleTransactionPhaseDTO::kSucceeded &&
         current_state != requested_state.state) ||
        (current_state && (current_domain_count != domains.size() ||
        std::any_of(domains.begin(), domains.end(), [&](const auto& domain) {
          return domain.current_state != current_state;
        })))) return false;
  } else if ((current_domain_count != 0U) != current_state.has_value()) {
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

  // Disable is idle-only: it runs under the same cell claim as Prepare and
  // Enable and never carries a Kernel Stop or configuration-fence record.
  return accepted_intent.intent != ComponentLifecycleIntentKindDTO::kDisable ||
         phase != ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown ||
         reconciliation_required;
}

}  // namespace manyforge::core
