#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <manyforge_core/types/controller_topology.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/controller_topology_codec.hpp>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

using Claim = core::ResourceInterfaceClaimDTO;

constexpr std::string_view kControlInterfaceSchemaRevision =
    "sha256:4abdf2c402d8e6e818bc26df59ce1c9cdc911d1e807144bdf525d2f69d0a449d";
constexpr std::string_view kControllerTopologySchemaRevision =
    "sha256:7415c5e2df17491366755b401337e3ad96dd96703357b20c9e0d08bf3f46a9f6";

static_assert(!std::is_aggregate_v<QualifiedControllerTopology>);
static_assert(!std::is_default_constructible_v<QualifiedControllerTopology>);
static_assert(core::kMaxControllerTopologyControllers == 256U);
static_assert(
    core::kMaxControllerTopologyControlInterfaceClaimsPerController == 256U);
static_assert(core::kMaxControllerTopologyClaimedDirectionsPerClaim == 2U);
static_assert(
    core::kMaxControllerTopologyExportedReferenceInterfacesPerController ==
    4096U);
static_assert(
    core::kMaxControllerTopologyExportedStateInterfacesPerController ==
    4096U);
static_assert(core::kMaxControllerTopologyChainEdges == 4096U);
static_assert(
    core::kMaxControllerTopologyReferenceInterfacesPerChainEdge == 4096U);
static_assert(core::kMaxControllerTopologyStateInterfacesPerChainEdge ==
              4096U);
static_assert(core::kMaxControllerTopologyActivationSets == 256U);
static_assert(
    core::kMaxControllerTopologyActiveControllerRolesPerActivationSet ==
    256U);
static_assert(core::kMaxControllerTopologyActiveChainEdgesPerActivationSet ==
              4096U);
static_assert(core::kMaxControllerTopologyControlRoutes == 256U);
static_assert(core::kMaxControllerTopologyOrderedControllerRolesPerRoute ==
              256U);
static_assert(
    core::kMaxControllerTopologyCompatibleActivationSetsPerRoute == 256U);
static_assert(
    core::kMaxControllerTopologyDistinctControlInterfaceContracts == 256U);

[[nodiscard]] CanonicalValue string_array(
    const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue interface_value(std::string role,
                                             std::string interface) {
  return CanonicalValue(CanonicalValue::Object{
      {"resource_role_id", CanonicalValue(std::move(role))},
      {"interface_name", CanonicalValue(std::move(interface))},
  });
}

[[nodiscard]] CanonicalValue interface_array(
    const std::vector<std::pair<std::string, std::string>>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& [role, interface] : values) {
    result.push_back(interface_value(role, interface));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] std::string revision_field(std::string_view document,
                                         std::string_view field) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* revision = parsed.root().find_object_member(field);
  EXPECT_NE(revision, nullptr);
  if (revision == nullptr) {
    return {};
  }
  const auto* text = std::get_if<std::string>(&revision->storage());
  EXPECT_NE(text, nullptr);
  return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] std::string contract_document(
    std::string_view contract_id, const std::vector<Claim>& command,
    const std::vector<Claim>& state,
    const std::vector<core::ResourceRoleId>& ordering) {
  CanonicalValue::Array commands;
  for (const auto& item : command) {
    commands.push_back(
        interface_value(item.resource_role_id.value(), item.interface_name));
  }
  CanonicalValue::Array states;
  for (const auto& item : state) {
    states.push_back(
        interface_value(item.resource_role_id.value(), item.interface_name));
  }
  std::vector<std::string> role_names;
  for (const auto& role : ordering) {
    role_names.push_back(role.value());
  }
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(
           std::string(core::kControlInterfaceContractApiVersion))},
      {"control_interface_contract_id",
       CanonicalValue(std::string(contract_id))},
      {"command_interfaces", CanonicalValue(std::move(commands))},
      {"state_interfaces", CanonicalValue(std::move(states))},
      {"ordering", string_array(role_names)},
  });
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/control_interface_contract");
  preimage.push_back('\0');
  preimage.append(core::kControlInterfaceContractApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("control_interface_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] QualifiedControlInterfaceContract qualify_contract(
    std::string_view id, const std::vector<Claim>& command,
    const std::vector<Claim>& state,
    const std::vector<core::ResourceRoleId>& ordering) {
  const auto document = contract_document(id, command, state, ordering);
  const core::ContentAddressedStoreKey key{"sha256/contract-" +
                                           std::string(id)};
  const core::ControlInterfaceContractCatalogRecordRefDTO reference{
      core::ControlInterfaceContractId(std::string(id)),
      core::ControlInterfaceRevision(
          revision_field(document, "control_interface_revision")),
      core::SchemaId(
          std::string(core::kControlInterfaceContractApiVersion)),
      core::SchemaRevision(std::string(kControlInterfaceSchemaRevision)),
      core::ContentDigest(sha256_digest(document)),
      key,
  };
  return resolve_and_qualify_control_interface_contract(reference, key,
                                                         document);
}

[[nodiscard]] CanonicalValue contract_claim(
    const QualifiedControlInterfaceContract& contract,
    std::vector<std::string> directions) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_interface_contract_id",
       CanonicalValue(
           contract.value().control_interface_contract_id.value())},
      {"control_interface_revision",
       CanonicalValue(contract.value().control_interface_revision.value())},
      {"claimed_directions", string_array(directions)},
  });
}

