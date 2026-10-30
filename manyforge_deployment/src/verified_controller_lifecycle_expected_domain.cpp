#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "control_domain_control_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw RetainedBundleError("controller-lifecycle expected-domain decode: " +
                            std::move(message));
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name,
                                           std::string_view path) {
  if (!value.is_object()) {
    fail(std::string(path) + " is not an object");
  }
  const auto *result = value.find_object_member(name);
  if (result == nullptr) {
    fail(std::string(path) + " omits " + std::string(name));
  }
  return *result;
}

[[nodiscard]] const CanonicalValue::Array &array(const CanonicalValue &value,
                                                 std::string_view path) {
  if (!value.is_array()) {
    fail(std::string(path) + " is not an array");
  }
  return value.as_array();
}

[[nodiscard]] const std::string &text(const CanonicalValue &value,
                                      std::string_view path) {
  if (!value.is_string()) {
    fail(std::string(path) + " is not a string");
  }
  return value.as_string();
}

[[nodiscard]] bool boolean(const CanonicalValue &value, std::string_view path) {
  if (!value.is_bool()) {
    fail(std::string(path) + " is not a boolean");
  }
  return value.as_bool();
}

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId &value) {
  return !value.empty() && value.value().size() <= 256U;
}

template <typename Value, typename Less>
void sort_unique(std::vector<Value> &values, Less less, std::string_view path) {
  std::sort(values.begin(), values.end(), less);
  const auto duplicate = std::adjacent_find(
      values.begin(), values.end(), [&](const Value &lhs, const Value &rhs) {
        return !less(lhs, rhs) && !less(rhs, lhs);
      });
  if (duplicate != values.end()) {
    fail(std::string(path) + " contains a duplicate key");
  }
}

template <typename Value, typename Less>
void sort_dedupe_equal(std::vector<Value> &values, Less less,
                       std::string_view path) {
  std::sort(values.begin(), values.end(), less);
  auto output = values.begin();
  for (auto input = values.begin(); input != values.end(); ++input) {
    if (output != values.begin() && !less(*(output - 1), *input) &&
        !less(*input, *(output - 1))) {
      if (*(output - 1) != *input) {
        fail(std::string(path) + " contains conflicting duplicate keys");
      }
      continue;
    }
    if (output != input) {
      *output = std::move(*input);
    }
    ++output;
  }
  values.erase(output, values.end());
}

struct ParsedClaims final {
  std::vector<core::ResourceInterfaceClaimDTO> abstract;
  std::vector<core::ControllerInterfaceInstanceDTO> concrete;
};

[[nodiscard]] ParsedClaims parse_claims(const CanonicalValue &value,
                                        std::string_view path) {
  ParsedClaims result;
  for (const auto &item : array(value, path)) {
    const auto &resource_role =
        text(member(item, "resource_role_id", path), path);
    const auto &resource_id = text(member(item, "resource_id", path), path);
    const auto &interface_name =
        text(member(item, "interface_name", path), path);
    result.abstract.push_back(
        {core::ResourceRoleId(resource_role), interface_name});
    result.concrete.push_back({core::ResourceId(resource_id), interface_name});
  }

  sort_dedupe_equal(
      result.abstract,
      [](const auto &lhs, const auto &rhs) {
        return std::tie(lhs.resource_role_id, lhs.interface_name) <
               std::tie(rhs.resource_role_id, rhs.interface_name);
      },
      path);
  sort_dedupe_equal(result.concrete,
                    core::CanonicalControllerInterfaceInstanceLess, path);
  return result;
}

[[nodiscard]] std::vector<core::ControllerRoleId>
parse_controller_roles(const CanonicalValue &value, std::string_view path) {
  std::vector<core::ControllerRoleId> result;
  for (const auto &item : array(value, path)) {
    result.emplace_back(text(item, path));
  }
  sort_unique(result, std::less<>{}, path);
  return result;
}

[[nodiscard]] std::vector<core::ActivationSetId>
parse_activation_ids(const CanonicalValue &value, std::string_view path) {
  std::vector<core::ActivationSetId> result;
  for (const auto &item : array(value, path)) {
    result.emplace_back(text(item, path));
  }
  sort_unique(result, std::less<>{}, path);
  return result;
}

[[nodiscard]] core::ControllerChainEdgeRefDTO
parse_edge_ref(const CanonicalValue &value, std::string_view path) {
  return {
      core::ControllerRoleId(
          text(member(value, "upstream_controller_role_id", path), path)),
      core::ControllerRoleId(
          text(member(value, "downstream_controller_role_id", path), path)),
  };
}

struct BindingFacts final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;
  core::ControlInterfaceContractId control_interface_contract_id;
  core::ControlInterfaceRevision control_interface_revision;
};

using BindingMap = std::map<std::string, BindingFacts, std::less<>>;

