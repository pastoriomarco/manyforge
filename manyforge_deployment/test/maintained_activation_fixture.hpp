#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_prepared_model_internal.hpp"
#include "prepared_activation_materialization_internal.hpp"

namespace manyforge::deployment::test {
namespace maintained_activation_fixture_detail {
inline std::string Read(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::logic_error("maintained source fixture missing: " +
                           path.string());
  std::string bytes{std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>()};
  if (input.bad() || bytes.empty())
    throw std::logic_error("maintained source fixture unreadable: " +
                           path.string());
  return bytes;
}
inline const CanonicalValue &Member(const CanonicalValue &value,
                                    std::string_view key) {
  const auto *selected = value.find_object_member(key);
  if (!selected)
    throw std::logic_error("maintained source fixture missing member: " +
                           std::string(key));
  return *selected;
}
inline CanonicalValue With(CanonicalValue value, std::string name,
                           CanonicalValue selected) {
  auto fields = value.as_object();
  const auto existing =
      std::find_if(fields.begin(), fields.end(),
                   [&](const auto &field) { return field.first == name; });
  if (existing == fields.end())
    fields.emplace_back(std::move(name), std::move(selected));
  else
    existing->second = std::move(selected);
  return CanonicalValue(std::move(fields));
}
/// The prepared structural root of one maintained model asset. Each is a
/// sibling of the combined Lite6 root named by MANYFORGE_LITE6_STRUCTURAL_
/// FIXTURE_ROOT, with the asset id in place of `lite6_lite_gripper`.
inline std::filesystem::path PreparedRoot(std::string_view model_asset_id) {
  const std::filesystem::path combined(MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT);
  constexpr std::string_view kCombined = "lite6_lite_gripper";
  const auto name = combined.filename().string();
  if (!name.starts_with(kCombined))
    throw std::logic_error("maintained structural fixture root is not the "
                           "combined Lite6 root: " + combined.string());
  return combined.parent_path() /
         (std::string(model_asset_id) + name.substr(kCombined.size()));
}
inline detail::CapturedModelSourceArtifact
Capture(std::string uri, std::string media, const std::filesystem::path &path) {
  auto bytes = Read(path);
  auto digest = core::ContentDigest(sha256_digest(bytes));
  return {std::move(uri), std::move(media), std::move(digest),
          std::move(bytes)};
}
} // namespace maintained_activation_fixture_detail

/// Use the same authored intent, source bytes and structural receipt as the
/// selected maintained Scenario (the original Lite6 project by default). Only
/// authored policy fields vary within that project. The normal activation
/// qualifier/materializer owns all catalog, ControlBinding and product proofs.
inline detail::PreparedActivationMaterialization
MaterializeMaintainedInterruptDeployment(
    const std::filesystem::path &package_root,
    const CanonicalValue &configuration,
    const std::filesystem::path &scenario_relative_path =
        "examples/mock_lite6_pick_and_place.scenario",
    void (*authored_mapper)(CanonicalValue::Object &) = nullptr,
    void (*profile_mapper)(std::vector<detail::ActivationComponentProfile> &) = nullptr) {
  using namespace maintained_activation_fixture_detail;
  const std::filesystem::path workspace(MANYFORGE_WORKSPACE_ROOT);
  const auto scenario = workspace / scenario_relative_path;
  auto authored =
      parse_strict_json_model_yaml(Read(scenario / "deployment.yaml")).root();
  auto policies = Member(authored, "policies");
  for (const auto &[name, value] : configuration.as_object())
    policies = With(std::move(policies), name, value);
  authored = With(std::move(authored), "policies", std::move(policies));
  if (authored_mapper != nullptr) {
    auto members = authored.as_object();
    authored_mapper(members);
    authored = CanonicalValue(std::move(members));
  }
  // Since the tool split a maintained Scenario has one model component per
  // physical model (arm, mounted tool), each with its own prepared root.
  std::vector<detail::CapturedComponentModelSource> captured;
  for (const auto &component : Member(authored, "components").as_array()) {
    const auto *source = component.find_object_member("model_source");
    if (source == nullptr)
      continue;
    const auto source_path = [&](std::string_view kind) {
      return scenario /
             Member(Member(*source, kind), "project_path").as_string();
    };
    captured.push_back({Member(component, "component_id").as_string(),
                        Capture("urdf", "application/xml", source_path("urdf")),
                        Capture("srdf", "application/xml", source_path("srdf")),
                        {}});
  }
  if (captured.empty())
    throw std::logic_error(
        "maintained Lite6 Scenario must have a model component");
  const auto source_profiles = detail::resolve_activation_component_profiles(
      workspace, detail::parse_activation_deployment_intent(authored, captured));
  // Resource paths are the exact maintained family paths, and the normal
  // prepared-model qualifier independently joins their bytes to this receipt.
  const auto description =
      parse_strict_json_model_yaml(
          Read(workspace / "manyforge_deployment/resources/maintained/"
                           "maintained_deployment_sources.v1.json"))
          .root();
  const auto &families = Member(description, "model_families").as_array();
  std::vector<detail::PreparedComponentModelSource> prepared_sources;
  for (auto &source : captured) {
    const auto profile = std::find_if(
        source_profiles.begin(), source_profiles.end(),
        [&](const auto &row) { return row.component_id == source.component_id; });
    if (profile == source_profiles.end())
      throw std::logic_error("maintained model component has no profile");
    const auto prepared_root = PreparedRoot(profile->model_asset_id);
    const auto receipt = parse_strict_json_model_yaml(
                             Read(prepared_root / "preparation-manifest.json"))
                             .root();
    const auto &prepared = Member(receipt, "prepared_source");
    const auto family =
        std::find_if(families.begin(), families.end(), [&](const auto &row) {
          return Member(row, "model_asset_id").as_string() ==
                 Member(receipt, "model_asset_id").as_string();
        });
    if (family == families.end())
      throw std::logic_error("maintained receipt has no model family");
    const auto resources =
        workspace /
        Member(Member(*family, "source"), "resource_root").as_string();
    for (const auto &rewrite : Member(receipt, "resource_rewrites").as_array())
      source.resources.push_back(
          Capture(Member(rewrite, "original_package_uri").as_string(),
                  Member(rewrite, "media_type").as_string(),
                  resources / Member(rewrite, "relative_path").as_string()));
    prepared_sources.push_back(
        {source.component_id, Member(receipt, "model_asset_id").as_string(),
         Member(receipt, "accepted_package_uri_prefix").as_string(), receipt,
         Capture(Member(prepared, "urdf_logical_uri").as_string(),
                 "application/xml", prepared_root / "model.urdf"),
         Capture(Member(prepared, "srdf_logical_uri").as_string(),
                 "application/xml", prepared_root / "model.srdf")});
  }
  const auto intent =
      detail::parse_activation_deployment_intent(authored, std::move(captured));
  auto profiles =
      detail::resolve_activation_component_profiles(workspace, intent);
  if (profile_mapper != nullptr) profile_mapper(profiles);
  auto models = detail::qualify_activation_prepared_models(
      intent, profiles, std::move(prepared_sources));
  auto result = detail::materialize_prepared_activation(
      workspace, std::move(models), intent,
      core::CellId("maintained_interrupt_fixture_cell"), profiles);
  if (!result.bundle || !result.model_map)
    throw std::logic_error(
        "actual maintained activation lost its bundle/model map");
  FilesystemCatalogArtifactStore catalogs(package_root);
  for (const auto &artifact : result.catalog_artifacts)
    (void)catalogs.put_if_absent(artifact.content_addressed_store_key,
                                 artifact.content_digest, artifact.bytes);
  FilesystemRawArtifactStore raw(package_root);
  for (const auto &artifact : result.raw_artifacts) {
    const auto stored = raw.put_if_absent(artifact.bytes);
    if (stored.content_addressed_store_key !=
            artifact.content_addressed_store_key ||
        stored.content_digest != artifact.content_digest)
      throw std::logic_error(
          "actual activation raw publication changed identity");
  }
  FilesystemRetainedBundleRepository bundles(package_root);
  (void)bundles.put_if_absent(result.bundle);
  return result;
}

/// The maintained Lite6 and its real tool, placed separately in the cell.
/// The normal activation path still qualifies both component types and models.
inline detail::PreparedActivationMaterialization
MaterializeMaintainedLite6WithUnmountedTool(
    const std::filesystem::path &package_root) {
  const auto separate_tool = [](CanonicalValue::Object &root) {
    using namespace maintained_activation_fixture_detail;
    const auto attachments = std::find_if(root.begin(), root.end(),
        [](const auto &row) { return row.first == "attachments"; });
    const auto components = std::find_if(root.begin(), root.end(),
        [](const auto &row) { return row.first == "components"; });
    if (attachments == root.end() || components == root.end())
      throw std::logic_error("maintained Lite6 fixture is incomplete");
    attachments->second = CanonicalValue(CanonicalValue::Array{});
    auto rows = components->second.as_array();
    auto tool = std::find_if(rows.begin(), rows.end(), [](const auto &row) {
      const auto *id = row.find_object_member("component_id");
      return id && id->as_string() == "lite_gripper";
    });
    if (tool == rows.end())
      throw std::logic_error("maintained Lite6 fixture has no tool");
    *tool = With(std::move(*tool), "root_pose_in_cell",
        parse_strict_json_model_yaml(
            R"({"position_m":[2.0,0.0,0.0],"orientation_xyzw":[0.0,0.0,0.0,1.0]})").root());
    components->second = CanonicalValue(std::move(rows));
  };
  return MaterializeMaintainedInterruptDeployment(
      package_root,
      parse_strict_json_model_yaml(R"({"interrupt_rules":[]})").root(),
      "examples/mock_lite6_pick_and_place.scenario", separate_tool);
}
} // namespace manyforge::deployment::test