[[nodiscard]] CanonicalValue controller(
    std::string role, CanonicalValue::Array claims = {},
    std::vector<std::pair<std::string, std::string>> references = {},
    std::vector<std::pair<std::string, std::string>> states = {}) {
  return CanonicalValue(CanonicalValue::Object{
      {"controller_role_id", CanonicalValue(std::move(role))},
      {"control_interface_claims", CanonicalValue(std::move(claims))},
      {"exported_reference_interfaces",
       interface_array(references)},
      {"exported_state_interfaces", interface_array(states)},
  });
}

[[nodiscard]] CanonicalValue edge_ref(std::string upstream,
                                      std::string downstream) {
  return CanonicalValue(CanonicalValue::Object{
      {"upstream_controller_role_id",
       CanonicalValue(std::move(upstream))},
      {"downstream_controller_role_id",
       CanonicalValue(std::move(downstream))},
  });
}

[[nodiscard]] CanonicalValue edge(
    std::string upstream, std::string downstream,
    std::vector<std::pair<std::string, std::string>> references,
    std::vector<std::pair<std::string, std::string>> states = {}) {
  auto fields = edge_ref(std::move(upstream), std::move(downstream)).as_object();
  fields.emplace_back("reference_interfaces", interface_array(references));
  fields.emplace_back("state_interfaces", interface_array(states));
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue activation(
    std::string id, std::vector<std::string> roles,
    CanonicalValue::Array edges = {}) {
  return CanonicalValue(CanonicalValue::Object{
      {"activation_set_id", CanonicalValue(std::move(id))},
      {"active_controller_role_ids", string_array(roles)},
      {"active_chain_edges", CanonicalValue(std::move(edges))},
  });
}

[[nodiscard]] CanonicalValue route(
    std::string id, const QualifiedControlInterfaceContract& contract,
    std::vector<std::string> roles, std::vector<std::string> sets) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_route_id", CanonicalValue(std::move(id))},
      {"control_interface_contract_id",
       CanonicalValue(
           contract.value().control_interface_contract_id.value())},
      {"control_interface_revision",
       CanonicalValue(contract.value().control_interface_revision.value())},
      {"ordered_controller_role_ids", string_array(roles)},
      {"compatible_activation_set_ids", string_array(sets)},
  });
}

[[nodiscard]] CanonicalValue topology_identity(
    CanonicalValue::Array controllers, CanonicalValue::Array edges,
    CanonicalValue::Array sets, CanonicalValue::Array routes,
    std::string id = "test_topology") {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kControllerTopologyApiVersion))},
      {"controller_topology_id", CanonicalValue(std::move(id))},
      {"controllers", CanonicalValue(std::move(controllers))},
      {"chain_edges", CanonicalValue(std::move(edges))},
      {"activation_sets", CanonicalValue(std::move(sets))},
      {"legal_routes", CanonicalValue(std::move(routes))},
  });
}