[[nodiscard]] BindingMap parse_bindings(const CanonicalValue &root) {
  BindingMap result;
  const auto &rows =
      array(member(root, "bindings", "control"), "control.bindings");
  for (const auto &row : rows) {
    const auto &binding_id =
        text(member(row, "control_binding_id", "control.bindings"),
             "control.bindings.control_binding_id");
    BindingFacts facts{
        core::ControlBindingId(binding_id),
        core::ControlBindingRevision(
            text(member(row, "control_binding_revision", "control.bindings"),
                 "control.bindings.control_binding_revision")),
        core::ControlInterfaceContractId(text(
            member(row, "control_interface_contract_id", "control.bindings"),
            "control.bindings.control_interface_contract_id")),
        core::ControlInterfaceRevision(
            text(member(row, "control_interface_revision", "control.bindings"),
                 "control.bindings.control_interface_revision")),
    };
    if (!result.emplace(binding_id, std::move(facts)).second) {
      fail("control.bindings duplicates a binding identity");
    }
  }
  if (result.empty()) {
    fail("control.bindings is empty");
  }
  return result;
}

[[nodiscard]] std::vector<core::ControllerClaimDirection>
parse_directions(const CanonicalValue &value, std::string_view path) {
  std::vector<core::ControllerClaimDirection> result;
  for (const auto &item : array(value, path)) {
    const auto &token = text(item, path);
    if (token == "command") {
      result.push_back(core::ControllerClaimDirection::kCommand);
    } else if (token == "state") {
      result.push_back(core::ControllerClaimDirection::kState);
    } else {
      fail(std::string(path) + " contains an unknown direction");
    }
  }
  sort_unique(result, std::less<>{}, path);
  return result;
}

struct ControllerTables final {
  std::vector<core::ControllerTopologyControllerDTO> topology_controllers;
  std::vector<core::ControllerLifecycleExpectedBindingDTO> expected_bindings;
  std::vector<core::ControllerLifecycleExpectedControllerDTO>
      expected_controllers;
};

