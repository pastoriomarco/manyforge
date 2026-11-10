#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/command_schema_codec.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/controller_topology_codec.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <manyforge_deployment/structural_model_preparation.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>

#include "canonical_empty_admission_internal.hpp"
#include "build_manifest_internal.hpp"
#include "content_identity_internal.hpp"
#include "activation_deployment_compiler_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "prepared_activation_materialization_internal.hpp"
#include "retained_bundle_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

// The cell-scoped canonical-empty predecessor keeps the candidate's one ROS
// execution host but with the build-qualified empty-host configuration
// (canonical_empty_package_generator.cpp). The host is retained across the
// component-owned Apply that follows, so the predecessor must share the host's
// identity and its launch product exactly; the unit configuration and its
// configuration manifest are component data and necessarily differ.
[[nodiscard]] bool canonical_empty_runtime_units_match(
    const std::vector<core::RuntimeUnitDTO> &candidate,
    const std::vector<core::RuntimeUnitDTO> &empty) {
  if (candidate.size() != 1U || empty.size() != 1U) {
    return false;
  }
  const auto &candidate_host = candidate.front();
  const auto &empty_host = empty.front();
  return candidate_host.runtime_unit_id == empty_host.runtime_unit_id &&
         candidate_host.runtime_profile_id == empty_host.runtime_profile_id &&
         candidate_host.runtime_profile_revision ==
             empty_host.runtime_profile_revision &&
         candidate_host.dependency_runtime_unit_ids ==
             empty_host.dependency_runtime_unit_ids;
}

[[nodiscard]] bool canonical_empty_host_products_match(
    const std::vector<core::RetainedRuntimeUnitProductsDTO> &candidate,
    const std::vector<core::RetainedRuntimeUnitProductsDTO> &empty) {
  const auto launch = [](const core::RetainedProductDTO &product)
      -> std::optional<core::RetainedArtifactRefDTO> {
    for (const auto &artifact : product.artifact_refs) {
      if (artifact.artifact_role_id.value() ==
          deployment::detail::
              kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole) {
        return artifact;
      }
    }
    return std::nullopt;
  };
  if (candidate.size() != 1U || empty.size() != 1U ||
      candidate.front().runtime_unit_id != empty.front().runtime_unit_id ||
      candidate.front().products.size() != empty.front().products.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < candidate.front().products.size();
       ++index) {
    const auto &left = candidate.front().products[index];
    const auto &right = empty.front().products[index];
    const auto left_launch = launch(left);
    if (left.product_kind != right.product_kind ||
        left.product_contract_id != right.product_contract_id ||
        left.product_contract_revision != right.product_contract_revision ||
        !left_launch.has_value() || left_launch != launch(right)) {
      return false;
    }
  }
  return true;
}

/// One Deployment to materialize: its source key and whether it selects the
/// ROS execution runtime.
struct Scenario final {
  std::string source;
  bool ros2_control_execution;
};

[[nodiscard]] std::string
read_bounded_plain_file(const std::filesystem::path &path,
                        std::uintmax_t maximum_bytes, std::string_view label) {
  if (std::filesystem::is_symlink(path) ||
      !std::filesystem::is_regular_file(path)) {
    throw std::invalid_argument(std::string(label) +
                                " is not one plain file: " + path.string());
  }
  const auto size = std::filesystem::file_size(path);
  if (size == 0U || size > maximum_bytes) {
    throw std::invalid_argument(std::string(label) +
                                " violates its byte bound: " + path.string());
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open " + std::string(label) + ": " +
                             path.string());
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (stream.gcount() != static_cast<std::streamsize>(bytes.size()) ||
      stream.peek() != std::char_traits<char>::eof()) {
    throw std::runtime_error(
        std::string(label) +
        " changed during its bounded read: " + path.string());
  }
  return bytes;
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  return read_bounded_plain_file(path, deployment::kMaxArtifactEncodedBytes,
                                 "maintained source");
}

[[nodiscard]] const deployment::CanonicalValue &
require_member(const deployment::CanonicalValue &value, std::string_view name,
               std::string_view label) {
  const auto *member = value.find_object_member(name);
  if (member == nullptr) {
    throw std::invalid_argument(std::string(label) + " is missing " +
                                std::string(name));
  }
  return *member;
}

[[nodiscard]] const deployment::CanonicalValue::Array &
require_array(const deployment::CanonicalValue &value, std::string_view label) {
  const auto *array =
      std::get_if<deployment::CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    throw std::invalid_argument(std::string(label) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const std::string &
require_text(const deployment::CanonicalValue &value, std::string_view label) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr || text->empty()) {
    throw std::invalid_argument(std::string(label) + " must be non-empty text");
  }
  return *text;
}

class ExactCompilerInputSource final
    : public deployment::detail::ICompilerInputArtifactSource {
public:
  void add(const core::ContentAddressedStoreKey &key,
           const core::ContentDigest &digest, std::string bytes) {
    if (deployment::sha256_digest(bytes) != digest.value()) {
      throw std::invalid_argument("compiler input bytes have the wrong digest");
    }
    const auto [position, inserted] =
        bytes_.emplace(key.value(), std::move(bytes));
    if (!inserted &&
        deployment::sha256_digest(position->second) != digest.value()) {
      throw std::invalid_argument("compiler input key aliases different bytes");
    }
  }

  [[nodiscard]] std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &key,
               const core::ContentDigest &expected_digest,
               std::optional<std::uint64_t> expected_size) const override {
    const auto found = bytes_.find(key.value());
    if (found == bytes_.end() ||
        deployment::sha256_digest(found->second) != expected_digest.value() ||
        (expected_size.has_value() && found->second.size() != *expected_size)) {
      return std::nullopt;
    }
    return found->second;
  }

private:
  std::map<std::string, std::string, std::less<>> bytes_;
};

