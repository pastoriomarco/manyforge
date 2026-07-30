#include "manyforge_deployment/controller_topology_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "catalog_record_referent_internal.hpp"
#include "content_identity_internal.hpp"

namespace manyforge::deployment {
namespace {

using ContractKey = std::pair<std::string, std::string>;
using EdgeKey = std::pair<std::string, std::string>;
using InterfaceKey = std::pair<std::string, std::string>;

[[noreturn]] void fail(std::string message) {
  throw ControllerTopologyCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue& object, std::string_view name) {
  const auto* member = object.find_object_member(name);
  if (member == nullptr) {
    fail("controller-topology normalized value is missing " +
         std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view name) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail("controller-topology normalized field is not a string: " +
         std::string(name));
  }
  return *text;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view name) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail("controller-topology normalized field is not an array: " +
         std::string(name));
  }
  return *array;
}

[[nodiscard]] core::ResourceInterfaceClaimDTO decode_interface(
    const CanonicalValue& value) {
  return {
      core::ResourceRoleId(require_string(
          require_member(value, "resource_role_id"), "resource_role_id")),
      require_string(require_member(value, "interface_name"),
                     "interface_name"),
  };
}

[[nodiscard]] std::vector<core::ResourceInterfaceClaimDTO> decode_interfaces(
    const CanonicalValue& value, std::string_view name) {
  const auto& array = require_array(value, name);
  std::vector<core::ResourceInterfaceClaimDTO> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.push_back(decode_interface(item));
  }
  return result;
}

[[nodiscard]] core::ControllerClaimDirection decode_direction(
    const CanonicalValue& value) {
  const auto& direction = require_string(value, "claimed_directions");
  if (direction == "command") {
    return core::ControllerClaimDirection::kCommand;
  }
  if (direction == "state") {
    return core::ControllerClaimDirection::kState;
  }
  fail("controller claim has an unknown direction");
}

[[nodiscard]] std::vector<core::ControllerClaimDirection> decode_directions(
    const CanonicalValue& value) {
  const auto& array = require_array(value, "claimed_directions");
  std::vector<core::ControllerClaimDirection> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.push_back(decode_direction(item));
  }
  return result;
}

[[nodiscard]] core::ControllerControlInterfaceClaimDTO
decode_control_interface_claim(const CanonicalValue& value) {
  return {
      core::ControlInterfaceContractId(require_string(
          require_member(value, "control_interface_contract_id"),
          "control_interface_contract_id")),
      core::ControlInterfaceRevision(require_string(
          require_member(value, "control_interface_revision"),
          "control_interface_revision")),
      decode_directions(require_member(value, "claimed_directions")),
  };
}

[[nodiscard]] std::vector<core::ControllerControlInterfaceClaimDTO>
decode_control_interface_claims(const CanonicalValue& value) {
  const auto& array = require_array(value, "control_interface_claims");
  std::vector<core::ControllerControlInterfaceClaimDTO> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.push_back(decode_control_interface_claim(item));
  }
  return result;
}

[[nodiscard]] core::ControllerTopologyControllerDTO decode_controller(
    const CanonicalValue& value) {
  return {
      core::ControllerRoleId(require_string(
          require_member(value, "controller_role_id"), "controller_role_id")),
      decode_control_interface_claims(
          require_member(value, "control_interface_claims")),
      decode_interfaces(require_member(value, "exported_reference_interfaces"),
                        "exported_reference_interfaces"),
      decode_interfaces(require_member(value, "exported_state_interfaces"),
                        "exported_state_interfaces"),
  };
}

[[nodiscard]] std::vector<core::ControllerTopologyControllerDTO>
decode_controllers(const CanonicalValue& value) {
  const auto& array = require_array(value, "controllers");
  std::vector<core::ControllerTopologyControllerDTO> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.push_back(decode_controller(item));
  }
  return result;
}

[[nodiscard]] core::ControllerChainEdgeRefDTO decode_edge_ref(
    const CanonicalValue& value) {
  return {
      core::ControllerRoleId(require_string(
          require_member(value, "upstream_controller_role_id"),
          "upstream_controller_role_id")),
      core::ControllerRoleId(require_string(
          require_member(value, "downstream_controller_role_id"),
          "downstream_controller_role_id")),
  };
}