[[nodiscard]] ControllerTables parse_controllers(const CanonicalValue &root,
                                                 const BindingMap &bindings) {
  ControllerTables result;
  std::map<std::string, core::ControllerLifecycleExpectedBindingDTO,
           std::less<>>
      expected_bindings;
  for (const auto &[binding_id, binding] : bindings) {
    expected_bindings.emplace(binding_id,
                              core::ControllerLifecycleExpectedBindingDTO{
                                  binding.control_binding_id,
                                  binding.control_binding_revision,
                                  binding.control_interface_contract_id,
                                  binding.control_interface_revision,
                                  {},
                                  {}});
  }
  for (const auto &row :
       array(member(root, "controllers", "control"), "control.controllers")) {
    const core::ControllerRoleId role(
        text(member(row, "controller_role_id", "control.controllers"),
             "control.controllers.controller_role_id"));
    const auto &implementation =
        member(row, "implementation", "control.controllers");

    std::map<std::pair<std::string, std::string>,
             std::set<core::ControllerClaimDirection>>
        claim_directions;
    std::vector<core::ResourceInterfaceClaimDTO> abstract_reference_exports;
    std::vector<core::ResourceInterfaceClaimDTO> abstract_state_exports;
    std::vector<core::ControllerInterfaceInstanceDTO> claimed_commands;
    std::vector<core::ControllerInterfaceInstanceDTO> claimed_states;
    std::vector<core::ControllerInterfaceInstanceDTO> reference_exports;
    std::vector<core::ControllerInterfaceInstanceDTO> state_exports;

    const auto &projections =
        array(member(row, "binding_projections", "control.controllers"),
              "control.controllers.binding_projections");
    for (const auto &projection : projections) {
      const auto &binding_id =
          text(member(projection, "control_binding_id",
                      "control.controllers.binding_projections"),
               "control.controllers.binding_projections.control_binding_id");
      const auto binding = bindings.find(binding_id);
      if (binding == bindings.end() ||
          binding->second.control_binding_revision.value() !=
              text(member(projection, "control_binding_revision",
                          "control.controllers.binding_projections"),
                   "control.controllers.binding_projections.control_binding_"
                   "revision")) {
        fail("controller projection names an unknown binding revision");
      }
      const auto directions = parse_directions(
          member(projection, "claimed_directions",
                 "control.controllers.binding_projections"),
          "control.controllers.binding_projections.claimed_directions");
      auto &merged = claim_directions[{
          binding->second.control_interface_contract_id.value(),
          binding->second.control_interface_revision.value()}];
      merged.insert(directions.begin(), directions.end());

      auto command = parse_claims(
          member(projection, "claimed_command_interfaces",
                 "control.controllers.binding_projections"),
          "control.controllers.binding_projections.claimed_command_interfaces");
      auto state = parse_claims(
          member(projection, "claimed_state_interfaces",
                 "control.controllers.binding_projections"),
          "control.controllers.binding_projections.claimed_state_interfaces");
      auto reference =
          parse_claims(member(projection, "exported_reference_interfaces",
                              "control.controllers.binding_projections"),
                       "control.controllers.binding_projections.exported_"
                       "reference_interfaces");
      auto exported_state = parse_claims(
          member(projection, "exported_state_interfaces",
                 "control.controllers.binding_projections"),
          "control.controllers.binding_projections.exported_state_interfaces");
      const bool claims_command =
          std::find(directions.begin(), directions.end(),
                    core::ControllerClaimDirection::kCommand) !=
          directions.end();
      const bool claims_state =
          std::find(directions.begin(), directions.end(),
                    core::ControllerClaimDirection::kState) != directions.end();
      if ((!claims_command && !command.concrete.empty()) ||
          (!claims_state && !state.concrete.empty())) {
        fail("controller projection publishes interfaces outside its claimed "
             "directions");
      }
      auto expected_binding = expected_bindings.find(binding_id);
      if (expected_binding == expected_bindings.end()) {
        fail("controller projection lost its verified binding");
      }
      expected_binding->second.claimed_command_interfaces.insert(
          expected_binding->second.claimed_command_interfaces.end(),
          command.concrete.begin(), command.concrete.end());
      expected_binding->second.claimed_state_interfaces.insert(
          expected_binding->second.claimed_state_interfaces.end(),
          state.concrete.begin(), state.concrete.end());
      claimed_commands.insert(claimed_commands.end(), command.concrete.begin(),
                              command.concrete.end());
      claimed_states.insert(claimed_states.end(), state.concrete.begin(),
                            state.concrete.end());
      abstract_reference_exports.insert(abstract_reference_exports.end(),
                                        reference.abstract.begin(),
                                        reference.abstract.end());
      abstract_state_exports.insert(abstract_state_exports.end(),
                                    exported_state.abstract.begin(),
                                    exported_state.abstract.end());
      reference_exports.insert(reference_exports.end(),
                               reference.concrete.begin(),
                               reference.concrete.end());
      state_exports.insert(state_exports.end(), exported_state.concrete.begin(),
                           exported_state.concrete.end());
    }

    std::vector<core::ControllerControlInterfaceClaimDTO> claims;
    for (const auto &[identity, directions] : claim_directions) {
      claims.push_back({core::ControlInterfaceContractId(identity.first),
                        core::ControlInterfaceRevision(identity.second),
                        std::vector<core::ControllerClaimDirection>(
                            directions.begin(), directions.end())});
    }
    const auto abstract_less = [](const auto &lhs, const auto &rhs) {
      return std::tie(lhs.resource_role_id, lhs.interface_name) <
             std::tie(rhs.resource_role_id, rhs.interface_name);
    };
    sort_dedupe_equal(abstract_reference_exports, abstract_less,
                      "controller reference exports");
    sort_dedupe_equal(abstract_state_exports, abstract_less,
                      "controller state exports");
    sort_dedupe_equal(claimed_commands,
                      core::CanonicalControllerInterfaceInstanceLess,
                      "controller command interfaces");
    sort_dedupe_equal(claimed_states,
                      core::CanonicalControllerInterfaceInstanceLess,
                      "controller state interfaces");
    sort_dedupe_equal(reference_exports,
                      core::CanonicalControllerInterfaceInstanceLess,
                      "controller reference interfaces");
    sort_dedupe_equal(state_exports,
                      core::CanonicalControllerInterfaceInstanceLess,
                      "controller exported state interfaces");

    result.topology_controllers.push_back(
        {role, std::move(claims), std::move(abstract_reference_exports),
         std::move(abstract_state_exports)});
    result.expected_controllers.push_back(
        {role,
         core::ControllerImplementationId(
             text(member(implementation, "implementation_id",
                         "control.controllers.implementation"),
                  "control.controllers.implementation.implementation_id")),
         core::ControllerImplementationRevision(text(
             member(implementation, "implementation_revision",
                    "control.controllers.implementation"),
             "control.controllers.implementation.implementation_revision")),
         text(member(implementation, "implementation_type",
                     "control.controllers.implementation"),
              "control.controllers.implementation.implementation_type"),
         core::ContentDigest(
             text(member(row, "configuration_digest", "control.controllers"),
                  "control.controllers.configuration_digest")),
         std::move(claimed_commands), std::move(claimed_states),
         std::move(reference_exports), std::move(state_exports)});
  }

  sort_unique(
      result.topology_controllers,
      [](const auto &lhs, const auto &rhs) {
        return lhs.controller_role_id < rhs.controller_role_id;
      },
      "control.controllers");
  sort_unique(
      result.expected_controllers,
      [](const auto &lhs, const auto &rhs) {
        return lhs.controller_role_id < rhs.controller_role_id;
      },
      "control.controllers");
  for (auto &[unused, binding] : expected_bindings) {
    static_cast<void>(unused);
    sort_dedupe_equal(binding.claimed_command_interfaces,
                      core::CanonicalControllerInterfaceInstanceLess,
                      "control binding command interfaces");
    sort_dedupe_equal(binding.claimed_state_interfaces,
                      core::CanonicalControllerInterfaceInstanceLess,
                      "control binding state interfaces");
    if (!binding.locally_valid()) {
      fail("control binding resolves no valid concrete interface contract");
    }
    result.expected_bindings.push_back(std::move(binding));
  }
  if (result.expected_controllers.empty()) {
    fail("control.controllers is empty");
  }
  return result;
}

