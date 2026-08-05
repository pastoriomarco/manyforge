#include "manyforge_deployment/cell_applicability.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "cell_applicability_budget_internal.hpp"
#include "manyforge_deployment/site_overlay_resolution.hpp"
#include "mf1_name_internal.hpp"

namespace manyforge::deployment {
namespace {

using PairKey = std::pair<std::string, std::string>;

[[noreturn]] void fail(std::string message) {
  throw CellApplicabilityError(std::move(message));
}

template <typename Id, typename Revision>
[[nodiscard]] PairKey pair_key(const Id& id, const Revision& revision) {
  return {id.value(), revision.value()};
}

void checked_add(std::size_t& total, std::size_t added,
                 std::size_t maximum, std::string_view family) {
  if (added > maximum || total > maximum - added) {
    fail(std::string(family) + " exceeds its aggregate bound");
  }
  total += added;
}

[[nodiscard]] std::size_t checked_product(std::size_t left,
                                          std::size_t right,
                                          std::size_t maximum,
                                          std::string_view family) {
  if (left != 0U && right > maximum / left) {
    fail(std::string(family) + " overflows or exceeds its bound");
  }
  return left * right;
}

template <typename Qualified, typename KeyFunction>
[[nodiscard]] auto index_exact_span(std::span<const Qualified> values,
                                    KeyFunction key_function,
                                    std::string_view family) {
  using Key = std::invoke_result_t<KeyFunction, const Qualified&>;
  std::map<Key, const Qualified*> result;
  for (const auto& value : values) {
    const Key key = key_function(value);
    if (!result.emplace(key, &value).second) {
      fail(std::string(family) +
           " contains a duplicate exact qualified referent");
    }
  }
  return result;
}

template <typename Map, typename Set>
void require_exact_closure(const Map& supplied, const Set& required,
                           std::string_view family) {
  for (const auto& key : required) {
    if (!supplied.contains(key)) {
      fail(std::string(family) +
           " is missing a selected exact qualified referent");
    }
  }
  for (const auto& [key, unused] : supplied) {
    (void)unused;
    if (!required.contains(key)) {
      fail(std::string(family) +
           " contains an unused exact qualified referent");
    }
  }
}

template <typename Qualified, typename Map, typename Set>
[[nodiscard]] std::vector<Qualified> copy_closure_in_key_order(
    const Map& supplied, const Set& required) {
  std::vector<Qualified> result;
  result.reserve(required.size());
  for (const auto& key : required) {
    result.push_back(*supplied.at(key));
  }
  return result;
}

template <typename Range, typename Predicate>
[[nodiscard]] const typename Range::value_type* find_one(
    const Range& values, Predicate predicate) {
  const auto iterator =
      std::find_if(values.begin(), values.end(), std::move(predicate));
  return iterator == values.end() ? nullptr : &*iterator;
}

[[nodiscard]] bool contains_role(
    const std::vector<core::EndpointRoleId>& values,
    const core::EndpointRoleId& role) {
  return std::binary_search(values.begin(), values.end(), role);
}

[[nodiscard]] bool contains_activation_set(
    const std::vector<core::ActivationSetId>& values,
    const core::ActivationSetId& activation_set_id) {
  return std::binary_search(values.begin(), values.end(),
                            activation_set_id);
}

[[nodiscard]] bool contains_controller_role(
    const std::vector<core::ControllerRoleId>& values,
    const core::ControllerRoleId& role) {
  return std::binary_search(values.begin(), values.end(), role);
}

[[nodiscard]] bool contains_authority_class(
    const std::vector<core::StateAuthorityClass>& values,
    core::StateAuthorityClass authority_class) {
  return std::find(values.begin(), values.end(), authority_class) !=
         values.end();
}

[[nodiscard]] std::uint8_t granularity_rank(
    core::ContainmentGranularity granularity) {
  switch (granularity) {
    case core::ContainmentGranularity::kResource:
      return 0U;
    case core::ContainmentGranularity::kControllerSet:
      return 1U;
    case core::ContainmentGranularity::kControlDomain:
      return 2U;
  }
  fail("unknown containment granularity");
}

class LogicalNameRegistry final {
 public:
  explicit LogicalNameRegistry(const CompositionQualifiedCellIr& composition) {
    seed(composition.cell_frame().compiled_frame_name);
    seed(composition.flattened_urdf().robot_name);
    for (const auto& occurrence : composition.model_occurrences()) {
      seed(occurrence.compiled_model_name);
      seed(occurrence.compiled_robot_name);
      seed(occurrence.compiled_root_link_name);
    }
    for (const auto& rewrite : composition.symbol_rewrites()) {
      seed(rewrite.compiled_name);
    }
    for (const auto& frame : composition.frames()) {
      std::visit([this](const auto& value) { seed(value.compiled_frame_name); },
                 frame.frame);
    }
    for (const auto& connection : composition.fixed_connections()) {
      seed(connection.compiled_joint_name);
      seed(connection.compiled_parent_link_name);
      seed(connection.compiled_child_link_name);
    }
    for (const auto& coordinate : composition.coordinates()) {
      seed(coordinate.compiled_joint_name);
      if (coordinate.mimic_source_compiled_joint_name.has_value()) {
        seed(*coordinate.mimic_source_compiled_joint_name);
      }
    }
    for (const auto& target : composition.targets()) {
      for (const auto& group : target.model_groups) {
        seed(group.compiled_group_name);
        for (const auto& link : group.expanded_compiled_link_names) {
          seed(link);
        }
        for (const auto& joint : group.expanded_compiled_joint_names) {
          seed(joint);
        }
      }
      for (const auto& frame : target.frames) {
        std::visit(
            [this](const auto& value) { seed(value.compiled_frame_name); },
            frame.frame);
      }
      for (const auto& collision : target.collision_bodies) {
        for (const auto& link : collision.compiled_link_names) {
          seed(link);
        }
      }
    }
  }

  [[nodiscard]] std::string make(
      std::string_view tag,
      const std::vector<std::pair<std::string_view, std::string_view>>&
          fields) {
    const std::string name = detail::make_mf1_generated_name(tag, fields);
    std::string semantic_key(tag);
    for (const auto& [field, value] : fields) {
      semantic_key.push_back('\0');
      semantic_key.append(field);
      semantic_key.push_back('=');
      semantic_key.append(std::to_string(value.size()));
      semantic_key.push_back(':');
      semantic_key.append(value);
    }
    const auto [existing, inserted] =
        generated_names_.try_emplace(name, semantic_key);
    if (!inserted && existing->second != semantic_key) {
      fail("generated mf1 logical-name collision for '" + name + "'");
    }
    if (names_.contains(name) && inserted) {
      fail("generated mf1 logical name collides with a structural name for '" +
           name + "'");
    }
    names_.insert(name);
    return name;
  }

 private:
  void seed(const std::string& name) {
    if (!name.empty()) {
      names_.insert(name);
    }
  }

