#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/control_interface_contract.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/controller_topology_codec.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "proof_root_transfer_test.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kCapabilitySchemaRevision =
    "sha256:2de23ede0e941349e90f0a3a344b7d240426703d250234f86e788343213727e0";
constexpr std::string_view kComponentTypeSchemaRevision =
    "sha256:8e728ad4442d46b98f0d0534f8189df9b7982f982ab99cb656e6fdbeef231009";
constexpr std::string_view kControlInterfaceSchemaRevision =
    "sha256:4abdf2c402d8e6e818bc26df59ce1c9cdc911d1e807144bdf525d2f69d0a449d";
constexpr std::string_view kControllerTopologySchemaRevision =
    "sha256:7415c5e2df17491366755b401337e3ad96dd96703357b20c9e0d08bf3f46a9f6";
constexpr std::string_view kProviderManifestSchemaRevision =
    "sha256:b242431032bf183f81718ae1b7b8501ee5f705e52d455732e0052215565bf4cc";
constexpr std::string_view kRuntimeProfileSchemaRevision =
    "sha256:c254afa8c2bfcc803f12a7af63b1f09a8ee804b1ffa2a65b7361ad9309b87ae7";
constexpr std::string_view kStopEvidencePolicySchemaRevision =
    "sha256:c75dcdd59f2101b1c47ccc020cc61b28037256a40dc71fe896f0e4d912f4a970";
constexpr std::string_view kEmptyRuntimeConfigurationSchemaRevision =
    "sha256:03457d535d31818fedaa7e44f48432a08feb705e4d22c9a8f5e50977c8dbf785";
constexpr std::string_view kEmptyProviderContractSchemaRevision =
    "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270";

constexpr std::string_view kControlCapability = "state.observed.current";
constexpr std::string_view kProviderCapability = "planning.motion.joint_goal";

static_assert(!std::is_aggregate_v<ApplicabilityQualifiedCellIr>);
static_assert(!std::is_default_constructible_v<ApplicabilityQualifiedCellIr>);
static_assert(kMaxExpandedControlInterfaceClaimsPerDirection == 65536U);
static_assert(kMaxCellExpandedControlInterfaceClaimsPerDirection == 1000000U);
static_assert(kMaxCellLogicalApplicabilityRows == 1000000U);

template <typename Value>
concept HasResolvedValueMember = requires(const Value& value) {
  value.resolved_value;
};

template <typename Value>
concept HasSecretReferenceMember = requires(const Value& value) {
  value.secret_reference_id;
};

static_assert(!HasResolvedValueMember<LogicalExternalResourceTemplate>);
static_assert(!HasSecretReferenceMember<LogicalExternalResourceTemplate>);
static_assert(!HasResolvedValueMember<ExternalEndpointSourceTemplate>);
static_assert(!HasSecretReferenceMember<ExternalEndpointSourceTemplate>);

[[nodiscard]] CanonicalValue string_array(
    const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue string_set(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  return string_array(values);
}

[[nodiscard]] std::string revisioned_document(
    CanonicalValue identity, std::string_view identity_domain,
    std::string_view api_version, std::string_view revision_field) {
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append(identity_domain);
  preimage.push_back('\0');
  preimage.append(api_version);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back(std::string(revision_field),
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] std::string revision_from_document(
    std::string_view document, std::string_view field) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* value = parsed.root().find_object_member(field);
  if (value == nullptr) {
    throw std::runtime_error("test fixture document has no revision");
  }
  const auto* text = std::get_if<std::string>(&value->storage());
  if (text == nullptr) {
    throw std::runtime_error("test fixture revision is not a string");
  }
  return *text;
}

[[nodiscard]] std::string expected_mf1_name(
    std::string_view tag,
    const std::vector<std::pair<std::string_view, std::string_view>>&
        fields) {
  std::string preimage{"manyforge.occurrence-name.mf1"};
  for (const auto& [field, value] : fields) {
    preimage.push_back('\0');
    preimage.append(field);
    preimage.push_back('=');
    preimage.append(std::to_string(value.size()));
    preimage.push_back(':');
    preimage.append(value);
  }
  const auto digest = sha256_digest(preimage);
  return "mf1_" + std::string(tag) + "_" + digest.substr(7U);
}

[[nodiscard]] const core::CapabilityDefinitionDescriptor&
builtin_capability(std::string_view id) {
  const auto* descriptor = core::find_builtin_capability_definition(id);
  if (descriptor == nullptr) {
    throw std::runtime_error("required built-in capability is absent");
  }
  return *descriptor;
}

[[nodiscard]] QualifiedCapabilityDefinition qualify_capability(
    std::string_view id) {
  const auto& descriptor = builtin_capability(id);
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kCapabilityDefinitionApiVersion))},
      {"capability_id", CanonicalValue(std::string(id))},
      {"scope",
       CanonicalValue(std::string(core::to_string(descriptor.scope)))},
      {"provider_binding_kind",
       CanonicalValue(
           std::string(core::to_string(descriptor.provider_binding_kind)))},
  });
  auto root = identity.as_object();
  root.emplace_back(
      "capability_definition_revision",
      CanonicalValue(std::string(descriptor.capability_definition_revision)));
  const std::string document =
      canonical_json(CanonicalValue(std::move(root)));
  const core::ContentAddressedStoreKey key(
      "sha256/test-capability-" + std::string(id));
  return resolve_and_qualify_capability_definition(
      {core::CapabilityId(std::string(id)),
       core::CapabilityDefinitionRevision(
           std::string(descriptor.capability_definition_revision)),
       core::SchemaId(std::string(core::kCapabilityDefinitionApiVersion)),
       core::SchemaRevision(std::string(kCapabilitySchemaRevision)),
       core::ContentDigest(sha256_digest(document)), key},
      key, document);
}

[[nodiscard]] QualifiedComponentType qualify_component_type(
    const std::vector<QualifiedCapabilityDefinition>& capabilities,
    bool include_plus_one_role = false) {
  std::vector<const QualifiedCapabilityDefinition*> ordered_capabilities;
  ordered_capabilities.reserve(capabilities.size());
  for (const auto& capability : capabilities) {
    ordered_capabilities.push_back(&capability);
  }
  std::sort(
      ordered_capabilities.begin(), ordered_capabilities.end(),
      [](const auto* left, const auto* right) {
        return left->value().capability_id <
               right->value().capability_id;
      });
  CanonicalValue::Array capability_rows;
  capability_rows.reserve(capabilities.size());
  for (const auto* capability : ordered_capabilities) {
    capability_rows.emplace_back(CanonicalValue::Object{
        {"capability_id",
         CanonicalValue(capability->value().capability_id.value())},
        {"capability_definition_revision",
         CanonicalValue(
             capability->value().capability_definition_revision.value())},
    });
  }
  CanonicalValue::Array resource_roles{
      CanonicalValue(CanonicalValue::Object{
          {"resource_role_id", CanonicalValue("axis")},
          {"source_kind", CanonicalValue("component_local")},
          {"coordinate_kind", CanonicalValue("angular")},
      })};
  if (include_plus_one_role) {
    resource_roles.emplace_back(CanonicalValue::Object{
        {"resource_role_id", CanonicalValue("trigger")},
        {"source_kind", CanonicalValue("component_local")},
        {"coordinate_kind", CanonicalValue("angular")},
    });
  }
  const std::string document = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kComponentTypeApiVersion))},
          {"component_type_id", CanonicalValue("test_axis_component")},
          {"tags", CanonicalValue(CanonicalValue::Array{})},
          {"model_roles", CanonicalValue(CanonicalValue::Array{})},
          {"model_mounts", CanonicalValue(CanonicalValue::Array{})},
          {"command_targets",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"command_target_id", CanonicalValue("axis_target")},
                   {"model_group_roles",
                    CanonicalValue(CanonicalValue::Array{})},
                   {"resource_roles", CanonicalValue(std::move(resource_roles))},
                   {"frame_roles", CanonicalValue(CanonicalValue::Array{})},
                   {"collision_body_roles",
                    CanonicalValue(CanonicalValue::Array{})},
                   {"capabilities",
                    CanonicalValue(std::move(capability_rows))},
               })})},
          {"attachment_interfaces",
           CanonicalValue(CanonicalValue::Array{})},
      }),
      "catalog-record-revision/component_type",
      core::kComponentTypeApiVersion, "component_type_revision");
  const core::ContentAddressedStoreKey key("sha256/test-axis-component");
  return resolve_and_qualify_component_type(
      {core::ComponentTypeId("test_axis_component"),
       core::ComponentTypeRevision(
           revision_from_document(document, "component_type_revision")),
       core::SchemaId(std::string(core::kComponentTypeApiVersion)),
       core::SchemaRevision(std::string(kComponentTypeSchemaRevision)),
       core::ContentDigest(sha256_digest(document)), key},
      key, document, capabilities);
}

[[nodiscard]] CanonicalValue interface_claim(
    std::string interface_name, std::string resource_role = "axis") {
  return CanonicalValue(CanonicalValue::Object{
      {"resource_role_id", CanonicalValue(std::move(resource_role))},
      {"interface_name", CanonicalValue(std::move(interface_name))},
  });
}

[[nodiscard]] QualifiedControlInterfaceContract qualify_contract(
    std::string_view id = "axis_position",
    std::size_t interface_count = 1U,
    bool include_plus_one_role = false,
    bool include_command = true,
    bool include_state = true,
    bool expand_second_role = false,
    bool reverse_role_order = false) {
  CanonicalValue::Array command_interfaces;
  CanonicalValue::Array state_interfaces;
  command_interfaces.reserve(interface_count);
  state_interfaces.reserve(interface_count);
  for (std::size_t index = 0U; index < interface_count; ++index) {
    const std::string interface_name =
        interface_count == 1U ? "position"
                              : "position_" + std::to_string(index);
    if (include_command) {
      command_interfaces.push_back(interface_claim(interface_name));
    }
    if (include_state) {
      state_interfaces.push_back(interface_claim(interface_name));
    }
  }
  std::vector<std::string> ordering{"axis"};
  if (include_plus_one_role) {
    const std::size_t trigger_interface_count =
        expand_second_role ? interface_count : 1U;
    for (std::size_t index = 0U; index < trigger_interface_count; ++index) {
      const std::string interface_name =
          trigger_interface_count == 1U
              ? "trigger"
              : "trigger_" + std::to_string(index);
      if (include_command) {
        command_interfaces.push_back(
            interface_claim(interface_name, "trigger"));
      }
      if (include_state) {
        state_interfaces.push_back(
            interface_claim(interface_name, "trigger"));
      }
    }
    ordering.emplace_back("trigger");
  }
  if (reverse_role_order) {
    std::reverse(ordering.begin(), ordering.end());
  }
  const std::string document = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(
               std::string(core::kControlInterfaceContractApiVersion))},
          {"control_interface_contract_id",
           CanonicalValue(std::string(id))},
          {"command_interfaces",
           CanonicalValue(std::move(command_interfaces))},
          {"state_interfaces",
           CanonicalValue(std::move(state_interfaces))},
          {"ordering",
           string_array(ordering)},
      }),
      "catalog-record-revision/control_interface_contract",
      core::kControlInterfaceContractApiVersion,
      "control_interface_revision");
  const core::ContentAddressedStoreKey key(
      "sha256/test-contract-" + std::string(id));
  return resolve_and_qualify_control_interface_contract(
      {core::ControlInterfaceContractId(std::string(id)),
       core::ControlInterfaceRevision(
           revision_from_document(document, "control_interface_revision")),
       core::SchemaId(
           std::string(core::kControlInterfaceContractApiVersion)),
       core::SchemaRevision(std::string(kControlInterfaceSchemaRevision)),
       core::ContentDigest(sha256_digest(document)), key},
      key, document);
}

[[nodiscard]] CanonicalValue topology_contract_claim(
    const QualifiedControlInterfaceContract& contract) {
  std::vector<std::string> directions;
  if (!contract.value().command_interfaces.empty()) {
    directions.emplace_back("command");
  }
  if (!contract.value().state_interfaces.empty()) {
    directions.emplace_back("state");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"control_interface_contract_id",
       CanonicalValue(
           contract.value().control_interface_contract_id.value())},
      {"control_interface_revision",
       CanonicalValue(contract.value().control_interface_revision.value())},
      {"claimed_directions", string_set(std::move(directions))},
  });
}

[[nodiscard]] QualifiedControllerTopology qualify_topology(
    const QualifiedControlInterfaceContract& contract,
    bool repeated_routes_and_activation_sets,
    bool split_route_activation_sets = false,
    std::string_view topology_id = "axis_topology") {
  CanonicalValue::Array activation_sets{
      CanonicalValue(CanonicalValue::Object{
          {"activation_set_id", CanonicalValue("set_a")},
          {"active_controller_role_ids",
           string_set({"hardware"})},
          {"active_chain_edges", CanonicalValue(CanonicalValue::Array{})},
      })};
  CanonicalValue::Array route_sets{CanonicalValue("set_a")};
  if (repeated_routes_and_activation_sets) {
    activation_sets.emplace_back(CanonicalValue::Object{
        {"activation_set_id", CanonicalValue("set_b")},
        {"active_controller_role_ids",
         string_set({"hardware", "observer"})},
        {"active_chain_edges", CanonicalValue(CanonicalValue::Array{})},
    });
    route_sets.emplace_back("set_b");
  }
  const auto route_row = [&](std::string id) {
    const CanonicalValue compatible_sets =
        split_route_activation_sets
            ? string_set({id == "route_a" ? "set_a" : "set_b"})
            : CanonicalValue(route_sets);
    return CanonicalValue(CanonicalValue::Object{
        {"control_route_id", CanonicalValue(std::move(id))},
        {"control_interface_contract_id",
         CanonicalValue(
             contract.value().control_interface_contract_id.value())},
        {"control_interface_revision",
         CanonicalValue(contract.value().control_interface_revision.value())},
        {"ordered_controller_role_ids",
         CanonicalValue(
             CanonicalValue::Array{CanonicalValue("hardware")})},
        {"compatible_activation_set_ids", compatible_sets},
    });
  };
  CanonicalValue::Array routes{route_row("route_a")};
  if (repeated_routes_and_activation_sets) {
    routes.push_back(route_row("route_b"));
  }
  CanonicalValue::Array controllers{CanonicalValue(
      CanonicalValue::Object{
          {"controller_role_id", CanonicalValue("hardware")},
          {"control_interface_claims",
           CanonicalValue(CanonicalValue::Array{
               topology_contract_claim(contract)})},
          {"exported_reference_interfaces",
           CanonicalValue(CanonicalValue::Array{})},
          {"exported_state_interfaces",
           CanonicalValue(CanonicalValue::Array{})},
      })};
  if (repeated_routes_and_activation_sets) {
    controllers.emplace_back(CanonicalValue::Object{
        {"controller_role_id", CanonicalValue("observer")},
        {"control_interface_claims",
         CanonicalValue(CanonicalValue::Array{CanonicalValue(
             CanonicalValue::Object{
                 {"control_interface_contract_id",
                  CanonicalValue(
                      contract.value()
                          .control_interface_contract_id.value())},
                 {"control_interface_revision",
                  CanonicalValue(
                      contract.value().control_interface_revision.value())},
                 {"claimed_directions", string_set({"state"})},
             })})},
        {"exported_reference_interfaces",
         CanonicalValue(CanonicalValue::Array{})},
        {"exported_state_interfaces",
         CanonicalValue(CanonicalValue::Array{})},
    });
  }
  const std::string document = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kControllerTopologyApiVersion))},
          {"controller_topology_id",
           CanonicalValue(std::string(topology_id))},
          {"controllers", CanonicalValue(std::move(controllers))},
          {"chain_edges", CanonicalValue(CanonicalValue::Array{})},
          {"activation_sets", CanonicalValue(std::move(activation_sets))},
          {"legal_routes", CanonicalValue(std::move(routes))},
      }),
      "catalog-record-revision/controller_topology",
      core::kControllerTopologyApiVersion, "topology_revision");
  const core::ContentAddressedStoreKey key(
      "sha256/test-topology-" + std::string(topology_id));
  return resolve_and_qualify_controller_topology(
      {core::ControllerTopologyId(std::string(topology_id)),
       core::TopologyRevision(
           revision_from_document(document, "topology_revision")),
       core::SchemaId(std::string(core::kControllerTopologyApiVersion)),
       core::SchemaRevision(std::string(kControllerTopologySchemaRevision)),
       core::ContentDigest(sha256_digest(document)), key},
      key, document,
      std::span<const QualifiedControlInterfaceContract>(&contract, 1U));
}

[[nodiscard]] CanonicalValue topology_contract_claim(
    const QualifiedControlInterfaceContract& contract,
    std::vector<std::string> directions) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_interface_contract_id",
       CanonicalValue(
           contract.value().control_interface_contract_id.value())},
      {"control_interface_revision",
       CanonicalValue(contract.value().control_interface_revision.value())},
      {"claimed_directions", string_set(std::move(directions))},
  });
}

[[nodiscard]] CanonicalValue topology_controller(
    std::string role, CanonicalValue::Array claims,
    CanonicalValue::Array exported_reference_interfaces = {},
    CanonicalValue::Array exported_state_interfaces = {}) {
  return CanonicalValue(CanonicalValue::Object{
      {"controller_role_id", CanonicalValue(std::move(role))},
      {"control_interface_claims", CanonicalValue(std::move(claims))},
      {"exported_reference_interfaces",
       CanonicalValue(std::move(exported_reference_interfaces))},
      {"exported_state_interfaces",
       CanonicalValue(std::move(exported_state_interfaces))},
  });
}

[[nodiscard]] CanonicalValue topology_edge_ref(std::string upstream,
                                                std::string downstream) {
  return CanonicalValue(CanonicalValue::Object{
      {"upstream_controller_role_id",
       CanonicalValue(std::move(upstream))},
      {"downstream_controller_role_id",
       CanonicalValue(std::move(downstream))},
  });
}

[[nodiscard]] CanonicalValue topology_edge(
    std::string upstream, std::string downstream,
    CanonicalValue::Array reference_interfaces,
    CanonicalValue::Array state_interfaces) {
  auto fields =
      topology_edge_ref(std::move(upstream), std::move(downstream))
          .as_object();
  fields.emplace_back("reference_interfaces",
                      CanonicalValue(std::move(reference_interfaces)));
  fields.emplace_back("state_interfaces",
                      CanonicalValue(std::move(state_interfaces)));
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] QualifiedControllerTopology qualify_custom_topology(
    CanonicalValue::Array controllers, CanonicalValue::Array chain_edges,
    CanonicalValue::Array activation_sets, CanonicalValue::Array routes,
    const std::vector<QualifiedControlInterfaceContract>& contracts) {
  const std::string document = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kControllerTopologyApiVersion))},
          {"controller_topology_id", CanonicalValue("axis_topology")},
          {"controllers", CanonicalValue(std::move(controllers))},
          {"chain_edges", CanonicalValue(std::move(chain_edges))},
          {"activation_sets", CanonicalValue(std::move(activation_sets))},
          {"legal_routes", CanonicalValue(std::move(routes))},
      }),
      "catalog-record-revision/controller_topology",
      core::kControllerTopologyApiVersion, "topology_revision");
  const core::ContentAddressedStoreKey key(
      "sha256/test-axis-topology-custom");
  return resolve_and_qualify_controller_topology(
      {core::ControllerTopologyId("axis_topology"),
       core::TopologyRevision(
           revision_from_document(document, "topology_revision")),
       core::SchemaId(std::string(core::kControllerTopologyApiVersion)),
       core::SchemaRevision(std::string(kControllerTopologySchemaRevision)),
       core::ContentDigest(sha256_digest(document)), key},
      key, document, contracts);
}

[[nodiscard]] QualifiedControllerTopology qualify_chained_topology(
    const QualifiedControlInterfaceContract& contract) {
  const auto exported_interface = []() {
    return CanonicalValue::Array{
        interface_claim("position"),
    };
  };
  return qualify_custom_topology(
      {
          topology_controller("filter", {}, exported_interface(),
                              exported_interface()),
          topology_controller(
              "hardware",
              {topology_contract_claim(contract, {"command", "state"})},
              exported_interface(), exported_interface()),
          topology_controller("planner", {}),
      },
      {
          topology_edge("filter", "hardware", exported_interface(),
                        exported_interface()),
          topology_edge("planner", "filter", exported_interface(),
                        exported_interface()),
      },
      {CanonicalValue(CanonicalValue::Object{
          {"activation_set_id", CanonicalValue("set_a")},
          {"active_controller_role_ids",
           string_set({"filter", "hardware", "planner"})},
          {"active_chain_edges",
           CanonicalValue(CanonicalValue::Array{
               topology_edge_ref("filter", "hardware"),
               topology_edge_ref("planner", "filter"),
           })},
      })},
      {CanonicalValue(CanonicalValue::Object{
          {"control_route_id", CanonicalValue("route_a")},
          {"control_interface_contract_id",
           CanonicalValue(
               contract.value().control_interface_contract_id.value())},
          {"control_interface_revision",
           CanonicalValue(
               contract.value().control_interface_revision.value())},
          {"ordered_controller_role_ids",
           string_array({"planner", "filter", "hardware"})},
          {"compatible_activation_set_ids", string_set({"set_a"})},
      })},
      {contract});
}

[[nodiscard]] QualifiedControllerTopology
qualify_topology_with_directional_declarations(
    const QualifiedControlInterfaceContract& routed_contract,
    const QualifiedControlInterfaceContract& command_only_contract,
    const QualifiedControlInterfaceContract& state_only_contract) {
  return qualify_custom_topology(
      {
          topology_controller(
              "command_aux",
              {topology_contract_claim(command_only_contract, {"command"})}),
          topology_controller(
              "hardware",
              {topology_contract_claim(routed_contract,
                                       {"command", "state"})}),
          topology_controller(
              "state_aux",
              {topology_contract_claim(state_only_contract, {"state"})}),
      },
      {},
      {CanonicalValue(CanonicalValue::Object{
          {"activation_set_id", CanonicalValue("set_a")},
          {"active_controller_role_ids",
           string_set({"command_aux", "hardware", "state_aux"})},
          {"active_chain_edges", CanonicalValue(CanonicalValue::Array{})},
      })},
      {CanonicalValue(CanonicalValue::Object{
          {"control_route_id", CanonicalValue("route_a")},
          {"control_interface_contract_id",
           CanonicalValue(routed_contract.value()
                              .control_interface_contract_id.value())},
          {"control_interface_revision",
           CanonicalValue(
               routed_contract.value().control_interface_revision.value())},
          {"ordered_controller_role_ids", string_array({"hardware"})},
          {"compatible_activation_set_ids", string_set({"set_a"})},
      })},
      {routed_contract, command_only_contract, state_only_contract});
}