[[nodiscard]] std::string topology_document(CanonicalValue identity) {
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/controller_topology");
  preimage.push_back('\0');
  preimage.append(core::kControllerTopologyApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("topology_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] core::ControllerTopologyCatalogRecordRefDTO topology_reference(
    std::string_view id, std::string_view document,
    const core::ContentAddressedStoreKey& key) {
  return {
      core::ControllerTopologyId(std::string(id)),
      core::TopologyRevision(
          revision_field(document, "topology_revision")),
      core::SchemaId(std::string(core::kControllerTopologyApiVersion)),
      core::SchemaRevision(std::string(kControllerTopologySchemaRevision)),
      core::ContentDigest(sha256_digest(document)),
      key,
  };
}

[[nodiscard]] QualifiedControllerTopology qualify_topology(
    CanonicalValue identity,
    const std::vector<QualifiedControlInterfaceContract>& contracts,
    std::string_view id = "test_topology") {
  const auto document = topology_document(std::move(identity));
  const core::ContentAddressedStoreKey key{"sha256/topology"};
  return resolve_and_qualify_controller_topology(
      topology_reference(id, document, key), key, document, contracts);
}

[[nodiscard]] CanonicalValue direct_identity(
    const QualifiedControlInterfaceContract& contract) {
  return topology_identity(
      {controller("hardware",
                  {contract_claim(contract, {"command", "state"})})},
      {}, {activation("direct", {"hardware"})},
      {route("direct_route", contract, {"hardware"}, {"direct"})});
}

[[nodiscard]] CanonicalValue chained_identity(
    const QualifiedControlInterfaceContract& contract) {
  return topology_identity(
      {controller("downstream",
                  {contract_claim(contract, {"command", "state"})},
                  {{"arm", "position"}}, {{"arm", "position"}}),
       controller("upstream")},
      {edge("upstream", "downstream", {{"arm", "position"}},
            {{"arm", "position"}})},
      {activation("chain", {"downstream", "upstream"},
                  {edge_ref("upstream", "downstream")})},
      {route("chain_route", contract, {"upstream", "downstream"},
             {"chain"})});
}

TEST(ControllerTopologyCodec,
     QualifiesDirectAndChainedDeclarationsWithoutRuntimeFacts) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}},
      {{core::ResourceRoleId("arm"), "position"}},
      {core::ResourceRoleId("arm")}));

  const auto direct = qualify_topology(direct_identity(contracts.front()),
                                       contracts);
  EXPECT_EQ(direct.value().controllers.size(), 1U);
  EXPECT_TRUE(direct.value().chain_edges.empty());

  const auto chained = qualify_topology(
      chained_identity(contracts.front()), contracts);
  ASSERT_EQ(chained.value().chain_edges.size(), 1U);
  EXPECT_EQ(chained.value()
                .chain_edges.front()
                .upstream_controller_role_id,
            core::ControllerRoleId("upstream"));
  EXPECT_TRUE(
      chained.value().controllers.back().control_interface_claims.empty());
}