class MemoryBuildManifestRepository final
    : public deployment::IBuildManifestRepository {
 public:
  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const deployment::BuildQualifiedManifest> manifest)
      override {
    if (manifest == nullptr) {
      throw std::invalid_argument("in-memory manifest repository rejects null");
    }
    const auto reloaded = deployment::detail::reload_self_verified_build_manifest(
        manifest->key().manifest_kind, manifest->canonical_bytes());
    if (reloaded->key() != manifest->key()) {
      throw std::logic_error("in-memory manifest requalification diverged");
    }
    return bytes_.emplace(manifest->key(),
                          std::string(manifest->canonical_bytes())).second;
  }

  [[nodiscard]] std::shared_ptr<const deployment::BuildQualifiedManifest>
  get_verified(const deployment::BuildManifestKey &key) const override {
    const auto found = bytes_.find(key);
    if (found == bytes_.end()) return nullptr;
    const auto reloaded = deployment::detail::reload_self_verified_build_manifest(
        key.manifest_kind, found->second);
    return reloaded->key() == key ? reloaded : nullptr;
  }

 private:
  std::map<deployment::BuildManifestKey, std::string> bytes_;
};

class MemoryRawArtifactStore final : public deployment::IRawArtifactStore {
 public:
  [[nodiscard]] deployment::StoredRawArtifact
  put_if_absent(std::string_view bytes) override {
    const std::string digest = deployment::sha256_digest(bytes);
    const core::ContentAddressedStoreKey key("sha256/" + digest.substr(7U));
    const auto [position, inserted] = values_.emplace(
        key.value(), Value{core::ContentDigest(digest), std::string(bytes)});
    if (!inserted && (position->second.digest.value() != digest ||
                      position->second.bytes != bytes)) {
      throw std::logic_error("in-memory raw artifact identity collision");
    }
    return {core::ContentDigest(digest), key,
            static_cast<std::uint64_t>(bytes.size()), inserted};
  }

  [[nodiscard]] std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &key,
               const core::ContentDigest &expected_digest,
               std::uint64_t expected_size) const override {
    const auto found = values_.find(key.value());
    if (found == values_.end() ||
        found->second.digest != expected_digest ||
        found->second.bytes.size() != expected_size ||
        deployment::sha256_digest(found->second.bytes) !=
            expected_digest.value()) {
      return std::nullopt;
    }
    return found->second.bytes;
  }

  struct Value final {
    core::ContentDigest digest;
    std::string bytes;
  };
  [[nodiscard]] const std::map<std::string, Value, std::less<>> &values()
      const noexcept {
    return values_;
  }

 private:
  std::map<std::string, Value, std::less<>> values_;
};

class MemoryCatalogArtifactStore final
    : public deployment::ICatalogArtifactStore {
 public:
  [[nodiscard]] deployment::StoredCatalogArtifact put_if_absent(
      const core::ContentAddressedStoreKey &exact_key,
      const core::ContentDigest &expected_digest,
      std::string_view bytes) override {
    if (exact_key.value().empty() ||
        deployment::sha256_digest(bytes) != expected_digest.value()) {
      throw std::invalid_argument("in-memory catalog artifact is invalid");
    }
    const auto [position, inserted] = values_.emplace(
        exact_key.value(), Value{expected_digest, std::string(bytes)});
    if (!inserted && (position->second.digest != expected_digest ||
                      position->second.bytes != bytes)) {
      throw std::logic_error("in-memory catalog artifact key collision");
    }
    return {exact_key, expected_digest,
            static_cast<std::uint64_t>(bytes.size()), inserted};
  }

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey &exact_key,
      const core::ContentDigest &expected_digest,
      std::optional<std::uint64_t> expected_size) const override {
    const auto found = values_.find(exact_key.value());
    if (found == values_.end() || found->second.digest != expected_digest ||
        (expected_size.has_value() &&
         found->second.bytes.size() != *expected_size) ||
        deployment::sha256_digest(found->second.bytes) !=
            expected_digest.value()) {
      return std::nullopt;
    }
    return found->second.bytes;
  }

  struct Value final {
    core::ContentDigest digest;
    std::string bytes;
  };
  [[nodiscard]] const std::map<std::string, Value, std::less<>> &values()
      const noexcept {
    return values_;
  }

 private:
  std::map<std::string, Value, std::less<>> values_;
};

class MemoryRetainedBundleRepository final
    : public deployment::IRetainedBundleRepository {
 public:
  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const deployment::RetainedBundle> bundle) override {
    if (bundle == nullptr ||
        !deployment::verify_retained_bundle_identity(*bundle)) {
      throw std::invalid_argument("in-memory bundle repository rejects input");
    }
    const auto reloaded =
        deployment::detail::RetainedBundleReloadAuthority::reload(
            bundle->canonical_bytes());
    if (reloaded->retained_bundle_id() != bundle->retained_bundle_id()) {
      throw std::logic_error("in-memory bundle requalification diverged");
    }
    return bytes_.emplace(bundle->retained_bundle_id().value(),
                          std::string(bundle->canonical_bytes())).second;
  }

  [[nodiscard]] std::shared_ptr<const deployment::RetainedBundle>
  get_verified_envelope(
      const core::RetainedBundleId &retained_bundle_id) const override {
    const auto found = bytes_.find(retained_bundle_id.value());
    if (found == bytes_.end()) return nullptr;
    const auto reloaded =
        deployment::detail::RetainedBundleReloadAuthority::reload(found->second);
    return reloaded->retained_bundle_id() == retained_bundle_id ? reloaded
                                                                : nullptr;
  }

 private:
  std::map<std::string, std::string, std::less<>> bytes_;
};

struct MemoryPreparationStores final {
  MemoryBuildManifestRepository manifests;
  MemoryCatalogArtifactStore catalogs;
  MemoryRawArtifactStore raw;
  MemoryRetainedBundleRepository bundles;
};

[[nodiscard]] const std::string &
require_string_member(const deployment::CanonicalValue &root,
                      std::string_view name) {
  const auto *value = root.find_object_member(name);
  if (value == nullptr) {
    throw std::invalid_argument("catalog source is missing " +
                                std::string(name));
  }
  const auto *text = std::get_if<std::string>(&value->storage());
  if (text == nullptr) {
    throw std::invalid_argument("catalog source member is not text: " +
                                std::string(name));
  }
  return *text;
}