enum class StopForm : std::uint8_t {
  kNotStoppable,
  kIntrinsic,
  kProviderDefined,
  kBindingStateAuthority,
};

enum class ProviderStopEndpointCase : std::uint8_t {
  kExact,
  kCommandZeroIntersection,
  kCommandAmbiguousIntersection,
  kEvidenceWrongSide,
  kEvidenceOutsideSupport,
};

enum class ProviderStopCompatibilityCase : std::uint8_t {
  kExact,
  kEndpointless,
  kEndpointlessForbidden,
  kCommandKindMismatch,
  kCommandRegisteredKindMismatch,
  kEvidenceKindMismatch,
  kEvidenceRegisteredKindMismatch,
};

enum class ControlProviderOfferCase : std::uint8_t {
  kExact,
  kMissingCapability,
  kMissingRole,
  kExtraRole,
  kDualSideRole,
};

enum class BindingStateStopCase : std::uint8_t {
  kExact,
  kDisallowedAuthorityClass,
  kCommandSideEndpoint,
};

[[nodiscard]] std::vector<std::string> provider_stop_command_roles(
    ProviderStopEndpointCase endpoint_case,
    ProviderStopCompatibilityCase compatibility_case) {
  if (compatibility_case ==
      ProviderStopCompatibilityCase::kEndpointless) {
    return {};
  }
  if (endpoint_case ==
      ProviderStopEndpointCase::kCommandAmbiguousIntersection) {
    return {"stop_command", "stop_command_alt"};
  }
  return {"stop_command"};
}

[[nodiscard]] std::vector<std::string> provider_stop_evidence_roles(
    ProviderStopEndpointCase endpoint_case,
    ProviderStopCompatibilityCase compatibility_case) {
  if (compatibility_case ==
      ProviderStopCompatibilityCase::kEndpointlessForbidden) {
    return {};
  }
  if (endpoint_case ==
      ProviderStopEndpointCase::kEvidenceOutsideSupport) {
    return {"stop_evidence_alt"};
  }
  return {"stop_evidence"};
}

[[nodiscard]] std::vector<std::string> provider_stop_offer_roles(
    ProviderStopEndpointCase endpoint_case,
    ProviderStopCompatibilityCase compatibility_case) {
  std::vector<std::string> roles{"command", "state", "stop_command",
                                 "stop_evidence"};
  if (compatibility_case ==
      ProviderStopCompatibilityCase::kEndpointless) {
    return {"command", "state", "stop_evidence"};
  }
  if (endpoint_case ==
      ProviderStopEndpointCase::kCommandAmbiguousIntersection) {
    roles.emplace_back("stop_command_alt");
  }
  if (endpoint_case ==
      ProviderStopEndpointCase::kEvidenceOutsideSupport) {
    roles.emplace_back("stop_evidence_alt");
  }
  return roles;
}

[[nodiscard]] CanonicalValue provider_contract_ref() {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id", CanonicalValue("stop_contract")},
      {"contract_schema_id",
       CanonicalValue(std::string(core::kMotionControlledStopContractSchemaId))},
      {"contract_schema_revision",
       CanonicalValue(
           std::string(core::kMotionControlledStopContractSchemaRevision))},
  });
}

[[nodiscard]] CanonicalValue software_hard_contract_ref() {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id", CanonicalValue("software_hard_stop")},
      {"contract_schema_id",
       CanonicalValue("manyforge.value_schema.provider_contract.empty.v1")},
      {"contract_schema_revision",
       CanonicalValue(std::string(kEmptyProviderContractSchemaRevision))},
  });
}

[[nodiscard]] CanonicalValue safety_request_contract_ref() {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id", CanonicalValue("safety_stop_request")},
      {"contract_schema_id",
       CanonicalValue("manyforge.value_schema.provider_contract.empty.v1")},
      {"contract_schema_revision",
       CanonicalValue(std::string(kEmptyProviderContractSchemaRevision))},
  });
}

[[nodiscard]] QualifiedProviderManifest qualify_manifest(
    std::string_view manifest_id,
    const std::vector<QualifiedCapabilityDefinition>& capabilities,
    StopForm stop_form,
    bool include_provider_capability,
    ProviderStopEndpointCase endpoint_case =
        ProviderStopEndpointCase::kExact,
    ProviderStopCompatibilityCase compatibility_case =
        ProviderStopCompatibilityCase::kExact,
    ControlProviderOfferCase control_offer_case =
        ControlProviderOfferCase::kExact,
    std::string_view selected_control_capability_id = {},
    bool include_safety_stage = false) {
  const bool provider_defined_stop =
      stop_form == StopForm::kProviderDefined;
  const bool binding_state_stop =
      stop_form == StopForm::kBindingStateAuthority;
  std::vector<const QualifiedCapabilityDefinition*> ordered_capabilities;
  ordered_capabilities.reserve(capabilities.size());
  for (const auto& capability : capabilities) {
    ordered_capabilities.push_back(&capability);
  }
  std::sort(
      ordered_capabilities.begin(), ordered_capabilities.end(),
      [](const auto* left, const auto* right) {
        return left->value().capability_id <
               right->value().capability_id;
      });
  CanonicalValue::Array offers;
  for (const auto* capability : ordered_capabilities) {
    const bool is_provider =
        capability->value().provider_binding_kind ==
        core::CapabilityProviderBindingKind::kCapabilityProviderBinding;
    if (is_provider && !include_provider_capability) {
      continue;
    }
    if (!is_provider &&
        control_offer_case ==
            ControlProviderOfferCase::kMissingCapability &&
        capability->value().capability_id.value() ==
            selected_control_capability_id) {
      continue;
    }
    std::vector<std::string> roles =
        is_provider ? std::vector<std::string>{"plan_external", "plan_runtime"}
                    : std::vector<std::string>{"command", "state"};
    if (!is_provider && provider_defined_stop) {
      roles =
          provider_stop_offer_roles(endpoint_case, compatibility_case);
    } else if (!is_provider) {
      if (control_offer_case ==
          ControlProviderOfferCase::kMissingRole) {
        roles = {"command"};
      } else if (
          control_offer_case ==
          ControlProviderOfferCase::kExtraRole) {
        roles.emplace_back("unexpected");
      } else if (
          control_offer_case ==
          ControlProviderOfferCase::kDualSideRole) {
        roles.emplace_back("shared");
      }
    }
    offers.emplace_back(CanonicalValue::Object{
        {"capability_id",
         CanonicalValue(capability->value().capability_id.value())},
        {"capability_definition_revision",
         CanonicalValue(
             capability->value().capability_definition_revision.value())},
        {"required_endpoint_role_ids", string_set(std::move(roles))},
    });
  }
  CanonicalValue::Array contracts;
  if (provider_defined_stop || binding_state_stop) {
    CanonicalValue::Object contract{
        {"provider_contract_ref", provider_contract_ref()},
    };
    if (provider_defined_stop) {
      std::string command_kind{"motion_controlled_stop"};
      std::optional<std::string> registered_command_kind;
      if (compatibility_case ==
          ProviderStopCompatibilityCase::kCommandKindMismatch) {
        command_kind = "safe_output_command";
      } else if (
          compatibility_case ==
          ProviderStopCompatibilityCase::kCommandRegisteredKindMismatch) {
        command_kind = "registered";
        registered_command_kind = "manifest_stop_command";
      }
      CanonicalValue::Object command_support{
          {"command_kind", CanonicalValue(std::move(command_kind))},
          {"command_endpoint_role_ids",
           string_set(provider_stop_command_roles(
               endpoint_case, compatibility_case))},
      };
      if (registered_command_kind.has_value()) {
        command_support.emplace_back(
            "registered_command_kind_id",
            CanonicalValue(std::move(*registered_command_kind)));
      }
      contract.emplace_back(
          "stop_command_support",
          CanonicalValue(std::move(command_support)));
    }
    std::string evidence_kind{
        binding_state_stop ? "motion_rest" : "io_safe_output"};
    std::optional<std::string> registered_evidence_kind;
    if (provider_defined_stop &&
        compatibility_case ==
            ProviderStopCompatibilityCase::kEvidenceKindMismatch) {
      evidence_kind = "motion_rest";
    } else if (
        provider_defined_stop &&
        compatibility_case ==
            ProviderStopCompatibilityCase::kEvidenceRegisteredKindMismatch) {
      evidence_kind = "registered";
      registered_evidence_kind = "manifest_stop_evidence";
    }
    CanonicalValue::Object evidence_support{
        {"evidence_kind", CanonicalValue(std::move(evidence_kind))},
        {"evidence_endpoint_role_ids",
         binding_state_stop
             ? string_set({"state"})
             : string_set(provider_stop_evidence_roles(
                   endpoint_case, compatibility_case))},
    };
    if (registered_evidence_kind.has_value()) {
      evidence_support.emplace_back(
          "registered_evidence_kind_id",
          CanonicalValue(std::move(*registered_evidence_kind)));
    }
    contract.emplace_back(
        "stop_evidence_support",
        CanonicalValue(std::move(evidence_support)));
    contracts.emplace_back(std::move(contract));
  }
  if (stop_form != StopForm::kNotStoppable) {
    contracts.emplace_back(CanonicalValue::Object{
        {"provider_contract_ref", software_hard_contract_ref()},
        {"stop_command_support",
         CanonicalValue(CanonicalValue::Object{
             {"command_kind", CanonicalValue("registered")},
             {"registered_command_kind_id",
              CanonicalValue("manyforge.stop.software_hard")},
             {"command_endpoint_role_ids",
              CanonicalValue(CanonicalValue::Array{})},
         })},
        {"stop_evidence_support",
         CanonicalValue(CanonicalValue::Object{
             {"evidence_kind", CanonicalValue("motion_rest")},
             {"evidence_endpoint_role_ids", string_set({"state"})},
         })},
    });
  }
  if (include_safety_stage) {
    contracts.emplace_back(CanonicalValue::Object{
        {"provider_contract_ref", safety_request_contract_ref()},
        {"stop_command_support",
         CanonicalValue(CanonicalValue::Object{
             {"command_kind", CanonicalValue("registered")},
             {"registered_command_kind_id",
              CanonicalValue("manyforge.stop.safety.protective_request")},
             {"command_endpoint_role_ids", string_set({"stop_command"})},
         })},
    });
  }
  std::sort(contracts.begin(), contracts.end(), [](const auto& left,
                                                   const auto& right) {
    return canonical_json(left) < canonical_json(right);
  });
  const std::string document = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kProviderManifestApiVersion))},
          {"provider_manifest_id",
           CanonicalValue(std::string(manifest_id))},
          {"offered_capabilities", CanonicalValue(std::move(offers))},
          {"provider_contracts", CanonicalValue(std::move(contracts))},
      }),
      "catalog-record-revision/provider_manifest",
      core::kProviderManifestApiVersion, "provider_manifest_revision");
  const core::ContentAddressedStoreKey key(
      "sha256/test-manifest-" + std::string(manifest_id));
  return resolve_and_qualify_provider_manifest(
      {core::ProviderManifestId(std::string(manifest_id)),
       core::ProviderManifestRevision(
           revision_from_document(document, "provider_manifest_revision")),
       core::SchemaId(std::string(core::kProviderManifestApiVersion)),
       core::SchemaRevision(std::string(kProviderManifestSchemaRevision)),
       core::ContentDigest(sha256_digest(document)), key},
      key, document);
}

[[nodiscard]] CanonicalValue stop_command(
    StopForm form,
    ProviderStopCompatibilityCase compatibility_case) {
  std::string command_kind =
      form == StopForm::kProviderDefined ? "motion_controlled_stop"
                                         : "controller_goal_cancel";
  if (form == StopForm::kProviderDefined &&
      compatibility_case ==
          ProviderStopCompatibilityCase::kCommandRegisteredKindMismatch) {
    command_kind = "registered";
  }
  CanonicalValue::Object value{
      {"command_kind", CanonicalValue(command_kind)},
  };
  if (form == StopForm::kProviderDefined) {
    value.emplace_back("provider_contract_ref", provider_contract_ref());
  }
  if (command_kind == "registered") {
    value.emplace_back("registered_command_kind_id",
                       CanonicalValue("policy_stop_command"));
  }
  return CanonicalValue(std::move(value));
}

[[nodiscard]] CanonicalValue stop_evidence(
    StopForm form,
    ProviderStopCompatibilityCase compatibility_case) {
  const bool binding_state =
      form == StopForm::kBindingStateAuthority;
  std::string evidence_kind =
      binding_state
          ? "motion_rest"
          : (form == StopForm::kProviderDefined
                 ? "io_safe_output"
                 : "controller_goal_terminal");
  if (form == StopForm::kProviderDefined &&
      compatibility_case ==
          ProviderStopCompatibilityCase::kEvidenceRegisteredKindMismatch) {
    evidence_kind = "registered";
  }
  CanonicalValue::Object value{
      {"evidence_kind", CanonicalValue(evidence_kind)},
  };
  if (form == StopForm::kProviderDefined || binding_state) {
    value.emplace_back("provider_contract_ref", provider_contract_ref());
  }
  if (evidence_kind == "registered") {
    value.emplace_back("registered_evidence_kind_id",
                       CanonicalValue("policy_stop_evidence"));
  }
  return CanonicalValue(std::move(value));
}

[[nodiscard]] QualifiedStopEvidencePolicy qualify_stop_policy(
    StopForm form, std::string_view containment_granularity = "resource",
    BindingStateStopCase binding_state_case =
        BindingStateStopCase::kExact,
    ProviderStopCompatibilityCase compatibility_case =
        ProviderStopCompatibilityCase::kExact,
    bool include_safety_stage = false,
    bool safety_stage_nonfinal = false,
    bool omit_software_hard_floor = false) {
  CanonicalValue identity = [&]() {
    if (form == StopForm::kNotStoppable) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kStopEvidencePolicyApiVersion))},
          {"stop_evidence_policy_id",
           CanonicalValue("test_not_stoppable")},
          {"disposition", CanonicalValue("not_stoppable")},
          {"containment_granularity",
           CanonicalValue(std::string(containment_granularity))},
      });
    }
    const bool binding_state =
        form == StopForm::kBindingStateAuthority;
    CanonicalValue::Object source{
        {"source_kind",
         CanonicalValue(
             binding_state
                 ? "binding_state_authority"
                 : (form == StopForm::kProviderDefined
                        ? "provider_evidence"
                        : "controller_goal_status"))},
        {"freshness_budget_ns", CanonicalValue(25.0)},
    };
    if (form == StopForm::kProviderDefined) {
      source.emplace_back("evidence_endpoint_role_id",
                          CanonicalValue("stop_evidence"));
    } else if (binding_state) {
      source.emplace_back("state_endpoint_role_id",
                          CanonicalValue(
                              binding_state_case ==
                                      BindingStateStopCase::kCommandSideEndpoint
                                  ? "command"
                                  : "state"));
      source.emplace_back("allowed_authority_classes",
                          string_set(
                              {binding_state_case ==
                                       BindingStateStopCase::
                                           kDisallowedAuthorityClass
                                   ? "commanded"
                                   : "measured"}));
    }
    CanonicalValue::Object observation{
        {"observation_kind",
         CanonicalValue(
             binding_state
                 ? "post_command_dwell"
                 : (form == StopForm::kProviderDefined
                        ? "post_command_sample"
                        : "command_result"))},
    };
    if (binding_state) {
      observation.emplace_back("minimum_dwell_ns", CanonicalValue(10.0));
    }
    const bool distinct_nominal_evidence_phase =
        form == StopForm::kProviderDefined || binding_state;
    const bool requires_hard_floor =
        form != StopForm::kNotStoppable && !omit_software_hard_floor;
    const std::string nominal_command_phase =
        distinct_nominal_evidence_phase ? "nominal_command" : "command";
    const std::string nominal_evidence_phase =
        distinct_nominal_evidence_phase ? "nominal_terminal" : "command";
    CanonicalValue::Array phases{
        CanonicalValue(CanonicalValue::Object{
            {"phase_id", CanonicalValue(nominal_command_phase)},
            {"deadline_offset_ns", CanonicalValue(100.0)},
        })};
    if (distinct_nominal_evidence_phase) {
      phases.emplace_back(CanonicalValue::Object{
          {"phase_id", CanonicalValue(nominal_evidence_phase)},
          {"deadline_offset_ns", CanonicalValue(300.0)},
      });
    }
    if (requires_hard_floor) {
      phases.emplace_back(CanonicalValue::Object{
          {"phase_id", CanonicalValue("hard_command")},
          {"deadline_offset_ns", CanonicalValue(500.0)},
      });
      phases.emplace_back(CanonicalValue::Object{
          {"phase_id", CanonicalValue("hard_terminal")},
          {"deadline_offset_ns", CanonicalValue(800.0)},
      });
    }
    if (include_safety_stage) {
      if (!requires_hard_floor) {
        throw std::logic_error(
            "safety-stage fixture requires the complete motion Stop chain");
      }
      phases.emplace_back(CanonicalValue::Object{
          {"phase_id", CanonicalValue("safety_command")},
          {"deadline_offset_ns", CanonicalValue(1000.0)},
      });
      phases.emplace_back(CanonicalValue::Object{
          {"phase_id", CanonicalValue("safety_terminal")},
          {"deadline_offset_ns", CanonicalValue(1200.0)},
      });
    }
    CanonicalValue::Array stages;
    stages.emplace_back(CanonicalValue::Object{
        {"stop_escalation_stage_id", CanonicalValue("nominal")},
        {"stage_kind", CanonicalValue("nominal")},
        {"containment_granularity",
         CanonicalValue(std::string(containment_granularity))},
        {"containment_command",
         CanonicalValue(CanonicalValue::Object{
             {"command_phase_id", CanonicalValue(nominal_command_phase)},
             {"command", stop_command(form, compatibility_case)},
         })},
        {"evidence_conjunction", CanonicalValue("all_of")},
        {"evidence_requirements",
         CanonicalValue(CanonicalValue::Array{CanonicalValue(
             CanonicalValue::Object{
                 {"evidence_item_id", CanonicalValue("nominal_evidence")},
                 {"phase_id", CanonicalValue(nominal_evidence_phase)},
                 {"evidence", stop_evidence(form, compatibility_case)},
                 {"source_requirement", CanonicalValue(std::move(source))},
                 {"observation_semantics",
                  CanonicalValue(std::move(observation))},
             })})},
    });
    if (requires_hard_floor) {
      stages.emplace_back(CanonicalValue::Object{
          {"stop_escalation_stage_id", CanonicalValue("software_hard")},
          {"stage_kind", CanonicalValue("software_hard")},
          {"containment_granularity",
           CanonicalValue(std::string(containment_granularity))},
          {"containment_command",
           CanonicalValue(CanonicalValue::Object{
               {"command_phase_id", CanonicalValue("hard_command")},
               {"command",
                CanonicalValue(CanonicalValue::Object{
                    {"command_kind", CanonicalValue("registered")},
                    {"provider_contract_ref", software_hard_contract_ref()},
                    {"registered_command_kind_id",
                     CanonicalValue("manyforge.stop.software_hard")},
                })},
           })},
          {"evidence_conjunction", CanonicalValue("all_of")},
          {"evidence_requirements",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"evidence_item_id", CanonicalValue("hard_rest")},
                   {"phase_id", CanonicalValue("hard_terminal")},
                   {"evidence",
                    CanonicalValue(CanonicalValue::Object{
                        {"evidence_kind", CanonicalValue("motion_rest")},
                        {"provider_contract_ref",
                         software_hard_contract_ref()},
                    })},
                   {"source_requirement",
                    CanonicalValue(CanonicalValue::Object{
                        {"source_kind",
                         CanonicalValue("binding_state_authority")},
                        {"freshness_budget_ns", CanonicalValue(25.0)},
                        {"state_endpoint_role_id", CanonicalValue("state")},
                        {"allowed_authority_classes",
                         string_set({"measured"})},
                    })},
                   {"observation_semantics",
                    CanonicalValue(CanonicalValue::Object{
                        {"observation_kind",
                         CanonicalValue("post_command_dwell")},
                        {"minimum_dwell_ns", CanonicalValue(10.0)},
                    })},
               })})},
      });
    }
    if (include_safety_stage) {
      stages.emplace_back(CanonicalValue::Object{
          {"stop_escalation_stage_id", CanonicalValue("safety_request")},
          {"stage_kind", CanonicalValue("safety_system_request")},
          {"containment_granularity",
           CanonicalValue(std::string(containment_granularity))},
          {"containment_command",
           CanonicalValue(CanonicalValue::Object{
               {"command_phase_id", CanonicalValue("safety_command")},
               {"command",
                CanonicalValue(CanonicalValue::Object{
                    {"command_kind", CanonicalValue("registered")},
                    {"provider_contract_ref", safety_request_contract_ref()},
                    {"registered_command_kind_id",
                     CanonicalValue(
                         "manyforge.stop.safety.protective_request")},
                })},
           })},
          {"evidence_conjunction", CanonicalValue("all_of")},
          {"evidence_requirements",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"evidence_item_id", CanonicalValue("safety_ack")},
                   {"phase_id", CanonicalValue("safety_terminal")},
                   {"evidence",
                    CanonicalValue(CanonicalValue::Object{
                        {"evidence_kind",
                         CanonicalValue("discrete_acknowledgement")},
                    })},
                   {"source_requirement",
                    CanonicalValue(CanonicalValue::Object{
                        {"source_kind",
                         CanonicalValue("command_target_status")},
                        {"freshness_budget_ns", CanonicalValue(25.0)},
                    })},
                   {"observation_semantics",
                    CanonicalValue(CanonicalValue::Object{
                        {"observation_kind",
                         CanonicalValue("post_command_sample")},
                    })},
               })})},
      });
    }
    if (safety_stage_nonfinal) {
      if (stages.size() < 3U) {
        throw std::logic_error(
            "non-final safety-stage fixture lacks a complete Stop chain");
      }
      std::swap(stages[1], stages[2]);
    }
    return CanonicalValue(CanonicalValue::Object{
        {"api_version",
         CanonicalValue(std::string(core::kStopEvidencePolicyApiVersion))},
        {"stop_evidence_policy_id",
         CanonicalValue(
             binding_state
                 ? "test_binding_state_stop"
                 : (form == StopForm::kProviderDefined
                        ? "test_provider_stop"
                        : "test_intrinsic_stop"))},
        {"disposition", CanonicalValue("stoppable")},
        {"containment_granularity",
         CanonicalValue(std::string(containment_granularity))},
        {"deadline_budget",
         CanonicalValue(CanonicalValue::Object{
             {"outer_duration_ns",
              CanonicalValue(include_safety_stage ? 1400.0 : 1000.0)},
             {"kernel_completion_reserve_ns", CanonicalValue(100.0)},
             {"phases", CanonicalValue(std::move(phases))},
         })},
        {"escalation_stages", CanonicalValue(std::move(stages))},
    });
  }();
  const std::string policy_id =
      form == StopForm::kNotStoppable
          ? "test_not_stoppable"
          : (form == StopForm::kBindingStateAuthority
                 ? "test_binding_state_stop"
                 : (form == StopForm::kProviderDefined
                        ? "test_provider_stop"
                        : "test_intrinsic_stop"));
  const std::string document = revisioned_document(
      std::move(identity),
      "catalog-record-revision/stop_evidence_policy",
      core::kStopEvidencePolicyApiVersion,
      "stop_evidence_policy_revision");
  const core::ContentAddressedStoreKey key(
      "sha256/test-stop-" + policy_id);
  return resolve_and_qualify_stop_evidence_policy(
      {core::StopEvidencePolicyId(policy_id),
       core::StopEvidencePolicyRevision(
           revision_from_document(document,
                                  "stop_evidence_policy_revision")),
       core::SchemaId(std::string(core::kStopEvidencePolicyApiVersion)),
       core::SchemaRevision(std::string(kStopEvidencePolicySchemaRevision)),
       core::ContentDigest(sha256_digest(document)), key},
      key, document);
}