[[nodiscard]] core::ControllerChainEdgeDTO decode_edge(
    const CanonicalValue& value) {
  const auto ref = decode_edge_ref(value);
  return {
      ref.upstream_controller_role_id,
      ref.downstream_controller_role_id,
      decode_interfaces(require_member(value, "reference_interfaces"),
                        "reference_interfaces"),
      decode_interfaces(require_member(value, "state_interfaces"),
                        "state_interfaces"),
  };
}

[[nodiscard]] std::vector<core::ControllerChainEdgeDTO> decode_edges(
    const CanonicalValue& value) {
  const auto& array = require_array(value, "chain_edges");
  std::vector<core::ControllerChainEdgeDTO> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.push_back(decode_edge(item));
  }
  return result;
}

[[nodiscard]] std::vector<core::ControllerRoleId> decode_controller_roles(
    const CanonicalValue& value, std::string_view name) {
  const auto& array = require_array(value, name);
  std::vector<core::ControllerRoleId> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.emplace_back(require_string(item, name));
  }
  return result;
}

[[nodiscard]] std::vector<core::ControllerChainEdgeRefDTO> decode_edge_refs(
    const CanonicalValue& value) {
  const auto& array = require_array(value, "active_chain_edges");
  std::vector<core::ControllerChainEdgeRefDTO> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.push_back(decode_edge_ref(item));
  }
  return result;
}

[[nodiscard]] core::ControllerActivationSetDTO decode_activation_set(
    const CanonicalValue& value) {
  return {
      core::ActivationSetId(require_string(
          require_member(value, "activation_set_id"), "activation_set_id")),
      decode_controller_roles(
          require_member(value, "active_controller_role_ids"),
          "active_controller_role_ids"),
      decode_edge_refs(require_member(value, "active_chain_edges")),
  };
}

[[nodiscard]] std::vector<core::ControllerActivationSetDTO>
decode_activation_sets(const CanonicalValue& value) {
  const auto& array = require_array(value, "activation_sets");
  std::vector<core::ControllerActivationSetDTO> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.push_back(decode_activation_set(item));
  }
  return result;
}

[[nodiscard]] std::vector<core::ActivationSetId>
decode_activation_set_ids(const CanonicalValue& value) {
  const auto& array =
      require_array(value, "compatible_activation_set_ids");
  std::vector<core::ActivationSetId> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.emplace_back(
        require_string(item, "compatible_activation_set_ids"));
  }
  return result;
}

[[nodiscard]] core::ControlRouteDTO decode_route(
    const CanonicalValue& value) {
  return {
      core::ControlRouteId(require_string(
          require_member(value, "control_route_id"), "control_route_id")),
      core::ControlInterfaceContractId(require_string(
          require_member(value, "control_interface_contract_id"),
          "control_interface_contract_id")),
      core::ControlInterfaceRevision(require_string(
          require_member(value, "control_interface_revision"),
          "control_interface_revision")),
      decode_controller_roles(
          require_member(value, "ordered_controller_role_ids"),
          "ordered_controller_role_ids"),
      decode_activation_set_ids(
          require_member(value, "compatible_activation_set_ids")),
  };
}

[[nodiscard]] std::vector<core::ControlRouteDTO> decode_routes(
    const CanonicalValue& value) {
  const auto& array = require_array(value, "legal_routes");
  std::vector<core::ControlRouteDTO> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    result.push_back(decode_route(item));
  }
  return result;
}

[[nodiscard]] core::ControllerTopologyDTO decode_normalized(
    const CanonicalValue& normalized) {
  return {
      core::ControllerTopologyId(require_string(
          require_member(normalized, "controller_topology_id"),
          "controller_topology_id")),
      core::TopologyRevision(require_string(
          require_member(normalized, "topology_revision"),
          "topology_revision")),
      decode_controllers(require_member(normalized, "controllers")),
      decode_edges(require_member(normalized, "chain_edges")),
      decode_activation_sets(require_member(normalized, "activation_sets")),
      decode_routes(require_member(normalized, "legal_routes")),
  };
}

