#pragma once

#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/controller_topology_codec.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lite6_structural_retention_fixture.hpp"

namespace manyforge::deployment::test {

inline constexpr std::string_view kLite6ControllableComponentId =
    "lite6_arm";
inline constexpr std::string_view kLite6ControllableModelInstanceId =
    "lite6_fixture_instance";
inline constexpr std::string_view kLite6ArmTargetId = "arm";
inline constexpr std::string_view kLite6ArmBindingId = "lite6_arm_binding";
inline constexpr std::string_view kLite6ArmDomainId = "lite6_arm_domain";

struct Lite6ControllableRetentionFixture final {
  Lite6StructuralRetentionFixture structural;
  QualifiedCapabilityDefinition capability;
  QualifiedComponentType component_type;
  QualifiedControlInterfaceContract control_contract;
  QualifiedControllerTopology topology;
  QualifiedProviderManifest provider_manifest;
  QualifiedStopEvidencePolicy stop_policy;
  std::vector<Lite6InputArtifact> control_catalog_artifacts;
  RetentionQualifiedDeployment qualified;

  [[nodiscard]] std::span<
      const BuildQualifiedStructuralModelPreparation>
  structural_preparations() const noexcept {
    return structural.structural_preparations();
  }
};

namespace lite6_controllable_fixture_detail {

inline constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[nodiscard]] inline CanonicalValue text_array(
    std::initializer_list<std::string_view> values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.emplace_back(std::string(value));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] inline CanonicalValue identity_pose() {
  return CanonicalValue(CanonicalValue::Object{
      {"position_m",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(0.0), CanonicalValue(0.0),
           CanonicalValue(0.0)})},
      {"orientation_xyzw",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(0.0), CanonicalValue(0.0),
           CanonicalValue(0.0), CanonicalValue(1.0)})},
  });
}

[[nodiscard]] inline std::string revisioned_document(
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

[[nodiscard]] inline std::string revision_from_document(
    std::string_view document, std::string_view field) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* value = parsed.root().find_object_member(field);
  const auto* text = value == nullptr
                         ? nullptr
                         : std::get_if<std::string>(&value->storage());
  if (text == nullptr) {
    throw std::logic_error(
        "Lite6 control catalog fixture omitted its revision");
  }
  return *text;
}

[[nodiscard]] inline core::SchemaRevision schema_revision_for(
    core::CatalogRecordKind kind) {
  const auto* entry = build_qualified_closed_schema_registry()
                          .find_catalog_schema_family(kind);
  if (entry == nullptr ||
      entry->qualification() != CatalogSchemaQualification::kQualified ||
      entry->compiled_schema() == nullptr) {
    throw std::logic_error(
        "Lite6 control fixture requires a build-qualified catalog schema");
  }
  return entry->compiled_schema()->schema_revision();
}

template <typename Qualified>
[[nodiscard]] inline Lite6InputArtifact catalog_artifact(
    std::string logical_uri, std::string bytes,
    const Qualified& qualified) {
  const auto& reference = qualified.reference();
  if (sha256_digest(bytes) != reference.content_digest.value()) {
    throw std::logic_error(
        "Lite6 control fixture catalog bytes differ from their reference");
  }
  return Lite6InputArtifact{
      std::move(logical_uri),
      "application/json",
      reference.content_addressed_store_key,
      reference.content_digest,
      std::move(bytes),
  };
}

[[nodiscard]] inline std::string catalog_key(
    std::string_view kind, std::string_view digest) {
  return "catalog/i3-lite6/" + std::string(kind) + "/" +
         std::string(digest.substr(std::string_view("sha256:").size()));
}

struct QualifiedControlCatalog final {
  QualifiedCapabilityDefinition capability;
  QualifiedComponentType component_type;
  QualifiedControlInterfaceContract contract;
  QualifiedControllerTopology topology;
  QualifiedProviderManifest provider;
  QualifiedStopEvidencePolicy stop;
  std::vector<Lite6InputArtifact> artifacts;
};

struct AssistantOverlayFixture final {
  core::SiteOverlayRefDTO reference;
  std::string bytes;
};

[[nodiscard]] inline CanonicalValue assistant_external_resource(
    std::string_view alias, std::string_view resource_kind,
    std::string_view overlay_ref, const QualifiedProviderManifest& provider) {
  return CanonicalValue(CanonicalValue::Object{
      {"endpoint_role_id", CanonicalValue("state")},
      {"external_resource_alias", CanonicalValue(std::string(alias))},
      {"provider_instance_id",
       CanonicalValue("assistant_provider_instance")},
      {"provider_manifest_id",
       CanonicalValue(provider.value().provider_manifest_id.value())},
      {"provider_manifest_revision",
       CanonicalValue(provider.value().provider_manifest_revision.value())},
      {"resource_kind", CanonicalValue(std::string(resource_kind))},
      {"site_overlay_value_ref", CanonicalValue(std::string(overlay_ref))},
  });
}

