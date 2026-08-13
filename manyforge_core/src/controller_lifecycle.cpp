#include <algorithm>
#include <manyforge_core/types/controller_lifecycle.hpp>

namespace manyforge::core {
namespace {

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId& value) noexcept {
  return !value.empty() && value.value().size() <= 256U;
}

[[nodiscard]] bool portable_token(std::string_view value, std::size_t maximum) noexcept {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  const auto alphanumeric = [](unsigned char character) noexcept {
    return (character >= static_cast<unsigned char>('A') &&
            character <= static_cast<unsigned char>('Z')) ||
           (character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('z')) ||
           (character >= static_cast<unsigned char>('0') &&
            character <= static_cast<unsigned char>('9'));
  };
  if (!alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [&](char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return alphanumeric(byte) || character == '.' || character == '_' || character == '-';
  });
}

template <typename Value, typename Less>
[[nodiscard]] bool sorted_unique(const std::vector<Value>& values, Less less) noexcept {
  return std::is_sorted(values.begin(), values.end(), less) &&
         std::adjacent_find(values.begin(), values.end(), [&](const Value& lhs, const Value& rhs) {
           return !less(lhs, rhs) && !less(rhs, lhs);
         }) == values.end();
}

[[nodiscard]] bool interface_set_valid(
    const std::vector<ControllerInterfaceInstanceDTO>& values) noexcept {
  return values.size() <= kMaxControllerLifecycleInterfaces &&
         sorted_unique(values, CanonicalControllerInterfaceInstanceLess) &&
         std::all_of(values.begin(), values.end(),
                     [](const auto& value) { return value.locally_valid(); });
}

[[nodiscard]] bool desired_route_valid(const DesiredControlBindingRouteDTO& route) noexcept {
  return bounded_identity(route.control_binding_id) &&
         bounded_identity(route.control_binding_revision) &&
         bounded_identity(route.control_route_id);
}

[[nodiscard]] bool desired_route_less(const DesiredControlBindingRouteDTO& lhs,
                                      const DesiredControlBindingRouteDTO& rhs) noexcept {
  return std::tie(lhs.control_binding_id, lhs.control_binding_revision, lhs.control_route_id) <
         std::tie(rhs.control_binding_id, rhs.control_binding_revision, rhs.control_route_id);
}

[[nodiscard]] bool edge_less(const ControllerChainEdgeRefDTO& lhs,
                             const ControllerChainEdgeRefDTO& rhs) noexcept {
  return std::tie(lhs.upstream_controller_role_id, lhs.downstream_controller_role_id) <
         std::tie(rhs.upstream_controller_role_id, rhs.downstream_controller_role_id);
}

[[nodiscard]] bool edge_refs_valid(const std::vector<ControllerChainEdgeRefDTO>& edges) noexcept {
  return edges.size() <= kMaxControllerTopologyChainEdges && sorted_unique(edges, edge_less) &&
         std::all_of(edges.begin(), edges.end(), [](const auto& edge) {
           return bounded_identity(edge.upstream_controller_role_id) &&
                  bounded_identity(edge.downstream_controller_role_id) &&
                  edge.upstream_controller_role_id != edge.downstream_controller_role_id;
         });
}

[[nodiscard]] bool controller_roles_valid(const std::vector<ControllerRoleId>& roles,
                                          bool may_be_empty) noexcept {
  return (may_be_empty || !roles.empty()) && roles.size() <= kMaxControllerTopologyControllers &&
         sorted_unique(roles, std::less<>{}) &&
         std::all_of(roles.begin(), roles.end(),
                     [](const auto& role) { return bounded_identity(role); });
}

[[nodiscard]] bool expected_controller_less(
    const ControllerLifecycleExpectedControllerDTO& lhs,
    const ControllerLifecycleExpectedControllerDTO& rhs) noexcept {
  return lhs.controller_role_id < rhs.controller_role_id;
}

[[nodiscard]] bool observed_controller_less(
    const ControllerLifecycleObservedControllerDTO& lhs,
    const ControllerLifecycleObservedControllerDTO& rhs) noexcept {
  return lhs.expected_controller.controller_role_id < rhs.expected_controller.controller_role_id;
}

[[nodiscard]] bool observed_endpoint_less(
    const ControllerLifecycleObservedEndpointDTO& lhs,
    const ControllerLifecycleObservedEndpointDTO& rhs) noexcept {
  return CanonicalControllerLifecycleEndpointLess(lhs.expected_endpoint, rhs.expected_endpoint);
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

[[nodiscard]] bool known(ControllerLifecycleReconciliationDispositionDTO value) noexcept {
  switch (value) {
    case ControllerLifecycleReconciliationDispositionDTO::kReconciled:
    case ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(ControllerLifecycleTransitionKindDTO value) noexcept {
  switch (value) {
    case ControllerLifecycleTransitionKindDTO::kDeactivate:
    case ControllerLifecycleTransitionKindDTO::kActivate:
      return true;
  }
  return false;
}

}  // namespace

bool ControllerInterfaceInstanceDTO::locally_valid() const noexcept {
  return bounded_identity(resource_id) &&
         portable_token(interface_name, kMaxControllerLifecycleInterfaceTokenBytes);
}

bool ControllerLifecycleExpectedControllerDTO::locally_valid() const noexcept {
  return bounded_identity(controller_role_id) && bounded_identity(implementation_id) &&
         bounded_identity(implementation_revision) && !implementation_type.empty() &&
         implementation_type.size() <= kMaxControllerLifecycleImplementationTypeBytes &&
         bounded_identity(configuration_digest) &&
         interface_set_valid(claimed_command_interfaces) &&
         interface_set_valid(claimed_state_interfaces) &&
         interface_set_valid(exported_reference_interfaces) &&
         interface_set_valid(exported_state_interfaces);
}

bool ControllerLifecycleObservedControllerDTO::locally_valid() const noexcept {
  return expected_controller.locally_valid() && IsKnownControllerLifecycleActivity(activity);
}

bool ControllerLifecycleObservedConfigurationDTO::locally_valid() const noexcept {
  if (!bounded_identity(activation_set_id) || binding_routes.empty() ||
      binding_routes.size() > kMaxControllerLifecycleBindingRoutes ||
      !sorted_unique(binding_routes, CanonicalActiveControlConfigurationBindingRouteLess) ||
      !std::all_of(binding_routes.begin(), binding_routes.end(),
                   [](const auto& route) { return route.locally_valid(); })) {
    return false;
  }
  return std::adjacent_find(binding_routes.begin(), binding_routes.end(),
                            [](const auto& lhs, const auto& rhs) {
                              return lhs.control_binding_id == rhs.control_binding_id;
                            }) == binding_routes.end();
}

bool ControllerLifecycleExpectedEndpointDTO::locally_valid() const noexcept {
  return bounded_identity(control_binding_id) && bounded_identity(control_binding_revision) &&
         bounded_identity(control_route_id) && bounded_identity(endpoint_role_id) &&
         !logical_endpoint_symbol.empty() &&
         logical_endpoint_symbol.size() <= kMaxControllerLifecycleEndpointSymbolBytes &&
         (is_command_endpoint || is_state_endpoint);
}

bool ControllerLifecycleExpectedBindingDTO::locally_valid() const noexcept {
  return bounded_identity(control_binding_id) && bounded_identity(control_binding_revision) &&
         bounded_identity(control_interface_contract_id) &&
         bounded_identity(control_interface_revision) &&
         (!claimed_command_interfaces.empty() || !claimed_state_interfaces.empty()) &&
         interface_set_valid(claimed_command_interfaces) &&
         interface_set_valid(claimed_state_interfaces);
}

bool ControllerLifecycleExpectedDomainDTO::locally_valid() const noexcept {
  if (!bounded_identity(control_domain_id) || !bounded_identity(control_domain_revision) ||
      !bounded_identity(controller_topology_id) || !bounded_identity(topology_revision) ||
      !bounded_identity(product_id) || !bounded_identity(product_contract_id) ||
      !bounded_identity(product_contract_revision) || !bounded_identity(control_artifact_digest) ||
      !bounded_identity(source_conformance_digest) || !bounded_identity(source_runtime_unit_id) ||
      !bounded_identity(source_runtime_epoch) ||
      controller_topology.controller_topology_id != controller_topology_id ||
      controller_topology.topology_revision != topology_revision ||
      desired_configuration.control_domain_id != control_domain_id ||
      desired_configuration.control_domain_revision != control_domain_revision ||
      desired_configuration.controller_topology_id != controller_topology_id ||
      desired_configuration.topology_revision != topology_revision ||
      desired_configuration.activation_set_id.empty() || controller_topology.controllers.empty() ||
      controller_topology.controllers.size() > kMaxControllerTopologyControllers ||
      expected_bindings.size() != desired_configuration.binding_routes.size() ||
      expected_bindings.empty() ||
      expected_bindings.size() > kMaxControllerLifecycleBindingRoutes ||
      expected_controllers.size() != controller_topology.controllers.size() ||
      expected_controllers.empty() || expected_endpoints.empty() ||
      expected_endpoints.size() > kMaxControllerLifecycleEndpoints ||
      desired_configuration.binding_routes.empty() ||
      desired_configuration.binding_routes.size() > kMaxControllerLifecycleBindingRoutes ||
      !sorted_unique(expected_controllers, expected_controller_less) ||
      !std::all_of(expected_controllers.begin(), expected_controllers.end(),
                   [](const auto& controller) { return controller.locally_valid(); }) ||
      !sorted_unique(expected_endpoints, CanonicalControllerLifecycleEndpointLess) ||
      !std::all_of(expected_endpoints.begin(), expected_endpoints.end(),
                   [](const auto& endpoint) { return endpoint.locally_valid(); }) ||
      !sorted_unique(expected_bindings, CanonicalControllerLifecycleExpectedBindingLess) ||
      !std::all_of(expected_bindings.begin(), expected_bindings.end(),
                   [](const auto& binding) { return binding.locally_valid(); }) ||
      !sorted_unique(desired_configuration.binding_routes, desired_route_less) ||
      !std::all_of(desired_configuration.binding_routes.begin(),
                   desired_configuration.binding_routes.end(), desired_route_valid)) {
    return false;
  }

  for (std::size_t index = 0U; index < controller_topology.controllers.size(); ++index) {
    const auto& controller = controller_topology.controllers[index];
    if (!bounded_identity(controller.controller_role_id)) {
      return false;
    }
    for (std::size_t other = index + 1U; other < controller_topology.controllers.size(); ++other) {
      if (controller.controller_role_id ==
          controller_topology.controllers[other].controller_role_id) {
        return false;
      }
    }
    const auto expected = std::lower_bound(
        expected_controllers.begin(), expected_controllers.end(), controller.controller_role_id,
        [](const ControllerLifecycleExpectedControllerDTO& candidate,
           const ControllerRoleId& role) { return candidate.controller_role_id < role; });
    if (expected == expected_controllers.end() ||
        expected->controller_role_id != controller.controller_role_id) {
      return false;
    }
  }

  const ControllerActivationSetDTO* selected_activation = nullptr;
  for (const auto& activation : controller_topology.activation_sets) {
    if (activation.activation_set_id == desired_configuration.activation_set_id) {
      if (selected_activation != nullptr) {
        return false;
      }
      selected_activation = &activation;
    }
  }
  if (selected_activation == nullptr || selected_activation->active_controller_role_ids.empty()) {
    return false;
  }

  for (const auto& desired_route : desired_configuration.binding_routes) {
    const auto expected_binding = std::lower_bound(
        expected_bindings.begin(), expected_bindings.end(), desired_route.control_binding_id,
        [](const ControllerLifecycleExpectedBindingDTO& candidate,
           const ControlBindingId& binding_id) {
          return candidate.control_binding_id < binding_id;
        });
    if (expected_binding == expected_bindings.end() ||
        expected_binding->control_binding_id != desired_route.control_binding_id ||
        expected_binding->control_binding_revision != desired_route.control_binding_revision) {
      return false;
    }
    const ControlRouteDTO* selected_route = nullptr;
    for (const auto& legal_route : controller_topology.legal_routes) {
      if (legal_route.control_route_id == desired_route.control_route_id) {
        if (selected_route != nullptr) {
          return false;
        }
        selected_route = &legal_route;
      }
    }
    if (selected_route == nullptr ||
        selected_route->control_interface_contract_id !=
            expected_binding->control_interface_contract_id ||
        selected_route->control_interface_revision !=
            expected_binding->control_interface_revision ||
        std::find(selected_route->compatible_activation_set_ids.begin(),
                  selected_route->compatible_activation_set_ids.end(),
                  desired_configuration.activation_set_id) ==
            selected_route->compatible_activation_set_ids.end()) {
      return false;
    }
    if (std::none_of(
            expected_endpoints.begin(), expected_endpoints.end(), [&](const auto& endpoint) {
              return endpoint.control_binding_id == desired_route.control_binding_id &&
                     endpoint.control_binding_revision == desired_route.control_binding_revision &&
                     endpoint.control_route_id == desired_route.control_route_id;
            })) {
      return false;
    }
  }

  for (const auto& binding : expected_bindings) {
    if (std::none_of(expected_endpoints.begin(), expected_endpoints.end(),
                     [&](const auto& endpoint) {
                       return endpoint.control_binding_id == binding.control_binding_id &&
                              endpoint.control_binding_revision == binding.control_binding_revision;
                     })) {
      return false;
    }
  }
  return std::all_of(
      expected_endpoints.begin(), expected_endpoints.end(), [&](const auto& endpoint) {
        const auto binding = std::find_if(
            expected_bindings.begin(), expected_bindings.end(), [&](const auto& candidate) {
              return candidate.control_binding_id == endpoint.control_binding_id &&
                     candidate.control_binding_revision == endpoint.control_binding_revision;
            });
        if (binding == expected_bindings.end()) {
          return false;
        }
        const auto route =
            std::find_if(controller_topology.legal_routes.begin(),
                         controller_topology.legal_routes.end(), [&](const auto& candidate) {
                           return candidate.control_route_id == endpoint.control_route_id;
                         });
        return route != controller_topology.legal_routes.end() &&
               route->control_interface_contract_id == binding->control_interface_contract_id &&
               route->control_interface_revision == binding->control_interface_revision &&
               !route->compatible_activation_set_ids.empty();
      });
}

Result<ControllerLifecycleExpectedDomain> verify_controller_lifecycle_expected_domain(
    ControllerLifecycleExpectedDomainDTO value) {
  if (!value.locally_valid()) {
    return Result<ControllerLifecycleExpectedDomain>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvariantViolation)),
                 "controller lifecycle expected-domain closure is invalid",
                 {{"reason", "controller_lifecycle_expected_domain_invalid"}}});
  }
  return Result<ControllerLifecycleExpectedDomain>::Ok(
      ControllerLifecycleExpectedDomain(std::move(value)));
}