[[nodiscard]] ContractKey contract_key(
    const core::ControlInterfaceContractId& id,
    const core::ControlInterfaceRevision& revision) {
  return {id.value(), revision.value()};
}

[[nodiscard]] EdgeKey edge_key(const core::ControllerRoleId& upstream,
                               const core::ControllerRoleId& downstream) {
  return {upstream.value(), downstream.value()};
}

[[nodiscard]] InterfaceKey interface_key(
    const core::ResourceInterfaceClaimDTO& interface) {
  return {interface.resource_role_id.value(), interface.interface_name};
}

template <typename Value>
void validate_ordered_interfaces(const std::vector<Value>& interfaces,
                                 std::string_view label) {
  std::set<InterfaceKey> unique;
  std::set<std::string> completed_roles;
  std::string active_role;
  for (const auto& interface : interfaces) {
    if (!unique.insert(interface_key(interface)).second) {
      fail(std::string(label) +
           " contains a duplicate resource-role/interface pair");
    }
    const auto& role = interface.resource_role_id.value();
    if (active_role != role) {
      if (!active_role.empty()) {
        completed_roles.insert(active_role);
      }
      if (completed_roles.contains(role)) {
        fail(std::string(label) +
             " contains a non-contiguous resource-role group");
      }
      active_role = role;
    }
  }
}

[[nodiscard]] bool is_interface_subsequence(
    const std::vector<core::ResourceInterfaceClaimDTO>& selected,
    const std::vector<core::ResourceInterfaceClaimDTO>& available) {
  auto cursor = available.begin();
  for (const auto& interface : selected) {
    cursor = std::find_if(
        cursor, available.end(), [&interface](const auto& candidate) {
          return interface_key(candidate) == interface_key(interface);
        });
    if (cursor == available.end()) {
      return false;
    }
    ++cursor;
  }
  return true;
}

[[nodiscard]] bool contains_direction(
    const core::ControllerControlInterfaceClaimDTO& claim,
    core::ControllerClaimDirection direction) {
  return std::find(claim.claimed_directions.begin(),
                   claim.claimed_directions.end(),
                   direction) != claim.claimed_directions.end();
}

using ContractIndex =
    std::map<ContractKey, const QualifiedControlInterfaceContract*>;

[[nodiscard]] ContractIndex build_contract_index(
    std::span<const QualifiedControlInterfaceContract> contracts) {
  if (contracts.size() >
      core::kMaxControllerTopologyDistinctControlInterfaceContracts) {
    fail("qualified contract dependency view exceeds its fixed bound");
  }
  ContractIndex index;
  for (const auto& contract : contracts) {
    const auto& value = contract.value();
    if (!index
             .emplace(contract_key(value.control_interface_contract_id,
                                   value.control_interface_revision),
                      &contract)
             .second) {
      fail("qualified contract dependency view contains a duplicate exact "
           "referent");
    }
  }
  return index;
}

[[nodiscard]] const QualifiedControlInterfaceContract&
require_qualified_contract(const ContractIndex& contracts,
                           const ContractKey& key) {
  const auto exact = contracts.find(key);
  if (exact != contracts.end()) {
    return *exact->second;
  }
  const bool stale = std::any_of(
      contracts.begin(), contracts.end(),
      [&key](const auto& entry) { return entry.first.first == key.first; });
  fail(stale ? "controller-topology selects a stale control-interface "
                   "contract revision"
             : "controller-topology selects a missing qualified "
                   "control-interface contract");
}

