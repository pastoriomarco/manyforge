#pragma once

#include "lite6_controllable_retention_fixture.hpp"
#include "content_identity_internal.hpp"

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifndef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT
#error "MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT is required"
#endif
#ifndef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_SOURCE_ROOT
#error "MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_SOURCE_ROOT is required"
#endif
#ifndef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_RESOURCE_ROOT
#error "MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_RESOURCE_ROOT is required"
#endif
#ifndef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_CATALOG_PATH
#error "MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_CATALOG_PATH is required"
#endif

namespace manyforge::deployment::test {

[[nodiscard]] inline Lite6StructuralRetentionFixture
make_robotiq_gripper_only_structural_retention_fixture() {
  const std::filesystem::path prepared_root{
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT};
  const std::filesystem::path source_root{
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_SOURCE_ROOT};
  const std::filesystem::path resource_root{
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_RESOURCE_ROOT};
  const auto* schema = build_qualified_closed_schema_registry()
                           .find_catalog_schema_family(
                               core::CatalogRecordKind::kModelAsset);
  if (schema == nullptr ||
      schema->qualification() != CatalogSchemaQualification::kQualified ||
      schema->compiled_schema() == nullptr) {
    throw std::logic_error(
        "Robotiq standalone fixture requires the build-qualified model-asset schema");
  }
  const auto maintained = validate_and_normalize_unqualified_artifact_candidate(
      *schema->compiled_schema(),
      parse_strict_json_model_yaml(read_lite6_fixture_file(
          MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_CATALOG_PATH)));
  const auto& maintained_root = maintained.normalized_root();
  if (std::get<std::string>(
          maintained_root.find_object_member("model_asset_id")->storage()) !=
      "robotiq_2f_85") {
    throw std::logic_error("Robotiq standalone fixture requires the genuine robotiq_2f_85 asset");
  }
  const auto* expanded = maintained_root.find_object_member("expanded_source");
  const auto* declared = expanded->find_object_member("artifacts");
  std::vector<Lite6InputArtifact> artifacts;
  CanonicalValue::Array artifact_rows;
  constexpr std::string_view prefix = "asset://robotiq_2f_85/";
  for (const auto& row : declared->as_array()) {
    const auto& uri =
        std::get<std::string>(row.find_object_member("logical_uri")->storage());
    const auto& media_type =
        std::get<std::string>(row.find_object_member("media_type")->storage());
    if (!uri.starts_with(prefix)) {
      throw std::logic_error("Robotiq standalone fixture artifact has a foreign model identity");
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

  // The independently authored standalone asset selects only the qualified
  // Robotiq subtree. Build preparation changes bytes, never its role identities.
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
      "catalog/model-assets/robotiq_2f_85/" +
      revision.substr(std::string_view("sha256:").size()));
  const core::ModelAssetCatalogRecordRefDTO reference{
      core::ModelAssetId("robotiq_2f_85"), core::ModelRevision(revision),
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
      read_lite6_fixture_file(source_root / "robotiq_2f_85.urdf"),
      read_lite6_fixture_file(source_root / "robotiq_2f_85.srdf"),
      verified);
  return {reference, catalog_bytes, std::move(verified), std::move(artifacts),
          std::move(preparation)};
}

inline constexpr std::string_view kRobotiqGripperOnlyComponentId = "robotiq_gripper";
inline constexpr std::string_view kRobotiqGripperOnlyResourceId = "robotiq_gripper_joint";

// Map authored component input before its revision and qualification. This
// remains fixture input, never a mutation of a retained or qualified product.
inline void map_robotiq_gripper_only_component(CanonicalValue::Object &members) {
  for (auto &[name, value] : members) {
    if (name == "component_type_id") {
      if (value.as_string() != "lite6_gripper_component")
        throw std::logic_error("Robotiq fixture requires the scalar component input");
      value = CanonicalValue("robotiq_position_gripper_component");
    } else if (name == "tags") {
      value = CanonicalValue(CanonicalValue::Array{CanonicalValue("articulated-gripper")});
    }
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
        else if (key == "asset_frame_role_id" && item.as_string() == "tcp")
          item = CanonicalValue("gripper_tcp");
        else self(self, item);
      }
      value = CanonicalValue(std::move(fields));
    }
  };
  for (auto &[_, value] : members) map(map, value);
}