[[nodiscard]] std::vector<core::ControllerActivationSetDTO>
parse_activation_sets(const CanonicalValue &root) {
  std::vector<core::ControllerActivationSetDTO> result;
  for (const auto &row : array(member(root, "activation_sets", "control"),
                               "control.activation_sets")) {
    std::vector<core::ControllerChainEdgeRefDTO> edges;
    for (const auto &edge :
         array(member(row, "active_chain_edges", "control.activation_sets"),
               "control.activation_sets.active_chain_edges")) {
      edges.push_back(
          parse_edge_ref(edge, "control.activation_sets.active_chain_edges"));
    }
    sort_unique(
        edges,
        [](const auto &lhs, const auto &rhs) {
          return std::tie(lhs.upstream_controller_role_id,
                          lhs.downstream_controller_role_id) <
                 std::tie(rhs.upstream_controller_role_id,
                          rhs.downstream_controller_role_id);
        },
        "control.activation_sets.active_chain_edges");
    result.push_back(
        {core::ActivationSetId(
             text(member(row, "activation_set_id", "control.activation_sets"),
                  "control.activation_sets.activation_set_id")),
         parse_controller_roles(
             member(row, "active_controller_role_ids",
                    "control.activation_sets"),
             "control.activation_sets.active_controller_role_ids"),
         std::move(edges)});
  }
  sort_unique(
      result,
      [](const auto &lhs, const auto &rhs) {
        return lhs.activation_set_id < rhs.activation_set_id;
      },
      "control.activation_sets");
  return result;
}

struct RouteFacts final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;
  core::ControlRouteId control_route_id;
  std::vector<core::ControllerLifecycleExpectedEndpointDTO> endpoints;
};

struct RouteTables final {
  std::vector<core::ControllerChainEdgeDTO> topology_edges;
  std::vector<core::ControlRouteDTO> topology_routes;
  std::map<std::pair<std::string, std::string>, RouteFacts> routes_by_binding;
};