[[nodiscard]] QualifiedRuntimeProfile qualify_runtime_profile(
    std::string_view profile_id = "planner_runtime_profile",
    std::string_view status_role = "runtime_status") {
  const std::string document = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kRuntimeProfileApiVersion))},
          {"runtime_profile_id",
           CanonicalValue(std::string(profile_id))},
          {"configuration_schema_ref",
           CanonicalValue(CanonicalValue::Object{
               {"schema_id",
                CanonicalValue(
                    "manyforge.value_schema.runtime.empty.v1")},
               {"schema_revision",
                CanonicalValue(
                    std::string(kEmptyRuntimeConfigurationSchemaRevision))},
           })},
          {"process_contract",
           CanonicalValue(CanonicalValue::Object{
               {"lifecycle_control", CanonicalValue("manager_managed")},
           })},
          {"supervision_contract",
           CanonicalValue(CanonicalValue::Object{
               {"status_endpoint_role_id",
                CanonicalValue(std::string(status_role))},
           })},
          {"offered_runtime_role_ids", string_set({"planner"})},
          {"source_endpoint_role_ids",
           string_set({"plan_runtime", std::string(status_role)})},
      }),
      "catalog-record-revision/runtime_profile",
      core::kRuntimeProfileApiVersion, "runtime_profile_revision");
  const core::ContentAddressedStoreKey key(
      "sha256/test-runtime-profile-" + std::string(profile_id) + "-" +
      std::string(status_role));
  return resolve_and_qualify_runtime_profile(
      {core::RuntimeProfileId(std::string(profile_id)),
       core::RuntimeProfileRevision(
           revision_from_document(document, "runtime_profile_revision")),
       core::SchemaId(std::string(core::kRuntimeProfileApiVersion)),
       core::SchemaRevision(std::string(kRuntimeProfileSchemaRevision)),
       core::ContentDigest(sha256_digest(document)), key},
      key, document);
}

enum class ResourceBindingCase : std::uint8_t {
  kExact,
  kMisorderedRoleGroups,
  kReopenedRoleGroup,
};

enum class ControlEndpointProjectionCase : std::uint8_t {
  kExact,
  kMissingRole,
  kExtraRole,
  kDualSideRole,
};

enum class StopParameterCase : std::uint8_t {
  kExact,
  kMissingHardStage,
  kUnusedStage,
  kStalePolicyRevision,
  kMissingResource,
  kWrongCoordinateKind,
};

enum class CommissionedSafetyCase : std::uint8_t {
  kNone,
  kExact,
  kMissingRow,
  kMissingProof,
  kUnusedRow,
  kWrongRoute,
  kWrongActionClass,
  kUnknownExternalResource,
  kNonFinalStage,
};

inline constexpr std::string_view kCommissioningEvidenceBytes =
    R"({"api_version":"manyforge.stop_interface_commissioning_evidence.v1","site":"test"})";

struct GraphOptions final {
  std::size_t binding_count = 1U;
  std::size_t resource_count = 1U;
  bool include_plus_one_role = false;
  bool expand_second_role = false;
  bool repeated_routes_and_activation_sets = false;
  bool include_runtime = false;
  bool reverse_source_order = false;
  bool stale_control_capability_revision = false;
  ProviderStopEndpointCase provider_stop_endpoint_case =
      ProviderStopEndpointCase::kExact;
  ProviderStopCompatibilityCase provider_stop_compatibility_case =
      ProviderStopCompatibilityCase::kExact;
  ControlEndpointProjectionCase control_endpoint_projection_case =
      ControlEndpointProjectionCase::kExact;
  ResourceBindingCase resource_binding_case =
      ResourceBindingCase::kExact;
  std::string binding_containment_granularity = "resource";
  std::string domain_containment_granularity = "resource";
  std::string state_authority_class = "measured";
  StopParameterCase stop_parameter_case = StopParameterCase::kExact;
};

[[nodiscard]] GraphRevisionVerifiedCellGraph make_graph(
    const GraphOptions& options, const QualifiedComponentType& type,
    const QualifiedCapabilityDefinition& control_capability,
    const QualifiedControlInterfaceContract& contract,
    const QualifiedControllerTopology& topology,
    const QualifiedProviderManifest& manifest,
    const QualifiedStopEvidencePolicy& stop_policy,
    const QualifiedRuntimeProfile* runtime_profile) {
  CanonicalValue::Array runtime_units;
  if (options.include_runtime) {
    if (runtime_profile == nullptr) {
      throw std::runtime_error("runtime fixture omitted its profile");
    }
    runtime_units.emplace_back(CanonicalValue::Object{
        {"runtime_unit_id", CanonicalValue("planner_runtime")},
        {"runtime_profile_id",
         CanonicalValue(runtime_profile->value().runtime_profile_id.value())},
        {"runtime_profile_revision",
         CanonicalValue(
             runtime_profile->value().runtime_profile_revision.value())},
        {"dependency_runtime_unit_ids",
         CanonicalValue(CanonicalValue::Array{})},
        {"configuration", CanonicalValue(CanonicalValue::Object{})},
    });
  }

  CanonicalValue::Array components;
  CanonicalValue::Array bindings;
  std::vector<std::size_t> indices(options.binding_count);
  for (std::size_t index = 0U; index < indices.size(); ++index) {
    indices[index] = index;
  }
  if (options.reverse_source_order) {
    std::reverse(indices.begin(), indices.end());
  }
  const std::vector<std::string> routes =
      options.repeated_routes_and_activation_sets
          ? std::vector<std::string>{"route_b", "route_a"}
          : std::vector<std::string>{"route_a"};
  for (const std::size_t index : indices) {
    const std::string suffix(1U, static_cast<char>('a' + index));
    const std::string component_id = "component_" + suffix;
    const std::string binding_id = "binding_" + suffix;
    CanonicalValue::Array resource_bindings;
    resource_bindings.reserve(
        options.resource_count *
        (options.include_plus_one_role && options.expand_second_role ? 2U
                                                                     : 1U));
    for (std::size_t resource_index = 0U;
         resource_index < options.resource_count; ++resource_index) {
      resource_bindings.emplace_back(CanonicalValue::Object{
          {"resource_role_id", CanonicalValue("axis")},
          {"resource_id",
           CanonicalValue(
               options.resource_count == 1U
                   ? "axis_" + suffix
                   : "axis_" + suffix + "_" +
                         std::to_string(resource_index))},
      });
    }
    if (options.include_plus_one_role) {
      const std::size_t trigger_resource_count =
          options.expand_second_role ? options.resource_count : 1U;
      for (std::size_t resource_index = 0U;
           resource_index < trigger_resource_count; ++resource_index) {
        resource_bindings.emplace_back(CanonicalValue::Object{
            {"resource_role_id", CanonicalValue("trigger")},
            {"resource_id",
             CanonicalValue(
                 trigger_resource_count == 1U
                     ? "trigger_" + suffix
                     : "trigger_" + suffix + "_" +
                           std::to_string(resource_index))},
        });
      }
      if (options.resource_binding_case ==
          ResourceBindingCase::kMisorderedRoleGroups) {
        std::rotate(
            resource_bindings.begin(),
            resource_bindings.begin() +
                static_cast<std::ptrdiff_t>(options.resource_count),
            resource_bindings.end());
      } else if (
          options.resource_binding_case ==
              ResourceBindingCase::kReopenedRoleGroup &&
          options.resource_count >= 2U) {
        std::rotate(
            resource_bindings.begin() + 1,
            resource_bindings.begin() +
                static_cast<std::ptrdiff_t>(options.resource_count),
            resource_bindings.begin() +
                static_cast<std::ptrdiff_t>(options.resource_count + 1U));
      }
    }
    components.emplace_back(CanonicalValue::Object{
        {"controllable_component_id", CanonicalValue(component_id)},
        {"component_type_id",
         CanonicalValue(type.value().component_type_id.value())},
        {"component_type_revision",
         CanonicalValue(type.value().component_type_revision.value())},
        {"model_role_bindings", CanonicalValue(CanonicalValue::Array{})},
        {"runtime_unit_ids",
         options.include_runtime
             ? string_set({"planner_runtime"})
             : CanonicalValue(CanonicalValue::Array{})},
    });
    CanonicalValue::Array endpoint_projections;
    for (const auto& route : routes) {
      std::vector<std::string> command_roles{"command"};
      std::vector<std::string> state_roles{"state"};
      if (options.control_endpoint_projection_case ==
          ControlEndpointProjectionCase::kMissingRole) {
        state_roles.clear();
      } else if (
          options.control_endpoint_projection_case ==
          ControlEndpointProjectionCase::kExtraRole) {
        command_roles.emplace_back("unexpected");
      } else if (
          options.control_endpoint_projection_case ==
          ControlEndpointProjectionCase::kDualSideRole) {
        command_roles.emplace_back("shared");
        state_roles.emplace_back("shared");
      }
      if (stop_policy.value().stop_evidence_policy_id.value() ==
          "test_provider_stop") {
        switch (options.provider_stop_endpoint_case) {
        case ProviderStopEndpointCase::kExact:
          if (options.provider_stop_compatibility_case !=
              ProviderStopCompatibilityCase::kEndpointless) {
            command_roles.push_back("stop_command");
          }
          state_roles.push_back("stop_evidence");
          break;
        case ProviderStopEndpointCase::kCommandZeroIntersection:
          state_roles.push_back("stop_command");
          state_roles.push_back("stop_evidence");
          break;
        case ProviderStopEndpointCase::kCommandAmbiguousIntersection:
          command_roles.push_back("stop_command");
          command_roles.push_back("stop_command_alt");
          state_roles.push_back("stop_evidence");
          break;
        case ProviderStopEndpointCase::kEvidenceWrongSide:
          command_roles.push_back("stop_command");
          command_roles.push_back("stop_evidence");
          break;
        case ProviderStopEndpointCase::kEvidenceOutsideSupport:
          command_roles.push_back("stop_command");
          state_roles.push_back("stop_evidence");
          state_roles.push_back("stop_evidence_alt");
          break;
        }
      }
      endpoint_projections.emplace_back(CanonicalValue::Object{
          {"control_route_id", CanonicalValue(route)},
          {"command_endpoint_role_ids",
           string_set(std::move(command_roles))},
          {"state_endpoint_role_ids", string_set(std::move(state_roles))},
      });
    }
    bindings.emplace_back(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(binding_id)},
        {"control_binding_revision",
         CanonicalValue(std::string(kZeroDigest))},
        {"controllable_component_id", CanonicalValue(component_id)},
        {"command_target_id", CanonicalValue("axis_target")},
        {"capability_id",
         CanonicalValue(control_capability.value().capability_id.value())},
        {"capability_definition_revision",
         CanonicalValue(
             options.stale_control_capability_revision
                 ? std::string(kZeroDigest)
                 : control_capability.value()
                       .capability_definition_revision.value())},
        {"control_domain_id", CanonicalValue("axis_domain")},
        {"resource_bindings", CanonicalValue(std::move(resource_bindings))},
        {"control_interface_contract_id",
         CanonicalValue(
             contract.value().control_interface_contract_id.value())},
        {"control_interface_revision",
         CanonicalValue(contract.value().control_interface_revision.value())},
        {"controller_topology_id",
         CanonicalValue(topology.value().controller_topology_id.value())},
        {"topology_revision",
         CanonicalValue(topology.value().topology_revision.value())},
        {"legal_control_route_ids", string_set(routes)},
        {"route_endpoint_projections",
         CanonicalValue(std::move(endpoint_projections))},
        {"provider_instance_id",
         CanonicalValue("provider_" + suffix)},
        {"provider_runtime_unit_id", CanonicalValue("planner_runtime")},
        {"provider_manifest_id",
         CanonicalValue(manifest.value().provider_manifest_id.value())},
        {"provider_manifest_revision",
         CanonicalValue(manifest.value().provider_manifest_revision.value())},
        {"state_authority_id",
         CanonicalValue("authority_" + suffix)},
        {"state_authority_class",
         CanonicalValue(options.state_authority_class)},
        {"stop_evidence_policy_id",
         CanonicalValue(
             stop_policy.value().stop_evidence_policy_id.value())},
        {"stop_evidence_policy_revision",
         CanonicalValue(
             stop_policy.value().stop_evidence_policy_revision.value())},
        {"containment_granularity",
         CanonicalValue(options.binding_containment_granularity)},
    });
  }
  const CanonicalValue runtime_ids =
      options.include_runtime
          ? string_set({"planner_runtime"})
          : CanonicalValue(CanonicalValue::Array{});
  const auto parsed = parse_strict_json_model_yaml(canonical_json(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kCellGraphApiVersion))},
          {"graph_revision", CanonicalValue(std::string(kZeroDigest))},
          {"cell_id", CanonicalValue("applicability_cell")},
          {"runtime_units", CanonicalValue(std::move(runtime_units))},
          {"model_instances", CanonicalValue(CanonicalValue::Array{})},
          {"controllable_components", CanonicalValue(std::move(components))},
          {"control_domains",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"control_domain_id", CanonicalValue("axis_domain")},
                   {"control_domain_revision",
                    CanonicalValue(std::string(kZeroDigest))},
                   {"runtime_unit_ids", runtime_ids},
                   {"control_coordinator_endpoint_role",
                    CanonicalValue("coordinator")},
                   {"controller_topology_id",
                    CanonicalValue(
                        topology.value().controller_topology_id.value())},
                   {"topology_revision",
                    CanonicalValue(topology.value().topology_revision.value())},
                   {"containment_granularity",
                    CanonicalValue(
                        options.domain_containment_granularity)},
               })})},
          {"control_bindings", CanonicalValue(std::move(bindings))},
          {"component_attachments", CanonicalValue(CanonicalValue::Array{})},
      })));
  const auto intrinsic = validate_intrinsic_cell_graph_candidate(
      decode_cell_graph_candidate(parsed));
  if (runtime_profile == nullptr) {
    return compute_graph_revision(
        prepare_cell_graph_identity(intrinsic, {}));
  }
  return compute_graph_revision(prepare_cell_graph_identity(
      intrinsic,
      std::span<const QualifiedRuntimeProfile>(runtime_profile, 1U)));
}

[[nodiscard]] CompositionQualifiedCellIr compose_graph(
    const GraphRevisionVerifiedCellGraph& graph,
    const QualifiedComponentType& type) {
  return compose_cell_occurrences(
      graph, std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>(&type, 1U));
}

struct AuthoredOverlay final {
  core::SiteOverlayRefDTO reference;
  std::string bytes;
};

[[nodiscard]] std::string overlay_external_row(std::string_view ref) {
  return "{\"site_overlay_value_ref\":\"" + std::string(ref) +
         "\",\"semantic_kind\":\"external_resource\","
         "\"resource_kind\":\"service_endpoint\","
         "\"value\":{\"value_kind\":\"string\","
         "\"value\":\"http://site.invalid\"}}";
}

[[nodiscard]] std::string overlay_configuration_secret_row() {
  return "{\"site_overlay_value_ref\":\"configuration.secret\","
         "\"semantic_kind\":\"configuration_value\","
         "\"value\":{\"value_kind\":\"secret_reference\","
         "\"secret_reference_id\":\"vault.test_token\"}}";
}

[[nodiscard]] const std::string& overlay_row_ref(const CanonicalValue& row) {
  const auto* member = row.find_object_member("site_overlay_value_ref");
  if (member == nullptr) {
    throw std::runtime_error("overlay fixture row has no reference");
  }
  const auto* text = std::get_if<std::string>(&member->storage());
  if (text == nullptr) {
    throw std::runtime_error("overlay fixture reference is not a string");
  }
  return *text;
}

[[nodiscard]] AuthoredOverlay author_overlay(
    const std::vector<std::string>& external_refs) {
  CanonicalValue::Array rows;
  for (const auto& ref : external_refs) {
    rows.push_back(parse_strict_json_model_yaml(
                       overlay_external_row(ref))
                       .root());
  }
  rows.push_back(parse_strict_json_model_yaml(
                     overlay_configuration_secret_row())
                     .root());
  std::sort(rows.begin(), rows.end(),
            [](const auto& left, const auto& right) {
              return overlay_row_ref(left) < overlay_row_ref(right);
            });
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.site_overlay.v1")},
      {"site_overlay_id", CanonicalValue("test_site")},
      {"values", CanonicalValue(rows)},
  });
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("site-overlay-revision");
  preimage.push_back('\0');
  preimage.append("manyforge.site_overlay.v1");
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  const std::string revision = sha256_digest(preimage);
  CanonicalValue root(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.site_overlay.v1")},
      {"site_overlay_id", CanonicalValue("test_site")},
      {"site_overlay_revision", CanonicalValue(revision)},
      {"values", CanonicalValue(rows)},
  });
  const std::string bytes = canonical_json(root);
  return {
      {core::SiteOverlayId("test_site"),
       core::SiteOverlayRevision(revision),
       core::ContentDigest(sha256_digest(bytes))},
      bytes,
  };
}

[[nodiscard]] std::string site_overlay_reference_json(
    const core::SiteOverlayRefDTO& reference) {
  return canonical_json(CanonicalValue(CanonicalValue::Object{
      {"site_overlay_id",
       CanonicalValue(reference.site_overlay_id.value())},
      {"site_overlay_revision",
       CanonicalValue(reference.site_overlay_revision.value())},
      {"non_secret_digest",
       CanonicalValue(reference.non_secret_digest.value())},
  }));
}

enum class ProviderTargetCase : std::uint8_t {
  kExact,
  kWrongScope,
  kWrongTarget,
  kStaleTargetRevision,
};

enum class ProviderCoverageCase : std::uint8_t {
  kExact,
  kMissing,
  kDuplicateSemanticTarget,
};

enum class ProviderEndpointCase : std::uint8_t {
  kExact,
  kMissingRequiredRole,
  kExtraRole,
  kUnknownRuntimeUnit,
  kRuntimeSourceRoleOutsideProfile,
  kUnknownExternalAlias,
  kExternalSourceRoleMismatch,
  kExistingExternalAliasAsRuntimeUnit,
  kExistingRuntimeUnitAsExternalResource,
};

enum class ExternalResourceCase : std::uint8_t {
  kExact,
  kEndpointRoleOutsideManifest,
  kOverlayKindMismatch,
};

enum class DesiredSelectionCase : std::uint8_t {
  kExact,
  kWrongDomain,
  kWrongTopology,
  kUnknownActivationSet,
  kUnknownRoute,
  kNoCommonActivationSet,
};

enum class ProviderInstanceConflictCase : std::uint8_t {
  kNone,
  kControlExternal,
  kControlCapabilityProvider,
  kCapabilityProviderExternal,
  kExternalExternal,
};

struct InputOptions final {
  bool omit_desired = false;
  bool duplicate_desired = false;
  bool stale_desired_binding_revision = false;
  bool extra_desired_binding_route = false;
  DesiredSelectionCase desired_selection_case =
      DesiredSelectionCase::kExact;
  bool include_runtime_and_external_provider_binding = false;
  bool include_assistant_only_external = false;
  ProviderInstanceConflictCase provider_instance_conflict =
      ProviderInstanceConflictCase::kNone;
  bool reverse_source_order = false;
  ProviderTargetCase provider_target_case =
      ProviderTargetCase::kExact;
  ProviderCoverageCase provider_coverage_case =
      ProviderCoverageCase::kExact;
  ProviderEndpointCase provider_endpoint_case =
      ProviderEndpointCase::kExact;
  ExternalResourceCase external_resource_case =
      ExternalResourceCase::kExact;
  StopForm stop_form = StopForm::kNotStoppable;
  StopParameterCase stop_parameter_case = StopParameterCase::kExact;
  CommissionedSafetyCase commissioned_safety_case =
      CommissionedSafetyCase::kNone;
};

