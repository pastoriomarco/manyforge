#pragma once

#include "lite6_structural_retention_fixture.hpp"
#include "content_identity_internal.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifndef MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT
#error "MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT is required"
#endif
#ifndef MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_SOURCE_ROOT
#error "MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_SOURCE_ROOT is required"
#endif
#ifndef MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_RESOURCE_ROOT
#error "MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_RESOURCE_ROOT is required"
#endif
#ifndef MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_CATALOG_PATH
#error "MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_CATALOG_PATH is required"
#endif

namespace manyforge::deployment::test {

[[nodiscard]] inline Lite6StructuralRetentionFixture
make_ur5_arm_only_structural_retention_fixture() {
  const std::filesystem::path prepared_root{
      MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT};
  const std::filesystem::path source_root{
      MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_SOURCE_ROOT};
  const std::filesystem::path resource_root{
      MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_RESOURCE_ROOT};
  const auto* schema = build_qualified_closed_schema_registry()
                           .find_catalog_schema_family(
                               core::CatalogRecordKind::kModelAsset);
  if (schema == nullptr ||
      schema->qualification() != CatalogSchemaQualification::kQualified ||
      schema->compiled_schema() == nullptr) {
    throw std::logic_error(
        "UR5 fixture requires the build-qualified model-asset schema");
  }
  const auto maintained = validate_and_normalize_unqualified_artifact_candidate(
      *schema->compiled_schema(),
      parse_strict_json_model_yaml(read_lite6_fixture_file(
          MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_CATALOG_PATH)));
  const auto& maintained_root = maintained.normalized_root();
  if (std::get<std::string>(
          maintained_root.find_object_member("model_asset_id")->storage()) !=
      "ur5_roboplan") {
    throw std::logic_error("UR5 fixture requires the genuine ur5_roboplan asset");
  }
  const auto* expanded = maintained_root.find_object_member("expanded_source");
  const auto* declared = expanded->find_object_member("artifacts");
  std::vector<Lite6InputArtifact> artifacts;
  CanonicalValue::Array artifact_rows;
  constexpr std::string_view prefix = "asset://ur5_roboplan/";
  for (const auto& row : declared->as_array()) {
    const auto& uri =
        std::get<std::string>(row.find_object_member("logical_uri")->storage());
    const auto& media_type =
        std::get<std::string>(row.find_object_member("media_type")->storage());
    if (!uri.starts_with(prefix)) {
      throw std::logic_error("UR5 fixture artifact has a foreign model identity");
    }
    const std::string relative = uri.substr(prefix.size());
    const auto path = (relative == "model.urdf" || relative == "model.srdf")
                          ? prepared_root / relative
                          : resource_root / relative;
    std::string bytes = read_lite6_fixture_file(path);
    const std::string digest = sha256_digest(bytes);
    artifact_rows.emplace_back(CanonicalValue::Object{
        {"logical_uri", CanonicalValue(uri)},
        {"media_type", CanonicalValue(media_type)},
        {"byte_size", CanonicalValue(static_cast<double>(bytes.size()))},
        {"content_digest", CanonicalValue(digest)},
    });
    artifacts.push_back({
        uri, media_type,
        core::ContentAddressedStoreKey(
            "sha256/" + digest.substr(std::string_view("sha256:").size())),
        core::ContentDigest(digest), std::move(bytes),
    });
  }

  // Keep every maintained role, link, group and joint identity. Only the exact
  // prepared/source bytes and their derived revision vary with this build.
  auto expanded_members = expanded->as_object();
  for (auto& [name, value] : expanded_members) {
    if (name == "artifacts") {
      value = CanonicalValue(std::move(artifact_rows));
    }
  }
  auto root_members = maintained_root.as_object();
  for (auto& [name, value] : root_members) {
    if (name == "expanded_source") {
      value = CanonicalValue(std::move(expanded_members));
    }
  }
  const auto normalized = validate_and_normalize_unqualified_artifact_candidate(
      *schema->compiled_schema(),
      parse_strict_json_model_yaml(
          canonical_json(CanonicalValue(std::move(root_members)))));
  const std::string revision = detail::compute_content_identity(
      normalized.normalized_root(),
      {detail::catalog_record_identity_domain("model_asset"),
       std::string(core::kModelAssetApiVersion), "model_revision"});
  auto catalog_members = normalized.normalized_root().as_object();
  for (auto& [name, value] : catalog_members) {
    if (name == "model_revision") {
      value = CanonicalValue(revision);
    }
  }
  const std::string catalog_bytes =
      canonical_json(CanonicalValue(std::move(catalog_members)));
  const core::ContentAddressedStoreKey catalog_key(
      "catalog/model-assets/ur5_roboplan/" +
      revision.substr(std::string_view("sha256:").size()));
  const core::ModelAssetCatalogRecordRefDTO reference{
      core::ModelAssetId("ur5_roboplan"), core::ModelRevision(revision),
      core::SchemaId(std::string(core::kModelAssetApiVersion)),
      schema->compiled_schema()->schema_revision(),
      core::ContentDigest(sha256_digest(catalog_bytes)), catalog_key,
  };
  const auto qualified = resolve_and_qualify_model_asset(
      reference, catalog_key, catalog_bytes);
  std::vector<ResolvedModelSourceArtifactCandidate> candidates;
  candidates.reserve(artifacts.size());
  for (const auto& artifact : artifacts) {
    candidates.push_back(
        {artifact.logical_uri, artifact.store_key, artifact.bytes});
  }
  auto verified = verify_model_source_closure(qualified, std::move(candidates));
  auto preparation = verify_build_qualified_structural_model_preparation(
      read_lite6_fixture_file(prepared_root / "preparation-manifest.json"),
      read_lite6_fixture_file(source_root / "ur5_gripper.structural.urdf"),
      read_lite6_fixture_file(source_root / "ur5_gripper.structural.srdf"),
      verified);
  return {reference, catalog_bytes, std::move(verified), std::move(artifacts),
          std::move(preparation)};
}

}  // namespace manyforge::deployment::test
