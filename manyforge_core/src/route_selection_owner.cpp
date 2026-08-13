#include <algorithm>
#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <manyforge_core/route_selection_owner.hpp>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId& value) noexcept {
  return !value.empty() && value.value().size() <= kMaxRouteSelectionIdentityBytes;
}

template <typename T, typename Less>
[[nodiscard]] bool sorted_unique(const std::vector<T>& values, Less less) noexcept {
  return std::is_sorted(values.begin(), values.end(), less) &&
         std::adjacent_find(values.begin(), values.end(), [&](const T& lhs, const T& rhs) {
           return !less(lhs, rhs) && !less(rhs, lhs);
         }) == values.end();
}

[[nodiscard]] Result<RouteSelectionCurrentSnapshot> snapshot_error(std::string reason,
                                                                   std::string message) {
  return Result<RouteSelectionCurrentSnapshot>::Err(
      ErrorDTO{"route_selection_current_snapshot_invalid",
               std::move(message),
               {{"reason", std::move(reason)}}});
}

[[nodiscard]] Result<void> receipt_error(std::string reason, std::string message) {
  return Result<void>::Err(ErrorDTO{
      "route_readiness_publication_invalid", std::move(message), {{"reason", std::move(reason)}}});
}

template <typename Value>
[[nodiscard]] Result<Value> configuration_error(std::string reason, std::string message) {
  return Result<Value>::Err(ErrorDTO{"route_configuration_resolution_invalid",
                                     std::move(message),
                                     {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool active_configuration_matches_domain(
    const ActiveControlConfigurationDTO& configuration,
    const ControllerLifecycleExpectedDomainDTO& expected) noexcept {
  if (!configuration.locally_valid() ||
      configuration.control_domain_id != expected.control_domain_id ||
      configuration.control_domain_revision != expected.control_domain_revision ||
      configuration.controller_topology_id != expected.controller_topology_id ||
      configuration.topology_revision != expected.topology_revision ||
      configuration.source_runtime_unit_id != expected.source_runtime_unit_id ||
      configuration.source_runtime_epoch != expected.source_runtime_epoch ||
      configuration.binding_routes.size() != expected.desired_configuration.binding_routes.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < configuration.binding_routes.size(); ++index) {
    const auto& actual = configuration.binding_routes[index];
    const auto& desired = expected.desired_configuration.binding_routes[index];
    if (actual.control_binding_id != desired.control_binding_id ||
        actual.control_binding_revision != desired.control_binding_revision) {
      return false;
    }
  }
  return true;
}

}  // namespace

Result<RouteSelectionCurrentSnapshot> verify_route_selection_current_snapshot(
    RouteSelectionCurrentSnapshotDTO value) {
  if (!bounded_identity(value.retained_bundle_id) || !bounded_identity(value.graph_revision) ||
      value.component_expectations.empty() ||
      value.component_expectations.size() > kMaxRouteSelectionComponents ||
      !sorted_unique(value.component_expectations,
                     CanonicalRouteSelectionComponentExpectationLess) ||
      !std::all_of(value.component_expectations.begin(), value.component_expectations.end(),
                   [](const auto& row) { return row.locally_valid(); }) ||
      !value.configuration_fence_scope.valid()) {
    return snapshot_error("shape_invalid", "the route snapshot has an invalid closed shape");
  }

  auto expected = verify_controller_lifecycle_expected_domain(value.expected_controller_domain);
  if (!expected.ok()) {
    return snapshot_error("expected_domain_invalid",
                          "the route snapshot expected-domain closure is invalid");
  }
  const auto& expected_value = expected.value().value();
  if (value.configuration_fence_scope.domains.size() != 1U ||
      value.configuration_fence_scope.domains.front().control_domain_id !=
          expected_value.control_domain_id ||
      value.configuration_fence_scope.controllable_component_ids.size() !=
          value.component_expectations.size() ||
      value.configuration_fence_scope.bindings.size() !=
          expected_value.desired_configuration.binding_routes.size()) {
    return snapshot_error("scope_incomplete",
                          "the route snapshot fence scope is not the complete expected domain");
  }
  for (std::size_t index = 0U; index < value.component_expectations.size(); ++index) {
    if (value.configuration_fence_scope.controllable_component_ids[index] !=
        value.component_expectations[index].controllable_component_id) {
      return snapshot_error("component_scope_mismatch",
                            "the route snapshot component and fence sets differ");
    }
  }
  for (std::size_t index = 0U; index < expected_value.desired_configuration.binding_routes.size();
       ++index) {
    const auto& desired = expected_value.desired_configuration.binding_routes[index];
    const auto& scoped = value.configuration_fence_scope.bindings[index];
    if (desired.control_binding_id != scoped.control_binding_id ||
        desired.control_binding_revision != scoped.predecessor_control_binding_revision ||
        desired.control_binding_revision != scoped.candidate_control_binding_revision) {
      return snapshot_error("binding_scope_mismatch",
                            "the route snapshot binding and fence sets differ");
    }
  }
  if (value.active_control_configuration.has_value() &&
      !active_configuration_matches_domain(*value.active_control_configuration, expected_value)) {
    return snapshot_error("current_configuration_invalid",
                          "the route snapshot current configuration is not exact domain truth");
  }
  return Result<RouteSelectionCurrentSnapshot>::Ok(
      RouteSelectionCurrentSnapshot(std::move(value), std::move(expected.value())));
}

Result<ActiveControlConfigurationDTO> ResolveRouteSelectionActiveControlConfiguration(
    const RouteSelectionAcceptedDTO& acceptance,
    const ControllerLifecycleExpectedDomain& expected_controller_domain,
    const ControllerLifecycleObservationDTO& observation,
    const ControllerLifecycleReconciliationDTO& reconciliation,
    RouteSelectionEffectDTO transaction_effect,
    std::vector<KinematicResourceSourceProjectionDTO> source_projections) {
  const auto& expected = expected_controller_domain.value();
  if (!acceptance.locally_valid() || !observation.locally_valid() ||
      !reconciliation.locally_valid() ||
      expected.desired_configuration != acceptance.desired_configuration ||
      observation.query != acceptance.controller_lifecycle_observation_query ||
      observation.query != MakeControllerLifecycleObservationQuery(expected) ||
      reconciliation.query != observation.query ||
      reconciliation.source_sequence != observation.source_sequence ||
      reconciliation.desired_activation_set_id !=
          acceptance.desired_configuration.activation_set_id ||
      reconciliation.desired_binding_routes != acceptance.desired_configuration.binding_routes) {
    return configuration_error<ActiveControlConfigurationDTO>(
        "referent_mismatch",
        "route configuration resolution inputs do not form one exact accepted closure");
  }
  const auto recomputed =
      ReconcileControllerLifecycle(expected, observation, observation.received_at_ns);
  if (!recomputed.ok() || recomputed.value() != reconciliation) {
    return configuration_error<ActiveControlConfigurationDTO>(
        "reconciliation_mismatch",
        "route configuration reconciliation was not derived from the supplied observation");
  }
  if (!observation.active_configuration.has_value()) {
    return configuration_error<ActiveControlConfigurationDTO>(
        "observed_configuration_absent",
        "the adapter did not identify one complete selected configuration");
  }
  if (transaction_effect == RouteSelectionEffectDTO::kEffectUncertain) {
    return configuration_error<ActiveControlConfigurationDTO>(
        "effect_uncertain", "an uncertain route effect cannot publish configuration truth");
  }

  const auto state =
      reconciliation.disposition == ControllerLifecycleReconciliationDispositionDTO::kReconciled
          ? ActiveControlConfigurationStateDTO::kReconciled
          : ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  ActiveControlConfigurationDTO result{acceptance.candidate_active_control_configuration_id,
                                       acceptance.desired_configuration.control_domain_id,
                                       acceptance.desired_configuration.control_domain_revision,
                                       observation.active_configuration->binding_routes,
                                       observation.active_configuration->activation_set_id,
                                       acceptance.desired_configuration.controller_topology_id,
                                       acceptance.desired_configuration.topology_revision,
                                       observation.query.source_runtime_unit_id,
                                       observation.query.source_runtime_epoch,
                                       observation.source_sequence,
                                       std::move(source_projections),
                                       acceptance.candidate_active_control_configuration_generation,
                                       state};
  if (!result.locally_valid()) {
    return configuration_error<ActiveControlConfigurationDTO>(
        "result_invalid", "the observed route configuration result is invalid");
  }
  if (state == ActiveControlConfigurationStateDTO::kReconciled &&
      !active_configuration_matches_domain(result, expected)) {
    return configuration_error<ActiveControlConfigurationDTO>(
        "reconciled_result_mismatch",
        "a reconciled result differs from the complete accepted configuration");
  }
  return Result<ActiveControlConfigurationDTO>::Ok(std::move(result));
}

bool RouteReadinessPublicationRowDTO::locally_valid() const noexcept {
  return bounded_identity(controllable_component_id) && component_lifecycle_generation.nonzero() &&
         bounded_identity(control_binding_id) && bounded_identity(control_binding_revision) &&
         bounded_identity(resolved_stop_base_plan_id);
}

bool CanonicalRouteReadinessPublicationRowLess(
    const RouteReadinessPublicationRowDTO& lhs,
    const RouteReadinessPublicationRowDTO& rhs) noexcept {
  return std::tie(lhs.control_binding_id, lhs.control_binding_revision,
                  lhs.controllable_component_id) < std::tie(rhs.control_binding_id,
                                                            rhs.control_binding_revision,
                                                            rhs.controllable_component_id);
}

bool RouteReadinessPublicationReceiptDTO::locally_valid() const noexcept {
  return bounded_identity(route_selection_transaction_id) &&
         route_selection_transaction_generation.nonzero() &&
         bounded_identity(active_control_configuration_id) &&
         active_control_configuration_generation.nonzero() && published_at_ns >= 0 &&
         bounded_identity(receipt_digest) &&
         readiness_rows.size() <= kMaxRouteReadinessPublicationRows &&
         sorted_unique(readiness_rows, CanonicalRouteReadinessPublicationRowLess) &&
         std::all_of(readiness_rows.begin(), readiness_rows.end(),
                     [](const auto& row) { return row.locally_valid(); });
}

Result<void> VerifyRouteReadinessPublicationReceipt(
    const RouteSelectionStatusDTO& durable_status,
    const RouteReadinessPublicationReceiptDTO& receipt) {
  if (!durable_status.locally_valid() ||
      durable_status.phase != RouteSelectionTransactionPhaseDTO::kPublishing ||
      !durable_status.resulting_active_control_configuration.has_value() ||
      !receipt.locally_valid()) {
    return receipt_error("shape_invalid",
                         "the durable route status or publication receipt is invalid");
  }
  const auto& acceptance = durable_status.acceptance;
  const auto& configuration = *durable_status.resulting_active_control_configuration;
  if (receipt.route_selection_transaction_id != acceptance.route_selection_transaction_id ||
      receipt.route_selection_transaction_generation !=
          acceptance.route_selection_transaction_generation ||
      receipt.active_control_configuration_id != configuration.active_control_configuration_id ||
      receipt.active_control_configuration_generation !=
          configuration.active_control_configuration_generation) {
    return receipt_error("identity_mismatch",
                         "the publication receipt does not name the durable route result");
  }

  if (configuration.state == ActiveControlConfigurationStateDTO::kReconciled) {
    if (!receipt.operation_admission_open ||
        receipt.readiness_rows.size() != acceptance.desired_configuration.binding_routes.size()) {
      return receipt_error(
          "readiness_incomplete",
          "a reconciled route requires complete binding readiness and open admission");
    }
    for (std::size_t index = 0U; index < receipt.readiness_rows.size(); ++index) {
      const auto& row = receipt.readiness_rows[index];
      const auto& binding = acceptance.desired_configuration.binding_routes[index];
      if (row.control_binding_id != binding.control_binding_id ||
          row.control_binding_revision != binding.control_binding_revision) {
        return receipt_error("binding_mismatch",
                             "the readiness receipt does not cover the exact selected bindings");
      }
      const auto component = std::find_if(
          acceptance.component_expectations.begin(), acceptance.component_expectations.end(),
          [&](const auto& expected) {
            return expected.controllable_component_id == row.controllable_component_id &&
                   expected.expected_component_lifecycle_generation ==
                       row.component_lifecycle_generation;
          });
      if (component == acceptance.component_expectations.end()) {
        return receipt_error("component_mismatch",
                             "the readiness receipt names a foreign component generation");
      }
    }
    return Result<void>::Ok();
  }

  if (configuration.state != ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled ||
      receipt.operation_admission_open || !receipt.readiness_rows.empty()) {
    return receipt_error("unreconciled_gate_invalid",
                         "unreconciled route truth must retain a closed admission gate");
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