[[nodiscard]] std::string desired_route_json(
    const core::ControlBindingDTO& binding) {
  return canonical_json(CanonicalValue(CanonicalValue::Object{
      {"control_binding_id",
       CanonicalValue(binding.control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(binding.control_binding_revision.value())},
      {"control_route_id", CanonicalValue("route_a")},
  }));
}

[[nodiscard]] std::string external_resource_json(
    std::string_view alias, std::string_view overlay_ref,
    const QualifiedProviderManifest& manifest,
    std::string_view provider_instance,
    std::string_view endpoint_role = "plan_external",
    std::string_view resource_kind = "service_endpoint") {
  return canonical_json(CanonicalValue(CanonicalValue::Object{
      {"external_resource_alias", CanonicalValue(std::string(alias))},
      {"resource_kind", CanonicalValue(std::string(resource_kind))},
      {"provider_instance_id",
       CanonicalValue(std::string(provider_instance))},
      {"provider_manifest_id",
       CanonicalValue(manifest.value().provider_manifest_id.value())},
      {"provider_manifest_revision",
       CanonicalValue(manifest.value().provider_manifest_revision.value())},
      {"endpoint_role_id", CanonicalValue(std::string(endpoint_role))},
      {"site_overlay_value_ref",
       CanonicalValue(std::string(overlay_ref))},
  }));
}

[[nodiscard]] RevisionVerifiedDeploymentApplicabilityInputs make_inputs(
    const CompositionQualifiedCellIr& composition,
    const QualifiedComponentType& type,
    const QualifiedCapabilityDefinition* provider_capability,
    const QualifiedProviderManifest& primary_manifest,
    const QualifiedProviderManifest* conflicting_manifest,
    const InputOptions& options,
    const std::optional<core::SiteOverlayRefDTO>& overlay_reference) {
  const auto& graph = composition.graph();
  CanonicalValue::Array desired;
  if (!options.omit_desired) {
    CanonicalValue::Array binding_routes;
    for (std::size_t index = 0U;
         index < graph.control_bindings.size(); ++index) {
      const auto& binding = graph.control_bindings[index];
      if (options.stale_desired_binding_revision && index == 0U) {
        binding_routes.emplace_back(CanonicalValue::Object{
            {"control_binding_id",
             CanonicalValue(binding.control_binding_id.value())},
            {"control_binding_revision",
             CanonicalValue(std::string(kZeroDigest))},
            {"control_route_id", CanonicalValue("route_a")},
        });
      } else if (
          options.desired_selection_case ==
              DesiredSelectionCase::kUnknownRoute &&
          index == 0U) {
        binding_routes.emplace_back(CanonicalValue::Object{
            {"control_binding_id",
             CanonicalValue(binding.control_binding_id.value())},
            {"control_binding_revision",
             CanonicalValue(binding.control_binding_revision.value())},
            {"control_route_id", CanonicalValue("route_unknown")},
        });
      } else if (
          options.desired_selection_case ==
              DesiredSelectionCase::kNoCommonActivationSet &&
          index == 1U) {
        binding_routes.emplace_back(CanonicalValue::Object{
            {"control_binding_id",
             CanonicalValue(binding.control_binding_id.value())},
            {"control_binding_revision",
             CanonicalValue(binding.control_binding_revision.value())},
            {"control_route_id", CanonicalValue("route_b")},
        });
      } else {
        binding_routes.push_back(parse_strict_json_model_yaml(
                                     desired_route_json(binding))
                                     .root());
      }
    }
    if (options.extra_desired_binding_route) {
      binding_routes.emplace_back(CanonicalValue::Object{
          {"control_binding_id", CanonicalValue("binding_extra")},
          {"control_binding_revision",
           CanonicalValue(std::string(kZeroDigest))},
          {"control_route_id", CanonicalValue("route_a")},
      });
    }
    if (options.reverse_source_order) {
      std::reverse(binding_routes.begin(), binding_routes.end());
    }
    desired.emplace_back(CanonicalValue::Object{
        {"control_domain_id",
         CanonicalValue(
             options.desired_selection_case ==
                     DesiredSelectionCase::kWrongDomain
                 ? "wrong_domain"
                 : graph.control_domains.front()
                       .control_domain_id.value())},
        {"control_domain_revision",
         CanonicalValue(graph.control_domains.front()
                            .control_domain_revision.value())},
        {"controller_topology_id",
         CanonicalValue(
             options.desired_selection_case ==
                     DesiredSelectionCase::kWrongTopology
                 ? "wrong_topology"
                 : graph.control_domains.front()
                       .controller_topology_id.value())},
        {"topology_revision",
         CanonicalValue(
             graph.control_domains.front().topology_revision.value())},
        {"activation_set_id",
         CanonicalValue(
             options.desired_selection_case ==
                     DesiredSelectionCase::kUnknownActivationSet
                 ? "set_unknown"
                 : "set_a")},
        {"binding_routes", CanonicalValue(std::move(binding_routes))},
    });
    if (options.duplicate_desired) {
      desired.push_back(desired.front());
    }
  }

  CanonicalValue::Array provider_bindings;
  CanonicalValue::Array external_resources;
  if (options.include_runtime_and_external_provider_binding) {
    if (provider_capability == nullptr || !overlay_reference.has_value()) {
      throw std::runtime_error("provider-binding fixture is incomplete");
    }
    const auto provider_scope =
        options.provider_target_case == ProviderTargetCase::kWrongScope
            ? core::CapabilityScope::kCellGlobal
            : provider_capability->value().scope;
    const auto target_for = [&](const core::ControlBindingDTO& anchor) {
      switch (provider_scope) {
        case core::CapabilityScope::kCellGlobal:
          return CanonicalValue(CanonicalValue::Object{
              {"cell_id",
               CanonicalValue(
                   options.provider_target_case ==
                           ProviderTargetCase::kWrongTarget
                       ? "wrong_cell"
                       : graph.cell_id.value())},
          });
        case core::CapabilityScope::kControlDomain: {
          const auto& domain = graph.control_domains.front();
          return CanonicalValue(CanonicalValue::Object{
              {"control_domain_id",
               CanonicalValue(
                   options.provider_target_case ==
                           ProviderTargetCase::kWrongTarget
                       ? "wrong_domain"
                       : domain.control_domain_id.value())},
              {"control_domain_revision",
               CanonicalValue(domain.control_domain_revision.value())},
              {"controller_topology_id",
               CanonicalValue(domain.controller_topology_id.value())},
              {"topology_revision",
               CanonicalValue(domain.topology_revision.value())},
          });
        }
        case core::CapabilityScope::kTargetBinding:
          return CanonicalValue(CanonicalValue::Object{
              {"controllable_component_id",
               CanonicalValue(
                   options.provider_target_case ==
                           ProviderTargetCase::kWrongTarget
                       ? "wrong_component"
                       : anchor.controllable_component_id.value())},
              {"component_type_revision",
               CanonicalValue(
                   options.provider_target_case ==
                           ProviderTargetCase::kStaleTargetRevision
                       ? std::string(kZeroDigest)
                       : type.value().component_type_revision.value())},
              {"command_target_id",
               CanonicalValue(anchor.command_target_id.value())},
              {"control_binding_id",
               CanonicalValue(anchor.control_binding_id.value())},
              {"control_binding_revision",
               CanonicalValue(anchor.control_binding_revision.value())},
          });
      }
      throw std::runtime_error("test fixture selected an unknown scope");
    };
    const auto endpoints = [&]() {
      CanonicalValue::Array result;
      const bool runtime_as_external =
          options.provider_endpoint_case ==
          ProviderEndpointCase::kExistingRuntimeUnitAsExternalResource;
      result.emplace_back(CanonicalValue::Object{
          {"endpoint_role_id", CanonicalValue("plan_external")},
          {"source_kind", CanonicalValue("external_resource")},
          {"source_id",
           CanonicalValue(
               runtime_as_external
                   ? "planner_runtime"
                   : options.provider_endpoint_case ==
                       ProviderEndpointCase::kUnknownExternalAlias
                         ? "unknown_service"
                         : "planning_service")},
          {"source_endpoint_role_id",
           CanonicalValue(
               options.provider_endpoint_case ==
                       ProviderEndpointCase::kExternalSourceRoleMismatch
                   ? "runtime_status"
                   : "plan_external")},
      });
      if (options.provider_endpoint_case !=
          ProviderEndpointCase::kMissingRequiredRole) {
        const bool external_as_runtime =
            options.provider_endpoint_case ==
            ProviderEndpointCase::kExistingExternalAliasAsRuntimeUnit;
        result.emplace_back(CanonicalValue::Object{
            {"endpoint_role_id", CanonicalValue("plan_runtime")},
            {"source_kind", CanonicalValue("runtime_unit")},
            {"source_id",
             CanonicalValue(
                 external_as_runtime
                     ? "planning_service"
                     : options.provider_endpoint_case ==
                         ProviderEndpointCase::kUnknownRuntimeUnit
                           ? "unknown_runtime"
                           : "planner_runtime")},
            {"source_endpoint_role_id",
             CanonicalValue(
                 options.provider_endpoint_case ==
                         ProviderEndpointCase::
                             kRuntimeSourceRoleOutsideProfile
                     ? "unknown_runtime_role"
                     : "plan_runtime")},
        });
      }
      if (options.provider_endpoint_case ==
          ProviderEndpointCase::kExtraRole) {
        result.emplace_back(CanonicalValue::Object{
            {"endpoint_role_id", CanonicalValue("unexpected")},
            {"source_kind", CanonicalValue("runtime_unit")},
            {"source_id", CanonicalValue("planner_runtime")},
            {"source_endpoint_role_id", CanonicalValue("plan_runtime")},
        });
      }
      return result;
    };
    const QualifiedProviderManifest& provider_binding_manifest =
        options.provider_instance_conflict ==
                ProviderInstanceConflictCase::kControlCapabilityProvider
            ? *conflicting_manifest
            : primary_manifest;
    const std::string provider_binding_instance =
        options.provider_instance_conflict ==
                ProviderInstanceConflictCase::kCapabilityProviderExternal
            ? "capability_external_shared"
            : graph.control_bindings.front().provider_instance_id.value();
    const auto add_provider_binding =
        [&](std::string id, const core::ControlBindingDTO& anchor) {
          provider_bindings.emplace_back(CanonicalValue::Object{
              {"capability_provider_binding_id",
               CanonicalValue(std::move(id))},
              {"capability_provider_binding_revision",
               CanonicalValue(std::string(kZeroDigest))},
              {"capability_id",
               CanonicalValue(
                   provider_capability->value().capability_id.value())},
              {"capability_definition_revision",
               CanonicalValue(
                   provider_capability->value()
                       .capability_definition_revision.value())},
              {"scope",
               CanonicalValue(std::string(core::to_string(provider_scope)))},
              {"target", target_for(anchor)},
              {"provider_instance_id",
               CanonicalValue(provider_binding_instance)},
              {"provider_manifest_id",
               CanonicalValue(
                   provider_binding_manifest.value()
                       .provider_manifest_id.value())},
              {"provider_manifest_revision",
               CanonicalValue(provider_binding_manifest.value()
                                  .provider_manifest_revision.value())},
              {"endpoint_bindings", CanonicalValue(endpoints())},
          });
        };
    if (options.provider_coverage_case !=
        ProviderCoverageCase::kMissing) {
      if (provider_scope == core::CapabilityScope::kTargetBinding) {
        for (std::size_t index = 0U;
             index < graph.control_bindings.size(); ++index) {
          add_provider_binding(
              "planning_provider_binding_" + std::to_string(index),
              graph.control_bindings[index]);
        }
      } else {
        add_provider_binding("planning_provider_binding",
                             graph.control_bindings.front());
      }
      if (options.provider_coverage_case ==
          ProviderCoverageCase::kDuplicateSemanticTarget) {
        add_provider_binding("planning_provider_binding_duplicate",
                             graph.control_bindings.front());
      }
    }
    const auto external_role =
        options.external_resource_case ==
                ExternalResourceCase::kEndpointRoleOutsideManifest
            ? "unknown_external_role"
            : "plan_external";
    const auto external_kind =
        options.external_resource_case ==
                ExternalResourceCase::kOverlayKindMismatch
            ? "data_source"
            : "service_endpoint";
    const QualifiedProviderManifest& external_manifest =
        options.provider_instance_conflict ==
                ProviderInstanceConflictCase::kCapabilityProviderExternal
            ? *conflicting_manifest
            : primary_manifest;
    const std::string external_instance =
        options.provider_instance_conflict ==
                ProviderInstanceConflictCase::kCapabilityProviderExternal
            ? provider_binding_instance
            : graph.control_bindings.front().provider_instance_id.value();
    external_resources.push_back(
        parse_strict_json_model_yaml(external_resource_json(
            "planning_service", "resources.planning_service",
            external_manifest, external_instance,
            external_role, external_kind))
            .root());
  }
  if (options.include_assistant_only_external ||
      options.provider_instance_conflict ==
          ProviderInstanceConflictCase::kControlExternal) {
    if (conflicting_manifest == nullptr || !overlay_reference.has_value()) {
      throw std::runtime_error("conflict fixture is incomplete");
    }
    const auto& anchor = graph.control_bindings.front();
    external_resources.push_back(
        parse_strict_json_model_yaml(external_resource_json(
            "assistant_service", "resources.assistant_service",
            *conflicting_manifest,
            options.include_assistant_only_external
                ? "assistant_provider"
                : anchor.provider_instance_id.value(),
            "state"))
            .root());
  } else if (
      options.provider_instance_conflict ==
      ProviderInstanceConflictCase::kExternalExternal) {
    if (conflicting_manifest == nullptr || !overlay_reference.has_value()) {
      throw std::runtime_error("external conflict fixture is incomplete");
    }
    external_resources.push_back(
        parse_strict_json_model_yaml(external_resource_json(
            "assistant_service", "resources.assistant_service",
            primary_manifest, "external_shared", "state"))
            .root());
    external_resources.push_back(
        parse_strict_json_model_yaml(external_resource_json(
            "assistant_service_b", "resources.assistant_service_b",
            *conflicting_manifest, "external_shared", "state"))
            .root());
  }

  CanonicalValue::Object root{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"desired_control_configurations", CanonicalValue(std::move(desired))},
      {"capability_provider_bindings",
       CanonicalValue(std::move(provider_bindings))},
      {"external_resources", CanonicalValue(std::move(external_resources))},
  };
  if (options.stop_form != StopForm::kNotStoppable) {
    CanonicalValue::Array parameter_rows;
    CanonicalValue::Array commissioned_rows;
    for (const auto& binding : graph.control_bindings) {
      for (const auto& route_id : binding.legal_control_route_ids) {
        const auto parameter_row = [&](std::string_view stage_id,
                                       CanonicalValue contract,
                                       CanonicalValue parameters) {
          return CanonicalValue(CanonicalValue::Object{
              {"control_domain_id",
               CanonicalValue(binding.control_domain_id.value())},
              {"control_domain_revision",
               CanonicalValue(graph.control_domains.front()
                                  .control_domain_revision.value())},
              {"control_binding_id",
               CanonicalValue(binding.control_binding_id.value())},
              {"control_binding_revision",
               CanonicalValue(binding.control_binding_revision.value())},
              {"controller_topology_id",
               CanonicalValue(binding.controller_topology_id.value())},
              {"topology_revision",
               CanonicalValue(binding.topology_revision.value())},
              {"control_route_id", CanonicalValue(route_id.value())},
              {"stop_evidence_policy_id",
               CanonicalValue(binding.stop_evidence_policy_id.value())},
              {"stop_evidence_policy_revision",
               CanonicalValue(binding.stop_evidence_policy_revision.value())},
              {"stop_escalation_stage_id",
               CanonicalValue(std::string(stage_id))},
              {"provider_contract_ref", std::move(contract)},
              {"default_contract_parameters", std::move(parameters)},
          });
        };
        if (options.stop_form == StopForm::kProviderDefined) {
          CanonicalValue::Array resource_limits;
          resource_limits.reserve(binding.resource_bindings.size());
          for (std::size_t resource_index = 0U;
               resource_index < binding.resource_bindings.size();
               ++resource_index) {
            if (options.stop_parameter_case ==
                    StopParameterCase::kMissingResource &&
                resource_index + 1U == binding.resource_bindings.size()) {
              continue;
            }
            const auto& resource = binding.resource_bindings[resource_index];
            const bool wrong_kind =
                options.stop_parameter_case ==
                StopParameterCase::kWrongCoordinateKind;
            resource_limits.emplace_back(CanonicalValue::Object{
                {"resource_id", CanonicalValue(resource.resource_id.value())},
                {"max_deceleration",
                 CanonicalValue(CanonicalValue::Object{
                     {"coordinate_kind",
                      CanonicalValue(wrong_kind ? "linear_m_s2"
                                                : "angular_rad_s2")},
                     {wrong_kind ? "value_m_s2" : "value_rad_s2",
                      CanonicalValue(1.0)},
                 })},
                {"settle_velocity_threshold",
                 CanonicalValue(CanonicalValue::Object{
                     {"coordinate_kind",
                      CanonicalValue(wrong_kind ? "linear_m_s"
                                                : "angular_rad_s")},
                     {wrong_kind ? "value_m_s" : "value_rad_s",
                      CanonicalValue(0.01)},
                 })},
            });
          }
          parameter_rows.push_back(parameter_row(
              "nominal", provider_contract_ref(),
              CanonicalValue(CanonicalValue::Object{
                  {"stop_mode", CanonicalValue("soft_on_path")},
                  {"deceleration_time_s", CanonicalValue(0.25)},
                  {"per_resource_limits",
                   CanonicalValue(std::move(resource_limits))},
                  {"allow_natural_completion_on_short_path",
                   CanonicalValue(false)},
              })));
        }
        if (options.stop_parameter_case !=
            StopParameterCase::kMissingHardStage) {
          CanonicalValue hard_row = parameter_row(
              options.stop_parameter_case ==
                      StopParameterCase::kUnusedStage
                  ? "unused_stage"
                  : "software_hard",
              software_hard_contract_ref(),
              CanonicalValue(CanonicalValue::Object{}));
          if (options.stop_parameter_case ==
              StopParameterCase::kStalePolicyRevision) {
            auto fields = hard_row.as_object();
            for (auto& [name, value] : fields) {
              if (name == "stop_evidence_policy_revision") {
                value = CanonicalValue(std::string(kZeroDigest));
              }
            }
            hard_row = CanonicalValue(std::move(fields));
          }
          parameter_rows.push_back(std::move(hard_row));
        }
        if (options.commissioned_safety_case !=
                CommissionedSafetyCase::kNone &&
            options.commissioned_safety_case !=
                CommissionedSafetyCase::kUnusedRow) {
          parameter_rows.push_back(parameter_row(
              "safety_request", safety_request_contract_ref(),
              CanonicalValue(CanonicalValue::Object{})));
        }
      }
    }
    if (options.commissioned_safety_case !=
            CommissionedSafetyCase::kNone &&
        options.commissioned_safety_case !=
            CommissionedSafetyCase::kMissingRow) {
      const auto& binding = graph.control_bindings.front();
      const std::string evidence_digest =
          sha256_digest(kCommissioningEvidenceBytes);
      CanonicalValue::Object commissioned{
          {"commissioned_stop_interface_id",
           CanonicalValue("commissioned_safety_request")},
          {"commissioned_stop_interface_revision",
           CanonicalValue(std::string(kZeroDigest))},
          {"control_domain_id",
           CanonicalValue(binding.control_domain_id.value())},
          {"control_domain_revision",
           CanonicalValue(graph.control_domains.front()
                              .control_domain_revision.value())},
          {"control_binding_id",
           CanonicalValue(binding.control_binding_id.value())},
          {"control_binding_revision",
           CanonicalValue(binding.control_binding_revision.value())},
          {"control_route_id",
           CanonicalValue(
               options.commissioned_safety_case ==
                       CommissionedSafetyCase::kWrongRoute
                   ? "unknown_route"
                   : binding.legal_control_route_ids.front().value())},
          {"provider_instance_id",
           CanonicalValue(binding.provider_instance_id.value())},
          {"provider_manifest_id",
           CanonicalValue(binding.provider_manifest_id.value())},
          {"provider_manifest_revision",
           CanonicalValue(binding.provider_manifest_revision.value())},
          {"registered_command_kind_id",
           CanonicalValue("manyforge.stop.safety.protective_request")},
          {"provider_contract_ref", safety_request_contract_ref()},
          {"endpoint_role_id", CanonicalValue("stop_command")},
          {"stop_action_class",
           CanonicalValue(
               options.commissioned_safety_case ==
                       CommissionedSafetyCase::kWrongActionClass
                   ? "controlled"
                   : "safety_system_request")},
          {"safety_system_request_action_class",
           CanonicalValue("protective_stop_request")},
          {"commissioning_evidence_artifact_ref",
           CanonicalValue(CanonicalValue::Object{
               {"artifact_role_id",
                CanonicalValue(std::string(
                    core::kStopInterfaceCommissioningArtifactRole))},
               {"media_type",
                CanonicalValue(std::string(
                    core::kStopInterfaceCommissioningMediaType))},
               {"byte_size",
                CanonicalValue(static_cast<double>(
                    kCommissioningEvidenceBytes.size()))},
               {"content_digest", CanonicalValue(evidence_digest)},
               {"content_addressed_store_key",
                CanonicalValue("sha256/" + evidence_digest.substr(7U))},
           })},
      };
      if (options.commissioned_safety_case ==
          CommissionedSafetyCase::kUnknownExternalResource) {
        commissioned.emplace_back("external_resource_alias",
                                  CanonicalValue("unknown_safety_endpoint"));
        commissioned.emplace_back("external_resource_kind",
                                  CanonicalValue("service_endpoint"));
        commissioned.emplace_back(
            "site_overlay_value_ref",
            CanonicalValue("resources.unknown_safety_endpoint"));
      }
      commissioned_rows.emplace_back(std::move(commissioned));
    }
    root.emplace_back(
        "policies",
        CanonicalValue(CanonicalValue::Object{
            {"retained_policy_records",
             CanonicalValue(CanonicalValue::Array{})},
            {"stop_stage_parameters",
             CanonicalValue(std::move(parameter_rows))},
            {"commissioned_stop_interfaces",
             CanonicalValue(std::move(commissioned_rows))},
            {"interrupt_sources", CanonicalValue(CanonicalValue::Array{})},
            {"interrupt_policies", CanonicalValue(CanonicalValue::Array{})},
            {"interrupt_config",
             CanonicalValue(CanonicalValue::Object{
                 {"queue_depth", CanonicalValue(8.0)},
                 {"cancel_validation_timeout_ns", CanonicalValue(1000.0)},
                 {"motion_scope_arm_timeout_ns", CanonicalValue(1000.0)},
                 {"supervisory_poll_rate_hz", CanonicalValue(10.0)},
             })},
        }));
  }
  if (overlay_reference.has_value()) {
    root.emplace_back(
        "site_overlay_ref",
        parse_strict_json_model_yaml(
            site_overlay_reference_json(*overlay_reference))
            .root());
  }
  return compute_capability_provider_binding_revisions(
      decode_deployment_applicability_inputs_candidate(
          parse_strict_json_model_yaml(
              canonical_json(CanonicalValue(std::move(root))))));
}