TEST(ControllerTopologyCodec,
     CanonicalizesSetsButPreservesRouteAndInterfaceOrder) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"},
       {core::ResourceRoleId("arm"), "velocity"}},
      {{core::ResourceRoleId("arm"), "position"},
       {core::ResourceRoleId("arm"), "velocity"}},
      {core::ResourceRoleId("arm")}));
  const auto canonical_identity = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command", "state"})},
                  {{"arm", "position"}, {"arm", "velocity"}}),
       controller("upstream")},
      {edge("upstream", "downstream",
            {{"arm", "position"}, {"arm", "velocity"}})},
      {activation("chain", {"downstream", "upstream"},
                  {edge_ref("upstream", "downstream")})},
      {route("chain_route", contracts.front(), {"upstream", "downstream"},
             {"chain"})});
  const auto canonical = topology_document(canonical_identity);
  auto shuffled_identity = topology_identity(
      {controller("upstream"),
       controller("downstream",
                  {contract_claim(contracts.front(), {"state", "command"})},
                  {{"arm", "position"}, {"arm", "velocity"}})},
      {edge("upstream", "downstream",
            {{"arm", "position"}, {"arm", "velocity"}})},
      {activation("chain", {"upstream", "downstream"},
                  {edge_ref("upstream", "downstream")})},
      {route("chain_route", contracts.front(), {"upstream", "downstream"},
             {"chain"})});
  auto shuffled_root = shuffled_identity.as_object();
  shuffled_root.emplace_back(
      "topology_revision",
      CanonicalValue(revision_field(canonical, "topology_revision")));
  const auto shuffled =
      canonical_json(CanonicalValue(std::move(shuffled_root)));
  const core::ContentAddressedStoreKey key{"sha256/topology-shuffled"};
  const auto reference =
      topology_reference("test_topology", shuffled, key);
  const auto qualified = resolve_and_qualify_controller_topology(
      reference, key, shuffled, contracts);
  EXPECT_EQ(qualified.reference(), reference);
  ASSERT_EQ(qualified.value().controllers.size(), 2U);
  EXPECT_EQ(qualified.value().controllers.front().controller_role_id,
            core::ControllerRoleId("downstream"));
  EXPECT_EQ(
      qualified.value().activation_sets.front().active_controller_role_ids,
      (std::vector<core::ControllerRoleId>{
          core::ControllerRoleId("downstream"),
          core::ControllerRoleId("upstream")}));
  EXPECT_EQ(qualified.value()
                .controllers.front()
                .control_interface_claims.front()
                .claimed_directions,
            (std::vector<core::ControllerClaimDirection>{
                core::ControllerClaimDirection::kCommand,
                core::ControllerClaimDirection::kState}));
  EXPECT_EQ(
      qualified.value().legal_routes.front().ordered_controller_role_ids,
      (std::vector<core::ControllerRoleId>{
          core::ControllerRoleId("upstream"),
          core::ControllerRoleId("downstream")}));
  EXPECT_EQ(
      qualified.value().chain_edges.front().reference_interfaces,
      (std::vector<Claim>{
          {core::ResourceRoleId("arm"), "position"},
          {core::ResourceRoleId("arm"), "velocity"}}));
  EXPECT_EQ(canonical_json(qualified.normalized_root()), canonical);
}

TEST(ControllerTopologyCodec,
     RequiresExactQualifiedContractDependenciesAndNonemptyClaimSides) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}},
      {{core::ResourceRoleId("arm"), "position"}},
      {core::ResourceRoleId("arm")}));
  EXPECT_THROW(
      qualify_topology(direct_identity(contracts.front()), {}),
      ControllerTopologyCodecError);

  auto duplicates = contracts;
  duplicates.push_back(contracts.front());
  EXPECT_THROW(
      qualify_topology(direct_identity(contracts.front()), duplicates),
      ControllerTopologyCodecError);

  std::vector<QualifiedControlInterfaceContract> stale;
  stale.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "effort"}},
      {{core::ResourceRoleId("arm"), "position"}},
      {core::ResourceRoleId("arm")}));
  EXPECT_THROW(
      qualify_topology(direct_identity(contracts.front()), stale),
      ControllerTopologyCodecError);

  std::vector<QualifiedControlInterfaceContract> two_revisions = contracts;
  two_revisions.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "effort"}},
      {{core::ResourceRoleId("arm"), "position"}},
      {core::ResourceRoleId("arm")}));
  const auto duplicate_contract_id = topology_identity(
      {controller(
          "hardware",
          {contract_claim(two_revisions.front(), {"command"}),
           contract_claim(two_revisions.back(), {"command"})})},
      {}, {activation("direct", {"hardware"})},
      {route("direct_route", two_revisions.front(), {"hardware"},
             {"direct"})});
  EXPECT_THROW(
      qualify_topology(duplicate_contract_id, two_revisions),
      ClosedSchemaError);

  std::vector<QualifiedControlInterfaceContract> state_only;
  state_only.push_back(qualify_contract(
      "state_only", {},
      {{core::ResourceRoleId("arm"), "position"}},
      {core::ResourceRoleId("arm")}));
  const auto invalid = topology_identity(
      {controller("hardware",
                  {contract_claim(state_only.front(), {"command"})})},
      {}, {activation("direct", {"hardware"})},
      {route("direct_route", state_only.front(), {"hardware"}, {"direct"})});
  EXPECT_THROW(qualify_topology(invalid, state_only),
               ControllerTopologyCodecError);
}

