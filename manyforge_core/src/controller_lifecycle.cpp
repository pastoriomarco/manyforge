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

[[nodiscard]] bool expected_controller_less(
    const ControllerLifecycleExpectedControllerDTO& lhs,
    const ControllerLifecycleExpectedControllerDTO& rhs) noexcept {
  return lhs.controller_role_id < rhs.controller_role_id;
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

bool ControllerLifecycleExpectedEndpointDTO::locally_valid() const noexcept {
  return bounded_identity(control_binding_id) && bounded_identity(control_binding_revision) &&
         bounded_identity(control_route_id) && bounded_identity(endpoint_role_id) &&
         !logical_endpoint_symbol.empty() &&
         logical_endpoint_symbol.size() <= kMaxControllerLifecycleEndpointSymbolBytes &&
         (is_command_endpoint || is_state_endpoint);
}

bool ControllerLifecycleExpectedBindingDTO::locally_valid() const noexcept {
  if (gripper_actuation_kind && *gripper_actuation_kind != GripperActuationKind::kPositionCommanded &&
      *gripper_actuation_kind != GripperActuationKind::kPneumatic) return false;
  return bounded_identity(control_binding_id) && bounded_identity(control_binding_revision) &&
         bounded_identity(control_interface_contract_id) &&
         bounded_identity(control_interface_revision) &&
         logical_owner_resources.empty() &&
         (!claimed_command_interfaces.empty() || !claimed_state_interfaces.empty()) &&
         interface_set_valid(claimed_command_interfaces) &&
         interface_set_valid(claimed_state_interfaces);
}

bool ControllerLifecycleExpectedDomainDTO::locally_valid() const noexcept {
  const bool no_physical = physical_expectation ==
      ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone;
  if (physical_expectation != ControllerLifecyclePhysicalExpectation::kRequired && !no_physical)
    return false;
  if (no_physical && (!controller_topology.controllers.empty() ||
      !expected_controllers.empty() || !controller_topology.chain_edges.empty() ||
      std::any_of(controller_topology.activation_sets.begin(), controller_topology.activation_sets.end(),
          [](const auto& row) { return !row.active_controller_role_ids.empty() || !row.active_chain_edges.empty(); }) ||
      std::any_of(controller_topology.legal_routes.begin(), controller_topology.legal_routes.end(),
          [](const auto& row) { return !row.ordered_controller_role_ids.empty(); })))
    return false;
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
      desired_configuration.activation_set_id.empty() || (!no_physical && controller_topology.controllers.empty()) ||
      controller_topology.controllers.size() > kMaxControllerTopologyControllers ||
      expected_bindings.size() != desired_configuration.binding_routes.size() ||
      expected_bindings.empty() ||
      expected_bindings.size() > kMaxControllerLifecycleBindingRoutes ||
      expected_controllers.size() != controller_topology.controllers.size() ||
      (!no_physical && expected_controllers.empty()) || expected_endpoints.empty() ||
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
                   [no_physical](const auto& binding) {
                     if (binding.gripper_actuation_kind &&
                         *binding.gripper_actuation_kind != GripperActuationKind::kPositionCommanded &&
                         *binding.gripper_actuation_kind != GripperActuationKind::kPneumatic) return false;
                     return no_physical
                         ? bounded_identity(binding.control_binding_id) && bounded_identity(binding.control_binding_revision) &&
                           bounded_identity(binding.control_interface_contract_id) && bounded_identity(binding.control_interface_revision) &&
                           binding.claimed_command_interfaces.empty() && binding.claimed_state_interfaces.empty() &&
                           !binding.logical_owner_resources.empty() &&
                           binding.logical_owner_resources.size() <= kMaxControllerLifecycleInterfaces &&
                           sorted_unique(binding.logical_owner_resources, std::less<>{}) &&
                           std::all_of(binding.logical_owner_resources.begin(), binding.logical_owner_resources.end(),
                               [](const auto& resource) { return bounded_identity(resource); })
                         : binding.locally_valid();
                   }) ||
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
  if (selected_activation == nullptr || (!no_physical && selected_activation->active_controller_role_ids.empty())) {
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

}  // namespace manyforge::core