enum class ProviderCapabilityScopeCase : std::uint8_t {
  kTargetBinding,
  kCellGlobal,
  kControlDomain,
};

[[nodiscard]] std::string_view provider_capability_id(
    ProviderCapabilityScopeCase scope) {
  switch (scope) {
    case ProviderCapabilityScopeCase::kTargetBinding:
      return kProviderCapability;
    case ProviderCapabilityScopeCase::kCellGlobal:
      return "scene.reference.resolve";
    case ProviderCapabilityScopeCase::kControlDomain:
      return "control_domain.controller.observe";
  }
  throw std::runtime_error("unknown provider capability scope fixture");
}

struct FixtureOptions final {
  std::size_t binding_count = 1U;
  std::size_t resource_count = 1U;
  std::size_t interface_count = 1U;
  bool include_plus_one_role = false;
  bool expand_second_role = false;
  bool reverse_contract_role_order = false;
  ResourceBindingCase resource_binding_case =
      ResourceBindingCase::kExact;
  bool repeated_routes_and_activation_sets = false;
  bool incompatible_route_activation_sets = false;
  bool chained_route = false;
  bool directional_declarations = false;
  bool topology_contract_mismatch = false;
  bool binding_has_state_interfaces = true;
  bool control_uses_provider_capability = false;
  bool include_provider_binding = false;
  bool provider_uses_control_capability = false;
  ProviderCapabilityScopeCase provider_scope =
      ProviderCapabilityScopeCase::kTargetBinding;
  ProviderTargetCase provider_target_case =
      ProviderTargetCase::kExact;
  ProviderCoverageCase provider_coverage_case =
      ProviderCoverageCase::kExact;
  ProviderEndpointCase provider_endpoint_case =
      ProviderEndpointCase::kExact;
  ExternalResourceCase external_resource_case =
      ExternalResourceCase::kExact;
  bool include_assistant_only_external = false;
  ProviderInstanceConflictCase provider_instance_conflict =
      ProviderInstanceConflictCase::kNone;
  ControlProviderOfferCase control_provider_offer_case =
      ControlProviderOfferCase::kExact;
  ControlEndpointProjectionCase control_endpoint_projection_case =
      ControlEndpointProjectionCase::kExact;
  bool reverse_source_order = false;
  bool stale_control_capability_revision = false;
  bool omit_desired = false;
  bool duplicate_desired = false;
  bool stale_desired_binding_revision = false;
  bool extra_desired_binding_route = false;
  DesiredSelectionCase desired_selection_case =
      DesiredSelectionCase::kExact;
  StopForm stop_form = StopForm::kNotStoppable;
  ProviderStopEndpointCase provider_stop_endpoint_case =
      ProviderStopEndpointCase::kExact;
  ProviderStopCompatibilityCase provider_stop_compatibility_case =
      ProviderStopCompatibilityCase::kExact;
  BindingStateStopCase binding_state_stop_case =
      BindingStateStopCase::kExact;
  std::string policy_containment_granularity = "resource";
  std::string binding_containment_granularity = "resource";
  std::string domain_containment_granularity = "resource";
  std::string state_authority_class = "measured";
  StopParameterCase stop_parameter_case = StopParameterCase::kExact;
  CommissionedSafetyCase commissioned_safety_case =
      CommissionedSafetyCase::kNone;
  bool omit_policy_software_hard_floor = false;
};

struct ApplicabilityFixture final {
  CompositionQualifiedCellIr composition;
  RevisionVerifiedDeploymentApplicabilityInputs inputs;
  std::vector<QualifiedRuntimeProfile> runtime_profiles;
  std::vector<QualifiedCapabilityDefinition> capabilities;
  std::vector<QualifiedControlInterfaceContract> contracts;
  std::vector<QualifiedControllerTopology> topologies;
  std::vector<QualifiedProviderManifest> manifests;
  std::vector<QualifiedStopEvidencePolicy> stop_policies;
  std::vector<BuildQualifiedValueSchemaEntry> value_schemas;
  std::vector<QualifiedCommissioningEvidenceRef> commissioning_evidence_refs;
  QualifiedSelectedSiteOverlay selected_overlay;
};

[[nodiscard]] ApplicabilityFixture make_fixture(
    const FixtureOptions& options = {}) {
  if (options.chained_route &&
      options.repeated_routes_and_activation_sets) {
    throw std::runtime_error(
        "test fixture cannot combine custom chained and repeated routes");
  }
  if (options.chained_route && options.directional_declarations) {
    throw std::runtime_error(
        "test fixture cannot combine its two custom topologies");
  }
  if (options.incompatible_route_activation_sets &&
      !options.repeated_routes_and_activation_sets) {
    throw std::runtime_error(
        "split route compatibility requires repeated routes");
  }
  std::vector<QualifiedCapabilityDefinition> capabilities;
  capabilities.push_back(qualify_capability(
      options.control_uses_provider_capability ? kProviderCapability
                                               : kControlCapability));
  if (options.control_provider_offer_case ==
      ControlProviderOfferCase::kMissingCapability) {
    capabilities.push_back(
        qualify_capability("execution.trajectory.execute"));
  }
  const bool has_distinct_provider_capability =
      options.include_provider_binding &&
      !options.provider_uses_control_capability;
  if (has_distinct_provider_capability) {
    capabilities.push_back(
        qualify_capability(provider_capability_id(options.provider_scope)));
  }
  const std::size_t provider_capability_index =
      has_distinct_provider_capability ? capabilities.size() - 1U : 0U;
  std::vector<QualifiedCapabilityDefinition> target_capabilities{
      capabilities.front()};
  if (options.include_provider_binding &&
      options.provider_scope ==
          ProviderCapabilityScopeCase::kTargetBinding &&
      provider_capability_index != 0U) {
    target_capabilities.push_back(capabilities[provider_capability_index]);
  }
  const auto type = qualify_component_type(
      target_capabilities, options.include_plus_one_role);
  const auto contract =
      qualify_contract("axis_position", options.interface_count,
                       options.include_plus_one_role, true,
                       options.binding_has_state_interfaces,
                       options.expand_second_role,
                       options.reverse_contract_role_order);
  std::vector<QualifiedControlInterfaceContract> contracts{contract};
  std::optional<QualifiedControlInterfaceContract> topology_contract;
  if (options.topology_contract_mismatch) {
    topology_contract = qualify_contract(
        "axis_position", options.interface_count + 1U,
        options.include_plus_one_role, true,
        options.binding_has_state_interfaces,
        options.expand_second_role);
    contracts.push_back(*topology_contract);
  }
  const auto& selected_topology_contract =
      topology_contract.has_value() ? *topology_contract : contract;
  const auto topology = [&]() {
    if (options.chained_route) {
      return qualify_chained_topology(selected_topology_contract);
    }
    if (options.directional_declarations) {
      contracts.push_back(qualify_contract(
          "axis_command_only", 1U, false, true, false));
      contracts.push_back(qualify_contract(
          "axis_state_only", 1U, false, false, true));
      return qualify_topology_with_directional_declarations(
          contract, contracts[1], contracts[2]);
    }
    return qualify_topology(
        selected_topology_contract,
        options.repeated_routes_and_activation_sets,
        options.incompatible_route_activation_sets);
  }();
  const bool include_safety_stage =
      options.commissioned_safety_case != CommissionedSafetyCase::kNone &&
      options.commissioned_safety_case != CommissionedSafetyCase::kUnusedRow;
  const auto stop_policy = qualify_stop_policy(
      options.stop_form, options.policy_containment_granularity,
      options.binding_state_stop_case,
      options.provider_stop_compatibility_case, include_safety_stage,
      options.commissioned_safety_case ==
          CommissionedSafetyCase::kNonFinalStage,
      options.omit_policy_software_hard_floor);
  const auto primary_manifest = qualify_manifest(
      "primary_provider_manifest", capabilities, options.stop_form,
      options.include_provider_binding ||
          options.control_uses_provider_capability,
      options.provider_stop_endpoint_case,
      options.provider_stop_compatibility_case,
      options.control_provider_offer_case,
      capabilities.front().value().capability_id.value(),
      include_safety_stage);
  std::optional<QualifiedProviderManifest> conflicting_manifest;
  if (options.include_assistant_only_external ||
      options.provider_instance_conflict !=
          ProviderInstanceConflictCase::kNone) {
    conflicting_manifest = qualify_manifest(
        "conflicting_provider_manifest", capabilities,
        StopForm::kNotStoppable, options.include_provider_binding);
  }
  // Every command-bearing control binding names its exact managed provider
  // runtime, independently of whether this fixture also authors a planning
  // capability-provider binding.
  std::optional<QualifiedRuntimeProfile> runtime_profile(
      qualify_runtime_profile());
  const auto graph = make_graph(
      {.binding_count = options.binding_count,
       .resource_count = options.resource_count,
       .include_plus_one_role = options.include_plus_one_role,
       .expand_second_role = options.expand_second_role,
       .repeated_routes_and_activation_sets =
           options.repeated_routes_and_activation_sets,
       .include_runtime = true,
       .reverse_source_order = options.reverse_source_order,
       .stale_control_capability_revision =
           options.stale_control_capability_revision,
       .provider_stop_endpoint_case =
           options.provider_stop_endpoint_case,
       .provider_stop_compatibility_case =
           options.provider_stop_compatibility_case,
       .control_endpoint_projection_case =
           options.control_endpoint_projection_case,
       .resource_binding_case = options.resource_binding_case,
       .binding_containment_granularity =
           options.binding_containment_granularity,
       .domain_containment_granularity =
           options.domain_containment_granularity,
       .state_authority_class = options.state_authority_class},
      type, capabilities.front(), contract, topology, primary_manifest,
      stop_policy,
      runtime_profile.has_value() ? &*runtime_profile : nullptr);
  const auto composition = compose_graph(graph, type);

  std::vector<std::string> external_refs;
  if (options.include_provider_binding) {
    external_refs.emplace_back("resources.planning_service");
  }
  if (options.include_assistant_only_external ||
      options.provider_instance_conflict ==
          ProviderInstanceConflictCase::kControlExternal ||
      options.provider_instance_conflict ==
          ProviderInstanceConflictCase::kExternalExternal) {
    external_refs.emplace_back("resources.assistant_service");
  }
  if (options.provider_instance_conflict ==
      ProviderInstanceConflictCase::kExternalExternal) {
    external_refs.emplace_back("resources.assistant_service_b");
  }
  std::optional<AuthoredOverlay> authored_overlay;
  if (!external_refs.empty()) {
    authored_overlay = author_overlay(external_refs);
  }
  const std::optional<core::SiteOverlayRefDTO> overlay_reference =
      authored_overlay.has_value()
          ? std::optional<core::SiteOverlayRefDTO>(
                authored_overlay->reference)
          : std::nullopt;
  const auto inputs = make_inputs(
      composition, type,
      options.include_provider_binding
          ? &capabilities[provider_capability_index]
          : nullptr,
      primary_manifest,
      conflicting_manifest.has_value() ? &*conflicting_manifest : nullptr,
      {.omit_desired = options.omit_desired,
       .duplicate_desired = options.duplicate_desired,
       .stale_desired_binding_revision =
           options.stale_desired_binding_revision,
       .extra_desired_binding_route =
           options.extra_desired_binding_route,
       .desired_selection_case = options.desired_selection_case,
       .include_runtime_and_external_provider_binding =
           options.include_provider_binding,
       .include_assistant_only_external =
           options.include_assistant_only_external,
       .provider_instance_conflict =
           options.provider_instance_conflict,
       .reverse_source_order = options.reverse_source_order,
       .provider_target_case = options.provider_target_case,
       .provider_coverage_case = options.provider_coverage_case,
       .provider_endpoint_case = options.provider_endpoint_case,
       .external_resource_case = options.external_resource_case,
       .stop_form = options.stop_form,
       .stop_parameter_case = options.stop_parameter_case,
       .commissioned_safety_case = options.commissioned_safety_case},
      overlay_reference);

  QualifiedSelectedSiteOverlay selected_overlay = [&]() {
    if (!authored_overlay.has_value()) {
      return select_site_overlay_for_deployment(
          inputs.value().site_overlay_ref,
          inputs.value().external_resources, nullptr);
    }
    const auto projection =
        resolve_and_verify_site_overlay_artifact_projection(
            authored_overlay->reference, authored_overlay->bytes);
    return select_site_overlay_for_deployment(
        inputs.value().site_overlay_ref,
        inputs.value().external_resources, &projection);
  }();

  std::vector<QualifiedRuntimeProfile> runtime_profiles;
  if (runtime_profile.has_value()) {
    runtime_profiles.push_back(std::move(*runtime_profile));
  }
  std::vector<QualifiedProviderManifest> manifests;
  manifests.push_back(primary_manifest);
  if (conflicting_manifest.has_value()) {
    manifests.push_back(std::move(*conflicting_manifest));
  }
  std::vector<BuildQualifiedValueSchemaEntry> value_schemas;
  if (options.stop_form != StopForm::kNotStoppable) {
    const auto& registry = build_qualified_closed_schema_registry();
    const auto* empty = registry.find_qualified_value_schema(
        core::SchemaId("manyforge.value_schema.provider_contract.empty.v1"),
        core::SchemaRevision(std::string(kEmptyProviderContractSchemaRevision)));
    const auto* motion = registry.find_qualified_value_schema(
        core::SchemaId(
            std::string(core::kMotionControlledStopContractSchemaId)),
        core::SchemaRevision(
            std::string(core::kMotionControlledStopContractSchemaRevision)));
    const bool needs_empty_provider_schema =
        !options.omit_policy_software_hard_floor;
    if ((needs_empty_provider_schema && empty == nullptr) ||
        ((options.stop_form == StopForm::kProviderDefined ||
          options.stop_form == StopForm::kBindingStateAuthority) &&
         motion == nullptr)) {
      throw std::logic_error(
          "Stop applicability fixture lacks its exact value-schema closure");
    }
    if (needs_empty_provider_schema) {
      value_schemas.push_back(*empty);
    }
    if (motion != nullptr &&
        (options.stop_form == StopForm::kProviderDefined ||
         options.stop_form == StopForm::kBindingStateAuthority)) {
      value_schemas.push_back(*motion);
    }
    std::sort(value_schemas.begin(), value_schemas.end(),
              [](const auto& left, const auto& right) {
                return std::tie(left.schema_id(), left.schema_revision()) <
                       std::tie(right.schema_id(), right.schema_revision());
              });
  }
  std::vector<QualifiedCommissioningEvidenceRef> commissioning_evidence_refs;
  if (options.commissioned_safety_case !=
      CommissionedSafetyCase::kMissingProof) {
    for (const auto& row : inputs.value().commissioned_stop_interfaces) {
      commissioning_evidence_refs.push_back(
          resolve_and_verify_commissioning_evidence_ref(
              row,
              row.commissioning_evidence_artifact_ref
                  .content_addressed_store_key,
              kCommissioningEvidenceBytes));
    }
  }
  return {
      composition,
      inputs,
      std::move(runtime_profiles),
      std::move(capabilities),
      std::move(contracts),
      {topology},
      std::move(manifests),
      {stop_policy},
      std::move(value_schemas),
      std::move(commissioning_evidence_refs),
      std::move(selected_overlay),
  };
}

[[nodiscard]] ApplicabilityQualifiedCellIr qualify(
    const ApplicabilityFixture& fixture) {
  return qualify_cell_applicability(
      fixture.composition, fixture.inputs, fixture.runtime_profiles,
      {}, {}, fixture.capabilities, fixture.contracts, fixture.topologies,
      fixture.manifests, fixture.stop_policies,
      fixture.value_schemas, fixture.commissioning_evidence_refs,
      fixture.selected_overlay);
}

void expect_exact_catalog_reference(
    const core::CatalogRecordRefDTO& actual,
    const core::CatalogRecordRefDTO& expected) {
  ASSERT_EQ(actual.index(), expected.index());
  std::visit(
      [&expected](const auto& actual_typed) {
        using Typed = std::decay_t<decltype(actual_typed)>;
        const auto* expected_typed = std::get_if<Typed>(&expected);
        ASSERT_NE(expected_typed, nullptr);
        EXPECT_EQ(actual_typed.record_id, expected_typed->record_id);
        EXPECT_EQ(actual_typed.record_revision,
                  expected_typed->record_revision);
        EXPECT_EQ(actual_typed.schema_id, expected_typed->schema_id);
        EXPECT_EQ(actual_typed.schema_revision,
                  expected_typed->schema_revision);
        EXPECT_EQ(actual_typed.content_digest,
                  expected_typed->content_digest);
        EXPECT_EQ(actual_typed.content_addressed_store_key,
                  expected_typed->content_addressed_store_key);
      },
      actual);
}