[[nodiscard]] inline AssistantOverlayFixture make_assistant_overlay() {
  CanonicalValue::Array values{
      CanonicalValue(CanonicalValue::Object{
          {"resource_kind", CanonicalValue("service_endpoint")},
          {"semantic_kind", CanonicalValue("external_resource")},
          {"site_overlay_value_ref",
           CanonicalValue("resources.assistant_bridge")},
          {"value",
           CanonicalValue(CanonicalValue::Object{
               {"value", CanonicalValue("http://assistant.invalid")},
               {"value_kind", CanonicalValue("string")},
           })},
      }),
      CanonicalValue(CanonicalValue::Object{
          {"resource_kind", CanonicalValue("model_endpoint")},
          {"semantic_kind", CanonicalValue("external_resource")},
          {"site_overlay_value_ref",
           CanonicalValue("resources.assistant_model")},
          {"value",
           CanonicalValue(CanonicalValue::Object{
               {"value", CanonicalValue("http://model.invalid")},
               {"value_kind", CanonicalValue("string")},
           })},
      }),
  };
  const std::string bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue("manyforge.site_overlay.v1")},
          {"site_overlay_id", CanonicalValue("i5_assistant_site")},
          {"values", CanonicalValue(std::move(values))},
      }),
      "site-overlay-revision", "manyforge.site_overlay.v1",
      "site_overlay_revision");
  return {
      core::SiteOverlayRefDTO{
          core::SiteOverlayId("i5_assistant_site"),
          core::SiteOverlayRevision(
              revision_from_document(bytes, "site_overlay_revision")),
          core::ContentDigest(sha256_digest(bytes)),
      },
      bytes,
  };
}

[[nodiscard]] inline CanonicalValue assistant_source() {
  return CanonicalValue(CanonicalValue::Object{
      {"model_endpoint_aliases",
       CanonicalValue(CanonicalValue::Object{
           {"primary_model",
            CanonicalValue(CanonicalValue::Object{
                {"external_resource_alias",
                 CanonicalValue("assistant_model")},
                {"model_id", CanonicalValue("i5-test-model")},
            })},
       })},
      {"modes",
       CanonicalValue(CanonicalValue::Object{
           {"query",
            CanonicalValue(CanonicalValue::Object{
                {"catalog",
                 CanonicalValue(CanonicalValue::Object{
                     {"nodes", CanonicalValue(CanonicalValue::Array{})},
                     {"skills", CanonicalValue(CanonicalValue::Array{})},
                     {"tools",
                      text_array({"scene_inspect", "status_read"})},
                 })},
                {"description", CanonicalValue("Read-only H0 query")},
                {"enabled", CanonicalValue(true)},
                {"grant_policy",
                 CanonicalValue(CanonicalValue::Object{
                     {"approval_mode", CanonicalValue(nullptr)},
                     {"review_policy", CanonicalValue("not_required")},
                     {"session_kind", CanonicalValue(nullptr)},
                 })},
                {"model_endpoint_alias", CanonicalValue("primary_model")},
                {"provider_alias", CanonicalValue("direct_local")},
            })},
       })},
      {"provider_aliases",
       CanonicalValue(CanonicalValue::Object{
           {"direct_local",
            CanonicalValue(CanonicalValue::Object{
                {"bridge_external_resource_alias",
                 CanonicalValue("assistant_bridge")},
                {"provider_kind", CanonicalValue("direct")},
            })},
       })},
  });
}

