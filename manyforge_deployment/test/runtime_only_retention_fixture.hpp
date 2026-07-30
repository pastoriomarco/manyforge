#pragma once

#include <manyforge_core/types/runtime_profile.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace manyforge::deployment::test {

inline constexpr std::string_view kRuntimeOnlyZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
inline constexpr std::string_view kRuntimeOnlyProfileSchemaRevision =
    "sha256:c254afa8c2bfcc803f12a7af63b1f09a8ee804b1ffa2a65b7361ad9309b87ae7";
inline constexpr std::string_view kRuntimeOnlyConfigurationSchemaRevision =
    "sha256:03457d535d31818fedaa7e44f48432a08feb705e4d22c9a8f5e50977c8dbf785";

struct RuntimeOnlyRetentionFixture final {
  QualifiedRuntimeProfile runtime_profile;
  RevisionVerifiedDeploymentArtifact deployment;
  CompilerQualifiedCellIr compiler;
  RetentionQualifiedDeployment qualified;
};

[[nodiscard]] inline std::string runtime_only_profile_document() {
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kRuntimeProfileApiVersion))},
      {"runtime_profile_id", CanonicalValue("runtime_only_profile")},
      {"configuration_schema_ref",
       CanonicalValue(CanonicalValue::Object{
           {"schema_id",
            CanonicalValue("manyforge.value_schema.runtime.empty.v1")},
           {"schema_revision",
            CanonicalValue(
                std::string(kRuntimeOnlyConfigurationSchemaRevision))},
       })},
      {"process_contract",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
       })},
      {"supervision_contract",
       CanonicalValue(CanonicalValue::Object{
           {"status_endpoint_role_id", CanonicalValue("runtime_status")},
       })},
      {"offered_runtime_role_ids",
       CanonicalValue(CanonicalValue::Array{CanonicalValue("planner")})},
      {"source_endpoint_role_ids",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue("plan_runtime"),
           CanonicalValue("runtime_status"),
       })},
  });

  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/runtime_profile");
  preimage.push_back('\0');
  preimage.append(core::kRuntimeProfileApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("runtime_profile_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] inline std::string runtime_only_revision_from_document(
    std::string_view document) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const CanonicalValue* revision =
      parsed.root().find_object_member("runtime_profile_revision");
  if (revision == nullptr) {
    throw std::logic_error(
        "runtime-only fixture profile has no revision member");
  }
  const auto* text = std::get_if<std::string>(&revision->storage());
  if (text == nullptr) {
    throw std::logic_error(
        "runtime-only fixture profile revision is not a string");
  }
  return *text;
}

[[nodiscard]] inline QualifiedRuntimeProfile
qualify_runtime_only_profile() {
  const std::string document = runtime_only_profile_document();
  const core::ContentAddressedStoreKey store_key(
      "sha256/runtime-only-profile");
  return resolve_and_qualify_runtime_profile(
      {
          core::RuntimeProfileId("runtime_only_profile"),
          core::RuntimeProfileRevision(
              runtime_only_revision_from_document(document)),
          core::SchemaId(std::string(core::kRuntimeProfileApiVersion)),
          core::SchemaRevision(
              std::string(kRuntimeOnlyProfileSchemaRevision)),
          core::ContentDigest(sha256_digest(document)),
          store_key,
      },
      store_key, document);
}

[[nodiscard]] inline std::string runtime_only_graph(
    const QualifiedRuntimeProfile& profile,
    std::string_view runtime_unit_id = "runtime_only") {
  return "{\"runtime_units\":[{\"runtime_unit_id\":\"" +
         std::string(runtime_unit_id) +
         "\",\"runtime_profile_id\":\"" +
         profile.value().runtime_profile_id.value() +
         "\",\"runtime_profile_revision\":\"" +
         profile.value().runtime_profile_revision.value() +
         "\",\"dependency_runtime_unit_ids\":[],\"configuration\":{}}],"
         "\"model_instances\":[],\"controllable_components\":[],"
         "\"control_domains\":[],\"control_bindings\":[],"
         "\"component_attachments\":[]}";
}