TEST(CellApplicability,
     CanonicalEmptyPassesThroughRealGraphDeploymentAndOverlaySelectors) {
  const auto graph = compute_graph_revision(prepare_cell_graph_identity(
      validate_intrinsic_cell_graph_candidate(
          decode_cell_graph_candidate(parse_strict_json_model_yaml(
              "{\"api_version\":\"manyforge.cell_graph.v1\","
              "\"graph_revision\":\"" +
              std::string(kZeroDigest) +
              "\",\"cell_id\":\"empty_cell\",\"runtime_units\":[],"
              "\"model_instances\":[],\"controllable_components\":[],"
              "\"control_domains\":[],\"control_bindings\":[],"
              "\"component_attachments\":[]}"))),
      {}));
  const auto composition = compose_cell_occurrences(
      graph, std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto inputs = compute_capability_provider_binding_revisions(
      decode_deployment_applicability_inputs_candidate(
          parse_strict_json_model_yaml(
              "{\"api_version\":\"manyforge.deployment.v1\","
              "\"desired_control_configurations\":[],"
              "\"capability_provider_bindings\":[],"
              "\"external_resources\":[]}")));
  const auto overlay = select_site_overlay_for_deployment(
      inputs.value().site_overlay_ref, inputs.value().external_resources,
      nullptr);
  const auto result = qualify_cell_applicability(
      composition, inputs, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, overlay);

  EXPECT_EQ(result.composition().graph().cell_id,
            core::CellId("empty_cell"));
  EXPECT_FALSE(result.selected_site_overlay_ref().has_value());
  EXPECT_TRUE(result.binding_interfaces().empty());
  EXPECT_TRUE(result.controllers().empty());
  EXPECT_TRUE(result.routes().empty());
  EXPECT_TRUE(result.capabilities().empty());
  EXPECT_TRUE(result.stop_applicability().empty());
}

TEST(CellApplicability,
     RvalueTransfersPreserveAllQualifiedApplicabilityRootsAndReuse) {
  const auto fixture = make_fixture({.include_provider_binding = true});

  test::expect_source_preserving_rvalue_transfers(
      fixture.composition, [](const CompositionQualifiedCellIr& value) {
        return std::tuple{
            value.verified_graph().normalized_root(),
            emit_canonical_urdf_document(value.flattened_urdf()),
            emit_canonical_srdf_document(value.flattened_srdf()),
            value.model_occurrences().size(), value.targets().size()};
      });
  test::expect_immutable_shared_copy_storage(
      fixture.composition,
      [](const CompositionQualifiedCellIr& value)
          -> const UrdfDocumentIr& { return value.flattened_urdf(); });
  test::expect_source_preserving_rvalue_transfers(
      fixture.inputs,
      [](const RevisionVerifiedDeploymentApplicabilityInputs& value) {
        return std::tuple{
            value.value(),
            value.normalized_desired_control_configurations(),
            value.normalized_capability_provider_bindings(),
            value.normalized_external_resources(),
            value.normalized_site_overlay_ref(),
            value.external_resources_was_present()};
      });
  test::expect_immutable_shared_copy_storage(
      fixture.inputs,
      [](const RevisionVerifiedDeploymentApplicabilityInputs& value)
          -> const core::DeploymentApplicabilityInputsDTO& {
        return value.value();
      });

  ASSERT_FALSE(fixture.runtime_profiles.empty());
  test::expect_source_preserving_rvalue_transfers(
      fixture.runtime_profiles.front(),
      [](const QualifiedRuntimeProfile& value) {
        return std::tuple{value.reference(), value.value(),
                          value.normalized_root()};
      });
  test::expect_immutable_shared_copy_storage(
      fixture.runtime_profiles.front(),
      [](const QualifiedRuntimeProfile& value)
          -> const CanonicalValue& { return value.normalized_root(); });
  ASSERT_FALSE(fixture.capabilities.empty());
  test::expect_source_preserving_rvalue_transfers(
      fixture.capabilities.front(),
      [](const QualifiedCapabilityDefinition& value) {
        return std::tuple{value.reference(), value.value(),
                          value.normalized_root()};
      });
  test::expect_immutable_shared_copy_storage(
      fixture.capabilities.front(),
      [](const QualifiedCapabilityDefinition& value)
          -> const CanonicalValue& { return value.normalized_root(); });
  ASSERT_FALSE(fixture.contracts.empty());
  test::expect_source_preserving_rvalue_transfers(
      fixture.contracts.front(),
      [](const QualifiedControlInterfaceContract& value) {
        return std::tuple{value.reference(), value.value(),
                          value.normalized_root()};
      });
  test::expect_immutable_shared_copy_storage(
      fixture.contracts.front(),
      [](const QualifiedControlInterfaceContract& value)
          -> const CanonicalValue& { return value.normalized_root(); });
  ASSERT_FALSE(fixture.topologies.empty());
  test::expect_source_preserving_rvalue_transfers(
      fixture.topologies.front(),
      [](const QualifiedControllerTopology& value) {
        return std::tuple{value.reference(), value.value(),
                          value.normalized_root()};
      });
  test::expect_immutable_shared_copy_storage(
      fixture.topologies.front(),
      [](const QualifiedControllerTopology& value)
          -> const CanonicalValue& { return value.normalized_root(); });
  ASSERT_FALSE(fixture.manifests.empty());
  test::expect_source_preserving_rvalue_transfers(
      fixture.manifests.front(),
      [](const QualifiedProviderManifest& value) {
        return std::tuple{value.reference(), value.value(),
                          value.normalized_root()};
      });
  test::expect_immutable_shared_copy_storage(
      fixture.manifests.front(),
      [](const QualifiedProviderManifest& value)
          -> const CanonicalValue& { return value.normalized_root(); });
  ASSERT_FALSE(fixture.stop_policies.empty());
  test::expect_source_preserving_rvalue_transfers(
      fixture.stop_policies.front(),
      [](const QualifiedStopEvidencePolicy& value) {
        return std::tuple{value.reference(), value.value(),
                          value.normalized_root()};
      });
  test::expect_immutable_shared_copy_storage(
      fixture.stop_policies.front(),
      [](const QualifiedStopEvidencePolicy& value)
          -> const CanonicalValue& { return value.normalized_root(); });

  ASSERT_TRUE(fixture.selected_overlay.has_selection());
  const auto overlay_projection =
      [](const QualifiedSelectedSiteOverlay& value) {
        const auto* reference = value.reference();
        return std::tuple{
            value.has_selection(),
            reference == nullptr
                ? std::string{}
                : reference->site_overlay_id.value(),
            std::vector<core::SiteOverlayExternalResourceKeyDTO>(
                value.external_resource_keys().begin(),
                value.external_resource_keys().end())};
      };
  test::expect_source_preserving_rvalue_transfers(
      fixture.selected_overlay, overlay_projection);

  auto applicability_source = qualify(fixture);
  test::expect_source_preserving_rvalue_transfers(
      applicability_source, [](const ApplicabilityQualifiedCellIr& value) {
        return std::tuple{
            value.composition().verified_graph().normalized_root(),
            value.binding_interfaces().size(), value.controllers().size(),
            value.routes().size(), value.capabilities().size(),
            value.provider_endpoints().size(),
            value.external_resources().size(),
            value.stop_applicability().size()};
      });
  test::expect_immutable_shared_copy_storage(
      applicability_source,
      [](const ApplicabilityQualifiedCellIr& value)
          -> const CompositionQualifiedCellIr& {
        return value.composition();
      });

  const auto applicability_destination = std::move(applicability_source);
  EXPECT_EQ(applicability_source.composition().graph(),
            applicability_destination.composition().graph());
  EXPECT_EQ(applicability_source.binding_interfaces().size(),
            applicability_destination.binding_interfaces().size());

  // The same non-empty exact predecessors remain valid after rvalue syntax.
  const auto first_requalified = qualify(fixture);
  const auto second_requalified = qualify(fixture);
  EXPECT_EQ(first_requalified.composition().graph(),
            second_requalified.composition().graph());
  EXPECT_EQ(first_requalified.binding_interfaces().size(),
            second_requalified.binding_interfaces().size());

  auto compiler_input = qualify(fixture);
  const auto first_compiled = compile_cell_ir(std::move(compiler_input));
  const auto second_compiled = compile_cell_ir(std::move(compiler_input));
  EXPECT_EQ(compiler_input.composition().graph(),
            first_compiled.applicability().composition().graph());
  EXPECT_EQ(first_compiled.applicability().composition().graph(),
            second_compiled.applicability().composition().graph());
  EXPECT_EQ(first_compiled.applicability().deployment_inputs().value(),
            second_compiled.applicability().deployment_inputs().value());
  EXPECT_EQ(first_compiled.applicability().runtime_profiles().size(),
            second_compiled.applicability().runtime_profiles().size());
  EXPECT_EQ(first_compiled.applicability().binding_interfaces().size(),
            second_compiled.applicability().binding_interfaces().size());
  EXPECT_EQ(first_compiled.applicability().controllers().size(),
            second_compiled.applicability().controllers().size());
  EXPECT_EQ(first_compiled.applicability().routes().size(),
            second_compiled.applicability().routes().size());
  EXPECT_EQ(first_compiled.applicability().capabilities().size(),
            second_compiled.applicability().capabilities().size());
  EXPECT_EQ(first_compiled.applicability().provider_endpoints().size(),
            second_compiled.applicability().provider_endpoints().size());
  EXPECT_EQ(first_compiled.applicability().stop_applicability().size(),
            second_compiled.applicability().stop_applicability().size());

  const auto& retained = first_compiled.applicability();
  std::vector<core::CatalogRecordRefDTO> expected_references;
  for (const auto& asset : retained.composition().model_assets()) {
    expected_references.emplace_back(asset.qualified_asset().reference());
  }
  const auto append_references =
      [&expected_references](const auto& qualified_values) {
        for (const auto& value : qualified_values) {
          expected_references.emplace_back(value.reference());
        }
      };
  append_references(retained.composition().component_types());
  append_references(retained.runtime_profiles());
  append_references(retained.capability_definitions());
  append_references(retained.control_interface_contracts());
  append_references(retained.controller_topologies());
  append_references(retained.provider_manifests());
  append_references(retained.stop_evidence_policies());
  std::sort(expected_references.begin(), expected_references.end(),
            core::CanonicalCatalogRecordRefLess{});

  ASSERT_EQ(first_compiled.catalog_record_refs().size(),
            expected_references.size());
  EXPECT_TRUE(std::is_sorted(first_compiled.catalog_record_refs().begin(),
                             first_compiled.catalog_record_refs().end(),
                             core::CanonicalCatalogRecordRefLess{}));
  for (std::size_t index = 0U; index < expected_references.size();
       ++index) {
    expect_exact_catalog_reference(first_compiled.catalog_record_refs()[index],
                                   expected_references[index]);
  }

  for (const auto& asset : retained.composition().model_assets()) {
    EXPECT_EQ(asset.qualified_asset().reference(),
              asset.retrieval_plan().qualified_asset().reference());
    const core::CatalogRecordRefDTO source_reference =
        asset.retrieval_plan().qualified_asset().reference();
    EXPECT_NE(std::find(first_compiled.catalog_record_refs().begin(),
                        first_compiled.catalog_record_refs().end(),
                        source_reference),
              first_compiled.catalog_record_refs().end());
  }

  const auto shuffled_fixture = make_fixture(
      {.include_provider_binding = true, .reverse_source_order = true});
  const auto shuffled_applicability = qualify(shuffled_fixture);
  const auto shuffled_compiled = compile_cell_ir(shuffled_applicability);
  EXPECT_EQ(shuffled_compiled.catalog_record_refs(),
            first_compiled.catalog_record_refs());

  const auto detached_compiled = [] {
    const auto local_fixture =
        make_fixture({.include_provider_binding = true});
    const auto local_applicability = qualify(local_fixture);
    return compile_cell_ir(local_applicability);
  }();
  EXPECT_EQ(detached_compiled.catalog_record_refs(),
            first_compiled.catalog_record_refs());
  EXPECT_EQ(detached_compiled.applicability().composition().graph(),
            first_compiled.applicability().composition().graph());

  test::expect_source_preserving_rvalue_transfers(
      first_compiled, [](const CompilerQualifiedCellIr& value) {
        const auto& applicability = value.applicability();
        return std::tuple{
            applicability.composition().verified_graph().normalized_root(),
            applicability.deployment_inputs()
                .normalized_capability_provider_bindings(),
            applicability.runtime_profiles().size(),
            applicability.capability_definitions().size(),
            applicability.control_interface_contracts().size(),
            applicability.controller_topologies().size(),
            applicability.provider_manifests().size(),
            applicability.stop_evidence_policies().size(),
            applicability.binding_interfaces().size(),
            applicability.controllers().size(),
            applicability.routes().size(),
            applicability.capabilities().size(),
            applicability.provider_endpoints().size(),
            applicability.external_resources().size(),
            applicability.stop_applicability().size(),
            value.catalog_record_refs()};
      });
}

TEST(CellApplicability,
     DirectBindingRetainsDesiredControllerRouteClaimsAndNotStoppableStop) {
  const auto fixture = make_fixture();
  const auto result = qualify(fixture);

  ASSERT_EQ(result.binding_interfaces().size(), 1U);
  ASSERT_EQ(result.binding_interfaces().front().command_claims.size(), 1U);
  ASSERT_EQ(result.binding_interfaces().front().state_claims.size(), 1U);
  EXPECT_EQ(result.binding_interfaces().front().command_claims.front()
                .resource_id,
            core::ResourceId("axis_a"));
  EXPECT_EQ(result.binding_interfaces().front().command_claims.front()
                .interface_name,
            "position");
  ASSERT_EQ(result.controllers().size(), 1U);
  ASSERT_EQ(result.controllers().front().binding_projections.size(), 1U);
  ASSERT_EQ(result.routes().size(), 1U);
  EXPECT_TRUE(result.routes().front().path_edges.empty());
  ASSERT_EQ(result.routes().front().endpoint_templates.size(), 2U);
  ASSERT_EQ(result.stop_applicability().size(), 1U);
  ASSERT_TRUE(
      std::holds_alternative<LogicalNotStoppableApplicabilityTemplate>(
          result.stop_applicability().front()));
  EXPECT_EQ(
      std::get<LogicalNotStoppableApplicabilityTemplate>(
          result.stop_applicability().front())
          .disposition,
      core::StopEvidenceDisposition::kNotStoppable);
}

TEST(CellApplicability,
     EqualCommandAndStateClaimsMintDirectionDistinctInterfaceSymbols) {
  const auto result = qualify(make_fixture());
  ASSERT_EQ(result.binding_interfaces().size(), 1U);
  const auto& binding = result.binding_interfaces().front();
  ASSERT_EQ(binding.command_claims.size(), 1U);
  ASSERT_EQ(binding.state_claims.size(), 1U);
  EXPECT_EQ(binding.command_claims.front().resource_role_id,
            binding.state_claims.front().resource_role_id);
  EXPECT_EQ(binding.command_claims.front().resource_id,
            binding.state_claims.front().resource_id);
  EXPECT_EQ(binding.command_claims.front().interface_name,
            binding.state_claims.front().interface_name);
  EXPECT_NE(binding.command_claims.front().logical_interface_symbol,
            binding.state_claims.front().logical_interface_symbol);
}

TEST(CellApplicability,
     RouteEndpointOnBothSidesProducesOneDualMembershipTemplate) {
  const auto result = qualify(make_fixture({
      .control_provider_offer_case =
          ControlProviderOfferCase::kDualSideRole,
      .control_endpoint_projection_case =
          ControlEndpointProjectionCase::kDualSideRole,
  }));
  ASSERT_EQ(result.routes().size(), 1U);
  const auto& endpoints = result.routes().front().endpoint_templates;
  const auto shared = std::find_if(
      endpoints.begin(), endpoints.end(), [](const auto& endpoint) {
        return endpoint.endpoint_role_id ==
               core::EndpointRoleId("shared");
      });
  ASSERT_NE(shared, endpoints.end());
  EXPECT_TRUE(shared->is_command_endpoint);
  EXPECT_TRUE(shared->is_state_endpoint);
  EXPECT_EQ(std::count_if(
                endpoints.begin(), endpoints.end(),
                [](const auto& endpoint) {
                  return endpoint.endpoint_role_id ==
                         core::EndpointRoleId("shared");
                }),
            1);
}

TEST(CellApplicability,
     QualifiedTopologyRouteCannotSelectAnotherBindingContractRevision) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .topology_contract_mismatch = true,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     CommandOnlyBindingRejectsStateAuthorityStopEvidenceRequirement) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .binding_has_state_interfaces = false,
          .stop_form = StopForm::kBindingStateAuthority,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     ControlBindingEndpointProjectionMustEqualTheCompleteProviderOffer) {
  for (const auto projection_case : {
           ControlEndpointProjectionCase::kMissingRole,
           ControlEndpointProjectionCase::kExtraRole,
       }) {
    FixtureOptions options;
    options.control_endpoint_projection_case = projection_case;
    EXPECT_THROW((void)qualify(make_fixture(options)),
                 CellApplicabilityError);
  }
}

TEST(CellApplicability,
     ControlBindingProviderManifestMustOfferTheSelectedCapability) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .control_provider_offer_case =
              ControlProviderOfferCase::kMissingCapability,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     PerBindingDirectionBoundAcceptsExact65536AndRejectsOneProductOver) {
  const auto exact = qualify(make_fixture({
      .resource_count = 256U,
      .interface_count = 256U,
  }));
  ASSERT_EQ(exact.binding_interfaces().size(), 1U);
  EXPECT_EQ(exact.binding_interfaces().front().command_claims.size(),
            kMaxExpandedControlInterfaceClaimsPerDirection);
  EXPECT_EQ(exact.binding_interfaces().front().state_claims.size(),
            kMaxExpandedControlInterfaceClaimsPerDirection);

  const auto exact_plus_one = make_fixture({
      .resource_count = 256U,
      .interface_count = 256U,
      .include_plus_one_role = true,
  });
  static_assert(256U * 256U + 1U ==
                kMaxExpandedControlInterfaceClaimsPerDirection + 1U);
  EXPECT_THROW((void)qualify(exact_plus_one), CellApplicabilityError);
}

TEST(CellApplicability,
     MultiRoleResourceAndInterfaceExpansionUsesTheExactFrozenOrder) {
  const auto result = qualify(make_fixture({
      .resource_count = 2U,
      .interface_count = 2U,
      .include_plus_one_role = true,
      .expand_second_role = true,
  }));
  ASSERT_EQ(result.binding_interfaces().size(), 1U);
  const auto& binding = result.binding_interfaces().front();
  const auto projection = [](const auto& claims) {
    std::vector<std::string> values;
    values.reserve(claims.size());
    for (const auto& claim : claims) {
      values.push_back(claim.resource_role_id.value() + "/" +
                       claim.resource_id.value() + "/" +
                       claim.interface_name);
    }
    return values;
  };
  const std::vector<std::string> expected{
      "axis/axis_a_0/position_0",
      "axis/axis_a_0/position_1",
      "axis/axis_a_1/position_0",
      "axis/axis_a_1/position_1",
      "trigger/trigger_a_0/trigger_0",
      "trigger/trigger_a_0/trigger_1",
      "trigger/trigger_a_1/trigger_0",
      "trigger/trigger_a_1/trigger_1",
  };
  EXPECT_EQ(projection(binding.command_claims), expected);
  EXPECT_EQ(projection(binding.state_claims), expected);
}

TEST(CellApplicability,
     MultiRolePipelineRejectsContractAndResourceOrderMutationsBeforeEmission) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .resource_count = 2U,
          .interface_count = 2U,
          .include_plus_one_role = true,
          .expand_second_role = true,
          .reverse_contract_role_order = true,
      })),
      ControlInterfaceContractCodecError);
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .resource_count = 2U,
          .interface_count = 2U,
          .include_plus_one_role = true,
          .expand_second_role = true,
          .resource_binding_case =
              ResourceBindingCase::kMisorderedRoleGroups,
      })),
      CellCompositionError);
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .resource_count = 2U,
          .interface_count = 2U,
          .include_plus_one_role = true,
          .expand_second_role = true,
          .resource_binding_case =
              ResourceBindingCase::kReopenedRoleGroup,
      })),
      CellCompositionError);
}

TEST(CellApplicability,
     RepeatedBindingsRoutesAndActivationSetsRetainEveryProvenanceTuple) {
  const auto fixture = make_fixture({
      .binding_count = 2U,
      .repeated_routes_and_activation_sets = true,
  });
  const auto result = qualify(fixture);

  ASSERT_EQ(result.binding_interfaces().size(), 2U);
  ASSERT_EQ(result.controllers().size(), 2U);
  EXPECT_EQ(result.controllers()[0].binding_projections.size(), 8U);
  EXPECT_EQ(result.controllers()[1].binding_projections.size(), 4U);
  EXPECT_EQ(result.routes().size(), 4U);
  EXPECT_EQ(result.stop_applicability().size(), 4U);
  EXPECT_NE(result.binding_interfaces()[0].command_claims[0]
                .logical_interface_symbol,
            result.binding_interfaces()[1].command_claims[0]
                .logical_interface_symbol);
}

TEST(CellApplicability,
     MultiEdgeChainedRouteRetainsExactOrderedRolesEdgesAndProjections) {
  const auto result = qualify(make_fixture({.chained_route = true}));

  ASSERT_EQ(result.routes().size(), 1U);
  const auto& route = result.routes().front();
  EXPECT_EQ(
      route.controller_role_ids,
      (std::vector<core::ControllerRoleId>{
          core::ControllerRoleId("planner"),
          core::ControllerRoleId("filter"),
          core::ControllerRoleId("hardware")}));
  EXPECT_EQ(
      route.path_edges,
      (std::vector<core::ControllerChainEdgeRefDTO>{
          {core::ControllerRoleId("planner"),
           core::ControllerRoleId("filter")},
          {core::ControllerRoleId("filter"),
           core::ControllerRoleId("hardware")}}));
  ASSERT_EQ(result.controllers().size(), 3U);
  for (const auto& controller : result.controllers()) {
    ASSERT_EQ(controller.binding_projections.size(), 1U);
    EXPECT_EQ(controller.binding_projections.front().control_route_id,
              core::ControlRouteId("route_a"));
    EXPECT_EQ(controller.binding_projections.front().activation_set_id,
              core::ActivationSetId("set_a"));
  }
}

TEST(CellApplicability,
     IndependentCommandOnlyAndStateOnlyDeclarationsRemainQualifiedWithoutAStateOnlyBinding) {
  const auto result =
      qualify(make_fixture({.directional_declarations = true}));

  ASSERT_EQ(result.control_interface_contracts().size(), 3U);
  const auto command_contract = std::find_if(
      result.control_interface_contracts().begin(),
      result.control_interface_contracts().end(), [](const auto& value) {
        return value.value().control_interface_contract_id ==
               core::ControlInterfaceContractId("axis_command_only");
      });
  const auto state_contract = std::find_if(
      result.control_interface_contracts().begin(),
      result.control_interface_contracts().end(), [](const auto& value) {
        return value.value().control_interface_contract_id ==
               core::ControlInterfaceContractId("axis_state_only");
      });
  ASSERT_NE(command_contract, result.control_interface_contracts().end());
  ASSERT_NE(state_contract, result.control_interface_contracts().end());
  EXPECT_FALSE(command_contract->value().command_interfaces.empty());
  EXPECT_TRUE(command_contract->value().state_interfaces.empty());
  EXPECT_TRUE(state_contract->value().command_interfaces.empty());
  EXPECT_FALSE(state_contract->value().state_interfaces.empty());

  ASSERT_EQ(result.binding_interfaces().size(), 1U);
  EXPECT_EQ(result.binding_interfaces().front()
                .control_interface_contract_id,
            core::ControlInterfaceContractId("axis_position"));
  ASSERT_EQ(result.controllers().size(), 3U);
  for (const auto& controller : result.controllers()) {
    ASSERT_EQ(controller.binding_projections.size(), 1U);
  }
}

TEST(CellApplicability,
     ShuffledEquivalentInputsProduceIdenticalCanonicalLogicalTables) {
  const auto forward = qualify(make_fixture({
      .binding_count = 2U,
      .repeated_routes_and_activation_sets = true,
  }));
  auto reverse_fixture = make_fixture({
      .binding_count = 2U,
      .repeated_routes_and_activation_sets = true,
      .reverse_source_order = true,
  });
  std::reverse(reverse_fixture.capabilities.begin(),
               reverse_fixture.capabilities.end());
  std::reverse(reverse_fixture.contracts.begin(),
               reverse_fixture.contracts.end());
  std::reverse(reverse_fixture.topologies.begin(),
               reverse_fixture.topologies.end());
  std::reverse(reverse_fixture.manifests.begin(),
               reverse_fixture.manifests.end());
  std::reverse(reverse_fixture.stop_policies.begin(),
               reverse_fixture.stop_policies.end());
  const auto reverse = qualify(reverse_fixture);

  EXPECT_EQ(forward.binding_interfaces(), reverse.binding_interfaces());
  EXPECT_EQ(forward.controllers(), reverse.controllers());
  EXPECT_EQ(forward.routes(), reverse.routes());
  EXPECT_EQ(forward.capabilities(), reverse.capabilities());
  EXPECT_EQ(forward.stop_applicability(), reverse.stop_applicability());
}

TEST(CellApplicability,
     ExactQualifiedClosuresRejectMissingDuplicateAndUnusedReferents) {
  const auto fixture = make_fixture();
  EXPECT_THROW(
      (void)qualify_cell_applicability(
          fixture.composition, fixture.inputs, fixture.runtime_profiles,
          {}, {}, fixture.capabilities,
          std::span<const QualifiedControlInterfaceContract>{},
          fixture.topologies, fixture.manifests, fixture.stop_policies,
          {}, {},
          fixture.selected_overlay),
      CellApplicabilityError);

  auto duplicate_contracts = fixture.contracts;
  duplicate_contracts.push_back(fixture.contracts.front());
  EXPECT_THROW(
      (void)qualify_cell_applicability(
          fixture.composition, fixture.inputs, fixture.runtime_profiles,
          {}, {}, fixture.capabilities, duplicate_contracts,
          fixture.topologies,
          fixture.manifests, fixture.stop_policies,
          {}, {},
          fixture.selected_overlay),
      CellApplicabilityError);

  auto extra_contracts = fixture.contracts;
  extra_contracts.push_back(qualify_contract("unused_axis_contract"));
  EXPECT_THROW(
      (void)qualify_cell_applicability(
          fixture.composition, fixture.inputs, fixture.runtime_profiles,
          {}, {}, fixture.capabilities, extra_contracts, fixture.topologies,
          fixture.manifests, fixture.stop_policies,
          {}, {},
          fixture.selected_overlay),
      CellApplicabilityError);
}