  std::set<std::string> names_;
  std::map<std::string, std::string> generated_names_;
};

struct QualifiedIndexes final {
  std::map<PairKey, const QualifiedRuntimeProfile*> runtime_profiles;
  std::map<PairKey, const QualifiedCapabilityDefinition*>
      capability_definitions;
  std::map<PairKey, const QualifiedControlInterfaceContract*> contracts;
  std::map<PairKey, const QualifiedControllerTopology*> topologies;
  std::map<PairKey, const QualifiedProviderManifest*> manifests;
  std::map<PairKey, const QualifiedStopEvidencePolicy*> stop_policies;
  std::set<PairKey> required_runtime_profiles;
  std::set<PairKey> required_capability_definitions;
  std::set<PairKey> required_contracts;
  std::set<PairKey> required_topologies;
  std::set<PairKey> required_manifests;
  std::set<PairKey> required_stop_policies;
};

[[nodiscard]] QualifiedIndexes build_qualified_indexes(
    const CompositionQualifiedCellIr& composition,
    const RevisionVerifiedDeploymentApplicabilityInputs& deployment_inputs,
    std::span<const QualifiedRuntimeProfile> runtime_profiles,
    std::span<const QualifiedCapabilityDefinition> capability_definitions,
    std::span<const QualifiedControlInterfaceContract> contracts,
    std::span<const QualifiedControllerTopology> topologies,
    std::span<const QualifiedProviderManifest> manifests,
    std::span<const QualifiedStopEvidencePolicy> stop_policies) {
  QualifiedIndexes result{
      index_exact_span(
          runtime_profiles,
          [](const auto& value) {
            return pair_key(value.value().runtime_profile_id,
                            value.value().runtime_profile_revision);
          },
          "runtime-profile closure"),
      index_exact_span(
          capability_definitions,
          [](const auto& value) {
            return pair_key(value.value().capability_id,
                            value.value().capability_definition_revision);
          },
          "capability-definition closure"),
      index_exact_span(
          contracts,
          [](const auto& value) {
            return pair_key(
                value.value().control_interface_contract_id,
                value.value().control_interface_revision);
          },
          "control-interface-contract closure"),
      index_exact_span(
          topologies,
          [](const auto& value) {
            return pair_key(value.value().controller_topology_id,
                            value.value().topology_revision);
          },
          "controller-topology closure"),
      index_exact_span(
          manifests,
          [](const auto& value) {
            return pair_key(value.value().provider_manifest_id,
                            value.value().provider_manifest_revision);
          },
          "provider-manifest closure"),
      index_exact_span(
          stop_policies,
          [](const auto& value) {
            return pair_key(value.value().stop_evidence_policy_id,
                            value.value().stop_evidence_policy_revision);
          },
          "Stop-policy closure"),
      {},
      {},
      {},
      {},
      {},
      {},
  };

  const auto& graph = composition.graph();
  for (const auto& runtime : graph.runtime_units) {
    result.required_runtime_profiles.insert(
        pair_key(runtime.runtime_profile_id,
                 runtime.runtime_profile_revision));
  }
  for (const auto& domain : graph.control_domains) {
    result.required_topologies.insert(
        pair_key(domain.controller_topology_id, domain.topology_revision));
  }
  for (const auto& binding : graph.control_bindings) {
    result.required_capability_definitions.insert(
        pair_key(binding.capability_id,
                 binding.capability_definition_revision));
    result.required_contracts.insert(
        pair_key(binding.control_interface_contract_id,
                 binding.control_interface_revision));
    result.required_topologies.insert(
        pair_key(binding.controller_topology_id, binding.topology_revision));
    result.required_manifests.insert(
        pair_key(binding.provider_manifest_id,
                 binding.provider_manifest_revision));
    result.required_stop_policies.insert(
        pair_key(binding.stop_evidence_policy_id,
                 binding.stop_evidence_policy_revision));
  }
  for (const auto& component_type : composition.component_types()) {
    for (const auto& target : component_type.value().command_targets) {
      for (const auto& capability : target.capabilities) {
        result.required_capability_definitions.insert(
            pair_key(capability.capability_id,
                     capability.capability_definition_revision));
      }
    }
  }
  for (const auto& binding :
       deployment_inputs.value().capability_provider_bindings) {
    result.required_capability_definitions.insert(
        pair_key(binding.capability_id,
                 binding.capability_definition_revision));
    result.required_manifests.insert(
        pair_key(binding.provider_manifest_id,
                 binding.provider_manifest_revision));
  }
  for (const auto& resource : deployment_inputs.value().external_resources) {
    result.required_manifests.insert(
        pair_key(resource.provider_manifest_id,
                 resource.provider_manifest_revision));
  }

  require_exact_closure(result.runtime_profiles,
                        result.required_runtime_profiles,
                        "runtime-profile closure");
  require_exact_closure(result.topologies, result.required_topologies,
                        "controller-topology closure");
  require_exact_closure(result.manifests, result.required_manifests,
                        "provider-manifest closure");

  // Complete selected topology declarations are retained for I3. Their exact
  // contract selectors therefore belong to the required closure even when a
  // deployment binding chooses only a subset of the topology's legal routes.
  for (const auto& topology_key : result.required_topologies) {
    const auto& topology = result.topologies.at(topology_key)->value();
    for (const auto& controller : topology.controllers) {
      for (const auto& claim : controller.control_interface_claims) {
        result.required_contracts.insert(
            pair_key(claim.control_interface_contract_id,
                     claim.control_interface_revision));
      }
    }
    for (const auto& route : topology.legal_routes) {
      result.required_contracts.insert(
          pair_key(route.control_interface_contract_id,
                   route.control_interface_revision));
    }
  }
  for (const auto& manifest_key : result.required_manifests) {
    const auto& manifest = result.manifests.at(manifest_key)->value();
    for (const auto& offer : manifest.offered_capabilities) {
      result.required_capability_definitions.insert(
          pair_key(offer.capability_id,
                   offer.capability_definition_revision));
    }
  }

  require_exact_closure(result.capability_definitions,
                        result.required_capability_definitions,
                        "capability-definition closure");
  require_exact_closure(result.contracts, result.required_contracts,
                        "control-interface-contract closure");
  require_exact_closure(result.stop_policies,
                        result.required_stop_policies,
                        "Stop-policy closure");
  return result;
}

[[nodiscard]] const core::RuntimeUnitDTO& require_runtime_unit(
    const core::CellGraphDTO& graph, const core::RuntimeUnitId& id) {
  const auto* result = find_one(
      graph.runtime_units,
      [&id](const auto& value) { return value.runtime_unit_id == id; });
  if (result == nullptr) {
    fail("capability-provider endpoint references an unknown runtime unit");
  }
  return *result;
}

[[nodiscard]] const core::ControlDomainDTO& require_domain(
    const core::CellGraphDTO& graph, const core::ControlDomainId& id) {
  const auto* result = find_one(
      graph.control_domains,
      [&id](const auto& value) { return value.control_domain_id == id; });
  if (result == nullptr) {
    fail("applicability references an unknown control domain");
  }
  return *result;
}

[[nodiscard]] const core::ControlBindingDTO& require_binding(
    const core::CellGraphDTO& graph, const core::ControlBindingId& id) {
  const auto* result = find_one(
      graph.control_bindings,
      [&id](const auto& value) { return value.control_binding_id == id; });
  if (result == nullptr) {
    fail("applicability references an unknown control binding");
  }
  return *result;
}

[[nodiscard]] const core::ControllableComponentDTO& require_component(
    const core::CellGraphDTO& graph,
    const core::ControllableComponentId& id) {
  const auto* result = find_one(
      graph.controllable_components, [&id](const auto& value) {
        return value.controllable_component_id == id;
      });
  if (result == nullptr) {
    fail("control applicability references an unknown component");
  }
  return *result;
}

[[nodiscard]] const core::ComponentTypeCommandTargetDTO&
require_type_target(
    const CompositionQualifiedCellIr& composition,
    const core::ControllableComponentDTO& component,
    const core::CommandTargetId& id) {
  const auto* type = find_one(
      composition.component_types(), [&component](const auto& value) {
        return value.value().component_type_id == component.component_type_id &&
               value.value().component_type_revision ==
                   component.component_type_revision;
      });
  if (type == nullptr) {
    fail("structural component lost its exact component-type referent");
  }
  const auto* target = find_one(
      type->value().command_targets,
      [&id](const auto& value) { return value.command_target_id == id; });
  if (target == nullptr) {
    fail("structural component lost its exact command-target declaration");
  }
  return *target;
}

[[nodiscard]] const CompositionControlBindingCoordinateMap&
require_coordinate_map(const CompositionQualifiedCellIr& composition,
                       const core::ControlBindingDTO& binding) {
  const auto* result =
      find_one(composition.control_binding_coordinate_maps(),
               [&binding](const auto& value) {
                 return value.control_binding_id ==
                            binding.control_binding_id &&
                        value.controllable_component_id ==
                            binding.controllable_component_id &&
                        value.command_target_id ==
                            binding.command_target_id;
               });
  if (result == nullptr) {
    fail("control binding has no exact C0.2m coordinate map");
  }
  return *result;
}

[[nodiscard]] const core::ProviderCapabilityOfferDTO&
require_manifest_offer(const core::ProviderManifestDTO& manifest,
                       const core::CapabilityId& capability_id,
                       const core::CapabilityDefinitionRevision& revision) {
  const auto* offer = find_one(
      manifest.offered_capabilities,
      [&capability_id](const auto& value) {
        return value.capability_id == capability_id;
      });
  if (offer == nullptr ||
      offer->capability_definition_revision != revision) {
    fail("provider manifest does not offer the exact selected capability");
  }
  return *offer;
}

[[nodiscard]] const core::ProviderContractDeclarationDTO&
require_provider_contract(
    const core::ProviderManifestDTO& manifest,
    const core::ProviderContractRefDTO& reference) {
  const auto* declaration = find_one(
      manifest.provider_contracts, [&reference](const auto& value) {
        return value.provider_contract_ref.provider_contract_role_id ==
               reference.provider_contract_role_id;
      });
  if (declaration == nullptr ||
      declaration->provider_contract_ref != reference) {
    fail("Stop policy selects a missing or stale provider contract");
  }
  return *declaration;
}

[[nodiscard]] const core::ControllerTopologyControllerDTO&
require_controller(const core::ControllerTopologyDTO& topology,
                   const core::ControllerRoleId& role) {
  const auto* controller =
      find_one(topology.controllers, [&role](const auto& value) {
        return value.controller_role_id == role;
      });
  if (controller == nullptr) {
    fail("route or activation set references an unknown controller role");
  }
  return *controller;
}

[[nodiscard]] const core::ControllerActivationSetDTO&
require_activation_set(const core::ControllerTopologyDTO& topology,
                       const core::ActivationSetId& id) {
  const auto* activation =
      find_one(topology.activation_sets, [&id](const auto& value) {
        return value.activation_set_id == id;
      });
  if (activation == nullptr) {
    fail("desired or route applicability references an unknown activation "
         "set");
  }
  return *activation;
}

[[nodiscard]] const core::ControlRouteDTO& require_route(
    const core::ControllerTopologyDTO& topology,
    const core::ControlRouteId& id) {
  const auto* route =
      find_one(topology.legal_routes, [&id](const auto& value) {
        return value.control_route_id == id;
      });
  if (route == nullptr) {
    fail("control binding references an unknown topology route");
  }
  return *route;
}

[[nodiscard]] const core::ControllerChainEdgeDTO& require_edge(
    const core::ControllerTopologyDTO& topology,
    const core::ControllerRoleId& upstream,
    const core::ControllerRoleId& downstream) {
  const auto* edge =
      find_one(topology.chain_edges, [&upstream, &downstream](const auto& value) {
        return value.upstream_controller_role_id == upstream &&
               value.downstream_controller_role_id == downstream;
      });
  if (edge == nullptr) {
    fail("control route lost an exact topology chain edge");
  }
  return *edge;
}

[[nodiscard]] const core::RouteEndpointProjectionDTO&
require_route_projection(const core::ControlBindingDTO& binding,
                         const core::ControlRouteId& route_id) {
  const auto* projection =
      find_one(binding.route_endpoint_projections,
               [&route_id](const auto& value) {
                 return value.control_route_id == route_id;
               });
  if (projection == nullptr) {
    fail("control binding is missing a route endpoint projection");
  }
  return *projection;
}

[[nodiscard]] std::vector<core::ResourceRoleId>
target_resource_role_order(
    const core::ComponentTypeCommandTargetDTO& target) {
  std::vector<core::ResourceRoleId> result;
  result.reserve(target.resource_roles.size());
  for (const auto& resource : target.resource_roles) {
    result.push_back(std::visit(
        [](const auto& value) { return value.resource_role_id; }, resource));
  }
  return result;
}

using BoundResourceMap =
    std::map<core::ResourceRoleId, std::vector<core::ResourceId>>;

[[nodiscard]] BoundResourceMap validate_and_index_bound_resources(
    const core::ControlBindingDTO& binding,
    const core::ComponentTypeCommandTargetDTO& target,
    const CompositionControlBindingCoordinateMap& coordinate_map,
    const core::ControlInterfaceContractDTO& contract) {
  const auto target_roles = target_resource_role_order(target);
  if (contract.ordering != target_roles) {
    fail("control-interface contract ordering does not equal the target's "
         "exact resource-role order");
  }

  BoundResourceMap result;
  std::vector<core::ResourceRoleId> observed_groups;
  std::set<core::ResourceId> resource_ids;
  std::set<core::ResourceRoleId> closed_groups;
  std::optional<core::ResourceRoleId> previous_role;
  for (const auto& resource : binding.resource_bindings) {
    if (!resource_ids.insert(resource.resource_id).second) {
      fail("control binding repeats a concrete ResourceId");
    }
    if (previous_role.has_value() &&
        *previous_role != resource.resource_role_id) {
      closed_groups.insert(*previous_role);
      if (closed_groups.contains(resource.resource_role_id)) {
        fail("control binding reopens a noncontiguous resource-role group");
      }
    }
    auto [iterator, inserted] =
        result.try_emplace(resource.resource_role_id);
    if (inserted) {
      observed_groups.push_back(resource.resource_role_id);
    }
    iterator->second.push_back(resource.resource_id);
    previous_role = resource.resource_role_id;
  }
  if (observed_groups != target_roles || result.size() != target_roles.size()) {
    fail("control binding has missing, extra, duplicated, noncontiguous, or "
         "misordered resource-role coverage");
  }
  for (const auto& role : target_roles) {
    if (result.at(role).empty()) {
      fail("control binding has an empty concrete resource-role group");
    }
  }

  if (coordinate_map.resources.size() != binding.resource_bindings.size()) {
    fail("C0.2m coordinate-map coverage differs from the control binding");
  }
  for (std::size_t index = 0; index < binding.resource_bindings.size();
       ++index) {
    const auto& authored = binding.resource_bindings[index];
    const auto& composed = coordinate_map.resources[index];
    if (authored.resource_role_id != composed.resource_role_id ||
        authored.resource_id != composed.resource_id) {
      fail("C0.2m coordinate-map resource order differs from the control "
           "binding");
    }
  }
  return result;
}

[[nodiscard]] std::vector<const core::ResourceInterfaceClaimDTO*>
claims_for_role(const std::vector<core::ResourceInterfaceClaimDTO>& claims,
                const core::ResourceRoleId& role) {
  std::vector<const core::ResourceInterfaceClaimDTO*> result;
  for (const auto& claim : claims) {
    if (claim.resource_role_id == role) {
      result.push_back(&claim);
    }
  }
  return result;
}

[[nodiscard]] std::size_t preflight_expansion(
    const std::vector<core::ResourceInterfaceClaimDTO>& claims,
    const std::vector<core::ResourceRoleId>& ordering,
    const BoundResourceMap& resources, std::size_t maximum,
    std::string_view direction) {
  std::size_t total = 0U;
  for (const auto& role : ordering) {
    const auto interfaces = claims_for_role(claims, role);
    if (interfaces.empty()) {
      continue;
    }
    const auto found = resources.find(role);
    if (found == resources.end()) {
      fail("control claim references an unbound target resource role");
    }
    const std::size_t rows = checked_product(
        found->second.size(), interfaces.size(),
        maximum,
        std::string(direction) + " control-interface expansion");
    checked_add(total, rows,
                maximum,
                std::string(direction) + " control-interface expansion");
  }
  return total;
}

[[nodiscard]] std::vector<ExpandedControlInterfaceClaim> expand_claims(
    const core::CellId& cell_id, const core::ControlBindingDTO& binding,
    const std::vector<core::ResourceInterfaceClaimDTO>& claims,
    const std::vector<core::ResourceRoleId>& ordering,
    const BoundResourceMap& resources, std::string_view direction,
    std::size_t maximum, LogicalNameRegistry& names) {
  const std::size_t total =
      preflight_expansion(claims, ordering, resources, maximum, direction);
  std::vector<ExpandedControlInterfaceClaim> result;
  result.reserve(total);
  for (const auto& role : ordering) {
    const auto interfaces = claims_for_role(claims, role);
    if (interfaces.empty()) {
      continue;
    }
    const auto& bound_resources = resources.at(role);
    for (const auto& resource_id : bound_resources) {
      for (const auto* interface : interfaces) {
        result.push_back({
            role,
            resource_id,
            interface->interface_name,
            names.make(
                "interface",
                {{"cell_id", cell_id.value()},
                 {"kind", "interface"},
                 {"control_binding_id", binding.control_binding_id.value()},
                 {"control_binding_revision",
                  binding.control_binding_revision.value()},
                 {"direction", direction},
                 {"resource_role_id", role.value()},
                 {"resource_id", resource_id.value()},
                 {"interface_name", interface->interface_name}}),
        });
      }
    }
  }
  return result;
}

[[nodiscard]] std::vector<ExpandedControlInterfaceClaim>
project_topology_interfaces(
    const core::CellId& cell_id, const core::ControlBindingDTO& binding,
    const std::vector<core::ResourceInterfaceClaimDTO>& claims,
    const std::vector<core::ResourceRoleId>& ordering,
    const BoundResourceMap& resources, std::string_view direction,
    LogicalNameRegistry& names) {
  const auto total = preflight_expansion(
      claims, ordering, resources, kMaxCellLogicalApplicabilityRows,
      direction);
  std::vector<ExpandedControlInterfaceClaim> result;
  result.reserve(total);
  for (const auto& role : ordering) {
    const auto interfaces = claims_for_role(claims, role);
    if (interfaces.empty()) {
      continue;
    }
    for (const auto& resource_id : resources.at(role)) {
      for (const auto* interface : interfaces) {
        result.push_back({
            role,
            resource_id,
            interface->interface_name,
            names.make(
                "interface",
                {{"cell_id", cell_id.value()},
                 {"kind", "interface"},
                 {"control_binding_id", binding.control_binding_id.value()},
                 {"control_binding_revision",
                  binding.control_binding_revision.value()},
                 {"direction", direction},
                 {"resource_role_id", role.value()},
                 {"resource_id", resource_id.value()},
                 {"interface_name", interface->interface_name}}),
        });
      }
    }
  }
  return result;
}

[[nodiscard]] std::set<core::EndpointRoleId>
provider_manifest_endpoint_inventory(
    const core::ProviderManifestDTO& manifest) {
  std::set<core::EndpointRoleId> result;
  for (const auto& offer : manifest.offered_capabilities) {
    result.insert(offer.required_endpoint_role_ids.begin(),
                  offer.required_endpoint_role_ids.end());
  }
  for (const auto& contract : manifest.provider_contracts) {
    if (contract.stop_command_support.has_value()) {
      const auto& roles =
          contract.stop_command_support->command_endpoint_role_ids;
      result.insert(roles.begin(), roles.end());
    }
    if (contract.stop_evidence_support.has_value()) {
      const auto& roles =
          contract.stop_evidence_support->evidence_endpoint_role_ids;
      result.insert(roles.begin(), roles.end());
    }
  }
  return result;
}

void qualify_provider_instances(
    const core::CellGraphDTO& graph,
    const core::DeploymentApplicabilityInputsDTO& inputs) {
  using Selection =
      std::pair<core::ProviderManifestId, core::ProviderManifestRevision>;
  std::map<core::ProviderInstanceId, Selection> selections;
  const auto register_selection =
      [&selections](const core::ProviderInstanceId& instance,
                    const core::ProviderManifestId& manifest,
                    const core::ProviderManifestRevision& revision) {
        const Selection selection{manifest, revision};
        const auto [iterator, inserted] =
            selections.emplace(instance, selection);
        if (!inserted && iterator->second != selection) {
          fail("one ProviderInstanceId selects more than one exact provider "
               "manifest pair");
        }
      };

  for (const auto& binding : graph.control_bindings) {
    register_selection(binding.provider_instance_id,
                       binding.provider_manifest_id,
                       binding.provider_manifest_revision);
  }
  for (const auto& binding : inputs.capability_provider_bindings) {
    register_selection(binding.provider_instance_id,
                       binding.provider_manifest_id,
                       binding.provider_manifest_revision);
  }
  for (const auto& resource : inputs.external_resources) {
    register_selection(resource.provider_instance_id,
                       resource.provider_manifest_id,
                       resource.provider_manifest_revision);
  }

}

[[nodiscard]] std::optional<core::SiteOverlayRefDTO>
qualify_selected_overlay(
    const core::DeploymentApplicabilityInputsDTO& inputs,
    const QualifiedSelectedSiteOverlay& selected_site_overlay) {
  if (!inputs.site_overlay_ref.has_value()) {
    if (selected_site_overlay.has_selection()) {
      fail("a selected site-overlay proof was supplied when the deployment "
           "selected no overlay");
    }
    if (!inputs.external_resources.empty()) {
      fail("external resources require a selected site overlay");
    }
    return std::nullopt;
  }
  if (!selected_site_overlay.has_selection() ||
      selected_site_overlay.reference() == nullptr) {
    fail("deployment site_overlay_ref has no qualified selected overlay");
  }
  if (*selected_site_overlay.reference() != *inputs.site_overlay_ref) {
    fail("qualified selected overlay does not match the deployment's exact "
         "id/revision/digest triple");
  }

  const auto keys = selected_site_overlay.external_resource_keys();
  for (const auto& resource : inputs.external_resources) {
    const auto iterator = std::find_if(
        keys.begin(), keys.end(), [&resource](const auto& key) {
          return key.site_overlay_value_ref ==
                     resource.site_overlay_value_ref &&
                 key.resource_kind == resource.resource_kind;
        });
    if (iterator == keys.end()) {
      fail("external resource does not resolve through the exact selected "
           "site-overlay key/kind index");
    }
  }
  return inputs.site_overlay_ref;
}

struct BindingContext final {
  const core::ControlBindingDTO* binding;
  const core::ControlDomainDTO* domain;
  const CompositionControlBindingCoordinateMap* coordinate_map;
  const QualifiedCapabilityDefinition* capability;
  const QualifiedControlInterfaceContract* contract;
  const QualifiedControllerTopology* topology;
  const QualifiedProviderManifest* manifest;
  const core::ProviderCapabilityOfferDTO* offer;
  const QualifiedStopEvidencePolicy* stop_policy;
  BoundResourceMap resources;
  std::optional<LogicalBindingInterfaceTemplate> interface_template;
};

void require_target_capability(
    const core::ComponentTypeCommandTargetDTO& target,
    const core::CapabilityId& capability_id,
    const core::CapabilityDefinitionRevision& revision) {
  const auto* selected =
      find_one(target.capabilities, [&capability_id](const auto& value) {
        return value.capability_id == capability_id;
      });
  if (selected == nullptr ||
      selected->capability_definition_revision != revision) {
    fail("binding target does not declare the exact selected capability");
  }
}

[[nodiscard]] std::vector<BindingContext> qualify_binding_contexts(
    const CompositionQualifiedCellIr& composition,
    const QualifiedIndexes& indexes) {
  const auto& graph = composition.graph();
  std::vector<BindingContext> result;
  result.reserve(graph.control_bindings.size());

  for (const auto& binding : graph.control_bindings) {
    const auto& domain = require_domain(graph, binding.control_domain_id);
    if (binding.controller_topology_id !=
            domain.controller_topology_id ||
        binding.topology_revision != domain.topology_revision) {
      fail("control binding topology pair differs from its control domain");
    }

    const auto& component =
        require_component(graph, binding.controllable_component_id);
    const auto& type_target =
        require_type_target(composition, component,
                            binding.command_target_id);
    require_target_capability(type_target, binding.capability_id,
                              binding.capability_definition_revision);
    const auto& coordinate_map =
        require_coordinate_map(composition, binding);

    const auto& capability =
        *indexes.capability_definitions.at(
            pair_key(binding.capability_id,
                     binding.capability_definition_revision));
    if (capability.value().scope !=
            core::CapabilityScope::kTargetBinding ||
        capability.value().provider_binding_kind !=
            core::CapabilityProviderBindingKind::kControlBinding) {
      fail("ControlBinding selects a capability with incompatible scope or "
           "provider-binding kind");
    }

    const auto& contract =
        *indexes.contracts.at(
            pair_key(binding.control_interface_contract_id,
                     binding.control_interface_revision));
    if (contract.value().command_interfaces.empty()) {
      fail("v1 ControlBinding selects a command-empty control-interface "
           "contract");
    }
    const auto& topology =
        *indexes.topologies.at(
            pair_key(binding.controller_topology_id,
                     binding.topology_revision));
    const auto& manifest =
        *indexes.manifests.at(
            pair_key(binding.provider_manifest_id,
                     binding.provider_manifest_revision));
    const auto& offer =
        require_manifest_offer(manifest.value(), binding.capability_id,
                               binding.capability_definition_revision);
    const auto& stop_policy =
        *indexes.stop_policies.at(
            pair_key(binding.stop_evidence_policy_id,
                     binding.stop_evidence_policy_revision));

    if (binding.containment_granularity !=
        stop_policy.value().containment_granularity) {
      fail("Stop policy containment granularity differs from its control "
           "binding");
    }
    if (granularity_rank(binding.containment_granularity) <
        granularity_rank(domain.containment_granularity)) {
      fail("control binding claims containment finer than its domain can "
           "provide");
    }

    BoundResourceMap resources = validate_and_index_bound_resources(
        binding, type_target, coordinate_map, contract.value());
    result.push_back(
        {&binding,
         &domain,
         &coordinate_map,
         &capability,
         &contract,
         &topology,
         &manifest,
         &offer,
         &stop_policy,
         std::move(resources),
         std::nullopt});
  }
  return result;
}

void materialize_binding_interfaces(
    const core::CellId& cell_id, std::vector<BindingContext>& bindings,
    LogicalNameRegistry& names, std::size_t& logical_rows) {
  for (auto& context : bindings) {
    const auto& binding = *context.binding;
    const auto& contract = context.contract->value();
    auto command_claims = expand_claims(
        cell_id, binding, contract.command_interfaces, contract.ordering,
        context.resources, "command",
        kMaxExpandedControlInterfaceClaimsPerDirection, names);
    auto state_claims = expand_claims(
        cell_id, binding, contract.state_interfaces, contract.ordering,
        context.resources, "state",
        kMaxExpandedControlInterfaceClaimsPerDirection, names);
    context.interface_template.emplace(
        LogicalBindingInterfaceTemplate{
            binding.control_binding_id,
            binding.control_binding_revision,
            binding.control_interface_contract_id,
            binding.control_interface_revision,
            std::move(command_claims),
            std::move(state_claims)});
    checked_add(logical_rows, 1U, kMaxCellLogicalApplicabilityRows,
                "cell logical applicability rows");
  }
}

[[nodiscard]] std::size_t matching_claimed_direction_count(
    const core::ControllerTopologyControllerDTO& controller,
    const core::ControlBindingDTO& binding) {
  std::size_t result = 0U;
  for (const auto& claim : controller.control_interface_claims) {
    if (claim.control_interface_contract_id ==
            binding.control_interface_contract_id &&
        claim.control_interface_revision ==
            binding.control_interface_revision) {
      checked_add(result, claim.claimed_directions.size(),
                  kMaxCellLogicalApplicabilityRows,
                  "controller claimed-direction rows");
    }
  }
  return result;
}

void preflight_stop_rows(
    const BindingContext& context,
    const core::ControlRouteDTO& route,
    detail::ApplicabilityOutputBudget& budget) {
  const auto& policy = context.stop_policy->value();
  budget.charge_logical_rows(1U);
  if (policy.disposition ==
      core::StopEvidenceDisposition::kNotStoppable) {
    return;
  }
  if (!policy.containment_command.has_value() ||
      !policy.deadline_budget.has_value() ||
      !policy.evidence_conjunction.has_value()) {
    fail("qualified stoppable policy lost command, deadline budget, or "
         "evidence conjunction");
  }

  budget.charge_logical_rows(policy.deadline_budget->phases.size());
  budget.charge_logical_rows(policy.evidence_requirements.size());
  if (policy.containment_command->provider_contract_ref.has_value()) {
    const auto& contract = require_provider_contract(
        context.manifest->value(),
        *policy.containment_command->provider_contract_ref);
    if (!contract.stop_command_support.has_value()) {
      fail("selected provider contract does not support the Stop command");
    }
    if (!contract.stop_command_support->command_endpoint_role_ids.empty()) {
      budget.charge_logical_rows(1U);
    }
  }

  for (const auto& requirement : policy.evidence_requirements) {
    budget.charge_logical_rows(
        requirement.source_requirement.allowed_authority_classes.size());
    if (requirement.source_requirement.source_kind ==
            core::StopEvidenceSourceKind::kProviderEvidence &&
        requirement.source_requirement.endpoint_role_id.has_value()) {
      budget.charge_logical_rows(1U);
    }
    if (requirement.source_requirement.source_kind ==
        core::StopEvidenceSourceKind::kBindingStateAuthority) {
      budget.charge_logical_rows(1U);
    }
  }

  budget.charge_logical_rows(route.compatible_activation_set_ids.size());
  for (const auto& activation_set_id :
       route.compatible_activation_set_ids) {
    switch (context.binding->containment_granularity) {
      case core::ContainmentGranularity::kResource:
        budget.charge_logical_rows(
            context.binding->resource_bindings.size());
        break;
      case core::ContainmentGranularity::kControllerSet: {
        const auto& activation = require_activation_set(
            context.topology->value(), activation_set_id);
        budget.charge_logical_rows(
            activation.active_controller_role_ids.size());
        budget.charge_logical_rows(
            activation.active_controller_role_ids.size());
        break;
      }
      case core::ContainmentGranularity::kControlDomain:
        break;
    }
  }
}

[[nodiscard]] detail::ApplicabilityOutputCardinality
preflight_applicability_output(
    const std::vector<BindingContext>& bindings,
    const core::DeploymentApplicabilityInputsDTO& inputs) {
  detail::ApplicabilityOutputBudget budget;
  using ControllerKey =
      std::pair<core::ControlDomainId, core::ControllerRoleId>;
  std::set<ControllerKey> controllers;

  for (const auto& context : bindings) {
    const auto& binding = *context.binding;
    const auto& contract = context.contract->value();
    const auto command_rows = preflight_expansion(
        contract.command_interfaces, contract.ordering, context.resources,
        kMaxExpandedControlInterfaceClaimsPerDirection, "command");
    const auto state_rows = preflight_expansion(
        contract.state_interfaces, contract.ordering, context.resources,
        kMaxExpandedControlInterfaceClaimsPerDirection, "state");
    budget.charge_binding_directions(command_rows, state_rows);
    budget.charge_logical_rows(1U);

    const auto& topology = context.topology->value();
    for (const auto& route_id : binding.legal_control_route_ids) {
      const auto& route = require_route(topology, route_id);
      const auto& endpoint_projection =
          require_route_projection(binding, route_id);
      std::set<core::EndpointRoleId> endpoint_roles;
      endpoint_roles.insert(
          endpoint_projection.command_endpoint_role_ids.begin(),
          endpoint_projection.command_endpoint_role_ids.end());
      endpoint_roles.insert(
          endpoint_projection.state_endpoint_role_ids.begin(),
          endpoint_projection.state_endpoint_role_ids.end());

      budget.charge_logical_rows(1U);
      budget.charge_logical_rows(
          route.ordered_controller_role_ids.size());
      budget.charge_logical_rows(
          route.ordered_controller_role_ids.size());
      budget.charge_logical_rows(
          route.compatible_activation_set_ids.size());
      if (route.ordered_controller_role_ids.size() > 1U) {
        budget.charge_logical_rows(
            route.ordered_controller_role_ids.size() - 1U);
      }
      budget.charge_logical_rows(endpoint_roles.size());

      for (const auto& activation_set_id :
           route.compatible_activation_set_ids) {
        const auto& activation =
            require_activation_set(topology, activation_set_id);
        for (const auto& role :
             activation.active_controller_role_ids) {
          const auto& controller = require_controller(topology, role);
          if (controllers
                  .insert({context.domain->control_domain_id, role})
                  .second) {
            budget.charge_logical_rows(1U);
          }
          budget.charge_logical_rows(1U);
          budget.charge_logical_rows(
              matching_claimed_direction_count(controller, binding));
          budget.charge_logical_rows(
              preflight_expansion(
                  controller.exported_reference_interfaces,
                  contract.ordering, context.resources,
                  kMaxCellLogicalApplicabilityRows, "command"));
          budget.charge_logical_rows(
              preflight_expansion(
                  controller.exported_state_interfaces,
                  contract.ordering, context.resources,
                  kMaxCellLogicalApplicabilityRows, "state"));
        }
      }
      preflight_stop_rows(context, route, budget);
    }
  }

  budget.charge_logical_rows(bindings.size());
  budget.charge_logical_rows(inputs.capability_provider_bindings.size());
  budget.charge_logical_rows(inputs.external_resources.size());
  for (const auto& binding : inputs.capability_provider_bindings) {
    budget.charge_logical_rows(binding.endpoint_bindings.size());
  }
  return budget.cardinality();
}

void qualify_desired_configurations(
    const core::CellGraphDTO& graph,
    const core::DeploymentApplicabilityInputsDTO& inputs,
    const QualifiedIndexes& indexes) {
  std::map<core::ControlDomainId,
           const core::DesiredControlConfigurationDTO*>
      desired_by_domain;
  for (const auto& desired : inputs.desired_control_configurations) {
    if (!desired_by_domain
             .emplace(desired.control_domain_id, &desired)
             .second) {
      fail("deployment contains two desired configurations under one stable "
           "control-domain ID");
    }
  }
  if (desired_by_domain.size() != graph.control_domains.size()) {
    fail("desired-control-configuration coverage is not exact");
  }

  for (const auto& domain : graph.control_domains) {
    const auto found = desired_by_domain.find(domain.control_domain_id);
    if (found == desired_by_domain.end()) {
      fail("control domain has no desired configuration");
    }
    const auto& desired = *found->second;
    if (desired.control_domain_revision !=
            domain.control_domain_revision ||
        desired.controller_topology_id !=
            domain.controller_topology_id ||
        desired.topology_revision != domain.topology_revision) {
      fail("desired configuration selects a stale domain or topology "
           "revision");
    }
    const auto& topology =
        indexes.topologies
            .at(pair_key(domain.controller_topology_id,
                         domain.topology_revision))
            ->value();
    (void)require_activation_set(topology, desired.activation_set_id);

    std::map<core::ControlBindingId,
             const core::DesiredControlBindingRouteDTO*>
        desired_routes;
    for (const auto& route : desired.binding_routes) {
      if (!desired_routes.emplace(route.control_binding_id, &route).second) {
        fail("desired configuration selects more than one route for one "
             "stable control-binding ID");
      }
    }
    std::size_t domain_binding_count = 0U;
    for (const auto& binding : graph.control_bindings) {
      if (binding.control_domain_id != domain.control_domain_id) {
        continue;
      }
      ++domain_binding_count;
      const auto route_found =
          desired_routes.find(binding.control_binding_id);
      if (route_found == desired_routes.end()) {
        fail("desired configuration omits a binding in its control domain");
      }
      const auto& desired_route = *route_found->second;
      if (desired_route.control_binding_revision !=
          binding.control_binding_revision) {
        fail("desired configuration selects a stale control-binding "
             "revision");
      }
      if (!std::binary_search(binding.legal_control_route_ids.begin(),
                              binding.legal_control_route_ids.end(),
                              desired_route.control_route_id)) {
        fail("desired configuration selects a route outside the binding's "
             "legal set");
      }
      const auto& route =
          require_route(topology, desired_route.control_route_id);
      if (!contains_activation_set(route.compatible_activation_set_ids,
                                   desired.activation_set_id)) {
        fail("desired binding routes do not share the selected activation "
             "set");
      }
    }
    if (desired_routes.size() != domain_binding_count) {
      fail("desired configuration contains a route for another or unknown "
           "control binding");
    }
  }
}

struct ControllerAndRouteResult final {
  std::vector<LogicalControllerTemplate> controllers;
  std::vector<LogicalControlRouteTemplate> routes;
};

[[nodiscard]] std::vector<core::ControllerChainEdgeRefDTO>
route_path_edges(const core::ControllerTopologyDTO& topology,
                 const core::ControlRouteDTO& route,
                 const BoundResourceMap& resources) {
  std::vector<core::ControllerChainEdgeRefDTO> result;
  if (route.ordered_controller_role_ids.size() < 2U) {
    return result;
  }
  result.reserve(route.ordered_controller_role_ids.size() - 1U);
  for (std::size_t index = 1U;
       index < route.ordered_controller_role_ids.size(); ++index) {
    const auto& upstream =
        route.ordered_controller_role_ids[index - 1U];
    const auto& downstream =
        route.ordered_controller_role_ids[index];
    const auto& edge = require_edge(topology, upstream, downstream);
    bool has_binding_reference = false;
    for (const auto& claim : edge.reference_interfaces) {
      has_binding_reference |= resources.contains(claim.resource_role_id);
    }
    if (!has_binding_reference) {
      fail("route chain edge has no reference interface applicable to the "
           "binding's target resources");
    }
    result.push_back({upstream, downstream});
  }
  return result;
}

[[nodiscard]] LogicalControllerBindingProjection
make_controller_projection(
    const core::CellId& cell_id, const BindingContext& context,
    const core::ControlRouteId& route_id,
    const core::ActivationSetId& activation_set_id,
    const core::ControllerTopologyControllerDTO& controller,
    LogicalNameRegistry& names, std::size_t& logical_rows) {
  LogicalControllerBindingProjection result{
      context.binding->control_binding_id,
      context.binding->control_binding_revision,
      route_id,
      activation_set_id,
      {},
      {},
      {},
  };
  for (const auto& claim : controller.control_interface_claims) {
    if (claim.control_interface_contract_id !=
            context.binding->control_interface_contract_id ||
        claim.control_interface_revision !=
            context.binding->control_interface_revision) {
      continue;
    }
    for (const auto direction : claim.claimed_directions) {
      result.claimed_directions.push_back(direction);
    }
  }
  result.exported_reference_interfaces = project_topology_interfaces(
      cell_id, *context.binding,
      controller.exported_reference_interfaces,
      context.contract->value().ordering, context.resources,
      "command", names);
  result.exported_state_interfaces = project_topology_interfaces(
      cell_id, *context.binding,
      controller.exported_state_interfaces,
      context.contract->value().ordering, context.resources,
      "state", names);

  checked_add(logical_rows, 1U, kMaxCellLogicalApplicabilityRows,
              "cell logical applicability rows");
  checked_add(logical_rows, result.claimed_directions.size(),
              kMaxCellLogicalApplicabilityRows,
              "cell logical applicability rows");
  checked_add(logical_rows, result.exported_reference_interfaces.size(),
              kMaxCellLogicalApplicabilityRows,
              "cell logical applicability rows");
  checked_add(logical_rows, result.exported_state_interfaces.size(),
              kMaxCellLogicalApplicabilityRows,
              "cell logical applicability rows");
  return result;
}

[[nodiscard]] ControllerAndRouteResult qualify_controllers_and_routes(
    const core::CellId& cell_id,
    const std::vector<BindingContext>& bindings,
    LogicalNameRegistry& names, std::size_t& logical_rows) {
  using ControllerKey =
      std::pair<core::ControlDomainId, core::ControllerRoleId>;
  std::map<ControllerKey, LogicalControllerTemplate> controller_map;
  std::vector<LogicalControlRouteTemplate> routes;

  for (const auto& context : bindings) {
    const auto& binding = *context.binding;
    const auto& topology = context.topology->value();
    if (binding.legal_control_route_ids.size() !=
        binding.route_endpoint_projections.size()) {
      fail("control binding does not have exactly one endpoint projection "
           "per legal route");
    }

    std::set<core::ControlRouteId> projected_route_ids;
    for (const auto& projection :
         binding.route_endpoint_projections) {
      if (!projected_route_ids.insert(projection.control_route_id).second) {
        fail("control binding repeats a route endpoint projection");
      }
    }
    if (!std::equal(binding.legal_control_route_ids.begin(),
                    binding.legal_control_route_ids.end(),
                    projected_route_ids.begin(),
                    projected_route_ids.end())) {
      fail("control binding route and endpoint-projection closures differ");
    }

    for (const auto& route_id : binding.legal_control_route_ids) {
      const auto& route = require_route(topology, route_id);
      if (route.control_interface_contract_id !=
              binding.control_interface_contract_id ||
          route.control_interface_revision !=
              binding.control_interface_revision) {
        fail("binding route selects a different control-interface contract");
      }
      const auto& projection =
          require_route_projection(binding, route_id);
      if (projection.command_endpoint_role_ids.empty()) {
        fail("v1 ControlBinding route has no command-side endpoint");
      }
      std::set<core::EndpointRoleId> endpoint_union;
      endpoint_union.insert(projection.command_endpoint_role_ids.begin(),
                            projection.command_endpoint_role_ids.end());
      endpoint_union.insert(projection.state_endpoint_role_ids.begin(),
                            projection.state_endpoint_role_ids.end());
      const std::vector<core::EndpointRoleId> endpoint_union_vector(
          endpoint_union.begin(), endpoint_union.end());
      if (endpoint_union_vector !=
          context.offer->required_endpoint_role_ids) {
        fail("route endpoint projection does not equal the selected provider "
             "offer's complete required-role set");
      }

      for (const auto& activation_set_id :
           route.compatible_activation_set_ids) {
        const auto& activation =
            require_activation_set(topology, activation_set_id);
        for (const auto& role :
             route.ordered_controller_role_ids) {
          if (!contains_controller_role(
                  activation.active_controller_role_ids, role)) {
            fail("route-compatible activation set omits a path controller");
          }
        }
        for (const auto& role :
             activation.active_controller_role_ids) {
          const auto& controller = require_controller(topology, role);
          const ControllerKey key{context.domain->control_domain_id, role};
          auto [iterator, inserted] = controller_map.try_emplace(
              key,
              LogicalControllerTemplate{
                  context.domain->control_domain_id,
                  context.domain->control_domain_revision,
                  topology.controller_topology_id,
                  topology.topology_revision,
                  role,
                  {},
                  {},
              });
          auto& logical_controller = iterator->second;
          if (inserted) {
            logical_controller.logical_controller_symbol = names.make(
                "controller",
                {{"cell_id", cell_id.value()},
                 {"kind", "controller"},
                 {"control_domain_id",
                  context.domain->control_domain_id.value()},
                 {"control_domain_revision",
                  context.domain->control_domain_revision.value()},
                 {"controller_topology_id",
                  topology.controller_topology_id.value()},
                 {"topology_revision",
                  topology.topology_revision.value()},
                 {"controller_role_id", role.value()}});
            checked_add(logical_rows, 1U,
                        kMaxCellLogicalApplicabilityRows,
                        "cell logical applicability rows");
          } else if (
              logical_controller.control_domain_revision !=
                  context.domain->control_domain_revision ||
              logical_controller.controller_topology_id !=
                  topology.controller_topology_id ||
              logical_controller.topology_revision !=
                  topology.topology_revision) {
            fail("one control domain/controller role maps to conflicting "
                 "exact topology identity");
          }
          logical_controller.binding_projections.push_back(
              make_controller_projection(
                  cell_id, context, route_id, activation_set_id, controller,
                  names, logical_rows));
        }
      }

      LogicalControlRouteTemplate logical_route{
          binding.control_binding_id,
          binding.control_binding_revision,
          context.domain->control_domain_id,
          context.domain->control_domain_revision,
          topology.controller_topology_id,
          topology.topology_revision,
          route_id,
          names.make(
              "route",
              {{"cell_id", cell_id.value()},
               {"kind", "route"},
               {"control_binding_id",
                binding.control_binding_id.value()},
               {"control_binding_revision",
                binding.control_binding_revision.value()},
               {"control_route_id", route_id.value()}}),
          route.ordered_controller_role_ids,
          {},
          route_path_edges(topology, route, context.resources),
          route.compatible_activation_set_ids,
          {},
      };
      logical_route.logical_controller_symbols.reserve(
          route.ordered_controller_role_ids.size());
      for (const auto& role :
           route.ordered_controller_role_ids) {
        logical_route.logical_controller_symbols.push_back(
            controller_map
                .at({context.domain->control_domain_id, role})
                .logical_controller_symbol);
      }

      for (const auto& endpoint_role : endpoint_union) {
        logical_route.endpoint_templates.push_back(
            {endpoint_role,
             names.make(
                 "route_endpoint",
                 {{"cell_id", cell_id.value()},
                  {"kind", "route_endpoint"},
                  {"control_binding_id",
                   binding.control_binding_id.value()},
                  {"control_binding_revision",
                   binding.control_binding_revision.value()},
                  {"control_route_id", route_id.value()},
                  {"endpoint_role_id", endpoint_role.value()}}),
             contains_role(projection.command_endpoint_role_ids,
                           endpoint_role),
             contains_role(projection.state_endpoint_role_ids,
                           endpoint_role)});
      }
      checked_add(logical_rows, 1U, kMaxCellLogicalApplicabilityRows,
                  "cell logical applicability rows");
      checked_add(logical_rows, logical_route.endpoint_templates.size(),
                  kMaxCellLogicalApplicabilityRows,
                  "cell logical applicability rows");
      checked_add(logical_rows, logical_route.path_edges.size(),
                  kMaxCellLogicalApplicabilityRows,
                  "cell logical applicability rows");
      checked_add(logical_rows,
                  logical_route.controller_role_ids.size(),
                  kMaxCellLogicalApplicabilityRows,
                  "cell logical applicability rows");
      checked_add(logical_rows,
                  logical_route.logical_controller_symbols.size(),
                  kMaxCellLogicalApplicabilityRows,
                  "cell logical applicability rows");
      checked_add(logical_rows,
                  logical_route.compatible_activation_set_ids.size(),
                  kMaxCellLogicalApplicabilityRows,
                  "cell logical applicability rows");
      routes.push_back(std::move(logical_route));
    }
  }

  ControllerAndRouteResult result;
  result.controllers.reserve(controller_map.size());
  for (auto& [key, controller] : controller_map) {
    (void)key;
    std::sort(
        controller.binding_projections.begin(),
        controller.binding_projections.end(),
        [](const auto& left, const auto& right) {
          return std::tie(left.control_binding_id,
                          left.control_binding_revision,
                          left.control_route_id,
                          left.activation_set_id) <
                 std::tie(right.control_binding_id,
                          right.control_binding_revision,
                          right.control_route_id,
                          right.activation_set_id);
        });
    result.controllers.push_back(std::move(controller));
  }
  result.routes = std::move(routes);
  return result;
}

struct CapabilityAndEndpointResult final {
  std::vector<LogicalCapabilityTemplate> capabilities;
  std::vector<LogicalProviderEndpointTemplate> endpoints;
  std::vector<LogicalExternalResourceTemplate> external_resources;
};

[[nodiscard]] const core::ExternalResourceDTO& require_external_resource(
    const core::DeploymentApplicabilityInputsDTO& inputs,
    const core::ExternalResourceAlias& alias) {
  const auto* result =
      find_one(inputs.external_resources, [&alias](const auto& value) {
        return value.external_resource_alias == alias;
      });
  if (result == nullptr) {
    fail("capability-provider endpoint references an unknown external "
         "resource alias");
  }
  return *result;
}

void qualify_capability_provider_target(
    const CompositionQualifiedCellIr& composition,
    const core::CapabilityProviderBindingDTO& binding,
    const core::CapabilityDefinitionDTO& capability) {
  const auto& graph = composition.graph();
  if (binding.scope != capability.scope ||
      capability.provider_binding_kind !=
          core::CapabilityProviderBindingKind::
              kCapabilityProviderBinding) {
    fail("CapabilityProviderBinding selects a capability with incompatible "
         "scope or provider-binding kind");
  }

  switch (binding.scope) {
    case core::CapabilityScope::kCellGlobal: {
      const auto* target =
          std::get_if<core::CellGlobalCapabilityProviderTargetDTO>(
              &binding.target);
      if (target == nullptr || target->cell_id != graph.cell_id) {
        fail("cell-global capability-provider target does not match the "
             "compiled cell");
      }
      break;
    }
    case core::CapabilityScope::kControlDomain: {
      const auto* target =
          std::get_if<core::ControlDomainCapabilityProviderTargetDTO>(
              &binding.target);
      if (target == nullptr) {
        fail("control-domain capability has the wrong target alternative");
      }
      const auto& domain =
          require_domain(graph, target->control_domain_id);
      if (domain.control_domain_revision !=
              target->control_domain_revision ||
          domain.controller_topology_id !=
              target->controller_topology_id ||
          domain.topology_revision != target->topology_revision) {
        fail("control-domain capability-provider target is stale or selects "
             "the wrong topology");
      }
      break;
    }
    case core::CapabilityScope::kTargetBinding: {
      const auto* target =
          std::get_if<core::TargetBindingCapabilityProviderTargetDTO>(
              &binding.target);
      if (target == nullptr) {
        fail("target-binding capability has the wrong target alternative");
      }
      const auto& component =
          require_component(graph, target->controllable_component_id);
      if (component.component_type_revision !=
          target->component_type_revision) {
        fail("target-binding capability-provider target selects a stale "
             "component-type revision");
      }
      const auto& type_target =
          require_type_target(composition, component,
                              target->command_target_id);
      require_target_capability(
          type_target, binding.capability_id,
          binding.capability_definition_revision);
      const auto& anchor =
          require_binding(graph, target->control_binding_id);
      if (anchor.control_binding_revision !=
              target->control_binding_revision ||
          anchor.controllable_component_id !=
              target->controllable_component_id ||
          anchor.command_target_id != target->command_target_id) {
        fail("target-binding capability-provider target has a stale or "
             "cross-target control-binding anchor");
      }
      break;
    }
  }
}

using SemanticCapabilityCoverageKey = std::vector<std::string>;

[[nodiscard]] SemanticCapabilityCoverageKey capability_coverage_key(
    const core::CapabilityProviderBindingDTO& binding) {
  SemanticCapabilityCoverageKey result{
      binding.capability_id.value(),
      binding.capability_definition_revision.value(),
      std::string(core::to_string(binding.scope)),
  };
  std::visit(
      [&result](const auto& target) {
        using Target = std::decay_t<decltype(target)>;
        if constexpr (std::is_same_v<
                          Target,
                          core::CellGlobalCapabilityProviderTargetDTO>) {
          result.emplace_back("cell_global");
          result.push_back(target.cell_id.value());
        } else if constexpr (std::is_same_v<
                                 Target,
                                 core::
                                     TargetBindingCapabilityProviderTargetDTO>) {
          result.emplace_back("target_binding");
          result.push_back(target.controllable_component_id.value());
          result.push_back(target.component_type_revision.value());
          result.push_back(target.command_target_id.value());
          result.push_back(target.control_binding_id.value());
          result.push_back(target.control_binding_revision.value());
        } else {
          result.emplace_back("control_domain");
          result.push_back(target.control_domain_id.value());
          result.push_back(target.control_domain_revision.value());
          result.push_back(target.controller_topology_id.value());
          result.push_back(target.topology_revision.value());
        }
      },
      binding.target);
  return result;
}

void qualify_complete_capability_coverage(
    const CompositionQualifiedCellIr& composition,
    const core::DeploymentApplicabilityInputsDTO& inputs,
    const QualifiedIndexes& indexes) {
  const auto& graph = composition.graph();

  std::set<SemanticCapabilityCoverageKey> semantic_coverage;
  for (const auto& binding : inputs.capability_provider_bindings) {
    if (!semantic_coverage.insert(capability_coverage_key(binding)).second) {
      fail("two capability-provider bindings duplicate one exact semantic "
           "capability target");
    }
  }

  for (const auto& component : graph.controllable_components) {
    const auto* type = find_one(
        composition.component_types(), [&component](const auto& value) {
          return value.value().component_type_id ==
                     component.component_type_id &&
                 value.value().component_type_revision ==
                     component.component_type_revision;
        });
    if (type == nullptr) {
      fail("composed component lost its exact component-type referent");
    }
    for (const auto& target : type->value().command_targets) {
      for (const auto& declared : target.capabilities) {
        const auto& definition =
            indexes.capability_definitions
                .at(pair_key(
                    declared.capability_id,
                    declared.capability_definition_revision))
                ->value();
        if (definition.scope != core::CapabilityScope::kTargetBinding) {
          fail("a component command target declares a capability whose scope "
               "is not target_binding");
        }

        std::vector<const core::ControlBindingDTO*> target_bindings;
        for (const auto& binding : graph.control_bindings) {
          if (binding.controllable_component_id ==
                  component.controllable_component_id &&
              binding.command_target_id == target.command_target_id) {
            target_bindings.push_back(&binding);
          }
        }

        if (definition.provider_binding_kind ==
            core::CapabilityProviderBindingKind::kControlBinding) {
          const auto count =
              std::count_if(
                  target_bindings.begin(), target_bindings.end(),
                  [&declared](const auto* binding) {
                    return binding->capability_id ==
                               declared.capability_id &&
                           binding->capability_definition_revision ==
                               declared.capability_definition_revision;
                  });
          if (count == 0) {
            fail("a control-binding target capability has no exact graph "
                 "binding");
          }
          continue;
        }

        for (const auto* anchor : target_bindings) {
          const auto count = std::count_if(
              inputs.capability_provider_bindings.begin(),
              inputs.capability_provider_bindings.end(),
              [&](const auto& candidate) {
                if (candidate.capability_id !=
                        declared.capability_id ||
                    candidate.capability_definition_revision !=
                        declared.capability_definition_revision ||
                    candidate.scope !=
                        core::CapabilityScope::kTargetBinding) {
                  return false;
                }
                const auto* target_binding =
                    std::get_if<
                        core::TargetBindingCapabilityProviderTargetDTO>(
                        &candidate.target);
                return target_binding != nullptr &&
                       target_binding->controllable_component_id ==
                           component.controllable_component_id &&
                       target_binding->component_type_revision ==
                           component.component_type_revision &&
                       target_binding->command_target_id ==
                           target.command_target_id &&
                       target_binding->control_binding_id ==
                           anchor->control_binding_id &&
                       target_binding->control_binding_revision ==
                           anchor->control_binding_revision;
              });
          if (count != 1) {
            fail("a capability-provider target capability does not have "
                 "exactly one row for its target/control-binding anchor");
          }
        }
      }
    }
  }
}

[[nodiscard]] CapabilityAndEndpointResult
qualify_capabilities_and_endpoints(
    const CompositionQualifiedCellIr& composition,
    const RevisionVerifiedDeploymentApplicabilityInputs& deployment_inputs,
    const QualifiedIndexes& indexes,
    const std::optional<core::SiteOverlayRefDTO>& selected_overlay,
    LogicalNameRegistry& names, std::size_t& logical_rows) {
  const auto& graph = composition.graph();
  const auto& inputs = deployment_inputs.value();
  CapabilityAndEndpointResult result;
  result.capabilities.reserve(graph.control_bindings.size() +
                              inputs.capability_provider_bindings.size());
  result.external_resources.reserve(inputs.external_resources.size());

  for (const auto& binding : graph.control_bindings) {
    const auto& capability =
        indexes.capability_definitions
            .at(pair_key(binding.capability_id,
                         binding.capability_definition_revision))
            ->value();
    result.capabilities.push_back(
        LogicalCapabilityTemplate{
         QualifiedCapabilityOwner{ControlBindingCapabilityOwner{
             binding.control_binding_id,
             binding.control_binding_revision}},
         binding.capability_id,
         binding.capability_definition_revision,
         capability.scope,
         capability.provider_binding_kind,
         binding.provider_instance_id,
         binding.provider_manifest_id,
         binding.provider_manifest_revision,
         names.make(
             "capability",
             {{"cell_id", graph.cell_id.value()},
              {"kind", "capability"},
              {"owner_kind", "control_binding"},
              {"owner_id", binding.control_binding_id.value()},
              {"owner_revision",
               binding.control_binding_revision.value()},
              {"capability_id", binding.capability_id.value()},
              {"capability_definition_revision",
               binding.capability_definition_revision.value()}})});
    checked_add(logical_rows, 1U, kMaxCellLogicalApplicabilityRows,
                "cell logical applicability rows");
  }

  if (!inputs.external_resources.empty() &&
      !selected_overlay.has_value()) {
    fail("external-resource qualification lost the selected site overlay");
  }
  for (const auto& resource : inputs.external_resources) {
    const auto& manifest =
        indexes.manifests
            .at(pair_key(resource.provider_manifest_id,
                         resource.provider_manifest_revision))
            ->value();
    const auto inventory =
        provider_manifest_endpoint_inventory(manifest);
    if (!inventory.contains(resource.endpoint_role_id)) {
      fail("external resource endpoint role is outside its exact provider "
           "manifest inventory");
    }
    result.external_resources.push_back(
        {resource.external_resource_alias,
         resource.resource_kind,
         resource.provider_instance_id,
         resource.provider_manifest_id,
         resource.provider_manifest_revision,
         resource.endpoint_role_id,
         resource.site_overlay_value_ref,
         selected_overlay->site_overlay_id,
         selected_overlay->site_overlay_revision,
         selected_overlay->non_secret_digest,
         names.make(
             "external_resource",
             {{"cell_id", graph.cell_id.value()},
              {"kind", "external_resource"},
              {"external_resource_alias",
               resource.external_resource_alias.value()},
              {"resource_kind",
               core::to_string(resource.resource_kind)},
              {"provider_instance_id",
               resource.provider_instance_id.value()},
              {"provider_manifest_id",
               resource.provider_manifest_id.value()},
              {"provider_manifest_revision",
               resource.provider_manifest_revision.value()},
              {"endpoint_role_id", resource.endpoint_role_id.value()},
              {"site_overlay_value_ref",
               resource.site_overlay_value_ref.value()}})});
    checked_add(logical_rows, 1U, kMaxCellLogicalApplicabilityRows,
                "cell logical applicability rows");
  }

  std::set<core::CapabilityProviderBindingId> stable_binding_ids;
  for (const auto& binding :
       inputs.capability_provider_bindings) {
    if (!stable_binding_ids
             .insert(binding.capability_provider_binding_id)
             .second) {
      fail("deployment contains two revisions under one current "
           "CapabilityProviderBindingId");
    }
    const auto& capability =
        *indexes.capability_definitions.at(
            pair_key(binding.capability_id,
                     binding.capability_definition_revision));
    qualify_capability_provider_target(
        composition, binding, capability.value());
    const auto& manifest =
        *indexes.manifests.at(
            pair_key(binding.provider_manifest_id,
                     binding.provider_manifest_revision));
    const auto& offer =
        require_manifest_offer(
            manifest.value(), binding.capability_id,
            binding.capability_definition_revision);
    std::vector<core::EndpointRoleId> bound_roles;
    bound_roles.reserve(binding.endpoint_bindings.size());
    for (const auto& endpoint : binding.endpoint_bindings) {
      bound_roles.push_back(endpoint.endpoint_role_id);
    }
    if (bound_roles != offer.required_endpoint_role_ids) {
      fail("capability-provider endpoint bindings do not equal the exact "
           "provider-offer role set");
    }

    result.capabilities.push_back(
        LogicalCapabilityTemplate{
         QualifiedCapabilityOwner{CapabilityProviderCapabilityOwner{
             binding.capability_provider_binding_id,
             binding.capability_provider_binding_revision}},
         binding.capability_id,
         binding.capability_definition_revision,
         capability.value().scope,
         capability.value().provider_binding_kind,
         binding.provider_instance_id,
         binding.provider_manifest_id,
         binding.provider_manifest_revision,
         names.make(
             "capability",
             {{"cell_id", graph.cell_id.value()},
              {"kind", "capability"},
              {"owner_kind", "capability_provider_binding"},
              {"owner_id",
               binding.capability_provider_binding_id.value()},
              {"owner_revision",
               binding.capability_provider_binding_revision.value()},
              {"capability_id", binding.capability_id.value()},
              {"capability_definition_revision",
               binding.capability_definition_revision.value()}})});
    checked_add(logical_rows, 1U, kMaxCellLogicalApplicabilityRows,
                "cell logical applicability rows");

    for (const auto& endpoint : binding.endpoint_bindings) {
      LogicalEndpointSourceTemplate source =
          [&]() -> LogicalEndpointSourceTemplate {
        if (endpoint.source_kind ==
            core::CapabilityEndpointSourceKind::kRuntimeUnit) {
          const auto* runtime_id =
              std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
          if (runtime_id == nullptr) {
            fail("runtime-unit endpoint source carries a non-runtime ID");
          }
          const auto& runtime = require_runtime_unit(graph, *runtime_id);
          const auto& profile =
              indexes.runtime_profiles
                  .at(pair_key(runtime.runtime_profile_id,
                               runtime.runtime_profile_revision))
                  ->value();
          if (!std::binary_search(
                  profile.source_endpoint_role_ids.begin(),
                  profile.source_endpoint_role_ids.end(),
                  endpoint.source_endpoint_role_id)) {
            fail("runtime-unit endpoint source role is outside its exact "
                 "runtime profile");
          }
          return RuntimeEndpointSourceTemplate{
              runtime.runtime_unit_id,
              runtime.runtime_profile_id,
              runtime.runtime_profile_revision,
              endpoint.source_endpoint_role_id,
          };
        }

        const auto* alias =
            std::get_if<core::ExternalResourceAlias>(
                &endpoint.source_id);
        if (alias == nullptr) {
          fail("external-resource endpoint source carries a non-alias ID");
        }
        const auto& resource =
            require_external_resource(inputs, *alias);
        if (resource.endpoint_role_id !=
            endpoint.source_endpoint_role_id) {
          fail("external-resource source endpoint role differs from the "
               "aliased declaration");
        }
        return ExternalEndpointSourceTemplate{
            resource.external_resource_alias,
            resource.resource_kind,
            resource.provider_instance_id,
            resource.provider_manifest_id,
            resource.provider_manifest_revision,
            endpoint.source_endpoint_role_id,
            resource.site_overlay_value_ref,
            selected_overlay->site_overlay_id,
            selected_overlay->site_overlay_revision,
            selected_overlay->non_secret_digest,
        };
      }();

      result.endpoints.push_back(
          {binding.capability_provider_binding_id,
           binding.capability_provider_binding_revision,
           endpoint.endpoint_role_id,
           binding.provider_instance_id,
           binding.provider_manifest_id,
           binding.provider_manifest_revision,
           std::move(source),
           names.make(
               "provider_endpoint",
               {{"cell_id", graph.cell_id.value()},
                {"kind", "provider_endpoint"},
                {"capability_provider_binding_id",
                 binding.capability_provider_binding_id.value()},
                {"capability_provider_binding_revision",
                 binding.capability_provider_binding_revision.value()},
                {"endpoint_role_id",
                 endpoint.endpoint_role_id.value()}})});
      checked_add(logical_rows, 1U,
                  kMaxCellLogicalApplicabilityRows,
                  "cell logical applicability rows");
    }
  }
  std::sort(
      result.capabilities.begin(), result.capabilities.end(),
      [](const auto& left, const auto& right) {
        const auto key = [](const auto& value) {
          return std::visit(
              [&](const auto& owner) {
                using Owner = std::decay_t<decltype(owner)>;
                if constexpr (std::is_same_v<
                                  Owner,
                                  CapabilityProviderCapabilityOwner>) {
                  return std::tuple<std::string_view, std::string_view,
                                    std::string_view, std::string_view>{
                      "capability_provider_binding",
                      owner.capability_provider_binding_id.value(),
                      owner.capability_provider_binding_revision.value(),
                      value.capability_id.value()};
                } else {
                  return std::tuple<std::string_view, std::string_view,
                                    std::string_view, std::string_view>{
                      "control_binding", owner.control_binding_id.value(),
                      owner.control_binding_revision.value(),
                      value.capability_id.value()};
                }
              },
              value.owner);
        };
        return key(left) < key(right);
      });
  return result;
}

[[nodiscard]] const LogicalControlRouteTemplate& require_logical_route(
    const std::vector<LogicalControlRouteTemplate>& routes,
    const core::ControlBindingId& binding_id,
    const core::ControlRouteId& route_id) {
  const auto* result = find_one(
      routes, [&binding_id, &route_id](const auto& value) {
        return value.control_binding_id == binding_id &&
               value.control_route_id == route_id;
      });
  if (result == nullptr) {
    fail("Stop applicability lost its qualified logical route");
  }
  return *result;
}

[[nodiscard]] const LogicalRouteEndpointTemplate*
find_route_endpoint(const LogicalControlRouteTemplate& route,
                    const core::EndpointRoleId& role) {
  return find_one(route.endpoint_templates, [&role](const auto& value) {
    return value.endpoint_role_id == role;
  });
}

[[nodiscard]] bool registered_command_matches(
    const std::optional<core::RegisteredStopCommandKindId>& left,
    const std::optional<core::RegisteredStopCommandKindId>& right) {
  return left == right;
}

[[nodiscard]] bool registered_evidence_matches(
    const std::optional<core::RegisteredStopEvidenceKindId>& left,
    const std::optional<core::RegisteredStopEvidenceKindId>& right) {
  return left == right;
}

[[nodiscard]] std::optional<LogicalStopProviderCommandEndpoint>
qualify_stop_command_endpoint(
    const core::StopContainmentCommandDTO& command,
    const core::ProviderManifestDTO& manifest,
    const LogicalControlRouteTemplate& route) {
  if (!command.provider_contract_ref.has_value()) {
    return std::nullopt;
  }

  const auto& declaration =
      require_provider_contract(manifest,
                                *command.provider_contract_ref);
  if (!declaration.stop_command_support.has_value()) {
    fail("selected provider contract does not support the Stop command");
  }
  const auto& support = *declaration.stop_command_support;
  if (support.command_kind != command.command_kind ||
      !registered_command_matches(
          support.registered_command_kind_id,
          command.registered_command_kind_id)) {
    fail("provider Stop command support kind differs from the policy");
  }

  std::vector<const LogicalRouteEndpointTemplate*> candidates;
  for (const auto& role : support.command_endpoint_role_ids) {
    const auto* endpoint = find_route_endpoint(route, role);
    if (endpoint != nullptr && endpoint->is_command_endpoint) {
      candidates.push_back(endpoint);
    }
  }
  if (support.command_endpoint_role_ids.empty()) {
    return std::nullopt;
  }
  if (candidates.size() != 1U) {
    fail("provider Stop command endpoint alternatives do not select exactly "
         "one route command role");
  }
  return LogicalStopProviderCommandEndpoint{
      *command.provider_contract_ref,
      candidates.front()->endpoint_role_id,
      candidates.front()->logical_endpoint_symbol,
  };
}

void qualify_stop_evidence_sources(
    const core::StopEvidenceRequirementDTO& requirement,
    const BindingContext& context,
    const LogicalControlRouteTemplate& route,
    std::vector<LogicalStopProviderEvidenceEndpoint>& provider_endpoints,
    std::vector<LogicalStopBindingStateAuthoritySource>&
        binding_state_sources) {
  const auto source_kind = requirement.source_requirement.source_kind;
  if (source_kind ==
      core::StopEvidenceSourceKind::kBindingStateAuthority) {
    if (!context.interface_template.has_value() ||
        context.interface_template->state_claims.empty()) {
      fail("binding-state Stop evidence has no expanded state interfaces");
    }
    if (!contains_authority_class(
            requirement.source_requirement.allowed_authority_classes,
            context.binding->state_authority_class)) {
      fail("binding state-authority class is outside the Stop policy's "
           "allowed set");
    }
    const auto& source = requirement.source_requirement;
    if (!source.endpoint_role_id.has_value()) {
      fail("binding-state Stop evidence has no state endpoint role");
    }
    const auto* endpoint =
        find_route_endpoint(route, *source.endpoint_role_id);
    if (endpoint == nullptr || !endpoint->is_state_endpoint) {
      fail("binding-state Stop evidence endpoint is absent from the route's "
           "state-side projection");
    }
    binding_state_sources.push_back(
        {requirement.evidence_item_id,
         context.binding->state_authority_id,
         context.binding->state_authority_class,
         *source.endpoint_role_id,
         endpoint->logical_endpoint_symbol});
  }

  const core::ProviderStopEvidenceSupportDTO* support = nullptr;
  if (requirement.provider_contract_ref.has_value()) {
    const auto& declaration = require_provider_contract(
        context.manifest->value(),
        *requirement.provider_contract_ref);
    if (!declaration.stop_evidence_support.has_value()) {
      fail("selected provider contract does not support the Stop evidence");
    }
    support = &*declaration.stop_evidence_support;
    if (support->evidence_kind != requirement.evidence_kind ||
        !registered_evidence_matches(
            support->registered_evidence_kind_id,
            requirement.registered_evidence_kind_id)) {
      fail("provider Stop evidence support kind differs from the policy");
    }
  }

  const auto& source = requirement.source_requirement;
  if (source.endpoint_role_id.has_value()) {
    const auto* endpoint =
        find_route_endpoint(route, *source.endpoint_role_id);
    if (endpoint == nullptr || !endpoint->is_state_endpoint) {
      fail("Stop evidence endpoint is absent from the route's state-side "
           "projection");
    }
    if (support != nullptr &&
        !contains_role(support->evidence_endpoint_role_ids,
                       *source.endpoint_role_id)) {
      fail("provider Stop evidence endpoint is outside the exact provider "
           "support alternatives");
    }
    if (source_kind ==
        core::StopEvidenceSourceKind::kProviderEvidence) {
      if (support == nullptr) {
        fail("provider Stop evidence has no exact provider support");
      }
      provider_endpoints.push_back(
          {requirement.evidence_item_id,
           *requirement.provider_contract_ref,
           *source.endpoint_role_id,
           endpoint->logical_endpoint_symbol});
    }
  } else if (support != nullptr &&
             !support->evidence_endpoint_role_ids.empty()) {
    fail("endpointless Stop evidence selects a provider support role that "
         "requires an endpoint");
  }

  if (source_kind ==
          core::StopEvidenceSourceKind::kProviderEvidence &&
      !source.endpoint_role_id.has_value() &&
      support == nullptr) {
    fail("provider Stop evidence has no exact provider support");
  }
}

[[nodiscard]] const LogicalControllerTemplate& require_logical_controller(
    const std::vector<LogicalControllerTemplate>& controllers,
    const core::ControlDomainId& domain_id,
    const core::ControllerRoleId& role_id) {
  const auto* result = find_one(
      controllers, [&domain_id, &role_id](const auto& value) {
        return value.control_domain_id == domain_id &&
               value.controller_role_id == role_id;
      });
  if (result == nullptr) {
    fail("Stop containment scope lost a qualified logical controller");
  }
  return *result;
}

[[nodiscard]] std::vector<LogicalStopScopeTemplate> qualify_stop_scopes(
    const BindingContext& context, const core::ControlRouteDTO& route,
    const std::vector<LogicalControllerTemplate>& controllers) {
  std::vector<LogicalStopScopeTemplate> result;
  result.reserve(route.compatible_activation_set_ids.size());
  for (const auto& activation_set_id :
       route.compatible_activation_set_ids) {
    switch (context.binding->containment_granularity) {
      case core::ContainmentGranularity::kResource: {
        std::vector<core::ResourceId> resources;
        resources.reserve(context.binding->resource_bindings.size());
        for (const auto& binding : context.binding->resource_bindings) {
          resources.push_back(binding.resource_id);
        }
        if (resources.empty()) {
          fail("resource-granularity Stop scope has no bound resources");
        }
        result.emplace_back(LogicalResourceStopScopeTemplate{
            activation_set_id, std::move(resources)});
        break;
      }
      case core::ContainmentGranularity::kControllerSet: {
        const auto& activation = require_activation_set(
            context.topology->value(), activation_set_id);
        if (activation.active_controller_role_ids.empty()) {
          fail("controller-set Stop scope has no active controllers");
        }
        std::vector<std::string> symbols;
        symbols.reserve(activation.active_controller_role_ids.size());
        for (const auto& role : activation.active_controller_role_ids) {
          symbols.push_back(
              require_logical_controller(
                  controllers, context.domain->control_domain_id, role)
                  .logical_controller_symbol);
        }
        result.emplace_back(LogicalControllerSetStopScopeTemplate{
            activation_set_id, activation.active_controller_role_ids,
            std::move(symbols)});
        break;
      }
      case core::ContainmentGranularity::kControlDomain:
        result.emplace_back(LogicalControlDomainStopScopeTemplate{
            activation_set_id, context.domain->control_domain_id,
            context.domain->control_domain_revision});
        break;
    }
  }
  return result;
}

[[nodiscard]] std::size_t stop_nested_logical_rows(
    const LogicalStoppableApplicabilityTemplate& stoppable) {
  std::size_t rows = 0U;
  checked_add(rows, stoppable.deadline_budget.phases.size(),
              kMaxCellLogicalApplicabilityRows,
              "cell logical applicability rows");
  checked_add(rows, stoppable.all_of_evidence_requirements.size(),
              kMaxCellLogicalApplicabilityRows,
              "cell logical applicability rows");
  checked_add(rows, stoppable.provider_evidence_endpoints.size(),
              kMaxCellLogicalApplicabilityRows,
              "cell logical applicability rows");
  checked_add(rows, stoppable.binding_state_authority_sources.size(),
              kMaxCellLogicalApplicabilityRows,
              "cell logical applicability rows");
  checked_add(rows, stoppable.containment_scopes.size(),
              kMaxCellLogicalApplicabilityRows,
              "cell logical applicability rows");
  if (stoppable.provider_command_endpoint.has_value()) {
    checked_add(rows, 1U, kMaxCellLogicalApplicabilityRows,
                "cell logical applicability rows");
  }
  for (const auto& requirement : stoppable.all_of_evidence_requirements) {
    checked_add(
        rows,
        requirement.source_requirement.allowed_authority_classes.size(),
        kMaxCellLogicalApplicabilityRows,
        "cell logical applicability rows");
  }
  for (const auto& scope : stoppable.containment_scopes) {
    std::visit(
        [&rows](const auto& value) {
          using Scope = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<
                            Scope, LogicalResourceStopScopeTemplate>) {
            checked_add(rows, value.resource_ids.size(),
                        kMaxCellLogicalApplicabilityRows,
                        "cell logical applicability rows");
          } else if constexpr (std::is_same_v<
                                   Scope,
                                   LogicalControllerSetStopScopeTemplate>) {
            checked_add(rows, value.active_controller_role_ids.size(),
                        kMaxCellLogicalApplicabilityRows,
                        "cell logical applicability rows");
            checked_add(rows, value.logical_controller_symbols.size(),
                        kMaxCellLogicalApplicabilityRows,
                        "cell logical applicability rows");
          }
        },
        scope);
  }
  return rows;
}

[[nodiscard]] std::vector<LogicalStopApplicabilityTemplate>
qualify_stop_applicability(
    const core::CellId& cell_id,
    const std::vector<BindingContext>& bindings,
    const std::vector<LogicalControlRouteTemplate>& logical_routes,
    const std::vector<LogicalControllerTemplate>& logical_controllers,
    LogicalNameRegistry& names, std::size_t& logical_rows) {
  std::vector<LogicalStopApplicabilityTemplate> result;
  for (const auto& context : bindings) {
    const auto& binding = *context.binding;
    const auto& policy = context.stop_policy->value();
    for (const auto& route_id : binding.legal_control_route_ids) {
      const auto& logical_route = require_logical_route(
          logical_routes, binding.control_binding_id, route_id);
      if (policy.disposition ==
          core::StopEvidenceDisposition::kNotStoppable) {
        result.emplace_back(LogicalNotStoppableApplicabilityTemplate{
            binding.control_binding_id,
            binding.control_binding_revision,
            route_id,
            policy.stop_evidence_policy_id,
            policy.stop_evidence_policy_revision,
            context.domain->control_domain_id,
            context.domain->control_domain_revision,
            context.topology->value().controller_topology_id,
            context.topology->value().topology_revision,
            binding.containment_granularity,
            policy.disposition});
        checked_add(logical_rows, 1U,
                    kMaxCellLogicalApplicabilityRows,
                    "cell logical applicability rows");
        continue;
      }

      if (!policy.containment_command.has_value() ||
          !policy.deadline_budget.has_value() ||
          !policy.evidence_conjunction.has_value()) {
        fail("qualified stoppable policy lost command, deadline budget, or "
             "evidence conjunction");
      }
      const auto& route =
          require_route(context.topology->value(), route_id);
      auto provider_command_endpoint = qualify_stop_command_endpoint(
          *policy.containment_command, context.manifest->value(),
          logical_route);
      std::vector<LogicalStopProviderEvidenceEndpoint> provider_evidence;
      std::vector<LogicalStopBindingStateAuthoritySource>
          binding_state_sources;
      for (const auto& requirement :
           policy.evidence_requirements) {
        qualify_stop_evidence_sources(
            requirement, context, logical_route, provider_evidence,
            binding_state_sources);
      }
      std::sort(
          provider_evidence.begin(), provider_evidence.end(),
          [](const auto& left, const auto& right) {
            return left.evidence_item_id < right.evidence_item_id;
          });
      std::sort(
          binding_state_sources.begin(), binding_state_sources.end(),
          [](const auto& left, const auto& right) {
            return left.evidence_item_id < right.evidence_item_id;
          });
      auto scopes =
          qualify_stop_scopes(context, route, logical_controllers);
      LogicalStoppableApplicabilityTemplate stoppable{
          binding.control_binding_id,
          binding.control_binding_revision,
          route_id,
          policy.stop_evidence_policy_id,
          policy.stop_evidence_policy_revision,
          context.domain->control_domain_id,
          context.domain->control_domain_revision,
          context.topology->value().controller_topology_id,
          context.topology->value().topology_revision,
          binding.containment_granularity,
          policy.disposition,
          names.make(
              "stop",
              {{"cell_id", cell_id.value()},
               {"kind", "stop"},
               {"control_binding_id",
                binding.control_binding_id.value()},
               {"control_binding_revision",
                binding.control_binding_revision.value()},
               {"control_route_id", route_id.value()},
               {"stop_evidence_policy_id",
                policy.stop_evidence_policy_id.value()},
               {"stop_evidence_policy_revision",
                policy.stop_evidence_policy_revision.value()}}),
          *policy.containment_command,
          *policy.deadline_budget,
          *policy.evidence_conjunction,
          policy.evidence_requirements,
          std::move(provider_command_endpoint),
          std::move(provider_evidence),
          std::move(binding_state_sources),
          std::move(scopes),
      };
      checked_add(logical_rows, 1U, kMaxCellLogicalApplicabilityRows,
                  "cell logical applicability rows");
      checked_add(logical_rows, stop_nested_logical_rows(stoppable),
                  kMaxCellLogicalApplicabilityRows,
                  "cell logical applicability rows");
      result.emplace_back(std::move(stoppable));
    }
  }
  return result;
}

}  // namespace