[[nodiscard]] inline QualifiedControlCatalog
qualify_control_catalog(bool chained) {
  const auto* descriptor =
      core::find_builtin_capability_definition("state.observed.current");
  if (descriptor == nullptr) {
    throw std::logic_error(
        "Lite6 control fixture requires state.observed.current");
  }
  CanonicalValue::Object capability_root{
      {"api_version",
       CanonicalValue(std::string(core::kCapabilityDefinitionApiVersion))},
      {"capability_id", CanonicalValue("state.observed.current")},
      {"scope",
       CanonicalValue(std::string(core::to_string(descriptor->scope)))},
      {"provider_binding_kind",
       CanonicalValue(std::string(
           core::to_string(descriptor->provider_binding_kind)))},
      {"capability_definition_revision",
       CanonicalValue(
           std::string(descriptor->capability_definition_revision))},
  };
  const std::string capability_bytes =
      canonical_json(CanonicalValue(std::move(capability_root)));
  const core::ContentDigest capability_digest(
      sha256_digest(capability_bytes));
  const core::ContentAddressedStoreKey capability_key(
      catalog_key("capability", capability_digest.value()));
  const core::CapabilityDefinitionCatalogRecordRefDTO capability_ref{
      core::CapabilityId("state.observed.current"),
      core::CapabilityDefinitionRevision(
          std::string(descriptor->capability_definition_revision)),
      core::SchemaId(std::string(core::kCapabilityDefinitionApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kCapabilityDefinition),
      capability_digest,
      capability_key,
  };
  auto capability = resolve_and_qualify_capability_definition(
      capability_ref, capability_key, capability_bytes);

  const std::string component_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kComponentTypeApiVersion))},
          {"component_type_id",
           CanonicalValue("lite6_arm_component")},
          {"tags", CanonicalValue(CanonicalValue::Array{})},
          {"model_roles",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"model_role_id", CanonicalValue("arm_model")},
                   {"cardinality", CanonicalValue("exactly_one")},
               })})},
          {"component_root",
           CanonicalValue(CanonicalValue::Object{
               {"model_role_id", CanonicalValue("arm_model")},
               {"frame_role_id", CanonicalValue("root")},
           })},
          {"model_mounts", CanonicalValue(CanonicalValue::Array{})},
          {"command_targets",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"command_target_id",
                    CanonicalValue(std::string(kLite6ArmTargetId))},
                   {"model_group_roles",
                    CanonicalValue(CanonicalValue::Array{CanonicalValue(
                        CanonicalValue::Object{
                            {"model_group_role_id",
                             CanonicalValue("planning_group")},
                            {"model_role_id",
                             CanonicalValue("arm_model")},
                            {"asset_model_group_role_id",
                             CanonicalValue("arm")},
                        })})},
                   {"resource_roles",
                    CanonicalValue(CanonicalValue::Array{CanonicalValue(
                        CanonicalValue::Object{
                            {"resource_role_id",
                             CanonicalValue("joints")},
                            {"source_kind",
                             CanonicalValue("model_role")},
                            {"model_role_id",
                             CanonicalValue("arm_model")},
                            {"asset_resource_role_id",
                             CanonicalValue("arm_joints")},
                        })})},
                   {"frame_roles",
                    CanonicalValue(CanonicalValue::Array{CanonicalValue(
                        CanonicalValue::Object{
                            {"frame_role_id", CanonicalValue("tcp")},
                            {"model_role_id",
                             CanonicalValue("arm_model")},
                            {"asset_frame_role_id",
                             CanonicalValue("tcp")},
                        })})},
                   {"collision_body_roles",
                    CanonicalValue(CanonicalValue::Array{CanonicalValue(
                        CanonicalValue::Object{
                            {"collision_body_role_id",
                             CanonicalValue("arm_collision")},
                            {"model_role_id",
                             CanonicalValue("arm_model")},
                            {"asset_collision_body_role_id",
                             CanonicalValue("arm_collision")},
                        })})},
                   {"capabilities",
                    CanonicalValue(CanonicalValue::Array{CanonicalValue(
                        CanonicalValue::Object{
                            {"capability_id",
                             CanonicalValue(
                                 capability.value().capability_id.value())},
                            {"capability_definition_revision",
                             CanonicalValue(
                                 capability.value()
                                     .capability_definition_revision
                                     .value())},
                        })})},
               })})},
          {"attachment_interfaces",
           CanonicalValue(CanonicalValue::Array{})},
      }),
      "catalog-record-revision/component_type",
      core::kComponentTypeApiVersion, "component_type_revision");
  const core::ContentDigest component_digest(sha256_digest(component_bytes));
  const core::ContentAddressedStoreKey component_key(
      catalog_key("component", component_digest.value()));
  const core::ComponentTypeCatalogRecordRefDTO component_ref{
      core::ComponentTypeId("lite6_arm_component"),
      core::ComponentTypeRevision(revision_from_document(
          component_bytes, "component_type_revision")),
      core::SchemaId(std::string(core::kComponentTypeApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kComponentType),
      component_digest,
      component_key,
  };
  auto component_type = resolve_and_qualify_component_type(
      component_ref, component_key, component_bytes,
      std::span<const QualifiedCapabilityDefinition>(&capability, 1U));

  const std::string contract_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(
               std::string(core::kControlInterfaceContractApiVersion))},
          {"control_interface_contract_id",
           CanonicalValue("lite6_arm_position")},
          {"command_interfaces",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"resource_role_id", CanonicalValue("joints")},
                   {"interface_name", CanonicalValue("position")},
               })})},
          {"state_interfaces",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"resource_role_id", CanonicalValue("joints")},
                   {"interface_name", CanonicalValue("position")},
               })})},
          {"ordering", text_array({"joints"})},
      }),
      "catalog-record-revision/control_interface_contract",
      core::kControlInterfaceContractApiVersion,
      "control_interface_revision");
  const core::ContentDigest contract_digest(sha256_digest(contract_bytes));
  const core::ContentAddressedStoreKey contract_key(
      catalog_key("contract", contract_digest.value()));
  const core::ControlInterfaceContractCatalogRecordRefDTO contract_ref{
      core::ControlInterfaceContractId("lite6_arm_position"),
      core::ControlInterfaceRevision(revision_from_document(
          contract_bytes, "control_interface_revision")),
      core::SchemaId(
          std::string(core::kControlInterfaceContractApiVersion)),
      schema_revision_for(
          core::CatalogRecordKind::kControlInterfaceContract),
      contract_digest,
      contract_key,
  };
  auto contract = resolve_and_qualify_control_interface_contract(
      contract_ref, contract_key, contract_bytes);

  const CanonicalValue topology_contract_claim(
      CanonicalValue::Object{
          {"control_interface_contract_id",
           CanonicalValue(
               contract.value().control_interface_contract_id.value())},
          {"control_interface_revision",
           CanonicalValue(contract.value().control_interface_revision.value())},
          {"claimed_directions", text_array({"command", "state"})},
      });
  const CanonicalValue topology_interface_claim(CanonicalValue::Object{
      {"resource_role_id", CanonicalValue("joints")},
      {"interface_name", CanonicalValue("position")},
  });
  const std::string topology_id = chained ? "lite6_arm_jtc_admittance_topology"
                                          : "lite6_arm_direct_topology";
  CanonicalValue::Array controllers;
  CanonicalValue::Array chain_edges;
  CanonicalValue::Array activation_sets;
  CanonicalValue::Array legal_routes;
  if (chained) {
    controllers = CanonicalValue::Array{
        CanonicalValue(CanonicalValue::Object{
            {"controller_role_id", CanonicalValue("admittance")},
            {"control_interface_claims",
             CanonicalValue(CanonicalValue::Array{topology_contract_claim})},
            {"exported_reference_interfaces",
             CanonicalValue(CanonicalValue::Array{topology_interface_claim})},
            {"exported_state_interfaces",
             CanonicalValue(CanonicalValue::Array{topology_interface_claim})},
        }),
        CanonicalValue(CanonicalValue::Object{
            {"controller_role_id", CanonicalValue("trajectory")},
            {"control_interface_claims",
             CanonicalValue(CanonicalValue::Array{})},
            {"exported_reference_interfaces",
             CanonicalValue(CanonicalValue::Array{})},
            {"exported_state_interfaces",
             CanonicalValue(CanonicalValue::Array{})},
        }),
    };
    chain_edges = CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
        {"upstream_controller_role_id", CanonicalValue("trajectory")},
        {"downstream_controller_role_id", CanonicalValue("admittance")},
        {"reference_interfaces",
         CanonicalValue(CanonicalValue::Array{topology_interface_claim})},
        {"state_interfaces",
         CanonicalValue(CanonicalValue::Array{topology_interface_claim})},
    })};
    activation_sets =
        CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
            {"activation_set_id", CanonicalValue("arm_chain")},
            {"active_controller_role_ids",
             text_array({"admittance", "trajectory"})},
            {"active_chain_edges", CanonicalValue(CanonicalValue::Array{
                                       CanonicalValue(CanonicalValue::Object{
                                           {"upstream_controller_role_id",
                                            CanonicalValue("trajectory")},
                                           {"downstream_controller_role_id",
                                            CanonicalValue("admittance")},
                                       })})},
        })};
    legal_routes = CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
        {"control_route_id", CanonicalValue("arm_position")},
        {"control_interface_contract_id",
         CanonicalValue(
             contract.value().control_interface_contract_id.value())},
        {"control_interface_revision",
         CanonicalValue(contract.value().control_interface_revision.value())},
        {"ordered_controller_role_ids",
         text_array({"trajectory", "admittance"})},
        {"compatible_activation_set_ids", text_array({"arm_chain"})},
    })};
  } else {
    controllers = CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
        {"controller_role_id", CanonicalValue("hardware")},
        {"control_interface_claims",
         CanonicalValue(CanonicalValue::Array{topology_contract_claim})},
        {"exported_reference_interfaces",
         CanonicalValue(CanonicalValue::Array{})},
        {"exported_state_interfaces", CanonicalValue(CanonicalValue::Array{})},
    })};
    activation_sets =
        CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
            {"activation_set_id", CanonicalValue("arm_direct")},
            {"active_controller_role_ids", text_array({"hardware"})},
            {"active_chain_edges", CanonicalValue(CanonicalValue::Array{})},
        })};
    legal_routes = CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
        {"control_route_id", CanonicalValue("arm_position")},
        {"control_interface_contract_id",
         CanonicalValue(
             contract.value().control_interface_contract_id.value())},
        {"control_interface_revision",
         CanonicalValue(contract.value().control_interface_revision.value())},
        {"ordered_controller_role_ids", text_array({"hardware"})},
        {"compatible_activation_set_ids", text_array({"arm_direct"})},
    })};
  }
  const std::string topology_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kControllerTopologyApiVersion))},
          {"controller_topology_id", CanonicalValue(topology_id)},
          {"controllers", CanonicalValue(std::move(controllers))},
          {"chain_edges", CanonicalValue(std::move(chain_edges))},
          {"activation_sets", CanonicalValue(std::move(activation_sets))},
          {"legal_routes", CanonicalValue(std::move(legal_routes))},
      }),
      "catalog-record-revision/controller_topology",
      core::kControllerTopologyApiVersion, "topology_revision");
  const core::ContentDigest topology_digest(sha256_digest(topology_bytes));
  const core::ContentAddressedStoreKey topology_key(
      catalog_key("topology", topology_digest.value()));
  const core::ControllerTopologyCatalogRecordRefDTO topology_ref{
      core::ControllerTopologyId(topology_id),
      core::TopologyRevision(
          revision_from_document(topology_bytes, "topology_revision")),
      core::SchemaId(std::string(core::kControllerTopologyApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kControllerTopology),
      topology_digest,
      topology_key,
  };
  auto topology = resolve_and_qualify_controller_topology(
      topology_ref, topology_key, topology_bytes,
      std::span<const QualifiedControlInterfaceContract>(&contract, 1U));

  const std::string provider_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kProviderManifestApiVersion))},
          {"provider_manifest_id",
           CanonicalValue("lite6_arm_provider")},
          {"offered_capabilities",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"capability_id",
                    CanonicalValue(capability.value().capability_id.value())},
                   {"capability_definition_revision",
                    CanonicalValue(
                        capability.value()
                            .capability_definition_revision.value())},
                   {"required_endpoint_role_ids",
                    text_array({"command", "state"})},
               })})},
          {"provider_contracts",
           CanonicalValue(CanonicalValue::Array{})},
      }),
      "catalog-record-revision/provider_manifest",
      core::kProviderManifestApiVersion, "provider_manifest_revision");
  const core::ContentDigest provider_digest(sha256_digest(provider_bytes));
  const core::ContentAddressedStoreKey provider_key(
      catalog_key("provider", provider_digest.value()));
  const core::ProviderManifestCatalogRecordRefDTO provider_ref{
      core::ProviderManifestId("lite6_arm_provider"),
      core::ProviderManifestRevision(revision_from_document(
          provider_bytes, "provider_manifest_revision")),
      core::SchemaId(std::string(core::kProviderManifestApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kProviderManifest),
      provider_digest,
      provider_key,
  };
  auto provider = resolve_and_qualify_provider_manifest(
      provider_ref, provider_key, provider_bytes);

  const std::string stop_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kStopEvidencePolicyApiVersion))},
          {"stop_evidence_policy_id",
           CanonicalValue("lite6_not_stoppable")},
          {"disposition", CanonicalValue("not_stoppable")},
          {"containment_granularity", CanonicalValue("resource")},
      }),
      "catalog-record-revision/stop_evidence_policy",
      core::kStopEvidencePolicyApiVersion,
      "stop_evidence_policy_revision");
  const core::ContentDigest stop_digest(sha256_digest(stop_bytes));
  const core::ContentAddressedStoreKey stop_key(
      catalog_key("stop", stop_digest.value()));
  const core::StopEvidencePolicyCatalogRecordRefDTO stop_ref{
      core::StopEvidencePolicyId("lite6_not_stoppable"),
      core::StopEvidencePolicyRevision(revision_from_document(
          stop_bytes, "stop_evidence_policy_revision")),
      core::SchemaId(std::string(core::kStopEvidencePolicyApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kStopEvidencePolicy),
      stop_digest,
      stop_key,
  };
  auto stop = resolve_and_qualify_stop_evidence_policy(
      stop_ref, stop_key, stop_bytes);

  std::vector<Lite6InputArtifact> artifacts;
  artifacts.reserve(6U);
  artifacts.push_back(
      catalog_artifact("catalog://capability/state.observed.current",
                       capability_bytes, capability));
  artifacts.push_back(
      catalog_artifact("catalog://component_type/lite6_arm_component",
                       component_bytes, component_type));
  artifacts.push_back(
      catalog_artifact("catalog://control_interface/lite6_arm_position",
                       contract_bytes, contract));
  artifacts.push_back(
      catalog_artifact("catalog://controller_topology/" + topology_id,
                       topology_bytes, topology));
  artifacts.push_back(
      catalog_artifact("catalog://provider_manifest/lite6_arm_provider",
                       provider_bytes, provider));
  artifacts.push_back(catalog_artifact(
      "catalog://stop_policy/lite6_not_stoppable", stop_bytes, stop));

  return {
      std::move(capability),
      std::move(component_type),
      std::move(contract),
      std::move(topology),
      std::move(provider),
      std::move(stop),
      std::move(artifacts),
  };
}