[[nodiscard]] RouteTables parse_routes(const CanonicalValue &root,
                                       const BindingMap &bindings) {
  RouteTables result;
  std::map<std::pair<std::string, std::string>, core::ControllerChainEdgeDTO>
      edges;
  std::map<std::string, core::ControlRouteDTO, std::less<>> topology_routes;

  for (const auto &row :
       array(member(root, "routes", "control"), "control.routes")) {
    const auto &binding_id =
        text(member(row, "control_binding_id", "control.routes"),
             "control.routes.control_binding_id");
    const auto binding = bindings.find(binding_id);
    if (binding == bindings.end()) {
      fail("control.routes names an unknown binding");
    }
    const auto &binding_revision =
        text(member(row, "control_binding_revision", "control.routes"),
             "control.routes.control_binding_revision");
    if (binding_revision != binding->second.control_binding_revision.value()) {
      fail("control.routes names an unknown binding revision");
    }
    const auto &route_id =
        text(member(row, "control_route_id", "control.routes"),
             "control.routes.control_route_id");

    core::ControlRouteDTO topology_route{
        core::ControlRouteId(route_id),
        binding->second.control_interface_contract_id,
        binding->second.control_interface_revision,
        parse_controller_roles(
            member(row, "controller_role_ids", "control.routes"),
            "control.routes.controller_role_ids"),
        parse_activation_ids(
            member(row, "compatible_activation_set_ids", "control.routes"),
            "control.routes.compatible_activation_set_ids"),
    };
    const auto existing_route = topology_routes.find(route_id);
    if (existing_route == topology_routes.end()) {
      topology_routes.emplace(route_id, topology_route);
    } else if (existing_route->second != topology_route) {
      fail("one route ID has conflicting retained topology semantics");
    }

    for (const auto &edge_value :
         array(member(row, "path_edges", "control.routes"),
               "control.routes.path_edges")) {
      const auto edge_ref =
          parse_edge_ref(edge_value, "control.routes.path_edges");
      const auto reference =
          parse_claims(member(edge_value, "reference_interfaces",
                              "control.routes.path_edges"),
                       "control.routes.path_edges.reference_interfaces");
      const auto state = parse_claims(
          member(edge_value, "state_interfaces", "control.routes.path_edges"),
          "control.routes.path_edges.state_interfaces");
      core::ControllerChainEdgeDTO edge{
          edge_ref.upstream_controller_role_id,
          edge_ref.downstream_controller_role_id,
          reference.abstract,
          state.abstract,
      };
      const auto key =
          std::make_pair(edge_ref.upstream_controller_role_id.value(),
                         edge_ref.downstream_controller_role_id.value());
      const auto existing = edges.find(key);
      if (existing == edges.end()) {
        edges.emplace(key, std::move(edge));
      } else if (existing->second != edge) {
        fail("one chain edge has conflicting retained interface semantics");
      }
    }

    std::vector<core::ControllerLifecycleExpectedEndpointDTO> endpoints;
    for (const auto &endpoint :
         array(member(row, "endpoints", "control.routes"),
               "control.routes.endpoints")) {
      endpoints.push_back(
          {binding->second.control_binding_id,
           binding->second.control_binding_revision,
           core::ControlRouteId(route_id),
           core::EndpointRoleId(text(
               member(endpoint, "endpoint_role_id", "control.routes.endpoints"),
               "control.routes.endpoints.endpoint_role_id")),
           text(member(endpoint, "logical_endpoint_symbol",
                       "control.routes.endpoints"),
                "control.routes.endpoints.logical_endpoint_symbol"),
           boolean(member(endpoint, "is_command_endpoint",
                          "control.routes.endpoints"),
                   "control.routes.endpoints.is_command_endpoint"),
           boolean(member(endpoint, "is_state_endpoint",
                          "control.routes.endpoints"),
                   "control.routes.endpoints.is_state_endpoint")});
    }
    sort_unique(endpoints, core::CanonicalControllerLifecycleEndpointLess,
                "control.routes.endpoints");
    if (endpoints.empty()) {
      fail("control.routes contains an endpointless route");
    }

    const auto key = std::make_pair(binding_id, route_id);
    if (!result.routes_by_binding
             .emplace(key, RouteFacts{binding->second.control_binding_id,
                                      binding->second.control_binding_revision,
                                      core::ControlRouteId(route_id),
                                      std::move(endpoints)})
             .second) {
      fail("control.routes duplicates a binding/route key");
    }
  }

  for (auto &[unused, edge] : edges) {
    static_cast<void>(unused);
    result.topology_edges.push_back(std::move(edge));
  }
  for (auto &[unused, route] : topology_routes) {
    static_cast<void>(unused);
    result.topology_routes.push_back(std::move(route));
  }
  if (result.routes_by_binding.empty()) {
    fail("control.routes is empty");
  }
  return result;
}

// The retained artifact proves the complete logical route first. Only this
// exact strict source case can remove physical-controller expectations.
[[nodiscard]] bool selected_pneumatic_none_domain(
    const RetainedBundle &bundle, const core::ControlDomainId &domain_id,
    const core::RuntimeUnitId &source_runtime_unit_id) {
  const auto &graph = bundle.value().cell_graph;
  const auto unit = std::find_if(graph.runtime_units.begin(), graph.runtime_units.end(),
                                [&](const auto &row) {
    return row.runtime_unit_id == source_runtime_unit_id;
  });
  if (unit == graph.runtime_units.end() ||
      unit->runtime_profile_id.value() != detail::kRos2ControlExecutionRuntimeProfileId) {
    return false;
  }
  const core::ControlBindingDTO *command = nullptr;
  for (const auto &binding : graph.control_bindings) {
    if (binding.control_domain_id != domain_id ||
        binding.capability_id.value() != "tool.gripper.command") continue;
    if (command != nullptr) fail("gripper domain repeats its command binding");
    command = &binding;
  }
  if (command == nullptr) return false;
  const auto config = ros2_control_execution_runtime_contract::require_configuration(
      bundle, source_runtime_unit_id);
  const ros2_control_execution_runtime_contract::Configuration::GripperTargetConfiguration *target = nullptr;
  for (const auto &row : config.gripper_target_configurations) {
    if (row.controllable_component_id == command->controllable_component_id &&
        row.command_target_id == command->command_target_id) {
      if (target != nullptr) fail("gripper domain repeats its exact configured target");
      target = &row;
    }
  }
  // An absent optional row keeps the established physical-required path.
  // Only an exact matched strict None row, or a lane-free tool's pneumatic
  // row (D4: no controller profile, its lane in the referenced I/O), may
  // remove those expectations.
  if (target == nullptr) return false;
  const bool lane_free =
      target->pneumatic_io.has_value() &&
      ros2_control_execution_runtime_contract::find_controller_profile(
          config, command->controllable_component_id) == nullptr;
  if (target->output_mode != core::GripperOutputMode::kNone && !lane_free) return false;
  if (target->actuation_kind != "pneumatic" ||
      command->provider_runtime_unit_id != source_runtime_unit_id) {
    fail("no-output domain does not select its strict pneumatic runtime owner");
  }
  for (const auto &binding : graph.control_bindings) {
    if (binding.control_domain_id != domain_id) continue;
    if (binding.controllable_component_id != command->controllable_component_id ||
        binding.command_target_id != command->command_target_id ||
        binding.provider_runtime_unit_id != command->provider_runtime_unit_id ||
        binding.provider_instance_id != command->provider_instance_id ||
        binding.provider_manifest_id != command->provider_manifest_id ||
        binding.provider_manifest_revision != command->provider_manifest_revision) {
      fail("no-output gripper domain contains a foreign physical owner");
    }
  }
  return true;
}