inline void map_robotiq_gripper_only_graph(CanonicalValue::Object &members) {
  const std::map<std::string, std::string> identities{
      {"lite6_arm", std::string(kRobotiqGripperOnlyComponentId)},
      {"arm_model", "gripper_model"},
      {"lite6_fixture_instance", "robotiq_fixture_instance"},
      {"lite6_ros2_control_execution", "robotiq_ros2_control_execution"},
      {"lite6_gripper_binding", "robotiq_gripper_binding"},
      {"lite6_gripper_feedback", "robotiq_gripper_feedback"},
      {"lite6_gripper_domain", "robotiq_gripper_domain"},
      {"lite6_gripper_provider_instance", "robotiq_gripper_provider_instance"},
      {"lite6_gripper_commanded_state", "robotiq_gripper_commanded_state"},
      {"lite6_gripper_joint", std::string(kRobotiqGripperOnlyResourceId)},
      {"lite6_lite_gripper", "ur5e_robotiq_2f_85"},
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
      for (auto &[_, item] : fields) self(self, item);
      value = CanonicalValue(std::move(fields));
    }
  };
  for (auto &[_, value] : members) map(map, value);
  for (auto &[name, value] : members) {
    if (name != "runtime_units") continue;
    auto units = value.as_array();
    if (units.size() != 1U)
      throw std::logic_error("Robotiq fixture requires its sole ROS runtime");
    auto unit = units.front().as_object();
    for (auto &[field, selected] : unit) {
      if (field != "configuration") continue;
      auto configuration = selected.as_object();
      for (auto &[key, setting] : configuration) {
        if (key != "gripper_target_configurations") continue;
        // gripper_tcp is the real base frame used as an authored fixture
        // reference. It is not a calibrated jaw TCP or tool mounting claim.
        setting = CanonicalValue(CanonicalValue::Array{
            CanonicalValue(CanonicalValue::Object{
                {"actuation_kind", CanonicalValue("position_commanded")},
                {"attach_frame", CanonicalValue("robotiq_85_base_link")},
                {"auxiliary_frames", CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("robotiq_85_base_link")})},
                {"closed_state_ref", CanonicalValue("closed")},
                {"command_duration_s", CanonicalValue(1.0)},
                {"command_target_id", CanonicalValue("gripper")},
                {"controllable_component_id", CanonicalValue(std::string(kRobotiqGripperOnlyComponentId))},
                {"description", CanonicalValue("Standalone Robotiq 2F-85 Position fixture.")},
                {"mount_frame", CanonicalValue("robotiq_85_base_link")},
                {"open_state_ref", CanonicalValue("open")},
                {"output_mode", CanonicalValue("commanded")},
                {"state_freshness_timeout_s", CanonicalValue(1.0)},
                {"tcp_frame", CanonicalValue("robotiq_85_base_link")},
                {"touch_links", CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("robotiq_85_base_link"),
                    CanonicalValue("robotiq_85_left_finger_link"),
                    CanonicalValue("robotiq_85_left_finger_tip_link"),
                    CanonicalValue("robotiq_85_left_inner_knuckle_link"),
                    CanonicalValue("robotiq_85_left_knuckle_link"),
                    CanonicalValue("robotiq_85_right_finger_link"),
                    CanonicalValue("robotiq_85_right_finger_tip_link"),
                    CanonicalValue("robotiq_85_right_inner_knuckle_link"),
                    CanonicalValue("robotiq_85_right_knuckle_link")})},
            })});
      }
      selected = CanonicalValue(std::move(configuration));
    }
    units.front() = CanonicalValue(std::move(unit));
    value = CanonicalValue(std::move(units));
  }
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_robotiq_gripper_only_retention_fixture(std::string_view lifecycle_semantic_digest) {
  const auto structural = make_robotiq_gripper_only_structural_retention_fixture();
  return make_lite6_controllable_retention_fixture(
      false, false, 1U, true, true, "i6_robotiq_gripper_only_cell", false, true,
      "resource", true, lifecycle_semantic_digest, lifecycle_semantic_digest,
      true, true, true, std::nullopt, false, kLite6ArmTargetId,
      std::nullopt, std::nullopt, {}, true, {},
      "manyforge.provider.ros2_control", true, true, &structural,
      map_robotiq_gripper_only_graph, false, false, "position_commanded", 1.0,
      "commanded", map_robotiq_gripper_only_component);
}

}  // namespace manyforge::deployment::test
