#pragma once

#include "lite6_controllable_retention_fixture.hpp"
#include "content_identity_internal.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment::test {

/// One separate tool model, qualified from its maintained model-asset row and
/// its build-prepared structural root (model.urdf/model.srdf + receipt).
/// Mesh artifacts come from the model's resource root.
[[nodiscard]] inline Lite6StructuralRetentionFixture
make_prepared_gripper_structural_retention_fixture(
    std::string_view model_asset_id, const std::filesystem::path &catalog_path,
    const std::filesystem::path &prepared_root,
    const std::filesystem::path &source_urdf,
    const std::filesystem::path &source_srdf,
    const std::filesystem::path &resource_root) {
  const auto* schema = build_qualified_closed_schema_registry()
                           .find_catalog_schema_family(
                               core::CatalogRecordKind::kModelAsset);
  if (schema == nullptr ||
      schema->qualification() != CatalogSchemaQualification::kQualified ||
      schema->compiled_schema() == nullptr) {
    throw std::logic_error(
        "gripper-only fixture requires the build-qualified model-asset schema");
  }
  const auto maintained = validate_and_normalize_unqualified_artifact_candidate(
      *schema->compiled_schema(),
      parse_strict_json_model_yaml(read_lite6_fixture_file(catalog_path)));
  const auto& maintained_root = maintained.normalized_root();
  if (std::get<std::string>(
          maintained_root.find_object_member("model_asset_id")->storage()) !=
      model_asset_id) {
    throw std::logic_error("gripper-only fixture catalog names another model asset");
  }
  const auto* expanded = maintained_root.find_object_member("expanded_source");
  const auto* declared = expanded->find_object_member("artifacts");
  std::vector<Lite6InputArtifact> artifacts;
  CanonicalValue::Array artifact_rows;
  const std::string prefix = "asset://" + std::string(model_asset_id) + "/";
  for (const auto& row : declared->as_array()) {
    const auto& uri =
        std::get<std::string>(row.find_object_member("logical_uri")->storage());
    const auto& media_type =
        std::get<std::string>(row.find_object_member("media_type")->storage());
    if (!uri.starts_with(prefix)) {
      throw std::logic_error("gripper-only fixture artifact has a foreign model identity");
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

  // Build preparation changes bytes, never the asset's role identities.
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
      "catalog/model-assets/" + std::string(model_asset_id) + "/" +
      revision.substr(std::string_view("sha256:").size()));
  const core::ModelAssetCatalogRecordRefDTO reference{
      core::ModelAssetId(std::string(model_asset_id)), core::ModelRevision(revision),
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
      read_lite6_fixture_file(source_urdf), read_lite6_fixture_file(source_srdf),
      verified);
  return {reference, catalog_bytes, std::move(verified), std::move(artifacts),
          std::move(preparation)};
}

/// The maintained Lite gripper (models/lite_gripper), prepared beside the
/// Lite6 structural root: `lite_gripper` in place of `lite6_lite_gripper` in
/// the prepared root's name; its meshes live in the Lite6 resource root.
[[nodiscard]] inline Lite6StructuralRetentionFixture
make_lite_gripper_structural_retention_fixture() {
  const std::filesystem::path lite6_prepared(MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT);
  const std::filesystem::path resource_root(MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT);
  constexpr std::string_view kLite6Asset = "lite6_lite_gripper";
  const auto prepared_name = lite6_prepared.filename().string();
  if (!prepared_name.starts_with(kLite6Asset))
    throw std::logic_error("Lite6 structural root is not named after its asset: " +
                           lite6_prepared.string());
  const auto prepared_root = lite6_prepared.parent_path() /
      ("lite_gripper" + prepared_name.substr(kLite6Asset.size()));
  const auto models = resource_root.parent_path();
  const auto workspace = models.parent_path().parent_path();
  return make_prepared_gripper_structural_retention_fixture(
      "lite_gripper",
      workspace / "manyforge_deployment/resources/maintained/model_assets/"
                  "lite_gripper.model_asset.json",
      prepared_root, models / "lite_gripper/lite_gripper.urdf",
      models / "lite_gripper/lite_gripper.srdf", resource_root);
}

inline constexpr std::string_view kLiteGripperComponentId = "lite_gripper";

/// Map the gripper-only component input to the Lite gripper asset's roles.
inline void map_lite_gripper_only_component(CanonicalValue::Object &members) {
  for (auto &[name, value] : members) {
    if (name == "component_type_id")
      value = CanonicalValue("lite_gripper_component");
    else if (name == "component_root")
      value = CanonicalValue(CanonicalValue::Object{
          {"model_role_id", CanonicalValue("gripper_model")},
          {"frame_role_id", CanonicalValue("mount")},
      });
  }
  const auto map = [](auto &self, CanonicalValue &value) -> void {
    if (value.is_string() && value.as_string() == "arm_model") {
      value = CanonicalValue("gripper_model");
    } else if (value.is_array()) {
      auto rows = value.as_array();
      for (auto &row : rows) self(self, row);
      value = CanonicalValue(std::move(rows));
    } else if (value.is_object()) {
      auto fields = value.as_object();
      for (auto &[key, item] : fields) {
        if (key == "asset_resource_role_id" && item.as_string() == "gripper_joint")
          item = CanonicalValue("fingers");
        else self(self, item);
      }
      value = CanonicalValue(std::move(fields));
    }
  };
  for (auto &[_, value] : members) map(map, value);
}

/// Map the gripper-only graph input to the Lite gripper component: its own id,
/// model role, model family with its `lanes.mock` row (models/lite_gripper),
/// and its mount frame. Actuation, output and dwell stay as authored.
inline void map_lite_gripper_only_graph(CanonicalValue::Object &members) {
  const std::map<std::string, std::string> identities{
      {std::string(kLite6ControllableComponentId), std::string(kLiteGripperComponentId)},
      {"arm_model", "gripper_model"},
      {std::string(kLite6ControllableModelInstanceId), "lite_gripper_instance"},
  };
  const auto map = [&](auto &self, CanonicalValue &value) -> void {
    if (value.is_string()) {
      const auto found = identities.find(value.as_string());
      if (found != identities.end()) value = CanonicalValue(found->second);
    } else if (value.is_array()) {
      auto rows = value.as_array();
      for (auto &row : rows) self(self, row);
      value = CanonicalValue(std::move(rows));
    } else if (value.is_object()) {
      auto fields = value.as_object();
      for (auto &[key, item] : fields) {
        if (key == "model_family_id")
          item = CanonicalValue("lite_gripper");
        else if (key == "lane_row")
          item = CanonicalValue(
              R"({"adapter":"mock","controller":"lite_gripper_controller",)"
              R"("controllers_file":"mock_controllers.yaml",)"
              R"("gripper_stop_finish_tolerance":0.0004,)"
              R"("hard_stop_completion_timeout_s":1.5})");
        else if (key == "mount_frame")
          item = CanonicalValue("lite_gripper_mount");
        else self(self, item);
      }
      value = CanonicalValue(std::move(fields));
    }
  };
  for (auto &[_, value] : members) map(map, value);
}

/// The Lite gripper as its own Mock ROS component: no arm binding,
/// participant, seed or TCP limits, with its own gripper Stop policy.
[[nodiscard]] inline Lite6ControllableRetentionFixture
make_lite_gripper_only_retention_fixture(
    std::string_view lifecycle_semantic_digest = {},
    std::string_view actuation_kind = "pneumatic",
    std::optional<double> dwell_s = std::nullopt,
    std::string_view output_mode = "commanded", bool gripper_feedback = false,
    void (*graph_input_mapper)(CanonicalValue::Object &) = map_lite_gripper_only_graph) {
  const auto structural = make_lite_gripper_structural_retention_fixture();
  return make_lite6_controllable_retention_fixture(
      false, false, 1U, {}, false, true, "resource", true,
      lifecycle_semantic_digest, lifecycle_semantic_digest, gripper_feedback,
      true, std::nullopt, kLite6ArmTargetId, std::nullopt, std::nullopt, {},
      true, {}, "manyforge.provider.ros2_control", true, true, &structural,
      graph_input_mapper, false, actuation_kind, dwell_s, output_mode,
      map_lite_gripper_only_component);
}

}  // namespace manyforge::deployment::test