TEST(ControllerTopologyCodec,
     RejectsCyclesDanglingEdgesAndInactiveEdgeEndpoints) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}}, {},
      {core::ResourceRoleId("arm")}));
  const auto controllers = CanonicalValue::Array{
      controller("a", {contract_claim(contracts.front(), {"command"})},
                 {{"arm", "position"}}),
      controller("b", {contract_claim(contracts.front(), {"command"})},
                 {{"arm", "position"}})};
  const auto cycle = topology_identity(
      controllers,
      {edge("a", "b", {{"arm", "position"}}),
       edge("b", "a", {{"arm", "position"}})},
      {activation("both", {"a", "b"},
                  {edge_ref("a", "b"), edge_ref("b", "a")})},
      {route("cycle", contracts.front(), {"a", "b"}, {"both"})});
  EXPECT_THROW(qualify_topology(cycle, contracts),
               ControllerTopologyCodecError);

  const auto dangling = topology_identity(
      controllers, {edge("a", "missing", {{"arm", "position"}})},
      {activation("both", {"a", "b"})},
      {route("direct", contracts.front(), {"a"}, {"both"})});
  EXPECT_THROW(qualify_topology(dangling, contracts),
               ControllerTopologyCodecError);

  const auto inactive = topology_identity(
      controllers, {edge("a", "b", {{"arm", "position"}})},
      {activation("only_a", {"a"}, {edge_ref("a", "b")})},
      {route("direct", contracts.front(), {"a"}, {"only_a"})});
  EXPECT_THROW(qualify_topology(inactive, contracts),
               ControllerTopologyCodecError);
}

TEST(ControllerTopologyCodec,
     EnforcesExportGroupingOrderedSubsequenceAndSingleReferenceConsumer) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}}, {},
      {core::ResourceRoleId("arm")}));

  const auto noncontiguous = topology_identity(
      {controller("hardware",
                  {contract_claim(contracts.front(), {"command"})},
                  {{"arm", "position"}, {"tool", "position"},
                   {"arm", "velocity"}})},
      {}, {activation("direct", {"hardware"})},
      {route("direct", contracts.front(), {"hardware"}, {"direct"})});
  EXPECT_THROW(qualify_topology(noncontiguous, contracts),
               ControllerTopologyCodecError);

  const auto reordered = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command"})},
                  {{"arm", "position"}, {"arm", "velocity"}}),
       controller("upstream")},
      {edge("upstream", "downstream",
            {{"arm", "velocity"}, {"arm", "position"}})},
      {activation("chain", {"downstream", "upstream"},
                  {edge_ref("upstream", "downstream")})},
      {route("chain", contracts.front(), {"upstream", "downstream"},
             {"chain"})});
  EXPECT_THROW(qualify_topology(reordered, contracts),
               ControllerTopologyCodecError);

  const auto contested = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command"})},
                  {{"arm", "position"}}),
       controller("upstream_a"), controller("upstream_b")},
      {edge("upstream_a", "downstream", {{"arm", "position"}}),
       edge("upstream_b", "downstream", {{"arm", "position"}})},
      {activation("all", {"downstream", "upstream_a", "upstream_b"},
                  {edge_ref("upstream_a", "downstream"),
                   edge_ref("upstream_b", "downstream")})},
      {route("a", contracts.front(), {"upstream_a", "downstream"},
             {"all"}),
       route("b", contracts.front(), {"upstream_b", "downstream"},
             {"all"})});
  EXPECT_THROW(qualify_topology(contested, contracts),
               ControllerTopologyCodecError);
}

