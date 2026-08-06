#pragma once

#include <cstdint>

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
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "reference_runtime_unit_internal.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::deployment::test {

inline constexpr std::string_view kRuntimeOnlyZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
inline constexpr std::string_view kRuntimeOnlyProfileSchemaRevision =
    "sha256:c254afa8c2bfcc803f12a7af63b1f09a8ee804b1ffa2a65b7361ad9309b87ae7";

struct RuntimeOnlyRetentionFixture final {
  QualifiedRuntimeProfile runtime_profile;
  RevisionVerifiedDeploymentArtifact deployment;
  CompilerQualifiedCellIr compiler;
  RetentionQualifiedDeployment qualified;
};

[[nodiscard]] inline std::string runtime_only_profile_document() {
  return detail::reference_runtime_profile_document();
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
          core::RuntimeProfileId(
              std::string(detail::kReferenceRuntimeProfileId)),
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
    std::string_view runtime_unit_id = detail::kReferenceRuntimeUnitId) {
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

/// Exact build-owned source bytes for the one qualified runtime profile.
class RuntimeOnlyCompilerInputArtifactSource final
    : public detail::ICompilerInputArtifactSource {
 public:
  explicit RuntimeOnlyCompilerInputArtifactSource(
      const QualifiedRuntimeProfile& profile)
      : key_(profile.reference().content_addressed_store_key),
        digest_(profile.reference().content_digest),
        bytes_(runtime_only_profile_document()) {}

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey& key,
      const core::ContentDigest& expected_digest,
      std::optional<std::uint64_t> expected_size) const override {
    if (key != key_ || expected_digest != digest_ ||
        (expected_size.has_value() && *expected_size != bytes_.size()) ||
        sha256_digest(bytes_) != digest_.value()) {
      return std::nullopt;
    }
    return bytes_;
  }

 private:
  core::ContentAddressedStoreKey key_;
  core::ContentDigest digest_;
  std::string bytes_;
};

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

[[nodiscard]] inline std::string runtime_only_provider_binding(
    std::string_view cell_id = "runtime_only_cell") {
  return "{\"capability_provider_binding_id\":\"runtime_only_provider\","
         "\"capability_provider_binding_revision\":\"" +
         std::string(kRuntimeOnlyZeroDigest) +
         "\",\"capability_id\":\"planning.motion\","
         "\"capability_definition_revision\":\"" +
         std::string(kRuntimeOnlyZeroDigest) +
         "\",\"scope\":\"cell_global\","
         "\"target\":{\"cell_id\":\"" +
         std::string(cell_id) +
         "\"},"
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
    std::string capability_provider_bindings = "[]",
    std::string_view cell_id = "runtime_only_cell") {
  if (graph.empty()) {
    graph = runtime_only_graph(profile);
  }
  if (catalogs.empty()) {
    catalogs = runtime_only_catalogs(profile);
  }
  return "{\"api_version\":\"manyforge.deployment.v1\","
         "\"deployment_artifact_id\":\"runtime_only_deployment\","
         "\"deployment_revision\":\"" +
         std::string(kRuntimeOnlyZeroDigest) + "\",\"cell_id\":\"" + std::string(cell_id) +
         "\","
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
    std::string capability_provider_bindings = "[]",
    std::string_view cell_id = "runtime_only_cell") {
  return compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(runtime_only_deployment_document(
          profile, std::move(graph), std::move(catalogs),
          std::move(capability_provider_bindings), cell_id)),
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
      {}, {}, selected_overlay);
  return compile_cell_ir(applicability);
}

[[nodiscard]] inline RuntimeOnlyRetentionFixture
make_runtime_only_retention_fixture(
    std::string_view cell_id = "runtime_only_cell") {
  auto profile = qualify_runtime_only_profile();
  auto deployment = produce_runtime_only_deployment(profile, {}, {}, "[]", cell_id);
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
