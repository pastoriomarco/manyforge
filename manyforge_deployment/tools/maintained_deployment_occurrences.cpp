#include <algorithm>
#include <array>
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
#include <manyforge_deployment/runtime_policy_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/scenario_apply_candidate_source.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>
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
#include "retained_bundle_internal.hpp"

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

struct Options final {
  enum class Mode { kCheck, kCompileCandidate };

  Mode mode{Mode::kCheck};
  std::filesystem::path workspace_root;
  std::optional<std::filesystem::path> package_root;
  std::map<std::string, std::filesystem::path, std::less<>> prepared_roots;
  std::filesystem::path base_package_root;
  std::filesystem::path candidate_package_root;
  std::filesystem::path candidate_deployment_path;
  std::filesystem::path candidate_program_path;
  std::string candidate_scenario_id;
  std::string expected_scenario_catalog_revision;
};

struct Scenario final {
  std::string_view source;
  std::string_view family_id;
  bool assistant_enabled;
  bool ros2_control_execution;
  std::string_view isaac_profile;
};

constexpr std::array<Scenario, 12U> kScenarios{{
    {"examples/assistant_modes_scene_authoring.deployment.yaml",
     "ur10e_robotiq_2f_140", true, false, ""},
    {"examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml",
     "lite6_lite_gripper", false, true, "lite6"},
    {"examples/isaac_manipulator_scene_light_assistant.deployment.yaml",
     "ur10e_robotiq_2f_140", true, true, "ur10e"},
    {"examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml",
     "ur10e_robotiq_2f_140", false, true, "ur10e"},
    {"examples/"
     "isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml",
     "ur10e_robotiq_2f_140", false, true, "ur10e"},
    {"examples/lite6_lite_gripper_pick_and_place.deployment.yaml",
     "lite6_lite_gripper", false, false, ""},
    {"examples/pick_and_place.deployment.yaml", "ur5_roboplan", false, false,
     ""},
    {"examples/pick_and_place_ur10e_robotiq.deployment.yaml",
     "ur10e_robotiq_2f_140", false, false, ""},
    {"examples/semantic_shape_pick.deployment.yaml", "ur5_roboplan", true,
     false, ""},
    {"examples/trocar_lite6_foundationpose_pick.deployment.yaml",
     "lite6_lite_gripper", false, true, "lite6"},
    {"examples/vision_guided_bin_pick.deployment.yaml", "ur5_roboplan", true,
     false, ""},
    {"examples/vision_guided_pose_pick.deployment.yaml", "ur5_roboplan", true,
     false, ""},
}};