bool ControllerLifecycleObservationQueryDTO::locally_valid() const noexcept {
  return bounded_identity(control_domain_id) && bounded_identity(control_domain_revision) &&
         bounded_identity(controller_topology_id) && bounded_identity(topology_revision) &&
         bounded_identity(product_id) && bounded_identity(product_contract_id) &&
         bounded_identity(product_contract_revision) && bounded_identity(control_artifact_digest) &&
         bounded_identity(source_conformance_digest) && bounded_identity(source_runtime_unit_id) &&
         bounded_identity(source_runtime_epoch);
}

ControllerLifecycleObservationQueryDTO MakeControllerLifecycleObservationQuery(
    const ControllerLifecycleExpectedDomainDTO& expected) {
  return {
      expected.control_domain_id,
      expected.control_domain_revision,
      expected.controller_topology_id,
      expected.topology_revision,
      expected.product_id,
      expected.product_contract_id,
      expected.product_contract_revision,
      expected.control_artifact_digest,
      expected.source_conformance_digest,
      expected.source_runtime_unit_id,
      expected.source_runtime_epoch,
  };
}

bool ControllerLifecycleObservationDTO::locally_valid() const noexcept {
  return query.locally_valid() && source_sequence != 0U && observed_at_ns >= 0 &&
         received_at_ns >= observed_at_ns && valid_until_ns > received_at_ns &&
         (!active_configuration.has_value() || active_configuration->locally_valid()) &&
         !controllers.empty() && controllers.size() <= kMaxControllerTopologyControllers &&
         sorted_unique(controllers, observed_controller_less) &&
         std::all_of(controllers.begin(), controllers.end(),
                     [](const auto& controller) { return controller.locally_valid(); }) &&
         edge_refs_valid(active_chain_edges) && !endpoints.empty() &&
         endpoints.size() <= kMaxControllerLifecycleEndpoints &&
         sorted_unique(endpoints, observed_endpoint_less) &&
         std::all_of(endpoints.begin(), endpoints.end(),
                     [](const auto& endpoint) { return endpoint.locally_valid(); });
}