[[nodiscard]] const core::RetainedProductDTO &
require_control_product(const RetainedBundle &retained_bundle,
                        const core::ControlDomainId &control_domain_id) {
  const auto &groups =
      retained_bundle.value().compiled_products.control_domain_products;
  const core::RetainedControlDomainProductsDTO *selected_group = nullptr;
  for (const auto &group : groups) {
    if (group.control_domain_id != control_domain_id) {
      continue;
    }
    if (selected_group != nullptr) {
      fail("retained bundle duplicates the selected control-domain group");
    }
    selected_group = &group;
  }
  if (selected_group == nullptr) {
    fail("retained bundle omits the selected control-domain group");
  }

  const auto expected_product_id =
      detail::control_domain_control_product_id(control_domain_id);
  const core::RetainedProductDTO *selected_product = nullptr;
  for (const auto &product : selected_group->products) {
    if (product.product_kind !=
            core::RetainedProductKind::kControlDomainControl ||
        product.product_id.value() != expected_product_id) {
      continue;
    }
    if (selected_product != nullptr) {
      fail("retained bundle duplicates the selected control-domain product");
    }
    selected_product = &product;
  }
  if (selected_product == nullptr ||
      selected_product->product_contract_id.value() !=
          detail::control_domain_control_product_contract_id() ||
      selected_product->product_contract_revision !=
          detail::control_domain_control_product_contract_revision()) {
    fail("retained bundle omits the exact control-domain product contract");
  }
  return *selected_product;
}

[[nodiscard]] const core::RetainedArtifactRefDTO &
require_control_artifact(const core::RetainedProductDTO &product) {
  const core::RetainedArtifactRefDTO *selected = nullptr;
  for (const auto &artifact : product.artifact_refs) {
    if (artifact.artifact_role_id.value() !=
        detail::kControlDomainControlArtifactRole) {
      continue;
    }
    if (selected != nullptr) {
      fail("control-domain product duplicates its primary artifact role");
    }
    selected = &artifact;
  }
  if (selected == nullptr) {
    fail("control-domain product omits its primary artifact role");
  }
  return *selected;
}

[[nodiscard]] bool
desired_route_less(const core::DesiredControlBindingRouteDTO &lhs,
                   const core::DesiredControlBindingRouteDTO &rhs) {
  return std::tie(lhs.control_binding_id, lhs.control_binding_revision,
                  lhs.control_route_id) < std::tie(rhs.control_binding_id,
                                                   rhs.control_binding_revision,
                                                   rhs.control_route_id);
}

void require_desired_configuration(
    const core::DesiredControlConfigurationDTO &desired,
    const core::ControlDomainId &control_domain_id,
    const core::ControlDomainRevision &control_domain_revision,
    const core::ControllerTopologyId &topology_id,
    const core::TopologyRevision &topology_revision, const BindingMap &bindings,
    const std::vector<core::ControllerActivationSetDTO> &activation_sets,
    const RouteTables &routes) {
  if (desired.control_domain_id != control_domain_id ||
      desired.control_domain_revision != control_domain_revision ||
      desired.controller_topology_id != topology_id ||
      desired.topology_revision != topology_revision ||
      desired.activation_set_id.empty() || desired.binding_routes.empty() ||
      desired.binding_routes.size() != bindings.size() ||
      !std::is_sorted(desired.binding_routes.begin(),
                      desired.binding_routes.end(), desired_route_less)) {
    fail("desired configuration is not the canonical complete domain subject");
  }
  if (std::adjacent_find(
          desired.binding_routes.begin(), desired.binding_routes.end(),
          [](const auto &lhs, const auto &rhs) {
            return lhs.control_binding_id == rhs.control_binding_id;
          }) != desired.binding_routes.end()) {
    fail("desired configuration duplicates a binding");
  }
  if (std::none_of(activation_sets.begin(), activation_sets.end(),
                   [&](const auto &activation) {
                     return activation.activation_set_id ==
                            desired.activation_set_id;
                   })) {
    fail("desired configuration selects an unknown activation set");
  }

  for (const auto &desired_route : desired.binding_routes) {
    const auto binding =
        bindings.find(desired_route.control_binding_id.value());
    if (binding == bindings.end() ||
        binding->second.control_binding_revision !=
            desired_route.control_binding_revision) {
      fail("desired configuration names an unknown binding revision");
    }
    const auto route =
        routes.routes_by_binding.find({desired_route.control_binding_id.value(),
                                       desired_route.control_route_id.value()});
    if (route == routes.routes_by_binding.end() ||
        route->second.control_binding_revision !=
            desired_route.control_binding_revision) {
      fail("desired configuration names an unknown binding route");
    }
    const auto topology_route = std::find_if(
        routes.topology_routes.begin(), routes.topology_routes.end(),
        [&](const auto &candidate) {
          return candidate.control_route_id == desired_route.control_route_id;
        });
    if (topology_route == routes.topology_routes.end() ||
        std::find(topology_route->compatible_activation_set_ids.begin(),
                  topology_route->compatible_activation_set_ids.end(),
                  desired.activation_set_id) ==
            topology_route->compatible_activation_set_ids.end()) {
      fail("desired configuration route is incompatible with its activation "
           "set");
    }
  }
}

} // namespace