[[nodiscard]] Options parse_options(int argc, char **argv) {
  if (argc < 3 || (std::string_view(argv[1]) != "--check" &&
                   std::string_view(argv[1]) != "--compile-candidate")) {
    throw std::invalid_argument(
        "usage: manyforge_maintained_deployment_occurrences "
        "(--check | --compile-candidate) "
        "--workspace-root <manyforge-root> "
        "[--package-root <empty-output-directory>] "
        "[--base-package-root <installed-package> "
        "--candidate-package-root <writable-package> "
        "--candidate-deployment <deployment-file> "
        "--candidate-program <program-file> "
        "--scenario-id <canonical-scenario-id> "
        "--expected-scenario-catalog-revision <sha256>] "
        "--prepared <family-id>=<directory> [--prepared ...]");
  }
  Options result;
  result.mode = std::string_view(argv[1]) == "--check"
                    ? Options::Mode::kCheck
                    : Options::Mode::kCompileCandidate;
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
    if (option == "--base-package-root" &&
        result.base_package_root.empty()) {
      result.base_package_root = value;
      index += 2;
      continue;
    }
    if (option == "--candidate-package-root" &&
        result.candidate_package_root.empty()) {
      result.candidate_package_root = value;
      index += 2;
      continue;
    }
    if (option == "--candidate-deployment" &&
        result.candidate_deployment_path.empty()) {
      result.candidate_deployment_path = value;
      index += 2;
      continue;
    }
    if (option == "--candidate-program" &&
        result.candidate_program_path.empty()) {
      result.candidate_program_path = value;
      index += 2;
      continue;
    }
    if (option == "--scenario-id" && result.candidate_scenario_id.empty()) {
      result.candidate_scenario_id = value;
      index += 2;
      continue;
    }
    if (option == "--expected-scenario-catalog-revision" &&
        result.expected_scenario_catalog_revision.empty()) {
      result.expected_scenario_catalog_revision = value;
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
  if (result.workspace_root.empty() || result.prepared_roots.size() != 3U) {
    throw std::invalid_argument("maintained qualification requires one "
                                "workspace root and three prepared families");
  }
  if (result.mode == Options::Mode::kCheck) {
    if (!result.base_package_root.empty() ||
        !result.candidate_package_root.empty() ||
        !result.candidate_deployment_path.empty() ||
        !result.candidate_program_path.empty() ||
        !result.candidate_scenario_id.empty() ||
        !result.expected_scenario_catalog_revision.empty()) {
      throw std::invalid_argument(
          "candidate compilation options are invalid in --check mode");
    }
  } else if (result.package_root.has_value() ||
             result.base_package_root.empty() ||
             result.candidate_package_root.empty() ||
             result.candidate_deployment_path.empty() ||
             result.candidate_program_path.empty() ||
             result.candidate_scenario_id.empty() ||
             !deployment::is_sha256_digest(
                 result.expected_scenario_catalog_revision)) {
    throw std::invalid_argument(
        "candidate compilation requires exact source, destination, "
        "Deployment, Scenario, and expected-catalog inputs");
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
  if (result.size() != 3U) {
    throw std::invalid_argument(
        "maintained description must contain three model families");
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
  std::vector<deployment::QualifiedRuntimePolicy> runtime_policies;
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
  deployment::VerifiedRetainedModelMap model_map;
  std::filesystem::path canonical_empty_admission_filename;
};

void verify_gripper_resource_semantics(
    const Scenario &scenario, const core::CellGraphDTO &graph,
    const deployment::VerifiedRetainedModelMap &model_map) {
  if (graph.controllable_components.size() != 1U) {
    throw std::logic_error(
        "maintained gripper proof requires one controllable component");
  }
  const auto &component_id =
      graph.controllable_components.front().controllable_component_id;
  const core::ResourceId expected_local_id(component_id.value() +
                                           ".gripper.logical_position");
  bool found = false;
  for (const auto &binding : model_map.control_bindings()) {
    if (binding.controllable_component_id != component_id ||
        binding.command_target_id.value() != "gripper") {
      continue;
    }
    for (const auto &resource : binding.resources) {
      if (resource.resource_role_id.value() != "fingers") {
        continue;
      }
      found = true;
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
  if (!found) {
    throw std::logic_error(
        "maintained deployment has no retained gripper resource proof");
  }
}

[[nodiscard]] std::map<std::string, ScenarioProduct, std::less<>>
materialize_scenarios(const std::filesystem::path &root,
                      const QualifiedCatalogClosure &catalog,
                      const ExactCompilerInputSource &input_source,
                      const std::filesystem::path &package_root,
                      std::span<const Scenario> scenarios = kScenarios,
                      const std::optional<std::filesystem::path>
                          &deployment_override = std::nullopt,
                      const bool publish_canonical_empty = true) {
  if (deployment_override.has_value() && scenarios.size() != 1U) {
    throw std::invalid_argument(
        "one candidate Deployment override requires one Scenario");
  }
  const std::string overlay_bytes = read_file(
      root / "manyforge_deployment/resources/maintained/site_overlays/"
             "manyforge_local_examples.site_overlay.json");
  deployment::FilesystemBuildManifestRepository manifest_repository(
      package_root);
  deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
  deployment::FilesystemRawArtifactStore raw_store(package_root);
  deployment::FilesystemRetainedBundleRepository bundle_repository(
      package_root);
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
            read_file(deployment_override.has_value()
                          ? *deployment_override
                          : root / scenario.source)),
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
    const auto runtime_policies = select_catalog_values(
        catalog.runtime_policies, selected_records, [](const auto &value) {
          return CatalogKey{static_cast<std::uint8_t>(
                                core::CatalogRecordKind::kRuntimePolicy),
                            value.value().runtime_policy_id.value(),
                            value.value().runtime_policy_revision.value()};
        });
    const auto stop_policies = select_catalog_values(
        catalog.stop_policies, selected_records, [](const auto &value) {
          return CatalogKey{
              static_cast<std::uint8_t>(
                  core::CatalogRecordKind::kStopEvidencePolicy),
              value.value().stop_evidence_policy_id.value(),
              value.value().stop_evidence_policy_revision.value()};
        });

    if (model_assets.size() != 1U || component_types.size() != 1U ||
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
        runtime_policies, command_schemas, capabilities, contracts, topologies,
        manifests, stop_policies, selected_value_schemas,
        selected_commissioning_evidence, selected_overlay);
    const auto qualified = deployment::qualify_deployment_for_retention(
        verified, deployment::compile_cell_ir(applicability));

    const auto preparation = std::find_if(
        catalog.structural_preparations.begin(),
        catalog.structural_preparations.end(), [&](const auto &value) {
          return value.verified_model_asset().value().model_asset_id ==
                     model_assets.front().value().model_asset_id &&
                 value.verified_model_asset().value().model_revision ==
                     model_assets.front().value().model_revision;
        });
    if (preparation == catalog.structural_preparations.end()) {
      throw std::logic_error("scenario has no exact structural preparation");
    }
    const std::array<deployment::BuildQualifiedStructuralModelPreparation, 1U>
        selected_preparation{*preparation};
    auto bundle = deployment::detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(
            qualified, manifest_repository, input_source, selected_preparation,
            catalog_store, raw_store, bundle_repository);
    if (bundle == nullptr ||
        bundle->value().compiled_products.symbol_role_maps.size() != 1U) {
      throw std::logic_error("scenario did not retain one exact model map");
    }
    const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
        package_root);
    auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
        bundle->retained_bundle_id(),
        bundle->value().compiled_products.symbol_role_maps.front().product_id});
    if (!model_map.has_value()) {
      throw std::logic_error(
          "scenario model map cannot be independently reloaded");
    }

    std::filesystem::path admission_filename;
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
          std::span<const deployment::QualifiedRuntimePolicy>{},
          std::span<const deployment::QualifiedCommandSchema>{},
          std::span<const deployment::QualifiedCapabilityDefinition>{},
          std::span<const deployment::QualifiedControlInterfaceContract>{},
          std::span<const deployment::QualifiedControllerTopology>{},
          std::span<const deployment::QualifiedProviderManifest>{},
          std::span<const deployment::QualifiedStopEvidencePolicy>{},
          std::span<const deployment::BuildQualifiedValueSchemaEntry>{},
          std::span<const deployment::QualifiedCommissioningEvidenceRef>{},
          empty_overlay);
      const auto empty_qualified =
          deployment::qualify_deployment_for_retention(
              empty_deployment,
              deployment::compile_cell_ir(empty_applicability));
      const auto empty_bundle =
          deployment::detail::RetainedBundleMaterializer::
              materialize_store_and_reload_production(
                  empty_qualified, manifest_repository, input_source, {},
                  catalog_store, raw_store, bundle_repository);
      if (empty_bundle == nullptr ||
          empty_bundle->value().cell_graph.runtime_units !=
              bundle->value().cell_graph.runtime_units ||
          empty_bundle->value().compiled_products.runtime_unit_products !=
              bundle->value().compiled_products.runtime_unit_products) {
        throw std::logic_error(
            "cell-scoped canonical predecessor does not retain the "
            "candidate's exact execution host product");
      }
      admission_filename =
          deployment::cell_scoped_canonical_empty_admission_filename(
              verified.cell_id());
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
    verify_gripper_resource_semantics(scenario, verified.graph().graph(),
                                      *model_map);
    if (!result
             .emplace(
                 std::string(scenario.source),
                 ScenarioProduct{
                     verified.deployment_artifact_id(),
                     verified.deployment_revision(),
                     deployment::scenario_deployment_compatibility(verified),
                     bundle, std::move(*model_map), admission_filename})
             .second) {
      throw std::logic_error("scenario product source is duplicated");
    }
  }
  return result;
}

