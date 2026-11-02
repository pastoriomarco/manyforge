#include <algorithm>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/stop_control.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/deployment_authoring.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ros2_control_execution_runtime_unit_internal.hpp"

namespace manyforge::deployment {
namespace {

const std::string& RequireStringMember(const CanonicalValue& root, const std::string_view name) {
  const auto* value = root.find_object_member(name);
  if (value == nullptr) {
    throw DeploymentArtifactCodecError("source is missing " + std::string(name));
  }
  const auto* text = std::get_if<std::string>(&value->storage());
  if (text == nullptr) {
    throw DeploymentArtifactCodecError("source member is not text: " + std::string(name));
  }
  return *text;
}

QualifiedRuntimeProfile QualifiedRuntimeProfileForBuild(std::string bytes,
                                                        const std::string_view profile_id,
                                                        const std::string_view label) {
  const ParsedArtifactCandidate parsed = parse_strict_json_model_yaml(bytes);
  const std::string& revision = RequireStringMember(parsed.root(), "runtime_profile_revision");
  const auto* schema = build_qualified_closed_schema_registry().find_catalog_schema_family(
      core::CatalogRecordKind::kRuntimeProfile);
  if (schema == nullptr || schema->qualification() != CatalogSchemaQualification::kQualified ||
      schema->compiled_schema() == nullptr) {
    throw std::logic_error(std::string(label) + " runtime profile schema is not build-qualified");
  }
  const core::ContentDigest digest(sha256_digest(bytes));
  const core::ContentAddressedStoreKey store_key(
      "sha256/" + digest.value().substr(std::string_view("sha256:").size()));
  const core::RuntimeProfileCatalogRecordRefDTO reference{
      core::RuntimeProfileId(std::string(profile_id)),
      core::RuntimeProfileRevision(revision),
      core::SchemaId(std::string(core::kRuntimeProfileApiVersion)),
      schema->compiled_schema()->schema_revision(),
      digest,
      store_key};
  return resolve_and_qualify_runtime_profile(reference, store_key, bytes);
}

bool SelectsRuntimeProfile(const CanonicalValue& root, const std::string_view profile_id) {
  const auto* graph = root.find_object_member("graph");
  const auto* graph_object =
      graph == nullptr ? nullptr : std::get_if<CanonicalValue::Object>(&graph->storage());
  if (graph_object == nullptr) {
    return false;
  }
  const auto runtime_units =
      std::find_if(graph_object->begin(), graph_object->end(),
                   [](const auto& member) { return member.first == "runtime_units"; });
  if (runtime_units == graph_object->end()) {
    return false;
  }
  const auto* rows = std::get_if<CanonicalValue::Array>(&runtime_units->second.storage());
  if (rows == nullptr) {
    return false;
  }
  return std::any_of(rows->begin(), rows->end(), [&](const auto& row) {
    const auto* profile = row.find_object_member("runtime_profile_id");
    const auto* selected =
        profile == nullptr ? nullptr : std::get_if<std::string>(&profile->storage());
    return selected != nullptr && *selected == profile_id;
  });
}

std::vector<QualifiedRuntimeProfile> SelectedBuildProfiles(const CanonicalValue& root) {
  std::vector<QualifiedRuntimeProfile> profiles;
  if (SelectsRuntimeProfile(root, detail::kRos2ControlExecutionRuntimeProfileId)) {
    profiles.push_back(QualifiedRuntimeProfileForBuild(
        detail::ros2_control_execution_runtime_profile_document(),
        detail::kRos2ControlExecutionRuntimeProfileId, "ros2_control execution"));
  }
  return profiles;
}

CompiledDeploymentDraft Result(const RevisionVerifiedDeploymentArtifact& deployment) {
  return CompiledDeploymentDraft{
      .canonical_json = canonical_json(deployment.normalized_authored_root()),
      .deployment_artifact_id = deployment.deployment_artifact_id().value(),
      .deployment_revision = deployment.deployment_revision().value(),
      .graph_revision = deployment.graph().graph().graph_revision.value(),
  };
}

}  // namespace

CompiledDeploymentDraft compile_deployment_draft(const std::string_view source) {
  if (source.empty() || source.size() > kMaxArtifactEncodedBytes) {
    throw DeploymentArtifactCodecError(
        "deployment source is empty or exceeds the artifact byte bound");
  }
  const ParsedArtifactCandidate parsed = parse_strict_json_model_yaml(source);
  const auto profile_storage = SelectedBuildProfiles(parsed.root());

  const auto produced = compute_deployment_artifact_revision(
      parsed, std::span<const QualifiedRuntimeProfile>(profile_storage));
  const std::string canonical = canonical_json(produced.normalized_authored_root());
  const auto verified = verify_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical),
      std::span<const QualifiedRuntimeProfile>(profile_storage));
  if (verified.deployment_revision() != produced.deployment_revision() ||
      verified.normalized_authored_root() != produced.normalized_authored_root()) {
    throw std::logic_error("compiled deployment Draft did not independently verify");
  }
  return Result(produced);
}

CompiledDeploymentDraft verify_deployment_draft(const std::string_view source) {
  if (source.empty() || source.size() > kMaxArtifactEncodedBytes) {
    throw DeploymentArtifactCodecError(
        "deployment source is empty or exceeds the artifact byte bound");
  }
  const ParsedArtifactCandidate parsed = parse_strict_json_model_yaml(source);
  const auto profile_storage = SelectedBuildProfiles(parsed.root());
  return Result(verify_deployment_artifact_revision(
      parsed, std::span<const QualifiedRuntimeProfile>(profile_storage)));
}

}  // namespace manyforge::deployment