VerifiedControllerLifecycleExpectedDomain
load_verified_controller_lifecycle_expected_domain(
    const RetainedBundle &retained_bundle,
    const IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::DesiredControlConfigurationDTO &desired_configuration,
    core::RuntimeUnitId source_runtime_unit_id,
    core::RuntimeEpoch source_runtime_epoch) {
  if (!verify_retained_bundle_identity(retained_bundle)) {
    fail("retained bundle identity is invalid");
  }
  if (!bounded_identity(source_runtime_unit_id) ||
      !bounded_identity(source_runtime_epoch)) {
    fail("source runtime identity is empty or exceeds its bound");
  }

  const auto domain =
      std::find_if(retained_bundle.value().cell_graph.control_domains.begin(),
                   retained_bundle.value().cell_graph.control_domains.end(),
                   [&](const auto &candidate) {
                     return candidate.control_domain_id ==
                            desired_configuration.control_domain_id;
                   });
  if (domain == retained_bundle.value().cell_graph.control_domains.end() ||
      std::find(domain->runtime_unit_ids.begin(),
                domain->runtime_unit_ids.end(),
                source_runtime_unit_id) == domain->runtime_unit_ids.end()) {
    fail("source runtime unit is not retained by the selected control domain");
  }

  const auto &product = require_control_product(
      retained_bundle, desired_configuration.control_domain_id);
  const auto &expected_artifact = require_control_artifact(product);
  const RetainedProductArtifactQuery query{
      retained_bundle.retained_bundle_id(), product.product_id,
      product.product_kind, expected_artifact.artifact_role_id};
  const auto loaded = artifact_source.get_verified(query);
  if (!loaded.has_value() || loaded->query() != query ||
      loaded->product_contract_id() != product.product_contract_id ||
      loaded->product_contract_revision() !=
          product.product_contract_revision ||
      loaded->artifact_ref() != expected_artifact) {
    fail("exact retained control artifact is absent or mismatched");
  }

  const auto model_map = detail::RetainedModelMapVerificationAuthority::verify(
      retained_bundle, artifact_source);
  if (!model_map.has_value())
    fail("exact retained binding model map is absent");
  const auto &coordinate_maps = model_map->control_bindings();
  detail::require_control_domain_control_matches_retained_bundle(
      loaded->bytes(), retained_bundle.value(),
      desired_configuration.control_domain_id, {}, {}, {}, coordinate_maps);
  const CanonicalValue root = detail::parse_and_verify_control_domain_control(
      loaded->bytes(), &retained_bundle.value().cell_graph, coordinate_maps);

  const core::ControlDomainId control_domain_id(
      text(member(root, "control_domain_id", "control"),
           "control.control_domain_id"));
  const core::ControlDomainRevision control_domain_revision(
      text(member(root, "control_domain_revision", "control"),
           "control.control_domain_revision"));
  const core::ControllerTopologyId topology_id(
      text(member(root, "controller_topology_id", "control"),
           "control.controller_topology_id"));
  const core::TopologyRevision topology_revision(
      text(member(root, "topology_revision", "control"),
           "control.topology_revision"));
  const auto bindings = parse_bindings(root);
  auto controllers = parse_controllers(root, bindings);
  auto activation_sets = parse_activation_sets(root);
  auto routes = parse_routes(root, bindings);

  require_desired_configuration(
      desired_configuration, control_domain_id, control_domain_revision,
      topology_id, topology_revision, bindings, activation_sets, routes);

  std::vector<core::ControllerLifecycleExpectedEndpointDTO> endpoints;
  for (const auto &[unused, route] : routes.routes_by_binding) {
    static_cast<void>(unused);
    endpoints.insert(endpoints.end(), route.endpoints.begin(),
                     route.endpoints.end());
  }
  sort_unique(endpoints, core::CanonicalControllerLifecycleEndpointLess,
              "complete legal controller endpoints");
  if (endpoints.empty()) {
    fail("control domain resolves no legal controller endpoint");
  }

  core::ControllerTopologyDTO topology{
      topology_id,
      topology_revision,
      std::move(controllers.topology_controllers),
      std::move(routes.topology_edges),
      std::move(activation_sets),
      std::move(routes.topology_routes),
  };
  const auto &conformance = member(root, "source_conformance", "control");
  core::ControllerLifecycleExpectedDomainDTO value{
      control_domain_id,
      control_domain_revision,
      topology_id,
      topology_revision,
      product.product_id,
      product.product_contract_id,
      product.product_contract_revision,
      expected_artifact.content_digest,
      core::ContentDigest(
          text(member(conformance, "source_conformance_digest",
                      "control.source_conformance"),
               "control.source_conformance.source_conformance_digest")),
      std::move(source_runtime_unit_id),
      std::move(source_runtime_epoch),
      std::move(topology),
      desired_configuration,
      std::move(controllers.expected_bindings),
      std::move(controllers.expected_controllers),
      std::move(endpoints),
  };

  if (value.expected_bindings.empty() || value.expected_controllers.empty() ||
      value.expected_endpoints.empty() ||
      value.expected_controllers.size() !=
          value.controller_topology.controllers.size()) {
    fail("decoded expected-domain closure is incomplete");
  }
  for (auto &expected_binding : value.expected_bindings) {
    const auto &graph = retained_bundle.value().cell_graph;
    const auto source =
        std::find_if(graph.control_bindings.begin(),
                     graph.control_bindings.end(), [&](const auto &candidate) {
                       return candidate.control_binding_id ==
                                  expected_binding.control_binding_id &&
                              candidate.control_binding_revision ==
                                  expected_binding.control_binding_revision;
                     });
    if (source == graph.control_bindings.end() ||
        source->control_domain_id != control_domain_id)
      fail("Stop actuation source lost its exact expected binding");
    const auto actuation = detail::classify_ros_gripper_actuation_kind(
        graph, *source, coordinate_maps);
    // DX817: preserve all old arm/position/default serialized identities.
    if (actuation == core::GripperActuationKind::kPneumatic)
      expected_binding.gripper_actuation_kind = actuation;
  }
  if (selected_pneumatic_none_domain(retained_bundle, control_domain_id,
                                      value.source_runtime_unit_id)) {
    // DX791: preserve verified logical bindings/routes/endpoints and source
    // identities, but never turn their abstract topology into a fake CM.
    value.physical_expectation =
        core::ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone;
    value.controller_topology.controllers.clear();
    value.controller_topology.chain_edges.clear();
    for (auto &activation : value.controller_topology.activation_sets) {
      activation.active_controller_role_ids.clear();
      activation.active_chain_edges.clear();
    }
    for (auto &route : value.controller_topology.legal_routes)
      route.ordered_controller_role_ids.clear();
    for (auto &binding : value.expected_bindings) {
      std::set<core::ResourceId> verified_resources;
      for (const auto &claim : binding.claimed_command_interfaces)
        verified_resources.insert(claim.resource_id);
      for (const auto &claim : binding.claimed_state_interfaces)
        verified_resources.insert(claim.resource_id);
      const auto &graph_bindings = retained_bundle.value().cell_graph.control_bindings;
      const auto retained = std::find_if(graph_bindings.begin(), graph_bindings.end(),
                                        [&](const auto &candidate) {
        return candidate.control_binding_id == binding.control_binding_id &&
               candidate.control_binding_revision == binding.control_binding_revision &&
               candidate.control_interface_contract_id == binding.control_interface_contract_id &&
               candidate.control_interface_revision == binding.control_interface_revision;
      });
      if (retained == graph_bindings.end() || retained->control_domain_id != control_domain_id)
        fail("no-output logical resources lost their retained binding");
      std::set<core::ResourceId> retained_resources;
      for (const auto &resource : retained->resource_bindings)
        retained_resources.insert(resource.resource_id);
      if (verified_resources.empty() || verified_resources != retained_resources)
        fail("no-output logical resources differ from the complete retained binding");
      binding.logical_owner_resources.assign(verified_resources.begin(), verified_resources.end());
      binding.claimed_command_interfaces.clear();
      binding.claimed_state_interfaces.clear();
    }
    value.expected_controllers.clear();
  }
  auto sealed =
      core::verify_controller_lifecycle_expected_domain(std::move(value));
  if (!sealed.ok()) {
    fail("decoded expected-domain closure failed Core verification");
  }
  return VerifiedControllerLifecycleExpectedDomain(std::move(sealed.value()));
}

} // namespace manyforge::deployment