TEST(ControllerTopologyCodec,
     QualifiesRepeatedActivationSetsUsingCompactReferenceSlots) {
  std::vector<Claim> command_interfaces;
  std::vector<std::pair<std::string, std::string>> exported_interfaces;
  std::vector<std::pair<std::string, std::string>> first_edge_interfaces;
  std::vector<std::pair<std::string, std::string>> second_edge_interfaces;
  for (std::size_t index = 0U; index < 256U; ++index) {
    const auto suffix =
        (index < 10U ? "00" : (index < 100U ? "0" : "")) +
        std::to_string(index);
    command_interfaces.push_back(
        {core::ResourceRoleId("arm"), "interface_" + suffix});
    exported_interfaces.emplace_back("arm", "interface_" + suffix);
    auto& edge_interfaces =
        index < 128U ? first_edge_interfaces : second_edge_interfaces;
    edge_interfaces.emplace_back("arm", "interface_" + suffix);
  }

  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "wide_arm_command", command_interfaces, {},
      {core::ResourceRoleId("arm")}));

  CanonicalValue::Array controllers;
  controllers.push_back(controller(
      "downstream",
      {contract_claim(contracts.front(), {"command"})},
      exported_interfaces));
  for (std::size_t index = 0U; index < 128U; ++index) {
    const auto suffix =
        (index < 10U ? "00" : (index < 100U ? "0" : "")) +
        std::to_string(index);
    controllers.push_back(controller("marker_" + suffix));
  }
  controllers.push_back(controller("upstream_a"));
  controllers.push_back(controller("upstream_b"));

  CanonicalValue::Array activation_sets;
  std::vector<std::string> activation_set_ids;
  for (std::size_t index = 0U; index < 128U; ++index) {
    const auto suffix =
        (index < 10U ? "00" : (index < 100U ? "0" : "")) +
        std::to_string(index);
    const auto set_id = "set_" + suffix;
    activation_set_ids.push_back(set_id);
    activation_sets.push_back(activation(
        set_id,
        {"downstream", "marker_" + suffix, "upstream_a", "upstream_b"},
        {edge_ref("upstream_a", "downstream"),
         edge_ref("upstream_b", "downstream")}));
  }

  const auto identity = topology_identity(
      std::move(controllers),
      {edge("upstream_a", "downstream", first_edge_interfaces),
       edge("upstream_b", "downstream", second_edge_interfaces)},
      std::move(activation_sets),
      {route("route_a", contracts.front(), {"upstream_a", "downstream"},
             activation_set_ids),
       route("route_b", contracts.front(), {"upstream_b", "downstream"},
             activation_set_ids)});

  const auto qualified = qualify_topology(identity, contracts);
  EXPECT_EQ(qualified.value().activation_sets.size(), 128U);
  EXPECT_EQ(qualified.value().chain_edges.size(), 2U);
}

TEST(ControllerTopologyCodec,
     AllowsMultipleActiveConsumersOfOneDownstreamStateExport) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}},
      {{core::ResourceRoleId("arm"), "position"}},
      {core::ResourceRoleId("arm")}));
  const auto shared_state = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command", "state"})},
                  {}, {{"arm", "position"}}),
       controller("observer_a"), controller("observer_b")},
      {edge("observer_a", "downstream", {}, {{"arm", "position"}}),
       edge("observer_b", "downstream", {}, {{"arm", "position"}})},
      {activation("all", {"downstream", "observer_a", "observer_b"},
                  {edge_ref("observer_a", "downstream"),
                   edge_ref("observer_b", "downstream")})},
      {route("direct_route", contracts.front(), {"downstream"}, {"all"})});
  EXPECT_NO_THROW(qualify_topology(shared_state, contracts));
}

TEST(ControllerTopologyCodec,
     RejectsDuplicateActivationSignaturesAndOrphanDeclarations) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}}, {},
      {core::ResourceRoleId("arm")}));
  const auto duplicate_sets = topology_identity(
      {controller("hardware",
                  {contract_claim(contracts.front(), {"command"})})},
      {},
      {activation("first", {"hardware"}),
       activation("second", {"hardware"})},
      {route("direct", contracts.front(), {"hardware"},
             {"first", "second"})});
  EXPECT_THROW(qualify_topology(duplicate_sets, contracts),
               ControllerTopologyCodecError);

  const auto orphan_controller = topology_identity(
      {controller("hardware",
                  {contract_claim(contracts.front(), {"command"})}),
       controller("unused")},
      {}, {activation("direct", {"hardware"})},
      {route("direct", contracts.front(), {"hardware"}, {"direct"})});
  EXPECT_THROW(qualify_topology(orphan_controller, contracts),
               ControllerTopologyCodecError);

  const auto orphan_set = topology_identity(
      {controller("hardware",
                  {contract_claim(contracts.front(), {"command"})})},
      {},
      {activation("used", {"hardware"}),
       activation("unused", {"hardware"})},
      {route("direct", contracts.front(), {"hardware"}, {"used"})});
  EXPECT_THROW(qualify_topology(orphan_set, contracts),
               ControllerTopologyCodecError);
}