struct StoredProgramBinding final {
  std::string scenario_id;
  std::vector<std::string> scenario_aliases;
  std::string deployment_source;
  std::string program_source;
  core::ProgramBindingId program_binding_id;
  core::ProgramId program_id;
  core::ProgramRevision program_revision;
};

[[nodiscard]] const core::ProgramTargetRoleRequirementDTO &
require_target_requirement(const core::ProgramBindingContractDTO &contract,
                           const core::TargetRoleId &target_role_id) {
  const core::ProgramTargetRoleRequirementDTO *result = nullptr;
  for (const auto &candidate : contract.target_roles) {
    if (candidate.target_role_id != target_role_id) {
      continue;
    }
    if (result != nullptr) {
      throw std::logic_error("Program contract has an ambiguous target role");
    }
    result = &candidate;
  }
  if (result == nullptr) {
    throw std::invalid_argument(
        "role assignment selects an unknown target role");
  }
  return *result;
}

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

[[nodiscard]] std::vector<StoredProgramBinding> verify_program_bindings(
    const std::filesystem::path &root,
    const std::map<std::string, ScenarioProduct, std::less<>> &products,
    const std::filesystem::path &package_root) {
  const auto parsed = deployment::parse_strict_json_model_yaml(
      read_file(root / "manyforge_deployment/resources/maintained/"
                       "maintained_program_role_assignments.v1.json"));
  if (require_text(require_member(parsed.root(), "api_version",
                                  "maintained role assignments"),
                   "maintained role assignments.api_version") !=
      "manyforge.maintained_program_role_assignments.v1") {
    throw std::invalid_argument(
        "maintained role assignments have the wrong API version");
  }
  const auto &rows =
      require_array(require_member(parsed.root(), "assignments",
                                   "maintained role assignments"),
                    "maintained role assignments.assignments");
  if (rows.size() != 13U || products.size() != 12U) {
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
      const std::string &deployment_source = require_text(
          require_member(row, "deployment_source", "role assignment"),
          "role assignment.deployment_source");
      const std::string &scenario_id =
          require_text(require_member(row, "scenario_id", "role assignment"),
                       "role assignment.scenario_id");
      std::vector<std::string> scenario_aliases;
      for (const auto &alias : require_array(
               require_member(row, "scenario_aliases", "role assignment"),
               "role assignment.scenario_aliases")) {
        scenario_aliases.push_back(
            require_text(alias, "role assignment.scenario_aliases[]"));
      }
      const auto relative_deployment = require_relative_path(
          deployment_source, "role assignment.deployment_source");
      const auto product = products.find(relative_deployment.generic_string());
      if (product == products.end()) {
        throw std::invalid_argument(
            "role assignment has no exact materialized deployment");
      }
      const auto &context = product->second;
      if (context.deployment_artifact_id.value() !=
              require_text(require_member(row, "deployment_artifact_id",
                                          "role assignment"),
                           "role assignment.deployment_artifact_id") ||
          context.deployment_revision.value() !=
              require_text(
                  require_member(row, "deployment_revision", "role assignment"),
                  "role assignment.deployment_revision")) {
        throw std::invalid_argument(
            "role assignment selects a stale deployment identity");
      }

      const auto program_source = require_relative_path(
          require_text(require_member(row, "program_source", "role assignment"),
                       "role assignment.program_source"),
          "role assignment.program_source");
      const auto program = deployment::parse_strict_json_model_yaml(
          read_file(root / program_source));
      const std::string canonical_program =
          deployment::canonical_json(program.root());
      const auto contract = deployment::verify_program_binding_contract(
          canonical_program, *context.bundle);
      if (contract->value().program_id.value() !=
              require_text(require_member(row, "program_id", "role assignment"),
                           "role assignment.program_id") ||
          contract->value().program_revision.value() !=
              require_text(
                  require_member(row, "program_revision", "role assignment"),
                  "role assignment.program_revision")) {
        throw std::invalid_argument(
            "role assignment selects a stale Program identity");
      }
      (void)program_repository.put_if_absent(contract, *context.bundle);

      core::ProgramRoleAssignmentDTO assignment;
      for (const auto &[role, value] : require_object(
               require_member(row, "component_roles", "role assignment"),
               "role assignment.component_roles")) {
        assignment.component_roles.push_back(
            {core::ComponentRoleId(role),
             core::ControllableComponentId(require_text(value, role))});
      }
      for (const auto &[role, value] : require_object(
               require_member(row, "target_roles", "role assignment"),
               "role assignment.target_roles")) {
        const auto role_id = core::TargetRoleId(role);
        const auto &requirement =
            require_target_requirement(contract->value(), role_id);
        if (requirement.component_role_id.value() !=
            require_text(require_member(value, "component_role",
                                        "target role assignment"),
                         "target role assignment.component_role")) {
          throw std::invalid_argument(
              "target role assignment changes its Program component role");
        }
        assignment.target_roles.push_back(
            {std::move(role_id),
             core::CommandTargetId(
                 require_text(require_member(value, "command_target_id",
                                             "target role assignment"),
                              "target role assignment.command_target_id"))});
      }

      const auto binding = deployment::compile_program_binding(
          *contract, std::move(assignment), *context.bundle, context.model_map,
          catalog_store);
      verify_resolved_program_binding(*binding, *context.bundle,
                                      context.model_map);
      if (!repository.put_if_absent(binding, *context.bundle,
                                    context.model_map)) {
        throw std::logic_error(
            "maintained Program Binding identity is unexpectedly duplicated");
      }
      binding_ids.insert(binding->program_binding_id().value());
      program_revisions.insert(binding->value().program_revision.value());
      program_bundles[binding->value().program_revision.value()].insert(
          binding->value().retained_bundle_id.value());
      stored.push_back(StoredProgramBinding{
          scenario_id, std::move(scenario_aliases), deployment_source,
          program_source.generic_string(), binding->program_binding_id(),
          binding->value().program_id, binding->value().program_revision});
    }
    if (repository.size() != 13U) {
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
    const auto binding = reopened.get_verified(record.program_binding_id,
                                               *product->second.bundle,
                                               product->second.model_map);
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
                                    product->second.model_map);
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
  if (binding_ids.size() != 13U || program_revisions.size() != 11U ||
      reopened.size() != 13U || program_reopened.size() != 11U ||
      reused_bundle_count("pick_and_place_ur10e_robotiq_demo") != 2U ||
      reused_bundle_count("isaac_manipulator_scene_light") != 2U) {
    throw std::logic_error(
        "maintained Program-Binding set loses its eleven-to-thirteen closure");
  }
  return stored;
}

