#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
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
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
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
#include <variant>
#include <vector>

#include "canonical_empty_admission_internal.hpp"
#include "build_manifest_internal.hpp"
#include "content_identity_internal.hpp"
#include "activation_deployment_compiler_internal.hpp"
#include "prepared_activation_materialization_internal.hpp"
#include "retained_bundle_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

[[nodiscard]] bool canonical_empty_runtime_units_match(
    const std::vector<core::RuntimeUnitDTO> &candidate,
    const std::vector<core::RuntimeUnitDTO> &empty) {
  if (candidate.size() != empty.size()) {
    return false;
  }
  if (candidate.size() == 1U &&
      candidate.front().runtime_profile_id.value() ==
          deployment::native_execution_runtime_contract::kRuntimeProfileId) {
    const auto &candidate_host = candidate.front();
    const auto &empty_host = empty.front();
    return candidate_host.runtime_unit_id == empty_host.runtime_unit_id &&
           candidate_host.runtime_profile_id == empty_host.runtime_profile_id &&
           candidate_host.runtime_profile_revision ==
               empty_host.runtime_profile_revision &&
           candidate_host.dependency_runtime_unit_ids ==
               empty_host.dependency_runtime_unit_ids;
  }
  return candidate == empty;
}

struct Options final {
  std::filesystem::path workspace_root;
  std::optional<std::filesystem::path> package_root;
  std::map<std::string, std::filesystem::path, std::less<>> prepared_roots;
};

struct Scenario final {
  std::string_view source;
  std::string_view family_id;
  bool assistant_enabled;
  bool ros2_control_execution;
  std::string_view isaac_profile;
  /// Q63-Q66: the offline mock controller profile replacing the native
  /// simulated executor (empty for Isaac).
  std::string_view mock_profile;
};

constexpr std::array<Scenario, 12U> kScenarios{{
    {"examples/assistant_modes_scene_authoring.deployment.yaml",
     "ur10e_robotiq_2f_140", true, true, "", "ur10e_robotiq_2f_140"},
    {"examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml",
     "lite6_lite_gripper", false, true, "lite6", ""},
    {"examples/isaac_manipulator_scene_light_assistant.deployment.yaml",
     "ur10e_robotiq_2f_140", true, true, "ur10e", ""},
    {"examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml",
     "ur10e_robotiq_2f_140", false, true, "ur10e", ""},
    {"examples/"
     "isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml",
     "ur10e_robotiq_2f_140", false, true, "ur10e", ""},
    {"examples/lite6_lite_gripper_pick_and_place.deployment.yaml",
     "lite6_lite_gripper", false, true, "", "lite6_lite_gripper"},
    {"examples/pick_and_place.deployment.yaml", "ur5_roboplan", false, true,
     "", "ur5_roboplan"},
    {"examples/pick_and_place_ur10e_robotiq.deployment.yaml",
     "ur10e_robotiq_2f_140", false, true, "", "ur10e_robotiq_2f_140"},
    {"examples/semantic_shape_pick.deployment.yaml", "ur5_roboplan", true,
     true, "", "ur5_roboplan"},
    {"examples/trocar_lite6_foundationpose_pick.deployment.yaml",
     "lite6_lite_gripper", false, true, "lite6", ""},
    {"examples/vision_guided_bin_pick.deployment.yaml", "ur5_roboplan", true,
     true, "", "ur5_roboplan"},
    {"examples/vision_guided_pose_pick.deployment.yaml", "ur5_roboplan", true,
     true, "", "ur5_roboplan"},
}};

constexpr std::array<Scenario, 1U> kComposedScenarios{{
    {"examples/ur5e_lite6_wall.deployment.yaml", "", false, true, "",
     "mixed"},
}};

[[nodiscard]] Options parse_options(int argc, char **argv) {
  if (argc < 3 || std::string_view(argv[1]) != "--check") {
    throw std::invalid_argument(
        "usage: manyforge_maintained_deployment_occurrences "
        "--check "
        "--workspace-root <manyforge-root> "
        "[--package-root <empty-output-directory>] "
        "--prepared <family-id>=<directory> [--prepared ...]");
  }
  Options result;
  for (int index = 2; index < argc;) {
    const std::string_view option(argv[index]);
    if (index + 1 >= argc) {
      throw std::invalid_argument(
          "maintained qualification option has no value");
    }
    const std::string value(argv[index + 1]);
    if (option == "--workspace-root" && result.workspace_root.empty()) {
      result.workspace_root = value;
      index += 2;
      continue;
    }
    if (option == "--package-root" && !result.package_root.has_value()) {
      result.package_root = value;
      index += 2;
      continue;
    }
    if (option == "--prepared") {
      const auto separator = value.find('=');
      if (separator != std::string::npos && separator != 0U &&
          separator + 1U != value.size() &&
          result.prepared_roots
              .emplace(value.substr(0U, separator),
                       value.substr(separator + 1U))
              .second) {
        index += 2;
        continue;
      }
    }
    throw std::invalid_argument(
        "invalid or repeated maintained qualification option");
  }
  if (result.workspace_root.empty() || result.prepared_roots.empty()) {
    throw std::invalid_argument("maintained qualification requires one "
                                "workspace root and prepared families");
  }
  return result;
}

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

[[nodiscard]] const deployment::CanonicalValue::Object &
require_object(const deployment::CanonicalValue &value,
               std::string_view label) {
  const auto *object =
      std::get_if<deployment::CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    throw std::invalid_argument(std::string(label) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const std::string &
require_text(const deployment::CanonicalValue &value, std::string_view label) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr || text->empty()) {
    throw std::invalid_argument(std::string(label) + " must be non-empty text");
  }
  return *text;
}