[[nodiscard]] inline std::string runtime_only_catalogs(
    const QualifiedRuntimeProfile& profile,
    std::string_view store_key_override = {}) {
  const auto& reference = profile.reference();
  const std::string store_key =
      store_key_override.empty()
          ? reference.content_addressed_store_key.value()
          : std::string(store_key_override);
  return "{\"records\":[{\"record_kind\":\"runtime_profile\","
         "\"record_id\":\"" +
         reference.record_id.value() +
         "\",\"record_revision\":\"" +
         reference.record_revision.value() +
         "\",\"schema_id\":\"" + reference.schema_id.value() +
         "\",\"schema_revision\":\"" +
         reference.schema_revision.value() +
         "\",\"content_digest\":\"" + reference.content_digest.value() +
         "\",\"content_addressed_store_key\":\"" + store_key + "\"}]}";
}

[[nodiscard]] inline std::string runtime_only_provider_binding() {
  return "{\"capability_provider_binding_id\":\"runtime_only_provider\","
         "\"capability_provider_binding_revision\":\"" +
         std::string(kRuntimeOnlyZeroDigest) +
         "\",\"capability_id\":\"planning.motion\","
         "\"capability_definition_revision\":\"" +
         std::string(kRuntimeOnlyZeroDigest) +
         "\",\"scope\":\"cell_global\","
         "\"target\":{\"cell_id\":\"runtime_only_cell\"},"
         "\"provider_instance_id\":\"runtime_only_provider\","
         "\"provider_manifest_id\":\"runtime_only_manifest\","
         "\"provider_manifest_revision\":\"" +
         std::string(kRuntimeOnlyZeroDigest) +
         "\",\"endpoint_bindings\":[]}";
}

[[nodiscard]] inline std::string runtime_only_deployment_document(
    const QualifiedRuntimeProfile& profile,
    std::string graph = {},
    std::string catalogs = {},
    std::string capability_provider_bindings = "[]") {
  if (graph.empty()) {
    graph = runtime_only_graph(profile);
  }
  if (catalogs.empty()) {
    catalogs = runtime_only_catalogs(profile);
  }
  return "{\"api_version\":\"manyforge.deployment.v1\","
         "\"deployment_artifact_id\":\"runtime_only_deployment\","
         "\"deployment_revision\":\"" +
         std::string(kRuntimeOnlyZeroDigest) +
         "\",\"cell_id\":\"runtime_only_cell\","
         "\"name\":\"Runtime-only retention fixture\","
         "\"catalogs\":" + std::move(catalogs) +
         ",\"graph\":" + std::move(graph) +
         ",\"desired_control_configurations\":[],"
         "\"capability_provider_bindings\":" +
         std::move(capability_provider_bindings) + "}";
}

[[nodiscard]] inline RevisionVerifiedDeploymentArtifact
produce_runtime_only_deployment(
    const QualifiedRuntimeProfile& profile,
    std::string graph = {},
    std::string catalogs = {},
    std::string capability_provider_bindings = "[]") {
  return compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(runtime_only_deployment_document(
          profile, std::move(graph), std::move(catalogs),
          std::move(capability_provider_bindings))),
      std::span<const QualifiedRuntimeProfile>(&profile, 1U));
}

[[nodiscard]] inline CompilerQualifiedCellIr compile_runtime_only_deployment(
    const RevisionVerifiedDeploymentArtifact& deployment,
    const QualifiedRuntimeProfile& profile) {
  const auto composition = compose_cell_occurrences(
      deployment.graph(), std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto selected_overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources, nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(),
      std::span<const QualifiedRuntimeProfile>(&profile, 1U), {}, {}, {}, {},
      {}, selected_overlay);
  return compile_cell_ir(applicability);
}

[[nodiscard]] inline RuntimeOnlyRetentionFixture
make_runtime_only_retention_fixture() {
  auto profile = qualify_runtime_only_profile();
  auto deployment = produce_runtime_only_deployment(profile);
  auto compiler = compile_runtime_only_deployment(deployment, profile);
  auto qualified =
      qualify_deployment_for_retention(deployment, compiler);
  return {
      std::move(profile),
      std::move(deployment),
      std::move(compiler),
      std::move(qualified),
  };
}

}  // namespace manyforge::deployment::test