[[nodiscard]] std::vector<std::filesystem::path>
source_files(const std::filesystem::path &directory) {
  if (std::filesystem::is_symlink(directory) ||
      !std::filesystem::is_directory(directory)) {
    throw std::invalid_argument(
        "maintained catalog directory is unavailable: " + directory.string());
  }
  std::vector<std::filesystem::path> result;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() == ".json") {
      result.push_back(entry.path());
    }
  }
  std::sort(result.begin(), result.end());
  // Extra records are harmless; each consumer below requires what
  // it selects, so only an empty directory is refused here.
  if (result.empty()) {
    throw std::invalid_argument(
        "maintained catalog directory has no sources: " + directory.string());
  }
  return result;
}

using CatalogKey = std::tuple<std::uint8_t, std::string, std::string>;

[[nodiscard]] CatalogKey catalog_key(const core::CatalogRecordRefDTO &record) {
  return {static_cast<std::uint8_t>(core::catalog_record_kind(record)),
          core::catalog_record_id_value(record),
          core::catalog_record_revision_value(record)};
}

struct QualifiedCatalogClosure final {
  std::vector<deployment::QualifiedCommandSchema> command_schemas;
  std::vector<deployment::QualifiedComponentType> component_types;
  std::vector<deployment::QualifiedCapabilityDefinition> capabilities;
  std::vector<deployment::QualifiedControlInterfaceContract> contracts;
  std::vector<deployment::QualifiedControllerTopology> topologies;
  std::vector<deployment::QualifiedProviderManifest> provider_manifests;
  std::vector<deployment::QualifiedRuntimeProfile> runtime_profiles;
  std::vector<deployment::QualifiedStopEvidencePolicy> stop_policies;
  std::vector<deployment::SourceVerifiedModelAsset> model_assets;
  std::vector<deployment::BuildQualifiedStructuralModelPreparation>
      structural_preparations;
  std::map<CatalogKey, core::CatalogRecordRefDTO> records;
};

template <typename Value, typename Selector>
[[nodiscard]] std::vector<Value>
select_catalog_values(const std::vector<Value> &values,
                      const std::set<CatalogKey> &selected_records,
                      Selector selector) {
  std::vector<Value> result;
  for (const auto &value : values) {
    if (selected_records.contains(selector(value))) {
      result.push_back(value);
    }
  }
  return result;
}

struct ScenarioProduct final {
  core::DeploymentArtifactId deployment_artifact_id;
  core::DeploymentRevision deployment_revision;
  deployment::ScenarioDeploymentCompatibilityDTO deployment_compatibility;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::optional<deployment::VerifiedRetainedModelMap> model_map;
  std::filesystem::path canonical_empty_admission_filename;
  std::shared_ptr<const deployment::RetainedBundle> canonical_empty_bundle;
};

void verify_gripper_resource_semantics(
    const core::CellGraphDTO &graph,
    const deployment::VerifiedRetainedModelMap &model_map) {
  if (graph.controllable_components.empty()) {
    throw std::logic_error(
        "maintained gripper proof requires a controllable component");
  }
  const auto provider_only = [&](const auto &component) {
    // An arm-only component type has no gripper target to prove.
    if (component.component_type_id.value().ends_with(".arm_only.component"))
      return true;
    return component.model_role_bindings.empty() &&
           std::any_of(graph.control_domains.begin(),
                       graph.control_domains.end(), [&](const auto &domain) {
                         return domain.provider() &&
                                domain.provider()->controllable_component_id ==
                                    component.controllable_component_id;
                       });
  };
  const auto physical_components = static_cast<std::size_t>(std::count_if(
      graph.controllable_components.begin(),
      graph.controllable_components.end(),
      [&](const auto &component) { return !provider_only(component); }));
  // Genuine qualified provider-only components have no physical gripper fact.
  if (physical_components == 0U)
    return;
  // A split robot has no gripper (its mounted tool owns it, or it
  // has none). Every component that owns a gripper target proves its finger
  // resource.
  std::set<core::ControllableComponentId> gripper_components;
  std::set<core::ControllableComponentId> found_components;
  for (const auto &binding : model_map.control_bindings()) {
    if (binding.command_target_id.value() != "gripper") {
      continue;
    }
    gripper_components.insert(binding.controllable_component_id);
    const auto component = std::find_if(
        graph.controllable_components.begin(),
        graph.controllable_components.end(), [&](const auto &candidate) {
          return candidate.controllable_component_id ==
                 binding.controllable_component_id;
        });
    if (component == graph.controllable_components.end()) {
      throw std::logic_error(
          "maintained gripper binding names an unknown component");
    }
    for (const auto &resource : binding.resources) {
      if (resource.resource_role_id.value() != "fingers") {
        continue;
      }
      found_components.insert(binding.controllable_component_id);
      if (resource.coordinates.empty() ||
                 resource.component_local_coordinate_kind.has_value() ||
                 resource.coordinates.front().source_joint_name.empty() ||
                 resource.coordinates.front().compiled_joint_name.empty()) {
        throw std::logic_error(
            "maintained physical gripper lost its exact model-backed "
            "coordinate resource");
      }
    }
  }
  if (found_components != gripper_components) {
    throw std::logic_error(
        "maintained deployment lacks one retained gripper resource proof per "
        "gripper component");
  }
}