ApplicabilityQualifiedCellIr qualify_cell_applicability(
    const CompositionQualifiedCellIr& composition,
    const RevisionVerifiedDeploymentApplicabilityInputs& deployment_inputs,
    std::span<const QualifiedRuntimeProfile> runtime_profiles,
    std::span<const QualifiedCapabilityDefinition> capability_definitions,
    std::span<const QualifiedControlInterfaceContract>
        control_interface_contracts,
    std::span<const QualifiedControllerTopology> controller_topologies,
    std::span<const QualifiedProviderManifest> provider_manifests,
    std::span<const QualifiedStopEvidencePolicy> stop_evidence_policies,
    const QualifiedSelectedSiteOverlay& selected_site_overlay) {
  const QualifiedAssistantSettings assistant_settings =
      qualify_assistant_settings(
          deployment_inputs.assistant_settings(),
          deployment_inputs.value().external_resources);
  const auto indexes = build_qualified_indexes(
      composition, deployment_inputs, runtime_profiles,
      capability_definitions, control_interface_contracts,
      controller_topologies, provider_manifests,
      stop_evidence_policies);
  const auto selected_overlay = qualify_selected_overlay(
      deployment_inputs.value(), selected_site_overlay);
  qualify_provider_instances(
      composition.graph(), deployment_inputs.value());

  auto bindings = qualify_binding_contexts(composition, indexes);
  qualify_desired_configurations(composition.graph(),
                                 deployment_inputs.value(), indexes);
  qualify_complete_capability_coverage(
      composition, deployment_inputs.value(), indexes);
  const auto expected_cardinality = preflight_applicability_output(
      bindings, deployment_inputs.value());

  LogicalNameRegistry names(composition);
  std::size_t logical_rows = 0U;
  materialize_binding_interfaces(
      composition.graph().cell_id, bindings, names,
      logical_rows);
  auto controllers_and_routes = qualify_controllers_and_routes(
      composition.graph().cell_id, bindings, names,
      logical_rows);
  auto capabilities_and_endpoints =
      qualify_capabilities_and_endpoints(
          composition, deployment_inputs, indexes, selected_overlay,
          names, logical_rows);
  auto stop_applicability = qualify_stop_applicability(
      composition.graph().cell_id, bindings,
      controllers_and_routes.routes, controllers_and_routes.controllers,
      names, logical_rows);

  std::vector<LogicalBindingInterfaceTemplate> binding_interfaces;
  binding_interfaces.reserve(bindings.size());
  std::size_t materialized_command_claims = 0U;
  std::size_t materialized_state_claims = 0U;
  for (auto& binding : bindings) {
    if (!binding.interface_template.has_value()) {
      fail("internal C0.2n error: binding interfaces were not materialized");
    }
    checked_add(materialized_command_claims,
                binding.interface_template->command_claims.size(),
                kMaxCellExpandedControlInterfaceClaimsPerDirection,
                "materialized command-interface expansion");
    checked_add(materialized_state_claims,
                binding.interface_template->state_claims.size(),
                kMaxCellExpandedControlInterfaceClaimsPerDirection,
                "materialized state-interface expansion");
    binding_interfaces.push_back(
        std::move(*binding.interface_template));
  }
  if (materialized_command_claims != expected_cardinality.command_claims ||
      materialized_state_claims != expected_cardinality.state_claims ||
      logical_rows != expected_cardinality.logical_rows) {
    fail("internal C0.2n error: materialized output cardinality differs from "
         "its checked preflight");
  }

  return ApplicabilityQualifiedCellIr(
      composition, deployment_inputs, selected_overlay,
      assistant_settings,
      copy_closure_in_key_order<QualifiedRuntimeProfile>(
          indexes.runtime_profiles, indexes.required_runtime_profiles),
      copy_closure_in_key_order<QualifiedCapabilityDefinition>(
          indexes.capability_definitions,
          indexes.required_capability_definitions),
      copy_closure_in_key_order<QualifiedControlInterfaceContract>(
          indexes.contracts, indexes.required_contracts),
      copy_closure_in_key_order<QualifiedControllerTopology>(
          indexes.topologies, indexes.required_topologies),
      copy_closure_in_key_order<QualifiedProviderManifest>(
          indexes.manifests, indexes.required_manifests),
      copy_closure_in_key_order<QualifiedStopEvidencePolicy>(
          indexes.stop_policies, indexes.required_stop_policies),
      std::move(binding_interfaces),
      std::move(controllers_and_routes.controllers),
      std::move(controllers_and_routes.routes),
      std::move(capabilities_and_endpoints.capabilities),
      std::move(capabilities_and_endpoints.endpoints),
      std::move(capabilities_and_endpoints.external_resources),
      std::move(stop_applicability));
}

}  // namespace manyforge::deployment