[[nodiscard]] deployment::ScenarioAssistantMode
program_assistant_mode(const deployment::CanonicalValue &program_root) {
  const auto &compatibility =
      require_member(program_root, "compatibility", "maintained Program");
  const std::string &mode = require_text(
      require_member(compatibility, "assistant", "Program compatibility"),
      "Program compatibility.assistant");
  if (mode == "required") {
    return deployment::ScenarioAssistantMode::kRequired;
  }
  if (mode == "optional") {
    return deployment::ScenarioAssistantMode::kOptional;
  }
  if (mode == "forbidden") {
    return deployment::ScenarioAssistantMode::kForbidden;
  }
  throw std::invalid_argument(
      "maintained Program has an unsupported assistant mode");
}

[[nodiscard]] std::filesystem::path stage_scenario_source(
    const std::filesystem::path &workspace_root,
    const std::filesystem::path &package_root, std::string_view relative_source,
    std::map<std::string, std::filesystem::path, std::less<>> &staged) {
  const auto relative =
      require_relative_path(relative_source, "maintained scenario source");
  const std::filesystem::path source = workspace_root / relative;
  const std::filesystem::path destination = package_root / relative.filename();
  const auto [found, inserted] =
      staged.emplace(relative.generic_string(), destination);
  if (!inserted) {
    return found->second;
  }
  if (!std::filesystem::copy_file(source, destination)) {
    throw std::runtime_error("cannot stage maintained scenario source: " +
                             source.string());
  }
  return destination;
}

void stage_program_scene_assets(
    const std::filesystem::path &workspace_root,
    const std::filesystem::path &package_root,
    const deployment::CanonicalValue &program_root,
    std::map<std::string, std::filesystem::path, std::less<>> &staged) {
  const auto *scene = program_root.find_object_member("scene");
  if (scene == nullptr) {
    return;
  }
  for (const auto &resource :
       require_array(*scene, "maintained Program scene")) {
    const auto &shape = require_member(resource, "shape", "scene resource");
    const auto &type =
        require_text(require_member(shape, "type", "scene resource shape"),
                     "scene resource shape.type");
    if (type != "mesh") {
      continue;
    }
    const auto &uri = require_text(
        require_member(shape, "mesh_resource_uri", "scene mesh shape"),
        "scene mesh shape.mesh_resource_uri");
    const auto relative =
        require_relative_path(uri, "maintained Program scene mesh URI");
    if (relative.empty() || relative == ".") {
      throw std::invalid_argument(
          "maintained Program scene mesh URI must name one file");
    }
    const auto destination = package_root / relative;
    const auto [found, inserted] = staged.emplace(uri, destination);
    if (!inserted) {
      if (found->second != destination) {
        throw std::logic_error(
            "maintained Program scene mesh URI aliases another destination");
      }
      continue;
    }

    const auto source = workspace_root / relative;
    const std::string bytes = read_file(source);
    std::error_code filesystem_error;
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystem_error);
    if (filesystem_error || std::filesystem::exists(destination)) {
      throw std::runtime_error(
          "cannot reserve maintained Program scene mesh destination: " +
          destination.string());
    }
    {
      std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
      if (!stream) {
        throw std::runtime_error(
            "cannot stage maintained Program scene mesh: " +
            destination.string());
      }
      stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      stream.flush();
      if (!stream) {
        throw std::runtime_error(
            "cannot complete maintained Program scene mesh: " +
            destination.string());
      }
    }
    if (read_file(destination) != bytes) {
      throw std::runtime_error(
          "maintained Program scene mesh failed exact read-back: " +
          destination.string());
    }
  }
}