[[nodiscard]] std::map<std::string, ScenarioProduct, std::less<>>
materialize_scenarios(const std::filesystem::path &root,
                      const QualifiedCatalogClosure &catalog,
                      const ExactCompilerInputSource &input_source,
                      std::span<const Scenario> scenarios,
                      const deployment::CanonicalValue &deployment_value,
                      const bool publish_canonical_empty,
                      MemoryPreparationStores &stores) {
  if (scenarios.size() != 1U) {
    throw std::invalid_argument(
        "one candidate Deployment requires one Scenario");
  }
  const std::string overlay_bytes = read_file(
      root / "manyforge_deployment/resources/maintained/site_overlays/"
             "manyforge_local_examples.site_overlay.json");
  deployment::IBuildManifestRepository &manifest_repository = stores.manifests;
  deployment::ICatalogArtifactStore &catalog_store = stores.catalogs;
  deployment::IRawArtifactStore &raw_store = stores.raw;
  deployment::IRetainedBundleRepository &bundle_repository = stores.bundles;
  std::map<std::string, ScenarioProduct, std::less<>> result;

  for (const auto &scenario : scenarios) {
    if (!scenario.ros2_control_execution) {
      throw std::logic_error(
          "scenario cannot select its exact deployment runtime profile");
    }
    const std::string expected_runtime_profile_id =
        "manyforge.ros2_control_execution_runtime";
    std::vector<deployment::QualifiedRuntimeProfile> envelope_profiles;
    std::copy_if(
        catalog.runtime_profiles.begin(), catalog.runtime_profiles.end(),
        std::back_inserter(envelope_profiles), [&](const auto &profile) {
          return profile.value().runtime_profile_id.value() ==
                 expected_runtime_profile_id;
        });
    if (envelope_profiles.size() != 1U) {
      throw std::logic_error(
          "scenario cannot select its exact deployment runtime profile");
    }
    const auto verified = deployment::verify_deployment_artifact_revision(
        deployment::parse_strict_json_model_yaml(
            deployment::canonical_json(deployment_value)),
        envelope_profiles);
    std::set<CatalogKey> selected_records;
    for (const auto &reference : verified.catalog_records()) {
      selected_records.insert(catalog_key(reference));
    }

    const auto model_assets = select_catalog_values(
        catalog.model_assets, selected_records, [](const auto &value) {
          return CatalogKey{
              static_cast<std::uint8_t>(core::CatalogRecordKind::kModelAsset),
              value.value().model_asset_id.value(),
              value.value().model_revision.value()};
        });
    const auto component_types = select_catalog_values(
        catalog.component_types, selected_records, [](const auto &value) {
          return CatalogKey{static_cast<std::uint8_t>(
                                core::CatalogRecordKind::kComponentType),
                            value.value().component_type_id.value(),
                            value.value().component_type_revision.value()};
        });
    const auto command_schemas = select_catalog_values(
        catalog.command_schemas, selected_records, [](const auto &value) {
          return CatalogKey{static_cast<std::uint8_t>(
                                core::CatalogRecordKind::kCommandSchema),
                            value.value().command_schema_id.value(),
                            value.value().command_schema_revision.value()};
        });
    const auto capabilities = select_catalog_values(
        catalog.capabilities, selected_records, [](const auto &value) {
          return CatalogKey{
              static_cast<std::uint8_t>(
                  core::CatalogRecordKind::kCapabilityDefinition),
              value.value().capability_id.value(),
              value.value().capability_definition_revision.value()};
        });
    const auto contracts = select_catalog_values(
        catalog.contracts, selected_records, [](const auto &value) {
          return CatalogKey{
              static_cast<std::uint8_t>(
                  core::CatalogRecordKind::kControlInterfaceContract),
              value.value().control_interface_contract_id.value(),
              value.value().control_interface_revision.value()};
        });
    const auto topologies = select_catalog_values(
        catalog.topologies, selected_records, [](const auto &value) {
          return CatalogKey{static_cast<std::uint8_t>(
                                core::CatalogRecordKind::kControllerTopology),
                            value.value().controller_topology_id.value(),
                            value.value().topology_revision.value()};
        });
    const auto manifests = select_catalog_values(
        catalog.provider_manifests, selected_records, [](const auto &value) {
          return CatalogKey{static_cast<std::uint8_t>(
                                core::CatalogRecordKind::kProviderManifest),
                            value.value().provider_manifest_id.value(),
                            value.value().provider_manifest_revision.value()};
        });
    const auto profiles = select_catalog_values(
        catalog.runtime_profiles, selected_records, [](const auto &value) {
          return CatalogKey{static_cast<std::uint8_t>(
                                core::CatalogRecordKind::kRuntimeProfile),
                            value.value().runtime_profile_id.value(),
                            value.value().runtime_profile_revision.value()};
        });
    const auto stop_policies = select_catalog_values(
        catalog.stop_policies, selected_records, [](const auto &value) {
          return CatalogKey{
              static_cast<std::uint8_t>(
                  core::CatalogRecordKind::kStopEvidencePolicy),
              value.value().stop_evidence_policy_id.value(),
              value.value().stop_evidence_policy_revision.value()};
        });

    if ((model_assets.empty() != verified.graph().graph().model_instances.empty()) ||
        (component_types.empty() != verified.graph().graph().controllable_components.empty()) ||
        profiles.size() != 1U) {
      throw std::logic_error(
          "scenario cannot select its exact structural/runtime closure");
    }
    const auto composition = deployment::compose_cell_occurrences(
        verified.graph(), model_assets, component_types);
    const auto &inputs = verified.applicability_inputs().value();
    const auto selected_overlay = [&] {
      if (!inputs.site_overlay_ref.has_value()) {
        return deployment::select_site_overlay_for_deployment(
            inputs.site_overlay_ref, inputs.external_resources, nullptr);
      }
      const auto projection =
          deployment::resolve_and_verify_site_overlay_artifact_projection(
              *inputs.site_overlay_ref, overlay_bytes);
      return deployment::select_site_overlay_for_deployment(
          inputs.site_overlay_ref, inputs.external_resources, &projection);
    }();
    std::map<std::pair<std::string, std::string>,
             deployment::BuildQualifiedValueSchemaEntry>
        selected_value_schema_map;
    const auto &schema_registry =
        deployment::build_qualified_closed_schema_registry();
    for (const auto &row : inputs.stop_stage_parameters) {
      const auto &contract = row.provider_contract_ref;
      const auto *schema = schema_registry.find_qualified_value_schema(
          contract.contract_schema_id, contract.contract_schema_revision);
      if (schema == nullptr) {
        throw std::logic_error("maintained scenario has no exact "
                               "build-qualified Stop value schema");
      }
      selected_value_schema_map.emplace(
          std::pair{contract.contract_schema_id.value(),
                    contract.contract_schema_revision.value()},
          *schema);
    }
    std::vector<deployment::BuildQualifiedValueSchemaEntry>
        selected_value_schemas;
    selected_value_schemas.reserve(selected_value_schema_map.size());
    for (const auto &[_, schema] : selected_value_schema_map) {
      selected_value_schemas.push_back(schema);
    }

    std::vector<deployment::QualifiedCommissioningEvidenceRef>
        selected_commissioning_evidence;
    selected_commissioning_evidence.reserve(
        inputs.commissioned_stop_interfaces.size());
    for (const auto &row : inputs.commissioned_stop_interfaces) {
      const auto &artifact = row.commissioning_evidence_artifact_ref;
      const auto bytes = input_source.get_verified(
          artifact.content_addressed_store_key, artifact.content_digest,
          artifact.byte_size);
      if (!bytes.has_value()) {
        throw std::logic_error("maintained scenario cannot resolve exact Stop "
                               "commissioning evidence");
      }
      selected_commissioning_evidence.push_back(
          deployment::resolve_and_verify_commissioning_evidence_ref(
              row, artifact.content_addressed_store_key, *bytes));
    }
    const auto applicability = deployment::qualify_cell_applicability(
        composition, verified.applicability_inputs(), profiles,
        command_schemas, capabilities, contracts, topologies, manifests,
        stop_policies, selected_value_schemas,
        selected_commissioning_evidence, selected_overlay);
    const auto qualified = deployment::qualify_deployment_for_retention(
        verified, deployment::compile_cell_ir(applicability));

    std::vector<deployment::BuildQualifiedStructuralModelPreparation>
        selected_preparations;
    selected_preparations.reserve(model_assets.size());
    for (const auto &model_asset : model_assets) {
      const auto preparation = std::find_if(
          catalog.structural_preparations.begin(),
          catalog.structural_preparations.end(), [&](const auto &value) {
            return value.verified_model_asset().value().model_asset_id ==
                       model_asset.value().model_asset_id &&
                   value.verified_model_asset().value().model_revision ==
                       model_asset.value().model_revision;
          });
      if (preparation == catalog.structural_preparations.end()) {
        throw std::logic_error("scenario has no exact structural preparation");
      }
      selected_preparations.push_back(*preparation);
    }
    auto bundle = deployment::detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(
            qualified, manifest_repository, input_source, selected_preparations,
            catalog_store, raw_store, bundle_repository,
            /*retain_empty_model_map=*/true);
    if (bundle == nullptr ||
        bundle->value().compiled_products.symbol_role_maps.size() > 1U) {
      throw std::logic_error("scenario retained an invalid model-map set");
    }
    std::optional<deployment::VerifiedRetainedModelMap> model_map;
    if (!bundle->value().compiled_products.symbol_role_maps.empty()) {
      const auto &product =
          bundle->value().compiled_products.symbol_role_maps.front();
      if (product.artifact_refs.size() != 1U) {
        throw std::logic_error(
            "scenario model map has an incoherent artifact set");
      }
      const deployment::RetainedModelMapQuery map_query{
          bundle->retained_bundle_id(), product.product_id};
      const auto &artifact = product.artifact_refs.front();
      const auto bytes = stores.raw.get_verified(
          artifact.content_addressed_store_key, artifact.content_digest,
          artifact.byte_size);
      if (bytes.has_value()) {
        model_map = deployment::detail::
            RetainedModelMapVerificationAuthority::verify(*bundle, map_query,
                                                          *bytes);
      }
      if (!model_map.has_value()) {
        throw std::logic_error(
            "scenario model map cannot be independently reloaded");
      }
    } else if (!bundle->value().cell_graph.controllable_components.empty() ||
               !bundle->value().cell_graph.model_instances.empty()) {
      throw std::logic_error(
          "populated scenario does not retain its exact model map");
    }

    std::filesystem::path admission_filename;
    std::shared_ptr<const deployment::RetainedBundle> canonical_empty_bundle;
    if (publish_canonical_empty) {
      const auto empty_deployment =
          deployment::detail::derive_cell_scoped_canonical_empty_deployment(
              verified, profiles);
      const auto empty_composition = deployment::compose_cell_occurrences(
          empty_deployment.graph(),
          std::span<const deployment::SourceVerifiedModelAsset>{},
          std::span<const deployment::QualifiedComponentType>{});
      const auto empty_overlay = deployment::select_site_overlay_for_deployment(
          empty_deployment.applicability_inputs().value().site_overlay_ref,
          empty_deployment.applicability_inputs().value().external_resources,
          nullptr);
      const auto empty_applicability = deployment::qualify_cell_applicability(
          empty_composition, empty_deployment.applicability_inputs(), profiles,
          std::span<const deployment::QualifiedCommandSchema>{},
          std::span<const deployment::QualifiedCapabilityDefinition>{},
          std::span<const deployment::QualifiedControlInterfaceContract>{},
          std::span<const deployment::QualifiedControllerTopology>{},
          std::span<const deployment::QualifiedProviderManifest>{},
          std::span<const deployment::QualifiedStopEvidencePolicy>{},
          std::span<const deployment::BuildQualifiedValueSchemaEntry>{},
          std::span<const deployment::QualifiedCommissioningEvidenceRef>{},
          empty_overlay);
      const auto empty_qualified = deployment::qualify_deployment_for_retention(
          empty_deployment, deployment::compile_cell_ir(empty_applicability));
      const auto empty_bundle = deployment::detail::RetainedBundleMaterializer::
          materialize_store_and_reload_production(
              empty_qualified, manifest_repository, input_source, {},
              catalog_store, raw_store, bundle_repository);
      if (empty_bundle == nullptr) {
        throw std::logic_error(
            "cell-scoped canonical predecessor could not be materialized");
      }
      if (!canonical_empty_runtime_units_match(
              bundle->value().cell_graph.runtime_units,
              empty_bundle->value().cell_graph.runtime_units) ||
          !canonical_empty_host_products_match(
              bundle->value().compiled_products.runtime_unit_products,
              empty_bundle->value().compiled_products.runtime_unit_products)) {
        throw std::logic_error(
            "cell-scoped canonical predecessor does not retain the "
            "candidate's exact execution host product");
      }
      admission_filename =
          deployment::cell_scoped_canonical_empty_admission_filename(
              verified.cell_id());
      canonical_empty_bundle = empty_bundle;
    }
    // A cell without components has no gripper to prove; its empty map is
    // only the Program-binding model map.
    if (model_map.has_value() &&
        !verified.graph().graph().controllable_components.empty()) {
      verify_gripper_resource_semantics(verified.graph().graph(), *model_map);
    }
    if (!result
             .emplace(
                 std::string(scenario.source),
                 ScenarioProduct{
                     verified.deployment_artifact_id(),
                     verified.deployment_revision(),
                     deployment::scenario_deployment_compatibility(verified),
                     bundle, std::move(model_map), admission_filename,
                     std::move(canonical_empty_bundle)})
             .second) {
      throw std::logic_error("scenario product source is duplicated");
    }
  }
  return result;
}