[[nodiscard]] inline GraphRevisionVerifiedCellGraph make_graph(
    const Lite6StructuralRetentionFixture& structural,
    const QualifiedControlCatalog& catalog) {
  CanonicalValue::Array resource_bindings;
  for (std::size_t index = 0U; index < 6U; ++index) {
    resource_bindings.emplace_back(CanonicalValue::Object{
        {"resource_role_id", CanonicalValue("joints")},
        {"resource_id",
         CanonicalValue("lite6_joint_" + std::to_string(index + 1U))},
    });
  }
  const CanonicalValue graph(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(core::kCellGraphApiVersion))},
      {"graph_revision", CanonicalValue(std::string(kZeroDigest))},
      {"cell_id", CanonicalValue("i3_lite6_controllable_cell")},
      {"runtime_units", CanonicalValue(CanonicalValue::Array{})},
      {"model_instances",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(
           CanonicalValue::Object{
               {"model_instance_id",
                CanonicalValue(
                    std::string(kLite6ControllableModelInstanceId))},
               {"model_asset_id",
                CanonicalValue(
                    structural.catalog_reference.record_id.value())},
               {"model_revision",
                CanonicalValue(
                    structural.catalog_reference.record_revision.value())},
           })})},
      {"controllable_components",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(
           CanonicalValue::Object{
               {"controllable_component_id",
                CanonicalValue(
                    std::string(kLite6ControllableComponentId))},
               {"component_type_id",
                CanonicalValue(
                    catalog.component_type.value().component_type_id.value())},
               {"component_type_revision",
                CanonicalValue(catalog.component_type.value()
                                   .component_type_revision.value())},
               {"model_role_bindings",
                CanonicalValue(CanonicalValue::Array{CanonicalValue(
                    CanonicalValue::Object{
                        {"model_role_id", CanonicalValue("arm_model")},
                        {"model_instance_id",
                         CanonicalValue(std::string(
                             kLite6ControllableModelInstanceId))},
                    })})},
               {"runtime_unit_ids",
                CanonicalValue(CanonicalValue::Array{})},
               {"root_pose_in_cell", identity_pose()},
           })})},
      {"control_domains",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(
           CanonicalValue::Object{
               {"control_domain_id",
                CanonicalValue(std::string(kLite6ArmDomainId))},
               {"control_domain_revision",
                CanonicalValue(std::string(kZeroDigest))},
               {"runtime_unit_ids",
                CanonicalValue(CanonicalValue::Array{})},
               {"control_coordinator_endpoint_role",
                CanonicalValue("coordinator")},
               {"controller_topology_id",
                CanonicalValue(
                    catalog.topology.value().controller_topology_id.value())},
               {"topology_revision",
                CanonicalValue(catalog.topology.value().topology_revision.value())},
               {"containment_granularity", CanonicalValue("resource")},
           })})},
      {"control_bindings",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(
           CanonicalValue::Object{
               {"control_binding_id",
                CanonicalValue(std::string(kLite6ArmBindingId))},
               {"control_binding_revision",
                CanonicalValue(std::string(kZeroDigest))},
               {"controllable_component_id",
                CanonicalValue(
                    std::string(kLite6ControllableComponentId))},
               {"command_target_id",
                CanonicalValue(std::string(kLite6ArmTargetId))},
               {"capability_id",
                CanonicalValue(catalog.capability.value().capability_id.value())},
               {"capability_definition_revision",
                CanonicalValue(catalog.capability.value()
                                   .capability_definition_revision.value())},
               {"control_domain_id",
                CanonicalValue(std::string(kLite6ArmDomainId))},
               {"resource_bindings",
                CanonicalValue(std::move(resource_bindings))},
               {"control_interface_contract_id",
                CanonicalValue(catalog.contract.value()
                                   .control_interface_contract_id.value())},
               {"control_interface_revision",
                CanonicalValue(catalog.contract.value()
                                   .control_interface_revision.value())},
               {"controller_topology_id",
                CanonicalValue(
                    catalog.topology.value().controller_topology_id.value())},
               {"topology_revision",
                CanonicalValue(catalog.topology.value().topology_revision.value())},
               {"legal_control_route_ids", text_array({"arm_position"})},
               {"route_endpoint_projections",
                CanonicalValue(CanonicalValue::Array{CanonicalValue(
                    CanonicalValue::Object{
                        {"control_route_id", CanonicalValue("arm_position")},
                        {"command_endpoint_role_ids",
                         text_array({"command"})},
                        {"state_endpoint_role_ids", text_array({"state"})},
                    })})},
               {"provider_instance_id",
                CanonicalValue("lite6_arm_provider_instance")},
               {"provider_manifest_id",
                CanonicalValue(catalog.provider.value()
                                   .provider_manifest_id.value())},
               {"provider_manifest_revision",
                CanonicalValue(catalog.provider.value()
                                   .provider_manifest_revision.value())},
               {"state_authority_id",
                CanonicalValue("lite6_measured_state")},
               {"state_authority_class", CanonicalValue("measured")},
               {"stop_evidence_policy_id",
                CanonicalValue(catalog.stop.value()
                                   .stop_evidence_policy_id.value())},
               {"stop_evidence_policy_revision",
                CanonicalValue(catalog.stop.value()
                                   .stop_evidence_policy_revision.value())},
               {"containment_granularity", CanonicalValue("resource")},
           })})},
      {"component_attachments", CanonicalValue(CanonicalValue::Array{})},
  });
  const auto intrinsic = validate_intrinsic_cell_graph_candidate(
      decode_cell_graph_candidate(parse_strict_json_model_yaml(
          canonical_json(graph))));
  return compute_graph_revision(prepare_cell_graph_identity(intrinsic, {}));
}