TEST(ControllerTopologyCodec,
     RequiresRoutePathActivationClosureAndTerminalCommandClaim) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}},
      {{core::ResourceRoleId("arm"), "position"}},
      {core::ResourceRoleId("arm")}));

  const auto missing_edge = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command"})},
                  {{"arm", "position"}}),
       controller("upstream")},
      {edge("upstream", "downstream", {{"arm", "position"}})},
      {activation("chain", {"downstream", "upstream"})},
      {route("chain", contracts.front(), {"upstream", "downstream"},
             {"chain"})});
  EXPECT_THROW(qualify_topology(missing_edge, contracts),
               ControllerTopologyCodecError);

  const auto state_terminal = topology_identity(
      {controller("hardware",
                  {contract_claim(contracts.front(), {"state"})})},
      {}, {activation("direct", {"hardware"})},
      {route("direct", contracts.front(), {"hardware"}, {"direct"})});
  EXPECT_THROW(qualify_topology(state_terminal, contracts),
               ControllerTopologyCodecError);

  const auto state_only_route = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command", "state"})},
                  {}, {{"arm", "position"}}),
       controller("observer")},
      {edge("observer", "downstream", {}, {{"arm", "position"}})},
      {activation("observed", {"downstream", "observer"},
                  {edge_ref("observer", "downstream")})},
      {route("invalid_observer_route", contracts.front(),
             {"observer", "downstream"}, {"observed"})});
  EXPECT_THROW(qualify_topology(state_only_route, contracts),
               ControllerTopologyCodecError);
}

TEST(ControllerTopologyCodec,
     RejectsRemainingInterfaceEdgeActivationAndRouteDefects) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}}, {},
      {core::ResourceRoleId("arm")}));

  const auto duplicate_export = topology_identity(
      {controller("hardware",
                  {contract_claim(contracts.front(), {"command"})},
                  {{"arm", "position"}, {"arm", "position"}})},
      {}, {activation("direct", {"hardware"})},
      {route("direct", contracts.front(), {"hardware"}, {"direct"})});
  EXPECT_THROW(qualify_topology(duplicate_export, contracts),
               ControllerTopologyCodecError);

  const auto empty_edge = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command"})}),
       controller("upstream")},
      {edge("upstream", "downstream", {})},
      {activation("active", {"downstream", "upstream"},
                  {edge_ref("upstream", "downstream")})},
      {route("direct", contracts.front(), {"downstream"}, {"active"})});
  EXPECT_THROW(qualify_topology(empty_edge, contracts),
               ControllerTopologyCodecError);

  const auto missing_export = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command"})}),
       controller("upstream")},
      {edge("upstream", "downstream", {{"arm", "position"}})},
      {activation("active", {"downstream", "upstream"},
                  {edge_ref("upstream", "downstream")})},
      {route("chain", contracts.front(), {"upstream", "downstream"},
             {"active"})});
  EXPECT_THROW(qualify_topology(missing_export, contracts),
               ControllerTopologyCodecError);

  const auto missing_activation_edge = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command"})}),
       controller("upstream")},
      {},
      {activation("active", {"downstream", "upstream"},
                  {edge_ref("upstream", "downstream")})},
      {route("direct", contracts.front(), {"downstream"}, {"active"})});
  EXPECT_THROW(qualify_topology(missing_activation_edge, contracts),
               ControllerTopologyCodecError);

  const auto orphan_reference_edge = topology_identity(
      {controller("downstream",
                  {contract_claim(contracts.front(), {"command"})},
                  {{"arm", "position"}}),
       controller("upstream")},
      {edge("upstream", "downstream", {{"arm", "position"}})},
      {activation("active", {"downstream", "upstream"},
                  {edge_ref("upstream", "downstream")})},
      {route("direct", contracts.front(), {"downstream"}, {"active"})});
  EXPECT_THROW(qualify_topology(orphan_reference_edge, contracts),
               ControllerTopologyCodecError);

  const auto missing_route_role = topology_identity(
      {controller("hardware",
                  {contract_claim(contracts.front(), {"command"})})},
      {}, {activation("direct", {"hardware"})},
      {route("bad", contracts.front(), {"missing"}, {"direct"})});
  EXPECT_THROW(qualify_topology(missing_route_role, contracts),
               ControllerTopologyCodecError);

  const auto missing_route_set = topology_identity(
      {controller("hardware",
                  {contract_claim(contracts.front(), {"command"})})},
      {}, {activation("direct", {"hardware"})},
      {route("bad", contracts.front(), {"hardware"}, {"missing"})});
  EXPECT_THROW(qualify_topology(missing_route_set, contracts),
               ControllerTopologyCodecError);

  auto oversized_dependencies = contracts;
  oversized_dependencies.resize(
      core::kMaxControllerTopologyDistinctControlInterfaceContracts + 1U,
      contracts.front());
  EXPECT_THROW(
      qualify_topology(direct_identity(contracts.front()),
                       oversized_dependencies),
      ControllerTopologyCodecError);
}