template <typename Record>
[[nodiscard]] Record make_reference(std::string_view bytes) {
  constexpr core::CatalogRecordKind kind = Record::kRecordKind;
  const auto *descriptor = core::catalog_record_type_descriptor(kind);
  if (descriptor == nullptr) {
    throw std::logic_error("catalog record kind has no descriptor");
  }
  const auto *schema = deployment::build_qualified_closed_schema_registry()
                           .find_catalog_schema_family(kind);
  if (schema == nullptr ||
      schema->qualification() !=
          deployment::CatalogSchemaQualification::kQualified ||
      schema->compiled_schema() == nullptr) {
    throw std::logic_error("catalog record schema is not build-qualified");
  }

  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const std::string &record_id =
      require_string_member(parsed.root(), descriptor->own_id_field);
  const std::string &record_revision =
      require_string_member(parsed.root(), descriptor->own_revision_field);
  if (!record_revision.starts_with("sha256:") ||
      record_revision.size() != 71U) {
    throw std::invalid_argument(
        "catalog source revision is not a SHA-256 digest");
  }
  const core::ContentDigest content_digest(deployment::sha256_digest(bytes));
  const core::ContentAddressedStoreKey store_key(
      "catalog/maintained/" + std::string(core::to_string(kind)) + "/" + record_id + "/" +
      record_revision.substr(7U));
  return Record{
      typename Record::IdType(record_id),
      typename Record::RevisionType(record_revision),
      core::SchemaId(std::string(descriptor->schema_family)),
      schema->compiled_schema()->schema_revision(),
      content_digest,
      store_key,
  };
}

