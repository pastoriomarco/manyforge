#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/command_schema_codec.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/controller_topology_codec.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/runtime_policy_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lite6_structural_retention_fixture.hpp"
#include "native_execution_runtime_unit_internal.hpp"

namespace manyforge::deployment::test {

inline constexpr std::string_view kLite6ControllableComponentId = "lite6_arm";
inline constexpr std::string_view kLite6ControllableModelInstanceId =
    "lite6_fixture_instance";
inline constexpr std::string_view kLite6ArmTargetId = "arm";
inline constexpr std::string_view kLite6ArmBindingId = "lite6_arm_binding";
inline constexpr std::string_view kLite6ArmDomainId = "lite6_arm_domain";
inline constexpr std::string_view kLite6GripperTargetId = "gripper";
inline constexpr std::string_view kLite6GripperBindingId =
    "lite6_gripper_binding";
inline constexpr std::string_view kLite6GripperDomainId =
    "lite6_gripper_domain";
inline constexpr std::string_view kLite6NativeExecutionRuntimeId =
    "lite6_native_execution";

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
  std::optional<QualifiedCapabilityDefinition> gripper_capability;
  std::optional<QualifiedCommandSchema> gripper_command_schema;
  std::optional<QualifiedControlInterfaceContract> gripper_control_contract;
  std::optional<QualifiedControllerTopology> gripper_topology;
  std::optional<QualifiedProviderManifest> gripper_provider_manifest;
  std::vector<QualifiedCapabilityDefinition> planner_capabilities;
  std::optional<QualifiedProviderManifest> planner_provider_manifest;
  std::optional<QualifiedRuntimeProfile> native_execution_runtime_profile;
  std::optional<QualifiedRuntimePolicy> native_runtime_policy;

  [[nodiscard]] std::span<const BuildQualifiedStructuralModelPreparation>
  structural_preparations() const noexcept {
    return structural.structural_preparations();
  }
};

namespace lite6_controllable_fixture_detail {

inline constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
inline constexpr std::string_view kEmptyProviderContractSchemaRevision =
    "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270";

[[nodiscard]] inline CanonicalValue text_array(
    std::initializer_list<std::string_view> values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.emplace_back(std::string(value));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] inline const std::string& provider_contract_role_id(
    const CanonicalValue& declaration) {
  const auto* reference =
      declaration.find_object_member("provider_contract_ref");
  const auto* role = reference == nullptr
                         ? nullptr
                         : reference->find_object_member(
                               "provider_contract_role_id");
  if (role == nullptr || !std::holds_alternative<std::string>(role->storage())) {
    throw std::logic_error(
        "provider-contract fixture declaration has no role identity");
  }
  return std::get<std::string>(role->storage());
}

[[nodiscard]] inline CanonicalValue software_hard_provider_contract_ref() {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id", CanonicalValue("software_hard_stop")},
      {"contract_schema_id",
       CanonicalValue("manyforge.value_schema.provider_contract.empty.v1")},
      {"contract_schema_revision",
       CanonicalValue(
           "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270")},
  });
}

[[nodiscard]] inline CanonicalValue native_execution_configuration() {
  return CanonicalValue(CanonicalValue::Object{
      {"mode", CanonicalValue("simulated")},
      {"planner_rng_seed", CanonicalValue(12345.0)},
      {"initial_joint_positions_rad",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(0.0), CanonicalValue(0.0), CanonicalValue(1.57),
           CanonicalValue(0.0), CanonicalValue(0.0), CanonicalValue(0.0),
       })},
      {"execution_tick_hz", CanonicalValue(100.0)},
      {"collision_monitor_period_ms", CanonicalValue(50.0)},
  });
}

[[nodiscard]] inline CanonicalValue translated_pose(double x_m) {
  return CanonicalValue(CanonicalValue::Object{
      {"position_m",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(x_m), CanonicalValue(0.0), CanonicalValue(0.0)})},
      {"orientation_xyzw", CanonicalValue(CanonicalValue::Array{
                               CanonicalValue(0.0), CanonicalValue(0.0),
                               CanonicalValue(0.0), CanonicalValue(1.0)})},
  });
}

[[nodiscard]] inline std::string indexed_id(std::string_view base,
                                            std::size_t index) {
  return index == 0U ? std::string(base)
                     : std::string(base) + "_" + std::to_string(index + 1U);
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
  const auto* text =
      value == nullptr ? nullptr : std::get_if<std::string>(&value->storage());
  if (text == nullptr) {
    throw std::logic_error(
        "Lite6 control catalog fixture omitted its revision");
  }
  return *text;
}