TEST(ControllerTopologyCodec, RejectsStrictShapeAndControllerBoundOverflow) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}}, {},
      {core::ResourceRoleId("arm")}));
  const auto valid = topology_document(direct_identity(contracts.front()));
  auto open = valid;
  const auto closing = open.rfind('}');
  ASSERT_NE(closing, std::string::npos);
  open.insert(closing, ",\"plugin\":\"forbidden\"");
  const core::ContentAddressedStoreKey key{"sha256/open-topology"};
  EXPECT_THROW(resolve_and_qualify_controller_topology(
                   topology_reference("test_topology", open, key), key,
                   open, contracts),
               ClosedSchemaError);

  CanonicalValue::Array too_many_controllers;
  for (std::size_t index = 0U;
       index < core::kMaxControllerTopologyControllers + 1U; ++index) {
    too_many_controllers.push_back(
        controller("controller_" + std::to_string(index)));
  }
  const auto overflow = topology_identity(
      std::move(too_many_controllers), {},
      {activation("active", {"controller_0"})},
      {route("route", contracts.front(), {"controller_0"}, {"active"})});
  const auto overflow_document = topology_document(overflow);
  EXPECT_THROW(resolve_and_qualify_controller_topology(
                   topology_reference("test_topology", overflow_document,
                                      key),
                   key, overflow_document, contracts),
               ClosedSchemaError);
}

TEST(ControllerTopologyCodec, RejectsEveryOuterReferentMismatch) {
  std::vector<QualifiedControlInterfaceContract> contracts;
  contracts.push_back(qualify_contract(
      "arm_position",
      {{core::ResourceRoleId("arm"), "position"}}, {},
      {core::ResourceRoleId("arm")}));
  const auto document =
      topology_document(direct_identity(contracts.front()));
  const core::ContentAddressedStoreKey key{"sha256/topology-referent"};
  const auto reference =
      topology_reference("test_topology", document, key);

  EXPECT_THROW(resolve_and_qualify_controller_topology(
                   reference, core::ContentAddressedStoreKey("sha256/other"),
                   document, contracts),
               ControllerTopologyCodecError);

  auto wrong_digest = reference;
  wrong_digest.content_digest = core::ContentDigest(
      "sha256:0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_controller_topology(
                   wrong_digest, key, document, contracts),
               ControllerTopologyCodecError);

  auto wrong_id = reference;
  wrong_id.record_id = core::ControllerTopologyId("different");
  EXPECT_THROW(resolve_and_qualify_controller_topology(
                   wrong_id, key, document, contracts),
               ControllerTopologyCodecError);

  auto wrong_revision = reference;
  wrong_revision.record_revision = core::TopologyRevision(
      "sha256:0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_controller_topology(
                   wrong_revision, key, document, contracts),
               ControllerTopologyCodecError);

  auto wrong_schema_id = reference;
  wrong_schema_id.schema_id =
      core::SchemaId("manyforge.component_type.v1");
  EXPECT_THROW(resolve_and_qualify_controller_topology(
                   wrong_schema_id, key, document, contracts),
               ControllerTopologyCodecError);

  auto wrong_schema_revision = reference;
  wrong_schema_revision.schema_revision = core::SchemaRevision(
      "sha256:0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_controller_topology(
                   wrong_schema_revision, key, document, contracts),
               ControllerTopologyCodecError);

  auto stale_semantic = document;
  const auto route_name = stale_semantic.find("\"direct_route\"");
  ASSERT_NE(route_name, std::string::npos);
  stale_semantic.replace(route_name, std::string("\"direct_route\"").size(),
                         "\"changed_route\"");
  const auto stale_reference =
      topology_reference("test_topology", stale_semantic, key);
  EXPECT_THROW(resolve_and_qualify_controller_topology(
                   stale_reference, key, stale_semantic, contracts),
               ControllerTopologyCodecError);
}

}  // namespace
}  // namespace manyforge::deployment