void insert_reference(QualifiedCatalogClosure &closure,
                      core::CatalogRecordRefDTO reference) {
  if (!closure.records.emplace(catalog_key(reference), reference).second) {
    throw std::invalid_argument(
        "maintained catalog contains a duplicate typed referent");
  }
}

[[nodiscard]] QualifiedCatalogClosure
qualify_catalog(const std::filesystem::path &root,
                ExactCompilerInputSource &input_source,
                std::vector<deployment::detail::ActivationPreparedModel>
                    prepared_models) {
  // A runtime-only Deployment has no model inputs; component/model
  // cardinality is checked by the authored compiler.
  const auto catalog_root =
      root / "manyforge_deployment/resources/maintained/catalog";
  QualifiedCatalogClosure result;

  for (const auto &path :
       source_files(catalog_root / "capability_definition")) {
    const std::string bytes = read_file(path);
    const auto reference =
        make_reference<core::CapabilityDefinitionCatalogRecordRefDTO>(bytes);
    result.capabilities.push_back(
        deployment::resolve_and_qualify_capability_definition(
            reference, reference.content_addressed_store_key, bytes));
    input_source.add(reference.content_addressed_store_key,
                     reference.content_digest, bytes);
    insert_reference(result, reference);
  }

  std::vector<deployment::QualifiedCapabilityDefinition> target_capabilities;
  for (const auto &capability : result.capabilities) {
    if (capability.value().scope == core::CapabilityScope::kTargetBinding) {
      target_capabilities.push_back(capability);
    }
  }
  // One component type per model family, plus an arm-only type where a Real
  // profile needs one. Generated per family, so no fixed count; each
  // file is still fully qualified and must find every target capability it
  // selects (a missing one is refused by its qualification).
  for (const auto &path : source_files(catalog_root / "component_type")) {
    const std::string bytes = read_file(path);
    const auto reference =
        make_reference<core::ComponentTypeCatalogRecordRefDTO>(bytes);
    const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
    std::set<std::string, std::less<>> selected_ids;
    for (const auto &target : require_array(
             require_member(parsed.root(), "command_targets", "component"),
             "command targets")) {
      for (const auto &capability : require_array(
               require_member(target, "capabilities", "command target"),
               "capabilities")) {
        selected_ids.insert(require_string_member(capability, "capability_id"));
      }
    }
    std::vector<deployment::QualifiedCapabilityDefinition>
        selected_capabilities;
    for (const auto &capability : target_capabilities) {
      if (selected_ids.contains(capability.value().capability_id.value())) {
        selected_capabilities.push_back(capability);
      }
    }
    result.component_types.push_back(
        deployment::resolve_and_qualify_component_type(
            reference, reference.content_addressed_store_key, bytes,
            selected_capabilities));
    input_source.add(reference.content_addressed_store_key,
                     reference.content_digest, bytes);
    insert_reference(result, reference);
  }

  std::vector<deployment::QualifiedControlInterfaceContract> contracts;
  for (const auto &path :
       source_files(catalog_root / "control_interface_contract")) {
    const std::string bytes = read_file(path);
    const auto reference =
        make_reference<core::ControlInterfaceContractCatalogRecordRefDTO>(
            bytes);
    contracts.push_back(
        deployment::resolve_and_qualify_control_interface_contract(
            reference, reference.content_addressed_store_key, bytes));
    input_source.add(reference.content_addressed_store_key,
                     reference.content_digest, bytes);
    insert_reference(result, reference);
  }

  for (const auto &path : source_files(catalog_root / "controller_topology")) {
    const std::string bytes = read_file(path);
    const auto reference =
        make_reference<core::ControllerTopologyCatalogRecordRefDTO>(bytes);
    const bool ros2_control =
        reference.record_id.value().starts_with("manyforge.ros2_control_");
    const bool arm =
        reference.record_id.value().find("_arm_") != std::string::npos;
    if (!ros2_control) {
      throw std::logic_error(
          "maintained topology cannot select its exact contract closure");
    }
    const std::string expected_contract_id =
        std::string("manyforge.ros2_control_") +
        (arm ? "arm_position" : "gripper_position");
    std::vector<deployment::QualifiedControlInterfaceContract> selected;
    std::copy_if(
        contracts.begin(), contracts.end(), std::back_inserter(selected),
        [&expected_contract_id](const auto &contract) {
          return contract.value().control_interface_contract_id.value() ==
                 expected_contract_id;
        });
    if (selected.size() != 1U) {
      throw std::logic_error(
          "maintained topology cannot select its exact contract closure");
    }
    result.topologies.push_back(
        deployment::resolve_and_qualify_controller_topology(
            reference, reference.content_addressed_store_key, bytes, selected));
    input_source.add(reference.content_addressed_store_key,
                     reference.content_digest, bytes);
    insert_reference(result, reference);
  }

  bool found_ros2_control_lifecycle_manifest = false;
  // The lifecycle providers below must each be present once with their Stop
  // contract roles. Their offer lists are not counted: the compiler refuses a
  // capability binding no selected provider offers, and extra offers are
  // harmless.
  for (const auto &path : source_files(catalog_root / "provider_manifest")) {
    const std::string bytes = read_file(path);
    const auto reference =
        make_reference<core::ProviderManifestCatalogRecordRefDTO>(bytes);
    auto manifest = deployment::resolve_and_qualify_provider_manifest(
        reference, reference.content_addressed_store_key, bytes);
    const auto has_contract_role = [&manifest](std::string_view role_id) {
      return std::any_of(manifest.value().provider_contracts.begin(),
                         manifest.value().provider_contracts.end(),
                         [role_id](const auto &contract) {
                           return contract.provider_contract_ref
                                      .provider_contract_role_id.value() ==
                                  role_id;
                         });
    };
    if (manifest.value().provider_manifest_id.value() ==
        "manyforge.provider.ros2_control") {
      if (found_ros2_control_lifecycle_manifest ||
          !has_contract_role("gripper_logical_cancel") ||
          !has_contract_role("software_hard_stop") ||
          !has_contract_role("stop_contract")) {
        throw std::logic_error(
            "maintained ROS control lifecycle provider manifest is not exact");
      }
      found_ros2_control_lifecycle_manifest = true;
    }
    result.provider_manifests.push_back(std::move(manifest));
    input_source.add(reference.content_addressed_store_key,
                     reference.content_digest, bytes);
    insert_reference(result, reference);
  }
  if (!found_ros2_control_lifecycle_manifest) {
    throw std::logic_error(
        "maintained lifecycle provider manifest closure is missing");
  }

  for (const auto &path : source_files(catalog_root / "runtime_profile")) {
    const std::string bytes = read_file(path);
    const auto reference =
        make_reference<core::RuntimeProfileCatalogRecordRefDTO>(bytes);
    result.runtime_profiles.push_back(
        deployment::resolve_and_qualify_runtime_profile(
            reference, reference.content_addressed_store_key, bytes));
    input_source.add(reference.content_addressed_store_key,
                     reference.content_digest, bytes);
    insert_reference(result, reference);
  }

  for (const auto &path :
       source_files(catalog_root / "stop_evidence_policy")) {
    const std::string bytes = read_file(path);
    const auto reference =
        make_reference<core::StopEvidencePolicyCatalogRecordRefDTO>(bytes);
    result.stop_policies.push_back(
        deployment::resolve_and_qualify_stop_evidence_policy(
            reference, reference.content_addressed_store_key, bytes));
    input_source.add(reference.content_addressed_store_key,
                     reference.content_digest, bytes);
    insert_reference(result, reference);
  }

  for (const std::string_view name : {"gripper.command_position.v1.yaml"}) {
    const std::filesystem::path command_path =
        root / "manyforge_deployment/resources/catalog" / name;
    const std::string command_bytes = read_file(command_path);
    const auto command_reference =
        make_reference<core::CommandSchemaCatalogRecordRefDTO>(command_bytes);
    result.command_schemas.push_back(
        deployment::resolve_and_qualify_command_schema(
            command_reference, command_reference.content_addressed_store_key,
            command_bytes));
    input_source.add(command_reference.content_addressed_store_key,
                     command_reference.content_digest, command_bytes);
    insert_reference(result, command_reference);
  }

  result.contracts = contracts;
  std::set<std::pair<std::string, std::string>> prepared_model_identities;
  for (auto &model : prepared_models) {
    const auto identity = std::pair{
        model.catalog_reference.record_id.value(),
        model.catalog_reference.record_revision.value()};
    if (!prepared_model_identities.insert(identity).second) {
      continue;
    }
    input_source.add(model.catalog_reference.content_addressed_store_key,
                     model.catalog_reference.content_digest,
                     model.catalog_bytes);
    for (const auto &artifact : model.source_artifacts) {
      input_source.add(
          core::ContentAddressedStoreKey(
              "sha256/" + artifact.content_digest.value().substr(7U)),
          artifact.content_digest, artifact.bytes);
    }
    insert_reference(result, model.catalog_reference);
    result.model_assets.push_back(std::move(model.verified_asset));
    result.structural_preparations.push_back(std::move(model.preparation));
  }
  const std::size_t expected_record_count =
      result.command_schemas.size() + result.component_types.size() +
      result.capabilities.size() + result.contracts.size() +
      result.topologies.size() + result.provider_manifests.size() +
      result.runtime_profiles.size() +
      result.stop_policies.size() + result.model_assets.size();
  if (result.records.size() != expected_record_count ||
      result.model_assets.size() != result.structural_preparations.size()) {
    throw std::logic_error("maintained exact catalog closure is incomplete");
  }
  return result;
}

} // namespace