void validate_acyclic(const std::set<std::string>& controller_roles,
                      const std::set<EdgeKey>& edges) {
  std::map<std::string, std::vector<std::string>> adjacency;
  for (const auto& role : controller_roles) {
    adjacency.emplace(role, std::vector<std::string>{});
  }
  for (const auto& [upstream, downstream] : edges) {
    adjacency.at(upstream).push_back(downstream);
  }

  enum class Mark { kUnvisited, kVisiting, kVisited };
  std::map<std::string, Mark> marks;
  for (const auto& role : controller_roles) {
    marks.emplace(role, Mark::kUnvisited);
  }
  std::function<void(const std::string&)> visit =
      [&](const std::string& role) {
        if (marks.at(role) == Mark::kVisiting) {
          fail("controller-topology chain graph contains a cycle");
        }
        if (marks.at(role) == Mark::kVisited) {
          return;
        }
        marks.at(role) = Mark::kVisiting;
        for (const auto& downstream : adjacency.at(role)) {
          visit(downstream);
        }
        marks.at(role) = Mark::kVisited;
      };
  for (const auto& role : controller_roles) {
    visit(role);
  }
}

void validate_semantics(
    const core::ControllerTopologyDTO& topology,
    std::span<const QualifiedControlInterfaceContract> qualified_contracts) {
  const auto contracts = build_contract_index(qualified_contracts);

  std::map<std::string, const core::ControllerTopologyControllerDTO*>
      controllers;
  for (const auto& controller : topology.controllers) {
    if (!controllers
             .emplace(controller.controller_role_id.value(), &controller)
             .second) {
      fail("controllers contains a duplicate controller role");
    }
    validate_ordered_interfaces(controller.exported_reference_interfaces,
                                "exported_reference_interfaces");
    validate_ordered_interfaces(controller.exported_state_interfaces,
                                "exported_state_interfaces");

    std::set<std::string> controller_claim_ids;
    for (const auto& claim : controller.control_interface_claims) {
      const auto key =
          contract_key(claim.control_interface_contract_id,
                       claim.control_interface_revision);
      if (!controller_claim_ids
               .insert(claim.control_interface_contract_id.value())
               .second) {
        fail("controller selects more than one revision of one contract");
      }
      const auto& contract =
          require_qualified_contract(contracts, key).value();
      if (contains_direction(claim,
                             core::ControllerClaimDirection::kCommand) &&
          contract.command_interfaces.empty()) {
        fail("controller claims an empty command side");
      }
      if (contains_direction(claim, core::ControllerClaimDirection::kState) &&
          contract.state_interfaces.empty()) {
        fail("controller claims an empty state side");
      }
    }
  }

  std::map<EdgeKey, const core::ControllerChainEdgeDTO*> edges;
  using ReferenceSlotKey =
      std::tuple<std::string, std::string, std::string>;
  std::map<ReferenceSlotKey, std::size_t> reference_slot_ids;
  std::map<EdgeKey, std::vector<std::size_t>> edge_reference_slots;
  std::size_t next_reference_slot_id = 0U;
  for (const auto& edge : topology.chain_edges) {
    const auto key = edge_key(edge.upstream_controller_role_id,
                              edge.downstream_controller_role_id);
    if (key.first == key.second) {
      fail("chain edge cannot connect a controller role to itself");
    }
    if (!controllers.contains(key.first) ||
        !controllers.contains(key.second)) {
      fail("chain edge references a missing controller role");
    }
    if (!edges.emplace(key, &edge).second) {
      fail("chain_edges contains a duplicate role pair");
    }
    if (edge.reference_interfaces.empty() &&
        edge.state_interfaces.empty()) {
      fail("chain edge must carry at least one interface");
    }
    validate_ordered_interfaces(edge.reference_interfaces,
                                "chain edge reference_interfaces");
    validate_ordered_interfaces(edge.state_interfaces,
                                "chain edge state_interfaces");

    const auto& downstream = *controllers.at(key.second);
    if (!is_interface_subsequence(
            edge.reference_interfaces,
            downstream.exported_reference_interfaces)) {
      fail("chain edge reference interfaces are not an exact ordered "
           "subsequence of downstream exports");
    }
    if (!is_interface_subsequence(edge.state_interfaces,
                                  downstream.exported_state_interfaces)) {
      fail("chain edge state interfaces are not an exact ordered subsequence "
           "of downstream exports");
    }
    auto& slots = edge_reference_slots[key];
    slots.reserve(edge.reference_interfaces.size());
    for (const auto& interface : edge.reference_interfaces) {
      const auto pair = interface_key(interface);
      const auto [slot, inserted] = reference_slot_ids.emplace(
          ReferenceSlotKey{key.second, pair.first, pair.second},
          next_reference_slot_id);
      if (inserted) {
        ++next_reference_slot_id;
      }
      slots.push_back(slot->second);
    }
  }
  std::set<EdgeKey> all_edges;
  for (const auto& [key, unused] : edges) {
    static_cast<void>(unused);
    all_edges.insert(key);
  }
  std::set<std::string> controller_roles;
  for (const auto& [role, unused] : controllers) {
    static_cast<void>(unused);
    controller_roles.insert(role);
  }
  validate_acyclic(controller_roles, all_edges);
  std::vector<std::size_t> reference_slot_generation(
      next_reference_slot_id, 0U);
  std::size_t activation_generation = 0U;

  struct ActivationSetSummary final {
    std::set<std::string> active_roles;
    std::set<EdgeKey> active_edges;
  };
  std::map<std::string, ActivationSetSummary> activation_sets;
  using ActivationSignature =
      std::pair<std::vector<std::string>, std::vector<EdgeKey>>;
  std::set<ActivationSignature> signatures;
  std::set<EdgeKey> edges_used_by_sets;
  std::set<std::string> controllers_used_by_sets;
  for (const auto& activation : topology.activation_sets) {
    ++activation_generation;
    if (activation_sets.contains(activation.activation_set_id.value())) {
      fail("activation_sets contains a duplicate activation-set ID");
    }
    std::set<std::string> active_roles;
    std::vector<std::string> signature_roles;
    for (const auto& role : activation.active_controller_role_ids) {
      if (!controllers.contains(role.value())) {
        fail("activation set references a missing controller role");
      }
      if (!active_roles.insert(role.value()).second) {
        fail("activation set contains a duplicate controller role");
      }
      signature_roles.push_back(role.value());
      controllers_used_by_sets.insert(role.value());
    }
    std::set<EdgeKey> active_edges;
    std::vector<EdgeKey> signature_edges;
    std::map<std::string, std::size_t> active_edges_by_downstream;
    for (const auto& edge : activation.active_chain_edges) {
      const auto key = edge_key(edge.upstream_controller_role_id,
                                edge.downstream_controller_role_id);
      if (!edges.contains(key)) {
        fail("activation set references a missing chain edge");
      }
      if (!active_roles.contains(key.first) ||
          !active_roles.contains(key.second)) {
        fail("activation-set edge endpoints must both be active");
      }
      if (!active_edges.insert(key).second) {
        fail("activation set contains a duplicate chain edge");
      }
      signature_edges.push_back(key);
      edges_used_by_sets.insert(key);
      ++active_edges_by_downstream[key.second];
    }
    for (const auto& key : active_edges) {
      if (active_edges_by_downstream.at(key.second) == 1U) {
        continue;
      }
      for (const auto slot : edge_reference_slots.at(key)) {
        if (reference_slot_generation[slot] == activation_generation) {
          fail("activation set gives one downstream reference export to "
               "multiple active upstream consumers");
        }
        reference_slot_generation[slot] = activation_generation;
      }
    }
    if (!signatures
             .emplace(std::move(signature_roles),
                      std::move(signature_edges))
             .second) {
      fail("activation_sets contains duplicate full signatures");
    }
    activation_sets.emplace(
        activation.activation_set_id.value(),
        ActivationSetSummary{std::move(active_roles),
                             std::move(active_edges)});
  }

  std::set<std::string> sets_used_by_routes;
  std::set<EdgeKey> edges_used_by_routes;
  std::set<std::string> route_ids;
  for (const auto& route : topology.legal_routes) {
    if (!route_ids.insert(route.control_route_id.value()).second) {
      fail("legal_routes contains a duplicate route ID");
    }
    const auto route_contract =
        contract_key(route.control_interface_contract_id,
                     route.control_interface_revision);
    static_cast<void>(
        require_qualified_contract(contracts, route_contract));

    std::set<std::string> route_roles;
    std::vector<EdgeKey> route_edges;
    for (const auto& role : route.ordered_controller_role_ids) {
      if (!controllers.contains(role.value())) {
        fail("control route references a missing controller role");
      }
      if (!route_roles.insert(role.value()).second) {
        fail("control route repeats a controller role");
      }
    }
    for (std::size_t index = 1U;
         index < route.ordered_controller_role_ids.size(); ++index) {
      const auto key =
          edge_key(route.ordered_controller_role_ids[index - 1U],
                   route.ordered_controller_role_ids[index]);
      if (!edges.contains(key)) {
        fail("adjacent route controller roles do not select a chain edge");
      }
      if (edges.at(key)->reference_interfaces.empty()) {
        fail("control route cannot traverse a state-only chain edge");
      }
      route_edges.push_back(key);
      edges_used_by_routes.insert(key);
    }

    const auto& terminal =
        *controllers.at(route.ordered_controller_role_ids.back().value());
    const bool terminal_claims_command = std::any_of(
        terminal.control_interface_claims.begin(),
        terminal.control_interface_claims.end(),
        [&route_contract](const auto& claim) {
          return contract_key(claim.control_interface_contract_id,
                              claim.control_interface_revision) ==
                     route_contract &&
                 contains_direction(
                     claim, core::ControllerClaimDirection::kCommand);
        });
    if (!terminal_claims_command) {
      fail("route terminal controller does not claim the route contract "
           "command side");
    }

    std::set<std::string> compatible_sets;
    for (const auto& set_id : route.compatible_activation_set_ids) {
      if (!compatible_sets.insert(set_id.value()).second) {
        fail("control route repeats a compatible activation-set ID");
      }
      const auto selected = activation_sets.find(set_id.value());
      if (selected == activation_sets.end()) {
        fail("control route references a missing activation set");
      }
      sets_used_by_routes.insert(set_id.value());
      for (const auto& role : route_roles) {
        if (!selected->second.active_roles.contains(role)) {
          fail("compatible activation set omits a route controller");
        }
      }
      for (const auto& edge : route_edges) {
        if (!selected->second.active_edges.contains(edge)) {
          fail("compatible activation set omits a route chain edge");
        }
      }
    }
  }

  if (controllers_used_by_sets.size() != controllers.size()) {
    fail("controller-topology contains a controller unused by every "
         "activation set");
  }
  if (edges_used_by_sets.size() != edges.size()) {
    fail("controller-topology contains a chain edge unused by every "
         "activation set");
  }
  for (const auto& [key, edge] : edges) {
    if (!edge->reference_interfaces.empty() &&
        !edges_used_by_routes.contains(key)) {
      fail("controller-topology contains a reference-bearing chain edge "
           "unused by every route");
    }
  }
  if (sets_used_by_routes.size() != activation_sets.size()) {
    fail("controller-topology contains an activation set unused by every "
         "route");
  }
}

}  // namespace

QualifiedControllerTopology resolve_and_qualify_controller_topology(
    const core::ControllerTopologyCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes,
    std::span<const QualifiedControlInterfaceContract> qualified_contracts) {
  UnqualifiedSchemaNormalizedArtifactCandidate normalized =
      [&reference, &resolved_store_key, stored_bytes]() {
        try {
          return detail::resolve_and_normalize_exact_catalog_referent(
              reference, resolved_store_key, stored_bytes,
              core::kControllerTopologyApiVersion, "controller topology");
        } catch (const detail::ExactCatalogReferentError& error) {
          fail(error.what());
        }
      }();

  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("controller_topology"),
      std::string(core::kControllerTopologyApiVersion),
      "topology_revision"};
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("topology_revision does not match normalized semantic content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  validate_semantics(decoded, qualified_contracts);
  if (decoded.controller_topology_id != reference.record_id ||
      decoded.topology_revision != reference.record_revision) {
    fail("controller-topology typed ID/revision selector does not match its "
         "referent");
  }
  return QualifiedControllerTopology(reference, std::move(decoded),
                                     normalized.normalized_root());
}

}  // namespace manyforge::deployment