bool ControllerLifecycleReconciliationDTO::locally_valid() const noexcept {
  const bool reasons_valid =
      reason_codes.size() <= kMaxControllerLifecycleReasonCodes &&
      sorted_unique(reason_codes, std::less<>{}) &&
      std::all_of(reason_codes.begin(), reason_codes.end(), [](const auto& reason) {
        return portable_token(reason, kMaxControllerLifecycleReasonCodeBytes);
      });
  return query.locally_valid() && source_sequence != 0U && known(disposition) &&
         bounded_identity(desired_activation_set_id) && !desired_binding_routes.empty() &&
         desired_binding_routes.size() <= kMaxControllerLifecycleBindingRoutes &&
         sorted_unique(desired_binding_routes, desired_route_less) &&
         std::all_of(desired_binding_routes.begin(), desired_binding_routes.end(),
                     desired_route_valid) &&
         reasons_valid &&
         ((disposition == ControllerLifecycleReconciliationDispositionDTO::kReconciled &&
           reason_codes.empty()) ||
          (disposition ==
               ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled &&
           !reason_codes.empty()));
}

bool ControllerLifecycleTransitionStepDTO::locally_valid() const noexcept {
  return known(kind) && controller_roles_valid(controller_role_ids, false);
}

bool ControllerLifecycleTransitionPlanDTO::locally_valid() const noexcept {
  if (!query.locally_valid() || expected_source_sequence == 0U ||
      !controller_roles_valid(observed_active_controller_role_ids, true) ||
      !edge_refs_valid(observed_active_chain_edges) ||
      !bounded_identity(desired_activation_set_id) ||
      !controller_roles_valid(desired_active_controller_role_ids, false) ||
      !edge_refs_valid(desired_active_chain_edges) ||
      steps.size() > kMaxControllerLifecycleTransitionSteps ||
      !std::all_of(steps.begin(), steps.end(),
                   [](const auto& step) { return step.locally_valid(); })) {
    return false;
  }
  bool saw_activation = false;
  for (const auto& step : steps) {
    if (step.kind == ControllerLifecycleTransitionKindDTO::kActivate) {
      saw_activation = true;
    } else if (saw_activation) {
      return false;
    }
  }
  return true;
}

bool ControllerLifecycleTransitionIntentDTO::locally_valid() const noexcept {
  return query.locally_valid() && expected_source_sequence != 0U &&
         step_index < kMaxControllerLifecycleTransitionSteps && step.locally_valid() &&
         remaining_duration_ns > 0;
}

bool ControllerLifecycleTransitionResultDTO::locally_valid() const noexcept {
  if (!intent.locally_valid() || (error.has_value() && !error_valid(*error))) {
    return false;
  }
  switch (dispatch_state) {
    case ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched:
      return !known_result.has_value() && error.has_value();
    case ControllerLifecycleTransitionDispatchStateDTO::kDispatchedKnown:
      if (!known_result.has_value()) {
        return false;
      }
      switch (*known_result) {
        case ControllerLifecycleTransitionKnownResultDTO::kSucceeded:
          return !error.has_value();
        case ControllerLifecycleTransitionKnownResultDTO::kFailed:
          return error.has_value();
      }
      return false;
    case ControllerLifecycleTransitionDispatchStateDTO::kDispatchedUncertain:
      return !known_result.has_value() && error.has_value();
  }
  return false;
}

}  // namespace manyforge::core