void publish_and_verify_scenario_catalog(
    const std::filesystem::path &workspace_root,
    const std::map<std::string, ScenarioProduct, std::less<>> &products,
    const std::vector<StoredProgramBinding> &bindings,
    const std::filesystem::path &package_root) {
  if (bindings.size() != 13U) {
    throw std::logic_error(
        "maintained scenario catalog requires thirteen Program Bindings");
  }
  std::map<std::string, std::filesystem::path, std::less<>> staged;
  std::vector<deployment::ScenarioCatalogRowDTO> rows;
  rows.reserve(bindings.size());
  for (const auto &record : bindings) {
    const auto product = products.find(record.deployment_source);
    if (product == products.end()) {
      throw std::logic_error(
          "maintained scenario lost its exact deployment product");
    }
    const auto program_path = stage_scenario_source(
        workspace_root, package_root, record.program_source, staged);
    (void)stage_scenario_source(workspace_root, package_root,
                                record.deployment_source, staged);
    const auto program =
        deployment::parse_strict_json_model_yaml(read_file(program_path));
    stage_program_scene_assets(workspace_root, package_root, program.root(),
                               staged);
    rows.push_back(deployment::ScenarioCatalogRowDTO{
        .scenario_id = record.scenario_id,
        .description = std::nullopt,
        .aliases = record.scenario_aliases,
        .deployment_artifact_id = product->second.deployment_artifact_id,
        .deployment_revision = product->second.deployment_revision,
        .retained_bundle_id = product->second.bundle->retained_bundle_id(),
        .program_binding_id = record.program_binding_id,
        .deployment_compatibility = product->second.deployment_compatibility,
        .assistant_mode = program_assistant_mode(program.root()),
        .typed_parameter_overrides =
            deployment::CanonicalValue(deployment::CanonicalValue::Object{}),
    });
  }
  std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
    return left.scenario_id < right.scenario_id;
  });
  const auto regenerated = deployment::build_scenario_catalog(rows);
  const auto catalog = deployment::build_scenario_catalog(std::move(rows));
  if (regenerated->revision() != catalog->revision() ||
      regenerated->canonical_bytes() != catalog->canonical_bytes()) {
    throw std::logic_error(
        "maintained scenario catalog regeneration is not byte-identical");
  }
  {
    deployment::FilesystemScenarioCatalogRepository repository(package_root,
                                                               "maintained");
    if (repository.compare_and_set(std::nullopt, catalog) !=
        deployment::ScenarioCatalogPublishResult::kPublished) {
      throw std::logic_error(
          "maintained scenario catalog did not publish from an empty head");
    }
  }

  deployment::FilesystemScenarioCatalogRepository reopened(package_root,
                                                           "maintained");
  const auto reloaded = reopened.get_verified();
  if (reloaded == nullptr || reloaded->revision() != catalog->revision() ||
      reloaded->rows() != catalog->rows()) {
    throw std::logic_error(
        "maintained scenario catalog did not independently reload exactly");
  }

  const deployment::FilesystemScenarioApplyCandidateSource scenario_source(
      package_root, "maintained");

  std::vector<deployment::ScenarioArtifactClosureQuery> verified_queries;
  verified_queries.reserve(bindings.size());
  for (const auto &record : bindings) {
    const auto product = products.find(record.deployment_source);
    const auto binding_row = std::find_if(
        reloaded->rows().begin(), reloaded->rows().end(),
        [&](const auto &row) { return row.scenario_id == record.scenario_id; });
    if (product == products.end() || binding_row == reloaded->rows().end()) {
      throw std::logic_error("maintained scenario catalog lost one closure");
    }
    const auto selected =
        scenario_source.LoadExactProgramSelection(record.scenario_id);
    if (!selected.valid() || selected.scenario_id != record.scenario_id ||
        selected.scenario_catalog_revision != reloaded->revision() ||
        selected.deployment_artifact_id !=
            product->second.deployment_artifact_id ||
        selected.deployment_revision != product->second.deployment_revision ||
        selected.retained_bundle_id !=
            product->second.bundle->retained_bundle_id() ||
        selected.graph_revision !=
            product->second.bundle->value().cell_graph.graph_revision ||
        selected.program_binding_id != record.program_binding_id ||
        selected.program_id != record.program_id ||
        selected.program_revision != record.program_revision ||
        selected.node_environment_digest !=
            product->second.bundle->value()
                .catalogs.node_environment.environment_digest ||
        selected.deployment_compatibility !=
            product->second.deployment_compatibility ||
        selected.assistant_mode != binding_row->assistant_mode ||
        selected.typed_parameter_overrides !=
            deployment::CanonicalValue(deployment::CanonicalValue::Object{})) {
      throw std::logic_error(
          "maintained scenario selector did not preserve its exact closure");
    }
    const auto binding_path =
        package_root / "program-bindings" / "sha256" /
        (record.program_binding_id.value().substr(7U) + ".json");
    const auto program_path =
        package_root / "programs" / "sha256" /
        (record.program_revision.value().substr(7U) + ".json");
    const auto retained_bundle_path =
        package_root / "retained-bundles" / "sha256" /
        (product->second.bundle->retained_bundle_id().value().substr(7U) +
         ".json");
    const deployment::ScenarioArtifactClosureQuery query{
        .scenario_id = record.scenario_id,
        .scenario_catalog_revision = reloaded->revision(),
        .repository_root = package_root,
        .scenario_catalog_path = reopened.catalog_path(),
        .deployment_path =
            package_root /
            std::filesystem::path(record.deployment_source).filename(),
        .retained_bundle_path = retained_bundle_path,
        .program_binding_path = binding_path,
        .program_path = program_path,
        .deployment_artifact_id = product->second.deployment_artifact_id,
        .deployment_revision = product->second.deployment_revision,
        .retained_bundle_id = product->second.bundle->retained_bundle_id(),
        .program_binding_id = record.program_binding_id,
        .graph_revision =
            product->second.bundle->value().cell_graph.graph_revision,
        .program_id = record.program_id,
        .program_revision = record.program_revision,
        .node_environment_digest =
            product->second.bundle->value()
                .catalogs.node_environment.environment_digest,
        .assistant_mode = binding_row->assistant_mode,
        .typed_parameter_overrides_json = "{}",
    };
    const auto verified = deployment::verify_scenario_artifact_closure(query);
    if (verified.lane != product->second.deployment_compatibility.lane ||
        verified.assistant_enabled !=
            product->second.bundle->value().assistant.has_value()) {
      throw std::logic_error(
          "maintained scenario verifier returned divergent presentation facts");
    }
    verified_queries.push_back(query);
  }

  if (verified_queries.size() < 2U) {
    throw std::logic_error(
        "maintained scenario rejection matrix requires two closures");
  }
  const auto require_rejection = [](auto candidate, std::string_view label) {
    try {
      (void)deployment::verify_scenario_artifact_closure(candidate);
    } catch (const deployment::ScenarioCatalogError &) {
      return;
    }
    throw std::logic_error("maintained scenario verifier accepted " +
                           std::string(label));
  };

  auto stale_catalog = verified_queries.front();
  stale_catalog.scenario_catalog_revision =
      core::ScenarioCatalogRevision("sha256:" + std::string(64U, 'f'));
  require_rejection(stale_catalog, "a stale catalog revision");

  auto swapped_deployment = verified_queries.front();
  swapped_deployment.deployment_path = verified_queries[1].deployment_path;
  require_rejection(swapped_deployment, "a swapped deployment source");

  auto swapped_binding = verified_queries.front();
  swapped_binding.program_binding_path =
      verified_queries[1].program_binding_path;
  require_rejection(swapped_binding, "a swapped Program Binding source");

  auto stale_environment = verified_queries.front();
  stale_environment.node_environment_digest =
      core::ContentDigest("sha256:" + std::string(64U, 'e'));
  require_rejection(stale_environment, "a stale node environment");
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
      "catalog/maintained/" + std::string(core::to_string(kind)) + "/" +
      record_id + "/" + record_revision.substr(7U));
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
                const std::vector<FamilySource> &families) {
  const auto catalog_root =
      root / "manyforge_deployment/resources/maintained/catalog";
  QualifiedCatalogClosure result;

  for (const auto &path :
       source_files(catalog_root / "capability_definition", 44U)) {
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
  if (target_capabilities.size() != 23U) {
    throw std::logic_error(
        "maintained component closure has the wrong target-capability count");
  }
  for (const auto &path : source_files(catalog_root / "component_type", 5U)) {
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
    if (manifest.value().provider_manifest_id.value() == "lite6_arm_provider") {
      if (found_lite6_arm_lifecycle_manifest ||
          !manifest.value().lifecycle_plan_catalog.has_value() ||
          manifest.value()
                  .lifecycle_plan_catalog->catalog_api_version.value() !=
              core::kBrandLifecyclePlanCatalogApiVersion ||
          manifest.value().offered_capabilities.size() != 16U ||
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
          manifest.value().offered_capabilities.size() != 27U ||
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
  if (!found_lite6_arm_lifecycle_manifest ||
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

  for (const auto &path : source_files(catalog_root / "runtime_policy", 12U)) {
    const std::string bytes = read_file(path);
    const auto reference =
        make_reference<core::RuntimePolicyCatalogRecordRefDTO>(bytes);
    result.runtime_policies.push_back(
        deployment::resolve_and_qualify_runtime_policy(
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
  if (result.records.size() != 86U || result.runtime_profiles.size() != 2U ||
      result.runtime_policies.size() != 12U) {
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
    const std::size_t expected_catalog_count =
        (scenario.ros2_control_execution
             ? 54U
             : (scenario.family_id == "lite6_lite_gripper" ? 57U : 51U)) +
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
            (scenario.ros2_control_execution ? 12U : 13U) ||
        applicability.capability_provider_bindings.size() != 26U) {
      throw std::invalid_argument("maintained scenario has an incomplete "
                                  "component/runtime/capability closure: " +
                                  std::string(scenario.source));
    }
    if (applicability.retained_policy_records.size() != 1U) {
      throw std::invalid_argument(
          "maintained scenario does not select one exact runtime policy: " +
          std::string(scenario.source));
    }
    const auto *runtime_policy_ref =
        std::get_if<core::RuntimePolicyCatalogRecordRefDTO>(
            &applicability.retained_policy_records.front());
    const auto selected_policy =
        runtime_policy_ref == nullptr
            ? catalog.runtime_policies.end()
            : std::find_if(catalog.runtime_policies.begin(),
                           catalog.runtime_policies.end(),
                           [runtime_policy_ref](const auto &policy) {
                             return policy.reference() == *runtime_policy_ref;
                           });
    if (selected_policy == catalog.runtime_policies.end() ||
        selected_policy->value().motion_speed_limits.size() != 2U) {
      throw std::invalid_argument(
          "maintained scenario runtime policy is absent or incomplete: " +
          std::string(scenario.source));
    }
    std::optional<double> cell_speed_limit;
    std::optional<double> target_speed_limit;
    for (const auto &limit : selected_policy->value().motion_speed_limits) {
      if (std::holds_alternative<core::RuntimePolicyCellScopeDTO>(
              limit.scope)) {
        cell_speed_limit = limit.maximum_tcp_linear_speed_mps;
        continue;
      }
      const auto &target =
          std::get<core::RuntimePolicyCommandTargetScopeDTO>(limit.scope);
      if (target.controllable_component_id ==
              graph.controllable_components.front().controllable_component_id &&
          target.command_target_id.value() == "arm") {
        target_speed_limit = limit.maximum_tcp_linear_speed_mps;
      }
    }
    const double expected_target_speed_limit =
        scenario.family_id == "lite6_lite_gripper" ? 0.15 : 0.25;
    if (cell_speed_limit != std::optional<double>{0.5} ||
        target_speed_limit !=
            std::optional<double>{expected_target_speed_limit}) {
      throw std::invalid_argument(
          "maintained scenario has the wrong exact target speed policy: " +
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
    if (!scenario.ros2_control_execution) {
      expected_runtime_configuration.emplace(
          "direct_control_joint_limits",
          core::JsonValueDTO{core::JsonValueDTO::Array{}});
      expected_runtime_configuration.emplace(
          "motion_target_limits",
          core::JsonValueDTO{core::JsonValueDTO::Array{core::JsonValueDTO{
              core::JsonObjectDTO{
                  {"command_target_id", core::JsonValueDTO{std::string("arm")}},
                  {"controllable_component_id",
                   core::JsonValueDTO{graph.controllable_components.front()
                                          .controllable_component_id.value()}},
                  {"maximum_tcp_linear_speed_mps",
                   core::JsonValueDTO{expected_target_speed_limit}}}}}});
      if (scenario.family_id ==
          std::string_view("lite6_lite_gripper")) {
        expected_runtime_configuration.emplace(
            "gripper_target_configurations",
            core::JsonValueDTO{core::JsonValueDTO::Array{core::JsonValueDTO{
                core::JsonObjectDTO{
                    {"actuation_kind",
                     core::JsonValueDTO{std::string("position_commanded")}},
                    {"attach_frame",
                     core::JsonValueDTO{std::string("link_tcp")}},
                    {"auxiliary_frames",
                     core::JsonValueDTO{core::JsonValueDTO::Array{
                         core::JsonValueDTO{
                             std::string("lite_gripper_base_link")}}}},
                    {"closed_state_ref",
                     core::JsonValueDTO{std::string("closed")}},
                    {"command_duration_s", core::JsonValueDTO{0.8}},
                    {"command_target_id",
                     core::JsonValueDTO{std::string("gripper")}},
                    {"controllable_component_id",
                     core::JsonValueDTO{graph.controllable_components.front()
                                            .controllable_component_id.value()}},
                    {"description",
                     core::JsonValueDTO{std::string(
                         "Native simulated UFACTORY lite pneumatic gripper.")}},
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
      expected_runtime_configuration.emplace(
          "isaac_profile",
          core::JsonValueDTO{std::string(scenario.isaac_profile)});
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
                "native trajectory binding does not retain one exact runtime-scale route: " +
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
            throw std::invalid_argument(
                "native trajectory binding has the wrong runtime-scale qualification: " +
                std::string(scenario.source));
          }
          ++native_runtime_scale_binding_count;
        } else if (!binding.runtime_scale_route_qualifications.empty()) {
          throw std::invalid_argument(
              "native non-trajectory binding unexpectedly advertises runtime scaling: " +
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
    if (historical_capabilities.size() != 36U ||
        has_operation_scale == scenario.ros2_control_execution) {
      throw std::invalid_argument(
          "maintained scenario does not retain all 36 historical facilities: " +
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
  if (family_identities.size() != 5U || family_models.size() != 3U) {
    throw std::logic_error("maintained scenarios do not cover three families");
  }
}

[[nodiscard]] const Scenario &candidate_scenario_definition(
    const std::filesystem::path &workspace_root,
    const std::string_view scenario_id) {
  const auto assignments = deployment::parse_strict_json_model_yaml(read_file(
      workspace_root / "manyforge_deployment/resources/maintained/"
                       "maintained_program_role_assignments.v1.json"));
  const auto &rows = require_array(
      require_member(assignments.root(), "assignments", "role assignments"),
      "role assignments.assignments");
  const deployment::CanonicalValue *selected = nullptr;
  for (const auto &row : rows) {
    if (require_text(require_member(row, "scenario_id", "role assignment"),
                     "role assignment.scenario_id") == scenario_id) {
      if (selected != nullptr) {
        throw std::invalid_argument(
            "candidate Scenario identity is duplicated");
      }
      selected = &row;
    }
  }
  if (selected == nullptr) {
    throw std::invalid_argument(
        "candidate Scenario is not a maintained compiler input");
  }
  const auto &deployment_source = require_text(
      require_member(*selected, "deployment_source", "role assignment"),
      "role assignment.deployment_source");
  const auto found = std::find_if(
      kScenarios.begin(), kScenarios.end(), [&](const auto &scenario) {
        return scenario.source == deployment_source;
      });
  if (found == kScenarios.end()) {
    throw std::logic_error(
        "candidate Scenario has no maintained Deployment definition");
  }
  return *found;
}

[[nodiscard]] core::ProgramRoleAssignmentDTO
assignment_from(const deployment::ProgramBinding &binding) {
  core::ProgramRoleAssignmentDTO result;
  result.component_roles.reserve(
      binding.value().component_role_bindings.size());
  for (const auto &row : binding.value().component_role_bindings) {
    result.component_roles.push_back(core::ProgramComponentRoleAssignmentDTO{
        row.component_role_id, row.controllable_component_id});
  }
  result.target_roles.reserve(binding.value().target_role_bindings.size());
  for (const auto &row : binding.value().target_role_bindings) {
    result.target_roles.push_back(core::ProgramTargetRoleAssignmentDTO{
        row.target_role_id, row.command_target_id});
  }
  return result;
}

[[nodiscard]] deployment::ScenarioCatalogRowDTO &require_scenario_row(
    std::vector<deployment::ScenarioCatalogRowDTO> &rows,
    const std::string_view scenario_id) {
  auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
    return row.scenario_id == scenario_id;
  });
  if (found == rows.end()) {
    throw std::invalid_argument(
        "candidate source catalog has no exact Scenario row");
  }
  return *found;
}

[[nodiscard]] deployment::CanonicalValue compile_candidate(
    const Options &options, const QualifiedCatalogClosure &catalog,
    const ExactCompilerInputSource &input_source) {
  const auto candidate_catalog_path =
      options.candidate_package_root / "maintained.scenario-catalog.yaml";
  const bool has_candidate_catalog =
      std::filesystem::is_regular_file(candidate_catalog_path) &&
      !std::filesystem::is_symlink(candidate_catalog_path);
  const auto publication_expected = [&]()
      -> std::optional<core::ScenarioCatalogRevision> {
    if (!has_candidate_catalog) {
      return std::nullopt;
    }
    const auto current = deployment::verify_scenario_catalog_artifact(
        read_file(candidate_catalog_path));
    if (current == nullptr) {
      throw std::logic_error(
          "candidate publication catalog did not independently verify");
    }
    return current->revision();
  }();
  const deployment::FilesystemScenarioApplyCandidateSource source(
      options.base_package_root, "maintained",
      options.candidate_package_root);
  const core::ScenarioCatalogRevision expected_catalog_revision(
      options.expected_scenario_catalog_revision);
  const auto source_catalog = deployment::verify_scenario_catalog_artifact(
      read_file(source.catalog_path_for_revision(expected_catalog_revision)));
  if (source_catalog == nullptr) {
    throw std::logic_error(
        "candidate source catalog did not independently verify");
  }
  const auto source_selection = source.LoadExactProgramSelectionForCatalog(
      options.candidate_scenario_id, expected_catalog_revision);
  const auto candidate_bundle_path =
      options.candidate_package_root / "retained-bundles" / "sha256" /
      (source_selection.retained_bundle_id.value().substr(7U) + ".json");
  const auto &source_package_root =
      std::filesystem::exists(candidate_bundle_path)
          ? options.candidate_package_root
          : options.base_package_root;
  const auto &scenario = candidate_scenario_definition(
      options.workspace_root, options.candidate_scenario_id);
  const std::array<Scenario, 1U> selected_scenarios{scenario};
  const auto products = materialize_scenarios(
      options.workspace_root, catalog, input_source,
      options.candidate_package_root, selected_scenarios,
      options.candidate_deployment_path, false);
  const auto product = products.find(std::string(scenario.source));
  if (product == products.end()) {
    throw std::logic_error(
        "candidate compiler did not materialize its selected Deployment");
  }
  const auto &candidate = product->second;
  if (candidate.deployment_artifact_id !=
      source_selection.deployment_artifact_id) {
    throw std::invalid_argument(
        "candidate Deployment changes its addressed artifact identity");
  }

  const deployment::FilesystemVerifiedRetainedBundleSource source_bundles(
      source_package_root);
  const auto source_bundle =
      source_bundles.get_verified(source_selection.retained_bundle_id);
  if (source_bundle == nullptr) {
    throw std::logic_error(
        "candidate source retained bundle is unavailable");
  }
  const deployment::FilesystemVerifiedRetainedModelMapSource source_maps(
      source_package_root);
  const auto source_model_map = source_maps.get_verified(
      deployment::RetainedModelMapQuery{
          source_selection.retained_bundle_id,
          source_bundle->value()
              .compiled_products.symbol_role_maps.front()
              .product_id});
  if (!source_model_map.has_value()) {
    throw std::logic_error(
        "candidate source model map is unavailable");
  }
  const deployment::FilesystemVerifiedProgramBindingSource source_bindings(
      source_package_root);
  const auto baseline = source_bindings.get_verified(
      source_selection.program_binding_id, *source_bundle,
      *source_model_map);
  if (baseline == nullptr) {
    throw std::logic_error(
        "candidate source Program Binding is unavailable");
  }

  const auto program_bytes = read_file(options.candidate_program_path);
  const auto program = deployment::verify_program_binding_contract(
      program_bytes, *candidate.bundle);
  deployment::verify_scenario_program_admission(
      program->normalized_program_root(), candidate.bundle->value(),
      candidate.deployment_compatibility, source_selection.assistant_mode,
      source_selection.typed_parameter_overrides);
  deployment::FilesystemCatalogArtifactStore candidate_catalog_store(
      options.candidate_package_root);
  const auto binding = deployment::compile_program_binding(
      *program, assignment_from(*baseline), *candidate.bundle,
      candidate.model_map, candidate_catalog_store);
  deployment::FilesystemProgramRepository program_repository(
      options.candidate_package_root);
  deployment::FilesystemProgramBindingRepository binding_repository(
      options.candidate_package_root);
  static_cast<void>(
      program_repository.put_if_absent(program, *candidate.bundle));
  static_cast<void>(binding_repository.put_if_absent(
      binding, *candidate.bundle, candidate.model_map));

  auto rows = source_catalog->rows();
  auto &row = require_scenario_row(rows, options.candidate_scenario_id);
  row.deployment_artifact_id = candidate.deployment_artifact_id;
  row.deployment_revision = candidate.deployment_revision;
  row.retained_bundle_id = candidate.bundle->retained_bundle_id();
  row.program_binding_id = binding->program_binding_id();
  row.deployment_compatibility = candidate.deployment_compatibility;
  const auto authored_catalog = deployment::build_scenario_catalog(rows);

  deployment::FilesystemScenarioCatalogRepository repository(
      options.candidate_package_root, "maintained");
  const auto published =
      repository.compare_and_set(publication_expected, authored_catalog);
  if (published == deployment::ScenarioCatalogPublishResult::kRevisionConflict) {
    throw std::runtime_error(
        "candidate scenario catalog changed during publication");
  }
  const auto reloaded = repository.get_verified();
  if (reloaded == nullptr ||
      reloaded->revision() != authored_catalog->revision() ||
      reloaded->rows() != authored_catalog->rows()) {
    throw std::runtime_error(
        "candidate scenario catalog did not reopen exactly");
  }
  const deployment::FilesystemScenarioApplyCandidateSource reopened(
      options.candidate_package_root, "maintained");
  const auto exact =
      reopened.LoadExactProgramSelection(options.candidate_scenario_id);
  if (!exact.valid() ||
      exact.deployment_revision != candidate.deployment_revision ||
      exact.retained_bundle_id != candidate.bundle->retained_bundle_id() ||
      exact.program_binding_id != binding->program_binding_id()) {
    throw std::runtime_error(
        "candidate scenario selection did not reopen exactly");
  }

  return deployment::CanonicalValue(deployment::CanonicalValue::Object{
      {"deployment_artifact_id",
       deployment::CanonicalValue(candidate.deployment_artifact_id.value())},
      {"deployment_revision",
       deployment::CanonicalValue(candidate.deployment_revision.value())},
      {"graph_revision",
       deployment::CanonicalValue(
           candidate.bundle->value().cell_graph.graph_revision.value())},
      {"program_binding_id",
       deployment::CanonicalValue(binding->program_binding_id().value())},
      {"program_id",
       deployment::CanonicalValue(program->value().program_id.value())},
      {"program_revision",
       deployment::CanonicalValue(program->value().program_revision.value())},
      {"retained_bundle_id",
       deployment::CanonicalValue(
           candidate.bundle->retained_bundle_id().value())},
      {"scenario_catalog_revision",
       deployment::CanonicalValue(reloaded->revision().value())},
      {"scenario_id",
       deployment::CanonicalValue(options.candidate_scenario_id)},
      {"schema_version", deployment::CanonicalValue(
                             "manyforge.deployment_candidate_compilation.v1")},
  });
}

} // namespace

int main(int argc, char **argv) {
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
    if (options.mode == Options::Mode::kCompileCandidate) {
      options.base_package_root =
          std::filesystem::canonical(options.base_package_root);
      options.candidate_deployment_path =
          std::filesystem::canonical(options.candidate_deployment_path);
      options.candidate_program_path =
          std::filesystem::canonical(options.candidate_program_path);
      std::filesystem::create_directories(options.candidate_package_root);
      options.candidate_package_root =
          std::filesystem::canonical(options.candidate_package_root);
      std::cout << deployment::canonical_json(
                       compile_candidate(options, catalog, input_source))
                << '\n';
      return 0;
    }
    verify_scenarios(root, catalog);
    TemporaryPackage package(options.package_root);
    const auto products =
        materialize_scenarios(root, catalog, input_source, package.path());
    const auto bindings =
        verify_program_bindings(root, products, package.path());
    publish_and_verify_scenario_catalog(root, products, bindings,
                                        package.path());
    std::cout << "12 deployments, 3 shared families, 36 historical facilities, "
                 "verified exact catalog referents, 13 durable Program Bindings, "
                 "and 13 scenario closures validated\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "manyforge_maintained_deployment_occurrences: " << error.what()
              << '\n';
    return 2;
  }
}