namespace manyforge::deployment::detail {

PreparedActivationMaterialization materialize_prepared_activation(
    const std::filesystem::path &capability_root,
    std::vector<ActivationPreparedModel> prepared_models,
    const ActivationDeploymentIntent &intent,
    const core::CellId &runtime_cell_id,
    const std::vector<ActivationComponentProfile> &component_profiles,
    const bool prepare_canonical_empty) {
  const auto workspace_root = std::filesystem::canonical(capability_root);

  ExactCompilerInputSource input_source;
  auto catalog = qualify_catalog(workspace_root, input_source,
                                 prepared_models);
  auto io_types = prepare_activation_io_types(intent, component_profiles,
                                              catalog.capabilities);
  // Pose sources and AI models reuse the model-free derived-type pipeline.
  for (auto &prepared : prepare_activation_pose_source_types(
           intent, component_profiles, prepared_models,
           catalog.capabilities))
    io_types.push_back(std::move(prepared));
  for (auto &prepared : prepare_activation_ai_model_types(
           intent, component_profiles, catalog.capabilities))
    io_types.push_back(std::move(prepared));
  // Machine owners: a derived type without targets.
  for (auto &prepared :
       prepare_activation_machine_types(intent, component_profiles))
    io_types.push_back(std::move(prepared));
  // Sensors: a derived type without targets, as a machine owner.
  for (auto &prepared :
       prepare_activation_sensor_types(intent, component_profiles))
    io_types.push_back(std::move(prepared));
  // The assistant: likewise.
  for (auto &prepared :
       prepare_activation_assistant_types(intent, component_profiles))
    io_types.push_back(std::move(prepared));
  const auto assistant_profiles =
      resolve_activation_assistant_profiles(workspace_root, intent, component_profiles);
  for (const auto &prepared : io_types) {
    const auto &reference = prepared.type.reference();
    input_source.add(reference.content_addressed_store_key,
                     reference.content_digest, prepared.catalog_bytes);
    if (!catalog.records.contains(catalog_key(reference))) {
      insert_reference(catalog, reference);
      catalog.component_types.push_back(prepared.type);
    }
  }
  std::vector<core::CatalogRecordRefDTO> catalog_records;
  catalog_records.reserve(catalog.records.size());
  for (const auto &[unused, reference] : catalog.records) {
    static_cast<void>(unused);
    catalog_records.push_back(reference);
  }
  CanonicalValue compiled;
  try {
    compiled = compile_activation_deployment(
        intent, runtime_cell_id, component_profiles, prepared_models,
        catalog_records, catalog.runtime_profiles, io_types,
        catalog.provider_manifests, assistant_profiles, catalog.component_types);
  } catch (const std::bad_alloc &) {
    throw std::runtime_error(
        "activation Deployment compiler exhausted memory");
  } catch (const std::length_error &error) {
    throw std::runtime_error(
        std::string("activation Deployment compiler produced an invalid size: ") +
        error.what());
  }
  const auto parsed = parse_strict_json_model_yaml(canonical_json(compiled));
  const auto &graph = require_member(parsed.root(), "graph", "Deployment");
  const auto &runtime_units = require_array(
      require_member(graph, "runtime_units", "Deployment.graph"),
      "Deployment.graph.runtime_units");
  if (runtime_units.empty()) {
    throw std::invalid_argument("Deployment has no runtime unit");
  }
  const std::string runtime_profile_id = require_text(
      require_member(runtime_units.front(), "runtime_profile_id", "runtime unit"),
      "runtime unit.runtime_profile_id");
  const std::string source_key = "captured.deployment";
  const Scenario scenario{
      source_key,
      runtime_profile_id == "manyforge.ros2_control_execution_runtime",
  };
  const std::array<Scenario, 1U> selected{scenario};
  MemoryPreparationStores stores;
  std::map<std::string, ScenarioProduct, std::less<>> products;
  try {
    products = materialize_scenarios(workspace_root, catalog, input_source,
                                     selected, compiled,
                                     prepare_canonical_empty, stores);
  } catch (const std::bad_alloc &) {
    throw std::runtime_error(
        "activation Deployment materialization exhausted memory");
  } catch (const std::length_error &error) {
    throw std::runtime_error(
        std::string("activation Deployment materialization produced an invalid size: ") +
        error.what());
  }
  const auto found = products.find(source_key);
  if (found == products.end()) {
    throw std::logic_error("prepared Deployment was not materialized");
  }
  std::vector<PreparedActivationArtifact> raw_artifacts;
  raw_artifacts.reserve(stores.raw.values().size());
  for (const auto &[key, value] : stores.raw.values()) {
    raw_artifacts.push_back({core::ContentAddressedStoreKey(key), value.digest,
                             value.bytes});
  }
  std::vector<PreparedActivationArtifact> catalog_artifacts;
  catalog_artifacts.reserve(stores.catalogs.values().size());
  for (const auto &[key, value] : stores.catalogs.values()) {
    catalog_artifacts.push_back(
        {core::ContentAddressedStoreKey(key), value.digest, value.bytes});
  }
  return PreparedActivationMaterialization{
      found->second.deployment_artifact_id,
      found->second.deployment_revision,
      found->second.deployment_compatibility,
      found->second.bundle,
      found->second.model_map,
      found->second.canonical_empty_bundle,
      std::move(raw_artifacts),
      std::move(catalog_artifacts),
  };
}

} // namespace manyforge::deployment::detail
