#pragma once

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_deployment/structural_model_preparation.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "deployment_catalog_codec_internal.hpp"
#include "retained_bundle_internal.hpp"

#ifndef MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT
#error "MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT is required"
#endif

#ifndef MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT
#error "MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT is required"
#endif

namespace manyforge::deployment::test {

inline constexpr std::string_view kLite6ModelAssetId =
    "lite6_lite_gripper";
inline constexpr std::string_view kLite6UrdfLogicalUri =
    "asset://lite6_lite_gripper/model.urdf";
inline constexpr std::string_view kLite6SrdfLogicalUri =
    "asset://lite6_lite_gripper/model.srdf";

struct Lite6InputArtifact final {
  std::string logical_uri;
  std::string media_type;
  core::ContentAddressedStoreKey store_key;
  core::ContentDigest content_digest;
  std::string bytes;

  friend bool operator==(const Lite6InputArtifact&,
                         const Lite6InputArtifact&) = default;
};

struct Lite6StructuralRetentionFixture final {
  core::ModelAssetCatalogRecordRefDTO catalog_reference;
  std::string catalog_bytes;
  SourceVerifiedModelAsset verified_asset;
  std::vector<Lite6InputArtifact> source_artifacts;
  BuildQualifiedStructuralModelPreparation preparation;

  [[nodiscard]] std::span<
      const BuildQualifiedStructuralModelPreparation>
  structural_preparations() const noexcept {
    return {&preparation, 1U};
  }
};

[[nodiscard]] inline std::string read_lite6_fixture_file(
    const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error(
        "cannot read Lite6 structural fixture file: " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

[[nodiscard]] inline CanonicalValue lite6_expanded_source(
    std::span<const Lite6InputArtifact> artifacts) {
  CanonicalValue::Array rows;
  rows.reserve(artifacts.size());
  for (const auto& artifact : artifacts) {
    rows.emplace_back(CanonicalValue::Object{
        {"logical_uri", CanonicalValue(artifact.logical_uri)},
        {"media_type", CanonicalValue(artifact.media_type)},
        {"byte_size",
         CanonicalValue(static_cast<double>(artifact.bytes.size()))},
        {"content_digest",
         CanonicalValue(artifact.content_digest.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"urdf_logical_uri",
       CanonicalValue(std::string(kLite6UrdfLogicalUri))},
      {"srdf_logical_uri",
       CanonicalValue(std::string(kLite6SrdfLogicalUri))},
      {"artifacts", CanonicalValue(std::move(rows))},
  });
}

[[nodiscard]] inline CanonicalValue lite6_model_asset_identity(
    std::span<const Lite6InputArtifact> artifacts,
    std::string_view arm_collision_role = "arm_collision") {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kModelAssetApiVersion))},
      {"model_asset_id",
       CanonicalValue(std::string(kLite6ModelAssetId))},
      {"instance_cardinality", CanonicalValue("multiple_per_cell")},
      {"expanded_source", lite6_expanded_source(artifacts)},
      {"root_frame_role_id", CanonicalValue("root")},
      {"frame_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"frame_role_id", CanonicalValue("base")},
               {"source_link_name", CanonicalValue("link_base")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"frame_role_id", CanonicalValue("flange")},
               {"source_link_name", CanonicalValue("link_eef")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"frame_role_id", CanonicalValue("root")},
               {"source_link_name", CanonicalValue("world")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"frame_role_id", CanonicalValue("tcp")},
               {"source_link_name", CanonicalValue("link_tcp")},
           }),
       })},
      {"model_group_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"model_group_role_id", CanonicalValue("arm")},
               {"source_group_name", CanonicalValue("lite6")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"model_group_role_id", CanonicalValue("gripper")},
               {"source_group_name", CanonicalValue("lite_gripper")},
           }),
       })},
      {"resource_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"resource_role_id", CanonicalValue("arm_joints")},
               {"source_joint_names",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("joint1"), CanonicalValue("joint2"),
                    CanonicalValue("joint3"), CanonicalValue("joint4"),
                    CanonicalValue("joint5"), CanonicalValue("joint6")})},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"resource_role_id", CanonicalValue("gripper_joint")},
               {"source_joint_names",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("right_finger_joint")})},
           }),
       })},
      {"collision_body_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"collision_body_role_id",
                CanonicalValue(std::string(arm_collision_role))},
               {"source_link_names",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("link1"), CanonicalValue("link2"),
                    CanonicalValue("link3"), CanonicalValue("link4"),
                    CanonicalValue("link5"), CanonicalValue("link6"),
                    CanonicalValue("link_base")})},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"collision_body_role_id",
                CanonicalValue("gripper_collision")},
               {"source_link_names",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("lite_gripper_base_link"),
                    CanonicalValue("lite_gripper_left_finger"),
                    CanonicalValue("lite_gripper_right_finger")})},
           }),
       })},
  });
}