[[nodiscard]] inline RevisionVerifiedDeploymentApplicabilityInputs
make_applicability_inputs(const GraphRevisionVerifiedCellGraph& graph,
                          const QualifiedProviderManifest& provider,
                          const AssistantOverlayFixture* assistant_overlay) {
  const auto& domain = graph.graph().control_domains.front();
  const auto& binding = graph.graph().control_bindings.front();
  CanonicalValue::Object root_members{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"desired_control_configurations",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(
           CanonicalValue::Object{
               {"control_domain_id",
                CanonicalValue(domain.control_domain_id.value())},
               {"control_domain_revision",
                CanonicalValue(domain.control_domain_revision.value())},
               {"controller_topology_id",
                CanonicalValue(domain.controller_topology_id.value())},
               {"topology_revision",
                CanonicalValue(domain.topology_revision.value())},
               {"activation_set_id",
                CanonicalValue(graph.graph()
                                           .control_domains.front()
                                           .controller_topology_id.value() ==
                                       "lite6_arm_direct_topology"
                                   ? "arm_direct"
                                   : "arm_chain")},
               {"binding_routes",
                CanonicalValue(CanonicalValue::Array{CanonicalValue(
                    CanonicalValue::Object{
                        {"control_binding_id",
                         CanonicalValue(binding.control_binding_id.value())},
                        {"control_binding_revision",
                         CanonicalValue(
                             binding.control_binding_revision.value())},
                        {"control_route_id",
                         CanonicalValue("arm_position")},
                    })})},
           })})},
      {"capability_provider_bindings",
       CanonicalValue(CanonicalValue::Array{})},
  };
  if (assistant_overlay != nullptr) {
    root_members.emplace_back("assistant", assistant_source());
    root_members.emplace_back(
        "external_resources",
        CanonicalValue(CanonicalValue::Array{
            assistant_external_resource(
                "assistant_bridge", "service_endpoint",
                "resources.assistant_bridge", provider),
            assistant_external_resource(
                "assistant_model", "model_endpoint",
                "resources.assistant_model", provider),
        }));
    root_members.emplace_back(
        "site_overlay_ref",
        CanonicalValue(CanonicalValue::Object{
            {"non_secret_digest",
             CanonicalValue(
                 assistant_overlay->reference.non_secret_digest.value())},
            {"site_overlay_id",
             CanonicalValue(
                 assistant_overlay->reference.site_overlay_id.value())},
            {"site_overlay_revision",
             CanonicalValue(
                 assistant_overlay->reference.site_overlay_revision.value())},
        }));
  }
  const CanonicalValue root(std::move(root_members));
  return compute_capability_provider_binding_revisions(
      decode_deployment_applicability_inputs_candidate(
          parse_strict_json_model_yaml(canonical_json(root))));
}

}  // namespace lite6_controllable_fixture_detail

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_controllable_retention_fixture(bool chained = false,
                                          bool assistant_enabled = false) {
  namespace fixture_detail = lite6_controllable_fixture_detail;
  auto structural = make_lite6_structural_retention_fixture();
  auto catalog = fixture_detail::qualify_control_catalog(chained);
  const auto graph = fixture_detail::make_graph(structural, catalog);
  const std::optional<fixture_detail::AssistantOverlayFixture>
      assistant_overlay = assistant_enabled
                              ? std::optional<
                                    fixture_detail::AssistantOverlayFixture>(
                                    fixture_detail::make_assistant_overlay())
                              : std::nullopt;
  const auto inputs = fixture_detail::make_applicability_inputs(
      graph, catalog.provider,
      assistant_overlay.has_value() ? &*assistant_overlay : nullptr);

  std::vector<core::CatalogRecordRefDTO> catalog_records{
      structural.catalog_reference,
      catalog.capability.reference(),
      catalog.component_type.reference(),
      catalog.contract.reference(),
      catalog.topology.reference(),
      catalog.provider.reference(),
      catalog.stop.reference(),
  };
  CanonicalValue::Object deployment_graph =
      graph.normalized_root().as_object();
  deployment_graph.erase(
      std::remove_if(
          deployment_graph.begin(), deployment_graph.end(),
          [](const auto& member) {
            return member.first == "api_version" ||
                   member.first == "graph_revision" ||
                   member.first == "cell_id";
          }),
      deployment_graph.end());
  CanonicalValue::Object deployment_members{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"deployment_artifact_id",
       CanonicalValue(chained ? "i3_lite6_chained_controllable_fixture"
                              : "i3_lite6_controllable_fixture")},
      {"deployment_revision",
       CanonicalValue(std::string(fixture_detail::kZeroDigest))},
      {"cell_id", CanonicalValue(graph.graph().cell_id.value())},
      {"name", CanonicalValue(chained ? "I3 Lite6 chained controllable fixture"
                                      : "I3 Lite6 controllable fixture")},
      {"catalogs", detail::encode_deployment_catalogs(catalog_records)},
      {"graph", CanonicalValue(std::move(deployment_graph))},
      {"desired_control_configurations",
       inputs.normalized_desired_control_configurations()},
      {"capability_provider_bindings",
       inputs.normalized_capability_provider_bindings()},
  };
  if (assistant_enabled) {
    deployment_members.emplace_back(
        "assistant", inputs.assistant_settings().normalized_source());
    deployment_members.emplace_back(
        "external_resources", inputs.normalized_external_resources());
    deployment_members.emplace_back(
        "site_overlay_ref", *inputs.normalized_site_overlay_ref());
  }
  const CanonicalValue deployment_value(std::move(deployment_members));
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical_json(deployment_value)),
      std::span<const QualifiedRuntimeProfile>{});

  const std::array<SourceVerifiedModelAsset, 1U> assets{
      structural.verified_asset};
  const std::array<QualifiedComponentType, 1U> component_types{
      catalog.component_type};
  const auto composition = compose_cell_occurrences(
      deployment.graph(), assets, component_types);
  const std::optional<VerifiedSiteOverlayArtifactProjection>
      verified_assistant_overlay =
          assistant_overlay.has_value()
              ? std::optional<VerifiedSiteOverlayArtifactProjection>(
                    resolve_and_verify_site_overlay_artifact_projection(
                        assistant_overlay->reference,
                        assistant_overlay->bytes))
              : std::nullopt;
  const auto overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources,
      verified_assistant_overlay.has_value() ? &*verified_assistant_overlay
                                             : nullptr);
  const std::array<QualifiedCapabilityDefinition, 1U> capabilities{
      catalog.capability};
  const std::array<QualifiedControlInterfaceContract, 1U> contracts{
      catalog.contract};
  const std::array<QualifiedControllerTopology, 1U> topologies{
      catalog.topology};
  const std::array<QualifiedProviderManifest, 1U> providers{
      catalog.provider};
  const std::array<QualifiedStopEvidencePolicy, 1U> stop_policies{
      catalog.stop};
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, capabilities,
      contracts, topologies, providers, stop_policies, overlay);
  auto qualified = qualify_deployment_for_retention(
      deployment, compile_cell_ir(applicability));

  return {
      std::move(structural),
      std::move(catalog.capability),
      std::move(catalog.component_type),
      std::move(catalog.contract),
      std::move(catalog.topology),
      std::move(catalog.provider),
      std::move(catalog.stop),
      std::move(catalog.artifacts),
      std::move(qualified),
  };
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_chained_controllable_retention_fixture() {
  return make_lite6_controllable_retention_fixture(true);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_assistant_retention_fixture() {
  return make_lite6_controllable_retention_fixture(false, true);
}

class Lite6ControllableCompilerInputArtifactSource final
    : public detail::ICompilerInputArtifactSource {
 public:
  explicit Lite6ControllableCompilerInputArtifactSource(
      const Lite6ControllableRetentionFixture& fixture) {
    add(fixture.structural.catalog_reference.content_addressed_store_key,
        fixture.structural.catalog_reference.content_digest,
        fixture.structural.catalog_bytes);
    for (const auto& artifact : fixture.structural.source_artifacts) {
      add(artifact.store_key, artifact.content_digest, artifact.bytes);
    }
    for (const auto& artifact : fixture.control_catalog_artifacts) {
      add(artifact.store_key, artifact.content_digest, artifact.bytes);
    }
  }

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey& key,
      const core::ContentDigest& expected_digest,
      std::optional<std::uint64_t> expected_size) const override {
    const auto found = inputs_.find(key.value());
    if (found == inputs_.end() ||
        found->second.digest != expected_digest ||
        (expected_size.has_value() &&
         static_cast<std::uint64_t>(found->second.bytes.size()) !=
             *expected_size) ||
        sha256_digest(found->second.bytes) != expected_digest.value()) {
      return std::nullopt;
    }
    return found->second.bytes;
  }

 private:
  struct Entry final {
    core::ContentDigest digest;
    std::string bytes;
  };

  void add(const core::ContentAddressedStoreKey& key,
           const core::ContentDigest& digest, std::string bytes) {
    const auto [position, inserted] = inputs_.emplace(
        key.value(), Entry{digest, std::move(bytes)});
    if (!inserted && position->second.digest != digest) {
      throw std::logic_error(
          "Lite6 controllable input source contains a key collision");
    }
  }

  std::map<std::string, Entry, std::less<>> inputs_;
};

}  // namespace manyforge::deployment::test
