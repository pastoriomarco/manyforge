#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <manyforge_deployment/content_identity.hpp>
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
        "examples/lite6_pick_and_place.scenario") {
  using namespace maintained_activation_fixture_detail;
  const std::filesystem::path workspace(MANYFORGE_WORKSPACE_ROOT);
  const auto scenario = workspace / scenario_relative_path;
  const std::filesystem::path prepared_root(
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT);
  auto authored =
      parse_strict_json_model_yaml(Read(scenario / "deployment.yaml")).root();
  auto policies = Member(authored, "policies");
  for (const auto &[name, value] : configuration.as_object())
    policies = With(std::move(policies), name, value);
  authored = With(std::move(authored), "policies", std::move(policies));
  const auto &components = Member(authored, "components").as_array();
  const auto model_component =
      std::find_if(components.begin(), components.end(), [](const auto &row) {
        return row.find_object_member("model_source") != nullptr;
      });
  if (model_component == components.end() ||
      std::count_if(components.begin(), components.end(), [](const auto &row) {
        return row.find_object_member("model_source") != nullptr;
      }) != 1)
    throw std::logic_error(
        "maintained Lite6 Scenario must have exactly one model component");
  const auto &component = *model_component;
  const auto component_id = Member(component, "component_id").as_string();
  const auto &source = Member(component, "model_source");
  const auto source_path = [&](std::string_view kind) {
    return scenario / Member(Member(source, kind), "project_path").as_string();
  };
  const auto receipt = parse_strict_json_model_yaml(
                           Read(prepared_root / "preparation-manifest.json"))
                           .root();
  const auto &prepared = Member(receipt, "prepared_source");
  detail::CapturedComponentModelSource captured{
      component_id,
      Capture("urdf", "application/xml", source_path("urdf")),
      Capture("srdf", "application/xml", source_path("srdf")),
      {}};
  // Resource paths are the exact maintained family paths, and the normal
  // prepared-model qualifier independently joins their bytes to this receipt.
  const auto description =
      parse_strict_json_model_yaml(
          Read(workspace / "manyforge_deployment/resources/maintained/"
                           "maintained_deployment_sources.v1.json"))
          .root();
  const auto &families = Member(description, "model_families").as_array();
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
    captured.resources.push_back(
        Capture(Member(rewrite, "original_package_uri").as_string(),
                Member(rewrite, "media_type").as_string(),
                resources / Member(rewrite, "relative_path").as_string()));
  const auto intent =
      detail::parse_activation_deployment_intent(authored, {captured});
  const auto profiles =
      detail::resolve_activation_component_profiles(workspace, intent);
  detail::PreparedComponentModelSource prepared_source{
      component_id,
      Member(receipt, "model_asset_id").as_string(),
      Member(receipt, "accepted_package_uri_prefix").as_string(),
      receipt,
      Capture(Member(prepared, "urdf_logical_uri").as_string(),
              "application/xml", prepared_root / "model.urdf"),
      Capture(Member(prepared, "srdf_logical_uri").as_string(),
              "application/xml", prepared_root / "model.srdf")};
  auto models = detail::qualify_activation_prepared_models(intent, profiles,
                                                           {prepared_source});
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
} // namespace manyforge::deployment::test