[[nodiscard]] inline core::SchemaRevision schema_revision_for(
    core::CatalogRecordKind kind) {
  const auto* entry =
      build_qualified_closed_schema_registry().find_catalog_schema_family(kind);
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
    std::string logical_uri, std::string bytes, const Qualified& qualified) {
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

[[nodiscard]] inline std::string catalog_key(std::string_view kind,
                                             std::string_view digest) {
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
  std::optional<QualifiedCapabilityDefinition> gripper_capability;
  std::optional<QualifiedCommandSchema> gripper_command_schema;
  std::optional<QualifiedControlInterfaceContract> gripper_contract;
  std::optional<QualifiedControllerTopology> gripper_topology;
  std::optional<QualifiedProviderManifest> gripper_provider;
  std::vector<QualifiedCapabilityDefinition> planner_capabilities;
  std::optional<QualifiedProviderManifest> planner_provider;
  std::optional<QualifiedRuntimeProfile> native_execution_runtime_profile;
  std::optional<QualifiedRuntimePolicy> native_runtime_policy;
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
      {"provider_instance_id", CanonicalValue("assistant_provider_instance")},
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
          {"value", CanonicalValue(CanonicalValue::Object{
                        {"value", CanonicalValue("http://assistant.invalid")},
                        {"value_kind", CanonicalValue("string")},
                    })},
      }),
      CanonicalValue(CanonicalValue::Object{
          {"resource_kind", CanonicalValue("model_endpoint")},
          {"semantic_kind", CanonicalValue("external_resource")},
          {"site_overlay_value_ref",
           CanonicalValue("resources.assistant_model")},
          {"value", CanonicalValue(CanonicalValue::Object{
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

[[nodiscard]] inline CanonicalValue assistant_source(const bool composer_draft_enabled = false) {
  CanonicalValue::Object modes{
      {"query", CanonicalValue(CanonicalValue::Object{
                    {"catalog", CanonicalValue(CanonicalValue::Object{
                                    {"nodes", CanonicalValue(CanonicalValue::Array{})},
                                    {"skills", CanonicalValue(CanonicalValue::Array{})},
                                    {"tools", text_array({"scene_inspect", "status_read"})},
                                })},
                    {"description", CanonicalValue("Read-only H0 query")},
                    {"enabled", CanonicalValue(true)},
                    {"grant_policy", CanonicalValue(CanonicalValue::Object{
                                         {"approval_mode", CanonicalValue(nullptr)},
                                         {"review_policy", CanonicalValue("not_required")},
                                         {"session_kind", CanonicalValue(nullptr)},
                                     })},
                    {"model_endpoint_alias", CanonicalValue("primary_model")},
                    {"provider_alias", CanonicalValue("direct_local")},
                })},
  };
  if (composer_draft_enabled) {
    modes.emplace_back(
        "composer-assistant",
        CanonicalValue(CanonicalValue::Object{
            {"catalog", CanonicalValue(CanonicalValue::Object{
                            {"nodes", CanonicalValue(CanonicalValue::Array{})},
                            {"skills", CanonicalValue(CanonicalValue::Array{})},
                            {"tools", text_array({"program_read", "scene_draft_add_object"})},
                        })},
            {"description", CanonicalValue("Bounded Composer Draft authoring")},
            {"enabled", CanonicalValue(true)},
            {"grant_policy", CanonicalValue(CanonicalValue::Object{
                                 {"approval_mode", CanonicalValue(nullptr)},
                                 {"review_policy", CanonicalValue("required")},
                                 {"session_kind", CanonicalValue(nullptr)},
                             })},
            {"model_endpoint_alias", CanonicalValue("primary_model")},
            {"provider_alias", CanonicalValue("direct_local")},
        }));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"audit_policy", CanonicalValue(CanonicalValue::Object{
                           {"redact_argument_values", CanonicalValue(true)},
                           {"store_full_prompts", CanonicalValue(false)},
                           {"store_full_responses", CanonicalValue(false)},
                       })},
      {"model_endpoint_aliases",
       CanonicalValue(CanonicalValue::Object{
           {"primary_model",
            CanonicalValue(CanonicalValue::Object{
                {"external_resource_alias", CanonicalValue("assistant_model")},
                {"model_id", CanonicalValue("i5-test-model")},
                {"request_contract",
                 CanonicalValue(CanonicalValue::Object{
                     {"default_timeout_s", CanonicalValue(30.0)},
                     {"max_output_tokens", CanonicalValue(512.0)},
                     {"supported_image_transports",
                      text_array({"http_url", "inline_data_url"})},
                     {"supports_image_input", CanonicalValue(true)},
                     {"supports_json_object", CanonicalValue(true)},
                     {"supports_response_tools", CanonicalValue(true)},
                     {"tool_call_family",
                      CanonicalValue("openai_chat_tools")},
                 })},
            })},
       })},
      {"modes", CanonicalValue(std::move(modes))},
      {"provider_aliases",
       CanonicalValue(CanonicalValue::Object{
           {"direct_local",
            CanonicalValue(CanonicalValue::Object{
                {"bridge_external_resource_alias",
                 CanonicalValue("assistant_bridge")},
                {"provider_kind", CanonicalValue("direct")},
            })},
       })},
      {"prompt_templates", CanonicalValue(CanonicalValue::Object{})},
  });
}

[[nodiscard]] inline QualifiedControlCatalog qualify_control_catalog(
    bool chained, bool native_motion = false, bool stoppable = false,
    std::string_view containment_granularity = "resource",
    bool qualified_soft_to_spring_substitution = false) {
  const std::string_view primary_capability_id =
      native_motion ? "execution.trajectory.execute" : "state.observed.current";
  const auto* descriptor =
      core::find_builtin_capability_definition(primary_capability_id);
  if (descriptor == nullptr) {
    throw std::logic_error(
        "Lite6 control fixture requires its primary capability");
  }
  CanonicalValue::Object capability_root{
      {"api_version",
       CanonicalValue(std::string(core::kCapabilityDefinitionApiVersion))},
      {"capability_id", CanonicalValue(std::string(primary_capability_id))},
      {"scope",
       CanonicalValue(std::string(core::to_string(descriptor->scope)))},
      {"provider_binding_kind", CanonicalValue(std::string(core::to_string(
                                    descriptor->provider_binding_kind)))},
      {"capability_definition_revision",
       CanonicalValue(std::string(descriptor->capability_definition_revision))},
  };
  const std::string capability_bytes =
      canonical_json(CanonicalValue(std::move(capability_root)));
  const core::ContentDigest capability_digest(sha256_digest(capability_bytes));
  const core::ContentAddressedStoreKey capability_key(
      catalog_key("capability", capability_digest.value()));
  const core::CapabilityDefinitionCatalogRecordRefDTO capability_ref{
      core::CapabilityId(std::string(primary_capability_id)),
      core::CapabilityDefinitionRevision(
          std::string(descriptor->capability_definition_revision)),
      core::SchemaId(std::string(core::kCapabilityDefinitionApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kCapabilityDefinition),
      capability_digest,
      capability_key,
  };
  auto capability = resolve_and_qualify_capability_definition(
      capability_ref, capability_key, capability_bytes);

  std::vector<QualifiedCapabilityDefinition> planner_capabilities;
  std::vector<std::string> planner_capability_bytes;
  if (native_motion) {
    constexpr std::array<std::string_view, 3U> kPlannerCapabilities{
        "planning.motion.cartesian_path",
        "planning.motion.joint_goal",
        "planning.motion.pose_goal",
    };
    planner_capabilities.reserve(kPlannerCapabilities.size());
    planner_capability_bytes.reserve(kPlannerCapabilities.size());
    for (const auto capability_id : kPlannerCapabilities) {
      const auto* planner_descriptor =
          core::find_builtin_capability_definition(capability_id);
      if (planner_descriptor == nullptr) {
        throw std::logic_error(
            "Lite6 native-motion fixture requires its maintained planning "
            "capabilities");
      }
      std::string bytes = canonical_json(CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(
                              core::kCapabilityDefinitionApiVersion))},
          {"capability_id", CanonicalValue(std::string(capability_id))},
          {"scope", CanonicalValue(std::string(
                        core::to_string(planner_descriptor->scope)))},
          {"provider_binding_kind",
           CanonicalValue(std::string(
               core::to_string(planner_descriptor->provider_binding_kind)))},
          {"capability_definition_revision",
           CanonicalValue(std::string(
               planner_descriptor->capability_definition_revision))},
      }));
      const core::ContentDigest planner_digest(sha256_digest(bytes));
      const core::ContentAddressedStoreKey planner_key(
          catalog_key("capability", planner_digest.value()));
      const core::CapabilityDefinitionCatalogRecordRefDTO planner_ref{
          core::CapabilityId(std::string(capability_id)),
          core::CapabilityDefinitionRevision(
              std::string(planner_descriptor->capability_definition_revision)),
          core::SchemaId(std::string(core::kCapabilityDefinitionApiVersion)),
          schema_revision_for(core::CatalogRecordKind::kCapabilityDefinition),
          planner_digest,
          planner_key,
      };
      planner_capabilities.push_back(
          resolve_and_qualify_capability_definition(planner_ref, planner_key,
                                                    bytes));
      planner_capability_bytes.push_back(std::move(bytes));
    }
  }

  std::optional<QualifiedRuntimeProfile> native_execution_runtime_profile;
  std::optional<std::string> native_execution_runtime_profile_bytes;
  native_execution_runtime_profile_bytes =
      detail::native_execution_runtime_profile_document();
  const core::ContentDigest profile_digest(
      sha256_digest(*native_execution_runtime_profile_bytes));
  const core::ContentAddressedStoreKey profile_key(
      catalog_key("runtime_profile", profile_digest.value()));
  const core::RuntimeProfileCatalogRecordRefDTO profile_ref{
      core::RuntimeProfileId(
          std::string(detail::kNativeExecutionRuntimeProfileId)),
      core::RuntimeProfileRevision(
          detail::native_execution_runtime_profile_revision()),
      core::SchemaId(std::string(core::kRuntimeProfileApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kRuntimeProfile),
      profile_digest,
      profile_key,
  };
  native_execution_runtime_profile = resolve_and_qualify_runtime_profile(
      profile_ref, profile_key, *native_execution_runtime_profile_bytes);

  std::optional<QualifiedRuntimePolicy> native_runtime_policy;
  std::optional<std::string> native_runtime_policy_bytes;
  if (native_motion) {
    native_runtime_policy_bytes = revisioned_document(
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(core::kRuntimePolicyApiVersion))},
            {"runtime_policy_id", CanonicalValue("lite6.native.motion")},
            {"description",
             CanonicalValue("Qualified Lite6 native motion policy")},
            {"interrupt_policy_ids", CanonicalValue(CanonicalValue::Array{})},
            {"motion_speed_limits",
             CanonicalValue(CanonicalValue::Array{CanonicalValue(
                 CanonicalValue::Object{
                     {"scope_kind", CanonicalValue("cell")},
                     {"maximum_tcp_linear_speed_mps", CanonicalValue(0.5)},
                 })})},
            {"open_loop_grasp_acknowledgements",
             CanonicalValue(CanonicalValue::Array{})},
        }),
        "catalog-record-revision/runtime_policy",
        core::kRuntimePolicyApiVersion, "runtime_policy_revision");
    const core::ContentDigest policy_digest(
        sha256_digest(*native_runtime_policy_bytes));
    const core::ContentAddressedStoreKey policy_key(
        catalog_key("runtime_policy", policy_digest.value()));
    const core::RuntimePolicyCatalogRecordRefDTO policy_ref{
        core::RuntimePolicyId("lite6.native.motion"),
        core::RuntimePolicyRevision(revision_from_document(
            *native_runtime_policy_bytes, "runtime_policy_revision")),
        core::SchemaId(std::string(core::kRuntimePolicyApiVersion)),
        schema_revision_for(core::CatalogRecordKind::kRuntimePolicy),
        policy_digest,
        policy_key,
    };
    native_runtime_policy = resolve_and_qualify_runtime_policy(
        policy_ref, policy_key, *native_runtime_policy_bytes);
  }

  CanonicalValue::Array target_capabilities{
      CanonicalValue(CanonicalValue::Object{
          {"capability_id",
           CanonicalValue(capability.value().capability_id.value())},
          {"capability_definition_revision",
           CanonicalValue(
               capability.value().capability_definition_revision.value())},
      })};
  for (const auto& planner_capability : planner_capabilities) {
    target_capabilities.emplace_back(CanonicalValue::Object{
        {"capability_id",
         CanonicalValue(planner_capability.value().capability_id.value())},
        {"capability_definition_revision",
         CanonicalValue(planner_capability.value()
                            .capability_definition_revision.value())},
    });
  }

  const std::string component_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kComponentTypeApiVersion))},
          {"component_type_id", CanonicalValue("lite6_arm_component")},
          {"tags", CanonicalValue(CanonicalValue::Array{})},
          {"model_roles",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"model_role_id", CanonicalValue("arm_model")},
                   {"cardinality", CanonicalValue("exactly_one")},
               })})},
          {"component_root", CanonicalValue(CanonicalValue::Object{
                                 {"model_role_id", CanonicalValue("arm_model")},
                                 {"frame_role_id", CanonicalValue("root")},
                             })},
          {"model_mounts", CanonicalValue(CanonicalValue::Array{})},
          {"command_targets",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"command_target_id",
                    CanonicalValue(std::string(kLite6ArmTargetId))},
                   {"model_group_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"model_group_role_id",
                             CanonicalValue("planning_group")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_model_group_role_id",
                             CanonicalValue("arm")},
                        })})},
                   {"resource_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"resource_role_id", CanonicalValue("joints")},
                            {"source_kind", CanonicalValue("model_role")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_resource_role_id",
                             CanonicalValue("arm_joints")},
                        })})},
                   {"frame_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"frame_role_id", CanonicalValue("base")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_frame_role_id", CanonicalValue("base")},
                        }),
                        CanonicalValue(CanonicalValue::Object{
                            {"frame_role_id", CanonicalValue("tcp")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_frame_role_id", CanonicalValue("tcp")},
                        })})},
                   {"collision_body_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"collision_body_role_id",
                             CanonicalValue("arm_collision")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_collision_body_role_id",
                             CanonicalValue("arm_collision")},
                        })})},
                   {"capabilities",
                    CanonicalValue(std::move(target_capabilities))},
               })})},
          {"attachment_interfaces", CanonicalValue(CanonicalValue::Array{})},
      }),
      "catalog-record-revision/component_type", core::kComponentTypeApiVersion,
      "component_type_revision");
  const core::ContentDigest component_digest(sha256_digest(component_bytes));
  const core::ContentAddressedStoreKey component_key(
      catalog_key("component", component_digest.value()));
  const core::ComponentTypeCatalogRecordRefDTO component_ref{
      core::ComponentTypeId("lite6_arm_component"),
      core::ComponentTypeRevision(
          revision_from_document(component_bytes, "component_type_revision")),
      core::SchemaId(std::string(core::kComponentTypeApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kComponentType),
      component_digest,
      component_key,
  };
  std::vector<QualifiedCapabilityDefinition> component_capabilities{capability};
  component_capabilities.insert(component_capabilities.end(),
                                planner_capabilities.begin(),
                                planner_capabilities.end());
  auto component_type = resolve_and_qualify_component_type(
      component_ref, component_key, component_bytes, component_capabilities);

  const std::string contract_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(
                              core::kControlInterfaceContractApiVersion))},
          {"control_interface_contract_id",
           CanonicalValue("lite6_arm_position")},
          {"command_interfaces",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"resource_role_id", CanonicalValue("joints")},
                   {"interface_name", CanonicalValue("position")},
               })})},
          {"state_interfaces",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"resource_role_id", CanonicalValue("joints")},
                   {"interface_name", CanonicalValue("position")},
               })})},
          {"ordering", text_array({"joints"})},
      }),
      "catalog-record-revision/control_interface_contract",
      core::kControlInterfaceContractApiVersion, "control_interface_revision");
  const core::ContentDigest contract_digest(sha256_digest(contract_bytes));
  const core::ContentAddressedStoreKey contract_key(
      catalog_key("contract", contract_digest.value()));
  const core::ControlInterfaceContractCatalogRecordRefDTO contract_ref{
      core::ControlInterfaceContractId("lite6_arm_position"),
      core::ControlInterfaceRevision(
          revision_from_document(contract_bytes, "control_interface_revision")),
      core::SchemaId(std::string(core::kControlInterfaceContractApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kControlInterfaceContract),
      contract_digest,
      contract_key,
  };
  auto contract = resolve_and_qualify_control_interface_contract(
      contract_ref, contract_key, contract_bytes);

  const CanonicalValue topology_contract_claim(CanonicalValue::Object{
      {"control_interface_contract_id",
       CanonicalValue(contract.value().control_interface_contract_id.value())},
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

  const auto stop_provider_contract_ref =
      [qualified_soft_to_spring_substitution] {
    return CanonicalValue(CanonicalValue::Object{
        {"provider_contract_role_id", CanonicalValue("stop_contract")},
        {"contract_schema_id",
         CanonicalValue(
             qualified_soft_to_spring_substitution
                 ? std::string(core::kMotionControlledStopContractSchemaId)
                 : "manyforge.value_schema.provider_contract.empty.v1")},
        {"contract_schema_revision",
         CanonicalValue(
             qualified_soft_to_spring_substitution
                 ? std::string(
                       core::kMotionControlledStopContractSchemaRevision)
                 : std::string(kEmptyProviderContractSchemaRevision))},
    });
  };
  const auto software_hard_contract_ref = [] {
    return CanonicalValue(CanonicalValue::Object{
        {"provider_contract_role_id",
         CanonicalValue("software_hard_stop")},
        {"contract_schema_id",
         CanonicalValue("manyforge.value_schema.provider_contract.empty.v1")},
        {"contract_schema_revision",
         CanonicalValue(
             std::string(kEmptyProviderContractSchemaRevision))},
    });
  };
  CanonicalValue::Array provider_contracts;
  if (stoppable) {
    CanonicalValue::Object nominal_contract{
        {"provider_contract_ref", stop_provider_contract_ref()},
        {"stop_evidence_support",
         CanonicalValue(CanonicalValue::Object{
             {"evidence_kind", CanonicalValue("motion_rest")},
             {"evidence_endpoint_role_ids", text_array({"state"})},
         })},
    };
    if (qualified_soft_to_spring_substitution) {
      nominal_contract.emplace_back(
          "stop_command_support",
          CanonicalValue(CanonicalValue::Object{
              {"command_kind", CanonicalValue("motion_controlled_stop")},
              {"command_endpoint_role_ids", text_array({"command"})},
          }));
    }
    provider_contracts.emplace_back(std::move(nominal_contract));
    provider_contracts.emplace_back(CanonicalValue::Object{
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
             {"evidence_endpoint_role_ids", text_array({"state"})},
         })},
    });
  }
  std::sort(provider_contracts.begin(), provider_contracts.end(),
            [](const auto& left, const auto& right) {
              return provider_contract_role_id(left) <
                     provider_contract_role_id(right);
            });

  const std::string provider_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kProviderManifestApiVersion))},
          {"provider_manifest_id", CanonicalValue("lite6_arm_provider")},
          {"offered_capabilities",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
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
           CanonicalValue(std::move(provider_contracts))},
      }),
      "catalog-record-revision/provider_manifest",
      core::kProviderManifestApiVersion, "provider_manifest_revision");
  const core::ContentDigest provider_digest(sha256_digest(provider_bytes));
  const core::ContentAddressedStoreKey provider_key(
      catalog_key("provider", provider_digest.value()));
  const core::ProviderManifestCatalogRecordRefDTO provider_ref{
      core::ProviderManifestId("lite6_arm_provider"),
      core::ProviderManifestRevision(
          revision_from_document(provider_bytes, "provider_manifest_revision")),
      core::SchemaId(std::string(core::kProviderManifestApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kProviderManifest),
      provider_digest,
      provider_key,
  };
  auto provider = resolve_and_qualify_provider_manifest(
      provider_ref, provider_key, provider_bytes);

  std::optional<QualifiedProviderManifest> planner_provider;
  std::optional<std::string> planner_provider_bytes;
  if (!planner_capabilities.empty()) {
    CanonicalValue::Array offered_capabilities;
    offered_capabilities.reserve(planner_capabilities.size());
    for (const auto& planner_capability : planner_capabilities) {
      offered_capabilities.emplace_back(CanonicalValue::Object{
          {"capability_id",
           CanonicalValue(
               planner_capability.value().capability_id.value())},
          {"capability_definition_revision",
           CanonicalValue(planner_capability.value()
                              .capability_definition_revision.value())},
          {"required_endpoint_role_ids",
           text_array(
               {detail::kNativeExecutionRuntimeProjectionEndpointRoleId})},
      });
    }
    planner_provider_bytes = revisioned_document(
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(core::kProviderManifestApiVersion))},
            {"provider_manifest_id",
             CanonicalValue("lite6_native_joint_planner")},
            {"offered_capabilities",
             CanonicalValue(std::move(offered_capabilities))},
            {"provider_contracts", CanonicalValue(CanonicalValue::Array{})},
        }),
        "catalog-record-revision/provider_manifest",
        core::kProviderManifestApiVersion, "provider_manifest_revision");
    const core::ContentDigest planner_provider_digest(
        sha256_digest(*planner_provider_bytes));
    const core::ContentAddressedStoreKey planner_provider_key(
        catalog_key("provider", planner_provider_digest.value()));
    const core::ProviderManifestCatalogRecordRefDTO planner_provider_ref{
        core::ProviderManifestId("lite6_native_joint_planner"),
        core::ProviderManifestRevision(revision_from_document(
            *planner_provider_bytes, "provider_manifest_revision")),
        core::SchemaId(std::string(core::kProviderManifestApiVersion)),
        schema_revision_for(core::CatalogRecordKind::kProviderManifest),
        planner_provider_digest,
        planner_provider_key,
    };
    planner_provider = resolve_and_qualify_provider_manifest(
        planner_provider_ref, planner_provider_key, *planner_provider_bytes);
  }

  const std::string stop_policy_id =
      !stoppable
          ? "lite6_not_stoppable"
          : qualified_soft_to_spring_substitution
                ? "lite6_qualified_soft_to_spring_stop"
                : "lite6_binding_state_stop";
  const CanonicalValue stop_identity = [&]() {
    if (!stoppable) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kStopEvidencePolicyApiVersion))},
          {"stop_evidence_policy_id", CanonicalValue(stop_policy_id)},
          {"disposition", CanonicalValue("not_stoppable")},
          {"containment_granularity",
           CanonicalValue(std::string(containment_granularity))},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"api_version",
         CanonicalValue(std::string(core::kStopEvidencePolicyApiVersion))},
        {"stop_evidence_policy_id", CanonicalValue(stop_policy_id)},
        {"disposition", CanonicalValue("stoppable")},
        {"containment_granularity",
         CanonicalValue(std::string(containment_granularity))},
        {"deadline_budget",
         CanonicalValue(CanonicalValue::Object{
             {"outer_duration_ns", CanonicalValue(1'000'000'000.0)},
             {"kernel_completion_reserve_ns", CanonicalValue(100'000'000.0)},
             {"phases",
              CanonicalValue(CanonicalValue::Array{
                  CanonicalValue(CanonicalValue::Object{
                      {"phase_id", CanonicalValue("nominal_command")},
                      {"deadline_offset_ns", CanonicalValue(100'000'000.0)},
                  }),
                  CanonicalValue(CanonicalValue::Object{
                      {"phase_id", CanonicalValue("nominal_terminal")},
                      {"deadline_offset_ns", CanonicalValue(300'000'000.0)},
                  }),
                  CanonicalValue(CanonicalValue::Object{
                      {"phase_id", CanonicalValue("hard_command")},
                      {"deadline_offset_ns", CanonicalValue(500'000'000.0)},
                  }),
                  CanonicalValue(CanonicalValue::Object{
                      {"phase_id", CanonicalValue("hard_terminal")},
                      {"deadline_offset_ns", CanonicalValue(800'000'000.0)},
                  }),
              })},
         })},
        {"escalation_stages",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue(CanonicalValue::Object{
                 {"stop_escalation_stage_id", CanonicalValue("nominal")},
                 {"stage_kind", CanonicalValue("nominal")},
                 {"containment_granularity",
                  CanonicalValue(std::string(containment_granularity))},
                 {"containment_command",
                  CanonicalValue(CanonicalValue::Object{
                      {"command_phase_id",
                       CanonicalValue("nominal_command")},
                      {"command",
                       qualified_soft_to_spring_substitution
                           ? CanonicalValue(CanonicalValue::Object{
                                 {"command_kind",
                                  CanonicalValue(
                                      "motion_controlled_stop")},
                                 {"provider_contract_ref",
                                  stop_provider_contract_ref()},
                             })
                           : CanonicalValue(CanonicalValue::Object{
                                 {"command_kind",
                                  CanonicalValue(
                                      "controller_goal_cancel")},
                             })},
                  })},
                 {"evidence_conjunction", CanonicalValue("all_of")},
                 {"evidence_requirements",
                  CanonicalValue(CanonicalValue::Array{
                      CanonicalValue(CanonicalValue::Object{
                          {"evidence_item_id",
                           CanonicalValue("nominal_rest")},
                          {"phase_id",
                           CanonicalValue("nominal_terminal")},
                          {"evidence",
                           CanonicalValue(CanonicalValue::Object{
                               {"evidence_kind",
                                CanonicalValue("motion_rest")},
                               {"provider_contract_ref",
                                stop_provider_contract_ref()},
                           })},
                          {"source_requirement",
                           CanonicalValue(CanonicalValue::Object{
                               {"source_kind",
                                CanonicalValue(
                                    "binding_state_authority")},
                               {"freshness_budget_ns",
                                CanonicalValue(25.0)},
                               {"state_endpoint_role_id",
                                CanonicalValue("state")},
                               {"allowed_authority_classes",
                                text_array(
                                    {"commanded", "measured", "predicted"})},
                           })},
                          {"observation_semantics",
                           CanonicalValue(CanonicalValue::Object{
                               {"observation_kind",
                                CanonicalValue("post_command_dwell")},
                               {"minimum_dwell_ns", CanonicalValue(10.0)},
                           })},
                      }),
                  })},
             }),
             CanonicalValue(CanonicalValue::Object{
                 {"stop_escalation_stage_id",
                  CanonicalValue("software_hard")},
                 {"stage_kind", CanonicalValue("software_hard")},
                 {"containment_granularity",
                  CanonicalValue(std::string(containment_granularity))},
                 {"containment_command",
                  CanonicalValue(CanonicalValue::Object{
                      {"command_phase_id", CanonicalValue("hard_command")},
                      {"command",
                       CanonicalValue(CanonicalValue::Object{
                           {"command_kind", CanonicalValue("registered")},
                           {"provider_contract_ref",
                            software_hard_contract_ref()},
                           {"registered_command_kind_id",
                            CanonicalValue("manyforge.stop.software_hard")},
                       })},
                  })},
                 {"evidence_conjunction", CanonicalValue("all_of")},
                 {"evidence_requirements",
                  CanonicalValue(CanonicalValue::Array{
                      CanonicalValue(CanonicalValue::Object{
                          {"evidence_item_id", CanonicalValue("hard_rest")},
                          {"phase_id", CanonicalValue("hard_terminal")},
                          {"evidence",
                           CanonicalValue(CanonicalValue::Object{
                               {"evidence_kind",
                                CanonicalValue("motion_rest")},
                               {"provider_contract_ref",
                                software_hard_contract_ref()},
                           })},
                          {"source_requirement",
                           CanonicalValue(CanonicalValue::Object{
                               {"source_kind",
                                CanonicalValue(
                                    "binding_state_authority")},
                               {"freshness_budget_ns",
                                CanonicalValue(25.0)},
                               {"state_endpoint_role_id",
                                CanonicalValue("state")},
                               {"allowed_authority_classes",
                                text_array(
                                    {"commanded", "measured", "predicted"})},
                           })},
                          {"observation_semantics",
                           CanonicalValue(CanonicalValue::Object{
                               {"observation_kind",
                                CanonicalValue("post_command_dwell")},
                               {"minimum_dwell_ns", CanonicalValue(10.0)},
                           })},
                      }),
                  })},
             }),
         })},
    });
  }();
  const std::string stop_bytes = revisioned_document(
      stop_identity,
      "catalog-record-revision/stop_evidence_policy",
      core::kStopEvidencePolicyApiVersion, "stop_evidence_policy_revision");
  const core::ContentDigest stop_digest(sha256_digest(stop_bytes));
  const core::ContentAddressedStoreKey stop_key(
      catalog_key("stop", stop_digest.value()));
  const core::StopEvidencePolicyCatalogRecordRefDTO stop_ref{
      core::StopEvidencePolicyId(stop_policy_id),
      core::StopEvidencePolicyRevision(
          revision_from_document(stop_bytes, "stop_evidence_policy_revision")),
      core::SchemaId(std::string(core::kStopEvidencePolicyApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kStopEvidencePolicy),
      stop_digest,
      stop_key,
  };
  auto stop =
      resolve_and_qualify_stop_evidence_policy(stop_ref, stop_key, stop_bytes);

  std::vector<Lite6InputArtifact> artifacts;
  artifacts.reserve(native_motion ? 12U : 6U);
  artifacts.push_back(catalog_artifact("catalog://capability/" +
                                           std::string(primary_capability_id),
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
  if (!planner_capabilities.empty() &&
      planner_capabilities.size() == planner_capability_bytes.size() &&
      planner_provider.has_value() && planner_provider_bytes.has_value()) {
    for (std::size_t index = 0U; index < planner_capabilities.size(); ++index) {
      artifacts.push_back(catalog_artifact(
          "catalog://capability/" +
              planner_capabilities[index].value().capability_id.value(),
          planner_capability_bytes[index], planner_capabilities[index]));
    }
    artifacts.push_back(catalog_artifact(
        "catalog://provider_manifest/lite6_native_joint_planner",
        *planner_provider_bytes, *planner_provider));
  }
  if (native_execution_runtime_profile.has_value() &&
      native_execution_runtime_profile_bytes.has_value()) {
    artifacts.push_back(catalog_artifact(
        "catalog://runtime_profile/manyforge.native_execution_runtime",
        *native_execution_runtime_profile_bytes,
        *native_execution_runtime_profile));
  }
  if (native_runtime_policy.has_value() &&
      native_runtime_policy_bytes.has_value()) {
    artifacts.push_back(catalog_artifact(
        "catalog://runtime_policy/lite6.native.motion",
        *native_runtime_policy_bytes, *native_runtime_policy));
  }
  artifacts.push_back(catalog_artifact(
      "catalog://stop_policy/" + stop_policy_id, stop_bytes, stop));

  return {
      std::move(capability),
      std::move(component_type),
      std::move(contract),
      std::move(topology),
      std::move(provider),
      std::move(stop),
      std::move(artifacts),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::move(planner_capabilities),
      std::move(planner_provider),
      std::move(native_execution_runtime_profile),
      std::move(native_runtime_policy),
  };
}

[[nodiscard]] inline QualifiedControlCatalog add_aggregate_gripper_catalog(
    QualifiedControlCatalog catalog) {
  const auto* descriptor =
      core::find_builtin_capability_definition("tool.gripper.command");
  if (descriptor == nullptr) {
    throw std::logic_error(
        "Lite6 aggregate fixture requires tool.gripper.command");
  }
  const std::string capability_bytes =
      canonical_json(CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kCapabilityDefinitionApiVersion))},
          {"capability_id", CanonicalValue("tool.gripper.command")},
          {"scope",
           CanonicalValue(std::string(core::to_string(descriptor->scope)))},
          {"provider_binding_kind", CanonicalValue(std::string(core::to_string(
                                        descriptor->provider_binding_kind)))},
          {"capability_definition_revision",
           CanonicalValue(
               std::string(descriptor->capability_definition_revision))},
      }));
  const core::ContentDigest capability_digest(sha256_digest(capability_bytes));
  const core::ContentAddressedStoreKey capability_key(
      catalog_key("capability", capability_digest.value()));
  const core::CapabilityDefinitionCatalogRecordRefDTO capability_ref{
      core::CapabilityId("tool.gripper.command"),
      core::CapabilityDefinitionRevision(
          std::string(descriptor->capability_definition_revision)),
      core::SchemaId(std::string(core::kCapabilityDefinitionApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kCapabilityDefinition),
      capability_digest,
      capability_key,
  };
  auto gripper_capability = resolve_and_qualify_capability_definition(
      capability_ref, capability_key, capability_bytes);

  const std::string command_schema_bytes =
      canonical_json(CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kCommandSchemaApiVersion))},
          {"command_schema_id",
           CanonicalValue(std::string(core::kGripperPositionCommandSchemaId))},
          {"command_schema_revision",
           CanonicalValue(
               std::string(core::kGripperPositionCommandSchemaRevision))},
          {"compatible_capabilities",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"capability_id",
                    CanonicalValue(
                        gripper_capability.value().capability_id.value())},
                   {"capability_definition_revision",
                    CanonicalValue(
                        gripper_capability.value()
                            .capability_definition_revision.value())},
               })})},
          {"command_value",
           CanonicalValue(CanonicalValue::Object{
               {"value_kind", CanonicalValue("gripper_position")},
               {"accepted_position_kinds", text_array({"angular", "linear"})},
               {"minimum_resource_count", CanonicalValue(1.0)},
               {"maximum_resource_count", CanonicalValue(4096.0)},
               {"max_effort_contract",
                CanonicalValue("optional_nonnegative_matching_coordinate")},
           })},
          {"resource_role_selectors", CanonicalValue(CanonicalValue::Array{})},
          {"channel_role_selectors", CanonicalValue(CanonicalValue::Array{})},
          {"caller_options",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"option_id", CanonicalValue("command_duration_s")},
                   {"value_kind",
                    CanonicalValue("nonnegative_duration_seconds")},
                   {"required", CanonicalValue(false)},
               })})},
          {"completion_rule", CanonicalValue("goal_fenced_target_reached")},
          {"confirmation_rule", CanonicalValue("binding_feedback_required")},
          {"stop_evidence_requirements", text_array({"command_terminal"})},
      }));
  const core::ContentDigest command_schema_digest(
      sha256_digest(command_schema_bytes));
  const core::ContentAddressedStoreKey command_schema_key(
      catalog_key("command_schema", command_schema_digest.value()));
  const core::CommandSchemaCatalogRecordRefDTO command_schema_ref{
      core::CommandSchemaId(std::string(core::kGripperPositionCommandSchemaId)),
      core::CommandSchemaRevision(
          std::string(core::kGripperPositionCommandSchemaRevision)),
      core::SchemaId(std::string(core::kCommandSchemaApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kCommandSchema),
      command_schema_digest,
      command_schema_key,
  };
  auto command_schema = resolve_and_qualify_command_schema(
      command_schema_ref, command_schema_key, command_schema_bytes);

  CanonicalValue::Array arm_capabilities{CanonicalValue(CanonicalValue::Object{
      {"capability_id",
       CanonicalValue(catalog.capability.value().capability_id.value())},
      {"capability_definition_revision",
       CanonicalValue(
           catalog.capability.value().capability_definition_revision.value())},
  })};
  std::vector<QualifiedCapabilityDefinition> capabilities{catalog.capability,
                                                          gripper_capability};
  for (const auto& planner_capability : catalog.planner_capabilities) {
    arm_capabilities.emplace_back(CanonicalValue::Object{
        {"capability_id",
         CanonicalValue(planner_capability.value().capability_id.value())},
        {"capability_definition_revision",
         CanonicalValue(planner_capability.value()
                            .capability_definition_revision.value())},
    });
    capabilities.push_back(planner_capability);
  }
  const std::string component_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kComponentTypeApiVersion))},
          {"component_type_id", CanonicalValue("lite6_arm_gripper_component")},
          {"tags", text_array({"articulated-arm", "articulated-gripper"})},
          {"model_roles",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"model_role_id", CanonicalValue("arm_model")},
                   {"cardinality", CanonicalValue("exactly_one")},
               })})},
          {"component_root", CanonicalValue(CanonicalValue::Object{
                                 {"model_role_id", CanonicalValue("arm_model")},
                                 {"frame_role_id", CanonicalValue("root")},
                             })},
          {"model_mounts", CanonicalValue(CanonicalValue::Array{})},
          {"command_targets",
           CanonicalValue(CanonicalValue::Array{
               CanonicalValue(CanonicalValue::Object{
                   {"command_target_id",
                    CanonicalValue(std::string(kLite6ArmTargetId))},
                   {"model_group_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"model_group_role_id",
                             CanonicalValue("planning_group")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_model_group_role_id",
                             CanonicalValue("arm")},
                        })})},
                   {"resource_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"resource_role_id", CanonicalValue("joints")},
                            {"source_kind", CanonicalValue("model_role")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_resource_role_id",
                             CanonicalValue("arm_joints")},
                        })})},
                   {"frame_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"frame_role_id", CanonicalValue("base")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_frame_role_id", CanonicalValue("base")},
                        }),
                        CanonicalValue(CanonicalValue::Object{
                            {"frame_role_id", CanonicalValue("tcp")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_frame_role_id", CanonicalValue("tcp")},
                        })})},
                   {"collision_body_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"collision_body_role_id",
                             CanonicalValue("arm_collision")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_collision_body_role_id",
                             CanonicalValue("arm_collision")},
                        })})},
                   {"capabilities",
                    CanonicalValue(std::move(arm_capabilities))},
               }),
               CanonicalValue(CanonicalValue::Object{
                   {"command_target_id",
                    CanonicalValue(std::string(kLite6GripperTargetId))},
                   {"model_group_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"model_group_role_id",
                             CanonicalValue("planning_group")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_model_group_role_id",
                             CanonicalValue("gripper")},
                        })})},
                   {"resource_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"resource_role_id", CanonicalValue("fingers")},
                            {"source_kind", CanonicalValue("model_role")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_resource_role_id",
                             CanonicalValue("gripper_joint")},
                        })})},
                   {"frame_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"frame_role_id", CanonicalValue("tcp")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_frame_role_id", CanonicalValue("tcp")},
                        })})},
                   {"collision_body_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"collision_body_role_id",
                             CanonicalValue("gripper_collision")},
                            {"model_role_id", CanonicalValue("arm_model")},
                            {"asset_collision_body_role_id",
                             CanonicalValue("gripper_collision")},
                        })})},
                   {"capabilities",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"capability_id",
                             CanonicalValue(gripper_capability.value()
                                                .capability_id.value())},
                            {"capability_definition_revision",
                             CanonicalValue(
                                 gripper_capability.value()
                                     .capability_definition_revision.value())},
                        })})},
               }),
           })},
          {"attachment_interfaces", CanonicalValue(CanonicalValue::Array{})},
      }),
      "catalog-record-revision/component_type", core::kComponentTypeApiVersion,
      "component_type_revision");
  const core::ContentDigest component_digest(sha256_digest(component_bytes));
  const core::ContentAddressedStoreKey component_key(
      catalog_key("component", component_digest.value()));
  const core::ComponentTypeCatalogRecordRefDTO component_ref{
      core::ComponentTypeId("lite6_arm_gripper_component"),
      core::ComponentTypeRevision(
          revision_from_document(component_bytes, "component_type_revision")),
      core::SchemaId(std::string(core::kComponentTypeApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kComponentType),
      component_digest,
      component_key,
  };
  auto component_type = resolve_and_qualify_component_type(
      component_ref, component_key, component_bytes, capabilities);

  const std::string contract_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(
                              core::kControlInterfaceContractApiVersion))},
          {"control_interface_contract_id",
           CanonicalValue("lite6_gripper_position")},
          {"command_interfaces",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"resource_role_id", CanonicalValue("fingers")},
                   {"interface_name", CanonicalValue("position")},
               })})},
          {"state_interfaces",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"resource_role_id", CanonicalValue("fingers")},
                   {"interface_name", CanonicalValue("position")},
               })})},
          {"ordering", text_array({"fingers"})},
      }),
      "catalog-record-revision/control_interface_contract",
      core::kControlInterfaceContractApiVersion, "control_interface_revision");
  const core::ContentDigest contract_digest(sha256_digest(contract_bytes));
  const core::ContentAddressedStoreKey contract_key(
      catalog_key("contract", contract_digest.value()));
  const core::ControlInterfaceContractCatalogRecordRefDTO contract_ref{
      core::ControlInterfaceContractId("lite6_gripper_position"),
      core::ControlInterfaceRevision(
          revision_from_document(contract_bytes, "control_interface_revision")),
      core::SchemaId(std::string(core::kControlInterfaceContractApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kControlInterfaceContract),
      contract_digest,
      contract_key,
  };
  auto contract = resolve_and_qualify_control_interface_contract(
      contract_ref, contract_key, contract_bytes);

  const CanonicalValue interface_claim(CanonicalValue::Object{
      {"resource_role_id", CanonicalValue("fingers")},
      {"interface_name", CanonicalValue("position")},
  });
  const CanonicalValue contract_claim(CanonicalValue::Object{
      {"control_interface_contract_id",
       CanonicalValue(contract.value().control_interface_contract_id.value())},
      {"control_interface_revision",
       CanonicalValue(contract.value().control_interface_revision.value())},
      {"claimed_directions", text_array({"command", "state"})},
  });
  const std::string topology_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kControllerTopologyApiVersion))},
          {"controller_topology_id",
           CanonicalValue("lite6_gripper_direct_topology")},
          {"controllers",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"controller_role_id", CanonicalValue("hardware")},
                   {"control_interface_claims",
                    CanonicalValue(CanonicalValue::Array{contract_claim})},
                   {"exported_reference_interfaces",
                    CanonicalValue(CanonicalValue::Array{})},
                   {"exported_state_interfaces",
                    CanonicalValue(CanonicalValue::Array{})},
               })})},
          {"chain_edges", CanonicalValue(CanonicalValue::Array{})},
          {"activation_sets",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"activation_set_id", CanonicalValue("gripper_direct")},
                   {"active_controller_role_ids", text_array({"hardware"})},
                   {"active_chain_edges",
                    CanonicalValue(CanonicalValue::Array{})},
               })})},
          {"legal_routes",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"control_route_id", CanonicalValue("gripper_position")},
                   {"control_interface_contract_id",
                    CanonicalValue(contract.value()
                                       .control_interface_contract_id.value())},
                   {"control_interface_revision",
                    CanonicalValue(
                        contract.value().control_interface_revision.value())},
                   {"ordered_controller_role_ids", text_array({"hardware"})},
                   {"compatible_activation_set_ids",
                    text_array({"gripper_direct"})},
               })})},
      }),
      "catalog-record-revision/controller_topology",
      core::kControllerTopologyApiVersion, "topology_revision");
  const core::ContentDigest topology_digest(sha256_digest(topology_bytes));
  const core::ContentAddressedStoreKey topology_key(
      catalog_key("topology", topology_digest.value()));
  const core::ControllerTopologyCatalogRecordRefDTO topology_ref{
      core::ControllerTopologyId("lite6_gripper_direct_topology"),
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
  (void)interface_claim;

  const std::string provider_bytes = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kProviderManifestApiVersion))},
          {"provider_manifest_id", CanonicalValue("lite6_gripper_provider")},
          {"offered_capabilities",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"capability_id",
                    CanonicalValue(
                        gripper_capability.value().capability_id.value())},
                   {"capability_definition_revision",
                    CanonicalValue(
                        gripper_capability.value()
                            .capability_definition_revision.value())},
                   {"required_endpoint_role_ids",
                    text_array({"command", "state"})},
               })})},
          {"provider_contracts", CanonicalValue(CanonicalValue::Array{})},
      }),
      "catalog-record-revision/provider_manifest",
      core::kProviderManifestApiVersion, "provider_manifest_revision");
  const core::ContentDigest provider_digest(sha256_digest(provider_bytes));
  const core::ContentAddressedStoreKey provider_key(
      catalog_key("provider", provider_digest.value()));
  const core::ProviderManifestCatalogRecordRefDTO provider_ref{
      core::ProviderManifestId("lite6_gripper_provider"),
      core::ProviderManifestRevision(
          revision_from_document(provider_bytes, "provider_manifest_revision")),
      core::SchemaId(std::string(core::kProviderManifestApiVersion)),
      schema_revision_for(core::CatalogRecordKind::kProviderManifest),
      provider_digest,
      provider_key,
  };
  auto provider = resolve_and_qualify_provider_manifest(
      provider_ref, provider_key, provider_bytes);

  const auto old_component_key =
      catalog.component_type.reference().content_addressed_store_key;
  catalog.artifacts.erase(
      std::remove_if(catalog.artifacts.begin(), catalog.artifacts.end(),
                     [&](const auto& artifact) {
                       return artifact.store_key == old_component_key;
                     }),
      catalog.artifacts.end());
  catalog.artifacts.push_back(
      catalog_artifact("catalog://capability/tool.gripper.command",
                       capability_bytes, gripper_capability));
  catalog.artifacts.push_back(
      catalog_artifact("catalog://command_schema/gripper.command_position",
                       command_schema_bytes, command_schema));
  catalog.artifacts.push_back(
      catalog_artifact("catalog://component_type/lite6_arm_gripper_component",
                       component_bytes, component_type));
  catalog.artifacts.push_back(
      catalog_artifact("catalog://control_interface/lite6_gripper_position",
                       contract_bytes, contract));
  catalog.artifacts.push_back(catalog_artifact(
      "catalog://controller_topology/lite6_gripper_direct_topology",
      topology_bytes, topology));
  catalog.artifacts.push_back(
      catalog_artifact("catalog://provider_manifest/lite6_gripper_provider",
                       provider_bytes, provider));
  catalog.component_type = std::move(component_type);
  catalog.gripper_capability = std::move(gripper_capability);
  catalog.gripper_command_schema = std::move(command_schema);
  catalog.gripper_contract = std::move(contract);
  catalog.gripper_topology = std::move(topology);
  catalog.gripper_provider = std::move(provider);
  return catalog;
}