TEST(CellApplicability,
     EveryQualifiedFamilyClosureRejectsMissingDuplicateUnusedAndStaleRows) {
  const auto reject =
      [](const ApplicabilityFixture& fixture,
         const std::vector<QualifiedRuntimeProfile>& runtime_profiles,
         const std::vector<QualifiedCapabilityDefinition>& capabilities,
         const std::vector<QualifiedControlInterfaceContract>& contracts,
         const std::vector<QualifiedControllerTopology>& topologies,
         const std::vector<QualifiedProviderManifest>& manifests,
         const std::vector<QualifiedStopEvidencePolicy>& stop_policies) {
        EXPECT_THROW(
            (void)qualify_cell_applicability(
                fixture.composition, fixture.inputs, runtime_profiles,
                {}, {}, capabilities, contracts, topologies, manifests,
                stop_policies, {}, {}, fixture.selected_overlay),
            CellApplicabilityError);
      };

  const auto runtime_fixture =
      make_fixture({.include_provider_binding = true});
  reject(runtime_fixture, {}, runtime_fixture.capabilities,
         runtime_fixture.contracts, runtime_fixture.topologies,
         runtime_fixture.manifests, runtime_fixture.stop_policies);
  auto duplicate_runtimes = runtime_fixture.runtime_profiles;
  duplicate_runtimes.push_back(runtime_fixture.runtime_profiles.front());
  reject(runtime_fixture, duplicate_runtimes,
         runtime_fixture.capabilities, runtime_fixture.contracts,
         runtime_fixture.topologies, runtime_fixture.manifests,
         runtime_fixture.stop_policies);
  auto unused_runtimes = runtime_fixture.runtime_profiles;
  unused_runtimes.push_back(
      qualify_runtime_profile("unused_runtime_profile"));
  reject(runtime_fixture, unused_runtimes, runtime_fixture.capabilities,
         runtime_fixture.contracts, runtime_fixture.topologies,
         runtime_fixture.manifests, runtime_fixture.stop_policies);
  reject(runtime_fixture,
         {qualify_runtime_profile("planner_runtime_profile",
                                  "alternate_status")},
         runtime_fixture.capabilities, runtime_fixture.contracts,
         runtime_fixture.topologies, runtime_fixture.manifests,
         runtime_fixture.stop_policies);

  const auto fixture = make_fixture();
  reject(fixture, fixture.runtime_profiles, {}, fixture.contracts,
         fixture.topologies, fixture.manifests, fixture.stop_policies);
  auto duplicate_capabilities = fixture.capabilities;
  duplicate_capabilities.push_back(fixture.capabilities.front());
  reject(fixture, fixture.runtime_profiles, duplicate_capabilities,
         fixture.contracts, fixture.topologies, fixture.manifests,
         fixture.stop_policies);
  auto unused_capabilities = fixture.capabilities;
  unused_capabilities.push_back(qualify_capability("scene.object.query"));
  reject(fixture, fixture.runtime_profiles, unused_capabilities,
         fixture.contracts, fixture.topologies, fixture.manifests,
         fixture.stop_policies);
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .stale_control_capability_revision = true,
      })),
      CellApplicabilityError);

  reject(fixture, fixture.runtime_profiles, fixture.capabilities, {},
         fixture.topologies, fixture.manifests, fixture.stop_policies);
  auto duplicate_contracts = fixture.contracts;
  duplicate_contracts.push_back(fixture.contracts.front());
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         duplicate_contracts, fixture.topologies, fixture.manifests,
         fixture.stop_policies);
  auto unused_contracts = fixture.contracts;
  unused_contracts.push_back(qualify_contract("unused_contract"));
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         unused_contracts, fixture.topologies, fixture.manifests,
         fixture.stop_policies);
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         {qualify_contract("axis_position", 2U)}, fixture.topologies,
         fixture.manifests, fixture.stop_policies);

  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, {}, fixture.manifests,
         fixture.stop_policies);
  auto duplicate_topologies = fixture.topologies;
  duplicate_topologies.push_back(fixture.topologies.front());
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, duplicate_topologies, fixture.manifests,
         fixture.stop_policies);
  auto unused_topologies = fixture.topologies;
  unused_topologies.push_back(qualify_topology(
      fixture.contracts.front(), false, false, "unused_topology"));
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, unused_topologies, fixture.manifests,
         fixture.stop_policies);
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts,
         {qualify_topology(fixture.contracts.front(), true)},
         fixture.manifests, fixture.stop_policies);

  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, fixture.topologies, {},
         fixture.stop_policies);
  auto duplicate_manifests = fixture.manifests;
  duplicate_manifests.push_back(fixture.manifests.front());
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, fixture.topologies, duplicate_manifests,
         fixture.stop_policies);
  auto unused_manifests = fixture.manifests;
  unused_manifests.push_back(qualify_manifest(
      "unused_manifest", fixture.capabilities,
      StopForm::kNotStoppable, false));
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, fixture.topologies, unused_manifests,
         fixture.stop_policies);
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, fixture.topologies,
         {qualify_manifest("primary_provider_manifest",
                           fixture.capabilities,
                           StopForm::kProviderDefined, false)},
         fixture.stop_policies);

  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, fixture.topologies, fixture.manifests, {});
  auto duplicate_stop_policies = fixture.stop_policies;
  duplicate_stop_policies.push_back(fixture.stop_policies.front());
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, fixture.topologies, fixture.manifests,
         duplicate_stop_policies);
  auto unused_stop_policies = fixture.stop_policies;
  unused_stop_policies.push_back(
      qualify_stop_policy(StopForm::kIntrinsic));
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, fixture.topologies, fixture.manifests,
         unused_stop_policies);
  reject(fixture, fixture.runtime_profiles, fixture.capabilities,
         fixture.contracts, fixture.topologies, fixture.manifests,
         {qualify_stop_policy(StopForm::kNotStoppable,
                              "controller_set")});
}

TEST(CellApplicability, DesiredConfigurationCoverageMustBeExact) {
  const auto fixture = make_fixture({.omit_desired = true});
  EXPECT_THROW((void)qualify(fixture), CellApplicabilityError);
}

TEST(CellApplicability,
     DesiredConfigurationRejectsDuplicateStaleAndExtraRowsIndependently) {
  EXPECT_THROW(
      (void)qualify(make_fixture({.duplicate_desired = true})),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)qualify(
          make_fixture({.stale_desired_binding_revision = true})),
      CellApplicabilityError);
  EXPECT_THROW(
      (void)qualify(make_fixture({.extra_desired_binding_route = true})),
      CellApplicabilityError);
}

TEST(CellApplicability,
     DesiredConfigurationRejectsWrongDomainTopologyActivationAndRoute) {
  for (const auto selection_case : {
           DesiredSelectionCase::kWrongDomain,
           DesiredSelectionCase::kWrongTopology,
           DesiredSelectionCase::kUnknownActivationSet,
           DesiredSelectionCase::kUnknownRoute,
       }) {
    EXPECT_THROW(
        (void)qualify(make_fixture({
            .desired_selection_case = selection_case,
        })),
        CellApplicabilityError);
  }
}

TEST(CellApplicability,
     DesiredConfigurationRejectsIndividuallyLegalRoutesWithoutOneCommonActivationSet) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .binding_count = 2U,
          .repeated_routes_and_activation_sets = true,
          .incompatible_route_activation_sets = true,
          .desired_selection_case =
              DesiredSelectionCase::kNoCommonActivationSet,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     QualifiedClosureRejectsStaleReplacementAndMissingRuntimeReferent) {
  const auto fixture = make_fixture();
  const std::vector<QualifiedControlInterfaceContract> stale_contract{
      qualify_contract("axis_position", 2U)};
  EXPECT_THROW(
      (void)qualify_cell_applicability(
          fixture.composition, fixture.inputs, fixture.runtime_profiles,
          {}, {}, fixture.capabilities, stale_contract, fixture.topologies,
          fixture.manifests, fixture.stop_policies,
          {}, {},
          fixture.selected_overlay),
      CellApplicabilityError);

  const auto runtime_fixture =
      make_fixture({.include_provider_binding = true});
  EXPECT_THROW(
      (void)qualify_cell_applicability(
          runtime_fixture.composition, runtime_fixture.inputs,
          std::span<const QualifiedRuntimeProfile>{},
          {}, {}, runtime_fixture.capabilities, runtime_fixture.contracts,
          runtime_fixture.topologies, runtime_fixture.manifests,
          runtime_fixture.stop_policies,
          {}, {},
          runtime_fixture.selected_overlay),
      CellApplicabilityError);
}

TEST(CellApplicability,
     OneProviderInstanceCannotSelectTwoExactManifestPairs) {
  const auto fixture = make_fixture({
      .provider_instance_conflict =
          ProviderInstanceConflictCase::kControlExternal,
  });
  EXPECT_THROW((void)qualify(fixture), CellApplicabilityError);
}

TEST(CellApplicability,
     AssistantOnlyExternalResourceIsValidatedAndRetainedWithoutAProviderBinding) {
  const auto result = qualify(make_fixture({
      .include_assistant_only_external = true,
  }));

  EXPECT_TRUE(result.provider_endpoints().empty());
  ASSERT_EQ(result.external_resources().size(), 1U);
  EXPECT_EQ(result.external_resources().front().external_resource_alias,
            core::ExternalResourceAlias("assistant_service"));
  EXPECT_EQ(result.external_resources().front().provider_instance_id,
            core::ProviderInstanceId("assistant_provider"));
  EXPECT_EQ(result.external_resources().front().endpoint_role_id,
            core::EndpointRoleId("state"));
  EXPECT_EQ(result.external_resources().front().site_overlay_value_ref,
            core::SiteOverlayValueRef("resources.assistant_service"));
}

TEST(CellApplicability,
     ProviderTargetRejectsAnOtherwiseRevisionVerifiedStaleOccurrenceRevision) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .include_provider_binding = true,
          .provider_target_case =
              ProviderTargetCase::kStaleTargetRevision,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     CapabilityProviderBindingRetainsRuntimeAndExternalSourcesWithoutValues) {
  const auto fixture = make_fixture({.include_provider_binding = true});
  const auto result = qualify(fixture);

  ASSERT_TRUE(result.selected_site_overlay_ref().has_value());
  ASSERT_EQ(result.provider_endpoints().size(), 2U);
  const auto& first = result.provider_endpoints()[0];
  const auto& second = result.provider_endpoints()[1];
  ASSERT_TRUE(std::holds_alternative<ExternalEndpointSourceTemplate>(
      first.source));
  ASSERT_TRUE(std::holds_alternative<RuntimeEndpointSourceTemplate>(
      second.source));
  const auto& external =
      std::get<ExternalEndpointSourceTemplate>(first.source);
  EXPECT_EQ(external.site_overlay_value_ref,
            core::SiteOverlayValueRef("resources.planning_service"));
  EXPECT_EQ(external.selected_site_overlay_id,
            result.selected_site_overlay_ref()->site_overlay_id);
  EXPECT_EQ(external.selected_site_overlay_revision,
            result.selected_site_overlay_ref()->site_overlay_revision);
  EXPECT_EQ(external.selected_site_overlay_non_secret_digest,
            result.selected_site_overlay_ref()->non_secret_digest);
  ASSERT_EQ(result.external_resources().size(), 1U);
  EXPECT_EQ(result.external_resources().front().selected_site_overlay_id,
            result.selected_site_overlay_ref()->site_overlay_id);
  EXPECT_EQ(
      result.external_resources().front().selected_site_overlay_revision,
      result.selected_site_overlay_ref()->site_overlay_revision);
  EXPECT_EQ(
      result.external_resources()
          .front()
          .selected_site_overlay_non_secret_digest,
      result.selected_site_overlay_ref()->non_secret_digest);
  EXPECT_EQ(result.external_resources().front().external_resource_alias,
            core::ExternalResourceAlias("planning_service"));
}

TEST(CellApplicability,
     Mf1SymbolsUseTheExactFrozenFieldPreimagesAndNeverCarryOverlayValues) {
  const auto result = qualify(make_fixture({
      .include_provider_binding = true,
      .stop_form = StopForm::kProviderDefined,
  }));
  const auto& binding = result.binding_interfaces().front();
  const auto& command_claim = binding.command_claims.front();
  EXPECT_EQ(
      command_claim.logical_interface_symbol,
      expected_mf1_name(
          "interface",
          {{"cell_id", "applicability_cell"},
           {"kind", "interface"},
           {"control_binding_id", binding.control_binding_id.value()},
           {"control_binding_revision",
            binding.control_binding_revision.value()},
           {"direction", "command"},
           {"resource_role_id", command_claim.resource_role_id.value()},
           {"resource_id", command_claim.resource_id.value()},
           {"interface_name", command_claim.interface_name}}));

  const auto& controller = result.controllers().front();
  EXPECT_EQ(
      controller.logical_controller_symbol,
      expected_mf1_name(
          "controller",
          {{"cell_id", "applicability_cell"},
           {"kind", "controller"},
           {"control_domain_id", controller.control_domain_id.value()},
           {"control_domain_revision",
            controller.control_domain_revision.value()},
           {"controller_topology_id",
            controller.controller_topology_id.value()},
           {"topology_revision", controller.topology_revision.value()},
           {"controller_role_id",
            controller.controller_role_id.value()}}));

  const auto& route = result.routes().front();
  EXPECT_EQ(
      route.logical_route_symbol,
      expected_mf1_name(
          "route",
          {{"cell_id", "applicability_cell"},
           {"kind", "route"},
           {"control_binding_id", route.control_binding_id.value()},
           {"control_binding_revision",
            route.control_binding_revision.value()},
           {"control_route_id", route.control_route_id.value()}}));
  ASSERT_FALSE(route.endpoint_templates.empty());
  const auto& route_endpoint = route.endpoint_templates.front();
  EXPECT_EQ(
      route_endpoint.logical_endpoint_symbol,
      expected_mf1_name(
          "route_endpoint",
          {{"cell_id", "applicability_cell"},
           {"kind", "route_endpoint"},
           {"control_binding_id", route.control_binding_id.value()},
           {"control_binding_revision",
            route.control_binding_revision.value()},
           {"control_route_id", route.control_route_id.value()},
           {"endpoint_role_id",
            route_endpoint.endpoint_role_id.value()}}));

  const auto control_capability = std::find_if(
      result.capabilities().begin(), result.capabilities().end(),
      [](const auto& capability) {
        return std::holds_alternative<ControlBindingCapabilityOwner>(
            capability.owner);
      });
  ASSERT_NE(control_capability, result.capabilities().end());
  const auto& control_owner =
      std::get<ControlBindingCapabilityOwner>(
          control_capability->owner);
  EXPECT_EQ(
      control_capability->logical_capability_symbol,
      expected_mf1_name(
          "capability",
          {{"cell_id", "applicability_cell"},
           {"kind", "capability"},
           {"owner_kind", "control_binding"},
           {"owner_id", control_owner.control_binding_id.value()},
           {"owner_revision",
            control_owner.control_binding_revision.value()},
           {"capability_id",
            control_capability->capability_id.value()},
           {"capability_definition_revision",
            control_capability->capability_definition_revision.value()}}));

  const auto provider_capability = std::find_if(
      result.capabilities().begin(), result.capabilities().end(),
      [](const auto& capability) {
        return std::holds_alternative<
            CapabilityProviderCapabilityOwner>(capability.owner);
      });
  ASSERT_NE(provider_capability, result.capabilities().end());
  const auto& provider_owner =
      std::get<CapabilityProviderCapabilityOwner>(
          provider_capability->owner);
  EXPECT_EQ(
      provider_capability->logical_capability_symbol,
      expected_mf1_name(
          "capability",
          {{"cell_id", "applicability_cell"},
           {"kind", "capability"},
           {"owner_kind", "capability_provider_binding"},
           {"owner_id",
            provider_owner.capability_provider_binding_id.value()},
           {"owner_revision",
            provider_owner.capability_provider_binding_revision.value()},
           {"capability_id",
            provider_capability->capability_id.value()},
           {"capability_definition_revision",
            provider_capability->capability_definition_revision.value()}}));

  ASSERT_FALSE(result.provider_endpoints().empty());
  const auto& provider_endpoint = result.provider_endpoints().front();
  EXPECT_EQ(
      provider_endpoint.logical_endpoint_symbol,
      expected_mf1_name(
          "provider_endpoint",
          {{"cell_id", "applicability_cell"},
           {"kind", "provider_endpoint"},
           {"capability_provider_binding_id",
            provider_endpoint.capability_provider_binding_id.value()},
           {"capability_provider_binding_revision",
            provider_endpoint.capability_provider_binding_revision.value()},
           {"endpoint_role_id",
            provider_endpoint.endpoint_role_id.value()}}));

  ASSERT_EQ(result.external_resources().size(), 1U);
  const auto& external = result.external_resources().front();
  EXPECT_EQ(
      external.logical_external_resource_symbol,
      expected_mf1_name(
          "external_resource",
          {{"cell_id", "applicability_cell"},
           {"kind", "external_resource"},
           {"external_resource_alias",
            external.external_resource_alias.value()},
           {"resource_kind", core::to_string(external.resource_kind)},
           {"provider_instance_id",
            external.provider_instance_id.value()},
           {"provider_manifest_id",
            external.provider_manifest_id.value()},
           {"provider_manifest_revision",
            external.provider_manifest_revision.value()},
           {"endpoint_role_id", external.endpoint_role_id.value()},
           {"site_overlay_value_ref",
            external.site_overlay_value_ref.value()}}));

  ASSERT_EQ(result.stop_applicability().size(), 1U);
  const auto& stop = std::get<LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front());
  EXPECT_EQ(
      stop.logical_stop_symbol,
      expected_mf1_name(
          "stop",
          {{"cell_id", "applicability_cell"},
           {"kind", "stop"},
           {"control_binding_id", stop.control_binding_id.value()},
           {"control_binding_revision",
            stop.control_binding_revision.value()},
           {"control_route_id", stop.control_route_id.value()},
           {"stop_evidence_policy_id",
            stop.stop_evidence_policy_id.value()},
           {"stop_evidence_policy_revision",
            stop.stop_evidence_policy_revision.value()}}));

  std::vector<std::string_view> generated_symbols{
      command_claim.logical_interface_symbol,
      controller.logical_controller_symbol,
      route.logical_route_symbol,
      route_endpoint.logical_endpoint_symbol,
      control_capability->logical_capability_symbol,
      provider_capability->logical_capability_symbol,
      provider_endpoint.logical_endpoint_symbol,
      external.logical_external_resource_symbol,
      stop.logical_stop_symbol,
  };
  for (const auto symbol : generated_symbols) {
    EXPECT_NE(symbol,
              result.composition().cell_frame().compiled_frame_name);
    EXPECT_NE(symbol, result.composition().flattened_urdf().robot_name);
    EXPECT_EQ(symbol.find("http://site.invalid"), std::string_view::npos);
    EXPECT_EQ(symbol.find("vault.test_token"), std::string_view::npos);
    EXPECT_EQ(symbol.find("configuration.secret"), std::string_view::npos);
  }
}

TEST(CellApplicability,
     CapabilityProviderBindingsQualifyAllThreeDefinitionDerivedScopes) {
  const auto target = qualify(make_fixture({
      .binding_count = 2U,
      .include_provider_binding = true,
      .provider_scope =
          ProviderCapabilityScopeCase::kTargetBinding,
  }));
  EXPECT_EQ(target.provider_endpoints().size(), 4U);
  EXPECT_EQ(
      std::count_if(
          target.capabilities().begin(), target.capabilities().end(),
          [](const auto& capability) {
            return capability.scope ==
                       core::CapabilityScope::kTargetBinding &&
                   std::holds_alternative<
                       CapabilityProviderCapabilityOwner>(
                       capability.owner);
          }),
      2);

  const auto cell = qualify(make_fixture({
      .include_provider_binding = true,
      .provider_scope = ProviderCapabilityScopeCase::kCellGlobal,
  }));
  EXPECT_EQ(cell.provider_endpoints().size(), 2U);
  EXPECT_EQ(
      std::count_if(
          cell.capabilities().begin(), cell.capabilities().end(),
          [](const auto& capability) {
            return capability.scope ==
                       core::CapabilityScope::kCellGlobal &&
                   std::holds_alternative<
                       CapabilityProviderCapabilityOwner>(
                       capability.owner);
          }),
      1);

  const auto domain = qualify(make_fixture({
      .include_provider_binding = true,
      .provider_scope =
          ProviderCapabilityScopeCase::kControlDomain,
  }));
  EXPECT_EQ(domain.provider_endpoints().size(), 2U);
  EXPECT_EQ(
      std::count_if(
          domain.capabilities().begin(), domain.capabilities().end(),
          [](const auto& capability) {
            return capability.scope ==
                       core::CapabilityScope::kControlDomain &&
                   std::holds_alternative<
                       CapabilityProviderCapabilityOwner>(
                       capability.owner);
          }),
      1);
}

TEST(CellApplicability,
     CapabilityProviderTargetsRejectWrongScopeWrongTargetAndCoverageGaps) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .include_provider_binding = true,
          .provider_target_case = ProviderTargetCase::kWrongScope,
      })),
      CellApplicabilityError);
  for (const auto scope : {
           ProviderCapabilityScopeCase::kTargetBinding,
           ProviderCapabilityScopeCase::kCellGlobal,
           ProviderCapabilityScopeCase::kControlDomain,
       }) {
    EXPECT_THROW(
        (void)qualify(make_fixture({
            .include_provider_binding = true,
            .provider_scope = scope,
            .provider_target_case = ProviderTargetCase::kWrongTarget,
        })),
        CellApplicabilityError);
  }
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .include_provider_binding = true,
          .provider_coverage_case = ProviderCoverageCase::kMissing,
      })),
      CellApplicabilityError);
  for (const auto scope : {
           ProviderCapabilityScopeCase::kTargetBinding,
           ProviderCapabilityScopeCase::kCellGlobal,
           ProviderCapabilityScopeCase::kControlDomain,
       }) {
    EXPECT_THROW(
        (void)qualify(make_fixture({
            .include_provider_binding = true,
            .provider_scope = scope,
            .provider_coverage_case =
                ProviderCoverageCase::kDuplicateSemanticTarget,
        })),
        CellApplicabilityError);
  }
}