[[nodiscard]] std::uint64_t
require_size(const deployment::CanonicalValue &value, std::string_view label) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || *number < 0.0 ||
      *number > static_cast<double>(deployment::kMaxModelSourceClosureBytes) ||
      *number != static_cast<double>(static_cast<std::uint64_t>(*number))) {
    throw std::invalid_argument(std::string(label) +
                                " must be a bounded integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] std::filesystem::path
require_relative_path(std::string_view text, std::string_view label) {
  const std::filesystem::path path(text);
  if (text.empty() || path.is_absolute() || path != path.lexically_normal() ||
      std::find(path.begin(), path.end(), std::filesystem::path("..")) !=
          path.end()) {
    throw std::invalid_argument(std::string(label) +
                                " must be a normalized root-local path");
  }
  return path;
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

struct FamilySource final {
  std::string family_id;
  std::string model_asset_id;
  std::filesystem::path catalog_source;
  std::filesystem::path urdf_source;
  std::filesystem::path srdf_source;
  std::filesystem::path resource_root;
  std::filesystem::path prepared_root;
};

[[nodiscard]] std::vector<FamilySource> load_families(const Options &options) {
  const auto root = deployment::parse_strict_json_model_yaml(read_file(
      options.workspace_root / "manyforge_deployment/resources/maintained/"
                               "maintained_deployment_sources.v1.json"));
  const auto &rows = require_array(
      require_member(root.root(), "model_families", "maintained description"),
      "maintained description.model_families");
  std::vector<FamilySource> result;
  for (const auto &row : rows) {
    const std::string family_id =
        require_text(require_member(row, "model_family_id", "model family"),
                     "model family.model_family_id");
    const auto prepared = options.prepared_roots.find(family_id);
    if (prepared == options.prepared_roots.end() ||
        !prepared->second.is_absolute() ||
        std::filesystem::is_symlink(prepared->second) ||
        !std::filesystem::is_directory(prepared->second)) {
      throw std::invalid_argument("model family has no exact prepared root: " +
                                  family_id);
    }
    const auto &source = require_member(row, "source", "model family");
    const auto rooted = [&](std::string_view field) {
      return options.workspace_root /
             require_relative_path(
                 require_text(
                     require_member(source, field, "model family.source"),
                     std::string("model family.source.") + std::string(field)),
                 std::string("model family.source.") + std::string(field));
    };
    result.push_back(FamilySource{
        family_id,
        require_text(require_member(row, "model_asset_id", "model family"),
                     "model family.model_asset_id"),
        options.workspace_root /
            require_relative_path(
                require_text(
                    require_member(row, "catalog_source", "model family"),
                    "model family.catalog_source"),
                "model family.catalog_source"),
        rooted("urdf_path"),
        rooted("srdf_path"),
        rooted("resource_root"),
        prepared->second.lexically_normal(),
    });
  }
  if (result.size() != options.prepared_roots.size()) {
    throw std::invalid_argument(
        "prepared family set differs from the maintained description");
  }
  return result;
}

class TemporaryPackage final {
public:
  explicit TemporaryPackage(
      const std::optional<std::filesystem::path> &requested_root) {
    if (requested_root.has_value()) {
      path_ = std::filesystem::canonical(*requested_root);
      if (!std::filesystem::is_directory(path_) ||
          std::filesystem::is_symlink(path_) ||
          !std::filesystem::is_empty(path_)) {
        throw std::invalid_argument(
            "requested N6-a3 package root must be one empty plain directory");
      }
      remove_on_destruction_ = false;
      return;
    }
    std::array<char, 64U> pattern{};
    const std::string seed = "/tmp/manyforge-n6-a3-XXXXXX";
    std::copy(seed.begin(), seed.end(), pattern.begin());
    char *created = ::mkdtemp(pattern.data());
    if (created == nullptr) {
      throw std::runtime_error("cannot create N6-a3 temporary package");
    }
    path_ = created;
  }

  ~TemporaryPackage() {
    if (!remove_on_destruction_) {
      return;
    }
    std::error_code ignored;
    (void)std::filesystem::remove_all(path_, ignored);
  }
  TemporaryPackage(const TemporaryPackage &) = delete;
  TemporaryPackage &operator=(const TemporaryPackage &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
  bool remove_on_destruction_{true};
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
source_files(const std::filesystem::path &directory,
             std::size_t expected_count) {
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
  if (result.size() != expected_count) {
    throw std::invalid_argument(
        "maintained catalog directory has the wrong exact source count: " +
        directory.string());
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
    const Scenario &scenario, const core::CellGraphDTO &graph,
    const deployment::VerifiedRetainedModelMap &model_map) {
  if (graph.controllable_components.empty()) {
    throw std::logic_error(
        "maintained gripper proof requires a controllable component");
  }
  std::set<core::ControllableComponentId> found_components;
  for (const auto &binding : model_map.control_bindings()) {
    if (binding.command_target_id.value() != "gripper") {
      continue;
    }
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
    const core::ResourceId expected_local_id(
        binding.controllable_component_id.value() +
        ".gripper.logical_position");
    for (const auto &resource : binding.resources) {
      if (resource.resource_role_id.value() != "fingers") {
        continue;
      }
      found_components.insert(binding.controllable_component_id);
      if (scenario.family_id == std::string_view("ur5_roboplan")) {
        if (resource.resource_id != expected_local_id ||
            !resource.coordinates.empty() ||
            resource.component_local_coordinate_kind !=
                core::CoordinateKind::kLinear) {
          throw std::logic_error(
              "maintained UR5 logical gripper lost its exact linear, "
              "no-source component-local resource");
        }
      } else if (resource.coordinates.empty() ||
                 resource.component_local_coordinate_kind.has_value() ||
                 resource.coordinates.front().source_joint_name.empty() ||
                 resource.coordinates.front().compiled_joint_name.empty()) {
        throw std::logic_error(
            "maintained physical gripper lost its exact model-backed "
            "coordinate resource");
      }
    }
  }
  if (found_components.size() != graph.controllable_components.size()) {
    throw std::logic_error(
        "maintained deployment lacks one retained gripper resource proof per "
        "component");
  }
}

[[nodiscard]] std::map<std::string, ScenarioProduct, std::less<>>
materialize_scenarios(const std::filesystem::path &root,
                      const QualifiedCatalogClosure &catalog,
                      const ExactCompilerInputSource &input_source,
                      const std::filesystem::path &package_root,
                      std::span<const Scenario> scenarios = kScenarios,
                      const deployment::CanonicalValue *deployment_override =
                          nullptr,
                      const bool publish_canonical_empty = true,
                      MemoryPreparationStores *memory_stores = nullptr) {
  if (deployment_override != nullptr && scenarios.size() != 1U) {
    throw std::invalid_argument(
        "one candidate Deployment override requires one Scenario");
  }
  const std::string overlay_bytes = read_file(
      root / "manyforge_deployment/resources/maintained/site_overlays/"
             "manyforge_local_examples.site_overlay.json");
  std::unique_ptr<deployment::FilesystemBuildManifestRepository>
      filesystem_manifests;
  std::unique_ptr<deployment::FilesystemCatalogArtifactStore>
      filesystem_catalogs;
  std::unique_ptr<deployment::FilesystemRawArtifactStore> filesystem_raw;
  std::unique_ptr<deployment::FilesystemRetainedBundleRepository>
      filesystem_bundles;
  if (memory_stores == nullptr) {
    filesystem_manifests =
        std::make_unique<deployment::FilesystemBuildManifestRepository>(
            package_root);
    filesystem_catalogs =
        std::make_unique<deployment::FilesystemCatalogArtifactStore>(
            package_root);
    filesystem_raw =
        std::make_unique<deployment::FilesystemRawArtifactStore>(package_root);
    filesystem_bundles =
        std::make_unique<deployment::FilesystemRetainedBundleRepository>(
            package_root);
  }
  deployment::IBuildManifestRepository &manifest_repository =
      memory_stores != nullptr
          ? static_cast<deployment::IBuildManifestRepository &>(
                memory_stores->manifests)
          : static_cast<deployment::IBuildManifestRepository &>(
                *filesystem_manifests);
  deployment::ICatalogArtifactStore &catalog_store =
      memory_stores != nullptr
          ? static_cast<deployment::ICatalogArtifactStore &>(
                memory_stores->catalogs)
          : static_cast<deployment::ICatalogArtifactStore &>(
                *filesystem_catalogs);
  deployment::IRawArtifactStore &raw_store =
      memory_stores != nullptr
          ? static_cast<deployment::IRawArtifactStore &>(memory_stores->raw)
          : static_cast<deployment::IRawArtifactStore &>(*filesystem_raw);
  deployment::IRetainedBundleRepository &bundle_repository =
      memory_stores != nullptr
          ? static_cast<deployment::IRetainedBundleRepository &>(
                memory_stores->bundles)
          : static_cast<deployment::IRetainedBundleRepository &>(
                *filesystem_bundles);
  std::map<std::string, ScenarioProduct, std::less<>> result;

  for (const auto &scenario : scenarios) {
    const std::string expected_runtime_profile_id =
        scenario.ros2_control_execution
            ? "manyforge.ros2_control_execution_runtime"
            : "manyforge.native_execution_runtime";
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
            deployment_override != nullptr
                ? deployment::canonical_json(*deployment_override)
                : read_file(root / scenario.source)),
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
            catalog_store, raw_store, bundle_repository);
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
      if (memory_stores != nullptr) {
        const auto &artifact = product.artifact_refs.front();
        const auto bytes = memory_stores->raw.get_verified(
            artifact.content_addressed_store_key, artifact.content_digest,
            artifact.byte_size);
        if (bytes.has_value()) {
          model_map = deployment::detail::
              RetainedModelMapVerificationAuthority::verify(*bundle, map_query,
                                                            *bytes);
        }
      } else {
        const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
            package_root);
        model_map = map_source.get_verified(map_query);
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
          empty_bundle->value().compiled_products.runtime_unit_products !=
              bundle->value().compiled_products.runtime_unit_products) {
        throw std::logic_error(
            "cell-scoped canonical predecessor does not retain the "
            "candidate's exact execution host product");
      }
      if (bundle->value().cell_graph.runtime_units.size() == 1U &&
          bundle->value().cell_graph.runtime_units.front()
                  .runtime_profile_id.value() ==
              deployment::native_execution_runtime_contract::
                  kRuntimeProfileId) {
        const auto &empty_host =
            empty_bundle->value().cell_graph.runtime_units.front();
        static_cast<void>(
            deployment::native_execution_runtime_contract::
                require_configuration(*empty_bundle,
                                     empty_host.runtime_unit_id));
      }
      admission_filename =
          deployment::cell_scoped_canonical_empty_admission_filename(
              verified.cell_id());
      canonical_empty_bundle = empty_bundle;
      if (memory_stores == nullptr) {
        const auto admission =
            deployment::detail::publish_cell_scoped_canonical_empty_admission(
                package_root, admission_filename, *empty_bundle);
        if (admission.cell_id() != verified.cell_id() ||
            admission.retained_bundle_id() !=
                empty_bundle->retained_bundle_id()) {
          throw std::logic_error(
              "cell-scoped canonical predecessor did not reload exactly");
        }
      }
    }
    if (model_map.has_value()) {
      verify_gripper_resource_semantics(scenario, verified.graph().graph(),
                                        *model_map);
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

struct StoredProgramBinding final {
  std::string deployment_source;
  core::ProgramBindingId program_binding_id;
  core::ProgramId program_id;
  core::ProgramRevision program_revision;
};

void verify_resolved_program_binding(
    const deployment::ProgramBinding &binding,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map) {
  if (binding.value().component_role_bindings.empty() ||
      binding.value().target_role_bindings.empty()) {
    throw std::logic_error(
        "maintained Program Binding has an empty role closure");
  }
  for (const auto &component : binding.value().component_role_bindings) {
    if (component.model_role_bindings.empty()) {
      throw std::logic_error(
          "maintained Program Binding loses its model-role resolution");
    }
  }
  for (const auto &target : binding.value().target_role_bindings) {
    if (target.frame_role_bindings.empty() ||
        target.resource_role_bindings.empty() ||
        target.collision_body_role_bindings.empty() ||
        target.capability_bindings.empty()) {
      throw std::logic_error(
          "maintained Program Binding loses a target resolution family");
    }
    for (const auto &resource : target.resource_role_bindings) {
      if (resource.resource_ids.empty() ||
          resource.resource_ids.size() !=
              resource.compiled_resource_names.size() ||
          std::any_of(resource.compiled_resource_names.begin(),
                      resource.compiled_resource_names.end(),
                      [](const auto &value) { return value.empty(); })) {
        throw std::logic_error(
            "maintained Program Binding has an incomplete resource mapping");
      }
    }
    for (const auto &frame : target.frame_role_bindings) {
      if (frame.compiled_frame_name.empty()) {
        throw std::logic_error(
            "maintained Program Binding has an empty compiled frame");
      }
    }
    for (const auto &body : target.collision_body_role_bindings) {
      if (body.compiled_collision_body_names.empty()) {
        throw std::logic_error(
            "maintained Program Binding has an empty collision-body mapping");
      }
    }

    bool resolved_control_coordinates = false;
    for (const auto &capability : target.capability_bindings) {
      const auto resolved = deployment::resolve_runtime_program_target(
          binding, target.target_role_id, capability.capability_id, bundle,
          model_map);
      if (resolved.program_binding_id() != binding.program_binding_id() ||
          resolved.component().controllable_component_id !=
              target.controllable_component_id ||
          resolved.model_target().command_target_id !=
              target.command_target_id ||
          resolved.capability() != capability) {
        throw std::logic_error(
            "runtime Program resolver changed an exact retained selection");
      }
      if (capability.control_binding.has_value()) {
        if (!resolved.control_binding().has_value() ||
            !resolved.control_domain().has_value() ||
            !resolved.desired_control_configuration().has_value() ||
            !resolved.desired_route().has_value() ||
            !resolved.coordinate_map().has_value()) {
          throw std::logic_error(
              "control-backed Program capability has no complete runtime path");
        }
        for (const auto &resource : resolved.coordinate_map()->resources) {
          if (std::any_of(resource.coordinates.begin(),
                          resource.coordinates.end(), [](const auto &row) {
                            return row.source_joint_name.empty() ||
                                   row.compiled_joint_name.empty();
                          })) {
            throw std::logic_error(
                "runtime Program path has an incomplete source-joint map");
          }
        }
        resolved_control_coordinates = true;
      } else if (resolved.control_binding().has_value() ||
                 resolved.control_domain().has_value() ||
                 resolved.desired_route().has_value() ||
                 resolved.coordinate_map().has_value()) {
        throw std::logic_error(
            "provider-only Program capability acquired a control path");
      }
      if (capability.capability_provider_binding.has_value() !=
          resolved.capability_provider_binding().has_value()) {
        throw std::logic_error(
            "Program capability provider resolution changed disposition");
      }
    }
    if (!resolved_control_coordinates) {
      throw std::logic_error(
          "maintained Program target has no exact control-coordinate path");
    }
  }
}

struct MaintainedScenarioInput final {
  std::string scenario_id;
  std::filesystem::path source;
  deployment::CanonicalValue deployment;
  deployment::CanonicalValue program;
  core::ProgramRoleAssignmentDTO assignment;
};

[[nodiscard]] deployment::CanonicalValue program_from_authoring_draft(
    const deployment::CanonicalValue &draft) {
  const auto &draft_object = require_object(draft, "Scenario.program_draft");
  const auto &forest = require_array(
      require_member(draft, "forest", "Scenario.program_draft"),
      "Scenario.program_draft.forest");
  if (forest.size() != 1U) {
    throw std::invalid_argument(
        "maintained Scenario Program draft must contain one tree");
  }
  deployment::CanonicalValue::Object program;
  for (const auto &[name, value] : draft_object) {
    if (name != "schema_version" && name != "forest" && name != "positions") {
      program.emplace_back(name, value);
    }
  }
  program.emplace_back("program_revision", deployment::CanonicalValue(
                                               "sha256:" + std::string(64U, '0')));
  program.emplace_back("tree", forest.front());
  const deployment::CanonicalValue placeholder(program);
  const auto revision = deployment::detail::compute_content_identity(
      placeholder, deployment::detail::ContentIdentitySpec{
                  .identity_domain = "program-revision",
                  .api_version = std::string(core::kProgramApiVersion),
                  .own_field = "program_revision",
              });
  deployment::CanonicalValue::Object identified;
  identified.reserve(program.size());
  for (const auto &[name, value] : program) {
    if (name == "program_revision") {
      identified.emplace_back(name, deployment::CanonicalValue(revision));
    } else {
      identified.emplace_back(name, value);
    }
  }
  return deployment::CanonicalValue(std::move(identified));
}

// The closed maintained Scenario set: the thirteen described Program/Scenario
// assignments plus the two generated UI6-I dual-robot fixtures. Other folders
// under examples/ (for example the editable workbench Scenario) are ordinary
// user projects and are never maintained qualification inputs.
constexpr std::array<std::string_view, 15U> kMaintainedScenarioIds{{
    "isaac-lite6-pick-place",
    "isaac-lite6-trocar-foundationpose",
    "isaac-ur10e-light-scene",
    "isaac-ur10e-light-scene-assistant",
    "isaac-ur10e-light-scene-collision",
    "lite6_pick_and_place",
    "ur10e_assistant_scene_authoring",
    "ur10e_pick_and_place",
    "ur5_pick_and_place",
    "ur5_pick_and_place_minimal",
    "ur5_semantic_shape_pick",
    "ur5_vision_guided_bin_pick",
    "ur5_vision_guided_pose_pick",
    "ur5e_lite6_wall_grouped",
    "ur5e_lite6_wall_isolation",
}};

[[nodiscard]] std::vector<MaintainedScenarioInput>
load_maintained_scenario_inputs(const std::filesystem::path &root) {
  std::vector<MaintainedScenarioInput> result;
  const auto examples = root / "examples";
  for (const auto scenario_id : kMaintainedScenarioIds) {
    const std::filesystem::directory_entry entry(
        examples / (std::string(scenario_id) + ".scenario"));
    if (!entry.is_directory() || entry.is_symlink()) {
      throw std::invalid_argument(
          "maintained Scenario project is missing: " + entry.path().string());
    }
    const auto manifest_path = entry.path() / "scenario.yaml";
    const auto manifest = deployment::parse_strict_json_model_yaml(
        read_file(manifest_path));
    const auto &document = manifest.root();
    if (require_text(require_member(document, "api_version", "Scenario project"),
                     "Scenario project.api_version") !=
        "manyforge.scenario.project.v2") {
      throw std::invalid_argument(
          "maintained Scenario project has the wrong API version");
    }
    const auto &members = require_member(document, "members", "Scenario project");
    const auto read_member = [&](const std::string_view name) {
      const auto path = entry.path() / name;
      const auto bytes = read_file(path);
      const auto expected = require_text(
          require_member(members, name, "Scenario project members"),
          "Scenario project member digest");
      if (deployment::sha256_digest(bytes) != expected) {
        throw std::invalid_argument(
            "maintained Scenario project member differs from its manifest: " +
            path.string());
      }
      return deployment::parse_strict_json_model_yaml(bytes).root();
    };
    const auto deployment_value = read_member("deployment.yaml");
    const auto program_draft = read_member("program.yaml");
    const auto assignment_value = read_member("assignment.yaml");
    core::ProgramRoleAssignmentDTO assignment;
    for (const auto &row : require_array(
             require_member(assignment_value, "component_roles", "assignment"),
             "assignment.component_roles")) {
      assignment.component_roles.push_back(
          {core::ComponentRoleId(require_text(
               require_member(row, "component_role_id", "component role"),
               "component_role_id")),
           core::ControllableComponentId(require_text(
               require_member(row, "controllable_component_id", "component role"),
               "controllable_component_id"))});
    }
    for (const auto &row : require_array(
             require_member(assignment_value, "target_roles", "assignment"),
             "assignment.target_roles")) {
      assignment.target_roles.push_back(
          {core::TargetRoleId(require_text(
               require_member(row, "target_role_id", "target role"),
               "target_role_id")),
           core::CommandTargetId(require_text(
               require_member(row, "command_target_id", "target role"),
               "command_target_id"))});
    }
    const auto declared_id =
        require_text(require_member(document, "scenario_id", "Scenario"),
                     "Scenario.scenario_id");
    if (declared_id != scenario_id) {
      throw std::invalid_argument(
          "maintained Scenario project declares another Scenario ID: " +
          manifest_path.string());
    }
    result.push_back(MaintainedScenarioInput{
        declared_id,
        entry.path(),
        deployment_value,
        program_from_authoring_draft(program_draft),
        std::move(assignment),
    });
  }
  std::ranges::sort(result, {}, &MaintainedScenarioInput::scenario_id);
  return result;
}

[[nodiscard]] std::vector<StoredProgramBinding> verify_program_bindings(
    const std::filesystem::path &root,
    const std::map<std::string, ScenarioProduct, std::less<>> &products,
    const std::filesystem::path &package_root) {
  const auto rows = load_maintained_scenario_inputs(root);
  if (rows.size() != 15U || products.size() != 13U) {
    throw std::invalid_argument(
        "maintained Program/deployment assignment set has the wrong size");
  }

  deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
  std::vector<StoredProgramBinding> stored;
  stored.reserve(rows.size());
  std::set<std::string, std::less<>> binding_ids;
  std::set<std::string, std::less<>> program_revisions;
  std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
      program_bundles;
  {
    deployment::FilesystemProgramRepository program_repository(package_root);
    deployment::FilesystemProgramBindingRepository repository(package_root);
    for (const auto &row : rows) {
      const auto deployment_id = require_text(
          require_member(row.deployment, "deployment_artifact_id", "Deployment"),
          "Deployment.deployment_artifact_id");
      const auto product = std::find_if(
          products.begin(), products.end(), [&](const auto &candidate) {
            return candidate.second.deployment_artifact_id.value() == deployment_id;
          });
      if (product == products.end()) {
        throw std::invalid_argument(
            "Scenario has no exact materialized Deployment");
      }
      const auto &context = product->second;
      if (!context.model_map.has_value()) {
        throw std::logic_error(
            "maintained Program binding requires an exact model map");
      }
      const std::string canonical_program =
          deployment::canonical_json(row.program);
      const auto contract = deployment::verify_program_binding_contract(
          canonical_program, *context.bundle);
      (void)program_repository.put_if_absent(contract, *context.bundle);

      const auto binding = deployment::compile_program_binding(
          *contract, row.assignment, *context.bundle, *context.model_map,
          catalog_store);
      verify_resolved_program_binding(*binding, *context.bundle,
                                      *context.model_map);
      if (!repository.put_if_absent(binding, *context.bundle,
                                    *context.model_map)) {
        throw std::logic_error(
            "maintained Program Binding identity is unexpectedly duplicated");
      }
      binding_ids.insert(binding->program_binding_id().value());
      program_revisions.insert(binding->value().program_revision.value());
      program_bundles[binding->value().program_revision.value()].insert(
          binding->value().retained_bundle_id.value());
      stored.push_back(StoredProgramBinding{
          product->first, binding->program_binding_id(),
          binding->value().program_id, binding->value().program_revision});
    }
    if (repository.size() != 15U) {
      throw std::logic_error(
          "durable maintained Program-Binding repository has the wrong size");
    }
    if (program_repository.size() != program_revisions.size()) {
      throw std::logic_error(
          "durable maintained Program repository has the wrong size");
    }
  }

  deployment::FilesystemProgramRepository program_reopened(package_root);
  deployment::FilesystemProgramBindingRepository reopened(package_root);
  for (const auto &record : stored) {
    const auto product = products.find(record.deployment_source);
    if (product == products.end()) {
      throw std::logic_error(
          "stored Program Binding lost its deployment context");
    }
    if (!product->second.model_map.has_value()) {
      throw std::logic_error(
          "maintained Program Binding lost its exact model map");
    }
    const auto binding = reopened.get_verified(record.program_binding_id,
                                               *product->second.bundle,
                                               *product->second.model_map);
    if (binding == nullptr ||
        binding->value().program_id != record.program_id ||
        binding->value().program_revision != record.program_revision) {
      throw std::logic_error(
          "durable maintained Program Binding did not reload exactly");
    }
    const auto program = program_reopened.get_verified(
        record.program_id, record.program_revision, *product->second.bundle);
    if (program == nullptr ||
        program->value().program_id != record.program_id ||
        program->value().program_revision != record.program_revision) {
      throw std::logic_error(
          "durable maintained Program did not reload exactly");
    }
    verify_resolved_program_binding(*binding, *product->second.bundle,
                                    *product->second.model_map);
  }
  // The obligation is that the same verified Program is reusable across two
  // retained products. Ordinary source/catalog edits must not require another
  // hard-coded aggregate content hash in this construction check.
  const auto reused_bundle_count = [&](const std::string_view program_id) {
    const auto record =
        std::find_if(stored.begin(), stored.end(), [&](const auto &candidate) {
          return candidate.program_id.value() == program_id;
        });
    return record == stored.end()
               ? 0U
               : program_bundles.at(record->program_revision.value()).size();
  };
  if (binding_ids.size() != 15U || program_revisions.size() != 13U ||
      reopened.size() != 15U || program_reopened.size() != 13U ||
      reused_bundle_count("pick_and_place_ur10e_robotiq_demo") != 2U ||
      reused_bundle_count("isaac_manipulator_scene_light") != 2U) {
    throw std::logic_error(
        "maintained Program-Binding set loses its thirteen-to-fifteen closure");
  }
  return stored;
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
                const std::vector<FamilySource> &families,
                std::vector<deployment::detail::ActivationPreparedModel>
                    prepared_models = {}) {
  // A runtime-only Deployment has no model inputs. Component/model cardinality
  // is checked by the authored compiler; the two input forms cannot be mixed.
  if (!families.empty() && !prepared_models.empty()) {
    throw std::invalid_argument(
        "catalog qualification cannot mix model input forms");
  }
  const auto catalog_root =
      root / "manyforge_deployment/resources/maintained/catalog";
  QualifiedCatalogClosure result;

  for (const auto &path :
       source_files(catalog_root / "capability_definition", 42U)) {
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
  if (target_capabilities.size() != 21U) {
    throw std::logic_error(
        "maintained component closure has the wrong target-capability count");
  }
  // Five native component types plus the fixed-scale ROS type of every
  // family with an Isaac or offline mock controller profile (Q63-Q68).
  constexpr std::size_t kMaintainedComponentTypeCount = 9U;
  for (const auto &path :
       source_files(catalog_root / "component_type",
                    kMaintainedComponentTypeCount)) {
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
       source_files(catalog_root / "control_interface_contract", 4U)) {
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

  for (const auto &path :
       source_files(catalog_root / "controller_topology", 5U)) {
    const std::string bytes = read_file(path);
    const auto reference =
        make_reference<core::ControllerTopologyCatalogRecordRefDTO>(bytes);
    const bool ros2_control =
        reference.record_id.value().starts_with("manyforge.ros2_control_");
    const bool arm =
        reference.record_id.value().find("_arm_") != std::string::npos;
    const std::string expected_contract_id =
        std::string("manyforge.") +
        (ros2_control ? "ros2_control_" : "native_") +
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

  bool found_lite6_arm_lifecycle_manifest = false;
  bool found_lite6_gripper_lifecycle_manifest = false;
  bool found_native_control_lifecycle_manifest = false;
  bool found_ros2_control_lifecycle_manifest = false;
  for (const auto &path :
       source_files(catalog_root / "provider_manifest", 8U)) {
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
        "manyforge.provider.native_control") {
      if (found_native_control_lifecycle_manifest ||
          !manifest.value().lifecycle_plan_catalog.has_value() ||
          manifest.value().lifecycle_plan_catalog->catalog_api_version.value() !=
              core::kBrandLifecyclePlanCatalogApiVersion ||
          manifest.value().provider_contracts.size() != 2U ||
          !has_contract_role("software_hard_stop") ||
          !has_contract_role("stop_contract")) {
        throw std::logic_error(
            "maintained generic native lifecycle provider manifest is not exact");
      }
      found_native_control_lifecycle_manifest = true;
    } else if (manifest.value().provider_manifest_id.value() == "lite6_arm_provider") {
      if (found_lite6_arm_lifecycle_manifest ||
          !manifest.value().lifecycle_plan_catalog.has_value() ||
          manifest.value()
                  .lifecycle_plan_catalog->catalog_api_version.value() !=
              core::kBrandLifecyclePlanCatalogApiVersion ||
          manifest.value().offered_capabilities.size() != 14U ||
          manifest.value().provider_contracts.size() != 2U ||
          !has_contract_role("software_hard_stop") ||
          !has_contract_role("stop_contract")) {
        throw std::logic_error(
            "maintained Lite6 arm lifecycle provider manifest is not exact");
      }
      found_lite6_arm_lifecycle_manifest = true;
    } else if (manifest.value().provider_manifest_id.value() ==
               "lite6_gripper_provider") {
      if (found_lite6_gripper_lifecycle_manifest ||
          !manifest.value().lifecycle_plan_catalog.has_value() ||
          manifest.value()
                  .lifecycle_plan_catalog->catalog_api_version.value() !=
              core::kBrandLifecyclePlanCatalogApiVersion ||
          manifest.value().offered_capabilities.size() != 7U ||
          manifest.value().provider_contracts.size() != 1U ||
          !has_contract_role("software_hard_stop")) {
        throw std::logic_error("maintained Lite6 gripper lifecycle provider "
                               "manifest is not exact");
      }
      found_lite6_gripper_lifecycle_manifest = true;
    } else if (manifest.value().provider_manifest_id.value() ==
               "manyforge.provider.ros2_control") {
      if (found_ros2_control_lifecycle_manifest ||
          !manifest.value().lifecycle_plan_catalog.has_value() ||
          manifest.value()
                  .lifecycle_plan_catalog->catalog_api_version.value() !=
              core::kBrandLifecyclePlanCatalogApiVersion ||
          manifest.value().offered_capabilities.size() != 25U ||
          manifest.value().provider_contracts.size() != 2U ||
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
  if (!found_native_control_lifecycle_manifest ||
      !found_lite6_arm_lifecycle_manifest ||
      !found_lite6_gripper_lifecycle_manifest ||
      !found_ros2_control_lifecycle_manifest) {
    throw std::logic_error(
        "maintained lifecycle provider manifest closure is missing");
  }

  for (const auto &path : source_files(catalog_root / "runtime_profile", 2U)) {
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
       source_files(catalog_root / "stop_evidence_policy", 2U)) {
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

  const std::filesystem::path command_path =
      root / "manyforge_deployment/resources/catalog/"
             "gripper.command_position.v1.yaml";
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

  result.contracts = contracts;
  for (const auto &family : families) {
    const std::string bytes = read_file(family.catalog_source);
    const auto reference =
        make_reference<core::ModelAssetCatalogRecordRefDTO>(bytes);
    const auto qualified = deployment::resolve_and_qualify_model_asset(
        reference, reference.content_addressed_store_key, bytes);
    input_source.add(reference.content_addressed_store_key,
                     reference.content_digest, bytes);
    insert_reference(result, reference);

    const std::string manifest_bytes =
        read_file(family.prepared_root / "preparation-manifest.json");
    const auto manifest =
        deployment::parse_strict_json_model_yaml(manifest_bytes).root();
    if (deployment::canonical_json(manifest) != manifest_bytes) {
      throw std::invalid_argument(
          "structural preparation receipt is not canonical");
    }
    const auto &prepared =
        require_member(manifest, "prepared_source", "preparation receipt");
    const std::string &urdf_uri = require_text(
        require_member(prepared, "urdf_logical_uri", "prepared source"),
        "prepared source.urdf_logical_uri");
    const std::string &srdf_uri = require_text(
        require_member(prepared, "srdf_logical_uri", "prepared source"),
        "prepared source.srdf_logical_uri");
    std::map<std::string, std::filesystem::path, std::less<>> paths{
        {urdf_uri, family.prepared_root / "model.urdf"},
        {srdf_uri, family.prepared_root / "model.srdf"},
    };
    for (const auto &rewrite :
         require_array(require_member(manifest, "resource_rewrites",
                                      "preparation receipt"),
                       "preparation receipt.resource_rewrites")) {
      const std::string &uri =
          require_text(require_member(rewrite, "asset_uri", "resource rewrite"),
                       "resource rewrite.asset_uri");
      const auto relative = require_relative_path(
          require_text(
              require_member(rewrite, "relative_path", "resource rewrite"),
              "resource rewrite.relative_path"),
          "resource rewrite.relative_path");
      if (!paths.emplace(uri, family.resource_root / relative).second) {
        throw std::invalid_argument(
            "structural preparation repeats an artifact URI");
      }
    }

    std::vector<deployment::ResolvedModelSourceArtifactCandidate> candidates;
    const auto &artifacts =
        require_array(require_member(prepared, "artifacts", "prepared source"),
                      "prepared source.artifacts");
    candidates.reserve(artifacts.size());
    for (const auto &artifact : artifacts) {
      const std::string &uri = require_text(
          require_member(artifact, "logical_uri", "prepared artifact"),
          "prepared artifact.logical_uri");
      const std::string &digest = require_text(
          require_member(artifact, "content_digest", "prepared artifact"),
          "prepared artifact.content_digest");
      const auto path = paths.find(uri);
      if (path == paths.end()) {
        throw std::invalid_argument(
            "prepared artifact has no exact source path");
      }
      const std::string artifact_bytes = read_file(path->second);
      if (artifact_bytes.size() !=
              require_size(
                  require_member(artifact, "byte_size", "prepared artifact"),
                  "prepared artifact.byte_size") ||
          deployment::sha256_digest(artifact_bytes) != digest) {
        throw std::invalid_argument(
            "prepared artifact differs from its receipt");
      }
      const core::ContentAddressedStoreKey key("sha256/" + digest.substr(7U));
      input_source.add(key, core::ContentDigest(digest), artifact_bytes);
      candidates.push_back({uri, key, artifact_bytes});
    }
    if (candidates.size() != paths.size()) {
      throw std::invalid_argument(
          "prepared source path/declaration sets differ");
    }
    auto verified = deployment::verify_model_source_closure(
        qualified, std::move(candidates));
    auto preparation =
        deployment::verify_build_qualified_structural_model_preparation(
            manifest_bytes, read_file(family.urdf_source),
            read_file(family.srdf_source), verified);
    result.model_assets.push_back(std::move(verified));
    result.structural_preparations.push_back(std::move(preparation));
  }
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
      result.model_assets.size() != result.structural_preparations.size() ||
      result.runtime_profiles.size() != 2U) {
    throw std::logic_error("maintained exact catalog closure is incomplete");
  }
  return result;
}

void verify_scenarios(const std::filesystem::path &root,
                      const QualifiedCatalogClosure &catalog) {
  const std::string overlay_bytes = read_file(
      root / "manyforge_deployment/resources/maintained/site_overlays/"
             "manyforge_local_examples.site_overlay.json");
  std::map<std::string, std::pair<std::string, std::string>, std::less<>>
      family_identities;
  std::map<std::string, std::string, std::less<>> family_models;
  for (const auto &scenario : kScenarios) {
    const std::string bytes = read_file(root / scenario.source);
    const std::string expected_runtime_profile_id =
        scenario.ros2_control_execution
            ? "manyforge.ros2_control_execution_runtime"
            : "manyforge.native_execution_runtime";
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
        deployment::parse_strict_json_model_yaml(bytes), envelope_profiles);
    if (verified.assistant_settings().enabled() != scenario.assistant_enabled) {
      throw std::invalid_argument(
          "maintained scenario has the wrong assistant disposition: " +
          std::string(scenario.source));
    }
    const auto &applicability = verified.applicability_inputs().value();
    if (scenario.assistant_enabled) {
      if (!applicability.site_overlay_ref.has_value()) {
        throw std::invalid_argument(
            "assistant scenario has no exact site-overlay reference: " +
            std::string(scenario.source));
      }
      const auto projection =
          deployment::resolve_and_verify_site_overlay_artifact_projection(
              *applicability.site_overlay_ref, overlay_bytes);
      const auto selected = deployment::select_site_overlay_for_deployment(
          applicability.site_overlay_ref, applicability.external_resources,
          &projection);
      if (!selected.has_selection()) {
        throw std::logic_error(
            "assistant scenario did not retain its selected site overlay");
      }
    } else {
      const auto selected = deployment::select_site_overlay_for_deployment(
          applicability.site_overlay_ref, applicability.external_resources,
          nullptr);
      if (selected.has_selection()) {
        throw std::logic_error(
            "non-assistant scenario unexpectedly selected a site overlay");
      }
    }
    std::set<core::ProviderManifestId> external_manifest_ids;
    for (const auto &resource : applicability.external_resources) {
      external_manifest_ids.insert(resource.provider_manifest_id);
    }
    // Every native family uses the one generic lifecycle provider (its four
    // lifecycle capability definitions); the Lite6 vendor providers are no
    // longer selected by native scenarios.
    const std::size_t expected_catalog_count =
        (scenario.ros2_control_execution ? 51U : 52U) +
        2U * external_manifest_ids.size();
    if (verified.catalog_records().size() != expected_catalog_count) {
      throw std::invalid_argument(
          "maintained scenario has the wrong exact catalog closure: " +
          std::string(scenario.source) + " (expected " +
          std::to_string(expected_catalog_count) + ", observed " +
          std::to_string(verified.catalog_records().size()) + ")");
    }
    for (const auto &reference : verified.catalog_records()) {
      const auto expected = catalog.records.find(catalog_key(reference));
      if (expected == catalog.records.end() || expected->second != reference) {
        throw std::invalid_argument(
            "maintained scenario selects an unqualified catalog referent: " +
            std::string(scenario.source) + " (kind " +
            std::to_string(static_cast<std::uint8_t>(
                core::catalog_record_kind(reference))) +
            ", id " + core::catalog_record_id_value(reference) + ", revision " +
            core::catalog_record_revision_value(reference) + ")");
      }
    }

    const auto &graph = verified.graph().graph();
    if (graph.controllable_components.size() != 1U ||
        graph.model_instances.size() != 1U ||
        graph.runtime_units.size() != 1U ||
        graph.control_bindings.size() !=
            (scenario.ros2_control_execution ? 10U : 11U) ||
        applicability.capability_provider_bindings.size() != 26U) {
      throw std::invalid_argument("maintained scenario has an incomplete "
                                  "component/runtime/capability closure: " +
                                  std::string(scenario.source));
    }
    const auto &runtime = graph.runtime_units.front();
    if (runtime.runtime_profile_id.value() != expected_runtime_profile_id) {
      throw std::invalid_argument(
          "maintained scenario selects the wrong exact runtime product: " +
          std::string(scenario.source));
    }
    core::JsonValueDTO::Array initial_positions;
    if (scenario.ros2_control_execution &&
        scenario.isaac_profile == std::string_view("ur10e")) {
      initial_positions = {
          core::JsonValueDTO{2.63422544},   core::JsonValueDTO{-0.875980752},
          core::JsonValueDTO{1.551074106},  core::JsonValueDTO{-2.29231544},
          core::JsonValueDTO{-1.560498884}, core::JsonValueDTO{-2.038544566}};
    } else if (scenario.family_id == std::string_view("lite6_lite_gripper")) {
      initial_positions = {core::JsonValueDTO{0.0},  core::JsonValueDTO{0.0},
                           core::JsonValueDTO{1.57}, core::JsonValueDTO{0.0},
                           core::JsonValueDTO{0.0},  core::JsonValueDTO{0.0}};
    } else {
      initial_positions = {core::JsonValueDTO{0.0},   core::JsonValueDTO{-1.57},
                           core::JsonValueDTO{1.57},  core::JsonValueDTO{-1.57},
                           core::JsonValueDTO{-1.57}, core::JsonValueDTO{0.0}};
    }
    const bool mesh_heavy_isaac_runtime =
        scenario.source ==
            std::string_view("examples/isaac_manipulator_scene_light_"
                             "assistant.deployment.yaml") ||
        scenario.source ==
            std::string_view("examples/isaac_manipulator_scene_light_"
                             "ros2_control.deployment.yaml") ||
        scenario.source ==
            std::string_view("examples/isaac_manipulator_scene_light_"
                             "ros2_control_collision.deployment.yaml");
    const bool assistant_isaac_runtime =
        scenario.source ==
        std::string_view("examples/isaac_manipulator_scene_light_"
                         "assistant.deployment.yaml");
    core::JsonObjectDTO expected_runtime_configuration{
        {"collision_monitor_period_ms",
         core::JsonValueDTO{mesh_heavy_isaac_runtime ? 100.0 : 50.0}},
        {"execution_tick_hz", core::JsonValueDTO{100.0}},
        {"initial_joint_positions_rad",
         core::JsonValueDTO{std::move(initial_positions)}},
        {"mode",
         core::JsonValueDTO{std::string(
             scenario.ros2_control_execution ? "commanded_sim" : "simulated")}},
        {"planner_rng_seed", core::JsonValueDTO{12345.0}},
    };
    // Q81: retained Isaac configurations include the approved logical cell
    // world. Keep this exact maintained-input check alongside the old facts.
    if (scenario.ros2_control_execution && !scenario.isaac_profile.empty()) {
      const std::string world =
          scenario.source ==
                  std::string_view("examples/trocar_lite6_foundationpose_pick."
                                   "deployment.yaml")
              ? "test_scene_realsense_foundationpose_trocar"
              : scenario.isaac_profile == std::string_view("lite6")
                    ? "lite6_grf_ros2"
                    : "isaac_manipulator_scene_light";
      expected_runtime_configuration.emplace("isaac_world",
                                             core::JsonValueDTO{world});
    }
    const auto motion_targets =
        runtime.configuration.find("motion_target_limits");
    const auto *motion_target_rows =
        motion_targets == runtime.configuration.end()
            ? nullptr
            : std::get_if<core::JsonValueDTO::Array>(
                  &motion_targets->second.value);
    const auto *motion_target =
        motion_target_rows == nullptr || motion_target_rows->size() != 1U
            ? nullptr
            : std::get_if<core::JsonObjectDTO>(
                  &motion_target_rows->front().value);
    const auto speed =
        motion_target == nullptr
            ? core::JsonObjectDTO::const_iterator{}
            : motion_target->find("maximum_tcp_linear_speed_mps");
    const auto *expected_target_speed_limit =
        motion_target == nullptr || speed == motion_target->end()
            ? nullptr
            : std::get_if<double>(&speed->second.value);
    if (expected_target_speed_limit == nullptr ||
        !std::isfinite(*expected_target_speed_limit) ||
        *expected_target_speed_limit <= 0.0) {
      throw std::invalid_argument(
          "maintained scenario has no valid Deployment TCP speed limit: " +
          std::string(scenario.source));
    }
    core::JsonObjectDTO expected_motion_target{
        {"command_target_id", core::JsonValueDTO{std::string("arm")}},
        {"controllable_component_id",
         core::JsonValueDTO{graph.controllable_components.front()
                                .controllable_component_id.value()}},
        {"maximum_tcp_linear_speed_mps",
         core::JsonValueDTO{*expected_target_speed_limit}}};
    if (!scenario.ros2_control_execution) {
      expected_motion_target.emplace(
          "ordinary_motion_jerk",
          core::JsonValueDTO{core::JsonObjectDTO{
              {"enabled", core::JsonValueDTO{false}},
              {"preset_reference_time_s", core::JsonValueDTO{1.0}},
              {"joint_limits",
               core::JsonValueDTO{core::JsonValueDTO::Array{}}}}});
    }
      expected_runtime_configuration.emplace(
          "motion_target_limits",
          core::JsonValueDTO{core::JsonValueDTO::Array{
              core::JsonValueDTO{std::move(expected_motion_target)}}});
      if (!scenario.ros2_control_execution) {
        expected_runtime_configuration.emplace(
            "direct_control_joint_limits",
            core::JsonValueDTO{core::JsonValueDTO::Array{}});
      }
      // The offline mock lane keeps the native profile's authored gripper
      // targets unchanged (Q63-Q66); Isaac profiles have none.
      if (!scenario.ros2_control_execution || !scenario.mock_profile.empty()) {
        if (scenario.family_id == std::string_view("lite6_lite_gripper")) {
          expected_runtime_configuration.emplace(
              "gripper_target_configurations",
              core::JsonValueDTO{core::JsonValueDTO::Array{
                  core::JsonValueDTO{core::JsonObjectDTO{
                      {"actuation_kind",
                       core::JsonValueDTO{std::string("position_commanded")}},
                      {"attach_frame",
                       core::JsonValueDTO{std::string("link_tcp")}},
                      {"auxiliary_frames",
                       core::JsonValueDTO{
                           core::JsonValueDTO::Array{core::JsonValueDTO{
                               std::string("lite_gripper_base_link")}}}},
                      {"closed_state_ref",
                       core::JsonValueDTO{std::string("closed")}},
                      {"command_duration_s", core::JsonValueDTO{0.8}},
                      {"command_target_id",
                       core::JsonValueDTO{std::string("gripper")}},
                      {"controllable_component_id",
                       core::JsonValueDTO{
                           graph.controllable_components.front()
                               .controllable_component_id.value()}},
                      {"description", core::JsonValueDTO{std::string(
                                          "Native simulated UFACTORY lite "
                                          "pneumatic gripper.")}},
                      {"mount_frame",
                       core::JsonValueDTO{std::string("link_eef")}},
                      {"open_state_ref",
                       core::JsonValueDTO{std::string("open")}},
                      {"tcp_frame",
                       core::JsonValueDTO{std::string("link_tcp")}},
                      {"touch_links",
                       core::JsonValueDTO{core::JsonValueDTO::Array{
                           core::JsonValueDTO{
                               std::string("lite_gripper_base_link")},
                           core::JsonValueDTO{
                               std::string("lite_gripper_left_finger")},
                           core::JsonValueDTO{
                               std::string("lite_gripper_right_finger")}}}},
                  }}}});
        }
      }
    if (scenario.ros2_control_execution) {
      const auto &component_id = graph.controllable_components.front()
                                     .controllable_component_id.value();
      // Q68: the component's own controller profile row.
      expected_runtime_configuration.emplace(
          "controller_profiles",
          core::JsonValueDTO{core::JsonValueDTO::Array{core::JsonValueDTO{
              core::JsonObjectDTO{
                  {"controllable_component_id",
                   core::JsonValueDTO{component_id}},
                  {"frame_prefix", core::JsonValueDTO{std::string{}}},
                  {scenario.mock_profile.empty() ? "isaac_profile"
                                                 : "mock_profile",
                   core::JsonValueDTO{std::string(
                       scenario.mock_profile.empty()
                           ? scenario.isaac_profile
                           : scenario.mock_profile)}}}}}});
      // Q37: main's authored `default_fault_stop_profile`, read directly.
      // The offline mock profiles carry the native profiles' authored values
      // unchanged: the UR10e assistant and UR5 profiles name
      // `default_fault_spring_back` without backend confirmation, and the
      // UR10e Robotiq profile also lists its six gripper joints.
      const bool lite6 =
          scenario.family_id == std::string_view("lite6_lite_gripper");
      const bool legacy_default_fault =
          scenario.source == std::string_view("examples/assistant_modes_scene_"
                                              "authoring.deployment.yaml") ||
          scenario.family_id == std::string_view("ur5_roboplan");
      const bool ur10e_mock_with_fingers =
          scenario.source == std::string_view("examples/pick_and_place_ur10e_"
                                              "robotiq.deployment.yaml");
      std::vector<std::string_view> fault_joints;
      if (lite6) {
        fault_joints = {"joint1", "joint2", "joint3",
                        "joint4", "joint5", "joint6"};
      } else {
        fault_joints = {"elbow_joint",   "shoulder_lift_joint",
                        "shoulder_pan_joint", "wrist_1_joint",
                        "wrist_2_joint", "wrist_3_joint"};
        if (ur10e_mock_with_fingers) {
          fault_joints.insert(fault_joints.end(),
                              {"finger_joint", "left_inner_finger_joint",
                               "left_inner_knuckle_joint",
                               "right_inner_finger_joint",
                               "right_inner_knuckle_joint",
                               "right_outer_knuckle_joint"});
          std::sort(fault_joints.begin(), fault_joints.end());
        }
      }
      core::JsonValueDTO::Array limits;
      for (const std::string_view joint : fault_joints) {
        limits.push_back(core::JsonValueDTO{core::JsonObjectDTO{
            {"max_deceleration_rad_s2", core::JsonValueDTO{10.0}},
            {"source_joint_name", core::JsonValueDTO{std::string(joint)}}}});
      }
      core::JsonObjectDTO fault_row{
          {"command_target_id", core::JsonValueDTO{std::string("arm")}},
          {"controllable_component_id", core::JsonValueDTO{component_id}},
          {"deceleration_time_s", core::JsonValueDTO{1.0}},
          {"per_joint_max_deceleration_rad_s2",
           core::JsonValueDTO{std::move(limits)}},
          {"profile_id",
           core::JsonValueDTO{std::string(legacy_default_fault
                                              ? "default_fault_spring_back"
                                              : "fault_spring_back_hold")}},
          {"stop_mode", core::JsonValueDTO{std::string("spring_back_hold")}}};
      if (!legacy_default_fault) {
        fault_row.emplace("require_backend_confirmation",
                          core::JsonValueDTO{true});
      }
      expected_runtime_configuration.emplace(
          "default_fault_stop_profiles",
          core::JsonValueDTO{core::JsonValueDTO::Array{
              core::JsonValueDTO{std::move(fault_row)}}});
    }
    if (mesh_heavy_isaac_runtime) {
      expected_runtime_configuration.emplace(
          "runtime_collision_immediate_query_budget_ms",
          core::JsonValueDTO{75.0});
      expected_runtime_configuration.emplace(
          "runtime_collision_lookahead_cycle_budget_ms",
          core::JsonValueDTO{350.0});
      expected_runtime_configuration.emplace(
          "runtime_collision_lookahead_max_samples", core::JsonValueDTO{32.0});
      expected_runtime_configuration.emplace(
          "runtime_collision_lookahead_query_budget_ms",
          core::JsonValueDTO{75.0});
      expected_runtime_configuration.emplace("runtime_collision_lookahead_s",
                                             core::JsonValueDTO{2.0});
      expected_runtime_configuration.emplace(
          "runtime_collision_lookahead_workers", core::JsonValueDTO{4.0});
      expected_runtime_configuration.emplace(
          "runtime_dynamic_body_max_age_cap_ms",
          core::JsonValueDTO{assistant_isaac_runtime ? 1500.0 : 1000.0});
      expected_runtime_configuration.emplace("validation_collision_workers",
                                             core::JsonValueDTO{4.0});
    }
    if (runtime.configuration != expected_runtime_configuration) {
      throw std::invalid_argument(
          "maintained scenario loses its exact historical runtime inputs: " +
          std::string(scenario.source));
    }

    std::set<std::string> historical_capabilities;
    std::optional<core::ProviderInstanceId> ros2_control_provider_instance;
    std::size_t native_runtime_scale_binding_count = 0U;
    for (const auto &binding : graph.control_bindings) {
      historical_capabilities.insert(binding.capability_id.value());
      if (!scenario.ros2_control_execution) {
        const bool trajectory_binding =
            binding.command_target_id.value() == "arm" &&
            (binding.capability_id.value() == "execution.trajectory.execute" ||
             binding.capability_id.value() ==
                 "execution.runtime_scale.operation_scoped");
        if (trajectory_binding) {
          if (binding.runtime_scale_route_qualifications.size() != 1U) {
            throw std::invalid_argument(
                "native trajectory binding does not retain one exact "
                "runtime-scale route: " +
                std::string(scenario.source));
          }
          const auto &qualification =
              binding.runtime_scale_route_qualifications.front();
          if (qualification.control_route_id.value() != "arm_position" ||
              qualification.scaling_domain_id.value() !=
                  graph.controllable_components.front()
                          .controllable_component_id.value() +
                      ".arm.runtime_scale" ||
              qualification.command_endpoint_role_id.value() != "command" ||
              qualification.applied_state_endpoint_role_id.value() != "state" ||
              qualification.coordinated_barrier_id.has_value() ||
              qualification.acknowledgement_kind !=
                  core::RuntimeScaleAcknowledgementKind::kAppliedState ||
              qualification.maximum_slew_per_second != 4.0 ||
              qualification.freshness_budget_ns != 100'000'000) {
            throw std::invalid_argument("native trajectory binding has the "
                                        "wrong runtime-scale qualification: " +
                                        std::string(scenario.source));
          }
          ++native_runtime_scale_binding_count;
        } else if (!binding.runtime_scale_route_qualifications.empty()) {
          throw std::invalid_argument(
              "native non-trajectory binding unexpectedly advertises runtime "
              "scaling: " +
              std::string(scenario.source));
        }
      }
      if (scenario.ros2_control_execution) {
        if (binding.state_authority_class !=
                core::StateAuthorityClass::kCommanded ||
            binding.provider_manifest_id.value() !=
                "manyforge.provider.ros2_control" ||
            binding.provider_runtime_unit_id != runtime.runtime_unit_id) {
          throw std::invalid_argument(
              "Isaac scenario loses its commanded ROS control binding: " +
              std::string(scenario.source));
        }
        if (!ros2_control_provider_instance.has_value()) {
          ros2_control_provider_instance = binding.provider_instance_id;
        } else if (*ros2_control_provider_instance !=
                   binding.provider_instance_id) {
          throw std::invalid_argument(
              "Isaac scenario splits one physical ROS provider occurrence");
        }
      }
    }
    if (!scenario.ros2_control_execution &&
        native_runtime_scale_binding_count != 2U) {
      throw std::invalid_argument("native scenario does not select trajectory "
                                  "and node scaling bindings: " +
                                  std::string(scenario.source));
    }
    for (const auto &binding : applicability.capability_provider_bindings) {
      if (!binding.capability_id.value().starts_with("assistant.")) {
        historical_capabilities.insert(binding.capability_id.value());
      }
      if (binding.scope == core::CapabilityScope::kCellGlobal) {
        if (binding.endpoint_bindings.size() != 1U) {
          throw std::invalid_argument("cell-global capability does not have "
                                      "one logical-scene endpoint: " +
                                      binding.capability_id.value());
        }
        const auto &endpoint = binding.endpoint_bindings.front();
        if (endpoint.endpoint_role_id.value() != "logical_scene_services" ||
            endpoint.source_kind !=
                core::CapabilityEndpointSourceKind::kRuntimeUnit ||
            !std::holds_alternative<core::RuntimeUnitId>(endpoint.source_id) ||
            std::get<core::RuntimeUnitId>(endpoint.source_id) !=
                runtime.runtime_unit_id ||
            endpoint.source_endpoint_role_id.value() !=
                "logical_scene_services") {
          throw std::invalid_argument("cell-global capability does not select "
                                      "the logical-scene boundary: " +
                                      binding.capability_id.value());
        }
      } else if (scenario.ros2_control_execution &&
                 binding.provider_manifest_id.value() ==
                     "manyforge.provider.ros2_control" &&
                 (!ros2_control_provider_instance.has_value() ||
                  binding.provider_instance_id !=
                      *ros2_control_provider_instance)) {
        throw std::invalid_argument(
            "Isaac planning capability does not join its physical ROS "
            "provider occurrence");
      }
    }
    const bool has_operation_scale =
        historical_capabilities.erase(
            "execution.runtime_scale.operation_scoped") == 1U;
    if (historical_capabilities.size() != 34U ||
        has_operation_scale == scenario.ros2_control_execution) {
      throw std::invalid_argument(
          "maintained scenario does not retain all 34 historical facilities: " +
          std::string(scenario.source));
    }
    const auto &component = graph.controllable_components.front();
    const auto &model = graph.model_instances.front();
    const std::string expected_component_id =
        std::string(scenario.family_id) + (scenario.ros2_control_execution
                                               ? ".fixed_scale.component"
                                               : ".component");
    if (component.component_type_id.value() != expected_component_id ||
        model.model_asset_id.value() != scenario.family_id) {
      throw std::invalid_argument(
          "maintained scenario selects the wrong shared family closure: " +
          std::string(scenario.source));
    }
    const auto identity = std::pair{component.component_type_revision.value(),
                                    model.model_revision.value()};
    const auto [position, inserted] =
        family_identities.emplace(expected_component_id, identity);
    if (!inserted && position->second != identity) {
      throw std::invalid_argument(
          "same-profile scenarios select different component/model revisions");
    }
    const auto [model_position, model_inserted] = family_models.emplace(
        std::string(scenario.family_id), model.model_revision.value());
    if (!model_inserted &&
        model_position->second != model.model_revision.value()) {
      throw std::invalid_argument(
          "same-family profiles select different models");
    }
  }
  // Q63-Q66: every maintained scenario selects its family's fixed-scale ROS
  // component (Isaac or offline mock) of the three maintained families.
  if (family_identities.size() != 3U || family_models.size() != 3U) {
    throw std::logic_error("maintained scenarios do not cover three families");
  }
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
  Options options;
  options.workspace_root = std::filesystem::canonical(capability_root);

  ExactCompilerInputSource input_source;
  auto catalog = qualify_catalog(options.workspace_root, input_source, {},
                                 prepared_models);
  std::vector<core::CatalogRecordRefDTO> catalog_records;
  catalog_records.reserve(catalog.records.size());
  for (const auto &[unused, reference] : catalog.records) {
    static_cast<void>(unused);
    catalog_records.push_back(reference);
  }
  CanonicalValue compiled;
  try {
    compiled = compile_activation_deployment(
        intent, runtime_cell_id, component_profiles, prepared_models, catalog_records,
        catalog.runtime_profiles);
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
  const auto &model_instances = require_array(
      require_member(graph, "model_instances", "Deployment.graph"),
      "Deployment.graph.model_instances");
  const std::string family_id = model_instances.empty()
                                    ? std::string{}
                                    : require_text(
                                          require_member(model_instances.front(),
                                                         "model_asset_id",
                                                         "model instance"),
                                          "model instance.model_asset_id");
  const std::string source_key = "captured.deployment";
  const Scenario scenario{
      source_key,
      family_id,
      false,
      runtime_profile_id == "manyforge.ros2_control_execution_runtime",
      "",
      "",
  };
  const std::array<Scenario, 1U> selected{scenario};
  MemoryPreparationStores stores;
  std::map<std::string, ScenarioProduct, std::less<>> products;
  try {
    products = materialize_scenarios(
        options.workspace_root, catalog, input_source,
        {}, selected, &compiled, prepare_canonical_empty, &stores);
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

namespace manyforge::deployment::detail {

int run_maintained_example_qualification(int argc, char **argv) {
  try {
    Options options = parse_options(argc, argv);
    options.workspace_root = std::filesystem::canonical(options.workspace_root);
    for (auto &[family_id, prepared_root] : options.prepared_roots) {
      static_cast<void>(family_id);
      prepared_root = std::filesystem::canonical(prepared_root);
    }
    const auto &root = options.workspace_root;
    const auto families = load_families(options);
    ExactCompilerInputSource input_source;
    const QualifiedCatalogClosure catalog =
        qualify_catalog(root, input_source, families);
    verify_scenarios(root, catalog);
    TemporaryPackage package(options.package_root);
    auto products =
        materialize_scenarios(root, catalog, input_source, package.path());
    auto composed_products = materialize_scenarios(
        root, catalog, input_source, package.path(), kComposedScenarios);
    products.merge(composed_products);
    if (!composed_products.empty()) {
      throw std::logic_error("composed scenario product source is duplicated");
    }
    const auto bindings =
        verify_program_bindings(root, products, package.path());
    if (bindings.size() != 15U) {
      throw std::logic_error(
          "maintained scenario documents lost a Program Binding");
    }
    std::cout
        << "13 deployments, 5 shared families, 34 historical facilities, "
           "15 durable Program Bindings, and 15 self-contained scenario "
           "documents validated\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "manyforge_maintained_deployment_occurrences: " << error.what()
              << '\n';
    return 2;
  }
}

} // namespace manyforge::deployment::detail