[[nodiscard]] inline GraphRevisionVerifiedCellGraph make_graph(
    const Lite6StructuralRetentionFixture& structural,
    const QualifiedControlCatalog& catalog, std::size_t instance_count = 1U,
    std::string_view cell_id = {},
    std::string_view containment_granularity = "resource") {
  const bool aggregate = catalog.gripper_capability.has_value();
  const bool native_motion = !catalog.planner_capabilities.empty();
  if (instance_count == 0U || instance_count > 32U) {
    throw std::logic_error(
        "Lite6 control fixture instance count is outside its test bound");
  }
  if (aggregate &&
      (instance_count != 1U || !catalog.gripper_command_schema.has_value() ||
       !catalog.gripper_contract.has_value() ||
       !catalog.gripper_topology.has_value() ||
       !catalog.gripper_provider.has_value())) {
    throw std::logic_error(
        "Lite6 aggregate fixture requires one complete gripper catalog");
  }

  CanonicalValue::Array model_instances;
  CanonicalValue::Array runtime_units;
  CanonicalValue::Array components;
  CanonicalValue::Array domains;
  CanonicalValue::Array bindings;
  model_instances.reserve(instance_count);
  runtime_units.reserve(instance_count);
  components.reserve(instance_count);
  domains.reserve(instance_count);
  bindings.reserve(instance_count);

  for (std::size_t instance = 0U; instance < instance_count; ++instance) {
    const std::string model_instance_id =
        indexed_id(kLite6ControllableModelInstanceId, instance);
    const std::string component_id =
        indexed_id(kLite6ControllableComponentId, instance);
    const std::string domain_id = indexed_id(kLite6ArmDomainId, instance);
    const std::string binding_id = indexed_id(kLite6ArmBindingId, instance);
    const std::string runtime_unit_id =
        indexed_id(kLite6NativeExecutionRuntimeId, instance);
    const std::string resource_prefix =
        instance == 0U ? "lite6" : "lite6_" + std::to_string(instance + 1U);

    CanonicalValue::Array resource_bindings;
    for (std::size_t coordinate = 0U; coordinate < 6U; ++coordinate) {
      resource_bindings.emplace_back(CanonicalValue::Object{
          {"resource_role_id", CanonicalValue("joints")},
          {"resource_id", CanonicalValue(resource_prefix + "_joint_" +
                                         std::to_string(coordinate + 1U))},
      });
    }

    model_instances.emplace_back(CanonicalValue::Object{
        {"model_instance_id", CanonicalValue(model_instance_id)},
        {"model_asset_id",
         CanonicalValue(structural.catalog_reference.record_id.value())},
        {"model_revision",
         CanonicalValue(structural.catalog_reference.record_revision.value())},
    });
    runtime_units.emplace_back(CanonicalValue::Object{
        {"runtime_unit_id", CanonicalValue(runtime_unit_id)},
        {"runtime_profile_id",
         CanonicalValue(
             std::string(detail::kNativeExecutionRuntimeProfileId))},
        {"runtime_profile_revision",
         CanonicalValue(detail::native_execution_runtime_profile_revision())},
        {"dependency_runtime_unit_ids",
         CanonicalValue(CanonicalValue::Array{})},
        {"configuration", native_execution_configuration()},
    });
    components.emplace_back(CanonicalValue::Object{
        {"controllable_component_id", CanonicalValue(component_id)},
        {"component_type_id",
         CanonicalValue(
             catalog.component_type.value().component_type_id.value())},
        {"component_type_revision",
         CanonicalValue(
             catalog.component_type.value().component_type_revision.value())},
        {"model_role_bindings",
         CanonicalValue(
             CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                 {"model_role_id", CanonicalValue("arm_model")},
                 {"model_instance_id", CanonicalValue(model_instance_id)},
             })})},
        {"runtime_unit_ids", text_array({runtime_unit_id})},
        {"root_pose_in_cell",
         translated_pose(static_cast<double>(instance) * 2.0)},
    });
    domains.emplace_back(CanonicalValue::Object{
        {"control_domain_id", CanonicalValue(domain_id)},
        {"control_domain_revision", CanonicalValue(std::string(kZeroDigest))},
        {"runtime_unit_ids", text_array({runtime_unit_id})},
        {"control_coordinator_endpoint_role", CanonicalValue("coordinator")},
        {"controller_topology_id",
         CanonicalValue(
             catalog.topology.value().controller_topology_id.value())},
        {"topology_revision",
         CanonicalValue(catalog.topology.value().topology_revision.value())},
        {"containment_granularity",
         CanonicalValue(std::string(containment_granularity))},
    });
    CanonicalValue::Object arm_binding{
        {"control_binding_id", CanonicalValue(binding_id)},
        {"control_binding_revision", CanonicalValue(std::string(kZeroDigest))},
        {"controllable_component_id", CanonicalValue(component_id)},
        {"command_target_id", CanonicalValue(std::string(kLite6ArmTargetId))},
        {"capability_id",
         CanonicalValue(catalog.capability.value().capability_id.value())},
        {"capability_definition_revision",
         CanonicalValue(catalog.capability.value()
                            .capability_definition_revision.value())},
        {"control_domain_id", CanonicalValue(domain_id)},
        {"resource_bindings", CanonicalValue(std::move(resource_bindings))},
        {"control_interface_contract_id",
         CanonicalValue(
             catalog.contract.value().control_interface_contract_id.value())},
        {"control_interface_revision",
         CanonicalValue(
             catalog.contract.value().control_interface_revision.value())},
        {"controller_topology_id",
         CanonicalValue(
             catalog.topology.value().controller_topology_id.value())},
        {"topology_revision",
         CanonicalValue(catalog.topology.value().topology_revision.value())},
        {"legal_control_route_ids", text_array({"arm_position"})},
        {"route_endpoint_projections",
         CanonicalValue(
             CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                 {"control_route_id", CanonicalValue("arm_position")},
                 {"command_endpoint_role_ids", text_array({"command"})},
                 {"state_endpoint_role_ids", text_array({"state"})},
             })})},
        {"provider_instance_id",
         CanonicalValue(indexed_id("lite6_arm_provider_instance", instance))},
        {"provider_runtime_unit_id", CanonicalValue(runtime_unit_id)},
        {"provider_manifest_id",
         CanonicalValue(catalog.provider.value().provider_manifest_id.value())},
        {"provider_manifest_revision",
         CanonicalValue(
             catalog.provider.value().provider_manifest_revision.value())},
        {"state_authority_id",
         CanonicalValue(indexed_id(
             native_motion ? "lite6_predicted_state" : "lite6_measured_state",
             instance))},
        {"state_authority_class",
         CanonicalValue(native_motion ? "predicted" : "measured")},
        {"stop_evidence_policy_id",
         CanonicalValue(catalog.stop.value().stop_evidence_policy_id.value())},
        {"stop_evidence_policy_revision",
         CanonicalValue(
             catalog.stop.value().stop_evidence_policy_revision.value())},
        {"containment_granularity",
         CanonicalValue(std::string(containment_granularity))},
    };
    if (native_motion) {
      arm_binding.emplace_back(
          "runtime_scale_route_qualifications",
          CanonicalValue(
              CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                  {"control_route_id", CanonicalValue("arm_position")},
                  {"scaling_domain_id",
                   CanonicalValue(indexed_id("lite6_native_scale", instance))},
                  {"command_endpoint_role_id", CanonicalValue("command")},
                  {"applied_state_endpoint_role_id", CanonicalValue("state")},
                  {"acknowledgement_kind", CanonicalValue("applied_state")},
                  {"maximum_slew_per_second", CanonicalValue(4.0)},
                  {"freshness_budget_ns", CanonicalValue(100'000'000.0)},
              })}));
    }
    bindings.emplace_back(std::move(arm_binding));
    if (aggregate) {
      const auto& gripper_capability = *catalog.gripper_capability;
      const auto& gripper_contract = *catalog.gripper_contract;
      const auto& gripper_topology = *catalog.gripper_topology;
      const auto& gripper_provider = *catalog.gripper_provider;
      domains.emplace_back(CanonicalValue::Object{
          {"control_domain_id",
           CanonicalValue(std::string(kLite6GripperDomainId))},
          {"control_domain_revision", CanonicalValue(std::string(kZeroDigest))},
          {"runtime_unit_ids", text_array({runtime_unit_id})},
          {"control_coordinator_endpoint_role", CanonicalValue("coordinator")},
          {"controller_topology_id",
           CanonicalValue(
               gripper_topology.value().controller_topology_id.value())},
          {"topology_revision",
           CanonicalValue(gripper_topology.value().topology_revision.value())},
          {"containment_granularity", CanonicalValue("resource")},
      });
      bindings.emplace_back(CanonicalValue::Object{
          {"control_binding_id",
           CanonicalValue(std::string(kLite6GripperBindingId))},
          {"control_binding_revision",
           CanonicalValue(std::string(kZeroDigest))},
          {"controllable_component_id", CanonicalValue(component_id)},
          {"command_target_id",
           CanonicalValue(std::string(kLite6GripperTargetId))},
          {"capability_id",
           CanonicalValue(gripper_capability.value().capability_id.value())},
          {"capability_definition_revision",
           CanonicalValue(gripper_capability.value()
                              .capability_definition_revision.value())},
          {"control_domain_id",
           CanonicalValue(std::string(kLite6GripperDomainId))},
          {"resource_bindings",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"resource_role_id", CanonicalValue("fingers")},
                   {"resource_id", CanonicalValue("lite6_gripper_joint")},
               })})},
          {"control_interface_contract_id",
           CanonicalValue(
               gripper_contract.value().control_interface_contract_id.value())},
          {"control_interface_revision",
           CanonicalValue(
               gripper_contract.value().control_interface_revision.value())},
          {"controller_topology_id",
           CanonicalValue(
               gripper_topology.value().controller_topology_id.value())},
          {"topology_revision",
           CanonicalValue(gripper_topology.value().topology_revision.value())},
          {"legal_control_route_ids", text_array({"gripper_position"})},
          {"route_endpoint_projections",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"control_route_id", CanonicalValue("gripper_position")},
                   {"command_endpoint_role_ids", text_array({"command"})},
                   {"state_endpoint_role_ids", text_array({"state"})},
               })})},
          {"provider_instance_id",
           CanonicalValue("lite6_gripper_provider_instance")},
          {"provider_runtime_unit_id", CanonicalValue(runtime_unit_id)},
          {"provider_manifest_id",
           CanonicalValue(
               gripper_provider.value().provider_manifest_id.value())},
          {"provider_manifest_revision",
           CanonicalValue(
               gripper_provider.value().provider_manifest_revision.value())},
          {"state_authority_id",
           CanonicalValue(native_motion ? "lite6_gripper_predicted_state"
                                        : "lite6_gripper_measured_state")},
          {"state_authority_class",
           CanonicalValue(native_motion ? "predicted" : "measured")},
          {"stop_evidence_policy_id",
           CanonicalValue(
               catalog.stop.value().stop_evidence_policy_id.value())},
          {"stop_evidence_policy_revision",
           CanonicalValue(
               catalog.stop.value().stop_evidence_policy_revision.value())},
          {"containment_granularity", CanonicalValue("resource")},
      });
    }
  }

  const std::string selected_cell_id =
      cell_id.empty()
          ? (aggregate       ? "i6_lite6_arm_gripper_aggregate_cell"
             : native_motion ? "i6_lite6_native_motion_cell"
             : instance_count == 1U ? "i3_lite6_controllable_cell"
                                    : "i6_two_lite6_controllable_cell")
          : std::string(cell_id);
  const CanonicalValue graph(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(core::kCellGraphApiVersion))},
      {"graph_revision", CanonicalValue(std::string(kZeroDigest))},
      {"cell_id", CanonicalValue(selected_cell_id)},
      {"runtime_units", CanonicalValue(std::move(runtime_units))},
      {"model_instances", CanonicalValue(std::move(model_instances))},
      {"controllable_components", CanonicalValue(std::move(components))},
      {"control_domains", CanonicalValue(std::move(domains))},
      {"control_bindings", CanonicalValue(std::move(bindings))},
      {"component_attachments", CanonicalValue(CanonicalValue::Array{})},
  });
  const auto intrinsic =
      validate_intrinsic_cell_graph_candidate(decode_cell_graph_candidate(
          parse_strict_json_model_yaml(canonical_json(graph))));
  const std::span<const QualifiedRuntimeProfile> runtime_profiles =
      catalog.native_execution_runtime_profile.has_value()
          ? std::span<const QualifiedRuntimeProfile>(
                &*catalog.native_execution_runtime_profile, 1U)
          : std::span<const QualifiedRuntimeProfile>{};
  return compute_graph_revision(
      prepare_cell_graph_identity(intrinsic, runtime_profiles));
}