TEST(CellApplicability,
     CapabilityProviderEndpointRoleClosureRejectsMissingAndExtraRoles) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .include_provider_binding = true,
          .provider_endpoint_case =
              ProviderEndpointCase::kMissingRequiredRole,
      })),
      CellApplicabilityError);
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .include_provider_binding = true,
          .provider_endpoint_case = ProviderEndpointCase::kExtraRole,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     CapabilityProviderEndpointSourcesRejectCrossWiredRuntimeAndExternalRows) {
  for (const auto endpoint_case : {
           ProviderEndpointCase::kUnknownRuntimeUnit,
           ProviderEndpointCase::kRuntimeSourceRoleOutsideProfile,
           ProviderEndpointCase::kUnknownExternalAlias,
           ProviderEndpointCase::kExternalSourceRoleMismatch,
           ProviderEndpointCase::kExistingExternalAliasAsRuntimeUnit,
           ProviderEndpointCase::kExistingRuntimeUnitAsExternalResource,
       }) {
    EXPECT_THROW(
        (void)qualify(make_fixture({
            .include_provider_binding = true,
            .provider_endpoint_case = endpoint_case,
        })),
        CellApplicabilityError);
  }
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .include_provider_binding = true,
          .external_resource_case =
              ExternalResourceCase::kEndpointRoleOutsideManifest,
      })),
      CellApplicabilityError);
  EXPECT_THROW(
      (void)make_fixture({
          .include_provider_binding = true,
          .external_resource_case =
              ExternalResourceCase::kOverlayKindMismatch,
      }),
      SiteOverlayResolutionError);
}

TEST(CellApplicability,
     DefinitionDerivedProviderBindingKindRejectsBothCrossedOwnershipPaths) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .control_uses_provider_capability = true,
      })),
      CellApplicabilityError);
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .include_provider_binding = true,
          .provider_uses_control_capability = true,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     ProviderInstanceManifestConflictsRejectForEveryCrossTablePair) {
  for (const auto conflict_case : {
           ProviderInstanceConflictCase::kControlCapabilityProvider,
           ProviderInstanceConflictCase::kCapabilityProviderExternal,
       }) {
    FixtureOptions options;
    options.include_provider_binding = true;
    options.provider_instance_conflict = conflict_case;
    EXPECT_THROW((void)qualify(make_fixture(options)),
                 CellApplicabilityError);
  }

  FixtureOptions external_external;
  external_external.provider_instance_conflict =
      ProviderInstanceConflictCase::kExternalExternal;
  EXPECT_THROW((void)qualify(make_fixture(external_external)),
               CellApplicabilityError);
}

TEST(CellApplicability,
     IntrinsicStopHasNoFabricatedProviderEndpointAndRetainsResourceScope) {
  const auto result =
      qualify(make_fixture({.stop_form = StopForm::kIntrinsic}));
  ASSERT_EQ(result.stop_applicability().size(), 1U);
  ASSERT_TRUE(std::holds_alternative<
              LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front()));
  const auto& stop = std::get<LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front());
  ASSERT_EQ(stop.escalation_stages.size(), 2U);
  const auto& nominal = stop.escalation_stages.front();
  EXPECT_FALSE(nominal.provider_command_endpoint.has_value());
  EXPECT_TRUE(nominal.provider_evidence_endpoints.empty());
  EXPECT_TRUE(nominal.binding_state_authority_sources.empty());
  ASSERT_EQ(nominal.containment_scopes.size(), 1U);
  ASSERT_TRUE(std::holds_alternative<LogicalResourceStopScopeTemplate>(
      nominal.containment_scopes.front()));
  EXPECT_EQ(
      std::get<LogicalResourceStopScopeTemplate>(
          nominal.containment_scopes.front())
          .resource_ids,
      std::vector<core::ResourceId>{core::ResourceId("axis_a")});
}

TEST(CellApplicability,
     MultiRoleResourceStopScopePreservesCompleteSemanticBindingOrder) {
  const auto result = qualify(make_fixture({
      .resource_count = 2U,
      .include_plus_one_role = true,
      .expand_second_role = true,
      .stop_form = StopForm::kIntrinsic,
  }));
  ASSERT_EQ(result.stop_applicability().size(), 1U);
  const auto& stop = std::get<LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front());
  ASSERT_EQ(stop.escalation_stages.size(), 2U);
  ASSERT_EQ(stop.escalation_stages.front().containment_scopes.size(), 1U);
  const auto& scope = std::get<LogicalResourceStopScopeTemplate>(
      stop.escalation_stages.front().containment_scopes.front());
  EXPECT_EQ(
      scope.resource_ids,
      (std::vector<core::ResourceId>{
          core::ResourceId("axis_a_0"),
          core::ResourceId("axis_a_1"),
          core::ResourceId("trigger_a_0"),
          core::ResourceId("trigger_a_1"),
      }));
}

TEST(CellApplicability,
     StoppablePolicyRetainsControllerSetAndControlDomainScopesExactly) {
  const auto controller_set_result = qualify(make_fixture({
      .repeated_routes_and_activation_sets = true,
      .stop_form = StopForm::kIntrinsic,
      .policy_containment_granularity = "controller_set",
      .binding_containment_granularity = "controller_set",
      .domain_containment_granularity = "controller_set",
  }));
  ASSERT_EQ(controller_set_result.stop_applicability().size(), 2U);
  for (const auto& stop_variant :
       controller_set_result.stop_applicability()) {
    ASSERT_TRUE(std::holds_alternative<
                LogicalStoppableApplicabilityTemplate>(stop_variant));
    const auto& stop =
        std::get<LogicalStoppableApplicabilityTemplate>(stop_variant);
    ASSERT_EQ(stop.escalation_stages.size(), 2U);
    ASSERT_EQ(stop.escalation_stages.front().containment_scopes.size(), 2U);
    for (const auto& scope :
         stop.escalation_stages.front().containment_scopes) {
      ASSERT_TRUE(
          std::holds_alternative<LogicalControllerSetStopScopeTemplate>(
              scope));
      const auto& controller_scope =
          std::get<LogicalControllerSetStopScopeTemplate>(scope);
      EXPECT_EQ(controller_scope.active_controller_role_ids.size(),
                controller_scope.logical_controller_symbols.size());
      EXPECT_FALSE(controller_scope.active_controller_role_ids.empty());
    }
  }

  const auto domain_result = qualify(make_fixture({
      .stop_form = StopForm::kIntrinsic,
      .policy_containment_granularity = "control_domain",
      .binding_containment_granularity = "control_domain",
      .domain_containment_granularity = "control_domain",
  }));
  ASSERT_EQ(domain_result.stop_applicability().size(), 1U);
  ASSERT_TRUE(std::holds_alternative<
              LogicalStoppableApplicabilityTemplate>(
      domain_result.stop_applicability().front()));
  const auto& domain_stop =
      std::get<LogicalStoppableApplicabilityTemplate>(
          domain_result.stop_applicability().front());
  ASSERT_EQ(domain_stop.escalation_stages.size(), 2U);
  ASSERT_EQ(domain_stop.escalation_stages.front().containment_scopes.size(),
            1U);
  ASSERT_TRUE(
      std::holds_alternative<LogicalControlDomainStopScopeTemplate>(
          domain_stop.escalation_stages.front().containment_scopes.front()));
  const auto& domain_scope =
      std::get<LogicalControlDomainStopScopeTemplate>(
          domain_stop.escalation_stages.front().containment_scopes.front());
  EXPECT_EQ(domain_scope.control_domain_id,
            core::ControlDomainId("axis_domain"));
  EXPECT_EQ(domain_scope.control_domain_revision,
            domain_stop.control_domain_revision);
}

TEST(CellApplicability,
     ProviderDefinedStopReusesExactRouteCommandAndEvidenceEndpoints) {
  const auto result =
      qualify(make_fixture({.stop_form = StopForm::kProviderDefined}));
  ASSERT_EQ(result.stop_applicability().size(), 1U);
  ASSERT_TRUE(std::holds_alternative<
              LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front()));
  const auto& stop = std::get<LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front());
  ASSERT_EQ(stop.escalation_stages.size(), 2U);
  const auto& nominal = stop.escalation_stages.front();
  ASSERT_TRUE(nominal.provider_command_endpoint.has_value());
  ASSERT_EQ(nominal.provider_evidence_endpoints.size(), 1U);
  const auto& route = result.routes().front();
  const auto command_endpoint = std::find_if(
      route.endpoint_templates.begin(), route.endpoint_templates.end(),
      [](const auto& value) {
        return value.endpoint_role_id ==
               core::EndpointRoleId("stop_command");
      });
  const auto evidence_endpoint = std::find_if(
      route.endpoint_templates.begin(), route.endpoint_templates.end(),
      [](const auto& value) {
        return value.endpoint_role_id ==
               core::EndpointRoleId("stop_evidence");
      });
  ASSERT_NE(command_endpoint, route.endpoint_templates.end());
  ASSERT_NE(evidence_endpoint, route.endpoint_templates.end());
  EXPECT_EQ(nominal.provider_command_endpoint->logical_endpoint_symbol,
            command_endpoint->logical_endpoint_symbol);
  EXPECT_EQ(nominal.provider_evidence_endpoints.front().logical_endpoint_symbol,
            evidence_endpoint->logical_endpoint_symbol);
}

TEST(CellApplicability,
     StopV2MaterializesExactOrderedStagesParametersAndHardFloor) {
  const auto result =
      qualify(make_fixture({.stop_form = StopForm::kProviderDefined}));
  ASSERT_EQ(result.stop_applicability().size(), 1U);
  const auto& stop = std::get<LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front());
  ASSERT_EQ(stop.escalation_stages.size(), 2U);
  const auto& nominal = stop.escalation_stages[0];
  const auto& hard = stop.escalation_stages[1];
  EXPECT_EQ(nominal.stop_escalation_stage_id,
            core::StopEscalationStageId("nominal"));
  EXPECT_EQ(nominal.stage_kind, core::StopEscalationStageKind::kNominal);
  ASSERT_TRUE(nominal.default_contract_parameters.has_value());
  EXPECT_EQ(hard.stop_escalation_stage_id,
            core::StopEscalationStageId("software_hard"));
  EXPECT_EQ(hard.stage_kind,
            core::StopEscalationStageKind::kSoftwareHard);
  ASSERT_TRUE(hard.default_contract_parameters.has_value());
  EXPECT_TRUE(hard.default_contract_parameters->empty());
  ASSERT_TRUE(hard.containment_command.registered_command_kind_id.has_value());
  EXPECT_EQ(*hard.containment_command.registered_command_kind_id,
            core::RegisteredStopCommandKindId(
                "manyforge.stop.software_hard"));
  EXPECT_EQ(result.value_schemas().size(), 2U);
  EXPECT_TRUE(result.commissioning_evidence_refs().empty());
}

TEST(CellApplicability,
     StopV2MaterializesFinalCommissionedSafetyRequestAndMatchesPreflight) {
  const auto result = qualify(make_fixture({
      .stop_form = StopForm::kProviderDefined,
      .commissioned_safety_case = CommissionedSafetyCase::kExact,
  }));
  ASSERT_EQ(result.stop_applicability().size(), 1U);
  const auto& stop = std::get<LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front());
  ASSERT_EQ(stop.escalation_stages.size(), 3U);
  const auto& safety = stop.escalation_stages.back();
  EXPECT_EQ(safety.stage_kind,
            core::StopEscalationStageKind::kSafetySystemRequest);
  ASSERT_TRUE(safety.commissioned_stop_interface.has_value());
  EXPECT_EQ(safety.commissioned_stop_interface->registered_command_kind_id,
            core::RegisteredStopCommandKindId(
                "manyforge.stop.safety.protective_request"));
  EXPECT_EQ(result.commissioning_evidence_refs().size(), 1U);
}

TEST(CellApplicability,
     StopV2RejectsMissingUnusedOrMismatchedCommissioningClosure) {
  for (const auto commissioned_case : {
           CommissionedSafetyCase::kMissingRow,
           CommissionedSafetyCase::kMissingProof,
           CommissionedSafetyCase::kUnusedRow,
           CommissionedSafetyCase::kWrongRoute,
           CommissionedSafetyCase::kUnknownExternalResource,
       }) {
    EXPECT_THROW(
        (void)qualify(make_fixture({
            .stop_form = StopForm::kProviderDefined,
            .commissioned_safety_case = commissioned_case,
        })),
        CellApplicabilityError);
  }
}

TEST(CellApplicability,
     StopV2RejectsMalformedCommissioningProofAndStageClassification) {
  const auto exact = make_fixture({
      .stop_form = StopForm::kProviderDefined,
      .commissioned_safety_case = CommissionedSafetyCase::kExact,
  });
  ASSERT_EQ(exact.inputs.value().commissioned_stop_interfaces.size(), 1U);
  const auto& row = exact.inputs.value().commissioned_stop_interfaces.front();
  EXPECT_THROW(
      (void)resolve_and_verify_commissioning_evidence_ref(
          row,
          row.commissioning_evidence_artifact_ref
              .content_addressed_store_key,
          "wrong commissioning bytes"),
      DeploymentApplicabilityCodecError);

  EXPECT_THROW(
      (void)make_fixture({
          .stop_form = StopForm::kProviderDefined,
          .commissioned_safety_case =
              CommissionedSafetyCase::kWrongActionClass,
      }),
      DeploymentApplicabilityCodecError);
  EXPECT_THROW(
      (void)make_fixture({
          .stop_form = StopForm::kProviderDefined,
          .commissioned_safety_case =
              CommissionedSafetyCase::kNonFinalStage,
      }),
      StopEvidencePolicyCodecError);
}

TEST(CellApplicability,
     MotionBindingRejectsAStoppablePolicyWithoutSoftwareHardFloor) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .stop_form = StopForm::kIntrinsic,
          .stop_parameter_case = StopParameterCase::kMissingHardStage,
          .omit_policy_software_hard_floor = true,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     StopV2RejectsMissingAndExtraBuildQualifiedSchemaClosure) {
  auto missing = make_fixture({.stop_form = StopForm::kProviderDefined});
  ASSERT_EQ(missing.value_schemas.size(), 2U);
  missing.value_schemas.pop_back();
  EXPECT_THROW((void)qualify(missing), CellApplicabilityError);

  auto extra = make_fixture({.stop_form = StopForm::kProviderDefined});
  const auto* unrelated =
      build_qualified_closed_schema_registry().find_qualified_value_schema(
          core::SchemaId("manyforge.value_schema.runtime.empty.v1"),
          core::SchemaRevision(
              std::string(kEmptyRuntimeConfigurationSchemaRevision)));
  ASSERT_NE(unrelated, nullptr);
  extra.value_schemas.push_back(*unrelated);
  std::sort(extra.value_schemas.begin(), extra.value_schemas.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.schema_id(), left.schema_revision()) <
                     std::tie(right.schema_id(), right.schema_revision());
            });
  EXPECT_THROW((void)qualify(extra), CellApplicabilityError);
}

TEST(CellApplicability,
     StopV2RejectsMissingUnusedAndStaleParameterRows) {
  for (const auto parameter_case : {
           StopParameterCase::kMissingHardStage,
           StopParameterCase::kUnusedStage,
           StopParameterCase::kStalePolicyRevision,
       }) {
    EXPECT_THROW(
        (void)qualify(make_fixture({
            .stop_form = StopForm::kProviderDefined,
            .stop_parameter_case = parameter_case,
        })),
        CellApplicabilityError);
  }
}

TEST(CellApplicability,
     StopV2RejectsIncompleteResourceCoverageAndUnitKindMismatch) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .resource_count = 2U,
          .stop_form = StopForm::kProviderDefined,
          .stop_parameter_case = StopParameterCase::kMissingResource,
      })),
      CellApplicabilityError);
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .stop_form = StopForm::kProviderDefined,
          .stop_parameter_case = StopParameterCase::kWrongCoordinateKind,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     QualifiedEndpointlessProviderStopRetainsNoSyntheticEndpoint) {
  const auto result = qualify(make_fixture({
      .stop_form = StopForm::kProviderDefined,
      .provider_stop_compatibility_case =
          ProviderStopCompatibilityCase::kEndpointless,
  }));
  ASSERT_EQ(result.stop_applicability().size(), 1U);
  const auto& stop = std::get<LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front());
  ASSERT_EQ(stop.escalation_stages.size(), 2U);
  const auto& nominal = stop.escalation_stages.front();
  EXPECT_FALSE(nominal.provider_command_endpoint.has_value());
  ASSERT_EQ(nominal.provider_evidence_endpoints.size(), 1U);
  EXPECT_EQ(nominal.provider_evidence_endpoints.front().endpoint_role_id,
            core::EndpointRoleId("stop_evidence"));
  const auto& endpoints = result.routes().front().endpoint_templates;
  EXPECT_EQ(std::count_if(
                endpoints.begin(), endpoints.end(),
                [](const auto& endpoint) {
                  return endpoint.endpoint_role_id ==
                         core::EndpointRoleId("stop_command");
                }),
            0);
  EXPECT_EQ(std::count_if(
                endpoints.begin(), endpoints.end(),
                [](const auto& endpoint) {
                  return endpoint.endpoint_role_id ==
                         core::EndpointRoleId("stop_evidence");
                }),
            1);
}

TEST(CellApplicability,
     EndpointlessProviderEvidenceRejectsEndpointRequiringManifestSupport) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .stop_form = StopForm::kProviderDefined,
          .provider_stop_compatibility_case =
              ProviderStopCompatibilityCase::kEndpointlessForbidden,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     ProviderStopRejectsQualifiedKindAndRegisteredKindMismatches) {
  for (const auto compatibility_case : {
           ProviderStopCompatibilityCase::kCommandKindMismatch,
           ProviderStopCompatibilityCase::kCommandRegisteredKindMismatch,
           ProviderStopCompatibilityCase::kEvidenceKindMismatch,
           ProviderStopCompatibilityCase::kEvidenceRegisteredKindMismatch,
       }) {
    FixtureOptions options;
    options.stop_form = StopForm::kProviderDefined;
    options.provider_stop_compatibility_case = compatibility_case;
    EXPECT_THROW((void)qualify(make_fixture(options)),
                 CellApplicabilityError);
  }
}

TEST(CellApplicability,
     ProviderStopRejectsZeroAndMultipleCommandIntersectionsAndWrongEvidence) {
  for (const auto endpoint_case : {
           ProviderStopEndpointCase::kCommandZeroIntersection,
           ProviderStopEndpointCase::kCommandAmbiguousIntersection,
           ProviderStopEndpointCase::kEvidenceWrongSide,
           ProviderStopEndpointCase::kEvidenceOutsideSupport,
       }) {
    EXPECT_THROW(
        (void)qualify(make_fixture({
            .stop_form = StopForm::kProviderDefined,
            .provider_stop_endpoint_case = endpoint_case,
        })),
        CellApplicabilityError);
  }
}

TEST(CellApplicability,
     BindingStateStopRetainsExactMeasuredAuthorityAndRouteStateEndpoint) {
  const auto result = qualify(make_fixture(
      {.stop_form = StopForm::kBindingStateAuthority}));
  ASSERT_EQ(result.stop_applicability().size(), 1U);
  ASSERT_TRUE(std::holds_alternative<
              LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front()));
  const auto& stop = std::get<LogicalStoppableApplicabilityTemplate>(
      result.stop_applicability().front());
  ASSERT_EQ(stop.escalation_stages.size(), 2U);
  const auto& nominal = stop.escalation_stages.front();
  EXPECT_FALSE(nominal.provider_command_endpoint.has_value());
  EXPECT_TRUE(nominal.provider_evidence_endpoints.empty());
  ASSERT_EQ(nominal.binding_state_authority_sources.size(), 1U);
  const auto& source = nominal.binding_state_authority_sources.front();
  EXPECT_EQ(source.state_authority_id,
            core::StateAuthorityId("authority_a"));
  EXPECT_EQ(source.state_authority_class,
            core::StateAuthorityClass::kMeasured);
  EXPECT_EQ(source.state_endpoint_role_id,
            core::EndpointRoleId("state"));
  const auto endpoint = std::find_if(
      result.routes().front().endpoint_templates.begin(),
      result.routes().front().endpoint_templates.end(),
      [](const auto& value) {
        return value.endpoint_role_id == core::EndpointRoleId("state");
      });
  ASSERT_NE(endpoint, result.routes().front().endpoint_templates.end());
  EXPECT_EQ(source.logical_endpoint_symbol,
            endpoint->logical_endpoint_symbol);
}

TEST(CellApplicability,
     BindingStateStopRejectsDisallowedAuthorityClassAndCommandSideRole) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .stop_form = StopForm::kBindingStateAuthority,
          .binding_state_stop_case =
              BindingStateStopCase::kDisallowedAuthorityClass,
      })),
      CellApplicabilityError);
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .stop_form = StopForm::kBindingStateAuthority,
          .binding_state_stop_case =
              BindingStateStopCase::kCommandSideEndpoint,
      })),
      CellApplicabilityError);
}

TEST(CellApplicability,
     StopGranularityRejectsPolicyMismatchAndBindingFinerThanDomain) {
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .stop_form = StopForm::kIntrinsic,
          .policy_containment_granularity = "resource",
          .binding_containment_granularity = "controller_set",
          .domain_containment_granularity = "controller_set",
      })),
      CellApplicabilityError);
  EXPECT_THROW(
      (void)qualify(make_fixture({
          .stop_form = StopForm::kIntrinsic,
          .policy_containment_granularity = "resource",
          .binding_containment_granularity = "resource",
          .domain_containment_granularity = "controller_set",
      })),
      CellApplicabilityError);
}

}  // namespace
}  // namespace manyforge::deployment