[[nodiscard]] inline std::string lite6_model_asset_document(
    std::span<const Lite6InputArtifact> artifacts,
    std::string_view arm_collision_role = "arm_collision") {
  const CanonicalValue identity =
      lite6_model_asset_identity(artifacts, arm_collision_role);
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/model_asset");
  preimage.push_back('\0');
  preimage.append(core::kModelAssetApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("model_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] inline std::vector<Lite6InputArtifact>
load_lite6_source_artifacts() {
  const std::filesystem::path prepared_root{
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT};
  const std::filesystem::path source_root{
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT};
  std::vector<Lite6InputArtifact> artifacts;
  const auto add = [&artifacts](
                       std::string logical_uri, std::string media_type,
                       std::string bytes) {
    const std::string digest = sha256_digest(bytes);
    artifacts.push_back(Lite6InputArtifact{
        std::move(logical_uri),
        std::move(media_type),
        core::ContentAddressedStoreKey(
            "sha256/" + digest.substr(std::string_view("sha256:").size())),
        core::ContentDigest(digest),
        std::move(bytes),
    });
  };
  add(std::string(kLite6UrdfLogicalUri), "application/xml",
      read_lite6_fixture_file(prepared_root / "model.urdf"));
  add(std::string(kLite6SrdfLogicalUri), "application/xml",
      read_lite6_fixture_file(prepared_root / "model.srdf"));

  std::vector<std::filesystem::path> mesh_paths;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(
           source_root / "meshes")) {
    if (entry.is_regular_file() &&
        entry.path().extension() == ".stl") {
      mesh_paths.push_back(entry.path());
    }
  }
  std::sort(mesh_paths.begin(), mesh_paths.end());
  for (const auto& mesh_path : mesh_paths) {
    const std::string relative =
        std::filesystem::relative(mesh_path, source_root).generic_string();
    add("asset://" + std::string(kLite6ModelAssetId) + "/" + relative,
        "model/stl", read_lite6_fixture_file(mesh_path));
  }
  std::sort(
      artifacts.begin(), artifacts.end(),
      [](const Lite6InputArtifact& left,
         const Lite6InputArtifact& right) {
        return left.logical_uri < right.logical_uri;
      });
  return artifacts;
}

[[nodiscard]] inline Lite6StructuralRetentionFixture
make_lite6_structural_retention_fixture(
    std::string_view arm_collision_role = "arm_collision") {
  auto artifacts = load_lite6_source_artifacts();
  const std::string catalog_bytes =
      lite6_model_asset_document(artifacts, arm_collision_role);
  const CanonicalValue catalog =
      parse_strict_json_model_yaml(catalog_bytes).root();
  const auto* revision_value =
      catalog.find_object_member("model_revision");
  if (revision_value == nullptr) {
    throw std::logic_error("Lite6 model fixture has no revision");
  }
  const auto* revision =
      std::get_if<std::string>(&revision_value->storage());
  if (revision == nullptr) {
    throw std::logic_error("Lite6 model fixture revision is not text");
  }
  const std::string catalog_digest = sha256_digest(catalog_bytes);
  const core::ContentAddressedStoreKey catalog_key(
      "catalog/model-assets/lite6_lite_gripper/" +
      revision->substr(std::string_view("sha256:").size()));
  const auto* model_asset_schema =
      build_qualified_closed_schema_registry()
          .find_catalog_schema_family(
              core::CatalogRecordKind::kModelAsset);
  if (model_asset_schema == nullptr ||
      model_asset_schema->qualification() !=
          CatalogSchemaQualification::kQualified ||
      model_asset_schema->compiled_schema() == nullptr) {
    throw std::logic_error(
        "Lite6 model fixture requires the build-qualified model-asset schema");
  }
  const core::ModelAssetCatalogRecordRefDTO reference{
      core::ModelAssetId(std::string(kLite6ModelAssetId)),
      core::ModelRevision(*revision),
      core::SchemaId(std::string(core::kModelAssetApiVersion)),
      model_asset_schema->compiled_schema()->schema_revision(),
      core::ContentDigest(catalog_digest),
      catalog_key,
  };
  const QualifiedModelAsset qualified =
      resolve_and_qualify_model_asset(
          reference, catalog_key, catalog_bytes);
  std::vector<ResolvedModelSourceArtifactCandidate> candidates;
  candidates.reserve(artifacts.size());
  for (const auto& artifact : artifacts) {
    candidates.push_back({
        artifact.logical_uri,
        artifact.store_key,
        artifact.bytes,
    });
  }
  SourceVerifiedModelAsset verified =
      verify_model_source_closure(qualified, std::move(candidates));
  const std::filesystem::path prepared_root{
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT};
  const std::filesystem::path source_root{
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT};
  const std::string receipt_bytes = read_lite6_fixture_file(
      prepared_root / "preparation-manifest.json");
  const std::string original_urdf = read_lite6_fixture_file(
      source_root / "lite6_lite_gripper.urdf");
  const std::string original_srdf = read_lite6_fixture_file(
      source_root / "lite6_lite_gripper.srdf");
  auto preparation =
      verify_build_qualified_structural_model_preparation(
          receipt_bytes, original_urdf, original_srdf, verified);
  return {
      reference,
      catalog_bytes,
      std::move(verified),
      std::move(artifacts),
      std::move(preparation),
  };
}

class Lite6CompilerInputArtifactSource final
    : public detail::ICompilerInputArtifactSource {
 public:
  explicit Lite6CompilerInputArtifactSource(
      const Lite6StructuralRetentionFixture& fixture) {
    add_fixture(fixture);
  }

  Lite6CompilerInputArtifactSource(
      std::initializer_list<const Lite6StructuralRetentionFixture*>
          fixtures) {
    for (const auto* fixture : fixtures) {
      if (fixture == nullptr) {
        throw std::logic_error(
            "Lite6 input source rejects a null fixture");
      }
      add_fixture(*fixture);
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
  void add_fixture(const Lite6StructuralRetentionFixture& fixture) {
    inputs_.emplace(
        fixture.catalog_reference.content_addressed_store_key.value(),
        Entry{
            fixture.catalog_reference.content_digest,
            fixture.catalog_bytes,
        });
    for (const auto& artifact : fixture.source_artifacts) {
      const auto [position, inserted] = inputs_.emplace(
          artifact.store_key.value(),
          Entry{artifact.content_digest, artifact.bytes});
      if (!inserted &&
          (position->second.digest != artifact.content_digest ||
           position->second.bytes != artifact.bytes)) {
        throw std::logic_error(
            "Lite6 fixture has a conflicting input-store key");
      }
    }
  }
  struct Entry final {
    core::ContentDigest digest;
    std::string bytes;
  };
  std::map<std::string, Entry, std::less<>> inputs_;
};

struct Lite6PassiveModelSelection final {
  std::string model_instance_id;
  const Lite6StructuralRetentionFixture* fixture;
};

[[nodiscard]] inline RetentionQualifiedDeployment
qualify_lite6_passive_deployment(
    std::span<const Lite6PassiveModelSelection> selections) {
  if (selections.empty()) {
    throw std::logic_error(
        "Lite6 passive deployment requires one or more selections");
  }

  using AssetIdentity = std::pair<std::string, std::string>;
  std::map<AssetIdentity, const Lite6StructuralRetentionFixture*>
      exact_assets;
  std::map<std::string, const Lite6StructuralRetentionFixture*,
           std::less<>>
      instances;
  for (const auto& selection : selections) {
    if (selection.model_instance_id.empty() ||
        selection.fixture == nullptr ||
        !instances
             .emplace(selection.model_instance_id, selection.fixture)
             .second) {
      throw std::logic_error(
          "Lite6 passive deployment requires unique non-empty instances");
    }
    const auto& reference = selection.fixture->catalog_reference;
    const AssetIdentity identity{
        reference.record_id.value(),
        reference.record_revision.value()};
    const auto [position, inserted] =
        exact_assets.emplace(identity, selection.fixture);
    if (!inserted &&
        (position->second->catalog_reference != reference ||
         position->second->catalog_bytes !=
             selection.fixture->catalog_bytes)) {
      throw std::logic_error(
          "Lite6 passive deployment has conflicting exact assets");
    }
  }

  std::vector<core::CatalogRecordRefDTO> catalog_records;
  std::vector<SourceVerifiedModelAsset> assets;
  catalog_records.reserve(exact_assets.size());
  assets.reserve(exact_assets.size());
  for (const auto& [unused, fixture] : exact_assets) {
    (void)unused;
    catalog_records.emplace_back(fixture->catalog_reference);
    assets.push_back(fixture->verified_asset);
  }
  const CanonicalValue catalogs =
      detail::encode_deployment_catalogs(catalog_records);

  CanonicalValue::Array model_instances;
  model_instances.reserve(instances.size());
  for (const auto& [model_instance_id, fixture] : instances) {
    model_instances.emplace_back(CanonicalValue::Object{
        {"model_instance_id", CanonicalValue(model_instance_id)},
        {"model_asset_id",
         CanonicalValue(
             fixture->catalog_reference.record_id.value())},
        {"model_revision",
         CanonicalValue(
             fixture->catalog_reference.record_revision.value())},
        {"root_pose_in_cell",
         CanonicalValue(CanonicalValue::Object{
             {"position_m",
              CanonicalValue(CanonicalValue::Array{
                  CanonicalValue(0.0), CanonicalValue(0.0),
                  CanonicalValue(0.0)})},
             {"orientation_xyzw",
              CanonicalValue(CanonicalValue::Array{
                  CanonicalValue(0.0), CanonicalValue(0.0),
                  CanonicalValue(0.0), CanonicalValue(1.0)})},
         })},
    });
  }
  const CanonicalValue graph(CanonicalValue::Object{
      {"runtime_units", CanonicalValue(CanonicalValue::Array{})},
      {"model_instances", CanonicalValue(std::move(model_instances))},
      {"controllable_components",
       CanonicalValue(CanonicalValue::Array{})},
      {"control_domains", CanonicalValue(CanonicalValue::Array{})},
      {"control_bindings", CanonicalValue(CanonicalValue::Array{})},
      {"component_attachments",
       CanonicalValue(CanonicalValue::Array{})},
  });
  const CanonicalValue deployment_value(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"deployment_artifact_id",
       CanonicalValue("i3_lite6_passive_fixture")},
      {"deployment_revision",
       CanonicalValue(
           "sha256:0000000000000000000000000000000000000000000000000000000000000000")},
      {"cell_id", CanonicalValue("i3_lite6_passive_cell")},
      {"name", CanonicalValue("I3 Lite6 passive fixture")},
      {"catalogs", catalogs},
      {"graph", graph},
      {"desired_control_configurations",
       CanonicalValue(CanonicalValue::Array{})},
      {"capability_provider_bindings",
       CanonicalValue(CanonicalValue::Array{})},
  });
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical_json(deployment_value)),
      std::span<const QualifiedRuntimeProfile>{});
  const auto composition = compose_cell_occurrences(
      deployment.graph(), assets,
      std::span<const QualifiedComponentType>{});
  const auto overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources,
      nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, {}, {}, {},
      {}, {}, {}, {}, {}, overlay);
  return qualify_deployment_for_retention(
      deployment, compile_cell_ir(applicability));
}

[[nodiscard]] inline RetentionQualifiedDeployment
qualify_lite6_passive_deployment(
    const Lite6StructuralRetentionFixture& fixture) {
  const std::array<Lite6PassiveModelSelection, 1U> selections{{
      {"lite6_fixture_instance", &fixture},
  }};
  return qualify_lite6_passive_deployment(selections);
}

}  // namespace manyforge::deployment::test