[[nodiscard]] inline RevisionVerifiedDeploymentApplicabilityInputs
make_applicability_inputs(
    const GraphRevisionVerifiedCellGraph& graph,
    const QualifiedProviderManifest& provider,
    std::span<const QualifiedCapabilityDefinition> planner_capabilities,
    const QualifiedProviderManifest* planner_provider,
    const QualifiedRuntimePolicy* runtime_policy,
    const AssistantOverlayFixture* assistant_overlay,
    const bool composer_draft_enabled = false,
    const bool stoppable = false,
    const bool qualified_soft_to_spring_substitution = false) {
  CanonicalValue::Array desired_configurations;
  desired_configurations.reserve(graph.graph().control_domains.size());
  for (const auto& domain : graph.graph().control_domains) {
    CanonicalValue::Array binding_routes;
    for (const auto& binding : graph.graph().control_bindings) {
      if (binding.control_domain_id != domain.control_domain_id) {
        continue;
      }
      binding_routes.emplace_back(CanonicalValue::Object{
          {"control_binding_id",
           CanonicalValue(binding.control_binding_id.value())},
          {"control_binding_revision",
           CanonicalValue(binding.control_binding_revision.value())},
          {"control_route_id",
           CanonicalValue(binding.command_target_id.value() ==
                                  kLite6GripperTargetId
                              ? "gripper_position"
                              : "arm_position")},
      });
    }
    if (binding_routes.empty()) {
      throw std::logic_error(
          "Lite6 control fixture domain has no control binding");
    }
    desired_configurations.emplace_back(CanonicalValue::Object{
        {"control_domain_id", CanonicalValue(domain.control_domain_id.value())},
        {"control_domain_revision",
         CanonicalValue(domain.control_domain_revision.value())},
        {"controller_topology_id",
         CanonicalValue(domain.controller_topology_id.value())},
        {"topology_revision", CanonicalValue(domain.topology_revision.value())},
        {"activation_set_id",
         CanonicalValue(domain.controller_topology_id.value() ==
                                "lite6_gripper_direct_topology"
                            ? "gripper_direct"
                        : domain.controller_topology_id.value() ==
                                "lite6_arm_direct_topology"
                            ? "arm_direct"
                            : "arm_chain")},
        {"binding_routes", CanonicalValue(std::move(binding_routes))},
    });
  }
  CanonicalValue::Array capability_provider_bindings;
  if (planner_capabilities.empty() != (planner_provider == nullptr)) {
    throw std::logic_error(
        "Lite6 native-motion provider fixture is incomplete");
  }
  for (const auto& planner_capability : planner_capabilities) {
    const std::string_view capability_id =
        planner_capability.value().capability_id.value();
    const std::string_view binding_id =
        capability_id == "planning.motion.cartesian_path"
            ? "lite6_cartesian_planner_binding"
        : capability_id == "planning.motion.joint_goal"
            ? "lite6_joint_planner_binding"
        : capability_id == "planning.motion.pose_goal"
            ? "lite6_pose_planner_binding"
            : throw std::logic_error(
                  "Lite6 native-motion fixture selected an unknown planner "
                  "capability");
    for (std::size_t index = 0U; index < graph.graph().control_bindings.size();
         ++index) {
      const auto& binding = graph.graph().control_bindings[index];
      if (binding.command_target_id.value() != kLite6ArmTargetId) {
        continue;
      }
      const auto component =
          std::find_if(graph.graph().controllable_components.begin(),
                       graph.graph().controllable_components.end(),
                       [&](const auto& candidate) {
                         return candidate.controllable_component_id ==
                                binding.controllable_component_id;
                       });
      if (component == graph.graph().controllable_components.end()) {
        throw std::logic_error("Lite6 native-motion binding has no component");
      }
      if (component->runtime_unit_ids.size() != 1U) {
        throw std::logic_error(
            "Lite6 native-motion planner requires one exact execution "
            "runtime owner");
      }
      capability_provider_bindings.emplace_back(CanonicalValue::Object{
          {"capability_provider_binding_id",
           CanonicalValue(indexed_id(binding_id, index))},
          {"capability_provider_binding_revision",
           CanonicalValue(std::string(kZeroDigest))},
          {"capability_id",
           CanonicalValue(planner_capability.value().capability_id.value())},
          {"capability_definition_revision",
           CanonicalValue(planner_capability.value()
                              .capability_definition_revision.value())},
          {"scope", CanonicalValue("target_binding")},
          {"target",
           CanonicalValue(CanonicalValue::Object{
               {"controllable_component_id",
                CanonicalValue(binding.controllable_component_id.value())},
               {"component_type_revision",
                CanonicalValue(component->component_type_revision.value())},
               {"command_target_id",
                CanonicalValue(binding.command_target_id.value())},
               {"control_binding_id",
                CanonicalValue(binding.control_binding_id.value())},
               {"control_binding_revision",
                CanonicalValue(binding.control_binding_revision.value())},
           })},
          {"provider_instance_id",
           CanonicalValue(indexed_id("lite6_native_joint_planner", index))},
          {"provider_manifest_id",
           CanonicalValue(
               planner_provider->value().provider_manifest_id.value())},
          {"provider_manifest_revision",
           CanonicalValue(
               planner_provider->value().provider_manifest_revision.value())},
          {"endpoint_bindings",
           CanonicalValue(CanonicalValue::Array{
               CanonicalValue(CanonicalValue::Object{
                   {"endpoint_role_id",
                    CanonicalValue(std::string(
                        detail::
                            kNativeExecutionRuntimeProjectionEndpointRoleId))},
                   {"source_kind", CanonicalValue("runtime_unit")},
                   {"source_id",
                    CanonicalValue(
                        component->runtime_unit_ids.front().value())},
                   {"source_endpoint_role_id",
                    CanonicalValue(std::string(
                        detail::
                            kNativeExecutionRuntimeProjectionEndpointRoleId))},
               }),
           })},
      });
    }
  }

  CanonicalValue::Object root_members{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"desired_control_configurations",
       CanonicalValue(std::move(desired_configurations))},
      {"capability_provider_bindings",
       CanonicalValue(std::move(capability_provider_bindings))},
  };
  if (stoppable || runtime_policy != nullptr) {
    CanonicalValue::Array stop_stage_parameters;
    if (stoppable) {
      for (const auto& binding : graph.graph().control_bindings) {
        const auto domain = std::find_if(
            graph.graph().control_domains.begin(),
            graph.graph().control_domains.end(), [&](const auto& candidate) {
              return candidate.control_domain_id == binding.control_domain_id;
            });
        if (domain == graph.graph().control_domains.end()) {
          throw std::logic_error(
              "Lite6 Stop parameter binding has no exact control domain");
        }
        for (const auto& route_id : binding.legal_control_route_ids) {
          if (qualified_soft_to_spring_substitution) {
            CanonicalValue::Array resource_limits;
            resource_limits.reserve(binding.resource_bindings.size());
            for (const auto& resource : binding.resource_bindings) {
              resource_limits.emplace_back(CanonicalValue::Object{
                  {"resource_id",
                   CanonicalValue(resource.resource_id.value())},
                  {"max_deceleration",
                   CanonicalValue(CanonicalValue::Object{
                       {"coordinate_kind",
                        CanonicalValue("angular_rad_s2")},
                       {"value_rad_s2", CanonicalValue(4.0)},
                   })},
                  {"settle_velocity_threshold",
                   CanonicalValue(CanonicalValue::Object{
                       {"coordinate_kind",
                        CanonicalValue("angular_rad_s")},
                       {"value_rad_s", CanonicalValue(0.01)},
                   })},
              });
            }
            stop_stage_parameters.emplace_back(CanonicalValue::Object{
                {"control_domain_id",
                 CanonicalValue(binding.control_domain_id.value())},
                {"control_domain_revision",
                 CanonicalValue(domain->control_domain_revision.value())},
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
                {"stop_escalation_stage_id", CanonicalValue("nominal")},
                {"provider_contract_ref",
                 CanonicalValue(CanonicalValue::Object{
                     {"provider_contract_role_id",
                      CanonicalValue("stop_contract")},
                     {"contract_schema_id",
                      CanonicalValue(std::string(
                          core::kMotionControlledStopContractSchemaId))},
                     {"contract_schema_revision",
                      CanonicalValue(std::string(
                          core::kMotionControlledStopContractSchemaRevision))},
                 })},
                {"default_contract_parameters",
                 CanonicalValue(CanonicalValue::Object{
                     {"stop_mode", CanonicalValue("soft_on_path")},
                     {"deceleration_time_s", CanonicalValue(0.25)},
                     {"per_resource_limits",
                      CanonicalValue(std::move(resource_limits))},
                     {"allow_natural_completion_on_short_path",
                      CanonicalValue(false)},
                     {"recoverable_substitution_mode",
                      CanonicalValue("spring_back_hold")},
                 })},
            });
          }
          stop_stage_parameters.emplace_back(CanonicalValue::Object{
              {"control_domain_id",
               CanonicalValue(binding.control_domain_id.value())},
              {"control_domain_revision",
               CanonicalValue(domain->control_domain_revision.value())},
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
               CanonicalValue("software_hard")},
              {"provider_contract_ref",
               software_hard_provider_contract_ref()},
              {"default_contract_parameters",
               CanonicalValue(CanonicalValue::Object{})},
          });
        }
      }
    }
    CanonicalValue::Array retained_policy_records;
    if (runtime_policy != nullptr) {
      const auto& reference = runtime_policy->reference();
      retained_policy_records.emplace_back(CanonicalValue::Object{
          {"record_kind", CanonicalValue("runtime_policy")},
          {"record_id", CanonicalValue(reference.record_id.value())},
          {"record_revision",
           CanonicalValue(reference.record_revision.value())},
          {"schema_id", CanonicalValue(reference.schema_id.value())},
          {"schema_revision",
           CanonicalValue(reference.schema_revision.value())},
          {"content_digest",
           CanonicalValue(reference.content_digest.value())},
          {"content_addressed_store_key",
           CanonicalValue(reference.content_addressed_store_key.value())},
      });
    }
    root_members.emplace_back(
        "policies",
        CanonicalValue(CanonicalValue::Object{
            {"retained_policy_records",
             CanonicalValue(std::move(retained_policy_records))},
            {"stop_stage_parameters",
             CanonicalValue(std::move(stop_stage_parameters))},
            {"commissioned_stop_interfaces",
             CanonicalValue(CanonicalValue::Array{})},
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
  if (assistant_overlay != nullptr) {
    root_members.emplace_back("assistant", assistant_source(composer_draft_enabled));
    root_members.emplace_back(
        "external_resources",
        CanonicalValue(CanonicalValue::Array{
            assistant_external_resource("assistant_bridge", "service_endpoint",
                                        "resources.assistant_bridge", provider),
            assistant_external_resource("assistant_model", "model_endpoint",
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
                                          bool assistant_enabled = false,
                                          std::size_t instance_count = 1U,
                                          bool aggregate_gripper = false,
                                          bool native_motion = false,
                                          std::string_view cell_id = {},
                                          bool composer_draft_enabled = false,
                                          bool stoppable = false,
                                          std::string_view containment_granularity =
                                              "resource",
                                          bool qualified_soft_to_spring_substitution =
                                              false) {
  namespace fixture_detail = lite6_controllable_fixture_detail;
  if (aggregate_gripper &&
      (chained || assistant_enabled || instance_count != 1U)) {
    throw std::logic_error(
        "Lite6 aggregate fixture currently selects one direct native "
        "component");
  }
  if (composer_draft_enabled && !assistant_enabled) {
    throw std::logic_error("Composer Draft fixture requires assistant settings");
  }
  if (qualified_soft_to_spring_substitution &&
      (!stoppable || !native_motion || chained || aggregate_gripper ||
       instance_count != 1U)) {
    throw std::logic_error(
        "qualified soft-to-spring fixture requires one direct stoppable "
        "native arm");
  }
  auto structural = make_lite6_structural_retention_fixture();
  auto catalog =
      fixture_detail::qualify_control_catalog(chained, native_motion,
                                              stoppable,
                                              containment_granularity,
                                              qualified_soft_to_spring_substitution);
  if (aggregate_gripper) {
    catalog = fixture_detail::add_aggregate_gripper_catalog(std::move(catalog));
  }
  const auto graph =
      fixture_detail::make_graph(structural, catalog, instance_count, cell_id,
                                 containment_granularity);
  const std::optional<fixture_detail::AssistantOverlayFixture>
      assistant_overlay =
          assistant_enabled
              ? std::optional<fixture_detail::AssistantOverlayFixture>(
                    fixture_detail::make_assistant_overlay())
              : std::nullopt;
  const auto inputs = fixture_detail::make_applicability_inputs(
      graph, catalog.provider,
      catalog.planner_capabilities,
      catalog.planner_provider.has_value() ? &*catalog.planner_provider
                                           : nullptr,
      catalog.native_runtime_policy.has_value()
          ? &*catalog.native_runtime_policy
          : nullptr,
      assistant_overlay.has_value() ? &*assistant_overlay : nullptr,
      composer_draft_enabled, stoppable,
      qualified_soft_to_spring_substitution);

  std::vector<core::CatalogRecordRefDTO> catalog_records{
      structural.catalog_reference,       catalog.capability.reference(),
      catalog.component_type.reference(), catalog.contract.reference(),
      catalog.topology.reference(),       catalog.provider.reference(),
      catalog.stop.reference(),
  };
  if (catalog.gripper_capability.has_value()) {
    catalog_records.push_back(catalog.gripper_capability->reference());
    catalog_records.push_back(catalog.gripper_command_schema->reference());
    catalog_records.push_back(catalog.gripper_contract->reference());
    catalog_records.push_back(catalog.gripper_topology->reference());
    catalog_records.push_back(catalog.gripper_provider->reference());
  }
  for (const auto& planner_capability : catalog.planner_capabilities) {
    catalog_records.push_back(planner_capability.reference());
  }
  if (!catalog.planner_capabilities.empty()) {
    catalog_records.push_back(catalog.planner_provider->reference());
  }
  if (catalog.native_execution_runtime_profile.has_value()) {
    catalog_records.push_back(
        catalog.native_execution_runtime_profile->reference());
  }
  if (catalog.native_runtime_policy.has_value()) {
    catalog_records.push_back(catalog.native_runtime_policy->reference());
  }
  CanonicalValue::Object deployment_graph = graph.normalized_root().as_object();
  deployment_graph.erase(
      std::remove_if(deployment_graph.begin(), deployment_graph.end(),
                     [](const auto& member) {
                       return member.first == "api_version" ||
                              member.first == "graph_revision" ||
                              member.first == "cell_id";
                     }),
      deployment_graph.end());
  CanonicalValue::Object deployment_members{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"deployment_artifact_id",
       CanonicalValue(native_motion ? "i6_lite6_native_motion_fixture"
                      : aggregate_gripper
                          ? "i6_lite6_arm_gripper_aggregate_fixture"
                      : instance_count == 1U
                          ? (chained ? "i3_lite6_chained_controllable_fixture"
                                     : "i3_lite6_controllable_fixture")
                          : "i6_two_lite6_controllable_fixture")},
      {"deployment_revision",
       CanonicalValue(std::string(fixture_detail::kZeroDigest))},
      {"cell_id", CanonicalValue(graph.graph().cell_id.value())},
      {"name",
       CanonicalValue(native_motion ? "I6 Lite6 addressed native motion fixture"
                      : aggregate_gripper
                          ? "I6 Lite6 arm and gripper aggregate fixture"
                      : instance_count == 1U
                          ? (chained ? "I3 Lite6 chained controllable fixture"
                                     : "I3 Lite6 controllable fixture")
                          : "I6 two same-type Lite6 controllable fixture")},
      {"catalogs", detail::encode_deployment_catalogs(catalog_records)},
      {"graph", CanonicalValue(std::move(deployment_graph))},
      {"desired_control_configurations",
       inputs.normalized_desired_control_configurations()},
      {"capability_provider_bindings",
       inputs.normalized_capability_provider_bindings()},
  };
  if (inputs.normalized_policies().has_value()) {
    deployment_members.emplace_back("policies",
                                    *inputs.normalized_policies());
  }
  if (assistant_enabled) {
    deployment_members.emplace_back(
        "assistant", inputs.assistant_settings().normalized_source());
    deployment_members.emplace_back("external_resources",
                                    inputs.normalized_external_resources());
    deployment_members.emplace_back("site_overlay_ref",
                                    *inputs.normalized_site_overlay_ref());
  }
  const CanonicalValue deployment_value(std::move(deployment_members));
  std::vector<QualifiedRuntimeProfile> runtime_profiles;
  if (catalog.native_execution_runtime_profile.has_value()) {
    runtime_profiles.push_back(*catalog.native_execution_runtime_profile);
  }
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical_json(deployment_value)),
      runtime_profiles);

  const std::array<SourceVerifiedModelAsset, 1U> assets{
      structural.verified_asset};
  const std::array<QualifiedComponentType, 1U> component_types{
      catalog.component_type};
  const auto composition =
      compose_cell_occurrences(deployment.graph(), assets, component_types);
  const std::optional<VerifiedSiteOverlayArtifactProjection>
      verified_assistant_overlay =
          assistant_overlay.has_value()
              ? std::optional<VerifiedSiteOverlayArtifactProjection>(
                    resolve_and_verify_site_overlay_artifact_projection(
                        assistant_overlay->reference, assistant_overlay->bytes))
              : std::nullopt;
  const auto overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources,
      verified_assistant_overlay.has_value() ? &*verified_assistant_overlay
                                             : nullptr);
  std::vector<QualifiedCommandSchema> command_schemas;
  std::vector<QualifiedCapabilityDefinition> capabilities{catalog.capability};
  std::vector<QualifiedControlInterfaceContract> contracts{catalog.contract};
  std::vector<QualifiedControllerTopology> topologies{catalog.topology};
  std::vector<QualifiedProviderManifest> providers{catalog.provider};
  if (catalog.gripper_capability.has_value()) {
    command_schemas.push_back(*catalog.gripper_command_schema);
    capabilities.push_back(*catalog.gripper_capability);
    contracts.push_back(*catalog.gripper_contract);
    topologies.push_back(*catalog.gripper_topology);
    providers.push_back(*catalog.gripper_provider);
  }
  capabilities.insert(capabilities.end(), catalog.planner_capabilities.begin(),
                      catalog.planner_capabilities.end());
  if (!catalog.planner_capabilities.empty()) {
    providers.push_back(*catalog.planner_provider);
  }
  const std::array<QualifiedStopEvidencePolicy, 1U> stop_policies{catalog.stop};
  std::vector<QualifiedRuntimePolicy> runtime_policies;
  if (catalog.native_runtime_policy.has_value()) {
    runtime_policies.push_back(*catalog.native_runtime_policy);
  }
  std::map<std::pair<std::string, std::string>,
           BuildQualifiedValueSchemaEntry>
      selected_value_schema_map;
  const auto& applicability_inputs = deployment.applicability_inputs().value();
  const auto& schema_registry = build_qualified_closed_schema_registry();
  for (const auto& row : applicability_inputs.stop_stage_parameters) {
    const auto& contract = row.provider_contract_ref;
    const auto* schema = schema_registry.find_qualified_value_schema(
        contract.contract_schema_id, contract.contract_schema_revision);
    if (schema == nullptr) {
      throw std::logic_error(
          "Lite6 control fixture has no exact build-qualified Stop value schema");
    }
    selected_value_schema_map.emplace(
        std::pair{contract.contract_schema_id.value(),
                  contract.contract_schema_revision.value()},
        *schema);
  }
  std::vector<BuildQualifiedValueSchemaEntry> selected_value_schemas;
  selected_value_schemas.reserve(selected_value_schema_map.size());
  for (const auto& [_, schema] : selected_value_schema_map) {
    selected_value_schemas.push_back(schema);
  }
  if (!applicability_inputs.commissioned_stop_interfaces.empty()) {
    throw std::logic_error(
        "Lite6 control fixture does not declare a commissioned safety Stop stage");
  }
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), runtime_profiles,
      runtime_policies, command_schemas, capabilities,
      contracts, topologies, providers,
      stop_policies, selected_value_schemas,
      std::span<const QualifiedCommissioningEvidenceRef>{}, overlay);
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
      std::move(catalog.gripper_capability),
      std::move(catalog.gripper_command_schema),
      std::move(catalog.gripper_contract),
      std::move(catalog.gripper_topology),
      std::move(catalog.gripper_provider),
      std::move(catalog.planner_capabilities),
      std::move(catalog.planner_provider),
      std::move(catalog.native_execution_runtime_profile),
      std::move(catalog.native_runtime_policy),
  };
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_chained_controllable_retention_fixture() {
  return make_lite6_controllable_retention_fixture(true);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_stoppable_controllable_retention_fixture(
    std::string_view containment_granularity = "resource") {
  return make_lite6_controllable_retention_fixture(false, false, 1U, false,
                                                   false, {}, false, true,
                                                   containment_granularity);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_two_lite6_same_type_controllable_retention_fixture() {
  return make_lite6_controllable_retention_fixture(false, false, 2U);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_assistant_retention_fixture() {
  return make_lite6_controllable_retention_fixture(false, true);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_composer_assistant_retention_fixture() {
  return make_lite6_controllable_retention_fixture(false, true, 2U, false, false, {}, true);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_arm_gripper_aggregate_retention_fixture() {
  return make_lite6_controllable_retention_fixture(false, false, 1U, true);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_native_motion_retention_fixture() {
  return make_lite6_controllable_retention_fixture(false, false, 1U, false,
                                                   true);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_stop_qualified_native_motion_retention_fixture() {
  return make_lite6_controllable_retention_fixture(
      false, false, 1U, false, true, {}, false, true, "resource", true);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_native_motion_retention_fixture_for_cell(
    std::string_view cell_id) {
  if (cell_id.empty()) {
    throw std::invalid_argument("native-motion fixture cell ID is empty");
  }
  return make_lite6_controllable_retention_fixture(false, false, 1U, false,
                                                   true, cell_id);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_two_lite6_same_type_native_motion_retention_fixture() {
  return make_lite6_controllable_retention_fixture(false, false, 2U, false,
                                                   true);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_two_lite6_same_type_native_motion_retention_fixture_for_cell(
    std::string_view cell_id) {
  if (cell_id.empty()) {
    throw std::invalid_argument(
        "two-instance native-motion fixture cell ID is empty");
  }
  return make_lite6_controllable_retention_fixture(false, false, 2U, false,
                                                   true, cell_id);
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite6_native_motion_arm_gripper_aggregate_retention_fixture() {
  return make_lite6_controllable_retention_fixture(false, false, 1U, true,
                                                   true);
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
    if (found == inputs_.end() || found->second.digest != expected_digest ||
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
    const auto [position, inserted] =
        inputs_.emplace(key.value(), Entry{digest, std::move(bytes)});
    if (!inserted && position->second.digest != digest) {
      throw std::logic_error(
          "Lite6 controllable input source contains a key collision");
    }
  }

  std::map<std::string, Entry, std::less<>> inputs_;
};

}  // namespace manyforge::deployment::test
